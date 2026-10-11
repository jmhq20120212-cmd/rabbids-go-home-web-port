#!/usr/bin/env node
/**
 * Upload an extracted partition to Cloudflare R2 using Wrangler OAuth login.
 * Big files are split into game-supported .part00, .part01, ... objects so that
 * no upload exceeds Wrangler's object size limit. Streams one chunk at a time.
 *
 * Run once: npm install --no-save --no-package-lock wrangler
 * Dry run: node tools/r2-wrangler-upload.mjs --source "C:\\path\\entire partition" --bucket rabbids-go-home-web
 * Real upload (requires permission to distribute every file): append --execute --confirm-rights
 */
import { createReadStream, createWriteStream } from 'node:fs';
import { promises as fs } from 'node:fs';
import { createHash } from 'node:crypto';
import { spawn } from 'node:child_process';
import { tmpdir, homedir } from 'node:os';
import path from 'node:path';
import { pipeline } from 'node:stream/promises';
import { fileURLToPath } from 'node:url';

const SLICE = 128 * 1024 * 1024;
const LIMIT = 280 * 1024 * 1024;
const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(scriptDir, '..');

function parse(args) {
  const opts = { prefix: 'game', execute: false, confirmRights: false, resume: false };
  const flags = new Map([['--source','source'],['--bucket','bucket'],['--prefix','prefix'],['--state','state'],['--public-base','publicBase']]);
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--execute') opts.execute = true;
    else if (args[i] === '--confirm-rights') opts.confirmRights = true;
    else if (args[i] === '--resume') opts.resume = true;
    else if (flags.has(args[i])) opts[flags.get(args[i])] = args[++i];
    else throw new Error(`Unknown argument ${args[i]}`);
  }
  if (!opts.source || !opts.bucket) throw new Error('Required: --source PATH --bucket BUCKET');
  if (!/^[a-z0-9][a-z0-9-]{1,61}[a-z0-9]$/.test(opts.bucket)) throw new Error('Invalid R2 bucket name');
  if (!/^[a-z0-9-]+$/.test(opts.prefix)) throw new Error('Invalid prefix');
  if (opts.execute && !opts.confirmRights) throw new Error('Public distribution requires --confirm-rights');
  if (opts.publicBase && (!opts.publicBase.startsWith('https://') || new URL(opts.publicBase).username)) throw new Error('Public base must be HTTPS');
  opts.source = path.resolve(opts.source);
  opts.state ||= path.join(homedir(), '.rabbids-r2-wrangler-state.json');
  return opts;
}

async function inventory(opts) {
  if (!(await fs.stat(opts.source)).isDirectory()) throw new Error('Source is not a folder');
  for (const required of ['sys/boot.bin', 'sys/fst.bin', 'sys/main.dol']) {
    if (!(await fs.stat(path.join(opts.source, ...required.split('/')))).isFile()) throw new Error(`Missing ${required}`);
  }
  const items = [];
  async function walk(dir) {
    for (const item of (await fs.readdir(dir, { withFileTypes: true })).sort((a,b) => a.name.localeCompare(b.name))) {
      if (item.isSymbolicLink()) throw new Error(`Symlink in source: ${item.name}`);
      const abs = path.join(dir, item.name);
      if (item.isDirectory()) await walk(abs);
      else if (item.isFile()) {
        const stat = await fs.stat(abs);
        const relative = path.relative(opts.source, abs).split(path.sep).join('/');
        const count = stat.size > LIMIT ? Math.ceil(stat.size / SLICE) : 1;
        if (count > 100) throw new Error(`Too many parts (${count}) for ${relative}`);
        for (let part = 0; part < count; part++) {
          const start = count === 1 ? 0 : part * SLICE;
          const end = count === 1 ? stat.size : Math.min(start + SLICE, stat.size);
          const suffix = count === 1 ? '' : `.part${String(part).padStart(2,'0')}`;
          items.push({ abs, key: `${opts.prefix}/${relative}${suffix}`, start, end, size: end - start,
            stamp: `${stat.size}:${stat.mtimeMs}:${start}:${end}` });
        }
      }
    }
  }
  await walk(opts.source);
  return items;
}

async function sha256(file, start, end) {
  const h = createHash('sha256');
  for await (const chunk of createReadStream(file, { start, end: end - 1 })) h.update(chunk);
  return h.digest('hex');
}

async function runWrangler(cli, args) {
  const child = spawn(process.execPath, [cli, ...args], { cwd: repo, stdio: ['ignore','pipe','pipe'], windowsHide: true });
  let output = '';
  child.stdout.on('data', (chunk) => { output += chunk.toString(); });
  child.stderr.on('data', (chunk) => { output += chunk.toString(); });
  const code = await new Promise((resolve, reject) => { child.once('error', reject); child.once('close', resolve); });
  if (code !== 0) throw new Error(`Wrangler exit ${code}: ${output.slice(-500)}`);
}

async function saveState(name, data) {
  const temp = name + '.tmp';
  await fs.writeFile(temp, JSON.stringify(data, null, 2), { mode: 0o600 });
  await fs.rename(temp, name);
}

async function main() {
  const opts = parse(process.argv.slice(2));
  const items = await inventory(opts);
  const total = items.reduce((v, item) => v + item.size, 0);
  console.log(`Files/parts: ${items.length} - ${(total / (1024 ** 3)).toFixed(3)} GiB. Bucket: ${opts.bucket}, prefix: ${opts.prefix}`);
  if (!opts.execute) {
    for (const item of items.filter((it) => it.key.includes('.part')).slice(0, 16)) console.log(`  ${item.key} (${(item.size / (1024 ** 2)).toFixed(1)} MiB)`);
    console.log('Dry run completed; nothing was uploaded. To upload files you are authorized to distribute, pass --execute --confirm-rights.');
    return;
  }
  const cli = path.join(repo, 'node_modules', 'wrangler', 'bin', 'wrangler.js');
  try { await fs.access(cli); } catch { throw new Error('Install Wrangler locally: npm install --no-save --no-package-lock wrangler'); }
  const state = opts.resume ? await fs.readFile(opts.state, 'utf8').then(JSON.parse, () => ({})) : {};
  const fingerprint = opts.bucket + ':' + opts.prefix + ':' + opts.source;
  if (state.scope && state.scope !== fingerprint) throw new Error('Resume state belongs to a different source/bucket/prefix');
  const completed = opts.resume ? (state.completed || {}) : {};
  const nextState = {scope: fingerprint, completed};
  const tmp = await fs.mkdtemp(path.join(tmpdir(),'rabbids-wrangler-'));
  try {
    for (let i = 0; i < items.length; i++) {
      const item = items[i];
      if (completed[item.key] === item.stamp) { console.log(`SKIP ${i+1}/${items.length} ${item.key}`); continue; }
      const tempFile = path.join(tmp, 'current-object.bin');
      // The range constructor never reads more than one 128-MiB slice.
      if (item.size === 0) await fs.writeFile(tempFile, '');
      else await pipeline(createReadStream(item.abs, { start: item.start, end: item.end - 1 }), createWriteStream(tempFile));
      const digest = item.size === 0 ? createHash('sha256').digest('hex') : await sha256(tempFile, 0, item.size);
      let sent = false;
      for (let retry = 1; retry <= 8 && !sent; retry++) {
        try {
          await runWrangler(cli, ['r2','object','put',`${opts.bucket}/${item.key}`,'--file',tempFile,'--remote',
            '--content-type','application/octet-stream','--cache-control','public,max-age=86400']);
          if (opts.publicBase) {
            const base = opts.publicBase.endsWith('/') ? opts.publicBase : opts.publicBase + '/';
            const remote = new URL(item.key.split('/').map(encodeURIComponent).join('/'), base);
            const response = await fetch(remote, { method:'HEAD', cache:'no-store' });
            if (response.status !== 200 || Number(response.headers.get('content-length')) !== item.size) {
              throw new Error(`R2 HEAD verification failed for ${remote.pathname}`);
            }
          }
          sent = true;
        } catch (error) {
          if (retry === 8) throw error;
          console.warn(`RETRY ${retry}/8 ${item.key}: ${error.message}`);
          await new Promise((resolve) => setTimeout(resolve, retry * 1500));
        }
      }
      completed[item.key] = item.stamp;
      await saveState(opts.state, nextState);
      console.log(`OK ${i + 1}/${items.length} ${item.key} (${item.size} bytes, SHA256 ${digest.slice(0, 12)})`);
      await fs.unlink(tempFile);
    }
  } finally { await fs.rm(tmp, { recursive: true, force: true }); }
  console.log('Upload completed. Check the public HEAD/Range/CORS behavior before enabling game-config.json.');
}

main().catch((error) => { console.error('R2 upload:', error.message); process.exitCode = 1; });
