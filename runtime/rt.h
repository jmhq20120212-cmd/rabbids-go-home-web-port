/* rt.h - runtime interface shared by the recompiled guest code (gen/f*.c files) and the runtime.
 * The guest is the PowerPC 750CL ("Broadway") code of Rabbids Go Home, translated function by
 * function into C by tools/gen.py. This header defines the CPU state, guest memory access and the
 * helpers the generated code calls. Nothing here comes from the game; it is our own code. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>


typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef float f32; typedef double f64;

typedef struct CPU {
  u32 gpr[32];
  f64 ps0[32], ps1[32];
  u32 cr, lr, ctr;
  u32 xer_so, xer_ov, xer_ca, xer_bc;   /* xer_so is stored as bit value 0/1 */
  u32 msr, fpscr;
  u32 gqr[8];
  u32 srr0, srr1;
  u32 spr[1024];
  u32 pc_hint;                           /* last function entry / loop head executed (diagnostics, IRQ resume) */
} CPU;

extern CPU g_cpu;
typedef void (*GuestFn)(CPU *restrict);

/* ---------------------------------------------------------------- memory */
#define MEM1_SIZE 0x01800000u
#define MEM2_SIZE 0x04000000u
extern u8 *g_mem1, *g_mem2, *g_l2;
u8 *hp_slow(u32 a);
u32 mmio_r32(u32 a); u16 mmio_r16(u32 a); u8 mmio_r8(u32 a);
void mmio_w32(u32 a, u32 v); void mmio_w16(u32 a, u16 v); void mmio_w8(u32 a, u8 v);

static inline u8 *hp(u32 a) {
  u32 p = a & 0x1FFFFFFFu;
  if (__builtin_expect(a < 0xE0000000u, 1)) {
    if (p < MEM1_SIZE) return g_mem1 + p;
    if (p - 0x10000000u < MEM2_SIZE) return g_mem2 + (p - 0x10000000u);
  }
  return hp_slow(a);
}
static inline u32 bs32(u32 v) { return __builtin_bswap32(v); }
static inline u16 bs16(u16 v) { return __builtin_bswap16(v); }
static inline u32 R32(u32 a) { u8 *p = hp(a); if (!p) return mmio_r32(a); u32 v; memcpy(&v, p, 4); return bs32(v); }
static inline u32 R16(u32 a) { u8 *p = hp(a); if (!p) return mmio_r16(a); u16 v; memcpy(&v, p, 2); return bs16(v); }
static inline u32 R8(u32 a)  { u8 *p = hp(a); if (!p) return mmio_r8(a); return *p; }
static inline u64 R64(u32 a) { return ((u64)R32(a) << 32) | R32(a + 4); }
static inline void W32(u32 a, u32 v) { u8 *p = hp(a); if (!p) { mmio_w32(a, v); return; } v = bs32(v); memcpy(p, &v, 4); }
static inline void W16(u32 a, u32 v) { u8 *p = hp(a); if (!p) { mmio_w16(a, (u16)v); return; } u16 x = bs16((u16)v); memcpy(p, &x, 2); }
static inline void W8(u32 a, u32 v)  { u8 *p = hp(a); if (!p) { mmio_w8(a, (u8)v); return; } *p = (u8)v; }
static inline void W64(u32 a, u64 v) { W32(a, (u32)(v >> 32)); W32(a + 4, (u32)v); }
static inline f32 u2f(u32 v) { f32 f; memcpy(&f, &v, 4); return f; }
static inline u32 f2u(f32 f) { u32 v; memcpy(&v, &f, 4); return v; }
static inline f64 u2d(u64 v) { f64 f; memcpy(&f, &v, 8); return f; }
static inline u64 d2u(f64 f) { u64 v; memcpy(&v, &f, 8); return v; }
static inline f64 RFS(u32 a) { return (f64)u2f(R32(a)); }
static inline f64 RFD(u32 a) { return u2d(R64(a)); }
static inline void WFS(u32 a, f64 v) { W32(a, f2u((f32)v)); }
static inline void WFD(u32 a, f64 v) { W64(a, d2u(v)); }

/* ---------------------------------------------------------------- integer helpers */
static inline u32 rotl(u32 v, u32 n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }
static inline u32 cntlz(u32 v) { return v ? (u32)__builtin_clz(v) : 32; }
#define CRF_SET(c, f, v) ((c)->cr = ((c)->cr & ~(0xF0000000u >> ((f) * 4))) | ((u32)(v) << (28 - (f) * 4)))
static inline void cr0(CPU *c, u32 r) { s32 v = (s32)r; CRF_SET(c, 0, (v < 0 ? 8 : v > 0 ? 4 : 2) | c->xer_so); }
static inline void cmps(CPU *c, int f, s32 a, s32 b) { CRF_SET(c, f, (a < b ? 8 : a > b ? 4 : 2) | c->xer_so); }
static inline void cmpu(CPU *c, int f, u32 a, u32 b) { CRF_SET(c, f, (a < b ? 8 : a > b ? 4 : 2) | c->xer_so); }
#define CRB(c, i) (((c)->cr >> (31 - (i))) & 1u)
static inline void crset(CPU *c, int i, u32 v) { u32 m = 1u << (31 - i); c->cr = v ? (c->cr | m) : (c->cr & ~m); }
static inline void fcmp(CPU *c, int f, f64 a, f64 b) {
  u32 r = (a < b) ? 8 : (a > b) ? 4 : (a == b) ? 2 : 1;
  CRF_SET(c, f, r); c->fpscr = (c->fpscr & ~0x1F000u) | (r << 12);
}
static inline u32 addc(CPU *c, u32 a, u32 b, u32 ci) { u64 s = (u64)a + b + ci; c->xer_ca = (u32)(s >> 32); return (u32)s; }
static inline u32 addo(CPU *c, u32 a, u32 b, u32 r) { u32 ov = ((~(a ^ b)) & (a ^ r)) >> 31; c->xer_ov = ov; c->xer_so |= ov; return r; }
static inline s32 fctiw_r(f64 v, int mode) {
  if (v != v) return (s32)0x80000000;
  f64 r;
  switch (mode) { case 0: r = nearbyint(v); break; case 1: r = trunc(v); break; case 2: r = ceil(v); break; default: r = floor(v); }
  if (r >= 2147483647.0) return 0x7FFFFFFF;
  if (r <= -2147483648.0) return (s32)0x80000000;
  return (s32)r;
}
static inline f64 rnd_s(f64 v) { return (f64)(f32)v; }
static inline f64 fres(f64 v) { return rnd_s(1.0 / v); }
static inline f64 frsqrte(f64 v) { return 1.0 / sqrt(v); }
static inline f64 fsel(f64 a, f64 b, f64 cc) { return (a >= 0.0) ? b : cc; }
static inline void ps_set_bits(CPU *c, int n, u64 bits) { c->ps0[n] = u2d(bits); }
static inline u64 ps_bits(CPU *c, int n) { return d2u(c->ps0[n]); }

/* ---------------------------------------------------------------- runtime services */
void call_indirect(CPU *restrict c, u32 target);     /* call translated function at guest address */
GuestFn find_fn(u32 addr);
u32 rt_mfspr(CPU *c, int spr); void rt_mtspr(CPU *c, int spr, u32 v);
u32 rt_mftb(CPU *c, int tbu);
void rt_mtmsr(CPU *c, u32 v);
void rt_rfi(CPU *restrict c);
void rt_sc(CPU *restrict c);
void rt_trap(CPU *restrict c, u32 addr);
void rt_unimpl(CPU *restrict c, u32 addr, u32 word);
void rt_dcbz(u32 a); void rt_dcbi(u32 a); void rt_dcbf(u32 a); void rt_icbi(u32 a);
void rt_psq_l(CPU *c, int fd, u32 ea, int w, int i);
void rt_psq_st(CPU *c, int fs, u32 ea, int w, int i);
const char *rt_sym(u32 a);


extern int g_poll_budget;
void rt_poll(CPU *restrict c);
#ifdef RGH_FNHOOK
extern int g_fnhook_on; void rt_fnhook(CPU *c, u32 a);
#define TRACE_ENTER(a) do { if (g_fnhook_on) rt_fnhook(c, (a)); } while (0)
#elif defined(RGH_TRACE)
extern u32 g_trace_ring[256]; extern unsigned g_trace_pos;
#define TRACE_ENTER(a) (g_trace_ring[g_trace_pos++ & 255] = (a))
#else
#define TRACE_ENTER(a) ((void)0)
#endif
#define FENTER(a) do { c->pc_hint = (a); TRACE_ENTER(a); if (--g_poll_budget < 0) rt_poll(c); } while (0)
#define POLL(a) do { if (--g_poll_budget < 0) { c->pc_hint = (a); rt_poll(c); } } while (0)

/* HLE replacement declaration helper (runtime/hle.c) */
#define HLE(addr) void hle_##addr(CPU *restrict c)
void rt_lswi(CPU *c, int rd, u32 ea, int nb);
void rt_stswi(CPU *c, int rs, u32 ea, int nb);
