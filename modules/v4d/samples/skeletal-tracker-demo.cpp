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
//   ./skeletal-tracker-demo                              # the bundled dance clip
//   ./skeletal-tracker-demo <video.mp4>                  # any video or camera index
//   ./skeletal-tracker-demo <video.mp4> <out.mkv>        # also record the overlay
//   ./skeletal-tracker-demo <video.mp4> "" 960 540       # pick the viewport size
//
// Space toggles tracking. Everything else is in the ImGui panel.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <opencv2/dnn.hpp>
#include <opencv2/geometry/2d.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/v4d/v4d.hpp>
#include <string>
#include <utility>
#include <vector>

using namespace cv;
using namespace cv::v4d;
using namespace cv::v4d::event;

// ---------------------------------------------------------------------------
// MediaPipe Pose 33-keypoint skeleton topology.
//
// Two joints are also drawn as an emphasised ring: the nose and the mid-hip,
// the two points the detector itself reasons about.
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

// BlazePose keypoint indices the rest of the demo refers to by name.
enum PoseJoint {
    kJointNose = 0,
    kJointLeftHip = 23,
    kJointRightHip = 24,
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
        // Frames since this track last saw a detection; > 0 means the skeleton
        // is coasting on the last one and should be drawn as such.
        int missed = 0;
        // Detector confidence of the detection that last fed this track.
        float score = 0.f;
        cv::Rect box;
        std::vector<cv::Vec4f> keypoints; // 33 x [x, y, z, visibility x presence]
        // Recent mid-hip positions, oldest first, for the motion trail.
        std::vector<cv::Point2f> trail;
    };

    // Per-stage wall clock of one #run, for the ImGui panel.
    struct Stats {
        float detectMs = 0.f;
        float poseMs = 0.f;
        float totalMs = 0.f;
    };

    explicit MediaPipePosePipeline(const std::string& detModel, const std::string& poseModel)
        : detNet_(dnn::readNet(detModel)),
          poseNet_(dnn::readNet(poseModel)) {
        detNet_.setPreferableBackend(dnn::DNN_BACKEND_OPENCV);
        detNet_.setPreferableTarget(dnn::DNN_TARGET_OPENCL);
        poseNet_.setPreferableBackend(dnn::DNN_BACKEND_OPENCV);
        poseNet_.setPreferableTarget(dnn::DNN_TARGET_OPENCL);
    }

    [[nodiscard]] std::vector<Person> run(const cv::Mat& frameBGR, const Params& p,
                                          Stats* stats = nullptr) {
        cv::TickMeter total, detect, pose;
        total.start();
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

        // Person detection ----------------------------------------------------
        const Letterbox lb(frameBGR.size());
        dnn::Image2BlobParams detPrms;
        detPrms.datalayout = cv::DNN_LAYOUT_NCHW;
        detPrms.ddepth = CV_32F;
        detPrms.mean = cv::Scalar::all(127.5f);
        detPrms.scalefactor = cv::Scalar::all(1.f / 127.5f);
        detPrms.size = cv::Size(kDetSize, kDetSize);
        detPrms.swapRB = true;
        detPrms.paddingmode = dnn::DNN_PMODE_LETTERBOX;

        detect.start();
        detNet_.setInput(dnn::blobFromImageWithParams(frameBGR, detPrms));
        std::vector<cv::Mat> detOut;
        detNet_.forward(detOut, detNet_.getUnconnectedOutLayersNames());
        detect.stop();
        if (detOut.size() < 2) {
            report(stats, total, detect, pose);
            return {};
        }

        // The tensors are [1, 2254, 1] and [1, 2254, 12]. View them as one row
        // per anchor; note that cv::Size takes cols first, so the channel count
        // is the *first* argument.
        const cv::Mat scores(cv::Size(1, kNumAnchors), CV_32F, detOut[0].ptr<float>());
        const cv::Mat reg(cv::Size(kRegChannels, kNumAnchors), CV_32F, detOut[1].ptr<float>());

        // Channels 0..1 are the (x, y) centre deltas and 2..3 the (x, y) box
        // side lengths; both are in 224-input pixels and are added to the
        // anchor. Channels 4..11 are four auxiliary keypoints as (x, y) deltas
        // on the same anchor, in order: mid-hip, full body, shoulder, upper.
        //
        // The mid-hip and full-body points are decoded for every candidate
        // right here, because both the duplicate merge and the track
        // association below measure distance in that scale-invariant frame
        // rather than in raw pixels.
        std::vector<cv::Rect2f> boxes;
        std::vector<float> confs;
        std::vector<int> rows;
        std::vector<cv::Point2f> hips;
        std::vector<float> spans;
        for (int i = 0; i < kNumAnchors; ++i) {
            // Clamp before the sigmoid, as the reference does: an unbounded
            // logit overflows exp() and turns into inf/NaN further down.
            const float logit = std::clamp(scores.at<float>(i, 0), -100.f, 100.f);
            const float conf = 1.f / (1.f + std::exp(-logit));
            if (conf < p.detConf_) continue;
            const float xc = anchorX(i) + reg.at<float>(i, 0);
            const float yc = anchorY(i) + reg.at<float>(i, 1);
            const float bw = reg.at<float>(i, 2);
            const float bh = reg.at<float>(i, 3);
            const cv::Point2f tl = lb.toFrame(xc - bw * 0.5f, yc - bh * 0.5f);
            const cv::Point2f br = lb.toFrame(xc + bw * 0.5f, yc + bh * 0.5f);
            if (br.x - tl.x < 1.f || br.y - tl.y < 1.f) continue;
            const cv::Point2f hip = auxKeypoint(reg, i, 0, lb);
            const cv::Point2f body = auxKeypoint(reg, i, 1, lb);
            boxes.emplace_back(tl, cv::Size2f(br.x - tl.x, br.y - tl.y));
            confs.push_back(conf);
            rows.push_back(i);
            hips.push_back(hip);
            spans.push_back(static_cast<float>(std::max(1.0, cv::norm(hip - body))));
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
        // MediaPipe's multi-level detector regularly fires twice on one person,
        // from different anchors. Those boxes can be a couple of hundred pixels
        // apart and survive NMS, yet both decode to the same mid-hip -- which
        // would draw two skeletons on one person. Merge on the decoded hip:
        // measured duplicates sit within 0.22 of the mid-hip -> full-body span
        // of each other, while distinct people are about 0.44 apart. NMSBoxes
        // returns candidates in descending score order, so the strongest one is
        // kept.
        if (!boxes.empty()) {
            std::vector<int> kept;
            {
                std::vector<cv::Point2f> seenHips;
                for (int k : keep) {
                    const bool duplicate =
                        std::any_of(seenHips.begin(), seenHips.end(), [&](const cv::Point2f& seen) {
                            return cv::norm(hips[k] - seen) <= kHipMergeFactor * spans[k];
                        });
                    if (duplicate) continue;
                    seenHips.push_back(hips[k]);
                    kept.push_back(k);
                }
            }
            // Compact the surviving candidates into the front of the same five
            // parallel arrays. After this, a candidate index is also its
            // position, so downstream code never has to convert between the
            // two -- which is the easiest way to pair a detection with the
            // wrong keypoint row.
            std::vector<cv::Rect2f> keepBoxes;
            std::vector<float> keepConfs, keepSpans;
            std::vector<int> keepRows;
            std::vector<cv::Point2f> keepHips;
            keepBoxes.reserve(kept.size());
            keepConfs.reserve(kept.size());
            keepSpans.reserve(kept.size());
            keepRows.reserve(kept.size());
            keepHips.reserve(kept.size());
            for (int k : kept) {
                keepBoxes.push_back(boxes[k]);
                keepConfs.push_back(confs[k]);
                keepSpans.push_back(spans[k]);
                keepRows.push_back(rows[k]);
                keepHips.push_back(hips[k]);
            }
            boxes = std::move(keepBoxes);
            confs = std::move(keepConfs);
            spans = std::move(keepSpans);
            rows = std::move(keepRows);
            hips = std::move(keepHips);
        }

        // Greedy matching, strongest pairs first. Without it, taking the
        // top-N by confidence every frame makes identities swap whenever two
        // people cross, and makes tracks vanish on a single missed detection.
        const int numDet = static_cast<int>(boxes.size());
        std::vector<int> trackOfDet(numDet, -1);
        std::vector<int> detOfTrack(tracks_.size(), -1);
        {
            struct Pair {
                float iou;
                int track;
                int det;
            };
            std::vector<Pair> pairs;
            pairs.reserve(tracks_.size() * static_cast<size_t>(numDet));
            for (size_t t = 0; t < tracks_.size(); ++t) {
                for (int d = 0; d < numDet; ++d) {
                    pairs.push_back({iouOf(tracks_[t].box, boxes[d]), static_cast<int>(t), d});
                }
            }
            std::sort(pairs.begin(), pairs.end(),
                      [](const Pair& a, const Pair& b) { return a.iou > b.iou; });

            // Pass 1 pairs by box overlap. Pass 2 reconsiders everything still
            // unpaired and accepts the same-person test directly on the decoded
            // mid-hips. That matters because the detector's box is a square RoI
            // hint, not the person's extent: it is roughly constant across
            // frames while the person walks, so its IoU is a much weaker
            // identity cue than the hip is. Without the second pass a detection
            // that a coasting or fast-moving track barely overlaps would start a
            // *new* track, drawing a second skeleton over the person the old
            // track is still coasting on.
            for (int pass = 0; pass < 2; ++pass) {
                for (const Pair& pair : pairs) {
                    if (detOfTrack[pair.track] >= 0 || trackOfDet[pair.det] >= 0) continue;
                    if (pass == 0) {
                        if (pair.iou < kMatchIou) continue;
                    } else if (cv::norm(tracks_[pair.track].hip_ - hips[pair.det]) >
                               kHipGateFactor * spans[pair.det]) {
                        continue;
                    }
                    detOfTrack[pair.track] = pair.det;
                    trackOfDet[pair.det] = pair.track;
                }
            }
        }

        // --- Track update -----------------------------------------------------
        // From here on the cost is dominated by the per-person pose crops, so
        // the pose stage timer covers the rest of #run.
        pose.start();
        for (size_t t = 0; t < tracks_.size(); ++t) {
            Track& tr = tracks_[t];
            const int d = detOfTrack[t];
            if (d >= 0) {
                const cv::Rect2f measured = boxes[d];
                // Velocity is measured on the hip, not on the box centre, so it
                // means the same thing while the detector's box size drifts.
                const cv::Point2f measuredVel = hips[d] - tr.hip_;
                tr.box = blendBox(tr.box, measured, kBoxBlend);
                tr.velocity = tr.velocity * (1.f - kVelBlend) + measuredVel * kVelBlend;
                tr.hip_ = hips[d];
                tr.missed_ = 0;
                tr.score_ = confs[d];

                std::vector<cv::Vec4f> fresh = estimatePose(frameBGR, reg, rows[d], lb, p);
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
                    tr.hip_ += tr.velocity;
                }
            } else {
                ++tr.missed_;
            }
        }

        // --- New tracks -------------------------------------------------------
        const int limit = p.maxPersons_ > 0 ? p.maxPersons_ : numDet;
        for (int d = 0; d < numDet && static_cast<int>(tracks_.size()) < limit; ++d) {
            if (trackOfDet[d] >= 0) continue;
            // Estimate before committing the track. A detection whose pose
            // fails draws nothing, so keeping it would only burn an id and a
            // person slot until it coasts out -- and hold a slot against the
            // detection that could actually be drawn.
            std::vector<cv::Vec4f> fresh = estimatePose(frameBGR, reg, rows[d], lb, p);
            if (fresh.empty()) continue;
            Track tr;
            tr.id_ = nextId_++;
            tr.box = boxes[d];
            tr.hip_ = hips[d];
            tr.velocity = {0.f, 0.f};
            tr.score_ = confs[d];
            tr.configureFilters(kSmoothMinCutoff, kSmoothBeta);
            tr.keypoints_ = std::move(fresh);
            tracks_.push_back(std::move(tr));
        }

        // Retire tracks that have coasted for too long.
        tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                                     [&p](const Track& tr) { return tr.missed_ > p.maxMissed_; }),
                      tracks_.end());

        // --- Publish ----------------------------------------------------------
        pose.stop();
        std::vector<Person> result;
        result.reserve(tracks_.size());
        for (Track& tr : tracks_) {
            if (tr.keypoints_.empty()) continue;
            cv::Rect box = tr.box;
            box &= cv::Rect(0, 0, frameBGR.cols, frameBGR.rows);
            result.push_back({tr.id_, tr.missed_, tr.score_, box, tr.keypoints_, trailOf(tr)});
        }
        // Stable ordering keeps the overlay from shuffling between frames.
        std::sort(result.begin(), result.end(),
                  [](const Person& a, const Person& b) { return a.id < b.id; });
        report(stats, total, detect, pose);
        return result;
    }

private:
    // Copies the stage timings out, if the caller asked for them. On every
    // early return the caller sees the work done up to that point.
    static void report(Stats* stats, cv::TickMeter& total, cv::TickMeter& detect,
                       cv::TickMeter& pose) {
        if (!stats) return;
        total.stop();
        stats->detectMs = detect.getAvgTimeMilli();
        stats->poseMs = pose.getAvgTimeMilli();
        stats->totalMs = total.getAvgTimeMilli();
    }
    static constexpr int kDetSize = 224;
    static constexpr int kPoseSize = 256;
    static constexpr int kNumAnchors = cv::samples::kPoseDetectorNumAnchors;
    static constexpr int kRegChannels = 12;
    static constexpr float kMatchIou = 0.2f;
    // Detections whose mid-hips are closer than this fraction of the body span
    // are the same person; see the merge above.
    static constexpr float kHipMergeFactor = 0.22f;
    // The same test, used to hand a detection to an already existing track
    // instead of letting it start a duplicate one.
    static constexpr float kHipGateFactor = 0.35f;
    static constexpr float kBoxBlend = 0.5f;
    static constexpr float kVelBlend = 0.5f;
    // 1 Euro tuning, in pixels: ~10 px of jitter while still, <2 frames of lag
    // at typical dance speeds.
    static constexpr float kSmoothMinCutoff = 1.2f;
    static constexpr float kSmoothBeta = 0.35f;
    // Mid-hip positions kept per track for the motion trail drawn by the overlay.
    static constexpr size_t kTrailLength = 24;

    struct Track {
        int id_ = -1;
        cv::Rect2f box;
        // The detector's decoded mid-hip for this track. This is the identity
        // anchor: unlike `box` it is a specific body point, so distances in it
        // mean the same thing for every person regardless of their size.
        cv::Point2f hip_;
        cv::Point2f velocity;
        std::vector<cv::Vec4f> keypoints_;
        std::vector<cv::Point2f> trail_;
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

    static cv::Rect2f blendBox(const cv::Rect2f& prev, const cv::Rect2f& next, float a) {
        return {(1.f - a) * prev.x + a * next.x,
                (1.f - a) * prev.y + a * next.y,
                (1.f - a) * prev.width + a * next.width,
                (1.f - a) * prev.height + a * next.height};
    }

    // Appends the track's mid-hip to its trail and returns the trail, oldest
    // point first. The trail lives with the track rather than in the shared
    // state, so it is retired together with the track that owns it.
    static std::vector<cv::Point2f> trailOf(Track& tr) {
        tr.trail_.push_back(tr.hip_);
        if (tr.trail_.size() > kTrailLength) {
            tr.trail_.erase(tr.trail_.begin(),
                            tr.trail_.begin() + (tr.trail_.size() - kTrailLength));
        }
        return tr.trail_;
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
        // the standard BlazePose skeleton, 33..38 are auxiliary. Visibility says
        // the joint is not occluded, presence says it is inside the frame; a
        // joint is only worth drawing when both hold, so the score kept per
        // keypoint is their product. Both columns still need a sigmoid first.
        const cv::Mat landmarks = poseOut[0].reshape(0, 39);
        const float depthScale = static_cast<float>(std::max(sx, sy));
        std::vector<cv::Vec4f> kpts(kNumKeypoints);
        for (int i = 0; i < kNumKeypoints; ++i) {
            const double modelX = (static_cast<double>(landmarks.at<float>(i, 0)) - cx) * sx;
            const double modelY = (static_cast<double>(landmarks.at<float>(i, 1)) - cy) * sy;
            const float visibility = 1.f / (1.f + std::exp(-landmarks.at<float>(i, 3)));
            const float presence = 1.f / (1.f + std::exp(-landmarks.at<float>(i, 4)));
            kpts[i] = cv::Vec4f(
                static_cast<float>(invRot.at<double>(0, 0) * modelX +
                                   invRot.at<double>(0, 1) * modelY) +
                    originX + padBias.x,
                static_cast<float>(invRot.at<double>(1, 0) * modelX +
                                   invRot.at<double>(1, 1) * modelY) +
                    originY + padBias.y,
                landmarks.at<float>(i, 2) * depthScale,
                visibility * presence);
        }
        return kpts;
    }
};

// ---------------------------------------------------------------------------
// Shared state between GUI thread and workers.
// ---------------------------------------------------------------------------
struct SharedPoseState {
    std::vector<MediaPipePosePipeline::Person> persons_;
    // Stage timings of the last processed frame, in milliseconds, for the panel.
    float detectMs_ = 0.f;
    float poseMs_ = 0.f;
    float totalMs_ = 0.f;
    float detConf_ = 0.55f;
    float poseConf_ = 0.33f;
    float roiEnlarge_ = 1.15f;
    int maxPersons_ = 1;
    int maxMissed_ = 5;
    bool smooth_ = true;
    float frameDt_ = 1.f / 60.f;
    bool enabled_ = true;
    bool fullscreen_ = false;
    bool showBoxes_ = true;
    bool showTrails_ = true;
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
            Checkbox("Enable tracking  [Space]", &s.enabled_);
            Checkbox("Smooth (1 Euro)", &s.smooth_);
            SameLine();
            Checkbox("Boxes", &s.showBoxes_);
            Checkbox("Trails", &s.showTrails_);
            Separator();
            SliderFloat("Person conf", &s.detConf_, 0.1f, 0.9f);
            SliderFloat("Pose conf", &s.poseConf_, 0.1f, 0.9f);
            SliderFloat("Pose RoI enlarge", &s.roiEnlarge_, 1.0f, 2.0f);
            SliderInt("Max persons", &s.maxPersons_, 1, 6);
            SliderInt("Coast frames", &s.maxMissed_, 0, 30);
            Separator();
            Text("Persons detected: %zu", s.persons_.size());
            for (const auto& p : s.persons_) {
                // "coasting" is the state to look at while tuning: it means the
                // skeleton on screen is extrapolated, not measured this frame.
                Text("  #%d  det %.2f%s", p.id, p.score,
                     p.missed > 0 ? "  (coasting)" : "");
            }
            Separator();
            Text("detect %.1f ms   pose %.1f ms   total %.1f ms", s.detectMs_, s.poseMs_,
                 s.totalMs_);
            if (Button("Fullscreen")) s.fullscreen_ = !s.fullscreen_;
            End();
        }, RWS(shared_));
    }

    void infer() override {
        set(V4D::Keys::FULLSCREEN, CS(shared_.fullscreen_));

        Event<Keyboard> space = E<Keyboard>(Keyboard::PRESS);
        branch(
            RWS(shared_.enabled_) = IF(
                F(&Keyboard::List::empty, space),
                CS(shared_.enabled_),
                !CS(shared_.enabled_)
            )
        );
	{
          fb(UMAT_COPY_TO_, RW(frameBGR_))
          ->plain(runPipeline, RWS(shared_), R(frameBGR_))
          ->nvg(drawOverlay, size_, frameNo_, CS(shared_));
	}
	endBranch();
    }

private:
    static SharedPoseState shared_;
    Property<Size> size_ = P<Size>(V4D::Keys::SIZE);
    Property<uint64_t> frameNo_ = P<uint64_t>(GlobalState::Keys::FRAME_CNT);
    static Ptr<MediaPipePosePipeline> pipeline_;
    UMat frameBGR_;

    static void runPipeline(SharedPoseState& state, const cv::UMat& frameBGR) {
        if (frameBGR.empty() || pipeline_.empty()) return;
        MediaPipePosePipeline::Params p{state.detConf_,  state.poseConf_, state.roiEnlarge_,
                                        state.maxPersons_, state.smooth_,   state.maxMissed_,
                                        state.frameDt_};
        MediaPipePosePipeline::Stats stats;
        state.persons_ = pipeline_->run(frameBGR.getMat(cv::ACCESS_READ), p, &stats);
        state.detectMs_ = stats.detectMs;
        state.poseMs_ = stats.poseMs;
        state.totalMs_ = stats.totalMs;
    }

    // One colour per track id, so two people on screen are never confused for
    // each other. Indexed, so a track keeps its colour for its whole life.
    static cv::Scalar trackColor(int id) {
        // Not constexpr: cv::Scalar's constructor is not a constant expression.
        static const cv::Scalar kPalette[] = {
            cv::Scalar(60, 220, 255),   // amber
            cv::Scalar(255, 200, 60),   // azure
            cv::Scalar(90, 90, 255),    // red
            cv::Scalar(255, 140, 210),  // pink
            cv::Scalar(120, 255, 120),  // light green
            cv::Scalar(220, 255, 60),   // teal
            cv::Scalar(225, 120, 255),  // violet
            cv::Scalar(60, 255, 255),   // yellow
            cv::Scalar(160, 255, 255),  // pale yellow
            cv::Scalar(255, 180, 90),   // sky
        };
        const size_t i = static_cast<size_t>(id) % (sizeof(kPalette) / sizeof(kPalette[0]));
        return kPalette[i];
    }

    // Box around the skeleton itself, not around the detector's RoI hint: the
    // model's box is a near-constant square around the torso and says nothing
    // about where the person actually is. The body extent is squared up about
    // its own centre and given a little air, matching the pose crop's own
    // enlargement.
    static cv::Rect2f skeletonBox(const std::vector<cv::Vec4f>& kpts) {
        constexpr float kBoxEnlarge = 1.25f;
        float x0 = std::numeric_limits<float>::max(), y0 = x0;
        float x1 = -x0, y1 = -x0;
        for (const cv::Vec4f& k : kpts) {
            if (k[3] < 0.3f) continue;
            x0 = std::min(x0, k[0]);
            y0 = std::min(y0, k[1]);
            x1 = std::max(x1, k[0]);
            y1 = std::max(y1, k[1]);
        }
        if (x1 <= x0 || y1 <= y0) return {};
        const float cx = 0.5f * (x0 + x1), cy = 0.5f * (y0 + y1);
        const float half = 0.5f * kBoxEnlarge * std::max(x1 - x0, y1 - y0);
        return {cx - half, cy - half, 2 * half, 2 * half};
    }

    static void strokeRect(const cv::Rect2f& r) {
        using namespace cv::v4d::nvg;
        beginPath();
        rect(r.x, r.y, r.width, r.height);
        stroke();
    }

    static void drawOverlay(const cv::Size& sz, uint64_t frameNo,
                            const SharedPoseState& state) {
        using namespace cv::v4d::nvg;
        char buf[64];
        const auto& persons = state.persons_;

        // Line widths scale with the viewport so the overlay reads the same at
        // 720p and at 4K.
        const float scale = std::max(1.0f, static_cast<float>(sz.height) / 720.f);
        const float boneW = 3.0f * scale;
        const float jointR = 3.5f * scale;
        // A dark stroke of the same path under the coloured one keeps the
        // skeleton readable over a bright frame as well as a dark one.
        const cv::Scalar outline(0, 0, 0, 150);

        lineCap(NVG_ROUND);
        lineJoin(NVG_ROUND);

        for (const auto& person : persons) {
            const auto& kpts = person.keypoints;
            if (kpts.empty()) continue;
            const cv::Scalar color = trackColor(person.id);
            // A coasting track is one whose skeleton is extrapolated, not
            // measured this frame; fading it makes that readable at a glance.
            const uchar alpha = person.missed > 0 ? 110 : 235;

            // --- Motion trail: where this track has been over the last frames.
            if (state.showTrails_ && person.trail.size() > 1) {
                const size_t n = person.trail.size();
                for (size_t i = 1; i < n; ++i) {
                    globalAlpha(static_cast<float>(i) / static_cast<float>(n) * alpha / 255.f);
                    strokeColor(cv::Scalar(color[0], color[1], color[2], 1));
                    strokeWidth(boneW * 0.8f);
                    beginPath();
                    moveTo(person.trail[i - 1].x, person.trail[i - 1].y);
                    lineTo(person.trail[i].x, person.trail[i].y);
                    stroke();
                }
                globalAlpha(1.0f);
            }

            // --- Box around the skeleton.
            if (state.showBoxes_) {
                const cv::Rect2f box = skeletonBox(kpts);
                if (box.area() > 0) {
                    strokeColor(cv::Scalar(color[0], color[1], color[2], alpha));
                    strokeWidth(1.5f * scale);
                    strokeRect(box);
                }
            }

            // --- Bones, outlined then coloured.
            for (int pass = 0; pass < 2; ++pass) {
                strokeColor(pass == 0 ? outline : cv::Scalar(color[0], color[1], color[2], alpha));
                strokeWidth(pass == 0 ? boneW * 2.2f : boneW);
                for (const auto& [a, b] : kPoseSegments) {
                    if (a >= static_cast<int>(kpts.size()) || b >= static_cast<int>(kpts.size()))
                        continue;
                    if (kpts[a][3] < 0.3f || kpts[b][3] < 0.3f) continue;
                    beginPath();
                    moveTo(kpts[a][0], kpts[a][1]);
                    lineTo(kpts[b][0], kpts[b][1]);
                    stroke();
                }
            }

            // --- Joints: a dark disc under a coloured one, nose and mid-hip
            // larger because they are what the detector itself keys on.
            for (int pass = 0; pass < 2; ++pass) {
                fillColor(pass == 0 ? outline : cv::Scalar(color[0], color[1], color[2], alpha));
                for (size_t i = 0; i < kpts.size(); ++i) {
                    if (kpts[i][3] < 0.3f) continue;
                    const bool key = i == kJointNose ||
                                     (i == kJointLeftHip && kpts[kJointRightHip][3] >= 0.3f) ||
                                     (i == kJointRightHip && kpts[kJointLeftHip][3] >= 0.3f);
                    const float r = jointR * (pass == 0 ? (key ? 2.1f : 1.6f) : (key ? 1.4f : 1.0f));
                    beginPath();
                    circle(kpts[i][0], kpts[i][1], r);
                    fill();
                }
            }

            // --- Track id above the head, so identity stability is visible.
            const int head = kpts[kJointNose][3] >= 0.3f ? kJointNose : kJointLeftHip;
            std::snprintf(buf, sizeof(buf), "#%d", person.id);
            const float labelSize = 18.0f * scale;
            fontSize(labelSize);
            fontFace("sans-bold");
            textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
            const float tx = kpts[head][0];
            const float ty = kpts[head][1] - jointR * 3.0f;
            // Legibility: a filled pill behind the text, in the track colour.
            float bounds[4];
            textBounds(tx, ty, buf, buf + std::strlen(buf), bounds);
            fillColor(cv::Scalar(color[0], color[1], color[2], alpha));
            beginPath();
            roundedRect(bounds[0] - 5 * scale, bounds[1] - 2 * scale,
                        (bounds[2] - bounds[0]) + 10 * scale,
                        (bounds[3] - bounds[1]) + 4 * scale, 4 * scale);
            fill();
            fillColor(cv::Scalar(0, 0, 0, 255));
            text(tx, ty, buf, buf + std::strlen(buf));
        }

        // --- Status line.
        fontSize(18.0f * scale);
        fontFace("sans-bold");
        fillColor(cv::Scalar(255, 255, 255, 210));
        textAlign(NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        std::snprintf(buf, sizeof(buf), "frame %llu   persons %zu   %.1f ms",
                      static_cast<unsigned long long>(frameNo), persons.size(),
                      state.totalMs_);
        text(12.0f, 12.0f, buf, buf + std::strlen(buf));
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
    std::string outputVideo = 
	(argc > 2) ? argv[2] : "";

    // A readable file must exist even when an argument was supplied: without
    // this check a typo in the path just opens an empty window.
    if (inputVideo.empty() || !std::ifstream(inputVideo).good()) {
        std::cerr << "Cannot read input video: "
                  << (inputVideo.empty() ? "<no bundled video found>" : inputVideo) << "\n"
                  << "Usage: skeletal-tracker-demo [input-video] [output-video] [width height]"
                  << std::endl;
        return 1;
    }

    cv::Rect viewport(0, 0, 1920, 1080);
    cv::Ptr<V4D> runtime = V4D::init(viewport, "Skeletal Tracker",
                                     AllocateFlags::NANOVG | AllocateFlags::IMGUI,
                                     ConfigFlags::DISPLAY_MODE);

    auto src = Source::make(runtime, inputVideo);
    
    SkeletalTrackerPlan::sourceFps_ = src->fps();
    runtime->setSource(src);
    if (!outputVideo.empty()) {
        auto sink = Sink::make(runtime, outputVideo, src->fps(), viewport.size());
        runtime->setSink(sink);
    }

    V4DPlan::run<SkeletalTrackerPlan>(0);
    return 0;
}
