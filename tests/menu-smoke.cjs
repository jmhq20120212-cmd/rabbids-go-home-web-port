'use strict';
// Chrome must be running with --remote-debugging-port=9333 and the web files
// served locally on 8990. Checks actual images and where loading/DSU are shown.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const pause = (ms) => new Promise((done) => setTimeout(done, ms));
async function main() {
  const tabs = await (await fetch('http://127.0.0.1:9333/json')).json();
  const page = tabs.find((p) => p.type === 'page');
  assert(page);
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  const pending = new Map();
  let id = 0;
  ws.onmessage = ({data}) => {const m=JSON.parse(data);if(pending.has(m.id)){pending.get(m.id)(m);pending.delete(m.id);}};
  await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject;});
  const cdp = (method,params={})=>new Promise(resolve=>{const n=++id;pending.set(n,resolve);ws.send(JSON.stringify({id:n,method,params}));});
  const evalJS = async (expression) => {
    const r=await cdp('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});
    if(r.result?.exceptionDetails) throw new Error(JSON.stringify(r.result.exceptionDetails));
    return r.result?.result?.value;
  };
  try {
    await cdp('Page.enable'); await cdp('Runtime.enable');
    await cdp('Page.navigate',{url:process.argv[2]||'http://127.0.0.1:8990/'});
    let state;
    for(let i=0;i<40;i++) {
      await pause(350);
      state=await evalJS(`(() => {
        const p=(id)=>document.querySelector(id), ok=(id)=>!!p(id)?.complete && !!p(id)?.naturalWidth;
        return {ready:document.readyState,background:getComputedStyle(p('#title')).backgroundImage.includes('fondo.jpg'),
          images:[ok('#start img'),ok('#t-settings img'),ok('#t-credits img'),ok('#barbox img'),ok('.tab[data-pane="p-dsu"] img')],
          loaderHidden:getComputedStyle(p('#loader')).visibility==='hidden',menuVisible:getComputedStyle(p('#menu')).opacity==='1'};
      })()`);
      if(state?.ready==='complete' && state.images.every(Boolean))break;
    }
    console.log('Initial menu:',state);
    assert(state.images.every(Boolean),'Menu artwork did not all load');
    assert(state.background,'Background is not configured');
    assert(state.loaderHidden,'Loading bar should be hidden before Play');
    assert(state.menuVisible,'Menu should be visible');
    const shot=async (name)=>{const r=await cdp('Page.captureScreenshot',{format:'png'});if(r.result?.data)fs.writeFileSync(path.join(__dirname,name),Buffer.from(r.result.data,'base64'));};
    await pause(900);
    await shot('.menu-preview.png');
    await evalJS(`document.querySelector('#t-settings').click()`);
    const settings=await evalJS(`({visible:!document.querySelector('#controls').hidden,dsuIconLoaded:document.querySelector('.tab[data-pane="p-dsu"] img').naturalWidth>0,dsuLabel:document.querySelector('.tab[data-pane="p-dsu"]').textContent.trim()})`);
    console.log('Settings:',settings);
    assert(settings.visible && settings.dsuIconLoaded && settings.dsuLabel==='DSU Remote');
    await evalJS(`document.querySelector('.tab[data-pane="p-dsu"]').click()`);
    assert(await evalJS(`!document.querySelector('#p-dsu').hidden`),'DSU pane did not open');
    await pause(350);
    await shot('.menu-settings.png');
    await evalJS(`document.querySelector('#ctl-close').click()`);
    await evalJS(`document.querySelector('#start').click()`);
    let loading;
    for(let i=0;i<50;i++){
      await pause(200);
      loading=await evalJS(`({loading:document.querySelector('#title').classList.contains('loading'),barVisible:getComputedStyle(document.querySelector('#loader')).visibility==='visible',image:document.querySelector('#barbox img').naturalWidth>0})`);
      if(loading?.loading && loading?.barVisible)break;
    }
    console.log('After Play:',loading);
    assert(loading?.loading && loading?.barVisible && loading.image,'Bar must show only after Play');
    await shot('.menu-loading.png');
    console.log('PASS menu: supplied images loaded, DSU in Settings, bar only after Play');
  } finally { ws.close(); }
}
main().catch(e=>{console.error(e);process.exitCode=1;});
