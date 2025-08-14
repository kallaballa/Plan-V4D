# Tutorial: Parallel Rendering with Multiple OpenGL Contexts

This tutorial demonstrates one of Plan-V4D's most powerful features for high-performance graphics: the ability to use multiple OpenGL contexts in parallel. By default, all `gl` calls operate on a single context. However, you can instruct V4D to create and manage a pool of contexts, allowing you to execute independent OpenGL commands simultaneously on different worker threads.

We will modify the 3D cube demo to render ten cubes at once, arranged in a circle, with each cube being rendered in its own parallel context.

## The Code

This example uses the same `cubescene.hpp` from the previous 3D cube tutorials. The main application logic is in `samples/many_cubes-demo.cpp`.

```cpp
#include <opencv2/v4d/v4d.hpp>
#include "cubescene.hpp"

using namespace cv::v4d;

class ManyCubesDemoPlan : public Plan {
    constexpr static size_t NUMBER_OF_CONTEXTS_ = 10;

    CubeScene scene_;
public:
    void setup() override {
        // Initialize the scene in each context
        for(size_t i = 0; i < NUMBER_OF_CONTEXTS_; ++i) {
            gl<-1>(V(i), &CubeScene::init, RW(scene_));
        }
    }

    void infer() override {
        set(V4D::Keys::CLEAR_COLOR, V(cv::Scalar(102, 61, 51, 255)));
        clear();
        // Render one cube in each context, arranged in a circle
        for(size_t i = 0; i < NUMBER_OF_CONTEXTS_; ++i) {
            gl<-1>(V(i),
                &CubeScene::render, R(scene_),
                    V(sin((double(i) / NUMBER_OF_CONTEXTS_) * 2.0 * CV_PI) / 1.5),
                    V(cos((double(i) / NUMBER_OF_CONTEXTS_) * 2.0 * CV_PI) / 1.5));
        }
    }

    void teardown() override {
        // Destroy the scene's resources in each context
        for(size_t i = 0; i < NUMBER_OF_CONTEXTS_; ++i) {
            gl<-1>(V(i), &CubeScene::destroy, R(scene_));
        }
    }
};

int main() {
    cv::Rect viewport(0, 0, 1920, 1080);
    cv::Ptr<V4D> runtime = V4D::init(viewport, "Many Cubes Demo", AllocateFlags::IMGUI);
    Plan::run<ManyCubesDemoPlan>(2);
    return 0;
}
```

## Code Breakdown

### 1. The Multi-Context `gl` Call

The key to this demo is a new syntax for the `gl` context call.

```cpp
gl<-1>(V(i), ...);
```
-   **`gl<-1>`**: The template argument `-1` tells V4D that we want to use one of its worker OpenGL contexts, rather than the main one.
-   **`V(i)`**: The first argument is now an integer that specifies the **index** of the worker context to use.

By looping from `i = 0` to `9` and using `V(i)` as the context index, we are creating ten separate nodes in our task graph, each targeting a different, independent OpenGL context.

### 2. Parallel Execution

-   **`setup()`**: The `for` loop in `setup` creates ten graph nodes. Each node calls `CubeScene::init` in a different context. Because these contexts are independent, the Plan engine can schedule these calls to run in parallel on multiple CPU cores, potentially speeding up initialization.
-   **`infer()`**: The loop in `infer` creates ten nodes that call `CubeScene::render`. Again, these can be executed in parallel. Each call is given a different X and Y position, calculated with `sin` and `cos`, which arranges the cubes in a circle.
-   **`teardown()`**: The cleanup calls are also parallelized.

### 3. Shared Resources

It's important to note that all ten parallel contexts are operating on the *same* `CubeScene` object.
-   In `setup`, we use `RW(scene_)` because `init()` modifies the `scene_` object by populating its OpenGL handles.
-   In `infer` and `teardown`, we use `R(scene_)` because `render()` and `destroy()` only need to read the handles that were created during setup.

The Plan engine uses these edge-calls to correctly manage access to the shared `scene_` object, ensuring that the parallel operations are thread-safe.

## Summary

This demo revealed how to unlock the parallel rendering capabilities of Plan-V4D.
-   The `gl<-1>(V(index), ...)` syntax allows you to target specific worker OpenGL contexts.
-   By creating graph nodes in different contexts, you can perform OpenGL operations in parallel, which can significantly improve performance for complex scenes.
-   The Plan engine manages access to shared resources (like our `CubeScene` object) across these parallel contexts.

Next, we will look at the final demo, which displays an image using an alternative NanoVG technique.
