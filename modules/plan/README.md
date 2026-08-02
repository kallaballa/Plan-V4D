```markdown
# opencv_plan — Graph-Based Execution Engine for Computer Vision Pipelines

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-20-blue?logo=cplusplus" alt="C++20"/>
  <img src="https://img.shields.io/badge/OpenCV-module-green?logo=opencv" alt="OpenCV Module"/>
  <img src="https://img.shields.io/badge/license-Apache--2.0-orange" alt="License"/>
</p>

**Plan** is a lightweight, header-driven execution engine that lets you express computer vision pipelines as *deferred computation graphs*. Instead of writing imperative frame loops, you declare **what** data flows where and **when** branches fire — Plan schedules, locks, and executes the graph for you across one or many worker threads.

Extracted from the [Plan-V4D](https://github.com/kallaballa/Plan-V4D) visualization framework and stripped of all GPU/windowing dependencies, Plan is now a pure-CPU, dependency-free (beyond OpenCV Core) module suitable for any C++20 project.

---

## Table of Contents

- [Features](#features)
- [Quick Start](#quick-start)
- [Building](#building)
- [Core Concepts](#core-concepts)
  - [The Edge DSL](#the-edge-dsl)
  - [Transactions & `plain()`](#transactions--plain)
  - [Branching](#branching)
  - [Sub-Plan Composition](#sub-plan-composition)
  - [Shared State & Locking](#shared-state--locking)
  - [Global & Local State](#global--local-state)
  - [Multi-Worker Execution](#multi-worker-execution)
- [API Overview](#api-overview)
- [Examples](#examples)
- [Project Structure](#project-structure)
- [Design Philosophy](#design-philosophy)
- [Contributing](#contributing)
- [License](#license)

---

## Features

| Capability | Description |
|---|---|
| **Edge DSL** | Type-safe, compile-time-checked data-flow edges (`R`, `RW`, `RS`, `RWS`, `CS`, `V`, `P`, `F`) |
| **Operator overloads** | Arithmetic, comparison, logical, and bitwise operators compose edges into expressions |
| **Deferred execution** | Graphs are built declaratively, then executed in a single `runGraph()` call |
| **Branching** | `branch()` / `elseBranch()` / `endBranch()` with `ONCE`, `SINGLE`, `PARALLEL` semantics |
| **Sub-plans** | Hierarchical composition via `subInfer()`, `subSetup()`, `subTeardown()` |
| **Shared variables** | Mutex-backed `RWS`/`RS`/`CS` edges with automatic lock acquisition |
| **State machines** | `GlobalState` (process-wide) and `LocalState` (thread-local) key-value stores |
| **Multi-worker** | `Plan::run<T>(workers, runtime)` spawns N threads with barrier synchronization |
| **Pluggable contexts** | Register named execution contexts; default `PlainContext` requires zero setup |
| **Resequence** | Ordered frame completion across parallel workers |
| **Zero GPU deps** | No OpenGL, GLFW, NanoVG, ImGui, or bgfx — pure CPU, OpenCV Core only |

---

## Quick Start

```cpp
#include <opencv2/plan/plan.hpp>
#include <iostream>

using namespace cv;
using namespace cv::plan;

class CounterPlan : public Plan {
public:
    uint64_t frame_ = 0;
    double   fps_   = 0.0;

    void infer() override {
        // Increment frame counter
        plain([](uint64_t& f) { ++f; }, RW(frame_));

        // Branch: print every 60 frames
        branch(R(frame_) % V(uint64_t(60)) == V(uint64_t(0)))
            ->plain([](const uint64_t& f) {
                std::cout << "Frame " << f << std::endl;
            }, R(frame_))
        ->endBranch();
    }
};

int main() {
    GlobalState::init_keys();
    LocalState::init_keys();

    auto rt   = cv::makePtr<Runtime>(cv::Size(640, 480));
    auto plan = Plan::make<CounterPlan>();
    plan->setRuntime(rt);

    for (int i = 0; i < 300; ++i) {
        plan->infer();
        plan->makeGraph();
        plan->runGraph();
        plan->clearGraph();
    }
}
```

Compile:

```bash
g++ -std=c++20 counter.cpp -o counter \
    $(pkg-config --cflags --libs opencv4) \
    -lopencv_plan
```

---

## Building

### As an OpenCV module (recommended)

```bash
# Inside your OpenCV source tree:
cp -r plan modules/plan

# Configure & build
cmake -B build -S . \
  -DBUILD_LIST=core,plan \
  -DBUILD_TESTS=ON \
  -DBUILD_EXAMPLES=ON
cmake --build build -j$(nproc)
```

### CMake options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_TESTS` | `ON` | Build unit tests (`test_plan_*`) |
| `BUILD_EXAMPLES` | `ON` | Build sample executables |

### Requirements

- C++20 compiler (GCC ≥ 12, Clang ≥ 15, MSVC ≥ 19.34)
- OpenCV Core (`opencv_core`)
- CMake ≥ 3.20

---

## Core Concepts

### The Edge DSL

Edges are *typed handles* to data that participate in the computation graph. They carry compile-time metadata (read/write, shared, copy) that Plan uses to insert locks and copies automatically.

| Constructor | Semantics | Use case |
|---|---|---|
| `R(x)` | Read-only reference | Input data |
| `RW(x)` | Read-write reference | Mutable state |
| `RS(x)` | Read-only **shared** (locked) | Cross-thread reads |
| `RWS(x)` | Read-write **shared** (locked) | Cross-thread mutation |
| `CS(x)` | **Copy** of shared variable | Safe snapshot |
| `V(val)` | By-value literal | Constants |
| `P<T>(key)` | Property edge (state machine) | Config / counters |
| `F(fn, edges...)` | Function edge | Lazy computation |

Edges compose with standard operators:

```cpp
auto result = (R(a_) + R(b_)) * V(2) - V(1);   // arithmetic
auto flag   = R(x_) > V(100) && R(y_) < V(0);  // logical
auto idx    = R(vec_)[R(i_)];                   // indexing
```

### Transactions & `plain()`

`plain(fn, edges...)` emits a graph node. The function `fn` is **not** called immediately — it is wrapped in a `Transaction` and executed when `runGraph()` is invoked.

```cpp
void infer() override {
    plain([](cv::UMat& out, const cv::UMat& in) {
        cv::GaussianBlur(in, out, {5,5}, 0);
    }, RW(blurred_), R(source_));
}
```

### Branching

Branches are predicate nodes. The graph evaluator checks the predicate and enables/disables subsequent nodes until `endBranch()`.

```cpp
branch(R(temperature_) > V(80.0))
    ->plain(activate_cooling, RW(fan_speed_))
->elseBranch()
    ->plain(deactivate_cooling, RW(fan_speed_))
->endBranch();
```

**Branch types:**

| Type | Behavior |
|---|---|
| `PARALLEL` (default) | All workers evaluate the branch |
| `SINGLE` | Only one worker enters (mutex-guarded) |
| `ONCE` | Executes only on the first frame |
| `PARALLEL_ONCE` | One worker, one time |

### Sub-Plan Composition

Complex pipelines are built from smaller plans:

```cpp
class Pipeline : public Plan {
    cv::Ptr<BlurPlan>    blur_;
    cv::Ptr<DetectPlan>  detect_;

    Pipeline() {
        blur_   = _sub<BlurPlan>(this);
        detect_ = _sub<DetectPlan>(this);
    }

    void infer() override {
        assign(RW(blur_->input_), R(frame_));
        subInfer(blur_);
        assign(RW(detect_->input_), R(blur_->output_));
        subInfer(detect_);
    }
};
```

### Shared State & Locking

Declare shared variables in the constructor. Plan automatically acquires/releases mutexes when `RWS`/`RS`/`CS` edges are used inside transactions.

```cpp
class WorkerPlan : public Plan {
    int counter_ = 0;  // shared across workers

    WorkerPlan() { _shared(counter_); }

    void infer() override {
        plain([](int& c) { ++c; }, RWS(counter_));  // locked
    }
};
```

### Global & Local State

```cpp
// Process-wide (thread-safe)
GlobalState::set(GlobalState::Keys::FRAME_CNT, uint64_t(42));
auto fps = GlobalState::get<double>(GlobalState::Keys::FPS);

// Thread-local
LocalState::set(LocalState::Keys::WORKER_INDEX, size_t(3));

// Access from within a graph via P() edges
assign(RW(my_var_), P<uint64_t>(GlobalState::Keys::FRAME_CNT));
```

### Multi-Worker Execution

```cpp
int main() {
    auto rt = cv::makePtr<Runtime>(cv::Size(1920, 1080));
    // 4 worker threads + 1 main thread
    Plan::run<MyPlan>(4, rt);
}
```

Plan spawns workers, synchronizes via `std::barrier`, and uses `Resequence` to guarantee ordered frame completion.

---

## API Overview

### `Plan` (abstract)

```cpp
class Plan {
    virtual void setup() {}
    virtual void infer() = 0;
    virtual void teardown() {}

    // Graph lifecycle
    void makeGraph();
    void runGraph();
    void clearGraph();

    // Node emission
    cv::Ptr<Plan> plain(Fn fn, Edges... args);
    cv::Ptr<Plan> ctx(const std::string& name, Fn fn, Edges... args);

    // Branching
    cv::Ptr<Plan> branch(Edge predicate);
    cv::Ptr<Plan> branch(BranchType::Enum type, Fn fn, Edges... args);
    cv::Ptr<Plan> elseBranch();
    cv::Ptr<Plan> endBranch();

    // Sub-plans
    cv::Ptr<Plan> subInfer(cv::Ptr<T> sub);
    cv::Ptr<Plan> subSetup(cv::Ptr<T> sub);
    cv::Ptr<Plan> subTeardown(cv::Ptr<T> sub);

    // Edge constructors
    auto R(const T& t);    auto RW(T& t);
    auto RS(const T& t);   auto RWS(T& t);
    auto CS(T& t);         auto V(T val);
    auto P<Tval>(Key key); auto F(Fn fn, Edges... args);

    // Operators
    auto OP<Top>(Edges... edges);
    auto assign(Edges... edges);
    auto IF(Edges... edges);
    // ... +, -, *, /, %, &&, ||, ==, !=, <, >, <=, >=, !, ^, &, |, <<, >>

    // Factory
    template<typename T, typename... Args>
    static cv::Ptr<T> make(Args&&... args);

    template<typename T, typename... Args>
    static void run(int32_t workers, cv::Ptr<Runtime> rt, Args&&... args);
};
```

### `Runtime`

```cpp
class Runtime {
    Runtime(const cv::Size& size = {1920, 1080});

    void registerContext(const std::string& name, cv::Ptr<PlanContext> ctx);
    cv::Ptr<PlanContext> getContext(const std::string& name);

    template<typename T> void set(Keys::Enum key, const T& val);
    template<typename T> const T& get(Keys::Enum key);
};
```

### `PlanContext` (interface)

```cpp
class PlanContext {
    virtual int execute(const cv::Rect& viewport, std::function<void()> fn) = 0;
};
```

Implement this to integrate custom execution environments (thread pools, GPU queues, etc.).

---

## Examples

| Sample | Demonstrates |
|--------|-------------|
| [`simple_counter.cpp`](samples/simple_counter.cpp) | Basic lifecycle, `plain()`, branching, FPS |
| [`branching_demo.cpp`](samples/branching_demo.cpp) | Multi-way classification, `BranchType::ONCE` |
| [`subplan_demo.cpp`](samples/subplan_demo.cpp) | 3-stage pipeline, `_sub()`, inter-plan data flow |
| [`shared_state_demo.cpp`](samples/shared_state_demo.cpp) | `_shared()`, `RWS`/`CS`, concurrent access |
| [`image_pipeline_demo.cpp`](samples/image_pipeline_demo.cpp) | UMat processing, Canny, statistics, branching |

Run a sample:

```bash
./build/bin/example_plan_simple_counter 300
```

---

## Project Structure

```
modules/plan/
├── CMakeLists.txt
├── include/opencv2/plan/
│   ├── plan.hpp              # Plan class, Runtime, operator overloads
│   ├── context.hpp           # PlanContext, PlainContext
│   ├── transaction.hpp       # Transaction, Edge, Node, BranchType
│   ├── state.hpp             # (in util.hpp) GlobalState, LocalState, SharedVariables
│   ├── threadsafeanymap.hpp  # ThreadSafeAnyMap, AnyPropertyMap
│   ├── resequence.hpp        # Resequence (ordered completion)
│   └── util.hpp              # Type traits, SharedVariables, utilities
├── src/
│   ├── plan.cpp              # Plan::makeGraph, runGraph, run<T>
│   ├── transaction.cpp       # Transaction base
│   ├── state.cpp             # GlobalState / LocalState statics
│   └── resequence.cpp        # Resequence implementation
├── test/
│   ├── test_plan_edges.cpp
│   ├── test_plan_transactions.cpp
│   ├── test_plan_branching.cpp
│   ├── test_plan_state.cpp
│   ├── test_plan_shared.cpp
│   └── test_plan_integration.cpp
└── samples/
    ├── simple_counter.cpp
    ├── branching_demo.cpp
    ├── subplan_demo.cpp
    ├── shared_state_demo.cpp
    └── image_pipeline_demo.cpp
```

---

## Design Philosophy

1. **Declarative over imperative** — You describe the *shape* of computation; Plan handles scheduling.
2. **Type safety at compile time** — Edge metadata (`read_t`, `shared_t`, `lockie_t`) is encoded in the type system. Illegal access patterns are compile errors.
3. **Zero-cost abstractions** — Edges are thin wrappers around pointers. No heap allocation in the hot path unless you use `V()` or `F()`.
4. **Opt-in concurrency** — Single-threaded by default. Add `_shared()` + `RWS` and Plan inserts locks. Scale with `Plan::run<T>(N, rt)`.
5. **No hidden global state** — All state lives in `Runtime`, `GlobalState`, or your plan's members. No singletons (except the explicitly opt-in `GlobalState`).
6. **Composable** — Sub-plans are first-class citizens. Build complex pipelines from tested, reusable components.

---

## Contributing

1. Fork the repository.
2. Create a feature branch: `git checkout -b feature/my-feature`.
3. Ensure all tests pass: `ctest --test-dir build -R plan`.
4. Follow the existing code style (OpenCV conventions, 4-space indent).
5. Open a Pull Request with a clear description.

### Running tests

```bash
cd build
ctest -R plan --output-on-failure
```

---

## License

This module is part of the OpenCV project and is licensed under the [Apache License 2.0](https://www.apache.org/licenses/LICENSE-2.0).

Original Plan-V4D code: Copyright © Amir Hassan (kallaballa) \<amir@viel-zu.org\>

---

<p align="center">
  <sub>Built with ❤️ for the computer vision community.</sub>
</p>
```
