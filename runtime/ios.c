/* ios.c - Hollywood IPC registers and a high-level replacement for the IOS services the game uses
 * (/dev/fs + NAND files, /dev/es, /dev/di, /dev/stm, network stubs). The guest's own IPC driver
 * (recompiled SDK code) talks to this exactly as it would to the Starlet coprocessor.
 * Nothing is ever connected to the Internet: network services report "no connection". */
#define _GNU_SOURCE
#include "hw.h"
#include "nandfs.h"
#include "hostio.h"
#include <stdio.h>
#include <stdlib.h>

#define IOS_OPEN 1
#define IOS_CLOSE 2
#define IOS_READ 3
#define IOS_WRITE 4
#define IOS_SEEK 5
#define IOS_IOCTL 6
#define IOS_IOCTLV 7
#define IOS_REPLY 8
#define PENDING 0x7FFFFFFF

#define FS_EINVAL -101
#define FS_EACCESS -102
#define FS_EEXIST -105
#define FS_ENOENT -106

static int g_log;
static char g_nand[1024];
static u64 g_title_id = 0x0001000052475745ull;   /* replaced from the ticket/tmd when present */

/* ---------------------------------------------------------------- IPC registers */
static u32 ppc_msg, arm_msg, irq_flag, irq_mask;
static int X1, X2, Y1, Y2, IY1, IY2;
static u32 req_q[64]; static int req_n;
static u32 rep_q[256]; static int rep_n;

static void execute(u32 r);
static void update(void) {
  if (Y1 || Y2) return;
  if (req_n) {
    u32 r = req_q[0]; memmove(req_q, req_q + 1, --req_n * 4);
    arm_msg = r; Y2 = 1;                     /* acknowledge */
    execute(r);
    return;
  }
  if (rep_n) {
    arm_msg = rep_q[0]; memmove(rep_q, rep_q + 1, --rep_n * 4);
    Y1 = 1;
  }
}
int ios_irq(void) {
  if ((Y1 && IY1) || (Y2 && IY2)) irq_flag |= 0x40000000;
  return (irq_flag & irq_mask) != 0;
}
u32 ios_read(u32 off) {
  switch (off) {
    case 0x0: return ppc_msg;
    case 0x4: return (u32)(IY2 << 5 | IY1 << 4 | X2 << 3 | Y1 << 2 | Y2 << 1 | X1);
    case 0x8: return arm_msg;
    case 0x30: return irq_flag;
    case 0x34: return irq_mask;
  }
  return 0;
}
void ios_write(u32 off, u32 v) {
  switch (off) {
    case 0x0: ppc_msg = v; break;
    case 0x4:
      X1 = v & 1; X2 = (v >> 3) & 1;
      if (v & 4) Y1 = 0;
      if (v & 2) Y2 = 0;
      IY1 = (v >> 4) & 1; IY2 = (v >> 5) & 1;
      if (X1) { if (req_n < 64) req_q[req_n++] = ppc_msg; X1 = 0; }
      update();
      break;
    case 0x30: irq_flag &= ~v; break;
    case 0x34: irq_mask = v; break;
  }
  pi_set(PI_IPC, ios_irq());
}
static void reply(u32 r, s32 result) {
  W32(r + 4, (u32)result);
  u32 cmd = R32(r);
  W32(r + 8, cmd);
  W32(r, IOS_REPLY);
  if (rep_n < 256) rep_q[rep_n++] = r;
  if (g_log) fprintf(stderr, "[ios]   -> %d\n", result);
}
void ios_tick(void) { update(); }

/* ---------------------------------------------------------------- helpers */
static u32 gaddr(u32 a) { return a ? (a | 0x80000000u) : 0; }      /* IOS gets physical addresses */
static void gstr(u32 a, char *out, int max) { int i = 0; for (; i < max - 1; i++) { out[i] = (char)R8(gaddr(a) + i); if (!out[i]) break; } out[i] = 0; }
static void gzero(u32 a, u32 len) { if (a) for (u32 i = 0; i < len; i++) W8(gaddr(a) + i, 0); }
typedef struct { u32 no, nin, nio, vec; } Vec;
static u32 iov_addr(const Vec *v, int i) { return R32(gaddr(v->vec) + 8 * i); }
static u32 iov_len(const Vec *v, int i) { return R32(gaddr(v->vec) + 8 * i + 4); }

/* ---------------------------------------------------------------- devices */
typedef struct Dev Dev;
struct Dev {
  const char *name;
  int prefix;
  s32 (*open)(int fd, const char *path, u32 mode);
  void (*close)(int fd);
  s32 (*rw)(int fd, int write, u32 buf, u32 len);
  s32 (*seek)(int fd, s32 off, int whence);
  s32 (*ioctl)(int fd, u32 r, u32 no, u32 in, u32 inlen, u32 out, u32 outlen);
  s32 (*ioctlv)(int fd, u32 r, const Vec *v);
};
static struct { const Dev *dev; int hostfd; char path[128]; } fds[32];

/* /dev/stm */
static u32 stm_hook_req;
static s32 stm_ioctl(int fd, u32 r, u32 no, u32 in, u32 il, u32 out, u32 ol) {
  (void)fd; (void)in; (void)il; (void)out; (void)ol;
  if (no == 0x1000) { stm_hook_req = r; return PENDING; }     /* event hook: completes on reset/power events */
  if (no == 0x3002) { if (stm_hook_req) { reply(stm_hook_req, 0); stm_hook_req = 0; } return 0; }
  return 0;
}
static const Dev dev_stm_imm = {"/dev/stm/immediate", 0, NULL, NULL, NULL, NULL, stm_ioctl, NULL};
static const Dev dev_stm_ev = {"/dev/stm/eventhook", 0, NULL, NULL, NULL, NULL, stm_ioctl, NULL};

/* /dev/fs and NAND files (nandfs.c) */
static s32 fs_ioctl(int fd, u32 r, u32 no, u32 in, u32 il, u32 out, u32 ol) {
  (void)fd; (void)r; (void)il;
  char p[80]; int isdir;
  switch (no) {
    case 0x02: gzero(out, ol); W32(gaddr(out), 0x4000); W32(gaddr(out) + 4, 0x5DEC); W32(gaddr(out) + 8, 0x1000); return 0;
    case 0x03: case 0x09: {  /* CreateDir / CreateFile */
      gstr(in + 6, p, 65); if (!nand_path_ok(p)) return FS_EINVAL;
      int e = no == 0x03 ? nand_mkdir(p) : nand_create(p);
      if (!e) nandfs_save_host();
      return e == -2 ? FS_EEXIST : e < 0 ? FS_ENOENT : 0;
    }
    case 0x05: return 0;    /* SetAttr */
    case 0x06:              /* GetAttr */
      gstr(in, p, 65); if (!nand_path_ok(p)) return FS_EINVAL;
      if (nand_stat(p, &isdir, NULL)) return FS_ENOENT;
      gzero(out, ol);
      if (ol >= 0x4C) { W32(gaddr(out), 0x1000); W16(gaddr(out) + 4, 0x3031); for (int i = 0; i < 64 && p[i]; i++) W8(gaddr(out) + 6 + i, p[i]);
        W8(gaddr(out) + 0x46, 3); W8(gaddr(out) + 0x47, 3); W8(gaddr(out) + 0x48, 3); W8(gaddr(out) + 0x49, 0); }
      return 0;
    case 0x07:              /* Delete */
      gstr(in, p, 65); if (!nand_path_ok(p)) return FS_EINVAL;
      if (nand_delete(p)) return FS_ENOENT;
      nandfs_save_host(); return 0;
    case 0x08: {            /* Rename */
      char q[80];
      gstr(in, p, 65); gstr(in + 64, q, 65);
      if (!nand_path_ok(p) || !nand_path_ok(q)) return FS_EINVAL;
      if (nand_rename(p, q)) return FS_ENOENT;
      nandfs_save_host(); return 0;
    }
    case 0x01: return 0;
  }
  if (g_log) fprintf(stderr, "[ios] fs ioctl %X unhandled\n", no);
  return 0;
}
static s32 fs_ioctlv(int fd, u32 r, const Vec *v) {
  (void)fd; (void)r;
  char p[80]; int isdir;
  if (v->no == 0x04) {      /* ReadDir */
    gstr(iov_addr(v, 0), p, 65); if (!nand_path_ok(p)) return FS_EINVAL;
    if (nand_stat(p, &isdir, NULL)) return FS_ENOENT;
    if (!isdir) return FS_EINVAL;
    u32 count = 0, maxn = v->nin > 1 ? R32(gaddr(iov_addr(v, 1))) : 0, pos = 0;
    u32 names = v->nin > 1 ? iov_addr(v, 2) : 0;
    char nm[64];
    for (int i = 0; !nand_readdir(p, i, nm, sizeof nm); i++) {
      if (names) { if (count >= maxn) break; size_t l = strlen(nm); if (l > 12) l = 12;
        for (size_t k = 0; k < l; k++) W8(gaddr(names) + pos + k, nm[k]); W8(gaddr(names) + pos + l, 0); pos += l + 1; }
      count++;
    }
    W32(gaddr(names ? iov_addr(v, 3) : iov_addr(v, 1)), count);
    return 0;
  }
  if (v->no == 0x0C) {      /* GetUsage */
    W32(gaddr(iov_addr(v, 1)), 0); W32(gaddr(iov_addr(v, 2)), 0);
    return 0;
  }
  return 0;
}
static const Dev dev_fs = {"/dev/fs", 0, NULL, NULL, NULL, NULL, fs_ioctl, fs_ioctlv};

static u32 fpos[32];
static s32 file_open(int fd, const char *path, u32 mode) {
  (void)mode; int isdir;
  if (!nand_path_ok(path)) return FS_EINVAL;
  if (nand_stat(path, &isdir, NULL) || isdir) return FS_ENOENT;
  fpos[fd] = 0; return fd;
}
static s32 file_rw(int fd, int write, u32 buf, u32 len) {
  int n;
  u8 *direct = len ? hp(gaddr(buf)) : NULL, *dend = len ? hp(gaddr(buf) + len - 1) : NULL;
  if (direct && dend == direct + len - 1) {        /* contiguous guest memory: no byte-by-byte copy */
    n = write ? nand_write(fds[fd].path, fpos[fd], direct, len) : nand_read(fds[fd].path, fpos[fd], direct, len);
  } else {
    u8 *tmp = malloc(len ? len : 1);
    if (write) { for (u32 i = 0; i < len; i++) tmp[i] = (u8)R8(gaddr(buf) + i); n = nand_write(fds[fd].path, fpos[fd], tmp, len); }
    else { n = nand_read(fds[fd].path, fpos[fd], tmp, len); for (int i = 0; i < n; i++) W8(gaddr(buf) + i, tmp[i]); }
    free(tmp);
  }
  if (n > 0) fpos[fd] += n;
  return n < 0 ? FS_EACCESS : n;
}
static void file_close(int fd) { (void)fd; nandfs_save_host(); }   /* persist saves when a file is closed */
static s32 file_seek(int fd, s32 off, int whence) {
  u32 size = 0; nand_stat(fds[fd].path, NULL, &size);
  s64 np = whence == 0 ? off : whence == 1 ? (s64)fpos[fd] + off : (s64)size + off;
  if (np < 0 || np > size) return FS_EINVAL;
  fpos[fd] = (u32)np; return (s32)np;
}
static s32 file_ioctl(int fd, u32 r, u32 no, u32 in, u32 il, u32 out, u32 ol) {
  (void)r; (void)in; (void)il; (void)ol;
  if (no == 0x0B) { u32 size = 0; nand_stat(fds[fd].path, NULL, &size); W32(gaddr(out), size); W32(gaddr(out) + 4, fpos[fd]); return 0; }
  return FS_EINVAL;
}
static const Dev dev_file = {"/", 1, file_open, file_close, file_rw, file_seek, file_ioctl, NULL};

/* /dev/es */
static s32 es_ioctlv(int fd, u32 r, const Vec *v) {
  (void)fd; (void)r;
  for (u32 i = 0; i < v->nio; i++) gzero(iov_addr(v, v->nin + i), iov_len(v, v->nin + i));
  u32 o0 = v->nio ? gaddr(iov_addr(v, v->nin)) : 0;
  switch (v->no) {
    case 0x20: W32(o0, (u32)(g_title_id >> 32)); W32(o0 + 4, (u32)g_title_id); return 0;          /* GetTitleID */
    case 0x1D: {                                                                                  /* GetTitleDir */
      u32 i0 = gaddr(iov_addr(v, 0)); u64 t = (u64)R32(i0) << 32 | R32(i0 + 4);
      char s[64]; snprintf(s, sizeof s, "/title/%08x/%08x/data", (u32)(t >> 32), (u32)t);
      for (int i = 0; s[i]; i++) W8(o0 + i, s[i]);
      return 0;
    }
    case 0x07: W32(o0, 0x0403AC68); return 0;                                                    /* GetDeviceID */
    case 0x12: case 0x14: case 0x0E: case 0x0C: W32(o0, v->no == 0x14 ? 0 : 1); return 0;
  }
  if (g_log) fprintf(stderr, "[ios] es ioctlv %X (nin %u nio %u)\n", v->no, v->nin, v->nio);
  return 0;
}
static const Dev dev_es = {"/dev/es", 0, NULL, NULL, NULL, NULL, NULL, es_ioctlv};

/* /dev/di */
static u8 disc_header[0x20]; static int di_ok; static u32 di_error;
u32 g_di_reads, g_di_bytes;
static s32 di_ioctl(int fd, u32 r, u32 no, u32 in, u32 il, u32 out, u32 ol) {
  (void)fd; (void)r; (void)il;
  u32 iw[8]; for (int i = 0; i < 8; i++) iw[i] = R32(gaddr(in) + 4 * i);
  u32 cmd = iw[0] >> 24; (void)no;
  switch (cmd) {
    case 0x12: gzero(out, ol); W32(gaddr(out), 0x00000002); W32(gaddr(out) + 4, 0x20060526); W32(gaddr(out) + 8, 0x41000000); return 1;
    case 0x70: for (u32 i = 0; i < 0x20 && i < ol; i++) W8(gaddr(out) + i, disc_header[i]); return 1;
    case 0x71: case 0xD0: {
      u32 len = iw[1]; u64 off = (u64)iw[2] << 2;
      if (len > ol) len = ol;
      u8 *dst = hp(gaddr(out));
      if (!dst) return 2;
      int ok = disc_read(off, len, dst); g_di_reads++; g_di_bytes += len;
      { static int dl = -1; extern u64 g_vi_frames; if (dl < 0) dl = getenv("RGH_DILOG") ? atoi(getenv("RGH_DILOG")) : 0;
        if (dl && g_vi_frames >= (u64)dl) fprintf(stderr, "[di] vi %llu off %09llX len %X dst %08X\n", (unsigned long long)g_vi_frames, (unsigned long long)off, len, gaddr(out)); }
      if (!ok) fprintf(stderr, "[di] read failed off %09llX len %X\n", (unsigned long long)off, len);
      return ok ? 1 : 2;
    }
    case 0x79: return 1;
    case 0xA4: di_error = 0x052000; return 2;   /* ReportKey: not supported by the Wii drive (invalid command) */
    case 0x7A: W32(gaddr(out), 0); return 1;                       /* cover register: closed */
    case 0x88: W32(gaddr(out), 2); return 1;                       /* cover status: disc inside */
    case 0x86: case 0x8A: case 0x8E: case 0xE3: case 0xE4: case 0xAB: case 0x7E: case 0xD2: return 1;
    case 0xE0: W32(gaddr(out), di_error); di_error = 0; return 1;
    case 0xDB: case 0x95: case 0x96: W32(gaddr(out), 0); return 1;
    case 0x8D: {   /* unencrypted read: only the disc header area exists here; past the end of a
                      single-layer disc a real drive reports "LBA out of range" (the SDK's #001 check expects it) */
      u32 len = iw[1]; u64 off = (u64)iw[2] << 2; u8 *dst = hp(gaddr(out));
      if (off >= 0x50000) { di_error = 0x052100; return 2; }
      if (dst) memset(dst, 0, len < ol ? len : ol);
      return 1;
    }
  }
  fprintf(stderr, "[di] unhandled command %02X\n", cmd);
  return 1;
}
static s32 di_ioctlv(int fd, u32 r, const Vec *v) {
  (void)fd; (void)r;
  u32 cmd = R32(gaddr(iov_addr(v, 0))) >> 24;
  if (cmd == 0x8B) {     /* OpenPartition: out[0] = TMD (0x49E4), out[1] = error */
    char p[1200];
    if (v->nio >= 1) {
      u32 t = iov_addr(v, v->nin); u32 tl = iov_len(v, v->nin);
      gzero(t, tl);
      extern const char *g_partdir_for_di;
      snprintf(p, sizeof p, "%s/tmd.bin", g_partdir_for_di);
      u32 n = 0; u8 *b = host_file_load(p, &n);
      if (b) { for (u32 i = 0; i < n && i < tl; i++) W8(gaddr(t) + i, b[i]); free(b); }
    }
    if (v->nio >= 2) W32(gaddr(iov_addr(v, v->nin + 1)), 0);
    return 1;
  }
  if (cmd == 0x8C) return 1;
  fprintf(stderr, "[di] unhandled ioctlv command %02X\n", cmd);
  return 1;
}
static const Dev dev_di = {"/dev/di", 0, NULL, NULL, NULL, NULL, di_ioctl, di_ioctlv};

/* network: never connected */
static s32 net_ioctl(int fd, u32 r, u32 no, u32 in, u32 il, u32 out, u32 ol) { (void)fd; (void)r; (void)no; (void)in; (void)il; gzero(out, ol); if (out && ol >= 4) W32(gaddr(out), (u32)-29); return -29; }
static s32 net_ioctlv(int fd, u32 r, const Vec *v) { (void)fd; (void)r; (void)v; return -29; }
static const Dev dev_net = {"/dev/net/", 1, NULL, NULL, NULL, NULL, net_ioctl, net_ioctlv};

static const Dev *devices[] = {&dev_stm_imm, &dev_stm_ev, &dev_fs, &dev_es, &dev_di, &dev_net, NULL};
const char *g_partdir_for_di = "game/part";

static void execute(u32 r) {
  u32 cmd = R32(r); s32 fd = (s32)R32(r + 8); s32 res = -4;
  if (cmd == IOS_OPEN) {
    char path[128]; gstr(R32(r + 0xC), path, sizeof path);
    u32 mode = R32(r + 0x10);
    const Dev *d = NULL;
    for (int i = 0; devices[i]; i++) {
      const Dev *x = devices[i];
      if (x->prefix ? !strncmp(path, x->name, strlen(x->name)) : !strcmp(path, x->name)) { d = x; break; }
    }
    if (!d && path[0] == '/' && strncmp(path, "/dev/", 5)) d = &dev_file;
    int nfd = -1;
    for (int i = 0; i < 32; i++) if (!fds[i].dev) { nfd = i; break; }
    if (!d) res = -6;
    else if (nfd < 0) res = -22;
    else {
      fds[nfd].dev = d; fds[nfd].hostfd = -1; snprintf(fds[nfd].path, sizeof fds[nfd].path, "%s", path);
      res = d->open ? d->open(nfd, path, mode) : nfd;
      if (res < 0) fds[nfd].dev = NULL;
    }
    if (g_log) fprintf(stderr, "[ios] open %s mode %u -> %d\n", path, mode, res);
  } else if (fd < 0 || fd >= 32 || !fds[fd].dev) {
    res = -4;
    if (g_log) fprintf(stderr, "[ios] cmd %u on bad fd %d\n", cmd, fd);
  } else {
    const Dev *d = fds[fd].dev;
    switch (cmd) {
      case IOS_CLOSE: if (d->close) d->close(fd); fds[fd].dev = NULL; res = 0; break;
      case IOS_READ: case IOS_WRITE: res = d->rw ? d->rw(fd, cmd == IOS_WRITE, R32(r + 0xC), R32(r + 0x10)) : -4; break;
      case IOS_SEEK: res = d->seek ? d->seek(fd, (s32)R32(r + 0xC), (int)R32(r + 0x10)) : -4; break;
      case IOS_IOCTL: res = d->ioctl ? d->ioctl(fd, r, R32(r + 0xC), R32(r + 0x10), R32(r + 0x14), R32(r + 0x18), R32(r + 0x1C)) : -4; break;
      case IOS_IOCTLV: { Vec v = {R32(r + 0xC), R32(r + 0x10), R32(r + 0x14), R32(r + 0x18)};
        res = d->ioctlv ? d->ioctlv(fd, r, &v) : -4; break; }
    }
    if (g_log) fprintf(stderr, "[ios] %s cmd %u no %X -> %d\n", fds[fd].path, cmd, (cmd >= 6 ? R32(r + 0xC) : 0), res == PENDING ? 999 : res);
  }
  if (res != PENDING) reply(r, res);
}

/* ---------------------------------------------------------------- NAND defaults */
static void sysconf_default(const char *path) {
  u8 b[0x4000]; memset(b, 0, sizeof b);
  struct { const char *n; int type; u32 len; u64 val; } it[] = {
    {"IPL.LNG", 3, 1, 1}, {"IPL.AR", 3, 1, 0}, {"IPL.E60", 3, 1, 1}, {"IPL.PGS", 3, 1, 0}, {"IPL.SND", 3, 1, 1},
    {"IPL.SSV", 3, 1, 1}, {"IPL.CB", 5, 4, 0}, {"IPL.DH", 3, 1, 0}, {"IPL.UPT", 3, 1, 2}, {"IPL.EULA", 7, 1, 1},
    {"IPL.IDL", 2, 2, 0x0100}, {"IPL.INC", 5, 4, 8}, {"IPL.NIK", 2, 0x16, 0}, {"IPL.PC", 2, 0x4A, 0},
    {"IPL.SADR", 1, 0x1007, 0x31}, {"IPL.CD", 7, 1, 1}, {"IPL.CD2", 7, 1, 1},
    {"BT.SENS", 5, 4, 3}, {"BT.BAR", 3, 1, 0}, {"BT.MOT", 3, 1, 1}, {"BT.SPKV", 3, 1, 0x58},
    {"BT.DINF", 1, 0x461, 0}, {"BT.CDIF", 1, 0x205, 0},
    {"NET.CTPC", 5, 4, 0}, {"NET.WCFG", 5, 4, 1}, {"NET.PROF", 1, 0x1B5C, 0},
    {"DEV.BTM", 3, 1, 0}, {"DEV.VIM", 3, 1, 0}, {"DEV.CTC", 5, 4, 0}, {"DEV.DSM", 3, 1, 0},
    {"MPLS.MOVIE", 7, 1, 1},
  };
  int n = sizeof it / sizeof it[0];
  memcpy(b, "SCv0", 4); b[4] = (u8)(n >> 8); b[5] = (u8)n;
  u32 pos = 6 + 2 * (n + 1);
  for (int i = 0; i < n; i++) {
    b[6 + 2 * i] = (u8)(pos >> 8); b[7 + 2 * i] = (u8)pos;
    int nl = (int)strlen(it[i].n);
    b[pos++] = (u8)(it[i].type << 5 | (nl - 1));
    memcpy(b + pos, it[i].n, nl); pos += nl;
    if (it[i].type == 1) { b[pos++] = (u8)((it[i].len - 1) >> 8); b[pos++] = (u8)(it[i].len - 1); if (!strcmp(it[i].n, "IPL.SADR")) b[pos] = (u8)it[i].val; pos += it[i].len; }
    else if (it[i].type == 2) { b[pos++] = (u8)(it[i].len - 1); u32 m = it[i].len < 8 ? it[i].len : 8;
      for (u32 k = 0; k < m; k++) b[pos + it[i].len - m + k] = (u8)(it[i].val >> (8 * (m - 1 - k))); pos += it[i].len; }
    else { for (u32 k = 0; k < it[i].len; k++) b[pos + k] = (u8)(it[i].val >> (8 * (it[i].len - 1 - k))); pos += it[i].len; }
  }
  b[6 + 2 * n] = (u8)(pos >> 8); b[7 + 2 * n] = (u8)pos;
  memcpy(b + 0x3FFC, "SCed", 4);
  nand_put(path, b, sizeof b);
}
/* change a 1-byte SYSCONF setting in place (e.g. IPL.AR: 0 = 4:3, 1 = 16:9) */
static void sysconf_set_u8(const char *name, u8 val) {
  u8 b[0x4000]; if (nand_read("/shared2/sys/SYSCONF", 0, b, sizeof b) != (int)sizeof b || memcmp(b, "SCv0", 4)) return;
  int n = b[4] << 8 | b[5], nl = (int)strlen(name);
  for (int i = 0; i < n; i++) {
    u32 off = (u32)(b[6 + 2 * i] << 8 | b[7 + 2 * i]);
    if (off + 1 + nl + 1 > sizeof b) break;
    if ((b[off] & 0x1F) + 1 == nl && !memcmp(b + off + 1, name, nl) && (b[off] >> 5) == 3) {
      if (b[off + 1 + nl] != val) { b[off + 1 + nl] = val; nand_write("/shared2/sys/SYSCONF", 0, b, sizeof b); }
      return;
    }
  }
}
static void setting_default(const char *path) {
  const char *txt = "AREA=USA\r\nMODEL=RVL-001(USA)\r\nDVD=0\r\nMPCH=0x7FFE\r\nCODE=LU\r\nSERNO=000000000\r\nVIDEO=NTSC\r\nGAME=US\r\n";
  u8 b[0x100]; memset(b, 0, sizeof b); memcpy(b, txt, strlen(txt));
  u32 key = 0x73B5DBFA;
  for (int i = 0; i < 0x100; i++) { b[i] ^= (u8)key; key = (key << 1) | (key >> 31); }
  nand_put(path, b, sizeof b);
}

void ios_init(const char *partdir) {
  g_log = getenv("RGH_IOS") != NULL;
  g_partdir_for_di = partdir;
  const char *n = getenv("RGH_NAND");
  if (n) snprintf(g_nand, sizeof g_nand, "%s", n); else snprintf(g_nand, sizeof g_nand, "%s/nand", g_gamedir);
#ifndef __wasm__
  { extern int mkdir(const char *, unsigned); mkdir(g_nand, 0755); }
#endif
  nandfs_load_host(g_nand);
#ifndef __wasm__
  /* RGH_NANDIMG=file: start from a browser save image (partida-rabbids.bin) */
  { const char *img = getenv("RGH_NANDIMG"); if (img) { FILE *f = fopen(img, "rb");
      if (f) { fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); u8 *b = malloc((size_t)n); if (fread(b, 1, (size_t)n, f) == (size_t)n) { extern void nandfs_deserialize(const u8 *, u32); nandfs_deserialize(b, (u32)n); fprintf(stderr, "[nand] image %s loaded (%ld bytes)\n", img, n); } free(b); fclose(f); } } }
#endif
  if (nand_stat("/shared2/sys/SYSCONF", NULL, NULL)) sysconf_default("/shared2/sys/SYSCONF");
  if (nand_stat("/title/00000001/00000002/data/setting.txt", NULL, NULL)) setting_default("/title/00000001/00000002/data/setting.txt");
  { const char *ws = getenv("RGH_WIDESCREEN"); sysconf_set_u8("IPL.AR", ws && ws[0] == '0' ? 0 : 1); }   /* 16:9 unless asked */
  nand_mkdirs("/tmp");
  char p[1200]; u32 tn = 0; u8 *t;
  /* title id from the ticket (offset 0x1DC) */
  snprintf(p, sizeof p, "%s/ticket.bin", partdir);
  if ((t = host_file_load(p, &tn)) && tn >= 0x1E4) { g_title_id = 0; for (int i = 0; i < 8; i++) g_title_id = g_title_id << 8 | t[0x1DC + i]; }
  free(t);
  snprintf(p, sizeof p, "/title/%08x/%08x/data", (u32)(g_title_id >> 32), (u32)g_title_id); nand_mkdirs(p);
  nandfs_save_host();
  snprintf(p, sizeof p, "%s/sys/boot.bin", partdir);
  di_ok = host_file_read(p, 0, disc_header, 0x20) == 0x20;
  disc_open(partdir);
}
