/* The interface between the game (frame.c) and the platform layer (platform.c). It uses plain C types only: the
   platform layer is compiled without the game's headers. */
#ifndef FRAME_H
#define FRAME_H

/* One display list for the renderer: its address and size in bytes, and whether it is for the Sprite2D
   microcode. */
typedef struct PortFrameTask {
    unsigned int addr;
    unsigned int size;
    int sprite2d;
} PortFrameTask;

/* The player's ROM (big-endian bytes) and the controller, set by the platform layer. The game's u32 and s32 are
   unsigned int and int, on every host. */
extern unsigned char *port_rom;
extern unsigned int port_rom_size;
extern unsigned short port_buttons;
extern signed char port_stick_x, port_stick_y;
extern unsigned short port_buttons_read; /* the word that the game's latest read of the controller received */
extern unsigned int port_controller_reads; /* how many times the game has read the controller */
extern unsigned char port_eeprom[512];
extern int port_eeprom_dirty;

void port_game_init(int level);
/* Turns the Space demo on (spacedemo.c) before port_game_init. The start level is then ignored. */
void port_set_space_demo(int on);
/* Turns the easy difficulty's stick direction on the ball on for every difficulty (Enhancements menu). */
void port_set_easy_ball(int on);
/* Sets how much faster the camera follows the glove, 1.0 (original) to 3.0 (Enhancements menu). */
void port_set_camera_follow(float speed);
/* Ballworld (src/game/ballworld.c). The requested value comes from the menu, settings.ini or --ballworld. The game
   switches to it when a level starts (StartNewLevel calls port_ballworld_latch). */
void port_set_ballworld(int on);
int port_get_ballworld(void);                     /* the requested value */
void port_ballworld_latch(void);
extern int port_save_ballworld;                   /* the mode that the open save file belongs to */
extern int port_save_locked;                      /* 1: the mode differs from the save file's, so nothing is written */
/* The settings that change the game's logic, as the game holds them now. */
int port_get_easy_ball(void);
float port_get_camera_follow(void);
/* The host's scale for the distances from the game's camera in the sound, the wakes, the debris and the shadows
   (porthooks.h, port_camera_dist_scale). 1 keeps the game's own. */
void port_set_camera_dist_scale(float scale);
float port_get_camera_dist_scale(void);
/* Options that play with the game (Enhancements and Cheats menus). A cheat number is the game's own (cheat.c): 3 is
   INFINITE LIVES, 6 INFINITE ENERGY, 23 LEVEL SELECT, 24 OPEN PORTALS and 25 OPEN LEVELS. */
void port_set_cheat_hold(int mask);              /* the cheat bits held on at every update (0: none) */
void port_set_cheat_bit(int num, int on);       /* turns a cheat on or off now, and holds or releases it */
int port_get_cheat_bits(void);                   /* the game's cheat bits as they are now */
int port_space_demo(void);                       /* 1 in the Space demo */
void port_set_pause_cheats(int on);              /* R or Z in the pause menu opens the cheat list */
void port_set_skip_intro(int on);                /* skip the start-up screens up to the title menu */
unsigned short port_intro_skip_buttons(void);     /* buttons to add to the next update's to skip them */
int port_goto_possible(void);                   /* 1 in a level or in the title menu, where port_goto_level works */
void port_goto_level(int level);                 /* start the level at the next update, as the level select does */
int port_level_count(void);                      /* port_game_init's levels: 0 to this - 1 (and -1) */
int port_goto_level_count(void);                 /* port_goto_level's levels: 0 to this - 1 */
int port_game_frame(PortFrameTask *tasks, int max);
int port_game_update(float dt, PortFrameTask *tasks, int max);
int port_game_ticked(void);
int port_game_demo(void);
int port_game_retraces(void);
void port_game_dynamic_range(unsigned int *lo, unsigned int *hi);
/* 1 when the latest game frame does not continue the one before (mode, level or frame counter changed). Call it
   once for each game frame. */
int port_game_cut(void);
/* The addresses of the two colour images, as the display lists hold them (Ptr32 words, ptr32.h). */
void port_game_framebuffers(unsigned int *a, unsigned int *b);
int port_game_mode(void);
/* Called by port_fatal (libultra.c) after its message, in place of exit(1), when set. It must not return. */
extern void (*port_fatal_hook)(void);
/* Set: the game's own messages (port_log, the game's debug printing) are dropped. The studio's search workers set
   it. */
extern int port_log_quiet;
/* port_fatal's latest message, for the hook. */
extern char port_fatal_message[900];
int port_game_level(void);
/* What the Developer menu draws over the 3D view (devview.c). Both read the game's state and change nothing. */
typedef struct PortDebugLine {
    float a[3], b[3];       /* the ends, in world space */
    unsigned int color;     /* red in the lowest byte, alpha in the highest */
} PortDebugLine;

/* The collision faces within range of the glove (landscape and platforms, coloured by kind) and the collision
   spheres of the glove, the ball and the enemies near them, as lines. Returns the number of lines, at most max. It
   returns 0 outside a level. types is a set of PORT_COLL_ bits: the kinds to draw. */
int port_debug_collision(PortDebugLine *out, int max, float range, unsigned int types);

/* The kinds of collision lines, as bits of the types argument. */
enum {
    PORT_COLL_FLOOR = 1 << 0,
    PORT_COLL_WALL = 1 << 1,
    PORT_COLL_CEILING = 1 << 2,
    PORT_COLL_PLATFORM = 1 << 3,
    PORT_COLL_WATER = 1 << 4,
    PORT_COLL_HURT = 1 << 5,
    PORT_COLL_HAND = 1 << 6,
    PORT_COLL_BALL = 1 << 7,
    PORT_COLL_ENEMY = 1 << 8,
    PORT_COLL_ALL = (1 << 9) - 1
};

enum { PORT_DEBUG_HAND, PORT_DEBUG_BALL, PORT_DEBUG_ENEMY, PORT_DEBUG_PLATFORM, PORT_DEBUG_COLLECTABLE, PORT_DEBUG_OTHER };

typedef struct PortDebugLabel {
    float pos[3];
    float distance;         /* from the glove */
    int kind;               /* PORT_DEBUG_HAND and so on */
    char name[24];
} PortDebugLabel;

/* The actors within range of the glove with their names: the enemy type for an enemy, the object's name (the
   model's name in the level file) for the rest. Returns the number of labels, at most max. */
int port_debug_labels(PortDebugLabel *out, int max, float range);

/* The game's debug features (debugpad.c, docs/menu.md "Game debug"). The game's debug code reads a fifth controller
   entry, pad[4]. While the pad is on, the platform layer sets the three variables below before each update, and
   ControllerStep fills pad[4] from them in the same read that fills pad[0]. */
extern unsigned short port_debug_buttons;
extern signed char port_debug_stick_x, port_debug_stick_y;
void port_debug_pad_mode(int on);
int port_debug_pad_is_on(void);
void port_debug_pad_update(void);
/* Debug actions: each puts the input of a debug control into pad[4] for the next game frames. */
enum {
    PORT_DBG_PREV_PLATFORM, PORT_DBG_NEXT_PLATFORM, PORT_DBG_MARK_LEVELS, PORT_DBG_TIMING, PORT_DBG_PREV_SONG,
    PORT_DBG_NEXT_SONG, PORT_DBG_INERTIA, PORT_DBG_QUIT, PORT_DBG_HURT, PORT_DBG_AUTOPLAY, PORT_DBG_COUNT
};
int port_debug_action(int action);              /* 1 when started; 0 while the one before it is still going */
int port_debug_busy(void);
int port_debug_in_level(void);                   /* 1 where the debug controls act: in a level */
int port_debug_on_title(void);                   /* 1 while the title menu runs, where Z cycles the recording mode */
int port_debug_autoplayback(void);               /* the game's autoPlayback: 0 off, 1 record, 2 play back */
int port_debug_timer_mode(void);                 /* the game's timerMode: 0 off, 1 and 2 show the timing bars */
int port_debug_inertia(void);                    /* the game's inertia flag */
void port_debug_timer_scale(int scale);          /* how many times longer the timing bars show the time */
/* The folder for recordings and other files that the debug code writes: the save folder, with a separator at the end. */
void port_record_folder(char *out, int size);
extern char port_debug_folder[900];               /* the same folder, for the game's debug files (PCcreat) */

/* Only with the real sound player (mus-glue.c): when set, it gets each block of stereo frames that the sound player
   makes on the game's clock (without a sound device). The library sets it (src/lib/audio.c). */
extern void (*port_audio_sink)(const short *frames, int count);

void port_game_trace(char *buf, int size);
/* The positions of the glove, the ball and the camera: three floats each. */
void port_game_bodies(float out[9]);
void port_game_trace_update(char *buf, int size);

#endif
