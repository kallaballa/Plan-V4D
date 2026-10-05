// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#include "dinov3-marker-database.hpp"

#include <opencv2/core/persistence.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

namespace cv {
namespace samples {
namespace dinov3 {

namespace {

const char *kFormatNode = "dinov3_marker_db";
const int kFormatVersion = 1;

// Wall-clock milliseconds since the epoch, for the "when was this marker
// registered" field. getTickCount() would measure uptime instead, which is
// meaningless once the value has been written to a file and read back later.
int64_t nowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

// The descriptors are stored as one row per keypoint, matching points[i].
Mat pointsToMat(const std::vector<Point2f> &points) {
  Mat m((int)points.size(), 2, CV_32F);
  for (size_t i = 0; i < points.size(); ++i) {
    m.at<float>((int)i, 0) = points[i].x;
    m.at<float>((int)i, 1) = points[i].y;
  }
  return m;
}

std::vector<Point2f> matToPoints(const Mat &m) {
  std::vector<Point2f> points;
  if (m.dims != 2 || m.cols != 2 || m.rows <= 0)
    return points;
  points.resize(m.rows);
  for (int i = 0; i < m.rows; ++i)
    points[i] = Point2f(m.at<float>(i, 0), m.at<float>(i, 1));
  return points;
}

} // namespace

int MarkerDatabase::indexOf(const std::string &name) const {
  for (size_t i = 0; i < markers_.size(); ++i)
    if (markers_[i].name == name)
      return (int)i;
  return -1;
}

MarkerRecord &MarkerDatabase::recordAtLocked(size_t index) {
  return markers_[index];
}

bool MarkerDatabase::add(const std::string &name, const Mat &embedding,
                         const Mat &thumbnail, const LocalFeatures &features,
                         const Size2f &cropSize, const DenseReference &dense,
                         MarkerRecord &record, std::string &error) {
  if (name.empty()) {
    error = "a marker needs a name";
    return false;
  }
  if (embedding.empty() || embedding.cols < 1) {
    error = "the marker has no embedding";
    return false;
  }

  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (indexOf(name) >= 0) {
    error = "'" + name + "' is already registered -- update or remove it first";
    return false;
  }
  if (markers_.empty())
    dim_ = embedding.cols;
  else if (embedding.cols != dim_) {
    error = "the embedding has " + std::to_string(embedding.cols) +
            " dimensions, the database stores " + std::to_string(dim_);
    return false;
  }

  MarkerRecord entry;
  entry.id = nextId_++;
  entry.name = name;
  entry.embedding = embedding.reshape(1, 1).clone();
  entry.thumbnail = thumbnail.empty() ? Mat() : thumbnail.clone();
  entry.features = features;
  entry.cropSize = cropSize;
  entry.dense = dense;
  if (!entry.dense.patches.empty())
    entry.dense.patches = entry.dense.patches.clone();
  entry.createdAt = nowMs();
  markers_.push_back(std::move(entry));
  record = markers_.back();
  return true;
}

bool MarkerDatabase::update(const std::string &name, const Mat &embedding,
                            const Mat &thumbnail, const LocalFeatures &features,
                            const Size2f &cropSize, const DenseReference &dense,
                            UpdateMode mode, int &id, std::string &error) {
  if (embedding.empty() || embedding.cols < 1) {
    error = "the marker has no embedding";
    return false;
  }

  std::unique_lock<std::shared_mutex> lock(mutex_);
  int index = -1;
  if (mode == UpdateMode::Nearest) {
    double best = -2.0;
    for (size_t i = 0; i < markers_.size(); ++i) {
      if (markers_[i].embedding.cols != embedding.cols)
        continue;
      const double score = markers_[i].embedding.reshape(1, 1)
                               .dot(embedding.reshape(1, 1));
      if (score > best) {
        best = score;
        index = (int)i;
      }
    }
    if (index < 0) {
      error = "there is nothing to update yet";
      return false;
    }
  } else {
    index = indexOf(name);
    if (index < 0) {
      error = "no marker named '" + name + "'";
      return false;
    }
  }

  MarkerRecord &entry = recordAtLocked((size_t)index);
  entry.embedding = embedding.reshape(1, 1).clone();
  if (!thumbnail.empty())
    entry.thumbnail = thumbnail.clone();
  if (!features.empty()) {
    entry.features = features;
    entry.cropSize = cropSize;
  }
  // The dense reference travels with the local features: both are derived from
  // the same crop, so replacing one without the other would leave a marker whose
  // two localisers disagree about what it looks like.
  if (!dense.empty()) {
    entry.dense = dense;
    entry.dense.patches = dense.patches.clone();
  }
  entry.createdAt = nowMs();
  id = entry.id;
  return true;
}

bool MarkerDatabase::remove(const std::string &name, UpdateMode mode, int &id,
                            std::string &error) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  int index = -1;
  if (mode == UpdateMode::Nearest) {
    // Without an embedding to compare against, "nearest" degenerates to the
    // most recently added marker, which is what the HUD selects after a match.
    if (!markers_.empty())
      index = (int)markers_.size() - 1;
  } else {
    index = indexOf(name);
  }
  if (index < 0) {
    error = markers_.empty() ? "the database is empty" : "no marker named '" + name + "'";
    return false;
  }
  id = markers_[(size_t)index].id;
  markers_.erase(markers_.begin() + index);
  if (markers_.empty())
    dim_ = 0;
  return true;
}

bool MarkerDatabase::clear(std::string &error) {
  (void)error;
  std::unique_lock<std::shared_mutex> lock(mutex_);
  markers_.clear();
  dim_ = 0;
  nextId_ = 1;
  return true;
}

MatchResult MarkerDatabase::search(const Mat &embedding, float minScore) const {
  MatchResult result;
  if (embedding.empty())
    return result;

  std::shared_lock<std::shared_mutex> lock(mutex_);
  const Mat query = embedding.reshape(1, 1);
  for (size_t i = 0; i < markers_.size(); ++i) {
    const MarkerRecord &entry = markers_[i];
    if (entry.embedding.cols != query.cols)
      continue;
    const float score = entry.embedding.reshape(1, 1).dot(query);
    if (score >= minScore && score > result.score) {
      result.index = (int)i;
      result.id = entry.id;
      result.name = entry.name;
      result.score = score;
    }
  }
  return result;
}

size_t MarkerDatabase::size() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return markers_.size();
}

int MarkerDatabase::dim() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return dim_;
}

std::vector<std::string> MarkerDatabase::names() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  std::vector<std::string> out;
  out.reserve(markers_.size());
  for (const MarkerRecord &entry : markers_)
    out.push_back(entry.name);
  return out;
}

bool MarkerDatabase::record(const std::string &name, MarkerRecord &out) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  const int index = indexOf(name);
  if (index < 0)
    return false;
  out = markers_[(size_t)index];
  return true;
}

bool MarkerDatabase::thumbnail(const std::string &name, Mat &out) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  const int index = indexOf(name);
  if (index < 0)
    return false;
  out = markers_[(size_t)index].thumbnail;
  return !out.empty();
}

bool MarkerDatabase::load(const std::string &path, std::string &error) {
  FileStorage fs;
  if (!fs.open(path, FileStorage::READ)) {
    error = "cannot read the marker database '" + path + "'";
    return false;
  }

  FileNode root = fs[kFormatNode];
  if (root.empty()) {
    error = "'" + path + "' is not a DINOv3 marker database";
    return false;
  }
  const int version = (int)root["version"];
  if (version != kFormatVersion) {
    error = "marker database version " + std::to_string(version) +
            " (expected " + std::to_string(kFormatVersion) + ")";
    return false;
  }

  std::vector<MarkerRecord> loaded;
  FileNode list = root["markers"];
  for (FileNodeIterator it = list.begin(); it != list.end(); ++it) {
    MarkerRecord entry;
    entry.id = (int)(*it)["id"];
    entry.name = (std::string)(*it)["name"];
    (*it)["embedding"] >> entry.embedding;
    (*it)["thumbnail"] >> entry.thumbnail;
    (*it)["created_at"] >> entry.createdAt;
    Size2f cropSize;
    (*it)["crop_size"] >> cropSize;
    entry.cropSize = cropSize;

    Mat points;
    (*it)["keypoints"] >> points;
    entry.features.points = matToPoints(points);
    (*it)["descriptors"] >> entry.features.descriptors;

    // Dense tokens are optional: a database written before they existed still
    // loads, it just falls back to local-feature verification.
    (*it)["patches"] >> entry.dense.patches;
    (*it)["patch_grid"] >> entry.dense.grid;
    (*it)["roi_size"] >> entry.dense.roiSize;

    if (entry.name.empty() || entry.embedding.empty()) {
      continue; // a record without a name or descriptor is not usable
    }
    loaded.push_back(std::move(entry));
  }
  fs.release();

  std::unique_lock<std::shared_mutex> lock(mutex_);
  markers_ = std::move(loaded);
  dim_ = markers_.empty() ? 0 : markers_.front().embedding.cols;
  nextId_ = 1;
  for (const MarkerRecord &entry : markers_)
    nextId_ = std::max(nextId_, entry.id + 1);
  return true;
}

bool MarkerDatabase::save(const std::string &path, std::string &error) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  FileStorage fs;
  try {
    fs.open(path, FileStorage::WRITE);
  } catch (const std::exception &e) {
    error = std::string("cannot write the marker database: ") + e.what();
    return false;
  }
  if (!fs.isOpened()) {
    error = "cannot open '" + path + "' for writing";
    return false;
  }

  fs << kFormatNode << "{";
  fs << "version" << kFormatVersion;
  fs << "dim" << dim_;
  fs << "markers" << "[";
  for (const MarkerRecord &entry : markers_) {
    fs << "{";
    fs << "id" << entry.id;
    fs << "name" << entry.name;
    fs << "created_at" << entry.createdAt;
    fs << "crop_size" << Size2f(entry.cropSize);
    fs << "embedding" << entry.embedding;
    if (!entry.thumbnail.empty())
      fs << "thumbnail" << entry.thumbnail;
    if (!entry.features.empty()) {
      fs << "keypoints" << pointsToMat(entry.features.points);
      fs << "descriptors" << entry.features.descriptors;
    }
    if (!entry.dense.empty()) {
      fs << "patches" << entry.dense.patches;
      fs << "patch_grid" << entry.dense.grid;
      fs << "roi_size" << entry.dense.roiSize;
    }
    fs << "}";
  }
  fs << "]";
  fs << "}";
  fs.release();
  return true;
}

std::string MarkerDatabase::describe() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  std::ostringstream os;
  os << markers_.size() << (markers_.size() == 1 ? " marker" : " markers");
  if (dim_ > 0)
    os << ", " << dim_ << "-d";
  return os.str();
}

} // namespace dinov3
} // namespace samples
} // namespace cv