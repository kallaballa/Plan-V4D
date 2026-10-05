// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#ifndef SRC_OPENCV_V4D_SOURCE_HPP_
#define SRC_OPENCV_V4D_SOURCE_HPP_

#include <functional>
#include <mutex>
#include <opencv2/core.hpp>
#include <string>

using std::string;

namespace cv {
namespace v4d {

class V4D;
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
   * Signals if the source is ready to provide data.
   * @return true if the source is ready.
   */
  CV_EXPORTS bool isReady();

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
