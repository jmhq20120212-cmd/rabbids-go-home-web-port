// rgh-host.js - page side: loads the recompiled game (rgh.wasm), creates the shared memory and one
// worker per guest fiber, prepares every shader on the loading screen, then runs the game and presents frames.
'use strict';

const logEl = document.getElementById('log');
const statusEl = document.getElementById('status');
const statsEl = document.getElementById('stats');
const fpsEl = document.getElementById('fps');
const localLauncher = ['localhost', '127.0.0.1', '[::1]'].includes(location.hostname);
const lines = [];
const T0 = performance.now();
// the log also goes to the launcher, which saves it as rabbids-log.txt (to look at a crash afterwards)
const logOut = []; let logNew = true, logBusy = false;
function logFlush() {
  if (!localLauncher || logBusy || !logOut.length || location.protocol === 'file:') return;
  logBusy = true;
  const body = logOut.splice(0).join('\n') + '\n';
  fetch('log' + (logNew ? '?new=1' : ''), { method: 'POST', body, keepalive: body.length < 60000 }).catch(() => {}).finally(() => { logBusy = false; });
  logNew = false;
}
setInterval(logFlush, 2000);
addEventListener('pagehide', logFlush);
document.addEventListener('visibilitychange', () => log(`[page] tab ${document.visibilityState === 'visible' ? 'visible' : 'hidden'}`));
function log(text) {
  lines.push(text); if (lines.length > 400) lines.splice(0, lines.length - 400);
  logEl.textContent = lines.join('\n'); logEl.scrollTop = logEl.scrollHeight;
  if (window.__rghLog) window.__rghLog(text);
  if (localLauncher && (!/^\[OSReport\]/.test(text) || logOut.length < 20000)) logOut.push(`[${((performance.now() - T0) / 1000).toFixed(1)}] ${text}`);
}
addEventListener('keydown', (e) => { if (e.code === 'F9' && window.__dbgReq) { e.preventDefault(); window.__dbgReq('F9'); UI.toast('Game state saved to the log'); } });
addEventListener('error', (e) => { log(`[page] error: ${e.message} (${e.filename}:${e.lineno})`); logFlush(); });
addEventListener('unhandledrejection', (e) => { log(`[page] promesa rechazada: ${e.reason && (e.reason.stack || e.reason.message) || e.reason}`); logFlush(); });
log(`[page] ${navigator.userAgent} · ${navigator.hardwareConcurrency} cores · ${Math.round((navigator.deviceMemory || 0))} GB`);

const params = new URLSearchParams(location.search);
const env = {};
for (const [k, v] of params) if (k.startsWith('RGH_')) env[k] = v;
const NORENDER = params.has('norender');

// Public static hosting: use a directory picked from the visitor's own disc.
// The File objects stay in the browser and are cloned to workers as handles;
// their gigabytes of contents are only read in small slices on demand.
let localGameFiles = null;
const gameStatus = document.getElementById('game-status');
const chooseGame = document.getElementById('choose-game');
const folderInput = document.getElementById('game-folder');
chooseGame.onclick = () => folderInput.click();
folderInput.onchange = () => {
  const files = Array.from(folderInput.files);
  const paths = files.map((file) => (file.webkitRelativePath || file.name).replace(/\\/g, '/'));
  const candidates = paths.filter((path) => /(^|\/)sys\/boot\.bin$/i.test(path));
  const prefix = candidates.find((path) => paths.includes(path.slice(0, -'sys/boot.bin'.length) + 'files/LynWiiRetail.elf'))
    || candidates[0];
  if (!prefix) { localGameFiles = null; gameStatus.textContent = 'No sys/boot.bin found. Select the extracted entire partition folder.'; return; }
  const root = prefix.slice(0, -'sys/boot.bin'.length);
  localGameFiles = files.flatMap((file, index) => paths[index].startsWith(root)
    ? [['game/' + paths[index].slice(root.length), file]] : []);
  if (!localGameFiles.some(([path]) => path === 'game/sys/fst.bin') ||
      !localGameFiles.some(([path]) => path === 'game/sys/main.dol')) {
    localGameFiles = null;
    gameStatus.textContent = 'The folder is missing sys/fst.bin or sys/main.dol. Select the extracted partition, not an RVZ/ISO file.';
    return;
  }
  const total = localGameFiles.reduce((n, [, file]) => n + file.size, 0);
  gameStatus.textContent = `${localGameFiles.length} local files ready (${(total / 1073741824).toFixed(2)} GiB). Press Play.`;
};

const assetBasePromise = fetch('game-config.json', { cache: 'no-store' })
  .then(async (response) => response.ok ? await response.json() : null)
  .then((config) => {
    if (!config || typeof config.assetBase !== 'string') return null;
    const url = new URL(config.assetBase, location.href);
    if (!['https:', 'http:'].includes(url.protocol) || (location.protocol === 'https:' && url.protocol !== 'https:')) return null;
    if (!url.pathname.endsWith('/')) url.pathname += '/';
    gameStatus.textContent = 'Hosted game data configured. You can press Play directly.';
    chooseGame.textContent = 'Use a local game folder (optional)';
    return url.href;
  }).catch((e) => { console.warn('Game data configuration:', e); return null; });

// rgh.wasm.gz (preferred, smaller) or rgh.wasm
async function loadModule() {
  if (params.get('wasm')) return WebAssembly.compileStreaming(fetch(params.get('wasm')));   // debugging builds
  const gz = await fetch('rgh.wasm.gz', { cache: 'no-cache' }).catch(() => null);
  if (gz && gz.ok && typeof DecompressionStream === 'function') {
    const body = gz.body.pipeThrough(new DecompressionStream('gzip'));
    return WebAssembly.compileStreaming(new Response(body, { headers: { 'Content-Type': 'application/wasm' } }));
  }
  return WebAssembly.compileStreaming(fetch('rgh.wasm'));
}
// the game code and the save data start loading as soon as the page opens (while the title screen shows)
let t0Module = performance.now();
const modulePromise = crossOriginIsolated ? loadModule().then((m) => { log(`[host] module compiled in ${((performance.now() - t0Module) / 1000).toFixed(1)} s`); return m; }) : Promise.reject(new Error('not cross-origin isolated'));
modulePromise.catch(() => { /* reported on Play */ });
const nandPromise = nandLoad();

// ---- renderer: created on the loading screen so that every shader is compiled before the game starts
let renderer = null;
function makeRenderer() {
  if (renderer || NORENDER) return renderer;
  const canvas = document.getElementById('view');
  const ws = env.RGH_WIDESCREEN !== '0';           // the console is set to 16:9 unless ?RGH_WIDESCREEN=0
  if (!ws) document.body.classList.add('ar43');
  canvas.width = ws ? 1706 : 1280; canvas.height = 960;
  try { renderer = new GXRenderer(canvas, null, 0, { scale: Math.max(1, Math.min(3, Number(env.RGH_SCALE || UI.opts.scale) || 2)) }); }
  catch (e) { log('[render] ' + e.message); return null; }
  renderer.onError = (e) => log('[render] ' + (e.stack || e.message || e));
  try { const gl = renderer.gl, d = gl.getExtension('WEBGL_debug_renderer_info'); log(`[render] GPU: ${d ? gl.getParameter(d.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER)} · parallel compile: ${renderer.parallel ? 'yes' : 'no'} · scale ${renderer.scale}`); } catch (e) { /* no info */ }
  canvas.addEventListener('webglcontextlost', (e) => { e.preventDefault(); log('[render] WebGL CONTEXT LOST! (the graphics card or its driver was reset)'); logFlush(); UI.toast('The graphics card was reset: reload the page (F5) to continue'); });
  renderer.uberOnly = UI.opts.shaderMode === 'uber';
  if (!renderer.parallel) renderer.compilePerFrame = 1;   // without KHR_parallel_shader_compile (Firefox) every compile blocks
  window.__renderer = renderer;
  log('[render] WebGL2 ready');
  return renderer;
}

// ---- shader cache: the register state of every shader seen is kept in IndexedDB; shipped ones in shaders.bin
async function shaderList() {
  const all = [];
  try {
    const rsp = await fetch('shaders.bin', { cache: 'no-cache' });
    if (rsp.ok) { const b = await rsp.arrayBuffer(); for (let o = 0; o + 353 * 4 <= b.byteLength; o += 353 * 4) all.push(b.slice(o, o + 353 * 4)); }
  } catch (e) { /* optional */ }
  all.ownStart = all.length;
  for (const b of await UI.ownShaders()) all.push(b);
  return all;
}
// this browser's own shader list kept only states that give new code (it used to keep every state: tens of
// thousands that compiled to the same few thousand programs, and loading got slower every session)
async function compactOwnShaders(keep) {
  try {
    const db = await UI.shaderDB(), tx = db.transaction('s', 'readwrite'), os = tx.objectStore('s');
    os.clear(); keep.forEach((b, i) => os.put(b, 'u' + i));
    await new Promise((res) => { tx.oncomplete = res; tx.onerror = res; });
    log(`[render] browser shader list compacted to ${keep.length}`);
  } catch (e) { /* keep the old list */ }
}
async function keepNewShaders(r) {
  let db = null;
  try { db = await UI.shaderDB(); } catch (e) { return; }
  const pending = [];
  r.onNewProgram = (key, st) => { pending.push([key, st.buffer]); };
  setInterval(() => {
    if (!pending.length) return;
    try { const tx = db.transaction('s', 'readwrite'), os = tx.objectStore('s'); for (const [k, v] of pending.splice(0)) os.put(v, k); } catch (e) { /* ignore */ }
  }, 2000);
}
// Compile everything: hand every shader to the driver (it compiles them on its own threads), then draw with each
// finished one once so the driver also builds its final GPU code. onProgress(fraction, text). Resolves when done.
function prepareShaders(r, all, onProgress) {
  return new Promise((resolve) => {
    if (r.uberOnly) { onProgress(1, 'Mode: generic shader only'); resolve(); return; }
    let i = 0, j = 0; const progs = [], seen = new Set(), ownKeep = [], t00 = performance.now();
    const step = () => {
      const t0 = performance.now();
      while (i < all.length && performance.now() - t0 < 12) {
        const idx = i, st = new Uint32Array(all[i++]); const pr = r.warm(st);
        if (pr && !seen.has(pr)) { seen.add(pr); progs.push([pr, st[0]]); if (idx >= (all.ownStart ?? all.length)) ownKeep.push(all[idx]); }
      }
      while (j < progs.length && performance.now() - t0 < 24) { try { if (!r.prime(progs[j][0], progs[j][1])) break; } catch (e) { /* broken entry */ } j++; }
      const frac = 0.35 * (i / Math.max(1, all.length)) + 0.65 * (i < all.length ? 0 : j / Math.max(1, progs.length));
      onProgress(frac, i < all.length ? `Compiling shaders ${i} / ${all.length}` : `Preparing shaders on the GPU ${j} / ${progs.length}`);
      if (i < all.length || j < progs.length) setTimeout(step, 0);
      else {
        log(`[render] ${all.length} states → ${progs.length} shaders ready in ${((performance.now() - t00) / 1000).toFixed(1)} s`);
        const own = all.length - (all.ownStart ?? all.length);
        if (own > ownKeep.length * 1.2 + 50) compactOwnShaders(ownKeep);
        resolve();
      }
    };
    step();
  });
}

// ---- Play: loading screen, then the game
let started = false;
async function play() {
  if (started) return;
  const remoteBase = await assetBasePromise;
  if (!localGameFiles && !remoteBase && !localLauncher) {
    gameStatus.textContent = 'Choose the extracted game folder to play; no hosted game data is configured.';
    folderInput.click();
    return;
  }
  if (!localGameFiles && remoteBase) {
    try {
      const file = new URL('game/sys/boot.bin', remoteBase);
      const head = await fetch(file.href, { method: 'HEAD', cache: 'no-store' });
      if (!head.ok || !Number(head.headers.get('Content-Length'))) throw new Error('HEAD must expose Content-Length');
      const sample = await fetch(file.href, { headers: { Range: 'bytes=0-31' }, cache: 'no-store' });
      if (sample.status !== 206 || !sample.headers.get('Content-Range')) throw new Error('GET Range must return 206 and expose Content-Range');
    } catch (error) {
      gameStatus.textContent = `Game data server unavailable: ${error.message}. Select your local extracted folder instead.`;
      log(`[hosting] ${error.stack || error}`);
      return;
    }
  }
  started = true;
  primeAudio();
  if (!params.has('autostart') && !NORENDER) UI.fullscreen(true);
  const title = document.getElementById('title');
  title.classList.add('loading');
  const P = (f, t, s) => UI.progress(f, t, s);
  if (!crossOriginIsolated) { P(0, 'Cannot start', 'Browser is preparing cross-origin isolation. Reload the page and try again.'); log('ERROR: the page is not cross-origin isolated (COOP/COEP).'); started = false; return; }
  P(0.02, 'Loading…', 'Preparing the game');
  let skip = false;
  const skipBtn = document.getElementById('skip');
  const skipT = setTimeout(() => { skipBtn.style.display = 'inline-block'; }, 60000);
  skipBtn.onclick = () => { skip = true; };
  let module;
  try { module = await modulePromise; } catch (e) { P(0, 'Could not load the game', String(e.message || e)); return; }
  const nand = await nandPromise;
  P(0.08, 'Loading…', 'Reading shaders');
  const r = makeRenderer();
  if (r) {
    try { if (!r.uber) { r.uber = r.buildUber(); r.syncFinish(r.uber); } } catch (e) { log('[render] ubershader: ' + e.message); }
    keepNewShaders(r);
    const all = params.has('noshaderwarm') ? [] : await shaderList();
    log(`[render] precompiling ${all.length} shader states…`);
    await Promise.race([
      prepareShaders(r, all, (f, s) => P(0.08 + 0.9 * f, `Loading… ${Math.round((0.08 + 0.9 * f) * 100)}%`, s + (r.parallel ? '' : ' · loads and runs smoother in Chrome or Edge'))),
      new Promise((res) => { const k = setInterval(() => { if (skip) { clearInterval(k); res(); } }, 100); }),
    ]);
  }
  clearTimeout(skipT);
  if (remoteBase && !localGameFiles && !NORENDER) {
    // HTTP game data has a higher first-start latency than the local server.
    // Keep the loading screen up until the first actual rendered frame rather
    // than showing a black screen while the first disc blocks are downloaded.
    P(1, 'Starting the game…', 'Reading game files from Cloudflare R2');
    runGame(module, nand, remoteBase);
    await new Promise((resolve) => {
      const startedAt = performance.now();
      const id = setInterval(() => {
        if ((r && r.stats && (r.stats.xfbCopies || 0) > 0) ||
            statusEl.textContent !== 'Running' || performance.now() - startedAt > 90000) {
          clearInterval(id); resolve();
        }
      }, 100);
    });
  } else {
    P(1, 'Ready!', '');
    await new Promise((res) => setTimeout(res, 350));
    runGame(module, nand, remoteBase);
  }
  title.classList.add('gone');
  setTimeout(() => { title.hidden = true; }, 800);
  document.body.classList.add('playing');
}

function runGame(module, nand, remoteBase) {
  const memory = new WebAssembly.Memory({ initial: 4096, maximum: 16384, shared: true });
  const tidBuf = new Int32Array(new SharedArrayBuffer(4));
  if (nand) log(`[nand] saved game loaded (${nand.byteLength} bytes)`);
  const cfg = { base: remoteBase || new URL('./', location.href).href, env, nand, localFiles: localGameFiles };
  const workers = [];
  function spawn(tid, arg) {
    const w = new Worker('rgh-worker.js');
    w.onmessage = onMsg;
    w.onerror = (e) => { log(`[host] worker ${tid} error: ${e.message}`); logFlush(); };
    w.postMessage({ type: 'start', memory, module, cfg, tidBuf, tid, arg });
    workers.push(w);
  }
  function onMsg(e) {
    const m = e.data;
    if (m.type === 'log') log(m.text);
    else if (m.type === 'spawn') spawn(m.tid, m.arg);
    else if (m.type === 'ring') startRenderer(memory, m.addr);
    else if (m.type === 'dbg') { window.__dbgReq = (why) => { log(`[threads] dump requested (${why})`); Atomics.store(new Int32Array(memory.buffer, m.addr, 1), 0, 1); }; }
    else if (m.type === 'audio') startAudio(memory, m);
    else if (m.type === 'nand') nandSave(m.data);
    else if (m.type === 'pad' && !params.has('RGH_AUTOPRESS')) window.__input = new PadInput(document.getElementById('view'), memory, m.addr);
    else if (m.type === 'exit') { log(`[host] exit(${m.code})`); statusEl.textContent = 'Stopped'; logFlush(); UI.crashed(`The game stopped (code ${m.code}).`); }
    else if (m.type === 'crash') { log(`[host] hilo ${m.tid}: ${m.error}`); statusEl.textContent = 'Error (see log)'; logFlush(); UI.crashed('The game hit an error.'); }
  }
  spawn(0, 0);
  window.__rgh = { memory, workers };
  statusEl.textContent = 'Running';
  setInterval(() => {
    const r = window.__renderer;
    let perf = '', fpsLine = '';
    if (r && r.stats) {   // pictures shown per second, draw calls per picture, objects drawn with the ubershader
      const st = r.stats, p = statsEl._p || {}, xf = st.xfbCopies || 0, calls = st.calls || 0, pend = st.pending || 0;
      if (p.t) {
        const dt = (performance.now() - p.t) / 1000, nf = xf - p.xf, fps = nf / dt;
        perf = `${fps.toFixed(0)} fps · ${nf ? Math.round((calls - p.calls) / nf) : 0} calls/frame · `;
        fpsLine = `${fps.toFixed(0)} FPS`;
        if (pend > p.pend) { perf += 'compiling shaders · '; fpsLine += '  · new shader'; }
      }
      statsEl._p = { t: performance.now(), xf, calls, pend };
    }
    if (r && r.stats && window.__dbgReq) {
      const st = r.stats, q = statsEl._q || (statsEl._q = { n: 0, done: 0 });
      const lowDraws = q.d !== undefined && (st.draws - q.d) < 20 * Math.max(1, (st.xfbCopies | 0) - q.c), sameTex = q.t === st.texUploads;
      const quiet = !window.__audioLevel || window.__audioLevel() < 20;
      q.n = lowDraws && sameTex && quiet ? q.n + 1 : 0;
      if (q.n === 40 && q.done < 3) { q.done++; window.__dbgReq('looks stuck: still picture and no sound for 40 s'); }
      q.d = st.draws; q.c = st.xfbCopies | 0; q.t = st.texUploads;
      // total silence for 4 s while the game is running (a cutscene without its sound?): note the audio state once
      // a minute at most, so the log says what was playing
      q.s = quiet ? (q.s | 0) + 1 : 0;
      if (q.s === 4 && (!q.ls || performance.now() - q.ls > 60000) && (q.ns = (q.ns | 0) + 1) <= 20) { q.ls = performance.now(); window.__dbgReq('4 s of silence'); }
    }
    if (!(statsEl._n = (statsEl._n | 0) + 1) || statsEl._n % 10 === 0) {
      const st = r && r.stats ? r.stats : {};
      let ring = '';
      if (r && r.memory && r.ring) { const hh = new Int32Array(r.memory.buffer, r.ring, 8); ring = ` ring=${((Atomics.load(hh, 0) - Atomics.load(hh, 1)) >>> 0) >> 10} KB`; }
      if (window.__audioStat) log('[status] ' + window.__audioStat());
      log(`[status] ${perf}${ring} draws=${st.draws | 0} copies=${st.xfbCopies | 0} skipped=${st.skipped | 0} generic=${st.pending | 0} shaders=${st.progs | 0} new=${st.newShaders | 0} textures=${st.texUploads | 0} memory=${Math.round(memory.buffer.byteLength / 1048576)} MB threads=${workers.length}`);
    }
    statsEl.textContent = perf + `${workers.length} threads` + (r && r.stats ? ` · ${r.stats.draws} draws · ${r.stats.progs} shader states · ${r.stats.newShaders || 0} new · ${r.stats.texUploads} textures` : '');
    if (fpsEl && !fpsEl.hidden) fpsEl.textContent = fpsLine;
  }, 1000);
}
document.getElementById('start').onclick = () => { UI.pop(660); play(); };
if (params.has('autostart')) play();

function startRenderer(memory, addr) {
  if (NORENDER) {   // performance tests: drop the GPU stream, only count frames
    const h = new Int32Array(memory.buffer, addr, 8), u32 = new Uint32Array(memory.buffer), size = h[3] >>> 0;
    const st = { frames: 0, draws: 0, progs: 0, texUploads: 0 }; window.__renderer = { stats: st }; Atomics.store(h, 2, 1);
    setInterval(() => { let r = Atomics.load(h, 1) >>> 0; const w = Atomics.load(h, 0) >>> 0;
      while (r !== w) { const b = (addr + 32 + (r % size)) >> 2, hdr = u32[b], t = hdr >>> 24; if (t === 8) { st.frames++; if (st.pend) { st.pend = 0; st.flips = (st.flips || 0) + 1; } } else if (t === 3) st.draws++; else if (t === 7 && u32[b + 1] === 0) { st.xfb = (st.xfb || 0) + 1; st.pend = 1; } r = (r + (hdr & 0xFFFFFF) * 4) >>> 0; }
      Atomics.store(h, 1, r | 0); Atomics.notify(h, 1); }, 2);
    return;
  }
  const r = makeRenderer();
  if (!r) return;
  if (!r.uber) { try { r.uber = r.buildUber(); } catch (e) { log('[render] ubershader: ' + e.message); } }
  if (!r.onNewProgram) keepNewShaders(r);
  r.attach(memory, addr);
  // the loop must survive an error in one frame (before, one exception stopped the picture for good: 0 fps)
  let nerr = 0, lastRaf = performance.now(), lastPump = performance.now();
  const pumpSafe = (budget) => {
    lastPump = performance.now();
    try { r.pump(budget); }
    catch (e) { if (nerr++ < 30) log('[render] error in the loop: ' + (e.stack || e.message || e)); r.batch = null; r.invalidate(); }
  };
  const loop = () => { lastRaf = performance.now(); pumpSafe(14); requestAnimationFrame(loop); };
  // Watchdog: if the browser stops calling requestAnimationFrame (it does for hidden or covered windows, and
  // sometimes after a fullscreen change) the game would wait for the picture forever: keep the stream moving.
  // It also writes to the log what the picture stream was doing if it ever stops advancing.
  let lastR = -1, stuck = 0;
  setInterval(() => {
    const now = performance.now();
    if (now - lastRaf > 250 && now - lastPump > 100) pumpSafe(30);
    const hh = new Int32Array(memory.buffer, addr, 8), w = Atomics.load(hh, 0) >>> 0, rd = Atomics.load(hh, 1) >>> 0;
    if (w !== rd && rd === lastR) {
      if (++stuck === 30) {
        const off = rd % (hh[3] >>> 0), hdr = new Uint32Array(memory.buffer, addr + 32 + off, 4);
        log(`[render] drawing is not advancing! pending=${((w - rd) >>> 0) >> 10} KB record=${(hdr[0] >>> 24)}/${hdr[0] & 0xFFFFFF} ${hdr[1]} ${hdr[2]} raf=${Math.round(now - lastRaf)} ms pump=${Math.round(now - lastPump)} ms visible=${document.visibilityState} contexto=${r.gl.isContextLost() ? 'perdido' : 'ok'}`);
      }
    } else stuck = 0;
    lastR = rd;
  }, 100);
  requestAnimationFrame(loop);
}

let audioCtx = null;
function primeAudio() {   // must be created during a user gesture
  try { audioCtx = new AudioContext({ sampleRate: 32000, latencyHint: 'interactive' }); } catch (e) { log('[audio] ' + e.message); }
}
async function startAudio(memory, m) {
  if (!audioCtx) { log('[audio] no audio context (press Play to enable sound)'); return; }
  try {
    await audioCtx.audioWorklet.addModule('audio-worklet.js');
    const node = new AudioWorkletNode(audioCtx, 'rgh-audio', { outputChannelCount: [2], processorOptions: { memory, ring: m.ring, len: m.len, wpos: m.wpos, rpos: m.rpos } });
    node.connect(audioCtx.destination);
    // audio level and underruns, summarised every 10 s in the log
    const au = { rms: [], dry: 0, drop: 0, buf: 0 };
    node.port.onmessage = (e) => { const d = e.data; au.last = d.rms; au.rms.push(d.rms); au.dry += d.dry; au.drop += d.drop; au.buf = d.buf; au.target = d.target; };
    window.__audioLevel = () => au.last | 0;
    window.__audioStat = () => { const t = `audio: level ${au.rms.length ? Math.max(...au.rms) : 0} (min ${au.rms.length ? Math.min(...au.rms) : 0}) underruns=${au.dry} drops=${au.drop} buffer=${au.buf}/${au.target} ms state=${audioCtx.state}`; au.rms = []; au.dry = 0; au.drop = 0; return t; };
    new Int32Array(memory.buffer, m.on, 1)[0] = 1;
    if (audioCtx.state !== 'running') await audioCtx.resume();
    log(`[audio] ${audioCtx.sampleRate} Hz`);
  } catch (e) { log('[audio] ' + e.message); }
}

// ---- NAND (save data, settings) persisted in IndexedDB of this browser
function idb() {
  return new Promise((res, rej) => { const r = indexedDB.open('rgh-web', 1); r.onupgradeneeded = () => r.result.createObjectStore('nand'); r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error); });
}
async function nandLoad() {
  try { const db = await idb(); return await new Promise((res) => { const q = db.transaction('nand').objectStore('nand').get('image'); q.onsuccess = () => res(q.result || null); q.onerror = () => res(null); }); }
  catch (e) { return null; }
}
async function nandSave(buf) {
  try { const db = await idb(); db.transaction('nand', 'readwrite').objectStore('nand').put(buf, 'image'); log(`[nand] saved (${buf.byteLength} bytes)`); }
  catch (e) { log('[nand] no se pudo guardar: ' + e.message); }
  nandBackup(buf);
}
// a copy of the save goes to the launcher too (rabbids-save-backup.bin next to web/): a backup outside the browser
// (clearing the browser data would otherwise delete the game's progress)
let nandBackupT = 0, nandBackupBuf = null;
function nandBackup(buf) {
  if (!localLauncher || location.protocol === 'file:' || !buf) return;
  nandBackupBuf = buf; clearTimeout(nandBackupT);
  nandBackupT = setTimeout(() => { fetch('nand', { method: 'POST', body: nandBackupBuf }).catch(() => {}); nandBackupBuf = null; }, 1500);
}
nandPromise.then((b) => { if (b) nandBackup(b); });
