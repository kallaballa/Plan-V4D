// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#ifndef SRC_OPENCV_V4D_SOURCE_HPP_
#define SRC_OPENCV_V4D_SOURCE_HPP_

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <string>

using std::string;

namespace cv {
namespace v4d {

class V4D;

/*!
 * Metadata of the current Android camera frame in zero-copy mode. buffer is a
 * borrowed AHardwareBuffer* that backs the most recently grabbed frame; it is
 * only valid until the next grab. A GL consumer imports it as an EGLImage with
 * EGL_ANDROID_get_native_client_buffer + eglCreateImageKHR and samples it as
 * GL_TEXTURE_EXTERNAL_OES.
 */
struct CV_EXPORTS HardwareBufferInfo {
  void *buffer = nullptr; //!< AHardwareBuffer* of the current frame, or nullptr
  int format = 0;         //!< AHardwareBuffer_Format of buffer
  int sensorOrientation = 0; //!< clockwise degrees (0/90/180/270)
  bool isFrontFacing = false;
};

/*!
 * A Source object represents a way to provide data to V4D by using
 * a generator functor.
 */
class CV_EXPORTS Source {
  bool open_ = true;
  std::function<bool(cv::UMat &)> generator_;
  float fps_;
  std::mutex mtx_;
  inline static thread_local cv::UMat frame_;
  // Android zero-copy camera capture
  std::atomic<bool> zeroCopy_ = false;
  // When set (Android camera), the generator lambda shares this flag, so a
  // runtime switch back to the CPU path (e.g. the GPU draw failing) takes
  // effect on the next grab.
  std::shared_ptr<std::atomic<bool>> zeroCopyShared_;
  std::function<bool(HardwareBufferInfo &)> hwBufferProvider_;
  bool hwBufferReady_ = false;
  HardwareBufferInfo hwBufferInfo_;

public:
  /*!
   * Constructs the Source object from a generator functor.
   * @param generator A function object that accepts a reference to a UMat frame
   * that it manipulates. This is ultimatively used to provide video data to
   * #cv::viz::V4D
   * @param fps The fps the Source object provides data with.
   */
  CV_EXPORTS Source(std::function<bool(cv::UMat &)> generator, float fps);
  /*!
   * Constructs a null Source that is never open or ready.
   */
  CV_EXPORTS Source();
  /*!
   * Default destructor.
   */
  CV_EXPORTS virtual ~Source();
  /*!
   * Determines if the source is open.
   * @return true if the source is open.
   */
  CV_EXPORTS bool isOpen();
  /*!
   * Returns the fps the underlying generator provides data with.
   * @return The fps of the Source object.
   */
  CV_EXPORTS float fps();
  /*!
   * The source operator. It returns the frame count and the frame generated
   * (e.g. by VideoCapture)in a pair.
   * @return A pair containing the frame count and the frame generated.
   */
  CV_EXPORTS cv::UMat operator()();

  /*!
   * True when the Android camera source runs in zero-copy mode: the frame is
   * grabbed as an AHardwareBuffer and drawn into the framebuffer by
   * FrameBufferContext instead of being converted on the CPU. While it is on,
   * operator() may return an empty frame (the pixels never reach the host).
   */
  CV_EXPORTS bool zeroCopy() const;

  /*!
   * Enables or disables zero-copy camera capture. Disabling it makes the source
   * take the ordinary CPU path (VideoCapture >> UMat) like any other source.
   */
  CV_EXPORTS void setZeroCopy(bool enable);

  /*!
   * Binds the shared zero-copy flag the camera generator watches, so that
   * setZeroCopy() takes effect on the next grab. Internal use by makeCamera().
   */
  CV_EXPORTS void
  bindZeroCopyState(const std::shared_ptr<std::atomic<bool>> &state);

  /*!
   * Sets a functor that fills HardwareBufferInfo from the most recently grabbed
   * camera frame. Called once per grab, right after the generator ran.
   */
  CV_EXPORTS void
  setHardwareBufferProvider(const std::function<bool(HardwareBufferInfo &)> &provider);

  /*!
   * Returns the hardware buffer of the current camera frame and clears the
   * "fresh" flag, so the frame is drawn into the framebuffer exactly once.
   * @return false when the current frame has no hardware buffer (CPU fallback).
   */
  CV_EXPORTS bool consumeHardwareBufferInfo(HardwareBufferInfo &info);

  /*!
   * Opens a video file, using whatever hardware acceleration the platform
   * offers. On Android this is not usable for a live camera: it goes through
   * FFmpeg, which the Android build does not enable, so a sample that wants the
   * camera there wants makeDefault() instead.
   */
  static cv::Ptr<Source> make(cv::Ptr<V4D> window, const string &inputFilename);

  /*!
   * The source a sample that wants video should use, resolved for the platform
   * it was built for.
   *
   * On the host this is make(window, inputFilename). On Android it ignores
   * inputFilename entirely and opens camera `cameraIndex` through
   * cv::CAP_ANDROID, which OpenCV's Android backend implements on the NDK
   * (camera2 / MediaCodec) with no Java and no OpenCV Java build in sight.
   *
   * Samples call this rather than branching on __ANDROID__ themselves: a demo
   * that takes a path from argv[1] has no way to tell a user of an APK that the
   * argument is ignored there, and eleven samples that each carried their own
   * #ifdef is eleven chances to get that message wrong.
   *
   * @param cameraIndex Camera to open on Android. Ignored elsewhere. 0 is
   * whichever camera the platform lists first, which on a phone is the rear
   * one.
   */
  static cv::Ptr<Source> makeDefault(cv::Ptr<V4D> window,
                                     const string &inputFilename,
                                     int cameraIndex = 0);

private:
  /*!
   * Opens camera `cameraIndex` via cv::CAP_ANDROID. The frame rate is reported
   * as `fps` because a camera does not have one to read: it produces frames as
   * fast as the pipeline consumes them.
   */
  static cv::Ptr<Source> makeCamera(cv::Ptr<V4D> window, int cameraIndex,
                                    float fps = 30.f);

  static cv::Ptr<Source> makeVaSource(cv::Ptr<V4D> window,
                                      const string &inputFilename,
                                      const int vaDeviceIndex);
  static cv::Ptr<Source> makeAnyHWSource(cv::Ptr<V4D> window,
                                         const string &inputFilename);
};

} /* namespace v4d */
} // namespace cv

#endif /* SRC_OPENCV_V4D_SOURCE_HPP_ */
