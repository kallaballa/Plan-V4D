# Tutorial: An Introduction to Plan-V4D

Welcome to the Plan-V4D tutorial series. Before we dive into the code, it's important to understand the core philosophy behind the framework, as it's different from traditional, sequential programming. This introduction will give you the mental model you need to get the most out of the examples that follow.

## What is Plan-V4D?

Plan-V4D is a C++20 framework for building high-performance, graphical applications. It consists of two main components:
-   **Plan**: A task graph engine for creating highly parallel and concurrent applications.
-   **V4D**: A versatile 2D/3D graphics runtime that provides the tools for your Plan.

## The "Plan" Philosophy: A Compile-Time Task Graph

When you write a class that inherits from `Plan`, you are not writing a typical, sequential program. Instead, you are **describing a task graph at compile time**.

Through C++ template metaprogramming, the very structure of your code—the sequence of your context calls (`nvg`, `fb`, `gl`, etc.) inside your `infer()` method—is analyzed by the compiler and "baked" directly into your executable as a highly optimized, directed acyclic graph (DAG).

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

Using the most restrictive edge-call possible (e.g., using `R` instead of `RW` if you don't intend to modify the data) is a best practice that helps the Plan engine generate the most optimal graph.

## The V4D Runtime: Your Toolkit

If `Plan` is the blueprint, then `V4D` is the toolbox. The V4D runtime provides the set of tools—called **contexts**—that you can use as nodes in your graph. Each context is specialized for a certain task:

-   **`nvg`**: For 2D vector graphics and text via NanoVG.
-   **`fb`**: For direct access to the framebuffer as a `cv::UMat`.
-   **`gl`**: For executing raw OpenGL commands.
-   **`imgui`**: For creating user interfaces with Dear ImGui.
-   **`plain`**: For running general-purpose code, like standard OpenCV functions.

## Getting Started

This series of tutorials will guide you from the basics to advanced applications. Each tutorial builds on the concepts of the last, so it's recommended to follow them in order.

Ready to dive in? Let's start by displaying a simple image.
