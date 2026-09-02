# Tutorial: Custom Source and Sink

While Plan-V4D provides convenient `Source` and `Sink` objects for file I/O, you can also create your own from scratch. This gives you complete control over where your video data comes from (e.g., a procedural generator, a network stream) and where it goes (e.g., a custom analysis pipeline, multiple outputs).

This tutorial will demonstrate how to:

1. Create a custom `Source` that procedurally generates frames of solid color, cycling through a rainbow.
2. Create a custom `Sink` that passes frames along to a standard video file sink.
3. Use conditional branching within a `V4DPlan` to selectively write frames to the sink.

## The Code

Here is the complete source code for this example. You can find it in `modules/v4d/samples/custom_source_and_sink.cpp`.

```cpp
#include <opencv2/v4d/v4d.hpp>
#include <string>

using namespace cv;
using namespace cv::v4d;

// Helper class to find pure colors and draw their names. The actual implementation
// detects the dominant color of the current frame and exposes:
//   - find(const cv::UMat&): analyzes the captured frame (sets internal state)
//   - draw(const cv::Size&):  renders a status label into the current NanoVG context
//   - found() const:          predicate used by the conditional branch below
class PureColor {
    // ... see sample for the full implementation ...
};

class CustomSourceAndSinkPlan : public V4DPlan {
    PureColor finder_;
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    void infer() override {
        capture();

        // Analyze the frame to see if it's a pure color
        fb<1>(&PureColor::find, RW(finder_));
        // Draw the name of the last found color
        nvg(&PureColor::draw, R(finder_), size_);

        // Only write the frame to the sink if a pure color was found.
        // The dynamic_cast is necessary because Plan::branch() returns
        // cv::Ptr<Plan>, but the V4D-only write() lives on V4DPlan.
        std::dynamic_pointer_cast<V4DPlan>(
            branch(&PureColor::found, R(finder_))
        )->write()->endBranch();
    }
};

int main() {
    cv::Rect viewport(0, 0, 960, 960);
    cv::Ptr<V4D> runtime = V4D::init(viewport, "Custom Source/Sink", AllocateFlags::NANOVG | AllocateFlags::IMGUI);
    // Check out the video after. It will only contain frames filtered by the plan
    // by conditional branching. The frame rate is set to one frame every 3 seconds
    // because that is what we are going to emit to the video; anyway, you may choose
    // a fps value to your own liking.
    cv::Ptr<Sink> videoSink = Sink::make(runtime, "custom_source_and_sink.mkv", 10, viewport.size());

    // Make a source that generates a rainbow frames series.
    cv::Ptr<Source> src = new Source([](cv::UMat& frame){
        // NOTE: The source frame may be generated as RGB or RGBA.

        // The source is responsible for initializing the frame.
        if(frame.empty()) {
            frame.create(Size(960, 960), CV_8UC3);
        }
        uchar hue = (int64_t(seconds() * 15) % 255);

        // Convert from HLS to RGB and set the whole frame to the RGB color
        frame = convert_pix<cv::COLOR_HLS2RGB_FULL>(cv::Vec3b(hue, 128, 255));
        return true; // false signals end of stream (fatal errors should be propagated through exceptions)
    }, 60.f);

    // Make a sink that prints the main color of the frame and passes the frame to the video sink
    cv::Ptr<Sink> sink = new Sink([videoSink](const uint64_t& seq, const cv::UMat& frame){
        // NOTE: In sinks the frame is always RGBA.

        // We could do all kinds of operations and decisions here that are based
        // on the frame, the sequence number and any hidden state the sink holds
        // (e.g. the video sink).

        // Pass the frame on to the video sink.
        videoSink->operator()(seq, frame);
        return videoSink->isOpen(); // false signals a temporary error (fatal errors should be propagated through exceptions).
    });

    // Attach source and sink
    runtime->setSource(src);
    runtime->setSink(sink);

    V4DPlan::run<CustomSourceAndSinkPlan>(0);
}
```

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

### 3. Conditional Branching in a `V4DPlan`

The most interesting part of the `V4DPlan` is the conditional `write`.

```cpp
void infer() override {
    capture();

    // Analyze the frame using a helper object
    fb<1>(&PureColor::find, RW(finder_));
    // Draw info to the screen
    nvg(&PureColor::draw, R(finder_), size_);

    // Create a conditional branch in the task graph
    std::dynamic_pointer_cast<V4DPlan>(
        branch(&PureColor::found, R(finder_))
    )->write()
     ->endBranch();
}
```

- **`branch(&PureColor::found, R(finder_))`**: This is the branch condition. It calls the `found()` method of our `finder_` object. The nodes inside the branch will only execute if this method returns `true`.
- **`->write()`**: The `write()` operation is now *inside* the branch. This means it will only be called for frames where `finder_.found()` is true.
- **`std::dynamic_pointer_cast<V4Plan>(…)`**: This is required because the base `Plan::branch` returns `cv::Ptr<Plan>`, but the `write()` graph primitive only exists on `V4DPlan`. Downcasting to `V4DPlan` re-exposes `write()` and the rest of the fluent interface.
- **`->endBranch()`**: This closes the conditional block.

The result is that only frames identified as pure primary or secondary colors are written to our custom sink, and subsequently to the output video file.

## Summary

This tutorial introduced several advanced Plan-V4D concepts:

- Creating a procedural video **`Source`** from a lambda function.
- Creating a custom **`Sink`** from a lambda to define custom output behavior.
- Using **`branch()`** to create conditional logic within a `V4DPlan`'s task graph.
- Downcasting the `Plan` returned by `branch()` to `V4DPlan` in order to keep chaining V4D-only calls (`write()`, `endBranch()`).

In the next tutorial, we will see how to combine font rendering with a GUI.