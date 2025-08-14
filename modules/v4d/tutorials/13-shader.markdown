# Tutorial: Interactive Custom Shaders

This tutorial showcases a complete, interactive application built with Plan-V4D. We will create a Mandelbrot fractal explorer that renders the fractal using a custom GLSL shader, composites it over a video background, and allows for full user control via a GUI and direct mouse interaction (clicking to center, scrolling to zoom).

This example brings together nearly all the concepts covered so far:
-   Separation of low-level rendering logic into a helper class.
-   A complex `infer()` pipeline for compositing.
-   A `gui()` method for user-configurable settings.
-   Event handling for mouse clicks and scrolling.
-   A shared state object to communicate between the GUI, event handlers, and the renderer.

## The Code

The source code for this demo is in `samples/shader-demo.cpp`. It is quite large, so we will focus on the structure of the `Plan` and how it orchestrates the different components. The low-level OpenGL and shader code is encapsulated in a `MandelbrotScene` class, and camera logic is in a `Camera2D` struct, both of which are omitted here for brevity.

```cpp
// Abridged for clarity
class ShaderDemoPlan : public Plan {
    // Shared state for camera and fractal settings
    static struct Params {
        Camera2D camera_;
        MandelbrotScene::Settings settings_;
    } params_;

    MandelbrotScene scene_;

    // Event properties to capture mouse input
    Event<Mouse> release_ = E<Mouse>(Mouse::Type::RELEASE);
    Event<Mouse> scroll_ = E<Mouse>(Mouse::Type::SCROLL);

    // Static function to process mouse events
    static bool process_events(...) {
        // ... logic to update camera zoom/pan based on scroll/click events ...
        // ... disables auto-zoom on user interaction ...
        return params.settings_.autoZoom_; // Return true if auto-zoom should run
    }
public:
    // The gui() method provides ImGui controls for all fractal parameters
    void gui() override {
        imgui([](Params& params) {
            // ... ImGui::SliderFloat, DragFloat, ColorPicker4, Checkbox ...
        }, params_);
    }

    // The setup() method initializes the scene
    void setup() override {
        gl(&MandelbrotScene::init, RW(scene_));
    }

    // The infer() method orchestrates the main application logic
    void infer() override {
        capture();

        // A branch that processes events and conditionally updates the auto-zoom
        branch(process_events, ..., RWS(params_))
            ->plain(&Camera2D::updateAutoZoom, RWS(params_.camera_), ...)
        ->endBranch();

        // Render the fractal using the current settings and camera
        gl(&MandelbrotScene::render, R(scene_), ..., CS(params_.settings_), CS(params_.camera_));

        write();
    }
    // ... teardown() method to clean up resources ...
};
```

## Code Breakdown

### 1. The `MandelbrotScene` and `Camera2D` Helpers

-   **`MandelbrotScene`**: Just like `CubeScene` before, this class handles all the direct OpenGL calls: creating a quad to draw on, compiling the complex Mandelbrot fragment shader, and providing a `render()` method that sets the shader's `uniform` variables before drawing.
-   **`Camera2D`**: This struct holds the state for our view into the fractal (center coordinates and zoom level) and contains the logic for the smooth auto-zoom animation.

### 2. Event Handling

This `Plan` listens for mouse events to enable direct interaction.

```cpp
// Event properties to capture mouse input
Event<Mouse> release_ = E<Mouse>(Mouse::Type::RELEASE);
Event<Mouse> scroll_ = E<Mouse>(Mouse::Type::SCROLL);

// Static function to process mouse events
static bool process_events(..., const Mouse::List& scrollEvents, const Mouse::List& releaseEvents, ...) {
    if(!scrollEvents.empty() || !releaseEvents.empty()) {
        // ... logic to update camera zoom/pan based on scroll/click events ...
        params.settings_.autoZoom_ = false;
    }
    return params.settings_.autoZoom_;
}
```
-   **`E<Mouse>(...)`**: This creates an `Event` property. It's similar to `P<T>` but specifically for capturing event data. We create two: one for scroll events and one for mouse button release events.
-   **`process_events(...)`**: This function is used as the condition for a `branch`. It receives lists of any scroll or release events that occurred in the last frame. If the lists are not empty, it updates the camera position/zoom and disables the `autoZoom_` flag. The function's return value determines if the branch's body will execute.

### 3. The `infer()` Pipeline

The `infer` method ties everything together.

```cpp
void infer() override {
    capture();

    branch(process_events, ..., RWS(params_))
        ->plain(&Camera2D::updateAutoZoom, RWS(params_.camera_), ...)
    ->endBranch();

    gl(&MandelbrotScene::render, R(scene_), ..., CS(params_.settings_), CS(params_.camera_));

    write();
}
```
1.  **`capture()`**: Loads a video frame to use as a background.
2.  **`branch(process_events, ...)`**: This is the core of the interaction logic. The `process_events` function runs. If the user has interacted with the mouse, it updates the camera and returns `false`. If there's no interaction, it returns the current state of the `autoZoom_` flag.
3.  **`->plain(&Camera2D::updateAutoZoom, ...)`**: The body of the branch only executes if `process_events` returns `true`. This means the automatic zoom animation only runs when the user is not interacting via the mouse *and* the "Auto Zoom" checkbox in the GUI is checked.
4.  **`gl(&MandelbrotScene::render, ...)`**: Renders the fractal using the current state of the shared `params_` (which may have been updated by the GUI, mouse events, or the auto-zoom logic).
5.  **`write()`**: Writes the final composited frame to the sink.

## Summary

This tutorial demonstrated how to build a complex, fully-featured interactive application.
-   **Event Properties** (`E<T>`) are used to capture user input like mouse clicks and scrolls.
-   A `branch` can be used with a function as its condition to create sophisticated, stateful control flow.
-   The `gui()`, event handlers, and `infer()` methods can all work together on a shared data structure to create a responsive application.

This concludes the main tutorial series. The remaining demos (`font`, `pedestrian`, `optflow`, `beauty`, `many-cubes`) use the concepts you have learned here in various combinations to create more complex applications. You are encouraged to explore their source code to see more examples of Plan-V4D in action.
