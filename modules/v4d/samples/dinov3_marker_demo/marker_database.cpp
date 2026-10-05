#include "marker_database.hpp"
#include <opencv2/imgproc.hpp>
#include <opencv2/features.hpp>
#include <opencv2/core/persistence.hpp>
#include <cmath>
#include <limits>
#include <algorithm>
#include <chrono>

static float cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || b.empty() || a.size() != b.size()) return -1.0f;
    float dot = 0.0f;
    float na = 0.0f;
    float nb = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    if (na < 1e-12f || nb < 1e-12f) return -1.0f;
    return dot / (std::sqrt(na) * std::sqrt(nb));
}

void MarkerDatabase::deriveLocalFeatures(MarkerRecord& record) {
    record.keypoints.clear();
    record.descriptors.release();
    if (record.thumbnail.empty())
        return;
    cv::Mat gray;
    cv::cvtColor(record.thumbnail, gray, cv::COLOR_BGR2GRAY);
    // Enough keypoints to survive the marker being small or oblique, but capped:
    // the fallback matcher is O(marker x frame) in descriptors.
    cv::Ptr<cv::Feature2D> detector = cv::ORB::create(4000, 1.2f, 8, 31, 0, 2,
                                                      cv::ORB::HARRIS_SCORE, 31, 20);
    detector->detectAndCompute(gray, cv::noArray(), record.keypoints, record.descriptors);
}

int MarkerDatabase::addMarker(
    const std::string& name,
    const cv::Mat& thumbnail,
    const std::vector<float>& embedding,
    const PatchGrid& grid
) {
    std::unique_lock lock(mutex_);
    MarkerRecord rec;
    rec.id = nextId_++;
    rec.name = name;
    rec.thumbnail = thumbnail.clone();
    if (rec.thumbnail.channels() == 4) {
        cv::cvtColor(rec.thumbnail, rec.thumbnail, cv::COLOR_BGRA2BGR);
    }
    rec.embedding = embedding;
    rec.grid = grid;
    rec.updatedAt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    deriveLocalFeatures(rec);
    markers_.push_back(std::move(rec));
    return markers_.back().id;
}

bool MarkerDatabase::updateNearest(
    const std::vector<float>& queryEmbedding,
    const cv::Mat& thumbnail,
    const PatchGrid& grid,
    float minScore,
    std::string& updatedName
) {
    std::unique_lock lock(mutex_);
    if (markers_.empty()) return false;
    int bestIdx = -1;
    float bestScore = -1.0f;
    for (size_t i = 0; i < markers_.size(); ++i) {
        float s = cosineSimilarity(queryEmbedding, markers_[i].embedding);
        if (s > bestScore) {
            bestScore = s;
            bestIdx = static_cast<int>(i);
            if (bestScore > 0.99f) break;
        }
    }
    if (bestIdx < 0 || bestScore < minScore) return false;
    auto& rec = markers_[bestIdx];
    updatedName = rec.name;
    rec.thumbnail = thumbnail.clone();
    if (rec.thumbnail.channels() == 4) {
        cv::cvtColor(rec.thumbnail, rec.thumbnail, cv::COLOR_BGRA2BGR);
    }
    rec.embedding = queryEmbedding;
    rec.grid = grid;
    rec.updatedAt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    deriveLocalFeatures(rec);
    return true;
}

bool MarkerDatabase::search(
    const std::vector<float>& queryEmbedding,
    float threshold,
    int& bestId,
    float& bestScore,
    std::string& bestName
) const {
    std::shared_lock lock(mutex_);
    if (markers_.empty()) return false;
    bestId = -1;
    bestScore = -1.0f;
    bestName.clear();
    for (const auto& rec : markers_) {
        float s = cosineSimilarity(queryEmbedding, rec.embedding);
        if (s > bestScore) {
            bestScore = s;
            bestId = rec.id;
            bestName = rec.name;
            if (bestScore > 0.99f) break;
        }
    }
    if (bestScore < threshold) {
        bestId = -1;
        bestName.clear();
        return false;
    }
    return true;
}

bool MarkerDatabase::getMarker(int id, MarkerRecord& out) const {
    std::shared_lock lock(mutex_);
    for (const auto& rec : markers_) {
        if (rec.id == id) {
            out = rec;
            return true;
        }
    }
    return false;
}

void MarkerDatabase::clear() {
    std::unique_lock lock(mutex_);
    markers_.clear();
    nextId_ = 0;
}

size_t MarkerDatabase::size() const {
    std::shared_lock lock(mutex_);
    return markers_.size();
}

bool MarkerDatabase::save(const std::string& path, std::string& error) const {
    try {
        cv::FileStorage fs(path, cv::FileStorage::WRITE | cv::FileStorage::FORMAT_YAML);
        if (!fs.isOpened()) {
            error = "Cannot open file for writing: " + path;
            return false;
        }
        fs << "markers" << "[";
        for (const auto& rec : markers_) {
            fs << "{";
            fs << "id" << rec.id;
            fs << "name" << rec.name;
            fs << "thumbnail" << rec.thumbnail;
            cv::Mat embMat(rec.embedding.size(), 1, CV_32F, const_cast<float*>(rec.embedding.data()));
            fs << "embedding" << embMat;
            // The patch grid is what makes the outline stable across viewpoints,
            // so it is part of the marker rather than something re-derived from
            // the thumbnail. Note that YAML stores the descriptor as
            // rows x dim in the file and read() gives back dim x rows, which
            // reshape() fixes.
            fs << "patchDim" << static_cast<int>(rec.grid.descriptors.cols);
            fs << "patchCols" << rec.grid.shape.width;
            fs << "patchRows" << rec.grid.shape.height;
            fs << "patchAreaW" << rec.grid.area.width;
            fs << "patchAreaH" << rec.grid.area.height;
            cv::Mat patches = rec.grid.descriptors;
            fs << "patches" << patches;
            fs << "}";
        }
        fs << "]";
        fs << "nextId" << nextId_;
        fs.release();
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool MarkerDatabase::load(const std::string& path, std::string& error) {
    try {
        std::unique_lock lock(mutex_);
        markers_.clear();
        nextId_ = 0;
        cv::FileStorage fs(path, cv::FileStorage::READ);
        if (!fs.isOpened()) {
            error = "Cannot open file for reading: " + path;
            return false;
        }
        cv::FileNode markersNode = fs["markers"];
        if (markersNode.type() == cv::FileNode::SEQ) {
            for (const auto& mnode : markersNode) {
                MarkerRecord rec;
                mnode["id"] >> rec.id;
                mnode["name"] >> rec.name;
                mnode["thumbnail"] >> rec.thumbnail;
                cv::Mat embMat;
                mnode["embedding"] >> embMat;
                if (embMat.isContinuous() && embMat.type() == CV_32F) {
                    rec.embedding.assign(embMat.ptr<float>(), embMat.ptr<float>() + embMat.total());
                }

                int patchDim = 0, patchCols = 0, patchRows = 0;
                float areaW = 0.f, areaH = 0.f;
                mnode["patchDim"] >> patchDim;
                mnode["patchCols"] >> patchCols;
                mnode["patchRows"] >> patchRows;
                mnode["patchAreaW"] >> areaW;
                mnode["patchAreaH"] >> areaH;
                cv::Mat patches;
                mnode["patches"] >> patches;
                if (patchDim > 0 && patchCols > 0 && patchRows > 0 &&
                    patches.type() == CV_32F && !patches.empty()) {
                    rec.grid.shape = cv::Size(patchCols, patchRows);
                    rec.grid.descriptors = patches.reshape(1, patchRows * patchCols);
                    // A file written before the grid was stored keeps its tokens
                    // but not their pixel area; the thumbnail is a scaled copy of
                    // the same crop, so its size is the right answer.
                    rec.grid.area = (areaW > 0.0f && areaH > 0.0f)
                        ? cv::Size2f(areaW, areaH)
                        : cv::Size2f(static_cast<float>(rec.thumbnail.cols),
                                     static_cast<float>(rec.thumbnail.rows));
                    if (rec.grid.descriptors.cols != patchDim ||
                        rec.grid.descriptors.rows != patchRows * patchCols) {
                        rec.grid.descriptors.release();
                        rec.grid.shape = cv::Size(0, 0);
                    }
                }

                rec.updatedAt = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                deriveLocalFeatures(rec);
                markers_.push_back(std::move(rec));
                if (rec.id >= nextId_) nextId_ = rec.id + 1;
            }
        }
        fs["nextId"] >> nextId_;
        if (nextId_ < 0) nextId_ = static_cast<int>(markers_.size());
        fs.release();
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
