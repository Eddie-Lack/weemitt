/* Stops inside a logic frame (engine.h, engine_logic_stops).

   On the N64 a frame message can come while the game thread is in the middle of a logic frame. The controller and
   draw threads have higher priority, so the read and the draw then run at that point of the frame (docs/sync.md).
   The engine runs a logic frame as one call, so it cannot stop just anywhere. It stops at calls of the functions in
   hooks.def. The Makefile links the game's objects into game.o with --wrap: the game's calls of NAME then go to
   __wrap_NAME here, while NAME itself stays the game's function, which the hook calls. The hooks keep a stack of
   the hooked calls in progress. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"

#define MAX_DEPTH 32

static const char *const hookNames[] = {
#define HOOK0(name) #name,
#define OFF0(name)
#define DRAW0(name)
#define SKIP0(name, params, args)
#define SKIPP(name, params, args)
#define HOOK1(name) #name,
#define HOOKA(name, params, args) #name,
#include "hooks.def"
#undef HOOK0
#undef HOOK1
#undef HOOKA
#undef OFF0
#undef DRAW0
#undef SKIP0
#undef SKIPP
};

enum {
#define HOOK0(name) H_##name,
#define OFF0(name)
#define DRAW0(name)
#define SKIP0(name, params, args)
#define SKIPP(name, params, args)
#define HOOK1(name) H_##name,
#define HOOKA(name, params, args) H_##name,
#include "hooks.def"
#undef HOOK0
#undef HOOK1
#undef HOOKA
#undef OFF0
#undef DRAW0
#undef SKIP0
#undef SKIPP
    H_COUNT
};

/* The hooked calls in progress. */
static int depth;
static int stackHook[MAX_DEPTH];
static uint32_t stackArg[MAX_DEPTH];

/* The stops of the logic frame that runs now. */
static Engine *stopEngine;
static const EngineStop *stops;
static int stopCount;
static int nextStop;
static int seen; /* matching calls of the next stop that came before its nth */
static EngineStopFn stopFn;
static void *stopCtx;
/* $GLOVER_STOP_TRACE set: print the hooked calls of each logic frame with stops, and the stops, on standard error. */
static int trace = -1;

int engine_hook_index(const char *name) {
    int i;

    for (i = 0; i < H_COUNT; i++) {
        if (!strcmp(hookNames[i], name)) {
            return i;
        }
    }
    return -1;
}

const char *engine_hook_name(int index) {
    return index >= 0 && index < H_COUNT ? hookNames[index] : NULL;
}

/* 1 when the hooked calls in progress are the stop's. */
static int matches(const EngineStop *s) {
    int i;

    if (s->depth != depth) {
        return 0;
    }
    for (i = 0; i < depth; i++) {
        if (s->hook[i] != stackHook[i] || (s->arg[i] != ENGINE_ANY && s->arg[i] != stackArg[i])) {
            return 0;
        }
    }
    return 1;
}

/* Runs the stops that are due where the hooked call `leave` (0: starts, 1: returns). Several stops can fall on the
   same call. */
static void check(int leave) {
    while (nextStop < stopCount) {
        const EngineStop *s = &stops[nextStop];

        if (s->leave != leave || !matches(s)) {
            return;
        }
        if (seen < s->nth) {
            seen++;
            return;
        }
        nextStop++;
        seen = 0;
        if (trace) {
            fprintf(stderr, "stop %d\n", nextStop - 1);
        }
        stopFn(stopEngine, stopCtx, nextStop - 1);
    }
}

static void enter(int hook, const void *arg) {
    if (depth < MAX_DEPTH) {
        stackHook[depth] = hook;
        stackArg[depth] = arg != NULL && stopEngine != NULL ? engine_host_to_n64(arg) : 0;
    }
    depth++;
    if (nextStop < stopCount) {
        if (trace) {
            fprintf(stderr, "%*s%s 0x%08x\n", 2 * depth, "", hookNames[hook], (unsigned)stackArg[depth - 1]);
        }
        check(0);
    }
}

static void leave(void) {
    if (nextStop < stopCount) {
        check(1);
    }
    depth--;
}

#define HOOK0(name)            \
    void name(void);           \
    void __wrap_##name(void);  \
    void __wrap_##name(void) { \
        enter(H_##name, NULL); \
        name();                \
        leave();               \
    }
#define HOOK1(name)                 \
    void name(void *arg);           \
    void __wrap_##name(void *arg);  \
    void __wrap_##name(void *arg) { \
        enter(H_##name, arg);       \
        name(arg);                  \
        leave();                    \
    }
#define HOOKA(name, params, args)  \
    void name params;              \
    void __wrap_##name params;     \
    void __wrap_##name params {    \
        enter(H_##name, NULL);     \
        name args;                 \
        leave();                   \
    }
/* The trimmed draw: on while engine_trim_draw says so and the game is inside the draw. */
static int trimDraw;
static int inDraw;

void engine_trim_draw(Engine *e, int on) {
    (void)e;
    trimDraw = on;
}

#define DRAW0(name)            \
    void name(void);           \
    void __wrap_##name(void);  \
    void __wrap_##name(void) { \
        inDraw = 1;            \
        name();                \
        inDraw = 0;            \
    }
#define SKIP0(name, params, args) \
    void name params;             \
    void __wrap_##name params;    \
    void __wrap_##name params {   \
        if (!(trimDraw && inDraw)) { \
            name args;            \
        }                         \
    }
#define SKIPP(name, params, args)  \
    void *name params;             \
    void *__wrap_##name params;    \
    void *__wrap_##name params {   \
        return trimDraw && inDraw ? NULL : name args; \
    }
/* weemitt's cheat list in the pause menu (R or Z) is not in the N64 game: the engine keeps it closed. */
#define OFF0(name)                \
    int __wrap_##name(void);      \
    int __wrap_##name(void) {     \
        return 0;                 \
    }
#include "hooks.def"
#undef HOOK0
#undef HOOK1
#undef HOOKA
#undef OFF0
#undef DRAW0
#undef SKIP0
#undef SKIPP

int engine_logic_stops(Engine *e, const EngineStop *list, int count, EngineStopFn fn, void *ctx) {
    int missed;

    if (trace < 0) {
        trace = getenv("GLOVER_STOP_TRACE") != NULL;
    }
    depth = 0;
    stopEngine = e;
    stops = list;
    stopCount = count;
    nextStop = 0;
    seen = 0;
    stopFn = fn;
    stopCtx = ctx;
    engine_logic(e);
    missed = stopCount - nextStop;
    if (trace && missed) {
        int i;
        int j;

        for (i = nextStop; i < stopCount; i++) {
            fprintf(stderr, "missed stop %d:", i);
            for (j = 0; j < stops[i].depth; j++) {
                fprintf(stderr, " %s 0x%08x", hookNames[stops[i].hook[j]], (unsigned)stops[i].arg[j]);
            }
            fprintf(stderr, " nth %d leave %d\n", stops[i].nth, stops[i].leave);
        }
    }
    stopEngine = NULL;
    stops = NULL;
    stopCount = 0;
    nextStop = 0;
    return missed;
}
