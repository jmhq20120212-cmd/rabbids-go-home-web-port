/* gx.c - GX command processor front end: receives the byte stream the game writes to the
 * write-gather pipe, splits it into commands, keeps CP/XF/BP register state, follows display
 * lists, and raises the PE "draw done" / token interrupts the SDK waits for.
 * Rendering is done by a backend that consumes the decoded state (gx_backend_*). */
#include "hw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u64 g_gx_bytes;
static u8 fifo[4 << 20]; static u32 fifo_n;
u32 g_cp[0x100];          /* CP registers (VCD/VAT/array bases/strides/matrix indices) */
u32 g_xf[0x1100];         /* XF memory 0x0000-0x0FFF matrices, 0x1000-0x10FF registers */
u32 g_bp[0x100];          /* BP registers */
static u16 cp_regs[0x40], pe_regs[0x10];
static u32 pe_token;
u64 g_gx_draws, g_gx_verts;

/* backend hooks (weak defaults) */
void gx_backend_draw(int prim, int vat, int n, const u8 *data, u32 vsize);
void gx_backend_bp(u32 reg, u32 val);
void gx_backend_frame(u32 xfb);
void gx_backend_xf(u32 addr, u32 cnt, const u32 *vals);
void gx_backend_cp(u32 reg, u32 val);
void gxrender_init(void);

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void pe_update(void) {
  u16 c = pe_regs[0x0A / 2];
  pi_set(PI_PE_TOKEN, (c & 4) && (c & 1));
  pi_set(PI_PE_FINISH, (c & 8) && (c & 2));
}

/* vertex size from VCD + VAT */
static const int fmt_size[8] = {1, 1, 2, 2, 4, 0, 0, 0};
u32 gx_vertex_size(int vat) {
  u32 lo = g_cp[0x50], hi = g_cp[0x60];
  u32 a = g_cp[0x70 + vat], b = g_cp[0x80 + vat], cc = g_cp[0x90 + vat];
  u32 sz = 0;
  if (lo & 1) sz++;
  for (int i = 0; i < 8; i++) if (lo & (2 << i)) sz++;
  int idx_size[4] = {0, 0, 1, 2};
  /* position */
  int pt = (lo >> 9) & 3;
  if (pt == 1) sz += fmt_size[(a >> 1) & 7] * ((a & 1) ? 3 : 2); else sz += idx_size[pt];
  /* normal */
  int nt = (lo >> 11) & 3;
  if (nt) {
    int nbt = (a >> 9) & 1, n3 = (a >> 31) & 1;
    if (nt == 1) sz += fmt_size[(a >> 10) & 7] * (nbt ? 9 : 3);
    else sz += idx_size[nt] * ((nbt && n3) ? 3 : 1);
  }
  /* colors */
  for (int i = 0; i < 2; i++) {
    int ct = (lo >> (13 + 2 * i)) & 3;
    if (ct == 1) { int comp = (a >> (14 + 4 * i)) & 7; static const int cs[8] = {2, 3, 4, 2, 3, 4, 4, 4}; sz += cs[comp]; }
    else sz += idx_size[ct];
  }
  /* texcoords */
  for (int i = 0; i < 8; i++) {
    int tt = (hi >> (2 * i)) & 3;
    if (!tt) continue;
    if (tt != 1) { sz += idx_size[tt]; continue; }
    int el, fmt;
    switch (i) {
      case 0: el = (a >> 21) & 1; fmt = (a >> 22) & 7; break;
      case 1: el = b & 1; fmt = (b >> 1) & 7; break;
      case 2: el = (b >> 9) & 1; fmt = (b >> 10) & 7; break;
      case 3: el = (b >> 18) & 1; fmt = (b >> 19) & 7; break;
      case 4: el = (b >> 27) & 1; fmt = (b >> 28) & 7; break;
      case 5: el = (cc >> 5) & 1; fmt = (cc >> 6) & 7; break;
      case 6: el = (cc >> 14) & 1; fmt = (cc >> 15) & 7; break;
      default: el = (cc >> 23) & 1; fmt = (cc >> 24) & 7; break;
    }
    sz += fmt_size[fmt] * (el ? 2 : 1);
  }
  return sz;
}

static void bp_write(u32 v) {
  u32 reg = v >> 24, val = v & 0xFFFFFF;
  if (reg == 0xFE) { g_bp[0xFE] = val; return; }          /* BP mask for next write */
  u32 mask = g_bp[0xFE] ? g_bp[0xFE] : 0xFFFFFF; g_bp[0xFE] = 0;
  g_bp[reg] = (g_bp[reg] & ~mask) | (val & mask);
  { static int lg = -1; if (lg < 0) lg = getenv("RGH_GX") != NULL; if (lg && (reg == 0x45 || reg == 0x47 || reg == 0x48)) fprintf(stderr, "[gx] BP %02X = %06X\n", reg, val); }
  switch (reg) {
    case 0x45:   /* PE_DONE: draw done */
      if (val & 2) { extern void gx_writeback_all(void); gx_writeback_all(); pe_regs[0x0A / 2] |= 8; pe_update(); }
      break;
    case 0x47: pe_token = val & 0xFFFF; pe_regs[0x0E / 2] = (u16)pe_token; break;
    case 0x48: pe_token = val & 0xFFFF; pe_regs[0x0E / 2] = (u16)pe_token; pe_regs[0x0A / 2] |= 4; pe_update(); break;
  }
  gx_backend_bp(reg, g_bp[reg]);
}

static u32 g_want;   /* bytes the pending (incomplete) command needs, from the buffer start */
static u32 run(const u8 *p, u32 n, int depth);
static void call_dl(u32 addr, u32 size, int depth) {
  if (depth > 4) return;
  u8 *m = hp(addr | 0x80000000); if (!m) return;
  u32 w = g_want; run(m, size, depth + 1); g_want = w;
}

/* parse complete commands from p[0..n); return bytes consumed */
static u32 run(const u8 *p, u32 n, int depth) {
  u32 i = 0;
  while (i < n) {
    u8 op = p[i];
    u32 need;
    if (op == 0x00 || op == 0x48 || op == 0x44) { i++; continue; }
    if (op == 0x08) { need = 6; if (i + need > n) { g_want = i + need; break; } g_cp[p[i + 1]] = be32(p + i + 2); gx_backend_cp(p[i + 1], g_cp[p[i + 1]]); i += need; continue; }
    if (op == 0x10) {
      if (i + 5 > n) { g_want = i + 5; break; }
      u32 h = be32(p + i + 1); u32 cnt = ((h >> 16) & 0xF) + 1, addr = h & 0xFFFF;
      need = 5 + cnt * 4; if (i + need > n) { g_want = i + need; break; }
      { u32 tmp[16]; for (u32 k = 0; k < cnt; k++) { tmp[k] = be32(p + i + 5 + 4 * k); if (addr + k < 0x1100) g_xf[addr + k] = tmp[k]; }
        gx_backend_xf(addr, cnt, tmp); }
      i += need; continue;
    }
    if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
      need = 5; if (i + need > n) { g_want = i + need; break; }
      u32 v = be32(p + i + 1);
      u32 idx = v >> 16, len = ((v >> 12) & 0xF) + 1, addr = v & 0xFFF;
      int arr = 12 + ((op - 0x20) >> 3);
      u32 base = g_cp[0xA0 + arr], stride = g_cp[0xB0 + arr] & 0xFF;
      { u32 tmp[16]; for (u32 k = 0; k < len; k++) { tmp[k] = R32((base + idx * stride + 4 * k) | 0x80000000); if (addr + k < 0x1100) g_xf[addr + k] = tmp[k]; }
        gx_backend_xf(addr, len, tmp); }
      i += need; continue;
    }
    if (op == 0x40) {
      need = 9; if (i + need > n) { g_want = i + need; break; }
      call_dl(be32(p + i + 1), be32(p + i + 5), depth);
      i += need; continue;
    }
    if (op == 0x61) { need = 5; if (i + need > n) { g_want = i + need; break; } bp_write(be32(p + i + 1)); i += need; continue; }
    if (op & 0x80) {
      if (i + 3 > n) { g_want = i + 3; break; }
      u32 cnt = (u32)p[i + 1] << 8 | p[i + 2];
      u32 vs = gx_vertex_size(op & 7);
      need = 3 + cnt * vs; if (i + need > n) { g_want = i + need; break; }
      g_gx_draws++; g_gx_verts += cnt;
      gx_backend_draw((op >> 3) & 0xF, op & 7, (int)cnt, p + i + 3, vs);
      i += need; continue;
    }
    static int warned; if (warned++ < 10) fprintf(stderr, "[gx] unknown FIFO opcode %02X\n", op);
    i++;
  }
  return i;
}

#ifdef __wasm__
volatile int g_prof_tag;
#else
extern volatile int g_prof_tag;
#endif
void gx_fifo_write(u32 v, int bytes) {
  g_prof_tag = 1;
  g_gx_bytes += bytes;
  for (int k = bytes - 1; k >= 0; k--) fifo[fifo_n++] = (u8)(v >> (8 * k));
  if (fifo_n < g_want) { g_prof_tag = 0; return; }
  g_want = 0;
  u32 used = run(fifo, fifo_n, 0);
  if (g_want) g_want -= used;
  { static int lg = -1; if (lg < 0) lg = getenv("RGH_GX") != NULL;
    if (lg && fifo_n - used > 4096 && ((fifo_n - used) & 0xFFF) < (u32)bytes) {
      const u8 *p = fifo + used; fprintf(stderr, "[gx] %u bytes pending, head %02X %02X %02X %02X %02X, vsize(vat %d)=%u cp50=%08X cp60=%08X vat=%08X %08X %08X\n", fifo_n - used, p[0], p[1], p[2], p[3], p[4],
        p[0] & 7, gx_vertex_size(p[0] & 7), g_cp[0x50], g_cp[0x60], g_cp[0x70 + (p[0] & 7)], g_cp[0x80 + (p[0] & 7)], g_cp[0x90 + (p[0] & 7)]); } }
  if (used) { memmove(fifo, fifo + used, fifo_n - used); fifo_n -= used; }
  if (fifo_n > sizeof fifo - 64) { fprintf(stderr, "[gx] FIFO parser stalled\n"); fifo_n = 0; }
  g_prof_tag = 0;
}

/* CP: the "GPU" consumes commands as soon as they are written, so the read pointer is the write
 * pointer; a FIFO breakpoint fires when the stream reaches it (GXEnableBreakPt). */
static u32 cp_bp(void) { return ((u32)cp_regs[0x3E / 2] << 16 | cp_regs[0x3C / 2]) & 0x03FFFFE0; }
static void cp_update_irq(void) {
  u16 ctl = cp_regs[1], st = cp_regs[0];
  pi_set(PI_CP, (st & 0x10) && (ctl & 0x20));
}
static void cp_hit(void) { cp_regs[0] |= 0x10; cp_update_irq(); }
void gx_fifo_advanced(u32 from, u32 to) {
  if (!(cp_regs[1] & 0x02) || (cp_regs[0] & 0x10)) return;
  u32 bp = cp_bp();
  if (from <= to ? (bp > from && bp <= to) : (bp > from || bp <= to)) cp_hit();
}
u16 gx_cp_read(u32 off) {
  off &= 0x7E;
  switch (off) {
    case 0x00: return (cp_regs[0] & 0x10) | 0x0C;   /* GP idle for reads and commands */
    case 0x34: case 0x38: return (u16)hw_fifo_wptr();
    case 0x36: case 0x3A: return (u16)(hw_fifo_wptr() >> 16);
    case 0x30: case 0x32: return 0;                /* read/write distance */
  }
  return cp_regs[off / 2];
}
static int cp_ctl_set;
int gx_gp_linked(void) { return !cp_ctl_set || (cp_regs[1] & 0x10); }   /* CP_CTRL bit 4: GP linked to the CPU FIFO */
void gx_cp_write(u32 off, u16 v) {
  off &= 0x7E;
  u16 old = cp_regs[off / 2];
  cp_regs[off / 2] = v;
  if (off == 0x02) { static int n; (void)old; if (cp_ctl_set && ((old ^ v) & 0x10) && n++ < 20 && getenv("RGH_GX")) fprintf(stderr, "[gx] GP link %d\n", (v >> 4) & 1); cp_ctl_set = 1; }
  if (off == 0x02) {
    if (!(v & 0x02)) cp_regs[0] &= ~0x10;            /* breakpoint disabled: clear its status */
    else if (cp_bp() == (hw_fifo_wptr() & 0x03FFFFE0)) cp_hit();
    cp_update_irq();
  }
  if (off == 0x04 && (v & 0x10)) { cp_regs[0] &= ~0x10; cp_update_irq(); }
}
u16 gx_pe_read(u32 off) { return pe_regs[(off & 0x1E) / 2]; }
void gx_pe_write(u32 off, u16 v) {
  off &= 0x1E;
  { static int lg = -1; if (lg < 0) lg = getenv("RGH_GX") != NULL; if (lg) fprintf(stderr, "[gx] PE %02X <- %04X\n", off, v); }
  if (off == 0x0A) {
    u16 st = pe_regs[5] & 0xC;
    st &= ~(v & 0xC);
    pe_regs[5] = (v & 0x3) | st;
    pe_update();
    return;
  }
  pe_regs[off / 2] = v;
}
void gx_frame(u32 xfb) { gx_backend_frame(xfb); }
void gx_init(void) { memset(g_cp, 0, sizeof g_cp); gxrender_init(); }
