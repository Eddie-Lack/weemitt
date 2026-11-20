/* The save and load test: a loaded state must play on exactly as the game did when the state was saved.

   glover saveload --rom ROM [--level N] [--frames F] [--seed S]
       Runs F game frames with generated input and saves states on the way: at the start, at a quarter, a half and
       three quarters of the way, and at the end. Then it loads them in several orders, plays the same input again
       and compares a hash of the whole game state with the first run's every 5 frames. Loading a state from later
       or earlier in the game makes the heap's gap (the part of the heap never written, docs/engine.md) shrink or
       grow. It also checks right after each load, that the gap holds only zeros, and that saving a loaded state
       gives the same bytes. Last come SHUFFLE_STEPS random steps: a load of one of the states, or of one of three
       more buffers, then a few frames, and sometimes a save into one of the three buffers. A save into a buffer that
       holds an older state copies only what changed, and a load copies only what differs (docs/engine.md), so
       this mixes many histories. Prints "saveload: ok", or the first difference of each replay, and exits with 1
       then.

   glover saveload --rom ROM [--level N] [--seed S] --save FILE --at N --frames F
       Saves the state at frame N to FILE, then prints the hash after every 10th frame up to F.
   glover saveload --rom ROM [--level N] [--seed S] --load FILE --frames F
       Loads FILE and prints the hash after every 10th frame up to F, as --save does. Two processes give the same
       lines when states carry over between processes (tests/run.py compares them).

   A state holds the game's memory, which holds data from the ROM: keep FILE out of the repository. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "tools.h"
#include "util.h"

#define SHUFFLE_STEPS 300

static Engine *e;
static EngineInput *inputs;
static int failures;

static void fail(const char *what, long frame) {
    printf("saveload: FAILED: %s at frame %ld\n", what, frame);
    failures++;
}

/* Plays frames from the engine's current frame up to end, and checks or records the hash every `every` frames. */
static void play(long end, uint64_t *hashes, int record, int every, const char *what) {
    long f;

    for (f = engine_frame(e); f < end; f++) {
        engine_step(e, &inputs[f]);
        if ((f + 1) % every == 0 || f + 1 == end) {
            uint64_t h = engine_hash(e);

            if (record) {
                hashes[f + 1] = h;
            } else if (hashes[f + 1] != h) {
                char msg[200];

                snprintf(msg, sizeof(msg), "%s: the hash differs", what);
                fail(msg, f + 1);
                return;
            }
            if (engine_check_gap(e) != 0) {
                fail("nonzero memory outside the parts of the heap a state holds", f + 1);
                return;
            }
        }
    }
}

/* Loads a state and checks what holds right after the load. Then plays to the end and compares the hashes. */
static void load_and_play(const uint8_t *state, long stateFrame, long end, uint64_t *hashes, uint8_t *again,
                          const char *what) {
    size_t used = engine_state_used(state);
    size_t data = engine_state_data(state);

    if (engine_load(e, state) != 0) {
        fail("the state does not load", stateFrame);
        return;
    }
    if (engine_frame(e) != (uint32_t)stateFrame) {
        fail("the loaded frame counter differs", stateFrame);
    }
    if (engine_save(e, again) != used || memcmp(again, state, ENGINE_STATE_HEADER) != 0 ||
        memcmp(again + data, state + data, used - data) != 0) {
        fail("a loaded state does not save the same bytes", stateFrame);
    }
    if (engine_hash(e) != hashes[stateFrame]) {
        fail("the hash right after the load differs", stateFrame);
    }
    if (engine_check_gap(e) != 0) {
        fail("nonzero memory in the heap's gap right after the load", stateFrame);
    }
    play(end, hashes, 0, 5, what);
}

/* The random steps (see the top). Frames are multiples of 5, where the first run recorded a hash. */
static void shuffle(uint8_t **states, const long *stateFrames, long frames, uint64_t *hashes, size_t size,
                    uint32_t seed) {
    uint8_t *extra[3];
    long extraFrames[3] = {-1, -1, -1};
    uint32_t r = seed * 2654435761u + 1;
    int step;
    int i;

    for (i = 0; i < 3; i++) {
        extra[i] = calloc(1, size);
    }
    for (step = 0; step < SHUFFLE_STEPS && failures == 0; step++) {
        const uint8_t *state;
        long from;
        long to;

        r = r * 1664525u + 1013904223u;
        i = (int)((r >> 16) % 8);
        if (i < 5) {
            state = states[i];
            from = stateFrames[i];
        } else if (extraFrames[i - 5] >= 0) {
            state = extra[i - 5];
            from = extraFrames[i - 5];
        } else {
            continue;
        }
        if (engine_load(e, state) != 0 || engine_hash(e) != hashes[from]) {
            fail("shuffle: the hash right after a load differs", from);
            break;
        }
        r = r * 1664525u + 1013904223u;
        to = from - from % 5 + 5 * (long)(1 + (r >> 16) % 6);
        play(to < frames ? to : frames, hashes, 0, 5, "shuffle");
        r = r * 1664525u + 1013904223u;
        if ((r >> 16) % 3 == 0 && engine_frame(e) % 5 == 0) {
            i = (int)((r >> 20) % 3);
            engine_save(e, extra[i]);
            extraFrames[i] = engine_frame(e);
        }
    }
    for (i = 0; i < 3; i++) {
        free(extra[i]);
    }
}

int saveload_main(int argc, char **argv) {
    const char *rom = NULL;
    const char *saveFile = NULL;
    const char *loadFile = NULL;
    EngineOptions opt;
    char err[256];
    long frames = 600;
    long at = -1;
    uint32_t seed = 7;
    TestInput gen;
    uint64_t *hashes;
    /* States at the start, at a quarter, a half and three quarters of the way, and at the end. */
    uint8_t *states[5];
    long stateFrames[5];
    uint8_t *again;
    size_t size;
    long f;
    int i;

    engine_default_options(&opt);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;

        if (!strcmp(a, "--rom") && next) {
            rom = argv[++i];
        } else if (!strcmp(a, "--level") && next) {
            opt.startLevel = atoi(argv[++i]);
        } else if (!strcmp(a, "--frames") && next) {
            frames = atol(argv[++i]);
        } else if (!strcmp(a, "--seed") && next) {
            seed = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(a, "--save") && next) {
            saveFile = argv[++i];
        } else if (!strcmp(a, "--load") && next) {
            loadFile = argv[++i];
        } else if (!strcmp(a, "--at") && next) {
            at = atol(argv[++i]);
        } else if (!strcmp(a, "--verbose")) {
            opt.verbose = 1;
        } else {
            die("usage: glover saveload --rom ROM [--level N] [--frames F] [--seed S] [--save FILE --at N | --load "
                "FILE]");
        }
    }
    if (rom == NULL || frames < 8) {
        die("saveload: --rom is needed, and --frames must be 8 or more");
    }
    e = engine_create(rom, &opt, err, sizeof(err));
    if (e == NULL) {
        die("saveload: %s", err);
    }
    inputs = malloc(frames * sizeof(*inputs));
    hashes = calloc(frames + 1, sizeof(*hashes));
    test_input_init(&gen, seed);
    for (f = 0; f < frames; f++) {
        test_input_next(&gen, &inputs[f]);
    }
    size = engine_state_size(e);

    if (saveFile != NULL || loadFile != NULL) {
        uint8_t *state = malloc(size);

        if (saveFile != NULL) {
            if (at < 0 || at > frames) {
                die("saveload: --save needs --at N, with N at most --frames");
            }
            play(at, hashes, 1, 1000000, "");
            if (write_file(saveFile, state, engine_save(e, state)) != 0) {
                die("saveload: cannot write %s", saveFile);
            }
        } else {
            size_t n;
            uint8_t *data = read_file(loadFile, &n);

            if (data == NULL || n < ENGINE_STATE_HEADER || n > size || engine_state_used(data) != n ||
                engine_load(e, data) != 0) {
                die("saveload: %s is not a state of this program and ROM", loadFile);
            }
            free(data);
        }
        for (f = engine_frame(e); f < frames; f++) {
            engine_step(e, &inputs[f]);
            if ((f + 1) % 10 == 0) {
                printf("frame %ld hash %016llx\n", f + 1, (unsigned long long)engine_hash(e));
            }
        }
        return failures != 0;
    }

    /* The first run: a hash every 5 frames, and the states. */
    for (i = 0; i < 5; i++) {
        states[i] = malloc(size);
        stateFrames[i] = frames * i / 4;
    }
    again = malloc(size);
    hashes[0] = engine_hash(e);
    for (i = 0; i < 5; i++) {
        play(stateFrames[i], hashes, 1, 5, "");
        engine_save(e, states[i]);
    }
    {
        EngineStateInfo info;

        engine_state_info(e, &info);
        printf("saveload: %ld frames; a state is %lu bytes (game variables %lu, memory below the heap %lu, heap %lu "
               "and %lu)\n",
               frames, (unsigned long)info.total, (unsigned long)info.gameVars, (unsigned long)info.lowRam,
               (unsigned long)info.heapLow, (unsigned long)info.heapHigh);
    }

    /* Each state after a run to the end, from the newest to the oldest. Then some orders that move the gap: a middle
       state again after the start, the start after the end, the end after the start. */
    for (i = 3; i >= 0; i--) {
        char what[64];

        snprintf(what, sizeof(what), "state %d (frame %ld)", i, stateFrames[i]);
        load_and_play(states[i], stateFrames[i], frames, hashes, again, what);
    }
    load_and_play(states[2], stateFrames[2], frames, hashes, again, "state 2 after the start");
    load_and_play(states[0], stateFrames[0], frames, hashes, again, "the start after the end");
    load_and_play(states[4], stateFrames[4], frames, hashes, again, "the end after the start");
    load_and_play(states[1], stateFrames[1], frames, hashes, again, "state 1 after the end");
    shuffle(states, stateFrames, frames, hashes, size, seed);
    if (failures == 0) {
        printf("saveload: ok\n");
    }
    return failures != 0;
}
