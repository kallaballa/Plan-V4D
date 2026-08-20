# Tutorial: 2D Vector Graphics with NanoVG

Plan-V4D provides a powerful 2D vector graphics API through its integration with NanoVG. This tutorial demonstrates how to use the `nvg` context to draw shapes, apply gradients, and create simple animations. We will walk through an example that draws a pair of animated googly eyes.

## The Code

Here is the complete source code for this example. You can find it in `samples/vector_graphics.cpp`.

```cpp
#include <opencv2/v4d/v4d.hpp>

using namespace cv;
using namespace cv::v4d;

class VectorGraphicsPlan: public Plan {
Property<cv::Size> sz_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    void infer() override {
        //Creates a NanoVG context and draws googly eyes that occasionally blink.
        nvg([](const Size &sz) {
            //Calls from this namespace may only be used inside a nvg context.
            //Nvg calls work exactly like their c-funtion counterparts.
            //Please refer to the NanoVG documentation for details.
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

            bg = linearGradient(x, y + h * 0.5f, x + w * 0.1f, y + h,
                    cv::Scalar(0, 0, 0, 32), cv::Scalar(0, 0, 0, 16));
            beginPath();
            ellipse(lx + 3.0f, ly + 16.0f, ex, ey);
            ellipse(rx + 3.0f, ry + 16.0f, ex, ey);
            fillPaint(bg);
            fill();

            bg = linearGradient(x, y + h * 0.25f, x + w * 0.1f, y + h,
                    cv::Scalar(220, 220, 220, 255),
                    cv::Scalar(128, 128, 128, 255));
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
            ellipse(lx + dx, ly + dy + ey * 0.25f * (1 - blink), br,
                    br * blink);
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
            ellipse(rx + dx, ry + dy + ey * 0.25f * (1 - blink), br,
                    br * blink);
            fillColor(cv::Scalar(32, 32, 32, 255));
            fill();

            gloss = radialGradient(lx - ex * 0.25f, ly - ey * 0.5f,
                    ex * 0.1f, ex * 0.75f, cv::Scalar(255, 255, 255, 128),
                    cv::Scalar(255, 255, 255, 0));
            beginPath();
            ellipse(lx, ly, ex, ey);
            fillPaint(gloss);
            fill();

            gloss = radialGradient(rx - ex * 0.25f, ry - ey * 0.5f,
                    ex * 0.1f, ex * 0.75f, cv::Scalar(255, 255, 255, 128),
                    cv::Scalar(255, 255, 255, 0));
            beginPath();
            ellipse(rx, ry, ex, ey);
            fillPaint(gloss);
            fill();
        }, sz_);
    }
};

int main() {
    cv::Rect viewport(0,0, 960, 960);
    Ptr<V4D> runtime = V4D::init(viewport, "Vector Graphics", AllocateFlags::NANOVG | AllocateFlags::IMGUI);
    Plan::run<VectorGraphicsPlan>(0);
}
```

## Code Breakdown

### 1. The `VectorGraphicsPlan`

The structure is simple: a `Plan` with all logic contained in the `infer()` method. Since there are no resources to load, the `setup()` method is not needed.

```cpp
class VectorGraphicsPlan: public Plan {
    Property<cv::Size> sz_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    void infer() override {
        // ...
    }
};
```

### 2. The `infer()` Phase and the `nvg` Context

All drawing happens inside the `infer()` method, which is executed on every frame.

```cpp
void infer() override {
    nvg([](const Size &sz) {
        using namespace cv::v4d::nvg;
        clearScreen();
        // ... drawing code ...
    }, sz_);
}
```
- **`nvg([...], sz_)`**: We open a **NanoVG context**. The lambda receives the window size (`sz`) and provides an environment where we can call NanoVG drawing functions.
- **`using namespace cv::v4d::nvg;`**: This line is important. It brings all the V4D-wrapped NanoVG functions into the current scope, allowing us to call them directly (e.g., `beginPath()` instead of `cv::v4d::nvg::beginPath()`).
- **`clearScreen()`**: A helper function to clear the screen at the beginning of each frame.

### 3. Drawing with NanoVG

The code inside the lambda uses a sequence of NanoVG calls to draw the eyes. The drawing process generally follows these steps:
1.  **`beginPath()`**: Starts a new shape path.
2.  **Define Geometry**: Create a shape using functions like `ellipse()`, `rect()`, `moveTo()`, `lineTo()`, etc.
3.  **Set Paint**: Define the color or gradient to be used for filling or stroking. You can use `fillColor()` for solid colors or functions like `linearGradient()` and `radialGradient()` to create a `Paint` object, which is then applied with `fillPaint()`.
4.  **Render**: Call `fill()` or `stroke()` to render the path to the screen.

### 4. Animation

A simple time-based animation is created to make the eyes blink.

```cpp
static long start = cv::getTickCount() / cv::getTickFrequency();
float t = cv::getTickCount() / cv::getTickFrequency() - start;
// ...
float blink = 1 - pow(sinf(t * 0.5f), 200) * 0.8f;
// ...
ellipse(..., br * blink);
```
- A `static` variable `start` records the initial time.
- On each frame, the elapsed time `t` is calculated.
- A `blink` factor is computed using a `sin` function, which creates a periodic value. The `pow` function sharpens the curve, making the blink effect quick and snappy.
- This `blink` factor is then used to scale the height of the pupil's ellipse, creating the animation.

## Summary

This tutorial covered the basics of vector drawing in Plan-V4D:
- How to use the `nvg` context to access the NanoVG API.
- The basic workflow of creating and rendering shapes.
- How to use solid colors and gradients.
- A simple technique for creating time-based animations.

In the next tutorial, we will see how to combine vector graphics with framebuffer operations.
