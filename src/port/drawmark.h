/* The marks of a frame's display lists (drawmark.c, docs/library.md): where each part of the picture begins. Plain C
   types only: the library and the renderer include it without the game's headers. */
#ifndef DRAWMARK_H
#define DRAWMARK_H

/* The layers of a mark: glover.h's GLOVER_LAYER_ bits. 0 is the frame's setup: the clears, the matrices, the modes. */
#define DRAWMARK_SETUP 0x0
#define DRAWMARK_ACTORS 0x1
#define DRAWMARK_SCENERY 0x2
#define DRAWMARK_SKY 0x4
#define DRAWMARK_HUD 0x8
#define DRAWMARK_MAX 4096

/* From the command at the word addr of the game's memory on, the frame's commands belong to layer. The addresses
   rise through a frame. */
typedef struct DrawMark {
    unsigned int addr;
    unsigned int layer;
} DrawMark;

/* Turns the marks on or off (the hook in porthooks.h). */
void drawmark_enable(int on);
/* The marks of the frame that the latest update built, and their count. */
const DrawMark *drawmark_get(int *count);

#endif
