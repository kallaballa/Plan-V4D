// Image Carousel — a V4D sample
//
// Usage:
//   example_v4d_image_carousel <image-or-directory> [<image-or-directory> ...]
//
// Loads images from the given paths (files and directories) and displays them
// in an animated carousel with glossy cards, reflections, smooth transitions,
// and a minimal ImGui HUD.
//
// Controls:
//   Left / Right arrows        previous / next image
//   Space                      toggle auto-play (5 s interval)
//   Home / End                 jump to first / last
//   Mouse scroll               previous / next image
//   Click left / right side    previous / next image

#include <opencv2/v4d/v4d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace cv;
using namespace cv::v4d;
using namespace cv::v4d::event;

static constexpr const char* kImageExts[] = {
    ".png", ".jpg", ".jpeg", ".bmp", ".tiff", ".tif",
    ".gif", ".webp", ".pnm", ".ppm", ".pgm", ".pbm"
};

static bool isImageExt(const std::string& ext) {
    std::string lo = ext;
    std::transform(lo.begin(), lo.end(), lo.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* e : kImageExts)
        if (lo == e) return true;
    return false;
}

struct CarouselState {
    int   current_          = 0;
    float animOffset_       = 0.0f;   // fractional offset for smooth animation
    bool  autoPlay_         = false;
    float autoTimer_        = 0.0f;
    float prevTime_         = 0.0f;
    float autoPlayInterval_ = 1.0f;   // seconds between auto-advance
    float animSpeed_        = 8.0f;   // exponential interpolation speed
};

class ImageCarousel : public V4DPlan {
    struct Card {
        std::string path_;
        cv::UMat    rgba_;
        int         w_ = 0;
        int         h_ = 0;
        int         handle_ = -1;
        std::string name_;
    };
    std::vector<Card> cards_;
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);

    static constexpr float kCardHeightFrac  = 0.52f;
    static constexpr float kMaxCardWidthFrac = 0.50f;
    static constexpr float kSideScale       = 0.55f;
    static constexpr float kFarScale        = 0.30f;
    static constexpr int   kRadius          = 18;

    Event<Keyboard> pressKey_ = E<Keyboard>(Keyboard::PRESS);
    Event<Mouse>    scroll_   = E<Mouse>(Mouse::SCROLL);
    Event<Mouse>    click_    = E<Mouse>(Mouse::PRESS, Mouse::LEFT);

public:
    ImageCarousel() {
      _shared(state_);
    }

    explicit ImageCarousel(const std::vector<std::string>& paths) {
	_shared(state_);
        for (const auto& p : paths) {
            std::error_code ec;
            std::filesystem::path fsPath(p);
            if (std::filesystem::is_directory(fsPath, ec)) {
                for (const auto& entry : std::filesystem::directory_iterator(fsPath, ec)) {
                    if (ec) break;
                    if (entry.is_regular_file(ec) && isImageExt(entry.path().extension().string()))
                        loadFile(entry.path().string());
                }
            } else if (std::filesystem::is_regular_file(fsPath, ec)) {
                loadFile(p);
            }
        }
        if (cards_.empty()) {
            CV_LOG_WARNING(nullptr, "No images could be loaded from the given "
                                    "paths; pass image files or directories.");
        }
        std::sort(cards_.begin(), cards_.end(),
                  [](const Card& a, const Card& b) { return a.path_ < b.path_; });
    }

    void setup() override {
        set(GlobalState::Keys::TIME_TRACKER, V(false));
        set(GlobalState::Keys::SHOW_FRAME_TIME, V(false));

        // Upload the RGBA pixels of every card into a NanoVG image. This runs
        // once, on the worker thread, before the frame loop.
        nvg([](std::vector<Card>& cards) {
            using namespace cv::v4d::nvg;
            for (auto& c : cards) {
                c.handle_ = createImageRGBA(c.w_, c.h_, NVG_IMAGE_NEAREST,
                                            c.rgba_.getMat(cv::ACCESS_READ).data);
                CV_Assert(c.handle_ > 0);
            }
        }, RW(cards_));
    }

    void infer() override {
        set(V4D::Keys::CLEAR_COLOR, V(cv::Scalar(14, 14, 20, 255)));
        clear();

        // CarouselState is shared between the worker (here) and the ImGui
        // node (gui()).  DISPLAY_MODE serializes the two threads, so there
        // is no contention, but both must operate on the same object.
        nvg([](const std::vector<Card>& cards, const cv::Size& sz,
               const Keyboard::List& pressKeyEvts,
               const Mouse::List& scrollEvts,
               const Mouse::List& clickEvts,
               CarouselState& st) {

            using namespace cv::v4d::nvg;

            float now = static_cast<float>(cv::getTickCount() / cv::getTickFrequency());
            float dt  = st.prevTime_ > 0.0f ? (now - st.prevTime_) : 1.0f / 60.0f;
            dt = std::clamp(dt, 0.01f, 1.0f);
            st.prevTime_ = now;

            int N = static_cast<int>(cards.size());

            // --- Draw background gradient ---
            {
                float cx = sz.width * 0.5f;
                float cy = sz.height * 0.55f;
                float radius = std::max(sz.width, sz.height) * 0.75f;
                Paint bgGrad = radialGradient(cx, cy, 0.0f, radius,
                    cv::Scalar(45, 45, 65, 255), cv::Scalar(14, 14, 20, 255));
                beginPath();
                rect(0, 0, sz.width, sz.height);
                fillPaint(bgGrad);
                fill();
            }

            if (N == 0) {
                fontSize(16.0f);
                fontFace("sans");
                fillColor(cv::Scalar(170, 170, 185, 220));
                textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                const char* msg =
                    "No images found - pass image files or directories on the command line";
                text(sz.width * 0.5f, sz.height * 0.5f, msg, msg + std::strlen(msg));
                return;
            }

            // --- Input handling ---
            for (auto& e : pressKeyEvts) {
                switch (e->key()) {
                    case Keyboard::LEFT:   st.current_ = (st.current_ - 1 + N) % N; break;
                    case Keyboard::RIGHT:  st.current_ = (st.current_ + 1) % N; break;
                    case Keyboard::HOME:   st.current_ = 0; break;
                    case Keyboard::END:    st.current_ = N - 1; break;
                    case Keyboard::SPACE:  st.autoPlay_ = !st.autoPlay_; st.autoTimer_ = 0.0f; break;
                    default: break;
                }
            }
            for (auto& e : scrollEvts) {
                if (e->data().y > 0) st.current_ = (st.current_ - 1 + N) % N;
                else                 st.current_ = (st.current_ + 1) % N;
            }

            // Click to navigate: left third -> prev, right third -> next
            for (auto& ce : clickEvts) {
                float x = ce->position().x;
                if (x < sz.width * 0.33f)      st.current_ = (st.current_ - 1 + N) % N;
                else if (x > sz.width * 0.67f) st.current_ = (st.current_ + 1) % N;
            }

            // Auto-play
            if (st.autoPlay_) {
                st.autoTimer_ += dt;
                if (st.autoTimer_ >= st.autoPlayInterval_) {
                    st.autoTimer_ = 0.0f;
                    st.current_ = (st.current_ + 1) % N;
                }
            }

            // Smooth animation
            float target = static_cast<float>(st.current_);
            float diff = target - st.animOffset_;
            // wrap for shortest path
            if (diff > N / 2.0f)  diff -= N;
            if (diff < -N / 2.0f) diff += N;
            float t = 1.0f - std::exp(-st.animSpeed_ * dt);
            st.animOffset_ += diff * t;

            float cx = sz.width * 0.5f;
            float cy = sz.height * 0.50f;
            float cardH = sz.height * kCardHeightFrac;

            // --- Draw cards (back to front) ---
            struct CardDraw {
                int   idx;
                float rel;   // wrapped distance from the center
                float x, y;
                float s;
                float ang;
            };
            std::vector<CardDraw> visible;

            for (int d = -3; d <= 3; ++d) {
                int idx = static_cast<int>(std::round(st.animOffset_)) + d;
                // wrap
                idx = ((idx % N) + N) % N;
                float rel = static_cast<float>(idx) - st.animOffset_;
                // wrap rel
                while (rel > N / 2.0f)  rel -= N;
                while (rel < -N / 2.0f) rel += N;

                if (std::abs(rel) > 3.5f) continue;

                float ang = rel * (CV_PI * 0.33f);
                float depth = std::cos(ang);

                float scale;
                if (std::abs(rel) < 0.5f)
                    scale = 1.0f;
                else if (std::abs(rel) < 1.5f)
                    scale = kSideScale;
                else
                    scale = kFarScale;

                float xOff = std::sin(ang) * sz.width * 0.30f;
                float yOff = (1.0f - depth) * 30.0f;

                // A card can be reached from several d's when N is small
                // (N < 7); keep only the closest replica to avoid ghosting.
                bool replaced = false;
                for (auto& v : visible) {
                    if (v.idx == idx) {
                        if (std::abs(rel) < std::abs(v.rel)) {
                            v = {idx, rel, cx + xOff, cy + yOff, scale, ang};
                        }
                        replaced = true;
                        break;
                    }
                }
                if (!replaced)
                    visible.push_back({idx, rel, cx + xOff, cy + yOff, scale, ang});
            }

            std::sort(visible.begin(), visible.end(),
                      [](const CardDraw& a, const CardDraw& b) {
                          float da = std::cos(a.ang);
                          float db = std::cos(b.ang);
                          return da < db;
                      });

            for (const auto& cd : visible) {
                const Card& card = cards[cd.idx];

                float aspect = static_cast<float>(card.w_) /
                               std::max(card.h_, 1);
                float w = cardH * aspect;
                float h = cardH;
                // Keep extremely wide images on screen.
                if (w > sz.width * kMaxCardWidthFrac) {
                    w = sz.width * kMaxCardWidthFrac;
                    h = w / aspect;
                }
                float sw = w * cd.s;
                float sh = h * cd.s;
                float drawX = cd.x - sw * 0.5f;
                float drawY = cd.y - sh * 0.5f;

                bool isCenter = (cd.idx == st.current_ && cd.s > 0.9f);

                // --- Reflection (only for close cards) ---
                if (cd.s > 0.4f) {
                    save();
                    float reflH = sh * 0.28f;
                    float reflY = drawY + sh + 4.0f;

                    // Clip reflection region
                    intersectScissor(drawX, reflY, sw, reflH);

                    // Draw flipped image
                    save();
                    translate(drawX, reflY + reflH * 2.0f);
                    scale(sw / w, -sh / h);
                    beginPath();
                    rect(0, 0, w, h);
                    Paint reflImgPat = imagePattern(0, 0, w, h, 0.0f, card.handle_, 0.22f);
                    fillPaint(reflImgPat);
                    fill();
                    restore();

                    // Fade-out gradient over reflection
                    Paint fadeGrad = linearGradient(0, reflY, 0, reflY + reflH,
                        cv::Scalar(14, 14, 20, 0), cv::Scalar(14, 14, 20, 220));
                    beginPath();
                    rect(drawX, reflY, sw, reflH);
                    fillPaint(fadeGrad);
                    fill();

                    resetScissor();
                    restore();
                }

                // --- Shadow ---
                {
                    Paint shadow = boxGradient(drawX + 2, drawY + 4, sw, sh, kRadius + 2, 20,
                        cv::Scalar(0, 0, 0, 100), cv::Scalar(0, 0, 0, 0));
                    beginPath();
                    roundedRect(drawX + 2, drawY + 4, sw, sh, kRadius + 2);
                    fillPaint(shadow);
                    fill();
                }

                // --- Card image ---
                save();
                if (std::abs(cd.ang) > 0.01f) {
                    translate(drawX + sw * 0.5f, drawY + sh * 0.5f);
                    float skewAmt = cd.ang * 0.15f;
                    skewY(skewAmt);
                    translate(-sw * 0.5f, -sh * 0.5f);
                } else {
                    translate(drawX, drawY);
                }

                // Card background
                beginPath();
                roundedRect(0, 0, sw, sh, kRadius);
                fillColor(cv::Scalar(25, 25, 35, 255));
                fill();

                // Image
                {
                    Paint imgPat = imagePattern(0, 0, sw, sh, 0.0f, card.handle_, 1.0f);
                    beginPath();
                    roundedRect(0, 0, sw, sh, kRadius);
                    fillPaint(imgPat);
                    fill();
                }

                // --- Glossy overlay ---
                {
                    Paint gloss = linearGradient(0, 0, 0, sh * 0.5f,
                        cv::Scalar(255, 255, 255, 45), cv::Scalar(255, 255, 255, 0));
                    beginPath();
                    roundedRect(1, 1, sw - 2, sh * 0.5f, kRadius);
                    fillPaint(gloss);
                    fill();
                }

                // --- Border ---
                {
                    if (isCenter) {
                        strokeColor(cv::Scalar(255, 255, 255, 140));
                        strokeWidth(2.0f);
                    } else {
                        strokeColor(cv::Scalar(255, 255, 255, 40));
                        strokeWidth(1.0f);
                    }
                    beginPath();
                    roundedRect(0.5f, 0.5f, sw - 1, sh - 1, kRadius);
                    stroke();
                }

                restore();

                // --- Label (center card only) ---
                if (isCenter) {
                    float labelY = drawY + sh + 22.0f;
                    fontSize(16.0f);
                    fontFace("sans");
                    fillColor(cv::Scalar(200, 200, 210, 255));
                    textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
                    const std::string& label = card.name_;
                    text(cx, labelY, label.c_str(), label.c_str() + label.size());
                }
            }

            // --- Progress bar ---
            {
                float barW = std::min(static_cast<float>(sz.width) * 0.35f, 300.0f);
                float barH = 4.0f;
                float barX = cx - barW * 0.5f;
                float barY = static_cast<float>(sz.height) - 38.0f;
                float progress = (N > 1) ? static_cast<float>(st.current_) / (N - 1) : 0.0f;

                // Track
                beginPath();
                roundedRect(barX, barY, barW, barH, 2.0f);
                fillColor(cv::Scalar(80, 80, 100, 120));
                fill();

                // Fill
                float fillW = barW * progress;
                if (fillW > 1.0f) {
                    beginPath();
                    roundedRect(barX, barY, fillW, barH, 2.0f);
                    fillColor(cv::Scalar(160, 180, 255, 220));
                    fill();
                }

                // Counter text
                fontSize(13.0f);
                fontFace("sans");
                fillColor(cv::Scalar(160, 160, 180, 200));
                textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
                char counter[64];
                std::snprintf(counter, sizeof(counter), "%d / %d", st.current_ + 1, N);
                text(cx, barY + barH + 6.0f, counter, counter + std::strlen(counter));
            }

            // --- Auto-play indicator ---
            if (st.autoPlay_) {
                float dotR = 4.0f;
                float dotX = cx + std::min(static_cast<float>(sz.width) * 0.35f, 300.0f) * 0.5f + 16.0f;
                float dotY = static_cast<float>(sz.height) - 36.0f;
                beginPath();
                circle(dotX, dotY, dotR);
                fillColor(cv::Scalar(120, 200, 120, 200));
                fill();
            }

            // --- Controls hint ---
            {
                fontSize(11.0f);
                fontFace("sans");
                fillColor(cv::Scalar(120, 120, 140, 120));
                textAlign(NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
                const char* hint = "Panel or arrows: navigate | Space: auto-play | Scroll/click: navigate";
                text(12.0f, static_cast<float>(sz.height) - 12.0f, hint,
                     hint + std::strlen(hint));
            }

        }, R(cards_), size_,
           pressKey_, scroll_, click_,
           RWS(state_));
    }

    void gui() override {
        imgui([](const std::vector<Card>& cards, CarouselState& st, const cv::Size& sz) {
            using namespace ImGui;
            SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Once);
            SetNextWindowBgAlpha(0.55f);
            Begin("Image Carousel", nullptr,
                  ImGuiWindowFlags_NoResize |
                  ImGuiWindowFlags_AlwaysAutoResize |
                  ImGuiWindowFlags_NoTitleBar);

            int N = static_cast<int>(cards.size());
            bool hasCards = N > 0;

            if (hasCards) {
                const auto& c = cards[st.current_];
                Text("%s", c.name_.c_str());
                Text("%d x %d", c.w_, c.h_);
            } else {
                Text("No images loaded");
            }

            Separator();

            // Navigation
            if (Button("First")) {
                if (hasCards) st.current_ = 0;
            }
            SameLine();
            if (Button("Prev")) {
                if (hasCards) st.current_ = (st.current_ - 1 + N) % N;
            }
            SameLine();
            if (Button("Next")) {
                if (hasCards) st.current_ = (st.current_ + 1) % N;
            }
            SameLine();
            if (Button("Last")) {
                if (hasCards) st.current_ = N - 1;
            }

            if (hasCards && N > 1) {
                SliderInt("Index", &st.current_, 0, N - 1);
            }

            Separator();

            // Auto-play
            Checkbox("Auto-play", &st.autoPlay_);
            if (st.autoPlay_) {
                SameLine();
                TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "(active)");
            }
            SliderFloat("Interval (s)", &st.autoPlayInterval_, 1.0f, 30.0f, "%.1f s");

            Separator();

            // Animation
            SliderFloat("Anim speed", &st.animSpeed_, 1.0f, 20.0f, "%.1f");

            Separator();

            Text("Keyboard / mouse:");
            BulletText("Arrows: navigate");
            BulletText("Space: auto-play");
            BulletText("Home/End: first/last");
            BulletText("Scroll / click: navigate");

            End();
        }, R(cards_), RWS(state_), size_);
    }

    void teardown() override {
        nvg([](std::vector<Card>& cards) {
            using namespace cv::v4d::nvg;
            for (auto& c : cards) {
                if (c.handle_ > 0) {
                    deleteImage(c.handle_);
                    c.handle_ = -1;
                }
            }
        }, RW(cards_));
    }

private:
    void loadFile(const std::string& path) {
        cv::Mat img = cv::imread(path, cv::IMREAD_UNCHANGED);
        if (img.empty()) return;
        // NanoVG images are 8-bit; down-convert anything else up front.
        if (img.depth() != CV_8U)
            img.convertTo(img, CV_8U);

        cv::Mat rgba;
        switch (img.channels()) {
            case 1:
                cv::cvtColor(img, rgba, cv::COLOR_GRAY2RGBA);
                break;
            case 3:
                cv::cvtColor(img, rgba, cv::COLOR_BGR2RGBA);
                break;
            case 4:
                cv::cvtColor(img, rgba, cv::COLOR_BGRA2RGBA);
                break;
            default:
                CV_LOG_WARNING(nullptr, "Unsupported image channel count for '" << path << "'");
                return;
        }

        Card c;
        c.path_ = path;
        c.name_ = std::filesystem::path(path).filename().string();
        c.w_ = rgba.cols;
        c.h_ = rgba.rows;
        rgba.copyTo(c.rgba_);
        cards_.push_back(std::move(c));
    }

    static CarouselState state_;
};

CarouselState ImageCarousel::state_;

int main(int argc, char** argv) {
    cv::samples::addSamplesDataSearchPath(V4D_ASSETS_PATH);

    std::vector<std::string> paths;
    for (int i = 1; i < argc; ++i)
        paths.emplace_back(argv[i]);

    if (paths.empty()) {
        // Default to the sample image, mirroring the other sample programs.
        try {
            paths.push_back(cv::samples::findFile("lena.png"));
        } catch (...) {
            CV_LOG_WARNING(nullptr, "Could not find sample image. "
                                    "Pass image files or directories as arguments.");
            paths.push_back("");
        }
    }

    cv::Rect viewport(0, 0, 1024, 768);
    cv::Ptr<V4D> runtime = V4D::init(viewport, "Image Carousel",
                                     AllocateFlags::NANOVG | AllocateFlags::IMGUI,
                                     ConfigFlags::DISPLAY_MODE);

    V4DPlan::run<ImageCarousel>(0, std::move(paths));
    return 0;
}
