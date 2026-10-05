// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#include "samples.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <opencv2/calib3d/calib3d.hpp>
#include <opencv2/features.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/tracking.hpp>
#include <opencv2/v4d/v4d.hpp>

using std::string;
using std::vector;
using namespace cv::v4d;

struct MarkerState {
  cv::Mat markerImg_;
  std::vector<cv::KeyPoint> markerKeypoints_;
  cv::Mat markerDescriptors_;
  std::vector<cv::Point2f> markerCorners_;

  cv::Ptr<cv::Feature2D> detector_;
  cv::Ptr<cv::DescriptorMatcher> matcher_;
  cv::Ptr<cv::Tracker> tracker_;

  bool isTracked_ = false;
  int missCount_ = 0;
  uint64_t detectCnt_ = 0;
  cv::Rect currentBox_;
  std::vector<cv::Point2f> currentCorners_;
};

class NaturalMarkerDemoPlan : public V4DPlan {
private:
  constexpr static auto UMAT_COPY_TO_ =
      _OLMC_(void, cv::UMat, &cv::UMat::copyTo, cv::OutputArray);

  struct TrackParams {
    int detectInterval_ = 15;
    int maxMiss_ = 5;
    float smoothFactor_ = 0.3f;
    bool forceRedetect_ = false;
    bool enabled_ = true;
    bool showHud_ = true;
  };
  static TrackParams trackParams_;

  struct Params {
    cv::Size downSize_;
    cv::Size_<float> scale_;
    std::string markerPath_;
  } params_;

  struct Frames {
    cv::UMat background_;
    cv::UMat videoFrame_, videoFrameBGR_, videoFrameDown_;
    cv::UMat videoFrameDownGrey_;
  } frames_;

  inline static MarkerState markerState_;
  inline static std::vector<cv::Point2f> trackedCorners_;
  inline static bool isTrackedShared_ = false;

  std::vector<cv::Point2f> outCorners_;
  bool outIsTracked_ = false;

  Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
  Property<uint64_t> frameNo_ = P<uint64_t>(GlobalState::Keys::FRAME_CNT);

  static void prepare_frames(const Params &params, Frames &frames) {
    cv::resize(frames.videoFrameBGR_, frames.videoFrameDown_, params.downSize_);
    cv::cvtColor(frames.videoFrameDown_, frames.videoFrameDownGrey_,
                 cv::COLOR_RGB2GRAY);
    frames.videoFrame_.copyTo(frames.background_);
  }

  static void present(cv::UMat &framebuffer, const cv::UMat &background) {
    cv::add(background, framebuffer, framebuffer);
  }

  static void smooth_corners(std::vector<cv::Point2f> &oldCorners,
                            const std::vector<cv::Point2f> &newCorners,
                            float factor) {
    if (oldCorners.size() != 4 || newCorners.size() != 4) {
      oldCorners = newCorners;
      return;
    }
    for (size_t i = 0; i < 4; ++i) {
      oldCorners[i].x += factor * (newCorners[i].x - oldCorners[i].x);
      oldCorners[i].y += factor * (newCorners[i].y - oldCorners[i].y);
    }
  }

  static void update_marker_tracking(const cv::UMat &videoFrameDownGrey,
                                      MarkerState &ms, const TrackParams &tp,
                                      std::vector<cv::Point2f> &outCorners,
                                      bool &outIsTracked) {
    ++ms.detectCnt_;

    const int detectInterval = std::max(1, tp.detectInterval_);
    bool doDetect = !ms.isTracked_ || tp.forceRedetect_ ||
                    ((ms.detectCnt_ % size_t(detectInterval)) == 0);

    cv::Mat frameMat = videoFrameDownGrey.getMat(cv::ACCESS_READ);

    if (ms.isTracked_ && !tp.forceRedetect_) {
      cv::Rect predictedBox;
      if (ms.tracker_ && ms.tracker_->update(videoFrameDownGrey, predictedBox)) {
        ms.missCount_ = 0;

        // Shift corners based on tracker movement
        cv::Point2f centerDiff(
            (predictedBox.x + predictedBox.width / 2.0f) -
                (ms.currentBox_.x + ms.currentBox_.width / 2.0f),
            (predictedBox.y + predictedBox.height / 2.0f) -
                (ms.currentBox_.y + ms.currentBox_.height / 2.0f));

        std::vector<cv::Point2f> newCorners = ms.currentCorners_;
        for (auto &pt : newCorners) {
          pt += centerDiff;
        }

        smooth_corners(ms.currentCorners_, newCorners, tp.smoothFactor_);
        ms.currentBox_ = predictedBox;
      } else {
        ++ms.missCount_;
        if (ms.missCount_ >= tp.maxMiss_) {
          ms.isTracked_ = false;
          doDetect = true;
        }
      }
    }

    if (doDetect && !ms.markerDescriptors_.empty()) {
      std::vector<cv::KeyPoint> frameKeypoints;
      cv::Mat frameDescriptors;
      ms.detector_->detectAndCompute(frameMat, cv::noArray(), frameKeypoints,
                                     frameDescriptors);

      if (!frameDescriptors.empty() && frameKeypoints.size() >= 10) {
        std::vector<std::vector<cv::DMatch>> knnMatches;
        ms.matcher_->knnMatch(ms.markerDescriptors_, frameDescriptors,
                              knnMatches, 2);

        std::vector<cv::Point2f> objPts;
        std::vector<cv::Point2f> scenePts;

        for (const auto &m : knnMatches) {
          if (m.size() >= 2 && m[0].distance < 0.75f * m[1].distance) {
            objPts.push_back(ms.markerKeypoints_[m[0].queryIdx].pt);
            scenePts.push_back(frameKeypoints[m[0].trainIdx].pt);
          }
        }

        if (objPts.size() >= 8) {
          cv::Mat H = cv::findHomography(objPts, scenePts, cv::RANSAC, 3.0);
          if (!H.empty()) {
            std::vector<cv::Point2f> sceneCorners(4);
            cv::perspectiveTransform(ms.markerCorners_, sceneCorners, H);

            // Sanity check detected polygon
            double area = cv::contourArea(sceneCorners);
            if (area > 100.0 && area < (frameMat.cols * frameMat.rows * 0.9)) {
              ms.currentCorners_ = sceneCorners;
              ms.currentBox_ = cv::boundingRect(sceneCorners);

              // Constrain bounding box to frame bounds
              ms.currentBox_ &= cv::Rect(0, 0, frameMat.cols, frameMat.rows);

              if (ms.currentBox_.width > 10 && ms.currentBox_.height > 10) {
                ms.tracker_ = cv::TrackerKCF::create();
                ms.tracker_->init(videoFrameDownGrey, ms.currentBox_);
                ms.isTracked_ = true;
                ms.missCount_ = 0;
              }
            }
          }
        }
      }
    }

    outIsTracked = ms.isTracked_;
    if (ms.isTracked_) {
      outCorners = ms.currentCorners_;
    } else {
      outCorners.clear();
    }
  }

  static void copy_tracking_results(const std::vector<cv::Point2f> &srcCorners,
                                     bool srcTracked,
                                     std::vector<cv::Point2f> &dstCorners,
                                     bool &dstTracked) {
    dstCorners = srcCorners;
    dstTracked = srcTracked;
  }

  class MarkerOverlay {
  public:
    void draw(const cv::Size &sz, const Params &params,
              const std::vector<cv::Point2f> &corners, bool isTracked,
              uint64_t frameNo, bool showHud) const {
      using namespace cv::v4d::nvg;
      clearScreen();

      if (isTracked && corners.size() == 4) {
        // Transform downscaled coordinates back to full viewport scale
        std::vector<cv::Point2f> fullCorners(4);
        for (size_t i = 0; i < 4; ++i) {
          fullCorners[i].x = corners[i].x * params.scale_.width;
          fullCorners[i].y = corners[i].y * params.scale_.height;
        }

        // Polygon boundary
        beginPath();
        moveTo(fullCorners[0].x, fullCorners[0].y);
        lineTo(fullCorners[1].x, fullCorners[1].y);
        lineTo(fullCorners[2].x, fullCorners[2].y);
        lineTo(fullCorners[3].x, fullCorners[3].y);
        closePath();

        strokeWidth(std::fmax(4.0, sz.width / 480.0));
        strokeColor(cv::v4d::convert_pix(cv::Scalar(0, 255, 128, 220),
                                         cv::COLOR_HLS2BGR));
        fillColor(cv::v4d::convert_pix(cv::Scalar(0, 255, 128, 40),
                                       cv::COLOR_HLS2BGR));
        fill();
        stroke();

        // Corner indicators
        for (size_t i = 0; i < 4; ++i) {
          beginPath();
          circle(fullCorners[i].x, fullCorners[i].y, 6.0f);
          fillColor(cv::v4d::convert_pix(cv::Scalar(255, 200, 0, 255),
                                         cv::COLOR_HLS2BGR));
          fill();
        }

        // Label above marker
        beginPath();
        fontSize(24.0f);
        fontFace("sans-bold");
        fillColor(cv::Scalar(255, 255, 255, 255));
        textAlign(NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
        string label = "Natural Marker";
        text(fullCorners[0].x, fullCorners[0].y - 8.0f, label.c_str(),
             label.c_str() + label.size());
      }

      if (showHud) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Frame: %llu | Status: %s",
                 (unsigned long long)frameNo,
                 isTracked ? "TRACKING" : "SEARCHING");

        fontSize(22.0f);
        fontFace("sans-bold");
        fillColor(isTracked ? cv::Scalar(0, 255, 128, 240)
                            : cv::Scalar(255, 100, 100, 240));
        textAlign(NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        text(20.0f, 20.0f, buf, buf + strlen(buf));
      }
    }
  } overlay_;

public:
  NaturalMarkerDemoPlan(const std::string &markerPath) {
    _shared(trackParams_);
    params_.markerPath_ = markerPath;
  }

  void gui() override {
    imgui(
        [](TrackParams &tp, bool tracked) {
          using namespace ImGui;
          Begin("Natural Marker Tracking");
          Checkbox("Enable Tracking", &tp.enabled_);
          Checkbox("Show HUD", &tp.showHud_);
          SliderInt("Re-detect Interval", &tp.detectInterval_, 1, 60);
          SliderInt("Miss Threshold", &tp.maxMiss_, 1, 30);
          SliderFloat("Smoothing Factor", &tp.smoothFactor_, 0.05f, 0.95f);

          if (Button("Re-detect Marker")) {
            tp.forceRedetect_ = true;
          } else {
            tp.forceRedetect_ = false;
          }

          Text("Marker State: %s", tracked ? "TRACKED" : "SEARCHING");
          End();
        },
        RWS(trackParams_), CS(isTrackedShared_));
  }

  void setup() override {
    plain(
        [](const cv::Size &sz, MarkerState &ms, Frames &frames, Params &params) {
          params.scale_ = {2.0f, 2.0f};
          params.downSize_ = {
              static_cast<int>(sz.width / params.scale_.width),
              static_cast<int>(sz.height / params.scale_.height)};

          frames.videoFrame_.create(sz, CV_8UC4);
          frames.videoFrameBGR_.create(sz, CV_8UC3);
          frames.videoFrameDownGrey_.create(sz, CV_8UC1);

          // Load reference marker image
          std::string resolvedMarker = cv::samples::findFile(params.markerPath_);
          ms.markerImg_ = cv::imread(resolvedMarker, cv::IMREAD_GRAYSCALE);
          if (ms.markerImg_.empty()) {
            CV_Error(cv::Error::StsBadArg,
                     "Could not load marker image: " + params.markerPath_);
          }

          // Scale down marker image if too large
          if (ms.markerImg_.cols > 640 || ms.markerImg_.rows > 640) {
            float mscale = 640.0f / std::max(ms.markerImg_.cols, ms.markerImg_.rows);
            cv::resize(ms.markerImg_, ms.markerImg_, cv::Size(), mscale, mscale);
          }

          ms.detector_ = cv::ORB::create(1000);
          ms.matcher_ = cv::DescriptorMatcher::create(
              cv::DescriptorMatcher::BRUTEFORCE_HAMMING);

          ms.detector_->detectAndCompute(ms.markerImg_, cv::noArray(),
                                         ms.markerKeypoints_,
                                         ms.markerDescriptors_);

          ms.markerCorners_ = {
              cv::Point2f(0, 0),
              cv::Point2f(static_cast<float>(ms.markerImg_.cols), 0),
              cv::Point2f(static_cast<float>(ms.markerImg_.cols),
                          static_cast<float>(ms.markerImg_.rows)),
              cv::Point2f(0, static_cast<float>(ms.markerImg_.rows))};
        },
        size_, RWS(markerState_), RW(frames_), RW(params_));
  }

  void infer() override {
    fb(UMAT_COPY_TO_, RW(frames_.videoFrame_));

    plain(cv::cvtColor, R(frames_.videoFrame_), RW(frames_.videoFrameBGR_),
          V(cv::COLOR_BGRA2RGB), V(0), V(cv::ALGO_HINT_DEFAULT))
        ->plain(prepare_frames, R(params_), RW(frames_));

    branch(BranchType::SINGLE, always_)
        ->plain(update_marker_tracking, R(frames_.videoFrameDownGrey_),
                RWS(markerState_), CS(trackParams_), RW(outCorners_),
                RW(outIsTracked_))
        ->plain(copy_tracking_results, R(outCorners_), R(outIsTracked_),
                RWS(trackedCorners_), RWS(isTrackedShared_))
        ->endBranch();

    nvg(&MarkerOverlay::draw, R(overlay_), size_, R(params_),
        CS(trackedCorners_), CS(isTrackedShared_), frameNo_,
        CS(trackParams_.showHud_))
        ->fb(present, R(frames_.background_));
  }
};

NaturalMarkerDemoPlan::TrackParams NaturalMarkerDemoPlan::trackParams_;

V4D_DEMO_MAIN(int argc, char **argv) {
  cv::v4d::add_asset_search_paths();

  std::string videoFile = demo_video_input("videos/marker_test.mp4", argc, argv);
  std::string markerFile = (argc > 2) ? argv[2] : "harddisk_as_marker.jpeg";

  if (!demo_video_input_readable(videoFile)) {
    std::cerr << "Usage: natural-marker-demo [video-file] [marker-image-file]"
              << std::endl;
    return 1;
  }

  cv::Rect viewport(0, 0, 1280, 720);
  cv::Ptr<V4D> runtime =
      V4D::init(viewport, "Natural Marker Tracking Demo",
                AllocateFlags::NANOVG | AllocateFlags::IMGUI);

  auto src = Source::makeDefault(runtime, videoFile);
  runtime->setSource(src);

  V4DPlan::run<NaturalMarkerDemoPlan>(2, markerFile);
  return 0;
}
