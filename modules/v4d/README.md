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
| `imgui(fn, args...)` | ImGui          | Install a UI node from `gui()`            |
| `set(key, edge)`     | CPU            | Property write node                      |
| `clear()`            | GL             | Clear the framebuffer to `CLEAR_COLOR`   |

A typical frame looks like:

```cpp
void infer() override {
    fb(UMAT_COPY_TO_, RW(frames_.orig_));                // pull input
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

Sources and sinks are handled automatically by the runtime. When a source is set, its frame is loaded into the framebuffer before the plan runs; when a sink is set, the framebuffer content is written to it after the plan runs. Plans access the frame using `fb(...)` — there is no need for explicit `capture()` or `write()` calls.

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

## Assets

Samples and applications locate their assets (videos, models, fonts) with
`cv::samples::findFile` after registering the V4D asset directories:

```cpp
cv::v4d::add_asset_search_paths();

std::string video = cv::samples::findFile("videos/bunny.mp4");
```

The search path is a list of directories, not a single directory. It contains
the build tree (`<build>/modules/v4d/assets`, `<build>/modules/v4d/samples/data`),
the source tree (`modules/v4d/assets`, `modules/v4d/samples/data`) and the
install directory (`share/opencv4`), in that order of priority — so the samples
find their assets with or without `make install`. Only existing directories are
searched. Directories that do not exist at build time but appear later are still
searched for, as long as they exist at startup.

Additional directories can be given at runtime with the `V4D_ASSET_PATH`
environment variable, as a `:`-separated list (`;` on Windows). They take
precedence over the built-in list:

```bash
V4D_ASSET_PATH=~/videos:/opt/shared/assets ./bin/example_v4d_video-demo
```

`cv::v4d::asset_search_paths()` returns the effective list.

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
│   ├── video_editing.cpp          source → nvg → sink (read this first)
│   ├── video-demo.cpp             capture → gl → write
│   ├── cube-demo.cpp              pure GL rendering
│   ├── many_cubes-demo.cpp        multiple GL contexts in parallel
│   ├── font-demo.cpp              warping + GUI + multiple sub-plans
│   ├── font_with_gui.cpp          GUI feeding NanoVG
│   ├── nanovg-demo.cpp            NanoVG showcase
│   ├── shader-demo.cpp            GLSL fragment shader on a quad
│   ├── custom_source_and_sink.cpp custom I/O + conditional writing
│   ├── montage-demo.cpp           many windows in one process
│   ├── pedestrian-demo.cpp        HOG/NMS detection + KCF tracking + ImGui controls
│   ├── skeletal-tracker-demo.cpp  MediaPipe pose: detection, per-person RoI,
│   │                              pose net, multi-person tracking, 1 Euro smoothing
│   ├── skeletal-tracker-pipeline.hpp  its pipeline, tracker and shared state
│   ├── skeletal-tracker-anchors.hpp   the detector's anchors, computed
│   ├── optflow-demo.cpp           Farneback optical flow
│   ├── beauty-demo.cpp            the kitchen sink (read this second)
│   ├── imshow_reimplementation.cpp   full GUI image viewer
│   ├── image_carousel.cpp         animated glossy image carousel
│   ├── shadertoy-editor.cpp       offline Shadertoy editor: JSON projects,
│   │                              per-pass code, recompile as you type
│   ├── shadertoy_renderer.hpp     the Shadertoy render model, as plain OpenGL
│   ├── shadertoy_model.hpp        passes, channel inputs, the shader
│   ├── shadertoy_project.hpp      project files and local texture loading
│   ├── shadertoy_json.hpp         the minimal JSON reader behind that
│   └── shadertoy-editor-test.sh   compile / export / screenshot smoke test
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
    └── skeletal-tracker/      checks for the skeletal tracker demo
```

## Where to start

1. [`doc/v4d-application-programming-guide.markdown`](doc/v4d-application-programming-guide.markdown) — the V4D tutorial.
   It walks through the API and uses the samples as references.
2. [`samples/font_rendering.cpp`](samples/font_rendering.cpp) — the
   smallest program that does something visible. 32 lines.
3. [`samples/video_editing.cpp`](samples/video_editing.cpp) — the
   canonical "source → render → sink" pipeline.
4. [`samples/pedestrian-demo.cpp`](samples/pedestrian-demo.cpp) — HOG/NMS
   detection, multi-pedestrian KCF tracking, and interactive ImGui controls.
5. [`samples/beauty-demo.cpp`](samples/beauty-demo.cpp) — the most
   representative real program. Shared state, sub-plans, branching
   with `IF`, mouse events, NanoVG, framebuffer writes, ImGui GUI.
6. [`samples/imshow_reimplementation.cpp`](samples/imshow_reimplementation.cpp)
   — a full GUI image viewer; a tour de force.
7. [`samples/image_carousel.cpp`](samples/image_carousel.cpp) — animated
   glossy cards with reflections, keyboard/mouse navigation, and an ImGui HUD.
8. [`samples/skeletal-tracker-demo.cpp`](samples/skeletal-tracker-demo.cpp) — a
   complete vision pipeline: DNN person detection, per-person rotated crops, a
   pose network, multi-person tracking with 1 Euro smoothing, and per-stage
   timings; its pipeline is split into
   [`samples/skeletal-tracker-pipeline.hpp`](samples/skeletal-tracker-pipeline.hpp)
   so the [checks in `tools/skeletal-tracker/`](tools/skeletal-tracker/README.md)
   can drive it. See [Skeletal tracker](#skeletal-tracker).

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

The pedestrian demo can be built and run separately. It displays the video and
tracked pedestrian ellipses; it does not write an annotated output file:

```bash
cmake --build . --target example_v4d_pedestrian-demo
./bin/example_v4d_pedestrian-demo modules/v4d/assets/videos/dance.mp4
```

The `Tracking` ImGui window exposes the detection interval, maximum track
count, tracker refresh period, miss threshold, and smoothing parameters.

### Skeletal tracker

The skeletal tracker runs the MediaPipe pose stack on OpenCV DNN: a person
detector, then one rotated square RoI per person, then a pose network that
returns 33 BlazePose landmarks. `skeletal-tracker-anchors.hpp` is the detector's
2254 anchor table, computed at compile time from the three grids the OpenCV Zoo
model's graph defines.

The detector, the pose estimator, the tracker and the state the panel edits live
in `samples/skeletal-tracker-pipeline.hpp`; the sample itself is the plan that
drives it and the overlay that draws it.

```bash
cmake --build . --target example_v4d_skeletal-tracker-demo
# [input-video] [output-video]
./bin/example_v4d_skeletal-tracker-demo
./bin/example_v4d_skeletal-tracker-demo dance.mp4 out.mkv
```

With no arguments it plays the bundled `videos/dance.mp4` and shows a window
without recording. The output file is only written when you ask for one. The
viewport is a fixed 1920x1080, whatever the source's own resolution is, and the
recorder is written at that size too.

The pose models are downloaded by the build into
`modules/v4d/assets/models/pose/`. If they are missing, fetch them with
`make download-models`.

Press <kbd>Space</kbd> to toggle tracking. The `Skeletal Tracker` ImGui window
exposes the detector and pose confidence thresholds, the per-keypoint drawing
threshold, the RoI enlargement, the maximum number of people, how many frames a
track may coast before it is dropped, and the per-stage timings. Each person gets
a stable colour and track id, a motion trail, and a box drawn around the
skeleton; a faded skeleton is one the tracker is coasting on rather than one
measured in the current frame. `Detector RoI` additionally draws the box the
detector itself proposes, next to the skeleton box — they differ by design, the
detector's being a near-constant square around the torso.

The sample's checks live in
[`tools/skeletal-tracker/`](tools/skeletal-tracker/README.md): the anchor table,
the pose crop's geometry, and the pipeline's track, duplicate and timing
behaviour on one- and two-person clips.

### Shadertoy editor

`samples/shadertoy-editor.cpp` is a workbench for GLSL that never touches the
network. It opens and saves Shadertoy JSON — the format the site exports, so a
project can be dropped onto shadertoy.com unchanged — edits every pass in a
multiline field, recompiles as you type and puts the caret on the line the
driver complained about. Underneath, `shadertoy_renderer.hpp` renders with the
real Shadertoy semantics: per-pass framebuffers ping-ponged through the channel
graph, `common` code injected into every pass, the standard uniform set
(`iResolution`, `iTime`, `iFrame`, `iMouse`, `iDate`, …), textures loaded from
local files, and the 256x3 `iChannelKeyboard` texture.

```bash
cmake --build . --target example_v4d_shadertoy-editor
# [options] [project.json]
./bin/example_v4d_shadertoy-editor --new feedback
./bin/example_v4d_shadertoy-editor shader.json
```

`--new` starts from a built-in sample (`gradient`, `feedback`, `keyboard`,
`common`), `--size <WxH>` sets the window size, and `--export <file>` writes the
project back out as Shadertoy JSON. Two options make it usable unattended:
`--verify` compiles the project, prints the result and exits non-zero when it
does not compile, and `--shot <file>` renders `--frames` frames (12 by default)
into a PNG. Neither opens a window.

The panel works on the document: Open, Save (Save as when the project has no
file yet) and Reload, a combo box of the built-in samples, reorderable per-pass
tabs, the channel table with type, filter, wrap and a local texture file, an
error list with one entry per driver message, and playback: play/pause, reset
time, resolution scale, which pass to show, mouse + keyboard feeding the
shader, and a HUD with time, frame, fps, resolution and compile time.

<kbd>Space</kbd> play, <kbd>F5</kbd> compile, <kbd>F9</kbd> save,
<kbd>R</kbd> reset the time, <kbd>P</kbd> screenshot, <kbd>Tab</kbd> the panel,
<kbd>F</kbd> fullscreen, <kbd>H</kbd> the HUD.

`samples/shadertoy-editor-test.sh` is the smoke test: every built-in sample
compiles, a shader that does not compile is reported with the pass and the line
the author wrote, a project survives a round trip through `--export`, a channel
wired to a local image renders that image, and `--shot` writes a PNG that is not
a blank frame. It opens no socket — it needs no app key, no mock server and no
route to shadertoy.com. The window screenshot is skipped when there is no
compositor to grab:

```bash
./modules/v4d/samples/shadertoy-editor-test.sh
```

V4D requires:

* C++20
* OpenCV core, imgproc, videoio, video, plus (for the samples)
  imgcodecs, dnn, geometry, face, objdetect, tracking, optflow, plot,
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