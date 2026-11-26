/* The test host of the library (docs/library.md, docs/testing.md). It loads libglover.so or glover.dll at run time
   and runs the game as weemitt's headless mode does (platform.c, run_headless), with the same options and the same
   output. tools/regress.py, dtcheck.py and ptrdiff.py run it with --exe. It is plain C and needs no SDL.

   The library: $GLOVER_LIB, or libglover.so next to the program (glover.dll on Windows, libglover.dylib on macOS).

   Options as weemitt's: --rom FILE --headless FRAMES --level N --input SCRIPT --trace --hash --hash-every N --dt D
   --dt-jitter J --pace --dump FRAME FILE --save FILE --ballworld. Without --save the save is read from weemitt's
   folder ($XDG_DATA_HOME/weemitt/Glover/eeprom.bin or ~/.local/share/weemitt/Glover/eeprom.bin; %APPDATA% on
   Windows; eeprom-ballworld.bin with --ballworld), and never written.

   Test options (tools/libcheck.py):
     --fatal-at N      update number N stops the game; checks what the calls return after that
     --reenter         the log callback calls glover_status, which must return GLOVER_ERR_REENTER
     --host-mxcsr X    sets MXCSR to X before each call of the library and checks that it is X after it (on arm64
                       the FPCR)
     --thread          runs the game on a second thread with a 512 KB stack
     --audio-hash      reads the sound after each update and prints the count and a hash of the frames
     --audio-thread    the same, read on a second thread
     --audio-out FILE  writes the sound's frames to FILE, as weemitt's headless --audio-out does
     --volume M E      sets GLOVER_OPT_MUSIC_VOLUME to M and GLOVER_OPT_EFFECTS_VOLUME to E after glover_create
     --camera-scale S  sets GLOVER_OPT_CAMERA_DISTANCE_SCALE to S after glover_create, and checks its range
     --lifecycle       checks the argument checks, glover_destroy and the calls after it
     --fatal-at-start  the game stops while glover_create starts it; checks what create returns
     --reload          runs the game, unloads the library, loads it again and runs the game a second time
     --log             prints the library's log to stderr
     --clock lib       uses the library's own game clock instead of the host's
     --scene FILE      runs a host scene (src/port/hostscene.c) through the host-world calls, as weemitt's headless
                       --host-scene does; with --trace it also prints a "bodies" line after each frame
     --coll-args       checks the host-world calls' argument checks, the memory reserve and GloverBodies's sizes
     --hostgfx MASK    calls glover_host_gfx with these layers after each frame and checks its lists: no unknown
                       command, every pointer even, vertices inside the game's memory (but for the four of a texture
                       rectangle), every list ending with G_ENDDL,
                       and the triangles equal to those of the requested layers in the game's own lists (counted here
                       by a walk of its F3DEX lists); prints a "hostgfx" line at the end
     --hostgfx-t T     the same with t = T (0 to 1: the blend between the last two frames)
     --hostgfx-split   the same with GLOVER_HOSTGFX_SPLIT: the triangles of both lists together, and no invalidation
                       in the second
     --hostgfx-fog     the same with GLOVER_HOSTGFX_FOG and a fog of the test's own: G_FOG in every geometry mode, the
                       fog's blend in every render mode, the test's fog colour and position in every fog command, a
                       fog position before the first triangle of each entry list, the argument checks of the fog,
                       and every other command the same as in the lists of a call without the fog just before
     --hostgfx-dump F  prints the lists of the first call from frame F on that writes a texture rectangle, one
                       command a line, indented by call depth
   With -DCLASH the program defines symbols that the game's code also defines; the library must not see them. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif
#include "glover.h"
#include "hostscene.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#endif

#define F(name) __typeof__(&name) name
static struct {
    F(glover_api_version);
    F(glover_build_id);
    F(glover_default_config);
    F(glover_create);
    F(glover_destroy);
    F(glover_error);
    F(glover_update);
    F(glover_status);
    F(glover_bodies);
    F(glover_set_option);
    F(glover_get_option);
    F(glover_goto_level);
    F(glover_eeprom);
    F(glover_memory);
    F(glover_trace);
    F(glover_hash_memory);
    F(glover_audio_rate);
    F(glover_audio_read);
    F(glover_gl_shutdown);
    F(glover_coll_set_static);
    F(glover_coll_body_add);
    F(glover_coll_body_move);
    F(glover_coll_body_remove);
    F(glover_coll_body_conveyor);
    F(glover_coll_set_water);
    F(glover_coll_clear);
    F(glover_coll_stats);
    F(glover_place);
    F(glover_set_respawn);
    F(glover_set_view);
    F(glover_draw_marks);
    F(glover_host_gfx);
} lib;
#undef F

/* Each call of the library: with --host-mxcsr, MXCSR is set to the host's value before the call and checked after
   it. */
#if defined(__aarch64__)
static unsigned int get_csr(void) {
    unsigned long v;

    __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    return (unsigned int)v;
}
static void set_csr(unsigned int csr) {
    unsigned long v = csr;

    __asm__ volatile("msr fpcr, %0" : : "r"(v));
}
#define CSR_DEFAULT 0
#else
#define get_csr() _mm_getcsr()
#define set_csr(v) _mm_setcsr(v)
#define CSR_DEFAULT 0x1F80
#endif
static unsigned int hostCsr;
static int useHostCsr;
static long csrBad;
#define CALL(expr) (csr_before(), csr_after(expr))
static void csr_before(void) {
    if (useHostCsr) {
        set_csr(hostCsr);
    }
}
static long long csr_check(void) {
    if (useHostCsr) {
        if (get_csr() != hostCsr) {
            csrBad++;
        }
        set_csr(CSR_DEFAULT);
    }
    return 0;
}
#define csr_after(expr) __extension__({ __typeof__(expr) r_ = (expr); csr_check(); r_; })

static void *load_symbol(void *h, const char *name) {
#ifdef _WIN32
    void *p = (void *)GetProcAddress((HMODULE)h, name);
#else
    void *p = dlsym(h, name);
#endif
    if (p == NULL) {
        fprintf(stderr, "glover-host: the library has no %s\n", name);
        exit(2);
    }
    return p;
}

static void *libHandle;

static void unload_library(void) {
#ifdef _WIN32
    FreeLibrary((HMODULE)libHandle);
#else
    dlclose(libHandle);
#endif
    libHandle = NULL;
}

static void load_library(const char *argv0) {
    const char *path = getenv("GLOVER_LIB");
    char buf[4096];
    void *h;

    if (path == NULL || path[0] == 0) {
        const char *slash = strrchr(argv0, '/');
#ifdef _WIN32
        const char *bslash = strrchr(argv0, '\\');
        const char *name = "glover.dll";

        if (bslash != NULL && (slash == NULL || bslash > slash)) {
            slash = bslash;
        }
#elif defined(__APPLE__)
        const char *name = "libglover.dylib";
#else
        const char *name = "libglover.so";
#endif
        snprintf(buf, sizeof(buf), "%.*s%s", slash ? (int)(slash - argv0 + 1) : 0, argv0, name);
        path = buf;
    }
#ifdef _WIN32
    h = (void *)LoadLibraryA(path);
    if (h == NULL) {
        fprintf(stderr, "glover-host: cannot load %s (error %lu)\n", path, (unsigned long)GetLastError());
        exit(2);
    }
#else
    h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (h == NULL) {
        fprintf(stderr, "glover-host: cannot load %s: %s\n", path, dlerror());
        exit(2);
    }
#endif
    libHandle = h;
#define L(name) *(void **)&lib.name = load_symbol(h, #name)
    L(glover_api_version);
    L(glover_build_id);
    L(glover_default_config);
    L(glover_create);
    L(glover_destroy);
    L(glover_error);
    L(glover_update);
    L(glover_status);
    L(glover_bodies);
    L(glover_set_option);
    L(glover_get_option);
    L(glover_goto_level);
    L(glover_eeprom);
    L(glover_memory);
    L(glover_trace);
    L(glover_hash_memory);
    L(glover_audio_rate);
    L(glover_audio_read);
    L(glover_gl_shutdown);
    L(glover_coll_set_static);
    L(glover_coll_body_add);
    L(glover_coll_body_move);
    L(glover_coll_body_remove);
    L(glover_coll_body_conveyor);
    L(glover_coll_set_water);
    L(glover_coll_clear);
    L(glover_coll_stats);
    L(glover_place);
    L(glover_set_respawn);
    L(glover_set_view);
    L(glover_draw_marks);
    L(glover_host_gfx);
#undef L
}

/* ---- The options. */

static struct {
    const char *rom;
    long frames;
    long level;
    const char *input;
    int trace;
    int hashEvery;
    double dt;
    double dtJitter;
    int pace;
    long dumpFrame;
    const char *dumpPath;
    const char *save;
    long fatalAt;
    int reenter;
    int thread;
    int audioHash;
    int audioThread;
    int log;
    int libClock;
    const char *audioOut;
    int lifecycle;
    int fatalAtStart;
    int reload;
    const char *scene;
    int collArgs;
    int ballworld;
    long marksFrame;
    double musicVolume, effectsVolume;   /* -1: not set */
    int hostGfx;                         /* --hostgfx: the layers, 0 for off */
    double hostGfxT;
    long hostGfxDump;                    /* --hostgfx-dump: the frame, or -1 */
    int hostGfxSplit;                    /* --hostgfx-split */
    int hostGfxFog;                      /* --hostgfx-fog */
    double cameraScale;                  /* --camera-scale: 0 when not set */
} opt = {NULL, 0, -1, NULL, 0, 0, 1.0, 0.0, 0, -1, NULL, NULL, 0, 0, 0, 0, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, 0, -1,
         -1.0, -1.0, 0, -1.0, -1, 0, 0, 0.0};

static void usage(void) {
    fprintf(stderr, "usage: glover-host --rom FILE --headless FRAMES [--level N] [--input SCRIPT] [--trace] [--hash | "
                    "--hash-every N]\n       [--dt D] [--dt-jitter J] [--pace] [--dump FRAME FILE] [--save FILE] "
                    "[--ballworld] [test options, see libhost.c]\n");
    exit(2);
}

/* ---- The input script: "frame:buttons:x:y" lines, as weemitt's --input (platform.c, load_input_script). */

typedef struct ScriptedInput {
    long frame;
    unsigned short buttons;
    int x;
    int y;
} ScriptedInput;

static ScriptedInput *script;
static int scriptCount;

static void load_input_script(const char *path) {
    FILE *f = fopen(path, "r");
    char line[256];
    int cap = 0;

    if (f == NULL) {
        fprintf(stderr, "glover-host: cannot open the input script %s\n", path);
        exit(2);
    }
    while (fgets(line, sizeof(line), f)) {
        ScriptedInput s;
        unsigned int buttons;

        if (line[0] == '#' || sscanf(line, "%ld:%x:%d:%d", &s.frame, &buttons, &s.x, &s.y) != 4) {
            continue;
        }
        s.buttons = (unsigned short)buttons;
        if (scriptCount == cap) {
            cap = cap ? cap * 2 : 64;
            script = (ScriptedInput *)realloc(script, cap * sizeof(*script));
        }
        script[scriptCount++] = s;
    }
    fclose(f);
}

static GloverInput scripted_input(long frame) {
    GloverInput in = {0, 0, 0};
    int i;

    for (i = 0; i < scriptCount && script[i].frame <= frame; i++) {
        in.buttons = script[i].buttons;
        in.stickX = (int8_t)script[i].x;
        in.stickY = (int8_t)script[i].y;
    }
    return in;
}

/* ---- Files. */

static unsigned char *read_file(const char *path, long *size) {
    FILE *f = fopen(path, "rb");
    unsigned char *p;

    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = (unsigned char *)malloc(*size > 0 ? *size : 1);
    if (fread(p, 1, *size, f) != (size_t)*size) {
        free(p);
        p = NULL;
    }
    fclose(f);
    return p;
}

/* The save: --save FILE, or weemitt's own save, as SDL_GetPrefPath("weemitt", "Glover") names it. */
static int load_save(unsigned char eeprom[512]) {
    char path[4096];
    const char *base;
    const char *name = opt.ballworld ? "eeprom-ballworld.bin" : "eeprom.bin";
    FILE *f;
    size_t n;

    if (opt.save != NULL) {
        snprintf(path, sizeof(path), "%s", opt.save);
    } else {
#ifdef _WIN32
        base = getenv("APPDATA");
        if (base == NULL) {
            return 0;
        }
        snprintf(path, sizeof(path), "%s\\weemitt\\Glover\\%s", base, name);
#else
        base = getenv("XDG_DATA_HOME");
        if (base != NULL && base[0] != 0) {
            snprintf(path, sizeof(path), "%s/weemitt/Glover/%s", base, name);
        } else if ((base = getenv("HOME")) != NULL) {
            snprintf(path, sizeof(path), "%s/.local/share/weemitt/Glover/%s", base, name);
        } else {
            return 0;
        }
#endif
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    n = fread(eeprom, 1, 512, f);
    fclose(f);
    (void)n;
    return 1;
}

/* ---- The callbacks. */

static Glover *game;
static int32_t reenterResult = 1;
static int32_t reenterDestroy = 1;

static void on_log(void *user, const char *line) {
    (void)user;
    if (opt.reenter && reenterResult == 1) {
        GloverStatus s;

        memset(&s, 0, sizeof(s));
        s.size = sizeof(s);
        reenterResult = lib.glover_status(game, &s);
    }
    if (opt.lifecycle && reenterDestroy == 1) {
        reenterDestroy = lib.glover_destroy(game);
    }
    if (opt.log) {
        fprintf(stderr, "%s\n", line);
    }
}

/* The fake clock of weemitt's headless mode: it advances by each update's length times the retraces of a frame. */
static unsigned long long fakeTimeUs;

static uint64_t on_clock(void *user) {
    (void)user;
    return fakeTimeUs;
}

/* ---- Sound. */

static unsigned int audioHash = 2166136261u;
static unsigned long long audioFrames;
static volatile int audioStop;
static FILE *audioOutFile;

static void audio_drain(void) {
    int16_t buf[4096 * 2];
    int32_t n;

    while ((n = lib.glover_audio_read(game, buf, 4096)) > 0) {
        const unsigned char *b = (const unsigned char *)buf;
        long i;

        for (i = 0; i < n * 4; i++) {
            audioHash = (audioHash ^ b[i]) * 16777619u;
        }
        audioFrames += n;
        if (audioOutFile != NULL) {
            fwrite(buf, 4, (size_t)n, audioOutFile);
        }
    }
}

#ifdef _WIN32
static DWORD WINAPI audio_thread(LPVOID arg) {
#else
static void *audio_thread(void *arg) {
#endif
    (void)arg;
    while (!__atomic_load_n(&audioStop, __ATOMIC_ACQUIRE)) {
        audio_drain();
#ifdef _WIN32
        Sleep(1);
#else
        {
            struct timespec ts = {0, 500000};

            nanosleep(&ts, NULL);
        }
#endif
    }
    return 0;
}

/* ---- The run. */

static HostScene *scene;

static void print_trace(long frame) {
    char line[512];

    line[0] = 0;
    if (opt.trace) {
        CALL(lib.glover_trace(game, line, sizeof(line)));
    }
    if (opt.hashEvery && frame % opt.hashEvery == 0) {
        printf("frame %ld %s hash %08x\n", frame, line, CALL(lib.glover_hash_memory(game)));
    } else if (opt.trace) {
        printf("frame %ld %s\n", frame, line);
    }
    if (opt.trace && scene != NULL) {
        GloverBodies gb;
        HsBodies b;

        memset(&gb, 0, sizeof(gb));
        gb.size = sizeof(gb);
        CALL(lib.glover_bodies(game, &gb));
        memcpy(b.handVel, gb.handVel, sizeof(b.handVel));
        memcpy(b.ballVel, gb.ballVel, sizeof(b.ballVel));
        b.handYaw = gb.handYaw;
        b.handAction = gb.handAction;
        b.ballHeld = gb.ballHeld;
        b.handOnGround = gb.handOnGround;
        b.ballOnGround = gb.ballOnGround;
        b.handTerrain = gb.handTerrain;
        b.ballTerrain = gb.ballTerrain;
        b.ready = gb.ready;
        b.restarts = gb.restarts;
        b.handVoid = gb.handVoid;
        b.ballVoid = gb.ballVoid;
        b.handVoids = gb.handVoids;
        b.ballVoids = gb.ballVoids;
        memcpy(b.handSafe, gb.handSafe, sizeof(b.handSafe));
        memcpy(b.ballSafe, gb.ballSafe, sizeof(b.ballSafe));
        b.safeKnown = gb.safeKnown;
        b.handWater = gb.handWater;
        b.ballWater = gb.ballWater;
        hs_bodies_line(line, sizeof(line), frame, &b);
        printf("%s\n", line);
    }
}

/* --marks FRAME: the draw marks of the latest lists after that frame (glover_draw_marks): one line of address and
   layer pairs, and a line of the layers in order with repeats left out. */
static void print_marks(long frame) {
    static GloverDrawMark marks[GLOVER_MAX_DRAW_MARKS];
    uint32_t n = 0;
    uint32_t i;
    int32_t rc = CALL(lib.glover_draw_marks(game, marks, GLOVER_MAX_DRAW_MARKS, &n));

    printf("marks %ld: %d, %u marks:", frame, (int)rc, (unsigned)n);
    for (i = 0; i < n && i < GLOVER_MAX_DRAW_MARKS; i++) {
        printf(" %08x:%u", (unsigned)marks[i].addr, (unsigned)marks[i].layer);
    }
    printf("\nmark layers %ld:", frame);
    for (i = 0; i < n && i < GLOVER_MAX_DRAW_MARKS; i++) {
        if (i == 0 || marks[i].layer != marks[i - 1].layer) {
            printf(" %u", (unsigned)marks[i].layer);
        }
    }
    printf("\n");
}

static int dump_memory(void) {
    GloverMemory m;
    FILE *f;

    memset(&m, 0, sizeof(m));
    m.size = sizeof(m);
    if (CALL(lib.glover_memory(game, &m)) != GLOVER_OK || (f = fopen(opt.dumpPath, "wb")) == NULL) {
        return -1;
    }
    fwrite(m.ram, 1, m.ramSize, f);
    fclose(f);
    return 0;
}

/* ---- --scene: a host scene file (src/port/hostscene.c) through the library's host-world calls. weemitt's headless
   mode reads the same file with --host-scene and makes the same requests at the same moments. */

static GloverMesh glover_mesh(const HsMesh *m) {
    GloverMesh g;

    memset(&g, 0, sizeof(g));
    g.size = sizeof(g);
    g.flags = m->flags;
    g.verts = m->verts;
    g.vertCount = m->vertCount;
    g.indices = m->indices;
    g.terrain = m->terrain;
    g.triFlags = m->triFlags;
    g.triCount = m->triCount;
    g.conveyor = m->conveyor;
    g.conveyors = m->conveyors;
    g.conveyorCount = m->conveyorCount;
    return g;
}

static GloverPose glover_pose(const HsPose *p) {
    GloverPose g;

    memcpy(g.pos, p->pos, sizeof(g.pos));
    memcpy(g.rot, p->rot, sizeof(g.rot));
    memcpy(g.scale, p->scale, sizeof(g.scale));
    return g;
}

static int scene_static(void *u, const HsMesh *m, const HsOptions *o) {
    GloverMesh g = glover_mesh(m);
    GloverCollOptions c;

    (void)u;
    memset(&c, 0, sizeof(c));
    c.size = sizeof(c);
    c.cellSize = o->cellSize;
    c.maxChunkTris = o->maxChunkTris;
    c.linkRadius = o->linkRadius;
    c.keepLevel = o->keepLevel;
    c.levelOff = o->levelOff;
    return CALL(lib.glover_coll_set_static(game, &g, &c));
}

static int scene_body_add(void *u, const HsMesh *m, const HsPose *p, unsigned int *id) {
    GloverMesh g = glover_mesh(m);
    GloverPose q = glover_pose(p);
    uint32_t got = 0;
    int rc;

    (void)u;
    rc = CALL(lib.glover_coll_body_add(game, &g, &q, &got));
    *id = got;
    return rc;
}

static int scene_body_move(void *u, unsigned int id, const HsPose *p) {
    GloverPose q = glover_pose(p);

    (void)u;
    return CALL(lib.glover_coll_body_move(game, id, &q));
}

static int scene_body_remove(void *u, unsigned int id) {
    (void)u;
    return CALL(lib.glover_coll_body_remove(game, id));
}

static int scene_clear(void *u) {
    (void)u;
    return CALL(lib.glover_coll_clear(game));
}

static int scene_place(void *u, const HsPlace *p) {
    GloverPlace g;

    (void)u;
    memset(&g, 0, sizeof(g));
    g.size = sizeof(g);
    g.what = p->what;
    memcpy(g.hand, p->hand, sizeof(g.hand));
    memcpy(g.ball, p->ball, sizeof(g.ball));
    memcpy(g.handVel, p->handVel, sizeof(g.handVel));
    memcpy(g.ballVel, p->ballVel, sizeof(g.ballVel));
    g.handYaw = p->handYaw;
    return CALL(lib.glover_place(game, &g));
}

static int scene_respawn(void *u, const float pos[3], float yaw) {
    (void)u;
    return CALL(lib.glover_set_respawn(game, pos, yaw));
}

static int scene_view(void *u, const HsView *v) {
    GloverView g;

    (void)u;
    if (v == NULL) {
        return CALL(lib.glover_set_view(game, NULL));
    }
    memset(&g, 0, sizeof(g));
    g.size = sizeof(g);
    g.flags = v->flags;
    memcpy(g.eye, v->eye, sizeof(g.eye));
    memcpy(g.at, v->at, sizeof(g.at));
    memcpy(g.up, v->up, sizeof(g.up));
    g.fovy = v->fovy;
    g.aspect = v->aspect;
    g.nearZ = v->nearZ;
    g.farZ = v->farZ;
    return CALL(lib.glover_set_view(game, &g));
}

static int scene_goto(void *u, int level) {
    (void)u;
    return CALL(lib.glover_goto_level(game, level));
}

static void scene_log(void *u, const char *line) {
    (void)u;
    fprintf(stderr, "%s\n", line);
}

static int scene_body_conveyor(void *u, unsigned int id, const float vel[3]) {
    (void)u;
    return CALL(lib.glover_coll_body_conveyor(game, id, vel));
}

/* HsWater has GloverWater's layout. */
static int scene_set_water(void *u, const HsWater *w, unsigned int count) {
    (void)u;
    return CALL(lib.glover_coll_set_water(game, (const GloverWater *)w, count, sizeof(GloverWater)));
}

static const HsOps sceneOps = { scene_static, scene_body_add, scene_body_move, scene_body_remove, scene_clear,
                                scene_place, scene_respawn, scene_view, scene_goto, scene_log,
                                scene_body_conveyor, scene_set_water };

static void scene_stats(void) {
    GloverCollStats st;

    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    if (CALL(lib.glover_coll_stats(game, &st)) == GLOVER_OK) {
        fprintf(stderr, "hostscene: %u static triangles in %u chunks (%u linked), %u bodies, %u bytes of heap, %u free, "
                "%u rebuilds, %u failed; %u conveyor platforms (%u triangles); %u waters\n", st.staticTris, st.chunks,
                st.linkedChunks, st.bodies, st.heapUsed, st.heapFree, st.rebuilds, st.failed, st.conveyorPlatforms,
                st.conveyorTris, st.waters);
    }
}

static unsigned int jitterSeed = 1;
static int runResult;

/* platform.c, run_headless: each drawn frame lasts dt original frames (with jitter); a drawn frame longer than an
   original frame is split into updates of at most 1. The controller is polled at each drawn frame. */
/* ---- --hostgfx: glover_host_gfx's lists, checked. */

static struct {
    long calls, failed;
    unsigned long long commands, triangles, textures, skipped, unknown, rects;
    long bad;            /* calls whose lists broke a rule */
    long mismatch;       /* calls whose triangles differ from the game's own */
    double maxMs, sumMs;
    char first[200];     /* the first broken rule */
    unsigned long long texRects, fillRects, modifyColor; /* in the game's own lists, in the requested layers */
    unsigned long long libMatrices; /* matrices in the library's memory: the blended ones and the rectangles' */
} hg;

static int in_ram(const GloverMemory *m, uintptr_t p, size_t n) {
    return p >= (uintptr_t)m->ram && p + n <= (uintptr_t)m->ram + m->ramSize;
}

static void hg_bad(const char *fmt, unsigned long a, unsigned long b) {
    if (hg.first[0] == 0) {
        snprintf(hg.first, sizeof(hg.first), fmt, a, b);
    }
}

static int hgDump;
static long hgInval; /* the invalidations hg_walk saw */
static int hgFogSeen; /* --hostgfx-fog: the entry list being walked set the fog position */

/* --hostgfx-fog's fog: a colour and a position that the game does not use. */
#define HG_FOG_COLOR 0x12345600u
#define HG_FOG_MUL 0x0321
#define HG_FOG_OFF (-0x0123)
#define HG_FOG_WORD ((uint32_t)(uint16_t)HG_FOG_MUL << 16 | (uint16_t)HG_FOG_OFF)
#define HG_RM_CYCLE1 0xCCCC0000u
#define HG_RM_FOG_SHADE_A 0xC8000000u

/* --hostgfx-fog: whether a command keeps the host's fog. */
static int hg_fog_ok(unsigned op, uint32_t w0, uintptr_t w1) {
    if (op == 0xD9) {
        /* G_GEOMETRYMODE: the bits kept in w0, the bits set in w1. */
        return ((w0 & 0x10000u) || (w1 & 0x10000u)) ? 1 : 0;
    }
    if (op == 0xEF) {
        return ((uint32_t)w1 & HG_RM_CYCLE1) == HG_RM_FOG_SHADE_A;
    }
    if (op == 0xE2) {
        uint32_t len = (w0 & 0xFF) + 1;
        uint32_t shift = 32 - ((w0 >> 8) & 0xFF) - len;
        uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : (1u << len) - 1) << shift;

        return ((uint32_t)w1 & HG_RM_CYCLE1 & mask) == (HG_RM_FOG_SHADE_A & mask);
    }
    if (op == 0xF8) {
        return (uint32_t)w1 == HG_FOG_COLOR;
    }
    if (op == 0xDB && ((w0 >> 16) & 0xFF) == 0x08) {
        hgFogSeen = 1;
        return (uint32_t)w1 == HG_FOG_WORD;
    }
    return 1;
}

/* Walks a translated list and the lists it calls. Returns its triangles, or -1 when a rule broke. */
static long hg_walk(const GloverMemory *m, const GloverGfx *l, int depth) {
    long tris = 0;
    int n;

    if (depth > 20 || l == NULL || ((uintptr_t)l & 1)) {
        hg_bad("a list at depth %lu is %lx", (unsigned long)depth, (unsigned long)(uintptr_t)l);
        return -1;
    }
    for (n = 0; n < 200000; n++, l++) {
        unsigned op = (unsigned)(l->w0 >> 24) & 0xFF;
        uintptr_t p = l->w1;

        if (hgDump) {
            printf("%*s%08lx %016llx", depth * 2, "", (unsigned long)l->w0, (unsigned long long)p);
            if (op == 0x01 && !in_ram(m, p, 64)) {
                const int16_t *v = (const int16_t *)p;
                int k;

                for (k = 0; k < 4; k++) {
                    printf(" (%d %d %d st %d %d)", v[k * 8], v[k * 8 + 1], v[k * 8 + 2], v[k * 8 + 4], v[k * 8 + 5]);
                }
            }
            if (op == 0xFD && getenv("HG_TEXDUMP")) {
                const uint8_t *t = (const uint8_t *)p;
                int k;

                for (k = 0; k < 2048; k += 2) {
                    printf("%s%c", k % 64 == 0 ? "\n    " : "", (t[k + 1] & 1) ? '#' : '.');
                }
            }
            printf("\n");
        }
        if ((uint64_t)l->w0 >> 32) {
            hg_bad("command %lx has bits above 32 in w0", (unsigned long)op, 0);
            return -1;
        }
        if (opt.hostGfxFog) {
            if (!hg_fog_ok(op, (uint32_t)l->w0, p)) {
                hg_bad("command %lx does not keep the host's fog: w1 %lx", (unsigned long)op, (unsigned long)p);
                return -1;
            }
            if ((op == 0x05 || op == 0x06) && !hgFogSeen) {
                hg_bad("a triangle before the fog position", 0, 0);
                return -1;
            }
        }
        switch (op) {
            case 0xDF:
                return tris;
            case 0xDE: {
                long t = hg_walk(m, (const GloverGfx *)p, depth + 1);
                if (t < 0) {
                    return -1;
                }
                tris += t;
                if ((l->w0 >> 16) & 1) {
                    return tris;
                }
                break;
            }
            case 0x05:
                tris += 1;
                break;
            case 0x06:
                tris += 2;
                break;
            case 0x01:
                /* The four vertices of a texture rectangle are the library's. */
                if ((!in_ram(m, p, ((l->w0 >> 12) & 0xFF) * 16) && ((l->w0 >> 12) & 0xFF) != 4) || p == 0 || (p & 1)) {
                    hg_bad("a vertex load points at %lx", (unsigned long)p, 0);
                    return -1;
                }
                break;
            case 0xDA:
                hg.libMatrices += !in_ram(m, p, 64);
                /* fall through */
            case 0xDC:
            case 0xFD:
                if (p == 0 || (p & 1)) {
                    hg_bad("command %lx points at %lx", (unsigned long)op, (unsigned long)p);
                    return -1;
                }
                break;
            case 0x34:
                hgInval++;
                if (p & 1) {
                    hg_bad("an invalidation points at %lx", (unsigned long)p, 0);
                    return -1;
                }
                break;
            case 0x02: case 0x03: case 0xD7: case 0xD8: case 0xD9: case 0xDB: case 0xE2: case 0xE3:
            case 0xE6: case 0xE7: case 0xE8: case 0xE9: case 0xEE: case 0xEF: case 0xF0: case 0xF2: case 0xF3:
            case 0xF4: case 0xF5: case 0xF7: case 0xF8: case 0xF9: case 0xFA: case 0xFB: case 0xFC:
                break;
            default:
                hg_bad("an unknown command %lx at depth %lu", (unsigned long)op, (unsigned long)depth);
                return -1;
        }
    }
    hg_bad("a list does not end", 0, 0);
    return -1;
}

/* The game's own F3DEX lists: the triangles in the requested layers, as gfx_dl.c walks them, without culling. */
static struct {
    uint32_t seg[16];
    uint32_t segSet;
    const GloverDrawMark *marks;
    uint32_t markCount, next, kind, mask;
} gw;

static uint32_t gw_addr(uint32_t a) {
    uint32_t s = a >> 24;

    if (s != 0 && s < 16 && (gw.segSet & (1u << s))) {
        return gw.seg[s] + (a & 0xFFFFFF);
    }
    return (a >= 0x80000000u && a < 0xC0000000u) ? a & 0x1FFFFFFF : a;
}

static const uint32_t *gw_mem(const GloverMemory *m, uint32_t a) {
    if (a < m->wordBase || a - m->wordBase + 8 > m->ramSize) {
        return NULL;
    }
    return (const uint32_t *)(m->ram + (a - m->wordBase));
}

static long gw_list(const GloverMemory *m, uint32_t addr, int depth) {
    uint32_t pc = gw_addr(addr);
    uint32_t start = pc;
    long tris = 0;
    int n;

    for (n = 0; n < 100000; n++) {
        const uint32_t *c = gw_mem(m, pc);
        uint32_t op;

        if (c == NULL) {
            return tris;
        }
        if (depth == 0) {
            while (gw.next < gw.markCount && gw.marks[gw.next].addr <= addr + (pc - start)) {
                gw.kind = gw.marks[gw.next++].layer;
            }
        }
        op = c[0] >> 24;
        pc += 8;
        if (op == 0x06) {
            if (depth < 16) {
                tris += gw_list(m, c[1], depth + 1);
            }
            if ((c[0] >> 16) & 0xFF) {
                return tris;
            }
        } else if (op == 0xB8) {
            return tris;
        } else if (op == 0xE4 || op == 0xE5) {
            hg.texRects += (gw.kind & gw.mask) != 0;
            pc += 16;
        } else if (op == 0xF6) {
            hg.fillRects += (gw.kind & gw.mask) != 0;
        } else if (op == 0xB2 && ((c[0] >> 16) & 0xFF) != 0x14) {
            hg.modifyColor += (gw.kind & gw.mask) != 0;
        } else if (op == 0xBC && (c[0] & 0xFF) == 0x06) {
            uint32_t k = ((c[0] >> 8) & 0xFFFF) >> 2 & 0xF;

            gw.seg[k] = (c[1] >= 0x80000000u && c[1] < 0xC0000000u) ? c[1] & 0x1FFFFFFF : c[1];
            gw.segSet |= 1u << k;
        } else if ((op == 0xBF || op == 0xB1) && (gw.kind & gw.mask)) {
            tris += op == 0xB1 ? 2 : 1;
        }
    }
    return tris;
}

static long game_triangles(const GloverFrame *f, const GloverDrawMark *marks, uint32_t count, uint32_t mask) {
    GloverMemory m;
    long tris = 0;
    int i;

    memset(&m, 0, sizeof(m));
    m.size = sizeof(m);
    if (CALL(lib.glover_memory(game, &m)) != GLOVER_OK) {
        return -1;
    }
    memset(&gw, 0, sizeof(gw));
    gw.marks = marks;
    gw.markCount = count;
    gw.mask = mask;
    for (i = 0; i < f->listCount; i++) {
        if (!f->lists[i].sprite2d) {
            tris += gw_list(&m, f->lists[i].addr, 0);
        }
    }
    return tris;
}

static double now_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER t;
    LARGE_INTEGER f;

    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
#endif
}

/* --hostgfx-fog: a list and the lists it calls as one sequence of commands, without the calls, the ends, the
   invalidations and the fog positions. */
typedef struct HgFlat {
    GloverGfx *c;
    size_t n, cap;
} HgFlat;

static void hg_flatten(HgFlat *f, const GloverGfx *l, int depth) {
    int n;

    for (n = 0; l != NULL && depth < 20 && n < 200000; n++, l++) {
        unsigned op = (unsigned)(l->w0 >> 24) & 0xFF;

        if (op == 0xDF) {
            return;
        }
        if (op == 0xDE) {
            hg_flatten(f, (const GloverGfx *)l->w1, depth + 1);
            if ((l->w0 >> 16) & 1) {
                return;
            }
            continue;
        }
        if (op == 0x34 || (op == 0xDB && ((l->w0 >> 16) & 0xFF) == 0x08)) {
            continue;
        }
        if (f->n == f->cap) {
            f->cap = f->cap ? f->cap * 2 : 4096;
            f->c = (GloverGfx *)realloc(f->c, f->cap * sizeof(GloverGfx));
        }
        f->c[f->n++] = *l;
    }
}

/* --hostgfx-fog: the lists with the host's fog against the same lists without it. They must differ only in the fog's
   commands: the geometry modes, the render modes and the fog colours. */
static void hg_fog_same(const GloverHostGfx *fog, const HgFlat *plain, const HgFlat *plainXlu) {
    HgFlat f = {NULL, 0, 0};
    int pass;

    for (pass = 0; pass < 2; pass++) {
        const HgFlat *p = pass == 0 ? plain : plainXlu;
        size_t i;

        f.n = 0;
        hg_flatten(&f, pass == 0 ? fog->list : fog->xlu, 0);
        if (f.n != p->n) {
            hg_bad("with the host's fog the lists hold %lu commands, without it %lu", (unsigned long)f.n,
                   (unsigned long)p->n);
            hg.bad++;
            break;
        }
        for (i = 0; i < f.n; i++) {
            unsigned op = (unsigned)(f.c[i].w0 >> 24) & 0xFF;
            int pointer = op == 0x01 || op == 0xDA || op == 0xDC || op == 0xFD;
            int fogs = op == 0xD9 || op == 0xEF || op == 0xE2 || op == 0xF8;

            if (op != ((unsigned)(p->c[i].w0 >> 24) & 0xFF) || (!fogs && f.c[i].w0 != p->c[i].w0)
                || (!fogs && !pointer && f.c[i].w1 != p->c[i].w1)) {
                hg_bad("with the host's fog command %lx differs from the one without it (%lx)",
                       (unsigned long)f.c[i].w0, (unsigned long)p->c[i].w0);
                hg.bad++;
                break;
            }
        }
    }
    free(f.c);
}

static void host_gfx_check(const GloverFrame *lists) {
    static GloverDrawMark marks[GLOVER_MAX_DRAW_MARKS];
    static HgFlat plain, plainXlu;
    int comparePlain = 0;
    GloverHostGfx h;
    GloverMemory m;
    uint32_t count = 0;
    double t0;
    double ms;
    int32_t rc;
    long walked;
    long own;

    memset(&h, 0, sizeof(h));
    h.size = sizeof(h);
    h.gbi = GLOVER_GBI_F3DEX2;
    h.layers = (uint32_t)opt.hostGfx;
    h.t = (float)opt.hostGfxT;
    h.flags = opt.hostGfxSplit ? GLOVER_HOSTGFX_SPLIT : 0;
    if (opt.hostGfxFog && lists->listCount > 0) {
        /* The same lists without the fog first. */
        GloverHostGfx a = h;

        plain.n = plainXlu.n = 0;
        if (CALL(lib.glover_host_gfx(game, &a)) == GLOVER_OK) {
            hg_flatten(&plain, a.list, 0);
            hg_flatten(&plainXlu, a.xlu, 0);
            comparePlain = 1;
        }
    }
    if (opt.hostGfxFog) {
        h.flags |= GLOVER_HOSTGFX_FOG;
        h.fogColor = HG_FOG_COLOR;
        h.fogMultiplier = HG_FOG_MUL;
        h.fogOffset = HG_FOG_OFF;
        if (hg.calls == 0 && lists->listCount > 0) {
            /* The fog's argument checks: not with GLOVER_LAYER_NO_FOG, not in a structure that ends before it, and
               a structure of that size without the fog still works. */
            GloverHostGfx a = h;

            a.layers |= GLOVER_LAYER_NO_FOG;
            if (CALL(lib.glover_host_gfx(game, &a)) != GLOVER_ERR_ARG) {
                hg_bad("the host's fog with GLOVER_LAYER_NO_FOG is taken", 0, 0);
                hg.bad++;
            }
            a = h;
            a.size = GLOVER_HOSTGFX_SIZE_V4;
            if (CALL(lib.glover_host_gfx(game, &a)) != GLOVER_ERR_ARG) {
                hg_bad("the host's fog in a structure of %lu bytes is taken", (unsigned long)a.size, 0);
                hg.bad++;
            }
            a.flags &= ~GLOVER_HOSTGFX_FOG;
            if (CALL(lib.glover_host_gfx(game, &a)) != GLOVER_OK) {
                hg_bad("a structure of %lu bytes without the fog is refused", (unsigned long)a.size, 0);
                hg.bad++;
            }
        }
    }
    t0 = now_ms();
    rc = CALL(lib.glover_host_gfx(game, &h));
    ms = now_ms() - t0;
    if (rc == GLOVER_ERR_ARG && lists->listCount <= 0) {
        return;
    }
    hg.calls++;
    if (rc != GLOVER_OK) {
        hg.failed++;
        hg_bad("glover_host_gfx returned %ld", (unsigned long)(long)rc, 0);
        return;
    }
    hg.sumMs += ms;
    if (ms > hg.maxMs) {
        hg.maxMs = ms;
    }
    hg.commands += h.commands;
    hg.triangles += h.triangles;
    hg.textures += h.textures;
    hg.skipped += h.skipped;
    hg.unknown += h.unknown;
    hg.rects += h.rects;
    memset(&m, 0, sizeof(m));
    m.size = sizeof(m);
    CALL(lib.glover_memory(game, &m));
    hgDump = opt.hostGfxDump >= 0 && hg.calls - 1 >= opt.hostGfxDump && h.rects > 0;
    hgFogSeen = 0;
    walked = hg_walk(&m, h.list, 0);
    if (walked >= 0 && opt.hostGfxSplit) {
        long xlu;

        hgInval = 0;
        hgFogSeen = 0;
        xlu = hg_walk(&m, h.xlu, 0);
        if (xlu >= 0 && hgInval) {
            hg_bad("the second list has %lu invalidations", (unsigned long)hgInval, 0);
            xlu = -1;
        }
        walked = xlu < 0 ? -1 : walked + xlu;
    } else if (walked >= 0 && h.xlu != NULL) {
        hg_bad("a second list without GLOVER_HOSTGFX_SPLIT", 0, 0);
        walked = -1;
    }
    if (walked >= 0 && comparePlain) {
        hg_fog_same(&h, &plain, &plainXlu);
    }
    if (hgDump) {
        opt.hostGfxDump = -1;
        hgDump = 0;
    }
    if (walked < 0) {
        hg.bad++;
        return;
    }
    CALL(lib.glover_draw_marks(game, marks, GLOVER_MAX_DRAW_MARKS, &count));
    own = game_triangles(lists, marks, count, (uint32_t)opt.hostGfx & 0xF);
    /* Each texture rectangle written as triangles adds two. */
    walked -= 2 * (long)h.rects;
    if (walked != (long)h.triangles || own != (long)h.triangles) {
        hg.mismatch++;
        hg_bad("triangles: %lu in the lists, the game's own %lu", (unsigned long)walked, (unsigned long)own);
    }
}

static void host_gfx_report(void) {
    printf("hostgfx calls %ld failed %ld bad %ld mismatch %ld commands %llu triangles %llu textures %llu skipped %llu "
           "unknown %llu max-ms %.3f mean-ms %.3f game-texrects %llu game-fillrects %llu game-vertex-colours %llu"
           " library-matrices %llu rects %llu%s%s\n",
           hg.calls, hg.failed, hg.bad, hg.mismatch, hg.commands, hg.triangles, hg.textures, hg.skipped, hg.unknown,
           hg.maxMs, hg.calls ? hg.sumMs / hg.calls : 0.0, hg.texRects, hg.fillRects, hg.modifyColor,
           hg.libMatrices, hg.rects, hg.first[0] ? " first: " : "", hg.first);
}

static void run(void) {
    long frame = 0;
    long updates = 0;
    double pending = 0.0;
    double dt = opt.dt > 0.0 && opt.dt <= 1.0 ? opt.dt : 1.0;
    double sceneTime = 0.0;
    GloverInput in = {0, 0, 0};
    GloverFrame out;
    GloverFrame hostLists;  /* the latest lists, for --hostgfx */
    GloverStatus st;
    int32_t rc;

    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    memset(&out, 0, sizeof(out));
    out.size = sizeof(out);
    memset(&hostLists, 0, sizeof(hostLists));
    while (frame < opt.frames) {
        double step;

        if (pending < 1e-7) {
            in = scripted_input(frame);
            pending = dt;
            if (opt.dtJitter > 0.0) {
                jitterSeed = jitterSeed * 1103515245u + 12345u;
                pending = dt * (1.0 + opt.dtJitter * ((double)((jitterSeed >> 16) & 0x7fff) / 16383.5 - 1.0));
                if (!opt.pace && pending > 1.0) {
                    pending = 1.0;
                }
            }
        }
        step = pending > 1.0 ? 1.0 : pending;
        pending -= step;
        if (CALL(lib.glover_status(game, &st)) != GLOVER_OK) {
            runResult = 3;
            return;
        }
        if (scene != NULL) {
            hs_update(scene, sceneTime, step, &sceneOps, NULL);
            sceneTime += step;
        }
        rc = CALL(lib.glover_update(game, st.demo ? 1.0f : (float)step, &in, &out));
        updates++;
        if (rc != GLOVER_OK) {
            printf("update %ld: glover_update returned %d: %s\n", updates, (int)rc, lib.glover_error(game));
            if (opt.fatalAt) {
                double v;
                int32_t again = CALL(lib.glover_update(game, 1.0f, &in, &out));
                int32_t status = CALL(lib.glover_status(game, &st));
                int32_t option = CALL(lib.glover_get_option(game, GLOVER_OPT_EASY_BALL, &v));
                GloverConfig c;
                Glover *second = NULL;
                char err[256];
                int32_t create;

                lib.glover_default_config(&c);
                c.rom = (const uint8_t *)"x";
                c.romSize = 1;
                create = lib.glover_create(&c, &second, err, sizeof(err));
                printf("fatal: update %d, then update %d, status %d, get_option %d, a second create %d (%s)\n",
                       (int)rc, (int)again, (int)status, (int)option, (int)create, err);
                runResult = rc == GLOVER_ERR_FATAL && again == GLOVER_ERR_DEAD && status == GLOVER_ERR_DEAD
                                    && option == GLOVER_ERR_DEAD && create == GLOVER_ERR_BUSY
                                    && strcmp(lib.glover_error(game), "fatal: test") == 0
                                ? 0
                                : 4;
                return;
            }
            runResult = 3;
            return;
        }
        if (opt.audioHash && !opt.audioThread) {
            audio_drain();
        }
        CALL(lib.glover_status(game, &st));
        fakeTimeUs += (unsigned long long)((double)(float)step * st.retraces * 1000000.0 / 60.0);
        if (out.listCount >= 0) {
            hostLists = out;
        }
        if (!out.ticked) {
            continue;
        }
        if (opt.trace || opt.hashEvery) {
            print_trace(frame);
        } else if (frame < 5 || frame % 100 == 0) {
            long i;

            printf("frame %ld: mode %d level %d lists", frame, (int)st.mode, (int)st.level);
            for (i = 0; i < out.listCount; i++) {
                printf(" %u%s", (unsigned)(out.lists[i].size / 8), out.lists[i].sprite2d ? "s" : "");
            }
            printf("\n");
        }
        if (opt.dumpPath && frame == opt.dumpFrame) {
            dump_memory();
        }
        if (frame == opt.marksFrame) {
            print_marks(frame);
        }
        if (opt.hostGfx) {
            host_gfx_check(&hostLists);
        }
        frame++;
    }
    if (scene != NULL) {
        scene_stats();
    }
    if (opt.hostGfx) {
        host_gfx_report();
    }
    if (opt.fatalAt) {
        printf("fatal: the game did not stop\n");
        runResult = 4;
    }
}

#ifdef _WIN32
static DWORD WINAPI run_thread(LPVOID arg) {
    (void)arg;
    run();
    return 0;
}
#else
static void *run_thread(void *arg) {
    (void)arg;
    run();
    return NULL;
}
#endif

/* -DCLASH: names that the game's code defines too. The library binds its own. */
#ifdef CLASH
int osRecvMesg(void *q, void *m, int flags) {
    (void)q;
    (void)m;
    (void)flags;
    abort();
}
float camera[64] = {1.0f};
float hand[64] = {2.0f};
int Random(int n) {
    (void)n;
    abort();
}
unsigned char port_ram[64];
void plog_printf(const char *fmt, ...) {
    (void)fmt;
    abort();
}
unsigned long long port_time_us(void) {
    abort();
}
#endif

/* --lifecycle, before the game: glover_create's argument checks. */
static int check_create_args(GloverConfig c) {
    Glover *g = NULL;
    char err[256];
    int32_t high;
    int32_t low;
    int bad = 0;

    c.startLevel = 48;
    high = lib.glover_create(&c, &g, err, sizeof(err));
    bad |= high != GLOVER_ERR_ARG || g != NULL || err[0] == 0;
    c.startLevel = -2;
    low = lib.glover_create(&c, &g, err, sizeof(err));
    bad |= low != GLOVER_ERR_ARG || g != NULL;
    printf("lifecycle: start level 48 %d (%s), start level -2 %d\n", (int)high, err, (int)low);
    c.startLevel = -1;
    c.ballworld = 2;
    high = lib.glover_create(&c, &g, err, sizeof(err));
    bad |= high != GLOVER_ERR_ARG || g != NULL || err[0] == 0;
    c.ballworld = 0;
    c.size = (uint32_t)offsetof(GloverConfig, ballworld) - 1;
    low = lib.glover_create(&c, &g, err, sizeof(err));
    bad |= low != GLOVER_ERR_ARG || g != NULL;
    printf("lifecycle: ballworld 2 %d (%s), a size that ends inside clockUs %d\n", (int)high, err, (int)low);
    return bad;
}

/* --coll-args: the checks of the host-world calls (docs/host-world.md), with --level. Prints one line per check and
   returns the number that failed. */
static int collBad;

static void coll_expect(const char *what, int32_t got, int32_t want) {
    printf("coll-args: %s: %d%s\n", what, (int)got, got == want ? "" : " (WRONG)");
    collBad += got != want;
}

static void coll_update(int n) {
    GloverInput in = {0, 0, 0};
    GloverFrame out;

    while (n-- > 0) {
        memset(&out, 0, sizeof(out));
        out.size = sizeof(out);
        CALL(lib.glover_update(game, 1.0f, &in, &out));
        fakeTimeUs += 50000;
    }
}

static int check_coll_args(void) {
    static const float floorVerts[] = { -600, 0, -600, -600, 0, 600, 600, 0, 600, 600, 0, -600 };
    static const uint32_t floorIdx[] = { 0, 1, 2, 0, 2, 3 };
    static const uint32_t badIdx[] = { 0, 1, 4, 0, 2, 3 };
    static const uint8_t badTerrain[] = { 0, 14 };
    static const float tinyVerts[] = { -600, 0, -600, -600, 0, 600, 600, 0, 600, 600, 0, -600, 600.02f, 0, -600 };
    static const uint32_t tinyIdx[] = { 0, 1, 2, 0, 2, 3, 3, 2, 4 };
    static const float farVerts[] = { -600, 0, -600, -600, 0, 600, 600, 0, 600, 100000, 0, -600 };
    float nanVerts[12];
    GloverMesh m;
    GloverCollOptions o;
    GloverPose pose = { { 0, 40, 0 }, { 0, 0, 0, 1 }, { 1, 1, 1 } };
    GloverPose p2;
    GloverCollStats st;
    GloverBodies b;
    uint32_t id = 0;
    uint32_t id2 = 0;
    float *big;
    uint32_t *bigIdx;
    uint32_t n;
    uint32_t held;
    int32_t rc;
    int i;

    memset(&m, 0, sizeof(m));
    m.size = sizeof(m);
    m.verts = floorVerts;
    m.vertCount = 4;
    m.indices = floorIdx;
    m.triCount = 2;
    memset(&o, 0, sizeof(o));
    o.size = sizeof(o);
    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    /* Before the glove is ready, a request waits. */
    coll_expect("set_static at the start (busy, kept)", CALL(lib.glover_coll_set_static(game, &m, &o)), GLOVER_ERR_BUSY);
    CALL(lib.glover_coll_stats(game, &st));
    coll_expect("stats: pending", (int32_t)st.pending, 1);
    coll_update(150);
    CALL(lib.glover_coll_stats(game, &st));
    coll_expect("stats: built once ready (pending 0)", (int32_t)st.pending, 0);
    coll_expect("stats: 2 static triangles", (int32_t)st.staticTris, 2);
    coll_expect("set_static ready", CALL(lib.glover_coll_set_static(game, &m, &o)), GLOVER_OK);
    /* Triangles with an edge under 0.025 are left out. A set left with none fails, and the world stays as it was. */
    m.verts = tinyVerts;
    m.vertCount = 5;
    m.indices = tinyIdx;
    m.triCount = 3;
    coll_expect("set_static with a sliver", CALL(lib.glover_coll_set_static(game, &m, &o)), GLOVER_OK);
    CALL(lib.glover_coll_stats(game, &st));
    coll_expect("stats: the sliver left out (2 triangles)", (int32_t)st.staticTris, 2);
    held = st.heapUsed;
    m.indices = tinyIdx + 6;
    m.triCount = 1;
    coll_expect("set_static with only a sliver", CALL(lib.glover_coll_set_static(game, &m, &o)), GLOVER_ERR_ARG);
    CALL(lib.glover_coll_stats(game, &st));
    coll_expect("stats: the world as it was", st.staticTris == 2 && st.heapUsed == held, 1);
    m.verts = floorVerts;
    m.vertCount = 4;
    m.indices = floorIdx;
    m.triCount = 2;
    /* The arguments. */
    m.size = 0;
    coll_expect("mesh size 0", CALL(lib.glover_coll_set_static(game, &m, &o)), GLOVER_ERR_ARG);
    m.size = sizeof(m);
    o.size = 0;
    coll_expect("options size 0", CALL(lib.glover_coll_set_static(game, &m, &o)), GLOVER_ERR_ARG);
    /* A host built before levelOff gives the size up to it, and the library does not read the field. */
    o.size = offsetof(GloverCollOptions, levelOff);
    o.levelOff = 4;
    coll_expect("options without levelOff", CALL(lib.glover_coll_set_static(game, &m, &o)), GLOVER_OK);
    o.size = offsetof(GloverCollOptions, levelOff) - 1;
    coll_expect("options shorter than that", CALL(lib.glover_coll_set_static(game, &m, &o)), GLOVER_ERR_ARG);
    o.levelOff = 0;
    o.size = sizeof(o);
    m.indices = badIdx;
    coll_expect("an index out of range", CALL(lib.glover_coll_set_static(game, &m, &o)), GLOVER_ERR_ARG);
    m.indices = floorIdx;
    m.terrain = badTerrain;
    coll_expect("terrain 14", CALL(lib.glover_coll_set_static(game, &m, &o)), GLOVER_ERR_ARG);
    m.terrain = NULL;
    m.verts = farVerts;
    coll_expect("a coordinate of 100000", CALL(lib.glover_coll_set_static(game, &m, &o)), GLOVER_ERR_ARG);
    memcpy(nanVerts, floorVerts, sizeof(nanVerts));
    nanVerts[4] = 0.0f / 0.0f;
    m.verts = nanVerts;
    coll_expect("a NaN coordinate", CALL(lib.glover_coll_set_static(game, &m, &o)), GLOVER_ERR_ARG);
    m.verts = floorVerts;
    o.maxChunkTris = 32768;
    coll_expect("maxChunkTris 32768", CALL(lib.glover_coll_set_static(game, &m, &o)), GLOVER_ERR_ARG);
    o.maxChunkTris = 0;
    o.levelOff = 8;
    coll_expect("an unknown levelOff bit", CALL(lib.glover_coll_set_static(game, &m, &o)), GLOVER_ERR_ARG);
    o.levelOff = 0;
    m.flags = 4;
    coll_expect("an unknown mesh flag", CALL(lib.glover_coll_set_static(game, &m, &o)), GLOVER_ERR_ARG);
    m.flags = 0;
    /* Conveyors (version 3). A mesh of version 2 ends before them: it has none. */
    {
        static const uint16_t conv[] = { 1, 0 };
        static const uint16_t convHigh[] = { 2, 0 };
        float speeds[3] = { 1.0f, 0.0f, 0.0f };
        float vel[3] = { 0.5f, 0.0f, 0.0f };
        GloverCollStats old;

        m.conveyor = conv;
        m.conveyors = speeds;
        m.conveyorCount = 1;
        m.size = (uint32_t)offsetof(GloverMesh, conveyor);
        coll_expect("a version 2 mesh (its size ends before conveyor)", CALL(lib.glover_coll_set_static(game, &m, &o)),
                    GLOVER_OK);
        CALL(lib.glover_coll_stats(game, &st));
        coll_expect("  has no conveyor", (int32_t)st.conveyorTris, 0);
        m.size = (uint32_t)offsetof(GloverMesh, conveyor) - 1;
        coll_expect("a mesh shorter than version 2's", CALL(lib.glover_coll_set_static(game, &m, &o)), GLOVER_ERR_ARG);
        m.size = sizeof(m);
        m.conveyor = convHigh;
        coll_expect("a conveyor index above conveyorCount", CALL(lib.glover_coll_set_static(game, &m, &o)),
                    GLOVER_ERR_ARG);
        m.conveyor = conv;
        speeds[0] = 101.0f;
        coll_expect("a conveyor of 101", CALL(lib.glover_coll_set_static(game, &m, &o)), GLOVER_ERR_ARG);
        speeds[0] = 0.0f / 0.0f;
        coll_expect("a conveyor of NaN", CALL(lib.glover_coll_set_static(game, &m, &o)), GLOVER_ERR_ARG);
        speeds[0] = 1.0f;
        m.conveyorCount = 4097;
        coll_expect("4097 conveyors", CALL(lib.glover_coll_set_static(game, &m, &o)), GLOVER_ERR_ARG);
        m.conveyorCount = 1;
        coll_expect("set_static with a conveyor", CALL(lib.glover_coll_set_static(game, &m, &o)), GLOVER_OK);
        CALL(lib.glover_coll_stats(game, &st));
        coll_expect("stats: 1 conveyor triangle", (int32_t)st.conveyorTris, 1);
        coll_expect("stats: 1 conveyor platform", (int32_t)st.conveyorPlatforms, 1);
        memset(&old, 0x55, sizeof(old));
        old.size = 40;
        coll_expect("stats with version 2's size", CALL(lib.glover_coll_stats(game, &old)), GLOVER_OK);
        coll_expect("  fills it, and the rest untouched", old.staticTris == 2 && old.conveyorTris == 0x55555555u, 1);
        old.size = 39;
        coll_expect("stats with a size of 39", CALL(lib.glover_coll_stats(game, &old)), GLOVER_ERR_ARG);
        coll_expect("body_add with a conveyor in its mesh", CALL(lib.glover_coll_body_add(game, &m, &pose, &id)),
                    GLOVER_ERR_ARG);
        m.conveyor = NULL;
        m.conveyors = NULL;
        m.conveyorCount = 0;
        coll_expect("body_add", CALL(lib.glover_coll_body_add(game, &m, &pose, &id)), GLOVER_OK);
        coll_expect("body_conveyor of id 0", CALL(lib.glover_coll_body_conveyor(game, 0, vel)), GLOVER_ERR_ARG);
        vel[2] = -101.0f;
        coll_expect("body_conveyor of -101", CALL(lib.glover_coll_body_conveyor(game, id, vel)), GLOVER_ERR_ARG);
        vel[2] = 0.0f;
        coll_expect("body_conveyor", CALL(lib.glover_coll_body_conveyor(game, id, vel)), GLOVER_OK);
        CALL(lib.glover_coll_stats(game, &st));
        coll_expect("stats: 2 conveyor platforms", (int32_t)st.conveyorPlatforms, 2);
        coll_expect("body_conveyor NULL", CALL(lib.glover_coll_body_conveyor(game, id, NULL)), GLOVER_OK);
        coll_expect("body_remove", CALL(lib.glover_coll_body_remove(game, id)), GLOVER_OK);
        id = 0;
        coll_expect("set_static without conveyors", CALL(lib.glover_coll_set_static(game, &m, &o)), GLOVER_OK);
    }
    /* Waters (version 3). */
    {
        GloverWater w[2];
        GloverWater *many;

        memset(w, 0, sizeof(w));
        w[0].min[0] = -100.0f;
        w[0].min[1] = -200.0f;
        w[0].min[2] = -100.0f;
        w[0].size[0] = w[0].size[1] = w[0].size[2] = 200.0f;
        w[0].surface = -20.0f;
        w[1] = w[0];
        coll_expect("set_water", CALL(lib.glover_coll_set_water(game, w, 2, sizeof(GloverWater))), GLOVER_OK);
        CALL(lib.glover_coll_stats(game, &st));
        coll_expect("stats: 2 waters", (int32_t)st.waters, 2);
        coll_expect("set_water with an elemSize of 39", CALL(lib.glover_coll_set_water(game, w, 2, 39)),
                    GLOVER_ERR_ARG);
        many = (GloverWater *)calloc(257, sizeof(GloverWater));
        coll_expect("257 waters", CALL(lib.glover_coll_set_water(game, many, 257, sizeof(GloverWater))),
                    GLOVER_ERR_ARG);
        free(many);
        w[1].size[1] = 0.0f;
        coll_expect("a water of height 0", CALL(lib.glover_coll_set_water(game, w, 2, sizeof(GloverWater))),
                    GLOVER_ERR_ARG);
        w[1].size[1] = 200.0f;
        w[1].surface = 1.0f;
        coll_expect("a surface above the box", CALL(lib.glover_coll_set_water(game, w, 2, sizeof(GloverWater))),
                    GLOVER_ERR_ARG);
        w[1].surface = -20.0f;
        w[1].current[0] = 101.0f;
        coll_expect("a current of 101", CALL(lib.glover_coll_set_water(game, w, 2, sizeof(GloverWater))),
                    GLOVER_ERR_ARG);
        w[1].current[0] = 0.0f;
        w[1].min[0] = 0.0f / 0.0f;
        coll_expect("a NaN corner", CALL(lib.glover_coll_set_water(game, w, 2, sizeof(GloverWater))), GLOVER_ERR_ARG);
        CALL(lib.glover_coll_stats(game, &st));
        coll_expect("  the waters as they were (2)", (int32_t)st.waters, 2);
        coll_expect("set_water NULL", CALL(lib.glover_coll_set_water(game, NULL, 5, 0)), GLOVER_OK);
        CALL(lib.glover_coll_stats(game, &st));
        coll_expect("stats: 0 waters", (int32_t)st.waters, 0);
    }
    p2 = pose;
    p2.scale[1] = 0.0f;
    coll_expect("body with scale 0", CALL(lib.glover_coll_body_add(game, &m, &p2, &id)), GLOVER_ERR_ARG);
    coll_expect("its id is 0", (int32_t)id, 0);
    p2 = pose;
    p2.rot[3] = 0.9f;
    coll_expect("body with a quaternion of length 0.9", CALL(lib.glover_coll_body_add(game, &m, &p2, &id)),
                GLOVER_ERR_ARG);
    p2 = pose;
    p2.scale[0] = p2.scale[1] = p2.scale[2] = 1e-5f;
    coll_expect("body whose edges are under 0.025 at its scale", CALL(lib.glover_coll_body_add(game, &m, &p2, &id)),
                GLOVER_ERR_ARG);
    coll_expect("body_add without an id", CALL(lib.glover_coll_body_add(game, &m, &pose, NULL)), GLOVER_ERR_ARG);
    coll_expect("body_add", CALL(lib.glover_coll_body_add(game, &m, &pose, &id)), GLOVER_OK);
    coll_expect("its id is not 0", id != 0, 1);
    coll_expect("body_move of id 0", CALL(lib.glover_coll_body_move(game, 0, &pose)), GLOVER_ERR_ARG);
    coll_expect("body_move", CALL(lib.glover_coll_body_move(game, id, &pose)), GLOVER_OK);
    coll_update(2);
    coll_expect("body_remove", CALL(lib.glover_coll_body_remove(game, id)), GLOVER_OK);
    coll_expect("body_remove again", CALL(lib.glover_coll_body_remove(game, id)), GLOVER_ERR_ARG);
    /* The memory reserve: bodies of 4096 triangles until the heap would keep less than 2 MB. */
    big = (float *)malloc(3 * 3 * 4096 * sizeof(float));
    bigIdx = (uint32_t *)malloc(3 * 4096 * sizeof(uint32_t));
    for (n = 0; n < 4096; n++) {
        float x = (float)(n % 64) * 10.0f;
        float z = (float)(n / 64) * 10.0f;
        float v[9] = { x, 0, z, x, 0, z + 10, x + 10, 0, z + 10 };

        memcpy(&big[9 * n], v, sizeof(v));
        bigIdx[3 * n] = 3 * n;
        bigIdx[3 * n + 1] = 3 * n + 1;
        bigIdx[3 * n + 2] = 3 * n + 2;
    }
    m.verts = big;
    m.vertCount = 3 * 4096;
    m.indices = bigIdx;
    m.triCount = 4097;
    coll_expect("a body of 4097 triangles", CALL(lib.glover_coll_body_add(game, &m, &pose, &id)), GLOVER_ERR_ARG);
    m.triCount = 4096;
    rc = GLOVER_OK;
    for (i = 0; i < 256 && rc == GLOVER_OK; i++) {
        rc = CALL(lib.glover_coll_body_add(game, &m, &pose, &id2));
    }
    CALL(lib.glover_coll_stats(game, &st));
    printf("coll-args: %d bodies added, then %d; %u bytes held, %u free\n", i - 1, (int)rc, st.heapUsed, st.heapFree);
    coll_expect("bodies until the heap's reserve", rc, GLOVER_ERR_MEMORY);
    coll_expect("the heap keeps 2 MB", st.heapFree >= 2u * 1024 * 1024, 1);
    coll_update(2);
    coll_expect("clear", CALL(lib.glover_coll_clear(game)), GLOVER_OK);
    CALL(lib.glover_coll_stats(game, &st));
    coll_expect("after clear: 0 bodies", (int32_t)st.bodies, 0);
    coll_expect("after clear: 0 bytes held", (int32_t)st.heapUsed, 0);
    free(big);
    free(bigIdx);
    /* The places and the view. */
    {
        GloverPlace pl;
        GloverView v;
        float pos[3] = { 0, 10, 0 };

        memset(&pl, 0, sizeof(pl));
        pl.size = sizeof(pl);
        pl.what = 0x20;
        coll_expect("place with an unknown bit", CALL(lib.glover_place(game, &pl)), GLOVER_ERR_ARG);
        pl.what = GLOVER_PLACE_HAND;
        pl.hand[1] = 1.0f / 0.0f;
        coll_expect("place at infinity", CALL(lib.glover_place(game, &pl)), GLOVER_ERR_ARG);
        pl.hand[1] = 40.0f;
        coll_expect("place", CALL(lib.glover_place(game, &pl)), GLOVER_OK);
        coll_expect("set_respawn", CALL(lib.glover_set_respawn(game, pos, 0.5f)), GLOVER_OK);
        coll_expect("set_respawn without a position", CALL(lib.glover_set_respawn(game, NULL, 0.5f)), GLOVER_ERR_ARG);
        memset(&v, 0, sizeof(v));
        v.size = sizeof(v);
        v.flags = GLOVER_VIEW_DRAW;
        v.eye[2] = -100.0f;
        v.up[1] = 1.0f;
        v.fovy = 45.0f;
        v.aspect = 1.5f;
        v.nearZ = 10.0f;
        v.farZ = 1000.0f;
        coll_expect("set_view", CALL(lib.glover_set_view(game, &v)), GLOVER_OK);
        v.farZ = 5.0f;
        coll_expect("set_view with far before near", CALL(lib.glover_set_view(game, &v)), GLOVER_ERR_ARG);
        v.farZ = 1000.0f;
        v.flags = 4;
        coll_expect("set_view with an unknown flag", CALL(lib.glover_set_view(game, &v)), GLOVER_ERR_ARG);
        v.flags = GLOVER_VIEW_DRAW;
        v.up[1] = 0.0f;
        v.up[2] = 2.0f;
        coll_expect("set_view with up along the view", CALL(lib.glover_set_view(game, &v)), GLOVER_ERR_ARG);
        v.up[2] = 0.0f;
        v.up[1] = 1.0f;
        v.flags = GLOVER_VIEW_TEST_GAME;
        coll_expect("set_view with the test flag", CALL(lib.glover_set_view(game, &v)), GLOVER_OK);
        v.size = 8;
        coll_expect("set_view with a small size", CALL(lib.glover_set_view(game, &v)), GLOVER_ERR_ARG);
        coll_expect("set_view NULL", CALL(lib.glover_set_view(game, NULL)), GLOVER_OK);
    }
    coll_update(5);
    /* GloverBodies: the version 1 size gets the version 1 part. */
    memset(&b, 0x55, sizeof(b));
    b.size = 40;
    coll_expect("bodies with the version 1 size", CALL(lib.glover_bodies(game, &b)), GLOVER_OK);
    coll_expect("  and the rest untouched", b.handAction == 0x55555555, 1);
    b.size = 39;
    coll_expect("bodies with size 39", CALL(lib.glover_bodies(game, &b)), GLOVER_ERR_ARG);
    b.size = sizeof(b);
    coll_expect("bodies", CALL(lib.glover_bodies(game, &b)), GLOVER_OK);
    coll_expect("  ready", b.ready, 1);
    printf("coll-args: %d checks failed\n", collBad);
    return collBad;
}

/* --lifecycle, after the run: the other argument checks, glover_destroy and the calls after it. */
static int check_after_run(void) {
    GloverStatus st;
    GloverBodies b;
    GloverConfig c;
    Glover *second = NULL;
    char err[256];
    int32_t bodies;
    int32_t gotoHigh;
    int32_t gotoLast;
    int32_t destroy;
    int32_t again;
    int32_t status;
    int32_t rate;
    int32_t shutdown;
    int32_t create;
    int32_t setBallworld;
    double ballworld = -1.0;
    int bad = 0;

    /* play gave glover_create a version 2 host's size with ballworld 1: the field is not read. */
    CALL(lib.glover_get_option(game, GLOVER_OPT_BALLWORLD, &ballworld));
    setBallworld = CALL(lib.glover_set_option(game, GLOVER_OPT_BALLWORLD, 1.0));
    bad |= ballworld != 0.0 || setBallworld != GLOVER_ERR_ARG;
    printf("lifecycle: ballworld with the shorter size %g, set ballworld %d\n", ballworld, (int)setBallworld);
    memset(&b, 0, sizeof(b));
    bodies = CALL(lib.glover_bodies(game, &b));
    b.size = sizeof(b);
    bad |= bodies != GLOVER_ERR_ARG || CALL(lib.glover_bodies(game, &b)) != GLOVER_OK;
    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    CALL(lib.glover_status(game, &st));
    gotoHigh = CALL(lib.glover_goto_level(game, 43));
    gotoLast = CALL(lib.glover_goto_level(game, 42));
    bad |= gotoHigh != GLOVER_ERR_ARG || gotoLast != (st.gotoPossible ? GLOVER_OK : GLOVER_ERR_ARG);
    destroy = CALL(lib.glover_destroy(game));
    status = CALL(lib.glover_status(game, &st));
    rate = CALL(lib.glover_audio_rate(game));
    shutdown = CALL(lib.glover_gl_shutdown(game));
    again = CALL(lib.glover_destroy(game));
    lib.glover_default_config(&c);
    c.rom = (const uint8_t *)"x";
    c.romSize = 1;
    create = lib.glover_create(&c, &second, err, sizeof(err));
    bad |= reenterDestroy != GLOVER_ERR_REENTER || destroy != GLOVER_OK || status != GLOVER_ERR_DEAD || rate < 0
           || (shutdown != GLOVER_OK && shutdown != GLOVER_ERR_UNSUPPORTED) || again != GLOVER_OK
           || create != GLOVER_ERR_BUSY;
    printf("lifecycle: bodies without a size %d, goto 43 %d, goto 42 %d (gotoPossible %d); destroy in the log callback "
           "%d; destroy %d, then status %d, audio rate %d, gl shutdown %d, destroy %d, a second create %d\n",
           (int)bodies, (int)gotoHigh, (int)gotoLast, (int)st.gotoPossible, (int)reenterDestroy, (int)destroy,
           (int)status, (int)rate, (int)shutdown, (int)again, (int)create);
    return bad;
}

/* --fatal-at-start: the game stops inside glover_create. */
static int check_fatal_at_start(GloverConfig c) {
    Glover *g = (Glover *)&c;
    Glover *second = NULL;
    char err[256];
    char err2[256];
    int32_t rc;
    int32_t again;

    CALL(lib.glover_set_option(NULL, 0x7F000002, 1.0));
    rc = CALL(lib.glover_create(&c, &g, err, sizeof(err)));
    again = CALL(lib.glover_create(&c, &second, err2, sizeof(err2)));
    printf("fatal at start: create %d (%s), handle %s, a second create %d\n", (int)rc, err, g ? "set" : "NULL",
           (int)again);
    return rc == GLOVER_ERR_FATAL && g == NULL && strcmp(err, "fatal: test") == 0 && again == GLOVER_ERR_BUSY ? 0 : 4;
}

/* One game: glover_create, the run, glover_destroy. */
static int play(GloverConfig c) {
    char err[512];
    int32_t rc;

    if (opt.fatalAtStart) {
        return check_fatal_at_start(c);
    }
    if (opt.lifecycle) {
        if (check_create_args(c)) {
            return 4;
        }
        /* A host built before ballworld was added: check_after_run finds the mode off. */
        c.size = (uint32_t)offsetof(GloverConfig, ballworld);
        c.ballworld = 1;
    }
    if (opt.collArgs) {
        /* Hosts of versions 1 and 2 still get a game; version 5 does not exist. A start level of 99 shows that
           version 2 passes the version check: the start level is checked after it. */
        Glover *none = NULL;
        int32_t level = c.startLevel;

        c.apiVersion = GLOVER_API_VERSION + 1;
        coll_expect("glover_create with the next API version", lib.glover_create(&c, &none, err, sizeof(err)),
                    GLOVER_ERR_VERSION);
        c.apiVersion = 2;
        c.startLevel = 99;
        coll_expect("glover_create with API version 2 (and start level 99)",
                    lib.glover_create(&c, &none, err, sizeof(err)), GLOVER_ERR_ARG);
        c.startLevel = level;
        c.apiVersion = 1;
    }
    strcpy(err, "stale");
    rc = CALL(lib.glover_create(&c, &game, err, sizeof(err)));
    if (opt.reenter) {
        printf("reenter: glover_status in the log callback returned %d\n", (int)reenterResult);
        if (reenterResult != GLOVER_ERR_REENTER) {
            return 4;
        }
    }
    if (rc != GLOVER_OK) {
        fprintf(stderr, "glover-host: glover_create returned %d: %s\n", (int)rc, err);
        return 3;
    }
    if (err[0] != 0) {
        fprintf(stderr, "glover-host: glover_create returned 0 with the error text \"%s\"\n", err);
        return 4;
    }
    if (opt.fatalAt) {
        CALL(lib.glover_set_option(game, 0x7F000001, (double)opt.fatalAt));
    }
    if (opt.musicVolume >= 0.0) {
        double m = -1.0, e = -1.0;
        int32_t r1 = CALL(lib.glover_set_option(game, GLOVER_OPT_MUSIC_VOLUME, opt.musicVolume));
        int32_t r2 = CALL(lib.glover_set_option(game, GLOVER_OPT_EFFECTS_VOLUME, opt.effectsVolume));
        int32_t bad = CALL(lib.glover_set_option(game, GLOVER_OPT_MUSIC_VOLUME, 101.0));

        CALL(lib.glover_get_option(game, GLOVER_OPT_MUSIC_VOLUME, &m));
        CALL(lib.glover_get_option(game, GLOVER_OPT_EFFECTS_VOLUME, &e));
        printf("volume: set %d %d, 101 gives %d, get %.0f %.0f\n", (int)r1, (int)r2, (int)bad, m, e);
    }
    if (opt.cameraScale > 0.0) {
        double s = -1.0, before = -1.0;
        int32_t low, high, r;

        CALL(lib.glover_get_option(game, GLOVER_OPT_CAMERA_DISTANCE_SCALE, &before));
        low = CALL(lib.glover_set_option(game, GLOVER_OPT_CAMERA_DISTANCE_SCALE, 0.0));
        high = CALL(lib.glover_set_option(game, GLOVER_OPT_CAMERA_DISTANCE_SCALE, 101.0));
        r = CALL(lib.glover_set_option(game, GLOVER_OPT_CAMERA_DISTANCE_SCALE, opt.cameraScale));
        CALL(lib.glover_get_option(game, GLOVER_OPT_CAMERA_DISTANCE_SCALE, &s));
        printf("camera scale: %g at the start, set %d, 0 gives %d, 101 gives %d, get %g\n", before, (int)r, (int)low,
               (int)high, s);
    }
    if (opt.collArgs) {
        return check_coll_args() ? 4 : 0;
    }
#ifdef _WIN32
    HANDLE audio = NULL;

    if (opt.audioThread) {
        audio = CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
    }
    if (opt.thread) {
        HANDLE t = CreateThread(NULL, 512 * 1024, run_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);

        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
    } else {
        run();
    }
    if (audio != NULL) {
        __atomic_store_n(&audioStop, 1, __ATOMIC_RELEASE);
        WaitForSingleObject(audio, INFINITE);
        CloseHandle(audio);
    }
#else
    pthread_t audio;

    if (opt.audioThread) {
        pthread_create(&audio, NULL, audio_thread, NULL);
    }
    if (opt.thread) {
        pthread_attr_t attr;
        pthread_t t;

        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 512 * 1024);
        if (pthread_create(&t, &attr, run_thread, NULL) != 0) {
            fprintf(stderr, "glover-host: cannot start the thread\n");
            return 2;
        }
        pthread_join(t, NULL);
    } else {
        run();
    }
    if (opt.audioThread) {
        __atomic_store_n(&audioStop, 1, __ATOMIC_RELEASE);
        pthread_join(audio, NULL);
    }
#endif
    if (opt.audioHash) {
        GloverStatus st;

        audio_drain();
        memset(&st, 0, sizeof(st));
        st.size = sizeof(st);
        CALL(lib.glover_status(game, &st));
        printf("audio rate %d frames %llu hash %08x dropped %u game time %.6f s\n", (int)CALL(lib.glover_audio_rate(game)),
               audioFrames, audioHash, (unsigned)st.audioDropped, fakeTimeUs / 1e6);
    }
    if (useHostCsr) {
        printf("mxcsr: %ld calls returned with another MXCSR than 0x%X\n", csrBad, hostCsr);
        if (csrBad) {
            runResult = 4;
        }
    }
    if (opt.lifecycle && runResult == 0 && check_after_run()) {
        runResult = 4;
    }
    if (!opt.lifecycle && (runResult == 0 || !opt.fatalAt) && CALL(lib.glover_destroy(game)) != GLOVER_OK) {
        runResult = 4;
    }
    return runResult;
}

int main(int argc, char **argv) {
    GloverConfig c;
    unsigned char eeprom[512];
    unsigned char *rom;
    long romSize = 0;
    int rc;
    int i;

    setvbuf(stdout, NULL, _IOLBF, 0);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        int next = i + 1 < argc;

        if (!strcmp(a, "--rom") && next) {
            opt.rom = argv[++i];
        } else if (!strcmp(a, "--headless") && next) {
            opt.frames = atol(argv[++i]);
        } else if (!strcmp(a, "--level") && next) {
            opt.level = atol(argv[++i]);
        } else if (!strcmp(a, "--input") && next) {
            opt.input = argv[++i];
        } else if (!strcmp(a, "--trace")) {
            opt.trace = 1;
        } else if (!strcmp(a, "--hash")) {
            opt.hashEvery = 1;
        } else if (!strcmp(a, "--hash-every") && next) {
            opt.hashEvery = atoi(argv[++i]);
        } else if (!strcmp(a, "--dt") && next) {
            opt.dt = atof(argv[++i]);
        } else if (!strcmp(a, "--dt-jitter") && next) {
            opt.dtJitter = atof(argv[++i]);
        } else if (!strcmp(a, "--pace")) {
            opt.pace = 1;
        } else if (!strcmp(a, "--marks") && i + 1 < argc) {
            opt.marksFrame = atol(argv[++i]);
        } else if (!strcmp(a, "--dump") && i + 2 < argc) {
            opt.dumpFrame = atol(argv[++i]);
            opt.dumpPath = argv[++i];
        } else if (!strcmp(a, "--save") && next) {
            opt.save = argv[++i];
        } else if (!strcmp(a, "--fatal-at") && next) {
            opt.fatalAt = atol(argv[++i]);
        } else if (!strcmp(a, "--reenter")) {
            opt.reenter = 1;
        } else if (!strcmp(a, "--host-mxcsr") && next) {
            hostCsr = (unsigned int)strtoul(argv[++i], NULL, 0);
            useHostCsr = 1;
        } else if (!strcmp(a, "--thread")) {
            opt.thread = 1;
        } else if (!strcmp(a, "--audio-hash")) {
            opt.audioHash = 1;
        } else if (!strcmp(a, "--audio-thread")) {
            opt.audioHash = 1;
            opt.audioThread = 1;
        } else if (!strcmp(a, "--audio-out") && next) {
            opt.audioHash = 1;
            opt.audioOut = argv[++i];
        } else if (!strcmp(a, "--volume") && i + 2 < argc) {
            opt.musicVolume = atof(argv[++i]);
            opt.effectsVolume = atof(argv[++i]);
        } else if (!strcmp(a, "--camera-scale") && next) {
            opt.cameraScale = atof(argv[++i]);
        } else if (!strcmp(a, "--log")) {
            opt.log = 1;
        } else if (!strcmp(a, "--clock") && next) {
            opt.libClock = !strcmp(argv[++i], "lib");
        } else if (!strcmp(a, "--lifecycle")) {
            opt.lifecycle = 1;
        } else if (!strcmp(a, "--fatal-at-start")) {
            opt.fatalAtStart = 1;
        } else if (!strcmp(a, "--coll-args")) {
            opt.collArgs = 1;
        } else if (!strcmp(a, "--scene") && next) {
            opt.scene = argv[++i];
        } else if (!strcmp(a, "--reload")) {
            opt.reload = 1;
        } else if (!strcmp(a, "--hostgfx") && next) {
            opt.hostGfx = (int)strtol(argv[++i], NULL, 0);
        } else if (!strcmp(a, "--hostgfx-dump") && next) {
            opt.hostGfxDump = atol(argv[++i]);
        } else if (!strcmp(a, "--hostgfx-t") && next) {
            opt.hostGfxT = atof(argv[++i]);
        } else if (!strcmp(a, "--hostgfx-split")) {
            opt.hostGfxSplit = 1;
        } else if (!strcmp(a, "--hostgfx-fog")) {
            opt.hostGfxFog = 1;
        } else if (!strcmp(a, "--ballworld")) {
            opt.ballworld = 1;
        } else {
            usage();
        }
    }
    if (opt.rom == NULL || opt.frames <= 0) {
        usage();
    }
    load_library(argv[0]);
    if (lib.glover_api_version() != GLOVER_API_VERSION) {
        fprintf(stderr, "glover-host: the library has API version %u, not %d\n", (unsigned)lib.glover_api_version(),
                GLOVER_API_VERSION);
        return 2;
    }
    rom = read_file(opt.rom, &romSize);
    if (rom == NULL) {
        fprintf(stderr, "glover-host: cannot read the ROM %s\n", opt.rom);
        return 2;
    }
    if (opt.input) {
        load_input_script(opt.input);
    }
    if (opt.scene != NULL) {
        char err[300];

        scene = hs_load(opt.scene, err, sizeof(err));
        if (scene == NULL) {
            fprintf(stderr, "glover-host: %s\n", err);
            return 2;
        }
    }
    if (opt.audioOut != NULL && (audioOutFile = fopen(opt.audioOut, "wb")) == NULL) {
        fprintf(stderr, "glover-host: cannot write %s\n", opt.audioOut);
        return 2;
    }
    memset(eeprom, 0, sizeof(eeprom));
    lib.glover_default_config(&c);
    c.rom = rom;
    c.romSize = (uint32_t)romSize;
    c.eeprom = load_save(eeprom) ? eeprom : NULL;
    c.startLevel = (int32_t)opt.level;
    c.ballworld = opt.ballworld;
    c.log = opt.log || opt.reenter || opt.lifecycle ? on_log : NULL;
    c.clockUs = opt.libClock ? NULL : on_clock;
    rc = play(c);
    if (opt.reload && rc == 0) {
        /* A second game needs the library loaded again (glover.h). The host's own state starts again too. */
        unload_library();
        load_library(argv[0]);
        printf("reload\n");
        fakeTimeUs = 0;
        jitterSeed = 1;
        audioHash = 2166136261u;
        audioFrames = 0;
        rc = play(c);
    }
    free(rom);
    if (audioOutFile != NULL) {
        fclose(audioOutFile);
    }
    return rc;
}
