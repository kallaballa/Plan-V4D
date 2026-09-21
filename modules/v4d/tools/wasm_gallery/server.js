#!/usr/bin/env node
/**
 * Static server for the V4D WebAssembly gallery.
 *
 * Serves the build_wasm directory as the web root so that
 *   /gallery/index.html  (generated gallery)
 * and
 *   /bin/example_v4d_*.js / .wasm  (built samples)
 * are both reachable.
 *
 * Cross-origin isolation is REQUIRED for the pthread/SharedArrayBuffer wasm
 * builds, so we send COOP/COEP headers.
 */
'use strict';

const http = require('http');
const fs = require('fs');
const path = require('path');

const ROOT = process.env.GALLERY_ROOT
  ? path.resolve(process.env.GALLERY_ROOT)
  : '/home/elchaschab/devel/opencv/build_wasm';
const PORT = Number(process.env.GALLERY_PORT || 8090);
const HOST = process.env.GALLERY_HOST || '127.0.0.1';

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.wasm': 'application/wasm',
  '.json': 'application/json',
  '.css': 'text/css; charset=utf-8',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.jpeg': 'image/jpeg',
  '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon',
  '.txt': 'text/plain',
  '.webm': 'video/webm',
  '.ttf': 'font/ttf',
  '.map': 'application/json',
};

function resolvePath(urlPath) {
  const decoded = decodeURIComponent(urlPath.split('?')[0]);
  let p = path.normalize(path.join(ROOT, decoded));
  if (!p.startsWith(ROOT)) return null;
  if (fs.existsSync(p) && fs.statSync(p).isDirectory()) {
    p = path.join(p, 'index.html');
  }
  return p;
}

const server = http.createServer((req, res) => {
  if (req.method !== 'GET' && req.method !== 'HEAD') {
    res.writeHead(405).end('Method Not Allowed');
    return;
  }
  const file = resolvePath(req.url);
  if (!file || !fs.existsSync(file) || !fs.statSync(file).isFile()) {
    // Emscripten occasionally probes e.g. with HEAD; simple 404 otherwise
    res.writeHead(404, { 'Content-Type': 'text/plain' }).end('Not Found');
    return;
  }
  const ext = path.extname(file).toLowerCase();
  const header = {
    'Content-Type': MIME[ext] || 'application/octet-stream',
    // cross-origin isolation for SharedArrayBuffer (pthread wasm)
    'Cross-Origin-Opener-Policy': 'same-origin',
    'Cross-Origin-Embedder-Policy': 'require-corp',
    'Cache-Control': 'no-store',
    'Access-Control-Allow-Origin': '*',
  };
  const data = fs.readFileSync(file);
  res.writeHead(200, header);
  res.end(req.method === 'HEAD' ? undefined : data);
});

if (require.main === module) {
  server.listen(PORT, HOST, () => {
    console.log(`V4D gallery server: http://${HOST}:${PORT}/`);
    console.log(`  web root: ${ROOT}`);
  });
}

module.exports = { server, ROOT, PORT, HOST };
