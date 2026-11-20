#include "run.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inputs.h"
#include "util.h"

static void add(Run *r, const Event *ev, long *cap) {
    if (r->count == *cap) {
        *cap = *cap ? *cap * 2 : 4096;
        r->events = realloc(r->events, *cap * sizeof(Event));
    }
    r->events[r->count++] = *ev;
}

/* Parses a stop: "NAME[=ADDRESS] > NAME[=ADDRESS] ... [nth=N] [leave]". Returns 0 or -1. */
static int parse_stop(EngineStop *s, char *text, char *err, size_t errSize) {
    char *tok;

    memset(s, 0, sizeof(*s));
    for (tok = strtok(text, " \t\r\n"); tok != NULL; tok = strtok(NULL, " \t\r\n")) {
        char *eq;
        int hook;

        if (!strcmp(tok, ">")) {
            continue;
        }
        if (!strcmp(tok, "leave")) {
            s->leave = 1;
            continue;
        }
        if (!strncmp(tok, "nth=", 4)) {
            s->nth = atoi(tok + 4);
            continue;
        }
        eq = strchr(tok, '=');
        if (eq != NULL) {
            *eq = 0;
        }
        hook = engine_hook_index(tok);
        if (hook < 0) {
            snprintf(err, errSize, "%.100s is not a hooked function (engine/hooks.def)", tok);
            return -1;
        }
        if (s->depth == ENGINE_STOP_DEPTH) {
            snprintf(err, errSize, "a stop names at most %d calls", ENGINE_STOP_DEPTH);
            return -1;
        }
        s->hook[s->depth] = hook;
        s->arg[s->depth] = eq != NULL ? (uint32_t)strtoul(eq + 1, NULL, 0) : ENGINE_ANY;
        s->depth++;
    }
    if (s->depth == 0) {
        snprintf(err, errSize, "a stop names no call");
        return -1;
    }
    return 0;
}

int run_load(Run *r, const char *path, char *err, size_t errSize) {
    FILE *f = fopen(path, "r");
    char line[4096];
    long lineNo = 0;
    long cap = 0;
    long readCap = 0;
    long writeCap = 0;
    int busy = 0;
    long lastLogic = -1; /* the logic frame that groups attach to: the last event, when it is one */
    int inGroup = 0;

    memset(r, 0, sizeof(*r));
    r->eepromFill = -1;
    if (f == NULL) {
        snprintf(err, errSize, "cannot open %s", path);
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        Event ev;
        char msg[256];

        lineNo++;
        memset(&ev, 0, sizeof(ev));
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == 'R' || *p == 'F') {
            if (input_parse(p + 1, &ev.input, msg, sizeof(msg)) != 0) {
                snprintf(err, errSize, "%.200s line %ld: %.200s", path, lineNo, msg);
                fclose(f);
                return -1;
            }
            ev.type = *p == 'F' ? EV_FRAME : EV_READ;
            if (ev.type == EV_FRAME && inGroup) {
                snprintf(err, errSize, "%.200s line %ld: a group cannot hold a whole frame", path, lineNo);
                fclose(f);
                return -1;
            }
            if (r->reads == readCap) {
                readCap = readCap ? readCap * 2 : 4096;
                r->readAt = realloc(r->readAt, readCap * sizeof(long));
                r->busy = realloc(r->busy, readCap);
                r->grouped = realloc(r->grouped, readCap);
            }
            r->busy[r->reads] = (unsigned char)busy;
            r->grouped[r->reads] = (unsigned char)inGroup;
            busy = 0;
            r->readAt[r->reads++] = r->count;
        } else if (*p == 'L') {
            ev.type = EV_LOGIC;
            ev.count = (int)strtol(p + 1, NULL, 10);
            if (ev.count <= 0) {
                ev.count = 1;
            }
        } else if (*p == 'D') {
            char *q = p + 1;

            while (*q == ' ' || *q == '\t') {
                q++;
            }
            if (*q == '\n' || *q == '\r' || *q == 0) {
                ev.type = EV_DRAW;
            } else {
                ev.type = EV_DRAW_WITH;
                ev.arg = (int)r->ndraws;
                r->drawArgs = realloc(r->drawArgs, (r->ndraws + 1) * sizeof(char *));
                r->drawArgs[r->ndraws++] = strdup(q);
            }
        } else if (*p == 'W') {
            char name[128];
            char value[64];

            if (r->nwrites == writeCap) {
                writeCap = writeCap ? writeCap * 2 : 4096;
                r->writes = realloc(r->writes, writeCap * sizeof(Watch));
                r->writeValues = realloc(r->writeValues, writeCap * sizeof(uint32_t));
            }
            if (sscanf(p + 1, "%127s %63s", name, value) != 2 ||
                watch_parse(&r->writes[r->nwrites], name, msg, sizeof(msg)) != 0) {
                snprintf(err, errSize, "%.200s line %ld: bad write", path, lineNo);
                fclose(f);
                return -1;
            }
            r->writeValues[r->nwrites] = (uint32_t)strtoul(value, NULL, 0);
            ev.type = EV_WRITE;
            ev.arg = (int)r->nwrites++;
        } else if (!strncmp(p, "@end", 4)) {
            if (!inGroup) {
                snprintf(err, errSize, "%.200s line %ld: @end outside a group", path, lineNo);
                fclose(f);
                return -1;
            }
            inGroup = 0;
            r->groupTo[r->ngroups - 1] = r->count;
            r->events[lastLogic].skip = r->count;
            continue;
        } else if (*p == '@') {
            /* lastLogic stays set while only groups follow the frame. */
            if (inGroup || lastLogic < 0 || r->events[lastLogic].count != 1) {
                snprintf(err, errSize, "%.200s line %ld: a group must follow an \"L\" line or another group", path,
                         lineNo);
                fclose(f);
                return -1;
            }
            r->stops = realloc(r->stops, (r->ngroups + 1) * sizeof(EngineStop));
            r->groupFrom = realloc(r->groupFrom, (r->ngroups + 1) * sizeof(long));
            r->groupTo = realloc(r->groupTo, (r->ngroups + 1) * sizeof(long));
            if (parse_stop(&r->stops[r->ngroups], p + 1, msg, sizeof(msg)) != 0) {
                snprintf(err, errSize, "%.200s line %ld: %.200s", path, lineNo, msg);
                fclose(f);
                return -1;
            }
            if (r->events[lastLogic].ngroups == 0) {
                r->events[lastLogic].arg = (int)r->ngroups;
            }
            r->events[lastLogic].ngroups++;
            r->groupFrom[r->ngroups] = r->groupTo[r->ngroups] = r->count;
            r->ngroups++;
            inGroup = 1;
            continue;
        } else if (!strncmp(p, "#busy", 5)) {
            busy = 1;
            continue;
        } else if (!strncmp(p, "#state", 6)) {
            /* The file name runs to the end of the line, relative to the script's folder. */
            const char *slash = strrchr(path, '/');
            const char *back = strrchr(path, '\\');
            size_t dir;
            char *q = p + 6;
            char *end;

            while (*q == ' ' || *q == '\t') {
                q++;
            }
            end = q + strlen(q);
            while (end > q && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) {
                *--end = 0;
            }
            if (back != NULL && (slash == NULL || back > slash)) {
                slash = back;
            }
            dir = slash != NULL && q[0] != '/' && !(q[0] && q[1] == ':') ? (size_t)(slash - path + 1) : 0;
            free(r->statePath);
            r->statePath = malloc(dir + strlen(q) + 1);
            memcpy(r->statePath, path, dir);
            strcpy(r->statePath + dir, q);
            continue;
        } else if (!strncmp(p, "#eeprom-fill", 12)) {
            r->eepromFill = (int)strtol(p + 12, NULL, 0);
            continue;
        } else if (*p == '#' || *p == '\n' || *p == '\r' || *p == 0) {
            continue;
        } else {
            snprintf(err, errSize, "%.200s line %ld: unknown event", path, lineNo);
            fclose(f);
            return -1;
        }
        if (ev.type == EV_LOGIC && inGroup) {
            snprintf(err, errSize, "%.200s line %ld: a group cannot hold a logic frame", path, lineNo);
            fclose(f);
            return -1;
        }
        add(r, &ev, &cap);
        if (!inGroup) {
            lastLogic = ev.type == EV_LOGIC ? r->count - 1 : -1;
        }
    }
    fclose(f);
    if (inGroup) {
        snprintf(err, errSize, "%.200s: a group has no @end", path);
        return -1;
    }
    return 0;
}

int run_eeprom_fill(const char *path) {
    FILE *f = fopen(path, "r");
    char line[256];
    int fill = -1;

    while (f != NULL && fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "#eeprom-fill", 12)) {
            fill = (int)strtol(line + 12, NULL, 0);
            break;
        }
        if (line[0] != '#') {
            break;
        }
    }
    if (f != NULL) {
        fclose(f);
    }
    return fill;
}

void run_free(Run *r) {
    long i;

    for (i = 0; i < r->ndraws; i++) {
        free(r->drawArgs[i]);
    }
    free(r->drawArgs);
    free(r->events);
    free(r->readAt);
    free(r->busy);
    free(r->grouped);
    free(r->writes);
    free(r->writeValues);
    free(r->stops);
    free(r->groupFrom);
    free(r->groupTo);
    free(r->statePath);
    memset(r, 0, sizeof(*r));
}

/* The number of the read at event index i. */
static long read_number(const Run *r, long i) {
    long lo = 0;
    long hi = r->reads - 1;

    while (lo < hi) {
        long mid = (lo + hi) / 2;

        if (r->readAt[mid] < i) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

#define MAX_DRAW_VALUES 64

static void draw_with(Engine *e, const char *args) {
    Watch w[MAX_DRAW_VALUES];
    uint32_t saved[MAX_DRAW_VALUES];
    uint32_t values[MAX_DRAW_VALUES];
    char buf[4096];
    char err[256];
    char *tok;
    int n = 0;
    int i;

    snprintf(buf, sizeof(buf), "%s", args);
    for (tok = strtok(buf, " \t\r\n"); tok != NULL && n < MAX_DRAW_VALUES; tok = strtok(NULL, " \t\r\n")) {
        char *eq = strchr(tok, '=');

        if (eq == NULL) {
            continue;
        }
        *eq = 0;
        if (watch_parse(&w[n], tok, err, sizeof(err)) == 0) {
            values[n++] = (uint32_t)strtoul(eq + 1, NULL, 0);
        }
    }
    for (i = 0; i < n; i++) {
        saved[i] = watch_read(e, &w[i]);
        watch_write(e, &w[i], values[i]);
    }
    engine_draw(e);
    for (i = n - 1; i >= 0; i--) {
        watch_write(e, &w[i], saved[i]);
    }
}

typedef struct StopCtx {
    const Run *r;
    long first; /* the frame's first group */
    RunReadFn fn;
    void *fnCtx;
} StopCtx;

static void run_group(Engine *e, void *ctx, int stop) {
    StopCtx *c = ctx;

    run_exec_cb(e, c->r, c->r->groupFrom[c->first + stop], c->r->groupTo[c->first + stop], c->fn, c->fnCtx);
}

long run_exec_cb(Engine *e, const Run *r, long from, long to, RunReadFn fn, void *ctx) {
    long missed = 0;
    long i;

    for (i = from; i < to && i < r->count; i++) {
        const Event *ev = &r->events[i];
        int k;

        switch (ev->type) {
            case EV_READ:
                if (fn != NULL && fn(e, read_number(r, i), ctx)) {
                    return missed;
                }
                engine_read(e, &ev->input);
                break;
            case EV_FRAME:
                if (fn != NULL && fn(e, read_number(r, i), ctx)) {
                    return missed;
                }
                engine_step(e, &ev->input);
                break;
            case EV_DRAW:
                engine_draw(e);
                break;
            case EV_DRAW_WITH:
                if (r->replay) {
                    engine_draw(e);
                } else {
                    draw_with(e, r->drawArgs[ev->arg]);
                }
                break;
            case EV_LOGIC:
                if (ev->ngroups) {
                    StopCtx c;
                    int left;

                    c.r = r;
                    c.first = ev->arg;
                    c.fn = fn;
                    c.fnCtx = ctx;
                    left = engine_logic_stops(e, &r->stops[ev->arg], ev->ngroups, run_group, &c);
                    /* Groups whose stop the frame did not reach run after it. */
                    for (k = ev->ngroups - left; k < ev->ngroups; k++) {
                        if (getenv("GLOVER_STOP_TRACE") != NULL) {
                            long j = r->groupFrom[ev->arg + k];

                            while (j < r->groupTo[ev->arg + k] && r->events[j].type != EV_READ) {
                                j++;
                            }
                            fprintf(stderr, "the group of read %ld ran after its frame\n",
                                    j < r->groupTo[ev->arg + k] ? read_number(r, j) : -1L);
                        }
                        run_group(e, &c, k);
                    }
                    missed += left;
                    i = ev->skip - 1;
                    break;
                }
                for (k = 0; k < ev->count; k++) {
                    engine_logic(e);
                }
                break;
            case EV_WRITE:
                if (!r->replay) {
                    watch_write(e, &r->writes[ev->arg], r->writeValues[ev->arg]);
                }
                break;
        }
    }
    return missed;
}

int run_start(Engine *e, const Run *r, char *err, size_t errSize) {
    size_t n;
    uint8_t *state;

    if (r->statePath == NULL) {
        return 0;
    }
    state = read_file(r->statePath, &n);
    if (state == NULL) {
        snprintf(err, errSize, "cannot read the state %.400s", r->statePath);
        return -1;
    }
    if (n < ENGINE_STATE_HEADER || engine_state_used(state) != n || engine_load(e, state) != 0) {
        free(state);
        snprintf(err, errSize, "%.400s is not a state of this program and ROM", r->statePath);
        return -1;
    }
    free(state);
    return 0;
}

long run_exec(Engine *e, const Run *r, long from, long to) {
    return run_exec_cb(e, r, from, to, NULL, NULL);
}

int run_playable(const Run *r, long k) {
    int exits = engine_hook_index("CheckForExits");
    long i;

    if (k < 0 || k >= r->reads || r->busy[k] || r->grouped[k]) {
        return 0;
    }
    for (i = r->readAt[k] + 1; i < r->count && r->events[i].type != EV_READ; i++) {
        const Event *ev = &r->events[i];
        long g;
        long j;
        int reads = 0;

        if (ev->type != EV_LOGIC || ev->ngroups == 0) {
            continue;
        }
        for (g = ev->arg; g < ev->arg + ev->ngroups; g++) {
            /* CheckForExits loads the next level or the restart; that load needs the emulator's writes. */
            if (r->stops[g].hook[0] == exits) {
                return 0;
            }
            for (j = r->groupFrom[g]; j < r->groupTo[g]; j++) {
                reads += r->events[j].type == EV_READ;
            }
        }
        if (reads != ev->ngroups) {
            return 0;
        }
        i = ev->skip - 1;
    }
    return 1;
}
