// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
//
// Prelude for the harnesses that reach into the pipeline's private sections
// with `#define private public`. Pulling the standard library and the OpenCV
// headers in *first* keeps the macro from rewriting access specifiers inside
// them: they are all include-guarded, so the pipeline header's own includes
// become no-ops and nothing of theirs is seen through the macro.
#ifndef OPENCV_V4D_TOOLS_SKELETAL_TRACKER_PRELUDE_HPP
#define OPENCV_V4D_TOOLS_SKELETAL_TRACKER_PRELUDE_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/geometry/2d.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
// For add_asset_search_paths(), so the harnesses resolve the pose models and
// the bundled clips exactly the way the sample does.
#include <opencv2/v4d/v4d.hpp>

#endif  // OPENCV_V4D_TOOLS_SKELETAL_TRACKER_PRELUDE_HPP