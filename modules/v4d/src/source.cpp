// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#include <opencv2/v4d/detail/sourcecontext.hpp>
#include <opencv2/v4d/source.hpp>
#include <opencv2/v4d/v4d.hpp>

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
  cv::Ptr<cv::VideoCapture> capture = new cv::VideoCapture(
      cameraIndex, cv::CAP_ANDROID,
      {cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('B', 'G', 'R', '4')});

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

  return new Source(
      [=](cv::UMat &frame) {
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
        return !frame.empty();
      },
      fps);
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
  // first frame has the sequence number 1!
  return frame_;
}
} /* namespace v4d */
} // namespace cv
