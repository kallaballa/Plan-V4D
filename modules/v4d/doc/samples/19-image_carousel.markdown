# Tutorial: An Interactive Image Carousel

> **← [Parallel Rendering with Multiple OpenGL Contexts](18-many-cubes.markdown) | [Next: Reimplementing OpenCV's imshow](20-imshow_reimplementation.markdown) →**

This tutorial builds a self-contained image gallery application: it loads images from files and directories given on the command line and displays them as an animated carousel with glossy cards, reflections, smooth transitions, and a minimal ImGui HUD for navigation and tweaking.

Besides polishing everything you have learned so far (`nvg` rendering, `Event` input, `imgui`, shared state), this demo introduces two practical patterns:

- Loading and preparing arbitrary images for NanoVG at runtime (`createImageRGBA`).
- Sharing a single mutable state object between the rendering pipeline and the GUI thread with `_shared` / `RWS` under `ConfigFlags::DISPLAY_MODE`.

## The Code

The source code is in [modules/v4d/samples/image_carousel.cpp](../../samples/image_carousel.cpp). Run it with one or more image files or directories:

```
example_v4d_image_carousel <image-or-directory> [<image-or-directory> ...]
```

Controls:

- **Left / Right arrows** — previous / next image
- **Space** — toggle auto-play (advances every second by default, adjustable via the HUD)
- **Home / End** — jump to first / last
- **Mouse scroll / click left-right side** — previous / next image

```cpp
class ImageCarousel : public V4DPlan {
    struct Card {
        std::string path_;
        cv::UMat    rgba_;
        int         w_ = 0, h_ = 0;
        int         handle_ = -1;
        std::string name_;
    };
    std::vector<Card> cards_;
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);

    Event<Keyboard> pressKey_ = E<Keyboard>(Keyboard::PRESS);
    Event<Mouse>    scroll_   = E<Mouse>(Mouse::SCROLL);
    Event<Mouse>    click_    = E<Mouse>(Mouse::PRESS, Mouse::LEFT);

public:
    ImageCarousel() {
        _shared(state_);
    }

    void setup() override {
        set(GlobalState::Keys::TIME_TRACKER, V(false));
        set(GlobalState::Keys::SHOW_FRAME_TIME, V(false));

        // Upload the RGBA pixels of every card into a NanoVG image.
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

        nvg([](const std::vector<Card>& cards, const cv::Size& sz,
               const Keyboard::List& pressKeyEvts,
               const Mouse::List& scrollEvts,
               const Mouse::List& clickEvts,
               CarouselState& st) {
            // ... input, auto-play, smooth animation, and the
            //     back-to-front card rendering (see below) ...
        }, R(cards_), size_, pressKey_, scroll_, click_, RWS(state_));
    }

    void gui() override {
        imgui([](const std::vector<Card>& cards, CarouselState& st, const cv::Size& sz) {
            // Nav buttons, slider, auto-play checkbox, animation speed ...
        }, R(cards_), RWS(state_), size_);
    }

    void teardown() override {
        nvg([](std::vector<Card>& cards) {
            using namespace cv::v4d::nvg;
            for (auto& c : cards) {
                if (c.handle_ > 0) { deleteImage(c.handle_); c.handle_ = -1; }
            }
        }, RW(cards_));
    }
};
```

## Code Breakdown

### 1. Loading Images from Files and Directories

The constructor takes a list of paths and expands every argument that is a directory recursively (well, one level) into the image files it contains. A small `isImageExt` helper filters by extension, and all cards are sorted by path so the gallery order is deterministic.

```cpp
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
    std::sort(cards_.begin(), cards_.end(),
              [](const Card& a, const Card& b) { return a.path_ < b.path_; });
}
```

If no path yields a readable image, the plan still runs and simply draws a "No images found" message instead of a carousel. This makes the sample robust to bad arguments.

### 2. Preparing Pixels for NanoVG: `loadFile`

NanoVG images are 8-bit RGBA textures, so `loadFile` normalizes whatever `imread` returns:

- Any non-8-bit depth is down-converted with `convertTo`.
- 1-, 3- and 4-channel images are converted to RGBA with the matching `cvtColor` code:
  - `COLOR_GRAY2RGBA`, `COLOR_BGR2RGBA`, or `COLOR_BGRA2RGBA`.

The four-channel→RGBA case gives the carousel true alpha support: you can feed it PNGs with transparency and they keep their transparency.

```cpp
void loadFile(const std::string& path) {
    cv::Mat img = cv::imread(path, cv::IMREAD_UNCHANGED);
    if (img.empty()) return;
    if (img.depth() != CV_8U)
        img.convertTo(img, CV_8U);

    cv::Mat rgba;
    switch (img.channels()) {
        case 1:  cv::cvtColor(img, rgba, cv::COLOR_GRAY2RGBA);  break;
        case 3:  cv::cvtColor(img, rgba, cv::COLOR_BGR2RGBA);   break;
        case 4:  cv::cvtColor(img, rgba, cv::COLOR_BGRA2RGBA);  break;
        default: return;
    }

    Card c;
    c.path_ = path;
    c.name_ = std::filesystem::path(path).filename().string();
    c.w_ = rgba.cols;
    c.h_ = rgba.rows;
    rgba.copyTo(c.rgba_);
    cards_.push_back(std::move(c));
}
```

### 3. Uploading the Cards: `setup()`

Uploading happens once, in `setup()`, inside an `nvg` context. Each card's RGBA `UMat` becomes a NanoVG texture handle via `createImageRGBA`. The pixel data lives in the `UMat`; `getMat(cv::ACCESS_READ)` hands NanoVG a raw pointer to it. The handles are stored inside the same `cards_` vector that `infer()` reads, which is safe because setup runs on the worker before the frame loop.

### 4. Shared State Across Threads

`CarouselState` is a `static` member:

```cpp
struct CarouselState {
    int   current_          = 0;
    float animOffset_       = 0.0f;
    bool  autoPlay_         = false;
    float autoTimer_        = 0.0f;
    float prevTime_         = 0.0f;
    float autoPlayInterval_ = 1.0f;
    float animSpeed_        = 8.0f;
};
static CarouselState state_;
```

It is registered as shared in the constructor (`_shared(state_)`) and passed to both contexts:

- The `nvg` node in `infer()` takes `RWS(state_)` — read-write shared — because it both reads and updates the animation state.
- The `imgui` node in `gui()` takes `RWS(state_)` as well, since buttons and sliders mutate it.

The single worker and the display/GUI thread exchange this state every frame. Because the sample initializes the runtime with `ConfigFlags::DISPLAY_MODE`, the two threads are serialized, so no lock is needed — but both nodes must operate on the *same* object, which is exactly what `_shared` guarantees.

### 5. Input Handling with Event Lists

All interaction is handled inside the single `nvg` node, using the event *lists* captured since the last frame:

```cpp
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
for (auto& ce : clickEvts) {
    float x = ce->position().x;
    if (x < sz.width * 0.33f)      st.current_ = (st.current_ - 1 + N) % N;
    else if (x > sz.width * 0.67f) st.current_ = (st.current_ + 1) % N;
}
```

Notice the recurring `(index + N) % N` idiom — it wraps the index around the circular list. Clicking only reacts on the outer thirds of the window, so the middle of the screen remains a "scrub free" area.

### 6. Auto-Play and Smooth Animation

Auto-play is a simple accumulator: each frame `dt` is added to `autoTimer_`; when it crosses the (user-adjustable) interval, the current index advances. `dt` is measured from `cv::getTickCount()` and clamped so a paused window does not over-advance.

The interesting part is the animation. The raw integer index would jump instantly, so the demo keeps a fractional `animOffset_` and eases it toward the target using exponential interpolation:

```cpp
float target = static_cast<float>(st.current_);
float diff = target - st.animOffset_;
if (diff > N / 2.0f)  diff -= N;   // wrap for the shortest path
if (diff < -N / 2.0f) diff += N;
float t = 1.0f - std::exp(-st.animSpeed_ * dt);
st.animOffset_ += diff * t;
```

`t` is frame-rate independent: `1 - exp(-speed * dt)` approaches 1 faster for larger `dt`, so the motion looks identical regardless of monitor refresh rate. The `±N/2` wrap forces the carousel to always spin the short way around the circle.

### 7. Layout and Perspective

Each card gets a "relative index" `rel = idx - animOffset_` (wrapped to `[-N/2, N/2]`). From that, the demo computes a pseudo-3D pose:

```cpp
float ang = rel * (CV_PI * 0.33f);
float depth = std::cos(ang);

float scale;
if (std::abs(rel) < 0.5f) scale = 1.0f;        // center card: full size
else if (std::abs(rel) < 1.5f) scale = kSideScale;  // neighbors: 55%
else scale = kFarScale;                        // far cards: 30%

float xOff = std::sin(ang) * sz.width * 0.30f; // horizontal parade
float yOff = (1.0f - depth) * 30.0f;           // slight rise when "forward"
```

The center card is drawn full size, immediate neighbors smaller and offset sideways, and the far cards smaller still — with a small horizontal skew (`skewY`) applied per card to fake rotation. Cards are collected in a `visible` list, sorted back-to-front by `cos(ang)` (deepest first), and drawn. When `N` is small (< 7), a card can be reached from several replica offsets, so the code keeps only the closest replica to avoid ghosting.

### 8. Card Drawing Details

Each card is drawn with several stacked NanoVG layers:

1. **Reflection** (close cards only): the image is drawn flipped below the card (`scale(sw / w, -sh / h)`) inside a scissor region, then a linear gradient fades it out toward the bottom.
2. **Shadow**: a `boxGradient` behind the card, offset by a few pixels.
3. **Card image**: a rounded rectangle filled with the image `imagePattern`.
4. **Gloss**: a white-to-transparent linear gradient across the top half simulates a shiny surface.
5. **Border**: a bright stroke for the centered card, a faint one otherwise — a subtle "this is the one you control" affordance.
6. **Label**: the centered card's filename is drawn beneath it.

Scissoring via `intersectScissor` / `resetScissor` is what keeps the reflection from bleeding outside its box.

### 9. The ImGui HUD

`gui()` builds a small overlay window that reads and writes the same `CarouselState`:

- `First` / `Prev` / `Next` / `Last` buttons and an `Index` slider.
- An `Auto-play` checkbox with a live-adjustable interval slider.
- An `Anim speed` slider that tunes the exponential easing constant.

Because the state is shared and the runtime runs in `DISPLAY_MODE`, moving the slider in the GUI shows up in the animation on the very next frame.

### 10. Cleanup

`teardown()` frees every NanoVG texture handle with `deleteImage`, mirroring the `createImageRGBA` calls in `setup()`. Forgetting this would leak GPU memory on context re-creation.

## Summary

- Arbitrary images can be loaded, normalized to 8-bit RGBA, and uploaded to NanoVG at runtime via `createImageRGBA`.
- A single mutable struct shared with `_shared` and accessed with `RWS` lets the ImGui HUD and the rendering pipeline cooperate on the same state.
- Event lists (`Keyboard::List`, `Mouse::List`) centralize all input handling in one graph node.
- Per-card transforms, scissored reflections, gradients and sorting back-to-front produce a convincing 3D "parade" effect in pure 2D vector graphics.

Next, we will look at the most ambitious sample of all: a full reimplementation of OpenCV's `imshow` — including zooming, panning and per-pixel deep-zoom inspection.