#!/usr/bin/env python3
"""Give the YuNet face detector a dynamic input shape.

The YuNet model that opencv_zoo ships (face_detection_yunet_2023mar.onnx)
declares its graph input as a *static* 1x3x640x640 tensor. Everything in the
network is actually shape-agnostic -- all twelve Reshape nodes use -1 for the
spatial dimension, and the two Resize nodes carry `scales` rather than a
baked-in output size -- so the pinned 640x640 is nothing but a leftover of the
export.

That matters for cv::FaceDetectorYN, which does not run the network at the
size you ask for. detect() pads the frame up to a multiple of 32 and feeds the
result, so cv::FaceDetectorYN::create(..., Size(640, 360), ...) produces a 
1x3x384x640 blob. OpenCV's own graph engine treats a static input dim as merely
 advisory and runs it; ONNX Runtime validates it and refuses:

    Got invalid dimensions for input: input for the following indices
     index: 2 Got: 384 Expected: 640

So with cv::dnn::ENGINE_ORT the detector only works at input sizes that pad to
exactly 640x640. This script rewrites the two spatial input dims to symbolic
names ('height', 'width'), which is all ORT needs to accept any padded size.
The weights are untouched, and 640x640 still runs.

Run it from the repository root:

    ./make-dynamic-yunet.py                # rewrite the shipped asset in place
    ./make-dynamic-yunet.py --check        # verify, write nothing
    ./make-dynamic-yunet.py --stock -o F   # write the pristine model to F

"""

import argparse
import hashlib
import io
import os
import subprocess
import sys
import urllib.request
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ASSET = "face_detection_yunet_2023mar_dynamic.onnx"

# Same URL and checksum that modules/v4d/CMakeLists.txt pins for YUNET_PATH, so
# the input is the stock model even when the asset has already been rewritten.
STOCK_URL = (
    "https://github.com/opencv/opencv_zoo/raw/refs/heads/main/models"
    "/face_detection_yunet/face_detection_yunet_2023mar.onnx"
)
STOCK_SHA256 = "8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4"


def stock_bytes():
    """Return the pristine model bytes, verified against STOCK_SHA256.

    Prefer the local git blob so the script never needs network access, and fall
    back to the pinned download.
    """
    data = git_blob("HEAD:modules/v4d/assets/models/face_detection_yunet_2023mar.onnx")
    if data is None:
        print("-- fetching stock model", file=sys.stderr)
        with urllib.request.urlopen(STOCK_URL) as response:
            data = response.read()

    got = hashlib.sha256(data).hexdigest()
    if got != STOCK_SHA256:
        sys.exit(
            f"stock model checksum mismatch:\n"
            f"  expected {STOCK_SHA256}\n"
            f"  got      {got}"
        )
    return data


def git_blob(revision_path):
    """Contents of a blob in this repository, or None when it is unavailable."""
    try:
        proc = subprocess.run(
            ["git", "-C", str(SCRIPT_DIR), "cat-file", "blob", revision_path],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
    except OSError:
        return None
    return proc.stdout if proc.returncode == 0 else None


def ensure_onnx():
    """Return the onnx module, creating a venv for it when the current one lacks it.

    The re-exec keeps the rest of the script in a single interpreter instead of
    importing onnx behind our back.
    """
    try:
        import onnx

        return onnx
    except ImportError:
        pass

    venv = os.environ.get("YUNET_VENV") or os.path.join(os.environ.get("TMPDIR", "/tmp"), "yunet-dynamic-venv")
    python = Path(venv) / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    if not python.exists():
        print(f"-- creating venv at {venv}", file=sys.stderr)
        subprocess.run([sys.executable, "-m", "venv", str(venv)], check=True)
    subprocess.run([str(python), "-m", "pip", "-q", "install", "onnx"], check=True)
    os.execv(str(python), [str(python), str(Path(__file__).resolve()), *sys.argv[1:]])


def dims_of(value_info):
    return [d.dim_param or d.dim_value for d in value_info.type.tensor_type.shape.dim]


def dynamic_dims(model):
    """True when the graph input's two spatial dims are symbolic."""
    tt = model.graph.input[0].type.tensor_type
    if len(tt.shape.dim) != 4:
        return False
    return all(tt.shape.dim[i].dim_param for i in (2, 3))


def main():
    parser = argparse.ArgumentParser(
        description="Give the YuNet face detector a dynamic input shape.",
        epilog="Afterwards re-run ./build.sh plan+v4d so opencv/build/ picks up the new asset.",
    )
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--check", action="store_true", help="verify only, write nothing")
    group.add_argument("--stock", action="store_true", help="write the pristine model and exit")
    parser.add_argument(
        "-o",
        "--output",
        default=str(ASSET),
        metavar="FILE",
        help="where --stock writes (default: ./face_detection_yunet_2023mar.stock.onnx)",
    )
    args = parser.parse_args()

    if args.stock:
        out = Path(args.output)
        if out == ASSET:
            out = Path.cwd() / "face_detection_yunet_2023mar.stock.onnx"
        out.write_bytes(stock_bytes())
        print(f"stock model written to {out}")
        return 0

    onnx = ensure_onnx()
    from onnx import shape_inference

    asset = str(ASSET)
    if dynamic_dims(onnx.load(asset)):
        print(f"{asset} is already dynamic -- nothing to do")
        return 0

    if args.check:
        print(f"FAIL: {asset} still has a static input shape", file=sys.stderr)
        return 1

    # Keep the batch and channel dims static; only height and width become
    # symbolic.
    model = onnx.load_model(io.BytesIO(stock_bytes()))
    print(f"stock input: {dims_of(model.graph.input[0])}")
    tt = model.graph.input[0].type.tensor_type
    for axis, name in ((2, "height"), (3, "width")):
        d = tt.shape.dim[axis]
        d.ClearField("dim_value")
        d.dim_param = name

    # The graph outputs carry shape annotations that were inferred at 640x640.
    # Drop the derived ones so ONNX Runtime re-infers them per run instead of
    # treating them as a contract.
    for out in model.graph.output:
        for axis in (1, 2):
            d = out.type.tensor_type.shape.dim[axis]
            if d.dim_value:
                d.ClearField("dim_value")
                d.dim_param = f"{out.name}_d{axis}"

    # Stale inferred shapes would be re-validated against the new symbolic input.
    del model.graph.value_info[:]

    onnx.checker.check_model(model)
    onnx.save(model, asset)
    print(f"wrote {asset}: input {dims_of(model.graph.input[0])}")

    # Prove it: infer shapes at 384x640 and check nothing stayed pinned.
    for h, w in ((384, 640), (640, 640)):
        probe = onnx.load(asset)
        for axis, value in ((2, h), (3, w)):
            d = probe.graph.input[0].type.tensor_type.shape.dim[axis]
            d.ClearField("dim_param")
            d.dim_value = value
        inferred = shape_inference.infer_shapes(probe)
        bad = [
            out.name
            for out in inferred.graph.output
            if any(not d.dim_value for d in out.type.tensor_type.shape.dim)
        ]
        status = "OK" if not bad else f"outputs still symbolic: {bad}"
        print(f"  shape inference at {h}x{w}: {status}")
        if bad:
            return 1

    digest = hashlib.sha256(ASSET.read_bytes()).hexdigest()
    print(f"{digest}  {ASSET}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
