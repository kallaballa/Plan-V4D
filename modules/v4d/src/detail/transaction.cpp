#include "../../include/opencv2/v4d/detail/transaction.hpp"

#include <functional>
#include <opencv2/core.hpp>
#include <tuple>
#include <type_traits>
#include <utility>

namespace cv {
namespace v4d {
Transaction::Transaction() : btype_(BranchType::NONE) {}

bool Transaction::isBranch() { return btype_ != BranchType::NONE; }

void Transaction::setBranchType(BranchType::Enum btype) { btype_ = btype; }

BranchType::Enum Transaction::getBranchType() { return btype_; }

void Transaction::setContextCallback(
    std::function<cv::Ptr<cv::v4d::detail::V4DContext>()> cb) {
  ctxCallback_ = cb;
}

std::function<cv::Ptr<cv::v4d::detail::V4DContext>()>
Transaction::getContextCallback() {
  return ctxCallback_;
}

} // namespace v4d
} // namespace cv
