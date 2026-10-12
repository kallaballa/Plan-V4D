// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#include <android/log.h>
#endif

#include <opencv2/v4d/detail/sourcecontext.hpp>
#include <opencv2/v4d/source.hpp>
#include <opencv2/v4d/v4d.hpp>

#if defined(__ANDROID__)
#include "glfw_android.hpp"
#include <opencv2/imgproc.hpp>
namespace {
// Current screen rotation in clockwise degrees (0/90/180/270), taken live from
// the activity each time (glfw_android_display_rotation -> Java getDisplay());
// a JNI call, like the existing glfw_android_poll_unicode_char.
inline int v4dAndroidDisplayRotationDeg() {
  int raw = glfw_android_display_rotation();
  if (raw < 0)
    raw = 0;
  raw &= 3; // Surface.ROTATION_0..270
  return (raw * 90) % 360;
}

// Clockwise degrees to rotate the raw sensor frame by so it renders upright on
// the *current* screen orientation. The sensor's fixed mounting orientation
// alone is upright only at the device's natural orientation; folding in the
// display rotation (Camera2's per-facing "total rotation") is what keeps the
// preview upright however the screen is turned:   back : (sensor - display)
//                                                    front: (sensor + display)
inline int v4dTotalCameraRotationDeg(int sensor, bool frontFacing) {
  const int disp = v4dAndroidDisplayRotationDeg();
  int t = frontFacing ? (sensor + disp) : (sensor - disp);
  t %= 360;
  if (t < 0)
    t += 360;
  return t;
}

// The CAP_ANDROID backend already rotates by the fixed sensor orientation
// (autorotate ON). Rotate the remaining delta so the net rotation equals the
// total that both the zero-copy shader and the CPU fallback should produce,
// keeping the two paths byte-identical in orientation.
inline void v4dRotateFrameForDisplay(cv::VideoCapture *capture, cv::UMat &frame) {
  if (frame.empty() || capture == nullptr)
    return;
  const int sensor = cv::videoioAndroidCameraSensorOrientation(*capture);
  const bool front = cv::videoioAndroidCameraIsFrontFacing(*capture);
  const int total = v4dTotalCameraRotationDeg(sensor, front);
  const int delta = ((total - sensor) % 360 + 360) % 360;
  if (delta == 90)
    cv::rotate(frame, frame, cv::ROTATE_90_CLOCKWISE);
  else if (delta == 180)
    cv::rotate(frame, frame, cv::ROTATE_180);
  else if (delta == 270)
    cv::rotate(frame, frame, cv::ROTATE_90_COUNTERCLOCKWISE);
}
} // namespace
#endif

namespace cv {
namespace v4d {

cv::Ptr<Source> Source::makeVaSource(cv::Ptr<V4D> window,
                                     const string &inputFilename,
                                     const int vaDeviceIndex) {
  cv::Ptr<cv::VideoCapture> capture = new cv::VideoCapture(
      inputFilename, cv::CAP_FFMPEG,
      {cv::CAP_PROP_HW_DEVICE, vaDeviceIndex, cv::CAP_PROP_HW_ACCELERATION,
       cv::VIDEO_ACCELERATION_VAAPI, cv::CAP_PROP_HW_ACCELERATION_USE_OPENCL,
       1});
  float fps = capture->get(cv::CAP_PROP_FPS);
  CV_LOG_INFO(nullptr, "Using a VA source");

  std::dynamic_pointer_cast<detail::SourceContext>(window->sourceCtx())
      ->copyContext();

  return new Source(
      [=](cv::UMat &frame) {
        (*capture) >> frame;
        return !frame.empty();
      },
      fps);
}

cv::Ptr<Source> Source::makeAnyHWSource(cv::Ptr<V4D> window,
                                        const string &inputFilename) {
  cv::Ptr<cv::VideoCapture> capture = new cv::VideoCapture(
      inputFilename, cv::CAP_FFMPEG,
      {cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY});

  float fps = capture->get(cv::CAP_PROP_FPS);

  std::dynamic_pointer_cast<detail::SourceContext>(window->sourceCtx())
      ->copyContext();

  return new Source(
      [=](cv::UMat &frame) {
        (*capture) >> frame;
        return !frame.empty();
      },
      fps);
}

cv::Ptr<Source> Source::make(cv::Ptr<V4D> window, const string &inputFilename) {
#ifdef HAVE_VA
  if (is_intel_va_supported()) {
    return makeVaSource(window, inputFilename, 0);
  } else
#endif
  {
    try {
      return makeAnyHWSource(window, inputFilename);
    } catch (...) {
      CV_LOG_INFO(nullptr, "Failed to create hardware source");
    }
  }

  cv::Ptr<cv::VideoCapture> capture =
      new cv::VideoCapture(inputFilename, cv::CAP_FFMPEG);
  float fps = capture->get(cv::CAP_PROP_FPS);

  return new Source(
      [=](cv::UMat &frame) {
        (*capture) >> frame;
        return !frame.empty();
      },
      fps);
}

cv::Ptr<Source> Source::makeCamera(cv::Ptr<V4D> window, int cameraIndex,
                                   float fps) {
  // The int overload with CAP_ANDROID is what lands in
  // createAndroidCapture_cam: OpenCV's Android backend is pure NDK
  // (camera2 + MediaCodec via libcamera2ndk), so this needs no Java and no
  // OpenCV Java build. The filename overload would instead go to the MediaNDK
  // file backend, which is not what a demo with a live camera wants.
  //
  // CAP_PROP_FOURCC is not optional here. Without it the backend keeps its
  // default of FOURCC_UNKNOWN and then settles on the camera's own format --
  // NV21 or YV12
  // -- and hands back the packed YUV plane as a CV_8UC1 matrix rather than
  // converting it. detail::SourceContext asserts CV_8UC3/CV_8UC4 on every
  // frame, so an unconverted camera is an assertion failure on the first frame,
  // a few hundred milliseconds after a window that looks like it works has
  // already appeared.
  //
  // BGRA, and not the RGBA the conversion enum name suggests would be tidier: a
  // Source is required to deliver BGR, because that is what OpenCV's capture
  // backends give -- FFmpeg on the host hands back BGR -- and
  // detail::SourceContext's COLOR_RGB2BGRA is the swap that turns that into the
  // RGBA byte order the framebuffer uploads as GL_RGBA. Ask the camera for RGBA
  // instead and that swap runs on a frame that is already RGRA, which shows up
  // as red and blue exchanged in every pixel.
  //
  // CAP_PROP_ANDROID_HARDWARE_BUFFER opts into zero-copy capture: the backend
  // creates the ImageReader with a GPU-samplable usage and exports the
  // AHardwareBuffer of every frame, which FrameBufferContext then draws into
  // the framebuffer directly (no CPU conversion, no CPU resize, no
  // glTexImage2D upload). The BGRA fourCC above stays in the request so the
  // CPU fallback keeps working when a device or driver refuses the GPU buffer.
  cv::Ptr<cv::VideoCapture> capture = new cv::VideoCapture(
      cameraIndex, cv::CAP_ANDROID,
      {cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('B', 'G', 'R', '4'),
       cv::CAP_PROP_ANDROID_HARDWARE_BUFFER, 1});

  // A VideoCapture over a camera index only reports itself open once the
  // backend has opened a device; without the check the first frame comes back
  // empty and the caller sees a demo that renders nothing and says nothing.
  if (!capture->isOpened()) {
    CV_Error(Error::StsError,
             "Camera " + std::to_string(cameraIndex) +
                 " could not be opened. Is android.permission.CAMERA granted, "
                 "and does this device have that many cameras?");
  }

  CV_LOG_INFO(nullptr, "Using Android camera %d as BGRA", cameraIndex);

  std::dynamic_pointer_cast<detail::SourceContext>(window->sourceCtx())
      ->copyContext();

#if defined(__ANDROID__)
  // Zero-copy is the default on Android (the grabbable AHardwareBuffer is what
  // CAP_ANDROID is built for); it can be turned off to compare against the CPU
  // path or to work around a driver that rejects the EGLImage import.
  // A per-process environment cannot be injected into a device app, so besides
  // V4D_DISABLE_ZERO_COPY_CAMERA the opt-out is also honored via the
  // debug.v4d.zero_copy system property ("0"/"false"/"off"/"no" disables),
  // which adb can set on any device.
  bool zeroCopy = true;
  if (std::getenv("V4D_DISABLE_ZERO_COPY_CAMERA") != nullptr)
    zeroCopy = false;
  
  char prop[PROP_VALUE_MAX] = {0};
  if (__system_property_get("debug.v4d.zero_copy", prop) > 0) {
    const std::string value = prop;
    if (value == "0" || value == "false" || value == "off" || value == "no")
      zeroCopy = false;
  }
#else
  const bool zeroCopy = false;
#endif

  // The generator and the Source share this flag, so a runtime switch to the
  // CPU path (e.g. the GPU draw failing) is picked up by the next grab.
  auto zeroCopyState = std::make_shared<std::atomic<bool>>(zeroCopy);

  cv::Ptr<Source> src = new Source(
      [=](cv::UMat &frame) {
#if defined(__ANDROID__)
        if (zeroCopyState->load()) {
          // Grab only: the plane copy stays inside the backend (it keeps the
          // CPU path available without extra setup), but the convert-to-BGRA
          // cvtColor that VideoCapture::read() would trigger is skipped; the
          // GPU draws the frame from the AHardwareBuffer instead.
          if (!capture->grab())
            return false;
          if (cv::videoioAndroidCameraHardwareBuffer(*capture) != nullptr) {
            // Zero-copy: an empty frame from here means "the pixels are in the
            // hardware buffer, FrameBufferContext will draw them".
            frame.release();
            return true;
          }
          // The backend fell back to the CPU path (ImageReader without GPU
          // usage): convert here as usual, then apply the live display-rotation
          // delta so the net rotation matches the zero-copy shader exactly.
          if (capture->retrieve(frame) && !frame.empty()) {
            v4dRotateFrameForDisplay(capture.get(), frame);
            return true;
          }
          return false;
        }
#endif
        (*capture) >> frame;
        // Reported here rather than left to the assert in SourceContext,
        // because the fourCC above is a request and this is the check that it
        // was honoured: a device whose backend ignores it would otherwise die
        // with an assertion that names a file no one was looking at.
        if (!frame.empty() && frame.channels() != 3 && frame.channels() != 4) {
          CV_Error(Error::StsError,
                   "Android camera returned a " +
                       std::to_string(frame.channels()) +
                       "-channel frame; V4D needs 3 (BGR) or 4 (BGRA). The "
                        "CAP_ANDROID backend did not honour the BGRA fourCC.");
        }
#if defined(__ANDROID__)
        if (!frame.empty())
          v4dRotateFrameForDisplay(capture.get(), frame);
#endif
        return !frame.empty();
      },
      fps);

#if defined(__ANDROID__)
  if (zeroCopy) {
    src->bindZeroCopyState(zeroCopyState);
    src->setZeroCopy(true);
    src->setHardwareBufferProvider(
        [capture](HardwareBufferInfo &info) -> bool {
          void *buf = cv::videoioAndroidCameraHardwareBuffer(*capture);
          if (buf == nullptr)
            return false;
          info.buffer = buf;
          info.format = cv::videoioAndroidCameraHardwareBufferFormat(*capture);
          info.isFrontFacing = cv::videoioAndroidCameraIsFrontFacing(*capture);
          // Fold the live display rotation into the frame rotation the
          // external-texture shader applies. sensorOrientation here now carries
          // the TOTAL rotation; sensor-alone left the feed 90 deg off whenever
          // the screen was turned away from the device's natural orientation.
          info.sensorOrientation = v4dTotalCameraRotationDeg(
              cv::videoioAndroidCameraSensorOrientation(*capture),
              info.isFrontFacing);
          __android_log_print(ANDROID_LOG_INFO, "V4DOrient",
                              "camera: rotJNI=%d sensor=%d front=%d total=%d",
                              glfw_android_display_rotation(),
                              cv::videoioAndroidCameraSensorOrientation(*capture),
                              info.isFrontFacing ? 1 : 0, info.sensorOrientation);
          return true;
        });
  }
#endif

  return src;
}

cv::Ptr<Source> Source::makeDefault(cv::Ptr<V4D> window,
                                    const string &inputFilename,
                                    int cameraIndex) {
#if defined(__ANDROID__)
  // A path passed in by argv[1] cannot be honoured here: the APK carries no
  // video, and pulling one off the device needs storage permission and a Java
  // picker, neither of which a NativeActivity has. The camera is the one input
  // every video demo can rely on, so it is the one they get.
  (void)inputFilename;
  return makeCamera(window, cameraIndex);
#else
  (void)cameraIndex;
  return make(window, inputFilename);
#endif
}

Source::Source(std::function<bool(cv::UMat &)> generator, float fps)
    : generator_(generator), fps_(fps) {}

Source::Source() : open_(false), fps_(0) {}

Source::~Source() {}

bool Source::isOpen() {
  std::lock_guard<std::mutex> guard(mtx_);
  return generator_ && open_;
}

float Source::fps() { return fps_; }

cv::UMat Source::operator()() {
  std::lock_guard<std::mutex> guard(mtx_);

  open_ = generator_(frame_);
  if (zeroCopy_ && hwBufferProvider_) {
    hwBufferReady_ = hwBufferProvider_(hwBufferInfo_);
  }
  // first frame has the sequence number 1!
  return frame_;
}

bool Source::zeroCopy() const {
  return zeroCopyShared_ ? zeroCopyShared_->load() : zeroCopy_.load();
}

void Source::setZeroCopy(bool enable) {
  zeroCopy_.store(enable);
  if (zeroCopyShared_)
    zeroCopyShared_->store(enable);
  if (!enable) {
    hwBufferReady_ = false;
    hwBufferInfo_ = HardwareBufferInfo();
  }
}

void Source::bindZeroCopyState(
    const std::shared_ptr<std::atomic<bool>> &state) {
  zeroCopyShared_ = state;
  if (state)
    zeroCopy_.store(state->load());
}

void Source::setHardwareBufferProvider(
    const std::function<bool(HardwareBufferInfo &)> &provider) {
  hwBufferProvider_ = provider;
}

bool Source::consumeHardwareBufferInfo(HardwareBufferInfo &info) {
  if (!hwBufferReady_)
    return false;
  hwBufferReady_ = false;
  info = hwBufferInfo_;
  return true;
}
} /* namespace v4d */
} // namespace cv
