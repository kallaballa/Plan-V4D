# Pedestrian-Demo {#v4d_pedestrian}

@prev_tutorial{v4d_font}
@next_tutorial{v4d_optflow}

|    |    |
| -: | :- |
| Original author | Amir Hassan (kallaballa) <amir@viel-zu.org> |
| Compatibility | OpenCV >= 4.7 |

Multi-pedestrian detection and tracking on a video. The demo uses OpenCV's HOG people detector with non-maximum suppression (NMS), maintains one KCF tracker per pedestrian, and renders smoothed tracking boxes with NanoVG. Dear ImGui controls are available for tuning detection frequency, track limits, tracker refresh, and smoothing.

\htmlinclude "../samples/example_v4d_pedestrian-demo.html"

## Running the demo

The executable accepts exactly one video path and opens a V4D display window. It is display-only and does not write an annotated video file.

From the configured OpenCV build directory, run:

```bash
bin/example_v4d_pedestrian-demo modules/v4d/assets/videos/dance.mp4
```

A different video can be supplied in place of the bundled sample. The demo requires FFmpeg-enabled OpenCV video input and a graphical OpenGL-capable environment.

## Tracking controls

The ImGui `Tracking` window provides sliders for:

- HOG re-detection interval and maximum number of pedestrians
- Tracker refresh period and miss threshold
- Box smoothing and live-track re-anchoring
- A read-only count of active tracks

## Pipeline

The per-frame plan captures a frame, prepares a downscaled grayscale image, and runs detection and tracking in a `BranchType::SINGLE` region. `update_tracking()` runs HOG periodically, filters overlapping detections with NMS, updates staggered KCF trackers, associates detections with tracks using IoU, reinitializes lost tracks, and removes tracks that remain lost. The resulting boxes are shared with the NanoVG renderer, which draws an orange ellipse for each active track.

The tracking state is serialized because the KCF trackers are mutable. The `V4DPlan::run<PedestrianDemoPlan>(2)` call starts three workers in addition to the main/display thread, while the `SINGLE` branch limits concurrent access to the tracking state. See the [sample walkthrough](../doc/samples/README.md#tutorial-15--pedestrian-detection-and-tracking-demo) for the detailed graph breakdown.

@include samples/pedestrian-demo.cpp


