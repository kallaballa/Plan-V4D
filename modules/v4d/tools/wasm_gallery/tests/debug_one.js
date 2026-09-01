#!/usr/bin/env node
const { chromium } = require('playwright');
const serverMod = require('../server.js');

const BASE = `http://${serverMod.HOST}:${serverMod.PORT}`;
const DEMO = process.env.DEMO || 'cube-demo';

(async () => {
  await new Promise(r => serverMod.server.listen(serverMod.PORT, serverMod.HOST, r));
  const browser = await chromium.launch({
    headless: true,
    args: ['--no-sandbox', '--enable-unsafe-swiftshader', '--use-gl=swiftshader',
           '--enable-webgl', '--use-angle=swiftshader'],
  });
  const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });

  page.on('console', m => console.log(`[console.${m.type()}] ${m.text()}`));
  page.on('pageerror', e => console.log(`[pageerror] ${e}\n${(e.stack || '').split('\n').slice(0,10).join('\n')}`));
  page.on('requestfailed', r => console.log(`[reqfail] ${r.url()} :: ${r.failure() && r.failure().errorText}`));
  page.on('close', () => console.log('[PAGE CLOSED] (program exited)'));

  await page.goto(`${BASE}/gallery/demos/${DEMO}/index.html`, { waitUntil: 'domcontentloaded', timeout: 30000 });
  console.log(`navigated (demo=${DEMO}); observing for 35s...`);

  // observe until the page closes itself or timeout elapses
  const closed = await new Promise((resolve) => {
    page.once('close', () => resolve(true));
    page.waitForTimeout(35000).then(() => resolve(false)).catch(() => resolve(true));
  });
  console.log('closed-by-program:', closed);

  if (!closed) {
    const state = await page.evaluate(() => ({
      status: document.body.dataset.status,
      statusText: document.getElementById('status').textContent,
      isolated: self.crossOriginIsolated,
      sab: typeof SharedArrayBuffer,
    })).catch(() => ({}));
    console.log('final state:', JSON.stringify(state));
  }

  await browser.close();
  serverMod.server.close();
  process.exit(0);
})().catch(e => { console.error(e); try { serverMod.server.close(); } catch(_){} process.exit(1); });
