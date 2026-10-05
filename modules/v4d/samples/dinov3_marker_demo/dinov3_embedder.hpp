#ifndef DINOV3_EMBEDDER_HPP
#define DINOV3_EMBEDDER_HPP

#include <opencv2/core.hpp>

#include <opencv2/dnn.hpp>
#include <string>
#include <vector>

// The DINOv3 patch grid of one image.
//
// dinov3_vitb16.onnx returns last_hidden_state, a [1, N, 768] sequence: for a
// 224x224 input that is 14x14 = 196 patch tokens plus the CLS token and the four
// register tokens Meta inserts before them. Every patch token is a descriptor of
// the 16x16 pixel patch it came from, which is what makes them usable as *local*
// features: two views of the same planar marker do not have to agree globally to
// agree patch by patch, and a planar marker moves its patch tokens by a
// homography.
//
// That is the whole basis of the demo's outline. The pooled descriptor answers
// "is the marker I am looking for in view?"; the patch grid answers "and where is
// it?", for the price of normalising N rows that the forward pass has already
// computed -- no second inference.
struct PatchGrid {
    // N x D, CV_32F, each row L2-normalised so a dot product is a cosine
    // similarity. Patch tokens only, never the specials.
    cv::Mat descriptors;

    // Patches per row and per column. 14x14 at a 224x224 input.
    cv::Size shape = cv::Size(0, 0);

    // Pixel size of the image the grid describes. The ViT input is a plain
    // resize of that image to a square, so the grid-to-pixel mapping is
    // anisotropic whenever the image is not square; keeping `area` explicit is
    // what stops that from being silently assumed away.
    cv::Size2f area;

    bool empty() const {
        return descriptors.empty() || shape.width <= 0 || shape.height <= 0 ||
               area.width <= 0.0f || area.height <= 0.0f;
    }

    int count() const { return descriptors.rows; }

    // Pixel centre of patch `index`, row-major, within `area`.
    cv::Point2f centre(int index) const;

    // Pixel position -> normalised [0,1] patch coordinates. The marker and the
    // frame are compared in these units so that a change of resolution cannot
    // masquerade as a change of shape.
    static cv::Point2f normalise(const cv::Point2f& pixel, const cv::Size2f& area);
};

class Dinov3Embedder {
public:
    struct Config {
        int inputSize = 224;

        // ImageNet statistics in RGB order. Both are applied: blobFromImage has
        // no standard-deviation argument, so preprocess() divides by std itself.
        std::vector<float> mean = {0.485f, 0.456f, 0.406f};
        std::vector<float> stddev = {0.229f, 0.224f, 0.225f};

        bool scaleTo01 = true;
        bool swapRB = true;

        // cv::dnn::EngineType.
        //
        // ENGINE_ORT, not ENGINE_AUTO. ENGINE_AUTO resolves to ENGINE_OPENCV,
        // whose ONNX importer CHECK-fails out of protobuf on this graph's
        // external-weights file -- the process aborts during load, so no
        // exception can be caught and the demo cannot open at all. The ORT
        // engine reads the same .onnx plus its sibling .onnx.data and runs it.
        // On a build without WITH_ONNXRUNTIME this falls back to ENGINE_AUTO,
        // which is all that can be asked for there.
        int engine = cv::dnn::ENGINE_ORT;

        // Tokens DINOv3 puts before the first patch: CLS plus its registers.
        // Used only as the preferred answer -- embed() re-derives it from the
        // token count when the count does not add up, so a checkpoint with a
        // different register count still works.
        int registerTokens = 4;
    };

    bool load(const std::string& onnxPath, std::string& error);
    bool isLoaded() const;

    void setConfig(const Config& cfg);
    Config config() const;

    // Patches per row/column the model will produce at the configured input
    // size, e.g. Size(14, 14) at 224. Zero until load() has seen the output
    // shape.
    cv::Size patchShape() const;

    // How many tokens precede the patch grid. -1 when the output has not been
    // shaped yet.
    int specialTokens() const;

    // "engine=ORT input=224x224 patches=14x14 tokens=5 dim=768"
    std::string describe() const;

    // Pooled descriptor. `descriptor` is resized to the model's dimension.
    bool embed(
        const cv::UMat& image,
        std::vector<float>& descriptor,
        std::string& error
    );

    // The same forward pass, also handing back the patch tokens. The extra cost
    // over embed() is a per-row normalisation of an N x D matrix -- there is no
    // second inference -- so the localiser gets its correspondences from a pass
    // the demo was making anyway.
    bool embed(
        const cv::UMat& image,
        std::vector<float>& descriptor,
        PatchGrid& patches,
        std::string& error
    );

    // Last and average milliseconds per embed() call.
    double lastMs() const;
    double avgMs() const;

private:
    bool forward(const cv::UMat& image, cv::Mat& tokens, std::string& error);
    static void pool(const cv::Mat& tokens, int specials, std::vector<float>& descriptor);
    void noteShape(const cv::Mat& tokens);

    cv::dnn::Net net_;
    bool loaded_ = false;
    Config cfg_;
    std::vector<std::string> outputNames_;

    // Learned from the first forward pass, because the token count -- and
    // therefore the grid shape -- is a property of the graph, not of the config.
    int specials_ = -1;
    cv::Size patchShape_ = cv::Size(0, 0);
    int dim_ = 0;

    cv::TickMeter timer_;
};

#endif // DINOV3_EMBEDDER_HPP
