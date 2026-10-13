# Optflow-Demo {#v4d_optflow}

@prev_tutorial{v4d_pedestrian}
@next_tutorial{v4d_beauty}

|    |    |
| -: | :- |
| Original author | Amir Hassan (kallaballa) <amir@viel-zu.org> |
| Compatibility | OpenCV >= 5.0 |

Optical flow visualization on top of a video. Detects features to track (FAST, OpenCV), tracks them with sparse pyramidal Lucas-Kanade (OpenCV), renders the vectors with nanovg (OpenGL), reacts to scene changes through a heuristic detector, and post-processes the video with glow/bloom effects (OpenCV).

\htmlinclude "../samples/example_v4d_optflow-demo.html"

@include samples/optflow-demo.cpp



