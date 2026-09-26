# Plan-V4D Threading Model

## Executive Summary

**Plan-V4D fully supports running multiple Plans with multiple windows in parallel within the same process.** This is a first-class, explicitly designed feature — not an accidental side effect. The canonical proof is `modules/v4d/samples/two-windows-demo.cpp`, which demonstrates two `TrianglePlan` instances running simultaneously: one on the main thread and one on a dedicated `std::thread`, each with its own window, frame counter, GUI, and input queue.

---

## 1. Project Structure Overview

```
Plan-V4D/
├── modules/plan/                          ← Core Plan-DSL (graph language)
│   ├── include/opencv2/plan/
│   │   ├── plan.hpp                       ← Plan, PlanRuntime, PlanSession, GlobalState
│   │   ├── util.hpp                       ← PlanSession, GlobalState, LocalState
│   │   ├── flags.hpp                      ← AllocateFlags, ConfigFlags, DebugFlags
│   │   └── threadsafeanymap.hpp           ← Thread-safe property maps
│   └── src/plan.cpp                       ← PlanRuntime lifecycle helpers
│
└── modules/v4d/                           ← V4D graphics runtime (GLFW + OpenGL + NanoVG + ImGui)
    ├── include/opencv2/v4d/
    │   ├── v4d.hpp                        ← V4D (runtime), V4DPlan (Plan subclass)
    │   ├── source.hpp, sink.hpp           ← I/O
    │   ├── nvg.hpp                        ← NanoVG wrapper
    │   ├── events.hpp                     ← GLFW event system
    │   └── util.hpp                       ← keep_running, request_finish, signal handlers
    └── src/v4d.cpp                        ← Runtime lifecycle (run, display, initWorkerThread)
```

---

## 2. Plan Lifecycle

A `Plan` is a C++ class whose four lifecycle methods **record** (not execute) a per-frame computation graph:

| Method | When |
|--------|------|
| `setup()` | Once per worker thread, before the frame loop |
| `infer()` | Once per worker thread — records the per-frame graph |
| `teardown()` | Once per worker thread, after the frame loop |
| `gui()` | Once, on the main/display thread, before the frame loop |

`Plan::run<Tplan>(extra_workers, args...)` (`plan.hpp:1157`) orchestrates the full lifecycle:

1. **Determines role**: decides if the calling thread is the display thread or a worker thread (`plan.hpp:1172–1175`)
2. **Creates a `PlanSession`**: the display thread owns it; workers inherit it via `GlobalState::setSession(session)` (`plan.hpp:1171–1176`)
3. **Spawns N worker threads** (`plan.hpp:1222–1243`)
4. **Calls `gui()`** once on the display thread (`plan.hpp:1252–1253`)
5. **Calls `setup()` → `makeGraph()` → `runGraph()` → `clearGraph()`** on each worker (`plan.hpp:1261–1264`)
6. **Calls `infer()` → `makeGraph()`** on each worker to build the per-frame graph (`plan.hpp:1277–1278`)
7. **Waits at a `std::barrier`** for all workers to be ready (`plan.hpp:1286`)
8. **Enters the frame loop**: `runtime()->runFrameLoop(plan)` calls `plan->runGraph()` every frame (`plan.hpp:1289–1291`)
9. **On shutdown**: calls `teardown()` → graph → `runGraph()` → `clearGraph()` per worker (`plan.hpp:1300–1303`)
10. **Joins all worker threads** on the display thread (`plan.hpp:1317–1319`)

---

## 3. V4D Windowed Runtime

`V4DPlan` extends `Plan` with a GLFW+OpenGL window, NanoVG, ImGui, Sources/Sinks. Each `V4D` runtime owns one window and one OpenGL context.

### Key Architectural Properties

| Property | Location | Significance |
|----------|----------|-------------|
| `V4D::instance_` is `thread_local` | `v4d.hpp:136` | Each thread has its own runtime |
| `V4D::instance_mtx_` | `v4d.hpp:135` | Guards first `init()` call per thread |
| `RunState` — per-run frame sync | `v4d.hpp:148–166` | Completely isolated between plans |
| `keepRunning` — per-run atomic | `v4d.hpp:160` | Closing one window stops only that plan |
| `requestFinish()` — per-run | `v4d.hpp:416–419` | Stops only this runtime's plan |

### V4D Lifecycle

```cpp
// 1. Create a runtime for this thread (sets thread-local instance)
cv::Ptr<V4D> rt = V4D::init(viewport, "Title", AllocateFlags::NANOVG | AllocateFlags::IMGUI);

// 2. Run the plan — spawns workers, runs the frame loop
V4DPlan::run<MyPlan>(extraWorkers, args...);
```

`V4D::init()` (`v4d.cpp:33–43`) creates a new `V4D` instance and stores it in the calling thread's `instance_`. Each plan runs on its own thread (or set of threads), so it automatically gets its own runtime.

`V4D::run()` (`v4d.cpp:490–576`) implements the frame loop:
- **Display thread**: `glfwPollEvents()` → `runtime->display()` → blit framebuffer → `glfwSwapBuffers()`
- **Worker threads**: `event::poll()` → `runGraph()` → semaphore-synchronized display

---

## 4. PlanSession — The Keystone of Multi-Plan Isolation

### The Problem It Solves

> *"Everything the engine needs to know about 'this run' rather than about the process used to be a process-wide static: one property map, one 'main' thread id, one node-lock table, one set of fired once-branches, one setup semaphore and one frame barrier. That is what made a second concurrent plan impossible — it either deadlocked on a barrier sized for the first plan or stole its frame counters and node locks."*
>
> — `util.hpp:530–546`

### PlanSession State

**File:** `modules/plan/include/opencv2/plan/util.hpp:547–766`

| State | Purpose |
|-------|---------|
| `ThreadSafeAnyMap<Keys::Enum> map_` | Per-run property map (FRAME_CNT, FPS, WORKERS_READY, etc.) |
| `std::thread::id mainThreadID_` | Which thread is the display thread for this run |
| `std::unique_ptr<std::binary_semaphore> setupSema_` | Per-run setup synchronization |
| `std::unique_ptr<std::barrier<>> syncPoint_` | Per-run startup barrier (sized for exactly this run's participants) |
| `std::set<string> once_` | Per-run ONCE-branch tracking |
| `std::map<string, ...> nodeLockMap_` | Per-run SINGLE-branch locks |

Two concurrent plans therefore **never share** any of this state.

---

## 5. How Parallel Plans Work Together

### Threading Model Per Plan

Each plan uses a **1 + N thread model**:
- **1 display thread**: owns the window, runs the frame loop, calls `gui()`
- **N worker threads**: each runs `setup()` → `infer()` → frame loop → `teardown()`

Workers synchronize via the plan's own `PlanSession`:
- A `std::barrier<>` at startup ensures all workers are ready before the frame loop
- A `std::binary_semaphore` serializes the `setup()` phase
- Frame synchronization uses `RunState::frameSyncRender` / `frameSyncSemaSwap` counting semaphores

### Isolation Between Plans

| Resource | Per-Plan or Process-Wide? |
|----------|--------------------------|
| `PlanSession` | **Per-plan** |
| `V4D::RunState` | **Per-plan** |
| `V4D::instance_` | **Per-thread** (thread_local) |
| `V4D::properties_` | **Per-thread** (thread_local) |
| Worker threads | **Per-plan** |
| Frame barrier | **Per-plan** |
| Node lock map | **Per-plan** |
| ONCE-branch tracking | **Per-plan** |
| OpenCV thread pool (`cv::setNumThreads`) | **Process-wide** (set once, protected by `std::call_once`) |
| ImGui font atlas | **Process-wide limitation** — only one plan can use `AllocateFlags::IMGUI` |
| `windowRegistry_` | **Process-wide but thread-safe** — used only for GLFW callback routing |
| SIGINT/SIGTERM handler | **Process-wide** — intentional; stops all plans |

---

## 6. The Canonical Example: `two-windows-demo.cpp`

**File:** `modules/v4d/samples/two-windows-demo.cpp`

```cpp
// Lines 6–9: Explicit documentation
// Two plans, two windows, one process. One plan runs on the main thread,
// the other one on a dedicated thread. Both keep their own frame counters,
// their own frame synchronization, their own GUI and their own input queue,
// and closing one window ends that plan only - the other one keeps running.

// Lines 115–127: Parallel plan execution
std::thread second([maxFrames]() {
    runPlanInThread<TrianglePlan>("Threaded triangle",
        cv::Rect(0, 0, 480, 360),
        cv::Scalar(80, 200, 255, 255), maxFrames);
});

// The main thread displays the second window while the first one is running.
V4D::init(cv::Rect(0, 0, 480, 360), "Main triangle",
    AllocateFlags::NANOVG | AllocateFlags::IMGUI, ConfigFlags::DISPLAY_MODE);
V4DPlan::run<TrianglePlan>(extraWorkers, "Main triangle",
    cv::Scalar(255, 160, 80, 255), maxFrames);

second.join();
```

### Important Note on ImGui

Prior to the FreeType integration, only one plan per process could use `AllocateFlags::IMGUI` because ImGui's embedded stb_truetype decompressor kept state in process-wide statics. The `two-windows-demo.cpp` comments this explicitly (`two-windows-demo.cpp:94–97`).

This limitation has been removed by enabling `IMGUI_ENABLE_FREETYPE` in `modules/v4d/third/imgui/imconfig.h` and linking against FreeType. The FreeType font rasterizer does not rely on the process-wide statics that stb_truetype uses, so multiple plans can now use ImGui concurrently. The second plan in `two-windows-demo.cpp` can now use `AllocateFlags::NANOVG | AllocateFlags::IMGUI` as well.

---

## 7. Key Synchronization Points and Blocking Calls

### During Startup Only (`plan.hpp:1184–1189`)

```cpp
static std::mutex runMtx;
std::lock_guard<std::mutex> lock(runMtx);
```

> *"Only guards plan construction and worker spawning. Held for as long as it takes to hand work to the workers, never across a frame loop, so plans running in parallel only serialize on start."*

### OpenCV Thread Pool (`plan.hpp:1193–1200`)

```cpp
static std::once_flag cvThreadsOnce;
std::call_once(cvThreadsOnce, [extra_workers]() {
    cv::setNumThreads(extra_workers == -1 ? -1 : 0);
});
```

The first plan to run decides the OpenCV thread pool size. Protected by `std::call_once` — no race condition.

### Window Registry (`v4d.hpp:174–175`)

```cpp
static std::mutex windowRegistry_mtx_;
static std::map<GLFWwindow*, V4D*> windowRegistry_;
```

Process-wide registry, but protected by a mutex. Used only for GLFW/ImGui callback routing.

### Process-Wide Shutdown (`v4d/src/util.cpp:331–443`)

`SIGINT`/`SIGTERM` set a process-wide `g_finish_requested` atomic. This is intentional — a signal goes to the whole process, so it must stop all plans.

Use `V4D::requestFinish()` (per-run) vs `request_finish()` (process-wide) accordingly.

---

## 8. Summary

| Question | Answer |
|----------|--------|
| **Can multiple Plans run in parallel?** | **Yes.** Explicitly supported and demonstrated by `two-windows-demo.cpp`. |
| **Can multiple windows be open simultaneously?** | **Yes.** Each plan gets its own `V4D` runtime and its own GLFW window. |
| **Do plans run on separate threads?** | **Yes.** Each plan runs on its own display thread + worker threads. |
| **Is there any shared mutable state between concurrent plans?** | **No.** All per-run state lives in `PlanSession` and `RunState`, which are created per `Plan::run()` call. |
| **Are there any blocking serialization points?** | **Only at startup** (`runMtx` for plan construction/worker spawning). The frame loop runs completely in parallel. |
| **Does closing one window stop all plans?** | **No.** `requestFinish()` sets only that plan's `RunState::keepRunning`. Other plans continue. |
| **Can all plans use ImGui?** | **No.** Due to process-wide stb_truetype statics in ImGui, only one plan per process can use `AllocateFlags::IMGUI`. |

### How It Works (Bottom Line)

1. Each `Plan::run()` creates a new `PlanSession` and a new `V4D::RunState`
2. Each plan runs on its own thread(s): 1 display thread + N worker threads
3. Each thread gets its own `V4D` instance via thread-local storage (`V4D::instance_`)
4. Workers within a plan share the plan's session and run state, synchronizing via barriers and semaphores
5. Two concurrent plans have completely isolated sessions, run states, property maps, barriers, and frame counters

---

## 9. Known Flaws and Limitations

### 9.1 Exception in worker `setup()` leaks the setup semaphore and deadlocks all other workers

**Location:** `plan.hpp:1260–1265`

If `plan->setup()`, `makeGraph()`, `runGraph()`, or `clearGraph()` throws inside a worker thread, the catch block logs the error but **never releases `session->setupSemaphore()`**. Every other worker waiting on that binary semaphore hangs forever. There is no RAII guard or `try`/`finally` around the acquire/release pair.

```cpp
session->setupSemaphore().acquire();
plan->setup();
plan->makeGraph();
plan->runGraph();
plan->clearGraph();
session->setupSemaphore().release();  // never reached if any call above throws
```

**Impact:** A single buggy `setup()` in one worker kills the entire plan (all workers deadlock). The display thread also waits at `syncPoint().arrive_and_wait()` (`plan.hpp:1286`) and never proceeds.

**Workaround:** Do not let `setup()` throw. Catch internally and set an error flag.

---

### 9.2 A worker starting a sub-plan inherits the parent session and deadlocks on the barrier

**Location:** `plan.hpp:1172–1176`, `util.hpp:745–753`

If a worker thread calls `Plan::run()` (e.g. from `infer()` or `setup()`), `previousSession` is already set, so `displayThread = false` and the new plan inherits the parent's `PlanSession`. `ensureSyncPoint()` only creates the barrier if it does not already exist — it never resizes it.

```cpp
const std::shared_ptr<PlanSession> previousSession = GlobalState::currentSessionRef();
const bool displayThread = !previousSession || GlobalState::isMain();
std::shared_ptr<PlanSession> session = displayThread
    ? std::make_shared<PlanSession>(std::this_thread::get_id())
    : previousSession;   // <-- worker inherits parent session
```

The sub-plan arrives at a barrier sized for the parent's thread count, causing a permanent hang. The canonical demo (`two-windows-demo.cpp`) avoids this by always launching plans from a fresh `std::thread`, but nothing in the API prevents a worker from doing it.

**Impact:** Nested `Plan::run()` from within a worker is a deadlock trap.

**Workaround:** Always start plans from a thread that is not already participating in a run.

---

### 9.3 `windowRegistry_` use-after-free race on window destruction

**Location:** `v4d.cpp:25–31`, `v4d.cpp:96–119`

`runtimeForWindow()` looks up a raw `V4D*` from the static registry under a mutex, but the returned pointer is not protected against concurrent destruction. If a GLFW callback is in flight for a window that is being torn down, the callback can dereference a `V4D*` whose destructor has already run and erased the registry entry.

```cpp
V4D* V4D::runtimeForWindow(GLFWwindow* window) {
    std::lock_guard guard(windowRegistry_mtx_);
    auto it = windowRegistry_.find(window);
    if(it == windowRegistry_.end())
        return nullptr;
    return it->second;   // raw pointer; V4D may be destroyed before caller uses it
}
```

The registry lock does not extend to the callback's use of the pointer. In practice, GLFW typically delivers the close event synchronously during `poll()`, but the pattern is fragile.

**Impact:** Potential crash on window close if a callback is dispatched during or after `~V4D()`.

---

### 9.4 No timeout or recovery on synchronization primitives

**Locations:**
- `plan.hpp:1286` — `session->syncPoint().arrive_and_wait()`
- `v4d.cpp:503–538` — `state.frameSyncSemaSwap.acquire()`, `state.frameSyncRender.acquire()`
- `plan.hpp:1260` — `session->setupSemaphore().acquire()`

All semaphore `acquire()` and barrier `arrive_and_wait()` calls have **no timeout**. If a single worker crashes, enters an infinite loop, or is preempted for too long, every other participant in that run deadlocks with no escape hatch. The only recovery is process termination.

**Impact:** A single misbehaving worker or stalled pipeline blocks an entire plan indefinitely.

---

### 9.5 Sub-plan inference is not thread-safe at the API level

**Location:** `test/test_plan_core.cpp:851–867`

The test `sub_plan_subInfer` shows that sub-plans are spliced at graph-build time. There is no documented guarantee that a sub-plan's `infer()` can be called concurrently from multiple workers, or that a sub-plan's internal state is protected if two parent plans share the same sub-plan definition. The test runs single-threaded.

**Impact:** Sub-plans that hold mutable state are likely not safe for concurrent use across workers or across parent plans.

---

## 10. Design Limitations (Not Bugs, But Constraints)

| Limitation | Root Cause |
|---|---|
| OpenCV thread pool size is fixed by the first plan | `plan.hpp:1197–1200` uses `std::call_once` — whichever plan starts first wins |
| `runMtx` serializes plan startup | Brief, but two plans cannot spawn workers truly concurrently (`plan.hpp:1184–1189`) |
| No built-in inter-plan communication | Each plan is fully isolated; sharing state requires external synchronization |
| `glfwPollEvents()` is serialized process-wide | `events.hpp:1228` — all plans contend on `detail::poll_mtx`; only one thread polls events at a time |
| ImGui font atlas build is not parallel-safe without FreeType | Default stb_truetype backend uses process-wide statics; fixed by enabling `IMGUI_ENABLE_FREETYPE` |
