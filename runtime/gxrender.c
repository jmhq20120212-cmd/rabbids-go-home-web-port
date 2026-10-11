/* gxrender.c - GPU front end work that needs guest memory: decodes vertices (direct and indexed
 * attributes, all GX component formats) and textures (all GX texture formats, palettes, mipmaps,
 * EFB-copy aliases) and streams them with the register writes to the renderer through the ring.
 * The renderer (web/gx-renderer.js) implements transform, lighting, TEV and blending on WebGL2. */
#include "hw.h"
#include "gxring.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern u32 g_cp[0x100], g_xf[0x1100], g_bp[0x100];
u32 gx_vertex_size(int vat);

/* ---------------------------------------------------------------- ring */
GxRing *g_ring;
#define RING_BYTES (64u << 20)   /* a power of two: positions are u32 and wrap at 4 GB, offsets = pos % size must stay continuous */
static u32 ring_reserved;
#ifdef __wasm__
__attribute__((import_module("rgh"), import_name("ring_ready"))) void js_ring_ready(void *ring);
#endif
static void ring_init(void) {
  g_ring = calloc(1, sizeof(GxRing) + RING_BYTES);
  g_ring->size = RING_BYTES;
#ifdef __wasm__
  js_ring_ready(g_ring);
#else
  if (getenv("RGH_RINGTEST")) g_ring->consumer = 1;
#endif
}
static u32 ring_used(void) { return g_ring->wpos - g_ring->rpos; }   /* positions grow monotonically */
u32 *ring_alloc(u32 words) {
  u32 bytes = words * 4, size = g_ring->size;
  if (!g_ring->consumer) { ring_reserved = 0; return NULL; }
  /* a record that can never fit would wait forever (and freeze the game): drop it */
  if (bytes > size / 4) { static int n; if (n++ < 5) fprintf(stderr, "[gx] record too large (%u bytes): dropped\n", bytes); ring_reserved = 0; return NULL; }
  u32 off = g_ring->wpos % size;
  u32 pad = (off + bytes > size) ? size - off : 0;          /* records never wrap */
  while (ring_used() + pad + bytes > size) {
#ifdef __wasm__
    u32 r = __atomic_load_n(&g_ring->rpos, __ATOMIC_SEQ_CST);
    if (ring_used() + pad + bytes > size) __builtin_wasm_memory_atomic_wait32((int *)&g_ring->rpos, (int)r, 50000000);
    if (!g_ring->consumer) return NULL;
#else
    g_ring->rpos = g_ring->wpos;          /* native test consumer: drop everything */
#endif
  }
  if (pad) {
    if (pad >= 4) *(u32 *)(g_ring->data + off) = (u32)(pad / 4);   /* type 0 = skip */
    __atomic_store_n(&g_ring->wpos, g_ring->wpos + pad, __ATOMIC_SEQ_CST);
    off = 0;
  }
  ring_reserved = bytes;
  return (u32 *)(g_ring->data + off);
}

#ifndef __wasm__
/* ---------------------------------------------------------------- debug: renderer snapshots (native only)
 * RGH_SNAP="f1,f2,..." writes $RGH_SNAPDIR/snap_<f>.bin for each listed ring frame: the renderer state at that
 * frame (XF memory, CP/BP registers, texture bindings, the textures used) followed by RGH_SNAPLEN frames of
 * records. tests/replay.html plays such a file through web/gx-renderer.js. */
typedef struct { u32 id; u32 *rec; } TexRec;
static TexRec *s_tex; static int s_ntex, s_captex;
static u32 s_bp[256], s_xf[0x1100], s_cp[2], s_bound[8]; static u8 s_bpset[256], s_cpset[2];
static u32 s_frames, s_snaps[64]; static int s_nsnap = -1, s_snaplen = 2;
static int s_cap; static u32 s_capend, s_capframe;
static u32 *s_buf; static size_t s_blen, s_bcap;           /* captured window records */
static u32 s_used[8192]; static int s_nused;
static TexRec *s_startTex; static int s_startN;           /* texture table at window start (records kept alive) */
static u32 **s_deferred; static int s_ndef, s_capdef;
static void defer_free(u32 *p) { if (!s_cap) { free(p); return; } if (s_ndef == s_capdef) { s_capdef = s_capdef ? s_capdef * 2 : 256; s_deferred = realloc(s_deferred, s_capdef * sizeof *s_deferred); } s_deferred[s_ndef++] = p; }
static int tex_find(u32 id) { for (int i = 0; i < s_ntex; i++) if (s_tex[i].id == id) return i; return -1; }
static void bufw(const u32 *w, size_t n) { if (s_blen + n > s_bcap) { s_bcap = (s_blen + n) * 2 + 4096; s_buf = realloc(s_buf, s_bcap * 4); } memcpy(s_buf + s_blen, w, n * 4); s_blen += n; }
static void use_tex(u32 id) { for (int i = 0; i < s_nused; i++) if (s_used[i] == id) return; if (s_nused < 8192) s_used[s_nused++] = id; }
static void snap_begin(void) {
  s_cap = 1; s_capframe = s_frames; s_capend = s_frames + s_snaplen; s_blen = 0; s_nused = 0;
  s_startTex = malloc((s_ntex + 1) * sizeof *s_startTex); memcpy(s_startTex, s_tex, s_ntex * sizeof *s_tex); s_startN = s_ntex;
  /* state header */
  u32 h[3] = { RC_XF << 24 | (3 + 0x1100), 0, 0x1100 }; bufw(h, 3); bufw(s_xf, 0x1100);
  for (int i = 0; i < 2; i++) if (s_cpset[i]) { u32 r[3] = { RC_CP << 24 | 3, i ? 0x40 : 0x30, s_cp[i] }; bufw(r, 3); }
  for (int i = 0; i < 256; i++) if (s_bpset[i]) { u32 r[3] = { RC_BP << 24 | 3, (u32)i, s_bp[i] }; bufw(r, 3); }
  for (int i = 0; i < 8; i++) { u32 r[3] = { RC_TEX << 24 | 3, (u32)i, s_bound[i] }; bufw(r, 3); use_tex(s_bound[i]); }
}
static void snap_end(void) {
  char fn[600]; const char *dir = getenv("RGH_SNAPDIR"); snprintf(fn, sizeof fn, "%s/snap_%u.bin", dir ? dir : ".", s_capframe);
  FILE *f = fopen(fn, "wb");
  if (f) {
    /* textures used in the window that existed at its start go first */
    for (int u = 0; u < s_nused; u++) for (int i = 0; i < s_startN; i++) if (s_startTex[i].id == s_used[u]) { fwrite(s_startTex[i].rec, 4, s_startTex[i].rec[0] & 0xFFFFFF, f); break; }
    fwrite(s_buf, 4, s_blen, f); fclose(f);
    fprintf(stderr, "[snap] %s (%zu KB, %d textures)\n", fn, (s_blen * 4) >> 10, s_nused);
  }
  s_cap = 0; free(s_startTex); s_startTex = NULL;
  for (int i = 0; i < s_ndef; i++) free(s_deferred[i]); s_ndef = 0;
}
static void snap_rec(const u32 *rec) {
  if (s_nsnap < 0) {
    s_nsnap = 0; const char *e = getenv("RGH_SNAP");
    for (const char *p = e; p && *p && s_nsnap < 64; ) { s_snaps[s_nsnap++] = (u32)strtoul(p, (char **)&p, 10); while (*p == ',') p++; }
    if (getenv("RGH_SNAPLEN")) s_snaplen = atoi(getenv("RGH_SNAPLEN"));
    if (!s_nsnap && getenv("RGH_SNAPREQ")) s_snaps[s_nsnap++] = 0xFFFFFFFFu;   /* on request only */
  }
  if (!s_nsnap) return;
  u32 type = rec[0] >> 24, len = rec[0] & 0xFFFFFF;
  if (s_cap) { bufw(rec, len); if (type == RC_TEX) use_tex(rec[2]); }
  switch (type) {
  case RC_BP: s_bp[rec[1] & 255] = rec[2]; s_bpset[rec[1] & 255] = 1; break;
  case RC_CP: { int i = rec[1] == 0x40; s_cp[i] = rec[2]; s_cpset[i] = 1; break; }
  case RC_XF: for (u32 i = 0; i < rec[2]; i++) if (rec[1] + i < 0x1100) s_xf[rec[1] + i] = rec[3 + i]; break;
  case RC_TEX: s_bound[rec[1] & 7] = rec[2]; break;
  case RC_TEXUP: { int i = tex_find(rec[1]); u32 *c = malloc(len * 4); memcpy(c, rec, len * 4);
    if (i >= 0) { defer_free(s_tex[i].rec); s_tex[i].rec = c; }
    else { if (s_ntex == s_captex) { s_captex = s_captex ? s_captex * 2 : 1024; s_tex = realloc(s_tex, s_captex * sizeof *s_tex); } s_tex[s_ntex].id = rec[1]; s_tex[s_ntex++].rec = c; }
    break; }
  case RC_TEXFREE: { int i = tex_find(rec[1]); if (i >= 0) { defer_free(s_tex[i].rec); s_tex[i] = s_tex[--s_ntex]; } break; }
  case RC_FRAME:
    s_frames++;
    if (s_cap && s_frames >= s_capend) snap_end();
    if (!s_cap) for (int k = 0; k < s_nsnap; k++) if (s_snaps[k] == s_frames) { snap_begin(); break; }
    { static const char *rq; static int init; if (!init) { init = 1; rq = getenv("RGH_SNAPREQ"); }
      if (rq && !s_cap && (s_frames & 7) == 0) { FILE *f = fopen(rq, "r"); if (f) { fclose(f); remove(rq); snap_begin(); } } }
    break;
  }
}
#endif
void ring_commit(void) {
  if (!ring_reserved) return;
#ifndef __wasm__
  snap_rec((const u32 *)(g_ring->data + g_ring->wpos % g_ring->size));
#endif
  __atomic_store_n(&g_ring->wpos, g_ring->wpos + ring_reserved, __ATOMIC_SEQ_CST);
  ring_reserved = 0;
}
static void emit2(u32 type, u32 a, u32 b) {
  u32 *p = ring_alloc(3); if (!p) return;
  p[0] = type << 24 | 3; p[1] = a; p[2] = b; ring_commit();
}

/* ---------------------------------------------------------------- vertex decoding */
static inline f32 rdcomp(const u8 *p, int fmt, f32 scale) {
  switch (fmt) {
    case 0: return p[0] * scale;
    case 1: return (s8)p[0] * scale;
    case 2: return (u16)(p[0] << 8 | p[1]) * scale;
    case 3: return (s16)(p[0] << 8 | p[1]) * scale;
    default: { u32 v = (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; f32 f; memcpy(&f, &v, 4); return f; }
  }
}
static const int csize[8] = {1, 1, 2, 2, 4, 1, 1, 1};
static u32 rdcolor(const u8 *p, int comp, int *len) {   /* -> RGBA8 packed little endian (r in low byte) */
  u32 r, g, b, a = 255;
  switch (comp) {
    case 0: { u16 v = p[0] << 8 | p[1]; r = (v >> 11) * 255 / 31; g = ((v >> 5) & 63) * 255 / 63; b = (v & 31) * 255 / 31; *len = 2; break; }
    case 1: r = p[0]; g = p[1]; b = p[2]; *len = 3; break;
    case 2: r = p[0]; g = p[1]; b = p[2]; *len = 4; break;
    case 3: { u16 v = p[0] << 8 | p[1]; r = (v >> 12) * 17; g = ((v >> 8) & 15) * 17; b = ((v >> 4) & 15) * 17; a = (v & 15) * 17; *len = 2; break; }
    case 4: { u32 v = (u32)p[0] << 16 | p[1] << 8 | p[2]; r = (v >> 18) * 255 / 63; g = ((v >> 12) & 63) * 255 / 63; b = ((v >> 6) & 63) * 255 / 63; a = (v & 63) * 255 / 63; *len = 3; break; }
    default: r = p[0]; g = p[1]; b = p[2]; a = p[3]; *len = 4; break;
  }
  return r | g << 8 | b << 16 | a << 24;
}
static const u8 *array_ptr(int arr, u32 idx) {
  u32 base = g_cp[0xA0 + arr] & 0x1FFFFFFF, stride = g_cp[0xB0 + arr] & 0xFF;
  u8 *p = hp((base + idx * stride) | 0x80000000);
  return p;
}


#ifndef __wasm__
/* debug/tooling: RGH_SHADERS=file collects every distinct shader configuration (the same key as
 * web/gx-renderer.js program()); the file is shipped as web/shaders.bin so the browser can compile
 * all shaders before they are first needed. Record: mask, BP[256], XF[0x1000..0x105F] (353 words). */
static void shader_rec(u32 mask) {
  static FILE *f; static int init; static u64 *keys; static int nk, ck;
  if (!init) { init = 1; const char *p = getenv("RGH_SHADERS"); if (p) f = fopen(p, "ab"); }
  if (!f) return;
  const u32 *bp = g_bp, *xf = g_xf;
  u32 gen = bp[0], nst = ((gen >> 10) & 15) + 1, nind = (gen >> 16) & 7, ntg = xf[0x103F] & 15; if (ntg > 8) ntg = 8;
  u32 parts[160]; int n = 0;
  parts[n++] = mask; parts[n++] = nst; parts[n++] = ntg; parts[n++] = xf[0x1009] & 3; parts[n++] = nind;
  parts[n++] = xf[0x100E]; parts[n++] = xf[0x100F]; parts[n++] = xf[0x1010]; parts[n++] = xf[0x1011]; parts[n++] = xf[0x1012] & 1;
  parts[n++] = (bp[0xF3] >> 16) & 255; parts[n++] = bp[0xF1] >> 21;
  for (u32 i = 0; i < nst; i++) { parts[n++] = bp[0xC0 + 2 * i] & 0xFFFFFF; parts[n++] = bp[0xC1 + 2 * i] & 0xFFFFFF; parts[n++] = (bp[0x28 + (i >> 1)] >> ((i & 1) * 12)) & 0xFFF; parts[n++] = bp[0x10 + i] & 0x1FFFFF; }
  for (int i = 0; i < 8; i++) parts[n++] = bp[0xF6 + i] & 0xFFFFFF;
  for (u32 i = 0; i < ntg; i++) { parts[n++] = xf[0x1040 + i]; parts[n++] = xf[0x1050 + i] & 0x13F; }
  parts[n++] = bp[0x27] & 0xFFFFFF; parts[n++] = bp[0x25] & 0xFFFFFF; parts[n++] = bp[0x26] & 0xFFFFFF;
  u64 h = 1469598103934665603ull; for (int i = 0; i < n; i++) { h ^= parts[i]; h *= 1099511628211ull; h ^= h >> 31; }
  for (int i = 0; i < nk; i++) if (keys[i] == h) return;
  if (nk == ck) { ck = ck ? ck * 2 : 1024; keys = realloc(keys, ck * sizeof *keys); }
  keys[nk++] = h;
  u32 rec[353]; rec[0] = mask; memcpy(rec + 1, bp, 256 * 4); memcpy(rec + 257, xf + 0x1000, 0x60 * 4);
  fwrite(rec, 4, 353, f); fflush(f);
}
#else
static void shader_rec(u32 mask) { (void)mask; }
#endif

#ifndef __wasm__
/* RGH_DECODECHECK=1 (native): every draw is decoded a second time with the original, simple decoder and
 * compared with the fast path */
static u8 vbuf[1 << 22];
static void decode_check(const u8 *data, int n, const u8 *got, u32 bytes, u32 lo, int pt, int nt, int c0t, int c1t, int nbt, int n3,
                         const int *tt, const int *tel, const int *tfmt, const int *tfrac, int pel, int pfmt, int nfmt, int c0c, int c1c,
                         f32 pscale, f32 nscale, int hasIdx) {
  static int on = -1; static long nchk, nbad; if (on < 0) on = getenv("RGH_DECODECHECK") != NULL;
  if (!on) return;
  u8 *o = vbuf;
  const u8 *p = data;
  int isz[4] = {0, 0, 1, 2};
  for (int v = 0; v < n; v++) {
    u8 pmi = (u8)(g_cp[0x30] & 63), tmi[8];
    for (int i = 0; i < 8; i++) tmi[i] = (u8)(i < 4 ? (g_cp[0x30] >> (6 + 6 * i)) & 63 : (g_cp[0x40] >> (6 * (i - 4))) & 63);
    if (lo & 1) pmi = *p++;
    for (int i = 0; i < 8; i++) if (lo & (2 << i)) tmi[i] = *p++;
    /* position */
    { const u8 *s = p; int cnt = pel ? 3 : 2;
      if (pt != 1) { u32 idx = pt == 2 ? p[0] : (u32)(p[0] << 8 | p[1]); p += isz[pt]; s = array_ptr(0, idx); } else p += cnt * csize[pfmt];
      f32 xyz[3] = {0, 0, 0};
      if (s) for (int k = 0; k < cnt; k++) xyz[k] = rdcomp(s + k * csize[pfmt], pfmt, pscale);
      memcpy(o, xyz, 12); o += 12; }
    if (nt) {
      int cnt = nbt ? 9 : 3; f32 nn[9] = {0};
      if (nt == 1) { for (int k = 0; k < cnt; k++) nn[k] = rdcomp(p + k * csize[nfmt], nfmt, nscale); p += cnt * csize[nfmt]; }
      else if (nbt && n3) {
        for (int j = 0; j < 3; j++) { u32 idx = nt == 2 ? p[0] : (u32)(p[0] << 8 | p[1]); p += isz[nt];
          const u8 *s = array_ptr(1, idx); if (s) for (int k = 0; k < 3; k++) nn[j * 3 + k] = rdcomp(s + (j * 3 + k) * 0 + k * csize[nfmt], nfmt, nscale); }
      } else {
        u32 idx = nt == 2 ? p[0] : (u32)(p[0] << 8 | p[1]); p += isz[nt];
        const u8 *s = array_ptr(1, idx); if (s) for (int k = 0; k < cnt; k++) nn[k] = rdcomp(s + k * csize[nfmt], nfmt, nscale);
      }
      memcpy(o, nn, nbt ? 36 : 12); o += nbt ? 36 : 12;
    }
    for (int ci = 0; ci < 2; ci++) {
      int ct = ci ? c1t : c0t, cc = ci ? c1c : c0c; if (!ct) continue;
      int len; u32 col;
      if (ct == 1) { col = rdcolor(p, cc, &len); p += len; }
      else { u32 idx = ct == 2 ? p[0] : (u32)(p[0] << 8 | p[1]); p += isz[ct]; const u8 *s = array_ptr(2 + ci, idx); col = s ? rdcolor(s, cc, &len) : 0xFFFFFFFF; }
      memcpy(o, &col, 4); o += 4;
    }
    for (int i = 0; i < 8; i++) {
      if (!tt[i]) continue;
      int cnt = tel[i] ? 2 : 1; f32 sc = 1.0f / (f32)(1u << tfrac[i]); f32 st[2] = {0, 0};
      const u8 *s = p;
      if (tt[i] != 1) { u32 idx = tt[i] == 2 ? p[0] : (u32)(p[0] << 8 | p[1]); p += isz[tt[i]]; s = array_ptr(4 + i, idx); } else p += cnt * csize[tfmt[i]];
      if (s) for (int k = 0; k < cnt; k++) st[k] = rdcomp(s + k * csize[tfmt[i]], tfmt[i], sc);
      memcpy(o, st, 8); o += 8;
    }
    if (hasIdx) { o[0] = pmi; memcpy(o + 1, tmi, 8); o[9] = o[10] = o[11] = 0; o += 12; }
  }
  nchk++;
  if ((u32)(o - vbuf) != bytes || memcmp(vbuf, got, bytes)) { if (nbad++ < 10) fprintf(stderr, "[decodecheck] mismatch draw %ld (%u vs %u bytes)\n", nchk, (u32)(o - vbuf), bytes); }
  if ((nchk & 0xFFFFF) == 0) fprintf(stderr, "[decodecheck] %ld draws checked, %ld mismatches\n", nchk, nbad);
}
#endif
static void tex_bind_all(void);
void gx_backend_draw(int prim, int vat, int n, const u8 *data, u32 vsize) {
  (void)vsize;
  if (!g_ring || !g_ring->consumer || n <= 0) return;
  tex_bind_all();
  u32 lo = g_cp[0x50], hi = g_cp[0x60];
  u32 A = g_cp[0x70 + vat], B = g_cp[0x80 + vat], C = g_cp[0x90 + vat];
  int pt = (lo >> 9) & 3, nt = (lo >> 11) & 3, c0t = (lo >> 13) & 3, c1t = (lo >> 15) & 3;
  int nbt = (A >> 9) & 1, n3 = (A >> 31) & 1;
  int tt[8], tel[8], tfmt[8], tfrac[8];
  for (int i = 0; i < 8; i++) tt[i] = (hi >> (2 * i)) & 3;
  tel[0] = (A >> 21) & 1; tfmt[0] = (A >> 22) & 7; tfrac[0] = (A >> 25) & 31;
  tel[1] = B & 1; tfmt[1] = (B >> 1) & 7; tfrac[1] = (B >> 4) & 31;
  tel[2] = (B >> 9) & 1; tfmt[2] = (B >> 10) & 7; tfrac[2] = (B >> 13) & 31;
  tel[3] = (B >> 18) & 1; tfmt[3] = (B >> 19) & 7; tfrac[3] = (B >> 22) & 31;
  tel[4] = (B >> 27) & 1; tfmt[4] = (B >> 28) & 7; tfrac[4] = C & 31;
  tel[5] = (C >> 5) & 1; tfmt[5] = (C >> 6) & 7; tfrac[5] = (C >> 9) & 31;
  tel[6] = (C >> 14) & 1; tfmt[6] = (C >> 15) & 7; tfrac[6] = (C >> 18) & 31;
  tel[7] = (C >> 23) & 1; tfmt[7] = (C >> 24) & 7; tfrac[7] = (C >> 27) & 31;
  int pel = A & 1, pfmt = (A >> 1) & 7, pfrac = (A >> 4) & 31;
  int nfmt = (A >> 10) & 7;
  int c0c = (A >> 14) & 7, c1c = (A >> 18) & 7;
  f32 pscale = 1.0f / (f32)(1u << pfrac);
  f32 nscale = (nfmt == 1 || nfmt == 0) ? 1.0f / 64 : (nfmt == 3 || nfmt == 2) ? 1.0f / 16384 : 1.0f;
  u32 mask = VA_POS, stride = 12;
  if (nt) { mask |= VA_NRM; stride += 12; if (nbt) { mask |= VA_NBT; stride += 24; } }
  if (c0t) { mask |= VA_COL0; stride += 4; }
  if (c1t) { mask |= VA_COL1; stride += 4; }
  for (int i = 0; i < 8; i++) if (tt[i]) { mask |= VA_TEX0 << i; stride += 8; }
  int hasIdx = (lo & 0x1FF) != 0;
  if (hasIdx) { mask |= VA_PMI | VA_TMI; stride += 12; }
  if ((u64)stride * n > (1u << 22)) n = (int)((1u << 22) / stride);
  shader_rec(mask);
  /* the vertex data is placed at a ring offset that is a multiple of the vertex size (x4 for quads), so the
   * renderer can draw every batch with the same layout from one vertex array object; it is decoded straight
   * into the ring */
  u32 bytes = stride * (u32)n;
  u32 period = (prim <= 1 ? 4 : 1) * stride, maxpad = period / 4;
  u32 *r = ring_alloc(5 + maxpad + bytes / 4); if (!r) return;
  u32 doff = (u32)((u8 *)(r + 5) - g_ring->data), pad = period ? ((period - doff % period) % period) / 4 : 0;
  u32 words = 5 + pad + bytes / 4;
  ring_reserved = words * 4;
  r[0] = RC_DRAW << 24 | words; r[1] = (u32)prim; r[2] = (u32)n; r[3] = mask; r[4] = stride | pad << 16;
  u8 *o = (u8 *)(r + 5 + pad);
  const u8 *p = data;
  static const int isz[4] = {0, 0, 1, 2};
  /* loop invariants: default matrix indices, array bases, component sizes and scales */
  u8 pmi0 = (u8)(g_cp[0x30] & 63), tmi0[8];
  for (int i = 0; i < 8; i++) tmi0[i] = (u8)(i < 4 ? (g_cp[0x30] >> (6 + 6 * i)) & 63 : (g_cp[0x40] >> (6 * (i - 4))) & 63);
  int pcnt = pel ? 3 : 2, psz = csize[pfmt];
  int ncnt = nbt ? 9 : 3, nsz = csize[nfmt];
  f32 tsc[8]; int tcnt[8], tsz[8];
  for (int i = 0; i < 8; i++) { tsc[i] = 1.0f / (f32)(1u << tfrac[i]); tcnt[i] = tel[i] ? 2 : 1; tsz[i] = csize[tfmt[i]]; }
  int ntex = 0, tlist[8]; for (int i = 0; i < 8; i++) if (tt[i]) tlist[ntex++] = i;
  #define IDX(t) ((t) == 2 ? (u32)p[0] : (u32)(p[0] << 8 | p[1]))
  for (int v = 0; v < n; v++) {
    u8 pmi = pmi0, tmi[8]; memcpy(tmi, tmi0, 8);
    if (hasIdx) { if (lo & 1) pmi = *p++; for (int i = 0; i < 8; i++) if (lo & (2 << i)) tmi[i] = *p++; }
    /* position */
    { f32 xyz[3] = {0, 0, 0}; const u8 *s = p;
      if (pt != 1) { u32 idx = IDX(pt); p += isz[pt]; s = array_ptr(0, idx); } else p += pcnt * psz;
      if (s) {
        if (pfmt == 4) for (int k = 0; k < pcnt; k++) { u32 w; memcpy(&w, s + 4 * k, 4); w = __builtin_bswap32(w); memcpy(&xyz[k], &w, 4); }
        else if (pfmt == 3) for (int k = 0; k < pcnt; k++) xyz[k] = (s16)(s[2 * k] << 8 | s[2 * k + 1]) * pscale;
        else for (int k = 0; k < pcnt; k++) xyz[k] = rdcomp(s + k * psz, pfmt, pscale);
      }
      memcpy(o, xyz, 12); o += 12; }
    if (nt) {
      f32 nn[9] = {0};
      if (nt == 1) { for (int k = 0; k < ncnt; k++) nn[k] = rdcomp(p + k * nsz, nfmt, nscale); p += ncnt * nsz; }
      else if (nbt && n3) {
        for (int j = 0; j < 3; j++) { u32 idx = IDX(nt); p += isz[nt];
          const u8 *s = array_ptr(1, idx); if (s) for (int k = 0; k < 3; k++) nn[j * 3 + k] = rdcomp(s + k * nsz, nfmt, nscale); }
      } else {
        u32 idx = IDX(nt); p += isz[nt];
        const u8 *s = array_ptr(1, idx); if (s) for (int k = 0; k < ncnt; k++) nn[k] = rdcomp(s + k * nsz, nfmt, nscale);
      }
      memcpy(o, nn, nbt ? 36 : 12); o += nbt ? 36 : 12;
    }
    for (int ci = 0; ci < 2; ci++) {
      int ct = ci ? c1t : c0t, cc = ci ? c1c : c0c; if (!ct) continue;
      int len; u32 col;
      if (ct == 1) { col = rdcolor(p, cc, &len); p += len; }
      else { u32 idx = IDX(ct); p += isz[ct]; const u8 *s = array_ptr(2 + ci, idx); col = s ? rdcolor(s, cc, &len) : 0xFFFFFFFF; }
      memcpy(o, &col, 4); o += 4;
    }
    for (int j = 0; j < ntex; j++) {
      int i = tlist[j], cnt = tcnt[i], fm = tfmt[i]; f32 st[2] = {0, 0};
      const u8 *s = p;
      if (tt[i] != 1) { u32 idx = IDX(tt[i]); p += isz[tt[i]]; s = array_ptr(4 + i, idx); } else p += cnt * tsz[i];
      if (s) {
        if (fm == 4) for (int k = 0; k < cnt; k++) { u32 w; memcpy(&w, s + 4 * k, 4); w = __builtin_bswap32(w); memcpy(&st[k], &w, 4); }
        else for (int k = 0; k < cnt; k++) st[k] = rdcomp(s + k * tsz[i], fm, tsc[i]);
      }
      memcpy(o, st, 8); o += 8;
    }
    if (hasIdx) { o[0] = pmi; memcpy(o + 1, tmi, 8); o[9] = o[10] = o[11] = 0; o += 12; }
  }
  #undef IDX
#ifndef __wasm__
  decode_check(data, n, (const u8 *)(r + 5 + pad), bytes, lo, pt, nt, c0t, c1t, nbt, n3, tt, tel, tfmt, tfrac, pel, pfmt, nfmt, c0c, c1c, pscale, nscale, hasIdx);
#endif
  ring_commit();
}

/* ---------------------------------------------------------------- register forwarding */
static int tex_dirty = 0xFF;
static u32 g_tlut_gen = 1;   /* bumped by every TLUT load */
void gx_backend_bp(u32 reg, u32 val) {
  if ((reg >= 0x80 && reg <= 0xBB) || reg == 0x64 || reg == 0x65) tex_dirty = 0xFF;
  if (reg == 0x65) {   /* TLUT load: copy palette data from memory into TMEM */
    extern u8 g_tmem[];
    u32 src = (g_bp[0x64] & 0xFFFFFF) << 5, tm = (val & 0x3FF) << 9, cnt = ((val >> 10) & 0x7FF) * 32;
    u8 *s = hp(src | 0x80000000);
    if (s && tm + cnt <= 0x100000) memcpy(g_tmem + tm, s, cnt);
    g_tlut_gen++;
  }
  if (reg == 0x52) {
    extern void gx_efb_copy(u32 val);
    gx_efb_copy(val);
    return;
  }
  if (reg == 0xFE || reg == 0x45 || reg == 0x47 || reg == 0x48) return;
  emit2(RC_BP, reg, val);
}
void gx_backend_xf(u32 addr, u32 cnt, const u32 *vals) {
  if (!g_ring || !g_ring->consumer) return;
  u32 *r = ring_alloc(3 + cnt); if (!r) return;
  r[0] = RC_XF << 24 | (3 + cnt); r[1] = addr; r[2] = cnt; memcpy(r + 3, vals, cnt * 4);
  ring_commit();
}
void gx_backend_cp(u32 reg, u32 val) { if (reg == 0x30 || reg == 0x40) emit2(RC_CP, reg, val); }

/* ---------------------------------------------------------------- textures */
u8 g_tmem[0x100000];
typedef struct { u32 key_addr, w, h, fmt, levels; u64 hash; u32 id; u32 last; int efb; u32 hframe, tlut, stable, pixels; } TexEnt;
static u64 tex_pixels_total;              /* decoded texels held by the GPU (RGBA8 each) */
#define TEX_BUDGET (80u << 20)             /* about 320 MB of video memory for textures */
#define NTEX 4096
static TexEnt texc[NTEX]; static u32 tex_next_id = 1, tex_clock;
#define TMAP 16384
static int tmap[TMAP];          /* texture key -> cache entry + 1 */
static u32 bound[8];

static u64 hash_mem(const u8 *p, u32 n) {
  u64 h = 0x9E3779B97F4A7C15ull ^ n;
  u32 step = n > 65536 ? 16 : n > 8192 ? 8 : 4;   /* larger textures: sample every 2nd / 4th word */
  for (u32 i = 0; i + 4 <= n; i += step) { u32 w; memcpy(&w, p + i, 4); h = (h ^ w) * 0x100000001B3ull; h ^= h >> 29; }
  return h;
}
static u32 rgb565(u16 v) { u32 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31; return (r << 3 | r >> 2) | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2) << 16 | 0xFF000000u; }
static u32 rgb5a3(u16 v) {
  if (v & 0x8000) { u32 r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31; return (r << 3 | r >> 2) | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2) << 16 | 0xFF000000u; }
  u32 a = (v >> 12) & 7, r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;
  return r * 17 | (g * 17) << 8 | (b * 17) << 16 | ((a << 5) | (a << 2) | (a >> 1)) << 24;
}
static u32 ia8(u16 v) { u32 a = v >> 8, i = v & 0xFF; return i | i << 8 | i << 16 | a << 24; }
static u32 tlut_color(u32 tlut, u32 idx) {
  u32 off = ((tlut & 0x3FF) << 9) + idx * 2; int f = (tlut >> 10) & 3;
  u16 v = (u16)(g_tmem[off & 0xFFFFF] << 8 | g_tmem[(off + 1) & 0xFFFFF]);
  return f == 0 ? ia8(v) : f == 1 ? rgb565(v) : rgb5a3(v);
}
/* block sizes per format */
static void blk(int fmt, int *bw, int *bh, int *bpb) {
  switch (fmt) {
    case 0: case 8: case 14: *bw = 8; *bh = 8; *bpb = 32; break;   /* I4, C4, CMPR */
    case 1: case 2: case 9: *bw = 8; *bh = 4; *bpb = 32; break;    /* I8, IA4, C8 */
    case 6: *bw = 4; *bh = 4; *bpb = 64; break;                    /* RGBA8 */
    default: *bw = 4; *bh = 4; *bpb = 32; break;                   /* IA8, RGB565, RGB5A3, C14X2 */
  }
}
u32 gx_tex_size(int fmt, u32 w, u32 h) { int bw, bh, bpb; blk(fmt, &bw, &bh, &bpb); return ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bpb; }
static void dxt_block(const u8 *s, u32 out[16]) {
  u16 c0 = (u16)(s[0] << 8 | s[1]), c1 = (u16)(s[2] << 8 | s[3]);
  u32 p[4]; p[0] = rgb565(c0); p[1] = rgb565(c1);
  u32 r0 = p[0] & 0xFF, g0 = (p[0] >> 8) & 0xFF, b0 = (p[0] >> 16) & 0xFF, r1 = p[1] & 0xFF, g1 = (p[1] >> 8) & 0xFF, b1 = (p[1] >> 16) & 0xFF;
  if (c0 > c1) {
    p[2] = ((2 * r0 + r1) / 3) | ((2 * g0 + g1) / 3) << 8 | ((2 * b0 + b1) / 3) << 16 | 0xFF000000u;
    p[3] = ((r0 + 2 * r1) / 3) | ((g0 + 2 * g1) / 3) << 8 | ((b0 + 2 * b1) / 3) << 16 | 0xFF000000u;
  } else {
    p[2] = ((r0 + r1) / 2) | ((g0 + g1) / 2) << 8 | ((b0 + b1) / 2) << 16 | 0xFF000000u;
    p[3] = 0;
  }
  for (int y = 0; y < 4; y++) { u8 row = s[4 + y]; for (int x = 0; x < 4; x++) out[y * 4 + x] = p[(row >> (6 - 2 * x)) & 3]; }
}
static void decode_level(const u8 *src, int fmt, u32 w, u32 h, u32 tlut, u32 *dst) {
  int bw, bh, bpb; blk(fmt, &bw, &bh, &bpb);
  u32 bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh;
  for (u32 j = 0; j < by; j++) for (u32 i = 0; i < bx; i++) {
    const u8 *b = src + (j * bx + i) * bpb;
    for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) {
      u32 px = i * bw + x, py = j * bh + y; u32 c;
      switch (fmt) {
        case 0: { u8 v = b[(y * bw + x) / 2]; v = (x & 1) ? v & 15 : v >> 4; v *= 17; c = v | v << 8 | v << 16 | (u32)v << 24; break; }
        case 1: { u8 v = b[y * bw + x]; c = v | v << 8 | v << 16 | (u32)v << 24; break; }
        case 2: { u8 v = b[y * bw + x]; u32 a = (v >> 4) * 17, l = (v & 15) * 17; c = l | l << 8 | l << 16 | a << 24; break; }
        case 3: { const u8 *q = b + (y * 4 + x) * 2; c = ia8((u16)(q[0] << 8 | q[1])); break; }
        case 4: { const u8 *q = b + (y * 4 + x) * 2; c = rgb565((u16)(q[0] << 8 | q[1])); break; }
        case 5: { const u8 *q = b + (y * 4 + x) * 2; c = rgb5a3((u16)(q[0] << 8 | q[1])); break; }
        case 6: { int k = y * 4 + x; u32 a = b[k * 2], r = b[k * 2 + 1], g = b[32 + k * 2], bb = b[32 + k * 2 + 1]; c = r | g << 8 | bb << 16 | a << 24; break; }
        case 8: { u8 v = b[(y * bw + x) / 2]; v = (x & 1) ? v & 15 : v >> 4; c = tlut_color(tlut, v); break; }
        case 9: c = tlut_color(tlut, b[y * bw + x]); break;
        case 10: { const u8 *q = b + (y * 4 + x) * 2; c = tlut_color(tlut, (u16)(q[0] << 8 | q[1]) & 0x3FFF); break; }
        case 14: { /* 8x8 = 2x2 DXT1 sub-blocks */
          static u32 sub[16]; int sb = (y / 4) * 2 + (x / 4);
          dxt_block(b + sb * 8, sub); c = sub[(y & 3) * 4 + (x & 3)]; break; }
        default: c = 0xFFFF00FF; break;
      }
      if (px < w && py < h) dst[py * w + px] = c;
    }
  }
}

#ifndef __wasm__
/* debug: write decoded textures as PNG (RGH_TEXDUMP=dir) */
static u32 crc_tab[256];
static u32 crc(u32 c, const u8 *b, u32 n) { if (!crc_tab[1]) for (u32 i = 0; i < 256; i++) { u32 k = i; for (int j = 0; j < 8; j++) k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1; crc_tab[i] = k; }
  c = ~c; for (u32 i = 0; i < n; i++) c = crc_tab[(c ^ b[i]) & 255] ^ (c >> 8); return ~c; }
static void be(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void png_chunk(FILE *f, const char *t, const u8 *d, u32 n) { u8 h[8]; be(h, n); memcpy(h + 4, t, 4); fwrite(h, 1, 8, f); if (n) fwrite(d, 1, n, f);
  u32 c = crc(0, (const u8 *)t, 4); c = crc(c, d, n); u8 cc[4]; be(cc, c); fwrite(cc, 1, 4, f); }
static void write_png(const char *path, const u32 *px, u32 w, u32 h) {
  FILE *f = fopen(path, "wb"); if (!f) return;
  fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
  u8 ih[13]; be(ih, w); be(ih + 4, h); ih[8] = 8; ih[9] = 6; ih[10] = ih[11] = ih[12] = 0; png_chunk(f, "IHDR", ih, 13);
  u32 row = w * 4 + 1, raw = row * h;
  u32 nblk = (raw + 65534) / 65535; u32 zl = 2 + raw + nblk * 5 + 4;
  u8 *z = malloc(zl), *q = z; *q++ = 0x78; *q++ = 1;
  u8 *r = malloc(raw); for (u32 y = 0; y < h; y++) { r[y * row] = 0; memcpy(r + y * row + 1, px + y * w, w * 4); }
  u32 a = 1, b = 0; for (u32 i = 0; i < raw; i++) { a = (a + r[i]) % 65521; b = (b + a) % 65521; }
  for (u32 o = 0; o < raw; o += 65535) { u32 n = raw - o > 65535 ? 65535 : raw - o; *q++ = o + n >= raw; *q++ = n; *q++ = n >> 8; *q++ = ~n; *q++ = ~n >> 8; memcpy(q, r + o, n); q += n; }
  be(q, b << 16 | a); q += 4;
  png_chunk(f, "IDAT", z, (u32)(q - z)); png_chunk(f, "IEND", NULL, 0); fclose(f); free(z); free(r);
}
#endif

/* EFB copies made into memory (render-to-texture): address -> renderer texture id */
/* Up to 1024 copies are remembered (before: 64, and when full a random one was forgotten, so a texture made once,
 * such as the customized Rabbid's skin, could vanish and be drawn black). When full the least recently used goes.
 * A copy is also forgotten when the game writes something else to that memory (it was reused for another texture). */
#define NALIAS 1024
typedef struct { u32 addr, w, h, id, frame, used, sig, sigframe, fmt, bytes, wb; } EfbAlias;
static EfbAlias efb_alias[NALIAS]; static int n_alias;
static int alias_ht[4096];            /* addr -> index + 1, open addressing; rebuilt when an entry is replaced */
static u32 g_frame_no;
static u32 alias_sig(u32 addr, u32 bytes) {   /* sample of the memory behind a copy: changes when the game reuses it */
  u8 *p = hp(addr | 0x80000000u); if (!p || bytes < 4) return 0;
  u32 step = (bytes / 64) & ~3u; if (!step) step = 4;
  u32 h = 2166136261u; for (u32 o = 0, n = 0; o + 4 <= bytes && n < 64; o += step, n++) { u32 v; memcpy(&v, p + o, 4); h = (h ^ v) * 16777619u; }
  return h;
}
static void alias_rehash(void) {
  memset(alias_ht, 0, sizeof alias_ht);
  for (int i = 0; i < n_alias; i++) { if (!efb_alias[i].addr && !efb_alias[i].id) continue; u32 s = (efb_alias[i].addr >> 5) * 2654435761u >> 20; while (alias_ht[s]) s = (s + 1) & 4095; alias_ht[s] = i + 1; }
}
static int alias_find(u32 addr) {
  u32 s = (addr >> 5) * 2654435761u >> 20;
  for (int n = 0; n < 4096 && alias_ht[s]; n++, s = (s + 1) & 4095) if (efb_alias[alias_ht[s] - 1].addr == addr && efb_alias[alias_ht[s] - 1].id) return alias_ht[s] - 1;
  return -1;
}

/* EFB copies into memory are made on the GPU only. When the game reads one back with the CPU (the photo of the
 * customized Rabbid is saved as a JPEG taken from such a copy; before, it came out black) it first invalidates
 * the data cache (DCInvalidateRange) or waits for the GPU (GXDrawDone): then the copy is written to memory,
 * in the texture format the game asked for, and the game waits until it is there. */
u32 g_wb_pending, g_wb_lo, g_wb_hi;
static volatile u32 wb_flag;
static void writeback(EfbAlias *e) {
  e->wb = 0; if (g_wb_pending) g_wb_pending--;
  { static int nl; if (nl++ < 50) fprintf(stderr, "[gx] EFB copy written back to memory: %08X %ux%u format %u\n", e->addr, e->w, e->h, e->fmt); }
  if (!e->id || !g_ring || !g_ring->consumer) return;
#ifdef __wasm__
  u8 *dst = hp(e->addr | 0x80000000u); if (!dst) return;
  wb_flag = 0;
  u32 *r = ring_alloc(7); if (!r) return;
  r[0] = RC_READBACK << 24 | 7; r[1] = e->id; r[2] = (u32)(uintptr_t)dst; r[3] = e->w; r[4] = e->h; r[5] = e->fmt; r[6] = (u32)(uintptr_t)&wb_flag;
  ring_commit();
  for (int t = 0; t < 60 && !__atomic_load_n(&wb_flag, __ATOMIC_SEQ_CST); t++) {   /* at most 3 s */
    __builtin_wasm_memory_atomic_wait32((int *)&wb_flag, 0, 50000000);
    if (!g_ring->consumer) break;
  }
  e->sig = alias_sig(e->addr, e->w * e->h);     /* the memory now holds the copy: not "reused" */
#endif
}
void gx_writeback_addr(u32 a) {
  a &= 0x1FFFFFFF;
  for (int i = 0; i < n_alias; i++) { EfbAlias *e = &efb_alias[i]; if (e->wb && a >= e->addr && a < e->addr + e->bytes) writeback(e); }
  if (!g_wb_pending) g_wb_lo = g_wb_hi = 0;
}
void gx_writeback_all(void) {
  for (int i = 0; i < n_alias && g_wb_pending; i++) if (efb_alias[i].wb) writeback(&efb_alias[i]);
  g_wb_pending = 0; g_wb_lo = g_wb_hi = 0;
}

static u32 tex_lookup(int slot) {
  u32 r0 = slot < 4 ? 0x80 + slot : 0xA0 + slot - 4;
  u32 mode0 = g_bp[r0], img0 = g_bp[r0 + 8], img3 = g_bp[r0 + 0x14], tlut = g_bp[r0 + 0x18];
  u32 mode1 = g_bp[r0 + 4];
  u32 w = (img0 & 0x3FF) + 1, h = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 15;
  u32 addr = (img3 & 0xFFFFFF) << 5;
  { int i = alias_find(addr);
    if (i >= 0 && efb_alias[i].w == w && efb_alias[i].h == h) {
      EfbAlias *e = &efb_alias[i]; e->used = g_frame_no;
      if (e->sigframe != g_frame_no) { e->sigframe = g_frame_no;
        if (alias_sig(addr, e->w * e->h) != e->sig) {   /* memory reused for something else: the copy is gone */
          { static int nl; if (nl++ < 20) fprintf(stderr, "[gx] EFB copy %08X replaced by other data\n", addr); }
          emit2(RC_TEXFREE, e->id, 0); for (int k = 0; k < 8; k++) if (bound[k] == e->id) { bound[k] = 0; tex_dirty |= 1 << k; }
          if (e->wb) { e->wb = 0; if (g_wb_pending) g_wb_pending--; }
          e->id = 0; e->addr = 0; alias_rehash(); goto not_alias; } }
      return e->id; } }
  not_alias:;
  int mip = ((mode0 >> 5) & 7) >= 2;
  u32 levels = 1;
  if (mip) { u32 maxl = (mode1 >> 8) & 0xFF; levels = maxl / 16 + 1; u32 ww = w, hh = h; u32 lv = 1; while ((ww > 1 || hh > 1) && lv < levels) { ww = ww > 1 ? ww / 2 : 1; hh = hh > 1 ? hh / 2 : 1; lv++; } levels = lv; if (levels > 11) levels = 11; }
  u32 total = 0; { u32 ww = w, hh = h; for (u32 l = 0; l < levels; l++) { total += gx_tex_size((int)fmt, ww, hh); ww = ww > 1 ? ww / 2 : 1; hh = hh > 1 ? hh / 2 : 1; } }
  u8 *src = hp(addr | 0x80000000);
  if (!src || addr + total > 0x14000000) return 0;
#ifndef __wasm__
  { static int dbg = -1; if (dbg < 0) dbg = getenv("RGH_ZEROTEX") != NULL;
    if (dbg && w >= 128 && h >= 128) { int z = 1; for (u32 i = 0; i < 1024 && z; i += 4) if (*(u32 *)(src + i)) z = 0;
      static u32 seen[64]; static int ns; int k = 0; for (; k < ns; k++) if (seen[k] == addr) break;
      if (z && k == ns && ns < 64) { seen[ns++] = addr; fprintf(stderr, "[zerotex] %08X %ux%u fmt %u frame %u\n", addr, w, h, fmt, g_frame_no); } } }
#endif
  /* find the cached texture for this address/size/format without scanning the whole cache; its memory is
   * hashed again at most once per frame (games bind the same textures many times per frame) */
  u32 hk = (addr * 2654435761u) ^ (w * 40503u) ^ (h * 9973u) ^ (fmt * 131u) ^ levels;
  u64 pal = 0;
  if (fmt == 8 || fmt == 9 || fmt == 10) {
    /* palette hash, remembered until the next TLUT load (palettes only change through BP 0x65) */
    static u32 pk[64], pg[64]; static u64 ph[64];
    u32 key = (tlut & 0x3FFF) | fmt << 16, sl = (key * 2654435761u) >> 26;
    if (pk[sl] == key + 1 && pg[sl] == g_tlut_gen) pal = ph[sl];
    else { u32 n = fmt == 8 ? 16 : fmt == 9 ? 256 : 16384; pal = hash_mem(g_tmem + (((tlut & 0x3FF) << 9) & 0xFFFFF), n * 2 > 0x8000 ? 0x8000 : n * 2) * 31 + (tlut >> 10);
           pk[sl] = key + 1; pg[sl] = g_tlut_gen; ph[sl] = pal; }
    hk ^= (u32)pal; }
  tex_clock++;
  TexEnt *hit = NULL; int hslot = -1;
  for (int k = 0; k < 32; k++) {
    int si = (int)((hk + (u32)k) & (TMAP - 1)), e = tmap[si] - 1;
    if (e < 0) { if (hslot < 0) hslot = si; break; }
    TexEnt *t = &texc[e];
    if (t->id && t->key_addr == addr && t->w == w && t->h == h && t->fmt == fmt && t->levels == levels && t->tlut == (u32)pal) { hit = t; hslot = si; break; }
    if (hslot < 0 && !t->id) hslot = si;
  }
  if (hit && hit->hframe == g_frame_no) { hit->last = tex_clock; return hit->id; }
  /* a texture that has not changed for a second is checked only every 8th frame (videos and other textures the
   * game rewrites keep being checked every frame) */
  if (hit && hit->stable >= 60 && g_frame_no - hit->hframe < 8) { hit->last = tex_clock; return hit->id; }
  u64 hsh = hash_mem(src, total) ^ pal;
  if (hit && hit->hash == hsh) { hit->hframe = g_frame_no; hit->last = tex_clock; if (hit->stable < 1000) hit->stable++; return hit->id; }
  int freeslot = -1, lru = 0;
  for (int i = 0; i < NTEX; i++) {
    TexEnt *t = &texc[i];
    if (!t->id) { if (freeslot < 0) freeslot = i; continue; }
    if (t->key_addr == addr && t->w == w && t->h == h && t->fmt == fmt && t->hash == hsh && t->levels == levels) {
      t->last = tex_clock; t->hframe = g_frame_no; t->tlut = (u32)pal; t->stable = 0;
      if (hslot >= 0) tmap[hslot] = i + 1;
      return t->id;
    }
    if (t->last < texc[lru].last || !texc[lru].id) lru = i;
  }
  u32 pixels = 0; { u32 ww = w, hh = h; for (u32 l = 0; l < levels; l++) { pixels += ww * hh; ww = ww > 1 ? ww / 2 : 1; hh = hh > 1 ? hh / 2 : 1; } }
  TexEnt *t;
  if (hit) {
    /* same texture slot, new content (a video frame, an animated texture): replace it in place. Before, every
     * new content became one more texture on the GPU until 4096 of them piled up (gigabytes during videos). */
    t = hit; tex_pixels_total -= t->pixels;
  } else {
    if (freeslot < 0) { freeslot = lru; emit2(RC_TEXFREE, texc[lru].id, 0); tex_pixels_total -= texc[lru].pixels; texc[lru].id = 0; }
    t = &texc[freeslot]; t->id = tex_next_id++;
    if (hslot >= 0) tmap[hslot] = freeslot + 1; else tmap[(hk) & (TMAP - 1)] = freeslot + 1;
  }
  t->key_addr = addr; t->w = w; t->h = h; t->fmt = fmt; t->hash = hsh; t->levels = levels; t->last = tex_clock;
  t->hframe = g_frame_no; t->tlut = (u32)pal; t->stable = 0; t->pixels = pixels;
  tex_pixels_total += pixels;
  /* keep the textures within a video memory budget: free the least recently used ones */
  while (tex_pixels_total > TEX_BUDGET) {
    int v = -1;
    for (int i = 0; i < NTEX; i++) if (texc[i].id && &texc[i] != t && !texc[i].efb && (v < 0 || texc[i].last < texc[v].last)) v = i;
    if (v < 0) break;
    for (int i = 0; i < 8; i++) if (bound[i] == texc[v].id) { bound[i] = 0; tex_dirty |= 1 << i; }
    emit2(RC_TEXFREE, texc[v].id, 0); tex_pixels_total -= texc[v].pixels; texc[v].id = 0;
  }
  /* decode all levels */
  u32 words = 5 + pixels;
  u32 *r = ring_alloc(words); if (!r) return t->id;
  r[0] = RC_TEXUP << 24 | words; r[1] = t->id; r[2] = w; r[3] = h; r[4] = levels;
  u32 *dst = r + 5; const u8 *s = src; u32 ww = w, hh = h;
  for (u32 l = 0; l < levels; l++) {
    decode_level(s, (int)fmt, ww, hh, tlut, dst);
    s += gx_tex_size((int)fmt, ww, hh); dst += ww * hh;
    ww = ww > 1 ? ww / 2 : 1; hh = hh > 1 ? hh / 2 : 1;
  }
  ring_commit();
#ifndef __wasm__
  { static const char *dd = (const char *)1; static int cnt; if (dd == (const char *)1) dd = getenv("RGH_TEXDUMP");
    if (dd && cnt < 4000) { char fn[512]; snprintf(fn, sizeof fn, "%s/t%04d_%08X_f%u_%ux%u.png", dd, cnt++, addr, fmt, w, h); write_png(fn, r + 5, w, h); } }
#endif
  return t->id;
}
static void tex_bind_all(void) {
  /* texmaps referenced by the TEV stages and indirect stages */
  u32 gen = g_bp[0x00]; int nst = ((gen >> 10) & 15) + 1, nind = (gen >> 16) & 7;
  u32 used = 0;
  for (int s = 0; s < nst; s++) { u32 tr = g_bp[0x28 + s / 2] >> ((s & 1) * 12); if (tr & 0x40) used |= 1u << (tr & 7); }
  for (int s = 0; s < nind; s++) used |= 1u << ((g_bp[0x27] >> (6 * s)) & 7);
  for (int i = 0; i < 8; i++) {
    if (!(used & (1u << i))) continue;
    if (!(tex_dirty & (1 << i)) && bound[i]) continue;
    u32 id = tex_lookup(i);
    if (id != bound[i] || (tex_dirty & (1 << i))) { emit2(RC_TEX, (u32)i, id); bound[i] = id; }
  }
  tex_dirty = 0;
}

/* BP 0x52: EFB copy (to XFB for display, or to a texture in memory) */
void gx_efb_copy(u32 val) {
  u32 tl = g_bp[0x49], wh = g_bp[0x4A];
  u32 x = tl & 0x3FF, y = (tl >> 10) & 0x3FF, w = (wh & 0x3FF) + 1, h = ((wh >> 10) & 0x3FF) + 1;
  int to_xfb = (val >> 14) & 1;
  u32 id = 0, dw = w, dh = h;
  u32 fmt = ((val >> 4) & 7) | ((val >> 3 & 1) << 3) ;
  if (!to_xfb) {
    if (val & (1 << 9)) { dw = w / 2; dh = h / 2; }      /* half-size (box filter) */
    u32 addr = (g_bp[0x4B] & 0xFFFFFF) << 5;
    int k = -1;
    k = alias_find(addr);
    u32 old = 0; int rehash = 0;
    if (k < 0) {
      for (int i = 0; i < n_alias; i++) if (!efb_alias[i].id) { k = i; break; }        /* a forgotten slot */
      if (k < 0 && n_alias < NALIAS) k = n_alias++;
      if (k < 0) { k = 0; for (int i = 1; i < n_alias; i++) if (efb_alias[i].used < efb_alias[k].used) k = i;   /* least recently used */
        static int nl; if (nl++ < 20) fprintf(stderr, "[gx] EFB copy forgotten (table full): %08X unused since frame %u\n", efb_alias[k].addr, efb_alias[k].used); }
      { static int hw; if (n_alias > hw && (n_alias == 64 || n_alias % 128 == 0)) { hw = n_alias; fprintf(stderr, "[gx] %d EFB copies to texture remembered\n", n_alias); } }
      old = efb_alias[k].id; efb_alias[k].id = 0; rehash = 1;
    }
    if (!efb_alias[k].id || efb_alias[k].w != dw || efb_alias[k].h != dh) {
      if (efb_alias[k].id) old = efb_alias[k].id;
      efb_alias[k].id = 0x40000000 + (u32)k * 4 + (((old ? old : efb_alias[k].id) + 1) & 3);
    }
    /* the GPU copy that this one replaces is freed (before, replaced copies stayed in video memory forever,
     * which after a long game could fill the graphics card) */
    if (old && old != efb_alias[k].id) { emit2(RC_TEXFREE, old, 0); for (int i = 0; i < 8; i++) if (bound[i] == old) { bound[i] = 0; tex_dirty |= 1 << i; } }
    efb_alias[k].addr = addr; efb_alias[k].w = dw; efb_alias[k].h = dh; efb_alias[k].frame = g_frame_no;
    efb_alias[k].used = g_frame_no; efb_alias[k].sig = alias_sig(addr, dw * dh); efb_alias[k].sigframe = g_frame_no;
    { static const u8 tfmt[16] = {0, 1, 2, 3, 4, 5, 6, 1, 1, 1, 1, 3, 3, 6, 6, 6};
      efb_alias[k].fmt = fmt & 15; efb_alias[k].bytes = gx_tex_size(tfmt[fmt & 15], dw, dh); int was = efb_alias[k].wb; efb_alias[k].wb = 1;
      u32 lo = addr, hi = addr + efb_alias[k].bytes;
      if (!g_wb_pending || lo < g_wb_lo) g_wb_lo = lo; if (!g_wb_pending || hi > g_wb_hi) g_wb_hi = hi; if (!was) g_wb_pending++; }
    if (rehash) alias_rehash();
    id = efb_alias[k].id;
    for (int i = 0; i < 8; i++) if (bound[i] == id) tex_dirty |= 1 << i;
  }
#ifndef __wasm__
  { static int dbg = -1; if (dbg < 0) dbg = getenv("RGH_ZEROTEX") != NULL;
    if (dbg && !to_xfb) { u32 a = (g_bp[0x4B] & 0xFFFFFF) << 5; static u32 seen[256]; static int ns; int k = 0; for (; k < ns; k++) if (seen[k] == a) break;
      if (k == ns && ns < 256) { seen[ns++] = a; fprintf(stderr, "[efbcopy] %08X %ux%u -> %ux%u fmt %u frame %u\n", a, w, h, dw, dh, fmt, g_frame_no); } } }
#endif
  u32 *r = ring_alloc(11); if (!r) return;
  r[0] = RC_COPY << 24 | 11; r[1] = id; r[2] = x; r[3] = y; r[4] = w; r[5] = h; r[6] = dw; r[7] = dh; r[8] = fmt; r[9] = val;
  r[10] = (g_bp[0x4B] & 0xFFFFFF) << 5;      /* destination address (XFB copies: which framebuffer) */
  ring_commit();
}

void gx_backend_frame(u32 xfb) {
  g_frame_no++;
  if (!g_ring) return;
  if (!g_ring->consumer) return;
  emit2(RC_FRAME, g_frame_no, xfb);
  g_ring->frames++;
}
void gxrender_init(void) { ring_init(); }
