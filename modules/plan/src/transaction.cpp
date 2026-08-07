// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
#include "../include/opencv2/plan/detail/transaction.hpp"
#include <tuple>
#include <functional>
#include <utility>
#include <type_traits>
namespace cv {
namespace plan {
Transaction::Transaction() : btype_(BranchType::NONE) {
}
bool Transaction::isBranch() {
    return btype_ != BranchType::NONE;
}
void Transaction::setBranchType(BranchType::Enum btype) {
    btype_ = btype;
}
BranchType::Enum Transaction::getBranchType() {
    return btype_;
}
void Transaction::setContextCallback(std::function<Ptr<cv::plan::detail::PlanContext>()> cb) {
    ctxCallback_ = cb;
}
std::function<Ptr<detail::PlanContext>()> Transaction::getContextCallback() {
    return ctxCallback_;
}
} // namespace plan
} // namespace cv
