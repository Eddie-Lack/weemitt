/* A run: an events script in memory (glover play --events explains the script; tools/convert.py writes one from a
   movie). The search plays a run to the start of its window, then plays its own inputs.

   A run of whole game frames ("F INPUT" lines) needs no emulator: the studio writes one (docs/studio.md). With a
   "#state FILE" line it starts from that saved state instead of power-on; FILE is relative to the script's folder,
   and the state must come from the same program and ROM. */
#ifndef RUN_H
#define RUN_H

#include <stddef.h>

#include "engine.h"
#include "watch.h"

typedef enum EventType { EV_READ, EV_DRAW, EV_LOGIC, EV_WRITE, EV_DRAW_WITH, EV_FRAME } EventType;

typedef struct Event {
    EventType type;
    EngineInput input; /* EV_READ, EV_FRAME (a whole game frame: a read, a draw and a logic frame, engine_step) */
    int count;         /* EV_LOGIC: frames */
    int arg;           /* EV_WRITE: the index in writes; EV_DRAW_WITH: the index in drawArgs; EV_LOGIC: the first
                          group */
    int ngroups;       /* EV_LOGIC: the groups of events that run at stops inside this frame */
    long skip;         /* EV_LOGIC with groups: the event after its last group */
} Event;

typedef struct Run {
    Event *events;
    long count;
    long *readAt; /* the event index of each read */
    unsigned char *busy; /* 1: the game thread was in the middle of a frame at this read ("#busy" before it) */
    unsigned char *grouped; /* 1: the read is in a group */
    long reads;
    Watch *writes;
    uint32_t *writeValues;
    long nwrites;
    char **drawArgs;
    long ndraws;
    /* Groups: events that run at a stop inside a logic frame (engine_logic_stops). Group g holds the events
       [groupFrom[g], groupTo[g]) and runs at stops[g]. */
    EngineStop *stops;
    long *groupFrom;
    long *groupTo;
    long ngroups;
    /* 1: run_exec plays other inputs than the movie's: it skips the writes and draws without values. These hold
       the emulator's values for the movie's own inputs. */
    int replay;
    /* The EEPROM's bytes at power-on ("#eeprom-fill N" in the script, EngineOptions.eepromFill), or -1 for the
       engine's default. BizHawk's core starts with zeros. */
    int eepromFill;
    /* The saved state the run starts from ("#state"), or NULL for power-on. */
    char *statePath;
} Run;

/* Reads an events script. Returns 0, or -1 with a message in err.

   After an "L" line, groups may follow: "@ STOP" starts one and "@end" ends it. Its events run at the stop inside
   that logic frame. STOP names the hooked calls in progress, from the outermost in, separated by ">": "NAME" or
   "NAME=ADDRESS" (the N64 address of the call's pointer argument), then "nth=N" for the N-th such call (default 0)
   and "leave" for where the innermost call returns. For example "@ UpdateEnemies > UpdateActor=0x80299ce8". */
int run_load(Run *r, const char *path, char *err, size_t errSize);
/* The script's "#eeprom-fill" value, or -1, without loading the script. */
int run_eeprom_fill(const char *path);
void run_free(Run *r);
/* Starts the run: loads its state when it has one. Returns 0, or -1 with a message in err. */
int run_start(Engine *e, const Run *r, char *err, size_t errSize);

/* Called before each read with the read's number. A nonzero return stops run_exec_cb before the read; inside a
   group it ends only the group. */
typedef int (*RunReadFn)(Engine *e, long read, void *ctx);

/* Runs events [from, to). A draw with values (EV_DRAW_WITH) writes them, draws and puts the game's values back. A
   logic frame with groups runs each group at its stop; a group whose stop the frame does not reach runs after the
   frame. Returns the number of such groups. run_exec_cb calls fn before each read. */
long run_exec(Engine *e, const Run *r, long from, long to);
long run_exec_cb(Engine *e, const Run *r, long from, long to, RunReadFn fn, void *ctx);

/* 1 when read k is outside a group and the engine can play other inputs from it to the next such read with
   run_exec in replay mode: the game thread waited between frames at the read, and what follows is writes, draws
   and logic frames whose groups each hold one read. A lag frame has this form. A load does not: its reads come
   outside groups, or at a stop in CheckForExits (a level's exit). */
int run_playable(const Run *r, long k);

#endif
