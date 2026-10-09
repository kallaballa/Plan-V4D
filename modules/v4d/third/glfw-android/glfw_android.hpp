// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// Internal interface between the Android GLFW shim (glfw_android.cpp), the
// activity entry point (android_main.cpp) and the v4d library itself. This is
// not a public header: nothing outside modules/v4d should include it.

#ifndef MODULES_V4D_THIRD_GLFW_ANDROID_GLFW_ANDROID_HPP_
#define MODULES_V4D_THIRD_GLFW_ANDROID_GLFW_ANDROID_HPP_

#if defined(OPENCV_V4D_ANDROID)

#include <android/configuration.h>
#include <android/input.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <cstdint>

// The shim talks to ANativeActivity through these; android_main.cpp is the only
// caller that sets them.

/// Records the activity. Called from ANativeActivity_onCreate(), before any
/// surface or input queue exists.
void glfw_android_set_activity(ANativeActivity *activity);

/// Called from ANativeActivity::onInputQueueCreated().
void glfw_android_set_input_queue(AInputQueue *queue);

/// Called from ANativeActivity::onNativeWindowCreated(): publishes the surface
/// to whoever is blocked in glfwCreateWindow().
void glfw_android_set_native_window(ANativeWindow *window);

/// Called from ANativeActivity::onNativeWindowDestroyed(): the EGL surface is
/// recreated lazily from the next window that arrives.
void glfw_android_clear_native_window(ANativeWindow *window);

/// Called from ANativeActivity::onStart()/onStop(). Rendering must not touch the
/// surface while stopped, so glfwSwapBuffers() becomes a no-op in that state.
void glfw_android_set_activity_resumed(bool resumed);

/// Records the screen size in pixels reported by the last configuration change,
/// so a size query before the first surface has something to answer with.
void glfw_android_set_display_size(int widthPx, int heightPx);

/// The surface the shim is currently rendering into, with a reference the caller
/// owns (acquire/release it around a call to ANativeWindow_release). Returns
/// nullptr before a window arrives and after it goes away.
ANativeWindow *glfw_android_acquire_native_window();

/// Bumped whenever glfw_android_acquire_native_window() starts returning a
/// different surface. ImGui's Android backend caches the ANativeWindow* it was
/// given, so it has to be re-initialised when this changes.
uint64_t glfw_android_native_window_generation();

/// The raw input event handler to run for every AInputEvent before the GLFW
/// callbacks fire. ImGuiContextImpl installs one so that the whole ImGui input
/// set (which wants raw AInputEvents) keeps working, without the platform layer
/// having to know about ImGui at all.
using GlfwAndroidInputHandler = int32_t (*)(const AInputEvent *event);

/// \returns the previous handler, so it can be restored.
GlfwAndroidInputHandler glfw_android_set_input_handler(GlfwAndroidInputHandler handler);

/// Shows or hides the on-screen keyboard for the activity.
///
/// The NDK's own ANativeActivity_showSoftInput() is not usable from a
/// NativeActivity -- InputMethodManager rejects the NativeContentView as "not
/// served" -- so the request is forwarded to the activity's Java helpers
/// (showSoftInput()/hideSoftInput()) over JNI. Safe to call from any thread and
/// a no-op when no activity is recorded yet. The Java methods are looked up by
/// name, so this works with any NativeActivity subclass that provides them.
void glfw_android_set_soft_input_visible(bool show);

/// Pops one Unicode codepoint typed on the input method, or 0 when the queue
/// is empty.
///
/// The NDK has no AKeyEvent_getUnicodeChar(), so characters are captured in the
/// activity's Java layer (dispatchKeyEvent -> getUnicodeChar) and drained here.
/// Call until it returns 0 to empty the queue for a frame. Safe to call from any
/// thread; a no-op returning 0 when no activity is recorded.
int32_t glfw_android_poll_unicode_char();

#endif // OPENCV_V4D_ANDROID

#endif // MODULES_V4D_THIRD_GLFW_ANDROID_GLFW_ANDROID_HPP_