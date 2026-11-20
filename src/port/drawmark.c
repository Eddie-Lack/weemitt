/* The marks of a frame's display lists (drawmark.h, docs/library.md). A host that draws only some of the picture
   needs to know which commands draw what. The game calls port_draw_mark_hook (porthooks.h) where each part of the
   frame begins: the setup, the backdrops, the world, each actor of the drawing list, and the HUD. The marks keep the
   address of the next command and its layer. They live outside the game's memory, and the game never reads them. */
PORT_C_BEGIN
#include "port.h"
#include "common.h"
#include "actor.h"
#include "display.h"
#include "porthooks.h"
#include "drawmark.h"

static DrawMark marks[DRAWMARK_MAX];
static int markCount;
static int full;

static void Mark(unsigned int layer) {
    unsigned int addr = PORT_P2W((Gfx *)glistp);

    /* The last two entries are kept: one for the rest of the world when the table fills up, and one for the HUD.
       That rest counts as both actors and scenery. */
    if (layer & (DRAWMARK_ACTORS | DRAWMARK_SCENERY)) {
        if (full) {
            return;
        }
        if (markCount >= DRAWMARK_MAX - 2) {
            layer = DRAWMARK_ACTORS | DRAWMARK_SCENERY;
            full = 1;
        }
    }
    if (markCount > 0 && marks[markCount - 1].layer == layer) {
        return;
    }
    /* Nothing was drawn since the last mark: it changes its layer. */
    if (markCount > 0 && marks[markCount - 1].addr == addr) {
        marks[markCount - 1].layer = layer;
        if (markCount > 1 && marks[markCount - 2].layer == layer) {
            markCount--;
        }
        return;
    }
    if (markCount < DRAWMARK_MAX) {
        marks[markCount].addr = addr;
        marks[markCount].layer = layer;
        markCount++;
    }
}

static void Hook(s32 kind, struct Actor *actor) {
    switch (kind) {
    case PORT_DRAW_FRAME:
        markCount = 0;
        full = 0;
        Mark(DRAWMARK_SETUP);
        break;
    case PORT_DRAW_SKY:
        Mark(DRAWMARK_SKY);
        break;
    case PORT_DRAW_WORLD:
    case PORT_DRAW_ACTOR_END:
        Mark(DRAWMARK_ACTORS);
        break;
    case PORT_DRAW_ACTOR:
        Mark(actor->type == LANDSCAPE_ACTOR || actor->type == WATER_ACTOR || actor->type == PLAT_ACTOR
                 ? DRAWMARK_SCENERY
                 : DRAWMARK_ACTORS);
        break;
    case PORT_DRAW_HUD:
        Mark(DRAWMARK_HUD);
        break;
    }
}

void drawmark_enable(int on) {
    port_draw_mark_hook = on ? Hook : NULL;
    markCount = 0;
    full = 0;
}

const DrawMark *drawmark_get(int *count) {
    *count = markCount;
    return marks;
}
PORT_C_END
