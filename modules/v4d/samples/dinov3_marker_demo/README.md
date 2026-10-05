# DINOv3 Marker Demo

A Plan-V4D sample that registers a natural image patch as a "marker" and then
recognizes it in a video stream. A DINOv3 ONNX model turns the patch into an
embedding; recognition is a nearest-neighbour cosine search over the stored
embeddings, optionally backed by an AKAZE + RANSAC homography check that outlines
the marker in the frame.

Inference runs through `cv::dnn` (OpenCV 5.x), the marker database persists via
`cv::FileStorage`, and the overlay is drawn with NanoVG alongside an ImGui
control panel.

## Files

| File | Contents |
| --- | --- |
| `dinov3_marker_demo.cpp` | `main()`, CLI parsing, and the `Dinov3MarkerPlan` V4D plan (setup / gui / infer pipeline) |
| `dinov3_embedder.{hpp,cpp}` | ONNX loading and image → descriptor embedding |
| `marker_database.{hpp,cpp}` | `MarkerRecord` store, cosine search, and YAML persistence |
| `geometric_verifier.{hpp,cpp}` | AKAZE matching + RANSAC homography against a record |

## Build status

**This directory is not currently wired into the build.** There is no CMake
target for it, so `build.sh` will not produce a binary from these sources, and
the whole translation set does not compile as written. Two independent
breakers, both confirmed with `g++ -fsyntax-only` against this tree's real
include flags:

1. `dinov3_embedder.hpp:5` and `geometric_verifier.hpp:5` include
   `<opencv2/core/umatrix.hpp>`, which does not exist in OpenCV 5. `UMat` now
   lives in `opencv2/core/mat.hpp` and is re-exported by `opencv2/core.hpp`, so
   those includes should just be dropped (or point at `opencv2/core.hpp`).
2. `marker_database.cpp` and `geometric_verifier.hpp` use `cv::AKAZE`, which is
   not part of this OpenCV 5 `features` module — `opencv2/features.hpp` ships
   `SIFT`, `ORB`, `MSER`, `DISK`, `XFeat`, and `ALIKED`, but no AKAZE. A
   substitute detector is needed.

The hyphenated sibling directory `../dinov3-marker-demo/` *is* registered
(`modules/v4d/CMakeLists.txt:499`) and is the current implementation; it splits
the same functionality into V4D-free units, uses ORB/SIFT instead of AKAZE, and
adds a head-less self-test target. Treat that directory as authoritative and
this one as an earlier single-plan draft.

Once the above are fixed, add a target next to the existing DINOv3 block:

```cmake
add_multisource_sample(example_v4d_dinov3_marker_demo
  samples/dinov3_marker_demo/dinov3_marker_demo.cpp
  samples/dinov3_marker_demo/dinov3_embedder.cpp
  samples/dinov3_marker_demo/marker_database.cpp
  samples/dinov3_marker_demo/geometric_verifier.cpp)
ocv_target_link_libraries(example_v4d_dinov3_marker_demo opencv_geometry)
ocv_target_include_directories(example_v4d_dinov3_marker_demo PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/samples/dinov3_marker_demo")
```

`opencv_geometry` is required because `findHomography` and `perspectiveTransform`
live there in OpenCV 5, not in `calib3d`.

Then build the stack:

```bash
./build.sh -t plan+v4d -b debug
```

## Model

The demo expects a DINOv3 ViT-B/16 ONNX export. The repo already carries the
graph and its external weights at `modules/v4d/assets/models/`:

```
dinov3_vitb16.onnx
dinov3_vitb16.onnx.data
```

`modules/v4d/CMakeLists.txt` copies both into the build output, and **both files
must stay side by side** — the weights are resolved relative to the graph, so
copying only the `.onnx` yields a model that loads but fails at inference time.

Preprocessing assumes standard DINOv3 ImageNet normalization and BGR input
(`swapRB = true`), which is what the embedder is configured for.

## Usage

```
example_v4d_dinov3_marker_demo [options] <input> [output]
```

| Argument | Default | Meaning |
| --- | --- | --- |
| `--model=<path>` | *(none)* | DINOv3 ONNX graph. Unset means no recognition. |
| `--db=<path>` | `dinov3_markers.yml.gz` | Marker database; loaded at startup, saved on request |
| `--input-size=<n>` | `224` | DNN input side length; values below 128 are ignored |
| `--threshold=<f>` | `0.62` | Cosine similarity required to accept a match, clamped to `[0, 1]` |
| `<input>` | *(none)* | Input video; without it the plan runs on an empty source |
| `<output>` | *(none)* | Optional sink video, written at the source fps and 1280×720 |

```bash
# Preview only
./build/plan+v4d/bin/example_v4d_dinov3_marker_demo input.mp4 \
    --model=dinov3_vitb16.onnx --db=markers.yml.gz

# Record the overlay to a file
./build/plan+v4d/bin/example_v4d_dinov3_marker_demo input.mp4 output.mkv \
    --model=dinov3_vitb16.onnx --db=markers.yml.gz
```

The window opens at 1280×720 and is resizable. All arguments are parsed by hand
from `argv`; unknown `--flags` are ignored, and only the first two non-flag
positional arguments are treated as input and output.

## Workflow

1. Start the demo on a video where the region of interest (the centre crop) shows
   the marker you want to teach.
2. Type a name in **Marker name** and press **Register Marker**. The current
   centre crop is embedded and stored, together with a thumbnail and AKAZE
   keypoints.
3. Press **Save Database** to persist it to `--db`.
4. Recognition then runs every frame (subject to **Process every N**) and reports
   the best match above the threshold.

All GUI controls act on the frame currently in flight, so pause or step to the
frame you want before registering.

## Controls

The ImGui panel (`Dinov3MarkerPlan::drawGui`) shows model state, marker count,
the last action performed, and the current recognition, plus:

**Actions**

- **Register Marker** — embed the current centre crop and append it to the database
- **Update Nearest** — overwrite the closest existing marker with the current crop
- **Clear All** — drop every marker and reset the id counter
- **Save Database** / **Load Database** — read/write the `--db` file

**Toggles and sliders**

| Control | Default | Range | Effect |
| --- | --- | --- | --- |
| Enable recognition | on | — | Master switch; skips the embed/search path entirely |
| HUD | on | — | Draw the NanoVG overlay |
| Draw ROI | on | — | Outline the centre crop that is embedded |
| Geometric verification | on | — | Run the homography check after a match |
| Match threshold | 0.62 | 0.0 – 1.0 | Minimum cosine similarity to accept a match |
| ROI scale | 0.80 | 0.2 – 1.0 | Centre-crop fraction, clamped to `[0.1, 1.0]` |
| DNN input size | 224 | 128 – 518 | Embedder input resolution |
| Process every N | 1 | 1 – 30 | Run recognition only on frames where `frameNo % N == 0` |
| Marker name | `marker` | 64 chars | Name for the next registration |

Read-outs: recognized name, cosine score, inlier ratio (when verification is on),
and the embed / search / verify timings in milliseconds.

## Pipeline

`Dinov3MarkerPlan::infer()` builds the frame graph each tick:

1. `fb` — copy the framebuffer into the working `UMat`.
2. `branch` on the register / update / clear request flags. Each is a take-and-clear
   read of `SharedParams`, so a GUI click fires exactly one frame later at most.
3. Two `SINGLE`-type branches for save and load.
4. `branch` on `shouldRunRecognition` → `recognizeFrame`, which crops, embeds,
   searches, and optionally verifies.
5. `branch` on the HUD flag → `drawHud` (NanoVG), gated so the overlay costs
   nothing when disabled.

Parameters are snapshotted once per frame into `paramsSnapshot` so a mid-frame GUI
change cannot make two nodes disagree about the configuration. `SharedParams` and
`SharedStatus` are `_shared` between the GUI and inference nodes.

## How recognition works

### Embedding — `Dinov3Embedder::embed`

1. Normalize to 3 channels (BGRA→BGR, gray→BGR).
2. Bilinear resize to `inputSize × inputSize`.
3. `blobFromImage` with scale `1/255`, mean `{0.485, 0.456, 0.406}`, `swapRB`.
4. `net.forward()`.
5. Reduce the output to a fixed-length vector by rank:
   - **1-D** — used as-is.
   - **2-D** `[N, D]` — average across rows (or take the row directly if `N == 1`).
   - **3-D** `[1, N, D]` — average across `N`.
   - **4-D** `[1, C, H, W]` — global average pool each channel to `C` values.
6. L2-normalize, so all comparisons are plain dot products.

### Search — `MarkerDatabase::search`

Linear scan over every record, cosine similarity via `cosineSimilarity`
(`marker_database.cpp:10`), keeping the best. The loop short-circuits once a score
exceeds `0.99`, on the assumption that nothing can score meaningfully higher. If
the best score is under the threshold, the function returns `false` and clears the
id and name — a rejection is not reported as "found something with a low score".

The database is guarded by a `std::shared_mutex`: `search`, `size`, and
`getMarker` take a shared lock, mutations take an exclusive one.

### Geometric verification — `GeometricVerifier::verify`

Runs on the **full frame** (not the crop) after a match has already been accepted:

1. Detect AKAZE keypoints and descriptors in the frame.
2. `BFMatcher` with `NORM_HAMMING` and cross-check enabled, matching stored
   marker descriptors against frame descriptors.
3. Require at least `minMatches` (12) matches; sort by distance and keep at most
   `4 × minMatches` (48) correspondences.
4. `findHomography` with RANSAC at a 4.0 px reprojection threshold.
5. Accept only if inliers ≥ `minInliers` (10) **and** inlier ratio ≥ 0.35.
6. Project the thumbnail's four corners through the homography to obtain the
   marker outline in frame coordinates.

## Database format

`save` writes YAML through `cv::FileStorage`, which gzips transparently when the
path ends in `.gz`:

```yaml
markers:
  -
    id: 0
    name: "my marker"
    thumbnail: [...]      # rows of the centre crop
    embedding: [...]      # D floats as a 1-column CV_32F matrix
nextId: 1
```

`load` re-extracts AKAZE keypoints from each restored thumbnail rather than
serializing them. Ids come from the file; `nextId` is read from the file after the
loop that derives it from the record ids, so the stored value wins when present.

## Overlay

`drawHud` renders in the top-left corner: title, FPS / frame number / marker
count, and the recognized name in green when a match is live. Below that it draws
the ROI rectangle in yellow and, when verification produced corners, the marker
outline in green.

## Things to be aware of

- **Verification does not veto a recognition.** `recognizeFrame` sets
  `recognition.valid = true` before calling the verifier and never clears it on
  failure (`dinov3_marker_demo.cpp:453-469`). A failed homography check leaves the
  match reported with `inlierRatio == 0` and no corners. Treat the inlier ratio,
  not the recognition flag, as the geometric signal.
- **`--input-size` after startup has no effect on the model.** `initModel` runs
  once in `setup()` (`dinov3_marker_demo.cpp:270`); dragging the **DNN input size**
  slider changes `SharedParams` but nothing re-reads it, because only
  `inputSize` is copied into the embedder config and only in `setup()`. Restart
  with `--input-size=` to change it.
- **`Config::stddev` is never applied.** The embedder holds a `stddev` member but
  `blobFromImage` is called with mean only, so the values `{0.229, 0.224, 0.225}`
  have no effect. Preprocessing is mean-subtraction without standard-deviation
  scaling.
- **`GeometricVerifier::Config::ratioThreshold` is never applied.** There is no
  Lowe ratio test; cross-checked brute-force matching stands in for it. The field
  is dead.
- **`SharedParams::autoSave` is dead.** It is declared, never read, and has no
  GUI control, so nothing is persisted unless you press **Save Database**.
- **Outputs of rank > 4 yield an empty descriptor.** `embed` returns `true` with
  an empty vector for such models rather than reporting an error. Every
  `cosineSimilarity` call then returns `-1`, so nothing ever matches.
- **Timing is stored but not shown.** `preprocessMs` is measured and never
  rendered; only embed, search, and verify appear in the panel.
- **Update Nearest uses its own gate.** It requires `max(0.25, threshold * 0.75)`
  similarity (`dinov3_marker_demo.cpp:390`), so it can be more permissive than
  recognition at low thresholds.
- **The ROI is always a centre crop.** There is no region selection in this
  version — the recognizer re-reads the same centre crop every frame.