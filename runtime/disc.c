/* disc.c - the game partition as a flat, decrypted address space, served from the files the user
 * extracted from their own disc (sys/ files, main.dol, files/...). Large files may be split into
 * equally sized pieces named NAME.part00, NAME.part01, ... */
#define _GNU_SOURCE
#include "hw.h"
#include "hostio.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct { u64 off, len; char *path; u64 partsz; int nparts; } Region;
static Region *rg; static int nrg, caprg;
static int g_disc_log;

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static int name_ok(const char *n) { return n[0] && !strstr(n, "..") && !strchr(n, '\\') && !strchr(n, ':'); }

static void add(u64 off, u64 len, const char *path) {
  if (!len) return;
  if (nrg == caprg) { caprg = caprg ? caprg * 2 : 256; rg = realloc(rg, caprg * sizeof *rg); }
  Region *r = &rg[nrg++]; memset(r, 0, sizeof *r);
  r->off = off; r->len = len; r->path = strdup(path);
  if (host_file_size(path) >= 0) { r->nparts = 0; return; }
  char p[2048]; snprintf(p, sizeof p, "%s.part00", path);
  int64_t ps = host_file_size(p);
  if (ps > 0) { r->partsz = (u64)ps; r->nparts = (int)((len + r->partsz - 1) / r->partsz); }
  else { r->nparts = -1; if (getenv("RGH_DISC")) fprintf(stderr, "[disc] missing %s\n", path); }
}
static int cmp(const void *a, const void *b) { const Region *x = a, *y = b; return x->off < y->off ? -1 : x->off > y->off; }

int disc_open(const char *pd) {
  g_disc_log = getenv("RGH_DISC") != NULL;
  char p[2048], q[2048];
  snprintf(p, sizeof p, "%s/sys/boot.bin", pd);
  u8 boot[0x440]; if (host_file_read(p, 0, boot, sizeof boot) != sizeof boot) { fprintf(stderr, "[disc] no %s\n", p); return 0; }
  u64 dol = (u64)be32(boot + 0x420) << 2, fsto = (u64)be32(boot + 0x424) << 2, fsts = (u64)be32(boot + 0x428) << 2;
  int64_t sz;
  add(0, 0x440, p);
  snprintf(q, sizeof q, "%s/sys/bi2.bin", pd); add(0x440, 0x2000, q);
  snprintf(q, sizeof q, "%s/sys/apploader.img", pd); if ((sz = host_file_size(q)) >= 0) add(0x2440, (u64)sz, q);
  snprintf(q, sizeof q, "%s/sys/main.dol", pd); if ((sz = host_file_size(q)) >= 0) add(dol, (u64)sz, q);
  snprintf(q, sizeof q, "%s/sys/fst.bin", pd); add(fsto, fsts, q);
  u32 got32 = 0; u8 *fst = host_file_load(q, &got32); if (!fst) return 0; size_t got = got32;
  u32 n = be32(fst + 8);
  if ((u64)n * 12 > got) { free(fst); return 0; }
  const char *strs = (const char *)fst + n * 12;
  /* walk directories: stack of (end index, path) */
  struct { u32 end; char path[1024]; } stk[64]; int sp = 0;
  stk[0].end = n; snprintf(stk[0].path, sizeof stk[0].path, "%s/files", pd);
  for (u32 i = 1; i < n; i++) {
    while (sp > 0 && i >= stk[sp].end) sp--;
    const u8 *e = fst + i * 12;
    const char *name = strs + (be32(e) & 0xFFFFFF);
    if (!name_ok(name)) continue;
    if (e[0]) {   /* directory */
      if (sp < 63) { sp++; stk[sp].end = be32(e + 8); snprintf(stk[sp].path, sizeof stk[sp].path, "%s/%s", stk[sp - 1].path, name); }
    } else {
      char fp[2048]; snprintf(fp, sizeof fp, "%s/%s", stk[sp].path, name);
      add((u64)be32(e + 4) << 2, be32(e + 8), fp);
    }
  }
  free(fst);
  qsort(rg, nrg, sizeof *rg, cmp);
  fprintf(stderr, "[disc] %d regions from %s\n", nrg, pd);
  return 1;
}

static int region_read(Region *r, u64 off, u8 *dst, u32 len) {
  if (r->nparts < 0) return 0;
  while (len) {
    int part = r->nparts ? (int)(off / r->partsz) : 0;
    u64 po = r->nparts ? off % r->partsz : off;
    u32 chunk = len;
    if (r->nparts && po + chunk > r->partsz) chunk = (u32)(r->partsz - po);
    char p[2048];
    if (r->nparts) snprintf(p, sizeof p, "%s.part%02d", r->path, part); else snprintf(p, sizeof p, "%s", r->path);
    int g = host_file_read(p, po, dst, chunk);
    if (g < 0) { fprintf(stderr, "[disc] cannot read %s\n", p); return 0; }
    if ((u32)g < chunk) memset(dst + g, 0, chunk - g);
    dst += chunk; off += chunk; len -= chunk;
  }
  return 1;
}

int disc_read(u64 off, u32 len, u8 *dst) {
  if (g_disc_log) fprintf(stderr, "[disc] read %09llX +%X\n", (unsigned long long)off, len);
  memset(dst, 0, len);
  int ok = 1;
  int lo = 0, hi = nrg - 1, first = nrg;
  while (lo <= hi) { int m = (lo + hi) / 2; if (rg[m].off + rg[m].len > off) { first = m; hi = m - 1; } else lo = m + 1; }
  for (int i = first; i < nrg && rg[i].off < off + len; i++) {
    Region *r = &rg[i];
    u64 a = r->off > off ? r->off : off, b = (r->off + r->len) < off + len ? r->off + r->len : off + len;
    if (a >= b) continue;
    if (!region_read(r, a - r->off, dst + (a - off), (u32)(b - a))) ok = 0;
  }
  return ok;
}
