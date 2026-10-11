/* hle.c - host replacements for guest functions listed in runtime/hle.list */
#include "hw.h"
#include <stdio.h>
#include <stdlib.h>

/* __OSInitAudioSystem: uploads and runs a DSP init microcode, then waits on DSP mailbox handshakes.
 * There is no DSP to initialise here; the audio path is handled at the AX level. */
HLE(80366170) { (void)c; }

/* minimal guest printf: PowerPC EABI varargs (r3.. / f1..) */
static void gfmt(CPU *c, int firstg, char *out, size_t cap) {
  u32 fmt = c->gpr[firstg - 1];
  int g = firstg, fr = 1; u32 stk = c->gpr[1] + 8;
  size_t o = 0;
  #define NEXTG() (g <= 10 ? c->gpr[g++] : (stk += 4, R32(stk - 4)))
  for (u32 p = fmt; o + 64 < cap; p++) {
    char ch = (char)R8(p); if (!ch) break;
    if (ch != '%') { out[o++] = ch; continue; }
    char spec[32]; int sl = 0; spec[sl++] = '%';
    int lng = 0;
    for (;;) {
      ch = (char)R8(++p);
      if (!ch) { p--; break; }
      if (ch == 'l') { lng++; continue; }
      if (ch == 'h') continue;
      if (sl < 30) spec[sl++] = ch;
      if (strchr("diouxXcspfFeEgG%", ch)) break;
    }
    spec[sl] = 0;
    switch (ch) {
      case '%': out[o++] = '%'; break;
      case 's': { char tmp[256]; u32 a = NEXTG(); int i = 0; if (a) for (; i < 255; i++) { tmp[i] = (char)R8(a + i); if (!tmp[i]) break; } tmp[i] = 0;
        o += snprintf(out + o, cap - o, spec, a ? tmp : "(null)"); break; }
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
        f64 v = fr <= 8 ? c->ps0[fr++] : 0; o += snprintf(out + o, cap - o, spec, v); break; }
      case 'c': o += snprintf(out + o, cap - o, spec, (int)NEXTG()); break;
      default:
        if (lng >= 2) { if (g % 2 == 0) g++; u32 hi = NEXTG(), lo = NEXTG(); char s2[40]; snprintf(s2, sizeof s2, "%s", spec); s2[strlen(s2) - 1] = 0;
          strcat(s2, "ll"); size_t L = strlen(s2); s2[L] = ch; s2[L + 1] = 0; o += snprintf(out + o, cap - o, s2, ((u64)hi << 32) | lo); }
        else if (ch == 'd' || ch == 'i') o += snprintf(out + o, cap - o, spec, (int)NEXTG());
        else o += snprintf(out + o, cap - o, spec, (unsigned)NEXTG());
    }
    if (o >= cap) o = cap - 1;
  }
  out[o] = 0;
}

/* OSReport(const char *fmt, ...) */
static int g_report = -1;
void hle_OSReport(CPU *restrict c) {
  if (g_report < 0) g_report = getenv("RGH_QUIET") == NULL;
  if (!g_report) return;
  char b[2048]; gfmt(c, 4, b, sizeof b);
  fprintf(stderr, "[OSReport] %s%s", b, (b[0] && b[strlen(b) - 1] == '\n') ? "" : "\n");
}
/* OSPanic(const char *file, int line, const char *fmt, ...) */
void hle_OSPanic(CPU *restrict c) {
  char file[128]; int i = 0; for (; i < 127; i++) { file[i] = (char)R8(c->gpr[3] + i); if (!file[i]) break; } file[i] = 0;
  int line = (int)c->gpr[4];
  char b[2048]; gfmt(c, 6, b, sizeof b);
  fprintf(stderr, "[OSPanic] %s:%d: %s\n", file, line, b);
  hw_shutdown(); exit(7);
}
/* OSFatal(GXColor fg, GXColor bg, const char *msg) */
void hle_OSFatal(CPU *restrict c) {
  char b[1024]; int i = 0; for (; i < 1023; i++) { b[i] = (char)R8(c->gpr[5] + i); if (!b[i]) break; } b[i] = 0;
  fprintf(stderr, "[OSFatal] %s\n(called from %08X %s)\n", b, c->lr, rt_sym(c->lr));
  hw_shutdown(); exit(8);
}
