#!/usr/bin/env node
// Verify the uploaded game data against the extracted files without downloading
// the whole disc. R2 must provide accurate HEAD sizes and 206 byte ranges.
// Example:
// node tools/verify-r2.mjs --source "C:\path\entire partition" --base https://YOUR-R2-DOMAIN/
import { promises as fs, createReadStream } from 'node:fs';
import path from 'node:path';

const MiB = 1024 * 1024;
const SLICE = 128 * MiB;
const LIMIT = 280 * MiB;
const PAGE_ORIGIN = 'https://teaserlua.github.io';

function argsOf(argv) {
  const a = {};
  for (let i = 0; i < argv.length; i += 2) {
    if (!['--source','--base','--sample'].includes(argv[i]) || !argv[i+1]) throw new Error(`Unrecognized option ${argv[i]}`);
    a[argv[i].slice(2)] = argv[i+1];
  }
  if (!a.source || !a.base || !a.base.startsWith('https://')) throw new Error('Use --source PATH --base https://PUBLIC-R2-DOMAIN/');
  a.sample = a.sample === undefined ? 12 : Number(a.sample);
  if (!Number.isSafeInteger(a.sample) || a.sample < 1 || a.sample > 1000) throw new Error('Invalid --sample count');
  return a;
}

async function objects(source) {
  const base = path.resolve(source), all = [];
  async function visit(dir) {
    for (const entry of await fs.readdir(dir, {withFileTypes:true})) {
      if (entry.isSymbolicLink()) throw new Error('Source contains a symbolic link');
      const abs = path.join(dir, entry.name);
      if (entry.isDirectory()) await visit(abs);
      else if (entry.isFile()) {
        const n = (await fs.stat(abs)).size;
        const rel = path.relative(base, abs).split(path.sep).join('/');
        const parts = n > LIMIT ? Math.ceil(n / SLICE) : 1;
        for (let p = 0; p < parts; p++) {
          const start = parts === 1 ? 0 : p * SLICE;
          const end = parts === 1 ? n : Math.min(n, start + SLICE);
          all.push({key:`game/${rel}${parts === 1 ? '' : '.part' + String(p).padStart(2,'0')}`,abs,start,end,size:end-start});
        }
      }
    }
  }
  await visit(base);
  return all.sort((a,b) => a.key.localeCompare(b.key));
}

async function localRange(file, start, end) {
  if (start === end) return new Uint8Array();
  const chunks = [];
  for await (const b of createReadStream(file, { start, end:end-1 })) chunks.push(b);
  return Buffer.concat(chunks);
}

async function one(item, base) {
  const url = new URL(item.key.split('/').map(encodeURIComponent).join('/'), base);
  const head = await fetch(url, {method:'HEAD',headers:{Origin:PAGE_ORIGIN},cache:'no-store'});
  if (head.status !== 200 || Number(head.headers.get('content-length')) !== item.size) {
    throw new Error(`${item.key}: expected HEAD 200 size=${item.size}; got ${head.status} size=${head.headers.get('content-length')}`);
  }
  if (head.headers.get('access-control-allow-origin') !== PAGE_ORIGIN) throw new Error(`${item.key}: CORS origin is missing`);
  if (!item.size) return;
  const offset = Math.max(0, item.size - 128); // verify the last bytes of every selected object
  const expected = await localRange(item.abs, item.start + offset, item.end);
  const rsp = await fetch(url, {headers:{Origin:PAGE_ORIGIN,Range:`bytes=${offset}-${item.size-1}`},cache:'no-store'});
  const bytes = Buffer.from(await rsp.arrayBuffer());
  if (rsp.status !== 206 || bytes.compare(expected) !== 0 ||
    rsp.headers.get('content-range') !== `bytes ${offset}-${item.size-1}/${item.size}`) {
    throw new Error(`${item.key}: Range response does not match source`);
  }
}

async function main() {
  const opts = argsOf(process.argv.slice(2));
  const all = await objects(opts.source);
  const critical = all.filter(x => /game\/sys\/(boot|fst|main\.dol)|game\/files\/(LynWiiRetail\.elf|Ai2CppWiiFinal\.rso|RGH\.)/.test(x.key));
  const chosen = new Map(critical.map(x => [x.key,x]));
  const increment = Math.max(1, Math.floor(all.length / opts.sample));
  for (let i = 0; i < all.length; i += increment) chosen.set(all[i].key,all[i]);
  let passed = 0;
  const base = opts.base.endsWith('/') ? opts.base : opts.base + '/';
  for (const item of chosen.values()) {
    await one(item,base);
    passed++;
    console.log(`PASS ${passed}/${chosen.size} ${item.key} (${item.size} bytes)`);
  }
  console.log(`Verified ${passed} sampled R2 objects against local files (${all.length} total objects).`);
}

main().catch(e => { console.error(e.stack || e); process.exitCode = 1; });
