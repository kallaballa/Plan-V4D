// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// DINOv3 Marker Demo — register a physical object once, then find it in a live
// camera feed.
//
// Two signals cooperate, and the split matters:
//
//   DINOv3 embedding   decides *whether* a registered marker is on screen. It is
//                      one 768-d descriptor over the region of interest, which
//                      survives rotation, scale and lighting, and it costs a
//                      single forward pass.
//   local features     decides *where* it is, and whether the embedding's answer
//                      deserves to be trusted. ORB features plus a homography
//                      localise the marker precisely, which is what the green
//                      outline is drawn from.
//
// The embedding runs on every Nth frame and verification only on frames the
// embedding liked, so the expensive work is spent where it can pay off. A
// verification failure downgrades a match to "unconfirmed" rather than
// discarding it: on the supplied clip that is the difference between recognising
// a marker through most of a handheld take and recognising it in a handful of
// frames.
//
// The heavy lifting lives in dinov3-marker-pipeline.*, which knows nothing about
// V4D so the head-less self-test can drive the identical code path.

#include "dinov3-marker-pipeline.hpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/v4d/v4d.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace cv;
using namespace cv::v4d;
using namespace cv::samples;
using namespace cv::samples::dinov3;

// ---------------------------------------------------------------------------
// Command line, parsed once in main() and read by the plan's setup node.
// ---------------------------------------------------------------------------
struct CliOptions {
  std::string inputVideo;
  std::string outputVideo;
  std::string modelPath;
  std::string markerImage;
  std::string markerName = "harddisk";
  std::string dbPath = "dinov3_markers.yml.gz";
  PoolMode pool = PoolMode::Mean;
  float threshold = -1.0f; // < 0: keep the measured default
  float roiScale = -1.0f;
  int processEveryN = 1;
  bool verify = true;
  Rect viewport = Rect(0, 0, 1920, 1080);
  double fallbackFps = 30.0;
};

static CliOptions g_cli;

namespace {

const char *poolName(PoolMode mode) {
  switch (mode) {
  case PoolMode::Cls:
    return "cls";
  case PoolMode::ClsMean:
    return "cls+mean";
  case PoolMode::Mean:
  default:
    return "mean";
  }
}

// V4D's source stretches the video to the viewport (cv::resize straight to
// vp.size(), no aspect preservation) and draws it at the viewport origin, so
// frame pixels map to screen by a plain scale with no offset. Getting this
// wrong is what makes an outline appear somewhere other than on the marker.
struct FrameToScreen {
  float sx = 1.0f, sy = 1.0f;

  FrameToScreen() = default;

  FrameToScreen(const Size &frame, const Size &screen) {
    if (frame.width > 0)
      sx = (float)screen.width / (float)frame.width;
    if (frame.height > 0)
      sy = (float)screen.height / (float)frame.height;
  }

  float x(float vx) const { return vx * sx; }
  float y(float vy) const { return vy * sy; }
  float w(float vw) const { return vw * sx; }
  float h(float vh) const { return vh * sy; }
};

bool parseOptions(int argc, char **argv, CliOptions &opts, std::string &error) {
  std::vector<std::string> positional;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&](const char *flag) -> const char * {
      const std::string prefix = std::string(flag) + "=";
      if (arg.rfind(prefix, 0) == 0)
        return arg.c_str() + prefix.size();
      return nullptr;
    };

    if (const char *v = value("--model"))
      opts.modelPath = v;
    else if (const char *v = value("--marker-image"))
      opts.markerImage = v;
    else if (const char *v = value("--marker-name"))
      opts.markerName = v;
    else if (const char *v = value("--db"))
      opts.dbPath = v;
    else if (const char *v = value("--output"))
      opts.outputVideo = v;
    else if (const char *v = value("--pool")) {
      const std::string p = v;
      if (p == "cls")
        opts.pool = PoolMode::Cls;
      else if (p == "mean")
        opts.pool = PoolMode::Mean;
      else if (p == "cls+mean" || p == "clsmean")
        opts.pool = PoolMode::ClsMean;
      else {
        error = "unknown --pool '" + p + "' (use cls, mean or cls+mean)";
        return false;
      }
    } else if (const char *v = value("--threshold")) {
      opts.threshold = (float)std::atof(v);
      if (!(opts.threshold > 0.0f && opts.threshold <= 1.0f)) {
        error = "--threshold must be in (0, 1]";
        return false;
      }
    } else if (const char *v = value("--roi")) {
      opts.roiScale = (float)std::atof(v);
      if (!(opts.roiScale > 0.0f && opts.roiScale <= 1.0f)) {
        error = "--roi must be in (0, 1]";
        return false;
      }
    } else if (const char *v = value("--every")) {
      opts.processEveryN = std::atoi(v);
      if (opts.processEveryN < 1) {
        error = "--every must be at least 1";
        return false;
      }
    } else if (arg == "--no-verify") {
      opts.verify = false;
    } else if (arg == "--help" || arg == "-h") {
      error = "help";
      return false;
    } else if (!arg.empty() && arg[0] == '-') {
      error = "unknown option '" + arg + "'";
      return false;
    } else {
      positional.push_back(arg);
    }
  }

  if (!positional.empty())
    opts.inputVideo = positional[0];
  return true;
}

void printUsage() {
  std::cout << "DINOv3 Marker Demo\n\n"
            << "Usage: dinov3-marker-demo [input-video] [options]\n\n"
            << "Options:\n"
            << "  --model=PATH         dinov3_vitb16.onnx (keep .onnx.data beside it)\n"
            << "  --marker-image=PATH  register this image as the marker at startup\n"
            << "  --marker-name=NAME   name for --marker-image (default: harddisk)\n"
            << "  --db=PATH            marker database (default: dinov3_markers.yml.gz)\n"
            << "  --pool=MODE          cls | mean | cls+mean (default: mean)\n"
            << "  --threshold=F        cosine threshold, default 0.50 (measured)\n"
            << "  --roi=F              centre crop scale, default 0.90 (measured)\n"
            << "  --every=N            embed every Nth frame (default: 1)\n"
            << "  --no-verify          embedding only, skip local-feature confirmation\n"
            << "  --output=PATH        also write the annotated result to a video\n"
            << "  -h, --help           this text\n";
}

} // namespace

// State shared between the ImGui panel (which only writes request flags) and the
// single worker that owns the pipeline (which consumes them and publishes
// results). Nothing else crosses the thread boundary.
struct MarkerState {
  MarkerParams params;
  MarkerStatus status;
  std::vector<std::string> names;
  bool fullscreen = false;
  bool confirmClear = false;
  uint64_t processed = 0;
  uint64_t matches = 0;
};

// The pipeline is deliberately not per-worker state: it owns a DNN session and
// a descriptor index, and only ever runs on the one worker inside the SINGLE
// branch below.
static std::unique_ptr<MarkerPipeline> g_pipeline;

// V4DPlan::run() takes a count of *extra* workers, and a plain node in setup()
// is executed once by every worker, not once by the plan. Without this guard two
// workers both build a DINOv3 session and both reset g_pipeline, which is both
// wasteful and a data race. std::call_once is the whole fix.
static std::once_flag g_initOnce;

class Dinov3MarkerPlan : public V4DPlan {
private:
  static MarkerState shared;

  Property<Size> size_ = P<Size>(V4D::Keys::SIZE);

  // BGRA framebuffer -> BGR, the layout the embedder documents and the layout
  // everything else in OpenCV uses.
  cv::UMat frameBGR_;

  // --- setup ---------------------------------------------------------------

  static void initDemo(MarkerState &state) {
    std::call_once(g_initOnce, [&]() { initOnce(state); });
  }

  static void initOnce(MarkerState &state) {
    PipelineOptions options;
    options.embedder.modelPath = g_cli.modelPath;
    options.embedder.pool = g_cli.pool;
    options.databasePath = g_cli.dbPath;
    g_pipeline.reset(new MarkerPipeline(options));

    if (g_cli.threshold > 0.0f)
      state.params.threshold = g_cli.threshold;
    if (g_cli.roiScale > 0.0f)
      state.params.roiScale = g_cli.roiScale;
    state.params.processEveryN = g_cli.processEveryN;
    state.params.verify = g_cli.verify;

    // A model that will not load makes this a degraded demo, not a dead one:
    // report it and keep the window up, so the user reads the real
    // OpenCV/onnxruntime message instead of a process that vanished silently.
    if (g_pipeline->initModel(state.status))
      state.status.lastMessage = "model ready";
    else
      std::fprintf(stderr, "[dinov3-marker] model unavailable: %s\n",
                   state.status.modelError.c_str());

    // Restore any saved database. A missing or unreadable file is normal on a
    // first run, so its absence is not worth reporting.
    if (g_pipeline->loadDatabase(state.status))
      std::fprintf(stderr, "[dinov3-marker] %s\n",
                   state.status.lastMessage.c_str());

    // Register, or refresh, the marker photograph. This is the path the
    // supplied harddisk image takes.
    if (!g_cli.markerImage.empty()) {
      g_pipeline->upsertMarkerFromImage(g_cli.markerImage, g_cli.markerName,
                                        state.params, state.status);
      std::fprintf(stderr, "[dinov3-marker] %s\n",
                   state.status.lastMessage.c_str());
    }

    publish(state);
  }

  // Refreshes the derived fields the panel reads, so the GUI never has to reach
  // into the pipeline itself.
  static void publish(MarkerState &state) {
    state.status.markerCount = g_pipeline->database().size();
    state.names = g_pipeline->markerNames();
  }

  // --- worker nodes -------------------------------------------------------

  // Database-wide operations need no frame, so they are kept apart from the
  // nodes below rather than every node taking a frame it does not use.
  static void handleDatabaseRequests(MarkerState &state) {
    if (!g_pipeline)
      return;
    MarkerParams &p = state.params;
    if (p.requestLoad) {
      g_pipeline->loadDatabase(state.status);
      p.requestLoad = false;
    }
    if (p.requestSave) {
      g_pipeline->saveDatabase(state.status);
      p.requestSave = false;
    }
    if (p.requestClear) {
      g_pipeline->clearDatabase(state.status);
      p.requestClear = false;
    }
    publish(state);
  }

  static void handleFrameRequests(MarkerState &state,
                                  const cv::UMat &frameBGR) {
    if (!g_pipeline)
      return;
    MarkerParams &p = state.params;
    const Mat bgr = frameBGR.getMat(cv::ACCESS_READ);
    if (p.requestRegister) {
      g_pipeline->registerMarker(bgr, p, state.status);
      p.requestRegister = false;
    }
    if (p.requestUpdateNearest) {
      g_pipeline->updateNearest(bgr, p, state.status);
      p.requestUpdateNearest = false;
    }
    if (p.requestRemove) {
      g_pipeline->removeNearest(bgr, p, state.status);
      p.requestRemove = false;
    }
    publish(state);
  }

  // The per-frame work, on the one worker inside the SINGLE branch.
  static void runPipeline(MarkerState &state, const cv::UMat &frameBGR) {
    if (!g_pipeline) {
      MarkerPipeline::resetRecognition(state.status);
      return;
    }
    // An empty frame means no fresh evidence about anything. Keeping the last
    // recognition would let the HUD go on claiming a marker is on screen for as
    // long as the source stalls.
    if (frameBGR.empty()) {
      MarkerPipeline::resetRecognition(state.status);
      return;
    }

    handleFrameRequests(state, frameBGR);

    if (!state.params.enabled) {
      MarkerPipeline::resetRecognition(state.status);
      return;
    }

    const int every = std::max(1, state.params.processEveryN);
    if (state.processed++ % (uint64_t)every != 0)
      return; // throttled: leave the previous result on screen

    if (g_pipeline->recognize(frameBGR.getMat(cv::ACCESS_READ), state.params,
                              state.status) &&
        state.status.valid)
      ++state.matches;
  }

  // --- HUD ----------------------------------------------------------------

  static void drawOverlay(const cv::Size &screen, const MarkerState &state) {
    using namespace cv::v4d::nvg;

    const MarkerStatus &st = state.status;
    const float scale = std::max(1.0f, (float)screen.height / 840.0f);
    const float lineH = 21.0f * scale;
    const FrameToScreen map(st.frameSize, screen);

    fontFace("sans");
    fontSize(16.0f * scale);
    textAlign(NVG_ALIGN_LEFT | NVG_ALIGN_TOP);

    const cv::Scalar panel(18, 14, 10, 170);
    const cv::Scalar dim = convert_pix(Scalar(205, 205, 205, 205),
                                       COLOR_HLS2BGR);
    const cv::Scalar good = convert_pix(Scalar(90, 230, 120, 245),
                                        COLOR_HLS2BGR);
    const cv::Scalar maybe = convert_pix(Scalar(40, 210, 250, 240),
                                         COLOR_HLS2BGR);
    const cv::Scalar bad = convert_pix(Scalar(80, 90, 255, 240), COLOR_HLS2BGR);

    float y = 26.0f * scale;
    // A dark bar behind each line keeps the text readable over a bright frame as
    // well as over a dark one. Neither the lambda nor its parameter may be called
    // "line" or "text": nvg has a text() free function in scope here, and a
    // parameter of that name shadows it into a call on a std::string.
    auto hudLine = [&](const std::string &msg, const cv::Scalar &color) {
      fillColor(panel);
      beginPath();
      rect(12.0f * scale, y - 14.0f * scale, 470.0f * scale, lineH);
      fill();
      fillColor(color);
      // nvg's free text() takes an explicit end pointer; a null end means
      // NUL-terminated, which is what a std::string always is.
      text(22.0f * scale, y, msg.c_str(), nullptr);
      y += lineH;
    };

    if (!st.modelLoaded)
      hudLine("no model: " + st.modelError, bad);
    else
      hudLine("markers: " + std::to_string(st.markerCount), dim);

    if (st.valid) {
      char buf[192];
      std::snprintf(buf, sizeof(buf), "%s   score %.3f%s", st.name.c_str(),
                    st.score, st.verified ? "  VERIFIED" : "  (unconfirmed)");
      hudLine(buf, st.verified ? good : maybe);

      std::snprintf(buf, sizeof(buf), "%.0f ms  embed %.0f  verify %.0f",
                    st.totalMs, st.embedMs, st.verifyMs);
      hudLine(buf, dim);
    } else if (st.modelLoaded) {
      hudLine("no marker in view", dim);
    }

    if (!st.lastMessage.empty())
      hudLine(st.lastMessage, dim);

    // The region of interest. Without it the user cannot tell what is being
    // embedded, and registration depends on putting the marker inside it.
    if (state.params.drawRoi && !st.roi.empty() && st.frameSize.width > 0) {
      strokeColor(convert_pix(Scalar(0, 140, 255, 185), COLOR_HLS2BGR));
      strokeWidth(1.5f * scale);
      beginPath();
      rect(map.x((float)st.roi.x), map.y((float)st.roi.y), map.w((float)st.roi.width),
           map.h((float)st.roi.height));
      stroke();
    }

    // The verified outline. This is what verification actually buys, so it is
    // drawn in green -- and an unconfirmed match gets no outline at all rather
    // than a misleading one.
    if (state.params.hud && st.verified && st.corners.size() == 4 &&
        st.frameSize.width > 0) {
      strokeColor(good);
      strokeWidth(3.0f * scale);
      beginPath();
      moveTo(map.x(st.corners[0].x), map.y(st.corners[0].y));
      for (size_t i = 1; i < st.corners.size(); ++i)
        lineTo(map.x(st.corners[i].x), map.y(st.corners[i].y));
      closePath();
      stroke();
    }
  }

public:
  Dinov3MarkerPlan() { _shared(shared); }

  void setup() override { plain(initDemo, RW(shared)); }

  void gui() override {
    // The panel writes flags and nothing else. Every button below only marks a
    // request: acting on it here is exactly what would make the plan perform
    // side effects at record time instead of replaying them.
    imgui(
        [](MarkerState &s) {
          using namespace ImGui;
          Begin("DINOv3 Marker");

          Text("pool: %s", poolName(g_cli.pool));
          if (!s.status.modelLoaded)
            TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s",
                        s.status.modelError.c_str());
          else
            Text("%s", s.status.modelDesc.c_str());

          Separator();
          Checkbox("Enabled", &s.params.enabled);
          Checkbox("HUD", &s.params.hud);
          Checkbox("Show RoI", &s.params.drawRoi);
          Checkbox("Geometric verification", &s.params.verify);
          Checkbox("Auto-save after each change", &s.params.autoSave);

          Separator();
          SliderFloat("Threshold", &s.params.threshold, 0.20f, 0.95f);
          SliderFloat("RoI scale", &s.params.roiScale, 0.30f, 1.00f);
          SliderInt("Embed every N frames", &s.params.processEveryN, 1, 30);

          Separator();
          InputText("Marker name", s.params.name, sizeof(s.params.name));

          if (Button("Register RoI"))
            s.params.requestRegister = true;
          SameLine();
          if (Button("Update nearest"))
            s.params.requestUpdateNearest = true;
          SameLine();
          if (Button("Remove nearest"))
            s.params.requestRemove = true;

          Separator();
          if (Button("Save"))
            s.params.requestSave = true;
          SameLine();
          if (Button("Load"))
            s.params.requestLoad = true;
          SameLine();
          // Two-step, because deleting every marker is not recoverable and the
          // button sits right next to Save.
          if (Button("Clear all"))
            s.confirmClear = true;
          if (s.confirmClear) {
            SameLine();
            TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "sure?");
            if (Button(" yes"))
              s.params.requestClear = true;
            SameLine();
            if (Button("no"))
              s.confirmClear = false;
          }

          Separator();
          Text("Markers: %zu", s.status.markerCount);
          for (const std::string &name : s.names)
            Text("  %s", name.c_str());

          if (!s.status.lastMessage.empty()) {
            Separator();
            TextWrapped("%s", s.status.lastMessage.c_str());
          }
          Text("frames %llu   matches %llu", (unsigned long long)s.processed,
               (unsigned long long)s.matches);

          Separator();
          if (Button(s.fullscreen ? "Leave fullscreen" : "Fullscreen"))
            s.fullscreen = !s.fullscreen;
          End();
        },
        RWS(shared));
  }

  void infer() override {
    set(V4D::Keys::FULLSCREEN, CS(shared.fullscreen));

    // framebuffer (BGRA) -> BGR for the embedder and the feature detector.
    fb(
        [](const cv::UMat &fb, cv::UMat &bgr) {
          if (!fb.empty())
            cvtColor(fb, bgr, COLOR_BGRA2BGR);
        },
        RW(frameBGR_))
        // Everything touching the DNN session, the database and the feature
        // detector runs on a single worker. It is one order of 45 ms per frame,
        // and both the detector and the descriptor index are not safe to share.
        ->branch(BranchType::SINGLE, always_)
        ->plain(handleDatabaseRequests, RWS(shared))
        ->plain(runPipeline, RWS(shared), R(frameBGR_))
        ->endBranch()
        ->nvg(drawOverlay, size_, CS(shared));
  }
};

// MarkerState lives at namespace scope, so the out-of-class definition of the
// static member uses the unqualified name.
MarkerState Dinov3MarkerPlan::shared;

int main(int argc, char **argv) {
  cv::v4d::add_asset_search_paths();

  std::string error;
  if (!parseOptions(argc, argv, g_cli, error)) {
    if (error != "help")
      std::cerr << "dinov3-marker-demo: " << error << "\n\n";
    printUsage();
    return error == "help" ? 0 : 2;
  }

  if (g_cli.inputVideo.empty())
    g_cli.inputVideo = findFile("videos/marker_test.mp4");
  if (g_cli.modelPath.empty())
    g_cli.modelPath = findFile("models/dinov3_vitb16.onnx");

  // A path that was typed in has to exist. One that came from the asset search
  // paths is allowed to be empty, because the HUD reports that case properly.
  if (!std::ifstream(g_cli.inputVideo).good()) {
    std::cerr << "Cannot read input video: " << g_cli.inputVideo
              << "\nRun with --help for usage.\n";
    return 1;
  }
  if (g_cli.modelPath.empty()) {
    std::cerr << "Cannot find dinov3_vitb16.onnx; pass --model=PATH.\n";
    return 1;
  }

  // Size the window to the footage. The supplied clip is 480x854, and opening
  // that in a 1080p landscape viewport would squeeze it into a sliver.
  {
    VideoCapture probe(g_cli.inputVideo);
    if (probe.isOpened()) {
      const int w = (int)probe.get(cv::CAP_PROP_FRAME_WIDTH);
      const int h = (int)probe.get(cv::CAP_PROP_FRAME_HEIGHT);
      const double fps = probe.get(cv::CAP_PROP_FPS);
      if (w > 0 && h > 0)
        g_cli.viewport = Rect(0, 0, w, h);
      if (fps > 1.0 && fps < 1000.0)
        g_cli.fallbackFps = fps;
    }
  }

  cv::Ptr<V4D> runtime = V4D::init(g_cli.viewport, "DINOv3 Marker Demo",
                                   AllocateFlags::NANOVG | AllocateFlags::IMGUI);
  auto src = Source::make(runtime, g_cli.inputVideo);
  runtime->setSource(src);

  if (!g_cli.outputVideo.empty()) {
    auto sink =
        Sink::make(runtime, g_cli.outputVideo,
                   src->fps() > 1.0 ? src->fps() : g_cli.fallbackFps,
                   g_cli.viewport.size());
    runtime->setSink(sink);
  }

  // run() takes a count of *extra* workers, so 0 means a single worker. One is
  // the right shape here: the graph already confines the pipeline to a SINGLE
  // branch, the DNN session and the feature detector are not re-entrant, and the
  // fb() node would overwrite frameBGR_ out from under a second worker reading
  // it. Use --every=N to keep up with the frame rate; the embedder costs ~45 ms,
  // so embedding every frame cannot sustain 25 fps on the CPU anyway.
  V4DPlan::run<Dinov3MarkerPlan>(0);
  return 0;
}