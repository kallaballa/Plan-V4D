#include "dinov3_embedder.hpp"
#include <opencv2/imgproc.hpp>
#include <numeric>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace {

std::string engineName(int engine) {
    switch (engine) {
    case cv::dnn::ENGINE_ORT: return "ORT";
    case cv::dnn::ENGINE_OPENCV: return "OPENCV";
    default: return "AUTO";
    }
}

// How many tokens sit before the first patch token.
//
// The preferred answer is the CLS token plus the model's register tokens. A
// checkpoint exported with a different register count still works, because the
// remainder has to be a grid whose shape is consistent with the input; we look
// for the register count that makes that true and only fall back to the
// preferred value when nothing does.
int specialsFor(int tokens, const cv::Size& inputSize, int preferredRegisters) {
    const int preferred = 1 + std::max(0, preferredRegisters);
    if (tokens - preferred > 0) {
        const int patches = tokens - preferred;
        const int side = static_cast<int>(std::lround(std::sqrt(static_cast<double>(patches))));
        if (side * side == patches)
            return preferred;
    }
    for (int registers = 0; registers <= 8; ++registers) {
        const int patches = tokens - 1 - registers;
        if (patches <= 0)
            break;
        const double ratio = static_cast<double>(inputSize.width) /
                             static_cast<double>(std::max(1, inputSize.height));
        for (int rows = 1; rows * rows <= patches; ++rows) {
            if (patches % rows != 0)
                continue;
            const int cols = patches / rows;
            const double gridRatio = static_cast<double>(cols) / static_cast<double>(rows);
            const double error = std::abs(gridRatio - ratio) / std::max(1e-6, ratio);
            if (error < 0.05)
                return 1 + registers;
        }
    }
    return std::min(preferred, std::max(0, tokens - 1));
}

} // namespace

cv::Point2f PatchGrid::centre(int index) const {
    if (shape.width <= 0 || shape.height <= 0)
        return cv::Point2f(0.f, 0.f);
    const int row = index / shape.width;
    const int col = index % shape.width;
    return cv::Point2f(
        (static_cast<float>(col) + 0.5f) * area.width / static_cast<float>(shape.width),
        (static_cast<float>(row) + 0.5f) * area.height / static_cast<float>(shape.height));
}

cv::Point2f PatchGrid::normalise(const cv::Point2f& pixel, const cv::Size2f& a) {
    return cv::Point2f(pixel.x / std::max(1e-6f, a.width),
                       pixel.y / std::max(1e-6f, a.height));
}

bool Dinov3Embedder::load(const std::string& onnxPath, std::string& error) {
    loaded_ = false;
    specials_ = -1;
    patchShape_ = cv::Size(0, 0);
    dim_ = 0;
    try {
        net_ = cv::dnn::readNetFromONNX(onnxPath, cfg_.engine);
        if (net_.empty()) {
            error = "failed to load ONNX model: " + onnxPath;
            return false;
        }
        outputNames_.clear();
        try {
            for (const std::string& name : net_.getUnconnectedOutLayersNames())
                outputNames_.push_back(name);
        } catch (const std::exception&) {
            // A net built by the ORT engine exposes no layer names. forward()
            // then returns the graph's single output, which is what we want.
        }
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }

    // ONNX Runtime creates its session on the first forward pass, so a missing
    // external-weights file or an unsupported operator shows up here and not
    // during load(). One black-image pass warms the session too, so the first
    // real frame does not pay for it.
    cv::Mat tokens;
    std::string forwardError;
    const cv::Mat black(cv::Size(cfg_.inputSize, cfg_.inputSize), CV_8UC3, cv::Scalar::all(0));
    cv::UMat blackUmat;
    black.copyTo(blackUmat);
    if (!forward(blackUmat, tokens, forwardError)) {
        error = "the graph loaded but the first forward pass failed: " + forwardError;
        return false;
    }
    timer_.reset();
    loaded_ = true;
    error.clear();
    return true;
}

bool Dinov3Embedder::isLoaded() const {
    return loaded_;
}

void Dinov3Embedder::setConfig(const Config& cfg) {
    cfg_ = cfg;
}

Dinov3Embedder::Config Dinov3Embedder::config() const {
    return cfg_;
}

cv::Size Dinov3Embedder::patchShape() const {
    return patchShape_;
}

int Dinov3Embedder::specialTokens() const {
    return specials_;
}

double Dinov3Embedder::lastMs() const {
    return timer_.getLastTimeMilli();
}

double Dinov3Embedder::avgMs() const {
    return timer_.getAvgTimeMilli();
}

std::string Dinov3Embedder::describe() const {
    std::ostringstream os;
    os << "engine=" << engineName(cfg_.engine)
       << " input=" << cfg_.inputSize << "x" << cfg_.inputSize;
    if (patchShape_.width > 0)
        os << " patches=" << patchShape_.width << "x" << patchShape_.height;
    if (specials_ > 0)
        os << " specials=" << specials_;
    if (dim_ > 0)
        os << " dim=" << dim_;
    os << " " << std::fixed << std::setprecision(1) << avgMs() << " ms";
    return os.str();
}

void Dinov3Embedder::noteShape(const cv::Mat& tokens) {
    specials_ = specialsFor(tokens.rows, cv::Size(cfg_.inputSize, cfg_.inputSize), cfg_.registerTokens);
    const int patches = tokens.rows - specials_;
    dim_ = tokens.cols;
    // A square ViT input gives a square grid. Anything else would mean the grid
    // and the image disagree about aspect, which area/grid mapping assumes is
    // handled by scaling -- so recover a grid anyway rather than giving up.
    const int side = static_cast<int>(std::lround(std::sqrt(static_cast<double>(std::max(0, patches)))));
    patchShape_ = cv::Size(side, side);
    if (side * side != std::max(0, patches)) {
        // Not square: fall back to whatever factors the patch count has, biased
        // towards the input's own aspect.
        int bestRows = std::max(1, patches), bestCols = std::max(1, patches);
        double bestError = 1e9;
        for (int rows = 1; rows * rows <= std::max(1, patches); ++rows) {
            if (patches % rows != 0)
                continue;
            const int cols = patches / rows;
            const double error = std::abs(static_cast<double>(cols) / rows - 1.0);
            if (error < bestError) {
                bestError = error;
                bestRows = rows;
                bestCols = cols;
            }
        }
        patchShape_ = cv::Size(bestCols, bestRows);
    }
}

bool Dinov3Embedder::forward(const cv::UMat& image, cv::Mat& tokens, std::string& error) {
    if (!loaded_ && net_.empty()) {
        error = "model not loaded";
        return false;
    }
    if (image.empty()) {
        error = "empty image input";
        return false;
    }

    timer_.start();
    try {
        cv::UMat bgr;
        if (image.channels() == 4) {
            cv::cvtColor(image, bgr, cv::COLOR_BGRA2BGR);
        } else if (image.channels() == 1) {
            cv::cvtColor(image, bgr, cv::COLOR_GRAY2BGR);
        } else if (image.channels() != 3) {
            error = "unsupported channel count " + std::to_string(image.channels());
            return false;
        } else {
            bgr = image;
        }

        const cv::Size inputSize(cfg_.inputSize, cfg_.inputSize);
        cv::Mat blob = cv::dnn::blobFromImage(bgr,
                                              cfg_.scaleTo01 ? 1.0f / 255.0f : 1.0f,
                                              inputSize,
                                              cv::Scalar(cfg_.mean[0], cfg_.mean[1], cfg_.mean[2]),
                                              cfg_.swapRB,
                                              false,
                                              CV_32F);

        // blobFromImage takes no standard deviation, so apply it here rather than
        // leaving the stored values unused. This matches the training recipe.
        if (cfg_.stddev.size() == 3 && cfg_.stddev[0] > 0.0f && cfg_.stddev[1] > 0.0f && cfg_.stddev[2] > 0.0f) {
            // swapRB was applied above, so the channel order in the blob is RGB
            // and the standard deviations have to follow suit.
            const std::array<float, 3> sd = cfg_.swapRB
                ? std::array<float, 3>{cfg_.stddev[2], cfg_.stddev[1], cfg_.stddev[0]}
                : std::array<float, 3>{cfg_.stddev[0], cfg_.stddev[1], cfg_.stddev[2]};
            // The blob is NCHW, so a channel is a strided plane rather than a
            // cv::Mat of its own. Reshaping to Hx(W*C) keeps the three planes
            // contiguous and in order, which lets each one be scaled in place
            // without splitting and re-merging the whole thing.
            if (blob.dims == 4 && blob.size[0] == 1 && blob.size[1] == 3) {
                const int height = blob.size[2];
                const int width = blob.size[3];
                cv::Mat flat = blob.reshape(0, height);
                if (flat.isContinuous()) {
                    for (int c = 0; c < 3; ++c) {
                        float* p = flat.ptr<float>(c);
                        const float inv = 1.0f / sd[c];
                        for (int i = 0; i < width; ++i)
                            p[i] *= inv;
                    }
                } else {
                    for (int c = 0; c < 3; ++c) {
                        cv::Mat plane = flat.colRange(c * width, (c + 1) * width);
                        plane *= (1.0f / sd[c]);
                    }
                }
            }
        }

        net_.setInput(blob);
        const cv::Mat output = outputNames_.empty()
            ? net_.forward()
            : net_.forward(outputNames_.front());
        if (output.empty()) {
            error = "empty model output";
            return false;
        }

        // Normalise every plausible tensor rank to an N x D matrix of tokens,
        // where D is the trailing axis. Whether the exporter keeps the batch
        // axis, squeezes it, or keeps it twice differs between tools, so rank is
        // handled rather than assumed.
        const int dim = static_cast<int>(output.size[output.dims - 1]);
        if (dim < 1) {
            error = "the model output has an empty embedding axis";
            return false;
        }
        if (output.dims == 1)
            tokens = output.reshape(1, 1);
        else if (output.dims == 2)
            tokens = output;
        else if (output.dims == 3 || output.dims == 4)
            tokens = output.reshape(1, static_cast<int>(output.total() / dim));
        else {
            error = "unexpected embedding rank " + std::to_string(output.dims);
            return false;
        }
        if (tokens.empty() || tokens.cols < 1) {
            error = "the model produced no tokens";
            return false;
        }

        noteShape(tokens);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    timer_.stop();
    return true;
}

void Dinov3Embedder::pool(const cv::Mat& tokens, int specials, std::vector<float>& descriptor) {
    // Mean over every token, specials included. Averaging the whole sequence
    // rather than reading the CLS token keeps the marker's pose -- which the
    // hand-held footage varies far more than its identity -- from swamping the
    // part of the descriptor that says what it is.
    const int rows = tokens.rows;
    const int cols = tokens.cols;
    descriptor.assign(static_cast<size_t>(cols), 0.0f);
    const float* data = tokens.ptr<float>();
    for (int r = 0; r < rows; ++r) {
        const float* row = data + static_cast<size_t>(r) * cols;
        for (int c = 0; c < cols; ++c)
            descriptor[static_cast<size_t>(c)] += row[c];
    }
    const float inv = 1.0f / static_cast<float>(std::max(1, rows));
    for (auto& v : descriptor)
        v *= inv;
    (void)specials;

    float norm = 0.0f;
    for (float v : descriptor)
        norm += v * v;
    norm = std::sqrt(norm + 1e-12f);
    if (norm > 0.0f)
        for (auto& v : descriptor)
            v /= norm;
}

bool Dinov3Embedder::embed(const cv::UMat& image, std::vector<float>& descriptor, std::string& error) {
    PatchGrid unused;
    return embed(image, descriptor, unused, error);
}

bool Dinov3Embedder::embed(const cv::UMat& image, std::vector<float>& descriptor,
                           PatchGrid& patches, std::string& error) {
    patches.descriptors.release();
    patches.shape = cv::Size(0, 0);

    cv::Mat tokens;
    if (!forward(image, tokens, error))
        return false;

    pool(tokens, specials_, descriptor);
    if (descriptor.empty()) {
        error = "the model produced an empty descriptor";
        return false;
    }

    // Patch tokens: everything after the specials, one L2-normalised row each, so
    // a dot product between rows is a cosine similarity.
    const int patchRows = tokens.rows - std::max(0, specials_);
    if (patchRows > 0 && patchShape_.width > 0) {
        const cv::Mat block = tokens.rowRange(std::max(0, specials_), tokens.rows);
        // L2-normalise each row so a dot product is a cosine similarity. Written
        // as an explicit loop rather than mul()/reduce(): broadcasting an Nx1 norm
        // over an NxD block is easy to get subtly wrong, and the failure mode --
        // rows that are not unit length -- makes every downstream cosine
        // meaningless while still producing plausible-looking numbers.
        cv::Mat normalised(block.rows, block.cols, CV_32F);
        for (int r = 0; r < block.rows; ++r) {
            const float* src = block.ptr<float>(r);
            float* dst = normalised.ptr<float>(r);
            float sum = 0.0f;
            for (int c = 0; c < block.cols; ++c)
                sum += src[c] * src[c];
            // A zero-norm patch is left as zeros rather than scaled by an
            // epsilon: it will then score 0 against everything and be rejected
            // by the correspondence floor, which is the honest outcome.
            const float inv = sum > 1e-12f ? 1.0f / std::sqrt(sum) : 0.0f;
            for (int c = 0; c < block.cols; ++c)
                dst[c] = src[c] * inv;
        }
        patches.descriptors = normalised;
        patches.shape = patchShape_;
        patches.area = cv::Size2f(static_cast<float>(image.cols), static_cast<float>(image.rows));
    }

    error.clear();
    return true;
}
