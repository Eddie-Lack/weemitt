/* The library's own declarations, shared by its files (docs/library.md). Not for hosts: they include glover.h. */
#ifndef LIB_H
#define LIB_H
#include <stdint.h>
#include "glover.h"
#include "frame.h"
#include "drawmark.h"

/* The jump from port_fatal back to the API call (glover.c, enter; host.c, lib_fatal). Clang has no
   __builtin_setjmp for arm64, so arm64 uses _setjmp, which leaves the signal mask alone. */
#if defined(__aarch64__)
#include <setjmp.h>
typedef jmp_buf LibJmp;
#define LIB_SETJMP(b) _setjmp(b)
#define LIB_LONGJMP(b) _longjmp(b, 1)
#else
typedef void *LibJmp[5];
#define LIB_SETJMP(b) __builtin_setjmp(b)
#define LIB_LONGJMP(b) __builtin_longjmp(b, 1)
#endif

struct Glover {
    LibJmp jmp;                    /* port_fatal jumps back to the API call */
    int busy;                      /* 1 while a call runs game code (enter) */
    int dead;                      /* the game stopped or was destroyed: enter refuses every call */
    char error[900];               /* the fatal message */
    unsigned char *rom;            /* the library's copy of the ROM */
    void *user;
    void (*log)(void *user, const char *line);
    uint64_t (*clockUs)(void *user);
    uint64_t clock;                /* the game clock in microseconds, without clockUs */
    uint32_t frames;               /* original frames since glover_create */
    uint32_t updates;              /* calls of glover_update */
    uint32_t fatalAt;              /* the test option: update number fatalAt stops the game (0: never) */
    int skipIntro;
    int ballworld;
    int pauseCheats;
    int cheatHold;
    int32_t musicVolume, effectsVolume;   /* GLOVER_OPT_MUSIC_VOLUME and GLOVER_OPT_EFFECTS_VOLUME, percent */
    PortFrameTask lists[GLOVER_MAX_LISTS];   /* the latest display lists, for glover_gl_draw */
    int listCount;
    DrawMark marks[DRAWMARK_MAX];  /* the marks of the latest lists (glover_draw_marks, the layer) */
    int markCount;
};

/* The one game of the process, or NULL. host.c's functions reach it here. */
extern Glover *lib_game;

/* glover.c: runs fn(arg) for the game, with the guard of every call (docs/library.md). */
int32_t enter(Glover *g, void (*fn)(void *), void *arg);

/* gl.c: the widescreen mode of the latest glover_gl_options (0 without the renderer). */
extern int lib_widescreen;

/* hostgfx.c: 1 once glover_host_gfx asked for blended frames; each update then records its new lists with
   lib_host_record. */
extern int lib_host_interp;
void lib_host_record(const PortFrameTask *tasks, int count, int cut);
/* Frees glover_host_gfx's lists and texture copies, and forgets its state (glover_destroy). */
void lib_host_shutdown(void);

/* host.c */
void lib_host_start(void);
extern int lib_audio_rate;

/* audio.c: the queue of sound frames between the game thread and glover_audio_read. */
void lib_audio_push(const short *frames, int count);
uint32_t lib_audio_queued(void);
uint32_t lib_audio_dropped(void);
int32_t lib_audio_read(int16_t *out, int32_t frames);

#endif
