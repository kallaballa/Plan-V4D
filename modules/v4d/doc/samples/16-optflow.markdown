# Tutorial: Sparse Optical Flow Demo

This tutorial explores a highly advanced computer vision and visualization demo that creates a stylized representation of sparse optical flow in a video. It is a masterclass in structuring a complex, real-time application with Plan-V4D.

This demo brings together numerous advanced concepts:

- Extreme separation of concerns, with almost every logical unit in its own class.
- A multi-layer rendering pipeline using several intermediate `UMat`s.
- Integration of OpenCV's FAST feature detector and Lucas-Kanade optical flow.
- Heuristic-based scene change detection to reset the effect.
- A modular post-processing system for effects like bloom and glow.
- A comprehensive GUI to control dozens of effect parameters.

## The Code

The source code in `modules/v4d/samples/optflow-demo.cpp` is extensive. We will focus on the high-level structure of the `V4DPlan` and the `infer()` pipeline that orchestrates the various components.

The application is broken down into these key helper classes:

- **`FeaturePoints`**: Detects keypoints in a frame using `cv::FastFeatureDetector`.
- **`SceneChange`**: Determines if a drastic scene change has occurred.
- **`SparseOpticalFlow`**: Calculates optical flow between frames and uses NanoVG to render the flow vectors.
- **`BackgroundStyle`**: Applies different visual styles to the background video.
- **`PostProcessor`**: A container for post-processing effects like `GlowEffect` and `BloomEffect`.
- **`Compositor`**: The final stage that combines the background, foreground (flow vectors), and post-processing into the final image.

```cpp
// Abridged for clarity.
class OptflowDemoPlan : public V4DPlan {
private:
    // Structs for parameters and frame UMat objects.
    static struct Params { /* see sample for the many parameters */ };
    struct Frames {
        // BGRA
        cv::UMat background_, foreground_, composed_, oldForeground_;
        // GREY
        cv::UMat foregroundGrey_, prevGrey_, nextGrey_;
    } frames_;

    // Helper wrappers as graph-node functions.
    constexpr static auto UMAT_CREATE  = _OLM_(void, cv::UMat, &cv::UMat::create, cv::Size, int, cv::UMatUsageFlags);
    constexpr static auto UMAT_DIVIDE_ = _OL_(void, cv::divide, cv::InputArray, cv::InputArray, cv::OutputArray, double, int);
    constexpr static auto UMAT_COPY_TO_= _OLMC_(void, cv::UMat, &cv::UMat::copyTo, cv::OutputArray);
    constexpr static auto UMAT_RESHAPE_= _OLMC_(cv::UMat, cv::UMat, &cv::UMat::reshape, int, int);

    FeaturePoints featurePoints_;
    SceneChange sceneChange_;
    SparseOpticalFlow sparseOptflow_;
    Compositor compositor_;
    inline static vector<cv::Point2f> detectedPoints_;

    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
    Property<size_t> workerIndex_ = P<size_t>(LocalState::Keys::WORKER_INDEX);

public:
    void gui() override {
        // ... A comprehensive ImGui interface for all parameters ...
    }

    void setup() override {
        params_.size_ = V4D::get<cv::Size>(V4D::Keys::SIZE);
        // Compute kernel size from the frame diagonal, and force it odd.
        assign(RW(params_.kernelSize_), V(params_.size_.width + params_.size_.height) / V(375));
        assign(RW(params_.kernelSize_),
                IF(R(params_.kernelSize_) % V(2) == V(0),
                   R(params_.kernelSize_) + V(1),
                   R(params_.kernelSize_)));
        assign(RW(params_.maxPoints_), V(params_.size_.width + params_.size_.height) * V(100));
        // Construct the FAST detector with sensible defaults.
        construct(RW(featurePoints_), F(cv::FastFeatureDetector::create, V(10), V(false), V(cv::FastFeatureDetector::TYPE_9_16)));

        plain(UMAT_CREATE, RW(frames_.foreground_), size_, V(CV_8UC4), V(cv::USAGE_DEFAULT));
        plain(UMAT_CREATE, RW(frames_.prevGrey_),   size_, V(CV_8UC1), V(cv::USAGE_DEFAULT));
    }

    void infer() override {
        set(V4D::Keys::FULLSCREEN,  CS(params_.fullscreen_));
        set(V4D::Keys::CLEAR_COLOR, V(cv::Scalar(0, 0, 0, 0)));

        capture(RW(frames_.background_));

        plain(cv::cvtColor, R(frames_.background_),  RW(frames_.nextGrey_),      V(cv::COLOR_RGBA2GRAY), V(0), V(cv::ALGO_HINT_DEFAULT));
        plain(cv::cvtColor, R(frames_.foreground_),  RW(frames_.foregroundGrey_), V(cv::COLOR_RGBA2GRAY), V(0), V(cv::ALGO_HINT_DEFAULT));
        plain(UMAT_COPY_TO_, R(frames_.foreground_), RW(frames_.oldForeground_));

        // Detect features in the current frame.
        plain(&FeaturePoints::detect, RW(featurePoints_),
                R(frames_.nextGrey_),
                RWS(detectedPoints_)
        );

        // If no scene change, render the optical flow into the foreground UMat.
        branch(BranchType::SINGLE,
            !F(&SceneChange::detect, RW(sceneChange_),
                 RS(detectedPoints_),
                 CS(params_.sceneChangeThresh_),
                 CS(params_.sceneChangeThreshDiff_),
                 size_
            )
        )
            ->clear()
            ->nvg(&SparseOpticalFlow::visualize, RW(sparseOptflow_),
                    R(frames_.prevGrey_),
                    R(frames_.nextGrey_),
                    RS(detectedPoints_),
                    CS(params_.maxStroke_),
                    CS(params_.maxPoints_),
                    CS(params_.pointLoss_),
                    CS(params_.effectColor_)
            )
            ->fb(UMAT_COPY_TO_, RW(frames_.foreground_))
        ->endBranch();

        // Composite everything together.
        fb<4>(&Compositor::perform, RW(compositor_),
                                        R(frames_.background_),
                                        RW(frames_.oldForeground_),
                                        RW(frames_.foreground_),
                                        RW(frames_.composed_),
                                        CS(params_.backgroundMode_),
                                        CS(params_.postProcMode_),
                                        CS(params_.kernelSize_),
                                        CS(params_.gain_)
        );

        // Prepare for the next frame.
        plain(UMAT_COPY_TO_, R(frames_.nextGrey_), RW(frames_.prevGrey_));
        write(R(frames_.composed_));
    }
};
```

## Code Breakdown

### 1. A Multi-Layer Pipeline

This demo builds the final image in distinct layers, stored in `UMat` objects:

- `frames_.background_`: The raw video frame from `capture()`.
- `frames_.foreground_`: The visualization of the optical flow vectors, rendered by `SparseOpticalFlow::visualize`.
- `frames_.oldForeground_`: A copy of the previous frame's foreground, used by the `Compositor` to create a motion blur / trailing effect.
- `frames_.composed_`: The final output image after all layers and effects are combined.

### 2. The `infer()` Pipeline Explained

The `infer()` method is a sequence of carefully ordered steps:

1. **Preparation**: It captures a new frame and creates a grayscale version of it (`nextGrey_`). It also saves the previous frame's foreground visualization into `oldForeground_`.
2. **Feature Detection**: It runs the FAST feature detector on the new grayscale frame to get a set of points to track.
3. **Optical Flow Branch**: A `branch` with `BranchType::SINGLE` checks for a major scene change. If the scene is stable, it proceeds to:
    a. Clear a drawing surface.
    b. Call `SparseOpticalFlow::visualize`, which calculates the optical flow between `prevGrey_` and `nextGrey_` and uses NanoVG to draw the results.
    c. Copy the NanoVG rendering from the framebuffer into the `frames_.foreground_` `UMat`. This is the "render-to-texture" pattern.
4. **Compositing**: The `Compositor::perform` method is called. This is the final stage. It takes the background, the new foreground, and the old foreground, applies the user-selected styles and post-processing effects, and blends them all together into the final display framebuffer.
5. **State Update**: Finally, it copies `nextGrey_` to `prevGrey_` to prepare for the next frame's optical flow calculation.

## Summary

This demo is a powerful example of how to architect a complex, real-time CV application.

- **Modularity**: Breaking the problem down into many small, single-responsibility classes makes the code manageable and reusable.
- **Layer-Based Compositing**: Using intermediate `UMat`s as layers that are combined in a final step is a flexible way to build up complex visuals.
- **Performance**: The `branch` is used to skip the main visualization work if a scene change is detected, preventing visual artifacts and resetting the effect.

Next, we will look at a real-time "beauty filter" demo.