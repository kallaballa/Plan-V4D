# Display an image using the video pipeline {#v4d_display_image_pipeline}

@prev_tutorial{v4d}
@next_tutorial{v4d_display_image_fb}

|    |    |
| -: | :- |
| Original author | Amir Hassan (kallaballa) <amir@viel-zu.org> |
| Compatibility | OpenCV >= 5.0 |

## Using the video pipeline
There are several ways to get an image onto the screen with V4D. The most
convenient one is to hand it to the video pipeline: set a `Source` on the
runtime, and the runtime copies the frame into the framebuffer for you before
`infer()` runs.

That copy is what the `capture()` node is for, and `Plan::run` emits it for you:

```cpp
plan->capture();   // the Source's frame, resized and color-converted into the fb
plan->infer();
plan->write();     // the framebuffer, handed to the Sink if one is set
plan->makeGraph();
```

The convenience is that the runtime does the awkward part: it resizes the frame
to the viewport (preserving aspect ratio) and converts it to BGRA, because the
framebuffer is BGRA while video frames arrive as BGR. It also flips the frame
into OpenGL's bottom-up coordinate system. A plan therefore never resizes or
converts anything to *display* a frame — it only draws on top of it.

## Reading the frame back

A plan that needs the pixels, rather than just drawing over them, copies them
out of the framebuffer with `fb`. This is `display_image_fb.cpp` without the
`Source` — the same plan, with the source removed:

@include samples/display_image_fb.cpp

The plan body is the interesting part:

```cpp
void infer() override {
    fb([](UMat &framebuffer, const cv::UMat &c) { c.copyTo(framebuffer); },
       R(converted_));
}
```

`fb` inserts a `UMat&` referring to the framebuffer at argument position `pos`
— zero-based, and `0` unless you say otherwise. `fb<1>(cv::cvtColor, src, …)`
puts the framebuffer *second*, after the image being converted, which is the
shape you want when rendering a result into it.

## Feeding an image through a Source instead

To get the same behavior from the video pipeline, give the runtime a `Source`
that yields the image and drop the `setup()` resize entirely — the runtime
already did it:

```cpp
auto src = new Source([&](cv::UMat &frame) -> bool {
    image_.copyTo(frame);      // CV_8UC3 or CV_8UC4, RGB order
    return true;
}, /*fps=*/1.f);

runtime->setSource(src);
V4DPlan::run<DisplayImageFB>(2);
```

With a `Source` set and no explicit `write()`, the framebuffer is simply
displayed. Add a `Sink` and the same frame is recorded as well — see
\ref v4d_video_editing.

**Do not call `capture()` or `write()` yourself.** They are the runtime's two
I/O nodes; calling them records a second copy of work the runtime is already
doing, and the two copies race. Every sample in this tutorial set uses `fb`
instead. The methods are marked runtime-internal on `V4DPlan` for exactly this
reason.