// Run against a separate headless Chrome started with --remote-debugging-port=9333,
// while serving web/ without COOP or COEP response headers (e.g. python -m http.server).
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function main() {
  const gameTest = process.argv.includes('--game');
  const tabs = await (await fetch('http://127.0.0.1:9333/json')).json();
  const tab = tabs.find((x) => x.type === 'page');
  assert(tab, 'Chrome debugging endpoint has no page target');
  const socket = new WebSocket(tab.webSocketDebuggerUrl);
  const pending = new Map();
  let seq = 1;
  socket.addEventListener('message', ({ data }) => {
    const message = JSON.parse(data);
    if (pending.has(message.id)) { pending.get(message.id)(message); pending.delete(message.id); }
  });
  await new Promise((resolve, reject) => {
    socket.addEventListener('open', resolve, {once:true});
    socket.addEventListener('error', reject, {once:true});
  });
  const cdp = (method, params = {}) => new Promise((resolve) => {
    const id = seq++;
    pending.set(id, resolve);
    socket.send(JSON.stringify({id,method,params}));
  });
  try {
    await cdp('Page.enable');
    await cdp('Runtime.enable');
    await cdp('Page.navigate', {url: gameTest
      ? 'http://127.0.0.1:8991/?noshaderwarm&autostart'
      : 'http://127.0.0.1:8990/health.html'});
    if (gameTest) {
      let result;
      for (let attempt = 0; attempt < 60; attempt++) {
        await delay(500);
        const response = await cdp('Runtime.evaluate', {
          expression: '({isolated:crossOriginIsolated,status:document.querySelector("#status")?.textContent,workers:window.__rgh?.workers?.length||0,log:document.querySelector("#log")?.textContent.slice(-1600)})',
          returnByValue: true,
        });
        result = response.result?.result?.value;
        if (result?.workers > 0 || /Error|Could not load|Cannot start/.test(result?.status || '')) break;
      }
      console.log(result);
      assert(result?.isolated, 'The game page is not cross-origin isolated');
      assert(result?.workers > 0, 'The WASM game did not start a worker');
      await delay(10000);
      const late = await cdp('Runtime.evaluate', {
        expression: '({status:document.querySelector("#status")?.textContent,workers:window.__rgh?.workers?.length||0,log:document.querySelector("#log")?.textContent.slice(-2000)})',
        returnByValue: true,
      });
      console.log('Ten seconds later:', late.result?.result?.value);
      assert.equal(late.result?.result?.value?.status, 'Running', 'Game stopped or crashed after launch');
      const shot = await cdp('Page.captureScreenshot', {format: 'png', captureBeyondViewport: false});
      if (shot.result?.data) fs.writeFileSync(path.join(__dirname, '.game-smoke.png'), Buffer.from(shot.result.data, 'base64'));
      console.log('PASS Chrome actual WASM worker launched with local game partition');
      return;
    }
    let result;
    for (let attempt = 0; attempt < 20; attempt++) {
      await delay(500);
      const response = await cdp('Runtime.evaluate', {
        expression: '({url:location.href,isolated:crossOriginIsolated,controlled:!!navigator.serviceWorker.controller,text:document.querySelector("#results")?.innerText||""})',
        returnByValue: true,
      });
      result = response.result?.result?.value;
      if (result?.isolated && result?.controlled && result.text.includes('WASM asset: PASS')) break;
    }
    console.log(result);
    if (!result?.isolated) {
      const reg = await cdp('Runtime.evaluate', {
        expression: '(async()=>{try{const r=await navigator.serviceWorker.register("./coi-sw.js");return {secure:isSecureContext,scope:r.scope,active:r.active?.state,installing:r.installing?.state,waiting:r.waiting?.state}}catch(e){return {error:String(e),secure:isSecureContext,available:!!navigator.serviceWorker}}})()',
        awaitPromise: true, returnByValue: true,
      });
      console.log('Service worker registration diagnostics:', reg.result?.result?.value);
    }
    assert(result?.isolated, 'GitHub Pages-style service worker did not enable cross-origin isolation');
    assert(result?.controlled, 'Service worker did not control the page');
    assert.match(result.text, /WASM asset: PASS/);
    console.log('PASS Chrome static hosting, COOP/COEP service worker and WASM asset');
  } finally { socket.close(); }
}
main().catch((error) => { console.error(error); process.exitCode = 1; });
