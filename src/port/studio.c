/* The studio (studio.h). */
#include "studio.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <dirent.h>
#include <math.h>
#include <stddef.h>
#include <sys/stat.h>
#include <wchar.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "engine.h"
#include "inputs.h"
#include "log.h"
#include "procs.h"
#include "util.h"

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir((p), 0777)
#endif

int glover_has_command(const char *name);
int glover_main(int argc, char **argv);

/* A saved state every this many frames at first (3 seconds in a level). */
#define FIRST_SPACING 60
/* At most this many saved states (a state is about 5 MB). When there would be more, the spacing doubles and every
   other state goes. */
#define MAX_STATES 256
/* The frames that L and R jump while paused: one second in a level. */
#define JUMP 20

/* The controller's buttons (libultra's bits, as engine.h's BTN_). */
#define C_BUTTONS (BTN_CU | BTN_CD | BTN_CL | BTN_CR)

/* The display lists of one frame that the studio keeps: as many as the platform layer asks for. */
#define MAX_TASKS 16

/* A saved state of the timeline, with the display lists of its frame. They lie in the game's memory, which the
   state holds. After a load they draw the frame's picture without an update. */
typedef struct Key {
    long frame;
    uint8_t *state;
    size_t capacity;
    PortFrameTask tasks[MAX_TASKS];
    int taskCount;
} Key;

static Engine *engine;

/* The timeline. The game holds frame `cur`: the state after cur updates. inputs[f] is the input of update f. */
static EngineInput *inputs;
static long length;
static long inputsCapacity;
static long cur;
static Key *keys; /* in frame order */
static long nkeys;
static long spacing = FIRST_SPACING;

static int paused;
static int playing;
static int stepRequests;
static int seekPending;
static long seekTarget;
static int replaying; /* inside a seek: the hooks leave the timeline alone */

/* The display lists of the frame on show (cur). */
static PortFrameTask shownTasks[MAX_TASKS];
static int shownCount;
/* A state file was loaded: the next studio_apply_seek returns its frame's display lists. */
static int showPending;

/* The controller while paused: the buttons of the previous poll, and how long the held direction has repeated. */
static unsigned short prevPoll;
static unsigned short chordMask;
static unsigned short resumeMask; /* A after a resume with A: the game gets it once it has been released */
static int holdFrames;

/* Where the timeline starts, for route files: "power-on", "level N" or "state HASH". */
static char origin[64] = "power-on";

/* The self-test: the trace line of every frame, and the whole state's hash every HASH_EVERY frames. */
#define HASH_EVERY 50
static int selftest;
static char (*traces)[256];
static uint64_t *hashes;
static long tracesCapacity;

int studio_command(int argc, char **argv, int *code) {
    if (argc < 2 || !glover_has_command(argv[1])) {
        return 0;
    }
#ifdef _WIN32
    /* The studio is a window program, which Windows starts without a console. A command run from a console
       writes to it, unless its output is already redirected to a file or a pipe. */
    {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        HANDLE err = GetStdHandle(STD_ERROR_HANDLE);

        if ((out == NULL || out == INVALID_HANDLE_VALUE || err == NULL || err == INVALID_HANDLE_VALUE)
            && AttachConsole(ATTACH_PARENT_PROCESS)) {
            if (out == NULL || out == INVALID_HANDLE_VALUE) {
                freopen("CONOUT$", "w", stdout);
            }
            if (err == NULL || err == INVALID_HANDLE_VALUE) {
                freopen("CONOUT$", "w", stderr);
            }
        }
    }
#endif
    /* The game's own messages (the textures it does not find, its memory blocks) are left out, as the brute
       forcer's own program leaves them out. */
    port_log_quiet = 1;
    *code = glover_main(argc, argv);
    return 1;
}

static void fail(const char *what) {
    plog_printf("studio: %s", what);
    fprintf(stderr, "studio: %s\n", what);
    exit(1);
}

void studio_init(const unsigned char *rom, unsigned long romSize) {
    char err[256];

    engine = engine_attach(rom, romSize, err, sizeof(err));
    if (engine == NULL) {
        fail(err);
    }
    selftest = getenv("WEEMITT_STUDIO_SELFTEST") != NULL;
}

/* ---- Saved states. */

/* The index of the last state at or before frame, or -1. */
static long key_at_or_before(long frame) {
    long lo = 0;
    long hi = nkeys - 1;
    long best = -1;

    while (lo <= hi) {
        long mid = (lo + hi) / 2;

        if (keys[mid].frame <= frame) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return best;
}

static void drop_key(long i) {
    free(keys[i].state);
    memmove(&keys[i], &keys[i + 1], (nkeys - i - 1) * sizeof(Key));
    nkeys--;
}

/* Saves the game's state as frame cur's. A state saved into the buffer of an older one copies only what changed. */
static void save_key(void) {
    EngineStateInfo info;
    long i = key_at_or_before(cur);
    Key *k;

    if (i >= 0 && keys[i].frame == cur) {
        k = &keys[i];
    } else {
        Key *grown = realloc(keys, (nkeys + 1) * sizeof(Key));

        if (grown == NULL) {
            fail("out of memory for the timeline");
        }
        keys = grown;
        i++;
        memmove(&keys[i + 1], &keys[i], (nkeys - i) * sizeof(Key));
        nkeys++;
        k = &keys[i];
        k->frame = cur;
        k->state = NULL;
        k->capacity = 0;
    }
    engine_state_info(engine, &info);
    if (k->capacity < info.total) {
        uint8_t *grown = realloc(k->state, info.total);

        if (grown == NULL) {
            fail("out of memory for the timeline");
        }
        if (k->capacity == 0) {
            memset(grown, 0, ENGINE_STATE_HEADER);
        }
        k->state = grown;
        k->capacity = info.total;
    }
    engine_save(engine, k->state);
    memcpy(k->tasks, shownTasks, sizeof(shownTasks));
    k->taskCount = shownCount;
    if (nkeys > MAX_STATES) {
        /* Fewer, further apart: every other state goes. Frame 0's stays. */
        spacing *= 2;
        for (i = nkeys - 1; i > 0; i--) {
            if (keys[i].frame % spacing != 0) {
                drop_key(i);
            }
        }
    }
}

static void save_key_if_due(void) {
    long i;

    if (cur % spacing != 0) {
        return;
    }
    i = key_at_or_before(cur);
    if (i < 0 || keys[i].frame != cur) {
        save_key();
    }
}

/* The timeline ends at frame f: the frames after it and their states go. */
static void truncate_at(long f) {
    if (f >= length) {
        return;
    }
    length = f;
    while (nkeys > 1 && keys[nkeys - 1].frame > f) {
        drop_key(nkeys - 1);
    }
}

/* The whole state's hash, without the controller words: the platform layer sets them before every update, and
   between updates they hold whatever it polled last. */
static uint64_t selftest_hash(void) {
    unsigned short b = port_buttons;
    signed char x = port_stick_x;
    signed char y = port_stick_y;
    uint64_t h;

    port_buttons = 0;
    port_stick_x = 0;
    port_stick_y = 0;
    h = engine_hash(engine);
    port_buttons = b;
    port_stick_x = x;
    port_stick_y = y;
    return h;
}

static void record_selftest(void) {
    if (cur >= tracesCapacity) {
        long n = tracesCapacity ? tracesCapacity * 2 : 4096;

        traces = realloc(traces, n * sizeof(*traces));
        hashes = realloc(hashes, n * sizeof(*hashes));
        tracesCapacity = n;
    }
    engine_trace(engine, traces[cur], sizeof(traces[cur]));
    hashes[cur] = cur % HASH_EVERY == 0 ? selftest_hash() : 0;
}

static void keep_shown(const PortFrameTask *tasks, long count) {
    if (count >= 0) {
        shownCount = count < MAX_TASKS ? (int)count : MAX_TASKS;
        memcpy(shownTasks, tasks, shownCount * sizeof(PortFrameTask));
    }
}

void studio_started(long level) {
    if (level >= 0) {
        snprintf(origin, sizeof(origin), "level %ld", level);
    }
    cur = 0;
    length = 0;
    save_key();
    if (selftest) {
        record_selftest();
    }
}

/* ---- The updates. */

void studio_before_update(unsigned short *buttons, signed char *x, signed char *y) {
    if (replaying) {
        return;
    }
    if (cur < length) {
        /* A frame the timeline holds: its recorded input. */
        *buttons = inputs[cur].buttons;
        *x = inputs[cur].x;
        *y = inputs[cur].y;
        return;
    }
    if (length >= inputsCapacity) {
        long n = inputsCapacity ? inputsCapacity * 2 : 4096;
        EngineInput *grown = realloc(inputs, n * sizeof(EngineInput));

        if (grown == NULL) {
            fail("out of memory for the timeline");
        }
        inputs = grown;
        inputsCapacity = n;
    }
    inputs[length].buttons = *buttons;
    inputs[length].x = *x;
    inputs[length].y = *y;
    length++;
}

void studio_after_update(const PortFrameTask *tasks, long count) {
    if (replaying) {
        return;
    }
    keep_shown(tasks, count);
    engine_count_logic(engine);
    cur++;
    save_key_if_due();
    if (selftest) {
        record_selftest();
    }
    if (playing && cur >= length) {
        playing = 0;
        paused = 1;
    }
}

/* ---- Going to a frame. */

long studio_apply_seek(PortFrameTask *tasks, int max) {
    unsigned short buttons = port_buttons;
    signed char x = port_stick_x;
    signed char y = port_stick_y;
    long target;
    long n = 0;
    long f;

    if (!seekPending) {
        if (!showPending) {
            return -1;
        }
        /* The first frame of a loaded state file. */
        showPending = 0;
        n = shownCount < max ? shownCount : max;
        memcpy(tasks, shownTasks, n * sizeof(PortFrameTask));
        return n;
    }
    seekPending = 0;
    showPending = 0;
    target = seekTarget < 0 ? 0 : seekTarget > length ? length : seekTarget;
    if (target == cur) {
        return -1;
    }
    if (!(target > cur && target - cur <= spacing)) {
        /* Load the last state before the target frame. Frame 0's state is always there. */
        long i = key_at_or_before(target > 0 ? target - 1 : 0);

        if (engine_load(engine, keys[i].state) != 0) {
            fail("a saved state of the timeline does not load");
        }
        cur = keys[i].frame;
        keep_shown(keys[i].tasks, keys[i].taskCount);
        n = shownCount < max ? shownCount : max;
        memcpy(tasks, shownTasks, n * sizeof(PortFrameTask));
    }
    replaying = 1;
    for (f = cur; f < target; f++) {
        long m;

        /* The full draw: the trimmed one leaves picture data that lasts, such as a level's static matrices, which
           are computed once when the level starts. */
        /* run_update asks for the intro's skip presses before every update, which moves the skip's own state on.
           The recorded buttons hold the presses already. */
        (void)port_intro_skip_buttons();
        port_buttons = inputs[f].buttons;
        port_stick_x = inputs[f].x;
        port_stick_y = inputs[f].y;
        m = port_game_update(1.0f, tasks, max);
        if (m >= 0) {
            n = m;
            keep_shown(tasks, m);
        }
        engine_count_logic(engine);
        cur = f + 1;
        save_key_if_due();
    }
    replaying = 0;
    port_buttons = buttons;
    port_stick_x = x;
    port_stick_y = y;
    return n;
}

void studio_request_seek(long frame) {
    paused = 1;
    playing = 0;
    stepRequests = 0;
    seekPending = 1;
    seekTarget = frame;
}

/* ---- Pause and the controller. */

int studio_paused(void) {
    return paused;
}

int studio_take_step(void) {
    if (stepRequests > 0) {
        stepRequests--;
        return 1;
    }
    return 0;
}

static void drop_undo(void);

void studio_set_paused(int on) {
    if (on) {
        paused = 1;
        playing = 0;
        return;
    }
    /* Resume with the player's input: the frames after the one on show go, and so does the take's undo. */
    seekPending = 0;
    stepRequests = 0;
    truncate_at(cur);
    drop_undo();
    paused = 0;
    playing = 0;
}

void studio_play_recording(void) {
    if (cur < length) {
        seekPending = 0;
        stepRequests = 0;
        paused = 0;
        playing = 1;
    }
}

void studio_step_forward(void) {
    if (cur < length) {
        studio_request_seek(cur + 1);
    } else {
        paused = 1;
        stepRequests = 1;
    }
}

static void forget_marks(void);

int studio_goto_level(long level) {
    if (!port_goto_possible()) {
        return 0;
    }
    port_goto_level(level);
    stepRequests = 0;
    snprintf(origin, sizeof(origin), "level %ld", level);
    /* The new timeline's first state holds the jump that is due. */
    while (nkeys > 0) {
        drop_key(nkeys - 1);
    }
    spacing = FIRST_SPACING;
    length = 0;
    cur = 0;
    seekPending = 0;
    playing = 0;
    forget_marks();
    save_key();
    if (selftest) {
        record_selftest();
    }
    if (paused) {
        /* While paused, one frame runs at once: the level's first frame shows, not the old picture. */
        stepRequests = 1;
    }
    return 1;
}

/* ---- State files. */

#define STATE_FILE_MAGIC "WEEMSTAT"
#define STATE_FILE_FORMAT 1

/* The header of a state file. The engine's state follows it (engine_save's bytes). The values are in the PC's byte
   order. Each field lies at the same offset with the alignment rules of Linux and of Windows. */
typedef struct StateFileHeader {
    char magic[8];       /* STATE_FILE_MAGIC, without a terminating 0 */
    uint32_t format;     /* STATE_FILE_FORMAT */
    uint32_t headerSize; /* sizeof(StateFileHeader): where the engine's state starts */
    uint64_t gameHash;   /* STUDIO_GAME_HASH: a hash of the game's object (game.o) of the build that saved it */
    uint32_t layout;     /* engine_identity: where the game's memory, variables and code lie */
    uint32_t rom;        /* engine_identity: a hash of the ROM */
    uint64_t stateSize;  /* the bytes of the engine's state */
    uint64_t checksum;   /* a hash of those bytes */
    uint32_t frame;      /* the frame of the timeline it was saved at */
    int32_t level;       /* port_game_level then */
    int32_t mode;        /* port_game_mode then */
    uint32_t taskCount;  /* the display lists of the frame (Key.tasks) */
    PortFrameTask tasks[MAX_TASKS];
    char version[64];    /* the program's version (git describe), with a terminating 0 */
    uint32_t dataOffset; /* engine_state_data: where the game's data starts in the engine's state */
    char reserved[188];  /* 0 */
} StateFileHeader;

_Static_assert(offsetof(StateFileHeader, gameHash) == 16, "the state file header's layout");
_Static_assert(offsetof(StateFileHeader, tasks) == 64, "the state file header's layout");
_Static_assert(offsetof(StateFileHeader, dataOffset) == 320, "the state file header's layout");
_Static_assert(sizeof(StateFileHeader) == 512, "the state file header's size");

/* Opens a file by its UTF-8 path. On Windows fopen reads a path in the ANSI code page, and SDL gives the preference
   folder and the command line in UTF-8, so the path goes to _wfopen in UTF-16. */
static FILE *open_utf8(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t wpath[1024];
    wchar_t wmode[8];

    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) == 0 ||
        MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 8) == 0) {
        return NULL;
    }
    return _wfopen(wpath, wmode);
#else
    return fopen(path, mode);
#endif
}

/* Makes a folder by its UTF-8 path. Returns 0 when it made it. */
static int make_dir_utf8(const char *path) {
#ifdef _WIN32
    wchar_t wpath[1024];

    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) != 0) {
        return _wmkdir(wpath);
    }
    return -1;
#else
    return make_dir(path);
#endif
}

/* Removes a file or an empty folder by its UTF-8 path. Returns 0 when it did. */
static int remove_utf8(const char *path, int folder) {
#ifdef _WIN32
    wchar_t wpath[1024];

    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) == 0) {
        return -1;
    }
    return folder ? _wrmdir(wpath) : _wremove(wpath);
#else
    return folder ? rmdir(path) : remove(path);
#endif
}

/* Reads a whole file by its UTF-8 path, as util.c's read_file. Returns NULL when it cannot. */
static uint8_t *read_utf8(const char *path, size_t *size) {
    FILE *f = open_utf8(path, "rb");
    long n;
    uint8_t *p;

    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = n >= 0 ? malloc(n > 0 ? n : 1) : NULL;
    if (p == NULL || fread(p, 1, n, f) != (size_t)n) {
        fclose(f);
        free(p);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return p;
}

static uint64_t hash_bytes(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    size_t i;

    for (i = 0; i < n; i++) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

int studio_save_state(const char *path, char *err, int errSize) {
    EngineStateInfo info;
    StateFileHeader h;
    uint8_t *state;
    size_t used;
    FILE *f;
    int ok;

    engine_state_info(engine, &info);
    state = malloc(info.total);
    if (state == NULL) {
        snprintf(err, errSize, "Out of memory for the state.");
        return -1;
    }
    /* A buffer without a state header: the save copies everything. */
    memset(state, 0, ENGINE_STATE_HEADER);
    used = engine_save(engine, state);
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, STATE_FILE_MAGIC, sizeof(h.magic));
    h.format = STATE_FILE_FORMAT;
    h.headerSize = sizeof(h);
    h.gameHash = STUDIO_GAME_HASH;
    engine_identity(engine, &h.layout, &h.rom);
    h.stateSize = used;
    h.checksum = hash_bytes(state, used);
    h.frame = (uint32_t)cur;
    h.level = port_game_level();
    h.mode = port_game_mode();
    h.taskCount = (uint32_t)shownCount;
    memcpy(h.tasks, shownTasks, sizeof(h.tasks));
    snprintf(h.version, sizeof(h.version), "%s", STUDIO_VERSION);
    h.dataOffset = (uint32_t)engine_state_data(state);
    f = open_utf8(path, "wb");
    ok = f != NULL && fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(state, 1, used, f) == used;
    if (f != NULL && fclose(f) != 0) {
        ok = 0;
    }
    free(state);
    if (!ok) {
        snprintf(err, errSize, "Cannot write %s.", path);
        return -1;
    }
    plog_printf("studio: saved the timeline's frame %ld to %s", cur, path);
    return 0;
}

/* Checks a state file's header and its state. Returns 0, or -1 with the reason in err. */
static int check_state_file(const char *path, const uint8_t *data, size_t size, char *err, int errSize) {
    StateFileHeader h;
    uint32_t layout;
    uint32_t rom;

    if (size < sizeof(h) || memcmp(data, STATE_FILE_MAGIC, sizeof(h.magic)) != 0) {
        snprintf(err, errSize, "%s is not a studio state file.", path);
        return -1;
    }
    memcpy(&h, data, sizeof(h));
    if (h.format != STATE_FILE_FORMAT) {
        snprintf(err, errSize, "%s has state format %u. This build reads format %d.", path, h.format,
                 STATE_FILE_FORMAT);
        return -1;
    }
    h.version[sizeof(h.version) - 1] = 0;
    engine_identity(engine, &layout, &rom);
    if (h.gameHash != STUDIO_GAME_HASH || h.layout != layout) {
        if (strcmp(h.version, STUDIO_VERSION) != 0) {
            snprintf(err, errSize,
                     "%s was saved by another build of the studio (version %s; this is version %s). A state file "
                     "loads only in the build that saved it.",
                     path, h.version, STUDIO_VERSION);
        } else {
            snprintf(err, errSize,
                     "%s was saved by another build of the studio. A state file loads only in the build that saved "
                     "it.",
                     path);
        }
        return -1;
    }
    if (h.rom != rom) {
        snprintf(err, errSize, "%s was saved with another ROM.", path);
        return -1;
    }
    if (h.headerSize != sizeof(h) || h.stateSize != size - sizeof(h) || h.taskCount > MAX_TASKS ||
        hash_bytes(data + sizeof(h), (size_t)h.stateSize) != h.checksum ||
        engine_state_used(data + sizeof(h)) != h.stateSize) {
        snprintf(err, errSize, "%s is damaged: its size or its checksum is wrong.", path);
        return -1;
    }
    return 0;
}

int studio_load_state(const char *path, char *err, int errSize) {
    StateFileHeader h;
    size_t size;
    uint8_t *data = read_utf8(path, &size);

    if (data == NULL) {
        snprintf(err, errSize, "Cannot read %s.", path);
        return -1;
    }
    if (check_state_file(path, data, size, err, errSize) != 0) {
        free(data);
        return -1;
    }
    memcpy(&h, data, sizeof(h));
    if (engine_load(engine, data + sizeof(h)) != 0) {
        free(data);
        snprintf(err, errSize, "The state in %s does not load.", path);
        return -1;
    }
    free(data);
    /* A new timeline from the loaded frame, as studio_goto_level starts one. */
    while (nkeys > 0) {
        drop_key(nkeys - 1);
    }
    spacing = FIRST_SPACING;
    length = 0;
    cur = 0;
    seekPending = 0;
    stepRequests = 0;
    playing = 0;
    paused = 1;
    shownCount = (int)h.taskCount;
    memcpy(shownTasks, h.tasks, sizeof(shownTasks));
    showPending = 1;
    snprintf(origin, sizeof(origin), "state %016llx", (unsigned long long)h.checksum);
    forget_marks();
    save_key();
    if (selftest) {
        record_selftest();
    }
    plog_printf("studio: loaded %s (frame %u of the timeline that saved it)", path, h.frame);
    return 0;
}

const char *studio_states_folder(void) {
    static char folder[1024];
    char *base;

    if (folder[0] != 0) {
        return folder;
    }
    base = SDL_GetPrefPath("weemitt", "Glover");
    if (base == NULL) {
        return NULL;
    }
    snprintf(folder, sizeof(folder), "%sstudio", base);
    make_dir_utf8(folder);
    snprintf(folder, sizeof(folder), "%sstudio/states/", base);
    make_dir_utf8(folder);
    SDL_free(base);
    return folder;
}

static int compare_names(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

/* Adds the file name to the list when it ends in ext and its name fits. */
static int add_listed(char (*names)[128], int n, const char *file, const char *ext) {
    size_t len = strlen(file);
    size_t e = strlen(ext);

    if (len > e && len - e < sizeof(names[0]) && strcmp(file + len - e, ext) == 0) {
        memcpy(names[n], file, len - e);
        names[n][len - e] = 0;
        return n + 1;
    }
    return n;
}

static int list_folder(const char *folder, const char *ext, char (*names)[128], int max);

int studio_list_states(char (*names)[128], int max) {
    return list_folder(studio_states_folder(), ".state", names, max);
}

/* The names of the files NAME.EXT in the folder, sorted. */
static int list_folder(const char *folder, const char *ext, char (*names)[128], int max) {
    int n = 0;
#ifdef _WIN32
    char pattern[1100];
    wchar_t wpattern[1100];
    WIN32_FIND_DATAW fd;
    HANDLE find;

    if (folder == NULL) {
        return 0;
    }
    /* The names come as UTF-16 and go into the list as UTF-8. */
    snprintf(pattern, sizeof(pattern), "%s*%s", folder, ext);
    if (MultiByteToWideChar(CP_UTF8, 0, pattern, -1, wpattern, 1100) == 0) {
        return 0;
    }
    find = FindFirstFileW(wpattern, &fd);
    if (find == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        char file[512];

        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, file, sizeof(file), NULL, NULL) != 0) {
            n = add_listed(names, n, file, ext);
        }
    } while (n < max && FindNextFileW(find, &fd));
    FindClose(find);
#else
    DIR *d = folder != NULL ? opendir(folder) : NULL;
    struct dirent *ent;

    if (d == NULL) {
        return 0;
    }
    while (n < max && (ent = readdir(d)) != NULL) {
        n = add_listed(names, n, ent->d_name, ext);
    }
    closedir(d);
#endif
    qsort(names, n, sizeof(names[0]), compare_names);
    return n;
}

int studio_listed_state_path(const char *name, char *path, int pathSize) {
    const char *folder = studio_states_folder();

    if (folder == NULL) {
        return -1;
    }
    snprintf(path, pathSize, "%s%s.state", folder, name);
    return 0;
}

/* 1 when the name, up to its first '.' and without spaces at its end, is a device name of Windows (CON, NUL, COM1
   and the others). Windows opens the device for such a name in any folder. */
static int windows_device_name(const char *name, size_t len) {
    static const char *const devices[] = {"con", "prn", "aux", "nul"};
    char stem[8];
    size_t n = 0;
    size_t i;

    while (n < len && name[n] != '.') {
        n++;
    }
    while (n > 0 && name[n - 1] == ' ') {
        n--;
    }
    if (n < 3 || n > 4) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        char c = name[i];

        stem[i] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
    }
    stem[n] = 0;
    if (n == 3) {
        for (i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
            if (strcmp(stem, devices[i]) == 0) {
                return 1;
            }
        }
        return 0;
    }
    return (memcmp(stem, "com", 3) == 0 || memcmp(stem, "lpt", 3) == 0) && stem[3] >= '0' && stem[3] <= '9';
}

int studio_state_path(const char *name, char *path, int pathSize, char *err, int errSize) {
    const char *folder = studio_states_folder();
    size_t len = strlen(name);
    size_t i;

    if (folder == NULL) {
        snprintf(err, errSize, "There is no folder for state files: %s", SDL_GetError());
        return -1;
    }
    if (len > 6 && strcmp(name + len - 6, ".state") == 0) {
        len -= 6;
    }
    while (len > 0 && name[len - 1] == ' ') {
        /* Spaces at the end are left out: "route " is "route". */
        len--;
    }
    if (len == 0) {
        snprintf(err, errSize, "Give it a name.");
        return -1;
    }
    if (len > 100) {
        snprintf(err, errSize, "A name has at most 100 characters.");
        return -1;
    }
    if (name[0] == '.' || name[0] == ' ') {
        snprintf(err, errSize, "A name cannot start with '.' or a space.");
        return -1;
    }
    for (i = 0; i < len; i++) {
        char c = name[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' ||
              c == '_' || c == '.')) {
            snprintf(err, errSize, "A name can hold letters, digits, spaces, '-', '_' and '.' only.");
            return -1;
        }
    }
    if (windows_device_name(name, len)) {
        snprintf(err, errSize, "Windows keeps that name for a device. Give the state another name.");
        return -1;
    }
    snprintf(path, pathSize, "%s%.*s.state", folder, (int)len, name);
    return 0;
}

int studio_state_exists(const char *name) {
    char path[1024];
    char err[256];
    FILE *f;

    if (studio_state_path(name, path, sizeof(path), err, sizeof(err)) != 0) {
        return 0;
    }
    f = open_utf8(path, "rb");
    if (f == NULL) {
        return 0;
    }
    fclose(f);
    return 1;
}

const char *studio_controls_help(void) {
    return "Paused. D-pad left, right: a frame back or on. L, R: a second back or on. A or all C buttons: play on "
           "from here. B: play the recording.";
}

/* A held button acts once, then repeats after a short wait. */
static int pressed_or_repeated(unsigned short now, unsigned short bit) {
    if (!(now & bit)) {
        return 0;
    }
    if (!(prevPoll & bit)) {
        holdFrames = 0;
        return 1;
    }
    return holdFrames > 15 && holdFrames % 3 == 0;
}

void studio_poll(unsigned short *buttons, signed char *x, signed char *y) {
    unsigned short b = *buttons;
    int chord = (b & C_BUTTONS) == C_BUTTONS && (prevPoll & C_BUTTONS) != C_BUTTONS;

    if ((b & C_BUTTONS) == C_BUTTONS) {
        chordMask = C_BUTTONS;
    } else if ((b & C_BUTTONS) == 0) {
        chordMask = 0;
    }
    if (!(b & BTN_A)) {
        resumeMask = 0;
    }
    if (chord) {
        studio_set_paused(!paused);
    } else if (paused) {
        holdFrames++;
        if (pressed_or_repeated(b, BTN_DL)) {
            studio_request_seek(cur - 1);
        } else if (pressed_or_repeated(b, BTN_DR)) {
            studio_step_forward();
        } else if (pressed_or_repeated(b, BTN_L)) {
            studio_request_seek(cur - JUMP);
        } else if (pressed_or_repeated(b, BTN_R)) {
            studio_request_seek(cur + JUMP);
        } else if ((b & BTN_A) && !(prevPoll & BTN_A)) {
            /* The A that resumes is the studio's: the game gets A again after it has been released. */
            studio_set_paused(0);
            resumeMask = BTN_A;
        } else if ((b & BTN_B) && !(prevPoll & BTN_B)) {
            studio_play_recording();
        }
    }
    prevPoll = b;
    if (paused) {
        /* A step at the end of the timeline records no buttons: the ones held are the studio's controls. */
        *buttons = 0;
        *x = 0;
        *y = 0;
    } else {
        *buttons = b & ~(chordMask | resumeMask);
    }
}

unsigned short studio_input_mask(void) {
    return paused ? 0xFFFF : chordMask | resumeMask;
}

int studio_input_at(long f, unsigned short *buttons, signed char *x, signed char *y) {
    if (f < 0 || f >= length) {
        return 0;
    }
    *buttons = inputs[f].buttons;
    *x = inputs[f].x;
    *y = inputs[f].y;
    return 1;
}

/* ---- The bar. */

void studio_info(StudioInfo *info) {
    long i;
    double bytes = 0.0;

    for (i = 0; i < nkeys; i++) {
        bytes += keys[i].capacity;
    }
    info->frame = cur;
    info->length = length;
    info->states = nkeys;
    info->spacing = spacing;
    info->megabytes = bytes / 1048576.0;
    info->paused = paused;
    info->playing = playing;
}

/* ---- Points and the search. */

static char romPath[1024];
static long startFrame = -1;
static float startAt[3];
static StudioPoint points[STUDIO_MAX_POINTS];
static int npoints;

static int searchProc = -1;
static long searchStart;   /* the start frame of the latest search */
static long searchWindow;  /* the frames of its window */
static Uint32 lastPoll;
static StudioSearchInfo search;
static EngineInput *best;  /* the best inputs of the window */
static long bestLength;
/* The timeline before the first studio_take_result of the latest search. It stays until the player plays on,
   undoes the take or starts a new search. */
static EngineInput *undoInputs;
static long undoLength;
static long undoStart = -1;
static Uint32 searchStartTicks;  /* when the latest search started (SDL_GetTicks) */

static void drop_undo(void) {
    free(undoInputs);
    undoInputs = NULL;
    undoLength = 0;
    undoStart = -1;
    search.takenFrame = -1;
}

void studio_set_rom_path(const char *path) {
    /* The search reads the ROM from its own folder: a relative path must be made absolute. */
#ifdef _WIN32
    if (_fullpath(romPath, path, sizeof(romPath)) == NULL) {
        snprintf(romPath, sizeof(romPath), "%s", path);
    }
#else
    if (realpath(path, romPath) == NULL) {
        snprintf(romPath, sizeof(romPath), "%s", path);
    }
#endif
}

int studio_input_changed(long f) {
    if (undoInputs == NULL || f < undoStart || f >= length) {
        return 0;
    }
    return f >= undoLength || memcmp(&inputs[f], &undoInputs[f], sizeof(EngineInput)) != 0;
}

/* The position (Vec3 at 0x34 of gloveworld's Actor) of the glove or the ball now. */
static void actor_position(int ofBall, float out[3]) {
    uint32_t a = engine_symbol(ofBall ? "ball" : "hand") + 0x34;

    out[0] = engine_readf(engine, a);
    out[1] = engine_readf(engine, a + 4);
    out[2] = engine_readf(engine, a + 8);
}

void studio_mark_start(void) {
    startFrame = cur;
    actor_position(0, startAt);
}

static StudioPoint *new_point(void) {
    StudioPoint *p;

    if (npoints == STUDIO_MAX_POINTS) {
        return NULL;
    }
    p = &points[npoints++];
    memset(p, 0, sizeof(*p));
    p->frame = cur;
    return p;
}

void studio_add_place(int ofBall, float radius) {
    StudioPoint *p = new_point();

    if (p != NULL) {
        p->place = 1;
        p->ofBall = ofBall;
        p->radius = radius;
        actor_position(ofBall, p->at);
    }
}

void studio_add_level_end(void) {
    StudioPoint *p = new_point();

    if (p != NULL) {
        p->place = 0;
        /* gameInfo.exitFlags (the point's condition, see studio_start_search). A search needs a route that already
           ends the level: the point gives it no distance to follow. */
        if (!(engine_read8(engine, engine_symbol("gameInfo") + 0x54) & 2)) {
            snprintf(search.message, sizeof(search.message),
                     "The level is not done at frame %ld. The search needs a route that ends the level: mark this "
                     "point on a frame after the level's exit has started.", cur);
        } else {
            search.message[0] = 0;
        }
    }
}

void studio_remove_point(int i) {
    if (i >= 0 && i < npoints) {
        memmove(&points[i], &points[i + 1], (npoints - i - 1) * sizeof(StudioPoint));
        npoints--;
    }
}

long studio_start_frame(void) {
    return startFrame;
}

int studio_points(const StudioPoint **out) {
    *out = points;
    return npoints;
}

/* Forgets the points, the search and the take of the old timeline when a new timeline starts: their frames and
   inputs belong to the old one. A running search is asked to stop. */
static void forget_marks(void) {
    if (searchProc >= 0 && proc_running(searchProc)) {
        studio_stop_search();
        plog_printf("studio: the timeline starts again, so the search in %s stops", search.folder);
    }
    searchProc = -1;
    startFrame = -1;
    npoints = 0;
    memset(&search, 0, sizeof(search));
    search.bestFrame = -1;
    search.firstFrame = -1;
    search.baseFrame = -2;
    search.takenFrame = -1;
    free(best);
    best = NULL;
    bestLength = 0;
    drop_undo();
}

/* Goes to a frame at once (the window picks up the new picture at its next seek). */
static void seek_now(long target) {
    PortFrameTask tasks[16];

    studio_request_seek(target);
    studio_apply_seek(tasks, 16);
}

/* Writes the text as a JSON string, with its backslashes and quotes escaped. */
static void json_string_out(FILE *f, const char *text) {
    fputc('"', f);
    for (; *text; text++) {
        if (*text == '\\' || *text == '"') {
            fputc('\\', f);
        }
        fputc(*text, f);
    }
    fputc('"', f);
}

int studio_start_search(int workers, double seconds, const char *limits) {
    char path[1024];
    char cmd[4096] = "";
    char *base;
    EngineStateInfo info;
    uint8_t *state;
    FILE *f;
    long back = cur;
    long last = startFrame;
    long k;
    int i;

    search.message[0] = 0;
    if (searchProc >= 0 && proc_running(searchProc)) {
        snprintf(search.message, sizeof(search.message), "A search is running. Stop it first.");
        return -1;
    }
    if (startFrame < 0) {
        snprintf(search.message, sizeof(search.message), "Mark a start point on the timeline first.");
        return -1;
    }
    if (startFrame >= length) {
        snprintf(search.message, sizeof(search.message),
                 "The start point is at the end of the timeline. Mark it on an earlier frame: the search changes the "
                 "frames after it.");
        return -1;
    }
    if (npoints == 0) {
        snprintf(search.message, sizeof(search.message), "Add an end point first.");
        return -1;
    }
    for (i = 0; i < npoints; i++) {
        if (points[i].frame <= startFrame) {
            snprintf(search.message, sizeof(search.message),
                     "Point %d is at frame %ld, not after the start point (frame %ld). Mark the points after the start.",
                     i + 1, points[i].frame, startFrame);
            return -1;
        }
    }
    for (i = 0; i < npoints; i++) {
        if (points[i].frame > last) {
            last = points[i].frame;
        }
    }
    /* The window: from the start to the frame the last point was marked at, or to the end of the timeline. */
    searchWindow = (last > startFrame ? last : length) - startFrame;
    if (searchWindow > length - startFrame) {
        searchWindow = length - startFrame;
    }
    base = SDL_GetPrefPath("weemitt", "Glover");
    if (base == NULL) {
        snprintf(search.message, sizeof(search.message), "No folder for the search: %s", SDL_GetError());
        return -1;
    }
    snprintf(path, sizeof(path), "%sstudio", base);
    make_dir_utf8(path);
    /* The first number whose folder can be made: the folders of earlier searches stay. The search's files go
       through the UTF-8 helpers, so a preference folder with other letters (such as é) works on Windows too. */
    for (i = 1; i < 10000; i++) {
        snprintf(search.folder, sizeof(search.folder), "%sstudio/search-%04d", base, i);
        if (make_dir_utf8(search.folder) == 0) {
            break;
        }
    }
    SDL_free(base);
    if (i == 10000) {
        snprintf(search.message, sizeof(search.message), "Cannot make the folder %.200s", search.folder);
        search.folder[0] = 0;
        return -1;
    }
    /* A new search: the take of the old one can no longer be undone. */
    drop_undo();
    /* The start state, saved at the start frame. */
    seek_now(startFrame);
    engine_state_info(engine, &info);
    state = malloc(info.total);
    memset(state, 0, ENGINE_STATE_HEADER);
    snprintf(path, sizeof(path), "%s/start.state", search.folder);
    f = open_utf8(path, "wb");
    if (state == NULL || f == NULL || fwrite(state, 1, engine_save(engine, state), f) == 0) {
        free(state);
        if (f != NULL) {
            fclose(f);
        }
        seek_now(back);
        snprintf(search.message, sizeof(search.message), "Cannot write %.200s", path);
        return -1;
    }
    fclose(f);
    free(state);
    seek_now(back);
    /* The base run: the timeline's frames from the start, as whole frames from the state. */
    snprintf(path, sizeof(path), "%s/base.events", search.folder);
    f = open_utf8(path, "w");
    if (f == NULL) {
        snprintf(search.message, sizeof(search.message), "Cannot write %.200s", path);
        return -1;
    }
    fprintf(f, "# The studio's timeline from frame %ld\n#state start.state\n", startFrame);
    for (k = startFrame; k < length; k++) {
        char line[64];

        input_format(&inputs[k], line, sizeof(line));
        fprintf(f, "F %s\n", line);
    }
    fclose(f);
    snprintf(path, sizeof(path), "%s/search.json", search.folder);
    f = open_utf8(path, "w");
    if (f == NULL) {
        snprintf(search.message, sizeof(search.message), "Cannot write %.200s", path);
        return -1;
    }
    fprintf(f, "{\n  \"rom\": ");
    json_string_out(f, romPath);
    fprintf(f, ",\n  \"run\": \"base.events\",\n  \"window\": [0, %ld],\n  \"lookahead\": 200,\n", searchWindow);
    fprintf(f,
            "  \"out\": \".\",\n  \"workers\": %d,\n  \"seconds\": %g,\n  \"limits\": \"%s\",\n  \"loadFrames\": %d,\n"
            "  \"points\": [\n",
            workers, seconds, limits, STUDIO_LOAD_FRAMES);
    for (i = 0; i < npoints; i++) {
        if (points[i].place) {
            fprintf(f, "    {\"place\": [%.9g, %.9g, %.9g], \"radius\": %.9g, \"of\": \"%s\"}", points[i].at[0],
                    points[i].at[1], points[i].at[2], points[i].radius, points[i].ofBall ? "ball" : "glove");
        } else {
            /* gameInfo.exitFlags: bit 1 is set from the frame the level's exit starts after the level is done. */
            fprintf(f, "    {\"condition\": \"gameInfo+0x54:u8 & 2\"}");
        }
        fprintf(f, "%s\n", i + 1 < npoints ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    quote_arg(cmd, sizeof(cmd), self_path());
    quote_arg(cmd, sizeof(cmd), "search");
    quote_arg(cmd, sizeof(cmd), path);
    snprintf(path, sizeof(path), "%s/search.log", search.folder);
    searchProc = proc_spawn(cmd, path);
    if (searchProc < 0) {
        snprintf(search.message, sizeof(search.message), "The search did not start.");
        return -1;
    }
    searchStart = startFrame;
    searchStartTicks = SDL_GetTicks();
    search.seconds = 0.0;
    search.tries = 0;
    search.timeLimit = seconds;
    search.baseFrame = -2;
    search.running = 1;
    search.haveBest = 0;
    search.bestFrame = -1;
    search.firstFrame = -1;
    search.bestLoads = 0;
    search.firstLoads = 0;
    search.progress = 0.0;
    free(best);
    best = NULL;
    bestLength = 0;
    plog_printf("studio: search started in %s", search.folder);
    return 0;
}

void studio_stop_search(void) {
    char path[1024];
    FILE *f;

    if (search.folder[0] == 0) {
        return;
    }
    snprintf(path, sizeof(path), "%s/stop", search.folder);
    f = open_utf8(path, "w");
    if (f != NULL) {
        fclose(f);
    }
}

/* The first line of a "best FRAME PROGRESS LOADS" file as a timeline frame (-1: the end point is not reached), or -2
   when the file has none yet. */
static long score_frame(const char *path, int *loads) {
    size_t n;
    char *text = (char *)read_utf8(path, &n);
    long frame;
    double progress;
    long out = -2;

    *loads = 0;
    if (text == NULL) {
        return -2;
    }
    text = realloc(text, n + 1);
    text[n] = 0;
    if (memchr(text, '\n', n) != NULL && sscanf(text, "best %ld %lf %d", &frame, &progress, loads) >= 2) {
        out = frame != 0x7FFFFFFFL ? searchStart + frame + 1 : -1;
    }
    free(text);
    return out;
}

/* The search's time and tries, from the workers' status files ("TRIES SECONDS"), the score of the player's own
   route (the first line that worker 0 writes is the score of the base inputs), and the last line of search.log
   when the search ended without a result. */
static void poll_progress(void) {
    char path[1100];
    long tries = 0;
    int i;

    if (search.running) {
        search.seconds = (SDL_GetTicks() - searchStartTicks) / 1000.0;
    }
    for (i = 0; i < 256; i++) {
        size_t n;
        char *text;
        long count;
        double secs;

        snprintf(path, sizeof(path), "%s/worker-%d.status", search.folder, i);
        text = (char *)read_utf8(path, &n);
        if (text == NULL) {
            break;
        }
        text = realloc(text, n + 1);
        text[n] = 0;
        if (sscanf(text, "%ld %lf", &count, &secs) == 2) {
            tries += count;
        }
        free(text);
    }
    search.tries = tries;
    if (search.baseFrame == -2) {
        snprintf(path, sizeof(path), "%s/worker-0.txt", search.folder);
        search.baseFrame = score_frame(path, &search.baseLoads);
    }
    if (!search.running && !search.haveBest && search.message[0] == 0) {
        /* The search ended without a result: its log's last line says why. */
        size_t n;
        char *text;

        snprintf(path, sizeof(path), "%s/search.log", search.folder);
        text = (char *)read_utf8(path, &n);
        if (text != NULL) {
            char *end;
            char *start;

            text = realloc(text, n + 1);
            text[n] = 0;
            end = text + n;
            while (end > text && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) {
                end--;
            }
            *end = 0;
            start = end;
            while (start > text && start[-1] != '\n' && start[-1] != '\r') {
                start--;
            }
            snprintf(search.message, sizeof(search.message), "The search ended without a result: %s",
                     start[0] ? start : "(search.log is empty)");
            free(text);
        }
    }
}

/* Reads the search's best.txt: its last line, "# best FRAME PROGRESS LOADS |input|input|...". */
static void poll_search(void) {
    char path[1024];
    char *text;
    char *line;
    size_t n;
    long frame;
    double progress;
    int loads = 0;
    char *p;
    long count = 0;
    EngineInput *in;

    search.running = searchProc >= 0 && proc_running(searchProc);
    poll_progress();
    snprintf(path, sizeof(path), "%s/best.txt", search.folder);
    text = (char *)read_utf8(path, &n);
    if (text == NULL) {
        return;
    }
    text = realloc(text, n + 1);
    text[n] = 0;
    line = strstr(text, "# best ");
    if (line == NULL || sscanf(line, "# best %ld %lf %d", &frame, &progress, &loads) < 2) {
        free(text);
        return;
    }
    for (p = strchr(line, '|'); p != NULL; p = strchr(p + 1, '|')) {
        count++;
    }
    in = calloc(count > 0 ? count : 1, sizeof(EngineInput));
    count = 0;
    for (p = strchr(line, '|'); p != NULL && in != NULL; p = strchr(p + 1, '|')) {
        char item[128];
        char err[64];
        const char *end = strchr(p + 1, '|');
        size_t len = end ? (size_t)(end - p - 1) : strcspn(p + 1, "\r\n");

        if (len >= sizeof(item)) {
            break;
        }
        memcpy(item, p + 1, len);
        item[len] = 0;
        if (input_parse(item, &in[count], err, sizeof(err)) != 0) {
            break;
        }
        count++;
    }
    free(text);
    if (in == NULL || count != searchWindow) {
        free(in);
        return;
    }
    free(best);
    best = in;
    bestLength = count;
    search.haveBest = 1;
    /* The goal at read F: the end point holds after F + 1 frames from the start. */
    search.bestFrame = frame != 0x7FFFFFFFL ? searchStart + frame + 1 : -1;
    search.bestLoads = loads;
    search.progress = progress;
}

void studio_search_info(StudioSearchInfo *info) {
    Uint32 now = SDL_GetTicks();
    int had = search.haveBest;

    if (search.folder[0] != 0 && searchProc >= 0 && (search.running || !had || search.baseFrame == -2)
        && now - lastPoll > 500) {
        lastPoll = now;
        poll_search();
        if (!had && search.haveBest) {
            /* The first result seen is the base: the player's route, or a little better already. */
            search.firstFrame = search.bestFrame;
            search.firstLoads = search.bestLoads;
        }
    }
    *info = search;
}

void studio_take_result(void) {
    long newLength;
    long k;

    if (best == NULL || searchStart > length) {
        return;
    }
    if (undoInputs == NULL) {
        /* The first take of this search keeps the timeline from before it. A later take replaces the taken inputs
           and keeps that undo. */
        undoInputs = malloc((length > 0 ? length : 1) * sizeof(EngineInput));
        if (undoInputs == NULL) {
            return;
        }
        memcpy(undoInputs, inputs, length * sizeof(EngineInput));
        undoLength = length;
        undoStart = searchStart;
    }
    search.takenFrame = search.bestFrame;
    search.takenLoads = search.bestLoads;
    /* The best inputs over the window; after it the old timeline, as the search played it. The timeline ends where
       the end point is reached, or after the window. */
    newLength = search.bestFrame >= 0 ? search.bestFrame : searchStart + bestLength;
    truncate_at(searchStart);
    while (inputsCapacity < newLength) {
        inputsCapacity = inputsCapacity ? inputsCapacity * 2 : 4096;
        inputs = realloc(inputs, inputsCapacity * sizeof(EngineInput));
    }
    for (k = searchStart; k < newLength; k++) {
        if (k < searchStart + bestLength) {
            inputs[k] = best[k - searchStart];
        } else if (k < undoLength) {
            inputs[k] = undoInputs[k];
        } else {
            memset(&inputs[k], 0, sizeof(EngineInput));
        }
    }
    length = newLength;
    studio_request_seek(searchStart);
}

int studio_can_undo_take(void) {
    return undoInputs != NULL && undoStart >= 0;
}

void studio_undo_take(void) {
    if (!studio_can_undo_take()) {
        return;
    }
    truncate_at(undoStart);
    while (inputsCapacity < undoLength) {
        inputsCapacity = inputsCapacity ? inputsCapacity * 2 : 4096;
        inputs = realloc(inputs, inputsCapacity * sizeof(EngineInput));
    }
    memcpy(inputs, undoInputs, undoLength * sizeof(EngineInput));
    length = undoLength;
    studio_request_seek(undoStart);
    drop_undo();
}

int studio_has_work(void) {
    return startFrame >= 0 || npoints > 0 || (searchProc >= 0 && proc_running(searchProc)) || best != NULL
           || undoInputs != NULL;
}

/* The files that a search writes in its folder (bruteforce/src/search.c and studio_start_search). */
static void remove_search_folder(const char *folder) {
    static const char *const files[] = { "start.state", "base.events", "search.json", "search.log", "best.txt",
                                         "log.txt", "stop", NULL };
    static const char *const kinds[] = { "txt", "status", "done", NULL };
    char path[1200];
    int i, k;

    for (i = 0; files[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", folder, files[i]);
        remove_utf8(path, 0);
    }
    for (i = 0; i < 256; i++) {
        for (k = 0; kinds[k]; k++) {
            snprintf(path, sizeof(path), "%s/worker-%d.%s", folder, i, kinds[k]);
            remove_utf8(path, 0);
        }
    }
    remove_utf8(folder, 1);
}

/* Calls fn for every search folder that exists, with its path. */
static int each_search_folder(void (*fn)(const char *folder, void *user), void *user) {
    char *base = SDL_GetPrefPath("weemitt", "Glover");
    char folder[1100];
    char probe[1200];
    int count = 0;
    int i;

    if (base == NULL) {
        return 0;
    }
    for (i = 1; i < 10000; i++) {
        FILE *f;

        snprintf(folder, sizeof(folder), "%sstudio/search-%04d", base, i);
        /* Every search folder holds search.json or start.state (a failed start may hold only that). */
        snprintf(probe, sizeof(probe), "%s/start.state", folder);
        f = open_utf8(probe, "rb");
        if (f == NULL) {
            snprintf(probe, sizeof(probe), "%s/search.json", folder);
            f = open_utf8(probe, "rb");
        }
        if (f != NULL) {
            fclose(f);
            fn(folder, user);
            count++;
        }
    }
    SDL_free(base);
    return count;
}

static void delete_if_old(const char *folder, void *user) {
    int *n = (int *)user;

    if (search.folder[0] == 0 || strcmp(folder, search.folder) != 0) {
        remove_search_folder(folder);
        (*n)++;
    }
}

int studio_delete_old_searches(void) {
    int n = 0;

    /* The latest search, running or not, keeps its folder. */
    each_search_folder(delete_if_old, &n);
    plog_printf("studio: removed %d old search folders", n);
    return n;
}

static void add_size(const char *folder, void *user) {
    double *bytes = (double *)user;
    char path[1200];
    FILE *f;

    snprintf(path, sizeof(path), "%s/start.state", folder);
    f = open_utf8(path, "rb");
    if (f != NULL) {
        fseek(f, 0, SEEK_END);
        *bytes += (double)ftell(f);
        fclose(f);
    }
}

int studio_search_folders(double *megabytes) {
    double bytes = 0.0;
    int n = each_search_folder(add_size, &bytes);

    *megabytes = bytes / 1048576.0;
    return n;
}

const char *studio_origin(void) {
    return origin;
}

const char *studio_routes_folder(void) {
    static char folder[1024];
    char *base;

    if (folder[0] != 0) {
        return folder;
    }
    base = SDL_GetPrefPath("weemitt", "Glover");
    if (base == NULL) {
        return NULL;
    }
    snprintf(folder, sizeof(folder), "%sstudio", base);
    make_dir_utf8(folder);
    snprintf(folder, sizeof(folder), "%sstudio/routes/", base);
    make_dir_utf8(folder);
    SDL_free(base);
    return folder;
}

int studio_list_routes(char (*names)[128], int max) {
    return list_folder(studio_routes_folder(), ".route", names, max);
}

/* The path of NAME.route, with the rules of state names. */
static int route_path(const char *name, char *path, int pathSize, char *err, int errSize) {
    char statePath[1100];
    const char *folder = studio_routes_folder();
    const char *file;
    size_t n;

    if (folder == NULL) {
        snprintf(err, errSize, "There is no folder for route files: %s", SDL_GetError());
        return -1;
    }
    if (studio_state_path(name, statePath, sizeof(statePath), err, errSize) != 0) {
        return -1;
    }
    /* The checked name is the state path's file name without .state. */
    file = statePath + strlen(statePath);
    while (file > statePath && file[-1] != '/' && file[-1] != '\\') {
        file--;
    }
    n = strlen(file) - 6;
    snprintf(path, pathSize, "%s%.*s.route", folder, (int)n, file);
    return 0;
}

#define ROUTE_MAGIC "# weemitt studio route 1"

int studio_save_route(const char *name, char *err, int errSize) {
    char path[1200];
    FILE *f;
    long k;
    int ok;

    if (route_path(name, path, sizeof(path), err, errSize) != 0) {
        return -1;
    }
    f = open_utf8(path, "w");
    if (f == NULL) {
        snprintf(err, errSize, "Cannot write %s.", path);
        return -1;
    }
    /* The header names the start of the timeline. Each F line is one update's input, as in a search's base.events
       (bruteforce/README.md). */
    fprintf(f, "%s\n#origin %s\n#frames %ld\n", ROUTE_MAGIC, origin, length);
    for (k = 0; k < length; k++) {
        char line[64];

        input_format(&inputs[k], line, sizeof(line));
        fprintf(f, "F %s\n", line);
    }
    ok = !ferror(f);
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        snprintf(err, errSize, "Cannot write %s.", path);
        return -1;
    }
    plog_printf("studio: saved the route of %ld frames to %s", length, path);
    return 0;
}

int studio_load_route(const char *name, char *err, int errSize) {
    char path[1200];
    char fileOrigin[64] = "";
    size_t size;
    char *text;
    char *line;
    EngineInput *in = NULL;
    long count = 0;
    long cap = 0;
    long lineNo = 0;

    if (route_path(name, path, sizeof(path), err, errSize) != 0) {
        return -1;
    }
    text = (char *)read_utf8(path, &size);
    if (text == NULL) {
        snprintf(err, errSize, "Cannot read %s.", path);
        return -1;
    }
    text = realloc(text, size + 1);
    text[size] = 0;
    if (strncmp(text, ROUTE_MAGIC, strlen(ROUTE_MAGIC)) != 0) {
        snprintf(err, errSize, "%s is not a route file.", path);
        free(text);
        return -1;
    }
    for (line = strtok(text, "\r\n"); line != NULL; line = strtok(NULL, "\r\n")) {
        lineNo++;
        if (!strncmp(line, "#origin ", 8)) {
            snprintf(fileOrigin, sizeof(fileOrigin), "%s", line + 8);
        } else if (line[0] == 'F' && line[1] == ' ') {
            char perr[64];

            if (count == cap) {
                EngineInput *grown;

                cap = cap ? cap * 2 : 4096;
                grown = realloc(in, cap * sizeof(EngineInput));
                if (grown == NULL) {
                    free(in);
                    free(text);
                    snprintf(err, errSize, "Out of memory for the route.");
                    return -1;
                }
                in = grown;
            }
            if (input_parse(line + 2, &in[count], perr, sizeof(perr)) != 0) {
                snprintf(err, errSize, "%s, line %ld: %s", path, lineNo, perr);
                free(in);
                free(text);
                return -1;
            }
            count++;
        }
    }
    free(text);
    if (strcmp(fileOrigin, origin) != 0) {
        snprintf(err, errSize, "The route starts at %s, and this timeline at %s. Start the timeline there first.",
                 fileOrigin[0] ? fileOrigin : "an unknown place", origin);
        free(in);
        return -1;
    }
    /* The route becomes the timeline from its first frame, which keeps its state. */
    forget_marks();
    truncate_at(0);
    while (inputsCapacity < count) {
        inputsCapacity = inputsCapacity ? inputsCapacity * 2 : 4096;
        inputs = realloc(inputs, inputsCapacity * sizeof(EngineInput));
    }
    if (count > 0) {
        memcpy(inputs, in, count * sizeof(EngineInput));
    }
    free(in);
    length = count;
    studio_request_seek(0);
    plog_printf("studio: loaded the route %s (%ld frames)", path, count);
    return 0;
}

/* Three circles around the centre: in the planes XY, YZ and XZ. */
static long add_sphere(PortDebugLine *out, long n, long max, const float c[3], float r, unsigned int color) {
    int plane;
    int i;

    for (plane = 0; plane < 3; plane++) {
        for (i = 0; i < 16 && n < max; i++) {
            double a0 = i * 2.0 * M_PI / 16.0;
            double a1 = (i + 1) * 2.0 * M_PI / 16.0;
            float p0[3] = {c[0], c[1], c[2]};
            float p1[3] = {c[0], c[1], c[2]};
            int u = plane == 1 ? 1 : 0;
            int v = plane == 0 ? 1 : 2;

            p0[u] += (float)(r * cos(a0));
            p0[v] += (float)(r * sin(a0));
            p1[u] += (float)(r * cos(a1));
            p1[v] += (float)(r * sin(a1));
            memcpy(out[n].a, p0, sizeof(p0));
            memcpy(out[n].b, p1, sizeof(p1));
            out[n].color = color;
            n++;
        }
    }
    return n;
}

long studio_overlay_lines(PortDebugLine *out, long max) {
    long n = 0;
    int i;

    if (startFrame >= 0) {
        n = add_sphere(out, n, max, startAt, 15.0f, 0xFF40FF40u);
    }
    for (i = 0; i < npoints; i++) {
        if (points[i].place) {
            n = add_sphere(out, n, max, points[i].at, points[i].radius, i + 1 == npoints ? 0xFF4040FFu : 0xFF40FFFFu);
        }
    }
    return n;
}

/* ---- The self-test. */

long studio_selftest(void) {
    PortFrameTask tasks[16];
    long recorded = length;
    long failures = 0;
    long checks = 0;
    uint32_t r = 12345;
    long i;

    if (!selftest) {
        return 0;
    }
    /* Back to the start, forward in small and large steps, and random jumps. */
    for (i = 0; i < 400 && failures < 5; i++) {
        long target;
        char line[256];

        r = r * 1664525u + 1013904223u;
        if (i % 4 == 0) {
            target = (long)((r >> 8) % (uint32_t)(recorded + 1));
        } else if (i % 4 == 1) {
            target = cur + 1;
        } else if (i % 4 == 2) {
            target = cur - (long)((r >> 8) % 7);
        } else {
            target = (cur / HASH_EVERY + 1) * HASH_EVERY;
        }
        if (target < 0 || target > recorded) {
            target = (long)((r >> 12) % (uint32_t)(recorded + 1));
        }
        studio_request_seek(target);
        studio_apply_seek(tasks, 16);
        if (cur != target) {
            printf("studio selftest: asked for frame %ld, got %ld\n", target, cur);
            failures++;
            continue;
        }
        engine_trace(engine, line, sizeof(line));
        checks++;
        if (strcmp(line, traces[target]) != 0) {
            printf("studio selftest: frame %ld differs\n  live:   %s\n  replay: %s\n", target, traces[target], line);
            failures++;
        } else if (hashes[target] != 0 && selftest_hash() != hashes[target]) {
            printf("studio selftest: frame %ld: the state's hash differs\n", target);
            failures++;
        }
    }
    printf("studio selftest: %ld frames recorded, %ld saved states (every %ld frames), %ld jumps checked, %ld failures\n",
           recorded, nkeys, spacing, checks, failures);
    return failures;
}
