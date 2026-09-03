# Tutorial: Combining Vector Graphics and Framebuffer Processing

A powerful feature of Plan-V4D is the ability to chain different contexts together to create sophisticated rendering and processing pipelines. In this tutorial, we will extend the previous vector graphics example by adding a framebuffer processing step.

First, we will draw the animated googly eyes using the `nvg` context. Then, we will immediately access the resulting image in the framebuffer using the `fb` context and apply a blur effect to it using a standard OpenCV function.

## The Code

Here is the complete source code for this example. You can find it in `modules/v4d/samples/vector_graphics_and_fb.cpp`.

```cpp
#include <opencv2/v4d/v4d.hpp>

using namespace cv;
using namespace cv::v4d;

class VectorGraphicsAndFBPlan : public V4DPlan {
    Property<cv::Size> sz_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    void infer() override {
        // Again creates a NanoVG context and draws googly eyes
        nvg([](const Size& sz) {
            // Calls from this namespace may only be used inside a nvg context
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

            bg = linearGradient(x, y + h * 0.5f, x + w * 0.1f, y + h, cv::Scalar(0, 0, 0, 32), cv::Scalar(0, 0, 0, 16));
            beginPath();
            ellipse(lx + 3.0f, ly + 16.0f, ex, ey);
            ellipse(rx + 3.0f, ry + 16.0f, ex, ey);
            fillPaint(bg);
            fill();

            bg = linearGradient(x, y + h * 0.25f, x + w * 0.1f, y + h,
                    cv::Scalar(220, 220, 220, 255), cv::Scalar(128, 128, 128, 255));
            beginPath();
            ellipse(lx, ly, ex, ey);
            ellipse(rx, ry, ex, ey);
            fillPaint(bg);
            fill();

            dx = (mx - rx) / (ex * 10);
            dy = (my - ry) / (ey * 10);
            d = sqrtf(dx * dx + dy * dy);
            if (d > 1.0f) {
                dx /= d;
                dy /= d;
            }
            dx *= ex * 0.4f;
            dy *= ey * 0.5f;
            beginPath();
            ellipse(lx + dx, ly + dy + ey * 0.25f * (1 - blink), br, br * blink);
            fillColor(cv::Scalar(32, 32, 32, 255));
            fill();

            dx = (mx - rx) / (ex * 10);
            dy = (my - ry) / (ey * 10);
            d = sqrtf(dx * dx + dy * dy);
            if (d > 1.0f) {
                dx /= d;
                dy /= d;
            }
            dx *= ex * 0.4f;
            dy *= ey * 0.5f;
            beginPath();
            ellipse(rx + dx, ry + dy + ey * 0.25f * (1 - blink), br, br * blink);
            fillColor(cv::Scalar(32, 32, 32, 255));
            fill();

            gloss = radialGradient(lx - ex * 0.25f, ly - ey * 0.5f, ex * 0.1f, ex * 0.75f,
                    cv::Scalar(255, 255, 255, 128), cv::Scalar(255, 255, 255, 0));
            beginPath();
            ellipse(lx, ly, ex, ey);
            fillPaint(gloss);
            fill();

            gloss = radialGradient(rx - ex * 0.25f, ry - ey * 0.5f, ex * 0.1f, ex * 0.75f,
                    cv::Scalar(255, 255, 255, 128), cv::Scalar(255, 255, 255, 0));
            beginPath();
            ellipse(rx, ry, ex, ey);
            fillPaint(gloss);
            fill();
        }, sz_);

        // Provides the framebuffer as left-off by the nvg context.
        fb([](UMat& framebuffer) {
            // Heavily blurs the eyes using a cheap boxFilter
            boxFilter(framebuffer, framebuffer, -1, Size(15, 15), Point(-1, -1), true, BORDER_REPLICATE);
        });
    }
};

int main() {
    cv::Rect viewport(0, 0, 960, 960);
    Ptr<V4D> runtime = V4D::init(viewport, "Vector Graphics and Framebuffer", AllocateFlags::NANOVG | AllocateFlags::IMGUI);
    V4DPlan::run<VectorGraphicsAndFBPlan>(0);
}
```

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
        boxFilter(framebuffer, framebuffer, -1, Size(15, 15), Point(-1, -1), true, BORDER_REPLICATE);
    });
}
```

### Step 1: The `nvg` Context

The first part of the `infer` method is identical to our previous tutorial. The `nvg` context is used to draw the animated googly eyes. When this context finishes, the resulting image is present in the window's framebuffer.

### Step 2: The `fb` Context

This is the new addition. Immediately following the `nvg` context, we open an `fb` context.

- **State Preservation**: The `fb` context picks up the framebuffer exactly where the `nvg` context left off. The pixels drawn by NanoVG are now available as a `UMat`.
- **Image Processing on the GPU**: The lambda receives a `UMat` handle to the framebuffer. We can then use any `UMat`-compatible OpenCV function to process this image directly.
- **`boxFilter(…)`**: We apply a simple `boxFilter` to blur the entire framebuffer image in-place. Because we are operating on a `UMat`, this processing can be hardware-accelerated on the GPU if OpenCL is available.

The final result is that the vector-drawn googly eyes appear blurred on the screen.

## Summary

This tutorial demonstrated a key concept in V4D: **context chaining**.

- You can execute different contexts sequentially within the same `infer()` call.
- The output of one graphics context (like `nvg`) can serve as the input for a subsequent processing context (like `fb`).
- This allows you to create powerful pipelines that combine rendering with GPU-accelerated image processing.

In the next tutorial, we will explore how to perform custom OpenGL rendering.