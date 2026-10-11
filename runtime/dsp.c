/* dsp.c - DSP interface (0xCC005000): mailboxes, control register, ARAM/AI DMA, plus a high-level
 * stand-in for the DSP side: the boot ROM handshake and the AX microcode's mail protocol.
 * Command lists are acknowledged at once; mixing for the host audio output is done separately
 * from the guest's AX voice blocks (see audio.c), so no DSP microcode is executed. */
#include "hw.h"
#include <stdio.h>
#include <stdlib.h>

#define DSP_INIT 0xDCD10000u
#define DSP_RESUME 0xDCD10001u
#define DSP_YIELD 0xDCD10002u
#define DSP_DONE 0xDCD10003u

static u16 regs[0x40];
static u32 out_q[64]; static int out_n;        /* DSP -> CPU mails waiting */
static int g_log;
static enum { ROM, ROM_BOOT, UCODE } state = ROM;
static int boot_step; static u32 pending_cmd; static int expect_cmdlist; static u32 cmdlist_len;
u64 g_dsp_cmdlists;
static u64 ai_next; static u32 ai_period;

static void update_irq(void) {
  u16 c = regs[5];
  pi_set(PI_DSP, ((c & 0x08) && (c & 0x10)) || ((c & 0x20) && (c & 0x40)) || ((c & 0x80) && (c & 0x100)));
}
static void load_mbox(void) {
  if (!(regs[2] & 0x8000) && out_n) {
    u32 m = out_q[0]; memmove(out_q, out_q + 1, --out_n * 4);
    regs[2] = (u16)(m >> 16) | 0x8000; regs[3] = (u16)m;
  }
}
static void push_mail(u32 m, int irq) {
  if (g_log) fprintf(stderr, "[dsp] -> %08X\n", m);
  if (out_n < 64) out_q[out_n++] = m;
  load_mbox();
  if (irq) { regs[5] |= 0x80; update_irq(); }
}

static void reset(void) { state = ROM; boot_step = 0; out_n = 0; regs[2] = 0; push_mail(0x8071FEED, 0); }

static void cpu_mail(u32 m) {
  if (g_log) fprintf(stderr, "[dsp] <- %08X\n", m);
  if (state == ROM || state == ROM_BOOT) {
    /* boot ROM: pairs (0x80F3xxxx, value); 0x80F3D001 + init vector starts the microcode */
    if (boot_step == 0) { pending_cmd = m; boot_step = 1; state = ROM_BOOT; return; }
    boot_step = 0;
    if (pending_cmd == 0x80F3D001) { state = UCODE; expect_cmdlist = 0; push_mail(DSP_INIT, 1); }
    return;
  }
  /* AX microcode */
  if (expect_cmdlist) {
    expect_cmdlist = 0; g_dsp_cmdlists++;
    { extern void ax_process(u32 addr, u32 len); ax_process(m, cmdlist_len); }
    push_mail(DSP_YIELD, 1);            /* command list "processed" */
    return;
  }
  if ((m & 0xFFFF0000) == 0xBABE0000) { expect_cmdlist = 1; cmdlist_len = m & 0xFFFF; return; }
  if (m == 0xCDD10000) { state = ROM; boot_step = 0; push_mail(0x8071FEED, 0); return; }   /* new ucode: back to ROM */
  if (m == 0xCDD10001) { reset(); return; }
  if (m == 0xCDD10002) { push_mail(DSP_RESUME, 1); return; }                              /* continue */
}

u16 dsp_read16(u32 o) {
  switch (o) {
    case 0x00: return regs[0] & 0x7FFF;                 /* CPU->DSP mailbox: always delivered */
    case 0x02: return regs[1];
    case 0x04: load_mbox(); return regs[2];
    case 0x06: { u16 v = regs[3]; if (regs[2] & 0x8000) { regs[2] &= 0x7FFF; load_mbox(); } return v; }
    case 0x0A: return regs[5] & ~0x0201;
    case 0x3A: return 0;
  }
  return regs[(o / 2) & 0x3F];
}
void dsp_write16(u32 o, u16 v) {
  switch (o) {
    case 0x00: regs[0] = v; break;
    case 0x02: regs[1] = v; cpu_mail((u32)regs[0] << 16 | v); break;
    case 0x0A: {
      u16 st = regs[5] & (0x08 | 0x20 | 0x80);
      st &= ~(v & (0x08 | 0x20 | 0x80));
      regs[5] = (v & ~(0x08 | 0x20 | 0x80 | 0x01 | 0x200)) | st;
      if (v & 0x01) reset();                            /* RES */
      break;
    }
    case 0x2A: regs[0x15] = v; regs[5] |= 0x20; break;  /* ARAM DMA: completes at once (no ARAM on Wii) */
    case 0x36:
      regs[0x1B] = v;
      if (v & 0x8000) {
        extern u32 hw_ai_dsp_rate(void);
        u32 bytes = (u32)(v & 0x7FFF) * 32;
        ai_period = (u32)(TB_HZ * bytes / (hw_ai_dsp_rate() * 4)); if (!ai_period) ai_period = TB_HZ / 200;
        { static int once; if (!once++ && g_log) fprintf(stderr, "[dsp] AI DMA %u bytes at %u Hz\n", bytes, hw_ai_dsp_rate()); }
        ai_next = hw_timebase() + ai_period;
      }
      break;
    default: regs[(o / 2) & 0x3F] = v;
  }
  update_irq();
}
u32 dsp_ai_dma_addr(void) { return ((u32)regs[0x18] << 16 | regs[0x19]) & 0x1FFFFFE0; }
u32 dsp_ai_dma_len(void) { return (u32)(regs[0x1B] & 0x7FFF) * 32; }
void dsp_tick(u64 tb) {
  if ((regs[0x1B] & 0x8000) && ai_period && tb >= ai_next) {
    while (ai_next <= tb) ai_next += ai_period;
    regs[5] |= 0x08;                                     /* AIDINT: DMA block played */
    update_irq();
  }
}
void dsp_init(void) { g_log = getenv("RGH_DSP") != NULL; reset(); }
