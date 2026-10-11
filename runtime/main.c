/* main.c - memory, DOL loading, boot environment and CPU helpers for the recompiled guest. */
#define _GNU_SOURCE
#include "rt.h"
#include "hw.h"
#include "hostio.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifndef __wasm__
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>
#define PROF_N 65536
static u32 prof_a[PROF_N], prof_c[PROF_N]; static u64 prof_total; volatile int g_prof_tag;
static void on_prof(int sig) { (void)sig; u32 a = g_prof_tag ? 0xFFFF0000u + g_prof_tag : g_cpu.pc_hint; u32 h = (a * 2654435761u) >> 16;
  for (int i = 0; i < 64; i++, h = (h + 1) & (PROF_N - 1)) { if (prof_a[h] == a || !prof_a[h]) { prof_a[h] = a; prof_c[h]++; break; } } prof_total++; }
static void prof_dump(void) {
  if (!prof_total) return;
  fprintf(stderr, "[prof] %llu samples\n", (unsigned long long)prof_total);
  for (int k = 0; k < 40; k++) { int best = -1; for (int i = 0; i < PROF_N; i++) if (prof_c[i] && (best < 0 || prof_c[i] > prof_c[best])) best = i;
    if (best < 0) break; u32 a = prof_a[best];
    fprintf(stderr, "  %5.1f%%  %08X %s\n", 100.0 * prof_c[best] / prof_total, a, a >= 0xFFFF0000u ? (a == 0xFFFF0001u ? "[host: gx decode]" : "[host: other]") : rt_sym(a)); prof_c[best] = 0; }
}
/* RGH_PROF=host: histogram of host instruction addresses (map with nm), after RGH_PROF_DELAY seconds */
#include <ucontext.h>
static u64 hp_a[PROF_N], hp_c[PROF_N]; static u64 hp_total; static int hp_on; static time_t hp_start;
static void on_prof_host(int sig, siginfo_t *si, void *uc) { (void)sig; (void)si;
  if (!hp_on) { if (time(NULL) < hp_start) return; hp_on = 1; }
  u64 a = (u64)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP]; u32 h = (u32)((a * 11400714819323198485ull) >> 48);
  for (int i = 0; i < 64; i++, h = (h + 1) & (PROF_N - 1)) { if (hp_a[h] == a || !hp_a[h]) { hp_a[h] = a; hp_c[h]++; break; } } hp_total++; }
static void hp_dump(void) {
  if (!hp_total) return;
  FILE *f = fopen("build/hostprof.txt", "w"); if (!f) return;
  fprintf(f, "base %llx\n", (unsigned long long)(uintptr_t)&hp_dump);
  for (int i = 0; i < PROF_N; i++) if (hp_c[i]) fprintf(f, "%llx %llu\n", (unsigned long long)hp_a[i], (unsigned long long)hp_c[i]);
  fclose(f); fprintf(stderr, "[prof] %llu host samples -> build/hostprof.txt\n", (unsigned long long)hp_total);
}
static void on_term(int sig) { (void)sig; prof_dump(); hp_dump(); hw_shutdown(); _exit(0); }
#endif

CPU g_cpu;
u8 *g_mem1, *g_mem2, *g_l2;
static u8 g_sink[64];
const char *g_gamedir = "game";
int g_trace;

extern const struct { u32 a; GuestFn f; } g_funcs[];
extern const int g_nfuncs;

/* ---------------------------------------------------------------- memory */
u8 *hp_slow(u32 a) {
  if ((a >> 28) == 0xE && (a & 0x0FFFFFFF) < 0x4000) return g_l2 + (a & 0x3FFF);   /* locked L2 / L1 data cache */
  u32 p = a & 0x1FFFFFFF;
  if (p >= 0x0C000000 && p < 0x0E000000) return NULL;                              /* MMIO */
  static int warned;
  if (warned++ < 20) fprintf(stderr, "[mem] access to unmapped %08X (pc~%08X %s)\n", a, g_cpu.pc_hint, rt_sym(g_cpu.pc_hint));
  memset(g_sink, 0, sizeof g_sink);
  return g_sink;
}

/* ---------------------------------------------------------------- symbols */
static struct Sym { u32 a, e; char *n; } *g_syms; static int g_nsyms;
static void load_syms(const char *path) {
  u32 n = 0; char *buf = host_file_load(path, &n); if (!buf) return;
  buf = realloc(buf, n + 1); buf[n] = 0;
  static int cap;
  for (char *line = buf; line && *line; ) {
    char *nl = strchr(line, '\n'); if (nl) *nl = 0;
    unsigned a, e; int k = 0;
    if (sscanf(line, "%x %x %n", &a, &e, &k) == 2 && k) {
      if (g_nsyms == cap) { cap = cap ? cap * 2 : 32768; g_syms = realloc(g_syms, cap * sizeof *g_syms); }
      g_syms[g_nsyms++] = (struct Sym){a, e, strdup(line + k)};
    }
    line = nl ? nl + 1 : NULL;
  }
  free(buf);
}
int cmp(const void *x, const void *y) { const struct Sym *p = x, *q = y; return p->a < q->a ? -1 : p->a > q->a; }
const char *rt_sym(u32 a) {
  int lo = 0, hi = g_nsyms - 1, best = -1;
  while (lo <= hi) { int m = (lo + hi) / 2; if (g_syms[m].a <= a) { best = m; lo = m + 1; } else hi = m - 1; }
  if (best < 0 || a >= g_syms[best].e) return "?";
  return g_syms[best].n;
}

/* ---------------------------------------------------------------- calls */
/* code of the dynamically linked module (gen_rso/), translated for the address the game loads it at */
typedef struct { u32 a; GuestFn f; } FnEnt;
extern const FnEnt g_rso_funcs[] __attribute__((weak));
extern const int g_rso_nfuncs __attribute__((weak));
extern const u32 g_rso_text_lo __attribute__((weak)), g_rso_text_hi __attribute__((weak)), g_rso_cksum __attribute__((weak));
static int rso_state;   /* 0 unchecked, 1 ok, -1 mismatch */
static GuestFn find_in(const FnEnt *t, int n, u32 a) {
  int lo = 0, hi = n - 1;
  while (lo <= hi) { int m = (lo + hi) / 2; if (t[m].a == a) return t[m].f; if (t[m].a < a) lo = m + 1; else hi = m - 1; }
  return NULL;
}
void rt_dump_trace(void);
static GuestFn find_fn_slow(u32 a);
/* indirect calls (C++ virtual calls) are frequent: a direct-mapped cache in front of the binary search */
static FnEnt fn_cache[8192];
GuestFn find_fn(u32 a) {
  FnEnt *e = &fn_cache[(a >> 2) & 8191];
  if (e->a == a && e->f) return e->f;
  GuestFn f = find_fn_slow(a);
  if (f) { e->f = NULL; e->a = a; e->f = f; }
  return f;
}
static GuestFn find_fn_slow(u32 a) {
  GuestFn f = find_in((const FnEnt *)g_funcs, g_nfuncs, a);
  if (f || !g_rso_funcs || a < g_rso_text_lo || a >= g_rso_text_hi) return f;
  if (rso_state == 0) {
    u32 ck = 0;
    for (u32 x = g_rso_text_lo; x < g_rso_text_hi; x += 4) ck = ck * 31 + R32(x);
    rso_state = ck == g_rso_cksum ? 1 : -1;
    fprintf(stderr, "[rso] module code at %08X-%08X %s\n", g_rso_text_lo, g_rso_text_hi, rso_state > 0 ? "matches the translation" : "DIFFERS from the translation");
  }
  return rso_state > 0 ? find_in(g_rso_funcs, g_rso_nfuncs, a) : NULL;
}
/* The engine's level loader works in time slices of 3 ms (b_LoadList_Throttle, a virtual call). When the
 * main thread does a step itself (state 1, e.g. destroying the previous level's objects) the step only counts
 * as finished if the whole object list was walked inside one slice. Here a walk is slower than on a Wii (the
 * time checks, the audio and GPU work done on the same thread), so with a big level it never fitted in 3 ms
 * and the game stayed on a black screen forever after the level ended. Those steps get 100 ms instead. */
static void loader_throttle(CPU *restrict c, GuestFn f) {
  u32 self = c->gpr[3];
  if (c->gpr[4] == 0 && R32(self + 0x2048) == 1) {
    union { u32 u; float f; } b; b.u = R32(self + 0x2058);
    if (b.f < 0.1f) { u32 old = b.u; b.f = 0.1f; W32(self + 0x2058, b.u); f(c); W32(self + 0x2058, old); return; }
  }
  f(c);
}
void call_indirect(CPU *restrict c, u32 t) {
  GuestFn f = find_fn(t);
  if (t == 0x801714BCu && f) { loader_throttle(c, f); return; }   /* b_LoadList_Throttle__5ViD_SFUl */
  if (!f && t < 0x80000000u) f = find_fn(t | 0x80000000u);    /* real-mode (physical) address */
  if (!f) {
    fprintf(stderr, "[cpu] indirect call to untranslated %08X from ~%08X (%s), lr=%08X\n", t, c->pc_hint, rt_sym(c->pc_hint), c->lr);
    rt_dump_trace();
#ifndef __wasm__
    if (getenv("RGH_DUMP")) {
      FILE *f = fopen("build/mem1.bin", "wb"); fwrite(g_mem1, 1, MEM1_SIZE, f); fclose(f);
      f = fopen("build/mem2.bin", "wb"); fwrite(g_mem2, 1, MEM2_SIZE, f); fclose(f);
      fprintf(stderr, "[cpu] memory dumped to build/mem1.bin, build/mem2.bin\n");
    }
#endif
    hw_shutdown(); exit(3);
  }
  f(c);
}

/* ---------------------------------------------------------------- SPRs, time */
u32 rt_mfspr(CPU *c, int spr) {
  switch (spr) {
    case 22: return os_get_dec();
    case 26: return c->srr0; case 27: return c->srr1;
    case 287: return 0x00087102;               /* PVR: Broadway */
    case 268: return (u32)hw_timebase(); case 269: return (u32)(hw_timebase() >> 32);
    default: return c->spr[spr & 1023];
  }
}
void rt_mtspr(CPU *c, int spr, u32 v) {
  switch (spr) {
    case 22: os_set_dec(v); return;
    case 26: c->srr0 = v; return; case 27: c->srr1 = v; return;
    case 923:   /* DMA_L: locked-cache <-> memory DMA (used by LCLoadBlocks / LCStoreBlocks) */
      if (v & 2) {
        u32 u = c->spr[922];
        u32 mem = u & 0xFFFFFFE0, lc = v & 0xFFFFFFE0;
        u32 n = ((u & 0x1F) << 2) | ((v >> 2) & 3); if (!n) n = 128;
        for (u32 i = 0; i < n * 32; i += 4) {
          if (v & 0x10) W32(lc + i, R32(mem + i)); else W32(mem + i, R32(lc + i));
        }
        v &= ~2u;
      }
      c->spr[923] = v; return;
    default: c->spr[spr & 1023] = v;
  }
}
u32 rt_mftb(CPU *c, int tbu) { (void)c; u64 t = hw_timebase(); return tbu ? (u32)(t >> 32) : (u32)t; }
void rt_mtmsr(CPU *c, u32 v) {
  u32 old = c->msr; c->msr = v;
  if ((v & 0x8000) && !(old & 0x8000)) g_poll_budget = 0;     /* interrupts just enabled: deliver pending ones soon */
}
void rt_sc(CPU *restrict c) { (void)c; }   /* DCFlushRange etc. use sc as a sync: nothing to do */
void rt_trap(CPU *restrict c, u32 a) {
  fprintf(stderr, "[cpu] trap at %08X (%s) lr=%08X r3=%08X\n", a, rt_sym(a), c->lr, c->gpr[3]);
}
void rt_unimpl(CPU *restrict c, u32 a, u32 w) {
  fprintf(stderr, "[cpu] unimplemented instruction %08X at %08X (%s), lr=%08X\n", w, a, rt_sym(a), c->lr);
  hw_shutdown(); exit(4);
}
void rt_dcbz(u32 a) { a &= ~31u; for (int i = 0; i < 32; i += 4) W32(a + i, 0); }
void rt_dcbi(u32 a) {   /* the CPU is about to read memory the GPU may have written (EFB copies): see gxrender.c */
  extern u32 g_wb_pending, g_wb_lo, g_wb_hi; extern void gx_writeback_addr(u32 a);
  if (g_wb_pending) { u32 p = a & 0x1FFFFFFF; if (p >= g_wb_lo && p < g_wb_hi) gx_writeback_addr(p); }
}
void rt_dcbf(u32 a) { (void)a; }
void rt_icbi(u32 a) { (void)a; }
void rt_lswi(CPU *c, int rd, u32 ea, int nb) {
  int r = rd, sh = 24; c->gpr[r] = 0;
  for (int k = 0; k < nb; k++) {
    c->gpr[r] |= R8(ea + k) << sh; sh -= 8;
    if (sh < 0 && k + 1 < nb) { r = (r + 1) & 31; c->gpr[r] = 0; sh = 24; }
  }
}
void rt_stswi(CPU *c, int rs, u32 ea, int nb) {
  int r = rs, sh = 24;
  for (int k = 0; k < nb; k++) {
    W8(ea + k, c->gpr[r] >> sh); sh -= 8;
    if (sh < 0) { r = (r + 1) & 31; sh = 24; }
  }
}

/* ---------------------------------------------------------------- paired-single quantisation */
static const f64 qscale_ld[64] = {
#define S(n) (1.0 / (f64)(1ull << (n)))
#define N(n) ((f64)(1ull << (n)))
  S(0),S(1),S(2),S(3),S(4),S(5),S(6),S(7),S(8),S(9),S(10),S(11),S(12),S(13),S(14),S(15),
  S(16),S(17),S(18),S(19),S(20),S(21),S(22),S(23),S(24),S(25),S(26),S(27),S(28),S(29),S(30),S(31),
  N(32),N(31),N(30),N(29),N(28),N(27),N(26),N(25),N(24),N(23),N(22),N(21),N(20),N(19),N(18),N(17),
  N(16),N(15),N(14),N(13),N(12),N(11),N(10),N(9),N(8),N(7),N(6),N(5),N(4),N(3),N(2),N(1)
};
static f64 q_ld(u32 a, int type, int scale, int *size) {
  f64 s = qscale_ld[scale & 63];
  switch (type) {
    case 4: *size = 1; return (f64)(u8)R8(a) * s;
    case 5: *size = 2; return (f64)(u16)R16(a) * s;
    case 6: *size = 1; return (f64)(s8)R8(a) * s;
    case 7: *size = 2; return (f64)(s16)R16(a) * s;
    default: *size = 4; return (f64)u2f(R32(a));
  }
}
static int q_st(u32 a, f64 v, int type, int scale) {
  f64 s = 1.0 / qscale_ld[scale & 63];
  f64 x = v * s;
  switch (type) {
    case 4: { f64 y = x < 0 ? 0 : x > 255 ? 255 : x; W8(a, (u32)(s32)y); return 1; }
    case 5: { f64 y = x < 0 ? 0 : x > 65535 ? 65535 : x; W16(a, (u32)(s32)y); return 2; }
    case 6: { f64 y = x < -128 ? -128 : x > 127 ? 127 : x; W8(a, (u32)(s32)y); return 1; }
    case 7: { f64 y = x < -32768 ? -32768 : x > 32767 ? 32767 : x; W16(a, (u32)(s32)y); return 2; }
    default: W32(a, f2u((f32)v)); return 4;
  }
}
void rt_psq_l(CPU *c, int fd, u32 ea, int w, int i) {
  u32 g = c->gqr[i]; int type = (g >> 16) & 7, scale = (g >> 24) & 63, sz;
  if ((ea & 0xFFF) <= 0xFF8) {                   /* both values inside one 4 KB page: read memory directly */
    u8 *p = hp(ea);
    if (p) {
      f64 s = qscale_ld[scale & 63];
      switch (type) {
        case 4: c->ps0[fd] = p[0] * s; c->ps1[fd] = w ? 1.0 : p[1] * s; return;
        case 5: c->ps0[fd] = (u16)(p[0] << 8 | p[1]) * s; c->ps1[fd] = w ? 1.0 : (u16)(p[2] << 8 | p[3]) * s; return;
        case 6: c->ps0[fd] = (s8)p[0] * s; c->ps1[fd] = w ? 1.0 : (s8)p[1] * s; return;
        case 7: c->ps0[fd] = (s16)(p[0] << 8 | p[1]) * s; c->ps1[fd] = w ? 1.0 : (s16)(p[2] << 8 | p[3]) * s; return;
        default: { u32 x; memcpy(&x, p, 4); c->ps0[fd] = (f64)u2f(bs32(x));
                   if (w) c->ps1[fd] = 1.0; else { memcpy(&x, p + 4, 4); c->ps1[fd] = (f64)u2f(bs32(x)); } return; }
      }
    }
  }
  f64 v0 = q_ld(ea, type, scale, &sz);
  f64 v1 = w ? 1.0 : q_ld(ea + sz, type, scale, &sz);
  c->ps0[fd] = v0; c->ps1[fd] = v1;
}
void rt_psq_st(CPU *c, int fs, u32 ea, int w, int i) {
  u32 g = c->gqr[i]; int type = g & 7, scale = (g >> 8) & 63;
  if (type < 4 && (ea & 0x1FFFF000u) == 0x0C008000u) {   /* floats to the write-gather pipe (GX vertices) */
    extern void hw_wgpipe_w32(u32 v);
    hw_wgpipe_w32(f2u((f32)c->ps0[fs])); if (!w) hw_wgpipe_w32(f2u((f32)c->ps1[fs])); return;
  }
  if (type < 4 && (ea & 0xFFF) <= 0xFF8) {
    u8 *p = hp(ea);
    if (p && ea < 0xE0000000u) { u32 x = bs32(f2u((f32)c->ps0[fs])); memcpy(p, &x, 4);
             if (!w) { x = bs32(f2u((f32)c->ps1[fs])); memcpy(p + 4, &x, 4); } return; }
  }
  int sz = q_st(ea, c->ps0[fs], type, scale);
  if (!w) q_st(ea + sz, c->ps1[fs], type, scale);
}

/* ---------------------------------------------------------------- boot */
static u8 *read_file(const char *path, size_t *len) { u32 n = 0; u8 *b = host_file_load(path, &n); if (b && len) *len = n; return b; }
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static u32 load_dol(const u8 *d) {
  u32 bss = be32(d + 0xD8), bsz = be32(d + 0xDC);
  for (u32 i = 0; i < bsz; i += 4) W32(bss + i, 0);           /* clear BSS first: .sdata lies inside its range */
  for (int i = 0; i < 18; i++) {
    u32 off = be32(d + i * 4), addr = be32(d + 72 + i * 4), sz = be32(d + 144 + i * 4);
    if (sz) memcpy(hp(addr), d + off, sz);
  }
  return be32(d + 0xE0);
}

static void setup_lowmem(const u8 *boot, const u8 *tmd) {
  memcpy(hp(0x80000000), boot, 0x20);                    /* disc ID, maker, version, magic */
  W32(0x80000020, 0x0D15EA5E); W32(0x80000024, 1);
  W32(0x80000028, MEM1_SIZE); W32(0x8000002C, 0x23);
  W32(0x80000030, 0); W32(0x80000034, 0x817FEC60);
  W32(0x800000CC, 0);                                    /* VI: NTSC */
  W32(0x800000E4, 0x8008F7B8);
  W32(0x800000F0, MEM1_SIZE); W32(0x800000F4, 0x8179B500);
  W32(0x800000F8, 0x0E7BE2C0); W32(0x800000FC, 0x2B73A840);   /* bus 243 MHz, cpu 729 MHz */
  W32(0x800030C0, 0); W32(0x800030C4, 0); W32(0x800030DC, 0); W32(0x800030D8, 0xFFFFFFFF);
  W16(0x800030E6, 0x8201); W32(0x800030F0, 0);
  W32(0x80003100, MEM1_SIZE); W32(0x80003104, MEM1_SIZE); W32(0x8000310C, 0); W32(0x80003110, 0x8179D500);
  W32(0x80003118, MEM2_SIZE); W32(0x8000311C, MEM2_SIZE); W32(0x80003120, 0x93400000);
  W32(0x80003124, 0x90000800); W32(0x80003128, 0x93AE0000); W32(0x80003130, 0x93AE0000); W32(0x80003134, 0x93B00000);
  W32(0x80003138, 0x11);
  u32 ios = tmd ? be32(tmd + 0x188) : 36;               /* IOS the title asks for (low word of system version) */
  W32(0x80003140, (ios << 16) | 0xFFFF); W32(0x80003144, 0x00062507);
  W32(0x80003188, (ios << 16) | 0xFFFF);
  W16(0x8000315E, 0x0113); W32(0x80003158, 0x0000FF16); W32(0x80003160, 0);
  W8(0x8000315C, 0x80); W16(0x800030E0, 0);
  W32(0x80003180, be32(boot)); W32(0x80003184, 0x80000000);
}

static void place_fst(const u8 *boot, const char *partdir) {
  char p[1024]; size_t n;
  snprintf(p, sizeof p, "%s/sys/fst.bin", partdir);
  u8 *fst = read_file(p, &n);
  if (!fst) { fprintf(stderr, "[boot] no fst.bin\n"); return; }
  u32 maxsz = be32(boot + 0x42C) << 2;
  u32 hi = (0x817FFFFF - (u32)n) & ~31u;
  memcpy(hp(hi), fst, n);
  W32(0x80000034, hi); W32(0x80000038, hi); W32(0x8000003C, maxsz > n ? maxsz : (u32)n);
  W32(0x80003110, hi);
  free(fst);
}

int g_poll_budget = 1;

#ifdef RGH_FNHOOK
/* RGH_TRACEFN="addr,addr,...": log every call of those guest functions (native trace build: make -f Makefile.trace) */
int g_fnhook_on = 1;
static u32 fh_a[64]; static int fh_n = -1; static unsigned fh_lines;
void rt_fnhook(CPU *c, u32 a) {
  if (fh_n < 0) { fh_n = 0; const char *e = getenv("RGH_TRACEFN"); while (e && *e && fh_n < 64) { fh_a[fh_n++] = (u32)strtoul(e, (char **)&e, 16); while (*e == ',') e++; } if (!fh_n) g_fnhook_on = 0; }
  for (int i = 0; i < fh_n; i++) if (fh_a[i] == a) {
    { static u32 fa[16], fv[16]; static int nf = -1;   /* RGH_TRACEFILT="addr:r3,...": only calls with that r3 */
      if (nf < 0) { nf = 0; const char *e = getenv("RGH_TRACEFILT"); while (e && *e && nf < 16) { fa[nf] = (u32)strtoul(e, (char **)&e, 16); if (*e == ':') e++; fv[nf++] = (u32)strtoul(e, (char **)&e, 16); while (*e == ',') e++; } }
      int skip = 0; for (int k = 0; k < nf; k++) if (fa[k] == a && c->gpr[3] != fv[k]) skip = 1;
      if (skip) break; }
    extern u64 g_vi_frames;
    if (a == 0x8024F6C0u) { static u32 lastio; u32 io = R32(c->gpr[3] + 0x3C), vt = R32(io);
      if (io != lastio) { lastio = io; fprintf(stderr, "[fn]   u_StreamData wave %08X io %08X vt %08X +18=%s +28=%s\n", c->gpr[3], io, vt, rt_sym(R32(vt + 0x18)), rt_sym(R32(vt + 0x28))); } }
    { static int cnt = -1; static u32 cc[64]; static u64 next;   /* RGH_TRACECOUNT: calls per second instead of every call */
      if (cnt < 0) cnt = getenv("RGH_TRACECOUNT") != NULL;
      if (cnt) { cc[i]++; u64 now = hw_timebase();
        if (now >= next) { extern u64 g_vi_frames; next = now + TB_HZ; fprintf(stderr, "[fn/s] vi %llu", (unsigned long long)g_vi_frames);
          for (int k = 0; k < fh_n; k++) { fprintf(stderr, " %s=%u", rt_sym(fh_a[k]), cc[k]); cc[k] = 0; } fprintf(stderr, "\n"); }
        break; } }
    if (a == 0x80039F4Cu || a == 0x8003A6F4u) {   /* DXT compression / tiling: is the source image empty? */
      u32 src = a == 0x80039F4Cu ? c->gpr[3] : c->gpr[4], n = a == 0x80039F4Cu ? c->gpr[4] * c->gpr[5] * 4 : c->gpr[5] * c->gpr[6] / 2, nz = 0;
      for (u32 k = 0; k < n && k < 0x100000; k++) if (R8(src + k)) nz++;
      fprintf(stderr, "[fn]   source %08X: %u of %u bytes nonzero; first %08X %08X %08X %08X\n", src, nz, n, R32(src), R32(src + 4), R32(src + 0x8000), R32(src + 0x8004));
    }
    if (a == 0x8024F6C0u) {   /* SLib_wave::u_StreamData: which reader object */
      static u32 lastio; u32 io = R32(c->gpr[3] + 0x3C), vt = R32(io);
      if (io != lastio) { lastio = io; fprintf(stderr, "[fn]   u_StreamData wave %08X io %08X vt %08X +18=%s +28=%s\n", c->gpr[3], io, vt, rt_sym(R32(vt + 0x18)), rt_sym(R32(vt + 0x28))); }
    }
    if (a == 0x8009C480u) {   /* LoadList_ActionDestroy: how many objects, how long since the last call */
      static u64 last; u64 now = hw_timebase(); u32 v = c->gpr[3], n = R32(v + 0x2278), lst = R32(v + 0x226C), live = 0;
      for (u32 k = 0; k < n && k < 100000; k++) if (R32(lst + k * 4)) live++;
      fprintf(stderr, "[fn]   destroy: list %u, alive %u, %llu us since the previous call\n", n, live, (unsigned long long)((now - last) * 1000000 / TB_HZ)); last = now;
      if (fh_lines++ > 300) return;
    }
    if (fh_lines++ < 200000) fprintf(stderr, "[fn] vi %llu %s lr=%08X(%s) r3=%08X r4=%08X r5=%08X r6=%08X f1=%g\n", (unsigned long long)g_vi_frames, rt_sym(a), c->lr, rt_sym(c->lr), c->gpr[3], c->gpr[4], c->gpr[5], c->gpr[6], c->ps0[1]);
    break;
  }
}
#endif
u32 g_trace_ring[256]; unsigned g_trace_pos;
void rt_dump_trace(void) {
  fprintf(stderr, "[trace] last function entries (oldest first):\n");
  for (unsigned i = 0; i < 256; i++) { u32 a = g_trace_ring[(g_trace_pos + i) & 255]; if (a) fprintf(stderr, "  %08X %s\n", a, rt_sym(a)); }
}
int main(int argc, char **argv) {
  if (argc > 1) g_gamedir = argv[1];
  setvbuf(stderr, NULL, _IOLBF, 0);
#ifndef __wasm__
  signal(SIGTERM, on_term); signal(SIGINT, on_term);
  if (getenv("RGH_PROF") && !strcmp(getenv("RGH_PROF"), "host")) {
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_prof_host; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigaction(SIGPROF, &sa, NULL);
    hp_start = time(NULL) + (getenv("RGH_PROF_DELAY") ? atoi(getenv("RGH_PROF_DELAY")) : 0);
    struct itimerval it = {{0, 1000}, {0, 1000}}; setitimer(ITIMER_PROF, &it, NULL);
  } else if (getenv("RGH_PROF")) { signal(SIGPROF, on_prof); struct itimerval it = {{0, 1000}, {0, 1000}}; setitimer(ITIMER_PROF, &it, NULL); }
#endif
  g_mem1 = calloc(1, MEM1_SIZE); g_mem2 = calloc(1, MEM2_SIZE); g_l2 = calloc(1, 0x4000);
  char path[1024]; size_t n;
  snprintf(path, sizeof path, "%s/syms.txt", g_gamedir); load_syms(path);
  if (!g_nsyms) { load_syms("gen/syms.txt"); load_syms("gen_rso/syms.txt"); }
  { int sorted = 1; for (int i = 1; i < g_nsyms; i++) if (g_syms[i].a < g_syms[i - 1].a) sorted = 0;
    if (!sorted) { int cmp(const void *x, const void *y); qsort(g_syms, g_nsyms, sizeof *g_syms, cmp); } }
  /* game folder layouts: <game>/sys,files,tmd.bin (an extracted partition, e.g. Dolphin's
   * "Extract Entire Partition") or <game>/part/... ; main.dol is taken from sys/ */
  const char *partdir = getenv("RGH_PART"); static char pd[1024];
  if (!partdir) {
    snprintf(path, sizeof path, "%s/sys/boot.bin", g_gamedir);
    if (host_file_size(path) >= 0) snprintf(pd, sizeof pd, "%s", g_gamedir); else snprintf(pd, sizeof pd, "%s/part", g_gamedir);
    partdir = pd;
  }
  snprintf(path, sizeof path, "%s/sys/main.dol", partdir);
  u8 *dol = read_file(path, &n);
  if (!dol) { snprintf(path, sizeof path, "%s/main.dol", g_gamedir); dol = read_file(path, &n); }
  if (!dol) { fprintf(stderr, "cannot read main.dol (looked in %s/sys and %s)\n", partdir, g_gamedir); return 1; }
  snprintf(path, sizeof path, "%s/sys/boot.bin", partdir); u8 *boot = read_file(path, &n);
  snprintf(path, sizeof path, "%s/tmd.bin", partdir); u8 *tmd = read_file(path, &n);
  if (!boot) { fprintf(stderr, "cannot read %s\n", path); return 1; }
  u32 entry = load_dol(dol);
  setup_lowmem(boot, tmd);
  place_fst(boot, partdir);
  hw_init(partdir);
  CPU *c = &g_cpu;
  c->msr = 0x2032; c->spr[1011] = 0xE0000000;           /* HID2: paired singles, locked cache */
  c->gpr[1] = 0x816FFFF0;
  fprintf(stderr, "[boot] entry %08X (%s), %d functions\n", entry, rt_sym(entry), g_nfuncs);
  os_run(c, entry);
  hw_shutdown();
  return 0;
}
