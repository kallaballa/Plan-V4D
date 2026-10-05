// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The marker database of the DINOv3 Marker Demo.
//
// One record per registered marker:
//
//   name        what the user typed into the GUI
//   embedding   1 x 768 CV_32F, the DINOv3 descriptor of its RoI, L2-normalised
//   thumbnail   a small BGR crop of the RoI, drawn in the HUD
//   features    ORB/SIFT keypoints and descriptors of the same crop, for the
//               geometric verification pass
//
// Recognition is a plain linear scan: the database is expected to hold tens of
// markers, not millions, and a dot product over 768 floats per marker is far
// cheaper than an approximate index. Both descriptors are unit vectors, so that
// dot product is a cosine similarity.
//
// Every public method takes the shared lock, because the plan's worker thread
// writes this while the GUI thread reads it for the marker list.

#ifndef OPENCV_DINOV3_MARKER_DATABASE_HPP
#define OPENCV_DINOV3_MARKER_DATABASE_HPP

#include <opencv2/core.hpp>

#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

namespace cv {
namespace samples {
namespace dinov3 {

// Keypoints of the stored marker crop, in that crop's own coordinates, plus the
// descriptors that go with them (row i describes points[i]).
struct LocalFeatures {
  std::vector<Point2f> points;
  Mat descriptors;

  bool empty() const { return points.empty() || descriptors.empty(); }
  int count() const { return (int)points.size(); }
};

// The dense counterpart of LocalFeatures: the DINOv3 patch tokens of the marker's
// own crop. Row i describes the patch at grid position i, row-major.
//
// roiSize is the region of interest in the marker's own frame, in pixels, which
// is what turns a grid index back into a point. It is stored separately from
// MarkerRecord::cropSize because that one is the *detector's* downscaled working
// size: the two differ, and mixing them up puts the outline in the wrong place.
struct DenseReference {
  Mat patches; // N x dim, CV_32F, each row L2-normalised
  Size grid = Size(0, 0);
  Size2f roiSize;

  bool empty() const {
    return patches.empty() || grid.width <= 0 || grid.height <= 0 ||
           roiSize.width <= 0 || roiSize.height <= 0;
  }
  int count() const { return patches.rows; }
};

struct MarkerRecord {
  int id = -1;
  std::string name;
  Mat embedding; // 1 x dim, CV_32F
  Mat thumbnail; // BGR
  LocalFeatures features;
  DenseReference dense;
  int64_t createdAt = 0; // ms since the epoch

  // The crop rectangle the keypoints were measured in, so that verification
  // can map the marker's four corners into the current frame.
  Size2f cropSize;
};

struct MatchResult {
  int index = -1;
  int id = -1;
  std::string name;
  float score = 0.0f;

  bool valid() const { return index >= 0; }
};

enum class UpdateMode {
  ByName,  // replace the record with this exact name
  Nearest  // replace the record whose embedding is closest to this one
};

class MarkerDatabase {
public:
  // Rejects an empty name, an empty embedding or a duplicate name. The dense
  // reference is optional: a record without one can still be recognised by its
  // embedding and verified with local features.
  bool add(const std::string &name, const Mat &embedding, const Mat &thumbnail,
           const LocalFeatures &features, const Size2f &cropSize,
           const DenseReference &dense, MarkerRecord &record,
           std::string &error);

  // Replaces an existing record. With UpdateMode::Nearest the record is picked
  // by cosine similarity, which is how the GUI's "update nearest" button works:
  // the user holds the marker they want to replace in the RoI and presses it.
  bool update(const std::string &name, const Mat &embedding,
              const Mat &thumbnail, const LocalFeatures &features,
              const Size2f &cropSize, const DenseReference &dense,
              UpdateMode mode, int &id, std::string &error);

  bool remove(const std::string &name, UpdateMode mode, int &id,
              std::string &error);
  bool clear(std::string &error);

  // The best match at or above minScore, or an invalid result.
  MatchResult search(const Mat &embedding, float minScore) const;

  size_t size() const;
  int dim() const;
  std::vector<std::string> names() const;
  bool record(const std::string &name, MarkerRecord &out) const;
  bool thumbnail(const std::string &name, Mat &out) const;

  // Reads and writes a FileStorage YAML document (.yml or .yml.gz). load()
  // replaces the whole database; a missing file is an error, an empty one is
  // not.
  bool load(const std::string &path, std::string &error);
  bool save(const std::string &path, std::string &error) const;

  std::string describe() const;

private:
  int indexOf(const std::string &name) const;
  MarkerRecord &recordAtLocked(size_t index);

  mutable std::shared_mutex mutex_;
  std::vector<MarkerRecord> markers_;
  int nextId_ = 1;
  int dim_ = 0;
};

} // namespace dinov3
} // namespace samples
} // namespace cv

#endif // OPENCV_DINOV3_MARKER_DATABASE_HPP