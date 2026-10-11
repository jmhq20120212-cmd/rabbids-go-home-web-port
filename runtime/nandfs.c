/* nandfs.c - the console's NAND file system (SYSCONF, setting.txt, save data) kept in host memory.
 * Native builds load/save it from a host folder; the browser build persists it from JavaScript
 * (nandfs_serialize / nandfs_deserialize). Paths are absolute NAND paths ("/shared2/sys/SYSCONF"). */
#define _GNU_SOURCE
#include "nandfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef __wasm__
#include <dirent.h>
#include <sys/stat.h>
#endif

typedef struct Node { char *path; int dir; u8 *data; u32 size, cap; } Node;
static Node *nodes; static int nn, ncap;
static char g_host[1024];
int g_nand_dirty;

/* /tmp is scratch space (the console empties it at boot; this game keeps a big data cache there): changes
 * to it are not persisted, so they never trigger saving the NAND image */
static int is_tmp(const char *p) { return !strncmp(p, "/tmp/", 5) || !strcmp(p, "/tmp"); }
static void mark(const char *p) { if (!is_tmp(p)) g_nand_dirty = 1; }
static int find(const char *p) { for (int i = 0; i < nn; i++) if (!strcmp(nodes[i].path, p)) return i; return -1; }
static int parent_exists(const char *p) {
  const char *s = strrchr(p, '/'); if (!s || s == p) return 1;
  char q[256]; size_t l = (size_t)(s - p); if (l >= sizeof q) return 0;
  memcpy(q, p, l); q[l] = 0; int i = find(q); return i >= 0 && nodes[i].dir;
}
static Node *add(const char *p, int dir) {
  if (nn == ncap) { ncap = ncap ? ncap * 2 : 64; nodes = realloc(nodes, ncap * sizeof *nodes); }
  Node *n = &nodes[nn++]; memset(n, 0, sizeof *n); n->path = strdup(p); n->dir = dir; return n;
}
int nand_path_ok(const char *p) { return p[0] == '/' && strlen(p) < 200 && !strstr(p, "..") && !strchr(p, '\\') && !strchr(p, ':'); }

int nand_stat(const char *p, int *is_dir, u32 *size) {
  if (!strcmp(p, "/")) { if (is_dir) *is_dir = 1; if (size) *size = 0; return 0; }
  int i = find(p); if (i < 0) return -1;
  if (is_dir) *is_dir = nodes[i].dir; if (size) *size = nodes[i].size; return 0;
}
int nand_mkdir(const char *p) { if (find(p) >= 0) return -2; if (!parent_exists(p)) return -1; add(p, 1); mark(p); return 0; }
int nand_create(const char *p) { if (find(p) >= 0) return -2; if (!parent_exists(p)) return -1; add(p, 0); mark(p); return 0; }
int nand_delete(const char *p) {
  int i = find(p); if (i < 0) return -1;
  size_t l = strlen(p);
  for (int k = nn - 1; k >= 0; k--)
    if (k == i || (!strncmp(nodes[k].path, p, l) && nodes[k].path[l] == '/')) { free(nodes[k].path); free(nodes[k].data); nodes[k] = nodes[--nn]; }
  mark(p); return 0;
}
int nand_rename(const char *a, const char *b) {
  int i = find(a); if (i < 0) return -1;
  int j = find(b); if (j >= 0 && j != i) { nand_delete(b); i = find(a); }
  size_t la = strlen(a);
  for (int k = 0; k < nn; k++)
    if (k == i || (!strncmp(nodes[k].path, a, la) && nodes[k].path[la] == '/')) {
      char q[512]; snprintf(q, sizeof q, "%s%s", b, nodes[k].path + la); free(nodes[k].path); nodes[k].path = strdup(q);
    }
  mark(a); mark(b); return 0;
}
int nand_readdir(const char *p, int idx, char *name, int max) {
  size_t l = strlen(p); int n = 0;
  for (int k = 0; k < nn; k++) {
    const char *q = nodes[k].path;
    if (strncmp(q, p, l) || (l > 1 && q[l] != '/') ) continue;
    const char *rest = q + (l > 1 ? l + 1 : 1);
    if (!*rest || strchr(rest, '/')) continue;
    if (n++ == idx) { snprintf(name, max, "%s", rest); return 0; }
  }
  return -1;
}
int nand_read(const char *p, u32 off, u8 *dst, u32 len) {
  int i = find(p); if (i < 0 || nodes[i].dir) return -1;
  if (off >= nodes[i].size) return 0;
  if (len > nodes[i].size - off) len = nodes[i].size - off;
  memcpy(dst, nodes[i].data + off, len); return (int)len;
}
int nand_write(const char *p, u32 off, const u8 *src, u32 len) {
  int i = find(p); if (i < 0 || nodes[i].dir) return -1;
  Node *n = &nodes[i];
  if (off + len > n->cap) { n->cap = (off + len) * 2 + 64; n->data = realloc(n->data, n->cap); }
  if (off > n->size) memset(n->data + n->size, 0, off - n->size);
  memcpy(n->data + off, src, len);
  if (off + len > n->size) n->size = off + len;
  mark(p); return (int)len;
}
int nand_put(const char *p, const u8 *src, u32 len) {     /* create parents + whole file */
  char q[512]; snprintf(q, sizeof q, "%s", p);
  for (char *s = q + 1; *s; s++) if (*s == '/') { *s = 0; if (find(q) < 0) add(q, 1); *s = '/'; }
  int i = find(p); if (i < 0) add(p, 0); else nodes[i].size = 0;
  return nand_write(p, 0, src, len);
}
void nand_mkdirs(const char *p) {
  char q[512]; snprintf(q, sizeof q, "%s/", p);
  for (char *s = q + 1; *s; s++) if (*s == '/') { *s = 0; if (find(q) < 0) add(q, 1); *s = '/'; }
}

/* the console empties /tmp at boot */
static void clear_tmp(void) {
  for (int k = nn - 1; k >= 0; k--) if (is_tmp(nodes[k].path) && strcmp(nodes[k].path, "/tmp")) { free(nodes[k].path); free(nodes[k].data); nodes[k] = nodes[--nn]; }
}
/* ---- serialization: repeated { u8 dir; u16 pathlen; path; u32 size; data } */
u32 nandfs_serialize(u8 **out) {
  u32 sz = 0;
  for (int k = 0; k < nn; k++) if (!is_tmp(nodes[k].path)) sz += 1 + 2 + (u32)strlen(nodes[k].path) + 4 + nodes[k].size;
  u8 *b = malloc(sz ? sz : 1), *p = b;
  for (int k = 0; k < nn; k++) {
    if (is_tmp(nodes[k].path)) continue;
    u16 l = (u16)strlen(nodes[k].path);
    *p++ = (u8)nodes[k].dir; memcpy(p, &l, 2); p += 2; memcpy(p, nodes[k].path, l); p += l;
    memcpy(p, &nodes[k].size, 4); p += 4; if (nodes[k].size) memcpy(p, nodes[k].data, nodes[k].size); p += nodes[k].size;
  }
  *out = b; return sz;
}
void nandfs_deserialize(const u8 *b, u32 sz) {
  const u8 *p = b, *e = b + sz;
  while (p + 7 <= e) {
    int dir = *p++; u16 l; memcpy(&l, p, 2); p += 2;
    if (p + l + 4 > e) break;
    char path[512]; memcpy(path, p, l < 511 ? l : 511); path[l < 511 ? l : 511] = 0; p += l;
    u32 s; memcpy(&s, p, 4); p += 4; if (p + s > e) break;
    if (dir) nand_mkdirs(path); else nand_put(path, p, s);
    p += s;
  }
  g_nand_dirty = 0;
}

#ifndef __wasm__
static void load_dir(const char *host, const char *nandp) {
  DIR *d = opendir(host); if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    char h[1200], np[600]; snprintf(h, sizeof h, "%s/%s", host, e->d_name); snprintf(np, sizeof np, "%s/%s", nandp, e->d_name);
    struct stat st; if (stat(h, &st)) continue;
    if (S_ISDIR(st.st_mode)) { nand_mkdirs(np); load_dir(h, np); }
    else { FILE *f = fopen(h, "rb"); if (!f) continue; u8 *b = malloc(st.st_size + 1); size_t n = fread(b, 1, st.st_size, f); fclose(f); nand_put(np, b, (u32)n); free(b); }
  }
  closedir(d);
}
void nandfs_load_host(const char *dir) { snprintf(g_host, sizeof g_host, "%s", dir); load_dir(dir, ""); clear_tmp(); g_nand_dirty = 0; }
void nandfs_save_host(void) {
  if (!g_host[0] || !g_nand_dirty) return;
  for (int k = 0; k < nn; k++) {
    if (is_tmp(nodes[k].path)) continue;
    char h[1200]; snprintf(h, sizeof h, "%s%s", g_host, nodes[k].path);
    if (nodes[k].dir) { mkdir(h, 0755); continue; }
    FILE *f = fopen(h, "wb"); if (f) { fwrite(nodes[k].data, 1, nodes[k].size, f); fclose(f); }
  }
  g_nand_dirty = 0;
}
#else
/* browser: the NAND image lives in IndexedDB on the page side */
__attribute__((import_module("rgh"), import_name("nand_size"))) u32 js_nand_size(void);
__attribute__((import_module("rgh"), import_name("nand_read"))) void js_nand_read(void *dst);
__attribute__((import_module("rgh"), import_name("nand_save"))) void js_nand_save(const void *src, u32 len);
void nandfs_load_host(const char *dir) {
  (void)dir; u32 n = js_nand_size();
  if (n) { u8 *b = malloc(n); js_nand_read(b); nandfs_deserialize(b, n); free(b); }
  clear_tmp();
  g_nand_dirty = 0;
}
void nandfs_save_host(void) {
  if (!g_nand_dirty) return;
  u8 *b; u32 n = nandfs_serialize(&b); js_nand_save(b, n); free(b);
  g_nand_dirty = 0;
}
#endif
