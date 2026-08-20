# Tutorial: Font Rendering

Text is a fundamental part of most graphical applications. Plan-V4D makes font rendering easy by leveraging the capabilities of NanoVG. This tutorial will show you how to load fonts and draw text to the screen.

## The Code

Here is the complete source code for this example. You can find it in `samples/font_rendering.cpp`.

```cpp
#include <opencv2/v4d/v4d.hpp>

using namespace cv;
using namespace cv::v4d;

class FontRenderingPlan: public Plan {
    //The text to render
    string text_ = "Hello World";
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    void infer() override {
        //Render the text at the center of the screen.
        nvg([](const Size& sz, const string& str) {
            using namespace cv::v4d::nvg;
            clearScreen();
            fontSize(40.0f);
            fontFace("sans-bold");
            fillColor(Scalar(255, 0, 0, 255));
            textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            text(sz.width / 2.0, sz.height / 2.0, str.c_str(),
                    str.c_str() + str.size());
        }, size_, R(text_));
    }
};

int main() {
    cv::Rect viewport(0, 0, 960, 960);
    cv::Ptr<V4D> runtime = V4D::init(viewport, "Font Rendering", AllocateFlags::NANOVG | AllocateFlags::IMGUI);
    Plan::run<FontRenderingPlan>(0);

    return 0;
}
```

## Code Breakdown

### 1. The `FontRenderingPlan`

The `Plan` for this example is very simple. It stores the string we want to render as a member variable.

```cpp
class FontRenderingPlan: public Plan {
    string text_ = "Hello World";
    Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    // ...
};
```

### 2. The `infer()` Phase

All the rendering logic is in the `infer()` method, inside an `nvg` context.

```cpp
void infer() override {
    nvg([](const Size& sz, const string& str) {
        using namespace cv::v4d::nvg;
        clearScreen();
        fontSize(40.0f);
        fontFace("sans-bold");
        fillColor(Scalar(255, 0, 0, 255));
        textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        text(sz.width / 2.0, sz.height / 2.0, str.c_str(),
                str.c_str() + str.size());
    }, size_, R(text_));
}
```
- **`nvg([...], size_, R(text_))`**: We open a NanoVG context, passing in the window size and our `text_` variable. We use the `R` (Read-only) edge-call since we are only reading the string, not modifying it.
- **`fontSize(40.0f)`**: Sets the size of the font.
- **`fontFace("sans-bold")`**: Selects the font to use. "sans-bold" is a default font provided by NanoVG.
- **`fillColor(...)`**: Sets the color of the text.
- **`textAlign(...)`**: Sets the alignment. Here, we align the text so that its top-center point is at the coordinates we specify.
- **`text(...)`**: The function that actually draws the text to the screen at the given coordinates.

### Loading Custom Fonts

While this example uses a built-in font, you can easily load your own TrueType (`.ttf`) fonts. To do this, you would typically call `createFont` in your `setup()` phase.

```cpp
// In your Plan class
int my_font_;

// In your setup() method
void setup() override {
    nvg([&](...) {
        // The first argument is a handle name, the second is the path to the file.
        my_font_ = createFont("my-cool-font", samples::findFile("fonts/Roboto-Regular.ttf"));
        CV_Assert(my_font_ >= 0); // Check that the font was loaded
    });
}

// In your infer() method
void infer() override {
    nvg([&](...) {
        // ...
        fontFace("my-cool-font"); // Use the handle name you defined
        // ...
    });
}
```

## Summary

This tutorial demonstrated the basics of text rendering in Plan-V4D:
- Using the `nvg` context to access font rendering functions.
- Setting font properties like size, face, color, and alignment.
- Drawing text to the screen.
- The basic process for loading and using custom fonts.

In the next tutorial, we will look at how to build a simple video editor.
