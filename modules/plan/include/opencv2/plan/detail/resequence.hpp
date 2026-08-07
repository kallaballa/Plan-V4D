#ifndef MODULES_PLAN_INCLUDE_OPENCV2_PLAN_DETAIL_RESEQUENCE_HPP_
#define MODULES_PLAN_INCLUDE_OPENCV2_PLAN_DETAIL_RESEQUENCE_HPP_
#include <functional>
#include <set>
#include <cstdint>
#include "../base.hpp"
#include <mutex>
#include <semaphore>
#include <condition_variable>
namespace cv {
namespace plan {
class PLAN_EXPORTS Resequence {
    bool finish_ = false;
    std::mutex mtx_;
    std::condition_variable cv_;
    uint64_t nextSeq_ = 0;
public:
    PLAN_EXPORTS Resequence(int firstSequenceNumber) : nextSeq_(firstSequenceNumber) {
    }
    PLAN_EXPORTS virtual ~Resequence() {}
    PLAN_EXPORTS void finish();
    PLAN_EXPORTS void waitFor(const uint64_t& seq, std::function<void(uint64_t)> completion);
};
} /* namespace plan */
} /* namespace cv */
#endif /* MODULES_PLAN_INCLUDE_OPENCV2_PLAN_DETAIL_RESEQUENCE_HPP_ */
