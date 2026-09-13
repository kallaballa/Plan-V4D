// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#include <opencv2/v4d/v4d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <filesystem>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>

using namespace cv;
using namespace cv::v4d;
using namespace cv::v4d::event;

/**
 * @brief 2.5D stacked NanoVG image gallery.
 *
 * Accepts images as command-line arguments or a directory path.
 * Displays them as stacked cards with a 2.5D rotation effect.
 * - Drag with the left mouse button to rotate the stack.
 * - Scroll to switch the active card.
 * - Press R to reset the view.
 */
class StackedImageGalleryPlan : public V4DPlan {
    struct ImageCard {
        std::string path;
        int imageHandle = 0;
        int width = 0;
        int height = 0;
        bool loaded = false;
    };

    struct Params {
        float rotationY = 0.0f;         ///< Current Y rotation (smoothed)
        float targetRotationY = 0.0f;   ///< Target Y rotation
        float tiltX = 0.0f;             ///< Current X tilt (smoothed)
        float targetTiltX = 0.0f;       ///< Target X tilt
        float cardSpacing = 35.0f;      ///< Vertical spacing between cards
        float cardScale = 0.8f;         ///< Image scale within a card
        int currentIndex = 0;           ///< Index of the front-most card
        bool autoRotate = true;         ///< Slowly rotate when idle
    };

    static Params params_;
    std::vector<ImageCard> cards_;
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);

    Event<Mouse> dragEvents_ = E<Mouse>(Mouse::DRAG, Mouse::LEFT);
    Event<Mouse> scrollEvents_ = E<Mouse>(Mouse::SCROLL);
    Event<Keyboard> keyEvents_ = E<Keyboard>(Keyboard::PRESS);

    /** @brief Collect image paths from a file or directory. */
    static std::vector<std::string> getImagePaths(const std::string& input) {
        std::vector<std::string> paths;
        std::error_code ec;

        if (std::filesystem::is_directory(input, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(input)) {
                if (!entry.is_regular_file(ec)) continue;

                auto ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" ||
                    ext == ".bmp" || ext == ".tiff" || ext == ".tif") {
                    paths.push_back(entry.path().string());
                }
            }
            std::sort(paths.begin(), paths.end());
        } else if (std::filesystem::exists(input, ec)) {
            paths.push_back(input);
        }

        return paths;
    }

public:
    explicit StackedImageGalleryPlan(const std::string& input) {
        auto paths = getImagePaths(input);
        cards_.reserve(paths.size());
        for (const auto& p : paths) {
            cards_.push_back({p, 0, 0, 0, false});
        }
        _shared(params_);
    }

    /** @brief Load images into NanoVG textures. */
    void setup() override {
        nvg([](std::vector<ImageCard>& cards) {
            using namespace cv::v4d::nvg;
            for (auto& card : cards) {
                if (card.path.empty()) continue;

                int handle = createImage(card.path.c_str(), NVG_IMAGE_NEAREST);
                if (handle > 0) {
                    imageSize(handle, &card.width, &card.height);
                    card.imageHandle = handle;
                    card.loaded = true;
                }
            }
        }, RW(cards_));
    }

    /** @brief Handle input events and update gallery state. */
    void infer() override {
        // Drag to rotate the stack
        branch([](const Mouse::List& drags) { return !drags.empty(); }, dragEvents_)
            ->plain([](Params& p, const Mouse::List& drags) {
                for (const auto& e : drags) {
                    p.targetRotationY += e->data().x * 0.008f;
                    p.targetTiltX += e->data().y * 0.005f;
                    p.targetTiltX = std::clamp(p.targetTiltX, -0.5f, 0.5f);
                }
            }, RWS(params_), dragEvents_)
        ->endBranch();

        // Scroll to switch the active card
        branch([](const Mouse::List& scrolls) { return !scrolls.empty(); }, scrollEvents_)
            ->plain([](Params& p, const Mouse::List& scrolls, const std::vector<ImageCard>& cards) {
                for (const auto& e : scrolls) {
                    if (e->data().y > 0) {
                        p.currentIndex = std::min(p.currentIndex + 1, (int)cards.size() - 1);
                    } else {
                        p.currentIndex = std::max(p.currentIndex - 1, 0);
                    }
                }
            }, RWS(params_), scrollEvents_, R(cards_))
        ->endBranch();

        // R key resets the view
        branch([](const Keyboard::List& keys) {
            return std::any_of(keys.begin(), keys.end(),
                [](const std::shared_ptr<Keyboard>& k) { return k->key() == Keyboard::R; });
        }, keyEvents_)
            ->plain([](Params& p) {
                p.targetRotationY = 0.0f;
                p.targetTiltX = 0.0f;
                p.currentIndex = 0;
            }, RWS(params_))
        ->endBranch();

        // Smooth interpolation and optional idle rotation
        plain([](Params& p) {
            if (p.autoRotate) {
                p.targetRotationY += 0.002f;
            }
            p.rotationY += (p.targetRotationY - p.rotationY) * 0.1f;
            p.tiltX += (p.targetTiltX - p.tiltX) * 0.1f;
        }, RWS(params_));

        // Render the stacked card gallery
        nvg([](const Size& sz, const Params& params, const std::vector<ImageCard>& cards) {
            using namespace cv::v4d::nvg;
            clearScreen(Scalar(35, 35, 35, 255));

            if (cards.empty() || cards.front().path.empty()) {
                fontSize(24);
                fontFace("sans");
                fillColor(Scalar(200, 200, 200, 255));
                textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                text(sz.width / 2.0f, sz.height / 2.0f, "No images loaded", nullptr);
                return;
            }

            const int idx = std::clamp(params.currentIndex, 0, (int)cards.size() - 1);
            const float cx = sz.width * 0.5f;
            const float cy = sz.height * 0.5f;

            save();
            translate(cx, cy);

            // 2.5D rotation and tilt
            rotate(params.rotationY);
            const float scaleX = 1.0f + params.tiltX * 0.3f;
            const float scaleY = 1.0f - params.tiltX * 0.3f;
            scale(scaleX, scaleY);

            // Draw cards back-to-front
            const int stackDepth = std::min(7, (int)cards.size());
            for (int i = stackDepth - 1; i >= 0; --i) {
                int cardIdx = idx - i;
                if (cardIdx < 0) cardIdx = 0;
                if (cardIdx >= (int)cards.size()) continue;

                const ImageCard& card = cards[cardIdx];
                if (!card.loaded) continue;

                const float depth = stackDepth > 1 ? (float)i / (stackDepth - 1) : 0.0f;
                const float yOffset = depth * params.cardSpacing;
                const float cardScale = 1.0f - depth * 0.04f;
                const float alpha = 1.0f - depth * 0.2f;

                save();
                translate(0, yOffset);
                scale(cardScale, cardScale);

                const float sw = card.width * params.cardScale;
                const float sh = card.height * params.cardScale;

                // Drop shadow
                beginPath();
                roundedRect(-sw / 2.0f + 5.0f, -sh / 2.0f + 5.0f, sw, sh, 12.0f);
                fillColor(Scalar(0, 0, 0, 50));
                fill();

                // White card background
                beginPath();
                roundedRect(-sw / 2.0f, -sh / 2.0f, sw, sh, 12.0f);
                fillColor(Scalar(255, 255, 255, 255));
                fill();

                // Image
                if (card.imageHandle != 0) {
                    Paint paint = imagePattern(-sw / 2.0f, -sh / 2.0f, sw, sh, 0.0f, card.imageHandle, alpha);
                    beginPath();
                    roundedRect(-sw / 2.0f, -sh / 2.0f, sw, sh, 12.0f);
                    fillPaint(paint);
                    fill();
                }

                restore();
            }

            restore();

            // HUD
            fontSize(18);
            fontFace("sans");
            fillColor(Scalar(255, 255, 255, 180));
            textAlign(NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            char buf[256];
            snprintf(buf, sizeof(buf),
                "%d / %d  |  Drag to rotate  |  Scroll to switch  |  R to reset",
                idx + 1, (int)cards.size());
            text(16, 16, buf, buf + strlen(buf));
        }, size_, R(params_), R(cards_));
    }

    /** @brief ImGui controls panel. */
    void gui() override {
        imgui([](Params& p) {
            using namespace ImGui;
            Begin("Gallery Controls");
            SliderFloat("Spacing", &p.cardSpacing, 10.0f, 80.0f);
            SliderFloat("Scale", &p.cardScale, 0.3f, 1.0f);
            Checkbox("Auto Rotate", &p.autoRotate);
            if (Button("Reset View")) {
                p.targetRotationY = 0.0f;
                p.targetTiltX = 0.0f;
                p.currentIndex = 0;
            }
            End();
        }, RWS(params_));
    }

    /** @brief Release NanoVG image resources. */
    void teardown() override {
        nvg([](std::vector<ImageCard>& cards) {
            using namespace cv::v4d::nvg;
            for (auto& card : cards) {
                if (card.imageHandle > 0) {
                    deleteImage(card.imageHandle);
                    card.imageHandle = 0;
                }
                card.loaded = false;
            }
        }, RW(cards_));
    }
};

StackedImageGalleryPlan::Params StackedImageGalleryPlan::params_;

int main(int argc, char** argv) {
    const std::string input = argc > 1 ? argv[1] : ".";

    cv::Rect viewport(0, 0, 1280, 960);
    cv::Ptr<V4D> runtime = V4D::init(viewport, "2.5D Stacked Image Gallery",
                                      AllocateFlags::NANOVG | AllocateFlags::IMGUI,
                                      ConfigFlags::DISPLAY_MODE);

    V4DPlan::run<StackedImageGalleryPlan>(2, input);
    return 0;
}
