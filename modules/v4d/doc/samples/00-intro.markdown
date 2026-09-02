# Tutorial: An Introduction to Plan-V4D

Welcome to the Plan-V4D tutorial series. Before we dive into the code, it's important to understand the core philosophy behind the framework, as it's different from traditional, sequential programming. This introduction will give you the mental model you need to get the most out of the examples that follow.

## What is Plan-V4D?

Plan-V4D is a C++20 framework for building high-performance, graphical applications. It consists of two main components:

- **Plan**: A task-graph engine for creating highly parallel and concurrent applications.
- **V4D**: A versatile 2D/3D graphics runtime that provides the tools for your Plan.

## The "Plan" Philosophy: A Compile-Time Task Graph

When you write a class that inherits from `V4DPlan`, you are not writing a typical, sequential program. Instead, you are **describing a task graph at compile time**.

Through C++ template metaprogramming, the very structure of your code — the sequence of your context calls (`nvg`, `fb`, `gl`, `imgui`, `plain`, …) inside your `infer()` method — is analyzed by the compiler and "baked" directly into your executable as a highly optimized, directed acyclic graph (DAG).

There is no heavyweight runtime that builds this graph when your application starts. The graph *is* your program. This compile-time approach is the key to Plan-V4D's performance, as it allows for optimizations and scheduling decisions to be made before the program even runs.

## Interacting with the Graph: Edges

Because the graph is built by the compiler, the compiler needs to understand exactly how each task (each node in the graph) interacts with the application's data. You provide this crucial information using **edge-calls**.

Edge-calls are small wrappers around your variables that declare your *intent*. They tell the graph whether you intend to read, write, or copy a piece of data. This declaration is what allows the Plan engine to automatically manage data, prevent race conditions, and schedule tasks for maximum parallelism.

Here are the primary edge-calls you will encounter:

| Edge-Call | Name | Purpose |
| :--- | :--- | :--- |
| `V(value)` | **V**alue | Passes a constant value or literal directly to a function. |
| `R(variable)` | **R**ead | Provides safe, read-only access to a variable. |
| `RW(variable)` | **R**ead-**W**rite | Provides read-write access to a variable. |
| `C(variable)` | **C**opy | Passes the variable by copy. |
| `RS(variable)` | **R**ead **S**hared | Provides thread-safe, read-only access to data shared between contexts (e.g., GUI and rendering). |
| `RWS(variable)` | **R**ead-**W**rite **S**hared | Provides thread-safe, read-write access to shared data. |
| `CS(variable)` | **C**opy **S**hared | Provides a thread-safe copy of shared data. |
| `F(fn, args…)` | **F**unction | Wraps a free function as a node in the graph. |
| `E<T>(…)` | **E**vent | Captures user input events (mouse, keyboard, joystick, window). |
| `P<T>(key)` | **P**roperty | A read-only pinhole into a runtime/worker state slot. |

Using the most restrictive edge-call possible (e.g., using `R` instead of `RW` if you don't intend to modify the data) is a best practice that helps the Plan engine generate the most optimal graph.

## Helper Macros

When wrapping existing OpenCV functions or class methods so the engine can dispatch them, the `util.hpp` header provides a small set of pointer‑cast macros (see `modules/v4d/include/opencv2/v4d/util.hpp`):

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
- `gui()` — emitted every frame on the display thread. Use it for ImGui UI.
- `teardown()` — emitted once after the frame loop exits. Use it to free resources.

## Getting Started

This series of tutorials will guide you from the basics to advanced applications. Each tutorial builds on the concepts of the last, so it's recommended to follow them in order.

Ready to dive in? Let's start by displaying a simple image.