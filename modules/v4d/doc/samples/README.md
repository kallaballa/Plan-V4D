# Plan-V4D Walkthrough Tutorial Series

> A comprehensive, hands-on guide to the Plan-V4D framework — from displaying your first image to building a full-featured `imshow` reimplementation.

---

## Table of Contents

| # | Tutorial | Topics |
|---|----------|--------|
| 00 | [An Introduction to Plan-V4D](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/00-intro.markdown) | Core philosophy, Record Once/Replay Forever, edge-calls, contexts |
| 01 | [Displaying an Image with NanoVG](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/01-display_image_nvg.markdown) | `V4DPlan`, `setup()`/`infer()`, `nvg` context, NanoVG image loading |
| 02 | [Displaying an Image via Framebuffer](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/02-display_image_fb.markdown) | `plain` context, `fb` context, direct framebuffer access, `DISPLAY_MODE` |
| 03 | [2D Vector Graphics with NanoVG](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/03-vector_graphics.markdown) | NanoVG drawing API, gradients, time-based animation |
| 04 | [Combining Vector Graphics and Framebuffer Processing](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/04-vector_graphics_and_fb.markdown) | Context chaining, `boxFilter` on framebuffer |
| 05 | [Direct OpenGL Rendering](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/05-render_opengl.markdown) | `gl` context, `V()` edge-call, OpenGL state preservation |
| 06 | [Font Rendering](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/06-font_rendering.markdown) | `R()` edge-call, NanoVG text API, custom font loading |
| 07 | [Simple Video Editing](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/07-video_editing.markdown) | `Source`/`Sink`, `capture()`, `write()` |
| 08 | [Custom Source and Sink](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/08-custom_source_and_sink.markdown) | Lambda sources/sinks, `branch()` conditional logic |
| 09 | [Font Rendering with a GUI](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/09-font_with_gui.markdown) | `gui()` method, `imgui` context, `RWS`/`CS` shared edges |
| 10 | [Rendering a 3D Cube](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/10-cube.markdown) | `teardown()`, separation of concerns, `set()`/`clear()` |
| 11 | [Compositing 3D Graphics on Video](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/11-video.markdown) | Compositing pipeline, `capture()` as background |
| 12 | [Advanced NanoVG and Processing Pipelines](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/12-nanovg.markdown) | `assign()`, `F()`, function wrappers, hue-shifting chain |
| 13 | [Interactive Custom Shaders](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/13-shader.markdown) | `E<T>` events, GLSL shaders, `branch` stateful control flow |
| 14 | [Advanced Font Effects Demo](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/14-font.markdown) | Render-to-texture, conditional execution, performance optimization |
| 15 | [Pedestrian Detection and Tracking Demo](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/15-pedestrian.markdown) | HOG detection, KCF tracking, `branch`/`elseBranch` |
| 16 | [Sparse Optical Flow Demo](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/16-optflow.markdown) | FAST features, Lucas-Kanade, multi-layer compositing |
| 17 | [Real-Time "Beauty Filter" Demo](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/17-beauty.markdown) | Sub-plans (`_sub`/`subInfer`), DNN models, `MultiBandBlender` |
| 18 | [Parallel Rendering with Multiple OpenGL Contexts](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/18-many-cubes.markdown) | `gl<-1>` multi-context, parallel OpenGL execution |
| 19 | [An Interactive Image Carousel](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/19-image_carousel.markdown) | `createImageRGBA`, `_shared`, event lists, perspective layout |
| 20 | [Reimplementing OpenCV's imshow](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/20-imshow_reimplementation.markdown) | Full-featured viewer, deep zoom, ImGui menus, file dialogs |

---

# Tutorial 00 — An Introduction to Plan-V4D

> **Source:** [`00-intro.markdown`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/00-intro.markdown) | **Next:** [Displaying an Image with NanoVG](#tutorial-01--displaying-an-image-with-nanovg)

Welcome to the Plan-V4D tutorial series. Before we dive into the code, it's important to understand the core philosophy behind the framework, as it is different from traditional, sequential programming. This introduction gives you the mental model you need to get the most out of the examples that follow.

## What is V4D?

V4D (Visualization for Video and Data) is a runtime built on top of Plan-DSL, a small graph-recording language embedded in C++. Plan-DSL is the core; V4D adds four things on top of it:

1. A **window and event loop** built on GLFW + OpenGL, with optional NanoVG (2D vector graphics) and ImGui (immediate-mode GUI) layers.
2. A **Source / Sink abstraction** — a `Plan` can read frames from a video file, a webcam, or any functor (`Source`), and write them to a file, a stream, or anything else (`Sink`).
3. **Side-effect contexts** — a way to schedule a C++ lambda or function onto a specific pipeline: the framebuffer (`fb`), NanoVG (`nvg`), OpenGL (`gl`), bgfx / external renderers (`bgfx`, `ext`), ImGui (`imgui`), or plain CPU (`plain`).
4. **`V4D::Keys` properties** — typed views onto runtime state (framebuffer size, viewport, fullscreen flag, etc.).

That is the whole system. Everything else in this tutorial is elaboration.

## The One Mental Model: Record Once, Replay Forever

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

## Interacting with the Graph: Edges

Because the graph is built by the compiler, the compiler needs to understand exactly how each task (each node in the graph) interacts with the application's data. You provide this crucial information using **edge-calls**.

Edge-calls are small wrappers around your variables that declare your *intent*. They tell the graph whether you intend to read, write, or copy a piece of data. This declaration is what allows the Plan engine to automatically manage data, prevent race conditions, and schedule tasks for maximum parallelism.

Here are the primary edge-calls you will encounter:

| Edge-Call | Name | Purpose |
| :--- | :--- | :--- |
| `V(value)` | **V**alue | Passes a constant value or literal directly to a function. |
| `R(variable)` | **R**ead | Provides safe, read-only access to a variable. |
| `RW(variable)` | **R**ead-**W**rite | Provides read-write access to a variable. |
| `RS(variable)` | **R**ead **S**hared | Provides thread-safe, read-only access to data shared between contexts. |
| `RWS(variable)` | **R**ead-**W**rite **S**hared | Provides thread-safe, read-write access to shared data. |
| `CS(variable)` | **C**opy **S**hared | Provides a thread-safe copy of shared data. |
| `F(fn, args…)` | **F**unction | Wraps a free function as a node in the graph. |
| `E<T>(…)` | **E**vent | Captures user input events (mouse, keyboard, joystick, window). |
| `P<T>(key)` | **P**roperty | A read-only edge bound to a value in `GlobalState` or `LocalState`. |

Using the most restrictive edge-call possible is a best practice that helps the Plan engine generate the most optimal graph.

## Helper Macros

When wrapping existing OpenCV functions or class methods so the engine can dispatch them, the `util.hpp` header provides a small set of pointer-cast macros (see [`modules/v4d/include/opencv2/v4d/util.hpp`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/include/opencv2/v4d/util.hpp)):

- `_OL_(r, fn, …)` — overload of a free function.
- `_OLC_(r, fn, …)` — const-qualified overload of a free function.
- `_OLM_(r, C, &C::fn, …)` — overload of a non-const member function.
- `_OLMC_(r, C, &C::fn, …)` — overload of a const member function.

They produce the typed `static_cast<r (*)(…)>(fn)` you would otherwise have to write by hand.

## Composing Graphs

Beyond context calls, `V4DPlan` exposes a few graph-level primitives (see `plan.hpp`):

- `assign(edges…)` — evaluate an expression made of edges and store the result.
- `op(edges…)` — evaluate a free-function graph node.
- `construct(edges…)` — construct an object via a graph node.
- `IF(c, t, e)`, `DIV`, `OR`, `INCR`, `DECR`, etc. — small DSL combinators you can use inside expressions.
- `branch(cond, …) -> … -> endBranch()` — conditional sub-graph.
- `subInfer(subPlan)` — splice another `V4DPlan`'s `infer()` into this one.

## The V4D Runtime: Your Toolkit

If `Plan` is the blueprint, then `V4D` is the toolbox. The V4D runtime provides the set of tools — called **contexts** — that you can use as nodes in your graph:

- **`nvg`**: For 2D vector graphics and text via NanoVG.
- **`fb`**: For direct access to the framebuffer as a `cv::UMat`.
- **`gl`**: For executing raw OpenGL commands. `gl<-1>(V(idx), …)` routes the call to a worker OpenGL context for parallelism.
- **`bgfx`**: For raw bgfx calls.
- **`ext`**: For arbitrary, runtime-specific contexts.
- **`imgui`**: For creating user interfaces with Dear ImGui.
- **`plain`**: For running general-purpose code, like standard OpenCV functions.

In addition, `V4D` provides a `Source` / `Sink` system, exposed inside a `V4DPlan` as the `capture()` and `write()` graph calls.

## Lifecycle of a `V4DPlan`

A `V4DPlan` (see [`v4d.hpp:422`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/include/opencv2/v4d/v4d.hpp)) inherits from `Plan` and adds four virtual hooks you can override:

- `setup()` — emitted once, before the frame loop. Use it to allocate resources.
- `infer()` — emitted every frame. Use it for the main rendering/processing pipeline.
- `gui()` — emitted once on the display thread, before the frame loop. Use it for ImGui UI.
- `teardown()` — emitted once after the frame loop exits. Use it to free resources.

## Getting Started

Every tutorial links to its complete source sample in the [samples directory](https://github.com/kallaballa/Plan-V4D/tree/beta/modules/v4d/samples).

1. [Tutorial 01 — Displaying an Image with NanoVG](#tutorial-01--displaying-an-image-with-nanovg) (`display_image_nvg.cpp`)
2. [Tutorial 02 — Displaying an Image via Framebuffer](#tutorial-02--displaying-an-image-via-framebuffer) (`display_image_fb.cpp`)
3. [Tutorial 03 — 2D Vector Graphics with NanoVG](#tutorial-03--2d-vector-graphics-with-nanovg) (`vector_graphics.cpp`)
4. [Tutorial 04 — Combining Vector Graphics and Framebuffer](#tutorial-04--combining-vector-graphics-and-framebuffer-processing) (`vector_graphics_and_fb.cpp`)
5. [Tutorial 05 — Direct OpenGL Rendering](#tutorial-05--direct-opengl-rendering) (`render_opengl.cpp`)
6. [Tutorial 06 — Font Rendering](#tutorial-06--font-rendering) (`font_rendering.cpp`)
7. [Tutorial 07 — Simple Video Editing](#tutorial-07--simple-video-editing) (`video_editing.cpp`)
8. [Tutorial 08 — Custom Source and Sink](#tutorial-08--custom-source-and-sink) (`custom_source_and_sink.cpp`)
9. [Tutorial 09 — Font Rendering with a GUI](#tutorial-09--font-rendering-with-a-gui) (`font_with_gui.cpp`)
10. [Tutorial 10 — Rendering a 3D Cube](#tutorial-10--rendering-a-3d-cube) (`cube-demo.cpp`, `cubescene.hpp`)
11. [Tutorial 11 — Compositing 3D Graphics on Video](#tutorial-11--compositing-3d-graphics-on-video) (`video-demo.cpp`)
12. [Tutorial 12 — Advanced NanoVG and Processing Pipelines](#tutorial-12--advanced-nanovg-and-processing-pipelines) (`nanovg-demo.cpp`)
13. [Tutorial 13 — Interactive Custom Shaders](#tutorial-13--interactive-custom-shaders) (`shader-demo.cpp`)
14. [Tutorial 14 — Advanced Font Effects Demo](#tutorial-14--advanced-font-effects-demo) (`font-demo.cpp`)
15. [Tutorial 15 — Pedestrian Detection and Tracking Demo](#tutorial-15--pedestrian-detection-and-tracking-demo) (`pedestrian-demo.cpp`)
16. [Tutorial 16 — Sparse Optical Flow Demo](#tutorial-16--sparse-optical-flow-demo) (`optflow-demo.cpp`)
17. [Tutorial 17 — Real-Time "Beauty Filter" Demo](#tutorial-17--real-time-beauty-filter-demo) (`beauty-demo.cpp`)
18. [Tutorial 18 — Parallel Rendering with Multiple OpenGL Contexts](#tutorial-18--parallel-rendering-with-multiple-opengl-contexts) (`many_cubes-demo.cpp`)
19. [Tutorial 19 — An Interactive Image Carousel](#tutorial-19--an-interactive-image-carousel) (`image_carousel.cpp`)
20. [Tutorial 20 — Reimplementing OpenCV's imshow](#tutorial-20--reimplementing-opencvs-imshow) (`imshow_reimplementation.cpp`)

Ready to dive in? Let's start by displaying a simple image.

---

# Tutorial 01 — Displaying an Image with NanoVG

> **Source:** [`01-display_image_nvg.markdown`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/01-display_image_nvg.markdown) | [Introduction](#tutorial-00--an-introduction-to-plan-v4d) | [Next: Displaying an Image via Framebuffer](#tutorial-02--displaying-an-image-via-framebuffer)

This tutorial demonstrates how to load and display an image using Plan-V4D's NanoVG context. We will create a simple `V4DPlan` that loads an image during its setup phase and then renders it to the screen in a continuous loop.

## The Code

Here is the complete source code for this example. You can find it in [`display_image_nvg.cpp`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/samples/display_image_nvg.cpp).

```cpp
#include <opencv2/v4d/v4d.hpp>
#include <opencv2/imgcodecs.hpp>

using namespace cv;
using namespace cv::v4d;

class DisplayImageNVG : public V4DPlan {
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);

    // Struct to hold image metadata and NanoVG paint object
    struct Image_t {
        std::string filename_; // Image file name
        nvg::Paint paint_;     // NanoVG paint object for the image
        int w_;                // Image width
        int h_;                // Image height
    } image_;

public:
    DisplayImageNVG(const std::string& filename) {
        image_.filename_ = filename;
    }

    void setup() override {
        nvg([](Image_t& img) {
            using namespace cv::v4d::nvg;
            int handle = createImage(img.filename_.c_str(), NVG_IMAGE_NEAREST);
            CV_Assert(handle > 0);
            imageSize(handle, &img.w_, &img.h_);
            img.paint_ = imagePattern(0, 0, img.w_, img.h_, 0.0f / 180.0f * NVG_PI, handle, 1.0);
        }, RW(image_));
    }

    void infer() override {
        nvg([](const cv::Size& sz, const Image_t& img) {
            using namespace cv::v4d::nvg;
            beginPath();
            scale(double(sz.width) / img.w_, double(sz.height) / img.h_);
            roundedRect(0, 0, img.w_, img.h_, 50);
            fillPaint(img.paint_);
            fill();
        }, size_, R(image_));
    }
};

int main() {
    cv::Rect viewport(0, 0, 960, 960);
    Ptr<V4D> runtime = V4D::init(viewport, "Display an image using NanoVG", AllocateFlags::NANOVG | AllocateFlags::IMGUI);
    V4DPlan::run<DisplayImageNVG>(0, samples::findFile("lena.png"));
}
```

## Code Breakdown

### 1. The `DisplayImageNVG` Plan

We define a class `DisplayImageNVG` that inherits from `cv::v4d::V4DPlan`. Inside the class, we define a `struct Image_t` to hold all the data related to our image: its filename, its dimensions, and a `nvg::Paint` object that NanoVG will use to render it.

### 2. The `setup()` Phase

The `setup()` method is called once when the `Plan` is initialized. This is the perfect place to load resources and perform one-time setup tasks.

- **`nvg([…], RW(image_))`**: This is a **NanoVG context**. The lambda function passed to it will be executed within a valid NanoVG rendering environment.
- **`RW(image_)`**: This specifies that the lambda needs **read-write** access to our `image_` struct.
- **`createImage(…)`**: This NanoVG function loads the image from the specified file and returns a handle to it.
- **`imagePattern(…)`**: We create a `paint` from the image. This paint can then be used to fill shapes, effectively drawing the image.

### 3. The `infer()` Phase

The `infer()` method is called repeatedly in a loop.

- **`nvg([…], size_, R(image_))`**: We open a NanoVG context, passing in the window size and read-only access to our image data.
- **`scale(…)`**: We scale the rendering context to make the image fit the window.
- **`roundedRect(…)`**: We create a rounded rectangle shape with the same dimensions as our image.
- **`fillPaint(img.paint_)`**: We set the fill style to our image pattern.
- **`fill()`**: We fill the rectangle, which draws the image to the screen.

### 4. The `main()` Function

- **`V4D::init(…)`**: Initializes the V4D runtime with a specified window size and title. We also pass flags to enable the `NANOVG` and `IMGUI` subsystems.
- **`V4DPlan::run<DisplayImageNVG>(…)`**: This static method creates an instance of our `DisplayImageNVG` plan, passes the filename "lena.png" to its constructor, and starts the execution loop. The first argument (`0`) selects the worker count; `0` means one worker plus the main thread.

## Summary

In this tutorial, we've seen how to:

- Create a `V4DPlan` to structure a graphical application.
- Use the `setup()` phase for one-time resource loading.
- Use the `infer()` phase for continuous rendering.
- Utilize the `nvg` context to perform 2D drawing operations with NanoVG.
- Pass data to our rendering lambdas using Plan's property and data access system.

---

# Tutorial 02 — Displaying an Image via Framebuffer

> **Source:** [`02-display_image_fb.markdown`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/02-display_image_fb.markdown) | [Displaying an Image with NanoVG](#tutorial-01--displaying-an-image-with-nanovg) | [Next: 2D Vector Graphics with NanoVG](#tutorial-03--2d-vector-graphics-with-nanovg)

This tutorial explains how to display an image by writing its data directly to the window's framebuffer. This method offers a more direct way to put pixels on the screen compared to using a graphics library like NanoVG.

## The Code

You can find the complete source in [`display_image_fb.cpp`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/samples/display_image_fb.cpp).

```cpp
#include <opencv2/v4d/v4d.hpp>
#include <opencv2/imgcodecs.hpp>

using namespace cv;
using namespace cv::v4d;

class DisplayImageFB : public V4DPlan {
    UMat image_;
    UMat converted_;
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    DisplayImageFB(const string& filename) {
        imread(filename).copyTo(image_);
    }

    void setup() override {
        plain([](const cv::Size& sz, cv::UMat& image, cv::UMat& converted) {
            resize(image, converted, sz);
            cvtColor(converted, converted, COLOR_RGB2BGRA);
        }, size_, RW(image_), RW(converted_));
    }

    void infer() override {
        fb([](UMat& framebuffer, const cv::UMat& c){
            c.copyTo(framebuffer);
        }, R(converted_));
    }
};

int main() {
    cv::Rect viewport(0, 0, 960, 960);
    Ptr<V4D> runtime = V4D::init(viewport, "Display an Image through direct FB access", AllocateFlags::IMGUI, ConfigFlags::DISPLAY_MODE);
    V4DPlan::run<DisplayImageFB>(0, samples::findFile("lena.png"));
    return 0;
}
```

## Code Breakdown

### 1. The `plain` Context

In the `setup()` phase, we prepare the image for display using a **plain context** — a general-purpose context for running standard CPU-side code like OpenCV functions.

### 2. The `fb` Context

The **framebuffer context** provides direct write access to the window's back-buffer. The lambda receives a `UMat` handle to the framebuffer, and we simply copy our prepared image data into it.

### 3. `ConfigFlags::DISPLAY_MODE`

This flag tells V4D that we intend to use it primarily for direct framebuffer rendering, which can enable certain optimizations.

## Summary

- Use a **plain context** for general-purpose image processing.
- Use a **framebuffer context** to copy pixel data directly to the screen.
- Prepare a `UMat` by resizing and color-converting it for display.
- Initialize V4D in `DISPLAY_MODE` for direct framebuffer access.

---

# Tutorial 03 — 2D Vector Graphics with NanoVG

> **Source:** [`03-vector_graphics.markdown`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/03-vector_graphics.markdown) | [Displaying an Image via Framebuffer](#tutorial-02--displaying-an-image-via-framebuffer) | [Next: Combining Vector Graphics and Framebuffer Processing](#tutorial-04--combining-vector-graphics-and-framebuffer-processing)

Plan-V4D provides a powerful 2D vector graphics API through its integration with NanoVG. This tutorial demonstrates how to use the `nvg` context to draw shapes, apply gradients, and create simple animations. We will walk through an example that draws a pair of animated googly eyes.

## The Code

You can find the complete source in [`vector_graphics.cpp`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/samples/vector_graphics.cpp).

```cpp
#include <opencv2/v4d/v4d.hpp>

using namespace cv;
using namespace cv::v4d;

class VectorGraphicsPlan: public V4DPlan {
    Property<cv::Size> sz_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    void infer() override {
        nvg([](const Size &sz) {
            using namespace cv::v4d::nvg;
            clearScreen();

            static long start = cv::getTickCount() / cv::getTickFrequency();
            float t = cv::getTickCount() / cv::getTickFrequency() - start;
            float x = 0;
            float y = 0;
            float w = sz.width / 4;
            float h = sz.height / 4;
            translate((sz.width / 2.0f) - (w / 2.0f), (sz.height / 2.0f) - (h / 2.0f));
            float mx = w / 2.0;
            float my = h / 2.0;
            Paint gloss, bg;
            float ex = w * 0.23f;
            float ey = h * 0.5f;
            float lx = x + ex;
            float ly = y + ey;
            float rx = x + w - ex;
            float ry = y + ey;
            float dx, dy, d;
            float br = (ex < ey ? ex : ey) * 0.5f;
            float blink = 1 - pow(sinf(t * 0.5f), 200) * 0.8f;

            bg = linearGradient(x, y + h * 0.5f, x + w * 0.1f, y + h,
                    cv::Scalar(0, 0, 0, 32), cv::Scalar(0, 0, 0, 16));
            beginPath();
            ellipse(lx + 3.0f, ly + 16.0f, ex, ey);
            ellipse(rx + 3.0f, ry + 16.0f, ex, ey);
            fillPaint(bg);
            fill();

            bg = linearGradient(x, y + h * 0.25f, x + w * 0.1f, y + h,
                    cv::Scalar(220, 220, 220, 255),
                    cv::Scalar(128, 128, 128, 255));
            beginPath();
            ellipse(lx, ly, ex, ey);
            ellipse(rx, ry, ex, ey);
            fillPaint(bg);
            fill();

            dx = (mx - rx) / (ex * 10);
            dy = (my - ry) / (ey * 10);
            d = sqrtf(dx * dx + dy * dy);
            if (d > 1.0f) { dx /= d; dy /= d; }
            dx *= ex * 0.4f;
            dy *= ey * 0.5f;
            beginPath();
            ellipse(lx + dx, ly + dy + ey * 0.25f * (1 - blink), br, br * blink);
            fillColor(cv::Scalar(32, 32, 32, 255));
            fill();

            // ... second pupil and gloss highlights ...
        }, sz_);
    }
};

int main() {
    cv::Rect viewport(0, 0, 960, 960);
    Ptr<V4D> runtime = V4D::init(viewport, "Vector Graphics", AllocateFlags::NANOVG | AllocateFlags::IMGUI);
    V4DPlan::run<VectorGraphicsPlan>(0);
}
```

## Code Breakdown

### 1. Drawing with NanoVG

The drawing process generally follows these steps:

1. **`beginPath()`**: Starts a new shape path.
2. **Define Geometry**: Create a shape using functions like `ellipse()`, `rect()`, `moveTo()`, `lineTo()`, etc.
3. **Set Paint**: Define the color or gradient using `fillColor()`, `linearGradient()`, `radialGradient()`, then apply with `fillPaint()`.
4. **Render**: Call `fill()` or `stroke()` to render the path to the screen.

### 2. Animation

A simple time-based animation is created using a `static` variable `start` that records the initial time. A `blink` factor is computed using a `sin` function sharpened with `pow`, then used to scale the height of the pupil's ellipse.

## Summary

- How to use the `nvg` context to access the NanoVG API.
- The basic workflow of creating and rendering shapes.
- How to use solid colors and gradients.
- A simple technique for creating time-based animations.

---

# Tutorial 04 — Combining Vector Graphics and Framebuffer Processing

> **Source:** [`04-vector_graphics_and_fb.markdown`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/04-vector_graphics_and_fb.markdown) | [2D Vector Graphics with NanoVG](#tutorial-03--2d-vector-graphics-with-nanovg) | [Next: Direct OpenGL Rendering](#tutorial-05--direct-opengl-rendering)

A powerful feature of Plan-V4D is the ability to chain different contexts together. We will draw animated googly eyes using the `nvg` context, then immediately access the resulting image in the framebuffer using the `fb` context and apply a blur effect.

## The Code

You can find the complete source in [`vector_graphics_and_fb.cpp`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/samples/vector_graphics_and_fb.cpp).

```cpp
void infer() override {
    // 1. Draw the vector graphics
    nvg([](const Size& sz) {
        // ... googly eyes drawing logic ...
    }, sz_);

    // 2. Process the result in the framebuffer
    fb([](UMat& framebuffer) {
        boxFilter(framebuffer, framebuffer, -1, Size(15, 15), Point(-1, -1), true, BORDER_REPLICATE);
    });
}
```

## Code Breakdown

### Context Chaining

The magic happens within the `infer()` method. Two contexts are called in sequence:

1. The **`nvg` context** draws the animated googly eyes. When it finishes, the resulting image is in the window's framebuffer.
2. The **`fb` context** picks up the framebuffer exactly where the `nvg` context left off. The pixels drawn by NanoVG are now available as a `UMat`, and we apply a `boxFilter` to blur them. Because we are operating on a `UMat`, this processing can be hardware-accelerated on the GPU.

## Summary

- You can execute different contexts sequentially within the same `infer()` call.
- The output of one graphics context (like `nvg`) can serve as the input for a subsequent processing context (like `fb`).
- This allows you to create powerful pipelines that combine rendering with GPU-accelerated image processing.

---

# Tutorial 05 — Direct OpenGL Rendering

> **Source:** [`05-render_opengl.markdown`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/05-render_opengl.markdown) | [Combining Vector Graphics and Framebuffer Processing](#tutorial-04--combining-vector-graphics-and-framebuffer-processing) | [Next: Font Rendering](#tutorial-06--font-rendering)

While V4D provides high-level contexts like `nvg` and `fb`, it also gives you direct access to the underlying OpenGL API through the `gl` context. This tutorial demonstrates the most basic use: clearing the screen to a solid blue color.

## The Code

You can find the complete source in [`render_opengl.cpp`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/samples/render_opengl.cpp).

```cpp
#include <opencv2/v4d/v4d.hpp>

using namespace cv;
using namespace cv::v4d;

class RenderOpenGLPlan : public V4DPlan {
public:
    void setup() override {
        gl(glClearColor, V(0.0f), V(0.0f), V(1.0f), V(1.0f));
    }

    void infer() override {
        gl(glClear, V(GL_COLOR_BUFFER_BIT));
    }
};

int main() {
    cv::Rect viewport(0, 0, 960, 960);
    Ptr<V4D> runtime = V4D::init(viewport, "GL Blue Screen", AllocateFlags::IMGUI);
    V4DPlan::run<RenderOpenGLPlan>(0);
}
```

## Code Breakdown

### The `gl` Context

Its usage is straightforward:

```cpp
gl(openGLFunction, arg1, arg2, …);
```

The two-argument form `gl<-1>(V(idx), …)` (see [`v4d.hpp:584`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/include/opencv2/v4d/v4d.hpp)) routes the call to one of V4D's worker OpenGL contexts for parallel execution.

### Setting State in `setup()`

```cpp
gl(glClearColor, V(0.0f), V(0.0f), V(1.0f), V(1.0f));
```

- **`V(…)`** is an **edge-call** that passes constant values directly to the function.
- V4D preserves the OpenGL state between context calls, so the clear color remains active for all subsequent `gl` contexts.

### Performing Actions in `infer()`

```cpp
gl(glClear, V(GL_COLOR_BUFFER_BIT));
```

This creates a graph node that calls `glClear` with the `GL_COLOR_BUFFER_BIT` flag.

## Summary

- You can call any standard OpenGL function by passing it to the `gl` context.
- OpenGL state is preserved between `gl` context calls.
- **Edge-calls** like `V()` are used to pass arguments to the OpenGL functions within the graph.
- Use `gl<-1>(V(idx), …)` to target a specific worker OpenGL context for parallel execution.

---

# Tutorial 06 — Font Rendering

> **Source:** [`06-font_rendering.markdown`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/06-font_rendering.markdown) | [Direct OpenGL Rendering](#tutorial-05--direct-opengl-rendering) | [Next: Simple Video Editing](#tutorial-07--simple-video-editing)

Text is a fundamental part of most graphical applications. Plan-V4D makes font rendering easy by leveraging NanoVG. This tutorial shows how to load fonts and draw text to the screen.

## The Code

You can find the complete source in [`font_rendering.cpp`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/samples/font_rendering.cpp).

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
    cv::Ptr<V4D> runtime = V4D::init(viewport, "Font Rendering", AllocateFlags::NANOVG | AllocateFlags::IMGUI);
    V4DPlan::run<FontRenderingPlan>(0);
    return 0;
}
```

## Code Breakdown

- **`R(text_)`**: We use the `R` (Read-only) edge-call since we are only reading the string.
- **`fontSize(40.0f)`**: Sets the size of the font.
- **`fontFace("sans-bold")`**: Selects the font to use. "sans-bold" is a default font provided by NanoVG.
- **`fillColor(…)`**: Sets the color of the text.
- **`textAlign(…)`**: Sets the alignment so the text's top-center point is at the specified coordinates.
- **`text(…)`**: Draws the text to the screen. The four-argument overload `(x, y, begin, end)` draws the half-open character range `[begin, end)`.

### Loading Custom Fonts

You can load your own TrueType (`.ttf`) fonts by calling `createFont` in your `setup()` phase. The TTF files in [`modules/v4d/samples/fonts/`](https://github.com/kallaballa/Plan-V4D/tree/beta/modules/v4d/samples/fonts/) are copied into the build directory at `assets/fonts/` by the `v4d` CMake module.

```cpp
int my_font_;

void setup() override {
    nvg([&]() {
        my_font_ = createFont("my-cool-font", "assets/fonts/Roboto-Regular.ttf");
        CV_Assert(my_font_ >= 0);
    });
}
```

## Summary

- Using the `nvg` context to access font rendering functions.
- Setting font properties like size, face, color, and alignment.
- Drawing text to the screen.
- The basic process for loading and using custom fonts.

---

# Tutorial 07 — Simple Video Editing

> **Source:** [`07-video_editing.markdown`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/07-video_editing.markdown) | [Font Rendering](#tutorial-06--font-rendering) | [Next: Custom Source and Sink](#tutorial-08--custom-source-and-sink)

Plan-V4D provides a simple yet powerful source/sink architecture for building video processing pipelines. This tutorial demonstrates how to build a simple video editor that reads from a video file, renders text on top of each frame, and saves the result to a new video file.

## The Code

You can find the complete source in [`video_editing.cpp`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/samples/video_editing.cpp).

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
        capture();

        nvg([](const Size& sz, const string& str) {
            using namespace cv::v4d::nvg;
            fontSize(40.0f);
            fontFace("sans-bold");
            fillColor(Scalar(255, 0, 0, 255));
            textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            text(sz.width / 2.0, sz.height / 2.0, str.c_str(), str.c_str() + str.size());
        }, sz_, R(hv_));

        write();
    }
};

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: video_editing <input-video-file> <output-video-file>" << std::endl;
        exit(1);
    }
    cv::Rect viewport(0, 0, 960, 960);
    Ptr<V4D> runtime = V4D::init(viewport, "Video Editing", AllocateFlags::NANOVG | AllocateFlags::IMGUI);

    auto src = Source::make(runtime, argv[1]);
    auto sink = Sink::make(runtime, argv[2], src->fps(), viewport.size());

    runtime->setSource(src);
    runtime->setSink(sink);

    V4DPlan::run<VideoEditingPlan>(0);
}
```

## Code Breakdown

### Setting up the Pipeline

- **`Source::make(…)`**: Creates a `Source` object from an input video file. V4D handles video decoding.
- **`Sink::make(…)`**: Creates a `Sink` object for the output file, with FPS from the source and the viewport size. V4D handles video encoding.
- **`runtime->setSource(src)`** and **`runtime->setSink(sink)`**: Attaches them to the runtime.

### The `infer()` Method

The three-step process:

1. **`capture()`**: Decodes one frame from the input video and places it into the main framebuffer.
2. **`nvg(…)`**: Renders text *on top of* the video frame.
3. **`write()`**: Takes the composited framebuffer and sends it to the video encoder.

## Summary

- **`Source`** and **`Sink`** objects handle video decoding and encoding.
- **`capture()`** reads a frame from the source into the framebuffer.
- **`write()`** writes the framebuffer's content to the sink.
- By sequencing `capture()`, rendering contexts, and `write()`, you can create elegant video processing pipelines.

---

# Tutorial 08 — Custom Source and Sink

> **Source:** [`08-custom_source_and_sink.markdown`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/08-custom_source_and_sink.markdown) | [Simple Video Editing](#tutorial-07--simple-video-editing) | [Next: Font Rendering with a GUI](#tutorial-09--font-rendering-with-a-gui)

While Plan-V4D provides convenient `Source` and `Sink` objects for file I/O, you can also create your own from scratch. This tutorial demonstrates creating a procedural rainbow source, a pass-through sink, and conditional branching within a `V4DPlan`.

## The Code

You can find the complete source in [`custom_source_and_sink.cpp`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/samples/custom_source_and_sink.cpp).

```cpp
cv::Ptr<Source> src = new Source([](cv::UMat& frame){
    if(frame.empty()) {
        frame.create(Size(960, 960), CV_8UC3);
    }
    uchar hue = (int64_t(seconds() * 15) % 255);
    frame = convert_pix<cv::COLOR_HLS2RGB_FULL>(cv::Vec3b(hue, 128, 255));
    return true;
}, 60.f);

cv::Ptr<Sink> sink = new Sink([videoSink](const uint64_t& seq, const cv::UMat& frame){
    videoSink->operator()(seq, frame);
    return videoSink->isOpen();
});
```

### Conditional Branching

```cpp
void infer() override {
    capture();
    fb<1>(&PureColor::find, RW(finder_));
    nvg(&PureColor::draw, R(finder_), size_);

    std::dynamic_pointer_cast<V4DPlan>(
        branch(&PureColor::found, R(finder_))
    )->write()->endBranch();
}
```

- **`branch(&PureColor::found, R(finder_))`**: The nodes inside the branch only execute if `found()` returns `true`.
- **`std::dynamic_pointer_cast<V4DPlan>(…)`**: Required because `Plan::branch` returns `cv::Ptr<Plan>`, but `write()` only exists on `V4DPlan`.

## Summary

- Creating a procedural video **`Source`** from a lambda function.
- Creating a custom **`Sink`** from a lambda to define custom output behavior.
- Using **`branch()`** to create conditional logic within a `V4DPlan`'s task graph.

---

# Tutorial 09 — Font Rendering with a GUI

> **Source:** [`09-font_with_gui.markdown`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/09-font_with_gui.markdown) | [Custom Source and Sink](#tutorial-08--custom-source-and-sink) | [Next: Rendering a 3D Cube](#tutorial-10--rendering-a-3d-cube)

This tutorial demonstrates how to add a GUI to our font rendering example, allowing the user to change the font size and color in real-time using [Dear ImGui](https://github.com/ocornut/imgui).

## The Code

You can find the complete source in [`font_with_gui.cpp`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/samples/font_with_gui.cpp).

```cpp
#include <opencv2/v4d/v4d.hpp>

using namespace cv;
using namespace cv::v4d;

class FontWithGuiPlan: public V4DPlan {
    static struct Params {
        float fontSize_ = 40.0f;
        cv::Scalar_<float> color_ = {1.0f, 0.0f, 0.0f, 1.0f};
    } params_;

    string hw_ = "hello world";
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    void gui() override {
        imgui([](Params& params) {
            using namespace ImGui;
            Begin("Settings");
            SliderFloat("Font Size", &params.fontSize_, 1.0f, 100.0f);
            ColorPicker4("Text Color", params.color_.val);
            End();
        }, RWS(params_));
    }

    void infer() override {
        nvg([](const cv::Size& sz, const string& str, const Params& params) {
            using namespace cv::v4d::nvg;
            clearScreen();
            fontSize(params.fontSize_);
            fontFace("sans-bold");
            fillColor(params.color_ * 255.0);
            textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            text(sz.width / 2.0, sz.height / 2.0, str.c_str(), str.c_str() + str.size());
        }, size_, R(hw_), CS(params_));
    }
};

FontWithGuiPlan::Params FontWithGuiPlan::params_;
```

## Code Breakdown

### The `gui()` Method

Called every frame on the display thread. The **`imgui` context** provides an environment where you can call standard ImGui functions.

- **`RWS(params_)`**: Because `params_` is shared between threads, we use the **R**ead-**W**rite **S**hared edge.

### Shared Parameters

A `static struct` ensures a single instance shared between the GUI and rendering logic.

### The `infer()` Method

- **`CS(params_)`**: The **C**opy **S**hared edge-call provides a consistent snapshot of parameters for the duration of the frame, preventing visual artifacts.

## Summary

- The **`gui()`** method defines the UI; it runs on the display thread once per frame.
- The **`imgui`** context allows standard ImGui functions.
- The **`RWS`** edge is used when a context mutates a shared resource.
- The **`CS`** edge safely reads shared data in the rendering context.

---

# Tutorial 10 — Rendering a 3D Cube

> **Source:** [`10-cube.markdown`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/10-cube.markdown) | [Font Rendering with a GUI](#tutorial-09--font-rendering-with-a-gui) | [Next: Compositing 3D Graphics on Video](#tutorial-11--compositing-3d-graphics-on-video)

This tutorial demonstrates how to structure a more complex rendering application by separating the low-level OpenGL logic from the main application `V4DPlan`, and introduces the `teardown` phase.

## The Code

This example is split into two files:

1. [`cubescene.hpp`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/samples/cubescene.hpp): A `CubeScene` class encapsulating all low-level OpenGL code.
2. [`cube-demo.cpp`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/samples/cube-demo.cpp): The `CubeDemoPlan` and `main` function.

### `cubescene.hpp` (Abridged)

```cpp
class CubeScene {
public:
    void init() {
        // Standard OpenGL setup: glGenVertexArrays, glGenBuffers, glBufferData, etc.
    }
    void render(const double xpos = 0.0, const double ypos = 0.0) const {
        // Calculates rotation matrices, sets uniform, draws cube.
    }
    void destroy() const {
        // Calls glDeleteProgram, glDeleteBuffers, glDeleteVertexArrays.
    }
};
```

### `cube-demo.cpp`

```cpp
#include <opencv2/v4d/v4d.hpp>
#include "cubescene.hpp"

using namespace cv::v4d;

class CubeDemoPlan : public V4DPlan {
    CubeScene scene_;
public:
    void setup() override {
        gl(&CubeScene::init, RW(scene_));
    }
    void infer() override {
        set(V4D::Keys::CLEAR_COLOR, V(cv::Scalar(102, 61, 51, 255)));
        clear();
        gl(&CubeScene::render, R(scene_), V(0.0), V(0.0));
    }
    void teardown() override {
        gl(&CubeScene::destroy, R(scene_));
    }
};

int main() {
    cv::Rect viewport(0, 0, 1920, 1080);
    cv::Ptr<V4D> runtime = V4D::init(viewport, "Cube Demo", AllocateFlags::IMGUI);
    V4DPlan::run<CubeDemoPlan>(2);
    return 0;
}
```

## Code Breakdown

### Separation of Concerns

`CubeDemoPlan` does not need to know *how* the cube is drawn; it only calls `init`, `render`, and `destroy` at the appropriate times. `CubeScene` is a self-contained, reusable component.

### The `V4DPlan` Lifecycle

- **`setup()`**: Called once at the beginning. Initializes OpenGL resources.
- **`infer()`**: Called repeatedly. Clears the screen and renders a frame.
- **`teardown()`**: Called once at the end. Cleans up OpenGL resources.

### High-Level V4D Functions

```cpp
set(V4D::Keys::CLEAR_COLOR, V(cv::Scalar(102, 61, 51, 255)));
clear();
```

- **`set(…)`**: Sets an internal V4D property.
- **`clear()`**: Clears the screen using the set color — a higher-level alternative to `glClearColor`/`glClear`.

## Summary

- Encapsulate complex rendering logic in a separate helper class.
- Use `setup()`, `infer()`, and `teardown()` to manage the lifecycle of rendering objects.
- V4D provides high-level functions like `set` and `clear` for common operations.

---

# Tutorial 11 — Compositing 3D Graphics on Video

> **Source:** [`11-video.markdown`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/11-video.markdown) | [Rendering a 3D Cube](#tutorial-10--rendering-a-3d-cube) | [Next: Advanced NanoVG and Processing Pipelines](#tutorial-12--advanced-nanovg-and-processing-pipelines)

This tutorial demonstrates compositing: reading a video, rendering a rotating 3D cube on top of it, and saving the result.

## The Code

You can find the complete source in [`video-demo.cpp`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/samples/video-demo.cpp).

```cpp
void infer() override {
    // 1. Load video frame into the framebuffer.
    capture();
    // 2. Render the 3D cube on top of the video frame.
    gl(&CubeScene::render, R(scene_), V(0.0), V(0.0));
    // 3. Write the composited frame to the sink.
    write();
}
```

## Code Breakdown

- **`capture()`**: Reads a frame from the source video into the framebuffer — this becomes the background.
- **`gl(&CubeScene::render, …)`**: Renders the 3D cube. Crucially, we did *not* clear the screen beforehand, so the cube is drawn directly on top of the video frame.
- **`write()`**: Takes the composited framebuffer and sends it to the video encoder.

The sequential execution of contexts is the foundation of building complex effects and pipelines in Plan-V4D.

## Summary

- The order of operations in `infer()` defines the rendering pipeline.
- `capture()` loads a video frame as a background.
- Subsequent rendering calls draw on top of the existing framebuffer content, allowing easy compositing.

---

# Tutorial 12 — Advanced NanoVG and Processing Pipelines

> **Source:** [`12-nanovg.markdown`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/12-nanovg.markdown) | [Compositing 3D Graphics on Video](#tutorial-11--compositing-3d-graphics-on-video) | [Next: Interactive Custom Shaders](#tutorial-13--interactive-custom-shaders)

This tutorial showcases a complex, multi-stage image processing pipeline: reading video, applying a real-time color-shifting effect, and overlaying a custom NanoVG widget.

## The Code

You can find the complete source in [`nanovg-demo.cpp`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/samples/nanovg-demo.cpp).

```cpp
// Abridged for clarity
class NanoVGDemoPlan : public V4DPlan {
    std::vector<cv::UMat> hsvChannels_;
    cv::UMat frame_, bgra_, hsv_;
    float hue_ = 0;
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);

    constexpr static auto SPLIT_ = _OL_(void, cv::split, cv::InputArray, cv::OutputArrayOfArrays);
    constexpr static auto MERGE_ = _OL_(void, cv::merge, cv::InputArrayOfArrays, cv::OutputArray);
    constexpr static auto RESIZE_VEC_ = _OLM_(void, std::vector<cv::UMat>, &std::vector<cv::UMat>::resize, size_t);

public:
    void setup() override {
        plain(RESIZE_VEC_, RW(hsvChannels_), V(size_t(3)));
    }

    void infer() override {
        capture(RW(bgra_));
        assign(RW(hue_), (F(&sinf, (F(&cv::getTickCount) / F(&cv::getTickFrequency)) * V(0.12) + V(1))) * V(255.0));

        plain(cv::cvtColor, R(bgra_), RW(frame_), V(cv::COLOR_BGRA2RGB), V(0), V(cv::ALGO_HINT_DEFAULT))
        ->plain(cv::cvtColor, R(frame_), RW(hsv_), V(cv::COLOR_RGB2HSV_FULL), V(0), V(cv::ALGO_HINT_DEFAULT))
        ->plain(SPLIT_, R(hsv_), RW(hsvChannels_))
        ->plain(&cv::UMat::setTo, RW(hsvChannels_[0]), F(&fmod, F(&fabs, R(hue_) - V(255)) - V(81), V(255)), V(cv::noArray()))
        ->plain(MERGE_, R(hsvChannels_), RW(hsv_))
        ->plain(cv::cvtColor, R(hsv_), RW(frame_), V(cv::COLOR_HSV2RGB_FULL), V(0), V(cv::ALGO_HINT_DEFAULT))
        ->plain(&std::vector<cv::UMat>::clear, RW(hsvChannels_));

        fb<1>(cv::cvtColor, R(frame_), V(cv::COLOR_RGB2BGRA), V(0), V(cv::ALGO_HINT_DEFAULT));
        nvg(draw_color_wheel, size_, R(hue_));
    }
};
```

## Code Breakdown

### The Hue-Shifting Chain

The core is a chain of `plain` contexts:

1. **RGB to HSV** conversion
2. **Split** into H, S, V channels
3. **Modify Hue** channel using `setTo` with an animated value
4. **Merge** channels back
5. **HSV to RGB** conversion

### Function Wrappers

- **`assign(…)`**: Graph-level primitive for variable assignment.
- **`F(…)`**: Wraps standard functions (`sinf`, `getTickCount`) as graph nodes.
- **`_OL_`, `_OLM_`**: Pre-wrap OpenCV functions for dispatch.

## Summary

- Complex pipelines can be constructed by chaining `plain` contexts.
- Function wrappers (`F()`, `_OL_`, `_OLM_`) integrate almost any standard function into the pipeline.
- You can composite processing results with UI elements drawn using NanoVG.

---

# Tutorial 13 — Interactive Custom Shaders

> **Source:** [`13-shader.markdown`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/13-shader.markdown) | [Advanced NanoVG and Processing Pipelines](#tutorial-12--advanced-nanovg-and-processing-pipelines) | [Next: Advanced Font Effects Demo](#tutorial-14--advanced-font-effects-demo)

This tutorial showcases a complete, interactive application: a Mandelbrot fractal explorer that renders the fractal using a custom GLSL shader, composites it over a video background, and allows full user control via a GUI and mouse interaction.

## The Code

You can find the complete source in [`shader-demo.cpp`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/samples/shader-demo.cpp).

```cpp
// Abridged for clarity
class ShaderDemoPlan : public V4DPlan {
    static struct Params {
        Camera2D camera_;
        MandelbrotScene::Settings settings_;
    } params_;
    MandelbrotScene scene_;

    Event<Mouse> release_ = E<Mouse>(M::RELEASE);
    Event<Mouse> scroll_  = E<Mouse>(M::SCROLL);

    static bool process_events(const cv::Size& sz, const cv::Size& winSz,
                               const Mouse::List& scrollEvents,
                               const Mouse::List& releaseEvents,
                               const double& scale, Params& params) {
        // ... updates camera zoom/pan on scroll/click ...
        return params.settings_.autoZoom_;
    }

public:
    void gui() override {
        imgui([](Params& params) {
            // ... ImGui widgets ...
        }, RWS(params_));
    }

    void setup() override {
        branch(BranchType::ONCE, always_)
            ->assign(RWS(params_.camera_), V(Camera2D(autoZoomSeconds_)))
        ->endBranch();
        gl(&MandelbrotScene::init, RW(scene_));
    }

    void infer() override {
        capture();
        branch(process_events, size_, winSz_, scroll_, release_, R(scale_), RWS(params_))
            ->plain(&Camera2D::updateAutoZoom, RWS(params_.camera_), R(params_.settings_.maxIterations_))
        ->endBranch();
        gl(&MandelbrotScene::render, R(scene_), size_, CS(params_.settings_), CS(params_.camera_));
        write();
    }

    void teardown() override {
        gl(&MandelbrotScene::destroy, R(scene_));
    }
};
```

## Code Breakdown

### Event Handling

- **`E<Mouse>(…)`**: Creates an `Event` edge for capturing mouse input.
- **`process_events(…)`**: Used as a `branch` condition. Updates camera on scroll/click and disables auto-zoom when the user interacts.

### The `infer()` Pipeline

1. **`capture()`**: Loads a video frame as background.
2. **`branch(process_events, …)`**: Runs auto-zoom only when the user isn't interacting.
3. **`gl(&MandelbrotScene::render, …)`**: Renders the fractal with current state.
4. **`write()`**: Writes the composited frame.

### One-Shot Initialization

`BranchType::ONCE` emits a one-shot sub-graph in `setup()` for first-frame initialization.

## Summary

- **Event Properties** (`E<T>`) capture user input.
- A `branch` with a function condition creates stateful control flow.
- The `gui()`, event handlers, and `infer()` methods work together on shared state.

---

# Tutorial 14 — Advanced Font Effects Demo

> **Source:** [`14-font.markdown`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/14-font.markdown) | [Interactive Custom Shaders](#tutorial-13--interactive-custom-shaders) | [Next: Pedestrian Detection and Tracking Demo](#tutorial-15--pedestrian-detection-and-tracking-demo)

This tutorial breaks down a complex, "Star Wars"-style opening crawl effect. It demonstrates how to structure a high-performance application by separating logic into reusable components and using conditional execution to avoid unnecessary work.

## The Code

You can find the complete source in [`font-demo.cpp`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/samples/font-demo.cpp).

```cpp
// Abridged for clarity.
struct TextRenderer { /* renders scrolling text into a UMat */ };
struct StarsRenderer { /* renders a starfield into a UMat */ };
struct Warp { /* warps the text rendering and composites it with the stars */ };

class FontDemoPlan : public V4DPlan {
    static TextRenderer text_;
    static StarsRenderer stars_;
    static Warp warp_;
    static double timeOffset_;

    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
    Property<cv::Rect> vp_ = P<cv::Rect>(V4D::Keys::VIEWPORT);
    cv::Size lastVpSize_;

public:
    void gui() override {
        // ... ImGui controls ...
    }

    void setup() override {
        branch(BranchType::ONCE, always_)
                ->assign(RW(timeOffset_), F(seconds))
                ->construct(RW(text_), F(hypot,
                                         F(&cv::Size::width,  size_),
                                         F(&cv::Size::height, size_)
                                     ) / V(60.0))
        ->endBranch();
    }

    void infer() override {
        branch(CS(warp_.update_) || RW(lastVpSize_) != F(&cv::Rect::size, vp_))
            ->plain(&Warp::calculate, RWS(warp_), size_)
        ->endBranch();

        branch(CS(stars_.update_) || RW(lastVpSize_) != F(&cv::Rect::size, vp_))
            ->nvg(&StarsRenderer::draw, RWS(stars_), size_)
            ->fb(UMAT_COPY_, RWS(stars_.rendering_))
        ->endBranch();

        nvg(&TextRenderer::draw, RWS(text_), size_, CS(timeOffset_));
        fb(UMAT_COPY_, RWS(text_.rendering_));

        clear();
        fb<3>(&Warp::perform, RWS(warp_), RS(text_.rendering_), RS(stars_.rendering_));

        branch(-CS(text_.textOffsetY_) > CS(text_.height_))
            ->assign(RW(timeOffset_), F(seconds))
        ->endBranch();

        assign(RW(lastVpSize_), F(&cv::Rect::size, vp_));
    }
};
```

## Code Breakdown

### Logic Separation and Render-to-Texture

Each major visual component renders its output to a `UMat` member variable, not directly to the screen. This gives us two intermediate renderings — stars and text — that we can use as inputs for a final compositing step.

### Conditional Execution for Performance

The starfield and warp would be expensive to redraw every frame. The `V4DPlan` uses `branch` conditions that check `update_` flags (set by the GUI when a slider moves) and viewport-size comparisons, so expensive work only runs when parameters actually change. The scrolling text, always moving, is drawn every frame.

### Final Compositing

```cpp
fb<3>(&Warp::perform, RWS(warp_), RS(text_.rendering_), RS(stars_.rendering_));
```

`Warp::perform` uses standard OpenCV functions (`warpPerspective`, `add`, etc.) to apply the 3D effect and write the final result to the display framebuffer.

## Summary

- **Code Organization**: Breaking complex effects into logical, reusable classes.
- **Render-to-Texture**: Rendering into intermediate `UMat`s enables complex post-processing.
- **Stateful Optimization**: `branch` flags skip unnecessary work on static frames.
- **Viewport Tracking**: The `lastVpSize_` trick re-runs branches on window resize.

---

# Tutorial 15 — Pedestrian Detection and Tracking Demo

> **Source:** [`15-pedestrian.markdown`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/15-pedestrian.markdown) | [Advanced Font Effects Demo](#tutorial-14--advanced-font-effects-demo) | [Next: Sparse Optical Flow Demo](#tutorial-16--sparse-optical-flow-demo)

This tutorial implements a classic "detect-then-track" strategy for finding pedestrians: an efficient pattern for real-time object tracking using HOG detection and KCF tracking.

## The Code

You can find the complete source in [`pedestrian-demo.cpp`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/samples/pedestrian-demo.cpp).

```cpp
void infer() override {
    capture(RW(frames_.videoFrame_));

    plain(cv::cvtColor, R(frames_.videoFrame_), RW(frames_.videoFrameBGR_), V(cv::COLOR_BGRA2RGB), V(0), V(cv::ALGO_HINT_DEFAULT))
    ->plain(prepare_frames, R(params_), RW(frames_));

    branch(doRedect_, R(detection_))
        ->plain(&HOG::detect, R(hog), R(frames_.videoFrameDownGrey_), RW(detection_), RW(nms), RW(params_))
    ->elseBranch()
        ->plain(&Tracking::perform, R(tracking), R(frames_.videoFrameDownGrey_), RW(detection_), RW(params_), CS(tracked_))
    ->endBranch();

    plain(&Tracking::save, R(tracking), R(params_), size_, RWS(tracked_))
    ->nvg(&ObjectMarker::draw, R(marker_), size_, R(params_), CS(tracked_))
    ->fb(present, R(frames_.background_));

    write();
}
```

## Code Breakdown

### The "Detect-then-Track" Strategy

1. **Detect**: Run the expensive HOG detector once to find the object.
2. **Track**: Initialize a lightweight KCF tracker with the location.
3. **Update**: Run the fast tracker on subsequent frames.
4. **Re-detect**: If the tracker loses the object, run the full detector again.

### Implementing with `branch`/`elseBranch`

The `Detection` struct holds `trackerInit_` and `redetect_` flags. The branch condition `doRedect_` returns `true` when detection is needed; otherwise the `elseBranch` runs the fast tracker — a perfect fit for state-based control flow.

### Visualization

A final chain composites the visualization: `Tracking::save` (smoothing), `ObjectMarker::draw` (nvg ellipse), and `present` (fb compositing) with the original video frame.

## Summary

- Complex logic can be organized into helper classes.
- `branch`/`elseBranch` is perfect for state-based control flow.
- Standard OpenCV algorithms (`HOGDescriptor`, `TrackerKCF`) integrate seamlessly.

---

# Tutorial 16 — Sparse Optical Flow Demo

> **Source:** [`16-optflow.markdown`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/16-optflow.markdown) | [Pedestrian Detection and Tracking Demo](#tutorial-15--pedestrian-detection-and-tracking-demo) | [Next: Real-Time "Beauty Filter" Demo](#tutorial-17--real-time-beauty-filter-demo)

This tutorial explores a stylized representation of sparse optical flow in a video — a masterclass in structuring a complex real-time application.

## The Key Helper Classes

- **`FeaturePoints`**: Detects keypoints using `cv::FastFeatureDetector`.
- **`SceneChange`**: Determines if a drastic scene change has occurred.
- **`SparseOpticalFlow`**: Calculates flow between frames and renders vectors with NanoVG.
- **`BackgroundStyle`**: Applies visual styles to the background video.
- **`PostProcessor`**: Post-processing effects like glow and bloom.
- **`Compositor`**: Combines background, foreground, and post-processing.

## A Multi-Layer Pipeline

```cpp
void infer() override {
    capture(RW(frames_.background_));
    // ... convert to grey, detect features ...

    branch(BranchType::SINGLE,
        !F(&SceneChange::detect, RW(sceneChange_),
             RS(detectedPoints_),
             CS(params_.sceneChangeThresh_),
             CS(params_.sceneChangeThreshDiff_),
             size_))
        ->clear()
        ->nvg(&SparseOpticalFlow::visualize, RW(sparseOptflow_), ...)
        ->fb(UMAT_COPY_TO_, RW(frames_.foreground_))
    ->endBranch();

    fb<4>(&Compositor::perform, RW(compositor_), ...);
    write(R(frames_.composed_));
}
```

The final image is built in distinct layers:

- `frames_.background_`: The raw video frame.
- `frames_.foreground_`: The optical-flow visualization.
- `frames_.oldForeground_`: Previous frame's foreground for motion blur / trails.
- `frames_.composed_`: The final output.

The `branch` with `BranchType::SINGLE` skips the expensive visualization when a scene change is detected, preventing artifacts.

## Summary

- **Modularity**: Many small, single-responsibility classes.
- **Layer-Based Compositing**: Intermediate `UMat`s combined in a final stage.
- **Performance**: `branch` skips work on scene change.

---

# Tutorial 17 — Real-Time "Beauty Filter" Demo

> **Source:** [`17-beauty.markdown`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/17-beauty.markdown) | [Sparse Optical Flow Demo](#tutorial-16--sparse-optical-flow-demo) | [Next: Parallel Rendering with Multiple OpenGL Contexts](#tutorial-18--parallel-rendering-with-multiple-opengl-contexts)

This is the most architecturally complex demo: a real-time "beauty filter" that detects facial landmarks and applies different image adjustments to the skin, eyes, and lips, then blends them back together. It introduces **sub-plans**.

## The Code

You can find the complete source in [`beauty-demo.cpp`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/samples/beauty-demo.cpp).

```cpp
// Main plan that orchestrates everything.
class BeautyDemoPlan : public V4DPlan {
    cv::Ptr<FaceFeatureMasksPlan> prepareFeatureMasksPlan_;
    cv::Ptr<BeautyFilterPlan> beautyFilterPlan_;
public:
    BeautyDemoPlan() {
        prepareFeatureMasksPlan_ = _sub<FaceFeatureMasksPlan>(this, features_, frames_);
        beautyFilterPlan_       = _sub<BeautyFilterPlan>      (this, params_,   frames_);
    }

    void infer() override {
        capture(RW(frames_.orig_));
        plain(prepare_frames, R(downSize_), RW(frames_));

        branch(RWS(params_.enabled_) = IF(
                                          F(&Mouse::List::empty, pressEvents_),
                                          CS(params_.enabled_),
                                          !CS(params_.enabled_)))
            ->branch(seqCnt_ % V(uint64_t(8)) == V(uint64_t(0)))
                ->branch(!F(&FaceFeatureExtractor::extract, RW(extractor_), R(frames_.down_), RWS(features_)))
                    ->assign(RWS(params_.state_), V(Params::NOT_DETECTED))
                    ->plain(compose_result, RW(frames_), CS(params_))
                ->endBranch()
            ->endBranch()
            ->branch(!(F(&FaceFeatures::empty, RS(features_))))
                assign(RWS(params_.state_), V(Params::ON))
                ->subInfer(prepareFeatureMasksPlan_)
                ->subInfer(beautyFilterPlan_)
                ->plain(compose_result, RW(frames_), CS(params_))
            ->endBranch()
        ->elseBranch()
            ->plain(compose_result, RW(frames_), CS(params_))
            ->assign(RWS(params_.state_), V(Params::OFF))
        ->endBranch();

        fb<1>(cv::cvtColor, R(frames_.result_), V(cv::COLOR_BGR2RGBA), V(0), V(cv::ALGO_HINT_DEFAULT));
        write(R(frames_.result_));
    }
};
```

## Code Breakdown

### Sub-Plans: Hierarchical Task Graphs

- **`_sub<T>(…)`**: Creates a sub-plan instance in the constructor (see `plan.hpp:979`).
- **`subInfer(…)`**: Executes the sub-plan's entire `infer()` graph as if it were a single node in the main graph (see [`v4d.hpp:531`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/include/opencv2/v4d/v4d.hpp)).

### The Pipeline Data Flow

1. **`BeautyDemoPlan` (Detection)**: Captures a frame, runs a DNN-based `FaceFeatureExtractor` every 8 frames.
2. **`FaceFeatureMasksPlan` (Masking)**: Renders black-and-white masks from the landmarks into `UMat`s (render-to-texture).
3. **`BeautyFilterPlan` (Filtering & Blending)**: Applies per-region adjustments and uses `cv::detail::MultiBandBlender` for seamless blending.
4. **`BeautyDemoPlan` (Compositing)**: Prepares the final image for display.

## Summary

- **Sub-plans** break a massive problem into a hierarchy of smaller, reusable task graphs.
- Data is passed between plans using shared objects and `UMat`s.
- This architecture separates high-level logic from low-level implementation details.

---

# Tutorial 18 — Parallel Rendering with Multiple OpenGL Contexts

> **Source:** [`18-many-cubes.markdown`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/18-many-cubes.markdown) | [Real-Time "Beauty Filter" Demo](#tutorial-17--real-time-beauty-filter-demo) | [Next: An Interactive Image Carousel](#tutorial-19--an-interactive-image-carousel)

This tutorial demonstrates one of Plan-V4D's most powerful high-performance features: the ability to use multiple OpenGL contexts in parallel. We render ten cubes at once, each in its own parallel context.

## The Code

You can find the complete source in [`many_cubes-demo.cpp`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/samples/many_cubes-demo.cpp).

```cpp
#include <opencv2/v4d/v4d.hpp>
#include "cubescene.hpp"

using namespace cv::v4d;

class ManyCubesDemoPlan : public V4DPlan {
    constexpr static size_t NUMBER_OF_CONTEXTS_ = 10;
    CubeScene scene_;
public:
    void setup() override {
        for(size_t i = 0; i < NUMBER_OF_CONTEXTS_; ++i) {
            gl<-1>(V(i), &CubeScene::init, RW(scene_));
        }
    }

    void infer() override {
        set(V4D::Keys::CLEAR_COLOR, V(cv::Scalar(102, 61, 51, 255)));
        clear();
        for(size_t i = 0; i < NUMBER_OF_CONTEXTS_; ++i) {
            gl<-1>(V(i),
                &CubeScene::render, R(scene_),
                    V(sin((double(i) / NUMBER_OF_CONTEXTS_) * 2.0 * CV_PI) / 1.5),
                    V(cos((double(i) / NUMBER_OF_CONTEXTS_) * 2.0 * CV_PI) / 1.5));
        }
    }

    void teardown() override {
        for(size_t i = 0; i < NUMBER_OF_CONTEXTS_; ++i) {
            gl<-1>(V(i), &CubeScene::destroy, R(scene_));
        }
    }
};

int main() {
    cv::Rect viewport(0, 0, 1920, 1080);
    cv::Ptr<V4D> runtime = V4D::init(viewport, "Many Cubes Demo", AllocateFlags::IMGUI);
    V4DPlan::run<ManyCubesDemoPlan>(2);
    return 0;
}
```

## Code Breakdown

### The Multi-Context `gl` Call

`gl<-1>(V(i), …)` tells V4D to use one of its worker OpenGL contexts (see [`v4d.hpp:584`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/include/opencv2/v4d/v4d.hpp)). `V(i)` specifies the index of the worker context — each cube is a separate graph node targeting an independent context.

### Parallel Execution

`setup()`, `infer()`, and `teardown()` all loop over context indices, creating nodes that can execute in parallel on multiple CPU cores.

### Shared Resources

All ten contexts operate on the *same* `CubeScene` object. The `RW(scene_)` (in setup) and `R(scene_)` (in infer/teardown) edge-calls let the Plan engine manage thread-safe access across parallel contexts.

## Summary

- `gl<-1>(V(index), …)` targets specific worker OpenGL contexts.
- Graph nodes in different contexts execute OpenGL operations in parallel.
- The Plan engine manages shared resources across these contexts.

---

# Tutorial 19 — An Interactive Image Carousel

> **Source:** [`19-image_carousel.markdown`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/19-image_carousel.markdown) | [Parallel Rendering with Multiple OpenGL Contexts](#tutorial-18--parallel-rendering-with-multiple-opengl-contexts) | [Next: Reimplementing OpenCV's imshow](#tutorial-20--reimplementing-opencvs-imshow)

This tutorial builds a self-contained image gallery application: it loads images from files and directories, and displays them as an animated carousel with glossy cards, reflections, smooth transitions, and a minimal ImGui HUD.

## The Code

You can find the complete source in [`image_carousel.cpp`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/samples/image_carousel.cpp). Run it with one or more image files or directories. Controls: arrow keys for prev/next, Space to toggle auto-play, mouse scroll/clicks, Home/End.

```cpp
class ImageCarousel : public V4DPlan {
    struct Card {
        std::string path_;
        cv::UMat    rgba_;
        int         w_ = 0, h_ = 0;
        int         handle_ = -1;
        std::string name_;
    };
    std::vector<Card> cards_;
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);

    Event<Keyboard> pressKey_ = E<Keyboard>(Keyboard::PRESS);
    Event<Mouse>    scroll_   = E<Mouse>(Mouse::SCROLL);
    Event<Mouse>    click_    = E<Mouse>(Mouse::PRESS, Mouse::LEFT);

public:
    ImageCarousel() {
        _shared(state_);
    }

    void setup() override {
        set(GlobalState::Keys::TIME_TRACKER, V(false));
        set(GlobalState::Keys::SHOW_FRAME_TIME, V(false));

        nvg([](std::vector<Card>& cards) {
            using namespace cv::v4d::nvg;
            for (auto& c : cards) {
                c.handle_ = createImageRGBA(c.w_, c.h_, NVG_IMAGE_NEAREST,
                                            c.rgba_.getMat(cv::ACCESS_READ).data);
                CV_Assert(c.handle_ > 0);
            }
        }, RW(cards_));
    }

    void infer() override {
        set(V4D::Keys::CLEAR_COLOR, V(cv::Scalar(14, 14, 20, 255)));
        clear();

        nvg([](const std::vector<Card>& cards, const cv::Size& sz,
               const Keyboard::List& pressKeyEvts,
               const Mouse::List& scrollEvts,
               const Mouse::List& clickEvts,
               CarouselState& st) {
            // input, auto-play, smooth animation, card rendering ...
        }, R(cards_), size_, pressKey_, scroll_, click_, RWS(state_));
    }

    void gui() override {
        imgui([](const std::vector<Card>& cards, CarouselState& st, const cv::Size& sz) {
            // Nav buttons, slider, auto-play checkbox ...
        }, R(cards_), RWS(state_), size_);
    }

    void teardown() override {
        nvg([](std::vector<Card>& cards) {
            using namespace cv::v4d::nvg;
            for (auto& c : cards) {
                if (c.handle_ > 0) { deleteImage(c.handle_); c.handle_ = -1; }
            }
        }, RW(cards_));
    }
};
```

## Code Breakdown

### Preparing Pixels for NanoVG

NanoVG images are 8-bit RGBA textures, so `loadFile` normalizes whatever `imread` returns, converting 1/3/4-channel images to RGBA. The four-channel→RGBA case gives true alpha support for PNGs.

### Uploading Images in `setup()`

Each card's RGBA `UMat` becomes a NanoVG texture handle via `createImageRGBA`, using `getMat(cv::ACCESS_READ)` to hand NanoVG a raw pixel pointer.

### Shared State Across Threads

`CarouselState` is a static member registered with `_shared(state_)` and accessed with `RWS` from both the `nvg` and `imgui` nodes. With `ConfigFlags::DISPLAY_MODE`, the worker and GUI threads are serialized.

### Smooth Animation

The demo keeps a fractional `animOffset_` and eases it toward the target index using frame-rate-independent exponential interpolation, wrapping by `±N/2` so the carousel always spins the short way.

### Card Drawing

Each card is drawn with stacked layers: reflection (scissored, flipped), shadow, image fill, gloss gradient, border, and filename label. Cards are sorted back-to-front by depth for a pseudo-3D "parade".

## Summary

- Arbitrary images can be loaded, normalized to 8-bit RGBA, and uploaded at runtime via `createImageRGBA`.
- A single mutable struct shared with `_shared`/`RWS` lets the HUD and pipeline cooperate.
- Event lists centralize all input handling in one graph node.
- Per-card transforms, scissored reflections, and sorting produce a 3D effect in pure 2D vector graphics.

---

# Tutorial 20 — Reimplementing OpenCV's imshow

> **Source:** [`20-imshow_reimplementation.markdown`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/doc/samples/20-imshow_reimplementation.markdown) | [An Interactive Image Carousel](#tutorial-19--an-interactive-image-carousel)

This tutorial dissects the most feature-complete sample: a reimplementation of OpenCV's Qt-style `imshow` window, with 1:1 display, zoom around the cursor, panning, reset-to-fit, and the famous "deep zoom" mode that overlays every pixel's RGB value.

## The Code

You can find the complete source in [`imshow_reimplementation.cpp`](https://github.com/kallaballa/Plan-V4D/blob/beta/modules/v4d/samples/imshow_reimplementation.cpp). Run it with an image path; it falls back to `lena.png`.

```cpp
class ImshowReimplementation : public V4DPlan {
    UMat image_;   // original, as loaded
    UMat bgra_;    // BGRA copy for status bar / deep-zoom readout
    UMat rgba_;    // RGBA copy for the NanoVG upload
    string filename_;

    struct State {
        int   imageHandle_  = -1;
        int   imageWidth_   = 0;
        int   imageHeight_  = 0;
        int   channels_     = 0;
        float zoom_         = 1.0f;
        cv::Point2f pan_    = {0.0f, 0.0f};
        bool  isDragging_   = false;
        cv::Point2f mousePos_ = {-1.0f, -1.0f};
        bool  mouseInside_  = false;
        bool  showProperties_ = false;
        bool  showHelp_     = true;
        bool  showStatusBar_ = true;
        static constexpr float kDeepZoomThreshold = 30.0f;
    };
    static State state_;

    Event<Mouse> scroll_      = E<Mouse>(Mouse::SCROLL);
    Event<Mouse> drag_        = E<Mouse>(Mouse::DRAG);
    Event<Mouse> pressLeft_   = E<Mouse>(Mouse::PRESS,   Mouse::LEFT);
    Event<Mouse> releaseLeft_ = E<Mouse>(Mouse::RELEASE, Mouse::LEFT);
    Event<Mouse> pressRight_  = E<Mouse>(Mouse::PRESS,   Mouse::RIGHT);
    Event<Mouse> pressMiddle_ = E<Mouse>(Mouse::PRESS,   Mouse::MIDDLE);
    Event<Mouse> move_        = E<Mouse>(Mouse::MOVE);
    Event<Mouse> hoverEnter_  = E<Mouse>(Mouse::HOVER_ENTER);
    Event<Mouse> hoverExit_   = E<Mouse>(Mouse::HOVER_EXIT);
};
```

## Code Breakdown

### Zoom Around the Cursor

```cpp
for (auto se : scrollEvents) {
    float zoomFactor = (se->data().y > 0) ? 1.1f : 1.0f / 1.1f;
    float worldX = (se->position().x - state.pan_.x) / state.zoom_;
    float worldY = (se->position().y - state.pan_.y) / state.zoom_;
    state.zoom_ *= zoomFactor;
    state.zoom_ = std::clamp(state.zoom_, 0.01f, 1000.0f);
    state.pan_.x = se->position().x - worldX * state.zoom_;
    state.pan_.y = se->position().y - worldY * state.zoom_;
}
```

Before changing zoom, the cursor is converted to *image coordinates*; after changing zoom, the pan is recomputed so the same image point stays under the cursor.

### Deep Zoom

At `zoom_ >= 30`, every on-screen pixel cell becomes a miniature label showing its numeric channel values. Color images show three rows of text (R/G/B), grayscale shows a single brightness-shifted value, and the grid is stroked in screen space for crisp boundaries.

### The ImGui Layer

A full application: keyboard shortcuts (`Ctrl+arrow` pan, `Ctrl++/-` zoom, `Ctrl+O` open, `Ctrl+S` save …), a main menu bar, a file browser dialog, save-image/save-view dialogs, a properties dialog, and a help overlay.

### Reload on the Fly

A `branch(RWS(state_.reloadRequested_))` sub-graph re-imreads a new file, re-converts and re-uploads the texture within the same image dimensions, keeping the view transform valid.

## Summary

- `imshow`-style viewers stress-test the event system and GUI layer.
- A view transform (zoom + pan) keeps rendering code trivial.
- Deep zoom reads pixels from a private `UMat` copy and draws per-cell text with NanoVG.
- Keeping the original image separate from RGBA/BGRA working copies makes "save original" vs "save view" trivially correct.

This concludes the tutorial series. You have now seen the entire Plan-V4D feature set in action, from a single NanoVG image to a full-blown `imshow` reimplementation.
