# Tutorial: Direct OpenGL Rendering

While V4D provides high-level contexts like `nvg` and `fb`, it also gives you direct access to the underlying OpenGL API for custom rendering. This is done through the `gl` context, which allows you to execute raw OpenGL commands within your `V4DPlan`.

This tutorial demonstrates the most basic use of the `gl` context: clearing the screen to a solid blue color.

## The Code

Here is the complete source code for this example. You can find it in `modules/v4d/samples/render_opengl.cpp`.

```cpp
#include <opencv2/v4d/v4d.hpp>

using namespace cv;
using namespace cv::v4d;

// A V4DPlan implementation that renders a blue screen using OpenGL
class RenderOpenGLPlan : public V4DPlan {
public:
    // Setup phase of inference: Creates graph nodes that run once at the start of the algorithm's lifetime
    void setup() override {
        // Sets the clear color to blue by creating a graph node with an OpenGL context (provided by V4D).
        gl(glClearColor, V(0.0f), V(0.0f), V(1.0f), V(1.0f));
    }

    // Main phase of inference: Creates graph nodes that run in a loop after the nodes created by the setup phase have run
    void infer() override {
        // Clears the screen. The clear color and other GL states are preserved between context-calls.
        gl(glClear, V(GL_COLOR_BUFFER_BIT));
    }
};

int main() {
    // The viewport may be changed at runtime by creating a set node
    cv::Rect viewport(0, 0, 960, 960);
    // Initialization of the V4D runtime must be invoked before V4DPlan::run is called.
    // There are AllocateFlags for selective initialization of subsystems, ConfigFlags, and DebugFlags.
    Ptr<V4D> runtime = V4D::init(viewport, "GL Blue Screen", AllocateFlags::IMGUI);
    // Build (infer) and run the graph. The number denotes the number of workers (0 meaning one worker plus the main thread).
    V4DPlan::run<RenderOpenGLPlan>(0);
}
```

## Code Breakdown

### 1. The `gl` Context

The `gl` context is the gateway to the OpenGL API. It is designed to be a lightweight wrapper that integrates standard OpenGL functions into the Plan task graph.

Its usage is very straightforward:

```cpp
gl(openGLFunction, arg1, arg2, …);
```

The two-argument form `gl<-1>(V(idx), …)` (see `v4d.hpp:584`) routes the call to one of V4D's worker OpenGL contexts for parallel execution.

### 2. The `setup()` Phase: Setting State

In the `setup()` method, we set the desired clear color. This only needs to be done once.

```cpp
void setup() override {
    gl(glClearColor, V(0.0f), V(0.0f), V(1.0f), V(1.0f));
}
```

- **`gl(glClearColor, …)`**: We are telling the `V4DPlan` to create a node in its task graph that will execute the `glClearColor` function.
- **`V(…)`**: This is an **edge-call**. Edge-calls are how you pass data to functions inside a context. `V` stands for **Value** (or constant). It tells the graph that we are passing these literal values directly to the function.

V4D preserves the OpenGL state between context calls. So, the clear color we set here will remain active for all subsequent `gl` contexts.

### 3. The `infer()` Phase: Performing Actions

In the `infer()` method, which runs on every frame, we perform the action of clearing the screen.

```cpp
void infer() override {
    gl(glClear, V(GL_COLOR_BUFFER_BIT));
}
```

This creates a graph node that calls `glClear` with the `GL_COLOR_BUFFER_BIT` flag, which clears the color buffer to the blue color we set in `setup()`.

## Summary

This tutorial introduced the `gl` context for direct OpenGL rendering:

- You can call any standard OpenGL function by passing it to the `gl` context.
- OpenGL state is preserved between `gl` context calls.
- **Edge-calls** like `V()` are used to pass arguments to the OpenGL functions within the graph.
- Use `gl<-1>(V(idx), …)` to target a specific worker OpenGL context for parallel execution.

In the next tutorial, we will look at font rendering.
