/* Plays inputs in the engine and prints the game's state per game frame.

   play --rom ROM [--level N] [--input FILE] [--frames N] [--watch LIST] [--trace]
   play --rom ROM --events FILE [--watch LIST] [--trace]

   --input: the engine's text format (inputs.h); with no file, or past its end, no buttons are held.
   --frames: how many game frames to play (default: the input's length).
   --watch: values to print (watch.h), separated by commas, for example "frameCount:s32,rseed:u32,hand+0x34:f32".
   --trace: weemitt's one-line state trace instead.
   --raw: print each value's bits in hex (32 bits; signed types sign-extended).

   Line k holds the state after k game frames: line 0 is the state at the start, before the game's first read of the
   controller. The game reads the controller once a game frame, so line k is also the state the k-th read sees.

   --events: a script of the parts of game frames (engine.h, engine_read), one per line: "R X Y BUTTONS..." (a read
   of the controller, with the input as in the text format), "D" (a frame of the draw thread) or "L [N]" (N frames
   of the game's logic, default 1), or "W NAME:TYPE VALUE" (write a value of the game's memory, watch.h; VALUE is a
   number, in hex with 0x). "D NAME:TYPE=VALUE ..." draws with these values written first; the game's own values
   are put back after the draw (tools/sync/events.py). Groups of events after an "L" line run at a stop inside that
   logic frame (src/run.h). Line k of the output then holds the state just before the k-th read. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "inputs.h"
#include "run.h"
#include "tools.h"
#include "util.h"
#include "watch.h"

#define MAX_WATCH 64

/* Runs an events script. Returns 0, or -1 with a message in err. */
static int run_events(Engine *e, const char *path, const Watch *watch, int nwatch, int trace, char *err,
                      size_t errSize);

static int raw;

static void print_state(Engine *e, long k, const Watch *watch, int nwatch, int trace) {
    char buf[2048];
    size_t n = snprintf(buf, sizeof(buf), "%ld:", k);
    int i;

    if (trace) {
        engine_trace(e, buf + n, sizeof(buf) - n);
    } else {
        for (i = 0; i < nwatch && n < sizeof(buf); i++) {
            buf[n++] = ' ';
            if (raw) {
                snprintf(buf + n, sizeof(buf) - n, "%08x", watch_read(e, &watch[i]));
            } else {
                watch_format(&watch[i], watch_read(e, &watch[i]), buf + n, sizeof(buf) - n);
            }
            n += strlen(buf + n);
        }
    }
    puts(buf);
}

int play_main(int argc, char **argv) {
    const char *rom = NULL;
    const char *inputPath = NULL;
    const char *eventsPath = NULL;
    const char *watchText = "frameCount:s32,gameInfo:s8,gameInfo+1:s8,rseed:u32";
    EngineOptions opt;
    Watch watch[MAX_WATCH];
    int nwatch;
    int trace = 0;
    int trimDraw = 0;
    long frames = -1;
    Inputs in;
    Engine *e;
    char err[256];
    long k;
    int i;

    engine_default_options(&opt);
    inputs_init(&in);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;

        if (!strcmp(a, "--rom") && next) {
            rom = argv[++i];
        } else if (!strcmp(a, "--level") && next) {
            opt.startLevel = atoi(argv[++i]);
        } else if (!strcmp(a, "--input") && next) {
            inputPath = argv[++i];
        } else if (!strcmp(a, "--events") && next) {
            eventsPath = argv[++i];
        } else if (!strcmp(a, "--frames") && next) {
            frames = atol(argv[++i]);
        } else if (!strcmp(a, "--watch") && next) {
            watchText = argv[++i];
        } else if (!strcmp(a, "--trace")) {
            trace = 1;
        } else if (!strcmp(a, "--trim-draw")) {
            trimDraw = 1;
        } else if (!strcmp(a, "--raw")) {
            raw = 1;
        } else if (!strcmp(a, "--verbose")) {
            opt.verbose = 1;
        } else {
            die("usage: play --rom ROM [--level N] [--input FILE | --events FILE] [--frames N] [--watch LIST] [--trace] "
                "[--raw] [--trim-draw]");
        }
    }
    if (rom == NULL) {
        die("play: give the ROM with --rom");
    }
    if (inputPath != NULL && inputs_read(&in, inputPath, err, sizeof(err)) != 0) {
        die("play: %s", err);
    }
    if (in.first != 0) {
        die("play: %s starts at frame %ld; play needs a movie from its start", inputPath, in.first);
    }
    nwatch = watch_parse_list(watch, MAX_WATCH, watchText, err, sizeof(err));
    if (nwatch < 0) {
        die("play: %s", err);
    }
    if (frames < 0) {
        frames = in.count;
    }
    if (eventsPath != NULL && run_eeprom_fill(eventsPath) >= 0) {
        opt.eepromFill = run_eeprom_fill(eventsPath);
    }
    e = engine_create(rom, &opt, err, sizeof(err));
    if (e == NULL) {
        die("play: %s", err);
    }
    engine_trim_draw(e, trimDraw);
    if (eventsPath != NULL) {
        if (opt.startLevel >= 0) {
            die("play: --events works from power-on only");
        }
        if (run_events(e, eventsPath, watch, nwatch, trace, err, sizeof(err)) != 0) {
            die("play: %s", err);
        }
        return 0;
    }
    print_state(e, 0, watch, nwatch, trace);
    for (k = 0; k < frames; k++) {
        EngineInput none = {0, 0, 0};

        engine_step(e, k < in.count ? &in.frames[k] : &none);
        print_state(e, k + 1, watch, nwatch, trace);
    }
    return 0;
}

typedef struct PrintCtx {
    const Watch *watch;
    int nwatch;
    int trace;
} PrintCtx;

static int print_read(Engine *e, long read, void *ctx) {
    PrintCtx *c = ctx;

    print_state(e, read, c->watch, c->nwatch, c->trace);
    return 0;
}

static int run_events(Engine *e, const char *path, const Watch *watch, int nwatch, int trace, char *err,
                      size_t errSize) {
    Run r;
    PrintCtx c;
    long missed;

    if (run_load(&r, path, err, errSize) != 0 || run_start(e, &r, err, errSize) != 0) {
        return -1;
    }
    c.watch = watch;
    c.nwatch = nwatch;
    c.trace = trace;
    missed = run_exec_cb(e, &r, 0, r.count, print_read, &c);
    print_state(e, r.reads, watch, nwatch, trace);
    if (missed) {
        fprintf(stderr, "play: %ld groups did not reach their stop and ran after their logic frame\n", missed);
    }
    run_free(&r);
    return 0;
}
