#include "hostio.h"
#include <stdlib.h>
#include <string.h>
#ifdef __wasm__
__attribute__((import_module("rgh"), import_name("file_size"))) int64_t js_file_size(const char *path, int len);
__attribute__((import_module("rgh"), import_name("file_read"))) int js_file_read(const char *path, int len, uint64_t off, void *dst, uint32_t n);
int64_t host_file_size(const char *path) { return js_file_size(path, (int)strlen(path)); }
int host_file_read(const char *path, uint64_t off, void *dst, uint32_t len) { return js_file_read(path, (int)strlen(path), off, dst, len); }
#else
#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
/* small cache of open descriptors (disc parts are read constantly) */
static struct { char path[512]; int fd; unsigned use; } cache[32]; static unsigned tick;
static int get_fd(const char *path) {
  for (int i = 0; i < 32; i++) if (cache[i].fd > 0 && !strcmp(cache[i].path, path)) { cache[i].use = ++tick; return cache[i].fd; }
  int fd = open(path, O_RDONLY); if (fd < 0) return -1;
  int v = 0; for (int i = 1; i < 32; i++) if (cache[i].use < cache[v].use) v = i;
  if (cache[v].fd > 0) close(cache[v].fd);
  strncpy(cache[v].path, path, sizeof cache[v].path - 1); cache[v].fd = fd; cache[v].use = ++tick;
  return fd;
}
int64_t host_file_size(const char *path) { struct stat st; return stat(path, &st) ? -1 : (int64_t)st.st_size; }
int host_file_read(const char *path, uint64_t off, void *dst, uint32_t len) {
  int fd = get_fd(path); if (fd < 0) return -1;
  ssize_t n = pread(fd, dst, len, (off_t)off); return n < 0 ? -1 : (int)n;
}
#endif
void *host_file_load(const char *path, uint32_t *len) {
  int64_t s = host_file_size(path); if (s < 0) return NULL;
  void *b = malloc(s ? (size_t)s : 1);
  if (host_file_read(path, 0, b, (uint32_t)s) != (int)s) { free(b); return NULL; }
  if (len) *len = (uint32_t)s; return b;
}
