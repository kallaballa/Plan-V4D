# V4D — Visualization for Video and Data

A windowed runtime for the Plan-DSL that adds:

* a GLFW + OpenGL window with an event loop,
* NanoVG and ImGui rendering contexts on top of GL,
* `Source` / `Sink` I/O for video files, webcams and user functors,
* a `V4D::Keys` property table for runtime state,
* a small set of "side-effect context" calls that route nodes to
  the right GPU/CPU pipeline (`gl`, `fb`, `nvg`, `bgfx`, `ext`, `imgui`,
  `set`, `clear`).

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

`V4D::init` has three forms:

```cpp
init(const cv::Rect &viewport, const string &title, AllocateFlags, ConfigFlags, DebugFlags, int samples);
init(const cv::Rect &viewport, const cv::Size &fbsize, const string &title, /* … */);
init(const V4D &v4d, const string &title);   // clone another runtime
```

The second form names the framebuffer size separately from the window rect
(`samples/font-demo.cpp` is the one sample that uses it), and `samples` is the
MSAA sample count (0 = off, the default).

## The context calls

V4D adds a set of side-effect contexts on top of the DSL's `plain(...)`:

| Call                | Context         | Purpose                                  |
|---------------------|-----------------|------------------------------------------|
| `gl(fn, args...)`   | OpenGL          | Raw GL commands on context `idx` (`gl<-1>`, `gl<0>`, …) |
| `fb<pos>(fn, args...)` | Framebuffer  | Framebuffer access; the `UMat&` is auto-inserted at argument position `pos` |
| `nvg(fn, args...)`  | NanoVG          | Vector graphics on top of GL             |
| `bgfx(fn, args...)` | bgfx            | bgfx rendering (alternative to GL)       |
| `ext(fn, args...)`  | External        | External renderer contexts               |
| `imgui(fn, args...)` | ImGui          | Install a UI node from `gui()`           |
| `set(key, edge)`     | CPU            | Property write node (`V4D::Keys`)        |
| `clear()`            | GL             | Clear the framebuffer to `CLEAR_COLOR`   |

A context may not be nested. A context call takes a functor, sets up the
subsystem, runs the functor, and tears the subsystem down again.

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

`fb` takes the zero-based argument position at which the `cv::UMat&` is
injected. `fb(fn, a, b)` puts the framebuffer first; `fb<1>(cv::cvtColor,
R(frames_.result_), …)` puts it second, after the source image.

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

## Initialization flags

* **`AllocateFlags`** — which contexts to allocate: `NONE` (= `DEFAULT`),
  `NANOVG`, `IMGUI`, `BGFX`. Pick what you use; nothing is allocated unless you
  ask for it.
* **`ConfigFlags`** — `DEFAULT`, `OFFSCREEN` (no visible window),
  `DISPLAY_MODE` (semaphore-synchronized display thread,
  required for `imshow`-style programs), `RESIZEABLE`.
* **`DebugFlags`** — `ONSCREEN_CONTEXTS`, `PRINT_CONTROL_FLOW`,
  `DEBUG_GL_CONTEXT`, `PRINT_LOCK_CONTENTION`, `MONITOR_RUNTIME_PROPERTIES`,
  `LOWER_WORKER_PRIORITY`, `DONT_PAUSE_LOG`.

`V4D::Keys` holds the runtime properties a plan reads with
`P<T>(V4D::Keys::…)`: `SIZE`, `VIEWPORT`, `WINDOW_SIZE`, `FRAMEBUFFER_SIZE`,
`CLEAR_COLOR`, `NAMESPACE`, `FULLSCREEN`, `DISABLE_INPUT_EVENTS`, `VISIBLE`,
`AUTO_SCALE`.

## Sources and sinks

Sources and sinks are handled automatically by the runtime. When a source is
set, its frame is loaded into the framebuffer before the plan runs; when a sink
is set, the framebuffer content is written to it after the plan runs. Plans
access the frame using `fb(...)` — there is no need for explicit `capture()` or
`write()` calls.

```cpp
auto src  = Source::makeDefault(runtime, "in.mp4");
auto sink = Sink::makeDefault(runtime, "out.mkv", src->fps(), viewport.size());
runtime->setSource(src);
runtime->setSink(sink);
```

Or build your own from functors (see
[samples/custom_source_and_sink.cpp](samples/custom_source_and_sink.cpp)):

```cpp
auto src  = new Source([](cv::UMat& frame) -> bool {
    if (frame.empty()) frame.create(Size(960, 960), CV_8UC3);
    frame = convert_pix<cv::COLOR_HLS2RGB_FULL>(cv::Vec3b(hue, 128, 255));
    return true;
}, /*fps=*/60.f);

auto sink = new Sink([](const uint64_t& seqno, const cv::UMat& frame) -> bool {
    return myWriter.write(seqno, frame);
});
```

These are the constructors:

```cpp
Source(std::function<bool(cv::UMat&)> generator, float fps);
Sink(std::function<bool(const uint64_t&, const cv::UMat&)> consumer);
```

`Source::make` / `Source::makeDefault` and `Sink::make` /
`Sink::makeDefault` are the video-file wrappers — `makeDefault` resolves the
input side for the platform, so on Android it picks the live camera instead of
going through FFmpeg. A generator returning `false` marks the source closed,
which stops the automatic capture; an *empty* frame raises `End of stream`. A
consumer returning `false` does the same for the sink.

The source generator fills a `CV_8UC3` or `CV_8UC4` frame in RGB order; the
runtime resizes it (aspect ratio preserved, fitted to the viewport), flips it
vertically into OpenGL's coordinate system and converts it to BGRA before it
reaches the framebuffer. `Sink::makeDefault` takes an fps, a frame size and
optionally a FourCC.

### `capture()` and `write()` are the runtime's business

`V4DPlan` inherits `capture(...)` and `write(...)` from its source/sink
contexts. They are how the runtime emits the two I/O nodes, and
`Plan::run` calls them for you, around `infer()`:

```cpp
plan->capture();   // source → framebuffer
plan->infer();
plan->write();     // framebuffer → sink
plan->makeGraph();
```

**A plan must not call them.** Calling `capture()` records a second copy of the
automatic read, and `write()` a second copy of the automatic write, so the frame
is fetched twice per frame and the two nodes race each other. Read the frame
with `fb(UMAT_COPY_TO_, RW(member))` and write the framebuffer with
`fb<1>(cv::cvtColor, …)` instead — both are Doxygen-marked as
runtime-internal on `V4DPlan` for this reason.

## Assets

Samples and applications locate their assets (videos, models, fonts) with
`cv::samples::findFile` after registering the V4D asset directories:

```cpp
cv::v4d::add_asset_search_paths();

std::string video = cv::samples::findFile("videos/bunny.mp4");
```

The search path is a list of directories, not a single directory. It contains
the build tree (`<build>/modules/v4d/assets`, `<build>/modules/v4d/samples/data`,
`<build>/modules/v4d/samples/fonts`), the source tree (`modules/v4d/assets`,
`modules/v4d/samples/data`, `modules/v4d/samples/fonts`) and the install
directory (`share/opencv4`), in that order of priority — so the samples
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

What ships in the tree:

| Asset | Path |
|---|---|
| Sample videos | `assets/videos/` — `bunny.mp4`, `dance.mp4`, `dance2.mp4`, `kristen.mp4` |
| Sample image | `samples/data/lena.png` |
| YuNet face detector | `assets/models/face_detection_yunet_2023mar.onnx` (used by `beauty-demo`) |
| LBF facemark | `assets/models/lbfmodel.yaml` (used by `beauty-demo`) |
| MediaPipe pose models | `assets/models/pose/` — `person_detection_mediapipe_2023mar.onnx`, `pose_estimation_mediapipe_2023mar.onnx` (used by `skeletal-tracker-demo`) |
| Fonts | `samples/fonts/` — Roboto, JetBrainsMono, entypo (all OFL) |

`skeletal-tracker-demo` hard-fails with a message naming the OpenCV Zoo when
the two pose models are absent; they are checked into the tree, so a normal
checkout already has them.

## Samples

27 samples are registered as CMake targets in
[modules/v4d/CMakeLists.txt](CMakeLists.txt) (`BUILD_EXAMPLES=ON`, plus two more
behind `OPENCV_V4D_ENABLE_BGFX=ON`). All of them link `opencv_v4d`,
`opencv_plan`, core, imgproc, ximgproc, videoio, video, imgcodecs, objdetect,
xobjdetect, optflow, tracking, stitching, face, features, dnn, glfw and nanovg.

| Target | Source | What it shows |
|---|---|---|
| `example_v4d_font_rendering` | `font_rendering.cpp` | the smallest visible program |
| `example_v4d_render_opengl` | `render_opengl.cpp` | the smallest OpenGL program |
| `example_v4d_display_image_fb` | `display_image_fb.cpp` | an image through the framebuffer, no video |
| `example_v4d_display_image_nvg` | `display_image_nvg.cpp` | an image through NanoVG |
| `example_v4d_vector_graphics` | `vector_graphics.cpp` | NanoVG primitives, gradients, animation |
| `example_v4d_vector_graphics_and_fb` | `vector_graphics_and_fb.cpp` | chaining `fb` and `nvg` |
| `example_v4d_video_editing` | `video_editing.cpp` | source → nvg → sink, the canonical pipeline |
| `example_v4d_custom_source_and_sink` | `custom_source_and_sink.cpp` | custom functors for I/O, conditional output |
| `example_v4d_font_with_gui` | `font_with_gui.cpp` | an ImGui panel feeding NanoVG |
| `example_v4d_cube-demo` | `cube-demo.cpp` | pure GL, `teardown()` |
| `example_v4d_many-cubes-demo` | `many_cubes-demo.cpp` | `gl<-1>`: several GL contexts in parallel |
| `example_v4d_two-windows-demo` | `two-windows-demo.cpp` | two `V4D` runtimes and two plans in one process |
| `example_v4d_video-demo` | `video-demo.cpp` | compositing GL geometry on video |
| `example_v4d_nanovg-demo` | `nanovg-demo.cpp` | the NanoVG showcase |
| `example_v4d_shader-demo` | `shader-demo.cpp` | a GLSL fragment shader over video, with events |
| `example_v4d_font-demo` | `font-demo.cpp` | render-to-texture, warping, several sub-plans |
| `example_v4d_pedestrian-demo` | `pedestrian-demo.cpp` | HOG + NMS detection, multi-target KCF tracking, ImGui controls |
| `example_v4d_skeletal-tracker-demo` | `skeletal-tracker-demo.cpp` | MediaPipe pose: detector → per-person RoI → pose net → tracking |
| `example_v4d_optflow-demo` | `optflow-demo.cpp` | Farneback optical flow |
| `example_v4d_beauty-demo` | `beauty-demo.cpp` | the kitchen sink: shared state, sub-plans, branching, events, ImGui |
| `example_v4d_imshow_reimplementation` | `imshow_reimplementation.cpp` | a full GUI image viewer with deep zoom |
| `example_v4d_image_carousel-demo` | `image_carousel-demo.cpp` | an animated glossy carousel, `_shared`, event lists |
| `example_v4d_pipeline-demo` | `pipeline-demo.cpp` | composes five other samples as sub-plans |
| `example_v4d_montage-demo` | `montage-demo.cpp` | nine samples side by side in one window |
| `example_v4d_shadertoy-editor` | `shadertoy-editor.cpp` | an offline Shadertoy workbench with a GLSL editor |
| `example_v4d_bgfx-demo` | `bgfx-demo.cpp` | bgfx only (`OPENCV_V4D_ENABLE_BGFX=ON`) |
| `example_v4d_bgfx-demo2` | `bgfx-demo2.cpp` | bgfx over a source, with a sink and ImGui (`OPENCV_V4D_ENABLE_BGFX=ON`) |

Shared sample headers: `cubescene.hpp` (the GL cube, used by `cube-demo`,
`many_cubes-demo` and `video-demo`), `samples.hpp` (`V4D_DEMO_MAIN` and the
Android entry point), `skeletal-tracker-pipeline.hpp` and
`skeletal-tracker-anchors.hpp` (the skeletal tracker's pipeline, tracker and
compile-time anchor table), and the `shadertoy-editor/` directory holding the
editor's `shadertoy_*.hpp` model, renderer, JSON reader, syntax highlighting and
theme headers.

### Command line

Twelve samples take no arguments at all. Eleven take one optional input video,
two take an optional output video, one takes any number of image paths, one takes
a single `--no-auto-close` flag, and one (`shadertoy-editor`) has a full option
set.

| Sample | `[args]` |
|---|---|
| `font_rendering`, `render_opengl`, `cube-demo`, `many-cubes-demo`, `font-demo`, `font_with_gui`, `vector_graphics`, `vector_graphics_and_fb`, `display_image_fb`, `display_image_nvg`, `custom_source_and_sink`, `bgfx-demo` | — |
| `video-demo`, `nanovg-demo`, `shader-demo`, `beauty-demo`, `montage-demo`, `optflow-demo`, `pedestrian-demo`, `pipeline-demo`, `bgfx-demo2` | `[input-video]` (default: the bundled asset) |
| `video_editing` | `[input-video] [output-video]` (output defaults to `video_editing_out.mkv`) |
| `skeletal-tracker-demo` | `[input-video] [output-video]` (no sink unless an output is given) |
| `imshow_reimplementation` | `[image]` (default `lena.png`) |
| `image_carousel-demo` | `[image-or-dir]…` — any number; directories are scanned non-recursively |
| `two-windows-demo` | `--no-auto-close` (otherwise both windows close after 5 seconds) |
| `shadertoy-editor` | see below |

The one-video samples resolve `argv[1]` through `demo_video_input()`, which falls
back to `cv::samples::findFile()` and prints a usage line when that fails too.
`custom_source_and_sink`, `video-demo`, `montage-demo`, `bgfx-demo2` and
`shadertoy-editor` write a hard-coded or defaulted output file
(`custom_source_and_sink.mkv`, `video-demo.mkv`, `montage-demo.mkv`,
`bgfx-demo2.mkv`).

```bash
# source only
./bin/example_v4d_video-demo modules/v4d/assets/videos/bunny.mp4
# source and sink
./bin/example_v4d_video_editing in.mp4 out.mkv
# window only, no recording
./bin/example_v4d_skeletal-tracker-demo modules/v4d/assets/videos/dance.mp4
```

### Shadertoy editor

`shadertoy-editor` is a workbench for GLSL that never touches the network. It
opens and saves Shadertoy JSON — the format the site exports, so a project can
be dropped onto shadertoy.com unchanged — edits every pass in a multiline
field, recompiles as you type and puts the caret on the line the driver
complained about. Underneath, `shadertoy_renderer.hpp` renders with the real
Shadertoy semantics: per-pass framebuffers ping-ponged through the channel
graph, `common` code injected into every pass, the standard uniform set
(`iResolution`, `iTime`, `iFrame`, `iMouse`, `iDate`, …), textures loaded from
local files, and the 256x3 `iChannelKeyboard` texture.

```bash
# [options] [project.json]
./bin/example_v4d_shadertoy-editor --new feedback
./bin/example_v4d_shadertoy-editor shader.json
./bin/example_v4d_shadertoy-editor --help
```

| Option | Effect |
|---|---|
| `--new <name>` | start from a built-in sample (`gradient`, `feedback`, `keyboard`, `common`) |
| `--verify` | compile the project, print the result, exit non-zero if it does not compile; opens no window |
| `--shot <file>` | render `--frames` frames into a PNG and exit; opens no window |
| `--frames <n>` | frames to wait before the shot (default 12, minimum 1) |
| `--export <file>` | write the project back out as Shadertoy JSON |
| `--size <WxH>` | window size (default 1600x900) |
| `--fullscreen` | the render fills the window instead of being inset by the panel |

Both `--name value` and `--name=value` are accepted. An unrecognized option
prints the usage and exits 1.

The panel works on the document: Open, Save (Save as when the project has no
file yet) and Reload, a combo box of the built-in samples, reorderable per-pass
tabs, the channel table with type, filter, wrap and a local texture file, an
error list with one entry per driver message, and playback: play/pause, reset
time, resolution scale, which pass to show, mouse + keyboard feeding the
shader, and a HUD with time, frame, fps, resolution and compile time.

<kbd>Space</kbd> play, <kbd>F5</kbd> compile, <kbd>F9</kbd> save,
<kbd>R</kbd> reset the time, <kbd>P</kbd> screenshot, <kbd>Tab</kbd> the panel,
<kbd>F</kbd> fullscreen, <kbd>H</kbd> the HUD.

Two shell scripts live next to it:
`samples/shadertoy-editor/shadertoy-editor-test.sh` is the smoke test (every
built-in sample compiles, a broken shader is reported with the pass and the
line the author wrote, a project survives a round trip through `--export`, a
channel wired to a local image renders that image, and `--shot` writes a PNG
that is not a blank frame — the window screenshot is skipped when there is no
compositor to grab), and `shadertoy-text-test.sh` covers the syntax highlighter.

```bash
./modules/v4d/samples/shadertoy-editor/shadertoy-editor-test.sh
```

## Files

```
modules/v4d/
├── CMakeLists.txt                   the module, its options and every sample target
├── README.md                        ← this file
├── assets/
│   ├── models/                      YuNet, LBF facemark, MediaPipe pose models
│   └── videos/                      bunny, dance, dance2, kristen
├── doc/
│   ├── README.md                    the tutorial-mapping index
│   ├── samples/README.md            walkthroughs 00–20, code inline
│   └── v4d-application-programming-guide.markdown
├── include/
│   └── opencv2/
│       └── v4d/
│           ├── v4d.hpp              V4D runtime, V4DPlan, Keys
│           ├── source.hpp           Source
│           ├── sink.hpp             Sink
│           ├── nvg.hpp              NanoVG C++ wrapper
│           ├── events.hpp           GLFW event helpers (Mouse, Keyboard, …)
│           ├── util.hpp             GL_CHECK, copy_cross, demo_video_input, …
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
│   ├── samples.hpp                  V4D_DEMO_MAIN and the Android entry point
│   ├── cubescene.hpp                the shared OpenGL cube
│   ├── data/                        lena.png
│   ├── fonts/                       Roboto, JetBrainsMono, entypo
│   ├── skeletal-tracker-anchors.hpp the detector's compile-time anchor table
│   ├── skeletal-tracker-pipeline.hpp the tracker's pipeline and shared state
│   └── shadertoy-editor/            the Shadertoy editor's headers and scripts
├── src/
│   ├── v4d.cpp                     V4D runtime lifecycle
│   ├── nvg.cpp                     NanoVG wrapper
│   ├── source.cpp
│   ├── sink.cpp
│   ├── util.cpp
│   ├── resequence.cpp              frame-sequencing for display mode
│   └── detail/                     per-context implementations
├── third/                          glfw, glfw-android, nanovg, bgfx, glad,
│                                   imgui, imgui-color-text-edit, AnyProperty,
│                                   PerlinNoise, stb, doxygen-bootstrapped
└── tools/
    └── make-dynamic-yunet.py       regenerates the YuNet anchor table
```

## Where to start

1. [`doc/v4d-application-programming-guide.markdown`](doc/v4d-application-programming-guide.markdown) — the V4D tutorial.
   It walks through the API and uses the samples as references.
2. [`samples/font_rendering.cpp`](samples/font_rendering.cpp) — the
   smallest program that does something visible.
3. [`samples/video_editing.cpp`](samples/video_editing.cpp) — the
   canonical "source → render → sink" pipeline.
4. [`samples/pedestrian-demo.cpp`](samples/pedestrian-demo.cpp) — HOG/NMS
   detection, multi-pedestrian KCF tracking, and interactive ImGui controls.
5. [`samples/beauty-demo.cpp`](samples/beauty-demo.cpp) — the most
   representative real program. Shared state, sub-plans, branching
   with `IF`, mouse events, NanoVG, framebuffer writes, ImGui GUI.
6. [`samples/imshow_reimplementation.cpp`](samples/imshow_reimplementation.cpp)
   — a full GUI image viewer; a tour de force.
7. [`samples/image_carousel-demo.cpp`](samples/image_carousel-demo.cpp) — animated
   glossy cards with reflections, keyboard/mouse navigation, and an ImGui HUD.
8. [`samples/skeletal-tracker-demo.cpp`](samples/skeletal-tracker-demo.cpp) — a
   complete vision pipeline: DNN person detection, per-person rotated crops, a
   pose network, multi-person tracking with 1 Euro smoothing, and per-stage
   timings, with the pipeline split out into
   [`samples/skeletal-tracker-pipeline.hpp`](samples/skeletal-tracker-pipeline.hpp).

For the language itself (edges, operators, control flow,
properties, events), read the
[Plan-DSL guide](../plan/doc/plan-dsl-programming-guide.markdown)
and the [Plan-DSL reference](../plan/doc/plan-dsl-reference.markdown).

## Building

From the repository root:

```bash
./build.sh plan+v4d -b release      # configure, build and install
./build.sh build -j 16             # compile an already-configured tree
```

Or build it the standard OpenCV way:

```bash
mkdir build && cd build
cmake -DOPENCV_EXTRA_MODULES_PATH=../modules \
      -DBUILD_EXAMPLES=ON \
      ../..
cmake --build . --target example_v4d_video_editing
./bin/example_v4d_video_editing in.mp4 out.mkv
```

`./build.sh android` cross-compiles the same demos with the NDK and
`--apk` assembles a signed APK per demo.

GLFW is vendored: `third/glfw` is a submodule, built together with the module
and installed next to `libnanovg.so`, so no GLFW package is needed. Its
include, library and pkg-config files land in `${prefix}/include/GLFW` and
`${prefix}/lib`/`${prefix}/lib64`. Configure with `-DOPENCV_V4D_USE_SYSTEM_GLFW=ON`
to link a system GLFW instead. Wayland support in the vendored copy follows
OpenCV's `-DWITH_WAYLAND`; X11 support needs the usual X11 development files.

Requirements:

* C++20
* OpenCV core, imgproc, videoio, video, ximgproc, and (for the samples)
  imgcodecs, dnn, geometry, face, objdetect, xobjdetect, tracking, optflow,
  plotting, stitching, features, flann
* An OpenGL-capable driver (or OpenGL ES 3.0 with
  `OPENCV_V4D_ENABLE_ES3=ON`)

| Option                          | Effect                                          |
|---------------------------------|-------------------------------------------------|
| `OPENCV_V4D_ENABLE_ES3`         | Use OpenGL ES 3.0 instead of desktop GL.        |
| `OPENCV_V4D_ENABLE_BGFX`        | Build the bgfx context and link against bgfx (and the two bgfx samples). |
| `OPENCV_V4D_ENABLE_MALI`        | Mali GPU support (requires libmali).            |
| `OPENCV_V4D_USE_SYSTEM_GLFW`    | Link the system GLFW instead of the vendored copy. |
| `OPENCV_V4D_SAMPLES`            | Android only: which samples to build as shared objects. |
| `BUILD_EXAMPLES`                | Build the programs in `samples/`.               |

### Building on macOS

V4D is a windowed, GLFW + OpenGL runtime, so on macOS it needs the Xcode
command line tools for the Cocoa backend and you must leave
`OPENCV_V4D_ENABLE_ES3=OFF` (the OpenGL ES path uses EGL, which is not
available on macOS). GLFW itself is vendored — see [Building](#building).

Requirements:

* macOS 13+ (Ventura) with Xcode 14+ (Apple Clang 14+ / libc++ 15+)
  for C++20 `<barrier>` and `<semaphore>`, and for the vendored
  third-party code.
* Homebrew's `opencv` (or build the main OpenCV tree from source).

```bash
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
CI via the `macOS-ARM64` and `macOS-X64` jobs in
`.github/workflows/PR-5.x.yaml`.

## License

Apache 2.0, like the rest of OpenCV. See the top-level
[`LICENSE`](../../LICENSE) of this repository. The third-party code
under `third/` is licensed under its own terms (see each subdir).