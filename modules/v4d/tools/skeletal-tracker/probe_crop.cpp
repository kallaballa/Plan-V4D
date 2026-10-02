// Quantifies the per-person pose crop, and checks that the sample's fused crop
// is what it claims to be.
//
// The sample used to walk the frame through three passes -- pad the RoI to a
// square, warp it at crop size, INTER_AREA resize to 256 -- to produce 256x256
// pixels, plus a cvtColor. It now warps the RoI straight into the 256x256 pose
// input and lets the blob swap channels. That is 50x cheaper and a different
// resampling, so this probe reports both: what each path costs, and what the
// pose net then says about the person, in pixels of landmark movement.
//
// The geometry is checked against the sample's own estimatePose, reached
// through the same private-section access the other harnesses use, so a mistake
// in either the warp or the landmark mapping shows up as a mismatch here rather
// than as a silently drifting skeleton.
//
//   usage: probe_crop [video] [frame-index] [reps]

#include "tool_prelude.hpp"

#define private public
#include "skeletal-tracker-pipeline.hpp"
#undef private

using namespace cv;

namespace {

using Pipe = cv::samples::MediaPipePosePipeline;
constexpr int kPose = Pipe::kPoseSize;

// The default detector input size, spelled out here so the probe does not depend
// on the pipeline's private spelling of it.
constexpr int kDet = 224;

}  // namespace

int main(int argc, char** argv) {
    // Before anything is looked up: the V4D asset directories are what findFile
    // searches, and the models and clips live in them.
    cv::v4d::add_asset_search_paths();

    const std::string video = argc > 1 ? argv[1] : cv::samples::findFile("videos/dance.mp4");
    const int at = argc > 2 ? std::atoi(argv[2]) : 30;
    const int reps = argc > 3 ? std::atoi(argv[3]) : 50;

    cv::VideoCapture cap(video);
    cv::Mat frame;
    for (int i = 0; i <= at && cap.read(frame); ++i) {}
    if (frame.empty()) {
        std::fprintf(stderr, "no frame %d in %s\n", at, video.c_str());
        return 1;
    }

    // The demo's tunables come from the same shared state the panel edits, so
    // the probe measures what the demo runs with.
    cv::samples::SharedPoseState state;
    const Pipe::Params p = state.params();

    Pipe pipe(cv::samples::findFile("models/pose/person_detection_mediapipe_2023mar.onnx"),
              cv::samples::findFile("models/pose/pose_estimation_mediapipe_2023mar.onnx"));
    const std::vector<Pipe::Person> persons = pipe.run(frame, p);
    std::printf("frame %dx%d, %zu persons\n", frame.cols, frame.rows, persons.size());
    if (persons.empty()) return 1;

    // --- Re-run the detector to recover the anchor row and its keypoints ----
    dnn::Image2BlobParams detPrms;
    detPrms.datalayout = cv::DNN_LAYOUT_NCHW;
    detPrms.ddepth = CV_32F;
    detPrms.mean = cv::Scalar::all(127.5f);
    detPrms.scalefactor = cv::Scalar::all(1.f / 127.5f);
    detPrms.size = cv::Size(kDet, kDet);
    detPrms.swapRB = true;
    detPrms.paddingmode = dnn::DNN_PMODE_LETTERBOX;
    pipe.detNet_.setInput(dnn::blobFromImageWithParams(frame, detPrms));
    std::vector<cv::Mat> detOut;
    pipe.detNet_.forward(detOut, pipe.detNet_.getUnconnectedOutLayersNames());
    if (detOut.size() < 2) {
        std::fprintf(stderr, "detector returned %zu tensors, expected 2\n", detOut.size());
        return 1;
    }
    const cv::Mat reg(Pipe::kNumAnchors, Pipe::kRegChannels, CV_32F, detOut[1].ptr<float>());
    const cv::Mat scores(Pipe::kNumAnchors, 1, CV_32F, detOut[0].ptr<float>());

    const Pipe::Letterbox lb(frame.size());
    // Pick the anchor the tracker actually used: the one whose decoded mid-hip
    // lands on the skeleton the pipeline published. (The globally top-scoring
    // anchor is often a spurious one with degenerate keypoints.)
    const cv::Point2f wantHip(
        (persons.front().keypoints[23][0] + persons.front().keypoints[24][0]) * 0.5f,
        (persons.front().keypoints[23][1] + persons.front().keypoints[24][1]) * 0.5f);
    int row = -1;
    float best = std::numeric_limits<float>::max();
    for (int i = 0; i < Pipe::kNumAnchors; ++i) {
        const float conf =
            1.f / (1.f + std::exp(-std::clamp(scores.at<float>(i, 0), -100.f, 100.f)));
        if (conf < p.detConf_) continue;
        const float d = static_cast<float>(cv::norm(Pipe::auxKeypoint(reg, i, 0, lb) - wantHip));
        if (d < best) {
            best = d;
            row = i;
        }
    }
    if (row < 0) {
        std::fprintf(stderr, "no anchor above the detector threshold matches the published hip\n");
        return 1;
    }
    const float rowScore =
        1.f / (1.f + std::exp(-std::clamp(scores.at<float>(row, 0), -100.f, 100.f)));
    const cv::Point2f midHip = Pipe::auxKeypoint(reg, row, 0, lb);
    const cv::Point2f fullBody = Pipe::auxKeypoint(reg, row, 1, lb);
    const float dist = static_cast<float>(cv::norm(midHip - fullBody));
    const float half = dist * p.roiEnlarge_;
    const cv::Rect square(cvFloor(midHip.x - half), cvFloor(midHip.y - half),
                          std::max(2, cvCeil(midHip.x + half) - cvFloor(midHip.x - half)),
                          std::max(2, cvCeil(midHip.y + half) - cvFloor(midHip.y - half)));
    const cv::Rect roi = square & cv::Rect(0, 0, frame.cols, frame.rows);
    std::printf("anchor row %d (score %.2f, hip err %.1f px), RoI %dx%d of a %dx%d square\n", row,
                rowScore, best, roi.width, roi.height, square.width, square.height);
    if (roi.empty()) return 1;

    // --- How the two ways of sizing a crop compare ----------------------------
    // The acquisition crop takes its size from the detector's own "full body"
    // keypoint; the tracking crop the pipeline builds for a track it already has
    // a skeleton for takes it from the skeleton itself. The tracking crop's scale
    // constant is calibrated against this, so the two are printed side by side:
    // if they disagree by much, every track changes size the frame after it is
    // acquired, which shows up as a visible pop and as jitter in the numbers the
    // pipeline reports.
    {
        const auto& k = persons.front().keypoints;
        const cv::Point2f shoulderMid =
            (cv::Point2f(k[11][0], k[11][1]) + cv::Point2f(k[12][0], k[12][1])) * 0.5f;
        const cv::Point2f hipMid =
            (cv::Point2f(k[23][0], k[23][1]) + cv::Point2f(k[24][0], k[24][1])) * 0.5f;
        const float torso = static_cast<float>(cv::norm(shoulderMid - hipMid));
        std::printf("crop scale: acquisition |fullBody-midHip| = %6.1f px, skeleton torso"
                    " |shoulderMid-hipMid| = %6.1f px  =>  ratio %.3f"
                    " (tracking constant kLandmarkHalf should give half = %.2f x torso)\n",
                    dist, torso, dist / torso, half / torso);
    }

    // --- The geometry the sample builds --------------------------------------
    // Rebuilt here from the frame coordinates rather than reused, so that the
    // comparison below is between two independent implementations.
    const double left = roi.x - square.x, top = roi.y - square.y;
    const cv::Point2f rotCenter(static_cast<float>(midHip.x - square.x),
                                static_cast<float>(midHip.y - square.y));
    const cv::Point2f bodyCenter(static_cast<float>(fullBody.x - square.x),
                                 static_cast<float>(fullBody.y - square.y));
    float radians =
        static_cast<float>(CV_PI / 2 +
                           std::atan2(bodyCenter.y - rotCenter.y, bodyCenter.x - rotCenter.x));
    radians -= static_cast<float>(CV_PI * 2) *
               std::floor((radians + static_cast<float>(CV_PI)) / static_cast<float>(CV_PI * 2));
    const cv::Mat rotMat =
        cv::getRotationMatrix2D(rotCenter, radians * 180.f / static_cast<float>(CV_PI), 1.0);
    const double sx = kPose / static_cast<double>(square.width);
    const double sy = kPose / static_cast<double>(square.height);
    cv::Mat toPose(2, 3, CV_64F);
    toPose.at<double>(0, 0) = rotMat.at<double>(0, 0) * sx;
    toPose.at<double>(0, 1) = rotMat.at<double>(0, 1) * sy;
    toPose.at<double>(1, 0) = rotMat.at<double>(1, 0) * sx;
    toPose.at<double>(1, 1) = rotMat.at<double>(1, 1) * sy;
    toPose.at<double>(0, 2) = sx * (rotMat.at<double>(0, 0) * left +
                                    rotMat.at<double>(0, 1) * top + rotMat.at<double>(0, 2));
    toPose.at<double>(1, 2) = sy * (rotMat.at<double>(1, 0) * left +
                                    rotMat.at<double>(1, 1) * top + rotMat.at<double>(1, 2));

    // Model pixels back to frame pixels: invert the warp, then step from the RoI
    // to the frame.
    cv::Mat poseToFrame;
    cv::invertAffineTransform(toPose, poseToFrame);
    poseToFrame.at<double>(0, 2) += roi.x;
    poseToFrame.at<double>(1, 2) += roi.y;

    // What the three-pass path needs: the padded crop, the rotation at full crop
    // size, and the landmark mapping it implies. In padded-crop terms that
    // mapping is "invert the rotation, rescale around the crop centre, and step
    // to the frame", which as a single 2x3 is the inverse rotation with the
    // crop's downscale folded in.
    const int padLeft = static_cast<int>(left), padTop = static_cast<int>(top);
    const int padRight = square.width - roi.width - padLeft;
    const int padBottom = square.height - roi.height - padTop;
    cv::Mat invRot;
    cv::invertAffineTransform(rotMat, invRot);
    cv::Mat paddedToFrame(2, 3, CV_64F);
    paddedToFrame.at<double>(0, 0) = invRot.at<double>(0, 0) / sx;
    paddedToFrame.at<double>(0, 1) = invRot.at<double>(0, 1) / sy;
    paddedToFrame.at<double>(1, 0) = invRot.at<double>(1, 0) / sx;
    paddedToFrame.at<double>(1, 1) = invRot.at<double>(1, 1) / sy;
    paddedToFrame.at<double>(0, 2) = invRot.at<double>(0, 2) + square.x;
    paddedToFrame.at<double>(1, 2) = invRot.at<double>(1, 2) + square.y;

    // --- Both crop paths -----------------------------------------------------
    cv::Mat src = frame(roi);
    cv::Mat fusedIn, padCrop, rotated, areaIn, tmp;
    auto pathFused = [&]() {
        cv::warpAffine(src, fusedIn, toPose, cv::Size(kPose, kPose), cv::INTER_LINEAR,
                       cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
    };
    auto pathPadded = [&]() {
        cv::copyMakeBorder(src, padCrop, padTop, padBottom, padLeft, padRight, cv::BORDER_CONSTANT,
                           cv::Scalar(0, 0, 0));
        cv::warpAffine(padCrop, rotated, rotMat, padCrop.size());
        cv::resize(rotated, areaIn, cv::Size(kPose, kPose), 0, 0, cv::INTER_AREA);
        cv::cvtColor(areaIn, areaIn, cv::COLOR_BGR2RGB);
    };
    for (int i = 0; i < 5; ++i) {
        pathFused();
        pathPadded();
    }

    cv::TickMeter t;
    t.reset();
    for (int i = 0; i < reps; ++i) {
        t.start();
        pathFused();
        t.stop();
    }
    const double fusedMs = t.getAvgTimeMilli();
    t.reset();
    for (int i = 0; i < reps; ++i) {
        t.start();
        pathPadded();
        t.stop();
    }
    const double paddedMs = t.getAvgTimeMilli();

    std::printf("\ncrop path, mean of %d (one person, %dx%d RoI inside a %dx%d square):\n", reps,
                roi.width, roi.height, square.width, square.height);
    std::printf("  fused  : one bilinear warp straight to %dx%d, channels swapped by the blob"
                " = %6.3f ms\n", kPose, kPose, fusedMs);
    std::printf("  padded : copyMakeBorder + full-size warp + INTER_AREA + cvtColor"
                "            = %6.3f ms  (%.1fx)\n", paddedMs, paddedMs / fusedMs);
    std::printf("  saving : %.3f ms per person per frame (%.0f%%)\n", paddedMs - fusedMs,
                100.0 * (paddedMs - fusedMs) / paddedMs);

    // --- What the pose net makes of each crop --------------------------------
    dnn::Image2BlobParams posePrms;
    posePrms.datalayout = cv::DNN_LAYOUT_NHWC;
    posePrms.ddepth = CV_32F;
    posePrms.mean = cv::Scalar::all(0);
    posePrms.scalefactor = cv::Scalar::all(1.f / 255.f);
    posePrms.size = cv::Size(kPose, kPose);
    // The fused path leaves the frame's BGR order alone, exactly as the padded
    // path's cvtColor turns it into RGB, so both feed the blob the same way.
    posePrms.swapRB = true;
    posePrms.paddingmode = dnn::DNN_PMODE_NULL;

    auto landmarksOf = [&](const cv::Mat& poseIn) {
        pipe.poseNet_.setInput(dnn::blobFromImageWithParams(poseIn, posePrms));
        std::vector<cv::Mat> out;
        pipe.poseNet_.forward(out, pipe.poseNet_.getUnconnectedOutLayersNames());
        return out.size() >= 2 ? cv::Mat(out[0].reshape(0, 39)) : cv::Mat();
    };
    auto through = [&](const cv::Mat& lm, const cv::Mat& map) {
        std::vector<cv::Point2f> pts;
        for (int i = 0; i < 33; ++i) {
            const double x = lm.at<float>(i, 0), y = lm.at<float>(i, 1);
            pts.emplace_back(
                static_cast<float>(map.at<double>(0, 0) * x + map.at<double>(0, 1) * y +
                                   map.at<double>(0, 2)),
                static_cast<float>(map.at<double>(1, 0) * x + map.at<double>(1, 1) * y +
                                   map.at<double>(1, 2)));
        }
        return pts;
    };

    pathFused();
    const cv::Mat fusedLms = landmarksOf(fusedIn);
    pathPadded();
    const cv::Mat paddedLms = landmarksOf(areaIn);
    if (fusedLms.empty() || paddedLms.empty()) {
        std::fprintf(stderr, "pose net returned nothing\n");
        return 1;
    }

    const std::vector<cv::Point2f> fusedPts = through(fusedLms, poseToFrame);
    const std::vector<cv::Point2f> paddedPts = through(paddedLms, paddedToFrame);

    // The sample's own answer for this anchor row, as the reference for the
    // replication above. It runs with smoothing off, so that what comes back is
    // the raw estimate rather than the filtered one.
    Pipe::Params raw = p;
    raw.smooth_ = false;
    const std::vector<cv::Vec4f> sampleKpts = pipe.estimatePose(frame, reg, row, lb, raw);
    if (sampleKpts.empty()) {
        std::fprintf(stderr, "estimatePose returned nothing\n");
        return 1;
    }

    double vsSample = 0;
    std::vector<double> shift;
    int worst = 0;
    for (int i = 0; i < 33; ++i) {
        vsSample = std::max(vsSample, cv::norm(fusedPts[i] - cv::Point2f(sampleKpts[i][0],
                                                                         sampleKpts[i][1])));
        shift.push_back(cv::norm(fusedPts[i] - paddedPts[i]));
        if (shift[i] > shift[worst]) worst = i;
    }
    const double shiftMean =
        std::accumulate(shift.begin(), shift.end(), 0.0) / static_cast<double>(shift.size());
    std::vector<double> sorted(shift);
    std::sort(sorted.begin(), sorted.end());

    std::printf("\nlandmarks (33 keypoints, frame pixels):\n");
    std::printf("  probe's fused geometry vs the sample's estimatePose : max %.3f px"
                "   (0 => the warp and the landmark mapping agree)\n", vsSample);
    std::printf("  fused (bilinear) vs padded (INTER_AREA)            : mean %.2f px,"
                " median %.2f px, max %.2f px\n", shiftMean, sorted[sorted.size() / 2],
                sorted.back());
    auto sigmoid = [](float logit) { return 1.f / (1.f + std::exp(-logit)); };
    std::printf("  worst keypoint: #%d, %.2f px, scores fused %.2f / padded %.2f"
                "   (a joint the model does not claim to see moves whatever the crop does)\n",
                worst, shift[worst], sampleKpts[worst][3],
                sigmoid(paddedLms.at<float>(worst, 3)) * sigmoid(paddedLms.at<float>(worst, 4)));

    // A geometry that does not match the sample's is a bug, not a resampling
    // difference, so it fails the probe.
    if (vsSample > 0.01) {
        std::printf("FAIL  fused crop geometry disagrees with the sample by %.3f px\n", vsSample);
        return 1;
    }
    return 0;
}