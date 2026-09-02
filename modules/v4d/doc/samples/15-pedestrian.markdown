# Tutorial: Pedestrian Detection and Tracking Demo

This tutorial breaks down a classic computer vision application: detecting an object in a video and then tracking it frame-by-frame. We will implement a "detect-then-track" strategy for finding pedestrians, a common and efficient pattern for real-time object tracking.

This demo showcases how to structure a complex computer vision pipeline in Plan-V4D:

- Integrating standard OpenCV algorithms like HOG person detection and KCF tracking.
- Using a `branch` to switch between an expensive detection phase and a lightweight tracking phase.
- Organizing state and logic into multiple helper classes.
- Compositing the final tracking visualization onto the video stream.

## The Code

The source code in `modules/v4d/samples/pedestrian-demo.cpp` is a complete application. For clarity, we will focus on the overall structure and the `V4DPlan`'s `infer()` method, which orchestrates the pipeline. The low-level logic is encapsulated in helper classes (`HOG`, `Tracking`, `ObjectMarker`, `NonMaxSupression`).

```cpp
// Abridged for clarity.
class PedestrianDemoPlan : public V4DPlan {
private:
    // Structs to hold parameters, frames, and detection/tracking state.
    struct Params { /* downSize_, scale_, newTracked_ */ };
    struct Frames { /* BGRA background_, RGB/BGR video frames, GREY downsized */ };
    struct Detection {
        // detected pedestrian locations rectangles
        std::vector<cv::Rect> locations_;
        // detected pedestrian locations as boxes
        vector<vector<double>> boxes_;
        // probability of detected object being a pedestrian - currently always set to 1.0
        vector<double> probs_;
        // Faster tracking parameters
        cv::TrackerKCF::Params params_;
        // KCF tracker used instead of continuous detection
        cv::Ptr<cv::Tracker> tracker_;
        // initialize tracker only once
        bool trackerInit_ = false;
        // If tracking fails re-detect
        bool redetect_ = true;
        // Descriptor used for pedestrian detection
        cv::HOGDescriptor hog_;
    } detection_;

    inline static cv::Rect tracked_ = cv::Rect(0, 0, 0, 0);

    constexpr static auto dontRedect_ = [](const Detection& d){ return d.trackerInit_ && !d.redetect_; };
    constexpr static auto doRedect_   = [](const Detection& d){ return !d.trackerInit_ || d.redetect_; };

    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);

    class NonMaxSupression { /* ... */ };
    class HOG              { /* ... */ };
    class Tracking         { /* ... */ };
    class ObjectMarker     { /* ... */ };

public:
    void setup() override {
        // Initializes the HOG detector and KCF tracker.
        plain([](const cv::Size& sz, Detection& detection, Frames& frames, Params& params){
            detection.params_.desc_pca = cv::TrackerKCF::GRAY;
            detection.params_.compress_feature = false;
            detection.params_.compressed_size = 1;
            detection.tracker_ = cv::TrackerKCF::create(detection.params_);
            detection.hog_.setSVMDetector(cv::HOGDescriptor::getDefaultPeopleDetector());
            params.downSize_ = { sz.width / 4 , sz.height / 4 };
            params.scale_    = { 4.0f, 4.0f };
            frames.videoFrame_.create(sz, CV_8UC4);
            frames.videoFrameBGR_.create(sz, CV_8UC3);
            frames.videoFrameDownGrey_.create(sz, CV_8UC1);
        }, size_, RW(detection_), RW(frames_), RW(params_));
    }

    void infer() override {
        capture(RW(frames_.videoFrame_));

        // Prepare frames (convert color, downsize).
        plain(cv::cvtColor, R(frames_.videoFrame_), RW(frames_.videoFrameBGR_), V(cv::COLOR_BGRA2RGB), V(0), V(cv::ALGO_HINT_DEFAULT))
        ->plain(prepare_frames, R(params_), RW(frames_));

        // Branch: either detect (HOG) or track (KCF).
        branch(doRedect_, R(detection_))
            ->plain(&HOG::detect, R(hog), R(frames_.videoFrameDownGrey_), RW(detection_), RW(nms), RW(params_))
        ->elseBranch()
            ->plain(&Tracking::perform, R(tracking), R(frames_.videoFrameDownGrey_), RW(detection_), RW(params_), CS(tracked_))
        ->endBranch();

        // Smooth the tracked box, draw it, and composite with the background.
        plain(&Tracking::save, R(tracking), R(params_), size_, RWS(tracked_))
        ->nvg(&ObjectMarker::draw, R(marker_), size_, R(params_), CS(tracked_))
        ->fb(present, R(frames_.background_));

        write();
    }
};
```

## Code Breakdown

### 1. The "Detect-then-Track" Strategy

Running an object detector like HOG on every single frame can be computationally expensive. A more efficient strategy is:

1. **Detect**: Run the expensive detector once to find the object of interest.
2. **Track**: Once found, initialize a lightweight tracker (like KCF) with the object's location.
3. **Update**: On subsequent frames, just run the fast tracker to update the object's position.
4. **Re-detect**: If the tracker loses the object, go back to step 1 and run the full detector again.

### 2. Implementing the Strategy in a `V4DPlan`

The `infer()` method implements this logic perfectly using a `branch`.

```cpp
// Branch condition functions.
constexpr static auto dontRedect_ = [](const Detection& d){ return d.trackerInit_ && !d.redetect_; };
constexpr static auto doRedect_   = [](const Detection& d){ return !d.trackerInit_ || d.redetect_; };

// ... inside infer() ...
branch(doRedect_, R(detection_))
    ->plain(&HOG::detect, …)
->elseBranch()
    ->plain(&Tracking::perform, …)
->endBranch();
```

- **State Flags**: The `Detection` struct holds two key boolean flags: `trackerInit_` (has the tracker been initialized?) and `redetect_` (has the tracker lost the object?).
- **Branch Condition**: The `branch` uses the `doRedect_` lambda as its condition. This function returns `true` if we need to run the detector (either because the tracker was never initialized or because it has failed).
- **The `if` Block**: If `doRedect_` is true, the first `plain` context is executed, which calls our `HOG::detect` method to find a pedestrian.
- **The `else` Block**: If `doRedect_` is false (meaning we have a successfully initialized and running tracker), the `elseBranch()` is taken. This executes a different `plain` context that calls our `Tracking::perform` method, which is much faster.

### 3. Visualization

After the detection/tracking logic, a final chain of contexts is used to visualize the result.

```cpp
plain(&Tracking::save, …)
    ->nvg(&ObjectMarker::draw, …)
    ->fb(present, R(frames_.background_));
```

1. **`Tracking::save`**: A `plain` context that applies some smoothing to the tracked bounding box to prevent jitter.
2. **`ObjectMarker::draw`**: An `nvg` context that draws a simple ellipse around the smoothed bounding box.
3. **`present`**: An `fb` context that composites the drawing with the original video frame for display.

## Summary

This demo is a powerful example of how to structure a real-world CV application in Plan-V4D.

- Complex logic can be cleanly organized into helper classes.
- The `branch`/`elseBranch` statement is perfect for implementing state-based control flow, like the "detect-then-track" pattern.
- You can seamlessly integrate standard OpenCV algorithms (`HOGDescriptor`, `TrackerKCF`) into a high-performance pipeline.

Next, we will look at an optical flow demo.