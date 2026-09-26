// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#ifndef SRC_OPENCV_V4D_SINKSOURCE_HPP_
#define SRC_OPENCV_V4D_SINKSOURCE_HPP_

#include <functional>
#include <map>
#include <mutex>
#include <condition_variable>
#include <opencv2/core.hpp>
#include <string>

using std::string;
namespace cv {
namespace v4d {

class V4D;
/*!
 * A SinkSource object represents a way to both provide and consume data
 * to and from V4D by combining a generator functor (Source) and a
 * consumer functor (Sink) in a single object.
 */
class CV_EXPORTS SinkSource {
    friend class cv::v4d::V4D;
    bool open_ = true;
    std::function<bool(cv::UMat&)> generator_;
    float fps_;
    std::mutex mtx_;
    inline static thread_local cv::UMat frame_;
    uint64_t nextSeq_ = 1;
    std::map<uint64_t, cv::UMat> buffer_;
    std::condition_variable cv_;
    std::function<bool(const uint64_t&, const cv::UMat&)> consumer_;
public:
    /*!
     * Constructs the SinkSource object from a generator and a consumer functor.
     * @param generator A function object that accepts a reference to a UMat frame
     * that it manipulates. This is ultimately used to provide video data to V4D.
     * @param fps The fps the generator provides data with.
     * @param consumer A function object that consumes a UMat frame (e.g. writes it to a video file).
     */
    CV_EXPORTS SinkSource(std::function<bool(cv::UMat&)> generator, float fps, std::function<bool(const uint64_t&, const cv::UMat&)> consumer);
    /*!
     * Constructs a null SinkSource that is never open or ready.
     */
    CV_EXPORTS SinkSource();
    /*!
     * Constructs an open SinkSource suitable for use as a bridge between plans.
     * @param fps The fps of the SinkSource.
     */
    CV_EXPORTS SinkSource(float fps);
    /*!
     * Default destructor.
     */
    CV_EXPORTS virtual ~SinkSource();
    /*!
     * Signals if the source part is ready to provide data.
     * @return true if the source is ready.
     */
    CV_EXPORTS bool isReady();
    /*!
     * Determines if the source part is open.
     * @return true if the source is open.
     */
    CV_EXPORTS bool isOpen();
    /*!
     * Returns the fps the underlying generator provides data with.
     * @return The fps of the SinkSource object.
     */
    CV_EXPORTS float fps();
    /*!
     * The source operator. It returns the generated frame.
     * @return The frame generated.
     */
    CV_EXPORTS cv::UMat operator()();
    /*!
     * The sink operator. It accepts a sequence number and a UMat frame
     * to pass to the consumer.
     * @param seq The sequence number of the frame.
     * @param frame The frame to pass to the consumer.
     */
    CV_EXPORTS void operator()(const uint64_t& seq, const cv::UMat& frame);
    /*!
     * Closes the SinkSource and wakes all waiting threads.
     */
    CV_EXPORTS void close();

    static cv::Ptr<SinkSource> make(cv::Ptr<V4D> window, const string& inputFilename, const string& outputFilename);
    static cv::Ptr<SinkSource> make(cv::Ptr<V4D> window, const string& inputFilename, const string& outputFilename, int fourcc);
    static cv::Ptr<SinkSource> make(float fps);
private:
    static cv::Ptr<SinkSource> makeVaSinkSource(cv::Ptr<V4D> window, const string& inputFilename, const string& outputFilename, const int fourcc, const int vaDeviceIndex);
    static cv::Ptr<SinkSource> makeAnyHWSinkSource(const string& inputFilename, const string& outputFilename, const int fourcc);
};

} /* namespace v4d */
} /* namespace cv */

#endif /* SRC_OPENCV_V4D_SINKSOURCE_HPP_ */
