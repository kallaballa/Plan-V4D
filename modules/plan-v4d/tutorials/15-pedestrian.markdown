# Tutorial: Pedestrian Detection and Tracking Demo

This tutorial breaks down a classic computer vision application: detecting an object in a video and then tracking it frame-by-frame. We will implement a "detect-then-track" strategy for finding pedestrians, a common and efficient pattern for real-time object tracking.

This demo showcases how to structure a complex computer vision pipeline in Plan-V4D:
-   Integrating standard OpenCV algorithms like HOG person detection and KCF tracking.
-   Using a `branch` to switch between an expensive detection phase and a lightweight tracking phase.
-   Organizing state and logic into multiple helper classes.
-   Compositing the final tracking visualization onto the video stream.

## The Code

The source code in `samples/pedestrian-demo.cpp` is a complete application. For clarity, we will focus on the overall structure and the `Plan`'s `infer()` method, which orchestrates the pipeline. The low-level logic is encapsulated in helper classes (`HOG`, `Tracking`, `ObjectMarker`, `NonMaxSupression`).

```cpp
// Abridged for clarity
class PedestrianDemoPlan : public Plan {
private:
    // Structs to hold parameters, frames, and detection/tracking state
    struct Params { /* ... */ };
    struct Frames { /* ... */ };
    struct Detection { /* ... */ };

    // Helper classes for different logic units
    class NonMaxSupression { /* ... */ };
    class HOG { /* ... */ };
    class Tracking { /* ... */ };
    class ObjectMarker { /* ... */ };

public:
    void setup() override {
        // Initializes the HOG detector and KCF tracker
        plain([](Detection& detection, ...){
            detection.tracker_ = cv::TrackerKCF::create(...);
            detection.hog_.setSVMDetector(cv::HOGDescriptor::getDefaultPeopleDetector());
            // ...
        }, ...);
    }

    void infer() override {
        capture(RW(frames_.videoFrame_));

        // Prepare frames (convert color, downsize)
        plain(cv::cvtColor,...)
        ->plain(prepare_frames, ...);

        // Branch: either detect or track
        branch(doRedect_, R(detection_))
            // If not tracking, run HOG detector
            ->plain(&HOG::detect, ...)
        ->elseBranch()
            // If tracking, update the tracker
            ->plain(&Tracking::perform, ...)
        ->endBranch();

        // Smooth the tracked box and draw it
        plain(&Tracking::save, ...)
        ->nvg(&ObjectMarker::draw, ...)
        ->fb(present, R(frames_.background_));

        write();
    }
};
```

## Code Breakdown

### 1. The "Detect-then-Track" Strategy

Running an object detector like HOG on every single frame can be computationally expensive. A more efficient strategy is:
1.  **Detect**: Run the expensive detector once to find the object of interest.
2.  **Track**: Once found, initialize a lightweight tracker (like KCF) with the object's location.
3.  **Update**: On subsequent frames, just run the fast tracker to update the object's position.
4.  **Re-detect**: If the tracker loses the object, go back to step 1 and run the full detector again.

### 2. Implementing the Strategy in a `Plan`

The `infer()` method implements this logic perfectly using a `branch`.

```cpp
// Branch condition functions
constexpr static auto dontRedect_ = [](const Detection& d){ return d.trackerInit_ && !d.redetect_; };
constexpr static auto doRedect_ = [](const Detection& d){ return !d.trackerInit_ || d.redetect_; };

// ... inside infer() ...
branch(doRedect_, R(detection_))
    ->plain(&HOG::detect, ...)
->elseBranch()
    ->plain(&Tracking::perform, ...)
->endBranch();
```
-   **State Flags**: The `Detection` struct holds two key boolean flags: `trackerInit_` (has the tracker been initialized?) and `redetect_` (has the tracker lost the object?).
-   **Branch Condition**: The `branch` uses the `doRedect_` lambda as its condition. This function returns `true` if we need to run the detector (either because the tracker was never initialized or because it has failed).
-   **The `if` Block**: If `doRedect_` is true, the first `plain` context is executed, which calls our `HOG::detect` method to find a pedestrian.
-   **The `else` Block**: If `doRedect_` is false (meaning we have a successfully initialized and running tracker), the `elseBranch()` is taken. This executes a different `plain` context that calls our `Tracking::perform` method, which is much faster.

### 3. Visualization

After the detection/tracking logic, a final chain of contexts is used to visualize the result.

```cpp
plain(&Tracking::save, ...)
->nvg(&ObjectMarker::draw, ...)
->fb(present, R(frames_.background_));
```
1.  **`Tracking::save`**: A `plain` context that applies some smoothing to the tracked bounding box to prevent jitter.
2.  **`ObjectMarker::draw`**: An `nvg` context that draws a simple ellipse around the smoothed bounding box.
3.  **`present`**: An `fb` context that composites the drawing with the original video frame for display.

## Summary

This demo is a powerful example of how to structure a real-world CV application in Plan-V4D.
-   Complex logic can be cleanly organized into helper classes.
-   The `branch` statement is perfect for implementing state-based control flow, like the "detect-then-track" pattern.
-   You can seamlessly integrate standard OpenCV algorithms (`HOGDescriptor`, `TrackerKCF`) into a high-performance pipeline.

Next, we will look at an optical flow demo.
