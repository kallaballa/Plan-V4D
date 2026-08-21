# Plan-DSL Module

**Plan-DSL** (`opencv_plan`) is a C++20 *task-graph language* embedded in C++ — the "Plan" part of
[Plan-V4D](https://github.com/kallaballa/Plan-V4D). Instead of writing imperative frame code, you describe
your program in the lifecycle methods of a `cv::plan::Plan` subclass using a small set of *edge-calls*,
*operators* and *control-flow constructs*. Every call records a **task node** together with its **data
dependencies**; the recorded accesses are compiled into a **directed acyclic graph (DAG)** that is executed
every frame/iteration by a pool of worker threads.

The DSL is deliberately modeled after an **Instruction Set Architecture (ISA)** — operands ("edges"),
instructions ("operators"), structured control flow, and peripheral I/O ("context calls") — so that other
intermediate representations (most notably LLVM IR) can be lowered onto it mechanically. The authoritative
ISA-style contract lives in [`doc/plan-dsl-reference.markdown`](doc/plan-dsl-reference.markdown).

Key properties:

- **Declarative dataflow, imperative recording.** You write ordinary C++; the calls are recorded, not
  executed, and later replayed as a graph.
- **Runtime-decoupled.** The module depends only on `opencv_core` and `opencv_imgproc`. Graphics contexts,
  windowing, input events and video I/O are supplied by a *runtime* that implements the abstract
  `PlanRuntime` interface — such as the [V4D module](../v4d) (OpenGL, NanoVG, bgfx, ImGui, source/sink,
  event polling).
- **Multi-worker by construction.** Each worker thread builds and executes its own copy of the graph.
  Sharing is explicit: shared variables carry mutexes, copy edges produce snapshots, and branch regions can
  be restricted to run once globally, once per worker, or single-threaded.

---

## Table of Contents

1. [Repository Layout](#repository-layout)
2. [Core Concepts](#core-concepts)
3. [Execution Model](#execution-model)
4. [The DSL Surface](#the-dsl-surface)
5. [State Model](#state-model)
6. [Usage Examples](#usage-examples)
7. [Relationship to the V4D Runtime](#relationship-to-the-v4d-runtime)
8. [Debugging and Introspection](#debugging-and-introspection)
9. [Requirements and Build](#requirements-and-build)
10. [Caveats and Design Notes](#caveats-and-design-notes)

---

## Repository Layout

```
modules/plan/
├── CMakeLists.txt                     # OpenCV contrib module build (requires C++20)
├── doc/
│   └── plan-dsl-reference.markdown    # ISA-style API reference (authoritative contract)
├── include/opencv2/plan/
│   ├── plan.hpp                       # Plan, PlanRuntime, edge-calls, operators, control flow,
│   │                                  #   C++ operator overloads (+,-,*,/,%,&&,||,!,==,...)
│   ├── util.hpp                       # type traits, SharedVariables, GlobalState, LocalState,
│   │                                  #   small helpers (cnz, setThreadName, get_epoch_nanos)
│   ├── threadsafeanymap.hpp           # AnyPropertyMap / ThreadSafeAnyMap (heterogeneous,
│   │                                  #   callback-aware property storage), demangle()
│   ├── flags.hpp                      # AllocateFlags, ConfigFlags, DebugFlags
│   └── detail/
│       ├── transaction.hpp            # Operators enum, Edge, Transaction/TransactionImpl,
│       │                              #   Node, BranchType, make_operator_func
│       └── context.hpp                # PlanContext (abstract) / PlainContext (CPU)
├── src/
│   └── plan.cpp                       # non-template Transaction impls, static state definitions,
│                                      #   cnz(), setThreadName()
└── samples/                           # (empty — runnable samples live in modules/v4d/samples/)
```

### Main classes

| Class | Role |
|-------|------|
| `cv::plan::Plan` | Base class of all plans. Provides the entire DSL surface plus the record → `makeGraph()` → `runGraph()` machinery. |
| `cv::plan::PlanRuntime` | Abstract runtime interface: context accessors (`plainCtx()`, `glCtx(i)`, `fbCtx()`, `nvgCtx()`, `bgfxCtx()`, `extCtx(i)`, `sourceCtx()`, `sinkCtx()`, `imguiCtx()`), `debugFlags()` and `getViewport()`. Implemented by V4D. |
| `cv::plan::detail::Edge<T, Tcopy, Tread, Tshared, Tbase, TbyValue>` | The operand type. A typed handle to storage or to a computed value, carrying *access intent*. |
| `cv::plan::detail::Transaction` / `TransactionImpl` | Unit of execution of one node: locks its "lockie" edges, invokes the callable with operand references, performs copy-backs. |
| `cv::plan::detail::Node` | Graph vertex: name, read/write dependency sets, owning transaction. |
| `cv::plan::BranchType` | `NONE`, `SINGLE`, `PARALLEL`, `ONCE`, `PARALLEL_ONCE`. |
| `cv::plan::SharedVariables` | Registry mapping variable addresses → (size, mutex). Backs `_shared`/`_safe`/`RS`/`RWS`/`CS`. |
| `cv::plan::GlobalState` / `LocalState` | Process-wide / thread-local named property tables (`ThreadSafeAnyMap`). |
| `cv::plan::detail::PlanContext` / `PlainContext` | Execution environment of a node. `PlainContext` just runs the callable on the calling (worker) thread. |

---

## Core Concepts

### Plans

A plan is a program (module). You subclass `cv::plan::Plan` (or a runtime-derived class such as
`V4DPlan`) and implement three lifecycle hooks:

| Method | Purpose |
|--------|---------|
| `setup()` | Record the initialization pipeline (one-shot). |
| `infer()` | Record the per-iteration pipeline (the "kernel"/main body). |
| `teardown()` | Record the shutdown pipeline (one-shot). |
| `gui()` | Optional UI hook; only invoked if the runtime provides a GUI. |

Inside these methods you *record* nodes via the DSL. A typical driver then compiles and runs the graph:

```cpp
plan->infer();      // record nodes + dependencies
plan->makeGraph();  // compile accesses_ into currentNodes_
plan->runGraph();   // execute (re-evaluates branch predicates each time)
plan->clearGraph(); // archive nodes, reset recording state
```

A runtime typically builds the graph **once per worker** and then calls `runGraph()` every frame —
branch predicates are re-evaluated on every execution, so behavior stays dynamic even though the graph
structure is static.

Instantiate with `Plan::make<Tplan>(args...)` (or the runtime's equivalent, e.g. `V4DPlan::make`).
Plans have a hierarchical name (`space()` = `parent-name`) used for node identity and sub-plan splicing.

### Edges are operands

An **edge** is the typed handle to either a memory object (a C++ variable, plan member, or runtime
property) or to a computed value (an operator's result). Every edge carries *access intent*:

| Intent | Meaning |
|--------|---------|
| read (`R`, `RS`) | Input only; never mutates. Multiple concurrent readers allowed. |
| read-write (`RW`, `RWS`) | Read-modify-write; orders against other accesses to the same object. |
| copy (`CS`) | Private snapshot under the variable's mutex; optionally copied back afterwards. |
| shared (`RS`, `RWS`, `CS`) | Accesses synchronize on the variable's mutex (registered via `_shared()`, or auto-registered if the address lies outside any plan instance). |

In LLVM terms: a wrapped variable ≈ an `alloca`/`global`; an operator's result edge ≈ an SSA virtual
register; access intent ≈ `readonly`/`writeonly` memory attributes.

### Transactions and nodes

Each recorded call creates one node keyed by a unique id derived from the callable's address, the operand
identities (their storage addresses), the plan name and the worker index — with `'+'` appended on
collision. Dependencies between nodes are the edges they share: reads link producers to consumers, writes
enforce ordering between mutating nodes.

Executing a node means executing its **transaction**: acquire the mutexes of all *lockie* edges at once
(via `std::lock`, i.e. deadlock-free all-or-nothing), invoke the callable with operand references, then
copy back any writeable copy edges.

---

## Execution Model

### Workers

Every worker thread independently builds and runs its own copy of the full graph (the V4D driver, for
instance, constructs one `Plan` instance per worker). The scheduler never migrates partial iterations
between workers. Per-thread identity is available as `LocalState::Keys::WORKER_INDEX`.

### Branch regions

Control flow is expressed with **predicated regions**, not jumps:

```cpp
branch(R(x) == V(0));     // enter region if predicate holds
{ /* nodes */ }
elseBranch();             // complement of the enclosing condition
{ /* nodes */ }
endBranch();              // close region (must match branch exactly)
```

Predicates are re-evaluated on every `runGraph()`. Region types (`BranchType::Enum`):

| Value | Name | Semantics |
|-------|------|-----------|
| `0` | `NONE` | plain node, no region |
| `1` | `SINGLE` | executed by at most one worker (globally locked, serialized) |
| `2` | `PARALLEL` | executed by every enabled worker when the predicate holds (default) |
| `4` | `ONCE` | executed once globally (first worker wins) |
| `8` | `PARALLEL_ONCE` | executed once per worker |

Regions may also be **pinned** to a specific worker: `branch(workerIdx, fn, args...)` only fires when
`LocalState::WORKER_INDEX == workerIdx`. Predefined predicates: `always_`, `isTrue_`, `isFalse_`,
`and_`, `or_`.

Because there is no goto-style control flow, loops are expressed as predicated regions whose predicate
reads values written inside the region body (see `modules/v4d/samples/video_editing.cpp` and
`montage-demo.cpp` for realistic examples).

### Sub-plans

Sub-plans are the function-call analogue:

```cpp
child_ = _sub<MySubPlan>(this, args...);   // create (links space name + address range)
// ...
subSetup(child_);      // splice child's setup() graph into this plan
subInfer(child_);      // splice child's infer() graph
subTeardown(child_);   // splice child's teardown() graph
```

Splicing merges the child's accesses and transactions into the parent graph, so dependencies across the
parent/child boundary are honored.

### Contexts

Nodes execute inside a **context** — a specialized execution environment handed out by the runtime. The
core defines exactly one: the CPU **plain context** (`plainCtx()`), whose `execute()` simply invokes the
callable. All operator nodes attach to it. The generic mechanism is `call(ctx, name, fn, args...)`;
everything else (`plain`, `F`) is sugar over it. Runtimes add further contexts (GL, framebuffer, NanoVG,
bgfx, ImGui, source/sink, custom) — see [below](#relationship-to-the-v4d-runtime).

---

## The DSL Surface

### Edge-calls (operand producers)

| Call | Produces | Notes |
|------|----------|-------|
| `V(value)` | constant/immediate | Wrapped in a `cv::Ptr`; passed by value. |
| `R(var)` | read-only edge | Most parallel-friendly intent — prefer it when you only read. |
| `RW(var)` | read-write edge | Destination of `ASSIGN`, `DEREF`, `NEG`, in-place ops. |
| `RS(var)` | read-shared edge | Shared access (see [State Model](#state-model)); locks the variable's mutex. |
| `RWS(var)` | read-write shared edge | Shared access; locks the mutex. |
| `CS(var)` | copy-shared snapshot | Thread-safe private copy under the mutex. |
| `P<T>(key)` | property edge | Read-only shared edge bound to `GlobalState`/`LocalState`/runtime keys. |
| `E<T>(...)` | event stream edge | `std::vector<std::shared_ptr<T>>` polled per iteration; core yields an empty list, the runtime overrides fetching. |
| `F(fn, args...)` | external call | Returns a result edge if `fn` returns non-void, else `cv::Ptr<Plan>`. Accepts functions, lambdas, member-function pointers. |
| `_(a, b, ...)` | operand tuple | Groups tail operands for n-ary operators: `R(a) + _(R(b), R(c))`. |
| `_shared(var)` | registration | Give `var` (and members within its address range) a mutex. Call from the constructor. |
| `_safe(var)` | opt-out | Mark a variable as never-shared (blocks accidental sharing). |

`Property<T>` (returned by `P<T>`) extends `Edge` and can be passed directly without an edge-call.

### Operators

Operators exist in four forms — **symbol** (`a + b`), **named** (`ADD(a, b)`), **generic**
(`OP<Operators::ADD_>(a, b)`) and **statement** (`op<ADD_>(a, b)`, `assign(dst, src)`,
`construct(dst, args...)`; lowercase = no result edge). Expression forms return a new result edge whose
value is recomputed by the node each iteration.

| Group | Opcodes |
|-------|---------|
| Arithmetic | `ADD` `+`, `SUB` `-`, `MUL` `*`, `DIV` `/`, `MOD` `%`, `NEG` (dst-first), `INCL` `++x`, `INCR` `x++`, `DECL` `--x`, `DECR` `x--` |
| Logical/bitwise | `AND` `&&`, `OR` `\|\|`, `NOT` `!`, `XOR` `^`, `BAND` `&`, `BOR` `\|`, `SHL` `<<`, `SHR` `>>` |
| Comparison | `EQ` `==`, `NEQ` `!=`, `LT` `<`, `GT` `>`, `LE` `<=`, `GE` `>=` |
| Select/memory/construction | `IF(c,t,f)` (eager select), `IDX` `[]`, `DEREF` (dst-first), `ASSIGN` `=`, `CONSTRUCT` via `plan(args...)` |

Notes:

- N-ary forms fold the tail right-associatively (`SUB(a,b,c)` = `a - (b - c)`); binary use is exact.
- Unary `-x` expands to `x * V(-1)`; `operator\|` maps to bitwise-or (`BOR`), matching C++ semantics.
- `DEREF(RW(dst), R(ptr))` and `NEG(RW(dst), R(src))` take the destination first.
- `CONSTRUCT` value-initializes, raw-news or `makePtr`s depending on the destination edge type.

See §3 and §8 of the [API reference](doc/plan-dsl-reference.markdown) for the full opcode tables including
LLVM IR equivalents.

---

## State Model

| Concept | DSL entity | Notes |
|---------|------------|-------|
| Plan member | plain C++ member passed as `R`/`RW` | Private per plan object; each worker owns a plan instance graph. |
| Shared variable | `_shared(x)` + `RS`/`RWS`/`CS` | One `std::mutex` per registered object; nested members share the parent's mutex (address-range lookup). Addresses **outside** any plan instance (globals, statics, heap objects) are auto-registered on first shared access — this is why V4D samples simply declare `static` members. |
| Safe variable | `_safe(x)` | Explicitly excluded from sharing; `RS`/`RWS`/`CS` on it throws. |
| Named global state | `GlobalState::get/set/create<V>(key)` + `P<T>(key)` | Core keys: `FRAME_CNT`, `CAPTURE_CNT`, `FPS_CNT`, `RUN_CNT`, `START_TIME`, `FPS`, `WORKERS_READY`, `WORKERS_STARTED`, `LOCKING`, `DISPLAY_READY`, `LOCK_CONTENTION_*`, `SHOW_GUI`, `TIME_TRACKER`. Optional change callbacks fire on writes. |
| Per-worker state | `LocalState::Keys::WORKER_INDEX` | Thread-local property table. |

Property storage is provided by `AnyPropertyMap`/`ThreadSafeAnyMap`
(`threadsafeanymap.hpp`): a vector of `std::any` values keyed by enum index, with write-protection for
read-only entries, type-checked access, and change callbacks.

---

## Usage Examples

### Minimal standalone program (CPU-only runtime)

The module itself ships no window system — implement `PlanRuntime` with a single `PlainContext`:

```cpp
#include <opencv2/plan/plan.hpp>

using namespace cv::plan;

class CpuRuntime : public PlanRuntime {
    cv::Ptr<detail::PlainContext> ctx_ = new detail::PlainContext();
public:
    cv::Ptr<detail::PlainContext> plainCtx() override { return ctx_; }
    cv::Ptr<detail::PlanContext> glCtx(int32_t) override { return nullptr; }
    cv::Ptr<detail::PlanContext> fbCtx() override { return nullptr; }
    cv::Ptr<detail::PlanContext> nvgCtx() override { return nullptr; }
    cv::Ptr<detail::PlanContext> bgfxCtx() override { return nullptr; }
    cv::Ptr<detail::PlanContext> extCtx(int32_t) override { return nullptr; }
    cv::Ptr<detail::PlanContext> sourceCtx() override { return nullptr; }
    cv::Ptr<detail::PlanContext> sinkCtx() override { return nullptr; }
    cv::Ptr<detail::PlanContext> imguiCtx() override { return nullptr; }

    bool hasPlainCtx() override { return true; }
    bool hasGlCtx(uint32_t) override { return false; }
    bool hasFbCtx() override { return false; }
    bool hasNvgCtx() override { return false; }
    bool hasBgfxCtx() override { return false; }
    bool hasExtCtx(uint32_t) override { return false; }
    bool hasSourceCtx() override { return false; }
    bool hasSinkCtx() override { return false; }
    bool hasImguiCtx() override { return false; }

    uint32_t debugFlags() const override { return 0; }
    cv::Rect getViewport() const override { return {0, 0, 1280, 720}; }
};

class MyPlan : public Plan {
    float x_ = 7.0f, y_ = 5.0f;
    float sum_ = 0.0f;
    uint64_t frames_ = 0;
public:
    void infer() override {
        // statement node: frames_ += 1
        plain([](uint64_t& n) { ++n; }, RW(frames_));

        // expression tree -> result edge, recomputed every iteration
        auto sum = ADD(R(x_), R(y_));

        // statement node: sum_ = sum
        assign(RW(sum_), sum);

        // predicated region
        branch(R(sum_) > V(10.0f))
            ->plain([](float& s) { s = 0.0f; }, RW(sum_))
        .endBranch();
    }
};

int main() {
    auto rt = cv::makePtr<CpuRuntime>();
    auto plan = Plan::make<MyPlan>();
    plan->setRuntime(rt);

    for (int i = 0; i < 100; ++i) {
        plan->infer();      // record
        plan->makeGraph();  // compile DAG
        plan->runGraph();   // execute (a real runtime does this on N workers)
        plan->clearGraph();
    }
}
```

### Shared state across workers

Because each worker owns its own plan instance, cross-worker state must live outside plan instances —
the idiomatic pattern is a `static` member, which is auto-registered as shared on first `RS`/`RWS`/`CS`
access:

```cpp
class SharedPlan : public Plan {
    // static => one object for the whole process; auto-registered as shared because
    // its address lies outside any plan instance's address range
    struct Params { float gain_ = 1.0f; bool enabled_ = true; };
    static Params params_;
public:
    void infer() override {
        // read-modify-write under params_' mutex (serialized across workers)
        plain([](Params& p) { p.gain_ *= 2.0f; }, RWS(params_));

        // snapshot reads: no lock held while the node runs
        branch(CS(params_.enabled_))
            ->plain([](const float& g) { /* ... */ }, CS(params_.gain_))
        .endBranch();
    }
};
SharedPlan::Params SharedPlan::params_;
```

Explicit registration with `_shared(var)` is available for globals/heap objects you want to share
without relying on auto-registration; `_safe(var)` permanently opts a variable out.

### With the V4D runtime (typical application code)

```cpp
class VideoEditingPlan : public V4DPlan {
    cv::UMat frame_;
    const string hv_ = "Hello Video!";
    Property<cv::Size> sz_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    void infer() override {
        capture();                                   // source context -> frame_
        nvg([](const Size& sz, const string& str) {  // NanoVG vector graphics context
            using namespace cv::v4d::nvg;
            fontSize(40.0f); fontFace("sans-bold");
            fillColor(Scalar(255, 0, 0, 255));
            textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            text(sz.width / 2.0, sz.height / 2.0, str.c_str(), str.c_str() + str.size());
        }, sz_, R(hv_));
        write();                                     // framebuffer -> sink context
    }
};

int main() {
    Ptr<V4D> runtime = V4D::init({0, 0, 960, 960}, "Video Editing",
                                 AllocateFlags::NANOVG | AllocateFlags::IMGUI);
    runtime->setSource(Source::make(runtime, "input.mp4"));
    runtime->setSink(Sink::make(runtime, "output.mp4", 30, {960, 960}));
    V4DPlan::run<VideoEditingPlan>(0);               // one worker thread + display thread
}
```

More elaborate patterns — nested branches, pinned workers, `SINGLE`/`ONCE` regions, events
(`E<Mouse>(Mouse::MOVE)`), sub-plans composing whole demos (`montage-demo.cpp`) — can be found in
[`modules/v4d/samples/`](../v4d/samples).

---

## Relationship to the V4D Runtime

This module contains only the DSL core. Everything concrete comes from a runtime implementing
`PlanRuntime`:

- **[V4D](../v4d)** implements the interface and adds, on top of `V4DPlan`:
  - named context calls: `gl(...)` (raw OpenGL), `fb(...)` (framebuffer access),
    `nvg(...)` (NanoVG vector graphics), `bgfx(...)`, `imgui(...)`, `ext(...)` (custom contexts),
    `capture(...)`/`write(...)` (video source/sink), `clear(...)`, `set(key, edge)` (property writes);
  - event sources: `E<Mouse>`, `E<Keyboard>`, `E<Window>`, `E<Joystick>`;
  - additional properties (`V4D::Keys`: `SIZE`, `VIEWPORT`, `WINDOW_SIZE`, `FRAMEBUFFER_SIZE`,
    `CLEAR_COLOR`, `FULLSCREEN`, ...);
  - the driver loop: `V4DPlan::run<Tplan>(workers, args...)` spawns worker threads (each building and
    running the graph), synchronizes setup/infer via a barrier, and enters the display loop.

The flags in `flags.hpp` (`AllocateFlags`, `ConfigFlags`, `DebugFlags`) are consumed by the runtime at
initialization time.

---

## Debugging and Introspection

`DebugFlags` (interpreted by the runtime):

| Flag | Effect |
|------|--------|
| `ONSCREEN_CONTEXTS` | Create on-screen GL contexts even in offscreen mode. |
| `PRINT_CONTROL_FLOW` | Print the executed branch structure per iteration (via the `pf()` hook that runtimes override). |
| `DEBUG_GL_CONTEXT` | Enable GL debug output. |
| `PRINT_LOCK_CONTENTION` | Report shared-variable lock contention. |
| `MONITOR_RUNTIME_PROPERTIES` | Log every runtime property write. |
| `LOWER_WORKER_PRIORITY` | Lower worker thread niceness (Linux). |
| `DONT_PAUSE_LOG` | Don't temporarily reduce log verbosity during startup. |

Useful helpers: `cnz(cv::Mat/UMat)` (channel-aware countNonZero), `setThreadName()` (Linux),
`get_epoch_nanos()`. OpenCV logging is used throughout (`CV_LOG_*`), and node names embed the plan name,
callable address and operand identities, which makes control-flow dumps readable.

---

## Requirements and Build

- **C++20** required (`<barrier>`, concepts-free but trait-heavy template metaprogramming). The module
  disables itself at CMake time if the compiler lacks `cxx_std_20`.
- **OpenCV** `core` and `imgproc` (this is a standard OpenCV contrib-style module).
- GCC/Clang recommended: symbol demangling uses `abi::__cxa_demangle` (`__GNUG__`); thread naming uses
  `pthread_setname_np` (Linux).

Build as part of the OpenCV superbuild:

```bash
cmake -DOPENCV_EXTRA_MODULES_PATH=<Plan-V4D>/modules <opencv_source_dir>
# disable explicitly if needed:
cmake -DBUILD_opencv_plan=OFF ...
```

Include from user code:

```cpp
#include <opencv2/plan/plan.hpp>   // namespace cv::plan
```

---

## Caveats and Design Notes

- **Recording order matters.** Nodes execute in recording order within a worker; the read/write
  dependency sets captured per node document the dataflow (and are the basis for tooling/lowering), but
  the scheduler itself is order-driven, not topological.
- **`IF` is eager.** Both select arms are computed as ordinary operands. For lazy evaluation use
  `branch` regions instead.
- **N-ary folds right.** `SUB(a,b,c)` computes `a - (b - c)`. Emit one op per binary instruction when
  lowering from IR.
- **Sharing must be declared (or be external).** `RS`/`RWS`/`CS` on a *plan member* that was never
  registered via `_shared()` throws; addresses outside plan instances are auto-registered; `_safe`
  permanently excludes a variable from sharing.
- **One graph per worker.** Each worker records its own copy of the graph; cross-worker coordination
  happens exclusively through shared variables, `SINGLE`/`ONCE` branch semantics and global properties.
- **Structured control flow only.** Arbitrary jumps cannot be expressed; irreducible CFGs must be
  restructured into balanced `branch`/`endBranch` regions before lowering.

---

## Documentation

- [Plan-DSL API Reference (ISA-style)](doc/plan-dsl-reference.markdown) — the complete instruction set:
  edge-calls, opcode tables with LLVM IR equivalents, control flow, state model, and lowering notes.
- [V4D module](../v4d) — the reference runtime, with runnable samples in [`modules/v4d/samples/`](../v4d/samples).

## License

This file is part of the OpenCV project and licensed under the Apache License 2.0 (see the LICENSE file
at the top-level directory or http://opencv.org/license.html).
Copyright Amir Hassan (kallaballa) &lt;amir@viel-zu.org&gt;.
