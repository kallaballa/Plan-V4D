// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
#include "opencv2/plan/transaction.hpp"

namespace plan {

Transaction::Transaction() : btype_(BranchType::NONE) {}
bool Transaction::isBranch() { return btype_ != BranchType::NONE; }
void Transaction::setBranchType(BranchType::Enum btype) { btype_ = btype; }
BranchType::Enum Transaction::getBranchType() { return btype_; }
void Transaction::setContextCallback(std::function<Ptr<Context>()> cb) { ctxCallback_ = cb; }
std::function<Ptr<Context>()> Transaction::getContextCallback() { return ctxCallback_; }

} // namespace plan

