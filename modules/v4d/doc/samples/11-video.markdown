# Tutorial: Compositing 3D Graphics on Video

This tutorial demonstrates a powerful capability of Plan-V4D: compositing. We will combine the concepts from the video processing and 3D rendering tutorials to create a pipeline that reads a video, renders a rotating 3D cube on top of it, and saves the result to a new file.

## The Code

This example uses the same `cubescene.hpp` from the previous tutorial to manage the low-level OpenGL rendering. The main application logic is in `modules/v4d/samples/video-demo.cpp`.

```cpp
// modules/v4d/samples/video-demo.cpp
#include <opencv2/v4d/v4d.hpp>
#include "cubescene.hpp"

using std::cerr;
using std::endl;

using namespace cv::v4d;

class VideoDemoPlan: public V4DPlan {
private:
    CubeScene scene_;
public:
    void setup() override {
        gl(&CubeScene::init, RW(scene_));
    }

    void infer() override {
        // 1. Load video frame into the framebuffer.
        capture();
        // 2. Render the 3D cube on top of the video frame.
        //    (CubeScene::render takes two doubles; the source currently has
        //     a leftover V(false) here — pass V(0.0), V(0.0) to compile.)
        gl(&CubeScene::render, R(scene_), V(0.0), V(0.0));
        // 3. Write the composited frame to the sink.
        write();
    }

    void teardown() override {
        gl(&CubeScene::destroy, RW(scene_));
    }
};

int main(int argc, char** argv) {
    if (argc != 2) {
        cerr << "Usage: video-demo <video-file>" << endl;
        exit(1);
    }

    cv::Rect viewport(0, 0, 1280, 720);
    cv::Ptr<V4D> runtime = V4D::init(viewport, "Video Demo", AllocateFlags::IMGUI);
    auto src = Source::make(runtime, argv[1]);
    auto sink = Sink::make(runtime, "video-demo.mkv", src->fps(), viewport.size());
    runtime->setSource(src);
    runtime->setSink(sink);
    V4DPlan::run<VideoDemoPlan>(0);

    return 0;
}
```

## Code Breakdown

### 1. Setup and Teardown

The `main` function and the `V4DPlan`'s `setup()` and `teardown()` methods are very similar to our previous examples.

- `main()`: Initializes the V4D runtime and sets up a video `Source` (from a command-line argument) and a video `Sink` (to a file named `video-demo.mkv`).
- `setup()`: Calls the `init()` method of our `CubeScene` object to prepare the OpenGL resources for the cube.
- `teardown()`: Calls the `destroy()` method of our `CubeScene` object to clean up the resources.

### 2. The `infer()` Method: The Compositing Pipeline

The `infer()` method clearly shows the compositing pipeline. The operations are executed in order on each frame.

```cpp
void infer() override {
    // Step 1: Set the background
    capture();
    // Step 2: Render the foreground
    gl(&CubeScene::render, R(scene_), V(0.0), V(0.0));
    // Step 3: Output the result
    write();
}
```

- **`capture()`**: This function reads a frame from the source video file and places it in the framebuffer. At this point, the framebuffer contains the video image, which will serve as our background.
- **`gl(&CubeScene::render, …)`**: This renders the 3D cube. Crucially, we did *not* clear the screen beforehand. Therefore, the cube is drawn directly on top of the video frame that is already in the framebuffer.
- **`write()`**: This function takes the final state of the framebuffer — which now contains the video frame composited with the 3D cube — and sends it to the sink to be encoded into the output video file.

This sequential execution of contexts and commands is the foundation of building complex effects and pipelines in Plan-V4D.

## Summary

This tutorial demonstrated how to composite 3D graphics on top of a video stream.

- The order of operations in the `infer()` method defines the rendering and processing pipeline.
- `capture()` can be used to load a video frame as a background.
- Subsequent rendering calls (using `gl`, `nvg`, etc.) will draw on top of the existing framebuffer content, allowing for easy compositing.

Next, we will look at a more advanced NanoVG demo.