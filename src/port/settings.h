/* The player's settings: the frame-rate limit, vsync and the keyboard mapping. They are kept in settings.ini in the
   save folder. This file is plain C and is part of every build, so the platform layer can use it without the menu. */
#ifndef PORT_SETTINGS_H
#define PORT_SETTINGS_H

/* The N64 inputs that the keyboard can press. */
enum {
    IN_A, IN_B, IN_Z, IN_START, IN_L, IN_R,
    IN_CUP, IN_CDOWN, IN_CLEFT, IN_CRIGHT,
    IN_DUP, IN_DDOWN, IN_DLEFT, IN_DRIGHT,
    IN_STICK_UP, IN_STICK_DOWN, IN_STICK_LEFT, IN_STICK_RIGHT,
    IN_COUNT
};

/* A controller binding is 0 (none), 1 + an SDL_GameControllerButton, or PAD_AXIS_BASE + 2 * an
   SDL_GameControllerAxis + 1 for the positive direction of the axis (0 for the negative one). */
#define PAD_AXIS_BASE 100
/* A raw joystick binding (a joystick that SDL has no game controller mapping for) is 0 (none), 1 + a button number,
   JOY_AXIS_BASE + 2 * an axis number + 1 for the positive direction (0 for the negative one), or JOY_HAT_BASE + 4 * a
   hat number + a direction (JOY_HAT_UP, JOY_HAT_DOWN, JOY_HAT_LEFT, JOY_HAT_RIGHT). */
#define JOY_BUTTON_MAX 64
#define JOY_AXIS_BASE 100
#define JOY_AXIS_MAX 16
#define JOY_HAT_BASE 200
#define JOY_HAT_MAX 8
enum { JOY_HAT_UP, JOY_HAT_DOWN, JOY_HAT_LEFT, JOY_HAT_RIGHT };
#define STICK_DEAD_DEFAULT 12
#define STICK_RANGE_DEFAULT 90
/* The master volume when settings.ini has none, in percent of the game's own level. */
#define VOLUME_DEFAULT 20

/* The picture options of the Graphics window (see gfx.h): the resolution scale in percent, the anti-aliasing samples
   (0 for none), the texture filter and the widescreen mode. Their numbers match GFX_FILTER_ and GFX_WIDE_. */
#define RENDER_SCALE_MIN 50
#define RENDER_SCALE_MAX 400
#define RENDER_SCALE_DEFAULT 100
#define MSAA_DEFAULT 4
enum { TEXFILTER_N64, TEXFILTER_BILINEAR, TEXFILTER_NEAREST };
enum { WIDESCREEN_OFF, WIDESCREEN_STRETCH, WIDESCREEN_HORPLUS };

#define KEY_SLOTS 2          /* each input has two keys */
#define FPS_LIMIT_MIN 20
#define FPS_LIMIT_MAX 240
/* The range of the debug overlays around the glove, in game units, and the keys of the frame step. */
#define DEBUG_RANGE_MIN 100
#define DEBUG_RANGE_MAX 2000
#define DEBUG_RANGE_DEFAULT 500
#define STEP_KEY_DEFAULT 67  /* SDL_SCANCODE_F10 */
#define PAUSE_KEY_DEFAULT 66 /* SDL_SCANCODE_F9 */

/* What fills the game's debug pad, pad[4] (Developer window, "Game debug"). Nothing is saved: the pad is off at
   every start. */
enum { DEBUGPAD_OFF, DEBUGPAD_SECOND, DEBUGPAD_KEYBOARD };
#define TIMER_SCALE_DEFAULT 16

/* How the game's logic runs. Smooth: one update per drawn frame, for the time that has passed (delta-time.md).
   Original: one whole update per original frame, and the drawn frames blend two of them (interpolation.md).
   Fixed: one whole update per original frame, and each drawn frame shows the latest one, as the N64 does (--fixed). */
enum { PHYSICS_SMOOTH, PHYSICS_ORIGINAL, PHYSICS_FIXED };

typedef struct Settings {
    int physicsMode;                   /* PHYSICS_SMOOTH or PHYSICS_ORIGINAL */
    int vsync;
    int maxFps;                        /* frames a second to draw, FPS_LIMIT_MIN to FPS_LIMIT_MAX (0: no limit) */
    int keys[IN_COUNT][KEY_SLOTS];     /* SDL scancodes (0: not bound) */
    int pads[IN_COUNT][KEY_SLOTS];     /* game controller bindings, see PAD_AXIS_BASE (0: not bound) */
    int joys[IN_COUNT][KEY_SLOTS];     /* raw joystick bindings, see JOY_AXIS_BASE (0: not bound) */
    int stickDead;                     /* dead zone of the controller stick, percent of its travel */
    int stickRange;                    /* travel at which the stick counts as full tilt, percent */
    int volume;                        /* master volume, percent of the game's own level */
    float cameraFollow;                /* how much faster than the original the camera follows the glove, 1.0 to 3.0 */
    int ballworld;                     /* Ballworld mode, taken up at the next level start (the saves at the next start) */
    int easyBall;                      /* the easy difficulty's stick direction on the ball at every difficulty */
    int fpsCap;                        /* not saved: the drawn rate that "Unlimited" is held to now (0: a limit is set) */
    int logFile;                       /* write weemitt.log in the save folder */
    int logVerbose;                    /* also log the frame rate, the game mode and the level once a second */
    int recordInput;                   /* record the input from the next start (record.h) */
    int musicVolume;                   /* volume of the songs, percent (default 100) */
    int effectsVolume;                 /* volume of the sound effects, percent (default 100) */
    int muteUnfocused;                 /* silence the sound while the window is not the active one */
    int skipIntro;                     /* skip the start-up screens up to the title menu */
    int infiniteLives;                 /* hold the game's INFINITE LIVES cheat on */
    int infiniteHealth;                /* hold the game's INFINITE ENERGY cheat on */
    int unlockAll;                     /* hold the game's OPEN PORTALS and OPEN LEVELS cheats on */
    int levelSelect;                   /* hold the game's LEVEL SELECT cheat on */
    int pauseCheats;                   /* R or Z in the pause menu opens the cheat list (default on) */
    /* The Developer window's debug tools (docs/menu.md). Only the two keys and the range are saved. */
    int pauseLogic;                    /* not saved: the game logic is paused */
    int stepRequests;                  /* not saved: single steps asked for, which the window loop answers while paused */
    int showCollision;                 /* not saved: draw the collision faces and spheres near the glove */
    int collisionDepth;                /* not saved: hide collision lines that lie behind the picture's surfaces */
    int showNames;                     /* not saved: label the actors near the glove */
    int debugRange;                    /* how far from the glove the overlays reach, in game units */
    int collisionTypes;                /* the kinds of collision lines to draw: PORT_COLL_ bits (frame.h) */
    int stepKey;                       /* SDL scancode of the key that runs one frame while paused */
    int pauseKey;                      /* SDL scancode of the key that pauses and resumes the logic */
    int renderScale;                   /* the 3D picture's size in percent of the window's, RENDER_SCALE_MIN to _MAX */
    int msaa;                          /* anti-aliasing samples: 0, 2, 4 or 8 */
    int texFilter;                     /* TEXFILTER_N64, TEXFILTER_BILINEAR or TEXFILTER_NEAREST */
    int widescreen;                    /* WIDESCREEN_OFF, WIDESCREEN_STRETCH or WIDESCREEN_HORPLUS */
    int debugPad;                      /* not saved: DEBUGPAD_OFF, DEBUGPAD_SECOND or DEBUGPAD_KEYBOARD */
    int timerScale;                    /* not saved: how many times longer the game's timing bars show the time */
    int fullscreen;                    /* the window fills the screen (F11, Alt+Enter, Graphics) */
    int windowX, windowY;              /* the window's place when it is not full screen (windowW 0: centred) */
    int windowW, windowH;              /* the window's size when it is not full screen (0: the default size) */
    int pauseMenu;                     /* the game waits while the menu is open (default off) */
    int pauseUnfocused;                /* the game waits while the window is not the active one (default off) */
    /* The studio's windows that show (Studio menu). The normal program keeps them in the file too. */
    int studioSearch;                  /* Points and search (default on) */
    int studioInput;                   /* the input display (default on) */
    int studioLevel;                   /* Start a level */
    int studioStates;                  /* Save and load states */
} Settings;

#ifdef __cplusplus
extern "C" {
#endif

/* The keyboard key of an input on the debug pad (DEBUGPAD_KEYBOARD) and its name. Inputs from IN_A to IN_STICK_RIGHT
   all have one. The keys are fixed. */
int settings_debug_key(int input);
/* The name of an SDL scancode for the menu. */
const char *settings_scancode_name(int scancode);

/* The name of an input as the menu and the file show it. */
const char *settings_input_name(int input);
void settings_defaults(Settings *st);
/* Puts the original key mapping back and leaves the other settings. */
void settings_default_keys(Settings *st);
/* Puts the original controller mapping back and leaves the other settings. */
void settings_default_pads(Settings *st);
/* Puts the raw joystick layout back (the raphnet N64 adapter's usual order) and leaves the other settings. */
void settings_default_joys(Settings *st);
/* The name of a raw joystick binding for the menu ("(none)" for 0), such as "Button 3", "Axis 2+" or "Hat 0 Up". */
const char *settings_joy_name(int code);
/* The binding for a joystick button, an axis moved to value v (0 when it has not moved far enough), or a hat
   moved to the SDL hat value (0 when it is centred or diagonal). */
int settings_joy_code_from_button(int button);
int settings_joy_code_from_axis(int axis, int v);
int settings_joy_code_from_hat(int hat, int value);
/* True when any raw joystick binding of the input is held; joy is an open SDL_Joystick. */
int settings_joy_down(const Settings *st, void *joy, int input);
/* The name of a controller binding for the menu ("(none)" for 0). */
const char *settings_pad_name(int code);
/* The binding for a controller axis moved to value v, or 0 when it has not moved far enough. */
int settings_pad_code_from_axis(int axis, int v);
/* True when any controller binding of the input is held; pad is an open SDL game controller. */
int settings_pad_down(const Settings *st, void *pad, int input);
/* Turns the raw left stick (SDL values, y down) into the N64 stick (about -80 to 80, y up). */
void settings_stick(const Settings *st, int rawX, int rawY, int *outX, int *outY);
/* Reads path into st. A missing file leaves the defaults. */
void settings_load(Settings *st, const char *path);
/* Writes st to path. Returns 0 when it worked. After settings_keep_file, a value that is as it was at the start of
   the run is written as the file had it: the command line's one-run options do not reach the file. */
int settings_save(const Settings *st, const char *path);
/* Keeps file (the settings as settings.ini gave them) and start (the same after the command line's options) for
   settings_save. */
void settings_keep_file(const Settings *file, const Settings *start);
/* Puts the path of settings.ini in the save folder into out. With no save folder, it is settings.ini in the
   working folder. */
void settings_path(char *out, int size);
/* The save folder, with a separator at its end: SDL's preference path. With none (SDL cannot make it), the
   working folder: an empty string. Returns 0 when it is the preference path. */
int settings_folder(char *out, int size);
/* True when any key bound to the input is down; keys is SDL's keyboard state. */
int settings_input_down(const Settings *st, const unsigned char *keys, int input);

#ifdef __cplusplus
}
#endif

#endif
