# Skeletal tracker verification tooling

The harnesses behind the numbers in
[`doc/skeletal-tracker-improvements.md`](../../../doc/skeletal-tracker-improvements.md).
They used to live in a scratch directory, which meant the next change to
`samples/skeletal-tracker-demo.cpp` had no way of being re-verified.

They drive the sample's real code: `bench_pipeline` and `probe_crop` include
`samples/skeletal-tracker-pipeline.hpp` — the detector, pose estimator, tracker
and shared state the sample itself compiles — rather than a copy of it, and with
the demo's own `SharedPoseState` for tunables. `tool_prelude.hpp` exists only so
that `#define private public` reaches the pipeline's internals without rewriting
access specifiers inside the standard library and OpenCV.

## Running

```bash
./verify.sh            # everything: build, then every check in order
./verify.sh --quick    # everything except the two runs of the demo itself
./build.sh             # just build the C++ harnesses
OPENCV_BUILD=/path/to/opencv/build ./build.sh   # a different OpenCV build tree
```

`verify.sh` needs `ffmpeg`/`ffprobe` for the clip work, `xvfb-run` for the demo
runs, and the models downloaded into `modules/v4d/assets/models/pose/` (the
sample's `make download-models`). The shell steps take `BIN` (the demo binary),
`CLIP` and `WORK` overrides from the environment.

| Harness | What it checks |
|---|---|
| `test_anchors.cpp` | The detector anchor table. Recomputes all 2254 anchors from an independently written description of the model's layout, and pins the float values from the generated table this replaced. Compiles with no OpenCV at all, which is what makes it runnable first and always. |
| `probe_crop.cpp` | The per-person pose crop: that the sample's fused warp and its landmark mapping agree with an independent rebuild of the same geometry (**0.000 px** or it fails), plus what the fused warp costs and what the change of resampling moves the landmarks. |
| `bench_pipeline.cpp` | The pipeline alone, on a clip: tracks, ids, coasting frames, duplicate skeletons on one person, trail saturation, keypoint scores and the detect/pose split. |
| `make_two_person.sh` | Builds the synthetic two-person clip. The bundled clips are single-person, so this is the only coverage of the multi-person association, the duplicate merge and the person limit. |
| `check_cli.sh` | The demo's command line contract: recording is opt-in, the sink honours the demo's fixed 1920x1080 viewport, and a missing input fails with usage. |
| `probe_e2e.sh` | The demo itself, headless: wall time, exit status, and how many of the source frames reached the sink. `NOSINK=1` leaves the sink out, which separates the cost of encode from the cost of inference. |

## What is deliberately not here

The harnesses are not CMake targets. They redefine `private`, and one of them
rebuilds the crop geometry independently on purpose; neither belongs in the
OpenCV build, so `build.sh` compiles them against the in-tree OpenCV the same
way it was done before, and `verify.sh` is the entry point.