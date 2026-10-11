#!/usr/bin/env node
// Preflight a public R2 base URL before enabling automatic game-data loading.
// Usage: node tools/check-r2.mjs --base https://assets.example.com/ --object game/sys/boot.bin
const args = process.argv.slice(2);
function arg(name, fallback) { const i = args.indexOf(name); return i < 0 ? fallback : args[i + 1]; }
const base = arg('--base');
const object = arg('--object', 'game/sys/boot.bin');
const origin = arg('--origin', 'https://jmhq20120212-cmd.github.io');
if (!base || !/^https:\/\//.test(base) || !/^[\w./$ ()-]+$/.test(object) || object.startsWith('/') || object.includes('..')) {
  console.error('Usage: node tools/check-r2.mjs --base https://PUBLIC-R2-DOMAIN/ [--object game/sys/boot.bin]');
  process.exit(2);
}
const fileUrl = new URL(object.split('/').map(encodeURIComponent).join('/'), base.endsWith('/') ? base : base + '/');
function checkCors(response, name) {
  const allowed = response.headers.get('access-control-allow-origin');
  if (allowed !== origin && allowed !== '*') throw new Error(`${name}: R2 CORS must allow ${origin}; got ${allowed || '(missing)'}`);
  const exposed = response.headers.get('access-control-expose-headers') || '';
  if (!exposed.includes('*') && !exposed.toLowerCase().includes('content-range')) throw new Error(`${name}: R2 must expose Content-Range`);
}
try {
  const head = await fetch(fileUrl, { method: 'HEAD', headers: { Origin: origin }, cache: 'no-store' });
  if (head.status !== 200) throw new Error(`HEAD returned HTTP ${head.status}`);
  checkCors(head, 'HEAD');
  const size = Number(head.headers.get('content-length'));
  if (!Number.isSafeInteger(size) || size < 32) throw new Error('HEAD: invalid Content-Length');
  const request = await fetch(fileUrl, { headers: { Origin: origin, Range: 'bytes=0-31' }, cache: 'no-store' });
  if (request.status !== 206) throw new Error(`GET Range should return HTTP 206; got ${request.status}`);
  checkCors(request, 'GET Range');
  const contentRange = request.headers.get('content-range');
  if (contentRange !== `bytes 0-31/${size}`) throw new Error(`Incorrect Content-Range: ${contentRange}`);
  const buffer = new Uint8Array(await request.arrayBuffer());
  if (buffer.byteLength !== 32) throw new Error(`Range returned ${buffer.byteLength} bytes, expected 32`);
  console.log(`PASS ${fileUrl.origin} - 200 HEAD, 206 GET Range, ${size} byte object, CORS origin ${origin}`);
} catch (error) {
  console.error(`FAIL ${fileUrl.href}: ${error.message}`);
  process.exitCode = 1;
}
