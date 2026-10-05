// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#include "dinov3-embedder.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace cv {
namespace samples {
namespace dinov3 {

std::string toString(PoolMode mode) {
  switch (mode) {
  case PoolMode::Cls:
    return "cls";
  case PoolMode::Mean:
    return "mean";
  case PoolMode::ClsMean:
    return "cls+mean";
  }
  return "cls";
}

bool poolModeFromString(const std::string &name, PoolMode &mode) {
  if (name == "cls") {
    mode = PoolMode::Cls;
    return true;
  }
  if (name == "mean") {
    mode = PoolMode::Mean;
    return true;
  }
  if (name == "clsmean" || name == "cls+mean") {
    mode = PoolMode::ClsMean;
    return true;
  }
  return false;
}

void preprocessPatch(const Mat &bgr, const EmbedderOptions &options, Mat &blob) {
  const Size &sz = options.inputSize;

  Mat rgb, resized, scaled;
  cvtColor(bgr, rgb, COLOR_BGR2RGB);
  resize(rgb, resized, sz, 0.0, 0.0, INTER_LINEAR);
  resized.convertTo(scaled, CV_32F, 1.0 / 255.0);

  std::vector<Mat> planes;
  split(scaled, planes);

  blob.create({1, 3, sz.height, sz.width}, CV_32F);
  for (int c = 0; c < 3; ++c) {
    // A Mat over one NCHW plane: the buffer belongs to `blob`, so writing into
    // it through an alias is what keeps the blob contiguous.
    Mat dst(sz.height, sz.width, CV_32F, blob.ptr(0, c));
    planes[c].convertTo(dst, CV_32F, 1.0, -options.mean[c]);
    dst /= options.stdDev[c];
  }
}

Dinov3Embedder::Dinov3Embedder(const EmbedderOptions &options)
    : options_(options) {}

Dinov3Embedder::~Dinov3Embedder() = default;

std::string Dinov3Embedder::engineName(int engine) {
  switch (engine) {
  case dnn::ENGINE_ORT:
    return "ORT";
  case dnn::ENGINE_OPENCV:
    return "OpenCV";
  case dnn::ENGINE_AUTO:
    return "auto";
  default:
    return "engine#" + std::to_string(engine);
  }
}

std::string Dinov3Embedder::targetName(int target) {
  switch (target) {
  case dnn::DNN_TARGET_CPU:
    return "CPU";
  case dnn::DNN_TARGET_OPENCL:
    return "OPENCL";
  case dnn::DNN_TARGET_OPENCL_FP16:
    return "OPENCL_FP16";
  case dnn::DNN_TARGET_CUDA:
    return "CUDA";
  case dnn::DNN_TARGET_CUDA_FP16:
    return "CUDA_FP16";
  case dnn::DNN_TARGET_VULKAN:
    return "VULKAN";
  default:
    return "target#" + std::to_string(target);
  }
}

std::vector<std::string> Dinov3Embedder::availableTargets() {
  std::vector<std::string> names;
  for (const auto &entry : dnn::getAvailableBackends()) {
    if (entry.first != dnn::DNN_BACKEND_OPENCV)
      continue;
    const std::string name = targetName((int)entry.second);
    if (std::find(names.begin(), names.end(), name) == names.end())
      names.push_back(name);
  }
  return names;
}

void Dinov3Embedder::resolveOutputNames() {
  outputNames_.clear();
  try {
    for (const String &name : net_.getUnconnectedOutLayersNames())
      outputNames_.push_back(name);
  } catch (const std::exception &) {
    // A net built by the ORT engine exposes no layer names; forward() then
    // returns the graph's single output, which is what we want.
  }
  outputNamesDesc_ = outputNames_.empty() ? std::string("(default)")
                                          : outputNames_.front();
}

bool Dinov3Embedder::load(std::string &error) {
  ready_ = false;
  if (options_.modelPath.empty()) {
    error = "no model path given";
    return false;
  }
  try {
    net_ = dnn::readNetFromONNX(options_.modelPath, options_.engine);
    net_.setPreferableBackend(dnn::DNN_BACKEND_OPENCV);
    net_.setPreferableTarget(options_.target);
    resolveOutputNames();
  } catch (const std::exception &e) {
    error = std::string("cannot load '") + options_.modelPath +
            "': " + e.what();
    return false;
  }

  if (!options_.warmUp) {
    ready_ = true;
    return true;
  }

  // The session itself is created on the first forward pass, so that is where a
  // missing external data file or an unsupported operator shows up.
  Mat embedding;
  if (!forwardOnce(Mat::zeros(options_.inputSize, CV_8UC3), embedding, nullptr,
                   error)) {
    error = "the graph loaded but the first forward pass failed: " + error;
    return false;
  }
  timer_.reset();
  ready_ = true;
  return true;
}

bool Dinov3Embedder::ready() const { return ready_; }

int Dinov3Embedder::dim() const { return dim_; }

Size Dinov3Embedder::inputSize() const { return options_.inputSize; }

Size Dinov3Embedder::patchGrid() const { return patchGrid_; }

int Dinov3Embedder::patchDim() const { return dim_; }

double Dinov3Embedder::lastMs() const { return timer_.getLastTimeMilli(); }

double Dinov3Embedder::avgMs() const { return timer_.getAvgTimeMilli(); }

std::string Dinov3Embedder::describe() const {
  std::ostringstream os;
  os << "engine=" << engineName(options_.engine)
     << " target=" << targetName(options_.target) << " input="
     << options_.inputSize.width << "x" << options_.inputSize.height
     << " pool=" << toString(options_.pool) << " out=" << outputNamesDesc_;
  if (dim_ > 0)
    os << " dim=" << dim_;
  if (patchGrid_.width > 0)
    os << " patches=" << patchGrid_.width << "x" << patchGrid_.height;
  os << " " << std::fixed << std::setprecision(1) << avgMs() << " ms";
  return os.str();
}

Point2f patchCentre(int index, Size grid, Size2f size) {
  const int row = grid.width > 0 ? index / grid.width : index;
  const int col = grid.width > 0 ? index % grid.width : 0;
  const float x = ((float)col + 0.5f) * size.width / (float)grid.width;
  const float y = ((float)row + 0.5f) * size.height / (float)grid.height;
  return Point2f(x, y);
}

namespace {

// How many tokens before the first patch: the CLS token plus the model's
// register tokens. options.registerTokens is the answer for DINOv3, but a
// checkpoint with a different register count should still work, so fall back to
// the register count that leaves a grid whose shape matches the input.
int specialsFor(int tokens, const EmbedderOptions &options) {
  const int preferred = 1 + std::max(0, options.registerTokens);
  if (tokens - preferred > 0) {
    const int patches = tokens - preferred;
    const int side = (int)std::lround(std::sqrt((double)patches));
    if (side * side == patches)
      return preferred;
  }
  for (int registers = 0; registers <= 8; ++registers) {
    const int patches = tokens - 1 - registers;
    if (patches <= 0)
      break;
    const double ratio = (double)options.inputSize.width /
                         (double)std::max(1, options.inputSize.height);
    for (int rows = 1; rows * rows <= patches; ++rows) {
      if (patches % rows != 0)
        continue;
      const int cols = patches / rows;
      // The closer the grid's aspect is to the input's, the more likely this is
      // the real one.
      const double gridRatio = (double)cols / (double)rows;
      const double error = std::abs(gridRatio - ratio) / std::max(1e-6, ratio);
      if (error < 0.05)
        return 1 + registers;
    }
  }
  return preferred;
}

} // namespace

bool Dinov3Embedder::poolTokens(const std::vector<Mat> &outputs,
                                Mat &embedding, PatchGrid *patches,
                                std::string &error) {
  if (outputs.empty()) {
    error = "the network produced no output";
    return false;
  }

  // last_hidden_state is [1, N, D]: a rank-3 tensor whose last axis is the
  // embedding. Rank 2 (batch squeezed out) and rank 4 (batch kept twice) show
  // up depending on the exporter, so normalise all three to an N x D matrix.
  const Mat &out = outputs[0];
  Mat tokens;
  if (out.dims == 1) {
    tokens = out.reshape(1, 1);
  } else if (out.dims == 2) {
    tokens = out;
  } else if (out.dims == 3 || out.dims == 4) {
    const int dim = (int)out.size[out.dims - 1];
    if (dim < 1) {
      error = "the output has an empty embedding axis";
      return false;
    }
    tokens = out.reshape(1, (int)(out.total() / dim));
  } else {
    error = "unexpected embedding rank " + std::to_string(out.dims) +
            " (expected 1, 2, 3 or 4)";
    return false;
  }
  if (tokens.type() != CV_32F)
    tokens.convertTo(tokens, CV_32F);
  if (tokens.cols == 1 && tokens.rows > 1)
    tokens = tokens.t();

  const int dim = tokens.cols;
  embedding.create({1, dim}, CV_32F);

  // The CLS token is token 0; the mean runs over every token, specials
  // included. Which of the two (or their average) separates markers best is an
  // empirical question -- see the numbers in the sample's README.
  Mat mean;
  switch (options_.pool) {
  case PoolMode::Cls:
    for (int i = 0; i < dim; ++i)
      embedding.at<float>(0, i) = tokens.at<float>(0, i);
    break;
  case PoolMode::Mean:
    reduce(tokens, mean, 0, REDUCE_AVG, CV_32F);
    for (int i = 0; i < dim; ++i)
      embedding.at<float>(0, i) = mean.at<float>(0, i);
    break;
  case PoolMode::ClsMean:
    reduce(tokens, mean, 0, REDUCE_AVG, CV_32F);
    for (int i = 0; i < dim; ++i)
      embedding.at<float>(0, i) =
          0.5f * (tokens.at<float>(0, i) + mean.at<float>(0, i));
    break;
  }

  const double norm = std::sqrt((double)embedding.dot(embedding));
  if (!(norm > 0.0)) {
    error = "the pooled descriptor is all zeros";
    return false;
  }
  embedding /= (float)norm;
  dim_ = embedding.cols;

  if (patches) {
    // The patch tokens are the rows after the specials, and they are the local
    // signal: normalise each row so the caller can compare patches by cosine.
    const int specials = std::min(specialsFor(tokens.rows, options_), tokens.rows);
    const int count = tokens.rows - specials;
    const int side = (int)std::lround(std::sqrt((double)count));
    if (count > 0 && side * side == count) {
      patches->descriptors.create({count, dim}, CV_32F);
      for (int i = 0; i < count; ++i) {
        Mat row = tokens.row(specials + i);
        Mat dst = patches->descriptors.row(i);
        row.copyTo(dst);
        const double n = std::sqrt((double)dst.dot(dst));
        if (n > 0.0)
          dst /= (float)n;
      }
      patches->grid = Size(side, side);
      patchGrid_ = patches->grid;
    } else {
      patches->descriptors.release();
      patches->grid = Size(0, 0);
      patchGrid_ = Size(0, 0);
    }
  }
  return true;
}

bool Dinov3Embedder::forwardOnce(const Mat &bgr, Mat &embedding,
                                 PatchGrid *patches, std::string &error) {
  try {
    timer_.start();
    preprocessPatch(bgr, options_, blob_);
    net_.setInput(blob_);

    std::vector<Mat> outputs;
    if (outputNames_.empty())
      net_.forward(outputs);
    else
      net_.forward(outputs, outputNames_);

    const bool pooled = poolTokens(outputs, embedding, patches, error);
    timer_.stop();
    return pooled;
  } catch (const std::exception &e) {
    timer_.stop();
    error = e.what();
    return false;
  }
}

bool Dinov3Embedder::embed(InputArray bgr, OutputArray embeddingOut,
                           std::string &error) {
  PatchGrid discard;
  return embed(bgr, embeddingOut, discard, error);
}

bool Dinov3Embedder::embed(InputArray bgr, OutputArray embeddingOut,
                           PatchGrid &patches, std::string &error) {
  if (!ready_ && !load(error))
    return false;

  const Mat bgrMat = bgr.getMat();
  if (bgrMat.empty()) {
    error = "empty image";
    return false;
  }

  Mat embedding;
  if (!forwardOnce(bgrMat, embedding, &patches, error))
    return false;

  embedding.copyTo(embeddingOut);
  return true;
}

} // namespace dinov3
} // namespace samples
} // namespace cv