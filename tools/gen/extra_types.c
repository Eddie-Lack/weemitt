/* The types of data symbols that the game's C does not declare, for gen_types.py. Most are read only by the port's
   own code. Without this file the generator would take their layout from splat's directives. */
#include "ultra64.h"

/* libultra's sinf and cosf constants (src/port/libultra.c). */
extern f64 D_8010C218, D_8010C220, D_8010C228, D_8010C230, D_8010C238, D_8010C240, D_8010C248, D_8010C258,
    D_8010C260;
extern f32 D_8010C250, D_8010C270;
extern f64 D_8010C318, D_8010C320, D_8010C328, D_8010C330, D_8010C338, D_8010C340, D_8010C348, D_8010C358,
    D_8010C360;
extern f32 D_8010C350;

/* libmus's sound effect streams (D_801E7760 to the end of D_801E9C9C) are byte streams, not words. */
extern u8 D_801E7760[];

