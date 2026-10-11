// Run against a separate headless Chrome started with --remote-debugging-port=9333,
// while serving web/ without COOP or COEP response headers (e.g. python -m http.server).
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function main() {
  const gameTest = process.argv.includes('--game');
  const publishedGame = process.argv.includes('--published-game');
  const urlArg = process.argv.indexOf('--url');
  const customUrl = urlArg < 0 ? null : process.argv[urlArg + 1];
  const simulateWasmAbort = process.argv.includes('--simulate-wasm-abort');
  const syncProbe = process.argv.includes('--sync-probe');
  const publishedTest = process.argv.includes('--published');
  const r2Arg = process.argv.indexOf('--r2-base');
  const r2Base = r2Arg < 0 ? null : process.argv[r2Arg + 1];
  const r2ObjArg = process.argv.indexOf('--r2-object');
  const r2Object = r2ObjArg < 0 ? 'test/range-check.bin' : process.argv[r2ObjArg + 1];
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
  let injectionId = null;
  try {
    await cdp('Page.enable');
    await cdp('Runtime.enable');
    if (simulateWasmAbort) {
      const injected = await cdp('Page.addScriptToEvaluateOnNewDocument', {source: `
        (() => {
          const original = WebAssembly.compileStreaming.bind(WebAssembly);
          let calls = 0;
          WebAssembly.compileStreaming = function(...args) {
            if (++calls === 1) return Promise.reject(new DOMException('The operation was aborted.', 'AbortError'));
            return original(...args);
          };
          Object.defineProperty(window, '__wasmAbortCount', {get: () => calls});
        })();
      `});
      assert(!injected.error,'Could not install artificial AbortError in Chrome');
      injectionId = injected.result?.identifier || null;
    }
    await cdp('Page.navigate', {url: customUrl || (gameTest || publishedGame
      ? (publishedGame
        ? 'https://teaserlua.github.io/rabbids-go-home-web-port/?noshaderwarm&autostart'
        : 'http://127.0.0.1:8991/?noshaderwarm&autostart')
      : publishedTest || syncProbe
        ? 'https://teaserlua.github.io/rabbids-go-home-web-port/health.html'
        : 'http://127.0.0.1:8990/health.html')});
    if (syncProbe) {
      await delay(1800);
      const probe = await cdp('Runtime.evaluate', {
        expression: `(async () => {
          const script = \`onmessage=()=>{
            const paths=['game/sys/boot.bin','game/sys/fst.bin','game/sys/main.dol','game/files/RGH.BF.part00'];
            const results=[];
            for (const path of paths) {
              const url='https://rabbids-go-home-assets.jmhq20120212.workers.dev/'+path;
              try {
                const h=new XMLHttpRequest();h.open('HEAD',url,false);h.send();
                const g=new XMLHttpRequest();g.open('GET',url,false);g.responseType='arraybuffer';g.setRequestHeader('Range','bytes=0-262143');g.send();
                results.push({path,head:h.status,size:h.getResponseHeader('content-length'),range:g.status,bytes:g.response?.byteLength,contentRange:g.getResponseHeader('content-range')});
              } catch(error) {results.push({path,error:String(error)});}
            }
            postMessage(results);
          };\`;
          const u=URL.createObjectURL(new Blob([script],{type:'text/javascript'}));
          const w=new Worker(u);
          try { return await Promise.race([new Promise((resolve)=>{w.onmessage=e=>resolve(e.data);w.onerror=e=>resolve({error:e.message});w.postMessage(1)}),new Promise((resolve)=>setTimeout(()=>resolve({timeout:true}),45000))]); }
          finally {w.terminate();URL.revokeObjectURL(u);}
        })()`,
        returnByValue: true, awaitPromise: true,
      });
      console.log('Synchronous XHR in isolated worker:', probe.result?.result?.value);
      return;
    }
    if (gameTest || publishedGame) {
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
      let late;
      for (let sample = 0; sample < (publishedGame ? 18 : 12); sample++) {
        await delay(5000);
        late = await cdp('Runtime.evaluate', {
          expression: '({status:document.querySelector("#status")?.textContent,workers:window.__rgh?.workers?.length||0,frames:window.__renderer?.stats?.xfbCopies||0,info:document.querySelector("#stats")?.textContent,log:document.querySelector("#log")?.textContent.slice(-3000)})',
          returnByValue: true,
        });
        const value = late.result?.result?.value;
        console.log(`${(sample+1)*5}s after startup: workers=${value?.workers} status=${value?.status} ${value?.info}`);
        if (value?.workers > 2 && value.frames > 0) break;
      }
      console.log('Final:', late.result?.result?.value);
      if (simulateWasmAbort) {
        const recover = await cdp('Runtime.evaluate',{
          expression: '({count:window.__wasmAbortCount||0, log:document.querySelector("#log")?.textContent||""})',returnByValue:true,
        });
        const proof = recover.result?.result?.value;
        assert(proof?.count >= 2, 'WASM compilation was not retried after simulated AbortError');
        assert.match(proof.log, /\[wasm\] retry 1: The operation was aborted\./);
        console.log(`PASS simulated AbortError recovered in browser after ${proof.count} compilation attempts`);
      }
      assert.equal(late.result?.result?.value?.status, 'Running', 'Game stopped or crashed after launch');
      assert(late.result?.result?.value?.workers > 2, 'Game did not advance beyond startup');
      assert(late.result?.result?.value?.frames > 0, 'Game never rendered a frame');
      const shot = await cdp('Page.captureScreenshot', {format: 'png', captureBeyondViewport: false});
      if (shot.result?.data) fs.writeFileSync(path.join(__dirname, '.game-smoke.png'), Buffer.from(shot.result.data, 'base64'));
      console.log(`PASS Chrome actual WASM worker launched with ${publishedGame ? 'Cloudflare R2 game data' : 'local game partition'}`);
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
    if (r2Base) {
      const address = new URL(r2Object.split('/').map(encodeURIComponent).join('/'), r2Base.endsWith('/') ? r2Base : r2Base + '/').href;
      const probe = await cdp('Runtime.evaluate', {
        expression: `(async () => {
          const url = ${JSON.stringify(address)};
          try {
            const head = await fetch(url, {method:'HEAD'});
            const range = await fetch(url, {headers:{Range:'bytes=0-31'}});
            const data = new Uint8Array(await range.arrayBuffer());
            return {isolated:crossOriginIsolated,head:head.status,size:head.headers.get('content-length'),range:range.status,contentRange:range.headers.get('content-range'),length:data.length,first:data[0],last:data[data.length-1]};
          } catch(error) { return {error:String(error)}; }
        })()`,
        awaitPromise: true, returnByValue: true,
      });
      const got = probe.result?.result?.value;
      console.log('R2 in actual GitHub Pages isolated browser:', got);
      assert.equal(got?.head, 200);
      assert(Number(got?.size) >= 32, 'R2 Content-Length missing');
      assert.equal(got?.range, 206);
      assert.equal(got?.contentRange, `bytes 0-31/${got.size}`);
      assert.equal(got?.length, 32);
    }
    console.log('PASS Chrome static hosting, COOP/COEP service worker and WASM asset');
  } finally {
    if (injectionId) await cdp('Page.removeScriptToEvaluateOnNewDocument', {identifier:injectionId});
    socket.close();
  }
}
main().catch((error) => { console.error(error); process.exitCode = 1; });
