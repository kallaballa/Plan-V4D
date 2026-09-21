# imshow Reimplementation {#v4d_imshow_reimplementation}

@prev_tutorial{v4d_image_carousel}
@next_tutorial{v4d}

|    |    |
| -: | :- |
| Original author | Amir Hassan (kallaballa) <amir@viel-zu.org> |
| Compatibility | OpenCV >= 4.7 |

A full reimplementation of OpenCV's `imshow` (Qt flavor) on top of V4D: zoom around the cursor, left-drag panning, right-click reset, middle-click deep zoom (per-pixel RGB / grayscale labels above 30×), a pixel-inspecting status bar, and a complete ImGui layer with menu bar, keyboard shortcuts, file browser and save dialogs.

\htmlinclude "../samples/example_v4d_imshow_reimplementation.html"

@include samples/imshow_reimplementation.cpp