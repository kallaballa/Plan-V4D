// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The Android entry point of a V4D demo.
//
// The activity is declared as android.app.NativeActivity, so the system loads
// this library and calls ANativeActivity_onCreate() on the UI thread. A V4D demo
// is ordinary code with an ordinary main(), so all this has to do is:
//
//   1. remember the activity, its input queue and its surface, each of which
//      arrives through a separate callback on the UI thread;
//   2. run the demo's main() on a render thread, which is where EGL contexts
//      belong and where V4D expects its display thread to live;
//   3. keep V4D_ASSET_PATH pointing at files extracted from the APK.
//
// The demo is linked into this same library and entered through v4ddemo_main(),
// which every sample defines (see samples/font_rendering.cpp).

#include "glfw_android.hpp"

#if defined(OPENCV_V4D_ANDROID)

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <android/native_activity.h>

#include <opencv2/v4d/util.hpp>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

// NativeActivity locates its entry point with dlsym(), so every callback here
// has to reach the dynamic symbol table. This build compiles with
// -fvisibility=hidden (OpenCV sets it for every module), and extern "C" V4D_ACTIVITY_EXPORT only
// settles the name mangling -- without this attribute the library loads and then
// fails to find its own entry point.
#if defined(__GNUC__) || defined(__clang__)
#define V4D_ACTIVITY_EXPORT __attribute__((visibility("default")))
#else
#define V4D_ACTIVITY_EXPORT
#endif

#define LOG_TAG "v4d-activity"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/// Defined by the demo this library is built around. Every sample renames its
/// main() to this (see samples/samples.hpp), so the same source builds as a
/// desktop executable and as the body of an Android .so. The signature is the
/// same one main() has, and there is nothing to pass: an APK has no argv.
extern "C" V4D_ACTIVITY_EXPORT int v4ddemo_main(int argc, char **argv);

namespace {

/// The thread the demo runs on.
std::thread gRenderThread;

/// Creates the directory holding \p file and every parent of it.
void makeParentDirectories(const std::string &file) {
  for (size_t slash = file.find('/'); slash != std::string::npos;
       slash = file.find('/', slash + 1))
    mkdir(file.substr(0, slash).c_str(), 0755);
}

/// Copies one asset out of the APK.
///
/// The APK is a zip file, so nanovg's fopen() cannot open a path inside it even
/// though the path resolves; the file has to exist as a real file on disk. The
/// activity's private storage is the only place that is guaranteed to exist and
/// to be writable, so everything is copied there once at startup.
///
/// \returns false when the asset was not in the APK at all.
bool copyAsset(ANativeActivity *activity, AAssetManager *assets,
               const std::string &assetPath, const std::string &relative) {
  const std::string target =
      std::string(activity->internalDataPath) + "/assets/" + relative;
  makeParentDirectories(target);

  AAsset *asset = AAssetManager_open(assets, assetPath.c_str(), AASSET_MODE_STREAMING);
  if (!asset) {
    LOGE("asset %s is not in the APK", assetPath.c_str());
    return false;
  }
  FILE *out = fopen(target.c_str(), "wb");
  if (!out) {
    LOGE("cannot write %s: %s", target.c_str(), strerror(errno));
    AAsset_close(asset);
    return false;
  }
  char buffer[16384];
  int read = 0;
  while ((read = AAsset_read(asset, buffer, sizeof(buffer))) > 0)
    fwrite(buffer, 1, static_cast<size_t>(read), out);
  AAsset_close(asset);
  fclose(out);
  LOGI("extracted %s", assetPath.c_str());
  return true;
}

/// The list of staged assets, written by package-apk.sh.
///
/// The NDK has no public way to walk the asset tree (AAssetManager_openDir opens
/// a directory but nothing enumerates it), so the assets cannot be discovered from
/// here -- they have to be named. Taking the names from the packager rather than
/// hard-coding them here is what keeps the two in step: this file extracts exactly
/// the set that package-apk.sh put into the APK, so adding an asset to a sample is
/// a one-line change in the script and needs no second list to update.
static const char *kAssetManifest = "assets.manifest";

/// Copies every asset named in the manifest, preserving the paths it gives.
///
/// \returns false when the manifest or any asset it names was missing.
bool copyAssets(ANativeActivity *activity, AAssetManager *assets,
                const std::string &root) {
  AAsset *manifest = AAssetManager_open(assets, kAssetManifest, AASSET_MODE_STREAMING);
  if (!manifest) {
    LOGE("the APK has no %s, so it was not packaged by tools/android/package-apk.sh",
         kAssetManifest);
    return false;
  }
  std::string names;
  char chunk[4096];
  int read = 0;
  while ((read = AAsset_read(manifest, chunk, sizeof(chunk))) > 0)
    names.append(chunk, static_cast<size_t>(read));
  AAsset_close(manifest);

  bool ok = true;
  for (size_t start = 0; start < names.size();) {
    size_t end = names.find('\n', start);
    if (end == std::string::npos)
      end = names.size();
    const std::string name = names.substr(start, end - start);
    start = end + 1;
    if (!name.empty())
      ok &= copyAsset(activity, assets, name, name);
  }
  LOGI("extracted %zu assets into %s",
       static_cast<size_t>(std::count(names.begin(), names.end(), '\n')), root.c_str());
  return ok;
}

/// The demo's assets: fonts, images, models, videos.
///
/// A missing file is not a cosmetic problem. NanoVG keeps an unregistered font
/// name with no glyphs, so every text() draws nothing and nothing says why;
/// cv::findFile() throws instead, but only once V4D::init() has already built a
/// GL context and a window.
///
/// The APK flattens the three source directories package-apk.sh stages
/// (samples/data, samples/fonts, modules/v4d/assets) into one asset root, and the
/// samples look their files up by those flattened names, so the extracted tree is
/// placed under V4D_ASSET_PATH exactly as the manifest spells it.
void installAssets(ANativeActivity *activity, AAssetManager *assets) {
  const std::string root = std::string(activity->internalDataPath) + "/assets";
  makeParentDirectories(root + "/fonts/.keep");
  setenv("V4D_ASSET_PATH", root.c_str(), 1);

  if (!copyAssets(activity, assets, root))
    LOGE("some assets are missing; whatever uses them will fail");
  LOGI("V4D_ASSET_PATH=%s", getenv("V4D_ASSET_PATH"));
}

void renderThread(ANativeActivity *activity) {
  LOGI("render thread starting (pid %d)", getpid());
  // The demo creates its window while it initialises, and glfwCreateWindow()
  // blocks until the activity hands over a surface, so there is nothing to wait
  // for here -- and V4D must not touch GL before that point anyway.
  const int result = v4ddemo_main(0, nullptr);
  LOGI("demo returned %d; render thread exiting", result);
  // Leaving the render thread does not end the app: the activity is still up and
  // still owns the surface.
  ANativeActivity_finish(activity);
}

} // namespace

// native_activity.h already declares this, as a function of type
// ANativeActivity_createFunc. Its third parameter is the saved-state size -- not a
// configuration -- so the definition below has to match it exactly.
// Every one of the callbacks below is reached through
// ANativeActivity::callbacks, never by name: only ANativeActivity_onCreate is
// resolved with dlsym() (which is why that one alone has to be exported, and why
// NativeActivity reports a missing library rather than a silent no-op). The
// header is explicit -- "By default, all callbacks are NULL; set to a pointer
// to your own function to have it called" -- so the app owns this table.
//
// The table is not one the app allocates, though. The framework embeds one in
// its own object and hands out a pointer to it before calling onCreate
// (android_app_NativeActivity.cpp: `code->ANativeActivity::callbacks =
// &code->callbacks;`), then dispatches every event by reading *that* struct.
// Publishing a table of our own and pointing activity->callbacks at it therefore
// has no effect at all: the framework keeps reading its own zeroed copy. The
// fields have to be written where it looks.
//
// Without them the app launches, draws nothing and throws ten seconds later: the
// surface arrives through onNativeWindowCreated, a NULL callback means that
// never happens, glfwCreateWindow() blocks until its timeout, and V4D reports
// "Unable to initialize window" -- which says nothing about the cause.
//
// Fields are assigned by name rather than through a brace initialiser, because
// the struct has grown over the years (onSaveInstanceState, then
// onNativeWindowResized and onNativeWindowRedrawNeeded) and naming each field
// keeps this right whatever this NDK's header contains. What is left NULL stays
// NULL -- notably onSaveInstanceState, since nothing here has state to restore.
extern "C" void ANativeActivity_onStart(ANativeActivity *activity);
extern "C" void ANativeActivity_onResume(ANativeActivity *activity);
extern "C" void ANativeActivity_onPause(ANativeActivity *activity);
extern "C" void ANativeActivity_onStop(ANativeActivity *activity);
extern "C" void ANativeActivity_onDestroy(ANativeActivity *activity);
extern "C" void ANativeActivity_onWindowFocusChanged(ANativeActivity *activity,
                                                     int hasFocus);
extern "C" void ANativeActivity_onNativeWindowCreated(ANativeActivity *activity,
                                                      ANativeWindow *window);
extern "C" void ANativeActivity_onNativeWindowDestroyed(ANativeActivity *activity,
                                                        ANativeWindow *window);
extern "C" void ANativeActivity_onInputQueueCreated(ANativeActivity *activity,
                                                    AInputQueue *queue);
extern "C" void ANativeActivity_onInputQueueDestroyed(ANativeActivity *activity,
                                                      AInputQueue *queue);
extern "C" void ANativeActivity_onContentRectChanged(ANativeActivity *activity,
                                                      const ARect *rect);
extern "C" void ANativeActivity_onConfigurationChanged(ANativeActivity *activity);

extern "C" V4D_ACTIVITY_EXPORT void ANativeActivity_onCreate(ANativeActivity *activity,
                                                              void *savedState,
                                                              size_t savedStateSize) {
  (void)savedState;
  (void)savedStateSize;

  // The framework's own table, which it zeroes and then reads. If some
  // platform ever handed over a NULL instead, fall back to one of our own so the
  // demo still runs rather than silently losing every callback.
  ANativeActivityCallbacks *callbacks = activity->callbacks;
  if (!callbacks) {
    static ANativeActivityCallbacks own = {};
    callbacks = &own;
    activity->callbacks = callbacks;
  }

  callbacks->onStart = ANativeActivity_onStart;
  callbacks->onResume = ANativeActivity_onResume;
  callbacks->onPause = ANativeActivity_onPause;
  callbacks->onStop = ANativeActivity_onStop;
  callbacks->onDestroy = ANativeActivity_onDestroy;
  callbacks->onWindowFocusChanged = ANativeActivity_onWindowFocusChanged;
  callbacks->onNativeWindowCreated = ANativeActivity_onNativeWindowCreated;
  callbacks->onNativeWindowDestroyed = ANativeActivity_onNativeWindowDestroyed;
  callbacks->onInputQueueCreated = ANativeActivity_onInputQueueCreated;
  callbacks->onInputQueueDestroyed = ANativeActivity_onInputQueueDestroyed;
  callbacks->onContentRectChanged = ANativeActivity_onContentRectChanged;
  callbacks->onConfigurationChanged = ANativeActivity_onConfigurationChanged;

  // onCreate runs before the surface and the input queue arrive, so the activity
  // is all that can be recorded yet.
  glfw_android_set_activity(activity);
  installAssets(activity, activity->assetManager);
  // Registered here rather than left to the demo: not every demo calls
  // add_asset_search_paths() -- font_rendering does not -- and on the desktop
  // those that do not still find their fonts, because "modules/v4d/assets/..."
  // resolves against the build directory the sample happens to be run from.
  // There is no such luck on a device, so every demo needs this. It is
  // idempotent, so a demo that calls it itself is unaffected.
  cv::v4d::add_asset_search_paths();

  gRenderThread = std::thread(renderThread, activity);
}

extern "C" V4D_ACTIVITY_EXPORT void ANativeActivity_onStart(ANativeActivity *activity) {
  LOGI("onStart");
  glfw_android_set_activity_resumed(true);
}

extern "C" V4D_ACTIVITY_EXPORT void ANativeActivity_onResume(ANativeActivity *activity) {
  LOGI("onResume");
  glfw_android_set_activity_resumed(true);
}

// Views are an alternative to surfaces; this app always renders to the surface.
extern "C" V4D_ACTIVITY_EXPORT void *ANativeActivity_onCreateView(ANativeActivity *activity,
                                              ANativeWindow *window) {
  (void)activity;
  (void)window;
  return nullptr;
}

extern "C" V4D_ACTIVITY_EXPORT void ANativeActivity_onInputQueueCreated(ANativeActivity *activity,
                                                     AInputQueue *queue) {
  (void)activity;
  LOGI("onInputQueueCreated: %p", (void *)queue);
  glfw_android_set_input_queue(queue);
}

extern "C" V4D_ACTIVITY_EXPORT void ANativeActivity_onInputQueueDestroyed(ANativeActivity *activity,
                                                       AInputQueue *queue) {
  (void)activity;
  (void)queue;
  glfw_android_set_input_queue(nullptr);
}

extern "C" V4D_ACTIVITY_EXPORT void ANativeActivity_onNativeWindowCreated(ANativeActivity *activity,
                                                       ANativeWindow *window) {
  (void)activity;
  LOGI("onNativeWindowCreated: %p (%dx%d)", (void *)window,
       window ? ANativeWindow_getWidth(window) : 0,
       window ? ANativeWindow_getHeight(window) : 0);
  glfw_android_set_native_window(window);
}

extern "C" V4D_ACTIVITY_EXPORT void ANativeActivity_onNativeWindowDestroyed(ANativeActivity *activity,
                                                         ANativeWindow *window) {
  (void)activity;
  LOGI("onNativeWindowDestroyed: %p", (void *)window);
  glfw_android_clear_native_window(window);
}

extern "C" V4D_ACTIVITY_EXPORT void ANativeActivity_onWindowFocusChanged(ANativeActivity *activity,
                                                     int hasFocus) {
  (void)activity;
  LOGI("onWindowFocusChanged: %d", hasFocus);
}

extern "C" V4D_ACTIVITY_EXPORT void ANativeActivity_onContentRectChanged(ANativeActivity *activity,
                                                      const ARect *rect) {
  (void)activity;
  (void)rect;
  LOGI("onContentRectChanged");
}

extern "C" V4D_ACTIVITY_EXPORT void ANativeActivity_onConfigurationChanged(ANativeActivity *activity) {
  (void)activity;
  LOGI("onConfigurationChanged");
  // Rotation and multi-window resize both land here, before the new surface
  // exists. Recording the dp size now is what lets the shim answer a size query
  // in that window between the configuration change and the new window.
  AConfiguration *config = AConfiguration_new();
  if (config) {
    AConfiguration_fromAssetManager(config, activity->assetManager);
    const int density = AConfiguration_getDensity(config);
    const float scale = density > 0 ? static_cast<float>(density) / 160.f : 1.f;
    glfw_android_set_display_size(
        static_cast<int>(AConfiguration_getScreenWidthDp(config) * scale),
        static_cast<int>(AConfiguration_getScreenHeightDp(config) * scale));
    AConfiguration_delete(config);
  }
}

extern "C" V4D_ACTIVITY_EXPORT void ANativeActivity_onLowMemory(ANativeActivity *activity) {
  (void)activity;
}

extern "C" V4D_ACTIVITY_EXPORT void ANativeActivity_onPause(ANativeActivity *activity) {
  (void)activity;
  LOGI("onPause");
  // Stop presenting: the surface is about to go away.
  glfw_android_set_activity_resumed(false);
}

extern "C" V4D_ACTIVITY_EXPORT void ANativeActivity_onStop(ANativeActivity *activity) {
  (void)activity;
  glfw_android_set_activity_resumed(false);
}

extern "C" V4D_ACTIVITY_EXPORT void ANativeActivity_onDestroy(ANativeActivity *activity) {
  LOGI("onDestroy");
  glfw_android_set_activity_resumed(false);
  // V4D still owns the EGL context and the OpenCV runtimes, and joining here
  // would block the UI thread for as long as the plan takes to notice, so the
  // render thread is left to unwind on its own.
  if (gRenderThread.joinable())
    gRenderThread.detach();
  ANativeActivity_finish(activity);
}

#endif // OPENCV_V4D_ANDROID