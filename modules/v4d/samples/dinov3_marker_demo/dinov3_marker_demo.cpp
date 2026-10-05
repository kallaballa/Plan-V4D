#include <opencv2/v4d/v4d.hpp>
#include "dinov3_embedder.hpp"
#include "marker_database.hpp"
#include "geometric_verifier.hpp"
#include <opencv2/imgproc.hpp>
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <chrono>

using namespace cv;
using namespace cv::v4d;

struct SharedParams {
    bool enabled = true;
    bool hud = true;
    bool drawRoi = true;
    bool geometricVerify = true;
    float matchThreshold = 0.62f;
    float roiScale = 0.80f;
    int dnnInputSize = 224;
    int processEveryN = 1;
    char markerName[64] = "marker";
    bool requestRegister = false;
    bool requestUpdateNearest = false;
    bool requestClear = false;
    bool requestSave = false;
    bool requestLoad = false;
    bool autoSave = false;
};

struct RecognitionResult {
    bool valid = false;
    int markerId = -1;
    std::string name;
    float score = 0.0f;
    float inlierRatio = 0.0f;
    std::vector<cv::Point2f> corners;
    // True when the outline is a filtered pose carried over from an earlier
    // frame. Drawing it is right -- a blinking outline is worse than a lagging
    // one -- but it is not a fresh measurement and the HUD says so.
    bool stale = false;
    Localiser which = Localiser::None;
    int inliers = 0;
    int correspondences = 0;
};

struct SharedStatus {
    bool modelLoaded = false;
    std::string modelError;
    int markerCount = 0;
    std::string lastMessage;
    double preprocessMs = 0.0;
    double embedMs = 0.0;
    double searchMs = 0.0;
    double verifyMs = 0.0;
    RecognitionResult recognition;
};

static inline cv::Rect computeCenterCrop(const Size& size, float scale) {
    scale = std::clamp(scale, 0.1f, 1.0f);
    int w = cvRound(size.width * scale);
    int h = cvRound(size.height * scale);
    int x = (size.width - w) / 2;
    int y = (size.height - h) / 2;
    return (Rect(x, y, w, h) & Rect(0, 0, size.width, size.height));
}

class Dinov3MarkerPlan : public V4DPlan {
private:
    cv::UMat frame_;
    cv::UMat roi_;

    cv::Ptr<Dinov3Embedder> embedder_;
    cv::Ptr<MarkerDatabase> db_;
    cv::Ptr<GeometricVerifier> verifier_;

    std::string modelPath_;
    std::string dbPath_;

    Property<Size> size_ = P<Size>(V4D::Keys::SIZE);
    Property<uint64_t> frameNo_ = P<uint64_t>(GlobalState::Keys::FRAME_CNT);
    Property<double> fps_ = P<double>(GlobalState::Keys::FPS);

public:
    static SharedParams params_;
    static SharedStatus status_;

    Dinov3MarkerPlan(
        cv::Ptr<MarkerDatabase> db,
        std::string modelPath,
        std::string dbPath
    ) {
        _shared(params_);
        _shared(status_);

        db_ = db;
        modelPath_ = std::move(modelPath);
        dbPath_ = std::move(dbPath);

        embedder_ = makePtr<Dinov3Embedder>();
        verifier_ = makePtr<GeometricVerifier>();
    }

    void setup() override {
        plain(
            &Dinov3MarkerPlan::initModel,
            RW(embedder_),
            V(modelPath_),
            CS(params_),
            RWS(status_)
        );

        plain(
            &Dinov3MarkerPlan::loadDatabase,
            RW(db_),
            V(dbPath_),
            RWS(status_)
        );
    }

    void gui() override {
        imgui(
            &Dinov3MarkerPlan::drawGui,
            RWS(params_),
            CS(status_)
        );
    }

    void infer() override {
        auto paramsSnapshot = CS(params_);

        fb(
            [](const UMat& framebuffer, UMat& dst) {
                framebuffer.copyTo(dst);
            },
            RW(frame_)
        );

        branch(F(&Dinov3MarkerPlan::takeRegisterRequest, RWS(params_)))
            ->plain(
                &Dinov3MarkerPlan::registerMarker,
                R(frame_),
                RW(embedder_),
                RW(db_),
                RWS(status_),
                CS(params_)
            )
        ->endBranch();

        branch(F(&Dinov3MarkerPlan::takeUpdateNearestRequest, RWS(params_)))
            ->plain(
                &Dinov3MarkerPlan::updateNearestMarker,
                R(frame_),
                RW(embedder_),
                RW(db_),
                RWS(status_),
                CS(params_)
            )
        ->endBranch();

        branch(F(&Dinov3MarkerPlan::takeClearRequest, RWS(params_)))
            ->plain(
                &Dinov3MarkerPlan::clearDatabase,
                RW(db_),
                RW(verifier_),
                RWS(status_)
            )
        ->endBranch();

        branch(
            BranchType::SINGLE,
            F(&Dinov3MarkerPlan::takeSaveRequest, RWS(params_))
        )
            ->plain(
                &Dinov3MarkerPlan::saveDatabase,
                RW(db_),
                V(dbPath_),
                RWS(status_)
            )
        ->endBranch();

        branch(
            BranchType::SINGLE,
            F(&Dinov3MarkerPlan::takeLoadRequest, RWS(params_))
        )
            ->plain(
                &Dinov3MarkerPlan::loadDatabase,
                RW(db_),
                V(dbPath_),
                RWS(status_)
            )
        ->endBranch();

        branch(
            F(
                &Dinov3MarkerPlan::shouldRunRecognition,
                paramsSnapshot,
                frameNo_
            )
        )
            ->plain(
                &Dinov3MarkerPlan::recognizeFrame,
                R(frame_),
                RW(roi_),
                RW(embedder_),
                RW(db_),
                RW(verifier_),
                RWS(status_),
                paramsSnapshot
            )
        ->endBranch();

        branch(
            F(
                [](const SharedParams& p) {
                    return p.hud;
                },
                CS(params_)
            )
        )
            ->nvg(
                &Dinov3MarkerPlan::drawHud,
                size_,
                frameNo_,
                fps_,
                CS(status_),
                CS(params_)
            )
        ->endBranch();
    }

private:
    static bool takeRegisterRequest(SharedParams& p) {
        bool v = p.requestRegister;
        p.requestRegister = false;
        return v;
    }

    static bool takeUpdateNearestRequest(SharedParams& p) {
        bool v = p.requestUpdateNearest;
        p.requestUpdateNearest = false;
        return v;
    }

    static bool takeClearRequest(SharedParams& p) {
        bool v = p.requestClear;
        p.requestClear = false;
        return v;
    }

    static bool takeSaveRequest(SharedParams& p) {
        bool v = p.requestSave;
        p.requestSave = false;
        return v;
    }

    static bool takeLoadRequest(SharedParams& p) {
        bool v = p.requestLoad;
        p.requestLoad = false;
        return v;
    }

    static bool shouldRunRecognition(
        const SharedParams& p,
        uint64_t frameNo
    ) {
        if (!p.enabled) return false;
        if (p.processEveryN <= 1) return true;
        return (frameNo % uint64_t(p.processEveryN)) == 0;
    }

    static void initModel(
        cv::Ptr<Dinov3Embedder>& embedder,
        const std::string& modelPath,
        const SharedParams& params,
        SharedStatus& status
    ) {
        (void)params;
        std::string error;
        Dinov3Embedder::Config cfg;
        cfg.inputSize = params.dnnInputSize;
        embedder->setConfig(cfg);
        if (!embedder->load(modelPath, error)) {
            status.modelLoaded = false;
            status.modelError = error;
            status.lastMessage = "Model load failed";
        } else {
            status.modelLoaded = true;
            status.modelError.clear();
            status.lastMessage = "Model loaded";
        }
    }

    static void loadDatabase(
        const cv::Ptr<MarkerDatabase>& db,
        const std::string& path,
        SharedStatus& status
    ) {
        std::string error;
        if (db->load(path, error)) {
            status.markerCount = static_cast<int>(db->size());
            status.lastMessage = "Loaded database from " + path;
        } else {
            status.markerCount = static_cast<int>(db->size());
            status.lastMessage = "Load failed: " + error;
        }
    }

    static void saveDatabase(
        const cv::Ptr<MarkerDatabase>& db,
        const std::string& path,
        SharedStatus& status
    ) {
        std::string error;
        if (db->save(path, error)) {
            status.lastMessage = "Saved database to " + path;
        } else {
            status.lastMessage = "Save failed: " + error;
        }
    }

    static void registerMarker(
        const UMat& frame,
        cv::Ptr<Dinov3Embedder>& embedder,
        const cv::Ptr<MarkerDatabase>& db,
        SharedStatus& status,
        const SharedParams& params
    ) {
        status.lastMessage = "Register marker...";
        if (!embedder || !embedder->isLoaded()) {
            status.lastMessage = "Registration failed: model not loaded.";
            return;
        }
        if (frame.empty()) {
            status.lastMessage = "Registration failed: empty frame.";
            return;
        }
        try {
            Rect roi = computeCenterCrop(frame.size(), params.roiScale);
            UMat roiUmat = frame(roi).clone();
            Mat thumbnail;
            roiUmat.copyTo(thumbnail);
            if (thumbnail.channels() == 4) {
                cvtColor(thumbnail, thumbnail, COLOR_BGRA2BGR);
            }
            std::vector<float> descriptor;
            PatchGrid grid;
            std::string error;
            if (!embedder->embed(roiUmat, descriptor, grid, error)) {
                status.lastMessage = "Registration failed: " + error;
                return;
            }
            std::string name = params.markerName;
            if (name.empty()) name = "marker";
            int id = db->addMarker(name, thumbnail, descriptor, grid);
            status.markerCount = static_cast<int>(db->size());
            status.lastMessage = "Registered marker '" + name + "' id=" + std::to_string(id);
        } catch (const std::exception& e) {
            status.lastMessage = std::string("Registration failed: ") + e.what();
        }
    }

    static void updateNearestMarker(
        const UMat& frame,
        cv::Ptr<Dinov3Embedder>& embedder,
        const cv::Ptr<MarkerDatabase>& db,
        SharedStatus& status,
        const SharedParams& params
    ) {
        status.lastMessage = "Update nearest...";
        if (!embedder || !embedder->isLoaded()) {
            status.lastMessage = "Update failed: model not loaded.";
            return;
        }
        if (frame.empty()) {
            status.lastMessage = "Update failed: empty frame.";
            return;
        }
        try {
            Rect roi = computeCenterCrop(frame.size(), params.roiScale);
            UMat roiUmat = frame(roi).clone();
            Mat thumbnail;
            roiUmat.copyTo(thumbnail);
            if (thumbnail.channels() == 4) {
                cvtColor(thumbnail, thumbnail, COLOR_BGRA2BGR);
            }
            std::vector<float> descriptor;
            PatchGrid grid;
            std::string error;
            if (!embedder->embed(roiUmat, descriptor, grid, error)) {
                status.lastMessage = "Update failed: " + error;
                return;
            }
            float updateMin = std::max(0.25f, params.matchThreshold * 0.75f);
            std::string updatedName;
            if (db->updateNearest(descriptor, thumbnail, grid, updateMin, updatedName)) {
                status.markerCount = static_cast<int>(db->size());
                status.lastMessage = "Updated marker '" + updatedName + "'";
            } else {
                status.lastMessage = "Update failed: no good match";
            }
        } catch (const std::exception& e) {
            status.lastMessage = std::string("Update failed: ") + e.what();
        }
    }

    static void clearDatabase(
        const cv::Ptr<MarkerDatabase>& db,
        const cv::Ptr<GeometricVerifier>& verifier,
        SharedStatus& status
    ) {
        db->clear();
        // A pose remembered for a marker that no longer exists is not a pose.
        verifier->reset();
        status.markerCount = 0;
        status.recognition.valid = false;
        status.lastMessage = "Cleared database";
    }

    static void recognizeFrame(
        const UMat& frame,
        UMat& roi,
        cv::Ptr<Dinov3Embedder>& embedder,
        const cv::Ptr<MarkerDatabase>& db,
        cv::Ptr<GeometricVerifier>& verifier,
        SharedStatus& status,
        const SharedParams& params
    ) {
        (void)roi;
        status.recognition.valid = false;
        if (!embedder || !embedder->isLoaded()) return;
        if (!params.enabled) return;
        if (frame.empty()) return;
        int64_t t0 = cv::getTickCount();
        try {
            Rect r = computeCenterCrop(frame.size(), params.roiScale);
            UMat roiUmat = frame(r).clone();
            roi = roiUmat.clone();
            int64_t t1 = cv::getTickCount();
            std::vector<float> descriptor;
            PatchGrid grid;
            std::string error;
            if (!embedder->embed(roiUmat, descriptor, grid, error)) {
                status.recognition.valid = false;
                return;
            }
            int64_t t2 = cv::getTickCount();
            int bestId = -1;
            float bestScore = 0.0f;
            std::string bestName;
            if (!db->search(descriptor, params.matchThreshold, bestId, bestScore, bestName)) {
                status.recognition.valid = false;
                int64_t tEnd = cv::getTickCount();
                status.preprocessMs = 1000.0 * (t1 - t0) / cv::getTickFrequency();
                status.embedMs = 1000.0 * (t2 - t1) / cv::getTickFrequency();
                status.searchMs = 1000.0 * (tEnd - t2) / cv::getTickFrequency();
                status.verifyMs = 0.0;
                return;
            }
            int64_t t3 = cv::getTickCount();
            RecognitionResult res;
            res.valid = true;
            res.markerId = bestId;
            res.name = bestName;
            res.score = bestScore;
            res.inlierRatio = 0.0f;
            if (params.geometricVerify) {
                MarkerRecord mr;
                if (db->getMarker(bestId, mr)) {
                    // The query's patch grid and the ROI it was computed over go
                    // in together: the grid's coordinates are only meaningful
                    // relative to the region they came from.
                    const Verification v = verifier->verify(mr, frame, r, grid, frame.size());
                    if (v.ok) {
                        res.corners = v.corners;
                        res.inlierRatio = v.inlierRatio;
                        res.stale = v.stale;
                        res.which = v.which;
                        res.inliers = v.inliers;
                        res.correspondences = v.correspondences;
                    } else {
                        status.lastMessage = "Localisation declined: " + v.reject;
                    }
                }
            }
            int64_t t4 = cv::getTickCount();
            status.recognition = res;
            status.preprocessMs = 1000.0 * (t1 - t0) / cv::getTickFrequency();
            status.embedMs = 1000.0 * (t2 - t1) / cv::getTickFrequency();
            status.searchMs = 1000.0 * (t3 - t2) / cv::getTickFrequency();
            status.verifyMs = 1000.0 * (t4 - t3) / cv::getTickFrequency();
        } catch (const std::exception&) {
            status.recognition.valid = false;
        }
    }

    static void drawGui(
        SharedParams& params,
        const SharedStatus& status
    ) {
        using namespace ImGui;
        Begin("DINOv3 Marker Demo");
        Text("Model: %s", status.modelLoaded ? "loaded" : "not loaded");
        if (!status.modelError.empty()) {
            TextWrapped("Model error: %s", status.modelError.c_str());
        }
        Text("Markers: %d", status.markerCount);
        TextWrapped("Last action: %s", status.lastMessage.c_str());
        Separator();
        Checkbox("Enable recognition", &params.enabled);
        Checkbox("HUD", &params.hud);
        Checkbox("Draw ROI", &params.drawRoi);
        Checkbox("Geometric verification", &params.geometricVerify);
        SliderFloat("Match threshold", &params.matchThreshold, 0.0f, 1.0f);
        SliderFloat("ROI scale", &params.roiScale, 0.2f, 1.0f);
        SliderInt("DNN input size", &params.dnnInputSize, 128, 518);
        SliderInt("Process every N", &params.processEveryN, 1, 30);
        Separator();
        InputText("Marker name", params.markerName, sizeof(params.markerName));
        if (Button("Register Marker")) {
            params.requestRegister = true;
        }
        if (Button("Update Nearest")) {
            params.requestUpdateNearest = true;
        }
        if (Button("Clear All")) {
            params.requestClear = true;
        }
        if (Button("Save Database")) {
            params.requestSave = true;
        }
        if (Button("Load Database")) {
            params.requestLoad = true;
        }
        Separator();
        if (status.recognition.valid) {
            Text("Recognized: %s", status.recognition.name.c_str());
            Text("Score: %.3f", status.recognition.score);
            if (params.geometricVerify) {
                const RecognitionResult& rec = status.recognition;
                if (rec.corners.empty()) {
                    // Recognition and localisation are separate verdicts, and the
                    // distinction is worth surfacing: the marker is there, we just
                    // cannot say where.
                    Text("Outline: none");
                } else {
                    const char* path = rec.which == Localiser::Dense
                                           ? "DINOv3 patches"
                                           : "ORB fallback";
                    Text("Outline: %s%s", path, rec.stale ? " (held)" : "");
                    if (!rec.stale) {
                        Text("Inliers: %d/%d (%.2f)", rec.inliers, rec.correspondences,
                             rec.inlierRatio);
                    }
                }
            }
        } else {
            Text("Recognized: none");
        }
        Text("Embed: %.1f ms", status.embedMs);
        Text("Search: %.1f ms", status.searchMs);
        Text("Verify: %.1f ms", status.verifyMs);
        End();
    }

    static void drawHud(
        const Size& size,
        uint64_t frameNo,
        double fps,
        const SharedStatus& status,
        const SharedParams& params
    ) {
        using namespace cv::v4d::nvg;
        fontSize(20.0f);
        fontFace("sans-bold");
        fillColor(Scalar(255, 255, 255, 220));
        textAlign(NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        char buf[256];
        snprintf(buf, sizeof(buf), "DINOv3 Marker Demo");
        text(16.0f, 12.0f, buf, buf + strlen(buf));
        snprintf(buf, sizeof(buf), "FPS: %.1f | Frame: %llu | Markers: %d",
                 fps, (unsigned long long)frameNo, status.markerCount);
        text(16.0f, 36.0f, buf, buf + strlen(buf));
        if (status.recognition.valid) {
            snprintf(buf, sizeof(buf), "Recognized: %s (%.3f)", status.recognition.name.c_str(), status.recognition.score);
            fillColor(Scalar(0, 255, 0, 230));
        } else {
            snprintf(buf, sizeof(buf), "Recognized: none");
            fillColor(Scalar(255, 255, 255, 220));
        }
        text(16.0f, 60.0f, buf, buf + strlen(buf));
        if (params.drawRoi) {
            Rect roi = computeCenterCrop(size, params.roiScale);
            beginPath();
            rect(float(roi.x), float(roi.y), float(roi.width), float(roi.height));
            strokeColor(Scalar(0, 255, 255, 200));
            strokeWidth(2.0f);
            stroke();
        }
        if (status.recognition.valid && !status.recognition.corners.empty()) {
            const auto& c = status.recognition.corners;
            // A held outline is drawn in a lighter tone. It is still the best
            // estimate of where the marker is, but dimming it keeps "carried
            // over from three frames ago" visually distinct from "measured just
            // now" -- otherwise a frame where localisation failed reads as
            // confidence it does not deserve.
            strokeColor(status.recognition.stale ? Scalar(0, 255, 0, 120)
                                                : Scalar(0, 255, 0, 230));
            strokeWidth(3.0f);
            beginPath();
            moveTo(c[0].x, c[0].y);
            for (size_t i = 1; i < c.size(); ++i) lineTo(c[i].x, c[i].y);
            closePath();
            stroke();
        }
    }
};

SharedParams Dinov3MarkerPlan::params_;
SharedStatus Dinov3MarkerPlan::status_;

static std::string getArgValue(int argc, char** argv, const std::string& prefix) {
    std::string p = prefix;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind(p, 0) == 0) {
            size_t eq = a.find('=');
            if (eq != std::string::npos) {
                return a.substr(eq + 1);
            }
        }
    }
    return std::string();
}

int main(int argc, char** argv) {
    std::string inputVideo;
    std::string outputVideo;
    std::string modelPath;
    std::string dbPath = "dinov3_markers.yml.gz";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--model=", 0) == 0) {
            modelPath = getArgValue(argc, argv, "--model=");
            continue;
        }
        if (a.rfind("--db=", 0) == 0) {
            dbPath = getArgValue(argc, argv, "--db=");
            continue;
        }
        if (a.rfind("--input-size=", 0) == 0) {
            int sz = std::stoi(getArgValue(argc, argv, "--input-size="));
            if (sz >= 128) Dinov3MarkerPlan::params_.dnnInputSize = sz;
            continue;
        }
        if (a.rfind("--threshold=", 0) == 0) {
            float th = std::stof(getArgValue(argc, argv, "--threshold="));
            Dinov3MarkerPlan::params_.matchThreshold = std::clamp(th, 0.0f, 1.0f);
            continue;
        }
        if (inputVideo.empty() && a.find("--") != 0) {
            inputVideo = a;
            continue;
        }
        if (outputVideo.empty() && a.find("--") != 0 && inputVideo != a) {
            outputVideo = a;
            continue;
        }
    }

    cv::Rect viewport(0, 0, 1280, 720);
    cv::Ptr<V4D> runtime = V4D::init(
        viewport,
        "DINOv3 Marker Demo",
        AllocateFlags::NANOVG | AllocateFlags::IMGUI,
        ConfigFlags::DISPLAY_MODE | ConfigFlags::RESIZEABLE
    );

    auto db = makePtr<MarkerDatabase>();
    if (!inputVideo.empty()) {
        auto src = Source::make(runtime, inputVideo);
        if (src) {
            runtime->setSource(src);
            if (!outputVideo.empty()) {
                auto sink = Sink::make(runtime, outputVideo, src->fps(), viewport.size());
                if (sink) runtime->setSink(sink);
            }
        }
    }

    V4DPlan::run<Dinov3MarkerPlan>(0, db, modelPath, dbPath);
    return 0;
}
