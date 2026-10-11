/* hw.h - emulated Wii hardware (Hollywood MMIO), HLE IOS, guest threads. */
#pragma once
#include "rt.h"

#define TB_HZ 60750000ull               /* Broadway timebase: bus clock / 4 */

/* PI interrupt cause bits */
#define PI_GPERR 0x1
#define PI_RSW 0x2
#define PI_DI 0x4
#define PI_SI 0x8
#define PI_EXI 0x10
#define PI_AI 0x20
#define PI_DSP 0x40
#define PI_MEM 0x80
#define PI_VI 0x100
#define PI_PE_TOKEN 0x200
#define PI_PE_FINISH 0x400
#define PI_CP 0x800
#define PI_HSP 0x2000
#define PI_IPC 0x4000

extern const char *g_gamedir;
extern int g_trace;

void hw_init(const char *partdir);
void hw_shutdown(void);
u64 hw_timebase(void);
void hw_tick(void);
int hw_irq_pending(void);
void pi_set(u32 bit, int on);

/* os.c */
void os_run(CPU *c, u32 entry);
u32 os_get_dec(void);
void os_set_dec(u32 v);

/* ios.c */
void ios_init(const char *partdir);
u32 ios_read(u32 off);
void ios_write(u32 off, u32 v);
int ios_irq(void);
void ios_tick(void);

/* gx.c */
void gx_init(void);
void gx_fifo_write(u32 v, int bytes);
u16 gx_cp_read(u32 off); void gx_cp_write(u32 off, u16 v);
u16 gx_pe_read(u32 off); void gx_pe_write(u32 off, u16 v);
void gx_frame(u32 xfb_addr);
void gx_fifo_advanced(u32 from, u32 to);
u32 hw_fifo_wptr(void);
extern u64 g_gx_bytes;

/* disc.c */
int disc_open(const char *partdir);
int disc_read(u64 off, u32 len, u8 *dst);
int gx_gp_linked(void);
