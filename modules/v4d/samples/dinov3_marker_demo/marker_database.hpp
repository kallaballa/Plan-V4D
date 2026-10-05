#ifndef MARKER_DATABASE_HPP
#define MARKER_DATABASE_HPP

#include "dinov3_embedder.hpp"

#include <opencv2/core.hpp>
#include <opencv2/features.hpp>
#include <string>
#include <vector>
#include <shared_mutex>
#include <mutex>

// One registered marker.
//
// The embedding and the patch grid are two views of the same crop, which is the
// point of storing them together: the embedding decides *which* marker this is,
// and the patch grid decides *where* it is. Registering them from different
// images would let those two answers describe different things.
struct MarkerRecord {
    int id = -1;
    std::string name;

    // Downscaled copy of the registered crop, kept for the panel and for a human
    // recognising what they registered. Localisation does not read it -- the
    // patch grid's own `area` is the coordinate system.
    cv::Mat thumbnail;

    std::vector<float> embedding;

    // DINOv3 patch tokens of the registered crop. `grid.area` is the pixel size
    // of the crop the tokens were computed over, so grid.centre(i) is directly a
    // point in the marker's own image.
    PatchGrid grid;

    // ORB fallback for when no patch grid is available. See GeometricVerifier:
    // the dense path is the one that works from a distance and at a steep angle.
    std::vector<cv::KeyPoint> keypoints;
    cv::Mat descriptors;

    int64_t updatedAt = 0;
};

class MarkerDatabase {
public:
    int addMarker(
        const std::string& name,
        const cv::Mat& thumbnail,
        const std::vector<float>& embedding,
        const PatchGrid& grid
    );

    bool updateNearest(
        const std::vector<float>& queryEmbedding,
        const cv::Mat& thumbnail,
        const PatchGrid& grid,
        float minScore,
        std::string& updatedName
    );

    bool search(
        const std::vector<float>& queryEmbedding,
        float threshold,
        int& bestId,
        float& bestScore,
        std::string& bestName
    ) const;

    bool getMarker(int id, MarkerRecord& out) const;

    void clear();
    size_t size() const;

    bool save(const std::string& path, std::string& error) const;
    bool load(const std::string& path, std::string& error);

private:
    // The local features are derived from the stored crop, so registration,
    // update and load cannot disagree about what the marker's image is.
    static void deriveLocalFeatures(MarkerRecord& record);

    mutable std::shared_mutex mutex_;
    std::vector<MarkerRecord> markers_;
    int nextId_ = 0;
};

#endif // MARKER_DATABASE_HPP
