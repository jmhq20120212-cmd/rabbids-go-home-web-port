/* os.c - guest threads and exceptions.
 *
 * The guest OS (Nintendo's RVL SDK, running as recompiled code) schedules threads by saving and
 * loading OSContext structures and enters interrupt handlers through exception vectors.
 * Translated code keeps its call stack on the host stack, so every guest thread runs on its own
 * host fiber (fiber.h):
 *   - OSSaveContext(ctx) (HLE, only called by SelectThread) binds ctx to the current fiber.
 *   - OSLoadContext(ctx) (HLE) switches to the fiber bound to ctx. When that fiber later gets
 *     resumed, its own OSLoadContext call returns and the generated SelectThread code continues
 *     right after its OSSaveContext call (see 'load'/'save' in hle.list), as on hardware.
 *   - A context never bound to a fiber (new thread) gets a fresh fiber starting at ctx->srr0.
 *   - Interrupts are taken at function entries / loop back-edges (rt_poll): the exception prologue
 *     is emulated on the interrupted fiber, which then hands over to the interrupt fiber; the guest
 *     handler's final OSLoadContext resumes the interrupted (or another) thread. */
#include "hw.h"
#include "fiber.h"
#include <stdio.h>
#include <stdlib.h>

#define CTX_CR 0x080
#define CTX_LR 0x084
#define CTX_CTR 0x088
#define CTX_XER 0x08C
#define CTX_SRR0 0x198
#define CTX_SRR1 0x19C
#define CTX_STATE 0x1A2
#define CTX_GQR 0x1A4
#define STATE_EXC 2

typedef struct Bind { u32 ctx; Fiber *f; f64 ps0[32], ps1[32]; u32 fpscr; u32 expect_pc; int used; } Bind;

static Bind g_binds[256]; static int g_nbinds;
static Bind *g_target;            /* bind the next resumed fiber must restore */
static u32 g_start_ctx;           /* context a fresh thread fiber starts from */
static Fiber *g_irq_pool[32]; static int g_nirq;   /* fibers that run guest interrupt handlers */
static int g_irq_exc; static u32 g_irq_ctx;
static int g_trace_threads;
u64 g_irq_count[16];

static Bind *bind_find(u32 ctx) { for (int i = 0; i < g_nbinds; i++) if (g_binds[i].used && g_binds[i].ctx == ctx) return &g_binds[i]; return NULL; }
static Bind *bind_get(u32 ctx) {
  Bind *b = bind_find(ctx); if (b) return b;
  for (int i = 0; i < g_nbinds; i++) if (!g_binds[i].used) { b = &g_binds[i]; goto found; }
  if (g_nbinds == 256) { fprintf(stderr, "[os] too many contexts\n"); exit(5); }
  b = &g_binds[g_nbinds++];
found:
  memset(b, 0, sizeof *b); b->used = 1; b->ctx = ctx; return b;
}
static void snapshot_fp(Bind *b, CPU *c) { memcpy(b->ps0, c->ps0, sizeof b->ps0); memcpy(b->ps1, c->ps1, sizeof b->ps1); b->fpscr = c->fpscr; }

/* OSLoadContext semantics: registers from the guest context; FPRs from the snapshot taken when the
 * context was bound (the guest saves FPRs lazily through an exception we never raise). */
static void restore_cpu(CPU *c, u32 ctx, Bind *b) {
  if (b) { memcpy(c->ps0, b->ps0, sizeof c->ps0); memcpy(c->ps1, b->ps1, sizeof c->ps1); c->fpscr = b->fpscr; }
  u16 st = (u16)R16(ctx + CTX_STATE);
  if (st & STATE_EXC) { W16(ctx + CTX_STATE, st & ~STATE_EXC); for (int i = 5; i < 32; i++) c->gpr[i] = R32(ctx + 4 * i); }
  else for (int i = 13; i < 32; i++) c->gpr[i] = R32(ctx + 4 * i);
  for (int i = 1; i < 8; i++) c->gqr[i] = R32(ctx + CTX_GQR + 4 * i);
  c->cr = R32(ctx + CTX_CR); c->lr = R32(ctx + CTX_LR); c->ctr = R32(ctx + CTX_CTR);
  u32 x = R32(ctx + CTX_XER); c->xer_so = x >> 31; c->xer_ov = (x >> 30) & 1; c->xer_ca = (x >> 29) & 1; c->xer_bc = x & 127;
  c->gpr[0] = R32(ctx + 0); c->gpr[1] = R32(ctx + 4); c->gpr[2] = R32(ctx + 8);
  c->srr0 = R32(ctx + CTX_SRR0); c->srr1 = R32(ctx + CTX_SRR1);
  c->gpr[4] = R32(ctx + 16); c->gpr[3] = R32(ctx + 12);
  c->msr = c->srr1;
  if (c->msr & 0x8000) g_poll_budget = 0;
  if (c->gpr[1] < 0x80000000u || c->gpr[1] >= 0x94000000u) {
    static int n; if (n++ < 5) fprintf(stderr, "[os] BAD restore ctx %08X r1=%08X srr0=%08X state=%04X bind=%p f=%d expect=%08X fiber=%d\n", ctx, c->gpr[1], c->srr0, st,
      (void *)b, b && b->f ? fiber_id(b->f) : -1, b ? b->expect_pc : 0, fiber_id(fiber_self()));
  }
}

/* leave the current fiber for 'to'; returns when someone resumes us (g_target = our bind) */
static void switch_and_resume(CPU *c, Fiber *to) {
  fiber_switch(to);
  Bind *t = g_target;
  restore_cpu(c, t->ctx, t);
  t->used = 0;              /* consumed: a context is bound again by its next save / interrupt */
}

static void thread_entry(void) {
  CPU *c = &g_cpu;
  u32 ctx = g_start_ctx;
  restore_cpu(c, ctx, NULL);
  memset(c->ps0, 0, sizeof c->ps0); memset(c->ps1, 0, sizeof c->ps1);
  u32 pc = c->srr0;
  if (g_trace_threads) fprintf(stderr, "[os] fiber %d: thread ctx %08X starts at %08X (%s)\n", fiber_id(fiber_self()), ctx, pc, rt_sym(pc));
  call_indirect(c, pc);
  u32 lr = c->lr;   /* the thread function returned: continue at its LR (OSExitThread) */
  call_indirect(c, lr);
  fprintf(stderr, "[os] thread exit path returned (ctx %08X)\n", ctx);
  exit(6);
}

/* ---------------------------------------------------------------- HLE: OSSaveContext / OSLoadContext */
void o_80366CC0(CPU *restrict c);
void hle_80366CC0(CPU *restrict c) {             /* u32 OSSaveContext(OSContext *ctx) */
  u32 ctx = c->gpr[3];
  o_80366CC0(c);                                  /* the real routine fills the guest structure, r3 = 0 */
  Bind *b = bind_get(ctx);
  b->f = fiber_self(); snapshot_fp(b, c); b->expect_pc = c->lr;
}

void hle_80366D40(CPU *restrict c) {             /* void OSLoadContext(OSContext *ctx) */
  u32 ctx = c->gpr[3];
  Bind *b = bind_find(ctx);
  u32 pc = R32(ctx + CTX_SRR0);
  if (b && b->expect_pc != pc) { if (g_trace_threads) fprintf(stderr, "[os] ctx %08X re-initialised (srr0 %08X)\n", ctx, pc); b->used = 0; b = NULL; }
  if (!b) {
    if (g_trace_threads) fprintf(stderr, "[os] new thread ctx %08X at %08X (%s)\n", ctx, pc, rt_sym(pc));
    g_start_ctx = ctx;
    switch_and_resume(c, fiber_new(thread_entry));
    return;
  }
  if (b->f == fiber_self()) { restore_cpu(c, ctx, b); b->used = 0; return; }    /* resuming ourselves */
  if (g_trace_threads > 1) fprintf(stderr, "[os] fiber %d -> %d (ctx %08X)\n", fiber_id(fiber_self()), fiber_id(b->f), ctx);
  g_target = b;
  switch_and_resume(c, b->f);
}

/* rfi executed by translated code (outside the HLE'd OSLoadContext) */
void rt_rfi(CPU *restrict c) {
  c->msr = c->srr1;
  if (c->srr0 == c->lr) return;
  call_indirect(c, c->srr0);
}

/* ---------------------------------------------------------------- exceptions */
static u32 dec_val; static u64 dec_tb; static int dec_armed; static u64 dec_deadline;
u32 os_get_dec(void) { return dec_val - (u32)(hw_timebase() - dec_tb); }
void os_set_dec(u32 v) {
  dec_val = v; dec_tb = hw_timebase();
  dec_armed = (s32)v >= 0; dec_deadline = dec_tb + v + 1;
  if (dec_armed && v < 2000) g_poll_budget = 0;
}

static void irq_entry(void) {
  CPU *c = &g_cpu;
  u32 h = R32(0x80003000 + 4 * g_irq_exc);
  c->gpr[3] = g_irq_exc; c->gpr[4] = g_irq_ctx; c->lr = 0;
  call_indirect(c, h);
  /* a handler always ends in OSLoadContext; if it returned, resume the interrupted thread */
  c->gpr[3] = g_irq_ctx; hle_80366D40(c);
  fprintf(stderr, "[os] interrupt fiber fell through\n"); exit(9);
}

/* a free interrupt fiber: not the current one and not holding an interrupted handler (nested IRQ) */
static Fiber *free_irq_fiber(void) {
  for (int i = 0; i < g_nirq; i++) {
    Fiber *x = g_irq_pool[i]; int busy = x == fiber_self();
    for (int k = 0; k < g_nbinds && !busy; k++) if (g_binds[k].used && g_binds[k].f == x) busy = 1;
    if (!busy) return x;
  }
  return NULL;
}
/* when every interrupt fiber is busy with a nested handler, the interrupt waits (it stays pending) until one
 * finishes: a slow start (the browser still compiling the code) can stack many timer interrupts */
static void dump_threads(void);
static int irq_can_inject(void) {
  static u64 since; static int warned;
  if (g_nirq < 32 || free_irq_fiber() != NULL) { since = 0; return 1; }
  u64 tb = hw_timebase();
  if (!since) since = tb;
  else if (tb - since > 3ull * 60750000 && warned < 3) { warned++; fprintf(stderr, "[os] WARNING: interrupts held back for more than 3 s (all handlers busy)\n"); dump_threads(); since = tb; }
  return 0;
}

static void inject(CPU *c, int exc) {
  u32 ctx = R32(0x800000D4);
  u32 h = R32(0x80003000 + 4 * exc);
  if (!h) return;
  /* emulated exception prologue (__OSEVStart): save r3-r5, cr, lr, ctr, xer, srr0/1 in the context */
  W32(ctx + 12, c->gpr[3]); W32(ctx + 16, c->gpr[4]); W32(ctx + 20, c->gpr[5]);
  W16(ctx + CTX_STATE, R16(ctx + CTX_STATE) | STATE_EXC);
  W32(ctx + CTX_CR, c->cr); W32(ctx + CTX_LR, c->lr); W32(ctx + CTX_CTR, c->ctr);
  W32(ctx + CTX_XER, (c->xer_so << 31) | (c->xer_ov << 30) | (c->xer_ca << 29) | c->xer_bc);
  u32 resume = c->pc_hint;
  W32(ctx + CTX_SRR0, resume); W32(ctx + CTX_SRR1, c->msr);
  Bind *b = bind_get(ctx);
  b->f = fiber_self(); snapshot_fp(b, c); b->expect_pc = resume;
  g_irq_count[exc]++;
  /* the handler runs on the interrupt fiber with the interrupted thread's stack pointer */
  c->srr0 = resume; c->srr1 = c->msr; c->msr &= ~0x8000u;
  g_irq_exc = exc; g_irq_ctx = ctx;
  Fiber *f = free_irq_fiber();
  if (f) fiber_restart(f, irq_entry);
  else { f = g_irq_pool[g_nirq++] = fiber_new(irq_entry); if (g_nirq > 8) fprintf(stderr, "[os] %d interrupt fibers (deep nesting)\n", g_nirq); }
  switch_and_resume(c, f);
}

static void dump_threads(void) {
  { CPU *c = &g_cpu; u32 r1 = c->gpr[1]; fprintf(stderr, "  current %08X: %s <- %s", R32(0x800000E4), rt_sym(c->pc_hint), rt_sym(c->lr));
    for (int k = 0; k < 14 && r1 > 0x80000000 && r1 < 0x94000000; k++) { u32 back = R32(r1); if (!back) break; fprintf(stderr, " <- %s", rt_sym(R32(back + 4))); r1 = back; }
    fprintf(stderr, "\n"); }
  u32 t = R32(0x800000DC); int n = 0;
  while (t && n++ < 40) {
    u16 st = (u16)R16(t + 0x2C8); s32 prio = (s32)R32(t + 0x2D0);
    u32 r1 = R32(t + 4), lr = R32(t + 0x84);
    u32 mx = R32(t + 0x2F0);
    fprintf(stderr, "  thread %08X state %d prio %d mutex %08X(owner %08X) lr %08X %s |", t, st, prio, mx, mx ? R32(mx + 8) : 0, lr, rt_sym(lr));
    for (int k = 0; k < 10 && r1 > 0x80000000 && r1 < 0x94000000; k++) {
      u32 back = R32(r1); if (!back) break;
      u32 ra = R32(back + 4); fprintf(stderr, " %s", rt_sym(ra)); r1 = back;
    }
    fprintf(stderr, "\n");
    t = R32(t + 0x2FC);
  }
}
/* the engine's level loader (ViD LoadList): state, time budget and the objects waiting to be destroyed */
static void loader_dump(void) {
  const u32 r13 = 0x806C8DA0u; u32 vid = R32(r13 - 0x652C);
  if (vid < 0x80000000u || vid >= 0x94000000u) { fprintf(stderr, "[loader] no loader\n"); return; }
  u64 boot = (u64)R32(r13 - 0x66F8) << 32 | R32(r13 - 0x66F4), now = hw_timebase();
  union { u64 u; double d; } k; k.u = (u64)R32(r13 - 0x66C8) << 32 | R32(r13 - 0x66C4);
  union { u32 u; float f; } st, th; st.u = R32(r13 - 0x6434); th.u = R32(vid + 0x2058);
  u32 n = R32(vid + 0x2278), stride = R32(vid + 0x2270), list = R32(vid + 0x226C);
  fprintf(stderr, "[loader] vid %08X state %u fn %08X %08X %08X max %u list %u (stride %u) budget %.4f s elapsed %.4f s\n",
          vid, R32(vid + 0x2048), R32(vid + 0x204C), R32(vid + 0x2050), R32(vid + 0x2054), R32(vid + 0x2040), n, stride, (double)th.f,
          (double)((float)((double)(now - boot) * k.d) - st.f));
  fprintf(stderr, "[loader]   action %s; fields 2040..2080:", rt_sym(R32(vid + 0x2054)));
  for (u32 o = 0x2040; o < 0x2080; o += 4) fprintf(stderr, " %08X", R32(vid + o));
  fprintf(stderr, "\n[loader]   fields 2260..2290:");
  for (u32 o = 0x2260; o < 0x2290; o += 4) fprintf(stderr, " %08X", R32(vid + o));
  fprintf(stderr, "\n");
  {  /* objects whose destruction is deferred until all their modifiers agree (sounds still playing, ...) */
    u32 dl = R32(vid + 0x2258), ds = R32(vid + 0x225C), dn = R32(vid + 0x2264);
    fprintf(stderr, "[loader]   deferred destruction: %u objects\n", dn);
    for (u32 i = 0; i < dn && i < 16 && ds && ds < 64; i++) {
      u32 o = R32(dl + i * ds); if (o < 0x80000000u || o >= 0x94000000u) continue;
      u32 nm = R32(o + 0x38), ml = R32(o + 0x2C), ms = R32(o + 0x30);
      fprintf(stderr, "[loader]     obj %08X wait %d modifiers %u:", o, (int)(s16)R16(o + 0x76), nm);
      for (u32 k = 0; k < nm && k < 12 && ms && ms < 64; k++) { u32 m = R32(ml + k * ms); fprintf(stderr, " %08X/vt%08X", m, m >= 0x80000000u && m < 0x94000000u ? R32(m) : 0); }
      fprintf(stderr, "\n");
    }
  }
  int shown = 0;
  for (u32 i = 0; i < n && i < 4096 && stride && stride < 64 && shown < 24; i++) {
    u32 o = R32(list + i * stride);
    if (!o) continue;
    shown++;
    fprintf(stderr, "[loader]   #%u obj %08X wait %d flags %08X type %08X\n", i, o, (int)(s16)R16(o + 0x76), R32(o + 0x18), R32(o + 0x60));
  }
}
static u64 wd_next; static int wd_on = -1, g_budget = -1;
/* debug request from the page (F9, or automatic when the game looks stuck): write the threads to the log */
volatile int g_dbg_req;
#ifdef __wasm__
__attribute__((import_module("rgh"), import_name("dbg_ready"))) void js_dbg_ready(volatile int *flag);
#endif
void rt_poll(CPU *restrict c) {
  { static int init; if (!init) { init = 1;
#ifdef __wasm__
      js_dbg_ready(&g_dbg_req);
#endif
    }
#ifndef __wasm__
    { static int n = -1; static u32 at[32]; static int k; extern u64 g_vi_frames;   /* RGH_DBGAT="vi1,vi2,..": dumps at those VI frames */
      if (n < 0) { n = 0; const char *e = getenv("RGH_DBGAT"); while (e && *e && n < 32) { at[n++] = (u32)strtoul(e, (char **)&e, 10); while (*e == ',') e++; } }
      if (k < n && g_vi_frames >= at[k]) { k++; g_dbg_req = 1; fprintf(stderr, "[dbgat] vi %llu\n", (unsigned long long)g_vi_frames); } }
    { extern u64 g_vi_frames; static const char *rq; static int init; static u64 nx; if (!init) { init = 1; rq = getenv("RGH_DBGREQ"); }   /* dump on request (file appears) */
      if (rq && hw_timebase() >= nx) { nx = hw_timebase() + TB_HZ / 10; FILE *f = fopen(rq, "r"); if (f) { fclose(f); remove(rq); g_dbg_req = 1; fprintf(stderr, "[dbgreq] vi %llu\n", (unsigned long long)g_vi_frames); } } }
#endif
    if (g_dbg_req) { g_dbg_req = 0; fprintf(stderr, "[threads] pc~%08X %s lr=%08X msr=%04X irqs dec=%llu ext=%llu\n", c->pc_hint, rt_sym(c->pc_hint), c->lr, c->msr & 0xFFFF,
                     (unsigned long long)g_irq_count[8], (unsigned long long)g_irq_count[4]); dump_threads(); loader_dump(); { extern void ax_dump_state(void); ax_dump_state(); } } }
  if (g_budget < 0) g_budget = getenv("RGH_BUDGET") ? atoi(getenv("RGH_BUDGET")) : 2000;
  g_poll_budget = g_budget;
  hw_tick();
  if (wd_on < 0) wd_on = getenv("RGH_WATCHDOG") != NULL;
  if (wd_on) {
    u64 tb = hw_timebase();
    if (tb >= wd_next) {
      wd_next = tb + TB_HZ * 2;
      fprintf(stderr, "[wd] pc~%08X %s lr=%08X msr=%04X fiber=%d irqs dec=%llu ext=%llu\n", c->pc_hint, rt_sym(c->pc_hint), c->lr, c->msr & 0xFFFF,
              fiber_id(fiber_self()), (unsigned long long)g_irq_count[8], (unsigned long long)g_irq_count[4]);
      if (getenv("RGH_TDUMP")) dump_threads();
    }
  }
  if (!(c->msr & 0x8000)) return;
  { extern void wpad_poll(CPU *); wpad_poll(c); }
  if (!irq_can_inject()) { static int warned; if (!warned++) fprintf(stderr, "[os] interrupts deferred (all handler fibers busy)\n"); return; }
  if (dec_armed && hw_timebase() >= dec_deadline) { dec_armed = 0; inject(c, 8); return; }
  if (hw_irq_pending()) inject(c, 4);
}

void os_run(CPU *c, u32 entry) {
  g_trace_threads = getenv("RGH_THREADS") ? atoi(getenv("RGH_THREADS")) : 0;
  if (getenv("RGH_THREADS") && !g_trace_threads) g_trace_threads = 1;
  fiber_self();
  call_indirect(c, entry);
  fprintf(stderr, "[os] entry returned\n");
}
