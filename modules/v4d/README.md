# V4D — Visualization for Video and Data

A windowed runtime for the Plan-DSL that adds:

* a GLFW + OpenGL window with an event loop,
* NanoVG and ImGui rendering contexts on top of GL,
* `Source` / `Sink` I/O for video files, webcams and user functors,
* a `V4D::Keys` property table for runtime state,
* a small set of "side-effect context" calls that route nodes to
  the right GPU/CPU pipeline (`nvg`, `fb`, `gl`, `bgfx`, `ext`).

V4D is built on top of the [Plan-DSL module](../plan/README.md).
If you haven't read the Plan-DSL guide, start there; the rest of this
README assumes you know what a `Plan` is.

## What is a V4D application?

A `V4DPlan` subclass plus a `main()` that initializes the runtime
and calls `V4DPlan::run<...>(N)`:

```cpp
#include <opencv2/v4d/v4d.hpp>

using namespace cv;
using namespace cv::v4d;

class MyPlan : public V4DPlan {
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    void infer() override {
        nvg([](const cv::Size& sz){
            using namespace cv::v4d::nvg;
            fontSize(40.0f);
            fillColor(Scalar(255, 0, 0, 255));
            textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            text(sz.width / 2.0, sz.height / 2.0, "Hello", nullptr);
        }, size_);
    }
};

int main() {
    cv::Rect viewport(0, 0, 1280, 720);
    cv::Ptr<V4D> runtime = V4D::init(viewport, "My App",
                                     AllocateFlags::NANOVG | AllocateFlags::IMGUI);
    V4DPlan::run<MyPlan>(0);
}
```

The plan has no `while` loop, no event loop, no GL boilerplate.
`V4DPlan::run<...>` spawns the worker(s), drives the frame loop,
and joins when the window is closed.

## The five context calls

V4D adds five side-effect contexts on top of the DSL's `plain(...)`:

| Call                | Context         | Purpose                                  |
|---------------------|-----------------|------------------------------------------|
| `gl(fn, args...)`   | OpenGL          | Raw GL commands on context `idx`         |
| `fb<pos>(fn, args...)` | Framebuffer  | Framebuffer access; the `UMat&` is auto-inserted at argument position `pos` |
| `nvg(fn, args...)`  | NanoVG          | Vector graphics on top of GL             |
| `bgfx(fn, args...)` | bgfx            | bgfx rendering (alternative to GL)       |
| `ext(fn, args...)`  | External        | External renderer contexts               |
| `capture()` / `capture(edge)` / `capture(fn, args)` | Source | Pull the next input frame |
| `write()` / `write(edge)` / `write(fn, args)`         | Sink   | Push the finished frame |
| `imgui(fn, args...)` | ImGui          | Install a UI node from `gui()`            |
| `set(key, edge)`     | CPU            | Property write node                      |
| `clear()`            | GL             | Clear the framebuffer to `CLEAR_COLOR`   |

A typical frame looks like:

```cpp
void infer() override {
    capture(RW(frames_.orig_));                          // pull input
    plain(prepare_frames, R(downSize_), RW(frames_));    // pre-process

    branch(RWS(params_.enabled_) = …)                    // toggle
        ->branch(!F(&Detector::detect, RW(det_), R(frames_.down_), RWS(features_)))
            ->assign(RWS(params_.state_), V(Params::NOT_DETECTED))
        ->elseBranch()
            ->subInfer(filter_)
        ->endBranch()
    ->endBranch();

    fb<1>(cv::cvtColor, R(frames_.result_),              // write framebuffer
          V(cv::COLOR_BGR2RGBA), V(0), V(cv::ALGO_HINT_DEFAULT));
    write(R(frames_.result_));                           // push to sink
}
```

## V4D-flavored lifecycle

| Method       | Where it runs                     | Notes                                    |
|--------------|-----------------------------------|------------------------------------------|
| `setup()`    | each worker, once                 | DNN load, GL resource allocation         |
| `infer()`    | each worker, recorded once, replayed every frame | the per-frame body           |
| `gui()`      | main thread, once                 | installs an ImGui node                   |
| `teardown()` | each worker, once                 | GL resource release                      |

`gui()` is special: it does not participate in the per-frame
graph. The ImGui lambda inside `imgui(...)` runs on the **main
thread** every display refresh. Mutate shared state from inside
`gui()` only through `RWS(member)` or `CS(member)`.

## Initialization

```cpp
cv::Ptr<V4D> rt = V4D::init(
    /* viewport     */ cv::Rect(0, 0, 1280, 720),
    /* window title */ "Title",
    /* subsystems   */ AllocateFlags::NANOVG | AllocateFlags::IMGUI,
    /* config       */ ConfigFlags::DEFAULT,
    /* debug        */ DebugFlags::DEFAULT,
    /* MSAA samples */ 0);
```

* **`AllocateFlags`** — which contexts to allocate (`NANOVG`,
  `IMGUI`, `BGFX`, or `NONE`). Pick what you use.
* **`ConfigFlags`** — `OFFSCREEN` (no visible window),
  `DISPLAY_MODE` (semaphore-synchronized display thread,
  required for `imshow`-style programs), `RESIZEABLE`.
* **`DebugFlags`** — `PRINT_CONTROL_FLOW`, `PRINT_LOCK_CONTENTION`,
  `MONITOR_RUNTIME_PROPERTIES`, `LOWER_WORKER_PRIORITY`,
  `DEBUG_GL_CONTEXT`.

## Sources and sinks

```cpp
auto src  = Source::make(rt, "in.mp4");
auto sink = Sink::make(rt, "out.mkv", src->fps(), viewport.size());
rt->setSource(src);
rt->setSink(sink);
```

Or build your own from a functor (see
`samples/custom_source_and_sink.cpp`):

```cpp
auto src = new Source([](cv::UMat& frame) -> bool {
    if (frame.empty()) frame.create(Size(960, 960), CV_8UC3);
    frame = convert_pix<cv::COLOR_HLS2RGB_FULL>(cv::Vec3b(hue, 128, 255));
    return true;
}, /*fps=*/60.f);
```

## Files

```
modules/v4d/
├── CMakeLists.txt
├── README.md                      ← this file
├── CMakeLists.txt
├── assets/                        ONNX / LBF model files (YuNet face detection, …)
├── doc/
│   ├── samples/                   (symlink/copy of samples, see below)
│   └── v4d-application-programming-guide.markdown
├── include/
│   └── opencv2/
│       └── v4d/
│           ├── v4d.hpp             V4D runtime, V4DPlan, Keys
│           ├── source.hpp          Source
│           ├── sink.hpp            Sink
│           ├── nvg.hpp             NanoVG C++ wrapper
│           ├── events.hpp          GLFW event helpers (Mouse, Keyboard, …)
│           ├── util.hpp            GL_CHECK, _OL_ helpers, copy_cross, …
│           └── detail/
│               ├── framebuffercontext.hpp
│               ├── glcontext.hpp
│               ├── nanovgcontext.hpp
│               ├── imguicontext.hpp
│               ├── bgfxcontext.hpp
│               ├── extcontext.hpp
│               ├── sourcecontext.hpp
│               ├── sinkcontext.hpp
│               ├── cl.hpp
│               ├── gl.hpp
│               ├── resequence.hpp
│               └── timetracker.hpp
├── samples/
│   ├── font_rendering.cpp         minimum NanoVG program
│   ├── render_opengl.cpp          minimum OpenGL program
│   ├── display_image_fb.cpp       imshow-style, direct fb access
│   ├── display_image_nvg.cpp      imshow-style, via NanoVG
│   ├── video_editing.cpp          capture → nvg → write (read this first)
│   ├── video-demo.cpp             capture → gl → write
│   ├── cube-demo.cpp              pure GL rendering
│   ├── many_cubes-demo.cpp        multiple GL contexts in parallel
│   ├── font-demo.cpp              warping + GUI + multiple sub-plans
│   ├── font_with_gui.cpp          GUI feeding NanoVG
│   ├── nanovg-demo.cpp            NanoVG showcase
│   ├── shader-demo.cpp            GLSL fragment shader on a quad
│   ├── custom_source_and_sink.cpp custom I/O + conditional writing
│   ├── montage-demo.cpp           many windows in one process
│   ├── pedestrian-demo.cpp        HOG detection + KCF tracking
│   ├── optflow-demo.cpp           Farneback optical flow
│   ├── beauty-demo.cpp            the kitchen sink (read this second)
│   └── imshow_reimplementation.cpp   full GUI image viewer
├── src/
│   ├── v4d.cpp                    V4D runtime lifecycle
│   ├── nvg.cpp                    NanoVG wrapper
│   ├── source.cpp
│   ├── sink.cpp
│   ├── util.cpp
│   ├── resequence.cpp             frame-sequencing for display mode
│   └── detail/
│       ├── nanovgcontext.cpp
│       ├── imguicontext.cpp
│       ├── …
├── third/                         third-party: glfw, nanovg, bgfx, glad, imgui
└── tools/
```

## Where to start

1. [`doc/v4d-application-programming-guide.markdown`](doc/v4d-application-programming-guide.markdown) — the V4D tutorial.
   It walks through the API and uses the samples as references.
2. [`samples/font_rendering.cpp`](samples/font_rendering.cpp) — the
   smallest program that does something visible. 32 lines.
3. [`samples/video_editing.cpp`](samples/video_editing.cpp) — the
   canonical "capture → render → write" pipeline.
4. [`samples/beauty-demo.cpp`](samples/beauty-demo.cpp) — the most
   representative real program. Shared state, sub-plans, branching
   with `IF`, mouse events, NanoVG, framebuffer writes, ImGui GUI.
5. [`samples/imshow_reimplementation.cpp`](samples/imshow_reimplementation.cpp)
   — a full GUI image viewer; a tour de force.

For the language itself (edges, operators, control flow,
properties, events), read the
[Plan-DSL guide](../plan/doc/plan-dsl-programming-guide.markdown)
and the [Plan-DSL reference](../plan/doc/plan-dsl-reference.markdown).

## Building

This is an OpenCV extra module. Build it the standard way:

```bash
mkdir build && cd build
cmake -DOPENCV_EXTRA_MODULES_PATH=../modules \
      -DBUILD_EXAMPLES=ON \
      ../..
cmake --build . --target example_v4d_video_editing
./bin/example_v4d_video_editing in.mp4 out.mkv
```

V4D requires:

* C++20
* OpenCV core, imgproc, videoio, video, plus (for the samples)
  imgcodecs, dnn, face, objdetect, tracking, optflow, plot,
  stitching, features2d, flann
* GLFW 3
* NanoVG (vendored under `third/nanovg/`)
* An OpenGL-capable driver (or OpenGL ES 3.0 if
  `OPENCV_V4D_ENABLE_ES3=ON`)
* Optionally bgfx (`OPENCV_V4D_ENABLE_BGFX=ON`)

CMake options:

| Option                          | Effect                                          |
|---------------------------------|-------------------------------------------------|
| `OPENCV_V4D_ENABLE_ES3`         | Use OpenGL ES 3.0 instead of desktop GL.        |
| `OPENCV_V4D_ENABLE_BGFX`        | Build the bgfx context and link against bgfx.   |
| `OPENCV_V4D_ENABLE_MALI`        | Mali GPU support (requires libmali).            |
| `BUILD_EXAMPLES`                | Build the programs in `samples/`.               |

## Building on macOS

V4D is a windowed, GLFW + OpenGL runtime, so on macOS you need GLFW
and must leave `OPENCV_V4D_ENABLE_ES3=OFF` (the OpenGL ES path uses
EGL, which is not available on macOS).

Requirements:

* macOS 13+ (Ventura) with Xcode 14+ (Apple Clang 14+ / libc++ 14+)
  for C++20 `<barrier>` and `<semaphore>`, and for the vendored
  third-party code.
* GLFW 3, via Homebrew: `brew install glfw`
* Homebrew's `opencv` (or build the main OpenCV tree from source).

```bash
brew install glfw
mkdir build && cd build
cmake -DOPENCV_EXTRA_MODULES_PATH=../modules \
      -DBUILD_opencv_plan=ON \
      -DBUILD_opencv_v4d=ON \
      -DBUILD_EXAMPLES=ON \
      ../..
cmake --build . --target example_v4d_video_editing
```

macOS-specific behavior and notes:

* On macOS, V4D automatically requests a desktop GL **3.2 core
  profile** window with forward compatibility (see
  `src/detail/framebuffercontext.cpp`). The `__APPLE__` code path is
  taken instead of the EGL-based ES3 branch, and `glad` loading is
  skipped because macOS exposes its own system GL function pointers.
* Apple has deprecated the desktop OpenGL API. This is harmless — V4D
  still builds and runs — but newer Xcode toolchains may emit
  deprecation warnings from the vendored GL bits.
* OpenCL/GL sharing is not exercised in the headless CI runners; if
  you rely on it, test locally on a real Mac.

macOS builds of V4D (with the samples) are verified continuously in
CI via the dedicated `macOS-ARM64-v4d` and `macOS-X64-v4d` GitHub
Actions jobs in `.github/workflows/PR-next.yaml`.

## License

Apache 2.0, like the rest of OpenCV. See the top-level
[`LICENSE`](../../LICENSE) of this repository. The third-party code
under `third/` is licensed under its own terms (see each subdir).