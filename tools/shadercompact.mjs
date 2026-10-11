// node tools/shadercompact.mjs in.bin out.bin - keeps one register state per distinct generated shader
import fs from 'fs';
const src = fs.readFileSync(new URL('../web/gx-renderer.js', import.meta.url), 'utf8');
const GXRenderer = new Function('self', src + '\nreturn GXRenderer;')({});
const R = Object.create(GXRenderer.prototype);
R.bp = new Uint32Array(256); R.xfRaw = new Uint32Array(0x1100); R.programs = new Map(); R.stats = { progs: 0 }; R.scale = 2;
R.gl = new Proxy({}, { get: (t, k) => (typeof k === 'string' && k === k.toUpperCase() ? 1 : () => 1) });
R.link = () => ({});
const b = new Uint32Array(fs.readFileSync(process.argv[2]).buffer.slice(0));
const seen = new Set(), keep = [];
for (let o = 0; o + 353 <= b.length; o += 353) { const st = b.slice(o, o + 353); const pr = R.warm(st); if (pr && !seen.has(pr)) { seen.add(pr); keep.push(st); } }
const out = new Uint32Array(keep.length * 353); keep.forEach((st, i) => out.set(st, i * 353));
fs.writeFileSync(process.argv[3], Buffer.from(out.buffer));
console.log(`${b.length / 353} estados -> ${keep.length}`);
