# The V4D Application Programming Guide

*A friendly introduction to writing video, image, GPU and GUI applications with Plan-V4D.*

> **Who this is for.** A C++ developer who has never touched Plan-V4D and
> wants to understand how to *write* a V4D application. After reading this
> document you should be able to read every sample in
> `modules/v4d/samples/` and write your own. For the canonical,
> no-nonsense description of every opcode and every edge-call, see the
> companion [`plan-dsl-programming-guide.markdown`](../../plan/doc/plan-dsl-programming-guide.markdown)
> and the [`plan-dsl-reference.markdown`](../../plan/doc/plan-dsl-reference.markdown).
>
> **Prerequisites.** You should be comfortable with modern C++ (C++20),
> OpenCV's `cv::Mat` / `cv::UMat`, and the basic shape of a video pipeline
> (capture → process → display/write). You do **not** need to know
> OpenGL, NanoVG, or ImGui to follow along — V4D will teach you the parts
> you need.
>
> **Companion documents:**
> - [`plan-dsl-programming-guide.markdown`](../../plan/doc/plan-dsl-programming-guide.markdown) — the core DSL
> - [`plan-dsl-reference.markdown`](../../plan/doc/plan-dsl-reference.markdown) — opcode reference
> - `modules/v4d/samples/` — every working program in this guide

---

## Table of contents

1. [What is V4D?](#1-what-is-v4d)
2. [The 30-second tour: hello, NanoVG](#2-the-30-second-tour-hello-nanovg)
3. [The shape of a V4D application](#3-the-shape-of-a-v4d-application)
4. [Initializing the runtime](#4-initializing-the-runtime)
5. [Reading input: Source and capture()](#5-reading-input-source-and-capture)
6. [Writing output: Sink and write()](#6-writing-output-sink-and-write)
7. [Rendering on top: NanoVG (`nvg(...)`)](#7-rendering-on-top-nanovg-nvg)
8. [Direct framebuffer access (`fb<...>(...)`)](#8-direct-framebuffer-access-fb)
9. [Raw OpenGL (`gl(...)`)](#9-raw-opengl-gl)
10. [Properties: reading the runtime's state](#10-properties-reading-the-runtimes-state)
11. [Events: keyboard, mouse, window](#11-events-keyboard-mouse-window)
12. [GUI: ImGui nodes with `gui()` + `imgui(...)`](#12-gui-imgui-nodes-with-gui--imgui)
13. [Sharing state between GUI, workers and time](#13-sharing-state-between-gui-workers-and-time)
14. [Branching and loops in the per-frame body](#14-branching-and-loops-in-the-per-frame-body)
15. [Sub-plans: composing large programs](#15-sub-plans-composing-large-programs)
16. [Threads, workers and contention](#16-threads-workers-and-contention)
17. [Custom Sources and Sinks](#17-custom-sources-and-sinks)
18. [Putting it together: anatomy of `video_editing.cpp`](#18-putting-it-together-anatomy-of-video_editingcpp)
19. [A larger example: anatomy of `beauty-demo.cpp`](#19-a-larger-example-anatomy-of-beauty-democpp)
20. [Cheat-sheet](#20-cheat-sheet)

---

## 1. What is V4D?

V4D (Visualization for Video and Data) is a runtime on top of the
Plan-DSL. Plan-DSL is the small graph-recording language described in
[`plan-dsl-programming-guide.markdown`](../../plan/doc/plan-dsl-programming-guide.markdown).
You should read that first if you have not. Briefly:

* A `Plan` is a C++ class whose methods (`setup`, `infer`, `teardown`,
  `gui`) describe *one frame's worth of work* by **recording** it as a list
  of *task nodes*.
* The runtime then *replays* that recorded list every frame on a worker
  thread. Nothing in the body of `infer()` actually executes while you
  are typing it — it is being recorded.

V4D adds four things on top of that core:

1. **A window and event loop** built on GLFW + OpenGL (with an optional
   NanoVG and ImGui layer on top).
2. **A Source / Sink abstraction** so a `Plan` can read frames from a
   video file, a webcam, or any user functor, and write them to a file, a
   network stream, or anything else.
3. **Side-effect *contexts***: a way to schedule a C++ lambda on a
   specific GPU/CPU pipeline (the framebuffer, NanoVG, OpenGL, ImGui).
4. **A handful of `V4D::Keys` properties** that describe the runtime
   (current framebuffer size, viewport, fullscreen flag, etc.).

That is the whole system. The rest of this guide explains each piece in
turn, using the working samples as the source of truth.

---

## 2. The 30-second tour: hello, NanoVG

Open `modules/v4d/samples/font_rendering.cpp`. It is 32 lines:

```cpp
#include <opencv2/v4d/v4d.hpp>

using namespace cv;
using namespace cv::v4d;

class FontRenderingPlan: public V4DPlan {
    string text_ = "Hello World";
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    void infer() override {
        nvg([](const Size& sz, const string& str) {
            using namespace cv::v4d::nvg;
            clearScreen();
            fontSize(40.0f);
            fontFace("sans-bold");
            fillColor(Scalar(255, 0, 0, 255));
            textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            text(sz.width / 2.0, sz.height / 2.0, str.c_str(),
                 str.c_str() + str.size());
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

Everything you see here will be explained later. For now, four things
to notice:

1. There is no `while (true)`, no `update()`, no `glfwPollEvents()`.
   The runtime drives the loop. You describe one iteration of the loop
   in `infer()`.
2. `nvg(...)` records a node that will, every frame, run the lambda
   inside a NanoVG drawing context. The runtime makes sure NanoVG is
   initialized, the framebuffer is bound, etc.
3. `size_` is a *property edge* — it reads the runtime's current
   framebuffer size and is updated automatically whenever the window
   resizes. You pass it directly to `nvg(...)` without wrapping it in
   `R(...)` (a `Property` *is* an edge).
4. `R(text_)` reads the local member `text_`. `text_` is just a regular
   C++ string; the `R(...)` turns it into an *edge* the recorded node
   will pull each frame.

That is enough vocabulary to read the rest of this guide. We'll fill in
the details.

---

## 3. The shape of a V4D application

Every V4D program has exactly the same skeleton:

```cpp
#include <opencv2/v4d/v4d.hpp>

using namespace cv;
using namespace cv::v4d;

// 1. Subclass V4DPlan (or Plan).
class MyPlan : public V4DPlan {
    // 2. Declare state as ordinary C++ members.
    cv::UMat scratch_;
    string label_ = "hello";
    //    Properties and events are typed views onto runtime state.
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);

public:
    // 3. (Optional) One-shot initialization, recorded on a setup graph.
    void setup() override {
        plain([](cv::UMat& s){ s.create(cv::Size(640, 480), CV_8UC3); },
              RW(scratch_));
    }

    // 4. The per-frame body. Recorded once, replayed every frame.
    void infer() override {
        capture(RW(scratch_));        // pull a frame into scratch_
        nvg(/* ... */);               // draw stuff on top
        write(R(scratch_));           // push the result to the sink
    }

    // 5. (Optional) Main-thread UI installation, runs once.
    void gui() override {
        imgui([](/* ... */){ /* ... */ }, RWS(label_));
    }

    // 6. (Optional) One-shot teardown.
    void teardown() override { /* ... */ }
};

int main(int argc, char** argv) {
    cv::Rect viewport(0, 0, 1280, 720);
    // 7. Initialize the runtime.
    cv::Ptr<V4D> runtime = V4D::init(viewport, "My V4D App",
                                     AllocateFlags::NANOVG | AllocateFlags::IMGUI);

    // 8. (Optional) Wire a Source and a Sink.
    auto src = Source::make(runtime, argv[1]);
    auto sink = Sink::make(runtime, argv[2], src->fps(), viewport.size());
    runtime->setSource(src);
    runtime->setSink(sink);

    // 9. Start the plan. The argument is the number of workers.
    V4DPlan::run<MyPlan>(/*workers=*/0);
}
```

Nine points. That is the whole API surface. The rest of this guide
walks through each one.

### 3.1 What goes where

| Phase       | Where it runs       | When                         |
|-------------|---------------------|------------------------------|
| `setup()`   | each worker thread  | once, before the frame loop  |
| `infer()`   | each worker thread  | recorded once, replayed each frame |
| `gui()`     | main thread         | once, before the frame loop  |
| `teardown()`| each worker thread  | once, after the frame loop   |

The split between `setup()`, `infer()` and `teardown()` is just like
OpenGL: `setup()` builds GPU resources, `infer()` does the per-frame
work, `teardown()` releases them. The novelty is that everything inside
those methods *records*, not *executes*. See
[`plan-dsl-programming-guide.markdown`](../../plan/doc/plan-dsl-programming-guide.markdown)
for the deep dive on the execution model.

---

## 4. Initializing the runtime

`V4D::init` is the one call you cannot skip:

```cpp
cv::Ptr<V4D> runtime = V4D::init(
    /* viewport     */ cv::Rect(0, 0, 1280, 720),
    /* window title */ "My V4D App",
    /* subsystems   */ AllocateFlags::NANOVG | AllocateFlags::IMGUI,
    /* config       */ ConfigFlags::DEFAULT,
    /* debug        */ DebugFlags::DEFAULT,
    /* MSAA samples */ 0);
```

There are three `init` overloads. The two common ones are:

```cpp
// Most samples
init(viewport, title, allocateFlags, configFlags, debugFlags, samples);

// When the window and the framebuffer have different sizes (e.g. for
// high-DPI), or you want the on-screen pixel resolution to differ from
// the logical one.
init(viewport, framebufferSize, title, allocateFlags, configFlags, debugFlags, samples);

// For child windows — rarely needed.
init(v4d, childTitle);
```

### 4.1 `AllocateFlags` — which subsystems to bring up

The runtime only allocates the contexts you ask for. If you don't ask
for `NANOVG`, calls to `nvg(...)` will be no-ops or asserts. Keep this
in mind when copying a sample: if it uses `nvg(...)`, you must pass
`AllocateFlags::NANOVG`.

| Flag              | What gets initialized                              |
|-------------------|-----------------------------------------------------|
| `AllocateFlags::NONE`   | Just the OpenGL framebuffer. |
| `AllocateFlags::NANOVG` | The NanoVG vector graphics context, on top of GL.   |
| `AllocateFlags::IMGUI`  | The ImGui immediate-mode GUI context, on top of GL.  |
| `AllocateFlags::BGFX`   | The bgfx rendering context (alternative to GL).      |
| `AllocateFlags::DEFAULT`| `NONE`. You almost always want `NANOVG \| IMGUI`.    |

`AllocateFlags` are bitmasked with `operator|`, e.g.
`AllocateFlags::NANOVG | AllocateFlags::IMGUI`.

### 4.2 `ConfigFlags` — how the window behaves

| Flag                    | Effect                                                       |
|-------------------------|--------------------------------------------------------------|
| `ConfigFlags::DEFAULT`  | Normal resizable window.                                     |
| `ConfigFlags::OFFSCREEN`| Render to an off-screen framebuffer (no visible window).     |
| `ConfigFlags::DISPLAY_MODE` | Synchronize the display thread and the worker thread with `swap`/`render` semaphores, so the frame loop drives the visible output directly. Needed for `imshow`-style programs. |
| `ConfigFlags::RESIZEABLE`| Allow the user to resize the window. (Default.)             |

### 4.3 `DebugFlags` — what to log

| Flag                              | Effect                                                |
|-----------------------------------|-------------------------------------------------------|
| `DebugFlags::PRINT_CONTROL_FLOW`  | Per-node log of branch enable/disable decisions.      |
| `DebugFlags::PRINT_LOCK_CONTENTION`| Log every shared-mutex contention event.              |
| `DebugFlags::MONITOR_RUNTIME_PROPERTIES` | Log every global property read/write.           |
| `DebugFlags::LOWER_WORKER_PRIORITY` | Linux-only. Lower worker thread niceness.           |
| `DebugFlags::DEBUG_GL_CONTEXT`    | Create a debug OpenGL context (huge log).             |
| `DebugFlags::DONT_PAUSE_LOG`      | Don't pause the OpenCV logger on shutdown.            |

For day-to-day development, leave `DebugFlags::DEFAULT`. Reach for
`PRINT_CONTROL_FLOW` when a branch isn't behaving the way you expect.

### 4.4 `samples` — MSAA

The last argument is the multi-sample anti-aliasing factor (`0`,
`2`, `4`, `8`...). It only matters if your plan touches the
framebuffer directly with `gl(...)` — `nvg(...)` does its own MSAA.

---

## 5. Reading input: Source and capture()

A *Source* is anything that produces frames. The factory
`Source::make` understands filenames:

```cpp
auto src = Source::make(runtime, "input.mp4");        // any format cv::VideoCapture handles
runtime->setSource(src);
```

Internally this calls `cv::VideoCapture`. You can also build your own
`Source` from a generator functor — see §17.

### 5.1 `capture(...)` — the only node you need

The DSL provides three forms:

```cpp
// 1. The default capture buffer — V4D binds a `cv::UMat` to the plan
//    under the hood. The buffer is resized to the source's frame size
//    on the first frame and stays that way.
void infer() override {
    capture();                // pull a frame into the default buffer
    // ... do stuff with it ...
    write();                  // push the default buffer out
}

// 2. Capture into a specific member:
void infer() override {
    capture(RW(frames_.orig_));   // pull into frames_.orig_
    // ...
}

// 3. Capture with a custom transform (e.g. color conversion on the way in):
void infer() override {
    capture([](const cv::UMat& in, cv::UMat& out){
        cv::cvtColor(in, out, cv::COLOR_BGR2GRAY);
    }, RW(frames_.gray_));
}
```

`capture()` always *records* a node. At replay time the runtime calls
the source's `operator()`, hands the frame to the lambda or the default
buffer, and waits for the next iteration.

### 5.2 The default buffer

When you call `capture()` with no argument, V4D binds an internal
`captureFrame_` member on the plan. The first frame determines its
size. The `display_image_*` and `font_rendering` samples rely on this
to skip the boilerplate of declaring a `UMat`. The `video_editing`
sample does it too.

If you want the frame in a specific local (e.g. a struct of buffers
like `Frames` in `beauty-demo.cpp`), pass it explicitly: `capture(RW(myBuf))`.

### 5.3 What `capture()` returns

Nothing. The data is written into the buffer (or the buffer's
internal storage), not produced as an edge. If you want to *also* do
something with the frame downstream, capture into a named buffer and
then read it with `R(myBuf)`.

---

## 6. Writing output: Sink and write()

A *Sink* is anything that consumes frames. Like `Source`, there is a
factory and a custom form:

```cpp
auto sink = Sink::make(runtime, "out.mkv", src->fps(), viewport.size());
runtime->setSink(sink);
```

The `Sink::make` overloads cover any container/codec `cv::VideoWriter`
understands. To roll your own, see §17.

### 6.1 `write(...)` — three forms

```cpp
// 1. Write the default capture buffer:
void infer() override {
    capture();
    // ... transform ...
    write();                          // pushes the default buffer
}

// 2. Write a specific member:
write(R(frames_.result_));

// 3. Write with a custom transform:
write([](cv::UMat& out, const cv::UMat& f) {
    f.copyTo(out);
}, R(frames_.result_));
```

### 6.2 The "no sink" mode

If you don't call `runtime->setSink(...)`, `write()` becomes a no-op.
This is the typical configuration for windowed demos (the visible
window is the sink — see `display_image_nvg.cpp`,
`display_image_fb.cpp`, `font_rendering.cpp`, `beauty-demo.cpp`).

`ConfigFlags::DISPLAY_MODE` plus no sink gives you an
`imshow`-style program. `display_image_fb.cpp` is the smallest such
example:

```cpp
int main() {
    cv::Rect viewport(0, 0, 960, 960);
    Ptr<V4D> runtime = V4D::init(viewport, "Display an Image through direct FB access",
                                 AllocateFlags::IMGUI,
                                 ConfigFlags::DISPLAY_MODE);
    V4DPlan::run<DisplayImageFB>(0, samples::findFile("lena.jpg"));
}
```

### 6.3 Sub-plans and `write()`

`write()` is silently a no-op when the plan has a parent plan — only
the top-level plan pushes to the sink. This lets a sub-plan safely
call `write()` without polluting the parent's output, which is a
useful composition trick (see §15).

---

## 7. Rendering on top: NanoVG (`nvg(...)`)

NanoVG is the easiest way to draw 2-D vector graphics, text and images
on top of a frame. The full API lives in `cv::v4d::nvg::*` and mirrors
the upstream C library almost line-for-line. Within a `nvg(...)` lambda
you can call any of them.

### 7.1 The pattern

```cpp
nvg([](const Size& sz, const string& str) {
    using namespace cv::v4d::nvg;
    // ... NanoVG calls here ...
}, size_, R(text_));
```

The first argument is the lambda. Subsequent arguments are *edges*
the lambda will read at replay time. They may be:

* `Property<T>` instances (`size_`)
* `R(member)`, `RW(member)`, etc.
* `V(constant)`, `V(enum_value)`
* `F(callable, args...)` if you need a derived value

A `nvg(...)` call records **one** node that runs the lambda inside the
NanoVG drawing context. V4D handles context activation, font setup,
and double-buffering.

### 7.2 Common NanoVG idioms

```cpp
using namespace cv::v4d::nvg;

clearScreen();                              // wipe before drawing
fontSize(40.0f);
fontFace("sans-bold");
fillColor(Scalar(255, 0, 0, 255));          // BGRA
textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
text(sz.width / 2.0, sz.height / 2.0,
     str.c_str(), str.c_str() + str.size());

// Paths:
beginPath();
rect(10, 10, 200, 100);
fillColor(Scalar(0, 255, 0, 128));
fill();

// Gradients:
Paint gloss = linearGradient(x, y, x+w, y+h,
                             Scalar(0,0,0,32), Scalar(0,0,0,16));
fillPaint(gloss);
fill();

// Images:
int handle = createImage("foo.png", NVG_IMAGE_NEAREST);
Paint img = imagePattern(0, 0, w, h, 0.0f, handle, 1.0f);
fillPaint(img);
fill();
```

### 7.3 Drawing on top of a frame: `capture()` then `nvg()` then `write()`

`video_editing.cpp` is the canonical recipe:

```cpp
void infer() override {
    capture();                                 // 1. pull a frame
    nvg([](const Size& sz, const string& s) {
        // ... draw text ...
    }, sz_, R(hv_));                           // 2. draw on top
    write();                                   // 3. push the result
}
```

The captured frame is in the default buffer; the NanoVG context writes
into the same framebuffer (the visible window and the sink buffer are
both bound to it). The result is the frame plus the text.

### 7.4 Drawing into an off-screen image with NanoVG

Sometimes you want to render into a `cv::UMat` and only later use it
as a texture (the font-demo and shader-demo do this). The trick is
`fb(...)` (§8) to copy the result out of the framebuffer:

```cpp
nvg(&StarsRenderer::draw, RWS(stars_), size_);
fb(UMAT_COPY_, RWS(stars_.rendering_));   // snapshot framebuffer into UMat
```

`UMAT_COPY_` is a function pointer to `cv::UMat::copyTo`. The `_OL_`
macros in `util.hpp` (and the `_OLC_`, `_OLM_`, `_OLMC_` variants) help
build these pointers:

```cpp
constexpr static auto UMAT_COPY_TO_ =
    _OLMC_(void, cv::UMat, &cv::UMat::copyTo, cv::OutputArray);
```

Read these as "static-cast a member-function pointer to a specific
signature so the DSL can deduce types". The macros are zero-cost —
they exist purely for `decltype` deduction.

---

## 8. Direct framebuffer access (`fb<...>(...)`)

If you need to read or write the OpenGL framebuffer as a `cv::UMat`,
use `fb(...)`. The template parameter `<pos>` (default 0) is the
positional index of the `UMat& framebuffer` argument in the lambda's
parameter list; if you write `fb<1>(...)`, the framebuffer is the
**second** argument.

The framebuffer argument is *implicitly added* by V4D; you do not write
it yourself. This is what makes the call look like a normal OpenCV
function call:

```cpp
fb<1>(cv::cvtColor,
      R(frames_.result_),                  // arg #1
      V(cv::COLOR_BGR2RGBA),               // arg #2
      V(0),                                // arg #3
      V(cv::ALGO_HINT_DEFAULT));           // arg #4
```

This says: at replay time, bind the framebuffer `UMat`, call
`cvtColor(UMat& fb, UMat& result_, int code, int dstCn, int algoHint)`
— except the first `UMat&` is the framebuffer and you only pass the
remaining four arguments. The runtime threads them through.

`fb<pos>` can also insert the framebuffer **anywhere** in the
argument list (negative indices count from the end). The point is that
V4D generates an OpenCL-OpenGL-shared `UMat` for the framebuffer,
calls your function, and gives you direct read/write access — this is
what enables OpenCL kernels on the framebuffer data without a
copy.

### 8.1 When to use `fb(...)` instead of `nvg(...)`

| Use `nvg(...)` when…                              | Use `fb(...)` when…                              |
|---------------------------------------------------|--------------------------------------------------|
| You want text, shapes, gradients, images.         | You want to read or write raw pixels.            |
| The result lives in the visible framebuffer.      | You want a `cv::UMat` you can later manipulate.  |
| You don't care about per-pixel performance.       | You want OpenCL kernels on the framebuffer data. |

`display_image_fb.cpp` shows the minimum useful program: load an
image, resize it, color-convert it, then `fb(...)`-copy it to the
framebuffer.

### 8.2 Multiple framebuffers (children)

V4D can spawn child framebuffers (one per worker, useful for
GPU-side parallelism). Calls like `fb<1>(...)` always target the
*root* framebuffer. To target a child you must manage it through
`runtime()->fbCtx()` directly — this is rarely needed.

---

## 9. Raw OpenGL (`gl(...)`)

For full OpenGL control — your own shaders, FBOs, vertex buffers —
use `gl(...)`. Three overloads:

```cpp
// 1. Default context (per-worker):
gl(glClearColor, V(0), V(0), V(1), V(1));
gl(glClear, V(GL_COLOR_BUFFER_BIT));
gl(&MyScene::render, R(scene_), V(false), V(0.0));

// 2. Multiple OpenGL contexts in parallel (advanced):
gl<-1>(V(i), &CubeScene::render, R(scene_), V(...), V(...));
//    ^context index -1 means "don't use the default -1; pick by edge"

// 3. Dynamic context selection (rarely needed):
gl<0>(V(glCtxIdx), &MyScene::render, RW(scene_));
```

`many_cubes-demo.cpp` shows pattern (2): ten independent OpenGL
contexts each render one cube in parallel. The setup/teardown loops
initialize/destroy each context.

### 9.1 A complete OpenGL program: `render_opengl.cpp`

```cpp
class RenderOpenGLPlan : public V4DPlan {
public:
    void setup() override {
        gl(glClearColor, V(0), V(0), V(1), V(1));      // blue
    }
    void infer() override {
        gl(glClear, V(GL_COLOR_BUFFER_BIT));           // each frame
    }
};

int main() {
    cv::Rect viewport(0, 0, 960, 960);
    Ptr<V4D> runtime = V4D::init(viewport, "GL Blue Screen", AllocateFlags::IMGUI);
    V4DPlan::run<RenderOpenGLPlan>(0);
}
```

That is the smallest useful OpenGL V4D program. It really is just
GL calls wrapped in a node.

### 9.2 `gl(...)` vs `nvg(...)`

`nvg(...)` *is* implemented as `gl(...)` underneath — it just manages
NanoVG state for you. If you want to drop down a level, call `gl(...)`
directly. You can freely mix them: `nvg(...)` for vector UI on top of
`gl(...)` shaders, `gl(...)` for the raw pipeline.

### 9.3 The `GL_CHECK` macro

For debug builds `GL_CHECK(expr)` evaluates `expr` and then calls
`cv::v4d::gl_check_error(...)`. Wrap every raw GL call you care about:

```cpp
GL_CHECK(glClear(GL_COLOR_BUFFER_BIT));
```

In release builds `GL_CHECK` is a no-op.

---

## 10. Properties: reading the runtime's state

`Property<T>` is an *edge* that reads a typed slot from
`GlobalState` or `LocalState`. V4D adds these keys (see
`v4d.hpp:118-130`):

| Key                       | Type        | What it is                                 |
|---------------------------|-------------|--------------------------------------------|
| `V4D::Keys::SIZE`         | `cv::Size`  | The framebuffer size in pixels.            |
| `V4D::Keys::WINDOW_SIZE`  | `cv::Size`  | The OS window size.                        |
| `V4D::Keys::VIEWPORT`     | `cv::Rect`  | The viewport rectangle.                    |
| `V4D::Keys::FRAMEBUFFER_SIZE` | `cv::Size` | The native framebuffer size (pre-stretch). |
| `V4D::Keys::CLEAR_COLOR`  | `cv::Scalar`| Color used by `clear()`.                   |
| `V4D::Keys::NAMESPACE`    | `string`    | Plan namespace, set automatically.         |
| `V4D::Keys::FULLSCREEN`   | `bool`      | Whether the window is fullscreen.          |
| `V4D::Keys::DISABLE_INPUT_EVENTS` | `bool` | Suppresses event delivery (useful for tests). |
| `V4D::Keys::VISIBLE`      | `bool`      | Window visibility.                         |

Plus the core DSL keys (`GlobalState::Keys::FRAME_CNT`, `FPS`,
`WORKERS_STARTED`, `TIME_TRACKER`, etc.).

### 10.1 Declaring a property

```cpp
Property<cv::Size>     size_   = P<cv::Size>(V4D::Keys::SIZE);
Property<cv::Rect>     vp_     = P<cv::Rect>(V4D::Keys::VIEWPORT);
Property<uint64_t>     seqCnt_ = P<uint64_t>(GlobalState::Keys::FRAME_CNT);
```

Because `Property<T>` *is* an edge, you can pass it directly to any
context call — no `R(...)` wrapping:

```cpp
nvg([](const cv::Size& sz) { /* ... */ }, size_);
fb(cv::cvtColor, R(src), V(cv::COLOR_BGR2RGBA), V(0), V(cv::ALGO_HINT_DEFAULT));
//    ^---------- src via R, viewport not needed
```

### 10.2 Writing a property

```cpp
// In infer():
set(V4D::Keys::FULLSCREEN, CS(params_.fullscreen_));
set(V4D::Keys::CLEAR_COLOR, V(cv::Scalar(30, 30, 30, 255)));

// In setup() / teardown() too:
set(GlobalState::Keys::TIME_TRACKER, V(false));
```

`set(key, edge)` records a write node that will fire every frame at
that point. To write *once*, wrap it in `branch(BranchType::ONCE, ...)`.

### 10.3 Tuple form

You can chain several sets:

```cpp
set(std::make_tuple(V4D::Keys::CLEAR_COLOR, V(cv::Scalar(0,0,0,255))),
    std::make_tuple(V4D::Keys::VISIBLE,      V(true)));
```

Mostly cosmetic; useful for symmetry with `imgui(...)` which has a
similar tuple form.

---

## 11. Events: keyboard, mouse, window

Events are how your plan reads input. The DSL defines four event
classes: `Mouse`, `Keyboard`, `Window`, `Joystick`. V4D's `events.hpp`
fills them in from GLFW.

### 11.1 Declaring an event edge

```cpp
Event<Mouse> pressEvents_ = E<Mouse>(Mouse::Type::PRESS);

// All events of a class:
Event<Mouse> allMouse_ = E<Mouse>();

// A specific sub-type:
Event<Mouse> dragEvents_ = E<Mouse>(Mouse::DRAG, Mouse::LEFT);   // left-button drags
Event<Mouse> scroll_     = E<Mouse>(Mouse::SCROLL);
Event<Mouse> move_       = E<Mouse>(Mouse::MOVE);
```

The `Event<T>` constructor installs a *node* that, every frame, calls
`gwe::fetch<T>(type, trigger)` and returns the result as a list. That
list is the value of the edge for that frame.

### 11.2 Using events

The most common pattern is to test whether the list is empty:

```cpp
auto anyPress = !F(&Mouse::List::empty, pressEvents_);
branch(anyPress)
    ->assign(RWS(params_.enabled_), !CS(params_.enabled_))
->endBranch();
```

That is from `beauty-demo.cpp`: any mouse press toggles the beauty
effect.

### 11.3 `Mouse` types

| Type            | What it represents                          |
|-----------------|---------------------------------------------|
| `Mouse::PRESS`  | A button was pressed down.                  |
| `Mouse::RELEASE`| A button was released.                      |
| `Mouse::CLICK`  | PRESS followed by RELEASE on the same spot.  |
| `Mouse::DRAG`   | A drag (movement with a button held).       |
| `Mouse::MOVE`   | Cursor moved.                               |
| `Mouse::SCROLL` | Scroll wheel event.                         |
| `Mouse::HOVER_ENTER` / `HOVER_EXIT` | Cursor entered/left the window. |

`Mouse::Button` values: `LEFT`, `RIGHT`, `MIDDLE`.

### 11.4 Disabling input

For automated tests or recording, set
`V4D::Keys::DISABLE_INPUT_EVENTS` to `true`. The event fetchers will
return empty lists every frame. (`imshow_reimplementation.cpp` does
this for its `TIME_TRACKER` flag too.)

### 11.5 The free-function API

If you need to fetch an event *outside* a `Property` (e.g. from a
`plain` lambda), use the `cv::v4d::event::fetch` helpers:

```cpp
using namespace cv::v4d::event;

if (!fetch<Mouse>(Mouse::PRESS).empty()) { /* ... */ }
// or:
consume(Mouse::PRESS);   // drains the queue
```

These are not graph nodes — they execute immediately. Use them only
in `setup()`, `teardown()`, or `gui()`.

---

## 12. GUI: ImGui nodes with `gui()` + `imgui(...)`

V4D has built-in ImGui. You get the context by passing
`AllocateFlags::IMGUI` to `V4D::init`. Your plan installs an ImGui node
by overriding `gui()`:

```cpp
void gui() override {
    imgui([](Params& params){
        using namespace ImGui;
        Begin("Effect");
        SliderFloat("Saturation", &params.skinSaturation_, 0.0f, 10.0f);
        End();
    }, RWS(params_));
}
```

### 12.1 The contract

`gui()` runs **once**, on the main thread, before the frame loop
begins. Inside it, `imgui(...)` records a node into a special
*UI transaction* attached to the main thread. That node is installed
into the ImGui context, which then drives it every frame from the
main thread on the display side.

The lambda you pass to `imgui(...)` runs on the **main thread** every
display refresh, NOT on a worker. That's a significant difference
from the rest of the DSL — your ImGui code does not participate in the
graph, and it can freely mutate shared state.

`beauty-demo.cpp:331-373` is the canonical example, with sliders, a
checkbox, a "Fullscreen" button that toggles a `set(...)` on
`V4D::Keys::FULLSCREEN`, and a status panel that reads
`params_.state_`.

### 12.2 Arguments to `imgui(...)`

`imgui(lambda, edges...)` records the lambda plus its edges (just
like `nvg(...)` or `fb(...)`). The edges are read under their
respective access intents — typically `RWS(member)` for things you
want the GUI to mutate, and `CS(member)` for things you only want to
read.

### 12.3 Why this is *not* in the graph

In the source you'll find this comment from `beauty-demo.cpp`:

> "at the moment gui is an exception from the rule that a Plan only
> implements the graph, because it runs on the display thread. in the
> future it should implement its own graph which would run in
> concurrent to the main algorithm - locking shared state where
> neccessary"

Practically this means:

* Don't share mutable C++ state between `gui()` and `infer()`
  without `_shared(...)` and `RWS/CS` access.
* `gui()`-only static members are fine — they live on the main
  thread's copy of the plan and are not visible to workers.
* If you ever want to mutate shared state from inside `gui()`, wrap
  the mutation in `RWS(member)` so the runtime knows.

---

## 13. Sharing state between GUI, workers and time

There are three classes of "state" in a V4D program, and each has a
different lifetime.

### 13.1 Per-worker (the default)

Plain C++ member variables are duplicated per worker thread:

```cpp
class MyPlan : public V4DPlan {
    cv::UMat scratch_;     // each worker has its own scratch_
    int      counter_ = 0; // each worker has its own counter_
    // ...
};
```

Each worker reads and writes its own copy. There is no
synchronization, no sharing. Use this for buffers, counters, scratch
state, DNN objects.

### 13.2 Shared across the program (`static` + `_shared`)

Declare a member `static` *and* pass it through `_shared(...)` in the
constructor:

```cpp
class BeautyDemoPlan : public V4DPlan {
    static Params params_;        // one Params for the entire program
    static FaceFeatures features_;
public:
    BeautyDemoPlan() {
        _shared(params_);
        _shared(features_);
    }
    // ...
};

Params BeautyDemoPlan::params_;          // definition
FaceFeatures BeautyDemoPlan::features_;  // definition
```

Now `params_` has a mutex. Access it with `RS(params_)` (read),
`RWS(params_)` (read-write), or `CS(params_)` (snapshot-copy). The
plan in `beauty-demo.cpp` does exactly this.

### 13.3 Per-thread state (`LocalState`)

`LocalState::Keys::WORKER_INDEX` is a per-thread counter that
identifies which worker you are. Mostly useful for diagnostic logs:

```cpp
Property<size_t> widx_ = P<size_t>(LocalState::Keys::WORKER_INDEX);
```

### 13.4 Global state (`GlobalState`)

Read with `P<T>(GlobalState::Keys::...)`, write with
`GlobalState::set<T>(...)` (or `set(GlobalState::Keys::..., edge)`).
Core keys: `FRAME_CNT`, `FPS`, `RUN_CNT`, `TIME_TRACKER`,
`WORKERS_READY`, etc.

### 13.5 Properties: V4D's typed `GlobalState`

The properties on `V4D::Keys` are typed wrappers over `GlobalState`
specifically for V4D state. Use those instead of stuffing things into
`GlobalState` directly.

---

## 14. Branching and loops in the per-frame body

`branch(...)`, `elseBranch()`, `endBranch()` are Plan-DSL primitives.
V4D re-exports them on `V4DPlan` so you can chain with `->`:

```cpp
branch(someCondition)
    ->plain(doA)
    ->plain(doB)
->elseBranch()
    ->plain(doC)
->endBranch();
```

For the full mechanics — predicates, branch types, predefined
predicates — see [`plan-dsl-programming-guide.markdown §8`](../../plan/doc/plan-dsl-programming-guide.markdown#8-control-flow-branch--elsebranch--endbranch).

### 14.1 The classic "branch on click to toggle" idiom

From `beauty-demo.cpp:394-421`:

```cpp
branch(
    RWS(params_.enabled_) = IF(
        F(&Mouse::List::empty, pressEvents_),    // if any press event…
        CS(params_.enabled_),                    //   …keep current
        !CS(params_.enabled_)                    //   …else flip
    )
)
    ->branch(seqCnt_ % V(uint64_t(8)) == V(uint64_t(0)))   // every 8th frame
        ->branch(!F(&FaceFeatureExtractor::extract, RW(extractor_), R(frames_.down_), RWS(features_)))
            ->assign(RWS(params_.state_), V(Params::NOT_DETECTED))
            ->plain(compose_result, RW(frames_), CS(params_))
        ->endBranch()
    ->endBranch()
    ->branch(!(F(&FaceFeatures::empty, RS(features_))))
        ->assign(RWS(params_.state_), V(Params::ON))
        ->subInfer(prepareFeatureMasksPlan_)
        ->subInfer(beautyFilterPlan_)
        ->plain(compose_result, RW(frames_), CS(params_))
    ->endBranch()
->elseBranch()
    ->plain(compose_result, RW(frames_), CS(params_))
    ->assign(RWS(params_.state_), V(Params::OFF))
->endBranch();
```

That single block is the entire state machine of the demo:
**if enabled and (every 8 frames try detection or features are ready),
run the effect; otherwise compose without it.**

The trick to read is `RWS(...) = IF(...)` — this is *one node*: the
branch's predicate is the result of assigning the new value back to
`params_.enabled_`. The whole edge expression evaluates to a `bool`
edge; that bool is the predicate.

### 14.2 Loops

Loops are branches whose body updates the predicate (see
[`plan-dsl-programming-guide.markdown §12`](../../plan/doc/plan-dsl-programming-guide.markdown#12-loops-as-a-special-case-of-branches)).
The DSL provides no `while` / `for` keywords.

### 14.3 One-shot init: `BranchType::ONCE`

```cpp
void setup() override {
    branch(BranchType::ONCE, always_)
        ->assign(RW(timeOffset_), F(seconds))
        ->construct(RW(text_), /*...*/)
    ->endBranch();
}
```

The body runs exactly once globally (across all workers, the first
time the graph reaches that branch). After that it is permanently
disabled.

---

## 15. Sub-plans: composing large programs

Sub-plans are `Plan` (or `V4DPlan`) instances owned by another plan.
You construct them in the parent's constructor with `_sub<T>(...)`:

```cpp
class BeautyDemoPlan : public V4DPlan {
    cv::Ptr<FaceFeatureMasksPlan> prepareFeatureMasksPlan_;
    cv::Ptr<BeautyFilterPlan>      beautyFilterPlan_;
public:
    BeautyDemoPlan() {
        prepareFeatureMasksPlan_ = _sub<FaceFeatureMasksPlan>(this, features_, frames_);
        beautyFilterPlan_        = _sub<BeautyFilterPlan>(this, params_, frames_);
    }
    // ...
};
```

The sub-plan receives references to the parent's state. From the
parent's `infer()` you splice the sub-plan's graph in:

```cpp
void infer() override {
    // ...
    subInfer(prepareFeatureMasksPlan_);   // emits the sub-plan's infer() graph
    subInfer(beautyFilterPlan_);
    // ...
}
```

There are also `subSetup(...)` and `subTeardown(...)`. The spliced
graph inherits the enclosing branch's predicate, which is the
primary tool for conditional sub-pipelines.

### 15.1 Why use sub-plans?

* **Organization.** A 500-line `infer()` is hard to read; three
  `FaceFeatureMasksPlan`, `BeautyFilterPlan`, `BeautyDemoPlan`
  classes are easy.
* **Reuse.** The same `FaceFeatureMasksPlan` could be plugged into a
  different parent.
* **Conditional logic.** Splicing only inside a branch makes the
  sub-pipeline conditional.

### 15.2 A sub-plan example

```cpp
class FaceFeatureMasksPlan : public V4DPlan {
    const FaceFeatures& inputFeatures_;
    BeautyDemoPlan::Frames& inputOutputFrames_;
public:
    FaceFeatureMasksPlan(const FaceFeatures& f, BeautyDemoPlan::Frames& io)
      : inputFeatures_(f), inputOutputFrames_(io) {}

    void infer() override {
        nvg(&FaceFeatures::drawFaceOvalMask, RS(inputFeatures_))
        ->fb(cv::cvtColor, RW(inputOutputFrames_.faceOval_),
             V(cv::COLOR_BGRA2GRAY), V(0), V(cv::ALGO_HINT_DEFAULT))
        ->nvg(&FaceFeatures::drawEyesAndLipsMask, RS(inputFeatures_))
        ->fb(cv::cvtColor, RW(inputOutputFrames_.eyesAndLipsMaskGrey_),
             V(cv::COLOR_BGRA2GRAY), V(0), V(cv::ALGO_HINT_DEFAULT))
        ->plain(prepare_masks, RW(inputOutputFrames_));
    }
};
```

The sub-plan inherits the parent's `BeautyDemoPlan::Frames` reference
so that its writes go to the same buffers the parent will later
read. This is the canonical pattern for sharing scratch buffers
across sub-plans.

---

## 16. Threads, workers and contention

`V4DPlan::run<Tplan>(N)` spawns:

| N   | Threads                                            |
|-----|----------------------------------------------------|
| `-1`| 1 worker + main (default; rarely used)             |
| `0` | 1 worker + main                                    |
| `1` | 2 workers + main                                   |
| `6` | 7 workers + main (e.g. `beauty-demo`)              |

`0` is the most common — you get one worker and the main thread
runs the display/event loop. Use higher numbers when you have heavy
parallel work (e.g. DNN inference in one branch, GL rendering in
another).

### 16.1 What gets shared

* `GlobalState` — one table for the whole program.
* `_shared(...)` variables — one mutex per variable.
* The runtime (`runtime_->`) — singleton.

### 16.2 What does *not* get shared

* Plan member variables — duplicated per worker.
* Local state — per-thread.
* `gui()`'s view of the plan — only the main thread.

### 16.3 Synchronization

The DSL gives you three access intents for shared variables:

| Edge    | Behavior                                            |
|---------|-----------------------------------------------------|
| `RS(x)` | Acquire the mutex, read, release.                   |
| `RWS(x)`| Acquire the mutex, read-write, release.             |
| `CS(x)` | Acquire the mutex, **copy** the value, release.     |

Use `CS(x)` whenever a downstream node reads from a snapshot — the
read is decoupled from the lock window. Use `RWS(x)` when you need
read-modify-write semantics.

### 16.4 `BranchType::SINGLE` and `BranchType::ONCE`

Sometimes you want only one worker to run a side-effecting node:

```cpp
branch(BranchType::SINGLE, isTrue_(someFlag))
    ->plain([](const Frame& f){ dumpToDisk(f); }, R(scratch_))
->endBranch();
```

The runtime acquires a global mutex inside the region so at most one
worker executes it. `ONCE` is the same but sticky: the branch never
runs again.

---

## 17. Custom Sources and Sinks

The factory functions cover the common cases. For anything else,
construct `Source` / `Sink` from a functor:

### 17.1 Custom Source

```cpp
cv::Ptr<Source> src = new Source([](cv::UMat& frame) -> bool {
    // Initialize on first call.
    if (frame.empty()) {
        frame.create(Size(960, 960), CV_8UC3);
    }
    // Fill the frame with the next color in a rainbow.
    uchar hue = (int64_t(seconds() * 15) % 255);
    frame = convert_pix<cv::COLOR_HLS2RGB_FULL>(cv::Vec3b(hue, 128, 255));
    return true;     // false = end of stream
}, /*fps=*/60.f);
```

`custom_source_and_sink.cpp` shows this pattern.

### 17.2 Custom Sink

```cpp
cv::Ptr<videoSink> videoSink = Sink::make(runtime, "out.mkv", 30, viewport.size());

cv::Ptr<Sink> sink = new Sink([videoSink](const uint64_t& seq, const cv::UMat& frame) -> bool {
    // Do something with the frame…
    videoSink->operator()(seq, frame);
    return videoSink->isOpen();   // false = temporary error
});
```

Note the sink functor takes `(uint64_t seq, const cv::UMat& frame)`.
The `seq` is the global frame counter; use it to drop, reorder or
deduplicate frames if you need to.

### 17.3 Conditional writes with `branch` + `write`

`custom_source_and_sink.cpp` is also a great example of conditional
output:

```cpp
branch(&PureColor::found, R(finder_))
    ->write()
->endBranch();
```

Inside the `branch` (where the predicate is true), `write()` pushes
the default buffer. Outside it, no frame is emitted. The sink's fps
(`10` here) determines the output rate independent of the input
rate.

---

## 18. Putting it together: anatomy of `video_editing.cpp`

The simplest possible "real" V4D program:

```cpp
#include <opencv2/v4d/v4d.hpp>
using namespace cv;
using namespace cv::v4d;

class VideoEditingPlan : public V4DPlan {
    cv::UMat frame_;
    const string hv_ = "Hello Video!";
    Property<cv::Size> sz_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    void infer() override {
        capture();                                  // 1. pull a frame
        nvg([](const Size& sz, const string& str) {
            using namespace cv::v4d::nvg;
            fontSize(40.0f);
            fontFace("sans-bold");
            fillColor(Scalar(255, 0, 0, 255));
            textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            text(sz.width / 2.0, sz.height / 2.0,
                 str.c_str(), str.c_str() + str.size());
        }, sz_, R(hv_));                            // 2. draw on top
        write();                                    // 3. push
    }
};

int main(int argc, char** argv) {
    if (argc != 3) { /* error */ }
    cv::Rect viewport(0, 0, 960, 960);
    Ptr<V4D> runtime = V4D::init(viewport, "Video Editing",
                                 AllocateFlags::NANOVG | AllocateFlags::IMGUI);

    auto src  = Source::make(runtime, argv[1]);
    auto sink = Sink::make(runtime, argv[2], src->fps(), viewport.size());
    runtime->setSource(src);
    runtime->setSink(sink);

    V4DPlan::run<VideoEditingPlan>(0);
}
```

Walking through:

* **`V4DPlan` instead of `Plan`.** You inherit from `V4DPlan` to
  gain the V4D-flavored helpers (`capture()`, `write()`, `nvg(...)`,
  the `Event<>` and `Property<>` shortcuts, etc.). Internally it is
  the same DSL.
* **`frame_` is unused.** It is a hint to the reader that a default
  capture buffer is bound. You can omit it.
* **`Property<cv::Size> sz_`.** Reads the framebuffer size. Updates
  on resize.
* **`capture()` / `write()` with no arguments.** Use the default
  capture buffer / writer buffer.
* **`nvg(lambda, sz_, R(hv_))`.** Two edges: `size_` (read-only,
  re-fetched every frame), `R(hv_)` (read of `hv_`).
* **`AllocateFlags::NANOVG | AllocateFlags::IMGUI`.** Both
  subsystems because we want NanoVG drawing and a (future) ImGui
  window.
* **`Source::make(runtime, "in.mp4")`.** Auto-detects format, uses
  `cv::VideoCapture`.
* **`V4DPlan::run<VideoEditingPlan>(0)`.** One worker + main. Use a
  larger number for parallelism.

Build with `make example_v4d_video_editing` (or whatever target the
CMake generates) and run as
`./example_v4d_video_editing in.mp4 out.mkv`.

---

## 19. A larger example: anatomy of `beauty-demo.cpp`

`beauty-demo.cpp` (522 lines) is the most representative V4D program
in the tree. It composes shared state, sub-plans, branching with
`IF`, mouse events, frame counters, NanoVG, framebuffer writes, and a
full ImGui GUI.

### 19.1 State layout

```cpp
class BeautyDemoPlan : public V4DPlan {
public:
    struct Params { /* tunables */ };
    struct Frames { /* per-worker scratch buffers */ };
private:
    static Params        params_;        // shared, mutated by GUI
    static FaceFeatures  features_;      // shared, written by extractor
    cv::Ptr<FaceFeatureExtractor> extractor_;
    float scale_ = 1;
    const cv::Size downSize_ = {640, 360};
    Frames frames_;                      // per-worker

    Property<cv::Size> size_    = P<cv::Size>(V4D::Keys::SIZE);
    Property<uint64_t> seqCnt_  = P<uint64_t>(GlobalState::Keys::FRAME_CNT);
    Event<Mouse>       pressEvents_ = E<Mouse>(Mouse::Type::PRESS);
    // ...
};
```

`params_` and `features_` are *static* so they live across plan
instances. `_shared(params_)` in the constructor (see source) gives
them a mutex; workers access via `RWS(...)` / `CS(...)`.

### 19.2 The GUI

```cpp
void gui() override {
    imgui([](Params& params) {
        using namespace ImGui;
        Begin("Effect");
        Checkbox("Side by side", &params.sideBySide_);
        // ...
        SliderFloat("Saturation", &params.skinSaturation_, 0.0f, 10.0f);
        // ...
        if (Button("Fullscreen")) {
            params.fullscreen_ = !params.fullscreen_;
        }
        // ...
    }, RWS(params_));
}
```

`gui()` runs once on the main thread; the ImGui node it installs is
re-invoked every frame on the main thread, mutating `params_` under
the shared mutex. Workers read it with `CS(params_)`.

### 19.3 `setup()`: one-time DNN construction

```cpp
void setup() override {
    assign(RW(scale_), F(aspect_preserving_scale, size_, R(downSize_)));
    plain(setLogLevel, V(LOG_LEVEL_WARNING))
    ->construct(RW(extractor_), R(downSize_), R(scale_))
    ->plain(setLogLevel, V(LOG_LEVEL_INFO));
}
```

`aspect_preserving_scale` is computed and assigned to `scale_`. Then
the log is quieted, the face-feature DNN is constructed (a heavy,
one-time cost), and the log level is restored. Each step is one node;
the chain is one continuous setup graph.

### 19.4 `infer()`: the per-frame state machine

(See §14.1 for the full source.)

The pipeline is:

1. **`set(V_::FULLSCREEN, CS(params_.fullscreen_))`** — propagate
   GUI changes to the runtime.
2. **`capture(RW(frames_.orig_))`** — pull the next video frame.
3. **`plain(prepare_frames, R(downSize_), RW(frames_))`** —
   downsample, color-convert, split into several buffers.
4. **The big branch.** Toggle `enabled_` on mouse press; every 8
   frames try face detection; if features are ready, run the
   effect; otherwise compose without it.
5. **`fb<1>(cv::cvtColor, R(frames_.result_), …)`** — write the
   composed result into the visible framebuffer (slot 1).
6. **`write(R(frames_.result_))`** — push the result to the sink
   (in this demo, no sink is configured, so this is a no-op; the
   visible window is the only output).

### 19.5 The sub-plans

`FaceFeatureMasksPlan` and `BeautyFilterPlan` (§15) break the
pipeline into composable pieces. The parent splices them in:

```cpp
subInfer(prepareFeatureMasksPlan_);
subInfer(beautyFilterPlan_);
```

…and they run with the parent's branch predicate inherited.

---

## 20. Cheat-sheet

### Includes and namespaces

```cpp
#include <opencv2/v4d/v4d.hpp>
using namespace cv;
using namespace cv::v4d;
using namespace cv::v4d::nvg;     // for NanoVG calls inside nvg(...) lambdas
using namespace cv::v4d::event;   // for Mouse::Type etc. outside event edges
```

### Initialization

```cpp
cv::Ptr<V4D> rt = V4D::init(viewport, "Title",
                            AllocateFlags::NANOVG | AllocateFlags::IMGUI,
                            ConfigFlags::DEFAULT,
                            DebugFlags::DEFAULT,
                            /*msaa*/ 0);
```

### Plan skeleton

```cpp
class MyPlan : public V4DPlan {
    cv::UMat scratch_;
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    void setup()    override { /* one-shot init */ }
    void infer()    override { /* per-frame body */ }
    void gui()      override { /* main-thread UI */ }
    void teardown() override { /* one-shot cleanup */ }
};
```

### Sources and Sinks

```cpp
auto src  = Source::make(rt, "in.mp4");
auto sink = Sink::make(rt, "out.mkv", src->fps(), viewport.size());
rt->setSource(src);
rt->setSink(sink);
// or roll your own:
auto src = new Source([](cv::UMat& f){ /* fill f */ return true; }, 60.f);
```

### Capture and write

```cpp
capture();                          // default buffer
capture(RW(myBuf));                 // into myBuf
capture([](const cv::UMat& in, cv::UMat& out){ /* transform */ }, RW(myBuf));

write();                            // default writer
write(R(myBuf));
write([](cv::UMat& out, const cv::UMat& in){ /* transform */ }, R(myBuf));
```

### Rendering contexts

```cpp
nvg(lambda, edges...);              // NanoVG drawing context
fb<pos>(fn, args...);               // framebuffer access (auto fb UMat at pos)
gl(fn, args...);                    // OpenGL
gl<-1>(V(ctxIdx), fn, args...);     // specific OpenGL context
bgfx(fn, args...);                  // bgfx (if enabled)
ext(fn, args...);                   // external renderer
plain(fn, args...);                 // plain CPU context
```

### Properties and events

```cpp
Property<cv::Size>  size_   = P<cv::Size>(V4D::Keys::SIZE);
Property<uint64_t>  seqCnt_ = P<uint64_t>(GlobalState::Keys::FRAME_CNT);
Property<size_t>    widx_   = P<size_t>(LocalState::Keys::WORKER_INDEX);

set(V4D::Keys::CLEAR_COLOR, V(cv::Scalar(0, 0, 0, 255)));
set(V4D::Keys::FULLSCREEN,  CS(params_.fullscreen_));

Event<Mouse> press_ = E<Mouse>(Mouse::Type::PRESS);
Event<Mouse> drag_  = E<Mouse>(Mouse::DRAG, Mouse::LEFT);
auto anyPress = !F(&Mouse::List::empty, press_);
```

### GUI

```cpp
void gui() override {
    imgui([](Params& p) {
        using namespace ImGui;
        Begin("Settings");
        SliderFloat("X", &p.x_, 0.0f, 1.0f);
        End();
    }, RWS(params_));
}
```

### Sharing state

```cpp
// In header:
static Params params_;

// In ctor:
_shared(params_);

// Access:
RS(params_)          // read under lock
RWS(params_)         // read/write under lock
CS(params_)          // snapshot under lock
```

### Branching

```cpp
branch(pred)
    ->plain(doA)
->elseBranch()
    ->plain(doB)
->endBranch();

branch(BranchType::SINGLE, pred)
    ->plain(serializedSideEffect)
->endBranch();

branch(BranchType::ONCE, always_)
    ->plain(initializeOnce)
->endBranch();
```

### Sub-plans

```cpp
// In parent ctor:
auto sub_ = _sub<SubPlan>(this, /*ctor args*/);

// In parent infer():
subInfer(sub_);     // splice sub->infer() graph
subSetup(sub_);     // splice sub->setup() graph
subTeardown(sub_);  // splice sub->teardown() graph
```

### Lifecycle entry point

```cpp
int main() {
    cv::Ptr<V4D> rt = V4D::init(viewport, "Title",
                                AllocateFlags::NANOVG | AllocateFlags::IMGUI);
    // ... wire source/sink ...
    V4DPlan::run<MyPlan>(/*workers=*/0);
}
```

---

## Where to go next

* [`plan-dsl-programming-guide.markdown`](../../plan/doc/plan-dsl-programming-guide.markdown) — the DSL tutorial. Read this if you skipped ahead.
* [`plan-dsl-reference.markdown`](../../plan/doc/plan-dsl-reference.markdown) — every opcode, every overload, every branch type. Keep it open while you write.
* `modules/v4d/samples/` — the canonical examples. Start with:
  * `font_rendering.cpp` — minimum NanoVG program.
  * `render_opengl.cpp` — minimum OpenGL program.
  * `display_image_fb.cpp` / `display_image_nvg.cpp` — minimum
    image-display programs (`fb` vs `nvg`).
  * `video_editing.cpp` — capture → nvg → write.
  * `video-demo.cpp` — capture → gl → write.
  * `cube-demo.cpp` / `many_cubes-demo.cpp` — pure GL rendering.
  * `font_with_gui.cpp` — GUI feeding into NanoVG.
  * `custom_source_and_sink.cpp` — rolling your own I/O.
  * `pedestrian-demo.cpp` / `optflow-demo.cpp` — non-trivial
    pipelines with detection + tracking.
  * `beauty-demo.cpp` — the kitchen sink.
  * `imshow_reimplementation.cpp` — a full GUI image viewer.
* `modules/v4d/include/opencv2/v4d/v4d.hpp` — the entry header.
  When in doubt, read it; everything in this guide is just an
  ergonomic summary of what's there.