# Tutorial: Combining Vector Graphics and Framebuffer Processing

A powerful feature of Plan-V4D is the ability to chain different contexts together to create sophisticated rendering and processing pipelines. In this tutorial, we will extend the previous vector graphics example by adding a framebuffer processing step.

First, we will draw the animated googly eyes using the `nvg` context. Then, we will immediately access the resulting image in the framebuffer using the `fb` context and apply a blur effect to it using a standard OpenCV function.

## The Code

Here is the complete source code for this example. You can find it in `samples/vector_graphics_and_fb.cpp`.

```cpp
#include <opencv2/v4d/v4d.hpp>
#include <opencv2/v4d/util.hpp>

using namespace cv;
using namespace cv::v4d;

class VectorGraphicsAndFBPlan : public Plan {
    Property<cv::Size> sz_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    void infer() override {
        //Again creates a NanoVG context and draws googly eyes
        nvg([](const Size& sz) {
            // (Drawing code is the same as the previous tutorial)
            // ...
        });

        //Provides the framebuffer as left-off by the nvg context.
        fb([](UMat& framebuffer) {
            //Heavily blurs the eyes using a cheap boxFilter
            boxFilter(framebuffer, framebuffer, -1, Size(15, 15), Point(-1,-1), true, BORDER_REPLICATE);
        });
    }
};

int main() {
    cv::Rect viewport(0, 0, 960, 960);
    Ptr<V4D> runtime = V4D::init(viewport, "Vector Graphics and Framebuffer", AllocateFlags::NANOVG | AllocateFlags::IMGUI);
    Plan::run<VectorGraphicsAndFBPlan>(0);
}
```
*(For brevity, the NanoVG drawing code, which is identical to the previous tutorial, has been omitted here.)*

## Code Breakdown

### The `infer()` Phase: Chaining Contexts

The magic happens within the `infer()` method. It contains two contexts called in sequence.

```cpp
void infer() override {
    // 1. First, draw the vector graphics
    nvg([](const Size& sz) {
        // ... googly eyes drawing logic ...
    }, sz_);

    // 2. Then, process the result in the framebuffer
    fb([](UMat& framebuffer) {
        boxFilter(framebuffer, framebuffer, -1, Size(15, 15));
    });
}
```

### Step 1: The `nvg` Context

The first part of the `infer` method is identical to our previous tutorial. The `nvg` context is used to draw the animated googly eyes. When this context finishes, the resulting image is present in the window's framebuffer.

### Step 2: The `fb` Context

This is the new addition. Immediately following the `nvg` context, we open an `fb` context.
- **State Preservation**: The `fb` context picks up the framebuffer exactly where the `nvg` context left off. The pixels drawn by NanoVG are now available as a `UMat`.
- **Image Processing on the GPU**: The lambda receives a `UMat` handle to the framebuffer. We can then use any `UMat`-compatible OpenCV function to process this image directly.
- **`boxFilter(...)`**: We apply a simple `boxFilter` to blur the entire framebuffer image in-place. Because we are operating on a `UMat`, this processing can be hardware-accelerated on the GPU if OpenCL is available.

The final result is that the vector-drawn googly eyes appear blurred on the screen.

## Summary

This tutorial demonstrated a key concept in V4D: **context chaining**.
- You can execute different contexts sequentially within the same `infer()` call.
- The output of one graphics context (like `nvg`) can serve as the input for a subsequent processing context (like `fb`).
- This allows you to create powerful pipelines that combine rendering with GPU-accelerated image processing.

In the next tutorial, we will explore how to perform custom OpenGL rendering.
