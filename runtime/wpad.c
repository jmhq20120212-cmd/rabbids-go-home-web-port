/* wpad.c - Wii Remote input without Bluetooth: the WPAD library's public functions are replaced
 * (see hle.list) and report one Wii Remote + Nunchuk on channel 0, fed from host input (g_pad).
 * The KPAD library above it stays the game's own code. */
#include "hw.h"
#include "input.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

Pad g_pad;
static u32 conn_cb[4], ext_cb[4], samp_cb[4], fmt[4], dpd_on[4], motor_on = 1;
static int connected_sent, sampling_ready;
static u32 kpad_user_seen, ext_seen;
static int g_log;
static void wl(const char *f, CPU *c) { static int n; if (getenv("RGH_WPAD") && n++ < 200) fprintf(stderr, "[wpad] %s(%d, %08X, %08X) from %s\n", f, c->gpr[3], c->gpr[4], c->gpr[5], rt_sym(c->lr)); }

/* call a guest function as a callback, preserving the caller's registers */
static void guest_call(CPU *c, u32 fn, u32 a0, u32 a1) {
  if (!fn) return;
  CPU save = *c;
  c->gpr[3] = a0; c->gpr[4] = a1;
  c->gpr[1] -= 0x40;
  c->lr = 0;
  c->msr &= ~0x8000u;          /* callbacks run with interrupts off, as from the Bluetooth interrupt */
  call_indirect(c, fn);
  *c = save;
}

#define RET(v) do { c->gpr[3] = (u32)(v); } while (0)

void hle_WPADInit(CPU *restrict c) { (void)c; g_log = getenv("RGH_WPAD") != NULL; }
void hle_WPADGetStatus(CPU *restrict c) { wl("WPADGetStatus", c); RET(3); }                       /* WPAD_STATE_SETUP */
void hle_WPADProbe(CPU *restrict c) { wl("WPADProbe", c);
  s32 ch = (s32)c->gpr[3]; u32 tp = c->gpr[4];
  if (ch == 0 && g_pad.connected) { if (tp) W32(tp, g_pad.nunchuk ? 1 : 0); RET(0); }
  else { if (tp) W32(tp, 253); RET(-1); }
}
void hle_WPADSetConnectCallback(CPU *restrict c) { wl("WPADSetConnectCallback", c); int ch = c->gpr[3] & 3; u32 o = conn_cb[ch]; conn_cb[ch] = c->gpr[4]; RET(o); if (ch == 0) connected_sent = 0; }
void hle_WPADSetExtensionCallback(CPU *restrict c) { wl("WPADSetExtensionCallback", c); int ch = c->gpr[3] & 3; u32 o = ext_cb[ch]; ext_cb[ch] = c->gpr[4]; RET(o); }
void hle_WPADSetSamplingCallback(CPU *restrict c) { wl("WPADSetSamplingCallback", c); int ch = c->gpr[3] & 3; u32 o = samp_cb[ch]; samp_cb[ch] = c->gpr[4]; RET(o); }
void hle_WPADGetDataFormat(CPU *restrict c) { RET(fmt[c->gpr[3] & 3]); }
void hle_WPADSetDataFormat(CPU *restrict c) { wl("WPADSetDataFormat", c); fmt[c->gpr[3] & 3] = c->gpr[4]; RET(0); }
void hle_WPADGetAccGravityUnit(CPU *restrict c) { u32 p = c->gpr[5]; W16(p, 100); W16(p + 2, 100); W16(p + 4, 100); }
void hle_WPADGetSensorBarPosition(CPU *restrict c) { RET(0); }
void hle_WPADGetRadioSensitivity(CPU *restrict c) { RET(100); }
void hle_WPADDisconnect(CPU *restrict c) { (void)c; }
void hle_WPADIsMotorEnabled(CPU *restrict c) { RET(motor_on); }
void hle_WPADEnableMotor(CPU *restrict c) { motor_on = c->gpr[3]; }
void hle_WPADControlMotor(CPU *restrict c) { g_pad.rumble = c->gpr[4] == 1; }
/* Wii Remote speaker: reported as present and working, so the game plays its remote sounds. The sound itself is
 * taken from the AX voices routed to the remote (runtime/ax.c) and played through the PC speakers; the
 * encoded stream the game sends to the remote is accepted and dropped. */
static int spk_on, spk_vol = 88;
int g_spk_audible;          /* read by runtime/ax.c: remote speaker on and not muted */
void hle_WPADIsSpeakerEnabled(CPU *restrict c) { RET(spk_on); }
void hle_WPADGetSpeakerVolume(CPU *restrict c) { RET(spk_vol); }
void hle_WPADSetSpeakerVolume(CPU *restrict c) { spk_vol = (int)(c->gpr[3] & 0x7F); }
void hle_WPADCanSendStreamData(CPU *restrict c) { RET(c->gpr[3] == 0 && spk_on); }
void hle_WPADSendStreamData(CPU *restrict c) { static int n; if (!n++ && g_log) fprintf(stderr, "[wpad] speaker stream data\n"); RET(0); }
void hle_WPADGetDpdSensitivity(CPU *restrict c) { RET(3); }
void hle_WPADIsDpdEnabled(CPU *restrict c) { RET(dpd_on[c->gpr[3] & 3]); }
void hle_WPADStartFastSimpleSync(CPU *restrict c) { RET(1); }
void hle_WPADStopSimpleSync(CPU *restrict c) { RET(1); }
void hle_WPADSetSimpleSyncCallback(CPU *restrict c) { RET(0); }
void hle_WPADSetAutoSamplingBuf(CPU *restrict c) { (void)c; }
/* asynchronous controls: complete immediately and run the callback (chan, WPAD_ERR_NONE) */
static void done_cb(CPU *c, u32 cb, u32 ch) { u32 r1 = c->gpr[1]; if (cb) guest_call(c, cb, ch, 0); c->gpr[1] = r1; }
void hle_WPADControlDpd(CPU *restrict c) { wl("WPADControlDpd", c); u32 ch = c->gpr[3], cmd = c->gpr[4], cb = c->gpr[5]; dpd_on[ch & 3] = cmd != 0; done_cb(c, cb, ch); RET(0); }
void hle_WPADControlLed(CPU *restrict c) { u32 ch = c->gpr[3], cb = c->gpr[5]; done_cb(c, cb, ch); RET(0); }
void hle_WPADControlSpeaker(CPU *restrict c) { u32 ch = c->gpr[3], cmd = c->gpr[4], cb = c->gpr[5];
  static int muted;
  if (ch == 0) { if (cmd == 0) spk_on = 0; else if (cmd == 1 || cmd == 4) spk_on = 1; else if (cmd == 2) muted = 1; else if (cmd == 3) { muted = 0; spk_on = 1; } }
  g_spk_audible = spk_on && !muted;
  if (g_log) fprintf(stderr, "[wpad] WPADControlSpeaker(%u, %u)\n", ch, cmd);
  done_cb(c, cb, ch); RET(0); }
void hle_WPADControlBLC(CPU *restrict c) { u32 ch = c->gpr[3], cb = c->gpr[5]; done_cb(c, cb, ch); RET(0); }
void hle_WPADSaveConfig(CPU *restrict c) { u32 cb = c->gpr[3]; done_cb(c, cb, 0); RET(1); }
void hle_WPADWriteExtReg(CPU *restrict c) { u32 ch = c->gpr[3]; u32 cb = c->gpr[7]; done_cb(c, cb, ch); RET(0); }
void hle_WPADGetInfoAsync(CPU *restrict c) { wl("WPADGetInfoAsync", c);
  u32 ch = c->gpr[3], info = c->gpr[4], cb = c->gpr[5];
  if (info) { for (int i = 0; i < 0x18; i++) W8(info + i, 0);
    W32(info + 0, dpd_on[ch & 3]); W32(info + 4, spk_on); W32(info + 8, g_pad.nunchuk); W8(info + 0x14, 4); W8(info + 0x15, 1); }
  done_cb(c, cb, ch); RET(0);
}

/* WPADRead(chan, void *status): fills WPADStatus / WPADFSStatus for the current data format */
static void fill_status(u32 s, int ch) {
  int f = (int)fmt[ch];
  W16(s + 0, g_pad.buttons & 0xFFFF);
  int ax = (int)(g_pad.acc[0] * 100), ay = (int)(g_pad.acc[1] * 100), az = (int)(g_pad.acc[2] * 100);
  W16(s + 2, (u32)ax); W16(s + 4, (u32)ay); W16(s + 6, (u32)az);
  for (int i = 0; i < 4; i++) { W16(s + 8 + 8 * i, 0); W16(s + 10 + 8 * i, 0); W16(s + 12 + 8 * i, 0); W8(s + 14 + 8 * i, 0); W8(s + 15 + 8 * i, 0); }
  if (dpd_on[ch] && g_pad.ir_valid) {
    /* two sensor-bar dots; the camera image moves opposite to the pointer */
    float cx = 512 - g_pad.ir_x * 226, cy = 486 + g_pad.ir_y * 225;   /* calibrated: KPAD pos == (ir_x, -ir_y), sensor bar offset included */
    for (int i = 0; i < 2; i++) {
      int x = (int)(cx + (i ? 100 : -100)), y = (int)cy;
      W16(s + 8 + 8 * i, (u32)x); W16(s + 10 + 8 * i, (u32)y); W16(s + 12 + 8 * i, 5); W8(s + 14 + 8 * i, i);
    }
  }
  W8(s + 0x28, g_pad.nunchuk ? 1 : 0);
  W8(s + 0x29, 0);
  if (f >= 3 && f <= 5 && g_pad.nunchuk) {
    W16(s + 0x2A, (u32)(s32)(g_pad.nacc[0] * 100)); W16(s + 0x2C, (u32)(s32)(g_pad.nacc[1] * 100)); W16(s + 0x2E, (u32)(s32)(g_pad.nacc[2] * 100));
    W8(s + 0x30, (u32)(s32)(g_pad.stick[0] * 100)); W8(s + 0x31, (u32)(s32)(g_pad.stick[1] * 100));
  }
}
void hle_WPADRead(CPU *restrict c) {
  int ch = c->gpr[3] & 3;
  if (ch != 0 || !g_pad.connected) { W8(c->gpr[4] + 0x29, (u32)-1); return; }
  fill_status(c->gpr[4], ch);
}

/* called from rt_poll with interrupts enabled: deliver connection and sampling events */
void wpad_poll(CPU *c) {
  static u64 next; u64 tb = hw_timebase();
  /* The Wii dims the screen and the game stops feeding its streamed sounds (music, the sounds of the
   * animations: SLib's ADPCM decoder outputs silence while VIGetDimmingCount() is 0) when the remote has been
   * idle for 5 minutes. The real WPAD library tells VI about remote activity; ours replaces that library,
   * so after 5 minutes of play everything streamed went silent. The remote here is always "active". */
  W32(0x806C302Cu, 0);    /* __VIDimmingFlag_RF_IDLE  (__VIResetRFIdle) */
  W32(0x805B24C8u, 0);    /* __VIDimmingFlag_DEV_IDLE (VIResetDimmingCount) */
  if (!g_pad.connected) return;
  { static int ap = -1; static u32 mask = 0x0800;
    if (ap < 0) { const char *e = getenv("RGH_AUTOPRESS"); ap = e != NULL; if (e && strtoul(e, NULL, 16) > 1) mask = (u32)strtoul(e, NULL, 16); }
    if (ap) { u64 ms = tb / (TB_HZ / 1000); g_pad.buttons = (ms % 2000) < 150 ? mask : 0; } }
  /* RGH_CTRL=file: interactive control for native test runs. The file holds one line
   * "B=<hex buttons> IR=<x>,<y> ST=<x>,<y> SH=<0|1> NS=<0|1>" and is re-read whenever it changes. */
  { static int ct = -1; static const char *path; static u64 nextchk; static char last[256];
    if (ct < 0) { path = getenv("RGH_CTRL"); ct = path != NULL; }
    if (ct && tb >= nextchk) { nextchk = tb + TB_HZ / 50;
      FILE *f = fopen(path, "r"); char line[256] = {0};
      if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
      if (strcmp(line, last)) { strcpy(last, line); fprintf(stderr, "[ctrl] %s", line[0] ? line : "(empty)\n"); }
      unsigned b = 0; float ix = g_pad.ir_x, iy = g_pad.ir_y, sx = 0, sy = 0; int sh = 0, ns = 0; const char *q;
      if ((q = strstr(line, "B="))) b = (unsigned)strtoul(q + 2, NULL, 16);
      if ((q = strstr(line, "IR="))) sscanf(q + 3, "%f,%f", &ix, &iy);
      if ((q = strstr(line, "ST="))) sscanf(q + 3, "%f,%f", &sx, &sy);
      if ((q = strstr(line, "SH="))) sh = atoi(q + 3);
      if ((q = strstr(line, "NS="))) ns = atoi(q + 3);
      g_pad.buttons = b; g_pad.ir_x = ix; g_pad.ir_y = iy; g_pad.stick[0] = sx; g_pad.stick[1] = sy;
      float w = (float)((tb / (TB_HZ / 1000)) % 1000) / 40.0f;
      g_pad.acc[0] = sh ? 2.5f * sinf(w) : 0; g_pad.acc[1] = sh ? 2.5f * cosf(w * 1.3f) : 0; g_pad.acc[2] = sh ? 1 + 2.5f * sinf(w * 0.7f) : 1;
      g_pad.nacc[0] = ns ? 2.5f * sinf(w) : 0; g_pad.nacc[1] = ns ? 2.5f * cosf(w * 1.3f) : 0; g_pad.nacc[2] = ns ? 1 + 2.5f * sinf(w * 0.7f) : 1;
    } }
  /* RGH_AUTOSEQ="0C00,0800,S,0800": one step every 1.5 s, held 150 ms; S = shake the remote, N = shake the nunchuk */
  { static int as = -1; static char seq[32][16]; static int nseq;
    if (as < 0) { const char *e = getenv("RGH_AUTOSEQ"); as = e != NULL; nseq = 0;
      while (e && *e && nseq < 32) { int k = 0; while (*e && *e != ',' && k < 15) seq[nseq][k++] = *e++; seq[nseq++][k] = 0; while (*e == ',') e++; } }
    { static int sw = -1; static u64 swms; if (sw < 0) { sw = getenv("RGH_AUTOSEQ2") != NULL; swms = getenv("RGH_AUTOSEQ_SWITCH") ? (u64)atoi(getenv("RGH_AUTOSEQ_SWITCH")) * 1000 : 60000; }
      if (sw == 1 && tb / (TB_HZ / 1000) >= swms) { sw = 2; const char *e = getenv("RGH_AUTOSEQ2"); nseq = 0;
        while (e && *e && nseq < 32) { int k = 0; while (*e && *e != ',' && k < 15) seq[nseq][k++] = *e++; seq[nseq++][k] = 0; while (*e == ',') e++; } } }
    if (as && nseq) { u64 ms = tb / (TB_HZ / 1000); const char *st = seq[(ms / 1500) % nseq]; int on = (ms % 1500) < 150, shk = (ms % 1500) < 450;
      if (st[0] == 'P') { sscanf(st + 1, "%f:%f", &g_pad.ir_x, &g_pad.ir_y); st = ""; }
      if (st[0] == 'X') { sscanf(st + 1, "%f:%f", &g_pad.stick[0], &g_pad.stick[1]); st = ""; }
      if (st[0] == 'H') { on = 1; st++; }          /* H<mask>: hold the buttons for the whole step */
      float w = (float)(ms % 1000) / 40.0f;
      g_pad.buttons = 0; g_pad.acc[0] = 0; g_pad.acc[1] = 0; g_pad.acc[2] = 1; g_pad.nacc[0] = 0; g_pad.nacc[1] = 0; g_pad.nacc[2] = 1;
      if (st[0] == 'S' && shk) { g_pad.acc[0] = 2.5f * sinf(w); g_pad.acc[1] = 2.5f * cosf(w * 1.3f); g_pad.acc[2] = 1 + 2.5f * sinf(w * 0.7f); }
      else if (st[0] == 'N' && shk) { g_pad.nacc[0] = 2.5f * sinf(w); g_pad.nacc[1] = 2.5f * cosf(w * 1.3f); g_pad.nacc[2] = 1 + 2.5f * sinf(w * 0.7f); }
      else if (on) g_pad.buttons = (u32)strtoul(st, NULL, 16); } }
  { static int jl = -1; static u64 nextlog; if (jl < 0) jl = getenv("RGH_JOYLOG") != NULL;
    if (jl && tb >= nextlog) { nextlog = tb + TB_HZ / 2;
      u32 base = R32(c->gpr[13] - 0x66A8);
      if (base) fprintf(stderr, "[joy] ir=%.2f,%.2f kpos=%.3f,%.3f ", g_pad.ir_x, g_pad.ir_y, (double)RFS(base + 0x168 + 0x20), (double)RFS(base + 0x168 + 0x24));
      if (base) fprintf(stderr, "[joy] pad=%04X base=%08X j0: cur=%08X prev=%08X state=%u chan=%u n=%d hold=%08X kerr=%d f154=%u f158=%u f15C=%u\n", g_pad.buttons, base, R32(base), R32(base + 4), R32(base + 0xC6C), R32(base + 8), (int)R32(base + 0x94), R32(base + 0x168), (int)(s8)R8(base + 0x1C6), R8(base + 0x154), R32(base + 0x158), R32(base + 0x15C)); } }
  if (!connected_sent && conn_cb[0]) {
    connected_sent = 1;
    if (g_log) fprintf(stderr, "[wpad] connect chan 0\n");
    guest_call(c, conn_cb[0], 0, 0);
    if (ext_cb[0]) guest_call(c, ext_cb[0], 0, g_pad.nunchuk ? 1 : 0);
    ext_seen = ext_cb[0];
    sampling_ready = 1;
  }
  /* On a console the remote connects seconds after boot, so callbacks installed late still see the
   * connection. Here it is "connected" from the start: replay the event to callbacks set afterwards.
   * KPAD keeps the game's connect callback in its channel block (0x80609380 + chan*1400 + 0x550). */
  if (connected_sent) {
    u32 ucb = R32(0x80609380u + 0x550);
    if (ucb && ucb != kpad_user_seen) { kpad_user_seen = ucb; if (g_log) fprintf(stderr, "[wpad] replay connect to %s\n", rt_sym(ucb)); guest_call(c, ucb, 0, 0); }
    if (ext_cb[0] && ext_cb[0] != ext_seen) { ext_seen = ext_cb[0]; if (g_log) fprintf(stderr, "[wpad] replay extension to %s\n", rt_sym(ext_cb[0])); guest_call(c, ext_cb[0], 0, g_pad.nunchuk ? 1 : 0); }
  }
  if (sampling_ready && samp_cb[0] && tb >= next) {
    next = tb + TB_HZ / 200;          /* 200 reports per second */
    guest_call(c, samp_cb[0], 0, 0);
  }
}
#ifdef __wasm__
__attribute__((import_module("rgh"), import_name("pad_ready"))) void js_pad_ready(void *pad);
#endif
void wpad_init(void) {
  g_pad.connected = 1; g_pad.nunchuk = 1; g_pad.acc[2] = 1.0f; g_pad.nacc[2] = 1.0f; g_pad.ir_valid = 1;
  { const char *e = getenv("RGH_IRTEST"); if (e) sscanf(e, "%f,%f", &g_pad.ir_x, &g_pad.ir_y); }
#ifdef __wasm__
  js_pad_ready(&g_pad);
#endif
}
