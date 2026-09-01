#!/usr/bin/env node
/**
 * Playwright test-suite for the V4D WebAssembly gallery.
 *
 * Requirements:
 *   - The wasm samples must have been built into <build_wasm>/bin as
 *     example_v4d_*.js / .wasm (run cmake/emscripten/build-wasm.sh).
 *   - The gallery must have been generated (run generate_gallery.py --out <build_wasm>/gallery).
 *   - A global playwright install must be reachable (NODE_PATH=/home/elchaschab/node_modules).
 *
 * The suite:
 *   1. serves the build_wasm directory (COOP/COEP enabled for pthread wasm),
 *   2. spins up headless Chromium with software WebGL (SwiftShader),
 *   3. asserts:
 *        a. the gallery index lists every enabled sample and excludes montage-demo,
 *        b. every demo's .js/.wasm assets are served with the correct MIME type,
 *        c. every demo page loads, its wasm module runtime initialises, a WebGL
 *           context is created on the canvas, and no fatal error is raised.
 */

'use strict';

const { chromium } = require('playwright');
const http = require('http');
const path = require('path');

const serverMod = require('../server.js');
const { ROOT } = serverMod;

const BASE = `http://${serverMod.HOST}:${serverMod.PORT}`;

const EXCLUDED = new Set(['montage-demo']);

// samples expected in the gallery = all example_v4d_*.js in bin minus montage-demo
function expectedSamples() {
  const fs = require('fs');
  const binDir = path.join(ROOT, 'bin');
  const out = [];
  for (const name of fs.readdirSync(binDir)) {
    const m = name.match(/^example_v4d_(.+)\.js$/);
    if (!m) continue;
    const base = m[1];
    if (EXCLUDED.has(base)) continue;
    if (fs.existsSync(path.join(binDir, `example_v4d_${base}.wasm`))) out.push(base);
  }
  return out.sort();
}

const results = [];
function report(suite, ok, msg) {
  results.push({ suite, ok, msg });
  const line = `  ${ok ? 'PASS' : 'FAIL'}  ${suite}` + (msg ? ` — ${msg}` : '');
  console.log(line);
}

function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

async function httpGet(urlPath) {
  return new Promise((resolve, reject) => {
    const req = http.get(BASE + urlPath, (res) => {
      const chunks = [];
      res.on('data', (c) => chunks.push(c));
      res.on('end', () =>
        resolve({ status: res.statusCode, headers: res.headers, body: Buffer.concat(chunks) })
      );
    });
    req.on('error', reject);
  });
}

async function main() {
  const samples = expectedSamples();
  console.log(`V4D wasm gallery test-suite`);
  console.log(`  web root   : ${ROOT}`);
  console.log(`  base url   : ${BASE}`);
  console.log(`  expected   : ${samples.length} demos`);

  // ---- boot the static server ----
  await new Promise((resolve) => serverMod.server.listen(serverMod.PORT, serverMod.HOST, resolve));
  console.log(`  server     : listening on ${BASE}`);

  // ---- 1. index lists every demo & excludes montage-demo ----
  const indexRes = await httpGet('/gallery/index.html');
  try {
    assert(indexRes.status === 200, 'index returned 200');
    const html = indexRes.body.toString();
    const countMatch = html.match(/data-count="(\d+)"/);
    assert(countMatch && Number(countMatch[1]) === samples.length,
      `index advertises ${countMatch && countMatch[1]} demos (expected ${samples.length})`);
    for (const s of samples) {
      assert(html.includes(`demos/${s}/`), `index links demo ${s}`);
    }
    assert(!html.includes('montage-demo'), 'montage-demo excluded from index');
    report('index-lists-all-demos', true, `${samples.length} demos, no montage-demo`);
  } catch (e) {
    report('index-lists-all-demos', false, e.message);
  }

  // ---- 2. every demo's assets are servable with correct MIME ----
  let allAssetsOk = true;
  for (const s of samples) {
    const js = await httpGet(`/bin/example_v4d_${s}.js`);
    const wasm = await httpGet(`/bin/example_v4d_${s}.wasm`);
    const jsOk = js.status === 200 && (js.headers['content-type'] || '').includes('javascript');
    const wasmOk = wasm.status === 200 && (wasm.headers['content-type'] || '').includes('wasm');
    if (!(jsOk && wasmOk)) {
      allAssetsOk = false;
      report('assets-servable', false, `${s} js=${js.status} wasm=${wasm.status}`);
    }
  }
  report('assets-servable', allAssetsOk);

  // ---- cross-origin isolation headers present ----
  const coRes = await httpGet('/bin/example_v4d_cube-demo.js');
  const coop = coRes.headers['cross-origin-opener-policy'];
  const coop2 = coRes.headers['cross-origin-embedder-policy'];
  report('cross-origin-isolation', coop === 'same-origin' && coop2 === 'require-corp',
    `COOP=${coop} COEP=${coop2}`);

  // ---- 3. load every demo page in headless Chromium ----
  let browser;
  try {
    browser = await chromium.launch({
      headless: true,
      args: [
        '--no-sandbox',
        '--disable-setuid-sandbox',
        '--enable-unsafe-swiftshader',
        '--use-gl=swiftshader',
        '--enable-webgl',
        '--use-angle=swiftshader',
        '--disable-gpu-sandbox',
      ],
    });
  } catch (e) {
    console.error('Failed to launch chromium: ' + e.message);
    serverMod.server.close();
    process.exit(2);
  }

  const context = await browser.newContext({ viewport: { width: 1280, height: 800 } });

  for (const s of samples) {
    const page = await context.newPage();
    const consoleErrors = [];
    const pageErrors = [];
    page.on('pageerror', (e) => pageErrors.push(String(e)));
    page.on('console', (msg) => {
      if (msg.type() === 'error') consoleErrors.push(msg.text());
    });

    let ok = true;
    const failures = [];
    try {
      await page.goto(`${BASE}/gallery/demos/${s}/index.html`, { waitUntil: 'domcontentloaded', timeout: 30000 });

      // wait for the wasm runtime to initialise (or fail fast on network 404)
      const initPath = `${BASE}/bin/example_v4d_${s}.js`;
      const initRespPromise = page.waitForResponse(
        (r) => r.url() === initPath, { timeout: 30000 }
      ).catch(() => null);
      const initResp = await initRespPromise;
      if (initResp && initResp.status() !== 200) {
        throw new Error(`sample module returned HTTP ${initResp.status()}`);
      }

      // give the module time to compile + initialise the runtime
      await page
        .waitForFunction(() => document.body.dataset.status === 'ready', null, { timeout: 45000 })
        .catch(() => {});

      const status = await page.evaluate(() => document.body.dataset.status);
      if (status !== 'ready') {
        throw new Error(`module did not reach 'ready', status=${status}`);
      }

      // verify a WebGL canvas is present and usable in this page
      const gl = await page.evaluate(() => {
        const c = document.getElementById('canvas');
        if (!c) return null;
        const ctx = c.getContext('webgl2') || c.getContext('webgl');
        return ctx ? ctx.getParameter ? String(ctx.getParameter(ctx.VERSION)) : 'context' : null;
      });
      if (!gl) throw new Error('no WebGL context created on canvas');

      // sanity: ensure module did not spam fatal errors
      const fatal = consoleErrors.filter(
        (t) => /fatal|Aborted|abort\(|RuntimeError|Assertion|oom/i.test(t)
      );
      if (fatal.length) throw new Error(`fatal console error: ${fatal.join(' | ').slice(0, 200)}`);

      if (pageErrors.length) throw new Error(`page errors: ${pageErrors.join(' | ').slice(0, 200)}`);
    } catch (e) {
      ok = false;
      failures.push(e.message);
    } finally {
      await page.close();
    }
    report(`demo-loads-${s}`, ok, failures.join(' | '));
  }

  await browser.close();
  serverMod.server.close();

  const passed = results.filter((r) => r.ok).length;
  const total = results.length;
  console.log(`\n${passed}/${total} checks passed`);
  if (passed !== total) process.exit(1);
}

main().catch((e) => {
  console.error(e);
  try { serverMod.server.close(); } catch (_) {}
  process.exit(1);
});
