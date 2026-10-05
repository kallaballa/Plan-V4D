// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// How a sample enters the world, on the host and on Android.
//
// On the host a sample is an executable and its entry point is main(). On
// Android there are no executables: the app is a NativeActivity, the system
// loads a shared object and calls ANativeActivity_onCreate(), and the shared
// object is then expected to start the demo itself. third/glfw-android/
// android_main.cpp does that -- it runs the demo on a render thread -- but it
// needs a name to call that is not main(), because a library loaded by
// NativeActivity is never entered at main() and a main() in it would be dead
// weight.
//
// V4D_DEMO_MAIN() is therefore just the entry point under whichever name the
// host that loads this code expects. A sample says
//
//     V4D_DEMO_MAIN(int argc, char **argv)
//     int main(int argc, char **argv) { ... }
//
// and compiles unchanged either way. The arguments are passed through verbatim
// so that every sample has one signature and android_main.cpp has one call site
// (v4ddemo_main(0, nullptr)); on Android there is nothing to pass them to.
//
// Note for montage-demo.cpp and pipeline-demo.cpp, which #include other
// samples: the entry point has to be renamed per sample, and the name it goes
// by depends on the host, so they redirect V4D_SAMPLE_ENTRY_NAME rather than
// `main`.

#ifndef MODULES_V4D_SAMPLES_SAMPLES_HPP_
#define MODULES_V4D_SAMPLES_SAMPLES_HPP_

// The name a sample's own entry point goes by on this host, and how that name
// is declared. Both halves are separate macros because montage-demo.cpp and
// pipeline-demo.cpp #include other samples and have to give each of them a name
// of their own: `#define main v4d_video_main` redirects the host expansion but
// not the Android one, where two included samples would both define
// v4ddemo_main and collide. Redirecting V4D_SAMPLE_ENTRY_NAME covers both hosts
// with one rename.
//
// V4D_SAMPLE_SELF_NAME is that platform name on its own, so that a file which
// renames other samples' entry points has something to restore its *own* entry
// point to afterwards. It has to be a macro rather than a value:
// V4D_SAMPLE_ENTRY substitutes its argument textually, so an #undef'd
// V4D_SAMPLE_ENTRY_NAME does not expand to nothing, it expands to itself, and
// the sample's entry point becomes a C function named V4D_SAMPLE_ENTRY_NAME
// that no caller ever looks for. It compiles, it links, and nothing is
// reachable.
#if defined(__ANDROID__)
// extern "C" so that the name android_main.cpp declares is the same C symbol in
// both translation units rather than a mangled one.
#define V4D_SAMPLE_ENTRY(name) extern "C" int name
#define V4D_SAMPLE_SELF_NAME v4ddemo_main
#else
#define V4D_SAMPLE_ENTRY(name) int name
#define V4D_SAMPLE_SELF_NAME main
#endif

// What V4D_SAMPLE_ENTRY_NAME means until a file that #includes other samples
// redirects it. Expanded, not merely aliased, so that restoring it with
// `#define V4D_SAMPLE_ENTRY_NAME V4D_SAMPLE_SELF_NAME` yields the platform
// name.
#define V4D_SAMPLE_ENTRY_NAME V4D_SAMPLE_SELF_NAME

// The argument is macro-expanded before it is substituted into the declaration,
// so a sample that redirected V4D_SAMPLE_ENTRY_NAME gets that name here.
#define V4D_DEMO_MAIN(...) V4D_SAMPLE_ENTRY(V4D_SAMPLE_ENTRY_NAME)(__VA_ARGS__)

#endif // MODULES_V4D_SAMPLES_SAMPLES_HPP_