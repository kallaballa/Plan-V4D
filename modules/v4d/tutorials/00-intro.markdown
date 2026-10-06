# V4D {#v4d}

[TOC]

|    |    |
| -: | :- |
| Original author | Amir Hassan (kallaballa) <amir@viel-zu.org> |
| Compatibility | OpenCV >= 5.0 |

# What is V4D?
V4D offers a way of writing graphical (on- and offscreen) high performance
applications with OpenCV. It is light-weight and unencumbered by QT or GTK
licenses. It features vector graphics using
[NanoVG](https://github.com/inniyah/nanovg), a GUI based on
[ImGUI](https://github.com/ocornut/imgui) and (on supported systems)
OpenCL/OpenGL and OpenCL/VAAPI interoperability. It is built on top of the
[Plan-DSL](https://github.com/kallaballa/Plan-V4D/tree/beta-5.x/modules/plan),
the type-safe dataflow language that V4D's `V4DPlan` derives from.

# Why V4D?
Please refer to the online demos in the \ref v4d_tutorials and \ref v4d_demos
section to see at a glance what it can do for you. **But note**: the online
demos are slower than native builds and are sometimes missing features. If you
want full performance (including hardware acceleration) you should really
create a native build and test it.

* **OpenGL**: Easy access to OpenGL.
* **GUI**: Simple yet powerful user interfaces through ImGui.
* **Vector graphics**: Elegant and fast vector graphics through NanoVG.
* **Font rendering**: Loading of fonts and sophisticated rendering options.
* **Video pipeline**: Through a simple source/sink system videos can be
  efficently read, displayed, edited and saved.
* **Hardware acceleration**: Transparent hardware acceleration usage where
  possible. (e.g. CL-GL interop, VAAPI and CL-VAAPI interop). Actually it is
  possible to write programs that run almost entirely on the GPU, given
  driver-features are available.
* **No more highgui** with it's heavy dependencies, licenses and limitations.
* **\ref v4d_webassembly_support**.

# Design Notes
* V4D is not thread safe. Though it is possible to have several V4D objects in
  one or more threads and synchronize them using ```V4D::init(const V4D&, ...)```
  to clone a runtime. That said, OpenCV algorithms are multi-threaded as usual,
  and Plan-DSL runs one worker per worker thread, each with its own runtime
  clone.
* V4D uses InputArray/OutputArray/InputOutputArray which gives you the option to
  work with Mat, std::vector and UMat. Anyway, you should prefer to use UMat
  whenever possible to automatically use hardware capabilities where available.
* Access to different subsystems (opengl, framebuffer, nanovg, imgui, bgfx) is
  provided through "contexts". A context is simply a function that takes a
  functor, sets up the subsystem, executes the functor and tears-down the
  subsystem.
* ```V4DPlan::run<YourPlan>(workers)``` is not a context. It is an abstraction of
  a run loop that records the plan and replays it every frame until the
  application terminates. This is necessary for portability reasons.
* Contexts ***may not*** be nested.

For example, to set the GL clear color when the runtime starts up, and clear
every frame:
@code{.cpp}
// Creates a V4D object for on screen rendering
Ptr<V4D> runtime = V4D::init(cv::Rect(0, 0, WIDTH, HEIGHT), "Blue screen",
                             AllocateFlags::IMGUI);

class BlueScreenPlan : public V4DPlan {
public:
  void setup() override {
    // "gl" is a context-call that provides OpenGL state to the node;
    // "V" is an edge-call that provides constants to the algorithm
    gl(glClearColor, V(0), V(0), V(1), V(1));
  }
  void infer() override {
    // The clear color set above is preserved between context-calls
    gl(glClear, V(GL_COLOR_BUFFER_BIT));
  }
};

// Takes care of the event loop, the workers and the frame loop
V4DPlan::run<BlueScreenPlan>(2);
@endcode

That program is `samples/render_opengl.cpp`; see \ref v4d_render_opengl.

# Input and output belong to the runtime
When a `Source` is set on the runtime, its frame is copied into the framebuffer
before the plan's graph runs; when a `Sink` is set, the framebuffer is handed to
it after. `Plan::run` emits those two nodes with the plan's own `capture()` and
`write()` calls, so **a plan never calls them itself** — it reads the frame with
`fb(...)` and draws into the framebuffer with `fb(...)` too. See
\ref v4d_display_image_pipeline and \ref v4d_video_editing.

# GPU Support
* Intel Gen 8+ (Tested: Gen 11 + Gen 13) is supported best
* NVIDIA Ada Lovelace (Tested: GTX 4070 Ti) with proprietary drivers works, but
  video writing is very slow unless: you change the codec to H264 or you create
  a gstreamer sink using nvenc.
* AMD: never tested

Note that on OpenCV 5.x the in-tree DNN engine only supports the OpenCV and CUDA
backends, so `DNN_TARGET_OPENCL` and `DNN_TARGET_VULKAN` (including
`ocl4dnn`) are silently no-ops there. The OpenVINO OpenCL GPU device V4D builds
is only reachable for models handed to OpenCV as OpenVINO IR. Plan-DSL samples
that load `.onnx` therefore run on CPU unless CUDA is built.

# Requirements
* C++20 (for `<barrier>` and `<semaphore>`)
* OpenCV 5.x
* OpenGL 3.2 Core (optionally Compat) / OpenGL ES 3.0 / WebGL2

# Optional requirements
* Support for OpenCL 1.2
* Support for cl_khr_gl_sharing and cl_intel_va_api_media_sharing OpenCL extensions.

# Dependencies
* GLFW 3 (vendored under `third/glfw`, built and installed with the module)
* NanoVG (vendored under `third/nanovg`)
* ImGui (vendored under `third/imgui`)
* GLAD (vendored under `third/glad`)
* [AnyProperty](https://github.com/kallaballa/AnyProperty) (vendored under
  `third/AnyProperty`, backs Plan-DSL's `GlobalState` / `LocalState`)
* bgfx, only with `OPENCV_V4D_ENABLE_BGFX=ON` (vendored, built through
  `third/bgfx.cmake`)

# Tutorials {#v4d_tutorials}
The tutorials are designed to be read one after the other to give you a good
overview over the key concepts of V4D. After that you can move on to the demos.
The same tutorials, with the walkthrough text and the code broken down line by
line, are in
[doc/samples/README.md](https://github.com/kallaballa/Plan-V4D/blob/beta-5.x/modules/v4d/doc/samples/README.md);
[doc/README.md](https://github.com/kallaballa/Plan-V4D/blob/beta-5.x/modules/v4d/doc/README.md)
maps the two sets onto the samples.

* \ref v4d_display_image_pipeline
* \ref v4d_display_image_fb
* \ref v4d_display_image_nvg
* \ref v4d_vector_graphics
* \ref v4d_vector_graphics_and_fb
* \ref v4d_render_opengl
* \ref v4d_font_rendering
* \ref v4d_video_editing
* \ref v4d_custom_source_and_sink
* \ref v4d_font_with_gui

# Demos {#v4d_demos}
The goal of the demos is to show how to use V4D to the fullest. Also they show
how to use V4D to create programs that run mostly (the part the matters) on the
GPU (when driver capabilities allow). They are also a good starting point for
your own applications because they touch many key aspects and algorithms of
OpenCV.

* \ref v4d_cube
* \ref v4d_many_cubes
* \ref v4d_video
* \ref v4d_nanovg
* \ref v4d_shader
* \ref v4d_font
* \ref v4d_pedestrian
* \ref v4d_optflow
* \ref v4d_beauty
* \ref v4d_image_carousel
* \ref v4d_imshow_reimplementation

# Samples without a tutorial
Five registered samples are documented only by their own comments, because they
compose other samples rather than introduce an API:

* `two-windows-demo.cpp` — two runtimes and two plans in one process
* `pipeline-demo.cpp` — five samples composed as sub-plans
* `montage-demo.cpp` — nine samples side by side in one window
* `skeletal-tracker-demo.cpp` — MediaPipe pose detection, per-person rotated
  RoIs, a pose network and multi-person tracking
* `shadertoy-editor.cpp` — an offline Shadertoy workbench with a GLSL editor

Two more, `bgfx-demo.cpp` and `bgfx-demo2.cpp`, need
`OPENCV_V4D_ENABLE_BGFX=ON`.

# Instructions for Ubuntu
You need to build OpenCV 5.x with V4D.

## Install required packages

```bash
apt install cmake make git-core build-essential pkg-config zlib1g-dev \
    libxinerama-dev libxcursor-dev libxi-dev libxrandr-dev libxext-dev \
    libx11-dev libgl1-mesa-dev libglu1-mesa-dev freeglut3-dev \
    ocl-icd-opencl-dev opencl-clhpp-headers clinfo info \
    libva-dev libva-drm2 libavcodec-dev libavdevice-dev libavfilter-dev \
    libavformat-dev libavutil-dev libpostproc-dev libswresample-dev \
    libswscale-dev doxygen ca-certificates
```

## Install if you want to build your own packages
```bash
apt install ubuntu-dev-tools dh-cmake gdebi
```

## EITHER: use the build script

`build.sh` at the repository root configures OpenCV with both modules, builds
them and installs them into `/usr/local`. It is the supported path and the one
the CI jobs use.

```bash
git clone --branch beta-5.x https://github.com/kallaballa/Plan-V4D.git
cd Plan-V4D
git submodule update --init --recursive

# The plan module and its tests
./build.sh plan -b release

# Both modules, plus every sample
./build.sh plan+v4d -b release

# Or the parts separately
./build.sh configure -t plan+v4d -b release
./build.sh build -j 16
./build.sh install
./build.sh clean
```

Run `./build.sh --help` for the full option list. `./build.sh plan` also runs the
test and perf binaries; pass `-- --gtest_filter=Plan.*` to narrow them.

## OR: build through OpenCV's CMake directly

```bash
git clone --branch beta-5.x https://github.com/kallaballa/Plan-V4D.git
cd Plan-V4D
git submodule update --init --recursive

mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCV_TRACE=OFF -DBUILD_SHARED_LIBS=ON \
      -DBUILD_opencv_plan=ON -DBUILD_opencv_v4d=ON \
      -DBUILD_EXAMPLES=ON -DBUILD_DOCS=ON \
      -DWITH_OPENGL=ON -DWITH_OPENCL=ON -DWITH_FFMPEG=ON -DWITH_GTK=OFF \
      -DOPENCV_EXTRA_MODULES_PATH=.. ..
make -j$(nproc)
sudo make install
```

## Build debian packages
```bash
cpack DEB
```

## Run the samples
```bash
# Examples — no arguments
bin/example_v4d_font_rendering
bin/example_v4d_render_opengl
bin/example_v4d_display_image_fb
bin/example_v4d_display_image_nvg
bin/example_v4d_vector_graphics
bin/example_v4d_vector_graphics_and_fb
bin/example_v4d_font_with_gui
bin/example_v4d_custom_source_and_sink

# Examples taking a video — the bundled assets are found automatically
bin/example_v4d_video_editing

# Demos
bin/example_v4d_cube-demo
bin/example_v4d_many-cubes-demo
bin/example_v4d_two-windows-demo
bin/example_v4d_video-demo modules/v4d/assets/videos/bunny.mp4
bin/example_v4d_nanovg-demo modules/v4d/assets/videos/bunny.mp4
bin/example_v4d_shader-demo modules/v4d/assets/videos/bunny.mp4
bin/example_v4d_font-demo
bin/example_v4d_pedestrian-demo modules/v4d/assets/videos/dance.mp4
bin/example_v4d_optflow-demo modules/v4d/assets/videos/dance.mp4
bin/example_v4d_skeletal-tracker-demo modules/v4d/assets/videos/dance.mp4
bin/example_v4d_beauty-demo modules/v4d/assets/videos/kristen.mp4
bin/example_v4d_imshow_reimplementation
bin/example_v4d_image_carousel-demo
bin/example_v4d_shadertoy-editor --help
bin/example_v4d_pipeline-demo modules/v4d/assets/videos/dance.mp4
bin/example_v4d_montage-demo modules/v4d/assets/videos/kristen.mp4
```

The sample videos ship in `modules/v4d/assets/videos/` (`bunny.mp4`,
`dance.mp4`, `dance2.mp4`, `kristen.mp4`), so nothing has to be downloaded. To
substitute your own, pass the path as the first argument; most samples also
accept an output file as the second.

# Attribution
* The author of the bunny video is the **Blender Foundation** ([Original video](https://www.bigbuckbunny.org)).
* The author of the dance video is **GNI Dance Company** ([Original video](https://www.youtube.com/watch?v=yg6LZtNeO_8)).
* The author of the video used in the beauty-demo video is **Kristen Leanne** ([Original video](https://www.youtube.com/watch?v=hUAT8Jm_dvw&t=11s)).
* The author of cxxpool is **Copyright (c) 2022 Christian Blume**: ([LICENSE](https://github.com/bloomen/cxxpool/blob/master/LICENSE))
