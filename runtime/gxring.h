/* gxring.h - command stream from the emulated GPU front end (gx.c / gxrender.c) to the renderer
 * (web/gx-renderer.js). All records are sequences of little-endian u32 words in a ring buffer in
 * shared memory; the first word is (type << 24) | length_in_words (including the header). */
#pragma once
#include "rt.h"

enum {
  RC_BP = 1,          /* reg, value                         : BP (raster/TEV/PE) register write */
  RC_XF = 2,          /* addr, count, data[count]           : XF memory/register write */
  RC_DRAW = 3,        /* prim, count, mask, stride | pad<<16, pad words, data[] : decoded vertices (see VA_*) */
  RC_TEX = 4,         /* slot, id                           : bind texture id to texmap slot */
  RC_TEXUP = 5,       /* id, w, h, levels, data             : RGBA8 texture (all mip levels) */
  RC_TEXFREE = 6,     /* id */
  RC_COPY = 7,        /* id(0=XFB), x, y, w, h, dw, dh, fmt, flags, addr : EFB copy (+clear flags) */
  RC_FRAME = 8,       /* vi frame number, xfb address        : a frame was scanned out */
  RC_CP = 9,          /* reg, value                         : CP register (matrix index A/B) */
  RC_READBACK = 10,   /* id, host address, w, h, copy fmt, flag address : write an EFB copy back to memory, then set *flag = 1 */
};
/* vertex attribute mask for RC_DRAW (attributes appear in this order, 4-byte aligned) */
#define VA_POS 0x0001     /* 3 x f32 */
#define VA_NRM 0x0002     /* 3 x f32 */
#define VA_NBT 0x0004     /* 6 x f32 (binormal, tangent) */
#define VA_COL0 0x0008    /* RGBA8 */
#define VA_COL1 0x0010    /* RGBA8 */
#define VA_TEX0 0x0020    /* 2 x f32 each, TEX0..TEX7 = bits 5..12 */
#define VA_PMI 0x2000     /* with VA_TMI: 12 bytes = pos/nrm matrix index, 8 tex matrix indices, 3 pad */
#define VA_TMI 0x4000

typedef struct GxRing {
  volatile u32 wpos, rpos;      /* byte offsets, modulo size */
  volatile u32 consumer;        /* set by the renderer when it is attached */
  u32 size;
  volatile u32 frames;          /* frame records written */
  u32 pad[3];
  u8 data[];
} GxRing;
extern GxRing *g_ring;
u32 *ring_alloc(u32 words);     /* reserve a record (blocks while the ring is full) */
void ring_commit(void);
