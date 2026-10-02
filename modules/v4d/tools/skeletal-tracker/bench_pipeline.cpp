// Standalone verification + stage timer for the skeletal tracker pipeline.
//
// The pipeline is included straight out of samples/skeletal-tracker-pipeline.hpp
// -- the same code the sample compiles, no copy of it -- and it is driven with
// the demo's own shared state, so what is measured is what the panel would run.
//
// Reports, per frame: how many tracks exist, their ids, coasting state,
// detector score, trail length and keypoint score range, plus the detect/pose
// stage split. Finally it checks the failure modes this demo has had before:
// overlapping duplicate skeletons on one person, and tracks that never retire.
//
//   usage: bench_pipeline [video|frame-dir] [frames] [width] [height]
//
// A directory is read as a frame%05d.png sequence, which is how a failure seen
// only inside the demo gets replayed offline: the demo's own pixels differ from
// what a VideoCapture of the clip hands over, so a bug that reproduces in one
// and not the other can only be fixed against the bytes that showed it.

#include "tool_prelude.hpp"

#define private public
#include "skeletal-tracker-pipeline.hpp"
#undef private

namespace {

using Pipe = cv::samples::MediaPipePosePipeline;

// Fraction of the smaller skeleton box that lies inside the larger one. Two
// tracks drawn on the same person score near 1.
float kpBoxIoU(const std::vector<cv::Vec4f>& a, const std::vector<cv::Vec4f>& b) {
    auto box = [](const std::vector<cv::Vec4f>& k) {
        cv::Rect2f r(1e9f, 1e9f, 0, 0);
        for (const cv::Vec4f& p : k) {
            if (p[3] < cv::samples::kKeypointConf) continue;
            r |= cv::Rect2f(p[0] - 1, p[1] - 1, 2, 2);
        }
        return r;
    };
    const cv::Rect2f ra = box(a), rb = box(b);
    if (ra.empty() || rb.empty()) return 0.f;
    const float inter = (ra & rb).area();
    const float uni = ra.area() + rb.area() - inter;
    return uni > 0.f ? inter / uni : 0.f;
}

void dumpPerson(const Pipe::Person& p) {
    float vmin = 2.f, vmax = -1.f;
    for (const cv::Vec4f& k : p.keypoints) {
        vmin = std::min(vmin, k[3]);
        vmax = std::max(vmax, k[3]);
    }
    // Mid-hip: mean of BlazePose hips 23 and 24, the point the tracker keys on.
    const cv::Point2f hip =
        (cv::Point2f(p.keypoints[23][0], p.keypoints[23][1]) +
         cv::Point2f(p.keypoints[24][0], p.keypoints[24][1])) * 0.5f;
    std::printf("      id=%-3d missed=%d score=%.2f trail=%zu kpts=%zu vis[%.2f..%.2f]"
                " hip=(%.0f,%.0f) detRoi=%d,%d %dx%d\n",
                p.id, p.missed, p.score, p.trail.size(), p.keypoints.size(), vmin, vmax, hip.x,
                hip.y, p.box.x, p.box.y, p.box.width, p.box.height);
}

}  // namespace

int main(int argc, char** argv) {
    cv::v4d::add_asset_search_paths();

    const std::string video =
        argc > 1 ? argv[1] : cv::samples::findFile("videos/dance.mp4");
    const int nFrames = argc > 2 ? std::atoi(argv[2]) : 60;
    const int dispW = argc > 3 ? std::atoi(argv[3]) : 1280;
    const int dispH = argc > 4 ? std::atoi(argv[4]) : 720;

    std::cout << "OpenCL available: " << cv::ocl::haveOpenCL() << "\n";

    const std::string detModel =
        cv::samples::findFile("models/pose/person_detection_mediapipe_2023mar.onnx");
    const std::string poseModel =
        cv::samples::findFile("models/pose/pose_estimation_mediapipe_2023mar.onnx");
    if (detModel.empty() || poseModel.empty()) {
        std::cerr << "pose models not found\n";
        return 1;
    }
    Pipe pipe(detModel, poseModel);

    // A frame directory is replayed instead of decoded: see the header note.
    const bool isSeq = !video.empty() && video.back() == '/';
    std::string dir = isSeq ? video : std::string();
    cv::VideoCapture cap;
    if (!isSeq) {
        cap.open(video);
        if (!cap.isOpened()) {
            std::cerr << "cannot open " << video << "\n";
            return 1;
        }
    } else if (!cv::utils::fs::exists(dir) || !cv::utils::fs::isDirectory(dir)) {
        std::cerr << "no such frame directory " << dir << "\n";
        return 1;
    }

    // The demo's tunables, from the one place they are defined. The filters run
    // in content time, so give them the clip's own rate rather than the default.
    cv::samples::SharedPoseState state;
    Pipe::Params p = state.params();
    const double fps = isSeq ? 59.94005994 : cap.get(cv::CAP_PROP_FPS);
    if (fps > 0.0) p.frameDt_ = static_cast<float>(1.0 / fps);
    std::printf("%s  %ux%u  detConf %.2f  poseConf %.2f  maxPersons %d  frameDt %.1f ms\n",
                isSeq ? (dir + " (png sequence)").c_str() : video.c_str(), dispW, dispH,
                p.detConf_, p.poseConf_, p.maxPersons_, p.frameDt_ * 1000.f);

    cv::TickMeter tAll;
    int shown = 0, maxPersons = 0, duplicateFrames = 0, coastFrames = 0, n = 0;
    int minTrail = 1 << 30, maxTrail = 0;
    std::map<int, int> idFirstSeen;
    float vminAll = 2.f, vmaxAll = -1.f;
    double sumDet = 0, sumPose = 0, sumTotal = 0;

    for (int i = 0; i < nFrames; i++) {
        cv::Mat frame;
        if (isSeq) {
            char name[4096];
            std::snprintf(name, sizeof(name), "%s/frame%05d.png", dir.c_str(), i);
            frame = cv::imread(name, cv::IMREAD_COLOR);
            if (frame.empty()) break;
        } else if (!(cap >> frame) || frame.empty()) {
            break;
        }
        cv::Mat f;
        if (frame.cols != dispW || frame.rows != dispH)
            cv::resize(frame, f, cv::Size(dispW, dispH), 0, 0, cv::INTER_LINEAR);
        else
            f = frame;

        Pipe::Stats st;
        tAll.start();
        const auto persons = pipe.run(f, p, &st);
        tAll.stop();
        n++;
        sumDet += st.detectMs;
        sumPose += st.poseMs;
        sumTotal += st.totalMs;
        shown += static_cast<int>(persons.size());
        maxPersons = std::max(maxPersons, static_cast<int>(persons.size()));

        bool dup = false;
        for (size_t a = 0; a < persons.size() && !dup; a++)
            for (size_t b = a + 1; b < persons.size(); b++)
                if (kpBoxIoU(persons[a].keypoints, persons[b].keypoints) > 0.4f) {
                    dup = true;
                    break;
                }
        duplicateFrames += dup;

        for (const auto& pp : persons) {
            coastFrames += pp.missed > 0;
            minTrail = std::min(minTrail, static_cast<int>(pp.trail.size()));
            maxTrail = std::max(maxTrail, static_cast<int>(pp.trail.size()));
            idFirstSeen.emplace(pp.id, i);
            for (const cv::Vec4f& k : pp.keypoints) {
                vminAll = std::min(vminAll, k[3]);
                vmaxAll = std::max(vmaxAll, k[3]);
            }
        }

        if (i % 25 == 0) {
            std::printf("  frame %4d  persons=%zu  det %.1f pose %.1f total %.1f ms\n", i,
                        persons.size(), st.detectMs, st.poseMs, st.totalMs);
            for (const auto& pp : persons) dumpPerson(pp);
        }
    }

    std::printf("\n=== summary over %d frames ===\n", n);
    std::printf("persons: mean %.2f  max %d\n", n ? static_cast<double>(shown) / n : 0.0, maxPersons);
    std::printf("frames with overlapping duplicate skeletons: %d\n", duplicateFrames);
    std::printf("frames containing a coasting track:          %d\n", coastFrames);
    std::printf("distinct track ids created:                  %zu\n", idFirstSeen.size());
    std::printf("trail length: min %d  max %d (cap %zu)\n", minTrail == (1 << 30) ? 0 : minTrail,
                maxTrail, Pipe::kTrailLength);
    std::printf("keypoint score (vis*presence): [%.3f .. %.3f]\n", vminAll, vmaxAll);
    std::printf("mean stage: detect %.2f ms  pose %.2f ms  total %.2f ms  => %.1f fps\n",
                sumDet / std::max(1, n), sumPose / std::max(1, n), sumTotal / std::max(1, n),
                1000.0 / std::max(0.01, tAll.getAvgTimeMilli()));
    return 0;
}