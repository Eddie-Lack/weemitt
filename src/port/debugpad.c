/* The game's debug features (docs/menu.md, "Game debug"). The retail game keeps debug code that reads a fifth
   controller entry, pad[4], which nothing fills. This file fills it when the Developer window asks. It also
   writes the input recordings that the debug code makes, and scales the timing bars. With the features off, none of
   this changes the game's memory. */
PORT_C_BEGIN
#include "port.h"
#include "common.h"
#include "controller.h"
#include "game.h"
#include "frame.h"

extern OSContPad controllerdata[];
extern s32 autoPlayback;
extern u8 timerMode;
extern u8 inertia;
extern s32 frameCount;
void ReadController(s32 cntlr);

/* ---- pad[4] */

u16 port_debug_buttons;
s8 port_debug_stick_x;
s8 port_debug_stick_y;

static s32 padOn;          /* the live input (port_debug_buttons) drives pad[4] */
static s32 padWasFilled;   /* pad[4] holds input from a read, and must be cleared when the reads stop */
static u16 padLatched;     /* buttons seen down in an update since the latest read */

/* An action is a short list of button words, one for each game frame. The last word is 0, so that every press ends. */
static const u16 seqPrevPlatform[] = {L_CBUTTONS, 0};
static const u16 seqNextPlatform[] = {R_CBUTTONS, 0};
static const u16 seqMarkLevels[] = {L_TRIG, 0};
static const u16 seqTiming[] = {Z_TRIG, 0};
static const u16 seqPrevSong[] = {L_JPAD, 0};
static const u16 seqNextSong[] = {R_JPAD, 0};
static const u16 seqInertia[] = {R_TRIG, 0};
/* The quit needs A held when B goes down. */
static const u16 seqQuit[] = {A_BUTTON, A_BUTTON | B_BUTTON, 0};
static const u16 seqHurt[] = {START_BUTTON, 0};
static const u16 seqAutoPlayback[] = {Z_TRIG, 0};

static const struct {
    const u16 *steps;
    s32 count;
} sequences[PORT_DBG_COUNT] = {
    [PORT_DBG_PREV_PLATFORM] = {seqPrevPlatform, 2},
    [PORT_DBG_NEXT_PLATFORM] = {seqNextPlatform, 2},
    [PORT_DBG_MARK_LEVELS] = {seqMarkLevels, 2},
    [PORT_DBG_TIMING] = {seqTiming, 2},
    [PORT_DBG_PREV_SONG] = {seqPrevSong, 2},
    [PORT_DBG_NEXT_SONG] = {seqNextSong, 2},
    [PORT_DBG_INERTIA] = {seqInertia, 2},
    [PORT_DBG_QUIT] = {seqQuit, 3},
    [PORT_DBG_HURT] = {seqHurt, 2},
    [PORT_DBG_AUTOPLAY] = {seqAutoPlayback, 2},
};

static const u16 *injectSteps;
static s32 injectCount;
static s32 injectPos;

/* pad[4] lies past the end of the array that the headers declare. The compiler is not told where it is. */
static Pad *debug_pad(void) {
    Pad *p = (Pad *)((u8 *)pad + 4 * sizeof(Pad));

    __asm__("" : "+r"(p));
    return p;
}

void port_debug_pad_mode(s32 on) {
    padOn = on != 0;
    padLatched = 0;
}

s32 port_debug_pad_is_on(void) {
    return padOn;
}

s32 port_debug_action(s32 action) {
    if (action < 0 || action >= PORT_DBG_COUNT || injectPos < injectCount) {
        return 0;
    }
    injectSteps = sequences[action].steps;
    injectCount = sequences[action].count;
    injectPos = 0;
    return 1;
}

s32 port_debug_busy(void) {
    return injectPos < injectCount;
}

/* Called for every update, before the game reads the controller. A press shorter than the time between two reads
   still counts. */
void port_debug_pad_update(void) {
    if (padOn) {
        padLatched |= port_debug_buttons;
    }
}

/* ControllerStep calls this right after it reads controller 1, as the debug ROM patch reads controller 2 there. The
   read is the game's own: ReadController builds the held buttons, the presses, the hold times and the stick from
   the button word, so pad[4] is filled as pad[0] is. ReadController works on the entry it is given, so the entry
   of controller 2, which the port never uses, takes pad[4]'s place for the call. */
void PortDebugPadStep(void) {
    Pad *dbg;
    Pad saved;
    OSContPad savedData;
    s32 mode;
    u16 buttons = 0;
    s32 sx = 0;
    s32 sy = 0;

    if (!padOn && injectPos >= injectCount) {
        if (padWasFilled) {
            memset(debug_pad(), 0, sizeof(Pad));
            padWasFilled = 0;
        }
        return;
    }
    if (padOn) {
        buttons = port_debug_buttons | padLatched;
        sx = port_debug_stick_x;
        sy = port_debug_stick_y;
        padLatched = 0;
    }
    if (injectPos < injectCount) {
        buttons |= injectSteps[injectPos++];
    }
    dbg = debug_pad();
    saved = pad[1];
    savedData = controllerdata[1];
    pad[1] = *dbg;
    controllerdata[1].button = buttons;
    controllerdata[1].stick_x = (s8)sx;
    controllerdata[1].stick_y = (s8)sy;
    /* The recorder of the debug code (autoPlayback 1) must not see this pad. */
    mode = autoPlayback;
    autoPlayback = 0;
    ReadController(1);
    autoPlayback = mode;
    *dbg = pad[1];
    pad[1] = saved;
    controllerdata[1] = savedData;
    padWasFilled = 1;
}

/* ---- Which screen the game shows, for the window. */

static u32 titleReads;

/* MainTitleWait calls this in the update that checks pad[4]'s Z. */
void PortDebugTitleWait(void) {
    titleReads = port_controller_reads;
}

s32 port_debug_on_title(void) {
    return titleReads != 0 && port_controller_reads - titleReads <= 1;
}

s32 port_debug_in_level(void) {
    return gameInfo.mode == 4 || gameInfo.mode == 5;
}

s32 port_debug_autoplayback(void) {
    return autoPlayback;
}

s32 port_debug_timer_mode(void) {
    return timerMode;
}

s32 port_debug_inertia(void) {
    return inertia;
}

/* ---- The timing bars. The bars show 20000 ticks to the pixel, which is 0.43 ms. The PC does a frame's work in
   a few milliseconds, so the window can magnify the time. */

static u32 timerScale = 1;

void port_debug_timer_scale(s32 scale) {
    timerScale = scale < 1 ? 1 : (u32)scale;
}

u32 PortTimerCount(void) {
    return osGetCount() * timerScale;
}

/* ---- The recordings. The debug code writes a level's recording to the development PC with PCcreat, PCwrite and
   PCclose, which SN's libsn provided. Here the file goes to the save folder under the name that the game gave it,
   without the PC's path. A recording is an N64 file: a size word and then 16-bit pairs, big-endian. */

#define PC_FILES 4

static struct {
    FILE *f;
    s32 moo;
    s32 pos;
    char path[1024];
} pcFiles[PC_FILES];

/* Set by the platform layer at start-up. The brute forcer leaves it empty. */
char port_debug_folder[900];

s32 PCcreat(char *name, s32 mode) {
    char base[128];
    const char *p;
    const char *start = name;
    s32 i;
    s32 n = 0;

    for (p = name; *p; p++) {
        if (*p == '\\' || *p == '/' || *p == ':') {
            start = p + 1;
        }
    }
    for (p = start; *p && n < (s32)sizeof(base) - 1; p++) {
        base[n++] = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_'
                            || *p == '-' || *p == '.'
                        ? *p
                        : '_';
    }
    base[n] = 0;
    if (n == 0) {
        return -1;
    }
    for (i = 0; i < PC_FILES; i++) {
        if (pcFiles[i].f == NULL) {
            break;
        }
    }
    if (i == PC_FILES) {
        return -1;
    }
    snprintf(pcFiles[i].path, sizeof(pcFiles[i].path), "%s%s", port_debug_folder, base);
    pcFiles[i].f = fopen(pcFiles[i].path, "wb");
    if (pcFiles[i].f == NULL) {
        port_log("PCcreat: cannot write %s", pcFiles[i].path);
        return -1;
    }
    pcFiles[i].moo = n > 4 && !strcmp(base + n - 4, ".moo");
    pcFiles[i].pos = 0;
    return i + 1;
}

s32 PCwrite(s32 fd, void *buf, s32 len) {
    u8 *copy;
    s32 i = 0;
    s32 ok;

    if (fd < 1 || fd > PC_FILES || pcFiles[fd - 1].f == NULL || len < 0) {
        return -1;
    }
    copy = (u8 *)malloc(len + 4);
    if (copy == NULL) {
        return -1;
    }
    memcpy(copy, buf, len);
    if (pcFiles[fd - 1].moo) {
        if (pcFiles[fd - 1].pos == 0 && len >= 4) {
            *(u32 *)copy = port_bswap32(*(u32 *)copy);
            i = 4;
        }
        for (; i + 2 <= len; i += 2) {
            *(u16 *)(copy + i) = port_bswap16(*(u16 *)(copy + i));
        }
    }
    ok = fwrite(copy, 1, len, pcFiles[fd - 1].f) == (size_t)len;
    free(copy);
    pcFiles[fd - 1].pos += len;
    return ok ? len : -1;
}

s32 PCclose(s32 fd) {
    /* The game also closes its file handle when it leaves a recorded level without saving: nothing is open then. */
    if (fd < 1 || fd > PC_FILES || pcFiles[fd - 1].f == NULL) {
        return -1;
    }
    fclose(pcFiles[fd - 1].f);
    pcFiles[fd - 1].f = NULL;
    port_log("wrote %s (%d bytes)", pcFiles[fd - 1].path, (int)pcFiles[fd - 1].pos);
    return 0;
}

PORT_C_END
