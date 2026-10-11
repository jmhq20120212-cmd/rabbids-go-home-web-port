/* ax.c - host-side replacement for the AX DSP microcode's voice mixing: walks the parameter
 * blocks (PBs) named by each command list, decodes ADPCM / PCM8 / PCM16 sample data from guest
 * memory, resamples, applies the voice volumes and writes the voice state back like the DSP does.
 * The mixed 32 kHz stereo stream goes to the host audio output (web: AudioWorklet ring). */
#include "hw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define FRAME 96                      /* 3 ms at 32 kHz */
static int g_log = -1;
s16 *g_audio_ring; u32 g_audio_ring_len = 32000; volatile u32 g_audio_wpos, g_audio_rpos; int g_audio_on;

static u16 r16(u32 a) { return (u16)R16(a | 0x80000000); }
static void w16(u32 a, u16 v) { W16(a | 0x80000000, v); }
static u32 rd32(u32 a) { return (u32)r16(a) << 16 | r16(a + 2); }
static void wr32(u32 a, u32 v) { w16(a, (u16)(v >> 16)); w16(a + 2, (u16)v); }
static u8 rmem8(u32 byteaddr) { return (u8)R8(byteaddr | 0x80000000); }

/* PB offsets (AXPBWii) */
enum { PB_NEXT = 0x00, PB_SRCTYPE = 0x08, PB_MIXCTL = 0x0C, PB_RUNNING = 0x10, PB_STREAM = 0x12, PB_MIX = 0x14,
       PB_VOLENV = 0x6A, PB_ADDR = 0x6E, PB_ADPCM = 0x7E, PB_SRC = 0xA6, PB_LOOPINFO = 0xB4 };

/* mixing buffers of one 3 ms frame: main, aux A, aux B, aux C buses, each with left / right / surround */
enum { B_MAIN = 0, B_AUXA = 1, B_AUXB = 2, B_AUXC = 3 };
static int bus[4][3][FRAME];
#define mix_l bus[B_MAIN][0]
#define mix_r bus[B_MAIN][1]
#define mix_s bus[B_MAIN][2]
static u16 last_aux_vol[3] = {0x8000, 0x8000, 0x8000}, last_out_vol = 0x8000;
extern int g_spk_audible;
extern u64 g_vi_frames;
static u64 g_vi_frames_ax(void) { return g_vi_frames; }
static int g_rmt_gain = -1;                 /* remote speaker sounds: 256 = same level as the main mix */
static struct { u32 voices, remote, frames, aux, cmds; } g_ax_stat;
static int clamp16(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

/* mixer control word -> per output (main/auxA/auxB/auxC x L/R/S): on, ramp (as the AX microcode does) */
static void mixer_flags(u32 mc, int on[12], int ramp[12]) {
  memset(on, 0, 12 * sizeof(int)); memset(ramp, 0, 12 * sizeof(int));
  /* outputs: 0 main L, 1 main R, 2 main S, 3 A L, 4 A R, 5 A S, 6 B L, 7 B R, 8 B S, 9 C L, 10 C R, 11 C S */
  static const u32 lbit[4] = {0x1, 0x10000, 0x200000, 0x4000000};
  for (int b = 0; b < 4; b++) {
    u32 l = lbit[b], r = l << 1, lr = l << 2, s = l << 3, sr = l << 4;
    if (mc & l) on[b * 3] = 1;
    if (mc & r) on[b * 3 + 1] = 1;
    if (mc & lr) { on[b * 3] = on[b * 3 + 1] = 1; ramp[b * 3] = ramp[b * 3 + 1] = 1; }
    if (mc & s) on[b * 3 + 2] = 1;
    if (mc & sr) { on[b * 3 + 2] = 1; ramp[b * 3 + 2] = 1; }
  }
}

int g_ax_dump;
static void voice(u32 pb) {
  if (r16(pb + PB_RUNNING) != 1) return;
  if (g_ax_dump) { static int k; if (k++ < 2) { fprintf(stderr, "[ax] PB dump %08X:", pb); for (u32 o = 0; o < 0x140; o += 2) fprintf(stderr, "%s%04X", (o % 32) ? " " : "\n  ", r16(pb + o)); fprintf(stderr, "\n"); } }
  u16 looping = r16(pb + PB_ADDR + 0), fmt = r16(pb + PB_ADDR + 2);
  u32 loop = rd32(pb + PB_ADDR + 4), end = rd32(pb + PB_ADDR + 8), cur = rd32(pb + PB_ADDR + 12);
  s16 coef[16]; for (int i = 0; i < 16; i++) coef[i] = (s16)r16(pb + PB_ADPCM + 2 * i);
  u16 ps = r16(pb + PB_ADPCM + 34); s16 yn1 = (s16)r16(pb + PB_ADPCM + 36), yn2 = (s16)r16(pb + PB_ADPCM + 38);
  u32 ratio = rd32(pb + PB_SRC); u32 frac = r16(pb + PB_SRC + 4);
  s16 last[4]; for (int i = 0; i < 4; i++) last[i] = (s16)r16(pb + PB_SRC + 6 + 2 * i);
  u16 vol = r16(pb + PB_VOLENV); s16 vold = (s16)r16(pb + PB_VOLENV + 2);
  int running = 1, stream = r16(pb + PB_STREAM) == 1;
  if (r16(pb + PB_SRCTYPE) == 2) ratio = 0x10000;     /* no sample rate conversion */

  /* the voice's 12 mixer outputs (volume, per-sample delta), gated by the mixer control word */
  int on[12], ramp[12]; mixer_flags(rd32(pb + PB_MIXCTL), on, ramp);
  u16 mv[12]; s16 md[12];
  for (int k = 0; k < 12; k++) {
    /* PB mixer layout: L, R pairs of main/A/B/C come first (8 x {vol, delta}), then the 4 surround entries */
    u32 o = k % 3 == 2 ? 0x20 + (k / 3) * 4 : (k / 3) * 8 + (k % 3) * 4;
    mv[k] = r16(pb + PB_MIX + o); md[k] = ramp[k] ? (s16)r16(pb + PB_MIX + o + 2) : 0;
  }
  /* the Wii Remote speaker mix (remote 0): those sounds are played through the PC speakers */
  int rmt = 0;
  if (r16(pb + 0xD6) && g_spk_audible && (r16(pb + 0xD8) & 3)) rmt = (g_rmt_gain * r16(pb + 0xDA)) >> 8;
  g_ax_stat.voices++; if (rmt) g_ax_stat.remote++;

  #define GET_SAMPLE(out) do { \
    if (cur > end || (fmt == 0 && cur == end + 1)) { \
      if (looping) { cur = loop; if (fmt == 0) { ps = r16(pb + PB_LOOPINFO); if (!stream) { yn1 = (s16)r16(pb + PB_LOOPINFO + 2); yn2 = (s16)r16(pb + PB_LOOPINFO + 4); } } } \
      else { running = 0; out = 0; break; } } \
    if (fmt == 0) { \
      if ((cur & 15) == 0) { ps = rmem8(cur >> 1); cur += 2; } \
      int sc = 1 << (ps & 15), ci = (ps >> 4) & 7; int nib = (cur & 1) ? rmem8(cur >> 1) & 15 : rmem8(cur >> 1) >> 4; \
      if (nib >= 8) nib -= 16; \
      int v = sc * nib + ((0x400 + coef[ci * 2] * yn1 + coef[ci * 2 + 1] * yn2) >> 11); \
      v = clamp16(v); yn2 = yn1; yn1 = (s16)v; out = v; cur++; \
    } else if (fmt == 0x0A) { out = (s16)r16(cur * 2); cur++; } \
    else { out = (s8)rmem8(cur) << 8; cur++; } \
  } while (0)

  for (int i = 0; i < FRAME && running; i++) {
    /* linear interpolation between last[2] and last[3] */
    frac += ratio & 0xFFFF; u32 adv = (ratio >> 16) + (frac >> 16); frac &= 0xFFFF;
    for (u32 k = 0; k < adv && running; k++) {
      int s = 0; GET_SAMPLE(s);
      last[0] = last[1]; last[1] = last[2]; last[2] = last[3]; last[3] = (s16)s;
    }
    int smp = last[2] + (((last[3] - last[2]) * (int)frac) >> 16);
    int v = clamp16((smp * (int)vol) >> 15);                  /* volume envelope */
    vol += vold;
    for (int k = 0; k < 12; k++) {
      if (!on[k]) continue;
      bus[k / 3][k % 3][i] += clamp16((v * (int)mv[k]) >> 15);
      mv[k] += md[k];
    }
    if (rmt) { int e = clamp16((v * rmt) >> 15); mix_l[i] += e; mix_r[i] += e; }
  }
  wr32(pb + PB_ADDR + 12, cur);
  w16(pb + PB_ADPCM + 34, ps); w16(pb + PB_ADPCM + 36, (u16)yn1); w16(pb + PB_ADPCM + 38, (u16)yn2);
  w16(pb + PB_SRC + 4, (u16)frac); for (int i = 0; i < 4; i++) w16(pb + PB_SRC + 6 + 2 * i, (u16)last[i]);
  w16(pb + PB_VOLENV, vol);
  for (int k = 0; k < 12; k++) if (md[k]) { u32 o = k % 3 == 2 ? 0x20 + (k / 3) * 4 : (k / 3) * 8 + (k % 3) * 4; w16(pb + PB_MIX + o, mv[k]); }
  if (!running) w16(pb + PB_RUNNING, 0);
}

/* guest buffers of 96 big-endian 32-bit samples */
static void put96(u32 addr, const int *src) { for (int k = 0; k < FRAME; k++) wr32(addr + 4 * k, (u32)src[k]); }
static void add96(u32 addr, int *dst, u16 v0, u16 v1, int sign) {
  for (int k = 0; k < FRAME; k++) {
    int vol = v0 + (((int)v1 - (int)v0) * k) / FRAME;
    dst[k] += sign * (int)(((s64)(s32)rd32(addr + 4 * k) * vol) >> 15);
  }
}

static void output(u16 vol) {
  /* the mix goes to the PC: through the AudioWorklet ring (web) and the optional WAV dump (native) */
  for (int k = 0; k < FRAME; k++) {
    int v = last_out_vol + (((int)vol - (int)last_out_vol) * k) / FRAME;
    mix_l[k] = clamp16((int)(((s64)mix_l[k] * v) >> 15)); mix_r[k] = clamp16((int)(((s64)mix_r[k] * v) >> 15));
  }
  last_out_vol = vol;
#ifndef __wasm__
  { static FILE *wf; static int init; static u32 total;
    if (!init) { init = 1; const char *p = getenv("RGH_WAV"); if (p) { wf = fopen(p, "wb"); u8 hdr[44] = {0}; fwrite(hdr, 1, 44, wf); } }
    if (wf) { for (int k = 0; k < FRAME; k++) { s16 b[2] = {(s16)mix_l[k], (s16)mix_r[k]}; fwrite(b, 2, 2, wf); } total += FRAME * 4;
      long pos = ftell(wf); fseek(wf, 0, SEEK_SET); u32 h[11] = {0x46464952, 36 + total, 0x45564157, 0x20746D66, 16, 0x00020001, 32000, 128000, 0x00100004, 0x61746164, total};
      fwrite(h, 4, 11, wf); fseek(wf, pos, SEEK_SET); } }
  { static int lv = -1; static double acc; static int nfr; if (lv < 0) lv = getenv("RGH_AXLEVEL") != NULL;
    if (lv) { for (int k = 0; k < FRAME; k++) acc += (double)mix_l[k] * mix_l[k];
      if (++nfr == 333) { fprintf(stderr, "[axlvl] vi %llu rms %.0f\n", (unsigned long long)g_vi_frames, sqrt(acc / (333.0 * FRAME))); acc = 0; nfr = 0; } } }
#endif
  if (!g_audio_on || !g_audio_ring) return;
  for (int k = 0; k < FRAME; k++) {
    u32 w = g_audio_wpos;
    if (w - g_audio_rpos >= g_audio_ring_len) break;          /* consumer behind: drop */
    g_audio_ring[(w % g_audio_ring_len) * 2] = (s16)mix_l[k]; g_audio_ring[(w % g_audio_ring_len) * 2 + 1] = (s16)mix_r[k];
    __atomic_store_n(&g_audio_wpos, w + 1, __ATOMIC_SEQ_CST);
  }
}

/* command list (AX microcode for Wii): u16 command words followed by their arguments */
static u32 g_last_list, g_last_len;
void ax_process(u32 addr, u32 len) {
  g_last_list = addr & 0x1FFFFFFF; g_last_len = len;
  if (g_log < 0) g_log = getenv("RGH_AX") ? 1 : 0;
  if (g_rmt_gain < 0) { const char *e = getenv("RGH_RMT"); g_rmt_gain = e ? atoi(e) : 256; }
  memset(bus, 0, sizeof bus);
  u32 a = addr & 0x1FFFFFFF;
  if (g_log > 0) {   /* print the raw list whenever its command sequence changes */
    static u32 lastsig; u32 sig = 0; for (u32 k = 0; k < len / 2 && k < 64; k++) { u16 v = r16(a + 2 * k); if (v < 16) sig = sig * 31 + v; }
    static int n; if (sig != lastsig && n++ < 40) { lastsig = sig; fprintf(stderr, "[ax] raw (vi %llu):", (unsigned long long)g_vi_frames_ax()); for (u32 k = 0; k < len / 2 && k < 64; k++) fprintf(stderr, " %04X", r16(a + 2 * k)); fprintf(stderr, "\n"); } }
  #define ARG(n) r16(a + 2 * (i + (n)))
  #define ARG32(n) ((u32)ARG(n) << 16 | ARG((n) + 1))
  int out_done = 0;
  for (u32 i = 0; i < len / 2 && i < 512; ) {
    u16 cmd = r16(a + 2 * i); i++;
    switch (cmd) {
      case 0x00: i += 2; break;                                             /* setup */
      case 0x01: case 0x02: { u32 p = ARG32(0) & 0x1FFFFFFF;               /* add / subtract a buffer to L and R */
        int sg = cmd == 1 ? 1 : -1; if (p) { add96(p, mix_l, 0x8000, 0x8000, sg); add96(p, mix_r, 0x8000, 0x8000, sg); } i += 2; break; }
      case 0x03: { u32 p = ARG32(0) & 0x1FFFFFFF;
        if (p) { add96(p, mix_l, 0x8000, 0x8000, 1); add96(p + 4 * FRAME, mix_r, 0x8000, 0x8000, -1); } i += 2; break; }
      case 0x04: {                                                          /* process the voices */
        u32 pb = ARG32(0);
        for (int guard = 0; pb && guard < 256; guard++) { voice(pb & 0x1FFFFFFF); pb = rd32((pb & 0x1FFFFFFF) + PB_NEXT); }
        i += 2; break; }
      case 0x05: case 0x06: case 0x07: {                                    /* aux bus: to the game's effect, effect back into main */
        int b = cmd - 0x05 + 1; u16 v = ARG(0); u32 up = ARG32(1) & 0x1FFFFFFF, dn = ARG32(3) & 0x1FFFFFFF;
        if (up) for (int c = 0; c < 3; c++) put96(up + c * 4 * FRAME, bus[b][c]);
        if (dn) for (int c = 0; c < 3; c++) add96(dn + c * 4 * FRAME, bus[B_MAIN][c], last_aux_vol[b - 1], v, 1);
        last_aux_vol[b - 1] = v; g_ax_stat.aux++; i += 5; break; }
      case 0x08: case 0x09: {                                               /* aux A/B upload + mix into L/R/S and aux C */
        int b = cmd == 0x08 ? B_AUXA : B_AUXB; u16 v = ARG(0); u32 ad[6]; for (int k = 0; k < 6; k++) ad[k] = ARG32(1 + 2 * k) & 0x1FFFFFFF;
        if (ad[0]) for (int c = 0; c < 3; c++) put96(ad[0] + c * 4 * FRAME, bus[b][c]);
        if (ad[1]) put96(ad[1], bus[B_AUXC][0]);
        int *dst[4] = {mix_l, mix_r, mix_s, bus[B_AUXC][0]};
        for (int k = 0; k < 4; k++) if (ad[2 + k]) add96(ad[2 + k], dst[k], last_aux_vol[b - 1], v, 1);
        last_aux_vol[b - 1] = v; i += 13; break; }
      case 0x0A: i += 4; break;                                             /* compressor */
      case 0x0B: case 0x0C: {                                               /* output: surround to memory, L/R to the speakers */
        u16 v = ARG(0); u32 sp = ARG32(1) & 0x1FFFFFFF;
        if (sp) put96(sp, mix_s);
        output(v); out_done = 1; i += 5; break; }
      case 0x0D: i += 8; break;                                             /* Wii Remote speaker buffers */
      case 0x0E: i = len; break;                                            /* end */
      default: i = len; break;
    }
    g_ax_stat.cmds++;
  }
  #undef ARG
  #undef ARG32
  if (!out_done) output(0x8000);
  if (g_log > 0) { static int n; n++; g_ax_dump = (n % 3000) == 0; }
  /* once a minute: what the game plays (to diagnose missing sounds from a log) */
  if (++g_ax_stat.frames == 20000) {
    { extern u32 g_di_reads, g_di_bytes; fprintf(stderr, "[ax] last minute: %u voice-frames, Wii Remote speaker %u, effects (aux) %u, disc reads %u (%u KB)\n", g_ax_stat.voices, g_ax_stat.remote, g_ax_stat.aux, g_di_reads, g_di_bytes >> 10); g_di_reads = g_di_bytes = 0; }
    memset(&g_ax_stat, 0, sizeof g_ax_stat);
  }
}

#ifdef __wasm__
__attribute__((import_module("rgh"), import_name("audio_ready"))) void js_audio_ready(void *ring, u32 len, volatile u32 *wpos, volatile u32 *rpos, int *on);
#endif
void ax_init(void) {
  g_audio_ring = calloc(g_audio_ring_len * 2, sizeof(s16));
#ifdef __wasm__
  js_audio_ready(g_audio_ring, g_audio_ring_len, &g_audio_wpos, &g_audio_rpos, &g_audio_on);
#endif
}

/* F9 / automatic diagnosis: the last command list and every running voice with its routing */
void ax_dump_state(void) {
  u32 a = g_last_list, len = g_last_len;
  fprintf(stderr, "[audio] list:"); for (u32 k = 0; k < len / 2 && k < 48; k++) fprintf(stderr, " %04X", r16(a + 2 * k)); fprintf(stderr, "\n");
  fprintf(stderr, "[audio] output volume %04X aux %04X %04X %04X speaker %d\n", last_out_vol, last_aux_vol[0], last_aux_vol[1], last_aux_vol[2], g_spk_audible);
  u32 pb = 0; for (u32 i = 0; i + 2 < len / 2; i++) if (r16(a + 2 * i) == 0x04) { pb = (u32)r16(a + 2 * i + 2) << 16 | r16(a + 2 * i + 4); break; }
  int n = 0, run = 0;
  for (int g = 0; pb && g < 256; g++, pb = rd32((pb & 0x1FFFFFFF) + PB_NEXT)) {
    u32 p = pb & 0x1FFFFFFF; n++;
    if (r16(p + PB_RUNNING) != 1) continue;
    run++;
    if (run <= 40) fprintf(stderr, "[audio] voice %08X fmt %04X stream %d loop %d mixctl %08X vol %04X L %04X R %04X A %04X/%04X B %04X/%04X C %04X S %04X rmt %d:%04X/%04X cur %08X end %08X ratio %08X\n",
      pb, r16(p + PB_ADDR + 2), r16(p + PB_STREAM), r16(p + PB_ADDR), rd32(p + PB_MIXCTL), r16(p + PB_VOLENV), r16(p + PB_MIX), r16(p + PB_MIX + 4),
      r16(p + PB_MIX + 8), r16(p + PB_MIX + 12), r16(p + PB_MIX + 16), r16(p + PB_MIX + 20), r16(p + PB_MIX + 24), r16(p + PB_MIX + 32),
      r16(p + 0xD6), r16(p + 0xD8), r16(p + 0xDA), rd32(p + PB_ADDR + 12), rd32(p + PB_ADDR + 8), rd32(p + PB_SRC));
    if (run <= 40) {   /* the sample data the voice is about to play: all zero = the game never filled it */
      u32 cur = rd32(p + PB_ADDR + 12), fmt = r16(p + PB_ADDR + 2), b = fmt == 0 ? cur >> 1 : fmt == 0x0A ? cur * 2 : cur; int nz = 0;
      for (u32 k = 0; k < 512; k++) if (rmem8(b + k)) nz++;
      { u32 base = (u32)R32(0x806C3268u) & 0x1FFFFFFF, idx = (p - base) / 0x140, v = 0x805C50C0u + idx * 0x168;
        if (p >= base && idx < 96) fprintf(stderr, "[audio]   cpu: voice %u state %u sync %08X cur %08X loop %08X end %08X\n", idx, r16(v + 0x28 + PB_RUNNING), (u32)R32(v + 0x1C),
          (u32)R32(v + 0xA2), (u32)R32(v + 0x9A), (u32)R32(v + 0x9E)); }
      fprintf(stderr, "[audio]   data %08X: %02X %02X %02X %02X %02X %02X %02X %02X nonzero %d/512 loop %08X pred %04X yn %d/%d\n", b | 0x80000000,
        rmem8(b), rmem8(b + 1), rmem8(b + 2), rmem8(b + 3), rmem8(b + 4), rmem8(b + 5), rmem8(b + 6), rmem8(b + 7), nz, rd32(p + PB_ADDR + 4),
        r16(p + PB_ADPCM + 34), (s16)r16(p + PB_ADPCM + 36), (s16)r16(p + PB_ADPCM + 38));
    }
  }
  fprintf(stderr, "[audio] %d voices, %d playing\n", n, run);
  {  /* while a full-screen Bink video runs the engine feeds silence to all disc streams (K3D_b_FullScreenBinkIsRunning) */
    u32 k3d = (u32)R32(0x806C8DA0u - 0x6128), bk = k3d >= 0x80000000u && k3d < 0x94000000u ? (u32)R32(k3d + 0x20000 - 0x61B0) : 0;
    fprintf(stderr, "[audio] full-screen Bink video: %08X", bk);
    if (bk >= 0x80000000u && bk < 0x94000000u) fprintf(stderr, " flags %02X (running=%d)", R8(bk + 0x1D), (R8(bk + 0x1D) & 3) == 1);
    fprintf(stderr, "\n"); }
  {  /* the engine's sound streams: RAM streams (decoded by the CPU into the voice buffers) and disc streams */
    u32 nram = (u32)R32(0x806C2B4Cu), cur = (u32)R32(0x806C2B60u), size = (u32)R32(0x806C2B68u), max = (u32)R32(0x806C2B64u);
    fprintf(stderr, "[audio] RAM streams registered %u (max 128); disc requests list %u size %u max %u thread %08X\n", nram, cur, size, max, (u32)R32(0x806C2B70u));
    for (int l = 0; l < 2; l++) {   /* disc streams waiting in the request lists */
      u32 lst = (u32)R32(0x806C2B58u + 4 * l); if (lst < 0x80000000u || lst >= 0x94000000u) continue;
      for (u32 k = 0; k < 6; k++) { u32 o = (u32)R32(lst + 4 * k); if (o < 0x80000000u || o >= 0x94000000u) continue;
        u32 st = (u32)R32(o + 0x234), vh = (u32)R32(o + 0x230), vo = vh >= 0x80000000u && vh < 0x94000000u ? (u32)R32(vh) : 0;
        fprintf(stderr, "[audio]   disc stream %08X vt %08X list %d: 238=%08X 240=%08X 244=%08X 248=%08X channels %u state %08X:", o, (u32)R32(o), l,
          (u32)R32(o + 0x238), (u32)R32(o + 0x240), (u32)R32(o + 0x244), (u32)R32(o + 0x248), r16(o + 0x1E0), st);
        if (st >= 0x80000000u && st < 0x94000000u) for (u32 q = 0; q < 0x30; q += 4) fprintf(stderr, " %08X", (u32)R32(st + q));
        fprintf(stderr, " | voice %08X", vo);
        if (vo >= 0x80000000u && vo < 0x94000000u) fprintf(stderr, " +38=%04X cur %08X loop %08X", r16(vo + 0x38), (u32)R32(vo + 0xA2), (u32)R32(vo + 0x9A));
        fprintf(stderr, "\n"); } }
    for (u32 i = 0; i < nram && i < 128; i++) {
      u32 in = (u32)R32(0x805A5288u + 4 * i); if (!in) continue;
      u32 fl = (u32)R32(in + 0x234);
      if (i < 24) fprintf(stderr, "[audio]   stream %u inst %08X activo %u buf %08X\n", i, in, fl >= 0x80000000u && fl < 0x94000000u ? (u32)R32(fl) : 0xFFFFFFFFu, (u32)R32(in + 0x210));
    }
  }
}
