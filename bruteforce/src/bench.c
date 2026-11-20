/* The benchmark: how fast the engine runs on this computer.

   glover bench --rom ROM [--level N] [--seconds S] [--workers LIST] [--candidate F]

   First one process: game frames a second with the full and with the trimmed draw (engine_trim_draw), and the
   time of a save and a load. Then groups of worker processes that run at the same time (LIST, for example 1,8,16;
   the default is 1, the number of cores and the number of logical processors). Each worker plays generated input
   in the level with the trimmed draw, as the search does, in two ways for S seconds each:
   - steady: 600 game frames from a state, then the state again;
   - candidates: load a state, play F game frames (default 20), as a search does. A candidate is one try of the
     search.
   The program prints a table. Send it whole.

   The line "Check" holds a hash of the state trace (engine_trace) of the first 600 frames of the test. Every build
   must print the same one, on Linux and on Windows: a different value means that a build computes another game.
   The trimmed draw must give the same hash; the line says so, or prints both. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "procs.h"
#include "tools.h"
#include "util.h"

#ifndef BUILD_OPT
#define BUILD_OPT "base"
#endif

#define SETTLE_FRAMES 60
#define STEADY_FRAMES 600

static Engine *e;
static uint8_t *startState;
static EngineInput steadyInput[STEADY_FRAMES];

/* Starts the engine in the level and saves the state after a few frames without input. */
static void setup(const char *rom, int level) {
    EngineOptions opt;
    EngineInput none = {0, 0, 0};
    TestInput gen;
    char err[256];
    int i;

    engine_default_options(&opt);
    opt.startLevel = level;
    e = engine_create(rom, &opt, err, sizeof(err));
    if (e == NULL) {
        die("bench: %s", err);
    }
    for (i = 0; i < SETTLE_FRAMES; i++) {
        engine_step(e, &none);
    }
    startState = malloc(engine_state_size(e));
    engine_save(e, startState);
    test_input_init(&gen, 12345);
    for (i = 0; i < STEADY_FRAMES; i++) {
        test_input_next(&gen, &steadyInput[i]);
    }
}

/* Plays STEADY_FRAMES frames from the start state again and again until the time `end`. Returns the frames. */
static long run_steady(double end) {
    long frames = 0;

    while (now_seconds() < end) {
        int i;

        engine_load(e, startState);
        for (i = 0; i < STEADY_FRAMES; i++) {
            engine_step(e, &steadyInput[i]);
        }
        frames += STEADY_FRAMES;
    }
    return frames;
}

/* Loads the start state and plays `len` frames of generated input, again and again until `end`. Each candidate
   starts at another place of the input. Returns the candidates. */
static long run_candidates(double end, int len) {
    long count = 0;

    while (now_seconds() < end) {
        int from = (int)((count * 37) % (STEADY_FRAMES - len));
        int i;

        engine_load(e, startState);
        for (i = 0; i < len; i++) {
            engine_step(e, &steadyInput[from + i]);
        }
        count++;
    }
    return count;
}

/* A worker: the two tests at the given times. It prints its frames and candidates a second, each over the time it
   measured: the last block of a test may end after the planned time. */
static int worker(const char *rom, int level, double start, double seconds, int len) {
    long frames;
    long candidates;
    double t0;
    double t1;
    double t2;

    setup(rom, level);
    engine_trim_draw(e, 1);
    sleep_until(start);
    t0 = now_seconds();
    frames = run_steady(start + seconds);
    t1 = now_seconds();
    sleep_until(start + seconds + 0.2);
    t2 = now_seconds();
    candidates = run_candidates(start + 2 * seconds + 0.2, len);
    printf("steady %.3f candidates %.3f\n", frames / (t1 - t0), candidates / (now_seconds() - t2));
    return 0;
}

/* Times op over about `seconds` seconds, with prep before each call (not timed). Returns the milliseconds a call. */
static double op_ms(void (*prep)(void), void (*op)(void), double seconds) {
    double t0 = now_seconds();
    double spent = 0.0;
    long n = 0;

    do {
        double t;

        prep();
        t = now_seconds();
        op();
        spent += now_seconds() - t;
        n++;
    } while (now_seconds() - t0 < seconds);
    return 1000.0 * spent / n;
}

static uint8_t *saveBuf;
static uint8_t *otherState;
static int step;
static int flip;

static void nothing(void) {
}

/* A buffer that holds no state: the save copies everything. */
static void clear_buffer(void) {
    memset(saveBuf, 0, ENGINE_STATE_HEADER);
}

static void one_frame(void) {
    engine_step(e, &steadyInput[step]);
    step = (step + 1) % STEADY_FRAMES;
}

static void load_start_and_frame(void) {
    engine_load(e, startState);
    one_frame();
}

static void do_save(void) {
    engine_save(e, saveBuf);
}

static void do_load_start(void) {
    engine_load(e, startState);
}

static void do_load_alternate(void) {
    engine_load(e, (flip ^= 1) ? otherState : startState);
}

int bench_main(int argc, char **argv) {
    const char *rom = NULL;
    int level = 13;
    double seconds = 10.0;
    int len = 20;
    int counts[16];
    int ncounts = 0;
    int isWorker = 0;
    double start = 0.0;
    char cpu[64];
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;

        if (!strcmp(a, "--rom") && next) {
            rom = argv[++i];
        } else if (!strcmp(a, "--level") && next) {
            level = atoi(argv[++i]);
        } else if (!strcmp(a, "--seconds") && next) {
            seconds = atof(argv[++i]);
        } else if (!strcmp(a, "--candidate") && next) {
            len = atoi(argv[++i]);
        } else if (!strcmp(a, "--workers") && next) {
            char *s = argv[++i];

            while (*s && ncounts < 16) {
                counts[ncounts++] = (int)strtol(s, &s, 10);
                if (*s == ',') {
                    s++;
                }
            }
        } else if (!strcmp(a, "--worker") && next) {
            isWorker = 1;
            start = atof(argv[++i]);
        } else {
            die("usage: glover bench --rom ROM [--level N] [--seconds S] [--workers 1,8,16] [--candidate FRAMES]");
        }
    }
    if (rom == NULL) {
        die("bench: give the ROM with --rom");
    }
    if (len < 1 || len > STEADY_FRAMES / 2) {
        die("bench: --candidate must be 1 to %d", STEADY_FRAMES / 2);
    }
    if (isWorker) {
        return worker(rom, level, start, seconds, len);
    }

    cpu_name(cpu, sizeof(cpu));
    if (ncounts == 0) {
        int phys = cpu_physical();
        int logical = cpu_logical();

        counts[ncounts++] = 1;
        if (phys > 1 && phys < logical) {
            counts[ncounts++] = phys;
        }
        if (logical > 1) {
            counts[ncounts++] = logical;
        }
    }
    printf("Glover brute forcer benchmark\n");
    printf("CPU: %s, %d cores, %d logical processors\n", cpu, cpu_physical(), cpu_logical());
    printf("Build: %s, %s\n", BUILD_OPT,
#ifdef _WIN32
           "Windows"
#else
           "Linux"
#endif
    );
    printf("Level %d, %.0f seconds a test\n", level, seconds);
    fflush(stdout);

    /* One process. */
    setup(rom, level);
    {
        uint64_t h[2];
        int trim;

        for (trim = 0; trim < 2; trim++) {
            int k;

            engine_load(e, startState);
            engine_trim_draw(e, trim);
            h[trim] = 1469598103934665603ull;
            for (k = 0; k < STEADY_FRAMES; k++) {
                char line[256];
                const char *c;

                engine_step(e, &steadyInput[k]);
                engine_trace(e, line, sizeof(line));
                for (c = line; *c; c++) {
                    h[trim] = (h[trim] ^ (uint8_t)*c) * 1099511628211ull;
                }
            }
        }
        engine_trim_draw(e, 0);
        if (h[0] == h[1]) {
            printf("Check: %016llx (the same with the trimmed draw)\n\n", (unsigned long long)h[0]);
        } else {
            printf("Check: %016llx; with the trimmed draw %016llx: DIFFERENT\n\n", (unsigned long long)h[0],
                   (unsigned long long)h[1]);
        }
        fflush(stdout);
    }
    {
        EngineStateInfo info;
        double t0 = now_seconds();
        long frames = run_steady(t0 + seconds / 2);
        double fps = frames / (now_seconds() - t0);
        double fpsTrim;

        engine_trim_draw(e, 1);
        t0 = now_seconds();
        frames = run_steady(t0 + seconds / 2);
        fpsTrim = frames / (now_seconds() - t0);
        engine_trim_draw(e, 0);
        double saveFull;
        double saveAfter;
        double loadAfter;
        double loadOther;

        saveBuf = malloc(engine_state_size(e));
        otherState = malloc(engine_state_size(e));
        engine_load(e, startState);
        for (i = 0; i < 300; i++) {
            engine_step(e, &steadyInput[i]);
        }
        engine_save(e, otherState);
        engine_state_info(e, &info);
        saveFull = op_ms(clear_buffer, do_save, 1.0);
        engine_load(e, startState);
        saveAfter = op_ms(one_frame, do_save, 1.0);
        loadAfter = op_ms(load_start_and_frame, do_load_start, 1.0);
        loadOther = op_ms(nothing, do_load_alternate, 1.0);
        printf("One process\n");
        printf("  game frames a second:  %.0f (%.3f ms a frame); %.0f (%.3f ms) with the trimmed draw\n", fps,
               1000.0 / fps, fpsTrim, 1000.0 / fpsTrim);
        printf("  state size:            %.2f MB (game variables %.2f, memory below the heap %.2f, heap %.2f)\n",
               info.total / 1048576.0, info.gameVars / 1048576.0, info.lowRam / 1048576.0,
               (info.heapLow + info.heapHigh) / 1048576.0);
        printf("  a save:                %.3f ms into an empty buffer; %.3f ms after a game frame, into the same "
               "buffer\n", saveFull, saveAfter);
        printf("  a load:                %.3f ms after a game frame; %.3f ms two states 300 frames apart in turn\n\n",
               loadAfter, loadOther);
        fflush(stdout);
    }

    printf("Workers (separate processes, all at once, with the trimmed draw)\n");
    printf("  workers  frames/s all  frames/s each  candidates/s all  (a candidate: a load and %d frames)\n", len);
    fflush(stdout);
    for (i = 0; i < ncounts; i++) {
        int n = counts[i];
        FILE **out = calloc(n, sizeof(FILE *));
        double t = now_seconds() + 2.0 + n * 0.05;
        double frames = 0;
        double candidates = 0;
        int ok = 0;
        int k;

        for (k = 0; k < n; k++) {
            char cmd[8192] = "";
            char num[64];

            quote_arg(cmd, sizeof(cmd), self_path());
            quote_arg(cmd, sizeof(cmd), "bench");
            quote_arg(cmd, sizeof(cmd), "--rom");
            quote_arg(cmd, sizeof(cmd), rom);
            snprintf(num, sizeof(num), "%d", level);
            quote_arg(cmd, sizeof(cmd), "--level");
            quote_arg(cmd, sizeof(cmd), num);
            snprintf(num, sizeof(num), "%g", seconds);
            quote_arg(cmd, sizeof(cmd), "--seconds");
            quote_arg(cmd, sizeof(cmd), num);
            snprintf(num, sizeof(num), "%d", len);
            quote_arg(cmd, sizeof(cmd), "--candidate");
            quote_arg(cmd, sizeof(cmd), num);
            snprintf(num, sizeof(num), "%.6f", t);
            quote_arg(cmd, sizeof(cmd), "--worker");
            quote_arg(cmd, sizeof(cmd), num);
            out[k] = proc_start(cmd);
        }
        for (k = 0; k < n; k++) {
            double f = 0;
            double c = 0;

            if (out[k] != NULL) {
                if (fscanf(out[k], "steady %lf candidates %lf", &f, &c) == 2) {
                    frames += f;
                    candidates += c;
                    ok++;
                }
                proc_wait(out[k]);
            }
        }
        free(out);
        if (ok != n) {
            printf("  %7d  only %d of the workers reported\n", n, ok);
        } else {
            printf("  %7d  %12.0f  %13.0f  %16.0f\n", n, frames, frames / n, candidates);
        }
        fflush(stdout);
    }
    return 0;
}
