# V4D Application Programming Tutorial

A hands-on, step-by-step guide to writing video, image, GPU and GUI applications with Plan-V4D.

**Audience.** You are comfortable with modern C++ (C++20) and OpenCV's `cv::Mat`/`cv::UMat`, and you know the basic shape of a video pipeline (capture → process → display/write). You do not need to know OpenGL, NanoVG or ImGui — this tutorial teaches the parts you need.

**Companion documents.** This tutorial is the guided path through the material in the Plan-DSL Programming Guide and the Plan-DSL Reference (the ISA-style contract). Where this tutorial and the plan-dsl documents disagree, the plan-dsl documents win. The samples in `modules/v4d/samples/` are the ultimate source of truth for V4D-layer behavior.

**Table of Contents**

1. [What is V4D?](#1-what-is-v4d)
2. [The One Mental Model: Record Once, Replay Forever](#2-the-one-mental-model-record-once-replay-forever)
3. [Milestone 1 — Hello, NanoVG](#3-milestone-1--hello-nanovg)
4. [Milestone 2 — The Shape of a V4D Application](#4-milestone-2--the-shape-of-a-v4d-application)
5. [Milestone 3 — Plan-DSL Crash Course](#5-milestone-3--plan-dsl-crash-course)
6. [Milestone 4 — Video In / Video Out](#6-milestone-4--video-in--video-out)
7. [Milestone 5 — Drawing on Top of Video](#7-milestone-5--drawing-on-top-of-video)
8. [Milestone 6 — Pixel Access with `fb(...)`](#8-milestone-6--pixel-access-with-fb)
9. [Milestone 7 — GUIs with ImGui](#9-milestone-7--guis-with-imgui)
10. [Milestone 8 — Events and Properties](#10-milestone-8--events-and-properties)
11. [Milestone 9 — Raw OpenGL (Optional)](#11-milestone-9--raw-opengl-optional)
12. [Milestone 10 — Going Big: Sub-plans, Threads, Shared State](#12-milestone-10--going-big-sub-plans-threads-shared-state)
13. [Capstone Project — Chroma, a Complete Video Filter App](#13-capstone-project--chroma-a-complete-video-filter-app)
14. [Debugging and Common Pitfalls](#14-debugging-and-common-pitfalls)
15. [Where to Go Next](#15-where-to-go-next)
16. [Appendix — Cheat Sheet](#16-appendix--cheat-sheet)

---

## 1. What is V4D?

V4D (Visualization for Video and Data) is a runtime built on top of Plan-DSL, a small graph-recording language embedded in C++. Plan-DSL is the core; V4D adds four things on top of it:

1. A **window and event loop** built on GLFW + OpenGL, with optional NanoVG (2D vector graphics) and ImGui (immediate-mode GUI) layers.
2. A **Source / Sink abstraction** — a `Plan` can read frames from a video file, a webcam, or any functor (`Source`), and write them to a file, a stream, or anything else (`Sink`).
3. **Side-effect contexts** — a way to schedule a C++ lambda or function onto a specific pipeline: the framebuffer (`fb`), NanoVG (`nvg`), OpenGL (`gl`), bgfx / external renderers (`bgfx`, `ext`), ImGui (`imgui`), or plain CPU (`plain`).
4. **`V4D::Keys` properties** — typed views onto runtime state (framebuffer size, viewport, fullscreen flag, etc.).

That is the whole system. Everything else in this tutorial is elaboration.

**What you will build**

| Milestone | You will build |
|---|---|
| 1 | A window that renders “Hello World” with NanoVG |
| 4 | A video-file → grayscale → video-file converter |
| 5 | A text overlay on live video |
| 6 | A per-pixel threshold effect using framebuffer access |
| 7 | A GUI with sliders controlling the effect |
| 8 | Mouse-driven toggling and runtime property writes |
| 13 | **Chroma** — a complete app combining everything |

---

## 2. The One Mental Model: Record Once, Replay Forever

Read this section twice. It is the single most important idea in V4D.

A `Plan` is a C++ class whose methods — `setup()`, `infer()`, `teardown()`, `gui()` — do not *execute* your code. They *record* it as a list of task nodes. The runtime then replays that recorded list every frame on worker threads.

```
┌───────────────────────── BUILD PHASE (once per worker) ─────────────────────────┐
│  setup()    → records one-shot init graph   → makeGraph → runGraph → clearGraph │
│  infer()    → records the per-frame graph   → makeGraph                         │
├───────────────────────── FRAME LOOP (forever) ──────────────────────────────────┤
│  every frame: runGraph()  ← re-executes the SAME recorded node list             │
├───────────────────────── SHUTDOWN (once per worker) ────────────────────────────┤
│  teardown() → records one-shot cleanup graph → makeGraph → runGraph → clearGraph│
└──────────────────────────────────────────────────────────────────────────────────┘
```

Key consequences:

- **There is no `while (true)`, no `update()`, no `glfwPollEvents()`.** The runtime drives the loop. You describe one iteration of the loop in `infer()`.
- **Nodes execute sequentially, in recording order.** `runGraph()` iterates the recorded node list in the order you wrote the calls. There is no data-dependency scheduler, no work stealing, and no automatic vectorization; dependency metadata is bookkeeping, not scheduling.
- **The graph structure is fixed at build time.** You cannot emit different nodes depending on runtime data. Runtime decisions are expressed with `branch(...)` regions whose predicates are re-evaluated every frame.
- **Side effects written directly in `infer()` (outside a node) happen once, at build time.** If you want something to happen every frame, it must be inside a node: `plain(...)`, `nvg(...)`, `fb(...)`, an operator, etc.
- **Each worker thread builds and runs its own independent copy of the graph.** Workers never share nodes, share partial iterations, or migrate work. Think of `infer()` as the per-thread body of an OpenMP `#pragma omp parallel` region.

A good analogy: Plan-DSL is a *CPU-side shader* for a per-frame computation graph. It reads inputs (`R`, `P`, `E`), computes (`+`, `IF`, `F`), and writes outputs (`RW`, `assign`, `set`, `write`) — once per frame, on fresh data.

---

## 3. Milestone 1 — Hello, NanoVG

Create `hello_nanovg.cpp`. This is modeled on the sample `modules/v4d/samples/font_rendering.cpp`:

```cpp
#include <opencv2/v4d/v4d.hpp>
using namespace cv;
using namespace cv::v4d;

class FontRenderingPlan : public V4DPlan {
    string text_ = "Hello World";                                // ordinary C++ member
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);     // runtime property edge
public:
    void infer() override {
        nvg([](const Size& sz, const string& str) {              // recorded, not executed!
            using namespace cv::v4d::nvg;
            clearScreen();
            fontSize(40.0f);
            fontFace("sans-bold");
            fillColor(Scalar(255, 0, 0, 255));                   // BGRA
            textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            text(sz.width / 2.0, sz.height / 2.0,
                 str.c_str(), str.c_str() + str.size());
        }, size_, R(text_));
    }
};

int main() {
    cv::Rect viewport(0, 0, 960, 960);
    cv::Ptr<V4D> runtime = V4D::init(viewport, "Font Rendering",
                                     AllocateFlags::NANOVG | AllocateFlags::IMGUI);
    V4DPlan::run<FontRenderingPlan>(0);
    return 0;
}
```

Build and run it. A 960×960 window opens with red “Hello World” text centered in it.

**Dissecting the program**

- `class FontRenderingPlan : public V4DPlan` — every V4D program subclasses `V4DPlan` (which itself extends the core `Plan`).
- `infer()` is the per-frame body. Here it records a single node: `nvg(...)` says “every frame, run this lambda inside the NanoVG drawing context.” V4D handles context activation, font setup, and double-buffering for you.
- **Lambda parameters are bound to edges.** The second and third arguments of `nvg(...)` — `size_` and `R(text_)` — are edges. Every frame, the runtime fetches their current values and passes them to the lambda as `sz` and `str`.
- `size_` is a `Property<cv::Size>` — a typed, auto-updating view onto the runtime's framebuffer size (`V4D::Keys::SIZE`). Resize handling comes for free. A `Property` *is* an edge; you never wrap it in `R(...)`.
- `R(text_)` is a read edge wrapping the plain C++ member `text_`.
- `V4D::init(...)` creates the runtime: window, OpenGL context, and whichever subsystems you request via `AllocateFlags`. Because this plan uses `nvg(...)`, we must pass `AllocateFlags::NANOVG`.
- `V4DPlan::run<FontRenderingPlan>(0)` starts the lifecycle. The argument `0` means **one worker thread plus the main thread** (the main thread runs the display/event loop). See §12.2 for the full table.

⚠️ **Common mistake:** copying a sample that uses `nvg(...)` but forgetting `AllocateFlags::NANOVG`. If you don't ask for a subsystem, calls into it are no-ops (or assert).

**Exercise 1.1.** Change the text, color, and font size. Then make the font size depend on the window width: compute it inside the lambda from `sz`.

---

## 4. Milestone 2 — The Shape of a V4D Application

Every V4D program has the same nine-point skeleton:

```cpp
#include <opencv2/v4d/v4d.hpp>
using namespace cv;
using namespace cv::v4d;

// 1. Subclass V4DPlan.
class MyPlan : public V4DPlan {
    // 2. Declare state as ordinary C++ members (per-worker copies).
    cv::UMat scratch_;
    string   label_ = "hello";
    //    Properties are typed views onto runtime state.
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    // 3. (Optional) One-shot initialization — recorded graph, runs once per worker.
    void setup() override {
        plain([](cv::UMat& s) { s.create(cv::Size(640, 480), CV_8UC3); },
              RW(scratch_));
    }
    // 4. The per-frame body. Recorded once, replayed every frame. REQUIRED.
    void infer() override {
        capture(RW(scratch_));        // pull a frame into scratch_
        nvg(/* ... draw ... */);      // draw on top
        write(R(scratch_));           // push the result to the sink
    }
    // 5. (Optional) Main-thread UI installation — runs once on the main thread.
    void gui() override {
        imgui(/* ... widgets ... */, RWS(label_));
    }
    // 6. (Optional) One-shot teardown — recorded graph, runs once per worker.
    void teardown() override { /* ... */ }
};

int main(int argc, char** argv) {
    cv::Rect viewport(0, 0, 1280, 720);
    // 7. Initialize the runtime.
    cv::Ptr<V4D> runtime = V4D::init(viewport, "My V4D App",
                                     AllocateFlags::NANOVG | AllocateFlags::IMGUI);
    // 8. (Optional) Wire a Source and a Sink.
    auto src  = Source::make(runtime, argv[1]);
    auto sink = Sink::make(runtime, argv[2], src->fps(), viewport.size());
    runtime->setSource(src);
    runtime->setSink(sink);
    // 9. Start the plan. The workers argument selects the worker count
    //    (0 → one worker plus the main thread; see §12.2).
    V4DPlan::run<MyPlan>(/*workers=*/0);
}
```

**Where each phase runs**

| Phase | Where it runs | When |
|---|---|---|
| `setup()` | each worker thread | once, before the frame loop |
| `infer()` | each worker thread | recorded once; the graph is replayed every frame |
| `gui()` | main thread | once, before the frame loop |
| `teardown()` | each worker thread | once, after the frame loop |

The split mirrors OpenGL habits: `setup()` builds resources, `infer()` does per-frame work, `teardown()` releases them. The novelty is that everything inside these methods *records* rather than *executes*.

**`V4D::init` in full**

```cpp
cv::Ptr<V4D> runtime = V4D::init(
    /* viewport     */ cv::Rect(0, 0, 1280, 720),
    /* window title */ "My V4D App",
    /* subsystems   */ AllocateFlags::NANOVG | AllocateFlags::IMGUI,
    /* config       */ ConfigFlags::DEFAULT,
    /* debug        */ DebugFlags::DEFAULT,
    /* MSAA samples */ 0);
```

There is also an overload taking a separate `framebufferSize` (for high-DPI or when window size ≠ pixel size).

`AllocateFlags` — which subsystems to bring up:

| Flag | Initializes |
|---|---|
| `NONE` | Just the OpenGL framebuffer |
| `NANOVG` | NanoVG vector-graphics context on top of GL |
| `IMGUI` | ImGui immediate-mode GUI context on top of GL |
| `BGFX` | bgfx rendering context (alternative to GL) |
| `DEFAULT` | Same as `NONE`. You almost always want `NANOVG \| IMGUI` |

`ConfigFlags` — how the window behaves:

| Flag | Effect |
|---|---|
| `DEFAULT` | No display-related bits set |
| `OFFSCREEN` | Render off-screen (no visible window) |
| `DISPLAY_MODE` | Sync display thread and workers via semaphores — needed for `imshow`-style programs |
| `RESIZEABLE` | Allow user resizing (not resizable by default!) |

`DebugFlags` — what to log: `PRINT_CONTROL_FLOW` (branch decisions), `PRINT_LOCK_CONTENTION`, `MONITOR_RUNTIME_PROPERTIES`, `LOWER_WORKER_PRIORITY` (Linux), `DEBUG_GL_CONTEXT`, `DONT_PAUSE_LOG`. Keep `DEFAULT` for day-to-day work; reach for `PRINT_CONTROL_FLOW` when a branch misbehaves.

The last argument is the MSAA sample count (`0`, `2`, `4`, `8`…). It only matters for direct `gl(...)` rendering — `nvg(...)` does its own anti-aliasing.

---

## 5. Milestone 3 — Plan-DSL Crash Course

You can't write V4D without a working knowledge of Plan-DSL. This section is the minimum viable subset; the Plan-DSL Programming Guide and Reference cover every detail, and their wording is authoritative.

### 5.1 Edges — the only values in the language

An edge is a typed handle to either a storage location (member variable, shared variable, runtime property) or a computed value. Every edge carries an **access intent** (read-only, read-write, copy, locked/shared). Edge-calls produce edges:

| Edge-call | Meaning | Intent |
|---|---|---|
| `V(x)` | Immediate constant `x` | constant |
| `R(x)` | Read of variable `x` | read-only |
| `RW(x)` | Read-write access to `x` (a *definition*) | read-write |
| `RS(x)` | Read of *shared* variable `x` | read under lock |
| `RWS(x)` | Read-write of *shared* `x` | read-write under lock |
| `CS(x)` | Snapshot copy of shared `x` | read + copy under lock |
| `P<T>(key)` | Runtime property (global or per-thread state) | shared, read-only |
| `E<T>(...)` | Stream of input events | polled each frame |
| `F(fn, args...)` | Call any C++ callable; non-void returns a result edge | — |
| `_(a, b, ...)` | Tuple of edges (for n-ary operators) | — |

```cpp
auto a  = R(counter_);                 // read-only edge
auto b  = RW(buffer_);                 // read-write edge (a destination)
auto c  = V(42);                       // constant edge
auto s  = CS(params_);                 // thread-safe snapshot of shared params
auto fc = P<uint64_t>(GlobalState::Keys::FRAME_CNT);
```

Rule of thumb: use the most restrictive intent you can prove. `R` when a value is only read; `RW` only for true destinations. The intent is what makes the runtime's locking and bookkeeping correct.

### 5.2 Operators — the ALU

Plan-DSL implements the full C++ operator set via operator overloading. In their **expression forms**, operators record a node and return a new result edge:

- Arithmetic: `+`, `-`, `*`, `/`, `%`, `++x`, `x++`, `--x`, `x--`
- Logical: `&&`, `||`, `!`
- Bitwise: `&`, `|`, `^`, `<<`, `>>`
- Comparison: `==`, `!=`, `<`, `>`, `<=`, `>=`
- Ternary select: `IF(cond, ifTrue, ifFalse)`
- Memory: `container[i]` (`IDX`), `*ptr` (`DEREF`), `dst = src` (`ASSIGN`)
- Construction: `construct(dst, args...)` (`CONSTRUCT`)

```cpp
auto every8th = (seqCnt_ % V(uint64_t(8))) == V(uint64_t(0));   // bool edge
auto bright   = F(&cv::mean, R(frame_)) > V(128.0);              // bool edge
assign(RW(x_), R(x_) + V(1));                                    // statement form: x_ += 1
```

Every operator has four spellings — symbol (`a + b`), named (`ADD(a, b)`), generic (`OP<Operators::ADD_>(a, b)`), and statement (`op<Operators::ADD_>(a, b)` / `assign(...)`). The symbol form is the most ergonomic.

**Expression vs. statement forms.** Symbol/named/generic forms return result edges. The lowercase statement helpers (`assign(...)`, `op<...>(...)`, `construct(...)`) create the same nodes but do **not** return a result edge; they return `cv::Ptr<Plan>` for chaining. Note that the symbol spelling `dst = src` is an *expression* form of `ASSIGN`: it records the store **and yields a result edge**. The toggle idiom in Milestone 8 relies on exactly that.

⚠️ **`IF` is eager.** All three operands of `IF(cond, a, b)` are computed every frame as graph nodes, then one is selected (like LLVM `select`). If the arms have side effects or are expensive, use `branch(...)` regions instead — only the taken arm executes.

### 5.3 Control flow — branch regions, not jumps

There is no `goto`, `break`, or `continue`. Control flow is structured and expressed with predicated regions:

```cpp
branch(R(x_) == V(0))
    ->plain(doA)
->elseBranch()
    ->plain(doC)
->endBranch();
```

The predicate is re-evaluated every frame. If true, the region's nodes run; if false, they are skipped.

**Chaining.** `branch`, `elseBranch`, `endBranch`, `plain`, statement-form operators, void-`F` calls, and the V4D context calls return a plan pointer (`cv::Ptr<Plan>`; most V4D context calls return `cv::Ptr<V4DPlan>`), so you can chain them with `->`. Semicolon-separated statements work equally well — chaining is sugar. A non-void `F(...)` is the exception: it returns a *result edge*, not a plan pointer.

Predicates can be bool edges (`R(x) == V(0)`), bool-returning callables (`branch(always_)`), or callables with operand edges. Predefined predicates: `always_`, `isTrue_(b)`, `isFalse_(b)`, `and_(a, b)`, `or_(a, b)`.

**Branch types** refine concurrency semantics:

| Type | Semantics |
|---|---|
| `NONE` | No branch; plain node behavior |
| `PARALLEL` *(default)* | Every worker executes the region when the predicate holds |
| `SINGLE` | At most *one* worker executes it (globally locked) — use for serialized side effects (logging, file I/O) |
| `ONCE` | Executes exactly once, globally, then permanently disabled (sticky) |
| `PARALLEL_ONCE` | Executes exactly once *per worker* |

```cpp
branch(BranchType::ONCE, always_)
    ->plain([] { /* global one-shot init */ })
->endBranch();
```

### 5.4 Loops — branches whose predicate the body updates

There is no `while` keyword. A loop is a `branch` region whose body updates the predicate. Because the graph is re-executed every frame, each loop iteration takes one frame:

```cpp
struct CountdownPlan : Plan {
    int counter_ = 10;
    void infer() override {
        branch(R(counter_) > V(0));
            plain([](int c) { std::cout << "tick " << c << std::endl; }, R(counter_));
            assign(RW(counter_), R(counter_) - V(1));
        endBranch();
    }
};
```

This prints one `tick` per frame for ten frames, then the region's predicate is false forever. The graph is not rebuilt; only the predicate value changes from frame to frame. Internalize this: V4D's semantics are those of a frame loop, not a thread of execution.

### 5.5 Variables and shared state

**Plain C++ members are per-worker storage.** Each worker gets its own copy; no synchronization, no sharing. Use for scratch buffers, per-worker counters, DNN objects.

**Shared variables get a mutex.** For plan members: declare the member, register it with `_shared(member_)` (usually in the constructor), then access it only via `RS` / `RWS` / `CS`. Using `RS`/`RWS` on an unregistered plan member throws `std::runtime_error`. (Storage *outside* the plan object — e.g. `static` globals — is implicitly registered as shared on first shared access.)

- `RS(x)` — read under the mutex.
- `RWS(x)` — read-write under the mutex.
- `CS(x)` — the variable is read under its mutex and a **private copy** is produced; downstream nodes use the copy, so the lock is held only for the copy itself. Prefer `CS` whenever consumers only need a snapshot (the canonical GUI→worker handoff).

`_safe(var)` is the opt-out: “this variable is never accessed shared; don't give it a mutex.”

`GlobalState` (program-wide) and `LocalState` (per-thread) are key-value tables read via `P<T>(key)` and written via `set(key, edge)` nodes or `GlobalState::set<T>(key, v)`.

**Check your understanding:**

1. Why does `capture()` inside `infer()` not grab a frame right now?
2. What happens if you put `std::cout << "hi"` directly in `infer()` (not inside a node)?
3. `IF` vs `branch` — which one is lazy?

Answers: (1) it records a node; the frame is pulled during replay. (2) It prints once, at graph-build time, on each worker. (3) `branch` is lazy; `IF` computes all operands eagerly.

---

## 6. Milestone 4 — Video In / Video Out

### 6.1 Sources and Sinks

A Source produces frames; a Sink consumes them. The factories understand filenames and use `cv::VideoCapture` / `cv::VideoWriter` under the hood:

```cpp
auto src  = Source::make(runtime, "input.mp4");       // anything VideoCapture handles
auto sink = Sink::make(runtime, "out.mkv", src->fps(), viewport.size());
runtime->setSource(src);
runtime->setSink(sink);
```

### 6.2 `capture(...)` and `write(...)`

Both come in three forms:

```cpp
// 1. Default buffer — V4D binds an internal capture buffer to the plan.
capture();                    // pull a frame into the default buffer
write();                      // push the default buffer out

// 2. Explicit member buffer:
capture(RW(frame_));
write(R(result_));

// 3. With an inline transform:
capture({ cv::cvtColor(in, out, cv::COLOR_BGR2GRAY); }, RW(gray_));
write({ f.copyTo(out); }, R(result_));
```

Notes:

- `capture()` records a node. At replay time the runtime pulls the next frame from the source into the buffer (or through your lambda).
- When you call `capture()` with no argument, the first frame determines the buffer's size.
- If no sink is configured, `write()` is a no-op. That's the normal setup for windowed demos — the visible window is the output.
- `write()` is also silently a no-op inside sub-plans; only the top-level plan pushes to the sink.

### 6.3 Exercise program: video → grayscale → video

```cpp
class GrayscalePlan : public V4DPlan {
    cv::UMat in_, gray_, out_;
public:
    void infer() override {
        capture(RW(in_));
        plain([](const cv::UMat& in, cv::UMat& gray, cv::UMat& out) {
            cv::cvtColor(in, gray, cv::COLOR_BGR2GRAY);
            cv::cvtColor(gray, out, cv::COLOR_GRAY2BGR);   // sink expects 3 channels
        }, R(in_), RW(gray_), RW(out_));
        write(R(out_));
    }
};

int main(int argc, char** argv) {
    cv::Rect viewport(0, 0, 1280, 720);
    cv::Ptr<V4D> runtime = V4D::init(viewport, "Grayscale",
                                     AllocateFlags::NANOVG | AllocateFlags::IMGUI);
    auto src  = Source::make(runtime, argv[1]);
    auto sink = Sink::make(runtime, argv[2], src->fps(), viewport.size());
    runtime->setSource(src);
    runtime->setSink(sink);
    V4DPlan::run<GrayscalePlan>(0);
}
```

Run it as `./grayscale in.mp4 out.mkv`. Notice how `plain(lambda, edges...)` binds edges to lambda parameters positionally — the same mechanism as `nvg(...)`.

**Exercise 4.1.** Rewrite the pipeline using `capture({ ... }, RW(gray_))` with an inline color conversion, eliminating the `in_` buffer.

**Exercise 4.2.** Make the output half resolution. Hint: `cv::resize` in the `plain` node; pass the sink the new size in `Sink::make`.

---

## 7. Milestone 5 — Drawing on Top of Video

The canonical V4D recipe (see `samples/video_editing.cpp`) is:

```
capture()  →  nvg(...)  →  write()
```

The captured frame lands in the default buffer; the NanoVG context draws into the same framebuffer that the visible window and the sink are bound to; `write()` pushes the composited result.

```cpp
class VideoEditingPlan : public V4DPlan {
    const string hv_ = "Hello Video!";
    Property<cv::Size> sz_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    void infer() override {
        capture();                                        // 1. pull a frame
        nvg([](const Size& sz, const string& str) {      // 2. draw on top
            using namespace cv::v4d::nvg;
            fontSize(40.0f);
            fontFace("sans-bold");
            fillColor(Scalar(255, 0, 0, 255));
            textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            text(sz.width / 2.0, sz.height / 2.0,
                 str.c_str(), str.c_str() + str.size());
        }, sz_, R(hv_));
        write();                                          // 3. push the result
    }
};
```

**NanoVG idioms worth knowing.** Inside a `nvg(...)` lambda, `using namespace cv::v4d::nvg;` exposes a near line-for-line mirror of the NanoVG C API:

```cpp
clearScreen();                                   // wipe before drawing

// Text
fontSize(40.0f); fontFace("sans-bold");
fillColor(Scalar(255, 0, 0, 255));               // BGRA
textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
text(x, y, begin, end);

// Shapes
beginPath();
rect(10, 10, 200, 100);
fillColor(Scalar(0, 255, 0, 128));
fill();

// Gradients
Paint gloss = linearGradient(x, y, x + w, y + h,
                             Scalar(0, 0, 0, 32), Scalar(0, 0, 0, 16));
fillPaint(gloss);
fill();

// Images
int handle = createImage("foo.png", NVG_IMAGE_NEAREST);
Paint img = imagePattern(0, 0, w, h, 0.0f, handle, 1.0f);
fillPaint(img);
fill();
```

Two extra tips:

- You can pass member functions instead of lambdas: `nvg(&FaceFeatures::drawFaceOvalMask, RS(features_))`.
- To render into an off-screen `cv::UMat` (e.g. to use the drawing as a texture later), draw with `nvg(...)` and then snapshot the framebuffer with `fb(...)` — covered next.

**Exercise 5.1.** Add a frame counter in the top-left corner. You'll need `Property<uint64_t> seq_ = P<uint64_t>(GlobalState::Keys::FRAME_CNT);` passed as an edge and formatted with `snprintf` inside the lambda.

---

## 8. Milestone 6 — Pixel Access with `fb(...)`

`nvg(...)` is for vector graphics; when you need to read or write raw pixels as a `cv::UMat`, use `fb(...)`. V4D creates an OpenCL-OpenGL-shared `UMat` for the framebuffer and implicitly inserts it into your function's argument list at the position given by the template parameter:

```cpp
fb<1>(cv::cvtColor,
      R(result_),                            // arg 0: src
      V(cv::COLOR_BGR2RGBA),                 // arg 2: code   (fb is arg 1: dst)
      V(0),                                  // arg 3: dstCn
      V(cv::ALGO_HINT_DEFAULT));             // arg 4: hint
```

At replay time this calls `cv::cvtColor(result_, framebuffer, ...)` — i.e. it writes `result_` into the visible framebuffer. With the default `fb<0>` (or just `fb(...)`), the framebuffer is the first argument — typically the source.

Because the framebuffer is a genuine `cv::UMat` backed by shared memory, you can run OpenCV (and thus OpenCL) directly on framebuffer data without copies.

**Copying the framebuffer out.** Snapshot the framebuffer into a `UMat` with `copyTo` (samples define the helper pointer `UMAT_COPY_` via the `_OLMC_` macro in `util.hpp`):

```cpp
nvg(&StarsRenderer::draw, RWS(stars_), size_);
fb(UMAT_COPY_, RWS(stars_.rendering_));      // framebuffer → UMat

constexpr static auto UMAT_COPY_ =
    _OLMC_(void, cv::UMat, &cv::UMat::copyTo, cv::OutputArray);
```

Read the macro as “static-cast a member-function pointer to a concrete signature so the DSL can deduce types.” It is zero-cost.

**When to use what**

| Use `nvg(...)` when… | Use `fb(...)` when… |
|---|---|
| You want text, shapes, gradients, images | You want raw pixel read/write |
| The result lives in the visible framebuffer | You want a `cv::UMat` to manipulate later |
| Per-pixel performance isn't critical | You want OpenCL kernels on framebuffer data |

**Exercise 6.1.** Build a threshold effect: `capture(RW(frame_))`, then in a `plain` node compute `cv::threshold` into `mask_`, and finally `fb<1>(cv::cvtColor, R(mask_), ...)` to display it.

---

## 9. Milestone 7 — GUIs with ImGui

V4D ships with ImGui. Request it with `AllocateFlags::IMGUI`, then override `gui()`:

```cpp
void gui() override {
    imgui([](Params& params) {
        using namespace ImGui;
        Begin("Effect");
        Checkbox("Enable",        &params.enabled_);
        SliderFloat("Saturation", &params.skinSaturation_, 0.0f, 10.0f);
        if (Button("Fullscreen")) params.fullscreen_ = !params.fullscreen_;
        End();
    }, RWS(params_));
}
```

**The contract (important!)**

- `gui()` runs once, on the main thread, before the frame loop, outside the normal worker graph. Inside it, `imgui(...)` records a node into a special UI transaction that ImGui re-invokes every display refresh, on the main thread.
- Your ImGui lambda therefore does not participate in the worker graph. It can freely mutate shared state — but only through proper edges. Passing `RWS(params_)` tells the runtime to take the shared mutex for the duration of the UI update.

The current design is a documented exception: *“at the moment gui is an exception from the rule that a Plan only implements the graph, because it runs on the display thread.”* Practical rules:

- Don't share mutable state between `gui()` and `infer()` without shared-variable discipline (`_shared` + `RS`/`RWS`/`CS`).
- Workers should read GUI-controlled parameters with `CS(params_)` — a snapshot copy under the lock — so the UI never holds the lock while the pipeline runs.

**The standard pattern**

```cpp
class MyPlan : public V4DPlan {
    struct Params { float strength_ = 1.0f; bool enabled_ = true; };
    static Params params_;                    // one instance for the whole program
public:
    MyPlan() { _shared(params_); }            // give it a mutex
    void gui() override {
        imgui([](Params& p) { /* widgets mutate p */ }, RWS(params_));
    }
    void infer() override {
        capture();
        branch(F([](const Params& p) { return p.enabled_; }, CS(params_)))
            ->plain(apply_effect, CS(params_))
        ->endBranch();
        write();
    }
};
MyPlan::Params MyPlan::params_;               // definition
```

The predicate snapshots the whole struct under the lock (`CS(params_)`) and extracts the field inside an `F` node — purely core-DSL constructs.

> 📌 **Member-level shared access in the samples.** The Plan-DSL Reference documents `RS`/`RWS`/`CS` on *registered variables*. The samples additionally apply them to *members of a registered shared struct* — e.g. `branch(CS(params_.enabled_))` in `beauty-demo.cpp` — relying on the struct's mutex to protect the whole object. That usage is sample-defined, not part of the formal contract. If in doubt, snapshot the whole struct with `CS(params_)` and select the field inside a node, as above.

(A `static` member living outside the plan object is implicitly registered as shared on first shared access, so the explicit `_shared` call is strictly required only for plan members — but writing it is good habit.)

**Exercise 7.1.** Add a slider that controls text size in your Milestone 5 overlay. The GUI writes `Params` on the main thread; the `nvg` lambda reads a `CS(...)` snapshot.

---

## 10. Milestone 8 — Events and Properties

### 10.1 Events

Events are edges that produce a list of input events for the current frame:

```cpp
Event<Mouse> pressEvents_ = E<Mouse>(Mouse::Type::PRESS);                  // presses only
Event<Mouse> allMouse_    = E<Mouse>();                                    // all mouse events
Event<Mouse> dragEvents_  = E<Mouse>(Mouse::Type::DRAG, Mouse::LEFT);      // type + trigger
Event<Mouse> scroll_      = E<Mouse>(Mouse::Type::SCROLL);
```

Event classes: `Mouse`, `Keyboard`, `Window`, `Joystick`. Mouse types include `PRESS`, `RELEASE`, `CLICK`, `DRAG`, `MOVE`, `SCROLL`, `HOVER_ENTER`, `HOVER_EXIT`. The DSL core produces empty lists; V4D's runtime fills them from GLFW each frame.

The classic usage pattern — test whether the list is empty:

```cpp
auto anyPress = !F(&Mouse::List::empty, pressEvents_);     // bool edge
branch(anyPress)
    ->assign(RWS(params_.enabled_), !CS(params_.enabled_)) // toggle
->endBranch();
```

### 10.2 The toggle idiom from `beauty-demo.cpp`

The demo folds “toggle on click” and “is it enabled?” into a single node — the assignment's result edge is the branch predicate:

```cpp
branch(
    RWS(params_.enabled_) = IF(
        F(&Mouse::List::empty, pressEvents_),   // cond: the event list is empty
        CS(params_.enabled_),                   // cond TRUE  (no press) → keep
        !CS(params_.enabled_)                   // cond FALSE (a press)  → flip
    )
)
    -> /* effect runs only while enabled_ is true */
->elseBranch()
    -> /* compose without the effect */
->endBranch();
```

Read `RWS(...) = IF(...)` as one `ASSIGN` node (symbol form, so it yields a result edge): compute the new value, write it under the shared mutex, and use that value as the predicate.

**Polarity, carefully.** `IF(cond, a, b)` is an eager `select`: it picks `a` when `cond` is true. Here the condition is *“the event list is empty”* — so the **true arm keeps the old value** (no click this frame) and the **false arm flips it** (there was a press). It is easy to misread the first operand as “was there a press?”; it is the negation of that. Trace it once by hand — understanding this expression means you understand edges. (See `beauty-demo.cpp` for the idiom in context.)

### 10.3 Properties: reading and writing runtime state

**Reading** — declare a `Property` and pass it around like any edge:

```cpp
Property<cv::Size>  size_   = P<cv::Size>(V4D::Keys::SIZE);
Property<cv::Rect>  vp_     = P<cv::Rect>(V4D::Keys::VIEWPORT);
Property<uint64_t>  seqCnt_ = P<uint64_t>(GlobalState::Keys::FRAME_CNT);
Property<size_t>    widx_   = P<size_t>(LocalState::Keys::WORKER_INDEX);
```

V4D keys: `SIZE`, `WINDOW_SIZE`, `VIEWPORT`, `FRAMEBUFFER_SIZE`, `CLEAR_COLOR`, `NAMESPACE`, `FULLSCREEN`, `DISABLE_INPUT_EVENTS`, `VISIBLE`. Core DSL keys include `FRAME_CNT`, `CAPTURE_CNT`, `FPS`, `RUN_CNT`, `TIME_TRACKER`, `WORKERS_READY`, and more (see the Plan-DSL Reference for the full lists).

**Writing** — record a `set(key, edge)` node:

```cpp
set(V4D::Keys::FULLSCREEN,  CS(params_.fullscreen_));
set(V4D::Keys::CLEAR_COLOR, V(cv::Scalar(30, 30, 30, 255)));
```

`set` fires every frame at that point in the graph; wrap it in `branch(BranchType::ONCE, always_)` for one-shot writes. There is also a tuple form for setting several keys in one node. (As in §9, the samples pass struct members like `CS(params_.fullscreen_)`; the formal contract describes whole registered variables.)

**Exercise 8.1.** Set `V4D::Keys::DISABLE_INPUT_EVENTS` to `true` and verify your click-toggle stops responding (useful for automated tests and recordings).

---

## 11. Milestone 9 — Raw OpenGL (Optional)

For full OpenGL control — your own shaders, FBOs, vertex buffers — use `gl(...)`. The smallest useful OpenGL program:

```cpp
class RenderOpenGLPlan : public V4DPlan {
public:
    void setup() override {
        gl(glClearColor, V(0.0f), V(0.0f), V(1.0f), V(1.0f));   // blue
    }
    void infer() override {
        gl(glClear, V(GL_COLOR_BUFFER_BIT));                     // each frame
    }
};

int main() {
    cv::Rect viewport(0, 0, 960, 960);
    cv::Ptr<V4D> runtime = V4D::init(viewport, "GL Blue Screen", AllocateFlags::IMGUI);
    V4DPlan::run<RenderOpenGLPlan>(0);
}
```

Notes:

- `gl(fn, args...)` records a node that calls `fn` inside the worker's OpenGL context. Free functions, member functions and lambdas all work: `gl(&MyScene::render, R(scene_), V(false))`.
- `nvg(...)` is implemented on top of `gl(...)` — it just manages NanoVG state for you. You can freely mix both.
- For debugging, wrap calls in `GL_CHECK(...)`: in debug builds it checks for GL errors after the call; in release builds it compiles away.
- **Multiple contexts.** The reference also defines `gl(idxEdge, fn, args...)`, which executes the GL commands on a selected context index so several contexts render in parallel (in the samples you will see this spelled `gl<-1>(V(ctxIdx), fn, args...)`; see `many_cubes-demo.cpp`, where ten contexts each render a cube).

---

## 12. Milestone 10 — Going Big: Sub-plans, Threads, Shared State

### 12.1 Sub-plans — composing large programs

A sub-plan is a `Plan`/`V4DPlan` instance owned by a parent plan.

```cpp
class BeautyDemoPlan : public V4DPlan {
    cv::Ptr<FaceFeatureMasksPlan> prepareFeatureMasksPlan_;
    cv::Ptr<BeautyFilterPlan>      beautyFilterPlan_;
public:
    BeautyDemoPlan() {
        // constructor-only! first statements after the initializer list
        prepareFeatureMasksPlan_ = _sub<FaceFeatureMasksPlan>(this, features_, frames_);
        beautyFilterPlan_        = _sub<BeautyFilterPlan>(this, params_, frames_);
    }
    void infer() override {
        // ...
        subInfer(prepareFeatureMasksPlan_);   // splice child's infer() graph here
        subInfer(beautyFilterPlan_);
        // ...
    }
};
```

Rules and reasons:

- `_sub<>` is **constructor-only**. Calling it in `infer()` would recreate the child every frame and break state. `subInfer(...)` (the per-frame splice) belongs in `infer()`; `subSetup(...)`/`subTeardown(...)` exist too.
- The child receives references to the parent's state, so its writes land in the same buffers the parent reads — the canonical way to share scratch buffers.
- A spliced graph **inherits the enclosing branch's predicate** — this is the primary tool for conditional sub-pipelines.

Why bother? Organization (a 500-line `infer()` becomes three focused classes), reuse, and conditional composition.

### 12.2 Threads and workers

`V4DPlan::run<Tplan>(N)` forwards `N` to `Plan::run`; the worker-count semantics are defined by Plan-DSL:

| N | Threads |
|---|---|
| -1 | runtime default worker count + main |
| 0 | 1 worker + main ← the common case |
| 1 | 1 worker + main |
| ≥ 1 | N workers + main (e.g. `beauty-demo` runs with 6) |

`0` is the special case meaning “one worker”; any `N ≥ 1` means exactly N workers. For V4D, the main thread usually handles the display and event loop.

**What's shared vs. not:**

| Shared across the program | Not shared |
|---|---|
| `GlobalState` table | Plan member variables (per-worker copies) |
| `_shared(...)` variables (one mutex each) | `LocalState` (per-thread) |
| The runtime itself | `gui()`'s view of the plan (main thread only) |

**Access intents for shared variables:** `RS(x)` (read under lock), `RWS(x)` (read-write under lock), `CS(x)` (read under lock and produce a private copy; downstream nodes use the copy, so prefer it when consumers only need a snapshot). Use `BranchType::SINGLE` to serialize a side-effecting region across workers, and `BranchType::ONCE` / `PARALLEL_ONCE` for one-shot work.

**Design rule:** think of `infer()` as the body of an OpenMP parallel region. Whatever you'd do in a `#pragma omp parallel` region, do here; whatever needs cross-thread visibility goes through shared variables.

---

## 13. Capstone Project — Chroma, a Complete Video Filter App

Let's combine everything: video input, an adjustable color effect on the framebuffer, a HUD overlay, an ImGui control panel, mouse toggling, fullscreen support, and optional file output.

```cpp
#include <opencv2/v4d/v4d.hpp>
using namespace cv;
using namespace cv::v4d;

class ChromaPlan : public V4DPlan {
public:
    struct Params {
        float contrast_   = 1.0f;
        float brightness_ = 0.0f;
        bool  enabled_    = true;
        bool  hud_        = true;
        bool  fullscreen_ = false;
    };
private:
    static Params params_;                       // shared: GUI writes, workers read
    Property<cv::Size>  size_    = P<cv::Size>(V4D::Keys::SIZE);
    Property<uint64_t>  frameNo_ = P<uint64_t>(GlobalState::Keys::FRAME_CNT);
    Event<Mouse>        clicks_  = E<Mouse>(Mouse::Type::CLICK);

    // CPU-side effect: runs inside an fb(...) node, directly on the framebuffer.
    static void adjust_colors(cv::UMat& img, const Params& p) {
        img.convertTo(img, -1, p.contrast_, p.brightness_);
    }
public:
    ChromaPlan() { _shared(params_); }

    // ---- Main-thread UI, installed once ------------------------------------
    void gui() override {
        imgui([](Params& p) {
            using namespace ImGui;
            Begin("Chroma");
            Checkbox("Enable effect", &p.enabled_);
            SliderFloat("Contrast",   &p.contrast_,   0.0f, 3.0f);
            SliderFloat("Brightness", &p.brightness_, -100.0f, 100.0f);
            Checkbox("HUD", &p.hud_);
            if (Button("Fullscreen")) p.fullscreen_ = !p.fullscreen_;
            End();
        }, RWS(params_));
    }

    // ---- Per-frame graph ----------------------------------------------------
    void infer() override {
        set(V4D::Keys::FULLSCREEN, CS(params_.fullscreen_));     // (0) GUI → runtime

        // (1) toggle enabled_ on mouse click; the assignment doubles as predicate.
        //     IF's condition is "the click list is empty":
        //     true arm (no click) keeps the value, false arm (click) flips it.
        branch(
            RWS(params_.enabled_) = IF(
                F(&Mouse::List::empty, clicks_),
                CS(params_.enabled_),          // no click → unchanged
                !CS(params_.enabled_)          // click    → flip
            )
        )
        ->endBranch();

        capture();                                             // (2) input

        branch(CS(params_.enabled_))                           // (3) effect, only while enabled
            ->fb(adjust_colors, CS(params_))
        ->endBranch();

        branch(CS(params_.hud_))                               // (4) HUD overlay
            ->nvg([](const Size& sz, uint64_t frameNo) {
                using namespace cv::v4d::nvg;
                char buf[64];
                snprintf(buf, sizeof(buf), "frame %llu", (unsigned long long)frameNo);
                fontSize(28.0f); fontFace("sans-bold");
                fillColor(Scalar(255, 255, 255, 220));
                textAlign(NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
                text(16.0f, 12.0f, buf, buf + strlen(buf));
            }, size_, frameNo_)
        ->endBranch();

        write();                                               // (5) output
    }
};
ChromaPlan::Params ChromaPlan::params_;
```

(As in §9, the sample-style `CS(params_.field)` spellings take member-level snapshots of the registered `params_` struct; the formal contract documents whole-variable snapshots. See the note in §9.)

And the `main`:

```cpp
int main(int argc, char** argv) {
    cv::Rect viewport(0, 0, 1280, 720);
    cv::Ptr<V4D> runtime = V4D::init(viewport, "Chroma",
                                     AllocateFlags::NANOVG | AllocateFlags::IMGUI,
                                     ConfigFlags::DISPLAY_MODE);
    // Input: argv[1] = video file (or wire a webcam Source here).
    auto src = Source::make(runtime, argv[1]);
    runtime->setSource(src);
    // Optional output: pass a third argument to record the composited frames.
    if (argc > 2) {
        auto sink = Sink::make(runtime, argv[2], src->fps(), viewport.size());
        runtime->setSink(sink);
    }
    V4DPlan::run<ChromaPlan>(0);
    return 0;
}
```

**Walk through what happens at runtime:**

1. The main thread runs `gui()` once, installing the ImGui panel. Workers build their graphs.
2. Every frame: click-toggle node → `capture()` → (maybe) `fb(adjust_colors)` mutating the framebuffer in place → (maybe) HUD → `write()`.
3. The GUI thread mutates `params_` under the shared mutex; workers always see consistent snapshots via `CS(...)`.
4. No sink configured? `write()` is a no-op and the window is the output. Sink configured? The composited frames (with HUD) are encoded.

**Capstone exercises.**

1. Add a `Keyboard` event that also toggles the effect.
2. Replace the HUD frame counter with a rolling FPS readout (property `GlobalState::Keys::FPS`).
3. Add a “side-by-side” checkbox that composes original vs. filtered frames (study `beauty-demo.cpp` — it does exactly this).
4. Make the window resizable (`ConfigFlags::RESIZEABLE`) and verify `size_` keeps the HUD positioned.

---

## 14. Debugging and Common Pitfalls

**Debug tools**

| Tool | Use when… |
|---|---|
| `DebugFlags::PRINT_CONTROL_FLOW` | A branch isn't behaving as expected — logs per-node enable/disable decisions |
| `DebugFlags::PRINT_LOCK_CONTENTION` | Suspected shared-mutex contention |
| `DebugFlags::MONITOR_RUNTIME_PROPERTIES` | You want every property read/write logged |
| `DebugFlags::DEBUG_GL_CONTEXT` | Deep OpenGL debugging (huge log) |
| `GL_CHECK(expr)` | Checking GL errors around raw GL calls (debug builds only) |

**Pitfall gallery**

| # | Pitfall | Why / Fix |
|---|---|---|
| 1 | “My `cout` prints once and never again” | Code outside nodes runs at graph-build time. Wrap per-frame work in `plain(...)`, `nvg(...)`, etc. |
| 2 | “My `if (edge)` at build time does nothing useful” | Graph structure is fixed at build time. Use `branch(...)` for runtime decisions. |
| 3 | `nvg(...)` silently does nothing | You forgot `AllocateFlags::NANOVG` in `V4D::init`. Same for `IMGUI` / `BGFX`. |
| 4 | `std::runtime_error` from `RS`/`RWS` | The plan member wasn't registered with `_shared(...)`. (Globals/statics outside the plan are implicitly shared.) |
| 5 | Both arms of `IF` run | `IF` is eager — it's a `select`, not a branch. Use `branch` regions for lazy/side-effecting arms. |
| 6 | “My loop is slow” | Loops advance *one iteration per frame* — that's the frame-sequential model, not a bug. |
| 7 | Window doesn't resize | Pass `ConfigFlags::RESIZEABLE`. |
| 8 | No output file appears | You never called `runtime->setSink(...)` — `write()` is a no-op without a sink (and inside sub-plans). |
| 9 | Data race between GUI and workers | Mutate shared state from `gui()` only through `RWS(...)`; read it in `infer()` with `CS(...)`. |
| 10 | “Tearing”/display desync in `imshow`-style apps | Use `ConfigFlags::DISPLAY_MODE`. |
| 11 | Toggle fires on the wrong arm | Re-read §10.2: `IF`'s first operand is the *condition*; the true arm is selected when it holds. |

---

## 15. Where to Go Next

Read, in order:

1. `plan-dsl-programming-guide.markdown` — the DSL tutorial (the foundation of everything here).
2. `plan-dsl-reference.markdown` — the ISA-style reference: every opcode, every overload, every branch type, plus the LLVM-IR lowering table. Keep it open while you write. If it disagrees with this tutorial, the reference wins.
3. `modules/v4d/include/opencv2/v4d/v4d.hpp` — the entry header; everything in these guides is an ergonomic summary of what's there.

Study the samples (`modules/v4d/samples/`), roughly in this order:

| Sample | Teaches |
|---|---|
| `font_rendering.cpp` | Minimum NanoVG program |
| `render_opengl.cpp` | Minimum OpenGL program |
| `display_image_fb.cpp` / `display_image_nvg.cpp` | Image display via `fb` vs `nvg` |
| `video_editing.cpp` | `capture → nvg → write` |
| `font_with_gui.cpp` | GUI feeding NanoVG |
| `custom_source_and_sink.cpp` | Rolling your own I/O + conditional `write()` in a branch |
| `cube-demo.cpp` / `many_cubes-demo.cpp` | Pure GL; multiple parallel GL contexts |
| `pedestrian-demo.cpp` / `optflow-demo.cpp` | Non-trivial detection + tracking pipelines |
| `imshow_reimplementation.cpp` | A full GUI image viewer |
| `beauty-demo.cpp` | The kitchen sink: shared state, sub-plans, `IF` toggling, events, GUI |

---

## 16. Appendix — Cheat Sheet

```cpp
// ── Includes ────────────────────────────────────────────────────────────────
#include <opencv2/v4d/v4d.hpp>
using namespace cv;
using namespace cv::v4d;

// ── Init ────────────────────────────────────────────────────────────────────
cv::Ptr<V4D> rt = V4D::init(viewport, "Title",
                            AllocateFlags::NANOVG | AllocateFlags::IMGUI,
                            ConfigFlags::DEFAULT, DebugFlags::DEFAULT, /*msaa*/0);

// ── Plan skeleton ───────────────────────────────────────────────────────────
class MyPlan : public V4DPlan {
    cv::UMat scratch_;                                    // per-worker
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    void setup()    override { /* one-shot init graph   */ }
    void infer()    override { /* per-frame graph (REQ) */ }
    void gui()      override { /* main-thread UI, once  */ }
    void teardown() override { /* one-shot cleanup      */ }
};

// ── Sources / sinks ─────────────────────────────────────────────────────────
auto src  = Source::make(rt, "in.mp4");
auto sink = Sink::make(rt, "out.mkv", src->fps(), viewport.size());
rt->setSource(src);  rt->setSink(sink);

// ── Capture / write ─────────────────────────────────────────────────────────
capture();  capture(RW(buf));  capture({ /*transform*/ }, RW(buf));
write();    write(R(buf));     write({ /*transform*/ }, R(buf));

// ── Edges ───────────────────────────────────────────────────────────────────
V(x)  R(x)  RW(x)  RS(x)  RWS(x)  CS(x)  P<T>(key)  E<T>(type)  F(fn, ...)  _(...)

// ── Contexts ────────────────────────────────────────────────────────────────
plain(fn, args...)          // CPU
nvg(fn, args...)            // NanoVG
fb<pos>(fn, args...)        // framebuffer UMat auto-inserted at pos
gl(fn, args...)             // OpenGL   (gl(idxEdge, fn, ...) selects a context index)
imgui(fn, args...)          // inside gui() only
set(key, edge)              // property write node

// ── Control flow ────────────────────────────────────────────────────────────
branch(pred)->plain(a)->elseBranch()->plain(b)->endBranch();
branch(BranchType::SINGLE, pred)   // at most one worker
branch(BranchType::ONCE, always_)  // once, globally

// ── Shared state ────────────────────────────────────────────────────────────
static Params params_;  /* ctor: */ _shared(params_);
RS(params_)  RWS(params_)  CS(params_)

// ── Sub-plans ───────────────────────────────────────────────────────────────
sub_ = _sub<Sub>(this, args...);      // constructor only
subInfer(sub_);                       // in infer()

// ── Run ─────────────────────────────────────────────────────────────────────
V4DPlan::run<MyPlan>(/*workers=*/0);  // 0 → one worker + main thread
```

Happy hacking — record once, replay forever.
