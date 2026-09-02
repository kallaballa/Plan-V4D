# Tutorial: Real-Time "Beauty Filter" Demo

This final tutorial explores the most architecturally complex demo: a real-time "beauty filter." This application detects facial landmarks in a video and applies different image adjustments to the skin, eyes, and lips independently, seamlessly blending them back together.

This demo introduces a powerful new concept: **sub-plans**. It showcases how to build a hierarchical application by creating a main `V4DPlan` that orchestrates other, more specialized `V4DPlan`s.

This demo highlights:

- Using **sub-plans** to encapsulate and reuse complex graph logic.
- Integrating deep learning models (for face and landmark detection) into a pipeline.
- A multi-stage pipeline that generates masks, filters image regions, and composites a final result.
- Using OpenCV's `MultiBandBlender` for seamless image stitching.

## The Code

The source code in `modules/v4d/samples/beauty-demo.cpp` is extensive. It is structured into a main `BeautyDemoPlan` and two sub-plans: `FaceFeatureMasksPlan` and `BeautyFilterPlan`. We will focus on this high-level architecture.

```cpp
// Abridged for clarity.

// Main plan that orchestrates everything.
class BeautyDemoPlan : public V4DPlan {
    // ... state, frames, parameters ...
    cv::Ptr<FaceFeatureMasksPlan> prepareFeatureMasksPlan_;
    cv::Ptr<BeautyFilterPlan> beautyFilterPlan_;
public:
    BeautyDemoPlan() {
        // Construct the sub-plans.
        prepareFeatureMasksPlan_ = _sub<FaceFeatureMasksPlan>(this, features_, frames_);
        beautyFilterPlan_       = _sub<BeautyFilterPlan>      (this, params_,   frames_);
    }

    void setup() override {
        // Derive a down-sample scale that preserves the input aspect ratio.
        assign(RW(scale_), F(aspect_preserving_scale, size_, R(downSize_)));

        // Model loading spams the log — silence it for the constructor call.
        plain(setLogLevel, V(LOG_LEVEL_WARNING))
        ->construct(RW(extractor_), R(downSize_), R(scale_))
        ->plain(setLogLevel, V(LOG_LEVEL_INFO));
    }

    void infer() override {
        set(V4D::Keys::FULLSCREEN, CS(params_.fullscreen_));

        capture(RW(frames_.orig_));
        plain(prepare_frames, R(downSize_), RW(frames_));

        // Re-detect facial landmarks every 8 frames.
        branch(RWS(params_.enabled_) = IF(
                                          F(&Mouse::List::empty, pressEvents_),
                                          CS(params_.enabled_),
                                          !CS(params_.enabled_)
                                       )
            )
            ->branch(seqCnt_ % V(uint64_t(8)) == V(uint64_t(0)))
                ->branch(!F(&FaceFeatureExtractor::extract, RW(extractor_), R(frames_.down_), RWS(features_)))
                    ->assign(RWS(params_.state_), V(Params::NOT_DETECTED))
                    ->plain(compose_result, RW(frames_), CS(params_))
                ->endBranch()
            ->endBranch()
            ->branch(!(F(&FaceFeatures::empty, RS(features_))))
                assign(RWS(params_.state_), V(Params::ON))
                // Run the sub-plans, each emitting their own nodes.
                ->subInfer(prepareFeatureMasksPlan_)
                ->subInfer(beautyFilterPlan_)
                ->plain(compose_result, RW(frames_), CS(params_))
            ->endBranch()
        ->elseBranch()
            ->plain(compose_result, RW(frames_), CS(params_))
            ->assign(RWS(params_.state_), V(Params::OFF))
        ->endBranch();

        fb<1>(cv::cvtColor, R(frames_.result_), V(cv::COLOR_BGR2RGBA), V(0), V(cv::ALGO_HINT_DEFAULT));
        write(R(frames_.result_));
    }
};

// Sub-plan to create masks from facial landmarks.
class FaceFeatureMasksPlan : public V4DPlan {
    const FaceFeatures& inputFeatures_;
    BeautyDemoPlan::Frames& inputOutputFrames_;
public:
    FaceFeatureMasksPlan(const FaceFeatures& inputFeatures, BeautyDemoPlan::Frames& inputOutputFrames) :
        inputFeatures_(inputFeatures), inputOutputFrames_(inputOutputFrames) {}

    void infer() override {
        nvg(&FaceFeatures::drawFaceOvalMask, RS(inputFeatures_))
        ->fb(cv::cvtColor, RW(inputOutputFrames_.faceOval_), V(cv::COLOR_BGRA2GRAY), V(0), V(cv::ALGO_HINT_DEFAULT))
        ->nvg(&FaceFeatures::drawEyesAndLipsMask, RS(inputFeatures_))
        ->fb(cv::cvtColor, RW(inputOutputFrames_.eyesAndLipsMaskGrey_), V(cv::COLOR_BGRA2GRAY), V(0), V(cv::ALGO_HINT_DEFAULT))
        ->plain(prepare_masks, RW(inputOutputFrames_));
    }
};

// Sub-plan to apply the filter effects.
class BeautyFilterPlan : public V4DPlan {
    const BeautyDemoPlan::Params& inputParams_;
    BeautyDemoPlan::Frames& inputOutputFrames_;
    cv::Ptr<cv::detail::MultiBandBlender> blender_ = new cv::detail::MultiBandBlender(true, 5);
    std::vector<cv::UMat> channels_;
    cv::UMat stitchedFloat_;
public:
    BeautyFilterPlan(const BeautyDemoPlan::Params& inputParams, BeautyDemoPlan::Frames& inputOutputFrames) :
        inputParams_(inputParams), inputOutputFrames_(inputOutputFrames) {}

    void infer() override {
        // 1. Adjust saturation/contrast of different regions.
        plain(adjust_face_features, RW(inputOutputFrames_), RW(channels_), CS(inputParams_))
        // 2. Use a MultiBandBlender to seamlessly stitch the modified regions back.
        ->plain(stitch_face, RW(blender_), RW(inputOutputFrames_), RW(stitchedFloat_));
    }
};
```

## Code Breakdown

### 1. Sub-Plans: Hierarchical Task Graphs

The key architectural pattern here is the use of sub-plans.

- The `BeautyDemoPlan` acts as a high-level orchestrator. Its `infer()` method defines the main application flow: capture video, detect a face, and then delegate the complex work to its children.
- **`_sub<T>(…)`**: This function, called in the main plan's constructor, creates an instance of a sub-plan (see `plan.hpp:979`).
- **`subInfer(…)`**: This function, called within the main plan's `infer()` method, executes the entire `infer()` graph of the specified sub-plan as if it were a single node in the main graph (see `v4d.hpp:531`).

This creates a clean hierarchy. The main plan doesn't need to know the details of mask generation or image blending; it just needs to know when to execute those logical blocks of work.

### 2. The Pipeline Data Flow

The application works in a clear, multi-stage pipeline that passes data between the plans.

1. **`BeautyDemoPlan` (Detection)**: The main plan captures a frame and runs a `FaceFeatureExtractor` (which uses a DNN model) to find facial landmarks. The result is stored in a shared `FaceFeatures` object. Detection runs only every 8 frames (`seqCnt_ % V(uint64_t(8)) == V(uint64_t(0))`).
2. **`FaceFeatureMasksPlan` (Masking)**: The first sub-plan is executed. It reads the `FaceFeatures` object and uses its NanoVG drawing functions to render a series of black-and-white masks into different `UMat`s (`faceOval_`, `eyesAndLipsMaskGrey_`). This is another example of the "render-to-texture" pattern.
3. **`BeautyFilterPlan` (Filtering & Blending)**: The second sub-plan is executed. It takes the original image and the masks as input. It uses the masks to isolate different regions of the face, applies user-configurable adjustments (e.g., boosts saturation on the lips, adjusts contrast on the skin), and then uses `cv::detail::MultiBandBlender` to seamlessly combine the modified regions back into the original image.
4. **`BeautyDemoPlan` (Compositing)**: Control returns to the main plan, which takes the final, blended image and prepares it for display, optionally creating a side-by-side view with the original.

This demo illustrates the pinnacle of application design in Plan-V4D.

- **Sub-plans** allow you to break down a massive problem into a manageable hierarchy of smaller, reusable task graphs.
- Data can be passed between plans using shared objects and `UMat`s.
- This architecture allows for a clean separation of high-level application logic from the low-level implementation details of each processing stage.

This concludes the tutorial series. You are encouraged to explore the remaining demos to see these concepts applied in different ways.