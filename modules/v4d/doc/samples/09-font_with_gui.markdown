# Tutorial: Font Rendering with a GUI

A key feature of a graphical application is user interaction. Plan-V4D integrates the popular [Dear ImGui](https://github.com/ocornut/imgui) library to make creating user interfaces simple and efficient.

This tutorial demonstrates how to add a GUI to our font rendering example, allowing the user to change the font size and color in real-time.

## The Code

Here is the complete source code for this example. You can find it in `modules/v4d/samples/font_with_gui.cpp`.

```cpp
#include <opencv2/v4d/v4d.hpp>

using namespace cv;
using namespace cv::v4d;

class FontWithGuiPlan: public V4DPlan {
    // A static struct to hold parameters shared between the GUI and rendering.
    // Because it is static, the GUI thread and the worker thread share one
    // instance. The GUI mutates it (RWS) and the renderer reads a thread-safe
    // copy (CS) every frame.
    static struct Params {
        float fontSize_ = 40.0f;
        cv::Scalar_<float> color_ = {1.0f, 0.0f, 0.0f, 1.0f};
    } params_;

    // The text
    string hw_ = "hello world";
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    // The gui() method is called every frame to draw the UI.
    void gui() override {
        imgui([](Params& params) {
            using namespace ImGui;
            Begin("Settings");
            SliderFloat("Font Size", &params.fontSize_, 1.0f, 100.0f);
            ColorPicker4("Text Color", params.color_.val);
            End();
        }, RWS(params_));
    }

    // The infer() method renders the scene.
    void infer() override {
        // Render the text at the center of the screen using parameters from the GUI.
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

// Define the static member variable
FontWithGuiPlan::Params FontWithGuiPlan::params_;

int main() {
    cv::Rect viewport(0, 0, 960, 960);
    Ptr<V4D> runtime = V4D::init(viewport, "Font Rendering with GUI", AllocateFlags::NANOVG | AllocateFlags::IMGUI);
    V4DPlan::run<FontWithGuiPlan>(0);
}
```

## Code Breakdown

### 1. The `gui()` Method

This is a new `V4DPlan` method dedicated to building and rendering the user interface. It is called on every frame on the display thread.

```cpp
void gui() override {
    imgui([](Params& params) {
        using namespace ImGui;
        Begin("Settings");
        SliderFloat("Font Size", &params.fontSize_, 1.0f, 100.0f);
        ColorPicker4("Text Color", params.color_.val);
        End();
    }, RWS(params_));
}
```

- **`imgui([…])`**: This is the **ImGui context**. It provides an environment where you can call standard ImGui functions.
- **`using namespace ImGui;`**: This brings the ImGui functions into the current scope for convenience.
- **`Begin("Settings")` / `End()`**: These functions create a new ImGui window.
- **`SliderFloat(…)`** and **`ColorPicker4(…)`**: These are standard ImGui widgets that create a slider and a color picker, respectively. They directly modify the fields of the `params` struct that is passed into the context.
- **`RWS(params_)`**: Because `params_` is shared between threads (the GUI thread mutates it, the worker thread reads it), we use the **R**ead-**W**rite **S**hared edge.

### 2. Shared Parameters

To share data between the `gui()` and `infer()` methods, we define a `static struct`.

```cpp
static struct Params {
    float fontSize_ = 40.0f;
    cv::Scalar_<float> color_ = {1.0f, 0.0f, 0.0f, 1.0f};
} params_;
```

It's declared `static` so that there is only one instance of it, which can be accessed from both the GUI and rendering logic. The `gui()` method writes to this struct, and the `infer()` method reads from it.

### 3. The `infer()` Method

The `infer()` method is similar to the previous font rendering example, but now it uses the shared `params_` to style the text.

```cpp
void infer() override {
    nvg([](…, const Params& params) {
        // ...
        fontSize(params.fontSize_);
        // ...
        fillColor(params.color_ * 255.0);
        // ...
    }, …, CS(params_));
}
```

- **`CS(params_)`**: This is the **C**opy **S**hared edge-call. It's used because `params_` is a shared resource, modified in the GUI context and read in this rendering context. Using `CS` provides the rendering thread with a consistent snapshot of the parameters for the duration of the frame, preventing visual artifacts that could happen if the GUI changed the values mid-render.

## Summary

This tutorial introduced the basics of creating user interfaces with ImGui in Plan-V4D:

- The **`gui()`** method in a `V4DPlan` is used to define the UI; it runs on the display thread once per frame.
- The **`imgui`** context allows you to use standard ImGui functions.
- You can share data between the `gui()` and `infer()` methods using a `static` struct.
- The **`RWS`** (Read-Write Shared) edge-call is used when a context mutates a shared resource.
- The **`CS`** (Copy Shared) edge-call is used to safely read shared data in the rendering context.

The remaining tutorials are more advanced demos. You are encouraged to explore their source code to see more complex examples of what Plan-V4D can do.