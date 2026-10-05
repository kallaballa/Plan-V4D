# Plan-DSL — OpenCV module

A small, embed-in-C++ language for building per-frame computation graphs. The
whole DSL fits in a single header (`opencv2/plan/plan.hpp`) plus a few small
support headers; the runtime is a single translation unit (`src/plan.cpp`).

## What it is

`Plan` is the base class. A plan describes one iteration of a loop by
**recording** it: every call you write inside `setup()`, `infer()` and
`teardown()` emits a *task node* into a list that the runtime then replays every
frame. Side effects are deferred; errors surface at graph-build time.

A plan has four lifecycle methods (see the `Plan` class in `plan.hpp`):

| Method       | When it runs                                     |
|--------------|--------------------------------------------------|
| `setup()`    | once per worker thread, before the frame loop    |
| `infer()`    | once per worker thread — records the per-frame graph |
| `teardown()` | once per worker thread, after the frame loop     |
| `gui()`      | once, on the display thread, before the frame loop |

`gui()` is the exception to the recording model: it is called directly on the
display thread and records nothing. Only an ImGui transaction installed there
survives to be replayed — see the [V4D README](../v4d/README.md).

`Plan::run<Tplan>(extra_workers, args...)` boots the lifecycle. The graph is
built during `infer()` and replayed by `runGraph()` every frame. `extra_workers`
is the number of worker threads to spawn; the display thread always runs on top
of them. `-1` additionally enables OpenCV's own threading
(`cv::setNumThreads(-1)`).

`extra_workers` and `args...` are forwarded to the plan's constructor, so a plan
can be configured at run time:

```cpp
Plan::run<MyPlan>(3, "input.mp4");
```

## Building blocks

* **Edges.** The only value type. Edge-calls `V(x)`, `R(x)`, `RW(x)`,
  `RS(x)`, `RWS(x)`, `CS(x)`, `P<T>(key)`, `E<T>()` describe how a
  node should access storage or a runtime value.
* **Operators.** C++ operators that record nodes (`ADD`, `MUL`,
  `IF`, `IDX`, `DEREF`, …). Use symbol form (`a + b`), named form
  (`ADD(a, b)`) or generic form (`OP<Operators::ADD_>(a, b)`).
  Statement form (`assign(RW(x), R(y))`) for void-result nodes.
* **Functions.** `F(callable, args...)` wraps any C++ callable as a
  node. Non-void results become edges.
* **Control flow.** `branch(pred) / elseBranch() / endBranch()`
  regions. Predicates may be bool edges, predefined predicates
  (`always_`, `isTrue_`, `and_`, `or_`, …), or callable returning
  bool. `BranchType::PARALLEL` (default), `SINGLE`, `ONCE`,
  `PARALLEL_ONCE` select execution semantics.
* **Sub-plans.** Construct with `_sub<T>(parent, args...)`; splice
  into the parent with `subInfer(sub)` / `subSetup(sub)` /
  `subTeardown(sub)`.
* **Shared state.** Per-worker by default. `_shared(member)` gives a
  plan member a mutex; access via `RS` / `RWS` / `CS`. Statics are
  registered automatically and do not need `_shared`.
* **Properties.** `Property<T>` is a typed edge bound to
  `GlobalState` / `LocalState`.
* **Events.** `Event<T>` is an edge producing a vector of input
  events (mouse, keyboard, window, joystick). The DSL core returns
  empty lists; runtimes like V4D plug in a real fetcher.

## Embedding the DSL in another runtime

A runtime implements `PlanRuntime` (see the `PlanRuntime` class in `plan.hpp`).
It is an abstract interface with 21 pure virtuals: nine context accessors
(`plainCtx`, `glCtx`, `fbCtx`, `nvgCtx`, `bgfxCtx`, `extCtx`, `sourceCtx`,
`sinkCtx`, `imguiCtx`), nine matching `has*Ctx()` predicates, `debugFlags()`,
`getViewport()` and `runFrameLoop()`. The rest have defaults:

| Hook | Default | Purpose |
|---|---|---|
| `onRunStart(int32_t workers)` | no-op | on the display thread, after the workers are spawned but before they synchronize — build run-scoped frame-sync primitives here |
| `initWorkerThread(int32_t workerIdx)` | no-op | in each worker, before its plan is built — associate the thread, propagate I/O, name the thread |
| `willGui(const Ptr<Plan>&)` | no-op | on the display thread, just before `gui()` |
| `releaseIo()` | no-op | after the frame loop, on every participating thread |
| `captureInput(const Ptr<Plan>&)` | no-op | emitted before `infer()` — see below |
| `writeOutput(const Ptr<Plan>&)` | no-op | emitted after `infer()` — see below |

`captureInput` and `writeOutput` are how a runtime owns input and output without
the plan having to ask. V4D implements them by recording its own `capture()` and
`write()` nodes on the plan, which is how a configured `Source` reaches the
framebuffer before the graph runs and the framebuffer reaches a `Sink` after it.
The defaults are no-ops, so a runtime without I/O needs to do nothing and a
plain `Plan` stays usable on its own.

[`test/MockPlanRuntime.hpp`](test/MockPlanRuntime.hpp) is the working
implementation of all of the above to copy from — it is the minimum a test or
embedding runtime has to provide.

## Files

```
modules/plan/
├── CMakeLists.txt
├── README.md                  ← this file
├── doc/
│   ├── plan-dsl-programming-guide.markdown
│   └── plan-dsl-reference.markdown
├── include/
│   └── opencv2/
│       └── plan/
│           ├── flags.hpp              AllocateFlags / ConfigFlags / DebugFlags
│           ├── plan.hpp               Plan, PlanRuntime, Edge, Property, Event, …
│           ├── util.hpp               misc helpers (lambda_ptr_hex, PlanSession, …)
│           └── detail/
│               ├── context.hpp        PlainContext, PlanContext (CPU/GPU dispatch)
│               └── transaction.hpp    Transaction, Node
├── perf/
│   ├── perf_main.cpp
│   ├── perf_plan_core.cpp
│   └── perf_precomp.hpp
├── test/
│   ├── test_main.cpp
│   ├── test_plan_core.cpp            edge, operator, branch and node-count tests
│   ├── test_plan_state.cpp           global/local/shared state tests
│   ├── test_plan_runtime.cpp         lifecycle and threading tests
│   ├── TestedPlan.hpp
│   ├── MockPlanRuntime.hpp           a minimal PlanRuntime to test against
│   └── test_precomp.hpp
└── src/
    └── plan.cpp                      thread-local session and state storage
```

## Tests

`opencv_test_plan` holds the accuracy tests (92 across `PlanNodeCount`,
`PlanCore` and friends) and `opencv_perf_plan` the perf tests (37 parameterized
cases). Both are built and run by:

```bash
./build.sh plan              # from the repository root
./build.sh plan -- --gtest_filter=Plan.*
```

## Where to start

1. Read [`doc/plan-dsl-programming-guide.markdown`](doc/plan-dsl-programming-guide.markdown).
   It's a friendly tour through the language, walking through
   `video_editing.cpp` and `beauty-demo.cpp` from the V4D module.
2. Keep [`doc/plan-dsl-reference.markdown`](doc/plan-dsl-reference.markdown)
   open while you write. It's the canonical opcode-by-opcode reference.
3. If you're building a windowed, GPU-capable, source-and-sink
   program, jump to the V4D module — it inherits from `Plan` and
   adds the runtime, the window, the source/sink, and the
   rendering contexts.
4. If you're embedding the DSL in a different runtime, read
   `PlanRuntime` in `plan.hpp` and start from
   [`test/MockPlanRuntime.hpp`](test/MockPlanRuntime.hpp).

## Requirements

* C++20
* OpenCV core, OpenCV imgproc
* A threading library with `<barrier>` and `<semaphore>` (libstdc++
  11+, libc++ 15+, MSVC 19.28+).
* [AnyProperty](https://github.com/kallaballa/AnyProperty) — the header-only
  property map that backs `GlobalState` / `LocalState`. It is pulled in as a git
  submodule at `modules/v4d/third/AnyProperty`, so run
  `git submodule update --init` before configuring. The CMake configure step
  does this for you.

## Building

From the repository root:

```bash
./build.sh plan -b release
```

`./build.sh --help` lists the commands and options.

## License

Apache 2.0, like the rest of OpenCV. See the top-level
[`LICENSE`](../../LICENSE) of this repository.