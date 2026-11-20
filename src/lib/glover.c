/* The library's interface (glover.h, docs/library.md). Each call that runs the game's code or reads its state goes
   through enter, which guards against re-entry, sets the floating-point mode the game expects and catches a fatal
   error.

   Two hidden options for the tests (docs/testing.md): glover_set_option(g, 0x7F000001, n) makes update number n
   stop the game with port_fatal("test"), and glover_set_option(NULL, 0x7F000002, 1) makes the next glover_create
   stop it the same way while it starts. */
#include <stdlib.h>
#include <string.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif
#include "lib.h"
#include "ptr32.h"
#include "rom.h"
#include "statehash.h"
#include "hostworld.h"

#define OPT_TEST_FATAL 0x7F000001
#define OPT_TEST_FATAL_START 0x7F000002

#ifndef GLOVER_BUILD_ID
#define GLOVER_BUILD_ID "unknown"
#endif
#ifndef GLOVER_RENDERER
#define GLOVER_RENDERER "null"
#endif
#if defined(_WIN64)
#define GLOVER_SYSTEM "windows64"
#elif defined(_WIN32)
#define GLOVER_SYSTEM "windows32"
#elif defined(__APPLE__) && defined(GLOVER_APPLE_SYSTEM)
#define GLOVER_SYSTEM GLOVER_APPLE_SYSTEM
#elif defined(__LP64__)
#define GLOVER_SYSTEM "linux64"
#else
#define GLOVER_SYSTEM "linux32"
#endif
#ifdef PORT_AUDIO_MUS
#define GLOVER_SOUND "mus"
/* src/audio/mus/mus-voice.c: the volume of the songs and of the sound effects, in percent, outside the game's
   memory. */
void musSetGroupGain(int song, int effect);
#else
#define GLOVER_SOUND "stub"
#endif

#if defined(__aarch64__)
/* arm64: the FPCR. The default is 0: round to nearest, no flush-to-zero, NaNs propagated, no traps. */
typedef unsigned long fp_mode;
#define FP_MODE_DEFAULT 0
static fp_mode fp_mode_get(void) {
    fp_mode v;

    __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    return v;
}
static void fp_mode_set(fp_mode v) {
    __asm__ volatile("msr fpcr, %0" : : "r"(v));
}
#else
/* x86: the MXCSR. The default: all exceptions masked, round to nearest, no flush-to-zero. */
typedef unsigned int fp_mode;
#define FP_MODE_DEFAULT 0x1F80
#define fp_mode_get() _mm_getcsr()
#define fp_mode_set(v) _mm_setcsr(v)
#endif

Glover *lib_game;
static int created;
static int testFatalStart;

/* Runs fn(arg) for the game. Returns GLOVER_OK, or an error when the call cannot run or the game stopped in it. */
int32_t enter(Glover *g, void (*fn)(void *), void *arg) {
    fp_mode csr;
    int32_t rc = GLOVER_OK;

    if (g == NULL || g != lib_game) {
        return GLOVER_ERR_ARG;
    }
    if (g->dead) {
        return GLOVER_ERR_DEAD;
    }
    if (__atomic_exchange_n(&g->busy, 1, __ATOMIC_ACQUIRE)) {
        return GLOVER_ERR_REENTER;
    }
    csr = fp_mode_get();
    fp_mode_set(FP_MODE_DEFAULT);
    if (LIB_SETJMP(g->jmp) == 0) {
        fn(arg);
    } else {
        g->dead = 1;
        rc = GLOVER_ERR_FATAL;
    }
    fp_mode_set(csr);
    __atomic_store_n(&g->busy, 0, __ATOMIC_RELEASE);
    return rc;
}

GLOVER_API uint32_t glover_api_version(void) {
    return GLOVER_API_VERSION;
}

GLOVER_API const char *glover_build_id(void) {
    return GLOVER_BUILD_ID " " GLOVER_SYSTEM " " GLOVER_SOUND " " GLOVER_RENDERER;
}

GLOVER_API void glover_default_config(GloverConfig *c) {
    if (c == NULL) {
        return;
    }
    memset(c, 0, sizeof(*c));
    c->size = sizeof(*c);
    c->apiVersion = GLOVER_API_VERSION;
    c->startLevel = -1;
}

static void set_error(char *err, uint32_t errSize, const char *msg) {
    if (err != NULL && errSize > 0) {
        size_t n = strlen(msg);

        if (n >= errSize) {
            n = errSize - 1;
        }
        memcpy(err, msg, n);
        err[n] = 0;
    }
}

static void free_rom(Glover *g) {
    port_rom = NULL;
    port_rom_size = 0;
    free(g->rom);
    g->rom = NULL;
}

static void call_init(void *arg) {
    Glover *g = lib_game;

    port_set_skip_intro(g->skipIntro);
    port_set_pause_cheats(g->pauseCheats);
    port_set_cheat_hold(g->cheatHold);
    port_set_ballworld(g->ballworld);
    port_save_ballworld = g->ballworld;
    if (testFatalStart) {
        port_fatal("test");
    }
    port_game_init(*(int *)arg);
}

GLOVER_API int32_t glover_create(const GloverConfig *c, Glover **out, char *err, uint32_t errSize) {
    Glover *g;
    char msg[256];
    int level;
    int32_t ballworld;
    int32_t rc;

    if (out != NULL) {
        *out = NULL;
    }
    /* A host built before ballworld was added gives the shorter size. */
    if (c == NULL || out == NULL || c->size < offsetof(GloverConfig, ballworld)) {
        set_error(err, errSize, "a bad argument or structure size");
        return GLOVER_ERR_ARG;
    }
    ballworld = c->size >= offsetof(GloverConfig, ballworld) + sizeof(c->ballworld) ? c->ballworld : 0;
    if (c->apiVersion < 1 || c->apiVersion > GLOVER_API_VERSION) {
        set_error(err, errSize, "the host was built for another GLOVER_API_VERSION");
        return GLOVER_ERR_VERSION;
    }
    if (c->rom == NULL || c->romSize == 0) {
        set_error(err, errSize, "no ROM");
        return GLOVER_ERR_ARG;
    }
    if (c->startLevel < -1 || c->startLevel >= port_level_count()) {
        set_error(err, errSize, "no such start level");
        return GLOVER_ERR_ARG;
    }
    if (ballworld != 0 && ballworld != 1) {
        set_error(err, errSize, "ballworld is not 0 or 1");
        return GLOVER_ERR_ARG;
    }
    if (__atomic_exchange_n(&created, 1, __ATOMIC_ACQ_REL)) {
        set_error(err, errSize, "a game was already created in this process");
        return GLOVER_ERR_BUSY;
    }
    g = (Glover *)calloc(1, sizeof(*g));
    if (g != NULL) {
        g->rom = (unsigned char *)malloc(c->romSize);
    }
    if (g == NULL || g->rom == NULL) {
        free(g);
        __atomic_store_n(&created, 0, __ATOMIC_RELEASE);
        set_error(err, errSize, "out of memory");
        return GLOVER_ERR_MEMORY;
    }
    memcpy(g->rom, c->rom, c->romSize);
    if (port_rom_prepare(g->rom, c->romSize, msg + 4, sizeof(msg) - 4) != 0) {
        memcpy(msg, "ROM ", 4);
        free(g->rom);
        free(g);
        __atomic_store_n(&created, 0, __ATOMIC_RELEASE);
        set_error(err, errSize, msg);
        return GLOVER_ERR_ROM;
    }
    g->user = c->user;
    g->log = c->log;
    g->clockUs = c->clockUs;
    g->pauseCheats = 1;
    g->musicVolume = 100;
    g->effectsVolume = 100;
    port_set_camera_dist_scale(1.0f);
    g->ballworld = ballworld;
    g->listCount = 0;
    if (c->eeprom != NULL) {
        memcpy(port_eeprom, c->eeprom, sizeof(port_eeprom));
    }
    port_rom = g->rom;
    port_rom_size = c->romSize;
    lib_game = g;
    lib_host_start();
    drawmark_enable(1);
    level = c->startLevel;
    /* The host has the game during the start too: a log callback may get GLOVER_ERR_REENTER for it. */
    *out = g;
    rc = enter(g, call_init, &level);
    if (rc != GLOVER_OK) {
        *out = NULL;
        set_error(err, errSize, g->error[0] ? g->error : "the game stopped while it started");
        /* The game's globals are no longer as at the start, so this process cannot start another game. The structure
           stays, dead, as after glover_destroy. */
        free_rom(g);
        return rc;
    }
    set_error(err, errSize, "");
    return GLOVER_OK;
}

GLOVER_API int32_t glover_destroy(Glover *g) {
    if (g == NULL || g != lib_game) {
        return GLOVER_ERR_ARG;
    }
    if (__atomic_exchange_n(&g->busy, 1, __ATOMIC_ACQUIRE)) {
        return GLOVER_ERR_REENTER;
    }
    g->dead = 1;
    free_rom(g);
    hw_forget();
    drawmark_enable(0);
    lib_host_shutdown();
    /* The game's memory stays as it was; this version runs one game per process. The structure stays too, so that
       a stale pointer finds a dead game. */
    __atomic_store_n(&g->busy, 0, __ATOMIC_RELEASE);
    return GLOVER_OK;
}

GLOVER_API const char *glover_error(Glover *g) {
    if (g == NULL || g != lib_game) {
        return "";
    }
    return g->error;
}

typedef struct UpdateCall {
    Glover *g;
    float dt;
    const GloverInput *in;
    GloverFrame *out;
} UpdateCall;

static void call_update(void *arg) {
    UpdateCall *u = (UpdateCall *)arg;
    Glover *g = u->g;
    PortFrameTask tasks[GLOVER_MAX_LISTS];
    int n;
    int ticked;
    int cut = 0;

    g->updates++;
    if (g->fatalAt != 0 && g->updates == g->fatalAt) {
        port_fatal("test");
    }
    port_buttons = (unsigned short)((u->in ? u->in->buttons : 0) | port_intro_skip_buttons());
    port_stick_x = u->in ? u->in->stickX : 0;
    port_stick_y = u->in ? u->in->stickY : 0;
    n = port_game_update(u->dt, tasks, GLOVER_MAX_LISTS);
    ticked = port_game_ticked();
    if (ticked) {
        cut = port_game_cut();
        g->frames++;
    }
    if (n >= 0) {
        const DrawMark *m = drawmark_get(&g->markCount);

        memcpy(g->lists, tasks, sizeof(tasks[0]) * n);
        g->listCount = n;
        memcpy(g->marks, m, sizeof(m[0]) * g->markCount);
        if (lib_host_interp && n > 0) {
            lib_host_record(tasks, n, cut);
        }
    }
    g->clock += (uint64_t)((double)u->dt * port_game_retraces() * 1000000.0 / 60.0);
    if (u->out != NULL) {
        u->out->listCount = n;
        if (n > 0) {
            memcpy(u->out->lists, tasks, sizeof(tasks[0]) * n);
        }
        u->out->ticked = ticked;
        u->out->cut = cut;
    }
}

GLOVER_API int32_t glover_update(Glover *g, float dt, const GloverInput *in, GloverFrame *out) {
    UpdateCall u;

    if (!(dt > 0.0f && dt <= 1.0f) || (out != NULL && out->size < sizeof(GloverFrame))) {
        return GLOVER_ERR_ARG;
    }
    u.g = g;
    u.dt = dt;
    u.in = in;
    u.out = out;
    return enter(g, call_update, &u);
}

static void call_status(void *arg) {
    GloverStatus *s = (GloverStatus *)arg;

    s->mode = port_game_mode();
    s->level = port_game_level();
    s->demo = port_game_demo();
    s->retraces = port_game_retraces();
    s->gotoPossible = port_goto_possible();
    s->frames = lib_game->frames;
    s->buttonsRead = port_buttons_read;
    s->reserved = 0;
    s->controllerReads = port_controller_reads;
    s->audioQueued = lib_audio_queued();
    s->audioDropped = lib_audio_dropped();
}

typedef struct MarksCall {
    GloverDrawMark *out;
    uint32_t max;
    uint32_t *count;
} MarksCall;

static void call_marks(void *arg) {
    MarksCall *c = (MarksCall *)arg;
    Glover *g = lib_game;
    uint32_t i;

    for (i = 0; i < (uint32_t)g->markCount && i < c->max; i++) {
        c->out[i].addr = g->marks[i].addr;
        c->out[i].layer = g->marks[i].layer;
    }
    *c->count = (uint32_t)g->markCount;
}

GLOVER_API int32_t glover_draw_marks(Glover *g, GloverDrawMark *out, uint32_t max, uint32_t *count) {
    MarksCall c;

    if (count == NULL || (out == NULL && max > 0)) {
        return GLOVER_ERR_ARG;
    }
    c.out = out;
    c.max = max;
    c.count = count;
    return enter(g, call_marks, &c);
}

GLOVER_API int32_t glover_status(Glover *g, GloverStatus *s) {
    if (s == NULL || s->size < sizeof(GloverStatus)) {
        return GLOVER_ERR_ARG;
    }
    return enter(g, call_status, s);
}

/* The size of version 1's GloverBodies: the size and three positions. */
#define BODIES_V1_SIZE 40

static void call_bodies(void *arg) {
    GloverBodies *b = (GloverBodies *)arg;
    GloverBodies full;
    HwBodies h;

    hw_bodies(&h);
    memcpy(full.hand, h.hand, sizeof(full.hand));
    memcpy(full.ball, h.ball, sizeof(full.ball));
    memcpy(full.camera, h.camera, sizeof(full.camera));
    memcpy(full.handVel, h.handVel, sizeof(full.handVel));
    memcpy(full.ballVel, h.ballVel, sizeof(full.ballVel));
    full.handYaw = h.handYaw;
    full.handAction = h.handAction;
    full.ballHeld = h.ballHeld;
    full.handOnGround = h.handOnGround;
    full.ballOnGround = h.ballOnGround;
    full.handTerrain = h.handTerrain;
    full.ballTerrain = h.ballTerrain;
    full.ready = h.ready;
    full.restarts = h.restarts;
    full.handVoid = h.handVoid;
    full.ballVoid = h.ballVoid;
    full.handVoids = h.handVoids;
    full.ballVoids = h.ballVoids;
    memcpy(full.handSafe, h.handSafe, sizeof(full.handSafe));
    memcpy(full.ballSafe, h.ballSafe, sizeof(full.ballSafe));
    full.safeKnown = h.safeKnown;
    full.handWater = h.handWater;
    full.ballWater = h.ballWater;
    /* A host of an earlier version gets the part that its structure holds. */
    memcpy((char *)b + 4, (char *)&full + 4, (b->size < sizeof(full) ? b->size : sizeof(full)) - 4);
}

GLOVER_API int32_t glover_bodies(Glover *g, GloverBodies *b) {
    if (b == NULL || b->size < BODIES_V1_SIZE) {
        return GLOVER_ERR_ARG;
    }
    return enter(g, call_bodies, b);
}

typedef struct OptionCall {
    int32_t option;
    double value;
    int32_t rc;
} OptionCall;

static void call_set_option(void *arg) {
    OptionCall *o = (OptionCall *)arg;
    Glover *g = lib_game;

    o->rc = GLOVER_OK;
    switch (o->option) {
    case GLOVER_OPT_EASY_BALL:
        port_set_easy_ball(o->value != 0.0);
        break;
    case GLOVER_OPT_CAMERA_FOLLOW:
        if (!(o->value >= 1.0 && o->value <= 3.0)) {
            o->rc = GLOVER_ERR_ARG;
            break;
        }
        port_set_camera_follow((float)o->value);
        break;
    case GLOVER_OPT_SKIP_INTRO:
        g->skipIntro = o->value != 0.0;
        port_set_skip_intro(g->skipIntro);
        break;
    case GLOVER_OPT_PAUSE_CHEATS:
        g->pauseCheats = o->value != 0.0;
        port_set_pause_cheats(g->pauseCheats);
        break;
    case GLOVER_OPT_CHEAT_HOLD:
        g->cheatHold = (int)o->value;
        port_set_cheat_hold(g->cheatHold);
        break;
    case GLOVER_OPT_MUSIC_VOLUME:
    case GLOVER_OPT_EFFECTS_VOLUME:
        if (!(o->value >= 0.0 && o->value <= 100.0)) {
            o->rc = GLOVER_ERR_ARG;
            break;
        }
        if (o->option == GLOVER_OPT_MUSIC_VOLUME) {
            g->musicVolume = (int32_t)(o->value + 0.5);
        } else {
            g->effectsVolume = (int32_t)(o->value + 0.5);
        }
#ifdef PORT_AUDIO_MUS
        musSetGroupGain(g->musicVolume, g->effectsVolume);
#endif
        break;
    case GLOVER_OPT_CAMERA_DISTANCE_SCALE:
        if (!(o->value >= 0.01 && o->value <= 100.0)) {
            o->rc = GLOVER_ERR_ARG;
            break;
        }
        port_set_camera_dist_scale((float)o->value);
        break;
    case OPT_TEST_FATAL:
        g->fatalAt = (uint32_t)o->value;
        break;
    default:
        o->rc = GLOVER_ERR_ARG;
        break;
    }
}

GLOVER_API int32_t glover_set_option(Glover *g, int32_t option, double value) {
    OptionCall o;
    int32_t rc;

    if (g == NULL && option == OPT_TEST_FATAL_START) {
        testFatalStart = value != 0.0;
        return GLOVER_OK;
    }
    o.option = option;
    o.value = value;
    o.rc = GLOVER_OK;
    rc = enter(g, call_set_option, &o);
    return rc != GLOVER_OK ? rc : o.rc;
}

static void call_get_option(void *arg) {
    OptionCall *o = (OptionCall *)arg;
    Glover *g = lib_game;

    o->rc = GLOVER_OK;
    switch (o->option) {
    case GLOVER_OPT_EASY_BALL:
        o->value = port_get_easy_ball();
        break;
    case GLOVER_OPT_CAMERA_FOLLOW:
        o->value = port_get_camera_follow();
        break;
    case GLOVER_OPT_SKIP_INTRO:
        o->value = g->skipIntro;
        break;
    case GLOVER_OPT_PAUSE_CHEATS:
        o->value = g->pauseCheats;
        break;
    case GLOVER_OPT_CHEAT_HOLD:
        o->value = g->cheatHold;
        break;
    case GLOVER_OPT_BALLWORLD:
        o->value = g->ballworld;
        break;
    case GLOVER_OPT_MUSIC_VOLUME:
        o->value = g->musicVolume;
        break;
    case GLOVER_OPT_EFFECTS_VOLUME:
        o->value = g->effectsVolume;
        break;
    case GLOVER_OPT_CAMERA_DISTANCE_SCALE:
        o->value = port_get_camera_dist_scale();
        break;
    default:
        o->rc = GLOVER_ERR_ARG;
        break;
    }
}

GLOVER_API int32_t glover_get_option(Glover *g, int32_t option, double *value) {
    OptionCall o;
    int32_t rc;

    if (value == NULL) {
        return GLOVER_ERR_ARG;
    }
    o.option = option;
    o.value = 0.0;
    o.rc = GLOVER_OK;
    rc = enter(g, call_get_option, &o);
    if (rc != GLOVER_OK) {
        return rc;
    }
    if (o.rc == GLOVER_OK) {
        *value = o.value;
    }
    return o.rc;
}

typedef struct GotoCall {
    int32_t level;
    int32_t rc;
} GotoCall;

static void call_goto(void *arg) {
    GotoCall *c = (GotoCall *)arg;

    if (!port_goto_possible()) {
        c->rc = GLOVER_ERR_ARG;
        return;
    }
    port_goto_level(c->level);
}

GLOVER_API int32_t glover_goto_level(Glover *g, int32_t level) {
    GotoCall c;
    int32_t rc;

    if (level < 0 || level >= port_goto_level_count()) {
        return GLOVER_ERR_ARG;
    }
    c.level = level;
    c.rc = GLOVER_OK;
    rc = enter(g, call_goto, &c);
    return rc != GLOVER_OK ? rc : c.rc;
}

typedef struct EepromCall {
    uint8_t *out;
    int32_t changed;
} EepromCall;

static void call_eeprom(void *arg) {
    EepromCall *e = (EepromCall *)arg;

    memcpy(e->out, port_eeprom, sizeof(port_eeprom));
    e->changed = port_eeprom_dirty != 0;
    port_eeprom_dirty = 0;
}

GLOVER_API int32_t glover_eeprom(Glover *g, uint8_t out[512]) {
    EepromCall e;
    int32_t rc;

    if (out == NULL) {
        return GLOVER_ERR_ARG;
    }
    e.out = out;
    e.changed = 0;
    rc = enter(g, call_eeprom, &e);
    return rc != GLOVER_OK ? rc : e.changed;
}

extern unsigned char port_ram[];

static void call_memory(void *arg) {
    GloverMemory *m = (GloverMemory *)arg;

    m->ram = port_ram;
    m->ramSize = 0x1800000;
    m->wordBase = PORT_PTR_BASE;
    port_game_framebuffers(&m->framebuffer[0], &m->framebuffer[1]);
    port_game_dynamic_range(&m->dynamicLo, &m->dynamicHi);
}

GLOVER_API int32_t glover_memory(Glover *g, GloverMemory *m) {
    if (m == NULL || m->size < sizeof(GloverMemory)) {
        return GLOVER_ERR_ARG;
    }
    return enter(g, call_memory, m);
}

typedef struct TraceCall {
    char *buf;
    uint32_t size;
} TraceCall;

static void call_trace(void *arg) {
    TraceCall *t = (TraceCall *)arg;

    t->buf[0] = 0;
    port_game_trace(t->buf, (int)t->size);
}

GLOVER_API int32_t glover_trace(Glover *g, char *buf, uint32_t size) {
    TraceCall t;

    if (buf == NULL || size == 0 || size > 0x7FFFFFFF) {
        return GLOVER_ERR_ARG;
    }
    t.buf = buf;
    t.size = size;
    return enter(g, call_trace, &t);
}

static void call_hash(void *arg) {
    *(uint32_t *)arg = port_hash_ram();
}

GLOVER_API uint32_t glover_hash_memory(Glover *g) {
    uint32_t h = 0;

    if (enter(g, call_hash, &h) != GLOVER_OK) {
        return 0;
    }
    return h;
}

/* The two calls that may run on a second thread. The rate is set once, while the game starts; the queue (audio.c)
   is the only state that both threads touch. */
GLOVER_API int32_t glover_audio_rate(Glover *g) {
    if (g == NULL || g != lib_game) {
        return GLOVER_ERR_ARG;
    }
    return lib_audio_rate;
}

GLOVER_API int32_t glover_audio_read(Glover *g, int16_t *out, int32_t frames) {
    if (g == NULL || g != lib_game || out == NULL || frames < 0) {
        return GLOVER_ERR_ARG;
    }
    return lib_audio_read(out, frames);
}
