# Tutorial: Displaying an Image via Framebuffer

This tutorial explains how to display an image by writing its data directly to the window's framebuffer. This method offers a more direct way to put pixels on the screen compared to using a graphics library like NanoVG. It's particularly useful when you are already manipulating image data in `cv::Mat` or `cv::UMat` and want a straightforward way to visualize it.

## The Code

Here is the complete source code for this example. You can find it in `modules/v4d/samples/display_image_fb.cpp`.

```cpp
#include <opencv2/v4d/v4d.hpp>
#include <opencv2/imgcodecs.hpp>

using namespace cv;
using namespace cv::v4d;

class DisplayImageFB : public V4DPlan {
    UMat image_;
    UMat converted_;
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    DisplayImageFB(const string& filename) {
        // Loads an image as a UMat (just in case we have hardware acceleration available).
        imread(filename).copyTo(image_);
    }

    void setup() override {
        plain([](const cv::Size& sz, cv::UMat& image, cv::UMat& converted) {
            // We have to manually resize and color convert the image when using direct framebuffer access.
            // NOTE: imread returns BGR by default, but the existing source uses COLOR_RGB2BGRA
            // so the channels end up swapped in this minimal demo.
            resize(image, converted, sz);
            cvtColor(converted, converted, COLOR_RGB2BGRA);
        }, size_, RW(image_), RW(converted_));
    }

    void infer() override {
        // Create an fb context and copy the prepared image to the framebuffer. The fb context
        // takes care of retrieving and storing the data on the graphics card (using CL-GL
        // interop if available), ready for other contexts to use.
        fb([](UMat& framebuffer, const cv::UMat& c){
            c.copyTo(framebuffer);
        }, R(converted_));
    }
};

int main() {
    cv::Rect viewport(0, 0, 960, 960);
    // Creates a V4D object
    Ptr<V4D> runtime = V4D::init(viewport, "Display an Image through direct FB access", AllocateFlags::IMGUI, ConfigFlags::DISPLAY_MODE);
    V4DPlan::run<DisplayImageFB>(0, samples::findFile("lena.jpg"));

    return 0;
}
```

## Code Breakdown

### 1. The `DisplayImageFB` Plan

We define a `V4DPlan` named `DisplayImageFB`. It holds two `UMat` objects: `image_` for the original image and `converted_` for the version that's been resized and color-converted for display.

```cpp
class DisplayImageFB : public V4DPlan {
    UMat image_;
    UMat converted_;
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    DisplayImageFB(const string& filename) {
        // Loads an image as a UMat
        imread(filename).copyTo(image_);
    }
    // ...
};
```

In the constructor, we load the image from a file directly into `image_`. We use `UMat` to take advantage of GPU memory and hardware acceleration if available.

### 2. The `setup()` Phase

In the `setup()` phase, we prepare the image for display. The framebuffer expects image data to match the window's dimensions and to be in BGRA format.

```cpp
void setup() override {
    plain([](const cv::Size& sz, cv::UMat& image, cv::UMat& converted) {
        // We have to manually resize and color convert the image
        resize(image, converted, sz);
        cvtColor(converted, converted, COLOR_RGB2BGRA);
    }, size_, RW(image_), RW(converted_));
}
```

- **`plain([…])`**: This is a **plain context**. It's a general-purpose context for running standard CPU-side code, like OpenCV functions. It doesn't have any specific graphics state (like OpenGL or NanoVG).
- **`resize(…)`**: We resize the original image to match the window's size (`sz`).
- **`cvtColor(…)`**: We convert the image's color format to BGRA, which is what the framebuffer expects.

### 3. The `infer()` Phase

The `infer()` phase is where we copy our prepared image data to the framebuffer.

```cpp
void infer() override {
    fb([](UMat& framebuffer, const cv::UMat& c){
        c.copyTo(framebuffer);
    }, R(converted_));
}
```

- **`fb([…])`**: This is the **framebuffer context**. It provides direct write access to the window's back-buffer.
- **`UMat& framebuffer`**: The context provides a `UMat` that acts as a handle to the framebuffer.
- **`c.copyTo(framebuffer)`**: We simply copy our `converted_` image data into the framebuffer `UMat`. V4D handles the underlying details of uploading the data to the GPU.

### 4. The `main()` Function

The `main` function is slightly different from the previous tutorial.

```cpp
int main() {
    cv::Rect viewport(0, 0, 960, 960);
    Ptr<V4D> runtime = V4D::init(viewport, "Display an Image through direct FB access", AllocateFlags::IMGUI, ConfigFlags::DISPLAY_MODE);
    V4DPlan::run<DisplayImageFB>(0, samples::findFile("lena.jpg"));
    return 0;
}
```

- **`ConfigFlags::DISPLAY_MODE`**: This flag is passed to `V4D::init`. It tells V4D that we intend to use it primarily for direct framebuffer rendering, which can enable certain optimizations.

## Summary

In this tutorial, we learned how to:

- Use a **plain context** for general-purpose image processing.
- Use a **framebuffer context** to copy pixel data directly to the screen.
- Prepare a `UMat` by resizing and color-converting it for display.
- Initialize V4D in `DISPLAY_MODE` for direct framebuffer access.

Next, we will look at how to perform 2D vector drawing.