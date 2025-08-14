# Tutorial: Simple Video Editing

Plan-V4D provides a simple yet powerful source/sink architecture for building video processing pipelines. This allows you to read from a video source (like a file or a camera), perform processing and rendering on each frame, and then write the result to a video sink (like an output file or a display window).

This tutorial demonstrates how to build a simple video editor that reads from a video file, renders text on top of each frame, and saves the result to a new video file.

## The Code

Here is the complete source code for this example. You can find it in `samples/video_editing.cpp`.

```cpp
#include <opencv2/v4d/v4d.hpp>

using namespace cv;
using namespace cv::v4d;

class VideoEditingPlan : public Plan {
    const string hv_ = "Hello Video!";
    Property<cv::Size> sz_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    void infer() override {
        // 1. Capture a frame from the source and load it into the framebuffer
        capture();

        // 2. Render text on top of the captured frame
        nvg([](const Size& sz, const string& str) {
            using namespace cv::v4d::nvg;

            fontSize(40.0f);
            fontFace("sans-bold");
            fillColor(Scalar(255, 0, 0, 255));
            textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            text(sz.width / 2.0, sz.height / 2.0, str.c_str(), str.c_str() + str.size());
        }, sz_, R(hv_));

        // 3. Write the modified framebuffer to the sink
        write();
    }
};

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: video_editing <input-video-file> <output-video-file>" << std::endl;
        exit(1);
    }
    cv::Rect viewport(0, 0, 960, 960);
    Ptr<V4D> runtime = V4D::init(viewport, "Video Editing", AllocateFlags::NANOVG | AllocateFlags::IMGUI);

    // Make the video source from the first command-line argument
    auto src = Source::make(runtime, argv[1]);

    // Make the video sink for the second command-line argument
    auto sink = Sink::make(runtime, argv[2], src->fps(), viewport.size());

    // Attach the source and sink to the runtime
    runtime->setSource(src);
    runtime->setSink(sink);

    Plan::run<VideoEditingPlan>(0);
}
```

## Code Breakdown

### 1. The `main()` Function: Setting up the Pipeline

The `main` function is responsible for setting up the I/O for our pipeline.

```cpp
int main(int argc, char** argv) {
    // ... argument checking ...
    Ptr<V4D> runtime = V4D::init(...);

    // Create a Source from the input file
    auto src = Source::make(runtime, argv[1]);

    // Create a Sink for the output file
    auto sink = Sink::make(runtime, argv[2], src->fps(), viewport.size());

    // Attach them to the runtime
    runtime->setSource(src);
    runtime->setSink(sink);

    Plan::run<VideoEditingPlan>(0);
}
```
- **`Source::make(...)`**: We create a `Source` object, passing it the V4D runtime and the path to the input video file (`argv[1]`). V4D handles the complexities of video decoding.
- **`Sink::make(...)`**: We create a `Sink` object, providing the runtime, the output file path (`argv[2]`), the desired frames-per-second (which we get from the source), and the frame size. V4D handles the video encoding.
- **`runtime->setSource(src)`** and **`runtime->setSink(sink)`**: We attach these objects to the runtime. This makes them available to be used by special commands within a `Plan`.

### 2. The `infer()` Method: The Processing Pipeline

The `infer()` method now defines the steps to be performed on each frame of the video.

```cpp
void infer() override {
    // Step 1: Get a frame
    capture();

    // Step 2: Process/render on the frame
    nvg(...);

    // Step 3: Save the frame
    write();
}
```
- **`capture()`**: This special function, available within a `Plan`, interacts with the `Source` attached to the runtime. It decodes one frame from the input video and places it into the main framebuffer, making it ready for processing or rendering.
- **`nvg(...)`**: This is the same NanoVG context from our previous tutorials. However, because `capture()` was just called, the framebuffer is not empty. The text is now rendered *on top of* the video frame.
- **`write()`**: This function interacts with the `Sink`. It takes the current state of the framebuffer (which now contains the video frame with text rendered on it) and sends it to the video encoder to be written to the output file.

This simple, three-step process forms a complete video editing pipeline.

## Summary

This tutorial introduced the powerful source and sink system in Plan-V4D.
- **`Source`** and **`Sink`** objects handle video decoding and encoding.
- They are attached to the V4D runtime to be used by a `Plan`.
- The **`capture()`** function reads a frame from the source into the framebuffer.
- The **`write()`** function writes the framebuffer's content to the sink.
- By sequencing `capture()`, rendering contexts, and `write()`, you can create elegant and efficient video processing pipelines.

In the next tutorial, we will explore how to create your own custom source and sink objects.
