'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const {gzipSync} = require('node:zlib');
const loader = require('../web/wasm-loader.js');

// Actual 8-byte wasm header + empty module, compressed with real gzip.
const compressed = gzipSync(Buffer.from([0,97,115,109,1,0,0,0]));
const response = () => new Response(compressed,{status:200});
const noWait = async () => {};

test('AbortError from streamed WASM compilation retries a clean new response', async () => {
  let requests = 0, compiles = 0, retries = 0;
  const wasm = {
    compileStreaming: async (resp) => {
      compiles++;
      if (compiles === 1) throw new DOMException('The operation was aborted.','AbortError');
      return WebAssembly.compileStreaming(resp);
    },
    compile: (bytes) => WebAssembly.compile(bytes),
  };
  const module = await loader.load({fetchFile:async()=>{requests++; return response();},wasm,delay:noWait,onRetry:()=>retries++});
  assert(module instanceof WebAssembly.Module);
  assert.equal(requests,2);
  assert.equal(retries,1);
  assert.equal(compiles,2);
});

test('two streaming failures fall back to compiling fully decompressed bytes', async () => {
  let requested=0, fallback=0;
  const wasm={
    compileStreaming:async()=>{throw new DOMException('The operation was aborted.','AbortError');},
    compile:async(bytes)=>{fallback++;return WebAssembly.compile(bytes);},
  };
  const module=await loader.load({fetchFile:async()=>{requested++;return response();},wasm,delay:noWait});
  assert(module instanceof WebAssembly.Module);
  assert.equal(requested,3);
  assert.equal(fallback,1);
});

test('persistent download aborts report meaningful failure after bounded retries',async()=>{
  let requested=0;
  await assert.rejects(
    loader.load({fetchFile:async()=>{requested++;throw new DOMException('The operation was aborted.','AbortError');},delay:noWait}),
    /after 3 attempts: The operation was aborted/,
  );
  assert.equal(requested,3);
});

test('HTTP 404 gives an immediate useful error without downloading missing uncompressed WASM',async()=>{
  let requested=0;
  await assert.rejects(loader.load({fetchFile:async()=>{requested++;return new Response('',{status:404});},delay:noWait}),/HTTP 404/);
  assert.equal(requested,1);
});
