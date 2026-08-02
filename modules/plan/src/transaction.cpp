#include "opencv2/plan/transaction.hpp"

namespace cv {
namespace plan {

Transaction::Transaction() : btype_(BranchType::NONE) {}

bool Transaction::isBranch() {
    return btype_ != BranchType::NONE;
}

void Transaction::setBranchType(BranchType::Enum btype) {
    btype_ = btype;
}

BranchType::Enum Transaction::getBranchType() {
    return btype_;
}

void Transaction::setContextCallback(std::function<cv::Ptr<cv::plan::detail::PlanContext>()> cb) {
    ctxCallback_ = cb;
}

std::function<cv::Ptr<cv::plan::detail::PlanContext>()> Transaction::getContextCallback() {
    return ctxCallback_;
}

} // namespace plan
} // namespace cv

