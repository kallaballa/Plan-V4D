// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
#ifndef OPENCV_PLAN_FLAGS_HPP_
#define OPENCV_PLAN_FLAGS_HPP_

#include <type_traits>

namespace cv {
namespace plan {

/*!
 * Debug flags controlling the runtime behavior of Plan.
 * (Graphics related flags of the former V4D runtime have been removed.)
 */
struct DebugFlags {
    enum Enum {
        DEFAULT = 0,
        PRINT_CONTROL_FLOW = 2,
        PRINT_LOCK_CONTENTION = 8,
        MONITOR_RUNTIME_PROPERTIES = 16,
        LOWER_WORKER_PRIORITY = 32,
        DONT_PAUSE_LOG = 64
    };
};

inline DebugFlags::Enum operator&(const DebugFlags::Enum& lhs, const DebugFlags::Enum& rhs) {
    return static_cast<DebugFlags::Enum>(static_cast<int>(lhs) & static_cast<int>(rhs));
}

inline DebugFlags::Enum operator|(const DebugFlags::Enum& lhs, const DebugFlags::Enum& rhs) {
    return static_cast<DebugFlags::Enum>(static_cast<int>(lhs) | static_cast<int>(rhs));
}

} /* namespace plan */
} /* namespace cv */
#endif /* OPENCV_PLAN_FLAGS_HPP_ */
