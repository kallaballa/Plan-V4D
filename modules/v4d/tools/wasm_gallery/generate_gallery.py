#!/usr/bin/env python3
"""Generate the V4D WebAssembly gallery web-app from the built wasm samples.

The build produces one emscripten JS+wasm pair per enabled sample in
<build_wasm>/bin/. This script scans that directory (excluding montage-demo,
which is intentionally not part of the gallery) and emits a gallery:

  <outdir>/                 (serve this as the web root)
  ├── index.html            gallery grid linking to each demo
  └── demos/<sample>/index.html   per-demo page that loads the sample module

Each demo page is self-contained: it creates the <canvas id="canvas"> that the
emscripten GLFW port binds to and loads the compiled <sample>.js, which locates
its .wasm next to it (so /bin must be served alongside /gallery).
"""

import argparse
import html
import json
import os
import re
import sys

# demos that are intentionally excluded from the gallery
EXCLUDED = {"montage-demo"}

TITLES = {
    "display_image_fb": "Display Image (Framebuffer)",
    "display_image_nvg": "Display Image (NanoVG)",
    "vector_graphics": "Vector Graphics",
    "vector_graphics_and_fb": "Vector Graphics + Framebuffer",
    "render_opengl": "GL Blue Screen",
    "custom_source_and_sink": "Custom Source / Sink",
    "font_rendering": "Font Rendering",
    "font_with_gui": "Font Rendering with GUI",
    "video_editing": "Video Editing",
    "cube-demo": "Cube Demo",
    "many_cubes-demo": "Many Cubes Demo",
    "video-demo": "Video Demo",
    "nanovg-demo": "NanoVG Demo",
    "font-demo": "Font Demo",
    "shader-demo": "Mandelbrot Shader Demo",
    "pedestrian-demo": "Pedestrian Demo",
    "optflow-demo": "Sparse Optical Flow Demo",
    "beauty-demo": "Beautification Demo",
    "bgfx-demo": "BGFX Demo",
    "bgfx-demo2": "BGFX Demo 2",
}

DEMO_PAGE_TEMPLATE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>{title} — V4D Gallery</title>
<style>
  html,body {{ margin:0; padding:0; height:100%; background:#111; color:#eee;
               font-family: system-ui, sans-serif; }}
  body {{ display:flex; flex-direction:column; }}
  header {{ padding:10px 16px; background:#1a1a1a; border-bottom:1px solid #333;
            display:flex; justify-content:space-between; align-items:center; }}
  header a {{ color:#6cf; text-decoration:none; }}
  h1 {{ font-size:16px; margin:0; font-weight:500; }}
  main {{ flex:1; display:flex; align-items:center; justify-content:center; overflow:hidden; }}
  #status {{ position:fixed; bottom:8px; left:8px; background:rgba(0,0,0,.7);
            padding:4px 8px; border-radius:4px; font-size:12px; color:#9f9; }}
  #status.error {{ color:#f99; }}
  canvas {{ max-width:100%; max-height:100%; background:#000; }}
</style>
</head>
<body>
<header>
  <a href="../../index.html">&larr; Gallery</a>
  <h1>{title}</h1>
  <span></span>
</header>
<main>
  <canvas id="canvas" width="{vw}" height="{vh}" style="width:{vw}px;height:{vh}px;"></canvas>
</main>
<div id="status">loading…</div>

<script>
  // Surface module load status so the gallery test-suite can assert on it.
  const statusEl = document.getElementById('status');
  const ok = (m) => {{ statusEl.textContent = m; statusEl.classList.remove('error'); }};
  const fail = (m) => {{ statusEl.textContent = m; statusEl.classList.add('error');
                          document.body.dataset.status = 'error';
                          console.error(m); }};
  document.body.dataset.status = 'loading';

  // Provide a synthetic argv[1] so demos expecting a media path don't abort
  // their init before GLFW/WebGL bring-up. Actual media decoding is not
  // available in the wasm build (FFmpeg is disabled), but the module still
  // initialises the runtime and creates the window/canvas.
  var Module = {{
    canvas: document.getElementById('canvas'),
    arguments: [{js_args}],
    onRuntimeInitialized() {{ ok('module ready'); document.body.dataset.status = 'ready'; }},
    print: (t) => console.log(t),
    printErr: (t) => {{ if (/fatal|abort|Assertion/.test(t + '')) fail(t); console.error(t); }},
  }};
</script>
<script src="/bin/example_v4d_{sample}.js"></script>
</body>
</html>
"""

INDEX_TEMPLATE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>V4D WebAssembly Gallery</title>
<style>
  body {{ margin:0; padding:24px; background:#111; color:#eee;
          font-family: system-ui, sans-serif; }}
  h1 {{ font-size:22px; margin:0 0 4px; }}
  p.sub {{ color:#999; margin:0 0 24px; }}
  .grid {{ display:grid; grid-template-columns:repeat(auto-fill,minmax(240px,1fr)); gap:14px; }}
  a.card {{ display:block; background:#1c1c1c; border:1px solid #333; border-radius:8px;
            padding:18px; text-decoration:none; color:#eee; transition:border-color .15s, transform .15s; }}
  a.card:hover {{ border-color:#6cf; transform:translateY(-2px); }}
  a.card .name {{ font-weight:600; margin-bottom:6px; }}
  a.card .src {{ font-family:monospace; font-size:12px; color:#999; }}
  body:not(.jsok) .status {{ display:none; }}
</style>
</head>
<body>
  <h1>V4D WebAssembly Gallery</h1>
  <p class="sub">All currently enabled v4d samples, built for the browser ({count} demos, the montage sample excluded).</p>
  <div id="demos" class="grid">
{cards}
  </div>
  <div id="count" data-count="{count}" style="display:none;"></div>
</body>
</html>
"""


def discover_samples(bin_dir):
    samples = set()
    for name in os.listdir(bin_dir):
        m = re.match(r"^example_v4d_(.+)\.js$", name)
        if not m:
            continue
        base = m.group(1)
        if base in EXCLUDED:
            continue
        # require both .js and .wasm to be present
        if os.path.isfile(os.path.join(bin_dir, f"example_v4d_{base}.wasm")):
            samples.add(base)
    return sorted(samples)


def sanitize(s):
    return re.sub(r"[^A-Za-z0-9_-]", "_", s)


def main():
    ap = argparse.ArgumentParser(description="Generate the V4D wasm gallery")
    ap.add_argument("--bin", required=True, help="build_wasm/bin directory with the built samples")
    ap.add_argument("--out", required=True, help="gallery output directory (served as web root)")
    ap.add_argument("--js-arg", default="video.webm",
                    help="synthetic argv[1] passed to every demo module")
    args = ap.parse_args()

    samples = discover_samples(args.bin)
    if not samples:
        print("No samples found in %s" % args.bin, file=sys.stderr)
        sys.exit(1)

    os.makedirs(os.path.join(args.out, "demos"), exist_ok=True)

    js_arg = json.dumps(args.js_arg)
    index_cards = []
    for sample in samples:
        title = TITLES.get(sample, sample.replace("-", " ").title())
        vw, vh = 960, 720
        if sample in ("render_opengl",):
            vw, vh = 960, 960
        page = DEMO_PAGE_TEMPLATE.format(
            title=html.escape(title),
            sample=sanitize(sample),
            js_args=js_arg,
            vw=vw,
            vh=vh,
        )
        demo_dir = os.path.join(args.out, "demos", sanitize(sample))
        os.makedirs(demo_dir, exist_ok=True)
        with open(os.path.join(demo_dir, "index.html"), "w") as f:
            f.write(page)
        index_cards.append(
            '<a class="card" href="demos/%s/">'
            '<div class="name">%s</div>'
            '<div class="src">%s</div></a>'
            % (sanitize(sample), html.escape(title), sample)
        )

    index_html = INDEX_TEMPLATE.format(
        count=len(samples),
        cards="\n".join(index_cards),
    )
    with open(os.path.join(args.out, "index.html"), "w") as f:
        f.write(index_html)

    print("Generated gallery for %d demos in %s" % (len(samples), args.out))
    for s in samples:
        print("  - %s" % s)


if __name__ == "__main__":
    main()
