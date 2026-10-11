#!/usr/bin/env node
/**
 * R2 uploader for an EXTRACTED disc partition (source contains sys/ and files/).
 *
 * Install on Node.js 20+ (upload mode only):
 *   npm install --no-save --no-package-lock @aws-sdk/client-s3
 *
 * Dry run (default; no R2 access, AWS package or credentials required):
 *   node tools/r2-upload.mjs --source "D:\Rabbids\game" --bucket MY_BUCKET --account-id MY_R2_ACCOUNT_ID
 *
 * To upload content for which you have distribution authorization:
 *   Set R2_ACCESS_KEY_ID and R2_SECRET_ACCESS_KEY in the shell environment.
 *   node tools/r2-upload.mjs --source "D:\Rabbids\game" --bucket MY_BUCKET --account-id MY_R2_ACCOUNT_ID --execute --confirm-rights
 *
 * Alternative: --endpoint https://ACCOUNT_ID.r2.cloudflarestorage.com
 * Optional: --prefix game (default), --part-mib 64, --concurrency 3,
 *           --retries 5, --state "D:\private\r2-state.json", --help
 *
 * Objects keep their relative paths under the prefix:
 *   SOURCE/sys/boot.bin -> game/sys/boot.bin
 *   SOURCE/files/movie.bin.part00 -> game/files/movie.bin.part00
 *
 * The default resume file lives in the user's home folder (outside the repository),
 * contains upload IDs/ETags and hashes but NO credentials. Each run verifies remote
 * objects by byte size + x-amz-meta-sha256. Interruption during multipart upload is
 * resumable from S3 ListParts as long as the source file has not changed. Multipart
 * uploads that were orphaned after losing the state file should be cleaned up in R2.
 * Configure an R2 public domain + bucket CORS separately; this script only uploads.
 * Do NOT put access keys in arguments, source files or version control.
 */
import { createReadStream } from 'node:fs';
import { promises as fs } from 'node:fs';
import { createHash } from 'node:crypto';
import { homedir } from 'node:os';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const MiB = 1024 * 1024;
const MAX_PARTS = 10000;
const HELP = [
  'Usage: node tools/r2-upload.mjs --source DIR --bucket BUCKET --account-id ID [options]',
  '       node tools/r2-upload.mjs --source DIR --bucket BUCKET --endpoint URL [options]',
  'Dry run is the DEFAULT; it does not contact R2 or load the AWS SDK.',
  'Options: --prefix game, --part-mib 64, --concurrency 3, --retries 5, --state FILE',
  '         --execute --confirm-rights (both needed for a real upload), --help',
  'Credentials for --execute: R2_ACCESS_KEY_ID and R2_SECRET_ACCESS_KEY environment vars.',
].join('\n');

function optionValue(argv, i, flag) {
  if (!argv[i + 1] || argv[i + 1].startsWith('--')) throw new Error('Missing value for ' + flag);
  return argv[i + 1];
}

function integer(value, name, min, max) {
  const n = Number(value);
  if (!Number.isSafeInteger(n) || n < min || n > max) {
    throw new Error(name + ' must be an integer from ' + min + ' to ' + max);
  }
  return n;
}

export function normalizePrefix(value = 'game') {
  if (value === '' || value === '/') return '';
  if (value.includes('\\') || value.startsWith('/') || value.endsWith('/') ||
      value.split('/').some((segment) => !segment || segment === '.' || segment === '..')) {
    throw new Error('Unsafe R2 prefix: ' + value);
  }
  return value;
}

export function parseArgs(argv) {
  const opts = { prefix: 'game', partMiB: 64, concurrency: 3, retries: 5, execute: false, rights: false };
  const named = { '--source': 'source', '--bucket': 'bucket', '--endpoint': 'endpoint',
    '--account-id': 'accountId', '--prefix': 'prefix', '--part-mib': 'partMiB',
    '--concurrency': 'concurrency', '--retries': 'retries', '--state': 'state' };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--help' || arg === '-h') { opts.help = true; continue; }
    if (arg === '--execute') { opts.execute = true; continue; }
    if (arg === '--confirm-rights') { opts.rights = true; continue; }
    if (arg === '--dry-run') { opts.dryRun = true; continue; }
    if (!Object.hasOwn(named, arg)) throw new Error('Unknown option: ' + arg);
    opts[named[arg]] = optionValue(argv, i, arg);
    i++;
  }
  if (opts.help) return opts;
  if (!opts.source || !opts.bucket) throw new Error('--source and --bucket are required');
  if (!/^[a-z0-9][a-z0-9.-]{1,61}[a-z0-9]$/.test(opts.bucket)) throw new Error('Invalid bucket name');
  if (opts.accountId && !/^[a-f0-9]{32}$/i.test(opts.accountId)) {
    throw new Error('--account-id must be a 32-digit hexadecimal R2 account ID');
  }
  if (opts.accountId && opts.endpoint) throw new Error('Choose --account-id OR --endpoint');
  opts.endpoint ||= opts.accountId ? 'https://' + opts.accountId + '.r2.cloudflarestorage.com' : undefined;
  if (opts.endpoint) {
    let url;
    try { url = new URL(opts.endpoint); } catch { throw new Error('Invalid --endpoint'); }
    if (url.protocol !== 'https:' || url.username || url.password || url.search || url.hash ||
        (url.pathname !== '/' && url.pathname !== '')) throw new Error('--endpoint must be an HTTPS origin without credentials or path');
    opts.endpoint = url.origin;
  } else if (opts.execute) throw new Error('--endpoint or --account-id is required for an upload');
  opts.partMiB = integer(opts.partMiB, '--part-mib', 5, 512);
  opts.concurrency = integer(opts.concurrency, '--concurrency', 1, 8);
  opts.retries = integer(opts.retries, '--retries', 1, 10);
  opts.prefix = normalizePrefix(opts.prefix);
  if (opts.execute && opts.dryRun) throw new Error('--dry-run and --execute conflict');
  if (opts.execute && !opts.rights) {
    throw new Error('Actual upload requires --confirm-rights to confirm distribution authorization');
  }
  opts.source = path.resolve(opts.source);
  return opts;
}

function objectKey(prefix, relative) {
  // Never use an absolute path or platform separator in the bucket.
  const slash = relative.split(path.sep).join('/');
  if (!slash || slash.split('/').some((s) => !s || s === '..' || s === '.')) {
    throw new Error('Unsafe source-relative path: ' + relative);
  }
  return prefix ? prefix + '/' + slash : slash;
}

export async function scanPartition(source, prefix = 'game') {
  const root = path.resolve(source);
  if (!(await fs.stat(root)).isDirectory()) throw new Error('--source must be a directory');
  const found = [];
  async function descend(dir) {
    const entries = await fs.readdir(dir, { withFileTypes: true });
    entries.sort((a, b) => a.name.localeCompare(b.name, 'en'));
    for (const entry of entries) {
      const full = path.join(dir, entry.name);
      // Refuse symbolic links (including directory junctions) to avoid leaking data outside source.
      const st = await fs.lstat(full);
      if (st.isSymbolicLink()) throw new Error('Symlink/junction found in partition: ' + full);
      if (st.isDirectory()) await descend(full);
      else if (st.isFile()) {
        const relative = path.relative(root, full);
        found.push({ full, relative, key: objectKey(prefix, relative), size: st.size, mtimeMs: st.mtimeMs });
      } else throw new Error('Unsupported filesystem entry: ' + full);
    }
  }
  await descend(root);
  found.sort((a, b) => a.key.localeCompare(b.key, 'en'));
  return found;
}

async function sha256File(file) {
  const hash = createHash('sha256');
  for await (const chunk of createReadStream(file.full)) hash.update(chunk);
  const after = await fs.stat(file.full);
  if (after.size !== file.size || after.mtimeMs !== file.mtimeMs) {
    throw new Error('Source file changed while hashing: ' + file.relative);
  }
  return hash.digest('hex');
}

function missing(err) {
  return err?.name === 'NotFound' || err?.name === 'NoSuchKey' ||
    err?.$metadata?.httpStatusCode === 404;
}

function missingMultipart(err) {
  return err?.name === 'NoSuchUpload' || err?.Code === 'NoSuchUpload';
}

function retryable(err) {
  const code = err?.name || err?.code;
  const status = err?.$metadata?.httpStatusCode;
  return !status && ['ETIMEDOUT', 'ECONNRESET', 'EAI_AGAIN', 'ENOTFOUND', 'TimeoutError', 'NetworkingError'].includes(code) ||
    [408, 425, 429, 500, 502, 503, 504].includes(status) ||
    ['SlowDown', 'RequestTimeout', 'Throttling', 'ThrottlingException'].includes(code);
}

export async function retry(action, retries, log = console, pause = (ms) => new Promise((r) => setTimeout(r, ms))) {
  for (let n = 1; ; n++) {
    try { return await action(); }
    catch (err) {
      if (n >= retries || !retryable(err)) throw err;
      const delay = Math.min(16000, 250 * (2 ** (n - 1))) + Math.round(Math.random() * 150);
      log.warn('Temporary R2 failure; attempt ' + n + '/' + retries + ', retrying (' + (err.name || err.code || 'network') + ')');
      await pause(delay);
    }
  }
}

export function defaultStatePath(opts) {
  const id = createHash('sha256').update([opts.source, opts.bucket, opts.endpoint, opts.prefix].join('\0')).digest('hex').slice(0, 20);
  return path.join(homedir(), '.rgh-r2-uploads', id + '.json');
}

async function acquireLock(lock) {
  for (let attempt = 0; attempt < 2; attempt++) {
    try {
      const handle = await fs.open(lock, 'wx', 0o600);
      try { await handle.writeFile(JSON.stringify({ pid: process.pid, startedAt: Date.now() })); }
      catch (err) { await handle.close(); await fs.unlink(lock); throw err; }
      return handle;
    } catch (err) {
      if (err.code !== 'EEXIST') throw err;
      // A killed process may leave its lock behind. Remove only a lock whose owner
      // can no longer be found, or an empty/invalid lock older than one minute.
      let stale = false;
      try {
        const st = await fs.stat(lock);
        let pid;
        try { pid = JSON.parse(await fs.readFile(lock, 'utf8')).pid; } catch { /* empty lock */ }
        if (Number.isSafeInteger(pid) && pid > 0) {
          try { process.kill(pid, 0); } catch (e) { stale = e.code === 'ESRCH'; }
        } else stale = Date.now() - st.mtimeMs > 60000;
      } catch (e) {
        if (e.code === 'ENOENT') continue; // another process removed the lock
        throw e;
      }
      if (!stale) throw new Error('Upload state is locked; another upload may be running. Lock: ' + lock);
      await fs.unlink(lock).catch((e) => { if (e.code !== 'ENOENT') throw e; });
    }
  }
  throw new Error('Unable to acquire upload state lock: ' + lock);
}

async function stateStorage(filename, source) {
  const dest = path.resolve(filename);
  const relative = path.relative(path.resolve(source), dest);
  if (relative === '' || (!relative.startsWith('..' + path.sep) && relative !== '..' && !path.isAbsolute(relative))) {
    throw new Error('--state must be outside the source directory');
  }
  await fs.mkdir(path.dirname(dest), { recursive: true });
  const lock = dest + '.lock';
  const handle = await acquireLock(lock);
  let state = { version: 1, uploads: {} };
  try {
    const bytes = await fs.readFile(dest, 'utf8');
    state = JSON.parse(bytes);
    if (state.version !== 1 || !state.uploads || Array.isArray(state.uploads)) throw new Error('Unsupported state file format');
  } catch (err) {
    if (err.code !== 'ENOENT') { await handle.close(); await fs.unlink(lock); throw err; }
  }
  return {
    state,
    async save() {
      const tmp = dest + '.' + process.pid + '.tmp';
      await fs.writeFile(tmp, JSON.stringify(state, null, 2) + '\n', { mode: 0o600 });
      await fs.rename(tmp, dest);
    },
    async close() { await handle.close(); await fs.unlink(lock); },
  };
}

async function listRemoteParts(api, key, uploadId, retries, log, pause) {
  const parts = new Map();
  let marker;
  for (let pages = 0; pages < 20; pages++) {
    const r = await retry(() => api.listParts(key, uploadId, marker), retries, log, pause);
    for (const part of r.Parts || []) {
      if (!Number.isSafeInteger(part.PartNumber) || !part.ETag) throw new Error('Invalid multipart state returned by R2');
      parts.set(part.PartNumber, { ETag: part.ETag, Size: part.Size });
    }
    if (!r.IsTruncated) return parts;
    const last = r.NextPartNumberMarker || r.Parts?.at(-1)?.PartNumber;
    if (!last || (marker !== undefined && last <= marker)) throw new Error('Non-advancing R2 multipart pagination');
    marker = last;
  }
  throw new Error('R2 multipart pagination exceeded 20 pages');
}

async function readPart(handle, start, length) {
  const buffer = Buffer.allocUnsafe(length);
  let got = 0;
  while (got < length) {
    const r = await handle.read(buffer, got, length - got, start + got);
    if (!r.bytesRead) throw new Error('Unexpected end of source file');
    got += r.bytesRead;
  }
  return buffer;
}

function expectedPartSize(fileSize, partSize, number) {
  return Math.min(partSize, fileSize - (number - 1) * partSize);
}

async function ensureMultipart(file, sha256, opts, api, storage, log, pause) {
  const key = file.key, retries = opts.retries, partSize = opts.partMiB * MiB;
  const count = Math.ceil(file.size / partSize);
  if (count > MAX_PARTS) throw new Error('Too many multipart parts (' + count + '): increase --part-mib');
  let saved = storage.state.uploads[key];
  if (saved && (saved.sha256 !== sha256 || saved.size !== file.size || saved.mtimeMs !== file.mtimeMs ||
                saved.partSize !== partSize)) {
    log.warn('Source changed since previous multipart upload, starting a new one: ' + key);
    // The previous multipart attempt belongs to this tool. Abort it to avoid storage charges.
    try { await retry(() => api.abort(key, saved.uploadId), retries, log, pause); }
    catch (e) { log.warn('Could not abort old multipart upload for ' + key + ': ' + e.name); }
    delete storage.state.uploads[key];
    await storage.save();
    saved = undefined;
  }
  let remote;
  if (saved) {
    try { remote = await listRemoteParts(api, key, saved.uploadId, retries, log, pause); }
    catch (err) {
      if (!missingMultipart(err)) throw err;
      delete storage.state.uploads[key];
      await storage.save();
      saved = undefined;
    }
  }
  if (!saved) {
    const created = await retry(() => api.create(key, { sha256 }), retries, log, pause);
    if (!created?.UploadId) throw new Error('R2 did not return an UploadId for ' + key);
    saved = { uploadId: created.UploadId, size: file.size, mtimeMs: file.mtimeMs, sha256, partSize };
    storage.state.uploads[key] = saved;
    await storage.save();
    remote = new Map();
  }
  const completed = new Map();
  for (const [number, part] of remote) {
    if (number >= 1 && number <= count && part.Size === expectedPartSize(file.size, partSize, number)) {
      completed.set(number, part.ETag);
    }
  }
  const handle = await fs.open(file.full, 'r');
  try {
    for (let start = 1; start <= count; start += opts.concurrency) {
      const numbers = [];
      for (let n = start; n < Math.min(start + opts.concurrency, count + 1); n++) {
        if (!completed.has(n)) numbers.push(n);
      }
      // Promise.allSettled ensures in-flight writes are finished before reporting failure.
      const results = await Promise.allSettled(numbers.map(async (number) => {
        const offset = (number - 1) * partSize;
        const body = await readPart(handle, offset, expectedPartSize(file.size, partSize, number));
        const response = await retry(() => api.uploadPart(key, saved.uploadId, number, body), retries, log, pause);
        if (!response?.ETag) throw new Error('Missing R2 part ETag for ' + key + ' part ' + number);
        return { number, etag: response.ETag };
      }));
      for (const r of results) if (r.status === 'fulfilled') completed.set(r.value.number, r.value.etag);
      const failure = results.find((r) => r.status === 'rejected');
      if (failure) throw failure.reason;
    }
  } finally { await handle.close(); }
  const after = await fs.stat(file.full);
  if (after.size !== file.size || after.mtimeMs !== file.mtimeMs) throw new Error('Source changed while uploading: ' + key);
  const parts = Array.from({ length: count }, (_, i) => ({ PartNumber: i + 1, ETag: completed.get(i + 1) }));
  if (parts.some((p) => !p.ETag)) throw new Error('Missing completed multipart part for ' + key);
  try { await retry(() => api.complete(key, saved.uploadId, parts), retries, log, pause); }
  catch (err) {
    // Completion may succeed while its response is lost. Check HEAD before failing.
    if (!(await remoteMatches(api, key, file.size, sha256, opts, log, pause))) throw err;
  }
  if (!(await remoteMatches(api, key, file.size, sha256, opts, log, pause))) {
    throw new Error('R2 could not verify completed object: ' + key);
  }
  delete storage.state.uploads[key];
  await storage.save();
}

async function remoteMatches(api, key, size, sha256, opts, log, pause) {
  let found;
  try { found = await retry(() => api.head(key), opts.retries, log, pause); }
  catch (err) { if (missing(err)) return false; throw err; }
  return Number(found?.ContentLength) === size &&
    String(found?.Metadata?.sha256 || '').toLowerCase() === sha256;
}

/**
 * For tests, api is an in-memory S3-compatible adapter. No network occurs unless
 * the caller explicitly constructs a live adapter and executes a transfer.
 */
export async function uploadFiles(files, opts, api, log = console, pause) {
  const storage = await stateStorage(opts.state || defaultStatePath(opts), opts.source);
  const result = { uploaded: 0, skipped: 0, bytesUploaded: 0, total: files.length };
  try {
    for (const file of files) {
      const sha = await sha256File(file);
      if (await remoteMatches(api, file.key, file.size, sha, opts, log, pause)) {
        if (storage.state.uploads[file.key]) {
          delete storage.state.uploads[file.key];
          await storage.save();
        }
        log.info('SKIP ' + file.key + ' (matching size and SHA256)');
        result.skipped++;
        continue;
      }
      if (file.size > opts.partMiB * MiB) {
        await ensureMultipart(file, sha, opts, api, storage, log, pause);
      } else {
        await retry(() => api.put(file.key, file.full, file.size, { sha256: sha }), opts.retries, log, pause);
        if (!(await remoteMatches(api, file.key, file.size, sha, opts, log, pause))) {
          throw new Error('R2 could not verify uploaded object: ' + file.key);
        }
      }
      result.uploaded++;
      result.bytesUploaded += file.size;
      log.info('UPLOAD ' + file.key + ' (' + file.size + ' bytes)');
    }
  } finally { await storage.close(); }
  return result;
}

export async function s3Adapter(opts) {
  const id = process.env.R2_ACCESS_KEY_ID || process.env.AWS_ACCESS_KEY_ID;
  const secret = process.env.R2_SECRET_ACCESS_KEY || process.env.AWS_SECRET_ACCESS_KEY;
  if (!id || !secret) throw new Error('Missing R2_ACCESS_KEY_ID or R2_SECRET_ACCESS_KEY environment variable');
  let aws;
  try { aws = await import('@aws-sdk/client-s3'); }
  catch { throw new Error('Install the uploader dependency: npm install --no-save --no-package-lock @aws-sdk/client-s3'); }
  const s3 = new aws.S3Client({
    region: 'auto', endpoint: opts.endpoint, forcePathStyle: true,
    credentials: { accessKeyId: id, secretAccessKey: secret }, maxAttempts: 3,
  });
  const send = (Command, props) => s3.send(new Command({ Bucket: opts.bucket, ...props }));
  return {
    head: (Key) => send(aws.HeadObjectCommand, { Key }),
    put: (Key, full, ContentLength, Metadata) => send(aws.PutObjectCommand, {
      Key, ContentLength, Metadata, Body: createReadStream(full), ContentType: 'application/octet-stream',
    }),
    create: (Key, Metadata) => send(aws.CreateMultipartUploadCommand, {
      Key, Metadata, ContentType: 'application/octet-stream',
    }),
    listParts: (Key, UploadId, PartNumberMarker) => send(aws.ListPartsCommand, { Key, UploadId, PartNumberMarker }),
    uploadPart: (Key, UploadId, PartNumber, Body) => send(aws.UploadPartCommand, {
      Key, UploadId, PartNumber, Body, ContentLength: Body.length,
    }),
    complete: (Key, UploadId, Parts) => send(aws.CompleteMultipartUploadCommand, {
      Key, UploadId, MultipartUpload: { Parts },
    }),
    abort: (Key, UploadId) => send(aws.AbortMultipartUploadCommand, { Key, UploadId }),
    close: () => s3.destroy(),
  };
}

async function main() {
  try {
    const opts = parseArgs(process.argv.slice(2));
    if (opts.help) { console.log(HELP); return; }
    const files = await scanPartition(opts.source, opts.prefix);
    const total = files.reduce((sum, file) => sum + file.size, 0);
    console.log((opts.execute ? 'UPLOAD' : 'DRY RUN') + ': ' + files.length + ' files, ' + total + ' bytes');
    console.log('Source: ' + opts.source);
    console.log('Destination: ' + opts.bucket + '/' + opts.prefix);
    console.log('Endpoint: ' + (opts.endpoint || '(not set; not needed for dry run)'));
    if (!opts.execute) {
      for (const file of files.slice(0, 20)) console.log('  ' + file.key + ' (' + file.size + ' bytes)');
      if (files.length > 20) console.log('  ... and ' + (files.length - 20) + ' more');
      console.log('Nothing uploaded. Supply --execute --confirm-rights only with distribution authorization.');
      return;
    }
    if (!files.length) throw new Error('Source folder is empty');
    // Help prevent an accidentally chosen personal directory from being uploaded.
    if (!(files.some((f) => f.relative.split(path.sep).join('/') === 'sys/boot.bin') &&
          files.some((f) => f.relative.split(path.sep).join('/') === 'sys/fst.bin') &&
          files.some((f) => f.relative.split(path.sep).join('/').startsWith('files/')))) {
      throw new Error('Source must contain extracted sys/boot.bin, sys/fst.bin and files/ data');
    }
    const api = await s3Adapter(opts);
    try {
      const report = await uploadFiles(files, opts, api);
      console.log('Done: uploaded ' + report.uploaded + ', skipped ' + report.skipped +
        ', bytes uploaded ' + report.bytesUploaded);
    } finally { api.close(); }
  } catch (e) {
    console.error('R2 uploader: ' + (e?.message || e));
    process.exitCode = 1;
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) await main();
