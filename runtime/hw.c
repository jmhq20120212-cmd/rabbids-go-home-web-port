/* hw.c - Hollywood/Flipper MMIO for the recompiled guest: PI, VI, EXI (RTC/SRAM/UART), SI, DSP/AI,
 * and dispatch of CP/PE/GX-FIFO to gx.c and IPC to ios.c.  Behaviour follows public hardware
 * documentation (YAGCD, WiiBrew) and the Dolphin emulator's register descriptions. */
#define _GNU_SOURCE
#include "hw.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int g_mmio_log;
static u64 g_tb0;
static u64 host_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (u64)t.tv_sec * 1000000000ull + t.tv_nsec; }
/* 60.75 MHz. RGH_TIMESCALE=n runs the clock n times faster (benchmarks: how fast could the game go) */
static u64 tb_scale;
u64 hw_timebase(void) {
  if (!tb_scale) { const char *e = getenv("RGH_TIMESCALE"); tb_scale = e && atoi(e) > 0 ? (u64)atoi(e) : 1; }
  return (host_ns() - g_tb0) * tb_scale * 6075ull / 100000ull;
}

/* ---------------------------------------------------------------- PI */
static u32 pi_cause, pi_mask, pi_fifo_base, pi_fifo_end, pi_fifo_wptr;
u32 hw_fifo_wptr(void) { return pi_fifo_wptr & 0x03FFFFFF; }
void pi_set(u32 bit, int on) { if (on) pi_cause |= bit; else pi_cause &= ~bit; }
int hw_irq_pending(void) { return (pi_cause & pi_mask) != 0; }
u32 hw_pi_cause(void) { return pi_cause & pi_mask; }

/* ---------------------------------------------------------------- VI */
static u8 vi[0x100];
static u64 vi_next, vi_frame_start, vi_period;
u64 g_vi_frames;
static u16 vi_r16(u32 o) { return (u16)(vi[o] << 8 | vi[o + 1]); }
static void vi_w16(u32 o, u16 v) { vi[o] = v >> 8; vi[o + 1] = (u8)v; }
static u32 vi_r32(u32 o) { return (u32)vi_r16(o) << 16 | vi_r16(o + 2); }
static void vi_update_irq(void) {
  int on = 0;
  for (int i = 0; i < 4; i++) { u32 d = vi_r32(0x30 + i * 4); if ((d & 0x80000000) && (d & 0x10000000)) on = 1; }
  pi_set(PI_VI, on);
}
static void vi_tick(u64 tb) {
  if (tb < vi_next) return;
  while (vi_next <= tb) vi_next += vi_period;
  vi_frame_start = vi_next - vi_period;
  g_vi_frames++;
  for (int i = 0; i < 4; i++) {
    u32 d = vi_r32(0x30 + i * 4);
    if (d & 0x10000000) { d |= 0x80000000; vi_w16(0x30 + i * 4, d >> 16); }
  }
  vi_update_irq();
  /* top-field framebuffer address (TFBL) for presenting */
  u32 tfbl = vi_r32(0x1C);
  u32 addr = (tfbl & 0x00FFFFFF);
  if (tfbl & 0x10000000) addr <<= 5;
  gx_frame(addr);
}
static u16 vi_read16(u32 o) {
  if (o == 0x2C || o == 0x2E) {
    u64 tb = hw_timebase();
    u64 pos = (tb - vi_frame_start) % vi_period;
    u32 line = (u32)(pos * 525 / vi_period) + 1, h = (u32)((pos * 525 % vi_period) * 858 / vi_period) + 1;
    return o == 0x2C ? (u16)line : (u16)h;
  }
  return vi_r16(o);
}
static void vi_write16(u32 o, u16 v) { vi_w16(o, v); if (o >= 0x30 && o < 0x40) vi_update_irq(); }

/* ---------------------------------------------------------------- EXI (IPL device: RTC, SRAM, UART) */
static struct { u32 csr, mar, len, cr, data; int dev_sel; u32 cmd; int have_cmd; u32 addr; } exi[3];
static u8 sram[64];
static u32 rtc_bias;
static void sram_init(void) {
  memset(sram, 0, 64);
  sram[0x12] = 1;                      /* language: English */
  sram[0x13] = 0x24;                   /* flags: sound stereo, ... */
  u16 s = 0, si = 0;
  for (int i = 0x0C; i < 0x14; i += 2) { u16 w = (u16)(sram[i] << 8 | sram[i + 1]); s += w; si += (u16)~w; }
  sram[0] = s >> 8; sram[1] = (u8)s; sram[2] = si >> 8; sram[3] = (u8)si;
}
static u32 rtc_now(void) {
  time_t t = time(NULL); struct tm lt; localtime_r(&t, &lt);
  return (u32)(t + lt.tm_gmtoff - 946684800) - rtc_bias;
}
static void ipl_xfer(int write, u8 *buf, u32 n) {
  for (u32 i = 0; i < n; i++, exi[0].addr++) {
    u32 a = exi[0].addr;
    if (a >= 0x800000 && a < 0x800004) {
      u32 r = rtc_now(); int k = a - 0x800000;
      if (!write) buf[i] = (u8)(r >> (24 - 8 * k));
    } else if (a >= 0x800004 && a < 0x800044) {
      if (write) sram[a - 0x800004] = buf[i]; else buf[i] = sram[a - 0x800004];
    } else if (a >= 0x800400 && a < 0x800500) {
      if (write && buf[i]) { if (buf[i] != '\r') fputc(buf[i], stderr); }
      else if (!write) buf[i] = 0;
      exi[0].addr--;                     /* UART does not auto-increment */
    } else if (!write) buf[i] = 0;
  }
}
static void exi_write(int ch, u32 reg, u32 v) {
  switch (reg) {
    case 0x00: {
      u32 keep = exi[ch].csr & (0x2 | 0x8 | 0x800);
      keep &= ~(v & (0x2 | 0x8 | 0x800));                    /* write-1-to-clear status bits */
      int sel_before = exi[ch].dev_sel;
      exi[ch].csr = (v & ~(0x2 | 0x8 | 0x800)) | keep;
      int sel = (v >> 7) & 7;
      exi[ch].dev_sel = sel == 1 ? 0 : sel == 2 ? 1 : sel == 4 ? 2 : -1;
      if (exi[ch].dev_sel != sel_before) exi[ch].have_cmd = 0;
      break;
    }
    case 0x04: exi[ch].mar = v & 0x03FFFFE0; break;
    case 0x08: exi[ch].len = v & 0x03FFFFE0; break;
    case 0x10: exi[ch].data = v; break;
    case 0x0C: {
      exi[ch].cr = v;
      if (!(v & 1)) break;
      int dma = (v >> 1) & 1, rw = (v >> 2) & 3, tlen = ((v >> 4) & 3) + 1;
      int ipl = (ch == 0 && exi[ch].dev_sel == 1);
      if (ipl) {
        if (!exi[ch].have_cmd && !dma && rw == 1) {
          exi[ch].cmd = exi[ch].data; exi[ch].have_cmd = 1;
          exi[ch].addr = (exi[ch].cmd & 0x7FFFFFFF) >> 6;
        } else if (dma) {
          u8 *p = hp(exi[ch].mar); ipl_xfer(rw == 1, p, exi[ch].len);
        } else {
          u8 b[4] = {exi[ch].data >> 24, exi[ch].data >> 16, exi[ch].data >> 8, exi[ch].data};
          ipl_xfer(rw == 1, b, tlen);
          if (rw != 1) { u32 d = 0; for (int i = 0; i < 4; i++) d = d << 8 | (i < tlen ? b[i] : 0); exi[ch].data = d; }
        }
      } else if (!dma && rw != 1) {
        exi[ch].data = 0;   /* nothing attached: reads return 0 (ID 0 = no device) */
      }
      exi[ch].cr &= ~1u;
      exi[ch].csr |= 0x8;   /* TCINT */
      break;
    }
  }
  int irq = 0;
  for (int i = 0; i < 3; i++) if ((exi[i].csr & 0x8 && exi[i].csr & 0x4) || (exi[i].csr & 0x2 && exi[i].csr & 0x1)) irq = 1;
  pi_set(PI_EXI, irq);
}
static u32 exi_read(int ch, u32 reg) {
  switch (reg) {
    case 0x00: return exi[ch].csr & ~0x1000u;   /* EXT=0: no memory card */
    case 0x04: return exi[ch].mar; case 0x08: return exi[ch].len;
    case 0x0C: return exi[ch].cr; case 0x10: return exi[ch].data;
  }
  return 0;
}

/* ---------------------------------------------------------------- SI (GameCube ports: nothing connected) */
static u32 si_regs[0x40], si_buf[32];
static void si_write(u32 o, u32 v) {
  if (o >= 0x80 && o < 0x100) { si_buf[(o - 0x80) / 4] = v; return; }
  if (o == 0x34) {           /* COMCSR */
    u32 old = si_regs[0x34 / 4];
    if (v & 0x80000000) old &= ~0x80000000u;              /* clear TCINT */
    old = (old & 0x80000000) | (v & 0x7FFFFFFE);
    if (v & 1) {             /* TSTART: complete immediately with "no response" */
      int ch = (v >> 1) & 3;
      si_regs[0x38 / 4] |= 0x08u << (24 - 8 * ch);
      old |= 0x80000000;
    }
    si_regs[0x34 / 4] = old;
    pi_set(PI_SI, (old & 0x80000000) && (old & 0x40000000));
    return;
  }
  if (o == 0x38) { si_regs[0x38 / 4] &= ~(v & 0x0F0F0F0F); return; }
  si_regs[o / 4 & 0x3F] = v;
}
static u32 si_read(u32 o) {
  if (o >= 0x80 && o < 0x100) return si_buf[(o - 0x80) / 4];
  if (o < 0x30 && (o % 12) != 0) return 0x80000000;       /* input buffers: ERRSTAT */
  return si_regs[o / 4 & 0x3F];
}

/* ---------------------------------------------------------------- DSP (dsp.c) / AI */
u16 dsp_read16(u32 o); void dsp_write16(u32 o, u16 v); void dsp_tick(u64 tb); void dsp_init(void);
static u32 ai_regs[4]; static u64 ai_tb_start;
static u32 ai_read(u32 o) {
  if (o == 0x08) return (ai_regs[0] & 1) ? (u32)((hw_timebase() - ai_tb_start) * 48000 / TB_HZ) : ai_regs[2];
  return ai_regs[(o / 4) & 3];
}
u32 hw_ai_dsp_rate(void) { return (ai_regs[0] & 0x40) ? 32000 : 48000; }
static void ai_write(u32 o, u32 v) {
  if (o == 0) {
    u32 st = ai_regs[0] & 0x8; if (v & 0x8) st = 0;
    if ((v & 1) && !(ai_regs[0] & 1)) ai_tb_start = hw_timebase();
    if (v & 0x20) ai_regs[2] = 0;
    ai_regs[0] = (v & ~0x28u) | st;
    pi_set(PI_AI, (ai_regs[0] & 0x8) && (ai_regs[0] & 0x4));
    return;
  }
  ai_regs[(o / 4) & 3] = v;
}

/* ---------------------------------------------------------------- Hollywood misc regs */
static u32 hw_regs[0x400];

/* ---------------------------------------------------------------- dispatch */
static void logm(const char *k, u32 a, u32 v, int w) {
  if (g_mmio_log) fprintf(stderr, "[mmio] %s%d %08X %s %08X (~%s)\n", k, w, a, w ? "<-" : "->", v, rt_sym(g_cpu.pc_hint));
}
static u32 mmio_read(u32 a, int sz) {
  u32 p = a & 0x0FFFFFFF;       /* 0x0C...... / 0x0D...... */
  u32 v = 0;
  if (p >= 0x0C000000 && p < 0x0C001000) v = gx_cp_read(p & 0xFFE);
  else if (p >= 0x0C001000 && p < 0x0C002000) v = gx_pe_read(p & 0xFFE);
  else if (p >= 0x0C002000 && p < 0x0C003000) {
    u32 o = p & 0xFF;
    v = sz == 4 ? ((u32)vi_read16(o) << 16 | vi_read16(o + 2)) : vi_read16(o & ~1u);
    if (sz == 1) v = (o & 1) ? (v & 0xFF) : (v >> 8);
    return v;
  } else if (p >= 0x0C003000 && p < 0x0C004000) {
    switch (p & 0xFF) {
      case 0x00: v = pi_cause | 0x10000; break;
      case 0x04: v = pi_mask; break;
      case 0x0C: v = pi_fifo_base; break; case 0x10: v = pi_fifo_end; break; case 0x14: v = pi_fifo_wptr; break;
      case 0x2C: v = 0x246500B1; break;
      default: v = 0;
    }
  } else if (p >= 0x0C005000 && p < 0x0C006000) {
    u32 o = p & 0xFF;
    v = sz == 4 ? ((u32)dsp_read16(o) << 16 | dsp_read16(o + 2)) : dsp_read16(o & ~1u);
    return v;
  } else if ((p & 0xFEFFF000) == 0x0C006000) {
    u32 o = p & 0xFFF;
    if (o >= 0x400 && o < 0x500) v = si_read(o - 0x400);
    else if (o >= 0x800 && o < 0x840) v = exi_read((o - 0x800) / 0x14, (o - 0x800) % 0x14);
    else if (o >= 0xC00 && o < 0xC20) v = ai_read(o - 0xC00);
    else v = 0;
    if (sz == 2) v = (p & 2) ? (v & 0xFFFF) : (v >> 16);
  } else if (p >= 0x0D000000 && p < 0x0D000010) v = ios_read(p & 0xF);
  else if (p >= 0x0D000000 && p < 0x0D001000) v = (p & 0xFFF) == 0x30 ? ios_read(0x30) : hw_regs[(p & 0xFFF) / 4];
  else if (p >= 0x0D800000 && p < 0x0D801000) v = hw_regs[(p & 0xFFF) / 4];
  logm("R", a, v, 0);
  return v;
}
static void mmio_write(u32 a, u32 v, int sz) {
  u32 p = a & 0x0FFFFFFF;
  logm("W", a, v, sz);
  if (p >= 0x0C008000 && p < 0x0C009000) {           /* write-gather pipe -> GX FIFO */
    if (!gx_gp_linked()) {
      /* CPU FIFO not linked to the GP (GXBeginDisplayList): the bytes only go to memory, where the
       * display list is later executed with GXCallDisplayList */
      u8 *m = hp((pi_fifo_wptr & 0x1FFFFFFF) | 0x80000000);   /* MEM2 too (0x1xxxxxxx): display lists can live there */
      if (m) for (int k = 0; k < sz; k++) m[k] = (u8)(v >> (8 * (sz - 1 - k)));
      pi_fifo_wptr += sz;
      if ((pi_fifo_wptr & 0x1FFFFFFF) >= (pi_fifo_end & 0x1FFFFFFF) && pi_fifo_end) pi_fifo_wptr = pi_fifo_base | 0x20000000;
      return;
    }
    gx_fifo_write(v, sz);
    u32 prev = pi_fifo_wptr & 0x03FFFFFF;
    pi_fifo_wptr += sz;
    if ((pi_fifo_wptr & 0x1FFFFFFF) >= (pi_fifo_end & 0x1FFFFFFF) && pi_fifo_end) pi_fifo_wptr = pi_fifo_base | 0x20000000;
    gx_fifo_advanced(prev, pi_fifo_wptr & 0x03FFFFFF);
    return;
  }
  if (p >= 0x0C000000 && p < 0x0C001000) { gx_cp_write(p & 0xFFE, (u16)v); return; }
  if (p >= 0x0C001000 && p < 0x0C002000) { gx_pe_write(p & 0xFFE, (u16)v); return; }
  if (p >= 0x0C002000 && p < 0x0C003000) {
    u32 o = p & 0xFF;
    if (sz == 4) { vi_write16(o, v >> 16); vi_write16(o + 2, (u16)v); }
    else if (sz == 2) vi_write16(o, (u16)v);
    else { vi[o] = (u8)v; }
    return;
  }
  if (p >= 0x0C003000 && p < 0x0C004000) {
    switch (p & 0xFF) {
      case 0x00: pi_cause &= ~(v & PI_RSW); break;
      case 0x04: pi_mask = v; break;
      case 0x0C: pi_fifo_base = v; break; case 0x10: pi_fifo_end = v; break; case 0x14: pi_fifo_wptr = v; break;
    }
    return;
  }
  if (p >= 0x0C005000 && p < 0x0C006000) {
    u32 o = p & 0xFF;
    if (sz == 4) { dsp_write16(o, v >> 16); dsp_write16(o + 2, (u16)v); } else dsp_write16(o & ~1u, (u16)v);
    return;
  }
  if ((p & 0xFEFFF000) == 0x0C006000) {
    u32 o = p & 0xFFF;
    if (o >= 0x400 && o < 0x500) si_write(o - 0x400, v);
    else if (o >= 0x800 && o < 0x840) exi_write((o - 0x800) / 0x14, (o - 0x800) % 0x14, v);
    else if (o >= 0xC00 && o < 0xC20) ai_write(o - 0xC00, v);
    return;
  }
  if (p >= 0x0D000000 && p < 0x0D000010) { ios_write(p & 0xF, v); return; }
  if (p >= 0x0D000000 && p < 0x0D001000) {
    if ((p & 0xFFF) == 0x30 || (p & 0xFFF) == 0x34) { ios_write(p & 0xFFF, v); return; }
    hw_regs[(p & 0xFFF) / 4] = v; return;
  }
  if (p >= 0x0D800000 && p < 0x0D801000) { hw_regs[(p & 0xFFF) / 4] = v; return; }
}
/* write-gather pipe store from paired-single / float stores (the GX vertex stream): skips the MMIO dispatch */
void hw_wgpipe_w32(u32 v) {
  if (!gx_gp_linked() || g_mmio_log) { mmio_write(0xCC008000u, v, 4); return; }
  gx_fifo_write(v, 4);
  u32 prev = pi_fifo_wptr & 0x03FFFFFF;
  pi_fifo_wptr += 4;
  if ((pi_fifo_wptr & 0x1FFFFFFF) >= (pi_fifo_end & 0x1FFFFFFF) && pi_fifo_end) pi_fifo_wptr = pi_fifo_base | 0x20000000;
  gx_fifo_advanced(prev, pi_fifo_wptr & 0x03FFFFFF);
}
u32 mmio_r32(u32 a) { return mmio_read(a, 4); }
u16 mmio_r16(u32 a) { return (u16)mmio_read(a, 2); }
u8 mmio_r8(u32 a) { u32 v = mmio_read(a & ~1u, 2); return (a & 1) ? (u8)v : (u8)(v >> 8); }
void mmio_w32(u32 a, u32 v) { if ((a & 0x0FFFF000u) == 0x0C008000u) { hw_wgpipe_w32(v); return; } mmio_write(a, v, 4); }
void mmio_w16(u32 a, u16 v) { mmio_write(a, v, 2); }
void mmio_w8(u32 a, u8 v) {
  u32 p = a & 0x0FFFFFFF;
  if (p >= 0x0C008000 && p < 0x0C009000) { mmio_write(a, v, 1); return; }
  if (p >= 0x0C002000 && p < 0x0C003000) { mmio_write(a, v, 1); return; }
  mmio_write(a, v, 1);
}

/* ---------------------------------------------------------------- tick */
void hw_tick(void) {
  u64 tb = hw_timebase();
  vi_tick(tb);
  dsp_tick(tb);
  ios_tick();
  pi_set(PI_IPC, ios_irq());
}

void hw_init(const char *partdir) {
  g_mmio_log = getenv("RGH_MMIO") != NULL;
  g_tb0 = host_ns();
  vi_period = TB_HZ * 1001 / 60000;
  vi_next = vi_period;
  sram_init();
  dsp_init();
  { extern void ax_init(void); ax_init(); }
  { extern void wpad_init(void); wpad_init(); }
  gx_init();
  ios_init(partdir);
}
void hw_shutdown(void) {
  extern u64 g_gx_draws, g_gx_verts;
  fprintf(stderr, "[hw] %llu VI frames, %llu GX FIFO bytes, %llu draws, %llu vertices\n", (unsigned long long)g_vi_frames,
          (unsigned long long)g_gx_bytes, (unsigned long long)g_gx_draws, (unsigned long long)g_gx_verts);
}
