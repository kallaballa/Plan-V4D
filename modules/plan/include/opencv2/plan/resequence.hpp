#ifndef OPENCV_PLAN_RESEQUENCE_HPP_
#define OPENCV_PLAN_RESEQUENCE_HPP_

#include <functional>
#include <mutex>
#include <condition_variable>
#include <opencv2/core/cvdef.h>

namespace cv {
namespace plan {

class CV_EXPORTS Resequence {
    bool finish_ = false;
    std::mutex mtx_;
    std::condition_variable cv_;
    uint64_t nextSeq_ = 0;

public:
    CV_EXPORTS Resequence(int firstSequenceNumber) : nextSeq_(firstSequenceNumber) {}
    CV_EXPORTS virtual ~Resequence() {}
    CV_EXPORTS void finish();
    CV_EXPORTS void waitFor(const uint64_t& seq, std::function<void(uint64_t)> completion);
};

} // namespace plan
} // namespace cv

#endif // OPENCV_PLAN_RESEQUENCE_HPP_

