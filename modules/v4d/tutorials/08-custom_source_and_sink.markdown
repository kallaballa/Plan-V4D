# Tutorial: Custom Source and Sink

While Plan-V4D provides convenient `Source` and `Sink` objects for file I/O, you can also create your own from scratch. This gives you complete control over where your video data comes from (e.g., a procedural generator, a network stream) and where it goes (e.g., a custom analysis pipeline, multiple outputs).

This tutorial will demonstrate how to:
1.  Create a custom `Source` that procedurally generates frames of solid color, cycling through a rainbow.
2.  Create a custom `Sink` that passes frames along to a standard video file sink.
3.  Use conditional branching within a `Plan` to selectively write frames to the sink.

## The Code

Here is the complete source code for this example. You can find it in `samples/custom_source_and_sink.cpp`.

```cpp
#include <opencv2/v4d/v4d.hpp>
#include <string>

using namespace cv;
using namespace cv::v4d;

// Helper class to find pure colors and draw their names
class PureColor {
    // ... implementation ...
};

class CustomSourceAndSinkPlan : public Plan {
    PureColor finder_;
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    void infer() override {
        capture();

        // Analyze the frame to see if it's a pure color
        fb<1>(&PureColor::find, RW(finder_));
        // Draw the name of the last found color
        nvg(&PureColor::draw, R(finder_), size_);

        // Only write the frame to the sink if a pure color was found
        branch(&PureColor::found, R(finder_))
            ->write()
        ->endBranch();
    }
};

int main() {
    cv::Rect viewport(0, 0, 960, 960);
    cv::Ptr<V4D> runtime = V4D::init(viewport, "Custom Source/Sink", AllocateFlags::NANOVG | AllocateFlags::IMGUI);
    cv::Ptr<Sink> videoSink = Sink::make(runtime, "custom_source_and_sink.mkv", 10, viewport.size());

    // Make a source that generates a rainbow frames series.
    cv::Ptr<Source> src = new Source([](cv::UMat& frame){
        if(frame.empty()) {
            frame.create(Size(960, 960), CV_8UC3);
        }
        uchar hue = (int64_t(seconds() * 15) % 255);
        frame = convert_pix<cv::COLOR_HLS2RGB_FULL>(cv::Vec3b(hue, 128, 255));
        return true; // false signals end of stream
    }, 60.f);

    // Make a sink that passes the frame to the video sink
    cv::Ptr<Sink> sink = new Sink([videoSink](const uint64_t& seq, const cv::UMat& frame){
        videoSink->operator()(seq, frame);
        return  videoSink->isOpen(); // false signals a temporary error
    });

    runtime->setSource(src);
    runtime->setSink(sink);

    Plan::run<CustomSourceAndSinkPlan>(0);
}
```
*(For brevity, the implementation of the `PureColor` helper class is omitted here.)*

## Code Breakdown

### 1. Creating a Custom `Source`

You can create a `Source` by passing a lambda function to its constructor. This lambda is responsible for generating a single frame.

```cpp
cv::Ptr<Source> src = new Source([](cv::UMat& frame){
    // Initialize the frame if it's empty
    if(frame.empty()) {
        frame.create(Size(960, 960), CV_8UC3);
    }
    // Generate a color that changes over time
    uchar hue = (int64_t(seconds() * 15) % 255);
    frame = convert_pix<cv::COLOR_HLS2RGB_FULL>(cv::Vec3b(hue, 128, 255));
    // Return true to continue, false to end the stream
    return true;
}, 60.f); // The second argument is the desired FPS
```
This source generates a new solid color frame for every `capture()` call, creating an animated rainbow effect.

### 2. Creating a Custom `Sink`

A custom `Sink` is also created from a lambda. It receives the frame sequence number and the final `UMat` from the framebuffer.

```cpp
cv::Ptr<Sink> sink = new Sink([videoSink](const uint64_t& seq, const cv::UMat& frame){
    // Pass the frame on to the video file sink
    videoSink->operator()(seq, frame);
    // Return true to continue, false to signal an error
    return videoSink->isOpen();
});
```
This sink acts as a simple pass-through, forwarding any frame it receives to the `videoSink` that writes to a file. This demonstrates how you can chain sinks to create complex output behaviors.

### 3. Conditional Branching in a `Plan`

The most interesting part of the `Plan` is the conditional `write`.

```cpp
void infer() override {
    capture();

    // Analyze the frame using a helper object
    fb<1>(&PureColor::find, RW(finder_));
    // Draw info to the screen
    nvg(&PureColor::draw, R(finder_), size_);

    // Create a conditional branch in the task graph
    branch(&PureColor::found, R(finder_))
        ->write()
    ->endBranch();
}
```
- **`branch(&PureColor::found, R(finder_))`**: This is the branch condition. It calls the `found()` method of our `finder_` object. The nodes inside the branch will only execute if this method returns `true`.
- **`->write()`**: The `write()` operation is now *inside* the branch. This means it will only be called for frames where `finder_.found()` is true.
- **`->endBranch()`**: This closes the conditional block.

The result is that only frames identified as pure primary or secondary colors are written to our custom sink, and subsequently to the output video file.

## Summary

This tutorial introduced several advanced Plan-V4D concepts:
- Creating a procedural video **`Source`** from a lambda function.
- Creating a custom **`Sink`** from a lambda to define custom output behavior.
- Using **`branch()`** to create conditional logic within a `Plan`'s task graph.

In the next tutorial, we will see how to combine font rendering with a GUI.
