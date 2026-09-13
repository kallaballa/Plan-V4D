#include <opencv2/v4d/v4d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <filesystem>
#include <algorithm>
#include <cmath>

using namespace cv;
using namespace cv::v4d;

class StackedNanoVGGalleryPlan : public V4DPlan {
    struct ImageEntry {
        int handle_ = 0;
        int w_ = 0;
        int h_ = 0;
        std::string path_;
    };

    struct GalleryState {
        float offsetX_ = 0.0f;
        float velocityX_ = 0.0f;
        int currentIndex_ = 0;
        bool dragging_ = false;
        float lastX_ = 0.0f;
    };

    std::vector<ImageEntry> images_;
    static GalleryState state_;
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
    Event<Mouse> dragEvents_ = E<Mouse>(Mouse::Type::DRAG, Mouse::LEFT);
    Event<Mouse> pressEvents_ = E<Mouse>(Mouse::Type::PRESS, Mouse::LEFT);
    Event<Mouse> releaseEvents_ = E<Mouse>(Mouse::Type::RELEASE, Mouse::LEFT);

public:
    StackedNanoVGGalleryPlan(const std::vector<std::string>& paths) {
        for (const auto& p : paths) {
            images_.push_back(ImageEntry{0, 0, 0, p});
        }
        _shared(state_);
    }

    void setup() override {
        nvg([](std::vector<ImageEntry>& imgs) {
            using namespace cv::v4d::nvg;
            for (auto& img : imgs) {
                if (img.handle_ == 0) {
                    int handle = createImage(img.path_.c_str(), NVG_IMAGE_NEAREST);
                    if (handle > 0) {
                        imageSize(handle, &img.w_, &img.h_);
                        img.handle_ = handle;
                    }
                }
            }
        }, RW(images_));
    }

    void infer() override {
        branch(BranchType::SINGLE, !F(&Mouse::List::empty, pressEvents_))
            ->plain([](GalleryState& state, const Mouse::List& evts) {
                state.dragging_ = true;
                state.lastX_ = evts.back().pos.x;
                state.velocityX_ = 0.0f;
            }, RWS(state_), pressEvents_)
        ->endBranch();

        branch(BranchType::SINGLE, !F(&Mouse::List::empty, releaseEvents_))
            ->plain([](GalleryState& state) {
                state.dragging_ = false;
            }, RWS(state_), releaseEvents_)
        ->endBranch();

        branch(BranchType::SINGLE, !F(&Mouse::List::empty, dragEvents_))
            ->plain([](GalleryState& state, const Mouse::List& evts) {
                float dx = evts.back().delta.x;
                state.offsetX_ += dx;
                state.velocityX_ = dx;
            }, RWS(state_), dragEvents_)
        ->endBranch();

        branch(BranchType::SINGLE, F([](const GalleryState& s) {
            return !s.dragging_ && std::abs(s.velocityX_) > 0.1f;
        }, CS(state_)))
            ->plain([](GalleryState& state) {
                state.offsetX_ += state.velocityX_;
                state.velocityX_ *= 0.92f;
            }, RWS(state_))
        ->endBranch();

        branch(BranchType::SINGLE, F([](const GalleryState& s) {
            return !s.dragging_ && std::abs(s.velocityX_) <= 0.1f;
        }, CS(state_)))
            ->plain([](GalleryState& state, const cv::Size& sz) {
                float cardW = sz.width * 0.65f;
                float gap = 40.0f;
                float totalW = cardW * 0.75f + gap;
                int idx = std::round(-state.offsetX_ / totalW);
                idx = std::clamp(idx, 0, (int)state.images_.size() - 1);
                state.currentIndex_ = idx;
                float target = -idx * totalW;
                state.offsetX_ += (target - state.offsetX_) * 0.25f;
                if (std::abs(target - state.offsetX_) < 0.5f) {
                    state.offsetX_ = target;
                    state.velocityX_ = 0.0f;
                }
            }, RWS(state_), size_)
        ->endBranch();

        nvg([](const cv::Size& sz, const GalleryState& state) {
            using namespace cv::v4d::nvg;
            clearScreen();

            if (state.images_.empty()) return;

            float cardW = sz.width * 0.65f;
            float cardH = sz.height * 0.7f;
            float gap = 40.0f;
            float totalW = cardW * 0.75f + gap;
            float startX = (sz.width - cardW) / 2.0f + state.offsetX_;
            float startY = (sz.height - cardH) / 2.0f;

            for (size_t i = 0; i < state.images_.size(); ++i) {
                if ((int)i == state.currentIndex_) continue;
                drawCard(sz, state, i, startX, startY, cardW, cardH, totalW, gap);
            }
            if (!state.images_.empty()) {
                drawCard(sz, state, state.currentIndex_, startX, startY, cardW, cardH, totalW, gap);
            }
        }, size_, CS(state_));
    }

private:
    static void drawCard(const cv::Size& sz, const GalleryState& state, size_t i,
                         float startX, float startY, float cardW, float cardH,
                         float totalW, float gap) {
        using namespace cv::v4d::nvg;
        const auto& img = state.images_[i];
        if (img.handle_ == 0) return;

        float x = startX + (float)i * totalW;
        float cx = x + cardW / 2.0f;
        float cy = startY + cardH / 2.0f;
        float distFromCenter = std::abs(cx - sz.width / 2.0f);
        float maxDist = sz.width * 0.8f;
        float t = 1.0f - std::min(distFromCenter / maxDist, 1.0f);
        float scale = 0.85f + 0.15f * t;

        float yOffset = (cx - sz.width / 2.0f) * 0.12f;

        save();
        translate(cx, cy + yOffset);
        scale(scale, scale);
        translate(-cx, -cy);

        beginPath();
        roundedRect(x + 8, startY + 8, cardW, cardH, 16.0f);
        fillColor(Scalar(0, 0, 0, 50));
        fill();

        beginPath();
        roundedRect(x, startY, cardW, cardH, 12.0f);
        fillColor(Scalar(45, 45, 45, 255));
        fill();

        if (img.w_ > 0 && img.h_ > 0) {
            float imgScale = std::min(cardW / (float)img.w_, cardH / (float)img.h_);
            float drawW = img.w_ * imgScale;
            float drawH = img.h_ * imgScale;
            float imgX = x + (cardW - drawW) / 2.0f;
            float imgY = startY + (cardH - drawH) / 2.0f;

            beginPath();
            roundedRect(x + 6, startY + 6, cardW - 12, cardH - 12, 8.0f);
            scissor();

            Paint paint = imagePattern(imgX, imgY, drawW, drawH, 0.0f, img.handle_, 1.0f);
            fillPaint(paint);
            beginPath();
            rect(imgX, imgY, drawW, drawH);
            fill();

            resetScissor();
        }

        if ((int)i == state.currentIndex_) {
            beginPath();
            roundedRect(x, startY, cardW, cardH, 12.0f);
            strokeColor(Scalar(255, 255, 255, 200));
            strokeWidth(2.0f);
            stroke();
        }

        restore();
    }
};

StackedNanoVGGalleryPlan::GalleryState StackedNanoVGGalleryPlan::state_;

int main(int argc, char** argv) {
    std::vector<std::string> paths;
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            std::error_code ec;
            if (std::filesystem::is_directory(arg, ec)) {
                for (auto& p : std::filesystem::directory_iterator(arg)) {
                    if (p.is_regular_file(ec)) {
                        auto ext = p.path().extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" ||
                            ext == ".bmp" || ext == ".webp" || ext == ".tiff" || ext == ".tif") {
                            paths.push_back(p.path().string());
                        }
                    }
                }
            } else {
                paths.push_back(arg);
            }
        }
    }

    if (paths.empty()) {
        std::cerr << "Usage: " << argv[0] << " <image1> [image2] ... OR <directory>" << std::endl;
        return 1;
    }

    cv::Rect viewport(0, 0, 960, 960);
    Ptr<V4D> runtime = V4D::init(viewport, "2.5D Stacked NanoVG Image Gallery",
                                  AllocateFlags::NANOVG | AllocateFlags::IMGUI,
                                  ConfigFlags::DISPLAY_MODE);

    V4DPlan::run<StackedNanoVGGalleryPlan>(2, paths);
}
