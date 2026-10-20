// Screenshots of the web portal served by .pio/host_portal, via headless Chrome
// (DevTools protocol). Usage: node --experimental-websocket tools/portal_shots.mjs <code> <outdir>
import { spawn } from 'node:child_process';
import { writeFileSync, mkdirSync } from 'node:fs';
const [code, out = 'docs/portal'] = process.argv.slice(2);
mkdirSync(out, { recursive: true });
const chrome = spawn('google-chrome', ['--headless=new', '--no-sandbox', '--disable-gpu', '--remote-debugging-port=9333',
  '--host-resolver-rules=MAP sticky-med.local:80 127.0.0.1:8080', '--user-data-dir=/tmp/portal-shots', 'about:blank'], { stdio: 'ignore' });
const sleep = ms => new Promise(r => setTimeout(r, ms));
let targets;
for (let i = 0; i < 50; i++) { try { targets = await (await fetch('http://127.0.0.1:9333/json/list')).json(); if (targets.length) break; } catch (e) {} await sleep(200); }
const page = targets.find(t => t.type === 'page');
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pending = new Map();
ws.onmessage = m => { const j = JSON.parse(m.data); if (j.id && pending.has(j.id)) { pending.get(j.id)(j); pending.delete(j.id); } };
const send = (method, params = {}) => new Promise(r => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const js = async expr => (await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
await send('Page.enable'); await send('Runtime.enable');
await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
async function shot(name, full) {
  await sleep(400);
  let params = { format: 'png' };
  if (full) {
    const h = await js('document.documentElement.scrollHeight');
    params = { format: 'png', captureBeyondViewport: true, clip: { x: 0, y: 0, width: 390, height: Math.max(844, h), scale: 1 } };
  }
  const r = await send('Page.captureScreenshot', params);
  writeFileSync(`${out}/${name}.png`, Buffer.from(r.result.data, 'base64'));
  console.log('saved', name);
}
await send('Page.navigate', { url: 'http://sticky-med.local/' }); await sleep(1200);
await shot('portal_1_login');
await js(`document.querySelector('#code').value='${code}'; document.querySelector('#go').click(); 1`); await sleep(800);
await shot('portal_2_medications', true);
await js(`document.querySelector('[data-e]').click(); 1`); await sleep(400);
await shot('portal_3_edit_medication', true);
await js(`document.querySelector('#save').click(); 1`); await sleep(600);  // round trip through the API (saves unchanged)
await js(`document.querySelector('[data-t=people]').click(); document.querySelector('#add').click(); document.querySelector('#p-name').value='Grandpa Joe'; document.querySelector('#save').click(); 1`); await sleep(600);
await js(`document.querySelector('[data-t=people]').click(); 1`);
await shot('portal_4_people');
await js(`document.querySelector('[data-t=history]').click(); 1`); await sleep(500);
await shot('portal_5_history');
await js(`document.querySelector('[data-t=data]').click(); 1`);
await shot('portal_6_data', true);
await js(`document.querySelector('[data-t=more]').click(); 1`); await sleep(500);
await shot('portal_7_more', true);
ws.close(); chrome.kill();
process.exit(0);
