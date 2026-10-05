// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The DINOv3 half of the DINOv3 Marker Demo: an ONNX Runtime inference wrapper
// that turns a BGR image region into a single L2-normalised descriptor.
//
// Model (Meta, Apache 2.0): dinov3_vitb16.onnx, a ViT-B/16 vision transformer
// with four register tokens. Its graph takes
//
//   pixel_values        float32 [1, 3, H, W]
//
// and returns
//
//   last_hidden_state   float32 [1, (H/16)*(W/16) + 5, 768]
//
// so 201 tokens for a 224x224 input: 14*14 patch tokens plus five specials
// (the CLS token and the four registers). The demo pools those tokens down to
// one 768-dimensional vector; which pooling discriminates best is an empirical
// question, so the mode is an option (see PoolMode).
//
// This header deliberately knows nothing about V4D: the same object drives the
// windowed demo and the head-less self-test.

#ifndef OPENCV_DINOV3_EMBEDDER_HPP
#define OPENCV_DINOV3_EMBEDDER_HPP

#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>

#include <string>
#include <vector>

namespace cv {
namespace samples {
namespace dinov3 {

// How the [1, N, 768] token sequence is reduced to one descriptor.
enum class PoolMode {
  Cls,     // the CLS token alone -- what the model was trained to align
  Mean,    // the average over every token, specials included
  ClsMean  // the mean of those two, renormalised
};

std::string toString(PoolMode mode);
bool poolModeFromString(const std::string &name, PoolMode &mode);

struct EmbedderOptions {
  // Path to dinov3_vitb16.onnx. Its weights live in a sibling ".onnx.data"
  // file; onnxruntime resolves that name relative to the graph, so keep the
  // two next to each other.
  std::string modelPath;

  // dnn::EngineType. ENGINE_ORT is what the demo defaults to; ENGINE_AUTO lets
  // OpenCV pick (it falls back to its own graph engine, which does not
  // implement every op a transformer needs).
  int engine = dnn::ENGINE_ORT;

  // dnn::Target. OpenCV 5.1 maps every target but DNN_TARGET_CUDA to the CPU
  // provider when the ORT engine is used, so this selects between CPU and
  // CUDA execution rather than between CPU, OpenCL and Vulkan.
  int target = dnn::DNN_TARGET_CPU;

  Size inputSize = Size(224, 224);

  // How many register tokens the graph puts between the CLS token and the first
  // patch. DINOv3's released ViT-B/16 has four, so last_hidden_state for a
  // 224x224 input is [1, 201, 768] = 1 CLS + 4 registers + 14*14 patches.
  // embed() does not rely on this alone: if the token count does not add up, it
  // looks for a register count that makes the remainder a grid of the right
  // shape, and only gives up if none does.
  int registerTokens = 4;

  // Mean is the default because it is what discriminates on the marker video.
  // A handheld clip varies the marker's pose and distance far more than its
  // identity, and averaging every token keeps that variation from swamping the
  // signal that identifies it: measured against the supplied marker video, mean
  // pooling detected 92% of the marker's frames with no false positives, where
  // the CLS token -- what the model was aligned on -- detected 67%. CLS is still
  // selectable, and is the better choice for a crop dominated by one face-like
  // subject; see README.md.
  PoolMode pool = PoolMode::Mean;

  // ImageNet statistics in RGB order; the crop is converted BGR->RGB first.
  Scalar mean = Scalar(0.485, 0.456, 0.406);
  Scalar stdDev = Scalar(0.229, 0.224, 0.225);

  // Run one forward pass on a black image after loading, so that the first
  // real frame is not the one paying for session setup (~200 ms).
  bool warmUp = true;
};

// OpenCV 5's blobFromImage() has no standard-deviation overload, so the demo
// fills the [1, 3, H, W] NCHW blob itself.
void preprocessPatch(const Mat &bgr, const EmbedderOptions &options, Mat &blob);

// The patch grid of one forward pass: one L2-normalised descriptor per ViT
// patch, row-major, plus the grid it came from.
//
// This is the cheap half of dense matching. The transformer has already spent
// its attention on every one of these tokens by the time the pooled descriptor
// exists, so keeping them costs a per-row normalisation and nothing else -- and
// they are what localises the marker, because a planar marker moves its patch
// tokens by a homography while ORB's keypoints have to be re-found at whatever
// scale the next frame happens to use.
// Pixel position of the centre of patch `index` in a `grid`-shaped patch layout
// stretched over an image of `size`. Row-major.
//
// The ViT input is a plain resize to a square, so this mapping is anisotropic
// whenever the crop is not square; doing it explicitly keeps that honest instead
// of assuming the crop was square.
Point2f patchCentre(int index, Size grid, Size2f size);

struct PatchGrid {
  // patchCount x dim, CV_32F, each row L2-normalised so a dot product is a
  // cosine similarity.
  Mat descriptors;

  // Patches per row and per column of the token grid: 14x14 at a 224x224 input.
  Size grid = Size(0, 0);

  int dim() const { return descriptors.cols; }
  int count() const { return descriptors.rows; }

  // Pixel position of the centre of patch (row, col) inside an image of `size`
  // that the grid was stretched over; see patchCentre().
  Point2f centre(int row, int col, Size2f size) const {
    return patchCentre(row * grid.width + col, grid, size);
  }

  bool empty() const { return descriptors.empty() || grid.width <= 0 || grid.height <= 0; }
};

class Dinov3Embedder {
public:
  explicit Dinov3Embedder(const EmbedderOptions &options);
  ~Dinov3Embedder();

  Dinov3Embedder(const Dinov3Embedder &) = delete;
  Dinov3Embedder &operator=(const Dinov3Embedder &) = delete;

  // Reads the graph and, with warmUp on, runs one forward pass. Both stages
  // report through `error`: onnxruntime creates its session lazily, so a
  // missing external data file surfaces here and not during load.
  bool load(std::string &error);

  bool ready() const;
  int dim() const;
  Size inputSize() const;

  // "engine=ORT target=CPU input=224x224 pool=cls dim=768 42.3 ms"
  std::string describe() const;

  // Milliseconds of the most recent and of the average embed() call.
  double lastMs() const;
  double avgMs() const;

  // Crops nothing: pass the region of interest itself. Returns a 1 x dim()
  // CV_32F row, L2-normalised, so that a dot product is a cosine similarity.
  bool embed(InputArray bgr, OutputArray embedding, std::string &error);

  // The same forward pass, also handing back the patch tokens. The extra cost
  // over embed() is a per-row normalisation of an N x dim matrix -- no second
  // inference -- so the dense localiser gets its correspondences for free from
  // a pass the demo was making anyway.
  bool embed(InputArray bgr, OutputArray embedding, PatchGrid &patches,
             std::string &error);

  // The patch grid the model produced, as inferred from the token count and the
  // input size. Valid after a successful load().
  Size patchGrid() const;
  int patchDim() const;

  static std::string engineName(int engine);
  static std::string targetName(int target);
  // The dnn::Target values this OpenCV build advertises for DNN_BACKEND_OPENCV,
  // as names, e.g. {"CPU", "OPENCL", "OPENCL_FP16"}.
  static std::vector<std::string> availableTargets();

private:
  bool forwardOnce(const Mat &bgr, Mat &embedding, PatchGrid *patches,
                   std::string &error);
  bool poolTokens(const std::vector<Mat> &outputs, Mat &embedding,
                  PatchGrid *patches, std::string &error);
  void resolveOutputNames();

  EmbedderOptions options_;
  dnn::Net net_;
  bool ready_ = false;
  int dim_ = 0;
  Size patchGrid_;
  std::string outputNamesDesc_;
  std::vector<std::string> outputNames_;
  TickMeter timer_;
  Mat blob_;
};

} // namespace dinov3
} // namespace samples
} // namespace cv

#endif // OPENCV_DINOV3_EMBEDDER_HPP