/* The display lists for a host's own interpreter (gfx_hostgfx.c, glover_host_gfx in docs/library.md): the game's F3DEX
   lists translated into F3DEX2 commands of two pointer-sized words, with host pointers in place of the game's
   addresses. */
#ifndef GFX_HOSTGFX_H
#define GFX_HOSTGFX_H

#include <stdint.h>

#include "gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One F3DEX2 command, as the host interpreter's Gfx holds it. */
typedef struct GfxHostCmd {
    uintptr_t w0, w1;
} GfxHostCmd;

typedef struct GfxHostRequest {
    const GfxTask *tasks;
    int count;
    const GfxMark *marks;
    int markCount;
    unsigned int mask;           /* the GFX_LAYER_ bits whose primitives are drawn */
    int noFog;                   /* 1: the game's fog is left out */
    float t;                     /* 0 to 1: blended from the previous recorded frame; else the latest frame */
    const void *projection[4];   /* the host's matrices that replace each projection group, NULL-ended */
    unsigned int generation;     /* changes whenever the game's memory may have changed */
    int split;                   /* 1: the triangles that write no depth go to a second list, xlu */
    int hostFog;                 /* 1: every triangle takes the host's fog, fogColor and fogWord, not the game's */
    unsigned int fogColor;       /* the host's fog colour, as G_SETFOGCOLOR's second word */
    unsigned int fogWord;        /* the host's fog multiplier and offset, as G_MW_FOG's data */
} GfxHostRequest;

typedef struct GfxHostResult {
    const GfxHostCmd *list;      /* the entry list; it ends with G_ENDDL */
    const GfxHostCmd *xlu;       /* with split: the entry list of the triangles that write no depth, run after list */
    unsigned int commands, triangles, textures, skipped, unknown;
    unsigned int rects;          /* texture rectangles written as triangles */
} GfxHostResult;

/* Builds the lists. They and the blended matrices stay valid until the next call. Returns 0 when it could not
   allocate its memory. */
int gfx_host_build(const GfxHostRequest *rq, GfxHostResult *out);
/* Where the lists' addresses point (gfx_set_memory_range and gfx_set_memory_base), and the buffers the game rebuilds
   in every frame (gfx_interp_set_volatile). */
void gfx_host_setup(unsigned int lo, unsigned int hi, uintptr_t base, unsigned int volatileLo, unsigned int volatileHi);
/* Records a new frame's matrices for the blend: cut 1 when the frame does not follow the last. */
void gfx_host_record(const GfxTask *tasks, int count, int cut);
/* Frees the texture copies and the lists, and forgets every texture: the next game starts afresh. */
void gfx_host_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
