# Tutorial: Sparse Optical Flow Demo

This tutorial explores a highly advanced computer vision and visualization demo that creates a stylized representation of sparse optical flow in a video. It is a masterclass in structuring a complex, real-time application with Plan-V4D.

This demo brings together numerous advanced concepts:
-   Extreme separation of concerns, with almost every logical unit in its own class.
-   A multi-layer rendering pipeline using several intermediate `UMat`s.
-   Integration of OpenCV's FAST feature detector and Lucas-Kanade optical flow.
-   Heuristic-based scene change detection to reset the effect.
-   A modular post-processing system for effects like bloom and glow.
-   A comprehensive GUI to control dozens of effect parameters.

## The Code

The source code in `samples/optflow-demo.cpp` is extensive. We will focus on the high-level structure of the `Plan` and the `infer()` pipeline that orchestrates the various components.

The application is broken down into these key helper classes:
-   **`FeaturePoints`**: Detects keypoints in a frame using `cv::FastFeatureDetector`.
-   **`SceneChange`**: Determines if a drastic scene change has occurred.
-   **`SparseOpticalFlow`**: Calculates optical flow between frames and uses NanoVG to render the flow vectors.
-   **`BackgroundStyle`**: Applies different visual styles to the background video.
-   **`PostProcessor`**: A container for post-processing effects like `GlowEffect` and `BloomEffect`.
-   **`Compositor`**: The final stage that combines the background, foreground (flow vectors), and post-processing into the final image.

```cpp
// Abridged for clarity
class OptflowDemoPlan : public Plan {
private:
    // Structs for parameters and frame UMat objects
    static struct Params { /* ... */ };
    struct Frames { /* ... */ };

    // Instances of all the helper classes
    FeaturePoints featurePoints_;
    SceneChange sceneChange_;
    SparseOpticalFlow sparseOptflow_;
    Compositor compositor_;
    // ...
public:
    void gui() override {
        // ... A comprehensive ImGui interface for all parameters ...
    }

    void setup() override {
        // ... Initializes FAST detector and UMat objects ...
    }

    void infer() override {
        // 1. Capture video and prepare grayscale frames
        capture(RW(frames_.background_));
        plain(cv::cvtColor, R(frames_.background_), RW(frames_.nextGrey_), ...);
        plain(UMAT_COPY_TO_, R(frames_.foreground_), RW(frames_.oldForeground_));

        // 2. Detect features in the current frame
        plain(&FeaturePoints::detect, RW(featurePoints_), R(frames_.nextGrey_), RWS(detectedPoints_));

        // 3. If no scene change, render the optical flow to the foreground UMat
        branch(BranchType::SINGLE, !F(&SceneChange::detect, ...))
            ->clear() // Clear a temporary buffer
            ->nvg(&SparseOpticalFlow::visualize, ...) // Draw flow vectors
            ->fb(UMAT_COPY_TO_, RW(frames_.foreground_)) // Copy result to foreground_
        ->endBranch();

        // 4. Composite everything together
        fb<4>(&Compositor::perform, RW(compositor_), R(frames_.background_),
            RW(frames_.oldForeground_), RW(frames_.foreground_), RW(frames_.composed_), ...);

        // 5. Prepare for the next frame
        plain(UMAT_COPY_TO_, R(frames_.nextGrey_), RW(frames_.prevGrey_));
        write(R(frames_.composed_));
    }
};
```

## Code Breakdown

### 1. A Multi-Layer Pipeline

This demo builds the final image in distinct layers, stored in `UMat` objects:
-   `frames_.background_`: The raw video frame from `capture()`.
-   `frames_.foreground_`: The visualization of the optical flow vectors, rendered by `SparseOpticalFlow::visualize`.
-   `frames_.oldForeground_`: A copy of the previous frame's foreground, used by the `Compositor` to create a motion blur / trailing effect.
-   `frames_.composed_`: The final output image after all layers and effects are combined.

### 2. The `infer()` Pipeline Explained

The `infer()` method is a sequence of carefully ordered steps:

1.  **Preparation**: It captures a new frame and creates a grayscale version of it (`nextGrey_`). It also saves the previous frame's foreground visualization into `oldForeground_`.
2.  **Feature Detection**: It runs the FAST feature detector on the new grayscale frame to get a set of points to track.
3.  **Optical Flow Branch**: A `branch` checks for a major scene change. If the scene is stable, it proceeds to:
    a.  Clear a drawing surface.
    b.  Call `SparseOpticalFlow::visualize`, which calculates the optical flow between `prevGrey_` and `nextGrey_` and uses NanoVG to draw the results.
    c.  Copy the NanoVG rendering from the framebuffer into the `frames_.foreground_` `UMat`. This is the "render-to-texture" pattern.
4.  **Compositing**: The `Compositor::perform` method is called. This is the final stage. It takes the background, the new foreground, and the old foreground, applies the user-selected styles and post-processing effects, and blends them all together into the final display framebuffer.
5.  **State Update**: Finally, it copies `nextGrey_` to `prevGrey_` to prepare for the next frame's optical flow calculation.

## Summary

This demo is a powerful example of how to architect a complex, real-time CV application.
-   **Modularity**: Breaking the problem down into many small, single-responsibility classes makes the code manageable and reusable.
-   **Layer-Based Compositing**: Using intermediate `UMat`s as layers that are combined in a final step is a flexible way to build up complex visuals.
-   **Performance**: The `branch` is used to skip the main visualization work if a scene change is detected, preventing visual artifacts and resetting the effect.

Next, we will look at a real-time "beauty filter" demo.
