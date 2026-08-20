# Tutorial: Direct OpenGL Rendering

While V4D provides high-level contexts like `nvg` and `fb`, it also gives you direct access to the underlying OpenGL API for custom rendering. This is done through the `gl` context, which allows you to execute raw OpenGL commands within your `Plan`.

This tutorial demonstrates the most basic use of the `gl` context: clearing the screen to a solid blue color.

## The Code

Here is the complete source code for this example. You can find it in `samples/render_opengl.cpp`.

```cpp
#include <opencv2/v4d/v4d.hpp>

using namespace cv;
using namespace cv::v4d;

// A Plan implementation that renders a blue screen using OpenGL
class RenderOpenGLPlan : public Plan {
public:
    // Setup phase: set the OpenGL clear color
    void setup() override {
        // Use a 'gl' context to call the standard glClearColor function.
        // The 'V' edge-call passes the following values as constants.
        gl(glClearColor, V(0.0f), V(0.0f), V(1.0f), V(1.0f));
    }

    // Infer phase: clear the screen on every frame
    void infer() override {
        // Use a 'gl' context to call glClear.
        // GL states (like the clear color) are preserved between context calls.
        gl(glClear, V(GL_COLOR_BUFFER_BIT));
    }
};

int main() {
    cv::Rect viewport(0, 0, 960, 960);
    // Initialize V4D. Note that we don't need to allocate NANOVG for this example.
    Ptr<V4D> runtime = V4D::init(viewport, "GL Blue Screen", AllocateFlags::IMGUI);
    // Build and run the graph
    Plan::run<RenderOpenGLPlan>(0);
}
```

## Code Breakdown

### 1. The `gl` Context

The `gl` context is the gateway to the OpenGL API. It's designed to be a lightweight wrapper that integrates standard OpenGL functions into the Plan task graph.

Its usage is very straightforward:
`gl(openGL_function_name, arg1, arg2, ...);`

### 2. The `setup()` Phase: Setting State

In the `setup()` method, we set the desired clear color. This only needs to be done once.

```cpp
void setup() override {
    gl(glClearColor, V(0.0f), V(0.0f), V(1.0f), V(1.0f));
}
```
- **`gl(glClearColor, ...)`**: We are telling the `Plan` to create a node in its task graph that will execute the `glClearColor` function.
- **`V(...)`**: This is an **edge-call**. Edge-calls are how you pass data to functions inside a context. `V` stands for **Value** (or constant). It tells the graph that we are passing these literal values directly to the function.

V4D preserves the OpenGL state between context calls. So, the clear color we set here will remain active for all subsequent `gl` contexts.

### 3. The `infer()` Phase: Performing Actions

In the `infer()` method, which runs on every frame, we perform the action of clearing the screen.

```cpp
void infer() override {
    gl(glClear, V(GL_COLOR_BUFFER_BIT));
}
```
This creates a graph node that calls `glClear` with the `GL_COLOR_BUFFER_BIT` flag, which clears the color buffer to the blue color we set in `setup()`.

### A Note on Edge-Calls

The `V` edge-call is the simplest one, used for constants. Plan provides others for more complex data handling:
- **`R(variable)`**: Read-only access to a variable.
- **`RW(variable)`**: Read-write access to a variable.
- **`C(variable)`**: Access by copy.
- **`RS`, `RWS`, `CS`**: Variants for data that is explicitly marked as `shared` between different threads or Plans.

Using the most restrictive edge-call possible (e.g., `R` instead of `RW` if you don't modify the data) helps the Plan engine to build a more optimal and parallelized task graph.

## Summary

This tutorial introduced the `gl` context for direct OpenGL rendering:
- You can call any standard OpenGL function by passing it to the `gl` context.
- OpenGL state is preserved between `gl` context calls.
- **Edge-calls** like `V()` are used to pass arguments to the OpenGL functions within the graph.

In the next tutorial, we will look at font rendering.
