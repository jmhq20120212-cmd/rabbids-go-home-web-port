/* input.h - host input state fed to the emulated Wii Remote (wpad.c) */
#pragma once
typedef struct Pad {
  int connected, nunchuk, ir_valid, rumble;
  unsigned buttons;          /* WPAD button bits (0x0800 A, 0x0400 B, 0x2000 Z, 0x4000 C, ...) */
  float ir_x, ir_y;          /* pointer, -1..1 (right / up positive) */
  float acc[3];              /* remote acceleration in g */
  float stick[2];            /* nunchuk stick -1..1 */
  float nacc[3];             /* nunchuk acceleration in g */
} Pad;
extern Pad g_pad;
void wpad_init(void);
