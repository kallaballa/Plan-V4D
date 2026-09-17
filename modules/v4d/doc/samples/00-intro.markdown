# Tutorial: An Introduction to Plan-V4D

> **→ [Next Tutorial](01-display_image_nvg.markdown)**

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
| `RS(variable)` | **R**ead **S**hared | Provides thread-safe, read-only access to data shared between contexts (e.g., GUI and rendering). |
| `RWS(variable)` | **R**ead-**W**rite **S**hared | Provides thread-safe, read-write access to shared data. |
| `CS(variable)` | **C**opy **S**hared | Provides a thread-safe copy of shared data. |
| `F(fn, args…)` | **F**unction | Wraps a free function as a node in the graph. |
| `E<T>(…)` | **E**vent | Captures user input events (mouse, keyboard, joystick, window). |
| `P<T>(key)` | **P**roperty | A read-only edge bound to a value in `GlobalState` or `LocalState`. |

Using the most restrictive edge-call possible (e.g., using `R` instead of `RW` if you don't intend to modify the data) is a best practice that helps the Plan engine generate the most optimal graph.

## Helper Macros

When wrapping existing OpenCV functions or class methods so the engine can dispatch them, the `util.hpp` header provides a small set of pointer-cast macros (see `modules/v4d/include/opencv2/v4d/util.hpp`):

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

If `Plan` is the blueprint, then `V4D` is the toolbox. The V4D runtime provides the set of tools — called **contexts** — that you can use as nodes in your graph. Each context is specialized for a certain task:

- **`nvg`**: For 2D vector graphics and text via NanoVG.
- **`fb`**: For direct access to the framebuffer as a `cv::UMat`.
- **`gl`**: For executing raw OpenGL commands. `gl<-1>(V(idx), …)` routes the call to a worker OpenGL context for parallelism.
- **`bgfx`**: For raw bgfx calls (`bgfx::touch`, `bgfx::dbgTextPrintf`, …).
- **`ext`**: For arbitrary, runtime-specific contexts.
- **`imgui`**: For creating user interfaces with Dear ImGui.
- **`plain`**: For running general-purpose code, like standard OpenCV functions.

In addition, `V4D` provides a `Source` / `Sink` system, exposed inside a `V4DPlan` as the `capture()` and `write()` graph calls.

## Lifecycle of a `V4DPlan`

A `V4DPlan` (see `v4d.hpp:422`) inherits from `Plan` and adds four virtual hooks you can override:

- `setup()` — emitted once, before the frame loop. Use it to allocate resources.
- `infer()` — emitted every frame. Use it for the main rendering/processing pipeline.
- `gui()` — emitted once on the display thread, before the frame loop. Use it for ImGui UI.
- `teardown()` — emitted once after the frame loop exits. Use it to free resources.

## Getting Started

This series of tutorials will guide you from the basics to advanced applications. Each tutorial builds on the concepts of the last, so it is recommended to follow them in order. Every tutorial links to its complete source sample in the [samples directory](../../samples/); all source files are also installed to `/usr/share/plan-v4d/v4d/samples/` on packaged builds.

1. [Tutorial 01 — Displaying an Image with NanoVG](01-display_image_nvg.markdown) (`display_image_nvg.cpp`)
2. [Tutorial 02 — Displaying an Image via Framebuffer](02-display_image_fb.markdown) (`display_image_fb.cpp`)
3. [Tutorial 03 — 2D Vector Graphics with NanoVG](03-vector_graphics.markdown) (`vector_graphics.cpp`)
4. [Tutorial 04 — Combining Vector Graphics and Framebuffer Processing](04-vector_graphics_and_fb.markdown) (`vector_graphics_and_fb.cpp`)
5. [Tutorial 05 — Direct OpenGL Rendering](05-render_opengl.markdown) (`render_opengl.cpp`)
6. [Tutorial 06 — Font Rendering](06-font_rendering.markdown) (`font_rendering.cpp`)
7. [Tutorial 07 — Simple Video Editing](07-video_editing.markdown) (`video_editing.cpp`)
8. [Tutorial 08 — Custom Source and Sink](08-custom_source_and_sink.markdown) (`custom_source_and_sink.cpp`)
9. [Tutorial 09 — Font Rendering with a GUI](09-font_with_gui.markdown) (`font_with_gui.cpp`)
10. [Tutorial 10 — Rendering a 3D Cube](10-cube.markdown) (`cube-demo.cpp`, `cubescene.hpp`)
11. [Tutorial 11 — Compositing 3D Graphics on Video](11-video.markdown) (`video-demo.cpp`)
12. [Tutorial 12 — Advanced NanoVG and Processing Pipelines](12-nanovg.markdown) (`nanovg-demo.cpp`)
13. [Tutorial 13 — Interactive Custom Shaders](13-shader.markdown) (`shader-demo.cpp`)
14. [Tutorial 14 — Advanced Font Effects Demo](14-font.markdown) (`font-demo.cpp`)
15. [Tutorial 15 — Pedestrian Detection and Tracking Demo](15-pedestrian.markdown) (`pedestrian-demo.cpp`)
16. [Tutorial 16 — Sparse Optical Flow Demo](16-optflow.markdown) (`optflow-demo.cpp`)
17. [Tutorial 17 — Real-Time "Beauty Filter" Demo](17-beauty.markdown) (`beauty-demo.cpp`)
18. [Tutorial 18 — Parallel Rendering with Multiple OpenGL Contexts](18-many-cubes.markdown) (`many_cubes-demo.cpp`)
19. [Tutorial 19 — An Interactive Image Carousel](19-image_carousel.markdown) (`image_carousel.cpp`)
20. [Tutorial 20 — Reimplementing OpenCV's imshow](20-imshow_reimplementation.markdown) (`imshow_reimplementation.cpp`)

Ready to dive in? Let's start by displaying a simple image.
