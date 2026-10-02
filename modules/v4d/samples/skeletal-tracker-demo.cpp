// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
//
// Skeletal Tracker Demo — real-time multi-person pose estimation with
// MediaPipe BlazePose (OpenCV Zoo / HuggingFace) drawn as a NanoVG skeleton
// overlay, with an ImGui control panel for confidence thresholds.
//
// Models (Apache 2.0, OpenCV Zoo):
//   person_detection_mediapipe_2023mar.onnx
//   pose_estimation_mediapipe_2023mar.onnx
//   https://github.com/opencv/opencv_zoo/tree/main/models/pose_estimation_mediapipe
//   https://huggingface.co/opencv/pose_estimation_mediapipe
//
// Build: requires opencv_dnn, opencv_videoio, opencv_v4d, opencv_plan, glfw, nanovg
//
// Usage:
//   ./skeletal-tracker-demo                              # webcam
//   ./skeletal-tracker-demo <video.mp4>                  # video file
//   ./skeletal-tracker-demo <video.mp4> <out.mkv>        # record output

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <opencv2/dnn.hpp>
#include <opencv2/geometry/2d.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/v4d/v4d.hpp>
#include <opencv2/video/tracking.hpp>
#include <string>
#include <utility>
#include <vector>

using namespace cv;
using namespace cv::v4d;
using namespace cv::v4d::event;

// ---------------------------------------------------------------------------
// MediaPipe Pose 33-keypoint skeleton topology.
// ---------------------------------------------------------------------------
static const std::vector<std::pair<int, int>> kPoseSegments = {
    // Torso
    {11, 12}, {11, 23}, {23, 24}, {24, 12},
    // Left arm
    {11, 13}, {13, 15}, {15, 17}, {15, 19}, {15, 21}, {17, 19},
    // Right arm
    {12, 14}, {14, 16}, {16, 18}, {16, 20}, {16, 22}, {18, 20},
    // Left leg
    {23, 25}, {25, 27}, {27, 29}, {27, 31}, {29, 31},
    // Right leg
    {24, 26}, {26, 28}, {28, 30}, {28, 32}, {30, 32},
    // Face
    {0, 1},  {1, 2},  {2, 3},  {3, 7},
    {0, 4},  {4, 5},  {5, 6},  {6, 8},
    {9, 10},
};

// ---------------------------------------------------------------------------
// Detector anchors.
//
// The BlazePose short-range person detector is an anchor-point detector: the
// graph bakes in the cell offsets, so the 2254 anchors below are only used to
// turn the predicted (x, y) *deltas* back into absolute input coordinates.
// Layout, per detector level (cell-major, x fastest, one row per head):
//   person_8 : 28x28 cells, stride  8, offset  4, 2 heads -> 1568 rows
//   person_16: 14x14 cells, stride 16, offset  8, 2 heads ->  392 rows
//   person_32:  7x7 cells, stride 32, offset 16, 6 heads ->  294 rows
// Values are normalised to the 224x224 detector input.
// Regenerate with ./gen_skeletal_tracker_anchors.py
// ---------------------------------------------------------------------------
#include "skeletal-tracker-anchors.hpp"
// ---------------------------------------------------------------------------
// Temporal smoothing.
// ---------------------------------------------------------------------------
// The "1 Euro" filter: an exponential low-pass whose cutoff rises with the
// observed speed, so slow motion is heavily smoothed while fast motion stays
// responsive (Casiez, Roussel & Vogel, CHI 2012). Per-frame independent
// estimates of a single keypoint jitter by ~10 px, which is very visible at
// 60 Hz; this removes that without adding lag on fast motion.
class OneEuro {
public:
    void configure(float minCutoff, float beta, float derivativeCutoff) {
        minCutoff_ = minCutoff;
        beta_ = beta;
        derivativeCutoff_ = derivativeCutoff;
        started_ = false;
    }

    [[nodiscard]] float filter(float x, float dt) {
        if (!(dt > 0.f)) dt = 1.f / 30.f;
        if (!started_) {
            started_ = true;
            prev_ = x;
            prevDerivative_ = 0.f;
            return x;
        }
        const float dx = (x - prev_) / dt;
        prevDerivative_ += alpha(derivativeCutoff_, dt) * (dx - prevDerivative_);
        // Speed-adaptive cutoff: faster motion -> higher cutoff -> less lag.
        const float cutoff = minCutoff_ + beta_ * std::abs(prevDerivative_);
        prev_ += alpha(cutoff, dt) * (x - prev_);
        return prev_;
    }

private:
    static float alpha(float cutoff, float dt) {
        const float tau = 1.f / (2.f * static_cast<float>(CV_PI) * cutoff);
        return 1.f / (1.f + tau / dt);
    }

    float minCutoff_ = 1.f;
    float beta_ = 0.f;
    float derivativeCutoff_ = 1.f;
    float prev_ = 0.f;
    float prevDerivative_ = 0.f;
    bool started_ = false;
};

// ---------------------------------------------------------------------------
// Lightweight wrappers around the two ONNX models.
// ---------------------------------------------------------------------------
class MediaPipePosePipeline {
public:
    static constexpr int kNumKeypoints = 33;

    struct Params {
        float detConf_ = 0.5f;
        float poseConf_ = 0.5f;
        // Square pose RoI half-size, relative to the mid-hip -> full-body
        // distance. 1.0 is the reference default; more keeps outstretched arms
        // and feet inside the crop, at the cost of pose-input resolution.
        float roiEnlarge_ = 1.25f;
        int maxPersons_ = 2;
        bool smooth_ = true;
        // How many consecutive frames a track survives without a detection.
        int maxMissed_ = 5;
        // Source frame interval, seconds. Set from the input frame rate.
        float frameDt_ = 1.f / 60.f;
    };

    struct Person {
        int id = -1;
        cv::Rect box;
        std::vector<cv::Vec4f> keypoints; // 33 x [x, y, z, visibility]
    };

    explicit MediaPipePosePipeline(const std::string& detModel, const std::string& poseModel)
        : detNet_(dnn::readNet(detModel)),
          poseNet_(dnn::readNet(poseModel)) {
        detNet_.setPreferableBackend(dnn::DNN_BACKEND_OPENCV);
        detNet_.setPreferableTarget(dnn::DNN_TARGET_OPENCL);
        poseNet_.setPreferableBackend(dnn::DNN_BACKEND_OPENCV);
        poseNet_.setPreferableTarget(dnn::DNN_TARGET_OPENCL);
    }

    [[nodiscard]] std::vector<Person> run(const cv::Mat& frameBGR, const Params& p) {
        if (frameBGR.empty()) return {};

        // A resolution change means a different stream; stale tracks are wrong.
        if (frameSize_ != frameBGR.size()) {
            frameSize_ = frameBGR.size();
            tracks_.clear();
        }

        // Frame interval in *content* time. The estimator's jitter is per source frame,
        // so filtering against wall-clock time would make the result depend on
        // how fast the machine happens to run.
        const float dt = std::clamp(p.frameDt_, 1.f / 240.f, 1.f);

        // --- Person detection ------------------------------------------------
        const Letterbox lb(frameBGR.size());
        dnn::Image2BlobParams detPrms;
        detPrms.datalayout = cv::DNN_LAYOUT_NCHW;
        detPrms.ddepth = CV_32F;
        detPrms.mean = cv::Scalar::all(127.5f);
        detPrms.scalefactor = cv::Scalar::all(1.f / 127.5f);
        detPrms.size = cv::Size(kDetSize, kDetSize);
        detPrms.swapRB = true;
        detPrms.paddingmode = dnn::DNN_PMODE_LETTERBOX;

        detNet_.setInput(dnn::blobFromImageWithParams(frameBGR, detPrms));
        std::vector<cv::Mat> detOut;
        detNet_.forward(detOut, detNet_.getUnconnectedOutLayersNames());
        if (detOut.size() < 2) return {};

        // The tensors are [1, 2254, 1] and [1, 2254, 12]. View them as one row
        // per anchor; note that cv::Size takes cols first, so the channel count
        // is the *first* argument.
        const cv::Mat scores(cv::Size(1, kNumAnchors), CV_32F, detOut[0].ptr<float>());
        const cv::Mat reg(cv::Size(kRegChannels, kNumAnchors), CV_32F, detOut[1].ptr<float>());

        // Channels 0..1 are the (x, y) centre deltas and 2..3 the (x, y) box
        // side lengths; both are in 224-input pixels and are added to the
        // anchor. Channels 4..11 are four auxiliary keypoints as (x, y) deltas
        // on the same anchor, in order: mid-hip, full body, shoulder, upper.
        std::vector<cv::Rect2f> boxes;
        std::vector<float> confs;
        std::vector<int> rows;
        for (int i = 0; i < kNumAnchors; ++i) {
            const float conf = 1.f / (1.f + std::exp(-scores.at<float>(i, 0)));
            if (conf < p.detConf_) continue;
            const float xc = anchorX(i) + reg.at<float>(i, 0);
            const float yc = anchorY(i) + reg.at<float>(i, 1);
            const float bw = reg.at<float>(i, 2);
            const float bh = reg.at<float>(i, 3);
            const cv::Point2f tl = lb.toFrame(xc - bw * 0.5f, yc - bh * 0.5f);
            const cv::Point2f br = lb.toFrame(xc + bw * 0.5f, yc + bh * 0.5f);
            if (br.x - tl.x < 1.f || br.y - tl.y < 1.f) continue;
            boxes.emplace_back(tl, cv::Point2f(br.x - tl.x, br.y - tl.y));
            confs.push_back(conf);
            rows.push_back(i);
        }

        std::vector<int> keep;
        if (!boxes.empty()) {
            // NMSBoxes works in integral rectangles, so hand it a converted copy.
            std::vector<cv::Rect> integral;
            integral.reserve(boxes.size());
            for (const cv::Rect2f& b : boxes) integral.emplace_back(b);
            dnn::NMSBoxes(integral, confs, p.detConf_, 0.3f, keep, 1.0f, 5000);
        }

        // --- Association ------------------------------------------------------
        // Greedy IoU matching, strongest pairs first. Without it, taking the
        // top-N by confidence every frame makes identities swap whenever two
        // people cross, and makes tracks vanish on a single missed detection.
        std::vector<int> trackOfDet(keep.size(), -1);
        std::vector<int> detOfTrack(tracks_.size(), -1);
        {
            struct Pair {
                float iou;
                int track;
                int det;
            };
            std::vector<Pair> pairs;
            pairs.reserve(tracks_.size() * keep.size());
            for (size_t t = 0; t < tracks_.size(); ++t) {
                for (size_t d = 0; d < keep.size(); ++d) {
                    const float iou = iouOf(tracks_[t].box, boxes[d]);
                    if (iou >= kMatchIou) pairs.push_back({iou, static_cast<int>(t), static_cast<int>(d)});
                }
            }
            std::sort(pairs.begin(), pairs.end(),
                      [](const Pair& a, const Pair& b) { return a.iou > b.iou; });
            std::vector<bool> trackUsed(tracks_.size(), false);
            for (const Pair& pair : pairs) {
                if (trackUsed[pair.track] || trackOfDet[pair.det] >= 0) continue;
                trackUsed[pair.track] = true;
                trackOfDet[pair.det] = pair.track;
                detOfTrack[pair.track] = pair.det;
            }
        }

        // --- Track update -----------------------------------------------------
        for (size_t t = 0; t < tracks_.size(); ++t) {
            Track& tr = tracks_[t];
            const int d = detOfTrack[t];
            if (d >= 0) {
                const cv::Rect2f measured = boxes[d];
                const cv::Point2f measuredVel = centerOf(measured) - centerOf(tr.box);
                tr.box = blendBox(tr.box, measured, kBoxBlend);
                tr.velocity = tr.velocity * (1.f - kVelBlend) + measuredVel * kVelBlend;
                tr.missed_ = 0;
                tr.score_ = confs[keep[d]];

                std::vector<cv::Vec4f> fresh =
                    estimatePose(frameBGR, reg, rows[keep[d]], lb, p);
                if (!fresh.empty()) {
                    tr.keypoints_ = p.smooth_ ? smoothKeypoints(tr, fresh, dt) : std::move(fresh);
                }
            } else if (!tr.keypoints_.empty()) {
                // Coast on the last skeleton, carried by the box velocity, so a
                // brief detection dropout does not make the overlay blink.
                ++tr.missed_;
                if (tr.missed_ <= p.maxMissed_) {
                    for (cv::Vec4f& k : tr.keypoints_) {
                        k[0] += tr.velocity.x;
                        k[1] += tr.velocity.y;
                    }
                    tr.box.x += tr.velocity.x;
                    tr.box.y += tr.velocity.y;
                }
            } else {
                ++tr.missed_;
            }
        }

        // --- New tracks -------------------------------------------------------
        const int limit = p.maxPersons_ > 0 ? p.maxPersons_ : static_cast<int>(keep.size());
        for (size_t d = 0; d < keep.size() && static_cast<int>(tracks_.size()) < limit; ++d) {
            if (trackOfDet[d] >= 0) continue;
            Track tr;
            tr.id_ = nextId_++;
            tr.box = boxes[d];
            tr.velocity = {0.f, 0.f};
            tr.score_ = confs[keep[d]];
            tr.configureFilters(kSmoothMinCutoff, kSmoothBeta);
            tr.keypoints_ = estimatePose(frameBGR, reg, rows[keep[d]], lb, p);
            tracks_.push_back(std::move(tr));
        }

        // Retire tracks that have coasted for too long.
        tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                                     [&p](const Track& tr) { return tr.missed_ > p.maxMissed_; }),
                      tracks_.end());

        // --- Publish ----------------------------------------------------------
        std::vector<Person> result;
        result.reserve(tracks_.size());
        for (const Track& tr : tracks_) {
            if (tr.keypoints_.empty()) continue;
            cv::Rect box = tr.box;
            box &= cv::Rect(0, 0, frameBGR.cols, frameBGR.rows);
            result.push_back({tr.id_, box, tr.keypoints_});
        }
        // Stable ordering keeps the overlay from shuffling between frames.
        std::sort(result.begin(), result.end(),
                  [](const Person& a, const Person& b) { return a.id < b.id; });
        return result;
    }

private:
    static constexpr int kDetSize = 224;
    static constexpr int kPoseSize = 256;
    static constexpr int kNumAnchors = cv::samples::kPoseDetectorNumAnchors;
    static constexpr int kRegChannels = 12;
    static constexpr float kMatchIou = 0.2f;
    static constexpr float kBoxBlend = 0.5f;
    static constexpr float kVelBlend = 0.5f;
    // 1 Euro tuning, in pixels: ~10 px of jitter while still, <2 frames of lag
    // at typical dance speeds.
    static constexpr float kSmoothMinCutoff = 1.2f;
    static constexpr float kSmoothBeta = 0.35f;

    struct Track {
        int id_ = -1;
        cv::Rect2f box;
        cv::Point2f velocity;
        std::vector<cv::Vec4f> keypoints_;
        OneEuro fx[kNumKeypoints];
        OneEuro fy[kNumKeypoints];
        int missed_ = 0;
        float score_ = 0.f;

        void configureFilters(float minCutoff, float beta) {
            for (int i = 0; i < kNumKeypoints; ++i) {
                fx[i].configure(minCutoff, beta, 1.f);
                fy[i].configure(minCutoff, beta, 1.f);
            }
        }
    };

    dnn::Net detNet_;
    dnn::Net poseNet_;
    cv::Size frameSize_;
    std::vector<Track> tracks_;
    int nextId_ = 0;

    static float anchorX(int row) {
        return cv::samples::kPoseDetectorAnchors[2 * row] * static_cast<float>(kDetSize);
    }
    static float anchorY(int row) {
        return cv::samples::kPoseDetectorAnchors[2 * row + 1] * static_cast<float>(kDetSize);
    }

    // Forward/inverse of the letterbox the detector input is built with.
    struct Letterbox {
        float scale = 1.f;
        int padX = 0;
        int padY = 0;

        explicit Letterbox(cv::Size frame) {
            scale = std::min(static_cast<float>(kDetSize) / frame.width,
                             static_cast<float>(kDetSize) / frame.height);
            const int rw = static_cast<int>(frame.width * scale);
            const int rh = static_cast<int>(frame.height * scale);
            padX = (kDetSize - rw) / 2;
            padY = (kDetSize - rh) / 2;
        }
        [[nodiscard]] cv::Point2f toFrame(float x, float y) const {
            return {(x - static_cast<float>(padX)) / scale, (y - static_cast<float>(padY)) / scale};
        }
    };

    // Auxiliary keypoint `pair` of anchor `row` (0 = mid hip, 1 = full body),
    // decoded to frame coordinates. The keypoints live in channels 4..11.
    static cv::Point2f auxKeypoint(const cv::Mat& reg, int row, int pair, const Letterbox& lb) {
        return lb.toFrame(anchorX(row) + reg.at<float>(row, 4 + 2 * pair),
                          anchorY(row) + reg.at<float>(row, 5 + 2 * pair));
    }

    static float iouOf(const cv::Rect2f& a, const cv::Rect2f& b) {
        const float x1 = std::max(a.x, b.x);
        const float y1 = std::max(a.y, b.y);
        const float x2 = std::min(a.x + a.width, b.x + b.width);
        const float y2 = std::min(a.y + a.height, b.y + b.height);
        const float w = x2 - x1;
        const float h = y2 - y1;
        if (w <= 0.f || h <= 0.f) return 0.f;
        const float inter = w * h;
        const float uni = a.width * a.height + b.width * b.height - inter;
        return uni > 0.f ? inter / uni : 0.f;
    }

    static cv::Point2f centerOf(const cv::Rect2f& r) {
        return {r.x + r.width * 0.5f, r.y + r.height * 0.5f};
    }

    static cv::Rect2f blendBox(const cv::Rect2f& prev, const cv::Rect2f& next, float a) {
        return {(1.f - a) * prev.x + a * next.x,
                (1.f - a) * prev.y + a * next.y,
                (1.f - a) * prev.width + a * next.width,
                (1.f - a) * prev.height + a * next.height};
    }

    std::vector<cv::Vec4f> smoothKeypoints(Track& tr, const std::vector<cv::Vec4f>& fresh, float dt) {
        std::vector<cv::Vec4f> out(fresh.size());
        for (size_t i = 0; i < fresh.size(); ++i) {
            const int k = static_cast<int>(i);
            out[i] = cv::Vec4f(tr.fx[k].filter(fresh[i][0], dt), tr.fy[k].filter(fresh[i][1], dt),
                               fresh[i][2], fresh[i][3]);
        }
        return out;
    }

    std::vector<cv::Vec4f> estimatePose(const cv::Mat& frameBGR, const cv::Mat& reg, int row,
                                        const Letterbox& lb, const Params& p) {
        // Keypoint 0 is the mid-hip, keypoint 1 the "full body" point; together
        // they fix the crop and the rotation that puts the person upright.
        const cv::Point2f midHip = auxKeypoint(reg, row, 0, lb);
        const cv::Point2f fullBody = auxKeypoint(reg, row, 1, lb);

        const float dist = static_cast<float>(cv::norm(midHip - fullBody));
        if (dist < 1.f) return {};

        // Square RoI around the mid-hip, enlarged a little so that extended
        // limbs stay inside the crop.
        const float half = dist * p.roiEnlarge_;
        const cv::Rect square(cvFloor(midHip.x - half), cvFloor(midHip.y - half),
                              std::max(2, cvCeil(midHip.x + half) - cvFloor(midHip.x - half)),
                              std::max(2, cvCeil(midHip.y + half) - cvFloor(midHip.y - half)));
        // Clipping the square instead would squash the person and cut off limbs,
        // so take the part that overlaps the frame and pad back to the square.
        const cv::Rect inner = square & cv::Rect(0, 0, frameBGR.cols, frameBGR.rows);
        if (inner.width < 2 || inner.height < 2) return {};
        const int left = inner.x - square.x;
        const int top = inner.y - square.y;
        const int right = square.width - inner.width - left;
        const int bottom = square.height - inner.height - top;
        cv::Mat crop;
        cv::copyMakeBorder(frameBGR(inner), crop, top, bottom, left, right,
                           cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
        // Frame coordinate of the padded crop's origin; everything below works
        // in padded-crop coordinates and this is what maps back at the end.
        const cv::Point2f padBias(static_cast<float>(square.x), static_cast<float>(square.y));

        // Rotate about the mid-hip so that fullBody ends up straight above it.
        const cv::Point2f rotCenter(midHip.x - padBias.x, midHip.y - padBias.y);
        const cv::Point2f bodyCenter(fullBody.x - padBias.x, fullBody.y - padBias.y);
        // `getRotationMatrix2D` turns points by -angle, hence the added pi/2.
        float radians =
            static_cast<float>(CV_PI / 2 +
                               std::atan2(bodyCenter.y - rotCenter.y, bodyCenter.x - rotCenter.x));
        radians -= static_cast<float>(CV_PI * 2) *
                   std::floor((radians + static_cast<float>(CV_PI)) /
                              static_cast<float>(CV_PI * 2));
        const cv::Mat rotMat =
            cv::getRotationMatrix2D(rotCenter, radians * 180.f / static_cast<float>(CV_PI), 1.0);
        cv::Mat rotated;
        cv::warpAffine(crop, rotated, rotMat, crop.size());

        // The landmarks come back in 256x256 model pixels. Rescale them around
        // the crop centre to padded-crop pixels, then undo the rotation.
        const double sx = crop.cols / static_cast<double>(kPoseSize);
        const double sy = crop.rows / static_cast<double>(kPoseSize);
        const double cx = kPoseSize / 2.0;
        const double cy = kPoseSize / 2.0;
        cv::Mat invRot;
        cv::invertAffineTransform(rotMat, invRot);
        // `invRot` is the inverse of the crop -> rotated-crop rotation. It is
        // applied in two pieces because the model point has to be expressed
        // relative to the rotated crop's centre first: un-rotating that offset
        // (linear part only) and adding where the centre itself lands in the
        // padded crop (the affine part).
        const double centreX = crop.cols * 0.5;
        const double centreY = crop.rows * 0.5;
        const float originX = static_cast<float>(invRot.at<double>(0, 0) * centreX +
                                                 invRot.at<double>(0, 1) * centreY +
                                                 invRot.at<double>(0, 2));
        const float originY = static_cast<float>(invRot.at<double>(1, 0) * centreX +
                                                 invRot.at<double>(1, 1) * centreY +
                                                 invRot.at<double>(1, 2));

        cv::Mat poseIn;
        cv::resize(rotated, poseIn, cv::Size(kPoseSize, kPoseSize), 0, 0, cv::INTER_AREA);
        cv::cvtColor(poseIn, poseIn, cv::COLOR_BGR2RGB);

        dnn::Image2BlobParams posePrms;
        posePrms.datalayout = cv::DNN_LAYOUT_NHWC;
        posePrms.ddepth = CV_32F;
        posePrms.mean = cv::Scalar::all(0);
        posePrms.scalefactor = cv::Scalar::all(1.f / 255.f);
        posePrms.size = cv::Size(kPoseSize, kPoseSize);
        posePrms.swapRB = false;
        posePrms.paddingmode = dnn::DNN_PMODE_NULL;

        poseNet_.setInput(dnn::blobFromImageWithParams(poseIn, posePrms));
        std::vector<cv::Mat> poseOut;
        poseNet_.forward(poseOut, poseNet_.getUnconnectedOutLayersNames());
        if (poseOut.size() < 2) return {};

        const float conf = poseOut[1].at<float>(0);
        if (conf < p.poseConf_) return {};

        // out[0] is 39 x 5 = (x, y, z, visibility, presence); indices 0..32 are
        // the standard BlazePose skeleton, 33..38 are auxiliary.
        const cv::Mat landmarks = poseOut[0].reshape(0, 39);
        const float depthScale = static_cast<float>(std::max(sx, sy));
        std::vector<cv::Vec4f> kpts(kNumKeypoints);
        for (int i = 0; i < kNumKeypoints; ++i) {
            const double modelX = (static_cast<double>(landmarks.at<float>(i, 0)) - cx) * sx;
            const double modelY = (static_cast<double>(landmarks.at<float>(i, 1)) - cy) * sy;
            kpts[i] = cv::Vec4f(
                static_cast<float>(invRot.at<double>(0, 0) * modelX +
                                   invRot.at<double>(0, 1) * modelY) +
                    originX + padBias.x,
                static_cast<float>(invRot.at<double>(1, 0) * modelX +
                                   invRot.at<double>(1, 1) * modelY) +
                    originY + padBias.y,
                landmarks.at<float>(i, 2) * depthScale,
                1.f / (1.f + std::exp(-landmarks.at<float>(i, 3))));
        }
        return kpts;
    }
};

// ---------------------------------------------------------------------------
// Shared state between GUI thread and workers.
// ---------------------------------------------------------------------------
struct SharedPoseState {
    std::vector<MediaPipePosePipeline::Person> persons_;
    float detConf_ = 0.5f;
    float poseConf_ = 0.5f;
    float roiEnlarge_ = 1.25f;
    int maxPersons_ = 2;
    bool smooth_ = true;
    float frameDt_ = 1.f / 60.f;
    bool enabled_ = true;
    bool fullscreen_ = false;
};

// ---------------------------------------------------------------------------
// V4D plan.
// ---------------------------------------------------------------------------
class SkeletalTrackerPlan : public V4DPlan {
public:
    // Frame rate of the input, so the filters can work in content time.
    static float sourceFps_;

    constexpr static auto UMAT_COPY_TO_ =
        _OLMC_(void, cv::UMat, &cv::UMat::copyTo, cv::OutputArray);

    SkeletalTrackerPlan() { _shared(shared_); }

    void setup() override {
        if (pipeline_.empty()) {
            std::string detModel = cv::samples::findFile(
                "models/pose/person_detection_mediapipe_2023mar.onnx");
            std::string poseModel = cv::samples::findFile(
                "models/pose/pose_estimation_mediapipe_2023mar.onnx");
            if (detModel.empty() || poseModel.empty()) {
                CV_Error(Error::StsError,
                         "Pose models not found. Download from OpenCV Zoo or run `make download-models`");
            }
            pipeline_ = makePtr<MediaPipePosePipeline>(detModel, poseModel);
        }
        // The filters work in content time, not wall-clock time.
        shared_.frameDt_ = sourceFps_ > 0.f ? 1.f / sourceFps_ : 1.f / 60.f;
    }

    void gui() override {
        imgui([](SharedPoseState& s) {
            using namespace ImGui;
            Begin("Skeletal Tracker");
            Checkbox("Enable tracking", &s.enabled_);
            Checkbox("Smooth (1 Euro)", &s.smooth_);
            SliderFloat("Person conf", &s.detConf_, 0.1f, 0.9f);
            SliderFloat("Pose conf", &s.poseConf_, 0.1f, 0.9f);
            SliderFloat("Pose RoI enlarge", &s.roiEnlarge_, 1.0f, 2.0f);
            SliderInt("Max persons", &s.maxPersons_, 1, 6);
            Text("Persons detected: %zu", s.persons_.size());
            if (Button("Fullscreen")) s.fullscreen_ = !s.fullscreen_;
            End();
        }, RWS(shared_));
    }

    void infer() override {
        set(V4D::Keys::FULLSCREEN, CS(shared_.fullscreen_));

        // Mouse click toggles tracking.
        Event<Mouse> presses = E<Mouse>(Mouse::PRESS);
        branch(
            RWS(shared_.enabled_) = IF(
                F(&Mouse::List::empty, presses),
                CS(shared_.enabled_),
                !CS(shared_.enabled_)
            )
        )->endBranch();

        // Run detection + pose on a single worker.
        branch(BranchType::SINGLE, CS(shared_.enabled_))
            ->fb(UMAT_COPY_TO_, RW(frameBGR_))
            ->plain(runPipeline, RWS(shared_), R(frameBGR_))
        ->endBranch();

        // Draw skeleton overlay.
        branch(CS(shared_.enabled_))
            ->nvg(drawOverlay, size_, frameNo_, CS(shared_.persons_))
        ->endBranch();
    }

private:
    static SharedPoseState shared_;
    Property<Size> size_ = P<Size>(V4D::Keys::SIZE);
    Property<uint64_t> frameNo_ = P<uint64_t>(GlobalState::Keys::FRAME_CNT);
    static Ptr<MediaPipePosePipeline> pipeline_;
    UMat frameBGR_;

    static void runPipeline(SharedPoseState& state, const cv::UMat& frameBGR) {
        if (frameBGR.empty() || pipeline_.empty()) return;
        MediaPipePosePipeline::Params p{state.detConf_, state.poseConf_, state.roiEnlarge_,
                                        state.maxPersons_, state.smooth_, 5, state.frameDt_};
        state.persons_ = pipeline_->run(frameBGR.getMat(cv::ACCESS_READ), p);
    }

    static void drawOverlay(const cv::Size& sz, uint64_t frameNo,
                            const std::vector<MediaPipePosePipeline::Person>& persons) {
        using namespace cv::v4d::nvg;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "frame %llu  persons %zu",
                      static_cast<unsigned long long>(frameNo), persons.size());
        fontSize(22.0f);
        fontFace("sans-bold");
        fillColor(cv::Scalar(255, 255, 255, 200));
        textAlign(NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        text(12.0f, 12.0f, buf, buf + std::strlen(buf));

        for (const auto& person : persons) {
            const auto& kpts = person.keypoints;
            if (kpts.empty()) continue;

            // Detection box, so it is obvious when a skeleton is coasting on a
            // stale box rather than following a fresh detection.
            if (person.box.area() > 0) {
                strokeColor(cv::Scalar(255, 255, 255, 120));
                strokeWidth(std::max(1.0f, sz.width / 900.0f));
                beginPath();
                moveTo(person.box.x, person.box.y);
                lineTo(person.box.x + person.box.width, person.box.y);
                lineTo(person.box.x + person.box.width, person.box.y + person.box.height);
                lineTo(person.box.x, person.box.y + person.box.height);
                closePath();
                stroke();
            }

            // Bones.
            strokeColor(cv::Scalar(0, 255, 255, 200));
            strokeWidth(std::max(2.0f, sz.width / 600.0f));
            for (auto [a, b] : kPoseSegments) {
                if (a >= static_cast<int>(kpts.size()) || b >= static_cast<int>(kpts.size()))
                    continue;
                if (kpts[a][3] < 0.3f || kpts[b][3] < 0.3f)
                    continue;
                beginPath();
                moveTo(kpts[a][0], kpts[a][1]);
                lineTo(kpts[b][0], kpts[b][1]);
                stroke();
            }

            // Joints.
            fillColor(cv::Scalar(255, 0, 0, 240));
            float r = std::max(3.0f, sz.width / 500.0f);
            for (size_t i = 0; i < kpts.size(); ++i) {
                if (kpts[i][3] < 0.3f) continue;
                beginPath();
                circle(kpts[i][0], kpts[i][1], r);
                fill();
            }

            // Track id above the head, so identity stability is visible.
            std::snprintf(buf, sizeof(buf), "#%d", person.id);
            fillColor(cv::Scalar(255, 255, 255, 230));
            text(kpts[0][0] - 10.f, kpts[0][1] - 28.f, buf, buf + std::strlen(buf));
        }
    }
};

SharedPoseState SkeletalTrackerPlan::shared_;
float SkeletalTrackerPlan::sourceFps_ = 0.f;
cv::Ptr<MediaPipePosePipeline> SkeletalTrackerPlan::pipeline_;

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    cv::v4d::add_asset_search_paths();

    std::string inputVideo =
        (argc > 1) ? argv[1] : cv::samples::findFile("videos/dance.mp4");
    std::string outputVideo = (argc > 2) ? argv[2] : "skeletal_tracker_out.mkv";
    if (inputVideo.empty()) {
        std::cerr << "Usage: skeletal-tracker-demo <input-video-file> [output-video-file]"
                  << std::endl;
        return 1;
    }

    cv::Rect viewport(0, 0, 1280, 720);
    cv::Ptr<V4D> runtime = V4D::init(viewport, "Skeletal Tracker",
                                     AllocateFlags::NANOVG | AllocateFlags::IMGUI,
                                     ConfigFlags::DISPLAY_MODE);

    auto src = Source::make(runtime, inputVideo);
    SkeletalTrackerPlan::sourceFps_ = src->fps();
    auto sink = Sink::make(runtime, outputVideo, src->fps(), viewport.size());
    runtime->setSource(src);
    runtime->setSink(sink);

    V4DPlan::run<SkeletalTrackerPlan>(0);
    return 0;
}
