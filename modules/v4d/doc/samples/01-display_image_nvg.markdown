# Tutorial: Displaying an Image with NanoVG

This tutorial demonstrates how to load and display an image using Plan-V4D's NanoVG context. We will create a simple `V4DPlan` that loads an image during its setup phase and then renders it to the screen in a continuous loop.

## The Code

Here is the complete source code for this example. You can find it in `modules/v4d/samples/display_image_nvg.cpp`.

```cpp
#include <opencv2/v4d/v4d.hpp>
#include <opencv2/imgcodecs.hpp>

using namespace cv;
using namespace cv::v4d;

class DisplayImageNVG : public V4DPlan {
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);

    // Struct to hold image metadata and NanoVG paint object
    struct Image_t {
        std::string filename_; // Image file name
        nvg::Paint paint_;     // NanoVG paint object for the image
        int w_;                // Image width
        int h_;                // Image height
    } image_;

public:
    // Constructor to initialize the image file name
    DisplayImageNVG(const std::string& filename) {
        image_.filename_ = filename;
    }

    // Setup phase: Create the NanoVG context and load the image
    void setup() override {
        nvg([](Image_t& img) {
            using namespace cv::v4d::nvg;

            // Load the image and get a NanoVG handle
            int handle = createImage(img.filename_.c_str(), NVG_IMAGE_NEAREST);
            CV_Assert(handle > 0); // Ensure the image was loaded successfully

            // Retrieve the image dimensions
            imageSize(handle, &img.w_, &img.h_);

            // Create a NanoVG paint object using the loaded image
            img.paint_ = imagePattern(0, 0, img.w_, img.h_, 0.0f / 180.0f * NVG_PI, handle, 1.0);
        }, RW(image_)); // `RW` denotes read-write access to the shared image data
    }

    // Inference phase: Render the loaded image to the screen
    void infer() override {
        nvg([](const cv::Size& sz, const Image_t& img) {
            using namespace cv::v4d::nvg;

            beginPath();

            // Scale further rendering calls to match the viewport size
            scale(double(sz.width) / img.w_, double(sz.height) / img.h_);

            // Create a rounded rectangle matching the scaled image dimensions
            roundedRect(0, 0, img.w_, img.h_, 50);

            // Fill the rectangle with the loaded image pattern
            fillPaint(img.paint_);
            fill();
        }, size_, RW(image_)); // Pass viewport and image data to the graph node
    }
};

int main() {
    // Define the viewport dimensions
    cv::Rect viewport(0, 0, 960, 960);

    // Initialize the V4D runtime with NanoVG and IMGUI subsystems
    Ptr<V4D> runtime = V4D::init(viewport, "Display an image using NanoVG", AllocateFlags::NANOVG | AllocateFlags::IMGUI);

    // Run the Plan with the specified image file.
    // The image is resolved via OpenCV's samples finder; for non-installed builds
    // you may need to pass an absolute path.
    V4DPlan::run<DisplayImageNVG>(7, samples::findFile("lena.jpg"));
}
```

## Code Breakdown

### 1. The `DisplayImageNVG` Plan

We define a class `DisplayImageNVG` that inherits from `cv::v4d::V4DPlan` (see `v4d.hpp:422`). This class will encapsulate the logic for our image display task.

```cpp
class DisplayImageNVG : public V4DPlan {
    // ...
};
```

Inside the class, we define a `struct Image_t` to hold all the data related to our image: its filename, its dimensions, and a `nvg::Paint` object that NanoVG will use to render it.

```cpp
struct Image_t {
    std::string filename_;
    nvg::Paint paint_;
    int w_;
    int h_;
} image_;
```

### 2. The `setup()` Phase

The `setup()` method is called once when the `Plan` is initialized. This is the perfect place to load resources and perform one-time setup tasks.

```cpp
void setup() override {
    nvg([](Image_t& img) {
        using namespace cv::v4d::nvg;

        int handle = createImage(img.filename_.c_str(), NVG_IMAGE_NEAREST);
        CV_Assert(handle > 0);

        imageSize(handle, &img.w_, &img.h_);

        img.paint_ = imagePattern(0, 0, img.w_, img.h_, 0.0f / 180.0f * NVG_PI, handle, 1.0);
    }, RW(image_));
}
```

- **`nvg([…], RW(image_))`**: This is a **NanoVG context**. The lambda function passed to it will be executed within a valid NanoVG rendering environment.
- **`RW(image_)`**: This specifies that the lambda needs **read-write** access to our `image_` struct. Plan's task graph engine uses this information to manage data access and prevent race conditions.
- **`createImage(…)`**: This NanoVG function loads the image from the specified file and returns a handle to it.
- **`imagePattern(…)`**: We create a `paint` from the image. This paint can then be used to fill shapes, effectively drawing the image.

### 3. The `infer()` Phase

The `infer()` method is called repeatedly in a loop. This is where the main rendering logic goes.

```cpp
void infer() override {
    nvg([](const cv::Size& sz, const Image_t& img) {
        using namespace cv::v4d::nvg;

        beginPath();
        scale(double(sz.width) / img.w_, double(sz.height) / img.h_);
        roundedRect(0, 0, img.w_, img.h_, 50);
        fillPaint(img.paint_);
        fill();
    }, size_, RW(image_));
}
```

- **`nvg([…], size_, RW(image_))`**: Again, we use a NanoVG context. This time, we also request access to `size_`, a `Property` that holds the current dimensions of the V4D window.
- **`scale(…)`**: We scale the rendering context to make the image fit the window.
- **`roundedRect(…)`**: We create a rounded rectangle shape with the same dimensions as our image.
- **`fillPaint(img.paint_)`**: We set the fill style to our image pattern.
- **`fill()`**: We fill the rectangle, which draws the image to the screen.

### 4. The `main()` Function

The `main` function sets up the V4D runtime and executes our `Plan`.

```cpp
int main() {
    cv::Rect viewport(0, 0, 960, 960);
    Ptr<V4D> runtime = V4D::init(viewport, "Display an image using NanoVG", AllocateFlags::NANOVG | AllocateFlags::IMGUI);
    V4DPlan::run<DisplayImageNVG>(7, samples::findFile("lena.jpg"));
}
```

- **`V4D::init(…)`**: Initializes the V4D runtime with a specified window size and title. We also pass flags to enable the `NANOVG` and `IMGUI` subsystems.
- **`V4DPlan::run<DisplayImageNVG>(…)`**: This static method creates an instance of our `DisplayImageNVG` plan, passes the filename "lena.jpg" to its constructor, and starts the execution loop. The first argument (`7`) selects the worker count; `-1` resolves to a sensible default.

## Summary

In this tutorial, we've seen how to:

- Create a `V4DPlan` to structure a graphical application.
- Use the `setup()` phase for one-time resource loading.
- Use the `infer()` phase for continuous rendering.
- Utilize the `nvg` context to perform 2D drawing operations with NanoVG.
- Pass data to our rendering lambdas using Plan's property and data access system.

Next, we will explore how to use a framebuffer for offscreen rendering.