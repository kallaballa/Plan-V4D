// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
#ifndef OPENCV_PLAN_DETAIL_RESEQUENCE_HPP_
#define OPENCV_PLAN_DETAIL_RESEQUENCE_HPP_

#include <functional>
#include <set>
#include <opencv2/core/cvdef.h>
#include <opencv2/core/mat.hpp>
#include <mutex>
#include <semaphore>
#include <condition_variable>

namespace cv {
namespace plan {

/*!
 * Ensures that out-of-order (parallel) results are consumed in sequence order.
 */
class CV_EXPORTS Resequence {
    bool finish_ = false;
    std::mutex mtx_;
    std::condition_variable cv_;
    uint64_t nextSeq_ = 0;

public:
    CV_EXPORTS Resequence(int firstSequenceNumber) : nextSeq_(firstSequenceNumber) {
    }

    CV_EXPORTS virtual ~Resequence() {}

    CV_EXPORTS void finish();

    CV_EXPORTS void waitFor(const uint64_t& seq, std::function<void(uint64_t)> completion);
};

} /* namespace plan */
} /* namespace cv */
#endif /* OPENCV_PLAN_DETAIL_RESEQUENCE_HPP_ */
