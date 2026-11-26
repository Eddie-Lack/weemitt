/* The studio: the window program with the brute forcer (bruteforce/). It keeps a timeline of the game's frames,
   pauses and scrubs it, and hands control between the player and the search. Only the studio build (make studio)
   has it; platform.c and menu.cpp call it where PORT_STUDIO is defined.

   The timeline holds the input of every game frame since the game started, and a saved state every few seconds.
   Frame f is the game after f updates. Going to a frame loads the nearest earlier state and replays the recorded
   inputs to it. */
#ifndef STUDIO_H
#define STUDIO_H

#include "frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Runs a brute forcer command (glover search, bench, play, saveload) when argv[1] names one, and returns 1 with
   its exit code in *code. Returns 0 otherwise. */
int studio_command(int argc, char **argv, int *code);

/* Sets up the savestates. Call it after the ROM is loaded and before the game starts. Exits with a message when
   it cannot. */
void studio_init(const unsigned char *rom, unsigned long romSize);

/* After port_game_init: the timeline's first state. level is --level (-1: the game starts at power-on). */
void studio_started(long level);

/* Once for each drawn frame, after the controller is read: the polled buttons and stick. All four C buttons at
   once pause the game, or resume it from the frame on show. While paused, the controller moves on the timeline
   (studio_controls_help) and the game gets nothing. The C buttons of the chord are taken out of the input. */
void studio_poll(unsigned short *buttons, signed char *x, signed char *y);
/* Buttons that the platform layer must drop from its pending taps (the chord's, the A that resumed, or all while
   paused). */
unsigned short studio_input_mask(void);
/* The input of update f of the timeline (the update from frame f to frame f + 1). Returns 0 when the timeline does
   not hold it. */
int studio_input_at(long f, unsigned short *buttons, signed char *x, signed char *y);
/* 1 when a take changed the input of update f, and the take can still be undone. */
int studio_input_changed(long f);

/* 1 while the game's logic waits. */
int studio_paused(void);
/* While paused: 1 when the window should run one update now (a step forward). */
int studio_take_step(void);

/* Around every update (platform.c's run_update). studio_before_update gets the input the game will get: on a frame
   that the timeline already holds it puts the recorded input there; otherwise it records it. studio_after_update
   counts the frame and saves the timeline's states. tasks and count are what port_game_update returned: the
   frame's display lists, which the studio keeps with its states. */
void studio_before_update(unsigned short *buttons, signed char *x, signed char *y);
void studio_after_update(const PortFrameTask *tasks, long count);

/* Goes to the frame asked for, if any: loads and replays, and returns the draw tasks of that frame and their count
   (0: nothing to draw). Returns -1 when there is nothing to do. The renderer must then start a new interpolation
   (gfx_interp_record with cut 1). */
long studio_apply_seek(PortFrameTask *tasks, int max);

/* For the studio bar (menu.cpp). */
typedef struct StudioInfo {
    long frame;     /* the frame on show */
    long length;    /* the frames the timeline holds */
    long states;    /* saved states */
    long spacing;   /* frames between saved states */
    double megabytes; /* memory of the saved states */
    int paused;
    int playing;    /* playing the recording */
} StudioInfo;
void studio_info(StudioInfo *info);
void studio_request_seek(long frame);
/* Pause (1) or resume (0). A resume plays on from the frame on show with the player's input: the frames after it
   are dropped. */
void studio_set_paused(int paused);
/* Plays the recorded frames from the frame on show, and pauses at the end. */
void studio_play_recording(void);
/* One frame on: the recorded one, or at the end of the timeline one more frame with no buttons. */
void studio_step_forward(void);
/* Starts a level, as the level select does, on a new timeline: the jump happens at its first update. Returns 0
   when the game is not where a level can start (port_goto_possible). */
int studio_goto_level(long level);
/* ---- State files (docs/studio.md, "State files"). A state file holds the game at one frame: its memory, as a
   savestate holds it. Only the build that saved it loads it. */
/* Writes the game at the frame on show to path. Returns 0, or -1 with the reason in err. */
int studio_save_state(const char *path, char *err, int errSize);
/* Loads a state file and makes it the first frame of a new timeline, paused, as studio_goto_level does. Returns 0,
   or -1 with the reason in err: the game and the timeline are then unchanged. */
int studio_load_state(const char *path, char *err, int errSize);
/* The menu's folder of state files, studio/states/ in the preference folder, with a slash at its end. It is made
   when it is missing. NULL when there is no preference folder. */
const char *studio_states_folder(void);
/* The names of the state files in that folder (NAME.state), sorted. Returns how many, at most max. */
int studio_list_states(char (*names)[128], int max);
/* The path of the state file NAME in that folder, with .state added. Returns 0, or -1 with the reason in err when
   the name is empty, holds a character other than letters, digits, spaces, '-', '_' and '.', or is a device name of
   Windows (CON, NUL, COM1 and the others). */
int studio_state_path(const char *name, char *path, int pathSize, char *err, int errSize);
/* The path of a name from studio_list_states, with .state added. It applies none of the rules for new names: a
   file put in the folder by hand may have any name. Returns -1 when there is no folder. */
int studio_listed_state_path(const char *name, char *path, int pathSize);
/* 1 when studio_state_path(name) names a file that exists. On Windows the name's case does not matter. */
int studio_state_exists(const char *name);

/* The controls while paused, as one line of text. */
const char *studio_controls_help(void);

/* ---- Points and the search (bruteforce/docs/plan.md). The start point is a frame of the timeline. The other
   points are places (a sphere at the glove's or the ball's position when it was marked) or the level's end, to
   reach in order; the last is the end point. */
typedef struct StudioPoint {
    int place;      /* 1: a place; 0: the level is done (its exit has started) */
    float at[3];
    float radius;
    int ofBall;     /* the place is for the ball, not the glove */
    long frame;     /* the frame it was marked at */
} StudioPoint;
#define STUDIO_MAX_POINTS 16
void studio_set_rom_path(const char *path);
/* The start point: the frame on show. */
void studio_mark_start(void);
/* A place at the glove's (or the ball's) position now, or the level's end, as the next point. */
void studio_add_place(int ofBall, float radius);
void studio_add_level_end(void);
void studio_remove_point(int i);
/* The start frame (-1: none) and the points. */
long studio_start_frame(void);
int studio_points(const StudioPoint **points);

/* The game frames that the search counts for each load (its "loadFrames"): 3.5 seconds on a console
   (bruteforce/docs/game.md). The game runs a load in one update. */
#define STUDIO_LOAD_FRAMES 70

typedef struct StudioSearchInfo {
    int running;
    int haveBest;
    long bestFrame;   /* the timeline frame at which the best inputs reach the end point, or -1 */
    long firstFrame;  /* the same for the first result seen (the player's route), or -1 */
    int bestLoads;    /* the loads the best inputs pass before the end point (the level is left) */
    int firstLoads;   /* the same for the first result */
    double progress;  /* when the end point is not reached */
    long baseFrame;   /* the player's own route: the frame at which it reaches the end point (-1: never; -2: not
                         known yet) */
    int baseLoads;
    long takenFrame;  /* the result on the timeline after a take (-1: none) */
    int takenLoads;
    double seconds;   /* how long the search has run */
    long tries;       /* the tries of all its workers */
    double timeLimit; /* the search's time limit in seconds (0: none) */
    char folder[512];
    char message[400];
} StudioSearchInfo;
/* Starts the search from the start point to the points, with the timeline's inputs after the start as its base.
   limits: what a runner can play, "none", "loose" or "strict" (bruteforce/README.md, "The search"). Returns 0, or
   -1 with the reason in the info's message. */
int studio_start_search(int workers, double seconds, const char *limits);
void studio_stop_search(void);
void studio_search_info(StudioSearchInfo *info);
/* 1 when a new timeline would drop work: points, a running search, a result or a take. */
int studio_has_work(void);
/* Removes the folders of the earlier searches (studio/search-NNNN), but not the latest one's. Returns how many. */
int studio_delete_old_searches(void);
/* The number and the bytes of the search folders in the studio's folder. */
int studio_search_folders(double *megabytes);
/* ---- Route files: the timeline's inputs from its first frame, NAME.route in studio/routes/ of the preference
   folder. They hold no game data, so they can be shared. A route loads on a timeline that starts as it did: at
   power-on, at the same level, or at the same state file. */
int studio_save_route(const char *name, char *err, int errSize);
int studio_load_route(const char *name, char *err, int errSize);
int studio_list_routes(char (*names)[128], int max);
const char *studio_routes_folder(void);
/* Where the timeline starts, as route files name it: "power-on", "level N" or "state HASH". */
const char *studio_origin(void);
/* Puts the best inputs on the timeline after the start point, up to the frame they reach the end point, and goes
   to the start point. The timeline before is kept for studio_undo_take. */
void studio_take_result(void);
int studio_can_undo_take(void);
void studio_undo_take(void);
/* The points as lines for the 3D view (PortDebugLine). Returns the number written, at most max. */
long studio_overlay_lines(PortDebugLine *out, long max);

/* The self-test (WEEMITT_STUDIO_SELFTEST with --headless): goes to many frames of the timeline and compares each
   with the state that the game had there. Returns the number of differences. */
long studio_selftest(void);

#ifdef __cplusplus
}
#endif

#endif
