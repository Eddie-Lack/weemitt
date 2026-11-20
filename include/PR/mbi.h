/* Written for weemitt: the parts of the N64 system interface that the port's code uses. */
#ifndef WEEMITT_PR_MBI_H
#define WEEMITT_PR_MBI_H

/* Puts the low w bits of v at bit s of a 32-bit command word. The graphics and the audio command macros use it. */
#define _SHIFTL(v, s, w) ((unsigned int)(((unsigned int)(v) & ((1 << (w)) - 1)) << (s)))

#define G_OFF (0)
#define G_ON (1)

#include <PR/gbi.h>
#include <PR/abi.h>

/* The type of a task for the RSP that holds a display list. */
#define M_GFXTASK 1

#endif
