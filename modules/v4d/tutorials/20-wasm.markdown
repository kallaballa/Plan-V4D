# WebAssembly Support {#v4d_webassembly_support}

@prev_tutorial{v4d_imshow_reimplementation}
@next_tutorial{v4d}

[TOC]

|    |    |
| -: | :- |
| Original author | Amir Hassan (kallaballa) <amir@viel-zu.org> |
| Compatibility | OpenCV >= 5.0 |

# What is WebAssembly?
It is possible to compile C++ (but also other languages) for the browser. The
resulting binaries contain usually WebAssembly (WASM) which the browser knows to
execute.

# So what makes it special for OpenCV and V4D?
For OpenCV there has been the possibility to run code in the browser for a while
using [OpenCV.js](https://docs.opencv.org/4.x/d0/d84/tutorial_js_usage.html).
But OpenCV.js merely offers the OpenCV APIs and visualization and GUI has to be
done by other means (e.g. HTML5 Canvas). That is where V4D steps in because it
uses OpenGL in a fashion that translates well to [WebGL](https://en.wikipedia.org/wiki/WebGL).
V4D enables you to write graphical OpenCV applications that run native as well
as in the browser.

# Status

**The instructions below are the historical ones, and they no longer match this
tree.** They target OpenCV 4.x, a standalone V4D repository and an Emscripten
build driven by a hand-written `emcmake` line. Three things changed:

* V4D is now the `v4d` module of this repository, alongside the `plan` module,
  and requires OpenCV **5.x**. There is no separate V4D repository.
* V4D is built as an Android NDK shared library by `./build.sh android`, not by
  hand through `emcmake`. `tools/android-build.sh` owns the NDK settings.
* The `third/` dependencies (GLFW, NanoVG, ImGui, GLAD) are vendored, so
  `third/glfw-android` is what links against Emscripten's GLES stubs.

The text is kept because the *reasoning* still holds and because the Emscripten
flags at the bottom are still the ones that matter. Re-check them against
`tools/android-build.sh` and `third/glfw-android/` before trusting any of it.

# Dependencies
* [Emscripten](https://emscripten.org)
* A WebGL2-capable browser

# Instructions for Ubuntu 22.04.2 LTS

## Install required packages
```bash
apt install cmake make git-core build-essential pkg-config python3 \
    software-properties-common
```

## Install emscripten
```bash
# Get the emsdk repo
git clone https://github.com/emscripten-core/emsdk.git

# Enter that directory
cd emsdk

# Install the latest SDK tools and activate them for the current user
# (this writes the .emscripten file)
./emsdk install latest
./emsdk activate latest

# Activate PATH and other environment variables in the current terminal
source ./emscripten_env.sh

# Leave the directory
cd ..
```

## Build
`build.sh` drives the cross-compilation; the Android command is the one that
produces per-demo shared objects. See `./build.sh --help` for `--abi`,
`--api-level`, `--demo` and `--apk`.

```bash
git clone --branch beta-5.x https://github.com/kallaballa/Plan-V4D.git
cd Plan-V4D
git submodule update --init --recursive
./build.sh android --configure-only
./build.sh android --demo cube-demo
```

## Run the examples and demos
WebAssembly features such as pthreads require
[special HTTP headers](https://emscripten.org/docs/porting/pthreads.html#webassembly-threads-and-sharedarraybuffer).
A plain `file://` URL will not do. Emscripten provides
[`emrun`](https://emscripten.org/docs/compiling/Running-html-files-with-emrun.html),
a web server configured with exactly those headers:

```bash
emrun --browser=firefox bin/example_v4d_cube-demo.html
```

Without a compositor the window screenshot of `--shot` is skipped; that is a
shell-script shortcut, not a WASM limitation.