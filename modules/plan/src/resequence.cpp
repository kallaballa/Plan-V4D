// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
#include "opencv2/plan/detail/resequence.hpp"

#include <opencv2/core/utils/logger.hpp>

namespace cv {
namespace plan {

void Resequence::finish() {
    std::lock_guard lock(mtx_);
    finish_ = true;
    cv_.notify_all();
}

void Resequence::waitFor(const uint64_t& seq, std::function<void(uint64_t)> completion) {
    while(true) {
        {
            std::lock_guard lock(mtx_);
            if(finish_)
                break;
            if(seq == nextSeq_) {
                ++nextSeq_;
                completion(seq);
                cv_.notify_all();
                break;
            }
        }
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this, seq](){ return seq == nextSeq_ || finish_;});
    }
}

} /* namespace plan */
} /* namespace cv */
