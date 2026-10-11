'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const zlib = require('node:zlib');
const vm = require('node:vm');

// The shipped WASM must be a valid module compatible with the JS host imports.
const wasm = zlib.gunzipSync(fs.readFileSync('web/rgh.wasm.gz'));
const wasmModule = new WebAssembly.Module(wasm);
const imported = new Set(WebAssembly.Module.imports(wasmModule).map((x) => `${x.module}.${x.name}`));
for (const name of ['rgh.file_size', 'rgh.file_read', 'rgh.nand_save', 'rgh.ring_ready']) {
  assert(imported.has(name), `WASM missing expected host import: ${name}`);
}
console.log(`PASS compiled WebAssembly (${wasm.length} bytes, ${imported.size} imports)`);

// Simulate a worker's synchronous, on-demand reads from the chosen directory.
const data = new Uint8Array(600_000);
for (let i = 0; i < data.length; i++) data[i] = i % 251;
const file = {
  size: data.length,
  slice(start, end) { return data.slice(start, end); },
};
const context = vm.createContext({
  Map, Uint8Array, DataView, TextDecoder, TextEncoder, ArrayBuffer,
  onmessage: null,
  XMLHttpRequest: class { constructor() { throw new Error('Unexpected network request for local data'); } },
  FileReaderSync: class { readAsArrayBuffer(bytes) { return bytes.buffer; } },
});
vm.runInContext(fs.readFileSync('web/rgh-worker.js', 'utf8'), context);
context._mem = { buffer: new ArrayBuffer(1024 * 1024) };
context._cfg = { base: 'https://example.invalid/', localFiles: [['game/files/RGH.BF', file]] };
vm.runInContext('mem = _mem; cfg = _cfg', context);
vm.runInContext('localFiles = new Map(cfg.localFiles); localReader = new FileReaderSync()', context);
assert.equal(vm.runInContext("fileSize('game/files/RGH.BF')", context), data.length);
assert.equal(vm.runInContext("fileSize('game/files/missing.BF')", context), -1);
assert.equal(vm.runInContext("fileRead('game/files/RGH.BF', 261000, 100, 300000)", context), 300000);
assert.deepEqual(new Uint8Array(context._mem.buffer, 100, 300000), data.subarray(261000, 561000));
assert.equal(vm.runInContext("fileRead('game/files/RGH.BF', 599990, 0, 100)", context), 10);
assert.deepEqual(new Uint8Array(context._mem.buffer, 0, 10), data.subarray(599990));
assert.equal(vm.runInContext("fileRead('game/files/missing.BF', 0, 0, 100)", context), -1);
console.log('PASS local folder file size, missing file, block boundary, partial EOF');
