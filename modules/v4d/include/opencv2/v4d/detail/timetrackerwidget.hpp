// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#ifndef SRC_OPENCV_TIME_TRACKER_WIDGET_HPP_
#define SRC_OPENCV_TIME_TRACKER_WIDGET_HPP_

#include "timetracker.hpp"
#include <opencv2/core/cvdef.h>
#include <string>

/*!
 * The on-screen view of a #TimeTracker: a sortable table of the tracked
 * sections with their per-call and per-frame timings.
 *
 * It is drawn by the display thread of a V4D window while `GlobalState::Keys::
 * TIME_TRACKER` is true - i.e. the widget is a view of the property, and the
 * property is what enables it. Closing the window clears that property, which
 * also stops the measurements the workers would take for it; to get the widget
 * back, set the property to true again (e.g. in `setup()`).
 *
 * The instance has to outlive the call, because it owns the state of the filter
 * field - one per window.
 */
class CV_EXPORTS TimeTrackerWidget {
  // Filter text of the name column. Kept here rather than in a local so that
  // what the user typed survives the frame in which they typed it.
  char filter_[128] = {};
  string tableId_;

public:
  TimeTrackerWidget();
  /*!
   * Draws the widget into the current ImGui context. Returns false if the
   * user closed the window, in which case the caller should disable the
   * `TIME_TRACKER` property.
   */
  CV_EXPORTS bool draw(TimeTracker &tracker);
};

#endif /* SRC_OPENCV_TIME_TRACKER_WIDGET_HPP_ */