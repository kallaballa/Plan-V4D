# Tutorial: Reimplementing OpenCV's imshow

> **← [An Interactive Image Carousel](19-image_carousel.markdown)**

This tutorial dissects the most feature-complete sample in the series: a reimplementation of OpenCV's `imshow` window — the Qt flavor, to be precise — built on Plan-V4D's Input Events, NanoVG and ImGui layers. It faithfully reproduces the behaviors power users expect from the original: 1:1 display, zoom around the cursor, panning, a reset-to-fit view, and the famous "deep zoom" mode where `imshow` overlays the RGB value of every pixel in each cell of a grid.

The sample touches on nearly every concept in this series at once:

- A single large `State` struct shared between the worker pipeline and the ImGui thread.
- Loading, converting and re-uploading an image to NanoVG (including on the fly, via a reload branch).
- Dense event handling: scroll, drag, press, release, move, hover enter/exit.
- Conditional sub-graphs with `branch` / `endBranch`.
- A serious ImGui application: menu bar, keyboard shortcuts, file browser, save dialogs.
- A status bar with live pixel readouts.

## The Code

The source code is in [modules/v4d/samples/imshow_reimplementation.cpp](../../samples/imshow_reimplementation.cpp). Run it with an image path:

```
example_v4d_imshow_reimplementation <image>
```

If no argument is given it falls back to the sample `lena.png`.

```cpp
class ImshowReimplementation : public V4DPlan {
    UMat image_;   // original, as loaded
    UMat bgra_;    // BGRA copy for the status bar / deep-zoom readout
    UMat rgba_;    // RGBA copy for the NanoVG upload
    string filename_;

    struct State {
        // Image
        int   imageHandle_  = -1;
        int   imageWidth_   = 0;
        int   imageHeight_  = 0;
        int   channels_     = 0;
        // View transform
        float zoom_         = 1.0f;
        cv::Point2f pan_    = {0.0f, 0.0f};
        bool  isDragging_   = false;
        // Cursor tracking
        cv::Point2f mousePos_ = {-1.0f, -1.0f};
        bool  mouseInside_  = false;
        // UI toggles & dialogs
        bool  showProperties_ = false;
        bool  showHelp_     = true;
        bool  showStatusBar_ = true;
        // ...
        static constexpr float kDeepZoomThreshold = 30.0f;
    };
    static State state_;

    Event<Mouse> scroll_      = E<Mouse>(Mouse::SCROLL);
    Event<Mouse> drag_        = E<Mouse>(Mouse::DRAG);
    Event<Mouse> pressLeft_   = E<Mouse>(Mouse::PRESS,   Mouse::LEFT);
    Event<Mouse> releaseLeft_ = E<Mouse>(Mouse::RELEASE, Mouse::LEFT);
    Event<Mouse> pressRight_  = E<Mouse>(Mouse::PRESS,   Mouse::RIGHT);
    Event<Mouse> pressMiddle_ = E<Mouse>(Mouse::PRESS,   Mouse::MIDDLE);
    Event<Mouse> move_        = E<Mouse>(Mouse::MOVE);
    Event<Mouse> hoverEnter_  = E<Mouse>(Mouse::HOVER_ENTER);
    Event<Mouse> hoverExit_   = E<Mouse>(Mouse::HOVER_EXIT);
};
```

## Code Breakdown

### 1. The Big Shared `State`

Everything the app needs to remember — the NanoVG image handle, the zoom/pan transform, dragging state, cursor position, whether each overlay is visible, and the file/save dialog buffers — lives in one static `State`. Like in the previous tutorials, the GUI thread and the worker thread both access it, and the runtime is created with `ConfigFlags::DISPLAY_MODE` so the two are serialized. Note that two members are declared `static constexpr` inside the struct (`kDeepZoomThreshold`, the image extension table) — a C++17 idiom to keep file-scope constants together with their struct.

### 2. Loading the Image: Constructor and `setup()`

The constructor validates the file eagerly: `imread` returns an empty `Mat` on failure, which triggers `CV_Error` with a helpful message. The successful image is copied into the `image_` `UMat`.

`setup()` then runs a small pre-processing chain:

```cpp
plain([](const UMat& src, UMat& bgra, UMat& rgba) {
    switch (src.channels()) {
        case 1: cvtColor(src, rgba, COLOR_GRAY2RGBA); break;
        case 3: cvtColor(src, rgba, COLOR_BGR2RGBA);  break;
        case 4: cvtColor(src, rgba, COLOR_BGRA2RGBA); break;
        default: CV_Error(Error::StsError, "Unsupported image format");
    }
    cvtColor(rgba, bgra, COLOR_RGBA2BGRA);
}, R(image_), RW(bgra_), RW(rgba_));
```

Two buffers are produced from the single original: an RGBA copy for the NanoVG upload (NanoVG wants `R,G,B,A` order), and a BGRA copy for the status-bar and deep-zoom pixel readouts (which use the OpenCV-native ordering). Keeping the original `image_` untouched is what allows "Save image as…" to write the *original* pixels even when the user has zoomed and panned.

After the color conversion, another `nvg` node creates the texture handle and initializes the view transform to a centered, 1:1 pan — exactly what Qt-based `imshow` shows on first open.

### 3. Reloading on the Fly: a `branch` Sub-Graph

`infer()` starts with a conditional sub-graph that is inactive most of the time:

```cpp
branch(RWS(state_.reloadRequested_))
    ->plain([this](UMat& image, UMat& bgra, UMat& rgba, State& state) {
        cv::Mat tmp = cv::imread(state.newFilename_, cv::IMREAD_UNCHANGED);
        if (!tmp.empty()) {
            if (tmp.cols != image.cols || tmp.rows != image.rows) {
                state.lastSaveOk_ = false;
                state.lastSaveMsg_ = "Cannot load image of different size (...)...";
            } else {
                tmp.copyTo(image);
                // ... re-convert to rgba_ and bgra_ ...
                filename_ = state.newFilename_;
            }
        }
        state.reloadRequested_ = false;
    }, RW(image_), RW(bgra_), RW(rgba_), RWS(state_))
    ->nvg([this](const UMat& rgba, const UMat& image, State& state) {
        // delete old handle, createImageRGBA the new pixels
    }, R(rgba_), R(image_), RWS(state_))
->endBranch();
```

Two important details:

- The branch predicate is `RWS(state_.reloadRequested_)` — a shared flag. When the file dialog asks to open a new file, it stores the path in `newFilename_`, sets `reloadRequested_ = true`, and the *next frame* executes this sub-graph once and clears the flag.
- The reload keeps the same image *dimensions*. The Zoom-to-Fit and pan transform assumed the old size; requiring the same dimensions keeps the view transform valid. Everything is also re-converted/re-uploaded, and the status-bar filename copy is updated to match. (The `[this]` captures are only used to reach the `filename_` member; the plan stays copyable because `State` itself is static and shared.)

### 4. Input Handling: Zoom, Pan, Reset, Deep Zoom

One `plain` node consumes all eight event lists and updates the transform. The scroll-wheel branch implements the classic "zoom around the cursor" math:

```cpp
for (auto se : scrollEvents) {
    float zoomFactor = (se->data().y > 0) ? 1.1f : 1.0f / 1.1f;
    float worldX = (se->position().x - state.pan_.x) / state.zoom_;
    float worldY = (se->position().y - state.pan_.y) / state.zoom_;
    state.zoom_ *= zoomFactor;
    state.zoom_ = std::clamp(state.zoom_, 0.01f, 1000.0f);
    state.pan_.x = se->position().x - worldX * state.zoom_;
    state.pan_.y = se->position().y - worldY * state.zoom_;
}
```

The trick: before changing the zoom, the cursor position is converted into *image coordinates* (`worldX/worldY`). After changing the zoom, the pan is recomputed so that the same image point ends up under the cursor once more. That makes the zoom appear anchored to the mouse.

The other inputs mirror Qt `imshow` exactly:

- **Left-drag** pans (`state.pan_ += data()`), while `pressLeft_`/`releaseLeft_` toggle `isDragging_`.
- **Right-click** resets zoom to 1:1 and re-centers.
- **Middle-click** zooms to `kDeepZoomThreshold` (30×) at the cursor — the "deep zoom" entry point.
- `move_` / `hoverEnter_` / `hoverExit_` keep `mousePos_` and `mouseInside_` fresh for the status bar.

### 5. Rendering the Canvas

The main `nvg` node draws the image with the current transform:

```cpp
save();
translate(state.pan_.x, state.pan_.y);
scale(state.zoom_, state.zoom_);
beginPath();
rect(0, 0, width, height);
fillPaint(imagePattern(0, 0, width, height, 0, state.imageHandle_, 1.0f));
fill();
```

Then, for moderate zoom levels (8×–30×), it draws grid lines whose alpha ramps in with the zoom — a preview that the deep-zoom overlay is about to take over.

### 6. Deep Zoom: Per-Pixel RGB Labels

At `zoom_ >= 30` the sample switches to a faithful reimplementation of Qt's `drawImgRegion()`: every on-screen pixel cell becomes a miniature label showing its numeric channel values.

- For **color images** (3 or 4 channels), each cell contains **three rows of text** — the red value in red, the green value in green, and the blue value in white/cyan — thin-pixel sampling so adjacent cells remain distinguishable.
- For **grayscale** images, a single value is drawn whose *text color* is brightness-shifted (`v > 127 ? v - 127 : 127 + v`) so it stays legible against both light and dark pixels.
- The **grid is stroked after the text**, in screen space, exactly like QPainter, so label boundaries are always crisp.

The on-screen text size grows with the zoom (`10 + (pixelHeight - 30) / 5`, clamped to 6–48 px) so it stays proportional to the cells. A cached `static cv::Mat pixels` and `pw/ph/pc` guard avoid re-copying the BGRA frame from the GPU unless the image actually changed.

### 7. The Status Bar

When enabled, a solid bar at the bottom shows, left to right: the filename, the pixel under the cursor (`(x=…, y=…)` plus `R`, `G`, `B` and optional `A` channel values, read straight out of `bgra_`), and the image size and current zoom percentage. This turns the sample into a usable pixel inspector.

### 8. The ImGui Layer: Menus, Shortcuts and Dialogs

`gui()` is where this sample really earns the "reimplementation" label:

- **Keyboard shortcuts** handled with ImGui's `IsKeyDown`/`IsKeyPressed`: `Ctrl+arrows` pan by 5% of the viewport, `Ctrl++/-` zoom around center, `Ctrl+0`/`Ctrl+P` reset, `Ctrl+F` fit to window, `Ctrl+X` deep zoom, `Ctrl+S` / `Ctrl+Shift+S` save image / save view, `Ctrl+O` open, `Ctrl+C` copy to clipboard (via `xclip`), `Esc` closes dialogs in priority order.
- A **main menu bar** (`File`, `View`, `Navigate`, `Help`) that re-exposes the same actions with mnemonics.
- A **file browser dialog** that lists directories and image files, supports typing a path, double-click to enter folders and applies the sample's own image-extension filter.
- **Save image as…** writes the original `bgra_`/`image_` pixels with the chosen format; **Save view as…** snapshots the rendered viewport (including the deep-zoom overlay). Both remember a last-save message that is color-coded green/red.
- A **properties dialog** and a **help overlay** that document every mouse and keyboard binding.

These dialogs all live in super-imposed ImGui windows, which the imgui context of Plan-V4D supports natively — the GUI thread and the worker share `State` through `RWS`.

### 9. Cleanup

`teardown()` deletes the NanoVG image handle, exactly mirroring `setup()`.

## Summary

- `imshow`-style viewers are a fantastic stress test for an event system and a GUI layer; everything here is built from plain `Event<Mouse>` lists, `branch` sub-graphs and a single shared `State`.
- A view transform (zoom + pan) keeps rendering code trivial: `translate` + `scale` + draw.
- Deep zoom is just a matter of reading pixels out of a private `UMat` copy and drawing text per cell with the *same* NanoVG API used for fonts in earlier tutorials.
- Keeping the original image separate from its RGBA/BGRA working copies is what makes "save original" vs "save view" trivially correct.

This concludes the tutorial series. You have now seen the entire Plan-V4D feature set in action, from a single NanoVG image to a full-blown `imshow` reimplementation.