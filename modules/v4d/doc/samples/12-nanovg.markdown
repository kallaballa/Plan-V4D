# Tutorial: Advanced NanoVG and Processing Pipelines

This tutorial dives into a more advanced example, showcasing how to build a complex, multi-stage image processing pipeline using Plan-V4D. We will create an application that reads a video, applies a real-time, color-shifting effect to it, and then overlays a custom NanoVG widget on top.

This example highlights several advanced features:

- Chaining multiple processing steps together in a single `infer` call.
- Manipulating individual color channels of an image.
- Wrapping standard C++ functions and class methods to be used within the `V4DPlan` graph.
- Drawing complex, custom UI elements with NanoVG.

## The Code

The source code for this demo can be found in `modules/v4d/samples/nanovg-demo.cpp`. It is more complex than previous examples, so we will focus on the structure of the `V4DPlan` and its `infer` method. A helper function `draw_color_wheel` contains the detailed NanoVG drawing code and is omitted here for brevity.

```cpp
// Abridged for clarity
class NanoVGDemoPlan : public V4DPlan {
    std::vector<cv::UMat> hsvChannels_;
    cv::UMat frame_;
    cv::UMat bgra_;
    cv::UMat hsv_;
    float hue_ = 0;
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);

    // Pre-wrapped overloads so the engine can dispatch these as graph nodes.
    constexpr static auto SPLIT_ = _OL_(void, cv::split, cv::InputArray, cv::OutputArrayOfArrays);
    constexpr static auto MERGE_ = _OL_(void, cv::merge, cv::InputArrayOfArrays, cv::OutputArray);
    constexpr static auto RESIZE_VEC_ = _OLM_(void, std::vector<cv::UMat>, &std::vector<cv::UMat>::resize, size_t);

public:
    void setup() override {
        // Reserve three channel UMats so SPLIT_/MERGE_ always have valid outputs.
        plain(RESIZE_VEC_, RW(hsvChannels_), V(size_t(3)));
    }

    void infer() override {
        // 1. Capture a frame from the video source.
        capture(RW(bgra_));

        // 2. Animate a hue value over time using assign(...) and F(...).
        assign(RW(hue_), (F(&sinf, (F(&cv::getTickCount) / F(&cv::getTickFrequency)) * V(0.12) + V(1))) * V(255.0));

        // 3. BGRA -> RGB, then run the hue-shifting chain.
        plain(cv::cvtColor, R(bgra_), RW(frame_), V(cv::COLOR_BGRA2RGB), V(0), V(cv::ALGO_HINT_DEFAULT))
        ->plain(cv::cvtColor, R(frame_), RW(hsv_), V(cv::COLOR_RGB2HSV_FULL), V(0), V(cv::ALGO_HINT_DEFAULT))
        ->plain(SPLIT_, R(hsv_), RW(hsvChannels_))
        ->plain(&cv::UMat::setTo, RW(hsvChannels_[0]), F(&fmod, F(&fabs, R(hue_) - V(255)) - V(81), V(255)), V(cv::noArray()))
        ->plain(MERGE_, R(hsvChannels_), RW(hsv_))
        ->plain(cv::cvtColor, R(hsv_), RW(frame_), V(cv::COLOR_HSV2RGB_FULL), V(0), V(cv::ALGO_HINT_DEFAULT))
        ->plain(&std::vector<cv::UMat>::clear, RW(hsvChannels_));

        // 4. Push the RGB result back to the framebuffer in BGRA form.
        fb<1>(cv::cvtColor, R(frame_), V(cv::COLOR_RGB2BGRA), V(0), V(cv::ALGO_HINT_DEFAULT));

        // 5. Render the color wheel widget on top of the video.
        nvg(draw_color_wheel, size_, R(hue_));
    }
};
```

## Code Breakdown

### 1. The `infer()` Pipeline

The `infer()` method in this `V4DPlan` is a long chain of operations that execute sequentially on each frame. This forms our processing pipeline.

- **`capture(RW(bgra_))`**: Reads a frame from the video source into the `bgra_` `UMat`.
- **`assign(…)`**: This is a graph-level primitive. It evaluates the expression on the right and assigns the result to the variable on the left (`hue_`). The `F(…)` wrapper is used to treat standard functions like `sinf` and `getTickCount` as nodes in the task graph.
- **`plain(cv::cvtColor, …)`**: The first `plain` context converts the captured frame to RGB format to prepare it for HSV conversion.

### 2. The Hue-Shifting Chain

The core of the effect is a chain of `plain` context calls that manipulate the hue of the video.

```cpp
plain(cv::cvtColor, R(frame_), RW(hsv_), V(cv::COLOR_RGB2HSV_FULL), V(0), V(cv::ALGO_HINT_DEFAULT))
    ->plain(SPLIT_, R(hsv_), RW(hsvChannels_))
    ->plain(&cv::UMat::setTo, RW(hsvChannels_[0]), …)
    ->plain(MERGE_, R(hsvChannels_), RW(hsv_))
    ->plain(cv::cvtColor, R(hsv_), RW(frame_), V(cv::COLOR_HSV2RGB_FULL), V(0), V(cv::ALGO_HINT_DEFAULT));
```

1. **RGB to HSV**: The first call converts the RGB frame to the HSV (Hue, Saturation, Value) color space.
2. **Split Channels**: The `SPLIT_` operation (a pre-wrapped version of `cv::split`) separates the HSV image into three individual `UMat`s for H, S, and V.
3. **Modify Hue**: The `&cv::UMat::setTo` call is the key. It modifies the first channel (`hsvChannels_[0]`), which is the Hue channel, to a new value derived from our animated `hue_` variable.
4. **Merge Channels**: The `MERGE_` operation combines the modified Hue channel with the original Saturation and Value channels back into a single HSV image.
5. **HSV to RGB**: The final call converts the color-shifted HSV image back to RGB.

### 3. Final Rendering

After the processing chain is complete, two final steps render the output.

```cpp
fb<1>(cv::cvtColor, R(frame_), V(cv::COLOR_RGB2BGRA), V(0), V(cv::ALGO_HINT_DEFAULT));
nvg(draw_color_wheel, size_, R(hue_));
```

- **`fb<1>(…)`**: The `fb` context is used to copy the final processed `frame_` to the display. It's converted back to BGRA format, which the display expects.
- **`nvg(…)`**: The `nvg` context then draws the `draw_color_wheel` widget on top of the processed video frame.

## Summary

This advanced demo illustrates the power and expressiveness of the Plan-V4D task graph.

- Complex image processing pipelines can be constructed by chaining `plain` contexts.
- The `assign` function allows for simple variable manipulation within the graph.
- Function wrappers (`F()`, `_OL_`, `_OLM_`, etc.) allow you to integrate almost any standard function or class method into the pipeline.
- You can easily composite the results of a processing pipeline with UI elements drawn using NanoVG.

Next, we will look at a demo that uses custom shaders.