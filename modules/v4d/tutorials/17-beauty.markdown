# Tutorial: Real-Time "Beauty Filter" Demo

This final tutorial explores the most architecturally complex demo: a real-time "beauty filter." This application detects facial landmarks in a video and applies different image adjustments to the skin, eyes, and lips independently, seamlessly blending them back together.

This demo introduces a powerful new concept: **sub-plans**. It showcases how to build a hierarchical application by creating a main `Plan` that orchestrates other, more specialized `Plan`s.

This demo highlights:
-   Using **sub-plans** to encapsulate and reuse complex graph logic.
-   Integrating deep learning models (for face and landmark detection) into a pipeline.
-   A multi-stage pipeline that generates masks, filters image regions, and composites a final result.
-   Using OpenCV's `MultiBandBlender` for seamless image stitching.

## The Code

The source code in `samples/beauty-demo.cpp` is extensive. It is structured into a main `BeautyDemoPlan` and two sub-plans: `FaceFeatureMasksPlan` and `BeautyFilterPlan`. We will focus on this high-level architecture.

```cpp
// Abridged for clarity

// Main plan that orchestrates everything
class BeautyDemoPlan : public Plan {
    // ... state, frames, parameters ...
    cv::Ptr<FaceFeatureMasksPlan> prepareFeatureMasksPlan_;
    cv::Ptr<BeautyFilterPlan> beautyFilterPlan_;
public:
    BeautyDemoPlan() {
        // Construct the sub-plans
        prepareFeatureMasksPlan_ = _sub<FaceFeatureMasksPlan>(this, ...);
        beautyFilterPlan_ = _sub<BeautyFilterPlan>(this, ...);
    }

    void infer() override {
        // ... capture, prepare frames ...

        // Complex branch to control logic
        branch(...)
            // If a face is detected...
            ->branch(...)
                // Execute the sub-plans in order
                ->subInfer(prepareFeatureMasksPlan_)
                ->subInfer(beautyFilterPlan_)
                // Composite the final result for display
                ->plain(compose_result, ...)
            ->endBranch()
        ->endBranch();

        // ... copy to display framebuffer and write to sink ...
    }
};

// Sub-plan to create masks from facial landmarks
class FaceFeatureMasksPlan : public Plan {
public:
    void infer() override {
        // Uses nvg and fb to render masks for the face oval, eyes, and lips
        // into UMat objects.
        // Uses plain contexts with OpenCV functions to derive a skin-only mask.
    }
};

// Sub-plan to apply the filter effects
class BeautyFilterPlan : public Plan {
public:
    void infer() override {
        // Uses plain contexts to:
        // 1. Adjust saturation/contrast of different regions.
        // 2. Use a MultiBandBlender to seamlessly stitch the modified
        //    regions back into the original image.
    }
};
```

## Code Breakdown

### 1. Sub-Plans: Hierarchical Task Graphs

The key architectural pattern here is the use of sub-plans.
-   The `BeautyDemoPlan` acts as a high-level orchestrator. Its `infer()` method defines the main application flow: capture video, detect a face, and then delegate the complex work to its children.
-   **`_sub<T>(...)`**: This function, called in the main plan's constructor, creates an instance of a sub-plan.
-   **`subInfer(...)`**: This function, called within the main plan's `infer()` method, executes the entire `infer()` graph of the specified sub-plan as if it were a single node in the main graph.

This creates a clean hierarchy. The main plan doesn't need to know the details of mask generation or image blending; it just needs to know when to execute those logical blocks of work.

### 2. The Pipeline Data Flow

The application works in a clear, multi-stage pipeline that passes data between the plans.

1.  **`BeautyDemoPlan` (Detection)**: The main plan captures a frame and runs a `FaceFeatureExtractor` (which uses a DNN model) to find facial landmarks. The result is stored in a shared `FaceFeatures` object.
2.  **`FaceFeatureMasksPlan` (Masking)**: The first sub-plan is executed. It reads the `FaceFeatures` object and uses its NanoVG drawing functions to render a series of black-and-white masks into different `UMat`s (e.g., `faceOval_`, `eyesAndLipsMaskGrey_`). This is another example of the "render-to-texture" pattern.
3.  **`BeautyFilterPlan` (Filtering & Blending)**: The second sub-plan is executed. It takes the original image and the masks as input. It uses the masks to isolate different regions of the face, applies user-configurable adjustments (e.g., boosts saturation on the lips, adjusts contrast on the skin), and then uses `cv::detail::MultiBandBlender` to seamlessly combine the modified regions back into the original image.
4.  **`BeautyDemoPlan` (Compositing)**: Control returns to the main plan, which takes the final, blended image and prepares it for display, optionally creating a side-by-side view with the original.

## Summary

This demo illustrates the pinnacle of application design in Plan-V-D.
-   **Sub-plans** allow you to break down a massive problem into a manageable hierarchy of smaller, reusable task graphs.
-   Data can be passed between plans using shared objects and `UMat`s.
-   This architecture allows for a clean separation of high-level application logic from the low-level implementation details of each processing stage.

This concludes the tutorial series. You are encouraged to explore the remaining demos to see these concepts applied in different ways.
