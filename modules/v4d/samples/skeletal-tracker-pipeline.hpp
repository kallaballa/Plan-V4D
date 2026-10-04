// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The media-free half of the Skeletal Tracker Demo: the MediaPipe BlazePose
// detector + pose pipeline, the state its plan shares with the GUI thread, and
// the overlay helpers both of them draw with. Kept out of
// skeletal-tracker-demo.cpp so that the harnesses in tools/skeletal-tracker/ can
// drive the pipeline without redefining main().
//
// Two stages, the way MediaPipe itself runs them:
//
//   detector  a person box and four auxiliary keypoints per person, every frame.
//             The mid-hip among them is a coarse body point: good enough to say
//             *where* somebody is, not good enough to aim a 256x256 crop with.
//   pose      one crop per person -> 33 landmarks. Everything the quality of this
//             stage is made of happens in that crop: its centre, its size and
//             its rotation decide what the pose model gets to look at.
//
// The crop for a person the tracker already has a skeleton for is built from
// that skeleton, not from the detector's keypoints. The skeleton is measured on
// this person and smoothed; the detector's keypoint is a coarse regression that
// moves by tens of pixels between frames even while the person stands still, and
// every pixel it moves moves the whole skeleton with it. Building the crop from
// the skeleton is also what lets a frame the detector dropped be measured
// instead of guessed -- see #run.

#ifndef OPENCV_SKELETAL_TRACKER_PIPELINE_HPP
#define OPENCV_SKELETAL_TRACKER_PIPELINE_HPP

#include <algorithm>
#include <cmath>
#include <limits>
#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/geometry/2d.hpp>
#include <opencv2/imgproc.hpp>
#include <string>
#include <utility>
#include <vector>

#include "skeletal-tracker-anchors.hpp"

namespace cv {
namespace samples {

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
        n_ = 0;
        prev_ = 0.f;
        prevDerivative_ = 0.f;
    }

    [[nodiscard]] float filter(float x, float dt, float trust = 1.f) {
        if (!(dt > 0.f)) dt = 1.f / 30.f;
        // The speed is estimated from a median of the last three samples rather
        // than from the last one. One noisy sample would otherwise be read as
        // motion, which raises the cutoff, which lowers the amount of smoothing
        // applied: the filter switching itself off exactly when the noise it
        // exists to remove is at its worst. A median of three is the cheapest
        // thing that rejects a one-frame outlier.
        //
        // The window is seeded with the first sample rather than left holding
        // zeroes: a partially filled window's median is the median of the
        // samples *and of the padding*, so a fresh filter would emit 0 for a
        // joint at x=700, and the track that owns it would start life anchored
        // at the top-left corner of the frame.
        if (n_ == 0) {
            history_[0] = history_[1] = history_[2] = x;
        } else {
            history_[n_ % 3] = x;
        }
        ++n_;
        const float med = median3(history_[0], history_[1], history_[2]);
        if (!started_) {
            started_ = true;
            prev_ = med;
            prevDerivative_ = 0.f;
            return prev_;
        }
        const float dx = (med - prev_) / dt;
        // `trust` scales how much of this sample the filter is allowed to take,
        // for a joint the model does not claim to see: the update still happens,
        // so the joint keeps following slow motion, but a sample the model is
        // unsure about moves it by a fraction of what it otherwise would, which
        // is enough to stop it following the noise. Holding the last position
        // outright would be simpler and is worse: it leaves the joint standing
        // still in the room while the rest of the person walks away from it.
        prevDerivative_ += trust * alpha(derivativeCutoff_, dt) * (dx - prevDerivative_);
        // Speed-adaptive cutoff: faster motion -> higher cutoff -> less lag.
        const float cutoff = minCutoff_ + beta_ * std::abs(prevDerivative_);
        prev_ += trust * alpha(cutoff, dt) * (med - prev_);
        return prev_;
    }

    // The last value the filter emitted, and whether it has emitted one at all:
    // a joint that has never been measured has nothing to hold on to.
    [[nodiscard]] float value() const { return prev_; }
    [[nodiscard]] bool started() const { return started_; }

private:
    static float median3(float a, float b, float c) {
        return std::max(std::min(a, b), std::min(std::max(a, b), c));
    }

    static float alpha(float cutoff, float dt) {
        const float tau = 1.f / (2.f * static_cast<float>(CV_PI) * cutoff);
        return 1.f / (1.f + tau / dt);
    }

    float minCutoff_ = 1.f;
    float beta_ = 0.f;
    float derivativeCutoff_ = 1.f;
    float prev_ = 0.f;
    float prevDerivative_ = 0.f;
    float history_[3] = {0.f, 0.f, 0.f};
    int n_ = 0;
    bool started_ = false;
};

// ---------------------------------------------------------------------------
// Lightweight wrappers around the two ONNX models.
// ---------------------------------------------------------------------------
class MediaPipePosePipeline {
public:
    static constexpr int kNumKeypoints = 33;

    // Every field is set by the caller; the struct deliberately has no defaults
    // of its own, because the demo's tunables live in one place (see
    // SharedPoseState below) and two sets of defaults can only ever disagree.
    struct Params {
        float detConf_;
        float poseConf_;
        // Square pose RoI half-size, relative to whatever the crop's scale is
        // measured in: the mid-hip -> full-body distance for a person just
        // detected, the torso length for one already tracked. 1.0 is the
        // reference default; more keeps outstretched arms and feet inside the
        // crop, at the cost of pose-input resolution.
        float roiEnlarge_;
        int maxPersons_;
        bool smooth_;
        // How many consecutive frames a track survives without a measurement.
        int maxMissed_;
        // Source frame interval, seconds. See SharedPoseState::frameDt_ for
        // where this comes from and what it assumes.
        float frameDt_;
        // Visibility*presence below which a landmark is not treated as a
        // measurement of where that joint is; 0 disables the rule. See
        // #smoothKeypoints.
        float keypointGate_;
        // Build the crop of an already tracked person out of its own skeleton
        // rather than out of the detector's keypoints, and re-measure tracks the
        // detector dropped. See #run, #trackingCrop.
        bool landmarkRoi_;
    };

    struct Person {
        int id = -1;
        // Frames since this track's skeleton was last measured from an image.
        // > 0 means the skeleton on screen is carried over from an earlier frame
        // rather than estimated from this one.
        int missed = 0;
        // Frames since the detector last confirmed this person. Below `missed`
        // when the skeleton alone kept the track measured -- a person the
        // detector lost but the tracker did not.
        int detMissed = 0;
        // Detector confidence of the detection that last fed this track.
        float score = 0.f;
        // The detector's own RoI hint for this person. Deliberately not the
        // same as the box the overlay draws around the skeleton: the detector's
        // box is a near-constant square around the torso.
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
        // Pose-net runs this frame over all people. This pipeline's cost is one
        // of these per person per frame, plus one more for a person the detector
        // dropped; the panel shows it so the effect of raising the person limit
        // is visible as what it is.
        int poseRuns = 0;
        // Crops this frame that had to be re-centred on the detector because the
        // track's own prediction and the detector disagreed by more than
        // kReanchor. A few are the crop checking itself against the detector; a
        // lot mean the prediction has drifted and the detector is carrying the
        // crop again, which is the thing this pipeline exists to avoid.
        int reanchors = 0;
    };

    explicit MediaPipePosePipeline(const std::string& detModel, const std::string& poseModel)
        : detNet_(dnn::readNetFromONNX(detModel, dnn::ENGINE_ORT)),
          poseNet_(dnn::readNetFromONNX(poseModel, dnn::ENGINE_ORT)) {
        detNet_.setPreferableBackend(dnn::DNN_BACKEND_OPENCV);
        poseNet_.setPreferableBackend(dnn::DNN_BACKEND_OPENCV);
        detNet_.setPreferableTarget(dnn::DNN_TARGET_OPENCL);
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
	for(cv::Rect2f& b : boxes) {
          b.width * 0.75;
	}
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
            // unpaired and accepts the same-person test directly on the mid-hips.
            // That matters because the detector's box is a square RoI hint, not
            // the person's extent: it is roughly constant across frames while the
            // person walks, so its IoU is a much weaker identity cue than the hip
            // is. Without the second pass a detection that a coasting or
            // fast-moving track barely overlaps would start a *new* track,
            // drawing a second skeleton over the person the old track is still
            // following.
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
        int poseRuns = 0;
        // Crops that had to be re-centred on the detector because the skeleton
        // and the detector disagreed by more than kReanchor. A handful of these
        // is the crop keeping itself honest; a lot of them means the track's own
        // prediction has drifted and the detector is carrying the crop again.
        int reanchors = 0;
        // Re-measuring a track the detector dropped is the one pose run in here
        // that is optional, so it is the one that gets a budget: a frame in which
        // several people are acquired at once must not also pay for a
        // re-measurement of every other person, or the pipeline's worst-case
        // latency becomes a function of how much of the scene changed at once.
        const int coastBudget = p.maxPersons_ > 0 ? p.maxPersons_ : 1;
        for (size_t t = 0; t < tracks_.size(); ++t) {
            Track& tr = tracks_[t];
            ++tr.detMissed_;
            const int d = detOfTrack[t];
            bool measured = false;
            if (d >= 0) {
                tr.detMissed_ = 0;
                tr.box = blendBox(tr.box, boxes[d], kBoxBlend);
                tr.score_ = confs[d];
                // The skeleton builds the crop's size and rotation; the
                // detector's mid-hip gets the last word on where it is centred.
                // A track with no skeleton to build a crop from -- one whose last
                // measurement was too occluded to say how long the torso is --
                // falls back to the detector's own proposal.
                bool reanchored = false;
                const Crop fromSkeleton =
                    p.landmarkRoi_ ? trackingCrop(tr, hips[d], p, reanchored) : Crop{};
                if (reanchored) ++reanchors;
                const Crop crop = fromSkeleton.half >= 1.f
                                      ? fromSkeleton
                                      : acquisitionCrop(reg, rows[d], lb, p);
                measured = measure(frameBGR, tr, crop, dt, p, poseRuns);
            } else if (p.landmarkRoi_ && tr.missed_ == 0 && poseRuns < coastBudget) {
                // No detection, but the track was measured last frame, so its
                // crop is one frame stale rather than arbitrary: this frame can
                // still be a measurement instead of a guess. That is what keeps a
                // skeleton on screen -- and the same id, colour and trail -- while
                // the detector is losing somebody.
                measured = measure(frameBGR, tr, coastCrop(tr, p), dt, p, poseRuns,
                                   /*gate=*/true);
            }
            if (!measured) {
                ++tr.missed_;
                carrySkeleton(tr);
            }
        }

        // --- Retire tracks that have coasted for too long ----------------------
        // The clock is the measurement one, not the detector's. A track the
        // detector dropped but whose own crop still measures is a person we can
        // see; retiring it would mint a new id, colour and trail for somebody who
        // never left the frame. What ends a track is having no evidence at all
        // about anybody at that place for that long.
        //
        // This has to happen before the new tracks below are allocated, not
        // after: a track that has just run out of frames still holds its slot
        // for one more frame otherwise, and that frame is exactly the one in
        // which a new person walks into view.
        tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                                     [&p](const Track& tr) { return tr.missed_ > p.maxMissed_; }),
                      tracks_.end());

        // --- New tracks -------------------------------------------------------
        const int limit = p.maxPersons_ > 0 ? p.maxPersons_ : numDet;
        // Makes room for one more track, if any slot is left or can be taken from
        // a track this frame did not measure. Which track is sacrificed matters
        // for identity, so: a track that was measured this frame is never
        // dropped -- handing its slot to a detection we have not even confirmed a
        // person in yet would swap identities for nothing -- and among carried
        // tracks the one that has been carried longest goes first, i.e. the one
        // already closest to being retired.
        auto freeSlot = [&]() {
            if (static_cast<int>(tracks_.size()) < limit) return true;
            auto victim = tracks_.end();
            for (auto it = tracks_.begin(); it != tracks_.end(); ++it) {
                if (it->missed_ == 0) continue;
                if (victim == tracks_.end() || it->missed_ > victim->missed_ ||
                    (it->missed_ == victim->missed_ && it->score_ < victim->score_)) {
                    victim = it;
                }
            }
            if (victim == tracks_.end()) return false;
            tracks_.erase(victim);
            return true;
        };
        for (int d = 0; d < numDet; ++d) {
            if (trackOfDet[d] >= 0) continue;
            // The merge above only compares detections with each other. One that
            // is a duplicate of somebody already tracked -- too far from the
            // track's box for the box test, but landing on the same hip -- is
            // caught here, where it would otherwise become a second track on top
            // of the first, with its own id, colour and trail.
            const bool alreadyTracked =
                std::any_of(tracks_.begin(), tracks_.end(), [&](const Track& tr) {
                    return cv::norm(tr.hip_ - hips[d]) <= kHipMergeFactor * spans[d];
                });
            if (alreadyTracked) continue;
// Estimate before committing the track. A detection whose pose
            // fails draws nothing, so keeping it would only burn an id and a
            // person slot until it coasts out -- and hold a slot against the
            // detection that could actually be drawn.
            ++poseRuns;
            std::vector<cv::Vec4f> fresh =
                estimatePose(frameBGR, acquisitionCrop(reg, rows[d], lb, p), p);
            if (fresh.empty()) continue;
            // Free the slot only now that the newcomer is known to be a person,
            // so a failed estimate never costs a tracked person their id.
            if (!freeSlot()) break;
            Track tr;
            tr.id_ = nextId_++;
            tr.box = boxes[d];
            tr.velocity = {0.f, 0.f};
            tr.score_ = confs[d];
            tr.configureFilters(kSmoothMinCutoff, kSmoothBeta);
            // `fresh` stays raw for the crop's geometry to filter on its own,
            // while the drawn skeleton takes the filtered positions.
            tr.keypoints_ = p.smooth_ ? smoothKeypoints(tr, fresh, dt, p) : fresh;
            adoptSkeleton(tr, fresh, p, dt);
            tracks_.push_back(std::move(tr));
        }

        // --- Publish ----------------------------------------------------------
        pose.stop();
        std::vector<Person> result;
        result.reserve(tracks_.size());
        for (Track& tr : tracks_) {
            if (tr.keypoints_.empty()) continue;
            cv::Rect box = tr.box;
            box &= cv::Rect(0, 0, frameBGR.cols, frameBGR.rows);
            result.push_back(
                {tr.id_, tr.missed_, tr.detMissed_, tr.score_, box, tr.keypoints_, trailOf(tr)});
        }
        // Stable ordering keeps the overlay from shuffling between frames.
        std::sort(result.begin(), result.end(),
                  [](const Person& a, const Person& b) { return a.id < b.id; });
        report(stats, total, detect, pose);
        if (stats) {
            stats->poseRuns = poseRuns;
            stats->reanchors = reanchors;
        }
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
    static constexpr int kNumAnchors = kPoseDetectorNumAnchors;
    static constexpr int kRegChannels = 12;
    static constexpr int kMatchIou = 0.2f;
    // Detections whose mid-hips are closer than this fraction of the body span
    // are the same person; see the merge above and the new-track guard below.
    static constexpr float kHipMergeFactor = 0.22f;
    // The same test, used to hand a detection to an already existing track
    // instead of letting it start a duplicate one.
    static constexpr float kHipGateFactor = 0.35f;
    static constexpr float kBoxBlend = 0.5f;
    static constexpr float kVelBlend = 0.5f;
    // 1 Euro tuning, in pixels. minCutoff is how hard a standing person is
    // smoothed; beta is how many Hz of cutoff are added per px/s of motion, and
    // it has to stay small for this to be a filter at all: at beta = 0.35 a
    // person walking at 5 px/frame already asks for a cutoff around 50 Hz, which
    // passes the measurements through unchanged -- and the speed that asked for
    // it is mostly the jitter being smoothed, not the person moving.
    static constexpr float kSmoothMinCutoff = 1.2f;
    static constexpr float kSmoothBeta = 0.02f;
    // How much of a sample a joint below the keypoint gate is allowed to
    // contribute. Enough to follow the body over a second or so, little enough
    // that a one-frame regression towards whatever happened to be in the crop
    // cannot move it.
    static constexpr float kGatedTrust = 0.15f;
    // Crop half-side per torso length for a tracked person, before roiEnlarge_.
    // Calibrated against the acquisition crop's own scale: this model's full-body
    // keypoint sits about 1.85 torso lengths from the mid-hip, so a tracked
    // person keeps the size it was given when it was acquired instead of visibly
    // changing scale the frame after.
    static constexpr float kLandmarkHalf = 1.85f;
    // How far ahead of the last measured hip a crop is centred, in frames of the
    // track's smoothed velocity. The landmark filters lag a moving person by a
    // frame or two; without this the crop trails them and spends its margin at
    // the wrong end.
    static constexpr float kPredictFrames = 1.5f;
    // Detector/skeleton disagreement, in torso lengths, at which a crop is
    // re-centred on the detector's mid-hip. The skeleton is a feedback loop
    // around its own crop and can slowly walk off a person; the detector cannot.
    // So it gets the last word -- but only where they actually disagree, because
    // using it every frame is what made this pipeline jitter in the first place.
    static constexpr float kReanchor = 0.35f;
    // How far, in torso lengths, a skeleton measured from a track's own crop may
    // sit from where that track was predicted to be. Past this, whoever the crop
    // locked onto is somebody else, and the track coasts rather than adopting
    // them.
    static constexpr float kAgree = 0.5f;
    // A carried skeleton is moved by the track's velocity, damped and clamped: a
    // constant-velocity extrapolation run long enough walks off the frame, taking
    // a skeleton that is about to be retired with it.
    static constexpr float kVelDamp = 0.75f;
    static constexpr float kVelMax = 0.4f;
    // A torso shorter than this in pixels says nothing about how large the crop
    // should be, so such a track falls back to the detector's own crop.
    static constexpr float kMinTorso = 4.f;
    // Visibility*presence below which a landmark is not used as one of the four
    // torso joints a crop is built from. Well below the overlay's own drawing
    // threshold: a joint worth keeping for scale is not worth drawing.
    static constexpr float kTorsoJointConf = 0.1f;
    // Mid-hip positions kept per track for the motion trail drawn by the overlay.
    static constexpr size_t kTrailLength = 24;

    // The four joints a crop is built from: the two hips and the two shoulders.
    // They say where the person is and which way up they are, and they are the
    // joints the pose model is most confident about.
    static constexpr int kShoulderL = 11, kShoulderR = 12, kHipL = 23, kHipR = 24;

    struct Track {
        int id_ = -1;
        cv::Rect2f box;
        // The track's mid-hip: the mean of the two hip landmarks of the skeleton
        // it last measured -- the same body point the detector regresses, but
        // measured on this person and smoothed. It is the identity anchor, the
        // crop centre and the trail point. `box` is a square RoI hint that is
        // none of those things.
        cv::Point2f hip_;
        cv::Point2f velocity;
        std::vector<cv::Vec4f> keypoints_;
        std::vector<cv::Point2f> trail_;
        // The crop's geometry, filtered separately from the skeleton that is
        // drawn: it is a different signal with a different job. It must not move
        // while the person stands still, and it may lag when they move, since a
        // whole torso of margin is around it either way. Reading it off the
        // drawn skeleton instead would couple it to the display smoothing, so
        // turning *that* off would make the crop jitter -- exactly backwards.
        OneEuro hipFx, hipFy, shoulderFx, shoulderFy, torsoF;
        // How long the torso is, and which way is up: what the next crop is built
        // from. An unset upValid_, or a torso below kMinTorso, means "no skeleton
        // worth building a crop from", and the track falls back to the detector's
        // crop.
        float torso_ = 0.f;
        float upAngle_ = 0.f;
        bool upValid_ = false;
        OneEuro fx[kNumKeypoints];
        OneEuro fy[kNumKeypoints];
        int missed_ = 0;
        int detMissed_ = 0;
        float score_ = 0.f;

        void configureFilters(float minCutoff, float beta) {
            for (int i = 0; i < kNumKeypoints; ++i) {
                fx[i].configure(minCutoff, beta, 1.f);
                fy[i].configure(minCutoff, beta, 1.f);
            }
            hipFx.configure(minCutoff, beta, 1.f);
            hipFy.configure(minCutoff, beta, 1.f);
            shoulderFx.configure(minCutoff, beta, 1.f);
            shoulderFy.configure(minCutoff, beta, 1.f);
            torsoF.configure(minCutoff, beta, 1.f);
        }
    };

    dnn::Net detNet_;
    dnn::Net poseNet_;
    cv::Size frameSize_;
    std::vector<Track> tracks_;
    int nextId_ = 0;

    static float anchorX(int row) {
        return kPoseDetectorAnchors[2 * row] * static_cast<float>(kDetSize);
    }
    static float anchorY(int row) {
        return kPoseDetectorAnchors[2 * row + 1] * static_cast<float>(kDetSize);
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

    // The mean of the two hip landmarks: the point both this pipeline and the
    // detector call the mid-hip, and the one both agree on well enough to
    // associate against.
    static cv::Point2f hipOf(const std::vector<cv::Vec4f>& k) {
        return (cv::Point2f(k[kHipL][0], k[kHipL][1]) + cv::Point2f(k[kHipR][0], k[kHipR][1])) *
               0.5f;
    }

    // The mean of the two shoulder landmarks: the other end of the torso.
    static cv::Point2f shoulderOf(const std::vector<cv::Vec4f>& k) {
        return (cv::Point2f(k[kShoulderL][0], k[kShoulderL][1]) +
                cv::Point2f(k[kShoulderR][0], k[kShoulderR][1])) *
               0.5f;
    }

    // Takes over the identity anchor and the crop geometry from the skeleton the
    // track has just measured. Runs for every accepted measurement, so a track
    // that is only ever measured from its own crop stays up to date without the
    // detector being involved at all. `raw` is the estimate before the display
    // filters: the crop geometry is filtered here rather than read off
    // `tr.keypoints_`, so that it does not depend on whether the demo is drawing
    // the skeleton smoothed. The torso joints are weighted by the same keypoint
    // gate as the drawn ones, because a crop centred and scaled from joints the
    // model does not believe in is a crop that is wrong in the same way twice.
    static void adoptSkeleton(Track& tr, const std::vector<cv::Vec4f>& raw, const Params& p,
                              float dt) {
        if (raw.size() < static_cast<size_t>(kNumKeypoints)) {
            tr.upValid_ = false;
            return;
        }
        const cv::Point2f prevHip = tr.hip_;
        const cv::Point2f rawHip = hipOf(raw);
        const float hipTrust = trustOf(p, 0.5f * (raw[kHipL][3] + raw[kHipR][3]));
        tr.hip_ = cv::Point2f(tr.hipFx.filter(rawHip.x, dt, hipTrust),
                              tr.hipFy.filter(rawHip.y, dt, hipTrust));
        // Velocity on the hip, not on the box centre, so it means the same thing
        // while the detector's box size drifts -- and measured between two
        // smoothed positions, so it is a velocity rather than the jitter that
        // used to be passed off as one.
        tr.velocity = tr.velocity * (1.f - kVelBlend) + (tr.hip_ - prevHip) * kVelBlend;

        // The crop is built from all four torso joints or from none: one hip on
        // its own is a guess about where the middle of the person is, and that
        // guess would be as noisy as the thing it replaces.
        if (raw[kShoulderL][3] < kTorsoJointConf || raw[kShoulderR][3] < kTorsoJointConf ||
            raw[kHipL][3] < kTorsoJointConf || raw[kHipR][3] < kTorsoJointConf) {
            tr.upValid_ = false;
            return;
        }
        const cv::Point2f rawShoulder = shoulderOf(raw);
        const float shoulderTrust =
            trustOf(p, 0.5f * (raw[kShoulderL][3] + raw[kShoulderR][3]));
        const cv::Point2f shoulder(tr.shoulderFx.filter(rawShoulder.x, dt, shoulderTrust),
                                   tr.shoulderFy.filter(rawShoulder.y, dt, shoulderTrust));
        const float torso =
            tr.torsoF.filter(static_cast<float>(cv::norm(rawShoulder - rawHip)), dt,
                             0.5f * (hipTrust + shoulderTrust));
        if (!(torso >= kMinTorso)) {
            tr.upValid_ = false;
            return;
        }
        tr.torso_ = torso;
        // Which way up the person is: from the mid-hip towards the shoulders.
        float angle = std::atan2(shoulder.y - tr.hip_.y, shoulder.x - tr.hip_.x);
        if (tr.upValid_) {
            // The same four joints a frame apart give the same direction, unless
            // one of the two estimates is bad enough to be upside down, in which
            // case this picks the reading nearest the last accepted one. A person
            // going through a handstand passes 180 degrees over many frames, so
            // nothing real is as abrupt as the flip this rejects.
            while (angle - tr.upAngle_ > kHalfPi) angle -= kTwoPi;
            while (tr.upAngle_ - angle > kHalfPi) angle += kTwoPi;
        }
        tr.upAngle_ = angle;
        tr.upValid_ = true;
    }

    // The crop a pose estimate is taken from: a square of half-side `half`,
    // centred on `centre`, rotated so that `up` ends up straight above `centre`,
    // read from the frame straight into the 256x256 pose input. Both crop sources
    // produce one of these, so everything from the warp onwards is shared and the
    // two differ only in where these three numbers come from.
    struct Crop {
        cv::Point2f centre;
        float half = 0.f;
        cv::Point2f up;
    };

    // The detector's own proposal: used to acquire a person, and as the fallback
    // whenever a track has no skeleton worth building a crop from. The half-side
    // is the mid-hip -> full-body distance, which is MediaPipe's own convention
    // for this model.
    static Crop acquisitionCrop(const cv::Mat& reg, int row, const Letterbox& lb, const Params& p) {
        const cv::Point2f midHip = auxKeypoint(reg, row, 0, lb);
        const cv::Point2f fullBody = auxKeypoint(reg, row, 1, lb);
        const float dist = static_cast<float>(cv::norm(midHip - fullBody));
        return {midHip, dist * p.roiEnlarge_, fullBody};
    }

    // Where a tracked person's crop goes: its skeleton says how big it is and
    // which way up it is, and `measuredHip` -- the detector's mid-hip, when this
    // frame has a detection -- gets the last word on where it is centred. Returns
    // an empty crop, and `reanchored` says whether the two disagreed, when the
    // track has no skeleton to build from.
    static Crop trackingCrop(const Track& tr, const cv::Point2f& measuredHip, const Params& p,
                             bool& reanchored) {
        reanchored = false;
        if (!tr.upValid_) return {};
        cv::Point2f centre = tr.hip_ + tr.velocity * kPredictFrames;
        if (cv::norm(measuredHip - centre) > kReanchor * tr.torso_) {
            centre = measuredHip;
            reanchored = true;
        }
        return {centre, p.roiEnlarge_ * kLandmarkHalf * tr.torso_, upOf(tr, centre)};
    }

    // The same crop for a track the detector dropped: there is no measurement to
    // re-centre on, so the track's own prediction is all there is.
    static Crop coastCrop(const Track& tr, const Params& p) {
        if (!tr.upValid_) return {};
        const cv::Point2f centre = tr.hip_ + tr.velocity * kPredictFrames;
        return {centre, p.roiEnlarge_ * kLandmarkHalf * tr.torso_, upOf(tr, centre)};
    }

    static cv::Point2f upOf(const Track& tr, const cv::Point2f& centre) {
        return centre + cv::Point2f(std::cos(tr.upAngle_), std::sin(tr.upAngle_));
    }

    // Runs the pose net on `crop` and, if it returns a person the model believes
    // in, makes it the track's skeleton. `gate` additionally requires the result
    // to be where the track was predicted to be, which only a re-measurement
    // without a detection needs: there, nothing else in this frame says whether
    // the crop still contains the person it was built for.
    bool measure(const cv::Mat& frameBGR, Track& tr, const Crop& crop, float dt, const Params& p,
                 int& poseRuns, bool gate = false) {
        if (crop.half < 1.f) return false;
        ++poseRuns;
        const cv::Point2f predicted = crop.centre;
        std::vector<cv::Vec4f> fresh = estimatePose(frameBGR, crop, p);
        if (fresh.empty()) return false;
        if (gate && tr.torso_ > 0.f && cv::norm(hipOf(fresh) - predicted) >
                                          kAgree * tr.torso_ + static_cast<float>(cv::norm(tr.velocity))) {
            // Whoever is standing where this track was is not the person it was
            // following. Not this frame's business to say so: the track coasts,
            // and coasts out.
            return false;
        }
        adoptSkeleton(tr, fresh, p, dt);
        tr.keypoints_ = p.smooth_ ? smoothKeypoints(tr, fresh, dt, p) : fresh;
        tr.missed_ = 0;
        return true;
    }

    // Moves a skeleton that was not measured this frame on by the track's
    // velocity, so a brief loss of evidence does not make the overlay blink.
    static void carrySkeleton(Track& tr) {
        if (tr.keypoints_.empty()) return;
        cv::Point2f v = tr.velocity * kVelDamp;
        const float limit = kVelMax * (tr.torso_ > 0.f ? tr.torso_ : 0.f);
        if (limit > 0.f && cv::norm(v) > limit) v *= limit / static_cast<float>(cv::norm(v));
        tr.velocity = v;
        for (cv::Vec4f& k : tr.keypoints_) {
            k[0] += v.x;
            k[1] += v.y;
        }
        tr.box.x += v.x;
        tr.box.y += v.y;
        tr.hip_ += v;
    }

    std::vector<cv::Vec4f> smoothKeypoints(Track& tr, const std::vector<cv::Vec4f>& fresh, float dt,
                                          const Params& p) {
std::vector<cv::Vec4f> out(fresh.size());
        for (size_t i = 0; i < fresh.size(); ++i) {
            const int k = static_cast<int>(i);
            const float score = fresh[i][3];
            // A joint the model does not claim to see is regressed from whatever
            // happens to be in the crop around that place, so its position is not
            // a measurement at all. Feeding it to the filter at full weight would
            // drag the skeleton towards it -- and the drag would then be read as
            // motion, which switches the filter off for every joint at once.
            // Below the gate the sample is taken at a fraction of its usual weight
            // instead of not at all, so the joint still follows the body but stops
            // following the guess. The score is never filtered: it is the model's
            // own statement about this frame, and a joint that stays unseen stops
            // being drawn either way.
            const float trust = p.smooth_ ? trustOf(p, score) : 1.f;
            out[i] = cv::Vec4f(tr.fx[k].filter(fresh[i][0], dt, trust),
                               tr.fy[k].filter(fresh[i][1], dt, trust), fresh[i][2], score);
        }
        return out;
    }

    // How much of a joint's sample the filter is allowed to take: everything for
    // a joint the model believes in, kGatedTrust for one below the gate. The
    // gate being a weight rather than a switch is what keeps an occluded joint
    // attached to the person instead of to the last place it was seen.
    static float trustOf(const Params& p, float score) {
        if (p.keypointGate_ <= 0.f || score >= p.keypointGate_) return 1.f;
        return kGatedTrust;
    }

    std::vector<cv::Vec4f> estimatePose(const cv::Mat& frameBGR, const Crop& crop, const Params& p) {
        if (crop.half < 1.f) return {};
        const cv::Rect square(cvFloor(crop.centre.x - crop.half), cvFloor(crop.centre.y - crop.half),
                              std::max(2, cvCeil(crop.centre.x + crop.half) -
                                              cvFloor(crop.centre.x - crop.half)),
                              std::max(2, cvCeil(crop.centre.y + crop.half) -
                                              cvFloor(crop.centre.y - crop.half)));
        // Clipping the square instead would squash the person and cut off limbs,
        // so take the part that overlaps the frame and let the warp's constant
        // border supply the padding outside it.
        const cv::Rect roi = square & cv::Rect(0, 0, frameBGR.cols, frameBGR.rows);
        if (roi.width < 2 || roi.height < 2) return {};
        // RoI coordinates are the square's minus these, which is how the
        // rotation's translation gets expressed in RoI coordinates below.
        const double left = roi.x - square.x;
        const double top = roi.y - square.y;

        // Rotate about the centre so that `up` ends up straight above it, with
        // the crop -> pose-input downscale folded into the same matrix, so that a
        // single warp reads the RoI straight into the 256x256 pose input.
        // Padding the RoI to a square, warping that at full size and only then
        // resizing to 256 -- what this replaces -- made three passes over a
        // ~1000x1000 buffer to produce 256x256 output pixels.
        const cv::Point2f rotCenter(crop.centre.x - static_cast<float>(square.x),
                                    crop.centre.y - static_cast<float>(square.y));
        const cv::Point2f upPoint(crop.up.x - static_cast<float>(square.x),
                                  crop.up.y - static_cast<float>(square.y));
        // `getRotationMatrix2D` turns points by -angle, hence the added pi/2.
        float radians = static_cast<float>(CV_PI / 2 + std::atan2(upPoint.y - rotCenter.y,
                                                                  upPoint.x - rotCenter.x));
        radians -= kTwoPi *
                   std::floor((radians + static_cast<float>(CV_PI)) / kTwoPi);
        const cv::Mat rotMat =
            cv::getRotationMatrix2D(rotCenter, radians * 180.f / static_cast<float>(CV_PI), 1.0);
        const double sx = kPoseSize / static_cast<double>(square.width);
        const double sy = kPoseSize / static_cast<double>(square.height);
        cv::Mat toPose(2, 3, CV_64F);
        toPose.at<double>(0, 0) = rotMat.at<double>(0, 0) * sx;
        toPose.at<double>(0, 1) = rotMat.at<double>(0, 1) * sy;
        toPose.at<double>(1, 0) = rotMat.at<double>(1, 0) * sx;
        toPose.at<double>(1, 1) = rotMat.at<double>(1, 1) * sy;
        toPose.at<double>(0, 2) = sx * (rotMat.at<double>(0, 0) * left +
                                        rotMat.at<double>(0, 1) * top + rotMat.at<double>(0, 2));
        toPose.at<double>(1, 2) = sy * (rotMat.at<double>(1, 0) * left +
                                        rotMat.at<double>(1, 1) * top + rotMat.at<double>(1, 2));
        // Landmarks come back in 256x256 model pixels; inverting the warp maps
        // them straight back onto the frame, so the rescale around the crop
        // centre and the un-rotation they used to need are already in there.
        cv::Mat poseToFrame;
        cv::invertAffineTransform(toPose, poseToFrame);
        poseToFrame.at<double>(0, 2) += roi.x;
        poseToFrame.at<double>(1, 2) += roi.y;

        cv::Mat poseIn;
        cv::warpAffine(frameBGR(roi), poseIn, toPose, cv::Size(kPoseSize, kPoseSize),
                       cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));

        dnn::Image2BlobParams posePrms;
        posePrms.datalayout = cv::DNN_LAYOUT_NHWC;
        posePrms.ddepth = CV_32F;
        posePrms.mean = cv::Scalar::all(0);
        posePrms.scalefactor = cv::Scalar::all(1.f / 255.f);
        posePrms.size = cv::Size(kPoseSize, kPoseSize);
        // The warp leaves the frame's BGR order alone; let the blob swap the
        // channels rather than paying for a separate full-size cvtColor.
        posePrms.swapRB = true;
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
        // Depth is in the same units as the pose input's pixels, so it has to be
        // scaled back up by however much the RoI was downscaled.
        const float depthScale = static_cast<float>(std::max(1.0 / sx, 1.0 / sy));
        std::vector<cv::Vec4f> kpts(kNumKeypoints);
        for (int i = 0; i < kNumKeypoints; ++i) {
            const double modelX = landmarks.at<float>(i, 0);
            const double modelY = landmarks.at<float>(i, 1);
            const float visibility = 1.f / (1.f + std::exp(-landmarks.at<float>(i, 3)));
            const float presence = 1.f / (1.f + std::exp(-landmarks.at<float>(i, 4)));
            kpts[i] = cv::Vec4f(
                static_cast<float>(poseToFrame.at<double>(0, 0) * modelX +
                                   poseToFrame.at<double>(0, 1) * modelY +
                                   poseToFrame.at<double>(0, 2)),
                static_cast<float>(poseToFrame.at<double>(1, 0) * modelX +
                                   poseToFrame.at<double>(1, 1) * modelY +
                                   poseToFrame.at<double>(1, 2)),
                landmarks.at<float>(i, 2) * depthScale,
                visibility * presence);
        }
        return kpts;
    }

    // The acquisition crop spelled out through the detector, for the harnesses
    // that drive this pipeline one stage at a time.
    std::vector<cv::Vec4f> estimatePose(const cv::Mat& frameBGR, const cv::Mat& reg, int row,
                                        const Letterbox& lb, const Params& p) {
        return estimatePose(frameBGR, acquisitionCrop(reg, row, lb, p), p);
    }

    static constexpr float kTwoPi = static_cast<float>(CV_PI * 2);
    static constexpr float kHalfPi = static_cast<float>(CV_PI / 2);
};

// ---------------------------------------------------------------------------
// Overlay helpers shared by the plan and the verification tools.
// ---------------------------------------------------------------------------

// A keypoint is drawn only if visibility * presence clears this. One place, so
// the box, the bones, the joints and the trail cannot disagree about which
// joints a skeleton has.
inline constexpr float kKeypointConf = 0.3f;

// Box around the skeleton itself, not around the detector's RoI hint: the
// model's box is a near-constant square around the torso and says nothing
// about where the person actually is. The body extent is squared up about
// its own centre and given a little air, matching the pose crop's own
// enlargement.
[[nodiscard]] inline cv::Rect2f skeletonBox(const std::vector<cv::Vec4f>& kpts,
                                            float keypointConf = kKeypointConf) {
    constexpr float kBoxEnlarge = 1.25f;
    float x0 = std::numeric_limits<float>::max(), y0 = x0;
    float x1 = -x0, y1 = -x0;
    for (const cv::Vec4f& k : kpts) {
        if (k[3] < keypointConf) continue;
        x0 = std::min(x0, k[0]);
        y0 = std::min(y0, k[1]);
        x1 = std::max(x1, k[0]);
        y1 = std::max(y1, k[1]);
    }
    if (x1 <= x0 || y1 <= y0) return {};
    const float cx = 0.5f * (x0 + x1), cy = 0.5f * (y0 + y1);
    const float half = 0.5f * kBoxEnlarge * std::max(x1 - x0, y1 - y0);
    return {(cx - half) * 1.25, cy - half, 1.5 * half, 2 * half};
}

// ---------------------------------------------------------------------------
// Shared state between GUI thread and workers.
// ---------------------------------------------------------------------------
struct SharedPoseState {
    std::vector<MediaPipePosePipeline::Person> persons_;
    // Stage timings of the last processed frame, in milliseconds, and how many
    // pose-net runs it took, for the panel.
    float detectMs_ = 0.f;
    float poseMs_ = 0.f;
    float totalMs_ = 0.f;
    int poseRuns_ = 0;
    // The tunables below are the single source of truth for the pipeline: the
    // panel edits them and #params is rebuilt from them on every frame, so
    // nothing else can quietly disagree about what the demo is running.
    float detConf_ = 0.65f;
    float poseConf_ = 0.4f;
    float roiEnlarge_ = 1.1f;
    int maxPersons_ = 2;
    int maxMissed_ = 5;
    bool smooth_ = true;
    // Track an already-tracked person with their own skeleton instead of with the
    // detector's keypoints. On: a still skeleton, and a dropped detection costs a
    // frame of tracking rather than the identity. Off: every frame is measured
    // through the detector's own, noisier crop.
    bool landmarkRoi_ = true;
    // Visibility*presence below which a landmark is treated as "not seen" and
    // keeps its last filtered position. 0 disables it. See the pipeline header.
    float keypointGate_ = 0.15f;
    // Source frame interval, seconds, used as the 1 Euro filter's time step.
    // It comes from the source's nominal frame rate because V4D's Source
    // exposes no per-frame timestamp; on a variable-rate source, or when the
    // pipeline drops frames to keep up, the filter therefore runs in a time
    // base that is smaller than the real one. That errs towards smoothing a
    // little too much rather than too little, so it adds lag on fast motion
    // instead of jitter, which is the better of the two failure modes here.
    float frameDt_ = 1.f / 60.f;
    // Per-keypoint drawing threshold; not a pipeline parameter, only the
    // overlay's.
    float keypointConf_ = kKeypointConf;
    bool enabled_ = true;
    bool fullscreen_ = false;
    bool showBoxes_ = true;
    bool showTrails_ = true;
    bool showDetectorBox_ = false;

    [[nodiscard]] MediaPipePosePipeline::Params params() const {
        return {detConf_,  poseConf_, roiEnlarge_, maxPersons_, smooth_,
                maxMissed_, frameDt_,   keypointGate_, landmarkRoi_};
    }
};

}  // namespace samples
}  // namespace cv

#endif  // OPENCV_SKELETAL_TRACKER_PIPELINE_HPP
