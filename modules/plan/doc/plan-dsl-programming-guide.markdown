# The Plan-DSL Programming Guide

*A practical introduction to the language that drives Plan-V4D.*

> **Audience.** C++ developers who are new to Plan-V4D and want to learn how to *write* programs in Plan-DSL.
>
> After reading this guide, you should be able to understand the samples in `modules/v4d/samples/` without constantly consulting the reference manual.
>
> For the formal specification of every edge-call, operator, and context, see the companion document:
>
> **`plan-dsl-reference.markdown`**

---

## Table of Contents

1. [Hello, graph](#1-hello-graph)
2. [What kind of language is Plan-DSL?](#2-what-kind-of-language-is-plan-dsl)
3. [Your first plan](#3-your-first-plan)
4. [The execution model: build once, run forever](#4-the-execution-model-build-once-run-forever)
5. [Edges: the only values in Plan-DSL](#5-edges-the-only-values-in-plan-dsl)
6. [Operators: the ALU](#6-operators-the-alu)
7. [Variables, locals, and shared state](#7-variables-locals-and-shared-state)
8. [Control flow: `branch`, `elseBranch`, `endBranch`](#8-control-flow-branch-elsebranch-endbranch)
9. [Functions and calls with `F(...)`](#9-functions-and-calls-with-f)
10. [Properties: reading runtime state](#10-properties-reading-runtime-state)
11. [Events: reading input](#11-events-reading-input)
12. [Loops as a special case of branches](#12-loops-as-a-special-case-of-branches)
13. [Sub-plans: modules and reusable logic](#13-sub-plans-modules-and-reusable-logic)
14. [Side-effect contexts: `gl`, `nvg`, `fb`, `capture`, `write`, `imgui`](#14-side-effect-contexts-gl-nvg-fb-capture-write-imgui)
15. [Lifecycle: `setup`, `infer`, `teardown`, `gui`, `run`](#15-lifecycle-setup-infer-teardown-gui-run)
16. [Workers: how parallelism works](#16-workers-how-parallelism-works)
17. [Walkthrough: `video_editing.cpp`](#17-walkthrough-video_editingcpp)
18. [Walkthrough: `beauty-demo.cpp`](#18-walkthrough-beauty-democpp)
19. [Cheat sheet](#19-cheat-sheet)

---

## 1. Hello, graph

Open `modules/v4d/samples/video_editing.cpp`. The entire plan is small enough to read in one sitting:

```cpp
class VideoEditingPlan : public V4DPlan {
    cv::UMat frame_;
    const std::string hv_ = "Hello Video!";
    Property<cv::Size> sz_ = P<cv::Size>(V4D::Keys::SIZE);

public:
    void infer() override {
        capture();

        nvg({
            using namespace cv::v4d::nvg;

            fontSize(40.0f);
            fontFace("sans-bold");
            fillColor(Scalar(255, 0, 0, 255));
            textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            text(sz_.width / 2.0, sz_.height / 2.0,
                 hv_.c_str(), hv_.c_str() + hv_.size());
        }, sz_, R(hv_));

        write();
    }
};

int main(int argc, char** argv) {
    // Open source and sink...
    V4DPlan::run<VideoEditingPlan>(0);
}
```

The class `VideoEditingPlan` does not directly run a frame loop. Instead, it *describes* what should happen every frame:

1. Pull in a frame.
2. Draw text over it.
3. Push the frame out.

Then this call:

```cpp
V4DPlan::run<VideoEditingPlan>(0);
```

starts the runtime and executes that description repeatedly.

There is no explicit `while (true)`, no `update()` method, and no hand-written main loop. Plan-DSL and the Plan runtime handle that.

Every piece of user code belongs to one of the lifecycle methods of a class derived from `Plan`, or from `V4DPlan` when using the V4D windowing and rendering runtime:

```cpp
setup()
infer()
teardown()
gui()
```

Everything else is mostly library glue.

---

## 2. What kind of language is Plan-DSL?

Plan-DSL is easiest to understand by comparing what it looks like with what it actually is.

| What it looks like | What it actually is |
|---|---|
| A C++ library | An embedded C++ language that records a graph |
| A dataflow language | A task graph with explicit memory dependencies |
| An interpreter | A compiler-like recording phase followed by replay |
| An actor framework | A flat list of nodes executed in record order every frame |

This distinction matters throughout the rest of the guide.

### Side effects are deferred

This call:

```cpp
add(R(x), R(y));
```

does **not** immediately compute `x + y`.

Instead, it records an `ADD` node. Every frame, that node will read the current values of `x` and `y` and produce a result.

This is the most important mental shift in Plan-DSL.

Because execution is deferred, many errors surface during graph construction rather than during normal C++ execution. Examples include:

* wrong types,
* dangling references,
* undeclared shared variables,
* invalid operand combinations.

### The function you are really writing is `infer()`

Every DSL call inside `infer()` emits one or more nodes into a graph.

That graph is built once per worker and then replayed every frame.

The lifecycle methods have different roles:

* `setup()` — one-shot initialization.
* `infer()` — records the per-frame graph.
* `teardown()` — one-shot cleanup.
* `runGraph()` — the runtime-provided frame loop.

### Plan-DSL describes one iteration of a frame loop

You are not writing a normal sequential program. You are describing what should happen during one frame.

Every frame, the same recorded graph is executed again, potentially on new data.

If you have written GPU shaders before, think of Plan-DSL as a CPU-side shader for a per-frame computation graph:

* inputs come from `R`, `P`, and `E`,
* computation happens through operators such as `ADD`, `MUL`, `IF`, and `F`,
* outputs happen through `RW`, `assign`, `set`, and `write`.

---

## 3. Your first plan

A minimal Plan-DSL program looks like this:

```cpp
#include <opencv2/plan/plan.hpp>

using namespace cv;
using namespace cv::plan;

struct CountToTen : Plan {
    void infer() override {
        branch(P<uint64_t>(GlobalState::Keys::FRAME_CNT) < V(uint64_t(10)));
            plain({ std::cout << "tick" << std::endl; });
        endBranch();
    }
};

int main() {
    Plan::run<CountToTen>(0);
}
```

Key points:

* `Plan` is the core base class.
* The main plan logic lives inside `infer()`.
* You do not write the frame loop yourself.
* `branch(predicate)` opens a conditional region.
* `plain({ ... })` creates a node containing ordinary C++ code.
* `V(uint64_t(10))` creates a constant edge.
* `P<uint64_t>(GlobalState::Keys::FRAME_CNT)` reads the runtime frame counter.
* `Plan::run<CountToTen>(0)` starts the runtime.

If built and run, this prints one `tick` per frame for ten frames, then exits.

---

## 4. The execution model: build once, run forever

This is the most important section in the guide. If you are writing a nontrivial plan, reread this before continuing.

A plan defines four lifecycle hooks:

| Method | When it runs |
|---|---|
| `setup()` | Once per worker thread, before the frame loop |
| `infer()` | Once per worker thread, to build the per-frame graph |
| `teardown()` | Once per worker thread, after the frame loop |
| `gui()` | Once, on the main thread, before the frame loop |

The essential idea is the split between **building** the graph and **running** the graph.

```text
setup()
infer()
teardown()
```

are called once per worker during initialization.

`infer()` is where the per-frame graph is recorded.

After all workers have built their graphs, each worker enters the runtime frame loop:

```text
loop forever:
    runGraph()
```

`runGraph()` is provided by the runtime. You do not write it.

It walks the recorded node list in record order. For each node:

1. If the node is a `branch`, evaluate its predicate.
2. If the predicate is false, skip the branch body until the matching `endBranch()`.
3. If the node is enabled, invoke its callable with the current operand values.

That is the entire execution model.

There is no dependency-based scheduler, no work stealing, and no automatic vectorization. Nodes run in the order they were recorded, on the worker thread that recorded them.

### Why this matters

Because the graph is built once, you can write code that would be incorrect in ordinary C++ if interpreted as immediate execution.

For example:

```cpp
auto tmp = F(compute_something, R(input));
use(R(tmp));
use(R(tmp));
use(R(tmp));
```

The call to `F(...)` records one compute node. The later uses of `R(tmp)` refer to that node’s result.

Similarly:

```cpp
branch(R(x) == V(0));
    // ...
endBranch();
```

is valid even if `x` is uninitialized when `infer()` runs. The comparison node is re-evaluated every frame.

### What you cannot do

You cannot rely on ordinary C++ side effects inside `infer()` to persist across frames.

This is usually wrong:

```cpp
void infer() override {
    counter++; // happens during graph construction, not every frame
}
```

If you want per-frame mutation, use a node that writes through a `RW` edge or through shared state.

You also cannot make the structure of the graph depend on runtime values.

This does not do what a normal C++ `if` does:

```cpp
if (someEdge) {
    emitAdd();
} else {
    emitMul();
}
```

Both sides would be recorded at build time. For runtime selection, use `branch(...)`.

---

## 5. Edges: the only values in Plan-DSL

Plan-DSL has one fundamental value type: the **edge**.

An edge is a typed handle to either:

* a storage location, such as a plan member or shared variable, or
* a computed value produced by an operator or function call.

Edges are created by **edge-calls** and consumed by operators.

A useful mental model is:

> An edge is like a typed SSA value with an explicit memory-access intent.

The intent tells the runtime how the node should treat the storage behind the edge.

### Edge-calls

| Edge-call | Meaning | Access intent |
|---|---|---|
| `V(x)` | Immediate constant | None |
| `R(x)` | Current value of `x` | Read-only |
| `RW(x)` | Storage of `x` | Read-write |
| `RS(x)` | Shared variable `x` | Read under lock |
| `RWS(x)` | Shared variable `x` | Read-write under lock |
| `CS(x)` | Snapshot of shared variable `x` | Read and copy under lock |
| `P(key)` | Runtime property | Shared read-only |
| `E<T>()` | Input event stream | Shared, polled each frame |

Here, `x` is any C++ lvalue that the plan owns or has registered.

### Access intent enables optimization and safety

The intent is not just notation. It lets the runtime reason about dependencies.

If a node uses only `R(x)`, the runtime knows it only reads `x`.

If a node uses `RW(x)`, the runtime knows it defines or modifies `x`.

If a node uses `RS(x)` or `RWS(x)`, the runtime knows the access must be synchronized.

### Edges are typed

Plan-DSL inherits C++ types.

For example:

```cpp
R(intVar)
```

is an edge of type `int`.

```cpp
R(frame_)
```

may be an edge of type `cv::UMat`.

Operator overloading then works naturally:

```cpp
R(intVar) + V(1)
```

records an `ADD` node that returns a new integer edge.

### The `_()` tuple helper

C++ does not support variadic operator overloading, so Plan-DSL provides `_()` as a tuple helper for n-ary operations:

```cpp
auto sum = R(a) + _(R(b), R(c));
```

This means roughly:

```cpp
a + (b + c)
```

or, depending on lowering, one n-ary `ADD` node with three operands.

Use `_()` whenever you need to pass multiple edges to a variadic operator.

### One variable, multiple views

A single C++ variable can be accessed through multiple edge types:

```cpp
int x = 0;

auto readEdge  = R(x);
auto writeEdge = RW(x);
auto constEdge = V(7);
```

* `R(x)` reads the current value.
* `RW(x)` designates `x` as a destination.
* `V(7)` is just the constant `7` and does not refer to `x`.

`R(x)` and `RW(x)` may refer to the same storage, but they play different roles in operators.

### Example

```cpp
int threshold_ = 128;

void infer() override {
    auto src = R(frame_);
    auto bright = F(&cv::mean, src) > V(threshold_);

    branch(bright);
        plain({
            // process bright frame
        }, R(frame_));
    endBranch();
}
```

What happens here?

1. `R(frame_)` creates a read-only edge to `frame_`.
2. `F(&cv::mean, src)` records a call node that computes the mean.
3. `> V(threshold_)` records a comparison node.
4. `branch(bright)` opens a conditional region that runs only when the comparison is true.

Nothing here executes immediately as a normal C++ computation. It is all recorded.

---

## 6. Operators: the ALU

Operators are the named instructions of Plan-DSL.

They are the arithmetic, logical, comparison, memory, and construction operations of the graph.

There are four ways to express an operator:

| Form | Example | Returns result edge? |
|---|---|---|
| Symbol form | `a + b` | Yes |
| Named form | `ADD(a, b)` | Yes |
| Generic form | `OP<Operators::ADD_>(a, b)` | Yes |
| Statement form | `assign(a, b)` | No |

Symbol form is usually the most readable.

Named and generic forms are useful for tools and code generation.

Statement forms are useful when the operation only writes to storage and does not need to return a result edge.

### Arithmetic and logic

Plan-DSL supports the normal C++ operator set:

#### Arithmetic

```cpp
+
-
*
/
%
-x
++x
x++
--x
x--
```

#### Logical

```cpp
&&
||
!
```

#### Bitwise

```cpp
&
|
^
<<
>>
```

#### Comparison

```cpp
==
!=
<
>
<=
>=
```

#### Ternary selection

```cpp
IF(cond, a, b)
```

#### Memory operations

```cpp
container[i]   // IDX
*ptr           // DEREF
dst = src      // ASSIGN
```

#### Construction

```cpp
T(args...)     // CONSTRUCT
```

### Statement form

If you only care about writing a value back to storage, use a statement form:

```cpp
assign(RW(x), R(y));
```

Equivalent in intent to:

```cpp
x = y;
```

You can also write:

```cpp
RW(x) = R(y);
```

Statement forms return `cv::Ptr<Plan>`, so they can be chained.

---

## 7. Variables, locals, and shared state

A plan’s C++ member variables act as its local storage.

They are similar to `alloca` slots in a traditional compiler.

Example:

```cpp
struct DemoPlan : Plan {
    int counter_ = 0;
    cv::UMat frame_;

    void infer() override {
        auto c = RW(counter_);
        c = c + V(1);
    }
};
```

### Local state is thread-local by default

By default, plan members are local to each worker thread.

Each worker builds its own graph and has its own copy of the plan’s non-shared members.

This is useful for:

* scratch buffers,
* per-worker counters,
* temporary images,
* thread-local state.

### Shared state must be explicit

If multiple workers, or the GUI thread and a worker, must share a variable, declare it shared:

```cpp
_shared(params_);
```

Shared variables are protected by a mutex.

Access them using shared edge-calls:

```cpp
RS(params_)
RWS(params_)
CS(params_)
```

where:

* `RS(x)` reads shared state under lock.
* `RWS(x)` reads and writes shared state under lock.
* `CS(x)` takes a thread-safe snapshot of shared state.

`CS` is especially useful for handoffs between threads, such as GUI-written parameters consumed by worker threads.

### `_safe`

`_safe(x)` is the opposite escape hatch:

```cpp
_safe(someVariable);
```

It tells the runtime that this variable will never be accessed as shared state.

### Global and local state tables

Plan provides two type-keyed state tables:

* `GlobalState`
* `LocalState`

`GlobalState` is shared across all workers and the GUI thread.

`LocalState` stores one value per key per thread.

Read values using `P<T>(key)`.

Write values using the appropriate state API, such as:

```cpp
GlobalState::set<T>(key, value);
```

The Plan core defines several keys, and runtimes such as V4D add more.

---

## 8. Control flow: `branch`, `elseBranch`, `endBranch`

Plan-DSL has no `goto`, `break`, or `continue`.

Control flow is structured and expressed using branch regions.

Basic form:

```cpp
branch(predicate);
    // statements
elseBranch();
    // statements
endBranch();
```

A branch region is itself part of the graph.

Every frame, when the runtime reaches a `branch`, it evaluates the predicate.

* If true, the branch body runs.
* If false, the branch body is skipped.
* If `elseBranch()` is present, the else body runs when the original predicate is false.

### Predicate forms

The predicate may be:

#### A boolean edge

```cpp
branch(R(x) == V(0));
```

#### A callable returning bool

```cpp
branch([] { return true; });
```

#### A callable taking operand edges

```cpp
branch([](auto a) { return a > 0; }, R(n));
```

### Predefined predicates

Plan provides several helper predicates:

```cpp
always_
isTrue_(bool)
isFalse_(bool)
and_(bool, bool)
or_(bool, bool)
```

### Branch types

Branches may carry a `BranchType::Enum`:

| Value | Name | Meaning |
|---|---|---|
| `0` | `NONE` | No branch; plain node behavior |
| `1` | `SINGLE` | At most one worker executes the region |
| `2` | `PARALLEL` | Every worker executes the region if predicate is true |
| `4` | `ONCE` | Executes exactly once globally |
| `8` | `PARALLEL_ONCE` | Executes exactly once per worker |

Example:

```cpp
branch(BranchType::SINGLE, predicate);
    // serialized work
endBranch();
```

`SINGLE` and `ONCE` acquire global locking.

`ONCE` is sticky: once it has run, it will not run again.

### Chaining

Many Plan-DSL calls return `cv::Ptr<Plan>`, so you can chain them:

```cpp
branch(cond)
    ->plain(workA)
    ->branch(subCond)
        ->plain(workB)
    ->endBranch()
->elseBranch()
    ->plain(workC)
->endBranch();
```

This is equivalent to the nested block form. Use whichever is clearer.

---

## 9. Functions and calls with `F(...)`

When you need an operation that does not have a dedicated operator, use `F(...)`.

`F` accepts any callable:

* free function,
* member function pointer,
* lambda,
* function object.

Examples:

```cpp
auto t = F(&cv::getTickCount);
auto w = F(&cv::Size::width, R(sz));
F(&cv::split, R(src), RW(dst));
```

If the callable returns a non-`void` value, `F` returns a fresh result edge.

If the callable returns `void`, `F` acts as a statement.

`F` is also the main way to integrate external C++ libraries into the graph.

There is no separate Plan-DSL function-definition syntax. To reuse logic, write a normal C++ function or lambda and call it through `F`.

The graph records each call site as a node. Identical call sites with identical operands may be deduplicated.

---

## 10. Properties: reading runtime state

A **property** is an edge bound to a value in `GlobalState` or `LocalState`.

Examples:

```cpp
Property<cv::Size>  size_  = P<cv::Size>(V4D::Keys::SIZE);
Property<uint64_t>  frame_ = P<uint64_t>(GlobalState::Keys::FRAME_CNT);
Property<size_t>    widx_  = P<size_t>(LocalState::Keys::WORKER_INDEX);
```

Properties behave like edges, so you can pass them directly to operators without wrapping them in `R()`.

Example:

```cpp
branch(seqCnt_ % V(uint64_t(8)) == V(uint64_t(0)));
```

This runs the branch body every eighth frame.

### Core global keys

Examples include:

```cpp
FRAME_CNT
CAPTURE_CNT
FPS_CNT
RUN_CNT
START_TIME
FPS
WORKERS_READY
WORKERS_STARTED
LOCKING
DISPLAY_READY
LOCK_CONTENTION_CNT
LOCK_CONTENTION_RATE
LCR_CNT
SHOW_GUI
SHOW_FRAME_TIME
TIME_TRACKER
```

### Core local keys

```cpp
WORKER_INDEX
```

Runtimes can add their own keys.

V4D adds keys such as:

```cpp
V4D::Keys::SIZE
V4D::Keys::VIEWPORT
V4D::Keys::NAMESPACE
V4D::Keys::FULLSCREEN
V4D::Keys::DISABLE_INPUT_EVENTS
```

---

## 11. Events: reading input

An **event edge** produces a vector of input events for the current frame.

Example:

```cpp
Event<Mouse> pressEvents_ = E<Mouse>(Mouse::Type::PRESS);
auto anyPress = !F(&Mouse::List::empty, pressEvents_);
```

The DSL core itself produces empty event lists. A runtime such as V4D connects the event edge to a real input source.

Available event classes include:

```cpp
Mouse
Keyboard
Window
Joystick
```

Each event class has:

* a nested `Type` enum,
* a `List` container.

Usage forms:

```cpp
E<T>()
E<T>(type)
E<T>(type, trigger)
```

where:

* `E<T>()` means all events of class `T`.
* `E<T>(type)` means events of a specific type.
* `E<T>(type, trigger)` adds a trigger predicate.

---

## 12. Loops as a special case of branches

Plan-DSL has no `for` or `while` keywords.

A loop is expressed as a branch whose predicate is updated by the loop body.

Example:

```cpp
struct CountdownPlan : Plan {
    int counter_ = 10;

    void infer() override {
        branch(R(counter_) > V(0));
            plain({ std::cout << "tick" << std::endl; }, R(counter_));
            assign(RW(counter_), R(counter_) - V(1));
        endBranch();
    }
};
```

Execution proceeds as follows:

1. First frame: `counter_ == 10`, predicate true, body runs, `counter_` becomes `9`.
2. Next frame: `counter_ == 9`, predicate true, body runs, `counter_` becomes `8`.
3. This continues until `counter_ == 0`.
4. Predicate becomes false, and the branch body is skipped.

This gives `while`-like behavior in a frame-based execution model.

The graph is not rebuilt. Only the predicate value changes from frame to frame.

---

## 13. Sub-plans: modules and reusable logic

A sub-plan is a `Plan` object owned by another `Plan`.

Create it in the parent constructor:

```cpp
struct ParentPlan : Plan {
    cv::Ptr<SubPlan> sub_;

    ParentPlan() {
        sub_ = _sub<SubPlan>(this);
    }

    void infer() override {
        subInfer(sub_);
    }
};
```

Important rule:

> `_sub` must be called only during parent construction.

Do not call `_sub` from `infer()`, `setup()`, or `teardown()`.

After construction, splice the sub-plan into the parent using:

```cpp
subInfer(sub);
subSetup(sub);
subTeardown(sub);
```

Most often, you call `subInfer(sub)` from the parent’s `infer()`.

Sub-plans are useful for:

* reusable logic,
* modularizing large plans,
* encapsulating filters or processing stages,
* calling reusable graphs from inside branch regions.

Because a sub-plan graph is spliced at the call site, it inherits the enclosing branch predicate.

---

## 14. Side-effect contexts: `gl`, `nvg`, `fb`, `capture`, `write`, `imgui`

A **context call** attaches a node to a specialized execution environment.

The core DSL defines the plain CPU context:

```cpp
plain(fn, args...)
F(fn, args...)
```

Runtimes such as V4D add additional contexts.

### V4D context calls

| Call | Context | Purpose |
|---|---|---|
| `gl(fn, args...)` | OpenGL | Execute GL commands |
| `fb<pos>(fn, args...)` | Framebuffer | Access framebuffer |
| `nvg(fn, args...)` | NanoVG | Vector graphics |
| `bgfx(fn, args...)` | bgfx | bgfx rendering |
| `ext(fn, args...)` | External | External renderer contexts |
| `capture(...)` | Source | Pull next input frame |
| `write(...)` | Sink | Push output frame |
| `imgui(...)` | ImGui | Install UI node |
| `set(key, edge)` | CPU | Property write node |

A typical frame body looks like this:

```cpp
void infer() override {
    capture(RW(frames_.orig_));

    plain(prepareFrames, R(downSize_), RW(frames_));

    branch(enabled());
        plain(processFrames, RW(frames_));
    ->elseBranch()
        plain(bypassProcessing, RW(frames_));
    ->endBranch();

    fb<1>(cv::cvtColor,
          R(frames_.result_),
          V(cv::COLOR_BGR2RGBA),
          V(0),
          V(cv::ALGO_HINT_DEFAULT));

    write(R(frames_.result_));
}
```

Each C++ function is wrapped into a node that the runtime dispatches to the correct context.

---

## 15. Lifecycle: `setup`, `infer`, `teardown`, `gui`, `run`

A program starts with:

```cpp
Plan::run<Tplan>(workers, args...);
```

or, for V4D:

```cpp
V4DPlan::run<Tplan>(workers, args...);
```

The arguments are forwarded to the plan constructor.

The runtime then performs roughly these steps:

1. Spawn worker threads.
2. Call `gui()` once on the main thread.
3. For each worker:
   * call `setup()`,
   * build and run the setup graph,
   * clear it.
4. For each worker:
   * call `infer()`,
   * build the per-frame graph.
5. Synchronize all workers and the main thread.
6. Enter the frame loop.
7. Every frame, each worker calls `runGraph()`.
8. When the runtime shuts down, each worker calls `teardown()`.

### Worker count semantics

The meaning of the `workers` argument is:

| `workers` value | Meaning |
|---|---|
| `-1` | Default worker count |
| `0` | One worker plus main thread |
| `>= 1` | N workers plus main thread |

For V4D, the main thread usually handles the display and event loop.

### `gui()` is special

`gui()` runs once on the main thread, outside the normal graph.

Use it for:

* installing ImGui nodes,
* one-shot main-thread UI setup,
* creating UI state.

Keep `gui()` simple.

Do not store state in plan members that are also written by worker graphs unless that state is properly shared.

---

## 16. Workers: how parallelism works

Each worker thread builds and runs its own copy of the graph.

Workers do not share nodes.

They do not migrate work.

They do not perform node-level scheduling.

There is no work stealing.

Workers do share:

* `GlobalState`,
* registered shared variables,
* the runtime object.

When a branch is marked `SINGLE`, the runtime serializes execution of that branch body across workers.

When a branch is marked `PARALLEL`, every worker evaluates and potentially executes the branch body concurrently.

If multiple workers write to the same state in a `PARALLEL` branch, that state must be shared and accessed through the appropriate shared edge-calls.

A good mental model is:

> Think of `infer()` as the body of an OpenMP parallel region.

Whatever you would safely do in a `#pragma omp parallel` region, do here.

---

## 17. Walkthrough: `video_editing.cpp`

```cpp
class VideoEditingPlan : public V4DPlan {
    cv::UMat frame_;
    const std::string hv_ = "Hello Video!";
    Property<cv::Size> sz_ = P<cv::Size>(V4D::Keys::SIZE);

public:
    void infer() override {
        capture();

        nvg({
            using namespace cv::v4d::nvg;

            fontSize(40.0f);
            fontFace("sans-bold");
            fillColor(Scalar(255, 0, 0, 255));
            textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            text(sz_.width / 2.0, sz_.height / 2.0,
                 hv_.c_str(), hv_.c_str() + hv_.size());
        }, sz_, R(hv_));

        write();
    }
};
```

The plan does three things every frame:

1. `capture()` pulls a frame from the source.
2. `nvg(...)` draws text over the frame.
3. `write()` pushes the frame to the sink.

The `main()` function initializes the V4D runtime, attaches a source and sink, and runs the plan:

```cpp
int main(int argc, char** argv) {
    cv::Rect viewport(0, 0, 960, 960);

    cv::Ptr<V4D> runtime = V4D::init(
        viewport,
        "Video Editing",
        AllocateFlags::NANOVG | AllocateFlags::IMGUI
    );

    auto src = Source::make(runtime, argv[1]);
    auto sink = Sink::make(runtime, argv[2], src->fps(), viewport.size());

    runtime->setSource(src);
    runtime->setSink(sink);

    V4DPlan::run<VideoEditingPlan>(0);
}
```

Because `0` is passed to `run`, the runtime uses one worker plus the main thread.

For V4D, the main thread handles display and events, while the worker executes the plan graph.

---

## 18. Walkthrough: `beauty-demo.cpp`

`beauty-demo.cpp` is one of the most complete examples in the project.

It demonstrates:

* shared state,
* sub-plans,
* branching,
* mouse events,
* frame counters,
* NanoVG drawing,
* framebuffer access,
* ImGui-based GUI.

### State and properties

A simplified view of the plan state:

```cpp
struct BeautyDemoPlan : public V4DPlan {
    struct Params {
        float eyesAndLipsSaturation_ = 1.25f;
        float skinSaturation_ = 1.35f;
        float skinContrast_ = 0.75f;
        bool sideBySide_ = false;
        bool stretch_ = true;
        bool fullscreen_ = false;
        bool enabled_ = true;

        enum State {
            ON,
            OFF,
            NOT_DETECTED
        } state_ = ON;
    };

    struct Frames {
        cv::UMat orig_;
        cv::UMat stitched_;
        cv::UMat down_;
        cv::UMat bgr_;
        cv::UMat faceOval_;
        cv::UMat eyesAndLips_;
        cv::UMat skin_;
        cv::UMat faceSkinMaskGrey_;
        cv::UMat eyesAndLipsMaskGrey_;
        cv::UMat backgroundMaskGrey_;
        cv::UMat result_;
    };

private:
    static Params params_;
    static FaceFeatures features_;

    cv::Ptr<FaceFeatureExtractor> extractor_;

    float scale_ = 1;
    const cv::Size downSize_ = {640, 360};

    Frames frames_;

    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
    Property<uint64_t> seqCnt_ = P<uint64_t>(GlobalState::Keys::FRAME_CNT);
    Event<Mouse> pressEvents_ = E<Mouse>(Mouse::Type::PRESS);
};
```

Important points:

* `params_` and `features_` are static, so they are shared across threads.
* `frames_` is a non-static member, so it is per-worker.
* `size_` and `seqCnt_` are property edges.
* `pressEvents_` is an event edge.

Because `params_` and `features_` are shared, worker threads and the GUI thread can safely interact with them through shared edge-calls.

### High-level flow

The demo generally does the following each frame:

1. Capture input.
2. Downscale or prepare frames.
3. Detect facial features.
4. Build masks.
5. Apply cosmetic filters.
6. Composite the result.
7. Write to framebuffer and/or sink.
8. Process GUI interaction.

The important lesson is that all of this is expressed as a graph, not as a normal immediate-mode C++ frame loop.

---

## 19. Cheat sheet

### Minimal plan

```cpp
struct MyPlan : Plan {
    void infer() override {
        plain({ std::cout << "frame" << std::endl; });
    }
};

int main() {
    Plan::run<MyPlan>(0);
}
```

### Constants and reads

```cpp
V(42)
R(x)
RW(x)
```

### Shared state

```cpp
_shared(params_);

RS(params_)
RWS(params_)
CS(params_)
```

### Properties

```cpp
Property<uint64_t> frameCnt_ = P<uint64_t>(GlobalState::Keys::FRAME_CNT);
Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
```

### Events

```cpp
Event<Mouse> mousePress_ = E<Mouse>(Mouse::Type::PRESS);
```

### Branching

```cpp
branch(R(x) > V(0));
    plain(doWork);
elseBranch();
    plain(doOtherWork);
endBranch();
```

### Function call

```cpp
auto result = F(&someFunction, R(input));
```

### Assignment

```cpp
assign(RW(x), R(y));
RW(x) = R(y);
```

### Sub-plan

```cpp
sub_ = _sub<SubPlan>(this);

void infer() override {
    subInfer(sub_);
}
```

### Context calls

```cpp
capture();
write();

nvg({ /* draw */ }, size_, R(text_));

gl({ /* OpenGL commands */ });

fb<1>(cv::cvtColor, R(src_), V(cv::COLOR_BGR2RGBA));
```

