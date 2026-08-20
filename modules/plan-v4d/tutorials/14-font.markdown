# Tutorial: Advanced Font Effects Demo

This tutorial breaks down a complex, "Star Wars"-style opening crawl effect. It's an excellent example of how to structure a high-performance application in Plan-V4D by separating logic into reusable components and using conditional execution to avoid unnecessary work.

This demo showcases:
-   Organizing rendering logic into distinct helper classes.
-   A "render-to-texture" pattern, where components are rendered into `UMat`s first and then composited later.
-   Using `branch` to conditionally run parts of the pipeline only when needed.
-   Combining `nvg` rendering with `plain` context OpenCV processing to create a final effect.

## The Code

The source code in `samples/font-demo.cpp` is broken into three main helper structs (`TextRenderer`, `StarsRenderer`, `Warp`) and the `FontDemoPlan` itself. We will focus on the structure and the `Plan`.

```cpp
// Abridged for clarity

// Renders scrolling text into a UMat
struct TextRenderer { /* ... */ };
// Renders a starfield into a UMat
struct StarsRenderer { /* ... */ };
// Warps the text rendering and composites it with the stars
struct Warp { /* ... */ };

class FontDemoPlan : public Plan {
    static TextRenderer text_;
    static StarsRenderer stars_;
    static Warp warp_;
    // ... other state variables ...

public:
    void gui() override {
        // ... ImGui controls for all text, stars, and warp parameters ...
    }

    void setup() override {
        // ... One-time setup for animation timer and text object ...
    }

    void infer() override {
        // Only recalculate the perspective transform if a parameter changed
        branch(CS(warp_.update_) || ... )
            ->plain(&Warp::calculate, RWS(warp_), size_)
        ->endBranch();

        // Only redraw the starfield if a parameter changed
        branch(CS(stars_.update_) || ... )
            ->nvg(&StarsRenderer::draw, RWS(stars_), size_)
            ->fb(UMAT_COPY_, RWS(stars_.rendering_)) // Copy result to UMat
        ->endBranch();

        // Always draw the scrolling text (as it's always moving)
        nvg(&TextRenderer::draw, RWS(text_), ...);
        fb(UMAT_COPY_, RWS(text_.rendering_)); // Copy result to UMat

        // Clear the main display
        clear();

        // Composite the final image in a plain context
        fb<3>(&Warp::perform, RWS(warp_), RS(text_.rendering_), RS(stars_.rendering_));

        // Reset the animation when the text scrolls off-screen
        branch(-CS(text_.textOffsetY_) > CS(text_.height_))
            ->assign(RWS(timeOffset_), F(seconds))
        ->endBranch();
    }
};
```

## Code Breakdown

### 1. Logic Separation and Render-to-Texture

The key to this demo's organization is that each major visual component is a separate class that renders its output to a `UMat` member variable, not directly to the screen.

-   **`StarsRenderer`**: The `infer` pipeline calls `StarsRenderer::draw` inside an `nvg` context. The next call, `fb(UMAT_COPY_, RWS(stars_.rendering_))`, copies the result from the framebuffer into the `stars_.rendering_` `UMat`.
-   **`TextRenderer`**: The same pattern is used. `TextRenderer::draw` renders the text, and `fb(UMAT_COPY_, ...)` saves the result to `text_.rendering_`.

This "render-to-texture" approach gives us two `UMat`s—one with stars, one with text—that we can now use as inputs for a final compositing step.

### 2. Conditional Execution for Performance

This demo would be inefficient if it redrew the starfield and recalculated the perspective warp on every frame. The `Plan` uses `branch` to avoid this.

```cpp
// Only run this block if the warp parameters changed in the GUI
branch(CS(warp_.update_) || ... )
    ->plain(&Warp::calculate, ...)
->endBranch();

// Only run this block if the star parameters changed in the GUI
branch(CS(stars_.update_) || ... )
    ->nvg(&StarsRenderer::draw, ...)
    ->fb(UMAT_COPY_, ...)
->endBranch();
```
-   Each renderer (`Warp`, `StarsRenderer`) has an `update_` flag that is set to `true` in the `gui()` method whenever one of its sliders is moved.
-   The `branch` condition checks this flag. The code inside the branch—the expensive part—only runs if the flag is true. This ensures we are not doing unnecessary work on static frames.

The scrolling text, however, is always moving, so it is drawn on every frame outside of a branch.

### 3. Final Compositing

The final effect is created in a `plain` context by the `Warp::perform` method.

```cpp
fb<3>(&Warp::perform, RWS(warp_), RS(text_.rendering_), RS(stars_.rendering_));
```
This single node in the task graph takes the two pre-rendered `UMat`s as input. Inside the `perform` method, it uses standard OpenCV functions (`warpPerspective`, `add`, etc.) to apply the 3D effect to the text and combine it with the starfield, writing the final result to the display framebuffer.

## Summary

This demo is a culmination of many Plan-V4D concepts.
-   **Code Organization**: Breaking down complex effects into logical, reusable classes is a clean and scalable approach.
-   **Render-to-Texture**: Rendering components into intermediate `UMat`s allows for complex post-processing and compositing.
-   **Stateful Optimization**: Using flags and `branch` to skip unnecessary work on static frames is critical for building high-performance applications.

Next, we will look at a pedestrian detection demo.
