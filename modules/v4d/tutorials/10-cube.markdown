# Tutorial: Rendering a 3D Cube

This tutorial demonstrates how to structure a more complex rendering application by separating the low-level OpenGL logic from the main application `Plan`. We will create a `Plan` that renders a rotating 3D cube, showcasing a clean way to organize your code and introducing the `teardown` phase of a `Plan`'s lifecycle.

## The Code

This example is split into two files:
1.  `samples/cubescene.hpp`: Contains a `CubeScene` class that encapsulates all the low-level OpenGL code for creating and drawing the cube.
2.  `samples/cube-demo.cpp`: Contains the `CubeDemoPlan` and the `main` function.

### `cubescene.hpp` (Abridged)

This file handles the nitty-gritty of OpenGL. It defines the cube's vertices, colors, and indices. It also has methods to initialize shaders and buffers, render the cube, and clean up resources.

```cpp
// samples/cubescene.hpp
class CubeScene {
    // ... private members for vertices, colors, shaders, GL handles ...
public:
    // Initializes objects, buffers, shaders and uniforms
    void init() {
        // Standard OpenGL setup: glGenVertexArrays, glGenBuffers, glBufferData, etc.
        // Compiles vertex and fragment shaders.
    }

    // Renders a rotating rainbow-colored cube
    void render(const double xpos = 0.0, const double ypos = 0.0) const {
        // Calculates rotation matrices based on time.
        // Sets the uniform transform matrix in the shader.
        // Binds the VAO and calls glDrawElements.
    }

    // Cleans up OpenGL resources
    void destroy() const {
        // Calls glDeleteProgram, glDeleteBuffers, glDeleteVertexArrays.
    }
};
```

### `cube-demo.cpp`

This file uses the `CubeScene` class within a `Plan` to manage the application's lifecycle.

```cpp
// samples/cube-demo.cpp
#include <opencv2/v4d/v4d.hpp>
#include "cubescene.hpp"

using namespace cv::v4d;

class CubeDemoPlan : public Plan {
    CubeScene scene_;
public:
    void setup() override {
        // Initialize the scene's OpenGL resources
        gl(&CubeScene::init, RW(scene_));
    }

    void infer() override {
        // Set a V4D property to change the clear color
        set(V4D::Keys::CLEAR_COLOR, V(cv::Scalar(102, 61, 51, 255)));
        // Use the high-level clear function
        clear();
        // Render the scene
        gl(&CubeScene::render, R(scene_), V(0.0), V(0.0));
    }

    void teardown() override {
        // Clean up the scene's OpenGL resources
        gl(&CubeScene::destroy, R(scene_));
    }
};

int main() {
    cv::Rect viewport(0, 0, 1920, 1080);
    cv::Ptr<V4D> runtime = V4D::init(viewport, "Cube Demo", AllocateFlags::IMGUI);
    Plan::run<CubeDemoPlan>(2);
    return 0;
}
```

## Code Breakdown

### 1. Separation of Concerns

The key design pattern here is the separation of OpenGL logic (`CubeScene`) from the application flow (`CubeDemoPlan`). This makes the code much cleaner:
-   `CubeDemoPlan` doesn't need to know *how* the cube is drawn; it only needs to know that it should call `init`, `render`, and `destroy` at the appropriate times.
-   `CubeScene` is a self-contained, reusable component that could be used in other `Plan`s.

### 2. The `Plan` Lifecycle: `setup`, `infer`, `teardown`

This example introduces the `teardown()` method, completing the picture of a `Plan`'s lifecycle.

-   **`setup()`**: Called once at the beginning. We use it to initialize our `CubeScene`'s OpenGL resources.
-   **`infer()`**: Called repeatedly in a loop. We use it to clear the screen and render a frame of our scene.
-   **`teardown()`**: Called once at the very end, after the main loop has finished. This is the perfect place to do cleanup, so we use it to destroy the `CubeScene`'s OpenGL resources.

### 3. High-Level V4D Functions

This example also shows a more abstract way to clear the screen.

```cpp
void infer() override {
    set(V4D::Keys::CLEAR_COLOR, V(cv::Scalar(102, 61, 51, 255)));
    clear();
    //...
}
```
-   **`set(V4D::Keys::CLEAR_COLOR, ...)`**: This is a V4D function that sets an internal property. Here, we set the clear color.
-   **`clear()`**: This function clears the screen using the color we just set. It's a higher-level alternative to calling `glClearColor` and `glClear` in `gl` contexts.

## Summary

This tutorial demonstrated a clean and scalable way to structure a 3D application in Plan-V4D.
-   Encapsulate complex rendering logic in a separate helper class.
-   Use the `Plan`'s `setup()`, `infer()`, and `teardown()` methods to manage the lifecycle of your rendering objects.
-   The `teardown()` method is the ideal place for resource cleanup.
-   V4D provides high-level functions like `set` and `clear` for common operations.

Next, we will look at a more complex video processing demo.
