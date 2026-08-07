// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
#include "opencv2/plan/detail/timetracker.hpp"

namespace cv {
namespace plan {

TimeTracker* TimeTracker::instance_;

TimeTracker::TimeTracker() : enabled_(false) {
}

TimeTracker::~TimeTracker() {
}

} /* namespace plan */
} /* namespace cv */
