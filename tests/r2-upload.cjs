'use strict';
// Offline fixtures only: run with node --test tests/r2-upload.cjs
// No real game data, AWS credentials, remote R2 account or network are used.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const { pathToFileURL } = require('node:url');

const modPromise = import(pathToFileURL(path.join(__dirname, '..', 'tools', 'r2-upload.mjs')).href);
const quiet = { info() {}, warn() {} };
const MiB = 1024 * 1024;

async function fixture(t) {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'rgh-r2-fixture-'));
  const source = path.join(root, 'partition');
  await fs.mkdir(path.join(source, 'sys'), { recursive: true });
  await fs.mkdir(path.join(source, 'files', 'level 01'), { recursive: true });
  await fs.writeFile(path.join(source, 'sys', 'boot.bin'), Buffer.from('fixture boot'));
  await fs.writeFile(path.join(source, 'sys', 'fst.bin'), Buffer.from('fixture fst'));
  await fs.writeFile(path.join(source, 'files', 'level 01', 'a.dat'), Buffer.from('fixture data'));
  t.after(async () => fs.rm(root, { recursive: true, force: true }));
  return { root, source, state: path.join(root, 'resume.json') };
}

function mockR2() {
  const objects = new Map();
  const uploads = new Map();
  const calls = { put: 0, create: 0, parts: [], head: 0, listParts: 0, abort: 0, complete: 0 };
  let sequence = 0;
  const err404 = () => Object.assign(new Error('missing'), { name: 'NotFound', $metadata: { httpStatusCode: 404 } });
  const api = {
    objects, uploads, calls, failPutOnce: false, failPartOnce: 0, failCompleteResponseOnce: false,
    async head(key) {
      calls.head++;
      const v = objects.get(key);
      if (!v) throw err404();
      return { ContentLength: v.body.length, Metadata: { ...v.metadata } };
    },
    async put(key, full, size, metadata) {
      calls.put++;
      if (api.failPutOnce) {
        api.failPutOnce = false;
        throw Object.assign(new Error('temporary'), { $metadata: { httpStatusCode: 503 }, name: 'SlowDown' });
      }
      const body = await fs.readFile(full);
      assert.equal(body.length, size);
      objects.set(key, { body, metadata });
    },
    async create(key, metadata) {
      calls.create++;
      const UploadId = 'up-' + ++sequence;
      uploads.set(UploadId, { key, metadata, parts: new Map() });
      return { UploadId };
    },
    async listParts(key, uploadId) {
      calls.listParts++;
      const up = uploads.get(uploadId);
      if (!up) throw Object.assign(new Error('upload not found'), { name: 'NoSuchUpload' });
      assert.equal(up.key, key);
      return {
        IsTruncated: false,
        Parts: [...up.parts.entries()].map(([PartNumber, p]) => ({ PartNumber, ETag: p.etag, Size: p.body.length })),
      };
    },
    async uploadPart(key, uploadId, number, body) {
      calls.parts.push(number);
      if (api.failPartOnce === number) {
        api.failPartOnce = 0;
        throw Object.assign(new Error('simulate hard connection failure'), { name: 'BadRequest', $metadata: { httpStatusCode: 400 } });
      }
      const up = uploads.get(uploadId);
      assert.equal(up.key, key);
      const etag = '"mock-part-' + number + '"';
      up.parts.set(number, { etag, body: Buffer.from(body) });
      return { ETag: etag };
    },
    async complete(key, uploadId, parts) {
      calls.complete++;
      const up = uploads.get(uploadId);
      assert.equal(up.key, key);
      assert.deepEqual(parts.map((p) => p.PartNumber), parts.map((p) => p.PartNumber).sort((a, b) => a - b));
      const buffers = parts.map(({ PartNumber, ETag }) => {
        const part = up.parts.get(PartNumber);
        assert.equal(part.etag, ETag);
        return part.body;
      });
      objects.set(key, { body: Buffer.concat(buffers), metadata: up.metadata });
      uploads.delete(uploadId);
      if (api.failCompleteResponseOnce) {
        api.failCompleteResponseOnce = false;
        throw Object.assign(new Error('lost response after success'), { name: 'NoSuchUpload' });
      }
    },
    async abort(key, uploadId) {
      calls.abort++;
      uploads.delete(uploadId);
    },
  };
  return api;
}

function options(f, more = {}) {
  return { source: f.source, bucket: 'mock-bucket', endpoint: 'https://mock.r2.cloudflarestorage.com',
    prefix: 'game', state: f.state, partMiB: 5, concurrency: 2, retries: 2, ...more };
}

test('CLI parser dry-run default, safe paths, rights gate, numeric bounds', async () => {
  const { parseArgs, normalizePrefix } = await modPromise;
  const args = ['--source', 'F:\\fake\\disc', '--bucket', 'mock-bucket',
    '--endpoint', 'https://sample.r2.cloudflarestorage.com'];
  assert.equal(parseArgs(args).execute, false);
  assert.equal(parseArgs(args).prefix, 'game');
  assert.equal(normalizePrefix(''), '');
  assert.throws(() => parseArgs([...args, '--execute']), /confirm-rights/);
  assert.equal(parseArgs([...args, '--execute', '--confirm-rights']).execute, true);
  assert.throws(() => parseArgs([...args, '--prefix', '../private']), /Unsafe/);
  assert.throws(() => parseArgs([...args, '--prefix', 'folder\\oops']), /Unsafe/);
  assert.throws(() => parseArgs([...args, '--part-mib', '4']), /part-mib/);
  assert.throws(() => parseArgs([...args, '--concurrency', '0']), /concurrency/);
  assert.throws(() => parseArgs([...args, '--execute', '--confirm-rights', '--dry-run']), /conflict/);
  assert.throws(() => parseArgs([...args, '--unknown']), /Unknown option/);
  assert.throws(() => parseArgs([...args, '--account-id', 'a'.repeat(32)]), /Choose/);
});

test('scan preserves extracted partition directory structure and file names', async (t) => {
  const f = await fixture(t);
  const { scanPartition } = await modPromise;
  const files = await scanPartition(f.source, 'game');
  assert.equal(files.length, 3);
  assert.deepEqual(files.map((v) => v.key).sort(), [
    'game/files/level 01/a.dat', 'game/sys/boot.bin', 'game/sys/fst.bin',
  ]);
  await fs.writeFile(path.join(f.source, 'files', 'level 01', 'movie.part00'), 'P');
  assert((await scanPartition(f.source)).some((v) => v.key === 'game/files/level 01/movie.part00'));
});

test('default CLI dry-run executes without SDK, credentials, network or state', async (t) => {
  const f = await fixture(t);
  const cmd = path.join(__dirname, '..', 'tools', 'r2-upload.mjs');
  const output = execFileSync(process.execPath, [cmd, '--source', f.source, '--bucket', 'mock-bucket'],
    { encoding: 'utf8', env: { PATH: process.env.PATH, SystemRoot: process.env.SystemRoot, HOME: f.root, USERPROFILE: f.root } });
  assert.match(output, /DRY RUN: 3 files/);
  assert.match(output, /Nothing uploaded/);
  await assert.rejects(fs.stat(f.state), { code: 'ENOENT' });
});

test('small objects retry transient 503 and resume by SHA256; same-length edit reuploads', async (t) => {
  const f = await fixture(t);
  const { uploadFiles, scanPartition } = await modPromise;
  const api = mockR2();
  api.failPutOnce = true;
  const opts = options(f);
  let files = await scanPartition(f.source);
  const first = await uploadFiles(files, opts, api, quiet, async () => {});
  assert.deepEqual([first.uploaded, first.skipped], [3, 0]);
  assert.equal(api.calls.put, 4); // one simulated transient error
  const second = await uploadFiles(files, opts, api, quiet, async () => {});
  assert.deepEqual([second.uploaded, second.skipped], [0, 3]);
  assert.equal(api.calls.put, 4);
  const target = path.join(f.source, 'sys', 'fst.bin');
  const previous = await fs.readFile(target);
  await fs.writeFile(target, Buffer.alloc(previous.length, 0x33));
  files = await scanPartition(f.source);
  const third = await uploadFiles(files, opts, api, quiet, async () => {});
  assert.deepEqual([third.uploaded, third.skipped], [1, 2]);
  assert.equal(api.calls.put, 5);
});

test('multipart failed part is resumed from remote ListParts without restarting completed part', async (t) => {
  const f = await fixture(t);
  const { uploadFiles, scanPartition } = await modPromise;
  const big = path.join(f.source, 'files', 'level 01', 'big.part00');
  await fs.writeFile(big, Buffer.alloc(11 * MiB + 31, 0x6d));
  const files = (await scanPartition(f.source)).filter((v) => v.key.endsWith('big.part00'));
  const api = mockR2();
  const opts = options(f, { retries: 1 });
  api.failPartOnce = 2;
  await assert.rejects(uploadFiles(files, opts, api, quiet), /simulate hard connection failure/);
  assert.equal(api.calls.create, 1);
  assert.equal(api.calls.parts.filter((n) => n === 1).length, 1);
  const state = JSON.parse(await fs.readFile(f.state, 'utf8'));
  assert(Object.keys(state.uploads).includes(files[0].key));
  const result = await uploadFiles(files, opts, api, quiet);
  assert.deepEqual([result.uploaded, result.skipped], [1, 0]);
  assert.equal(api.calls.create, 1, 'should reuse the same UploadId');
  assert.equal(api.calls.parts.filter((n) => n === 1).length, 1, 'part 1 is not reuploaded');
  assert.equal(api.calls.listParts, 1);
  assert.deepEqual(api.objects.get(files[0].key).body, await fs.readFile(big));
  assert.deepEqual(JSON.parse(await fs.readFile(f.state, 'utf8')).uploads, {});
});

test('lost CompleteMultipartUpload reply is recognized via remote HEAD', async (t) => {
  const f = await fixture(t);
  const { uploadFiles, scanPartition } = await modPromise;
  const big = path.join(f.source, 'files', 'big.part00');
  await fs.writeFile(big, Buffer.alloc(6 * MiB, 0x57));
  const files = (await scanPartition(f.source)).filter((v) => v.key.endsWith('big.part00'));
  const api = mockR2();
  api.failCompleteResponseOnce = true;
  const result = await uploadFiles(files, options(f), api, quiet);
  assert.equal(result.uploaded, 1);
  assert.equal(api.objects.get(files[0].key).body.length, 6 * MiB);
});

test('stale multipart is abandoned when local source changes; new upload ID created', async (t) => {
  const f = await fixture(t);
  const { uploadFiles, scanPartition } = await modPromise;
  const big = path.join(f.source, 'files', 'big.part00');
  await fs.writeFile(big, Buffer.alloc(6 * MiB, 0x42));
  const api = mockR2();
  const opts = options(f, { retries: 1 });
  api.failPartOnce = 2;
  await assert.rejects(uploadFiles((await scanPartition(f.source)).filter((v) => v.full === big), opts, api, quiet));
  await fs.writeFile(big, Buffer.alloc(6 * MiB, 0x43));
  const result = await uploadFiles((await scanPartition(f.source)).filter((v) => v.full === big), opts, api, quiet);
  assert.equal(result.uploaded, 1);
  assert.equal(api.calls.abort, 1);
  assert.equal(api.calls.create, 2);
});

test('state file inside source is rejected and lock is cleaned after failed run', async (t) => {
  const f = await fixture(t);
  const { uploadFiles, scanPartition } = await modPromise;
  const files = await scanPartition(f.source);
  await assert.rejects(uploadFiles(files, options(f, { state: path.join(f.source, 'unsafe-state.json') }),
    mockR2(), quiet), /outside the source/);
  const api = mockR2();
  api.failPutOnce = true;
  await assert.rejects(uploadFiles(files, options(f, { retries: 1 }), api, quiet));
  await assert.rejects(fs.stat(f.state + '.lock'), { code: 'ENOENT' });
});

test('stale lock from a dead process is recovered, active process lock is respected', async (t) => {
  const f = await fixture(t);
  const { uploadFiles, scanPartition } = await modPromise;
  const files = await scanPartition(f.source);
  await fs.writeFile(f.state + '.lock', JSON.stringify({ pid: 99999999, startedAt: 0 }));
  const result = await uploadFiles(files, options(f), mockR2(), quiet);
  assert.equal(result.uploaded, 3);
  await fs.writeFile(f.state + '.lock', JSON.stringify({ pid: process.pid, startedAt: Date.now() }));
  await assert.rejects(uploadFiles(files, options(f), mockR2(), quiet), /locked/);
  await fs.unlink(f.state + '.lock');
});
