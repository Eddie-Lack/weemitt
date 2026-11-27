/* The search: changes the inputs of a window of reads and keeps the changes that reach the goal sooner.

   glover search CONFIG [--rom ROM] [--workers N] [--seconds S]

   CONFIG is a JSON file (README.md, "The search"):
       "run"        the events script of the base movie (tools/convert.py to-run)
       "window"     [first, end): the reads whose inputs may change
       "lookahead"  reads after the window, played with the base inputs (default 100)
       "goal"       a condition (cond.h); the score is the read at which it first holds, the sooner the better
       "points"     instead of a goal: places and conditions to reach in order (the studio's mid-points and end
                    point). [{"place": [x, y, z], "radius": R, "of": "glove" or "ball"}, {"condition": "..."}]. The
                    score is the read at which the last one is reached; until then, the points reached and the
                    distance to the next place count as progress
       "fail"       a condition that ends a try as a failure (optional)
       "progress"   [{"value": "NAME:TYPE", "weight": W}, ...]: when the goal is not reached, the larger the
                    weighted sum at the end, the better (optional)
       "buttons"    the buttons that may change (default all but Start), "stick" (default true)
       "limits"     what a runner can play: "none" (the default), "loose", "strict", or {"minHold": H,
                    "minRelease": R, "stickStep": S}: a press of a button that may change lasts at least H frames,
                    the gap between two presses of one button at least R, and the stick's X and Y each change by at
                    most S from one frame to the next (0: no limit). A try may break them no more often than the
                    current inputs do
       "perturb"    the weights of the kinds of change (below)
       "out"        the output folder (default search-out)
       "workers", "seconds", "seed", "cache" (a saved state every this many reads, default 1)
       "trimDraw"   false: the full draw instead of the trimmed one (engine_trim_draw; default true)
       "loadFrames" what a load costs, in game frames (default 70: 3.5 seconds on a console, docs/game.md). The
                    engine runs a load in one game frame. A play that passes a load (frameCount goes back: the
                    level is left) counts this many frames for each

   The coordinator starts the workers (copies of this program with --worker) and prints the progress. Each worker
   plays the base run to the window's start, then tries changed inputs and keeps a try that scores better than its
   current inputs (hill climbing; a try that scores the same is kept now and then, to move along flat stretches).
   It writes each new best to OUT/worker-N.txt. The coordinator collects them into OUT/best.txt, the engine's text
   format with read numbers (tools/convert.py to-movie turns it into a movie), and the workers take up a better
   best.txt when they see one. OUT/log.txt has the score over time. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cond.h"
#include "engine.h"
#include "inputs.h"
#include "json.h"
#include "procs.h"
#include "run.h"
#include "tools.h"
#include "util.h"
#include "watch.h"

#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir((p), 0777)
#endif

#define MAX_PROGRESS 8
#define MAX_POINTS 16
#define NOT_REACHED 0x7FFFFFFFL

enum { P_STICK, P_STICK_NEW, P_BUTTON, P_SHIFT, P_COPY, P_REVERT, P_KINDS };
static const char *perturbNames[P_KINDS] = {"stick", "stick-new", "button", "shift", "copy", "revert"};

typedef struct Point {
    int place;        /* 1: a place to reach; 0: a condition */
    float at[3];
    float radius;
    uint32_t pos;     /* the N64 address of the position (Vec3) of the actor that must reach the place */
    Cond cond;
} Point;

typedef struct Config {
    const char *rom;
    const char *runPath;
    const char *out;
    long first, end; /* the window: reads [first, end) */
    long lookahead;
    Cond goal;
    Cond fail;
    Point points[MAX_POINTS];
    int npoints;
    Watch progress[MAX_PROGRESS];
    double weight[MAX_PROGRESS];
    int nprogress;
    uint16_t buttons; /* the buttons that may change */
    int stick;
    double perturb[P_KINDS];
    int minHold, minRelease, stickStep; /* "limits" */
    int workers;
    double seconds;
    uint32_t seed;
    int cacheStride;
    int trimDraw;
    long playEnd; /* the plays end before this read (window_check) */
} Config;

typedef struct Score {
    long frame;      /* the read at which the goal first held, or NOT_REACHED */
    double progress; /* when the goal was not reached */
    int loads;       /* the loads the play passed before that read */
} Score;

/* The game frames a load costs ("loadFrames"). */
static long loadFrames = 70;
/* frameCount's N64 address: it goes back to 0 when a level is freed, at a load. */
static uint32_t frameCountAt;

/* Counts a load when frameCount went back since the last read. One load sets it back twice: when the old level is
   freed, and about 15 frames later when the new one starts. The second goes back from a small count, which is not
   counted. */
static void count_loads(Engine *e, long *last, int *loads) {
    long now = (int32_t)engine_read32(e, frameCountAt);

    if (now < *last && *last >= 30) {
        (*loads)++;
    }
    *last = now;
}

/* The time to the goal in game frames, with the loads. */
static long cost(const Score *s) {
    return s->frame == NOT_REACHED ? NOT_REACHED : s->frame + s->loads * loadFrames;
}

/* 1 when a is better than b. */
static int better(const Score *a, const Score *b) {
    if (cost(a) != cost(b)) {
        return cost(a) < cost(b);
    }
    return a->frame == NOT_REACHED && a->progress > b->progress;
}

static int same(const Score *a, const Score *b) {
    return cost(a) == cost(b) && (a->frame != NOT_REACHED || a->progress == b->progress);
}

static void format_score(const Score *s, char *buf, size_t size) {
    if (s->frame != NOT_REACHED && s->loads > 0) {
        snprintf(buf, size, "goal at read %ld after %d loads (%ld game frames with the loads)", s->frame, s->loads,
                 cost(s));
    } else if (s->frame != NOT_REACHED) {
        snprintf(buf, size, "goal at read %ld", s->frame);
    } else {
        snprintf(buf, size, "goal not reached, progress %.4f", s->progress);
    }
}

/* ---- The config. */

static const struct {
    const char *name;
    uint16_t bit;
} buttonNames[] = {{"A", BTN_A},   {"B", BTN_B},   {"Z", BTN_Z},   {"S", BTN_START}, {"L", BTN_L},   {"R", BTN_R},
                   {"du", BTN_DU}, {"dd", BTN_DD}, {"dl", BTN_DL}, {"dr", BTN_DR},   {"cu", BTN_CU}, {"cd", BTN_CD},
                   {"cl", BTN_CL}, {"cr", BTN_CR}};

/* A path in the config, relative to the config's folder. */
static const char *beside(const char *config, const char *path) {
    const char *slash = strrchr(config, '/');
    const char *back = strrchr(config, '\\');
    size_t dir;
    char *out;

    if (back != NULL && (slash == NULL || back > slash)) {
        slash = back;
    }
    if (path == NULL || slash == NULL || path[0] == '/' || path[0] == '\\' || (path[0] && path[1] == ':')) {
        return path;
    }
    dir = (size_t)(slash - config) + 1;
    out = malloc(dir + strlen(path) + 1);
    memcpy(out, config, dir);
    strcpy(out + dir, path);
    return out;
}

static void load_config(Config *c, const char *path) {
    size_t size;
    char *text = (char *)read_file(path, &size);
    char err[512];
    Json *j;
    const Json *v;
    int i;

    if (text == NULL) {
        die("search: cannot read %s", path);
    }
    text = realloc(text, size + 1);
    text[size] = 0;
    j = json_parse(text, err, sizeof(err));
    if (j == NULL) {
        die("search: %s: %s", path, err);
    }
    if (j->type != JSON_OBJECT) {
        die("search: %s: the config is a JSON object", path);
    }
    {
        /* A key that the search does not know is a mistake (a typo would leave its default in place). */
        static const char *const known[] = {
            "rom", "run", "out", "window", "lookahead", "goal", "fail", "progress", "points", "buttons", "stick",
            "perturb", "limits", "workers", "seconds", "seed", "cache", "trimDraw", "loadFrames", NULL,
        };

        for (i = 0; i < j->count; i++) {
            int k;

            for (k = 0; known[k] != NULL && strcmp(known[k], j->keys[i]) != 0; k++) {
            }
            if (known[k] == NULL) {
                die("search: %s: unknown key \"%s\" in the config", path, j->keys[i]);
            }
        }
    }
    memset(c, 0, sizeof(*c));
    c->rom = beside(path, json_string(j, "rom", NULL));
    c->runPath = beside(path, json_string(j, "run", NULL));
    c->out = beside(path, json_string(j, "out", "search-out"));
    v = json_get(j, "window");
    if (c->runPath == NULL || v == NULL || v->type != JSON_ARRAY || v->count != 2) {
        die("search: the config needs \"run\" and \"window\": [first, end]");
    }
    c->first = (long)v->items[0].number;
    c->end = (long)v->items[1].number;
    c->lookahead = (long)json_number(j, "lookahead", 100);
    if (cond_parse(&c->goal, json_string(j, "goal", ""), err, sizeof(err)) != 0) {
        die("search: goal: %s", err);
    }
    if (cond_parse(&c->fail, json_string(j, "fail", ""), err, sizeof(err)) != 0) {
        die("search: fail: %s", err);
    }
    v = json_get(j, "progress");
    for (i = 0; v != NULL && v->type == JSON_ARRAY && i < v->count && i < MAX_PROGRESS; i++) {
        const char *spec = json_string(&v->items[i], "value", "");

        if (watch_parse(&c->progress[i], spec, err, sizeof(err)) != 0) {
            die("search: progress: %s", err);
        }
        c->weight[i] = json_number(&v->items[i], "weight", 1.0);
        c->nprogress++;
    }
    v = json_get(j, "points");
    for (i = 0; v != NULL && v->type == JSON_ARRAY && i < v->count; i++) {
        const Json *item = &v->items[i];
        const Json *at = json_get(item, "place");
        Point *pt;

        if (c->npoints == MAX_POINTS) {
            die("search: at most %d points", MAX_POINTS);
        }
        pt = &c->points[c->npoints++];
        if (at != NULL) {
            const char *of = json_string(item, "of", "glove");

            if (at->type != JSON_ARRAY || at->count != 3) {
                die("search: a place is [x, y, z]");
            }
            pt->place = 1;
            pt->at[0] = (float)at->items[0].number;
            pt->at[1] = (float)at->items[1].number;
            pt->at[2] = (float)at->items[2].number;
            pt->radius = (float)json_number(item, "radius", 40.0);
            /* The actor's position is its Vec3 at 0x34 (gloveworld's Actor). */
            pt->pos = engine_symbol(!strcmp(of, "ball") ? "ball" : "hand") + 0x34;
        } else if (cond_parse(&pt->cond, json_string(item, "condition", ""), err, sizeof(err)) != 0 ||
                   pt->cond.count == 0) {
            die("search: a point needs a \"place\" or a \"condition\"");
        }
    }
    if (c->goal.count == 0 && c->nprogress == 0 && c->npoints == 0) {
        die("search: the config needs a \"goal\", \"points\" or a \"progress\"");
    }
    v = json_get(j, "buttons");
    if (v != NULL && v->type == JSON_ARRAY) {
        for (i = 0; i < v->count; i++) {
            unsigned k;

            for (k = 0; k < sizeof(buttonNames) / sizeof(buttonNames[0]); k++) {
                if (v->items[i].type == JSON_STRING && !strcmp(v->items[i].string, buttonNames[k].name)) {
                    c->buttons |= buttonNames[k].bit;
                    break;
                }
            }
            if (k == sizeof(buttonNames) / sizeof(buttonNames[0])) {
                die("search: unknown button in \"buttons\"");
            }
        }
    } else {
        c->buttons = (uint16_t)(0xFFFF & ~BTN_START & ~0x00C0);
    }
    c->stick = (int)json_number(j, "stick", 1);
    v = json_get(j, "perturb");
    for (i = 0; i < P_KINDS; i++) {
        static const double defaults[P_KINDS] = {4, 1, 3, 3, 1, 1};

        c->perturb[i] = json_number(v, perturbNames[i], defaults[i]);
    }
    if (!c->stick) {
        c->perturb[P_STICK] = c->perturb[P_STICK_NEW] = 0;
    }
    if (c->buttons == 0) {
        c->perturb[P_BUTTON] = c->perturb[P_SHIFT] = 0;
    }
    if (c->perturb[P_STICK] + c->perturb[P_STICK_NEW] + c->perturb[P_BUTTON] + c->perturb[P_SHIFT] +
            c->perturb[P_COPY] <= 0) {
        die("search: nothing may change: no buttons, no stick or no weights in \"perturb\"");
    }
    v = json_get(j, "limits");
    if (v != NULL && v->type == JSON_STRING) {
        if (!strcmp(v->string, "loose")) {
            c->minHold = 2;
            c->minRelease = 2;
            c->stickStep = 96;
        } else if (!strcmp(v->string, "strict")) {
            c->minHold = 3;
            c->minRelease = 3;
            c->stickStep = 48;
        } else if (strcmp(v->string, "none") != 0) {
            die("search: \"limits\" is \"none\", \"loose\", \"strict\" or an object");
        }
    } else if (v != NULL) {
        c->minHold = (int)json_number(v, "minHold", 0);
        c->minRelease = (int)json_number(v, "minRelease", 0);
        c->stickStep = (int)json_number(v, "stickStep", 0);
    }
    c->workers = (int)json_number(j, "workers", 0);
    c->seconds = json_number(j, "seconds", 0);
    c->seed = (uint32_t)json_number(j, "seed", 1);
    c->cacheStride = (int)json_number(j, "cache", 1);
    c->trimDraw = (int)json_number(j, "trimDraw", 1);
    loadFrames = (long)json_number(j, "loadFrames", 70);
    frameCountAt = engine_symbol("frameCount");
    if (c->cacheStride < 1) {
        c->cacheStride = 1;
    }
    /* The config's strings stay in the parsed tree, which lives as long as the program. */
}

/* ---- A worker. */

typedef struct Worker {
    Config *cfg;
    Engine *e;
    Run run;
    long first, end, playEnd; /* window [first, end); the plays go on to playEnd */
    long n;                   /* reads played: playEnd - first */
    EngineInput *base;        /* the base inputs of the reads [first, playEnd) */
    EngineInput *cur;         /* the current inputs */
    EngineInput *cand;
    Score curScore;
    uint8_t **states;         /* states[i]: before read first + i * stride (window only) */
    size_t *capacity;         /* the size of states[i]'s buffer */
    unsigned char *valid;     /* valid[i]: states[i] holds a state (its read is not in the middle of a frame) */
    int *slotNext;            /* slotNext[i]: the points reached by states[i]'s read */
    int *slotLoads;           /* slotLoads[i]: the loads passed by then */
    long *slotCount;          /* slotCount[i]: frameCount then */
    long endEvent;            /* the event where the plays end: read playEnd's */
    long nstates;
    uint32_t rng;
    long tries;
    long kept;
    long kinds[P_KINDS];
    long kindsBetter[P_KINDS];
} Worker;

static uint32_t rnd(Worker *w) {
    uint32_t x = w->rng;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return w->rng = x;
}

static double rnd01(Worker *w) {
    return (rnd(w) >> 8) / 16777216.0;
}

/* The distance from point p's place to its actor. */
static double point_distance(const Worker *w, const Point *p) {
    double dx = engine_readf(w->e, p->pos) - p->at[0];
    double dy = engine_readf(w->e, p->pos + 4) - p->at[1];
    double dz = engine_readf(w->e, p->pos + 8) - p->at[2];

    return sqrt(dx * dx + dy * dy + dz * dz);
}

/* The points reached in order, from next on, now. */
static int points_reached(const Worker *w, int next) {
    const Config *c = w->cfg;

    while (next < c->npoints) {
        const Point *p = &c->points[next];

        if (p->place ? point_distance(w, p) > p->radius : !cond_eval(&p->cond, w->e)) {
            break;
        }
        next++;
    }
    return next;
}

/* The goal: the condition, or all the points. */
static int goal_holds(const Worker *w, int next) {
    if (w->cfg->npoints > 0) {
        return next == w->cfg->npoints;
    }
    return w->cfg->goal.count && cond_eval(&w->cfg->goal, w->e);
}

static double progress_now(const Worker *w, int next) {
    double sum = 0;
    int i;

    if (next < w->cfg->npoints) {
        /* Each point reached outweighs any distance; nearer to the next place is better. */
        sum += next * 1e6;
        if (w->cfg->points[next].place) {
            sum -= point_distance(w, &w->cfg->points[next]);
        }
    }
    for (i = 0; i < w->cfg->nprogress; i++) {
        sum += w->cfg->weight[i] * watch_value(&w->cfg->progress[i], watch_read(w->e, &w->cfg->progress[i]));
    }
    return sum;
}

/* Puts the inputs in[k] for k >= from into the run's reads first + k. The worker plays its run in replay mode
   (run.h): the run's own events with these inputs, without the emulator's writes. */
static void set_inputs(Worker *w, const EngineInput *in, long from) {
    long k;

    for (k = from; k < w->n; k++) {
        w->run.events[w->run.readAt[w->first + k]].input = in[k];
    }
}

/* The cache slot to start from for a change at read first + from: the nearest slot at or before it that holds a
   state. Slots at reads in the middle of a frame hold none. */
static long cache_slot(const Worker *w, long from) {
    long i = from / w->cfg->cacheStride;

    while (i > 0 && !w->valid[i]) {
        i--;
    }
    return i;
}

typedef struct PlayCtx {
    Worker *w;
    const Score *bound;
    long start; /* the read the play starts at */
    int next;   /* the points reached */
    long lastCount; /* frameCount at the read before */
    Score s;
    int done;
} PlayCtx;

/* Before each read where the game thread waits between frames: the state after the frames of the read before. */
static int play_read(Engine *e, long read, void *ctx) {
    PlayCtx *c = ctx;
    Worker *w = c->w;

    (void)e;
    if (read <= c->start) {
        return 0;
    }
    count_loads(w->e, &c->lastCount, &c->s.loads);
    if (w->run.busy[read]) {
        return 0;
    }
    c->next = points_reached(w, c->next);
    if (goal_holds(w, c->next)) {
        c->s.frame = read - 1;
        c->done = 1;
    } else if (w->cfg->fail.count && cond_eval(&w->cfg->fail, w->e)) {
        c->s.progress = -1e30;
        c->done = 1;
    } else if (c->bound != NULL && c->bound->frame != NOT_REACHED && read - 1 + c->s.loads * loadFrames >= cost(c->bound)) {
        /* The goal can no longer come sooner than the bound's. */
        c->done = 1;
    }
    return c->done;
}

/* Plays inputs from read first + from (the engine holds the state before it, and from is a cache slot's read) to
   the end, or until the goal or the fail condition holds, or until the play cannot beat `bound` any more. The
   score's read is the last read before the goal held: the goal is tested where the game thread waits between
   frames. Returns the score. */
static Score play(Worker *w, const EngineInput *in, long from, const Score *bound) {
    PlayCtx c;

    c.w = w;
    c.bound = bound;
    c.start = w->first + from;
    c.s.frame = NOT_REACHED;
    c.s.progress = 0;
    c.done = 0;
    c.next = w->slotNext[from / w->cfg->cacheStride];
    c.s.loads = w->slotLoads[from / w->cfg->cacheStride];
    c.lastCount = w->slotCount[from / w->cfg->cacheStride];
    set_inputs(w, in, from);
    run_exec_cb(w->e, &w->run, w->run.readAt[w->first + from], w->endEvent, play_read, &c);
    if (!c.done) {
        c.next = points_reached(w, c.next);
        if (goal_holds(w, c.next)) {
            c.s.frame = w->playEnd - 1;
        } else if (w->cfg->fail.count && cond_eval(&w->cfg->fail, w->e)) {
            c.s.progress = -1e30;
        } else {
            c.s.progress = progress_now(w, c.next);
        }
    }
    return c.s;
}

/* Saves the engine's state in cache slot i. The save goes straight into the slot's buffer: when that holds an
   earlier state of the same layout, only the changed pages are copied (docs/engine.md). */
static void save_slot(Worker *w, long i) {
    EngineStateInfo info;

    engine_state_info(w->e, &info);
    if (w->capacity[i] < info.total) {
        w->states[i] = realloc(w->states[i], info.total);
        if (w->states[i] == NULL) {
            die("search: out of memory for the saved states; set \"cache\" higher or use a shorter window");
        }
        if (w->capacity[i] == 0) {
            memset(w->states[i], 0, ENGINE_STATE_HEADER);
        }
        w->capacity[i] = info.total;
    }
    engine_save(w->e, w->states[i]);
}

typedef struct CacheCtx {
    Worker *w;
    long start;
    int next; /* the points reached */
    int loads;
    long lastCount;
} CacheCtx;

static int cache_read(Engine *e, long read, void *ctx) {
    CacheCtx *c = ctx;
    Worker *w = c->w;
    long k = read - w->first;

    if (k >= w->end - w->first) {
        return 1;
    }
    if (k > c->start) {
        count_loads(w->e, &c->lastCount, &c->loads);
    }
    if (k > c->start && !w->run.busy[read]) {
        c->next = points_reached(w, c->next);
    }
    if (k > c->start && k % w->cfg->cacheStride == 0) {
        long i = k / w->cfg->cacheStride;

        w->valid[i] = !w->run.busy[read];
        if (w->valid[i]) {
            save_slot(w, i);
            w->slotNext[i] = c->next;
            w->slotLoads[i] = c->loads;
            w->slotCount[i] = c->lastCount;
        }
    }
    return 0;
}

/* Saves the states of the current inputs from read first + from on (window reads only). */
static void build_cache(Worker *w, long from) {
    CacheCtx c;
    long i = cache_slot(w, from);

    c.w = w;
    c.start = i * w->cfg->cacheStride;
    c.next = w->slotNext[i];
    c.loads = w->slotLoads[i];
    c.lastCount = w->slotCount[i];
    engine_load(w->e, w->states[i]);
    set_inputs(w, w->cur, c.start);
    run_exec_cb(w->e, &w->run, w->run.readAt[w->first + c.start], w->endEvent, cache_read, &c);
}

static void clamp_stick(int x, int y, int8_t *ox, int8_t *oy) {
    *ox = (int8_t)(x < -128 ? -128 : x > 127 ? 127 : x);
    *oy = (int8_t)(y < -128 ? -128 : y > 127 ? 127 : y);
}

/* Changes cand (a copy of cur) in one way. Returns the first read changed, and the kind in *kind. */
static long perturb(Worker *w, int *kind) {
    const Config *c = w->cfg;
    long len = w->end - w->first;
    double total = 0;
    double r;
    long i = rnd(w) % len;
    long span = 1 + (rnd(w) % 4 == 0 ? rnd(w) % 6 : 0);
    long k;
    int t;

    for (t = 0; t < P_KINDS; t++) {
        total += c->perturb[t];
    }
    r = rnd01(w) * total;
    for (t = 0; t < P_KINDS - 1 && r >= c->perturb[t]; t++) {
        r -= c->perturb[t];
    }
    *kind = t;
    if (i + span > len) {
        span = len - i;
    }
    switch (t) {
        case P_STICK: {
            /* Turn the stick a little, and change its length a little now and then. */
            double a = atan2(w->cand[i].y, w->cand[i].x);
            double m = sqrt((double)w->cand[i].x * w->cand[i].x + (double)w->cand[i].y * w->cand[i].y);
            double da = (rnd01(w) - 0.5) * (rnd(w) % 3 == 0 ? 0.6 : 0.08);

            if (m < 8) {
                m = 80;
            }
            if (rnd(w) % 4 == 0) {
                m += (rnd01(w) - 0.5) * 20;
            }
            for (k = i; k < i + span; k++) {
                clamp_stick((int)lround(m * cos(a + da)), (int)lround(m * sin(a + da)), &w->cand[k].x,
                            &w->cand[k].y);
            }
            break;
        }
        case P_STICK_NEW: {
            double a = rnd01(w) * 2 * M_PI;
            double m = rnd(w) % 3 == 0 ? 0 : 80;

            for (k = i; k < i + span; k++) {
                clamp_stick((int)lround(m * cos(a)), (int)lround(m * sin(a)), &w->cand[k].x, &w->cand[k].y);
            }
            break;
        }
        case P_BUTTON: {
            /* Toggle a button for a few reads. */
            uint16_t bit;

            do {
                bit = (uint16_t)(1u << (rnd(w) % 16));
            } while (!(c->buttons & bit));
            for (k = i; k < i + span; k++) {
                w->cand[k].buttons ^= bit;
            }
            break;
        }
        case P_SHIFT: {
            /* Move the start or the end of a press by one read: copy a button's state from a neighbour. */
            uint16_t bit;
            long from = rnd(w) & 1 ? i - 1 : i + 1;

            do {
                bit = (uint16_t)(1u << (rnd(w) % 16));
            } while (!(c->buttons & bit));
            if (from < 0 || from >= len) {
                from = i;
            }
            w->cand[i].buttons = (uint16_t)((w->cand[i].buttons & ~bit) | (w->cand[from].buttons & bit));
            break;
        }
        case P_COPY: {
            long from = rnd(w) & 1 ? i - 1 : i + 1;

            /* Only what may change: the allowed buttons, and the stick when it may change. */
            if (from >= 0 && from < len) {
                w->cand[i].buttons = (uint16_t)((w->cand[i].buttons & ~c->buttons) |
                                                (w->cand[from].buttons & c->buttons));
                if (c->stick) {
                    w->cand[i].x = w->cand[from].x;
                    w->cand[i].y = w->cand[from].y;
                }
            }
            break;
        }
        default:
            for (k = i; k < i + span; k++) {
                w->cand[k] = w->base[k];
            }
            break;
    }
    return i;
}

/* How often inputs break the limits: presses and gaps that are too short, stick changes that are too large. Runs
   that touch the first or the last frame continue outside the inputs and do not count. */
static long violations(const Config *c, const EngineInput *in, long n) {
    long count = 0;
    long k;
    int b;

    if (c->minHold > 1 || c->minRelease > 1) {
        for (b = 0; b < 16; b++) {
            uint16_t bit = (uint16_t)(1u << b);
            long runStart = 0;

            if (!(c->buttons & bit)) {
                continue;
            }
            for (k = 1; k <= n; k++) {
                if (k == n || ((in[k].buttons ^ in[k - 1].buttons) & bit)) {
                    long runLength = k - runStart;
                    int pressed = (in[runStart].buttons & bit) != 0;

                    if (runStart > 0 && k < n && runLength < (pressed ? c->minHold : c->minRelease)) {
                        count++;
                    }
                    runStart = k;
                }
            }
        }
    }
    if (c->stickStep > 0 && c->stick) {
        for (k = 1; k < n; k++) {
            count += abs(in[k].x - in[k - 1].x) > c->stickStep || abs(in[k].y - in[k - 1].y) > c->stickStep;
        }
    }
    return count;
}

/* Writes a line of the worker's file: the score and the window's inputs. */
static void report(Worker *w, FILE *f) {
    char buf[64];
    long k;

    fprintf(f, "best %ld %.6f %d", w->curScore.frame, w->curScore.progress, w->curScore.loads);
    for (k = 0; k < w->end - w->first; k++) {
        input_format(&w->cur[k], buf, sizeof(buf));
        fprintf(f, " | %s", buf);
    }
    fprintf(f, "\n");
    fflush(f);
}

/* Reads a best line ("best FRAME PROGRESS LOADS | x y buttons | ...") into in and s. Returns 0 or -1. LOADS may be
   missing (0). */
static int parse_best(const char *line, long len, EngineInput *in, Score *s) {
    const char *p;
    long k = 0;
    char err[128];

    s->loads = 0;
    if (sscanf(line, "best %ld %lf %d", &s->frame, &s->progress, &s->loads) < 2) {
        return -1;
    }
    for (p = strchr(line, '|'); p != NULL && k < len; p = strchr(p + 1, '|'), k++) {
        char item[128];
        const char *end = strchr(p + 1, '|');
        size_t n = end ? (size_t)(end - p - 1) : strlen(p + 1);

        if (n >= sizeof(item)) {
            return -1;
        }
        memcpy(item, p + 1, n);
        item[n] = 0;
        if (input_parse(item, &in[k], err, sizeof(err)) != 0) {
            return -1;
        }
    }
    return k == len ? 0 : -1;
}

/* Takes up the shared best when it is better than the worker's current inputs. */
static void adopt_shared(Worker *w, const char *bestPath) {
    FILE *f = fopen(bestPath, "r");
    char *line;
    size_t cap = 1 << 20;
    Score s;

    if (f == NULL) {
        return;
    }
    line = malloc(cap);
    while (fgets(line, (int)cap, f)) {
        if (!strncmp(line, "# best ", 7) && parse_best(line + 2, w->end - w->first, w->cand, &s) == 0 &&
            better(&s, &w->curScore)) {
            long k;
            long changed = -1;

            for (k = 0; k < w->end - w->first; k++) {
                if (changed < 0 && memcmp(&w->cand[k], &w->cur[k], sizeof(EngineInput)) != 0) {
                    changed = k;
                }
            }
            memcpy(w->cur, w->cand, (w->end - w->first) * sizeof(EngineInput));
            w->curScore = s;
            build_cache(w, changed < 0 ? 0 : changed);
        }
    }
    free(line);
    fclose(f);
}

/* Checks the window against the run and sets c->playEnd: the plays go on after the window up to a load, and end at
   a read outside a group. */
static void window_check(Config *c, const Run *r) {
    long k;

    if (c->first < 0 || c->end > r->reads || c->first >= c->end) {
        die("search: the window must lie inside the run's %ld reads", r->reads);
    }
    if (!run_playable(r, c->first)) {
        die("search: the window must start at a read where the game waits between frames, and not in a load");
    }
    for (k = c->first; k < c->end; k++) {
        if (!r->grouped[k] && !run_playable(r, k)) {
            die("search: the window holds a load at read %ld (docs/sync.md)", k);
        }
    }
    for (k = c->end; k < r->reads && k < c->end + c->lookahead; k++) {
        if (!r->grouped[k] && !run_playable(r, k)) {
            break;
        }
    }
    while (k < r->reads && r->grouped[k]) {
        k++;
    }
    c->playEnd = k;
}

static char donePath[1024];

/* Leaves OUT/worker-N.done when the worker ends, through die too. The coordinator ends when all have ended. */
static void write_done(void) {
    FILE *f = fopen(donePath, "w");

    if (f != NULL) {
        fclose(f);
    }
}

static int worker_main(Config *c, int index) {
    Worker w;
    char err[512];
    char path[1024];
    char bestPath[1024];
    FILE *out;
    EngineOptions opt;
    long k;
    double start = now_seconds();
    double lastCheck = start;
    double lastStatus = start;
    int limited = c->minHold > 1 || c->minRelease > 1 || c->stickStep > 0;

    snprintf(donePath, sizeof(donePath), "%s/worker-%d.done", c->out, index);
    atexit(write_done);
    memset(&w, 0, sizeof(w));
    w.cfg = c;
    w.rng = c->seed * 2654435761u + (uint32_t)index * 40503u + 1;
    if (run_load(&w.run, c->runPath, err, sizeof(err)) != 0) {
        die("search: %s", err);
    }
    w.first = c->first;
    w.end = c->end;
    window_check(c, &w.run);
    w.playEnd = c->playEnd;
    w.n = w.playEnd - w.first;
    w.endEvent = w.playEnd < w.run.reads ? w.run.readAt[w.playEnd] : w.run.count;
    engine_default_options(&opt);
    if (w.run.eepromFill >= 0) {
        opt.eepromFill = w.run.eepromFill;
    }
    w.e = engine_create(c->rom, &opt, err, sizeof(err));
    if (w.e == NULL) {
        die("search: %s", err);
    }
    engine_trim_draw(w.e, c->trimDraw);
    w.base = malloc(w.n * sizeof(EngineInput));
    w.cur = malloc(w.n * sizeof(EngineInput));
    w.cand = malloc(w.n * sizeof(EngineInput));
    for (k = 0; k < w.n; k++) {
        w.base[k] = w.run.events[w.run.readAt[w.first + k]].input;
    }
    memcpy(w.cur, w.base, w.n * sizeof(EngineInput));
    /* The run up to the window. */
    if (run_start(w.e, &w.run, err, sizeof(err)) != 0) {
        die("search: %s", err);
    }
    run_exec(w.e, &w.run, 0, w.run.readAt[w.first]);
    w.nstates = (w.end - w.first + c->cacheStride - 1) / c->cacheStride;
    w.states = calloc(w.nstates, sizeof(uint8_t *));
    w.valid = calloc(w.nstates, 1);
    w.capacity = calloc(w.nstates, sizeof(size_t));
    w.slotNext = calloc(w.nstates, sizeof(int));
    w.slotLoads = calloc(w.nstates, sizeof(int));
    w.slotCount = calloc(w.nstates, sizeof(long));
    if (w.states == NULL || w.valid == NULL || w.capacity == NULL || w.slotNext == NULL || w.slotLoads == NULL ||
        w.slotCount == NULL) {
        die("search: out of memory");
    }
    save_slot(&w, 0);
    w.valid[0] = 1;
    w.slotNext[0] = points_reached(&w, 0);
    w.slotCount[0] = (int32_t)engine_read32(w.e, frameCountAt);
    if (index == 0) {
        /* The base inputs played without the emulator's writes must give what the run's own events give. */
        uint64_t a;
        uint64_t b;

        run_exec(w.e, &w.run, w.run.readAt[w.first], w.endEvent);
        a = engine_check_hash(w.e);
        engine_load(w.e, w.states[0]);
        w.run.replay = 1;
        run_exec(w.e, &w.run, w.run.readAt[w.first], w.endEvent);
        b = engine_check_hash(w.e);
        if (a != b) {
            fprintf(stderr, "search: warning: the base inputs played without the emulator's writes differ from the "
                            "run's events\n");
        }
        engine_load(w.e, w.states[0]);
    }
    w.run.replay = 1;
    w.curScore = play(&w, w.cur, 0, NULL);
    build_cache(&w, 0);

    snprintf(path, sizeof(path), "%s/worker-%d.txt", c->out, index);
    snprintf(bestPath, sizeof(bestPath), "%s/best.txt", c->out);
    out = fopen(path, "w");
    if (out == NULL) {
        die("search: cannot write %s", path);
    }
    report(&w, out);
    for (;;) {
        long from;
        int kind;
        Score s;
        double t = now_seconds();

        memcpy(w.cand, w.cur, w.n * sizeof(EngineInput));
        from = perturb(&w, &kind);
        if (limited) {
            /* A try that breaks the limits more often than the current inputs is not played. */
            int again;

            for (again = 0; again < 50 && violations(c, w.cand, w.n) > violations(c, w.cur, w.n); again++) {
                memcpy(w.cand, w.cur, w.n * sizeof(EngineInput));
                from = perturb(&w, &kind);
            }
            if (again == 50) {
                continue;
            }
        }
        engine_load(w.e, w.states[cache_slot(&w, from)]);
        s = play(&w, w.cand, cache_slot(&w, from) * c->cacheStride, &w.curScore);
        w.tries++;
        w.kinds[kind]++;
        if (better(&s, &w.curScore) || (same(&s, &w.curScore) && rnd(&w) % 8 == 0)) {
            int improved = better(&s, &w.curScore);

            memcpy(w.cur, w.cand, w.n * sizeof(EngineInput));
            w.curScore = s;
            build_cache(&w, from);
            w.kept++;
            if (improved) {
                w.kindsBetter[kind]++;
                report(&w, out);
            }
        }
        if (t - lastCheck > 0.5) {
            lastCheck = t;
            adopt_shared(&w, bestPath);
        }
        if (t - lastStatus > 1.0) {
            FILE *st;

            lastStatus = t;
            snprintf(path, sizeof(path), "%s/worker-%d.status", c->out, index);
            st = fopen(path, "w");
            if (st != NULL) {
                fprintf(st, "%ld %.3f\n", w.tries, t - start);
                fclose(st);
            }
            snprintf(path, sizeof(path), "%s/stop", c->out);
            st = fopen(path, "r");
            if (st != NULL) {
                fclose(st);
                break;
            }
        }
        if (c->seconds > 0 && t - start > c->seconds) {
            break;
        }
    }
    fclose(out);
    return 0;
}

/* ---- The coordinator. */

static void write_best(const Config *c, const char *line, long len) {
    char path[1024];
    char tmp[1024];
    EngineInput *in = malloc(len * sizeof(EngineInput));
    Score s;
    FILE *f;
    long k;

    snprintf(path, sizeof(path), "%s/best.txt", c->out);
    snprintf(tmp, sizeof(tmp), "%s/best.tmp", c->out);
    if (parse_best(line, len, in, &s) != 0) {
        free(in);
        return;
    }
    f = fopen(tmp, "w");
    if (f == NULL) {
        free(in);
        return;
    }
    {
        char sc[128];

        format_score(&s, sc, sizeof(sc));
        fprintf(f, "# The best inputs found for reads %ld to %ld: %s; the plays end at read %ld\n", c->first,
                c->end - 1, sc, c->playEnd);
    }
    for (k = 0; k < len; k++) {
        char buf[64];

        input_format(&in[k], buf, sizeof(buf));
        fprintf(f, "%ld: %s\n", c->first + k, buf);
    }
    /* The line the workers read. */
    fprintf(f, "# %s", line);
    fclose(f);
    remove(path);
    rename(tmp, path);
    free(in);
}

int search_main(int argc, char **argv) {
    Config c;
    const char *configPath = NULL;
    const char *romArg = NULL;
    int workerIndex = -1;
    int workers = -1;
    double seconds = -1;
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;

        if (!strcmp(a, "--rom") && next) {
            romArg = argv[++i];
        } else if (!strcmp(a, "--worker") && next) {
            workerIndex = atoi(argv[++i]);
        } else if (!strcmp(a, "--workers") && next) {
            workers = atoi(argv[++i]);
        } else if (!strcmp(a, "--seconds") && next) {
            seconds = atof(argv[++i]);
        } else if (a[0] != '-' && configPath == NULL) {
            configPath = a;
        } else {
            die("usage: glover search CONFIG [--rom ROM] [--workers N] [--seconds S]");
        }
    }
    if (configPath == NULL) {
        die("usage: glover search CONFIG [--rom ROM] [--workers N] [--seconds S]");
    }
    load_config(&c, configPath);
    if (romArg != NULL) {
        c.rom = romArg;
    }
    if (workers > 0) {
        c.workers = workers;
    }
    if (seconds >= 0) {
        c.seconds = seconds;
    }
    if (c.rom == NULL) {
        die("search: give the ROM with --rom or \"rom\" in the config");
    }
    if (workerIndex >= 0) {
        lower_priority();
        return worker_main(&c, workerIndex);
    }

    /* The coordinator. */
    {
        Run run;
        char err[512];

        if (run_load(&run, c.runPath, err, sizeof(err)) != 0) {
            die("search: %s", err);
        }
        window_check(&c, &run);
        run_free(&run);
    }
    {
        int n = c.workers > 0 ? c.workers : cpu_logical();
        FILE **procs = calloc(n, sizeof(FILE *));
        long *seen = calloc(n, sizeof(long));
        char *bestLine = NULL;
        Score best;
        double start = now_seconds();
        long len = c.end - c.first;
        char path[1024];
        FILE *logf;
        size_t cap = 1 << 20;
        char *line = malloc(cap);

        best.frame = NOT_REACHED;
        best.progress = -1e300;
        best.loads = 0;
        make_dir(c.out);
        snprintf(path, sizeof(path), "%s/stop", c.out);
        remove(path);
        snprintf(path, sizeof(path), "%s/best.txt", c.out);
        remove(path);
        for (i = 0; i < n; i++) {
            static const char *const kinds[] = {"txt", "status", "done"};
            int j;

            for (j = 0; j < 3; j++) {
                snprintf(path, sizeof(path), "%s/worker-%d.%s", c.out, i, kinds[j]);
                remove(path);
            }
        }
        snprintf(path, sizeof(path), "%s/log.txt", c.out);
        logf = fopen(path, "a");
        printf("search: %d workers, reads %ld to %ld (the plays end at read %ld), output in %s\n", n, c.first,
               c.end - 1, c.playEnd, c.out);
        for (i = 0; i < n; i++) {
            char cmd[8192] = "";
            char num[32];

            quote_arg(cmd, sizeof(cmd), self_path());
            quote_arg(cmd, sizeof(cmd), "search");
            quote_arg(cmd, sizeof(cmd), configPath);
            quote_arg(cmd, sizeof(cmd), "--rom");
            quote_arg(cmd, sizeof(cmd), c.rom);
            snprintf(num, sizeof(num), "%d", i);
            quote_arg(cmd, sizeof(cmd), "--worker");
            quote_arg(cmd, sizeof(cmd), num);
            snprintf(num, sizeof(num), "%g", c.seconds);
            quote_arg(cmd, sizeof(cmd), "--seconds");
            quote_arg(cmd, sizeof(cmd), num);
            procs[i] = proc_start(cmd);
        }
        int tty = isatty(fileno(stdout));

        for (;;) {
            long tries = 0;
            double t;

            sleep_until(now_seconds() + 1.0);
            t = now_seconds() - start;
            for (i = 0; i < n; i++) {
                FILE *f;
                long count;
                double secs;

                snprintf(path, sizeof(path), "%s/worker-%d.txt", c.out, i);
                f = fopen(path, "r");
                if (f != NULL) {
                    long lineNo = 0;

                    while (fgets(line, (int)cap, f)) {
                        size_t len0 = strlen(line);
                        Score s;

                        if (len0 == 0 || line[len0 - 1] != '\n') {
                            break; /* the worker is still writing this line */
                        }
                        if (lineNo++ < seen[i]) {
                            continue;
                        }
                        seen[i] = lineNo;
                        s.loads = 0;
                        if (sscanf(line, "best %ld %lf %d", &s.frame, &s.progress, &s.loads) >= 2 &&
                            better(&s, &best)) {
                            char sc[128];

                            best = s;
                            free(bestLine);
                            bestLine = strdup(line);
                            write_best(&c, bestLine, len);
                            format_score(&s, sc, sizeof(sc));
                            printf("search: %7.1f s  new best from worker %d: %s\n", t, i, sc);
                            if (logf != NULL) {
                                fprintf(logf, "%.1f %ld %.6f %d\n", t, s.frame, s.progress, s.loads);
                                fflush(logf);
                            }
                        }
                    }
                    fclose(f);
                }
                snprintf(path, sizeof(path), "%s/worker-%d.status", c.out, i);
                f = fopen(path, "r");
                if (f != NULL) {
                    if (fscanf(f, "%ld %lf", &count, &secs) == 2) {
                        tries += count;
                    }
                    fclose(f);
                }
            }
            if (tty) {
                printf("\rsearch: %7.1f s  %ld tries (%.0f a second)  ", t, tries, t > 0 ? tries / t : 0.0);
            } else if ((long)t % 10 == 0) {
                /* A file or a pipe (the studio's search.log) gets a whole line every 10 seconds. */
                printf("search: %7.1f s  %ld tries (%.0f a second)\n", t, tries, t > 0 ? tries / t : 0.0);
            }
            fflush(stdout);
            if (c.seconds > 0 && t > c.seconds + 3) {
                break;
            }
            /* The workers stop on OUT/stop; a worker that ends for any reason leaves worker-N.done. */
            {
                int done = 0;
                FILE *f;

                for (i = 0; i < n; i++) {
                    snprintf(path, sizeof(path), "%s/worker-%d.done", c.out, i);
                    f = fopen(path, "r");
                    if (f != NULL) {
                        done++;
                        fclose(f);
                    }
                }
                snprintf(path, sizeof(path), "%s/stop", c.out);
                f = fopen(path, "r");
                if (f != NULL) {
                    fclose(f);
                    break;
                }
                if (done == n) {
                    break;
                }
            }
        }
        if (tty) {
            printf("\n");
        }
        for (i = 0; i < n; i++) {
            if (procs[i] != NULL) {
                proc_wait(procs[i]);
            }
        }
        if (bestLine != NULL) {
            char sc[128];

            format_score(&best, sc, sizeof(sc));
            printf("search: the best: %s, in %s/best.txt\n", sc, c.out);
        }
        free(line);
        if (logf != NULL) {
            fclose(logf);
        }
        if (bestLine == NULL) {
            printf("search: the search ended without a result\n");
            return 1;
        }
    }
    return 0;
}
