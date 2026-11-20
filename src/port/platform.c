/* The platform layer: the program's entry point, the window and OpenGL context (SDL2), the controller, the save
   file, the frame pacing, and a headless mode for tests. It sees the game only through frame.h. */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "frame.h"
#include "ptr32.h"
#include "log.h"
#include "gfx.h"
#include "gfx_host.h"
#include "inputevent.h"
#include "menu.h"
#include "record.h"
#include "rom.h"
#include "statehash.h"
#include "hostworld.h"
#include "hostscene.h"
#ifdef PORT_STUDIO
#include "studio.h"
#endif
#include "settings.h"

/* The N64 stick reads about +-80 at full tilt. */
#define STICK_MAX 80

typedef struct Options {
    const char *rom;
    long headlessFrames;
    int fullscreen;
    int vsync;               /* set by --no-vsync (-1: not given) */
    const char *savePath;
    long shotFrames[64];     /* headless: draw these frames and save them as PNG files */
    int shotCount;
    const char *shotPrefix;  /* headless: the screenshots' file names start with this */
    int shotWidth;
    int shotHeight;
    const char *inputScript; /* headless: "frame:buttons:x:y" lines */
    int hashRam;             /* headless: print a hash of the game's memory every hashRam frames (0: never) */
    long level;              /* start this level directly, as the level select does (-1: the normal start) */
    int trace;               /* headless: print the game's state after each frame (port_game_trace) */
    int fixedRate;           /* run the game at the N64's own rate (20 or 30 frames a second) */
    double dt;               /* headless: the length of each update in original frames (default 1) */
    double dtJitter;         /* headless: each update's length varies by up to this fraction of dt (0: none) */
    int pace;                /* headless: with --dt-jitter, a long drawn frame is split into updates as the window does */
    int traceUpdates;        /* headless: print the glove and ball state after every update (port_game_trace_update) */
    int maxFps;              /* windowed: the most frames a second to draw (0: no limit; -1: not given) */
    long dumpFrame;          /* headless: after this frame, write the game's memory to dumpPath */
    const char *dumpPath;
    int noSound;             /* open no sound device */
    const char *logPath;     /* --log: write the log to this file */
    float cameraFollow;      /* --camera-follow: how much faster the camera follows the glove (0: not given) */
    int ballworld;           /* --ballworld: play every level as the ball alone */
    int easyBall;            /* --easy-ball: the easy difficulty's stick direction on the ball */
    int skipIntro;           /* --skip-intro */
    int infiniteLives;       /* --infinite-lives */
    int infiniteHealth;      /* --infinite-health */
    int unlockAll;           /* --unlock-all */
    int levelSelect;         /* --level-select */
    const char *debugInput;  /* --debug-input: a script for the debug pad, in the format of --input */
    long debugActionFrame[16]; /* --debug-action FRAME NAME: at this original frame, start the debug action */
    int debugAction[16];
    int debugActionDone[16];
    int debugActionCount;
    long gotoFrame;          /* headless --goto-level FRAME LEVEL: at this original frame, start gotoLevel (-1: never) */
    long gotoLevel;
    int physics;             /* --original-physics (PHYSICS_ORIGINAL) or --smooth-physics (PHYSICS_SMOOTH); -1: not given */
    int interpCheck;         /* headless: for every frame, compare the picture at t = 0 with the previous frame's picture */
    long interpShot;         /* headless: at this frame, save screenshots a quarter, half and all of the way between two frames */
    const char *hostScene;   /* headless: --host-scene FILE, a host world to run the glove and the ball in (hostscene.c) */
    int spaceDemo;           /* the Space demo (spacedemo.c): start in Space 1 and join Space 1, 2 and 3 */
    const char *recordPath;  /* --record: write the input recording (record.h) to this file */
    const char *replayPath;  /* --replay: run a recording, headless */
    int renderScale;         /* --render-scale: the Graphics window's resolution scale in percent (0: not given) */
    int msaa;                /* --msaa: anti-aliasing samples (-1: not given) */
    int texFilter;           /* --texture-filter: TEXFILTER_ (-1: not given) */
    int widescreen;          /* --widescreen: WIDESCREEN_ (-1: not given) */
    int shotWindow;          /* headless: --shot-window saves the whole window, not only the renderer's frame */
    const char *audioOut;    /* headless, with the real sound player: --audio-out writes its frames to this file */
#ifdef PORT_STUDIO
    const char *loadState;   /* --load-state: start from this state file */
    long saveStateFrame[8];  /* headless --save-state FRAME FILE: after this original frame, write a state file */
    const char *saveStatePath[8];
    int saveStateCount;
#endif
} Options;

static Options opt;

/* Gives the renderer the picture options of the settings, with the command line's choices over them. */
static void apply_gfx_options(const Settings *st) {
    GfxOptions g;

    g.scale = opt.renderScale > 0 ? opt.renderScale : st->renderScale;
    g.msaa = opt.msaa >= 0 ? opt.msaa : st->msaa;
    g.filter = opt.texFilter >= 0 ? opt.texFilter : st->texFilter;
    g.widescreen = opt.widescreen >= 0 ? opt.widescreen : st->widescreen;
    gfx_set_options(&g);
}

static Uint64 timeBase;
static unsigned long long fakeTimeUs;
static int useFakeTime;

unsigned long long port_time_us(void) {
#ifdef PORT_STUDIO
    /* The game's clock feeds only its profiler timers. A fixed time keeps the game's memory the same in every run,
       which the studio's savestates and the brute forcer's hashes need (bruteforce/engine/host.c does the same). */
    return 0;
#endif
    if (useFakeTime) {
        return fakeTimeUs;
    }
    return (SDL_GetPerformanceCounter() - timeBase) * 1000000ULL / SDL_GetPerformanceFrequency();
}

static void die(const char *fmt, const char *arg) {
    char msg[512];

    snprintf(msg, sizeof(msg), fmt, arg);
    plog_printf("weemitt: %s", msg);
    if (opt.headlessFrames == 0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Glover", msg, NULL);
    }
    exit(1);
}

/* ---- The ROM. The game wants big-endian (.z64) byte order; .v64 swaps each pair of bytes and .n64 each word. */

static void load_rom(const char *path) {
    FILE *f = fopen(path, "rb");
    long size;
    unsigned char *rom;
    char err[256];

    if (f == NULL) {
        die("cannot open the ROM %s", path);
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0x100000 || size > 0x4000000) {
        die("%s is not an N64 ROM", path);
    }
    rom = malloc(size);
    if (fread(rom, 1, size, f) != (size_t)size) {
        die("cannot read the ROM %s", path);
    }
    fclose(f);
    if (port_rom_prepare(rom, (unsigned long)size, err, sizeof(err)) != 0) {
        char msg[1400];

        snprintf(msg, sizeof(msg), "%s %s", path, err);
        die("%s", msg);
    }
    port_rom = rom;
    port_rom_size = size;
}

/* Looks for the ROM next to the program and in the working directory when none is given. */
static const char *find_rom(void) {
    static const char *names[] = { "baserom.us.z64", "glover.z64", "Glover.z64", "Glover (USA).z64", "glover.n64",
                                   "glover.v64", NULL };
    static char buf[1024];
    char *base = SDL_GetBasePath();
    int i;
    FILE *f;

    for (i = 0; names[i]; i++) {
        if (base) {
            snprintf(buf, sizeof(buf), "%s%s", base, names[i]);
            if ((f = fopen(buf, "rb")) != NULL) {
                fclose(f);
                SDL_free(base);
                return buf;
            }
        }
        if ((f = fopen(names[i], "rb")) != NULL) {
            fclose(f);
            if (base) {
                SDL_free(base);
            }
            return names[i];
        }
    }
    if (base) {
        SDL_free(base);
    }
    return NULL;
}

/* ---- The save file: the EEPROM's 512 bytes. */

static char savePath[1024];

static void load_save(void) {
    FILE *f;

    port_save_ballworld = port_get_ballworld();
    if (opt.savePath) {
        snprintf(savePath, sizeof(savePath), "%s", opt.savePath);
    } else {
        char *pref = SDL_GetPrefPath("weemitt", "Glover");
        snprintf(savePath, sizeof(savePath), port_get_ballworld() ? "%seeprom-ballworld.bin" : "%seeprom.bin", pref ? pref : "");
        if (pref) {
            SDL_free(pref);
        }
    }
    f = fopen(savePath, "rb");
    if (f != NULL) {
        if (fread(port_eeprom, 1, sizeof(port_eeprom), f) != sizeof(port_eeprom)) {
            plog_printf("weemitt: the save file %s is short; the rest reads as empty", savePath);
        }
        fclose(f);
    }
}

static void write_save(void) {
    FILE *f;
    char tmp[1100];

    port_eeprom_dirty = 0;
    if (opt.headlessFrames || port_save_locked) {
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", savePath);
    f = fopen(tmp, "wb");
    if (f == NULL) {
        plog_printf("weemitt: cannot write the save file %s", tmp);
        return;
    }
    fwrite(port_eeprom, 1, sizeof(port_eeprom), f);
    fclose(f);
    remove(savePath);
    if (rename(tmp, savePath) != 0) {
        plog_printf("weemitt: cannot replace the save file %s", savePath);
    }
}

/* The folder for the files that the debug code writes (recordings). It is the save folder, or the folder of the file
   that --save names. The path ends with a separator. */
void port_record_folder(char *out, int size) {
    if (opt.savePath) {
        const char *slash = strrchr(opt.savePath, '/');
        const char *back = strrchr(opt.savePath, '\\');
        int n;

        if (back != NULL && (slash == NULL || back > slash)) {
            slash = back;
        }
        n = slash != NULL ? (int)(slash - opt.savePath) + 1 : 0;
        snprintf(out, size, "%.*s", n, opt.savePath);
    } else {
        char *pref = SDL_GetPrefPath("weemitt", "Glover");

        snprintf(out, size, "%s", pref ? pref : "");
        if (pref) {
            SDL_free(pref);
        }
    }
}

/* ---- Input: the keyboard, and the first game controller or, failing that, the first plain joystick. */

static SDL_GameController *pad;
static SDL_Joystick *joy;       /* a joystick that SDL has no game controller mapping for; never open with pad */
static char deviceName[128];    /* the name of the device in use */

int port_input_device(char *name, int size) {
    snprintf(name, size, "%s", pad != NULL || joy != NULL ? deviceName : "");
    return pad != NULL ? PORT_DEVICE_CONTROLLER : joy != NULL ? PORT_DEVICE_JOYSTICK : PORT_DEVICE_NONE;
}

/* Writes what SDL knows about the joystick at the index to the log. */
static void log_device(int index, const char *what) {
    SDL_Joystick *j = SDL_JoystickOpen(index);
    char guid[40];

    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof(guid));
    if (j != NULL) {
        plog_printf("weemitt: %s \"%s\", GUID %s, %d axes, %d buttons, %d hats, %s", what, SDL_JoystickName(j), guid,
                    SDL_JoystickNumAxes(j), SDL_JoystickNumButtons(j), SDL_JoystickNumHats(j),
                    SDL_IsGameController(index) ? "game controller" : "joystick (no SDL mapping)");
        SDL_JoystickClose(j);
    } else {
        plog_printf("weemitt: %s \"%s\", GUID %s, cannot open it: %s", what, SDL_JoystickNameForIndex(index), guid,
                    SDL_GetError());
    }
}

static void open_pad(void) {
    int i;

    for (i = 0; i < SDL_NumJoysticks() && pad == NULL; i++) {
        if (SDL_IsGameController(i)) {
            pad = SDL_GameControllerOpen(i);
            if (pad != NULL) {
                snprintf(deviceName, sizeof(deviceName), "%s", SDL_GameControllerName(pad));
                plog_printf("weemitt: using \"%s\" as a game controller", deviceName);
                if (joy != NULL) {
                    SDL_JoystickClose(joy);
                    joy = NULL;
                }
            }
        }
    }
    for (i = 0; i < SDL_NumJoysticks() && pad == NULL && joy == NULL; i++) {
        if (!SDL_IsGameController(i)) {
            joy = SDL_JoystickOpen(i);
            if (joy != NULL) {
                snprintf(deviceName, sizeof(deviceName), "%s", SDL_JoystickName(joy));
                plog_printf("weemitt: using \"%s\" as a joystick (%d axes, %d buttons, %d hats)", deviceName,
                            SDL_JoystickNumAxes(joy), SDL_JoystickNumButtons(joy), SDL_JoystickNumHats(joy));
            }
        }
    }
}

/* Loads extra game controller mappings from gamecontrollerdb.txt next to the program and in the save folder. */
static void load_pad_mappings(void) {
    char path[1100];
    char *dirs[2];
    int i;

    dirs[0] = SDL_GetBasePath();
    dirs[1] = SDL_GetPrefPath("weemitt", "Glover");
    for (i = 0; i < 2; i++) {
        FILE *f;

        if (dirs[i] == NULL) {
            continue;
        }
        snprintf(path, sizeof(path), "%sgamecontrollerdb.txt", dirs[i]);
        if ((f = fopen(path, "r")) != NULL) {
            int n;

            fclose(f);
            n = SDL_GameControllerAddMappingsFromFile(path);
            if (n < 0) {
                plog_printf("weemitt: cannot read the controller mappings in %s: %s", path, SDL_GetError());
            } else {
                plog_printf("weemitt: added %d controller mappings from %s", n, path);
            }
        }
        SDL_free(dirs[i]);
    }
}

static void read_input(const Settings *st) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    unsigned short b = 0;
    int x = 0;
    int y = 0;

    if (!menu_is_open()) {
        if (settings_input_down(st, k, IN_STICK_LEFT)) x -= STICK_MAX;
        if (settings_input_down(st, k, IN_STICK_RIGHT)) x += STICK_MAX;
        if (settings_input_down(st, k, IN_STICK_UP)) y += STICK_MAX;
        if (settings_input_down(st, k, IN_STICK_DOWN)) y -= STICK_MAX;
        if (settings_input_down(st, k, IN_A)) b |= BTN_A;
        if (settings_input_down(st, k, IN_B)) b |= BTN_B;
        if (settings_input_down(st, k, IN_Z)) b |= BTN_Z;
        if (settings_input_down(st, k, IN_START)) b |= BTN_START;
        if (settings_input_down(st, k, IN_L)) b |= BTN_L;
        if (settings_input_down(st, k, IN_R)) b |= BTN_R;
        if (settings_input_down(st, k, IN_CUP)) b |= BTN_CUP;
        if (settings_input_down(st, k, IN_CDOWN)) b |= BTN_CDOWN;
        if (settings_input_down(st, k, IN_CLEFT)) b |= BTN_CLEFT;
        if (settings_input_down(st, k, IN_CRIGHT)) b |= BTN_CRIGHT;
        if (settings_input_down(st, k, IN_DUP)) b |= BTN_DUP;
        if (settings_input_down(st, k, IN_DDOWN)) b |= BTN_DDOWN;
        if (settings_input_down(st, k, IN_DLEFT)) b |= BTN_DLEFT;
        if (settings_input_down(st, k, IN_DRIGHT)) b |= BTN_DRIGHT;
    }
    if (pad != NULL || joy != NULL) {
        int (*down)(const Settings *, void *, int) = pad != NULL ? settings_pad_down : settings_joy_down;
        void *dev = pad != NULL ? (void *)pad : (void *)joy;
        int px, py;

        if (pad != NULL) {
            settings_stick(st, SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX),
                           SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY), &px, &py);
        } else {
            settings_stick(st, SDL_JoystickGetAxis(joy, 0), SDL_JoystickGetAxis(joy, 1), &px, &py);
        }
        py = -py;
        if (px || py) {
            x = px;
            y = py;
        }
        if (down(st, dev, IN_STICK_LEFT)) x = -STICK_MAX;
        if (down(st, dev, IN_STICK_RIGHT)) x = STICK_MAX;
        if (down(st, dev, IN_STICK_UP)) y = STICK_MAX;
        if (down(st, dev, IN_STICK_DOWN)) y = -STICK_MAX;
        if (down(st, dev, IN_A)) b |= BTN_A;
        if (down(st, dev, IN_B)) b |= BTN_B;
        if (down(st, dev, IN_Z)) b |= BTN_Z;
        if (down(st, dev, IN_START)) b |= BTN_START;
        if (down(st, dev, IN_L)) b |= BTN_L;
        if (down(st, dev, IN_R)) b |= BTN_R;
        if (down(st, dev, IN_CUP)) b |= BTN_CUP;
        if (down(st, dev, IN_CDOWN)) b |= BTN_CDOWN;
        if (down(st, dev, IN_CLEFT)) b |= BTN_CLEFT;
        if (down(st, dev, IN_CRIGHT)) b |= BTN_CRIGHT;
        if (down(st, dev, IN_DUP)) b |= BTN_DUP;
        if (down(st, dev, IN_DDOWN)) b |= BTN_DDOWN;
        if (down(st, dev, IN_DLEFT)) b |= BTN_DLEFT;
        if (down(st, dev, IN_DRIGHT)) b |= BTN_DRIGHT;
    }
    port_buttons = b;
    port_stick_x = (signed char)x;
    port_stick_y = (signed char)y;
}

/* ---- The debug pad (Developer window, "Game debug"): the keyboard's debug keys, or a second device. */

static SDL_GameController *pad2;
static SDL_Joystick *joy2;
static char deviceName2[128];
static int pad2Dirty;           /* a device came or went: look for the second one again */
static int debugScriptOn;       /* a --debug-input script drives the pad */

static int is_first_device(int index) {
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);

    return (pad != NULL && id == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)))
           || (joy != NULL && id == SDL_JoystickInstanceID(joy));
}

static void close_pad2(void) {
    if (pad2 != NULL) {
        SDL_GameControllerClose(pad2);
        pad2 = NULL;
    }
    if (joy2 != NULL) {
        SDL_JoystickClose(joy2);
        joy2 = NULL;
    }
}

/* Opens the first device that the player's controller does not use. With no controller in use, the first device
   is kept for it. */
static void open_pad2(void) {
    int i;

    close_pad2();
    for (i = 0; i < SDL_NumJoysticks() && pad2 == NULL && joy2 == NULL; i++) {
        if (is_first_device(i) || (pad == NULL && joy == NULL && i == 0)) {
            continue;
        }
        if (SDL_IsGameController(i)) {
            pad2 = SDL_GameControllerOpen(i);
            if (pad2 != NULL) {
                snprintf(deviceName2, sizeof(deviceName2), "%s", SDL_GameControllerName(pad2));
            }
        } else {
            joy2 = SDL_JoystickOpen(i);
            if (joy2 != NULL) {
                snprintf(deviceName2, sizeof(deviceName2), "%s", SDL_JoystickName(joy2));
            }
        }
        if (pad2 != NULL || joy2 != NULL) {
            plog_printf("weemitt: the debug pad uses \"%s\" as a %s", deviceName2, pad2 ? "game controller" : "joystick");
        }
    }
}

int port_debug_device(char *name, int size) {
    snprintf(name, size, "%s", pad2 != NULL || joy2 != NULL ? deviceName2 : "");
    return pad2 != NULL ? PORT_DEVICE_CONTROLLER : joy2 != NULL ? PORT_DEVICE_JOYSTICK : PORT_DEVICE_NONE;
}

/* Sets what the debug pad reads for the next updates: the settings' choice, with the same bindings as controller 1
   for a second device. */
static void read_debug_input(const Settings *st) {
    static int lastMode = -1;
    unsigned short b = 0;
    int x = 0;
    int y = 0;
    int in;

    if (st->debugPad != lastMode || (pad2Dirty && st->debugPad == DEBUGPAD_SECOND)) {
        pad2Dirty = 0;
        if (st->debugPad == DEBUGPAD_SECOND) {
            open_pad2();
        } else {
            close_pad2();
        }
        if (st->debugPad != DEBUGPAD_OFF || lastMode > 0) {
            plog_printf("weemitt: the debug pad is %s", st->debugPad == DEBUGPAD_SECOND ? "the second controller"
                        : st->debugPad == DEBUGPAD_KEYBOARD ? "on the debug keys" : "off");
        }
        lastMode = st->debugPad;
    }
    if (st->debugPad == DEBUGPAD_KEYBOARD && !menu_is_open()) {
        const Uint8 *k = SDL_GetKeyboardState(NULL);

        for (in = IN_A; in <= IN_DRIGHT; in++) {
            int sc = settings_debug_key(in);

            if (sc > 0 && sc < SDL_NUM_SCANCODES && k[sc]) {
                b |= input_button(in);
            }
        }
        if (k[settings_debug_key(IN_STICK_LEFT)]) x -= STICK_MAX;
        if (k[settings_debug_key(IN_STICK_RIGHT)]) x += STICK_MAX;
        if (k[settings_debug_key(IN_STICK_UP)]) y += STICK_MAX;
        if (k[settings_debug_key(IN_STICK_DOWN)]) y -= STICK_MAX;
    } else if (st->debugPad == DEBUGPAD_SECOND && (pad2 != NULL || joy2 != NULL)) {
        int (*down)(const Settings *, void *, int) = pad2 != NULL ? settings_pad_down : settings_joy_down;
        void *dev = pad2 != NULL ? (void *)pad2 : (void *)joy2;
        int px, py;

        if (pad2 != NULL) {
            settings_stick(st, SDL_GameControllerGetAxis(pad2, SDL_CONTROLLER_AXIS_LEFTX),
                           SDL_GameControllerGetAxis(pad2, SDL_CONTROLLER_AXIS_LEFTY), &px, &py);
        } else {
            settings_stick(st, SDL_JoystickGetAxis(joy2, 0), SDL_JoystickGetAxis(joy2, 1), &px, &py);
        }
        py = -py;
        if (px || py) {
            x = px;
            y = py;
        }
        if (down(st, dev, IN_STICK_LEFT)) x = -STICK_MAX;
        if (down(st, dev, IN_STICK_RIGHT)) x = STICK_MAX;
        if (down(st, dev, IN_STICK_UP)) y = STICK_MAX;
        if (down(st, dev, IN_STICK_DOWN)) y = -STICK_MAX;
        for (in = IN_A; in <= IN_DRIGHT; in++) {
            if (down(st, dev, in)) {
                b |= input_button(in);
            }
        }
    }
    port_debug_buttons = b;
    port_debug_stick_x = (signed char)x;
    port_debug_stick_y = (signed char)y;
    port_debug_pad_mode(st->debugPad != DEBUGPAD_OFF || debugScriptOn);
    port_debug_timer_scale(st->timerScale);
}

/* ---- Sound. With the real sound player (make AUDIO=mus), libmus calls these functions (src/audio/mus/mus-glue.c
   describes them). The game's s32 and s16 are int and short. */

#ifdef PORT_AUDIO_MUS
/* libmus's lock: a recursive mutex (SDL's mutexes are recursive). libmus takes it before it opens the stream. */
static SDL_mutex *audioLock;
static SDL_AudioDeviceID audioDevice;
static void (*audioRender)(short *out, int frames);
/* The master volume in percent (Settings, Audio). The window loop sets it; the sound thread reads it. */
static volatile int audioVolume = 100;
/* Declared in src/audio/mus/mus_internal.h: the volume of the songs and of the sound effects, in percent. */
void musSetGroupGain(int song, int effect);

void port_audio_lock(void) {
    if (audioLock == NULL) {
        audioLock = SDL_CreateMutex();
        if (audioLock == NULL) {
            die("cannot create the sound lock: %s", SDL_GetError());
        }
    }
    SDL_LockMutex(audioLock);
}

void port_audio_unlock(void) {
    SDL_UnlockMutex(audioLock);
}

void port_audio_yield(void) {
    SDL_Delay(1);
}

/* SDL's audio thread calls this with the device's lock held. musAudioRender then waits for libmus's lock. */
static void SDLCALL audio_callback(void *userdata, Uint8 *stream, int len) {
    short *out = (short *)stream;
    int volume = audioVolume;
    int i;

    (void)userdata;
    audioRender(out, len / 4);
    if (volume < 100) {
        for (i = 0; i < len / 2; i++) {
            out[i] = (short)(out[i] * volume / 100);
        }
    }
}

/* Opens a stereo stream of 16-bit samples at rate Hz. SDL converts it to the device's rate. libmus calls this
   while it holds its lock. The device starts playing here, and its first callback waits for the lock. After this
   the platform must not call SDL's audio functions while it holds the lock: the callback holds the device's lock
   while it waits for libmus's. Returns 0 in the headless mode, with --no-sound or when no device opens. libmus
   then runs on the game's clock and nothing is heard. */
int port_audio_open(int rate, void (*render)(short *out, int frames)) {
    SDL_AudioSpec want;
    SDL_AudioSpec have;

    if (opt.headlessFrames || opt.noSound) {
        return 0;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        plog_printf("weemitt: no sound: %s", SDL_GetError());
        return 0;
    }
    SDL_zero(want);
    want.freq = (int)rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_callback;
    audioRender = render;
    audioDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audioDevice == 0) {
        plog_printf("weemitt: no sound: %s", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return 0;
    }
    SDL_PauseAudioDevice(audioDevice, 0);
    return 1;
}
#endif

/* ---- Headless mode: runs the game without a window, as fast as it can, for tests. */

typedef struct ScriptedInput {
    long frame;
    unsigned short buttons;
    int x;
    int y;
} ScriptedInput;

static ScriptedInput *script;
static int scriptCount;
static ScriptedInput *debugScript;
static int debugScriptCount;

static void load_input_script(const char *path, ScriptedInput **script, int *scriptCount) {
    FILE *f = fopen(path, "r");
    char line[256];
    int cap = 0;

    if (f == NULL) {
        die("cannot open the input script %s", path);
    }
    while (fgets(line, sizeof(line), f)) {
        ScriptedInput s;
        unsigned int buttons;

        if (line[0] == '#' || sscanf(line, "%ld:%x:%d:%d", &s.frame, &buttons, &s.x, &s.y) != 4) {
            continue;
        }
        s.buttons = (unsigned short)buttons;
        if (*scriptCount == cap) {
            cap = cap ? cap * 2 : 64;
            *script = realloc(*script, cap * sizeof(**script));
        }
        (*script)[(*scriptCount)++] = s;
    }
    fclose(f);
}

/* Test options for the debug pad in the window. WEEMITT_DEBUG_PAD=second or keyboard sets the Developer window's
   choice. WEEMITT_VIRTUAL_PADS=N makes N virtual game controllers, and WEEMITT_VIRTUAL_PRESS=PAD:BUTTON:FROM:TO holds
   an SDL_CONTROLLER_BUTTON of virtual controller PAD (0 or 1) from original frame FROM up to TO. */
static SDL_Joystick *virtualPads[2];
static int virtualPress[4] = {-1, 0, 0, 0};

static void make_virtual_pads(void) {
    const char *n = getenv("WEEMITT_VIRTUAL_PADS");
    const char *press = getenv("WEEMITT_VIRTUAL_PRESS");
    int i;

    if (press != NULL) {
        sscanf(press, "%d:%d:%d:%d", &virtualPress[0], &virtualPress[1], &virtualPress[2], &virtualPress[3]);
    }
    for (i = 0; n != NULL && i < atoi(n) && i < 2; i++) {
        int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX,
                                              SDL_CONTROLLER_BUTTON_MAX, 0);

        if (index >= 0) {
            virtualPads[i] = SDL_JoystickOpen(index);
        }
    }
}

static void virtual_press(long frame) {
    if (virtualPress[0] >= 0 && virtualPress[0] < 2 && virtualPads[virtualPress[0]] != NULL) {
        SDL_JoystickSetVirtualButton(virtualPads[virtualPress[0]], virtualPress[1],
                                     frame >= virtualPress[2] && frame < virtualPress[3]);
    }
}

/* The names of the debug actions for --debug-action and WEEMITT_DEBUG_ACTIONS, in the order of PORT_DBG_. */
static const char *const debugActionNames[PORT_DBG_COUNT] = {
    "prev-platform", "next-platform", "mark-levels", "timing", "prev-song", "next-song", "inertia", "quit", "hurt",
    "autoplay",
};

static int debug_action_by_name(const char *name) {
    int i;

    for (i = 0; i < PORT_DBG_COUNT; i++) {
        if (!strcmp(name, debugActionNames[i])) {
            return i;
        }
    }
    return -1;
}

static void add_debug_action(long frame, const char *name) {
    int a = debug_action_by_name(name);

    if (a < 0 || opt.debugActionCount >= 16) {
        die("unknown or too many debug actions: %s", name);
    }
    opt.debugActionFrame[opt.debugActionCount] = frame;
    opt.debugAction[opt.debugActionCount++] = a;
}

/* WEEMITT_DEBUG_ACTIONS="FRAME:NAME,FRAME:NAME" for the window's tests. */
static void parse_debug_actions(const char *text) {
    char copy[512];
    char *tok;

    snprintf(copy, sizeof(copy), "%s", text);
    for (tok = strtok(copy, ","); tok != NULL; tok = strtok(NULL, ",")) {
        char *colon = strchr(tok, ':');

        if (colon == NULL) {
            die("bad debug action %s", tok);
        }
        *colon = 0;
        add_debug_action(atol(tok), colon + 1);
    }
}

/* The debug pad's test input at the original frame: the script's buttons, and the actions that are due. */
static void scripted_debug(long frame) {
    int i;

    if (debugScriptCount) {
        port_debug_buttons = 0;
        port_debug_stick_x = 0;
        port_debug_stick_y = 0;
        for (i = 0; i < debugScriptCount && debugScript[i].frame <= frame; i++) {
            port_debug_buttons = debugScript[i].buttons;
            port_debug_stick_x = (signed char)debugScript[i].x;
            port_debug_stick_y = (signed char)debugScript[i].y;
        }
    }
    for (i = 0; i < opt.debugActionCount; i++) {
        if (!opt.debugActionDone[i] && frame >= opt.debugActionFrame[i] && port_debug_action(opt.debugAction[i])) {
            opt.debugActionDone[i] = 1;
            plog_printf("weemitt: debug action %s at frame %ld", debugActionNames[opt.debugAction[i]], frame);
        }
    }
}

/* The input from the last script line at or before the frame. */
static void scripted_input(long frame) {
    int i;

    port_buttons = 0;
    port_stick_x = 0;
    port_stick_y = 0;
    for (i = 0; i < scriptCount && script[i].frame <= frame; i++) {
        port_buttons = script[i].buttons;
        port_stick_x = (signed char)script[i].x;
        port_stick_y = (signed char)script[i].y;
    }
}

extern unsigned char port_ram[];

/* ---- Recording and replay (record.h). The recorder writes one record for each call of port_game_update. */

/* A check value of the game's state is written after every REC_CHECK_EVERY updates. */
#define REC_CHECK_EVERY 30

/* A hash of the one-line trace of the game's state (port_game_trace). */
static unsigned trace_hash(void) {
    char line[256];

    line[0] = 0;
    port_game_trace(line, sizeof(line));
    return rec_hash_bytes(line, strlen(line));
}

/* Starts the recording: the header holds the settings of the run, a hash of the ROM and the save at the start. */
static void start_recording(const char *path, const Settings *st) {
    static RecInfo info;

    snprintf(info.buildId, sizeof(info.buildId), "%s", rec_build_id());
    info.spaceDemo = opt.spaceDemo;
    info.fixedRate = opt.fixedRate;
    info.level = opt.level;
    info.easyBall = st->easyBall;
    info.cameraFollow = st->cameraFollow;
    info.romHash = rec_hash_bytes(port_rom, port_rom_size);
    info.eepromHash = rec_hash_bytes(port_eeprom, sizeof(port_eeprom));
    memcpy(info.eeprom, port_eeprom, sizeof(port_eeprom));
    if (rec_start(path, &info) != 0) {
        plog_printf("weemitt: cannot write the recording %s", path);
        return;
    }
    plog_printf("weemitt: recording the input to %s (%s)", path, info.buildId);
}

/* Buttons whose press event came after the latest update and that the keyboard and controller state may not show
   any more (a tap shorter than a drawn frame). The next update has them held. */
static unsigned short tapPending;

/* One update of the game, written to the recording first. dt is what the game gets. */
static long run_update(float dt, PortFrameTask *tasks, int max) {
    unsigned long reads = port_controller_reads;
    unsigned short polled = port_buttons;
    long n;

    port_buttons |= tapPending;
    tapPending = 0;
    port_buttons |= port_intro_skip_buttons();
#ifdef PORT_STUDIO
    studio_before_update(&port_buttons, &port_stick_x, &port_stick_y);
#endif
    if (rec_active()) {
        rec_update(dt, (int)port_get_easy_ball(), port_get_camera_follow(), port_buttons, port_stick_x, port_stick_y);
    }
    n = port_game_update(dt, tasks, max);
#ifdef PORT_STUDIO
    studio_after_update(tasks, n);
#endif
    if (rec_active()) {
        if (port_controller_reads != reads) {
            rec_read(port_buttons_read);
        }
        if (rec_updates() % REC_CHECK_EVERY == 0) {
            rec_check(trace_hash());
        }
    }
    port_buttons = polled;
    return n;
}

/* Test option: WEEMITT_DUMP_END=FILE writes the game's memory to FILE when a recording or a replay ends. */
static void dump_end(void) {
    const char *path = getenv("WEEMITT_DUMP_END");
    FILE *f = path ? fopen(path, "wb") : NULL;

    if (f != NULL) {
        fwrite(port_ram, 1, 0x1800000, f);
        fclose(f);
    }
}

/* Closes the recording and logs the state it ended in. */
static void finish_recording(void) {
    char line[256];
    unsigned ram;

    if (!rec_active()) {
        return;
    }
    line[0] = 0;
    port_game_trace(line, sizeof(line));
    ram = port_hash_ram();
    plog_printf("weemitt: recording ended after %lu updates: %s memory hash %08x", rec_updates(), line, ram);
    rec_end(ram);
    dump_end();
}

static RecInfo replayInfo;
static unsigned long replayUpdates;
static unsigned long replayChecks;
static unsigned long replayBad;

static void replay_depart(const char *what) {
    if (replayBad++ == 0) {
        char line[256];

        line[0] = 0;
        port_game_trace(line, sizeof(line));
        plog_printf("weemitt: replay: the run departs from the recording at update %lu (%s). State: %s", replayUpdates,
                    what, line);
    }
}

/* Opens the recording and takes its settings before the game starts. The save of the recording is used unless
   --save names another one. */
static void replay_open(const Settings *st) {
    char err[256];

    (void)st;
    if (rec_open(opt.replayPath, &replayInfo, err, sizeof(err)) != 0) {
        die("%s", err);
    }
    plog_printf("weemitt: replaying %s, recorded by %s", opt.replayPath, replayInfo.buildId);
    /* The first word of the id is the source version. The system and the sound player follow it. */
    if (strcmp(replayInfo.buildId, rec_build_id()) != 0) {
        const char *mine = rec_build_id();
        size_t n = strcspn(replayInfo.buildId, " ");

        if (n != strcspn(mine, " ") || strncmp(replayInfo.buildId, mine, n) != 0) {
            plog_printf("weemitt: replay: this program is version %s and the recording is from version %s. The game "
                        "logic may differ; the checks in the file show where the run departs.", mine, replayInfo.buildId);
        } else if (strcmp(replayInfo.buildId + n, mine + strcspn(mine, " ")) != 0) {
            plog_printf("weemitt: replay: the recording is from the system and sound player \"%s\", this program is "
                        "\"%s\". The trace agrees at update lengths of exactly 1; shorter ones can differ in the last "
                        "bits between systems.", replayInfo.buildId + n + 1, mine + strcspn(mine, " ") + 1);
        }
    }
    if (replayInfo.romHash != rec_hash_bytes(port_rom, port_rom_size)) {
        plog_printf("weemitt: replay: this ROM is not the one the recording was made with");
    }
    if (opt.savePath != NULL) {
        if (rec_hash_bytes(port_eeprom, sizeof(port_eeprom)) != replayInfo.eepromHash) {
            plog_printf("weemitt: replay: the save file %s differs from the one the recording started with", opt.savePath);
        }
    } else {
        memcpy(port_eeprom, replayInfo.eeprom, sizeof(port_eeprom));
    }
    opt.level = replayInfo.level;
    opt.spaceDemo = replayInfo.spaceDemo;
    port_set_easy_ball(replayInfo.easyBall);
    port_set_camera_follow(replayInfo.cameraFollow);
}

/* Gives the next update of the recording: its length, with the controller set. Checks the records that follow an
   update when it reaches them. Returns 0 at the end of the recording. */
static int replay_next(float *dt) {
    RecEvent ev;

    for (;;) {
        rec_next(&ev);
        switch (ev.type) {
        case REC_EV_UPDATE:
            port_buttons = ev.buttons;
            port_stick_x = ev.x;
            port_stick_y = ev.y;
            *dt = ev.dt;
            replayUpdates++;
            return 1;
        case REC_EV_SETTINGS:
            port_set_easy_ball(ev.easyBall);
            port_set_camera_follow(ev.cameraFollow);
            break;
        case REC_EV_READ:
            if (ev.buttons != port_buttons_read) {
                replay_depart("the game read other buttons");
            }
            break;
        case REC_EV_CHECK:
            replayChecks++;
            if (ev.hash != trace_hash()) {
                replay_depart("the game state differs");
            }
            break;
        case REC_EV_FINISH: {
            unsigned ram = port_hash_ram();

            if (ev.updates != replayUpdates) {
                replay_depart("the update count differs");
            }
            dump_end();
            plog_printf("weemitt: replay: memory hash %08x, recorded %08x%s", ram, ev.hash,
                        ram == ev.hash ? " (same)" : " (differs; the game's profiler timers hold the clock)");
            plog_printf("weemitt: replay: %lu updates, %lu checks, %s", replayUpdates, replayChecks,
                        replayBad ? "DEPARTED from the recording" : "all checks equal");
            return 0;
        }
        default:
            plog_printf("weemitt: replay: the recording ends without an end record after %lu updates (the run was cut "
                        "short). %lu checks, %s",
                        replayUpdates, replayChecks, replayBad ? "DEPARTED from the recording" : "all checks equal");
            return 0;
        }
    }
}

/* ---- Screenshots in the headless mode: a hidden window with an OpenGL context, read back after drawing. */

static SDL_Window *shotWindow;
static SDL_GLContext shotContext;

static unsigned int crc_table[256];

static unsigned int png_crc(const unsigned char *p, size_t n, unsigned int c) {
    size_t i;

    if (crc_table[1] == 0) {
        unsigned int k;
        int j;

        for (k = 0; k < 256; k++) {
            unsigned int v = k;

            for (j = 0; j < 8; j++) {
                v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            }
            crc_table[k] = v;
        }
    }
    for (i = 0; i < n; i++) {
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    return c;
}

static void put32(FILE *f, unsigned int v) {
    fputc(v >> 24, f);
    fputc(v >> 16, f);
    fputc(v >> 8, f);
    fputc(v, f);
}

static void png_chunk(FILE *f, const char *type, const unsigned char *data, size_t n) {
    unsigned int c = png_crc((const unsigned char *)type, 4, 0xFFFFFFFFu);

    put32(f, (unsigned int)n);
    fwrite(type, 1, 4, f);
    if (n) {
        fwrite(data, 1, n, f);
    }
    c = png_crc(data, n, c);
    put32(f, c ^ 0xFFFFFFFFu);
}

/* Writes RGB rows (bottom row first, as OpenGL reads them) as a PNG with uncompressed deflate blocks. */
static void write_png(const char *path, int w, int h, const unsigned char *rgb) {
    size_t raw = (size_t)(w * 3 + 1) * h;
    size_t blocks = (raw + 65534) / 65535;
    unsigned char *z = malloc(2 + raw + blocks * 5 + 4);
    unsigned char *row = malloc(raw);
    unsigned char ihdr[13];
    unsigned int a = 1;
    unsigned int b = 0;
    size_t i;
    size_t pos = 0;
    size_t zn = 0;
    int y;
    FILE *f;

    for (y = 0; y < h; y++) {
        row[pos++] = 0;
        memcpy(row + pos, rgb + (size_t)(h - 1 - y) * w * 3, w * 3);
        pos += w * 3;
    }
    z[zn++] = 0x78;
    z[zn++] = 0x01;
    for (i = 0; i < raw; i += 65535) {
        size_t n = raw - i < 65535 ? raw - i : 65535;

        z[zn++] = i + n == raw;
        z[zn++] = n & 0xFF;
        z[zn++] = n >> 8;
        z[zn++] = ~n & 0xFF;
        z[zn++] = (~n >> 8) & 0xFF;
        memcpy(z + zn, row + i, n);
        zn += n;
    }
    for (i = 0; i < raw; i++) {
        a = (a + row[i]) % 65521;
        b = (b + a) % 65521;
    }
    z[zn++] = b >> 8;
    z[zn++] = b;
    z[zn++] = a >> 8;
    z[zn++] = a;
    f = fopen(path, "wb");
    if (f == NULL) {
        plog_printf("weemitt: cannot write %s", path);
    } else {
        fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
        ihdr[0] = w >> 24, ihdr[1] = w >> 16, ihdr[2] = w >> 8, ihdr[3] = w;
        ihdr[4] = h >> 24, ihdr[5] = h >> 16, ihdr[6] = h >> 8, ihdr[7] = h;
        ihdr[8] = 8, ihdr[9] = 2, ihdr[10] = 0, ihdr[11] = 0, ihdr[12] = 0;
        png_chunk(f, "IHDR", ihdr, 13);
        png_chunk(f, "IDAT", z, zn);
        png_chunk(f, "IEND", NULL, 0);
        fclose(f);
    }
    free(z);
    free(row);
}

static void open_shot_window(void) {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        die("SDL video could not start for screenshots: %s", SDL_GetError());
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    shotWindow = SDL_CreateWindow("Glover", 0, 0, opt.shotWidth, opt.shotHeight, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (shotWindow == NULL) {
        die("cannot open a window for screenshots: %s", SDL_GetError());
    }
    shotContext = SDL_GL_CreateContext(shotWindow);
    if (shotContext == NULL) {
        die("cannot create an OpenGL 3.3 context for screenshots: %s", SDL_GetError());
    }
    gfx_set_loader(SDL_GL_GetProcAddress);
    gfx_init();
    {
        Settings st;

        settings_defaults(&st);
        apply_gfx_options(&st);
    }
    gfx_set_drawable_size(opt.shotWidth, opt.shotHeight);
}

static const char *shotSuffix = "";

static void take_shot(const PortFrameTask *tasks, long count, long frame) {
    unsigned char *rgba;
    unsigned char *rgb;
    char path[1024];
    int w;
    int h;
    int i;

    gfx_draw_frame((const GfxTask *)tasks, (int)count, opt.shotWidth, opt.shotHeight);
    rgba = opt.shotWindow ? gfx_read_window(&w, &h) : gfx_read_frame(&w, &h);
    if (rgba == NULL) {
        plog_printf("weemitt: the renderer gave no frame for the screenshot of frame %ld", frame);
        return;
    }
    /* gfx_read_frame gives rows from the top; write_png takes OpenGL's order, the bottom row first. */
    rgb = malloc((size_t)w * h * 3);
    for (i = 0; i < w * h; i++) {
        int y = i / w;
        int x = i % w;
        unsigned char *dst = rgb + ((size_t)(h - 1 - y) * w + x) * 3;

        dst[0] = rgba[i * 4];
        dst[1] = rgba[i * 4 + 1];
        dst[2] = rgba[i * 4 + 2];
    }
    snprintf(path, sizeof(path), "%s%05ld%s.png", opt.shotPrefix ? opt.shotPrefix : "shot", frame, shotSuffix);
    write_png(path, w, h, rgb);
    SDL_GL_SwapWindow(shotWindow);
    free(rgb);
    free(rgba);
}

static unsigned int jitterSeed = 1;

/* --trace-updates: the glove and ball state after an update. */
static void print_update_trace(void) {
    char line[256];

    port_game_trace_update(line, sizeof(line));
    printf("update %s\n", line);
}

/* --trace and --hash-every: the state at the end of original frame number frame. Returns 1 when it printed. */
static int print_tick_trace(long frame) {
    char line[512];

    if (!opt.trace && !opt.hashRam) {
        return 0;
    }
    line[0] = 0;
    if (opt.trace) {
        port_game_trace(line, sizeof(line));
    }
    if (opt.hashRam && frame % opt.hashRam == 0) {
        printf("frame %ld %s hash %08x\n", frame, line, port_hash_ram());
    } else if (opt.trace) {
        printf("frame %ld %s\n", frame, line);
    }
    if (opt.trace && opt.hostScene != NULL) {
        HwBodies hb;
        HsBodies b;

        hw_bodies(&hb);
        memcpy(b.handVel, hb.handVel, sizeof(b.handVel));
        memcpy(b.ballVel, hb.ballVel, sizeof(b.ballVel));
        b.handYaw = hb.handYaw;
        b.handAction = hb.handAction;
        b.ballHeld = hb.ballHeld;
        b.handOnGround = hb.handOnGround;
        b.ballOnGround = hb.ballOnGround;
        b.handTerrain = hb.handTerrain;
        b.ballTerrain = hb.ballTerrain;
        b.ready = hb.ready;
        b.restarts = hb.restarts;
        b.handVoid = hb.handVoid;
        b.ballVoid = hb.ballVoid;
        b.handVoids = hb.handVoids;
        b.ballVoids = hb.ballVoids;
        memcpy(b.handSafe, hb.handSafe, sizeof(b.handSafe));
        memcpy(b.ballSafe, hb.ballSafe, sizeof(b.ballSafe));
        b.safeKnown = hb.safeKnown;
        b.handWater = hb.handWater;
        b.ballWater = hb.ballWater;
        hs_bodies_line(line, sizeof(line), frame, &b);
        printf("%s\n", line);
    }
    return 1;
}

/* The refresh rate of the display that the window is on, in hertz (0: unknown). */
static int display_refresh(SDL_Window *w) {
    SDL_DisplayMode mode;
    int index = SDL_GetWindowDisplayIndex(w);

    if (SDL_GetCurrentDisplayMode(index < 0 ? 0 : index, &mode) != 0 || mode.refresh_rate < 20) {
        return 0;
    }
    return mode.refresh_rate;
}

/* Draws the tasks at the interpolation time t and returns the frame (RGBA, from the top; free it), or NULL. */
static unsigned char *render_at(const PortFrameTask *tasks, long count, float t, int *w, int *h) {
    gfx_interp_set(t);
    gfx_draw_frame((const GfxTask *)tasks, (int)count, opt.shotWidth, opt.shotHeight);
    gfx_interp_set(-1.0f);
    return gfx_read_frame(w, h);
}

/* The mean difference of two frames of the same size, in colour levels (0 to 255). */
static double frame_diff(const unsigned char *a, const unsigned char *b, int w, int h) {
    double sum = 0.0;
    long i;

    for (i = 0; i < (long)w * h; i++) {
        sum += abs(a[i * 4] - b[i * 4]) + abs(a[i * 4 + 1] - b[i * 4 + 1]) + abs(a[i * 4 + 2] - b[i * 4 + 2]);
    }
    return sum / ((double)w * h * 3.0);
}

/* Tells the renderer which buffers the game rebuilds in every frame (see gfx_interp_set_volatile). */
static void set_interp_volatile(void) {
    unsigned int lo;
    unsigned int hi;

    port_game_dynamic_range(&lo, &hi);
    gfx_interp_set_volatile(lo, hi);
}

/* --audio-out: the sound player's frames on the game's clock, raw 16-bit stereo in the host's byte order. The library's
   test host writes the same file (docs/testing.md). */
#ifdef PORT_AUDIO_MUS
static FILE *audioOutFile;

static void write_audio_out(const short *frames, int count) {
    fwrite(frames, 4, (size_t)count, audioOutFile);
}
#endif

static void open_audio_out(void) {
    if (opt.audioOut == NULL) {
        return;
    }
#ifdef PORT_AUDIO_MUS
    audioOutFile = fopen(opt.audioOut, "wb");
    if (audioOutFile == NULL) {
        die("cannot write %s", opt.audioOut);
    }
    port_audio_sink = write_audio_out;
#else
    die("%s", "--audio-out needs the real sound player (AUDIO=mus)");
#endif
}

/* --host-scene: the scene's requests go straight to the host world (hostworld.c). The scene's structures have the
   layout of hostworld.h's. */
static int hs_static(void *u, const HsMesh *m, const HsOptions *o) {
    (void)u;
    return hw_set_static((const HwMesh *)m, (const HwOptions *)o);
}
static int hs_body_add(void *u, const HsMesh *m, const HsPose *p, unsigned int *id) {
    (void)u;
    return hw_body_add((const HwMesh *)m, (const HwPose *)p, id);
}
static int hs_body_move(void *u, unsigned int id, const HsPose *p) {
    (void)u;
    return hw_body_move(id, (const HwPose *)p);
}
static int hs_body_remove(void *u, unsigned int id) {
    (void)u;
    return hw_body_remove(id);
}
static int hs_clear(void *u) {
    (void)u;
    return hw_clear();
}
static int hs_place(void *u, const HsPlace *p) {
    (void)u;
    return hw_place((const HwPlace *)p);
}
static int hs_respawn(void *u, const float pos[3], float yaw) {
    (void)u;
    return hw_set_respawn(pos, yaw);
}
static int hs_view(void *u, const HsView *v) {
    (void)u;
    return hw_set_view((const HwView *)v);
}
static int hs_goto(void *u, int level) {
    (void)u;
    if (!port_goto_possible()) {
        return -1;
    }
    port_goto_level(level);
    return 0;
}
static void hs_log(void *u, const char *line) {
    (void)u;
    fprintf(stderr, "%s\n", line);
}
static int hs_body_conveyor(void *u, unsigned int id, const float vel[3]) {
    (void)u;
    return hw_body_conveyor(id, vel);
}
static int hs_water(void *u, const HsWater *w, unsigned int count) {
    (void)u;
    return hw_set_water((const HwWater *)w, count);
}
static const HsOps hsOps = { hs_static, hs_body_add, hs_body_move, hs_body_remove, hs_clear,
                             hs_place, hs_respawn, hs_view, hs_goto, hs_log, hs_body_conveyor, hs_water };

#ifdef PORT_STUDIO
/* --load-state: the game starts from the state file, on a new timeline. */
static void load_state_option(void) {
    char err[600];

    if (opt.loadState != NULL && studio_load_state(opt.loadState, err, sizeof(err)) != 0) {
        die("%s", err);
    }
}
#endif

static int run_headless(void) {
    PortFrameTask tasks[16];
    PortFrameTask lastTasks[16];
    long lastCount = 0;
    long frame = 0;
    long n;
    double pending = 0.0;
    double dt = opt.dt > 0.0 && opt.dt <= 1.0 ? opt.dt : 1.0;
    HostScene *scene = NULL;
    double sceneTime = 0.0;

    if (opt.hostScene != NULL) {
        char err[300];

        scene = hs_load(opt.hostScene, err, sizeof(err));
        if (scene == NULL) {
            fprintf(stderr, "%s\n", err);
            return 1;
        }
    }
    useFakeTime = 1;
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (opt.shotCount) {
        if (opt.shotWidth <= 0 || opt.shotHeight <= 0) {
            opt.shotWidth = 640;
            opt.shotHeight = 480;
        }
        open_shot_window();
    }
    port_set_space_demo(opt.spaceDemo);
    open_audio_out();
    port_game_init(opt.level);
#ifdef PORT_STUDIO
    studio_started();
    load_state_option();
#endif
    if (opt.shotCount) {
        unsigned int a;
        unsigned int b;

        port_game_framebuffers(&a, &b);
        gfx_set_framebuffers(a, b);
        gfx_set_memory_range(PORT_PTR_BASE, PORT_PTR_BASE + 0x1800000);
        gfx_set_memory_base(PORT_PTR_DELTA);
        set_interp_volatile();
    }
    /* FRAMES counts original game frames. With --dt each one takes several updates; the input script and the
       output follow the original frames. */
    while (frame < opt.headlessFrames) {
        {
            double step = 0.0;

            if (opt.replayPath) {
                /* The recording gives each update's length and controller. */
                float recorded;

                if (!replay_next(&recorded)) {
                    break;
                }
                step = recorded;
                pending = 0.0;
            } else if (pending < 1e-7) {
                /* A new drawn frame: the controller is polled once, and the time that has passed is its length. */
                scripted_input(frame);
                scripted_debug(frame);
                if (opt.gotoFrame >= 0 && frame >= opt.gotoFrame) {
                    port_goto_level(opt.gotoLevel);
                    opt.gotoFrame = -1;
                }
                pending = dt;
                if (opt.dtJitter > 0.0) {
                    /* A fixed generator, so that a run repeats exactly. */
                    jitterSeed = jitterSeed * 1103515245u + 12345u;
                    pending = dt * (1.0 + opt.dtJitter * ((double)((jitterSeed >> 16) & 0x7fff) / 16383.5 - 1.0));
                    if (!opt.pace && pending > 1.0) {
                        pending = 1.0;
                    }
                }
            }
            /* As the window's loop does: a gap longer than an original frame, or what is left of one, is split. */
            if (!opt.replayPath) {
                step = pending > 1.0 ? 1.0 : pending;
                pending -= step;
            }
            if (scene != NULL) {
                hs_update(scene, sceneTime, step, &hsOps, NULL);
                sceneTime += step;
            }
            n = run_update(opt.replayPath ? (float)step : port_game_demo() ? 1.0f : (float)step, tasks, 16);
            fakeTimeUs += (unsigned long long)(step * port_game_retraces() * 1000000.0 / 60.0);
        }
        if (opt.traceUpdates) {
            print_update_trace();
        }
        if (!port_game_ticked()) {
            continue;
        }
        if (!print_tick_trace(frame) && (frame < 5 || frame % 100 == 0)) {
            long i;

            printf("frame %ld: mode %d level %d lists", frame, port_game_mode(), port_game_level());
            for (i = 0; i < n; i++) {
                printf(" %u%s", tasks[i].size / 8, tasks[i].sprite2d ? "s" : "");
            }
            printf("\n");
        }
        if (opt.shotCount) {
            int k;

            if (n >= 0) {
                memcpy(lastTasks, tasks, sizeof(tasks));
                lastCount = n;
                if (opt.interpShot >= 0 || opt.interpCheck) {
                    gfx_interp_record((const GfxTask *)tasks, (int)n, (int)port_game_cut());
                }
            }
            if (opt.interpCheck && n >= 0) {
                /* The picture at t = 0 shows the previous game frame's matrices on the latest frame's data. It
                   should look like the previous frame's own picture. */
                static unsigned char *prevPic;
                static int prevW;
                static int prevH;
                int w;
                int h;
                unsigned char *at0 = render_at(lastTasks, lastCount, 0.0f, &w, &h);
                unsigned char *at1;

                if (at0 != NULL && prevPic != NULL && prevW == w && prevH == h) {
                    printf("interp-check frame %ld t0-vs-previous %.3f frame-change %.3f\n", frame,
                           frame_diff(prevPic, at0, w, h), frame_diff(prevPic, render_at(lastTasks, lastCount, 1.0f, &w, &h), w, h));
                }
                free(at0);
                at1 = render_at(lastTasks, lastCount, 1.0f, &w, &h);
                free(prevPic);
                prevPic = at1;
                prevW = w;
                prevH = h;
            }
            if (opt.interpShot >= 0 && frame == opt.interpShot) {
                static const char *const names[] = {"_t000", "_t025", "_t050", "_t075", "_t100"};
                static const float ts[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};

                for (k = 0; k < 5; k++) {
                    gfx_interp_set(ts[k]);
                    shotSuffix = names[k];
                    take_shot(lastTasks, lastCount, frame);
                }
                gfx_interp_set(-1.0f);
                shotSuffix = "";
            }
            for (k = 0; k < opt.shotCount; k++) {
                if (opt.shotFrames[k] == frame) {
                    take_shot(lastTasks, lastCount, frame);
                }
            }
        }
#ifdef PORT_STUDIO
        {
            int k;

            for (k = 0; k < opt.saveStateCount; k++) {
                char err[600];

                if (opt.saveStateFrame[k] == frame && studio_save_state(opt.saveStatePath[k], err, sizeof(err)) != 0) {
                    die("%s", err);
                }
            }
        }
#endif
        if (opt.dumpPath && frame == opt.dumpFrame) {
            FILE *f = fopen(opt.dumpPath, "wb");

            if (f != NULL) {
                fwrite(port_ram, 1, 0x1800000, f);
                fclose(f);
            }
        }
        frame++;
    }
    finish_recording();
    if (scene != NULL) {
        HwStats st;

        hw_stats(&st);
        fprintf(stderr, "hostscene: %u static triangles in %u chunks (%u linked), %u bodies, %u bytes of heap, %u free, "
                "%u rebuilds, %u failed; built in %u us; %u enemies and %u collectables on; %u conveyor platforms (%u "
                "triangles); %u waters\n", st.staticTris, st.chunks, st.linkedChunks, st.bodies, st.heapUsed,
                st.heapFree, st.rebuilds, st.failed, st.buildUs, st.enemiesOn, st.collectablesOn, st.conveyorPlatforms,
                st.conveyorTris, st.waters);
        hs_free(scene);
    }
#ifdef PORT_STUDIO
    if (getenv("WEEMITT_STUDIO_SELFTEST") != NULL) {
        return studio_selftest() != 0;
    }
#endif
    return 0;
}

/* ---- The window. */

static void usage(void) {
    printf("usage: weemitt [--rom FILE] [--level N] [--fullscreen] [--no-vsync] [--max-fps N] [--fixed] [--save FILE]\n"
           "               [--no-sound] [--space-demo] [--log FILE] [--easy-ball] [--camera-follow N] [--skip-intro]\n"
           "               [--infinite-lives] [--infinite-health] [--unlock-all] [--level-select] [--ballworld]\n"
           "       weemitt --headless FRAMES [--level N] [--dt D] [--input SCRIPT] [--trace] [--hash | --hash-every N]\n"
           "                [--dump FRAME FILE] [--shot FRAME]... [--shot-prefix PATH] [--shot-size W H] [--audio-out FILE]\n"
           "--audio-out FILE (headless, with the real sound player) writes the sound's 16-bit stereo frames to FILE.\n"
           "--log FILE writes the program's messages to FILE (the menu's Developer window can write weemitt.log).\n"
           "--skip-intro skips the logos and the fly-through up to the title menu. The cheat options hold the game's own cheats\n"
           "(INFINITE LIVES, INFINITE ENERGY, OPEN PORTALS with OPEN LEVELS, LEVEL SELECT) on. They work in the headless mode too,\n"
           "where --goto-level FRAME LEVEL starts that level after original frame FRAME.\n"
           "--debug-input SCRIPT drives the game's debug pad pad[4] (the format of --input), and --debug-action FRAME NAME\n"
           "starts a debug action at that frame (prev-platform, next-platform, mark-levels, timing, prev-song, next-song,\n"
           "inertia, quit, hurt, autoplay).\n"
           "--space-demo starts in Space 1 and joins Space 1, 2 and 3 into one timed run.\n"
           "--original-physics runs the logic as on the N64 and blends the drawn frames between two of its frames;\n"
           "--smooth-physics runs it in short steps, once per drawn frame (settings.ini keeps the choice).\n"
           "--render-scale PERCENT (50 to 400), --msaa SAMPLES (0, 2, 4, 8), --texture-filter n64|bilinear|nearest and\n"
           "--widescreen off|stretch|horplus set the Graphics window's picture options for this run.\n"
           "--shot-window (with --headless) saves the whole window in a screenshot, not only the renderer's frame.\n"
           "--interp-shot FRAME (with --headless) saves screenshots at 0, 0.5 and 1 of the way between frame FRAME-1 and FRAME.\n"
           "--interp-check (with --headless) prints, for every frame, how far the picture at t = 0 is from the previous frame's picture.\n"
           "--fixed runs the game at the N64's rate (20 or 30 frames a second) instead of once per drawn frame.\n"
           "--dt runs headless updates of D original frames each (for example 0.25); FRAMES counts original frames.\n"
           "--dt-jitter J varies each headless update's length by up to J times D (for example 0.3), the same way every run.\n"
           "--pace lets --dt-jitter make a drawn frame longer than D and splits it into updates, as the window does at a frame rate limit.\n"
           "--host-scene FILE (headless, with --level) runs the glove and the ball in a host world: triangles, bodies and\n"
           "                requests from FILE (docs/host-world.md).\n"
           "--trace-updates prints the glove and ball state after every update (headless, or with --trace in a window build).\n"
           "--record FILE writes an input recording (any build). The menu's Developer window can record to the save folder.\n"
           "--replay FILE runs a recording headless, with its updates and controller values; it works with --trace,\n"
           "                --trace-updates and --hash-every.\n");
#ifdef PORT_STUDIO
    printf("The studio:\n"
           "--load-state FILE starts from a state file that the studio saved: its frame is the first of the timeline.\n"
           "                The window starts paused. With --headless, the updates and the --input script start there.\n"
           "--save-state FRAME FILE (with --headless, up to 8 times) writes the state after original frame FRAME (the\n"
           "                frame of --trace's line) to FILE and goes on. A state file holds the game's memory: do not share it.\n"
           "weemitt-studio search|bench|play|saveload ... runs the brute forcer's commands (bruteforce/README.md).\n");
#endif
    exit(0);
}

static void parse_args(int argc, char **argv) {
    int i;

    opt.vsync = -1;
    opt.maxFps = -1;
    opt.msaa = -1;
    opt.texFilter = -1;
    opt.widescreen = -1;
    opt.level = -1;
    opt.physics = -1;
    opt.interpShot = -1;
    opt.gotoFrame = -1;
#ifdef PORT_SPACE_DEMO
    /* The demo build (make space-demo) is the Space demo without options. */
    opt.spaceDemo = 1;
#endif
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;

        if (!strcmp(a, "--rom") && next) {
            opt.rom = argv[++i];
        } else if (!strcmp(a, "--headless") && next) {
            opt.headlessFrames = atol(argv[++i]);
        } else if (!strcmp(a, "--input") && next) {
            opt.inputScript = argv[++i];
        } else if (!strcmp(a, "--level") && next) {
            opt.level = atol(argv[++i]);
        } else if (!strcmp(a, "--fixed")) {
            opt.fixedRate = 1;
        } else if (!strcmp(a, "--dt") && next) {
            opt.dt = atof(argv[++i]);
        } else if (!strcmp(a, "--dt-jitter") && next) {
            opt.dtJitter = atof(argv[++i]);
        } else if (!strcmp(a, "--pace")) {
            opt.pace = 1;
        } else if (!strcmp(a, "--record") && next) {
            opt.recordPath = argv[++i];
        } else if (!strcmp(a, "--replay") && next) {
            opt.replayPath = argv[++i];
        } else if (!strcmp(a, "--host-scene") && next) {
            opt.hostScene = argv[++i];
        } else if (!strcmp(a, "--trace-updates")) {
            opt.traceUpdates = 1;
        } else if (!strcmp(a, "--max-fps") && next) {
            opt.maxFps = atoi(argv[++i]);
        } else if (!strcmp(a, "--shot") && next) {
            if (opt.shotCount < 64) {
                opt.shotFrames[opt.shotCount++] = atol(argv[++i]);
            } else {
                i++;
            }
        } else if (!strcmp(a, "--interp-shot") && next) {
            opt.interpShot = atol(argv[++i]);
            if (opt.shotCount == 0) {
                opt.shotFrames[opt.shotCount++] = -1;
            }
        } else if (!strcmp(a, "--interp-check")) {
            opt.interpCheck = 1;
            if (opt.shotCount == 0) {
                opt.shotFrames[opt.shotCount++] = -1;
            }
        } else if (!strcmp(a, "--original-physics")) {
            opt.physics = PHYSICS_ORIGINAL;
        } else if (!strcmp(a, "--smooth-physics")) {
            opt.physics = PHYSICS_SMOOTH;
        } else if (!strcmp(a, "--render-scale") && next) {
            opt.renderScale = atoi(argv[++i]);
        } else if (!strcmp(a, "--msaa") && next) {
            opt.msaa = atoi(argv[++i]);
        } else if (!strcmp(a, "--texture-filter") && next) {
            const char *v = argv[++i];

            opt.texFilter = !strcmp(v, "bilinear") ? TEXFILTER_BILINEAR : !strcmp(v, "nearest") ? TEXFILTER_NEAREST : TEXFILTER_N64;
        } else if (!strcmp(a, "--widescreen") && next) {
            const char *v = argv[++i];

            opt.widescreen = !strcmp(v, "stretch") ? WIDESCREEN_STRETCH : !strcmp(v, "horplus") ? WIDESCREEN_HORPLUS : WIDESCREEN_OFF;
        } else if (!strcmp(a, "--shot-window")) {
            opt.shotWindow = 1;
        } else if (!strcmp(a, "--shot-prefix") && next) {
            opt.shotPrefix = argv[++i];
        } else if (!strcmp(a, "--shot-size") && i + 2 < argc) {
            opt.shotWidth = atoi(argv[++i]);
            opt.shotHeight = atoi(argv[++i]);
#ifdef PORT_STUDIO
        } else if (!strcmp(a, "--load-state") && next) {
            opt.loadState = argv[++i];
        } else if (!strcmp(a, "--save-state") && i + 2 < argc) {
            if (opt.saveStateCount == 8) {
                die("%s", "at most 8 --save-state options");
            }
            opt.saveStateFrame[opt.saveStateCount] = atol(argv[++i]);
            opt.saveStatePath[opt.saveStateCount++] = argv[++i];
#endif
        } else if (!strcmp(a, "--dump") && i + 2 < argc) {
            opt.dumpFrame = atol(argv[++i]);
            opt.dumpPath = argv[++i];
        } else if (!strcmp(a, "--trace")) {
            opt.trace = 1;
        } else if (!strcmp(a, "--hash")) {
            opt.hashRam = 1;
        } else if (!strcmp(a, "--hash-every") && next) {
            opt.hashRam = atoi(argv[++i]);
        } else if (!strcmp(a, "--save") && next) {
            opt.savePath = argv[++i];
        } else if (!strcmp(a, "--audio-out") && next) {
            opt.audioOut = argv[++i];
        } else if (!strcmp(a, "--fullscreen")) {
            opt.fullscreen = 1;
        } else if (!strcmp(a, "--no-vsync")) {
            opt.vsync = 0;
        } else if (!strcmp(a, "--no-sound")) {
            opt.noSound = 1;
        } else if (!strcmp(a, "--ballworld")) {
            opt.ballworld = 1;
        } else if (!strcmp(a, "--easy-ball")) {
            opt.easyBall = 1;
        } else if (!strcmp(a, "--skip-intro")) {
            opt.skipIntro = 1;
        } else if (!strcmp(a, "--infinite-lives")) {
            opt.infiniteLives = 1;
        } else if (!strcmp(a, "--infinite-health")) {
            opt.infiniteHealth = 1;
        } else if (!strcmp(a, "--unlock-all")) {
            opt.unlockAll = 1;
        } else if (!strcmp(a, "--level-select")) {
            opt.levelSelect = 1;
        } else if (!strcmp(a, "--debug-input") && i + 1 < argc) {
            opt.debugInput = argv[++i];
        } else if (!strcmp(a, "--debug-action") && i + 2 < argc) {
            long f = atol(argv[++i]);

            add_debug_action(f, argv[++i]);
        } else if (!strcmp(a, "--goto-level") && i + 2 < argc) {
            opt.gotoFrame = atol(argv[++i]);
            opt.gotoLevel = atol(argv[++i]);
        } else if (!strcmp(a, "--camera-follow") && next) {
            opt.cameraFollow = (float)atof(argv[++i]);
        } else if (!strcmp(a, "--log") && next) {
            opt.logPath = argv[++i];
        } else if (!strcmp(a, "--space-demo")) {
            opt.spaceDemo = 1;
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage();
        } else if (a[0] != '-' && opt.rom == NULL) {
            opt.rom = a;
        } else {
            plog_printf("weemitt: unknown option %s", a);
            usage();
        }
    }
    if (opt.replayPath && !opt.headlessFrames) {
        /* A replay runs until the recording ends. */
        opt.headlessFrames = 0x7fffffff;
    }
}

/* Test option: WEEMITT_TAPS=START,SPACING,COUNT. From original frame START, every SPACING frames, the window loop
   gets a key down and a key up event for the first key bound to L, at once, COUNT times. The keyboard state never
   shows such a tap. See docs/testing.md. */
static long tapStart = -1;
static long tapSpacing = 1;
static long tapCount;
static long tapNext;

static void parse_taps(void) {
    const char *t = getenv("WEEMITT_TAPS");

    if (t != NULL && sscanf(t, "%ld,%ld,%ld", &tapStart, &tapSpacing, &tapCount) == 3 && tapSpacing > 0) {
        tapNext = tapStart;
    } else {
        tapStart = -1;
    }
}

static void inject_taps(long ticks, const Settings *st) {
    SDL_Event e;

    if (tapStart < 0 || tapCount <= 0 || ticks < tapNext) {
        return;
    }
    tapNext += tapSpacing;
    tapCount--;
    memset(&e, 0, sizeof(e));
    e.key.keysym.scancode = (SDL_Scancode)st->keys[IN_L][0];
    e.key.state = SDL_PRESSED;
    e.type = SDL_KEYDOWN;
    SDL_PushEvent(&e);
    e.key.state = SDL_RELEASED;
    e.type = SDL_KEYUP;
    SDL_PushEvent(&e);
}

/* Test options for the Developer menu's frame step: WEEMITT_PAUSE_AT=N pushes the pause key once the game has run N
   original frames, and WEEMITT_STEP_PRESSES=C then pushes the step key C times, one every 5 drawn frames. */
static long pauseAt = -1;
static long stepPresses;
static long stepWait;

static void parse_step_test(void) {
    const char *t = getenv("WEEMITT_PAUSE_AT");

    if (t != NULL) {
        pauseAt = atol(t);
    }
    t = getenv("WEEMITT_STEP_PRESSES");
    if (t != NULL) {
        stepPresses = atol(t);
    }
}

static void push_key(int scancode) {
    SDL_Event e;

    memset(&e, 0, sizeof(e));
    e.key.keysym.scancode = (SDL_Scancode)scancode;
    e.key.keysym.sym = SDL_GetKeyFromScancode((SDL_Scancode)scancode);
    e.key.state = SDL_PRESSED;
    e.type = SDL_KEYDOWN;
    SDL_PushEvent(&e);
    e.key.state = SDL_RELEASED;
    e.type = SDL_KEYUP;
    SDL_PushEvent(&e);
}

static void inject_step_test(long ticks, const Settings *st) {
    if (pauseAt >= 0 && ticks >= pauseAt) {
        pauseAt = -1;
        push_key(st->pauseKey);
    } else if (pauseAt < 0 && st->pauseLogic && stepPresses > 0 && ++stepWait >= 5) {
        stepWait = 0;
        stepPresses--;
        push_key(st->stepKey);
    }
}

/* The most collision lines drawn in a frame. */
#define DEBUG_LINES_MAX 60000
static PortDebugLine debugLines[DEBUG_LINES_MAX];

/* Hands the renderer the collision lines for the next frame (Developer, Show collision). */
static void prepare_overlays(const Settings *st) {
    long n = 0;

#ifdef PORT_STUDIO
    /* The studio's points come first: the collision lines can fill the buffer. */
    n = studio_overlay_lines(debugLines, DEBUG_LINES_MAX);
#endif
    if (st->showCollision) {
        n += port_debug_collision(debugLines + n, DEBUG_LINES_MAX - n, (float)st->debugRange,
                                  (unsigned int)st->collisionTypes);
    }
    gfx_debug_lines((const GfxDebugLine *)debugLines, (int)n, st->collisionDepth);
}

/* One update in the window: recorded, and traced when --trace or --trace-updates asks (stdout). */
static long window_update(float dt, PortFrameTask *tasks, long *ticks) {
    long n = run_update(dt, tasks, 16);

    if (opt.traceUpdates) {
        print_update_trace();
    }
    if (port_game_ticked()) {
        print_tick_trace(*ticks);
        (*ticks)++;
    }
    return n;
}

#ifdef PORT_PTR_DISPLACED
#include <sys/mman.h>
extern unsigned char port_ram_guard[];

/* The displaced test build (docs/port64.md): the words that the game stores for pointers lie in
   [PORT_PTR_BASE, port_ram), and port_ram lies 32 MB above them. That range is made unreadable, so a word used as
   a pointer without being decoded faults where it is used. */
static void protect_ram_guard(void) {
    if ((uintptr_t)port_ram != PORT_PTR_BASE + PORT_PTR_DELTA || (uintptr_t)port_ram_guard != PORT_PTR_BASE) {
        fprintf(stderr, "port_ram is at %p and its guard at %p; the displaced build needs %08x and %08x\n",
                (void *)port_ram, (void *)port_ram_guard, PORT_PTR_BASE + (unsigned)PORT_PTR_DELTA, PORT_PTR_BASE);
        exit(1);
    }
    if (mprotect(port_ram_guard, PORT_PTR_DELTA, PROT_NONE) != 0) {
        perror("mprotect");
        exit(1);
    }
}
#endif

int main(int argc, char **argv) {
    SDL_Window *window;
    SDL_GLContext gl;
    PortFrameTask tasks[16];
    long taskCount = 0;
    unsigned int cfbA;
    unsigned int cfbB;
    Uint64 last;
    double acc = 0.0;
    int running = 1;
    Settings settings;
    char settingsFile[512];
    long crashTestFrame = -1;
    long drawn = 0;
    long quitFrames = -1;
    long windowTicks = 0;
    long updatesRun = 0; /* updates the window has run, to tell the start-up from a frame boundary */
    int original;
    int wasOriginal = 0;
    int pacing = 0; /* the swap interval set last, and the state that it came from (see the limiter below) */
    int pacingKey = -1;
    int refresh = 0; /* the display's refresh rate in hertz, 0 when unknown */
    int vsyncSlack = 0; /* drawn frames in a row that VSync did not slow down */
    int startW = 960;
    int startH = 720;
    int vsyncFails = 0; /* VSync is on but does not pace: the software limiter takes over */

#ifdef PORT_STUDIO
    {
        /* The studio's program also runs the brute forcer's commands (search workers among them). */
        int code;

        if (studio_command(argc, argv, &code)) {
            return code;
        }
    }
#endif
#ifdef PORT_PTR_DISPLACED
    protect_ram_guard();
#endif
    parse_args(argc, argv);
    if (opt.trace || opt.traceUpdates || opt.hashRam) {
        setvbuf(stdout, NULL, _IOLBF, 0);
    }
    settings_defaults(&settings);
    if (opt.logPath != NULL) {
        plog_set_override(opt.logPath);
        plog_set_file(opt.logPath);
    }
    if (!opt.headlessFrames) {
        /* The settings come first so that the log file captures the start-up messages. */
        settings_path(settingsFile, sizeof(settingsFile));
        settings_load(&settings, settingsFile);
        if (settings.logFile && opt.logPath == NULL) {
            char logFile[512];

            plog_path(logFile, sizeof(logFile));
            plog_set_file(logFile);
        }
        plog_set_verbose(settings.logVerbose);
        plog_install_crash_handler();
        port_set_easy_ball(settings.easyBall);
        port_set_ballworld(settings.ballworld);
        port_set_camera_follow(settings.cameraFollow);
        if (getenv("WEEMITT_CRASH_TEST") != NULL) {
            crashTestFrame = atol(getenv("WEEMITT_CRASH_TEST"));
        }
        /* Test options: an input script that the window applies per original frame (the format of --input), and
           a quit after that many original frames. */
        if (getenv("WEEMITT_INPUT_SCRIPT") != NULL && opt.inputScript == NULL) {
            opt.inputScript = getenv("WEEMITT_INPUT_SCRIPT");
        }
        if (getenv("WEEMITT_DEBUG_INPUT") != NULL && opt.debugInput == NULL) {
            opt.debugInput = getenv("WEEMITT_DEBUG_INPUT");
        }
        if (getenv("WEEMITT_DEBUG_ACTIONS") != NULL) {
            parse_debug_actions(getenv("WEEMITT_DEBUG_ACTIONS"));
        }
        if (getenv("WEEMITT_QUIT_FRAMES") != NULL) {
            quitFrames = atol(getenv("WEEMITT_QUIT_FRAMES"));
        }
    }
    if (opt.rom == NULL) {
        opt.rom = find_rom();
    }
    if (SDL_Init(opt.headlessFrames ? SDL_INIT_TIMER : SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) {
        die("SDL could not start: %s", SDL_GetError());
    }
    timeBase = SDL_GetPerformanceCounter();
    if (opt.rom == NULL) {
        die("%s", "No ROM found. Put the Glover (USA) ROM next to the program as glover.z64, or pass --rom FILE.");
    }
    if (!opt.headlessFrames) {
        plog_printf("weemitt starting, ROM %s", opt.rom);
    }
    load_rom(opt.rom);
    if (opt.ballworld) {
        settings.ballworld = 1;
        port_set_ballworld(1);
    }
    load_save();
    port_record_folder(port_debug_folder, sizeof(port_debug_folder));
#ifdef PORT_STUDIO
    studio_init(port_rom, port_rom_size);
    studio_set_rom_path(opt.rom);
#endif
    if (opt.inputScript) {
        load_input_script(opt.inputScript, &script, &scriptCount);
    }
    if (opt.debugInput) {
        load_input_script(opt.debugInput, &debugScript, &debugScriptCount);
        debugScriptOn = 1;
        port_debug_pad_mode(1);
    }

    if (opt.easyBall) {
        settings.easyBall = 1;
        port_set_easy_ball(1);
    }
    if (opt.cameraFollow > 0.0f) {
        settings.cameraFollow = opt.cameraFollow;
        port_set_camera_follow(opt.cameraFollow);
    }
    settings.skipIntro |= opt.skipIntro;
    settings.infiniteLives |= opt.infiniteLives;
    settings.infiniteHealth |= opt.infiniteHealth;
    settings.unlockAll |= opt.unlockAll;
    settings.levelSelect |= opt.levelSelect;
    port_set_skip_intro(settings.skipIntro);
    port_set_pause_cheats(settings.pauseCheats);
    port_set_cheat_hold((settings.infiniteLives ? 1 << 3 : 0) | (settings.infiniteHealth ? 1 << 6 : 0)
                        | (settings.levelSelect ? 1 << 23 : 0) | (settings.unlockAll ? 3 << 24 : 0));
    if (opt.replayPath) {
        replay_open(&settings);
        settings.easyBall = replayInfo.easyBall;
        settings.cameraFollow = replayInfo.cameraFollow;
    } else if (opt.recordPath != NULL || (!opt.headlessFrames && settings.recordInput)) {
        char path[1024];

        if (opt.recordPath != NULL) {
            snprintf(path, sizeof(path), "%s", opt.recordPath);
        } else {
            rec_default_path(path, sizeof(path));
        }
        start_recording(path, &settings);
    }
    if (opt.headlessFrames) {
        return run_headless();
    }
    if (opt.vsync >= 0) {
        settings.vsync = opt.vsync;
    }
    if (opt.maxFps >= 0) {
        settings.maxFps = opt.maxFps;
    }
    if (opt.physics >= 0) {
        settings.physicsMode = opt.physics;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    /* Test option: WEEMITT_WINDOW_SIZE=WxH sets the size of the window at the start. */
    if (getenv("WEEMITT_WINDOW_SIZE") != NULL && sscanf(getenv("WEEMITT_WINDOW_SIZE"), "%dx%d", &startW, &startH) != 2) {
        startW = 960;
        startH = 720;
    }
    window = SDL_CreateWindow(rec_active() ? (opt.spaceDemo ? "Glover: Space demo [REC]" : "Glover [REC]")
                                           : (opt.spaceDemo ? "Glover: Space demo" : "Glover"), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, startW, startH,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
                                  | (opt.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (window == NULL) {
        die("cannot open a window: %s", SDL_GetError());
    }
    gl = SDL_GL_CreateContext(window);
    if (gl == NULL) {
        die("cannot create an OpenGL 3.3 context: %s", SDL_GetError());
    }
    SDL_GL_SetSwapInterval(settings.vsync ? 1 : 0);
    menu_init(window, gl, &settings);
    load_pad_mappings();
    make_virtual_pads();
    if (getenv("WEEMITT_DEBUG_PAD") != NULL) {
        settings.debugPad = !strcmp(getenv("WEEMITT_DEBUG_PAD"), "second") ? DEBUGPAD_SECOND : DEBUGPAD_KEYBOARD;
    }
    parse_taps();
    parse_step_test();
    open_pad();
    gfx_set_loader(SDL_GL_GetProcAddress);
    gfx_init();
    apply_gfx_options(&settings);
    port_set_space_demo(opt.spaceDemo);
    port_game_init(opt.level);
#ifdef PORT_STUDIO
    studio_started();
    load_state_option();
#endif
    port_game_framebuffers(&cfbA, &cfbB);
    gfx_set_framebuffers(cfbA, cfbB);
    gfx_set_memory_range(PORT_PTR_BASE, PORT_PTR_BASE + 0x1800000);
    gfx_set_memory_base(PORT_PTR_DELTA);
    set_interp_volatile();

    last = SDL_GetPerformanceCounter();
    while (running) {
        SDL_Event e;
        Uint64 now;
        int w;
        int h;

        inject_taps(windowTicks, &settings);
        inject_step_test(windowTicks, &settings);
        while (SDL_PollEvent(&e)) {
            /* Every event goes through input_event_buttons, which follows the axes and hats. */
            if (e.type == SDL_JOYDEVICEADDED || e.type == SDL_JOYDEVICEREMOVED || e.type == SDL_CONTROLLERDEVICEADDED) {
                pad2Dirty = 1;
            }
            unsigned short pressed = input_event_buttons(&settings, &e, !menu_is_open(), pad != NULL,
                                                         pad == NULL && joy != NULL);

            if (menu_handle_event(&e)) {
                continue;
            }
            tapPending |= pressed;
            if (e.type == SDL_QUIT) {
                running = 0;
            } else if (e.type == SDL_KEYDOWN && settings.pauseKey != 0 && (int)e.key.keysym.scancode == settings.pauseKey
                       && !e.key.repeat) {
                /* Developer, Pause game logic. */
                settings.pauseLogic = !settings.pauseLogic;
                settings.stepRequests = 0;
            } else if (e.type == SDL_KEYDOWN && settings.stepKey != 0 && (int)e.key.keysym.scancode == settings.stepKey) {
                /* The step key pauses the logic first. While paused, each press (a key repeat too) runs one frame. */
                if (!settings.pauseLogic) {
                    settings.pauseLogic = 1;
                } else if (settings.stepRequests < 4) {
                    settings.stepRequests++;
                }
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F11) {
                Uint32 full = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                SDL_SetWindowFullscreen(window, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)) {
                Uint32 full = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                SDL_SetWindowFullscreen(window, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            } else if (e.type == SDL_JOYDEVICEADDED) {
                log_device(e.jdevice.which, "joystick found:");
                open_pad();
            } else if (e.type == SDL_CONTROLLERDEVICEADDED) {
                open_pad();
            } else if (e.type == SDL_JOYDEVICEREMOVED) {
                int mine = (joy != NULL && e.jdevice.which == SDL_JoystickInstanceID(joy))
                           || (pad != NULL && e.jdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)));

                plog_printf("weemitt: joystick removed (instance %d)%s%s", (int)e.jdevice.which, mine ? ": " : "",
                            mine ? deviceName : "");
                if (joy != NULL && e.jdevice.which == SDL_JoystickInstanceID(joy)) {
                    SDL_JoystickClose(joy);
                    joy = NULL;
                    open_pad();
                } else if (pad != NULL && e.jdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) {
                    SDL_GameControllerClose(pad);
                    pad = NULL;
                    open_pad();
                }
            }
        }
        read_input(&settings);
        virtual_press(windowTicks);
        read_debug_input(&settings);
#ifdef PORT_AUDIO_MUS
        audioVolume = settings.muteUnfocused && !(SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) ? 0 : settings.volume;
        musSetGroupGain(settings.musicVolume, settings.effectsVolume);
#endif
        if (scriptCount) {
            scripted_input(windowTicks);
        }
        scripted_debug(windowTicks);
#ifdef PORT_STUDIO
        studio_poll(&port_buttons, &port_stick_x, &port_stick_y);
        tapPending &= ~studio_input_mask();
#endif

        /* The renderer needs the window's size and the picture options before it records a frame. */
        SDL_GL_GetDrawableSize(window, &w, &h);
        gfx_set_drawable_size(w, h);
        apply_gfx_options(&settings);
        now = SDL_GetPerformanceCounter();
        acc += (double)(now - last) / SDL_GetPerformanceFrequency();
        last = now;
        if (acc > 0.25) {
            acc = 0.25;
        }
        original = settings.physicsMode == PHYSICS_ORIGINAL && !opt.fixedRate;
#ifdef PORT_STUDIO
        /* The timeline holds whole game frames: the studio always runs the Original physics, whatever the setting
           (which the normal program shares). */
        original = !opt.fixedRate;
#endif
        if (original && !wasOriginal && taskCount > 0) {
            /* Switched to the Original mode: the frame on show is the first one the renderer knows. */
            gfx_interp_record((const GfxTask *)tasks, (int)taskCount, 1);
        }
#ifdef PORT_STUDIO
        if (studio_paused()) {
            long n = studio_apply_seek(tasks, 16);

            if (n >= 0) {
                taskCount = n;
                gfx_interp_record((const GfxTask *)tasks, (int)n, 1);
            }
            if (studio_take_step()) {
                settings.stepRequests++;
            }
        }
        if ((settings.pauseLogic || studio_paused())
            && (updatesRun == 0 || port_game_ticked() || original || opt.fixedRate || port_game_demo())) {
#else
        if (settings.pauseLogic && (updatesRun == 0 || port_game_ticked() || original || opt.fixedRate || port_game_demo())) {
#endif
            /* Developer, Pause game logic. In the Smooth mode the pause starts at the end of the original frame
               in progress. A step is then a whole frame. Drawing goes on at the latest game frame. */
            acc = original ? port_game_retraces() / 60.0 * 0.999 : 0.0;
            if (settings.stepRequests > 0) {
                long n;

                settings.stepRequests--;
                n = window_update(1.0f, tasks, &windowTicks);
                updatesRun++;
                if (n >= 0) {
                    taskCount = n;
                    if (original) {
                        gfx_interp_record((const GfxTask *)tasks, (int)n, (int)port_game_cut());
                    }
                }
            }
            gfx_interp_set(-1.0f);
        } else if (opt.fixedRate || port_game_demo() || original) {
            /* The N64's timing: one original frame every port_game_retraces() sixtieths of a second. A recorded
               demo always runs this way, since its input was recorded per original frame. In the Original physics
               mode the renderer keeps the matrices of each new frame, and the drawn frame blends the previous
               frame and the latest by the time that has passed since the latest (interpolation.md). */
            while (acc >= port_game_retraces() / 60.0) {
                long n;

                acc -= port_game_retraces() / 60.0;
                n = window_update(1.0f, tasks, &windowTicks);
                updatesRun++;
                if (n >= 0) {
                    taskCount = n;
                    if (original) {
                        gfx_interp_record((const GfxTask *)tasks, (int)n, (int)port_game_cut());
                    }
                }
#ifdef PORT_STUDIO
                if (studio_paused()) {
                    /* The recording played to its end: no catch-up frame after it. */
                    acc = 0.0;
                    break;
                }
#endif
            }
            gfx_interp_set(original ? (float)(acc / (port_game_retraces() / 60.0)) : -1.0f);
        } else {
            gfx_interp_set(-1.0f);
            /* One update per drawn frame, for the time that has passed. A gap longer than an original frame is
               split into updates of at most one original frame each (delta.h). A drawn frame within 15% of an
               original frame runs exactly one original frame, and the difference is carried to the next drawn frame.
               At a limit equal to the game's rate the game then runs whole frames, as on the N64, instead of a whole
               frame plus a sliver of the next. A sliver of less than a tenth of a frame left after an update also
               waits for the next drawn frame. */
            int updated = 0;

            while (acc > 0.0) {
                double frameLength = port_game_retraces() / 60.0;
                double step = acc >= frameLength * 0.85 ? 1.0 : acc / frameLength;
                long n;

                if (updated && acc < frameLength * 0.1) {
                    break;
                }
                updated = 1;
                n = window_update((float)step, tasks, &windowTicks);
                updatesRun++;
                if (n >= 0) {
                    taskCount = n;
                }
                acc -= step * frameLength;
                if (acc > -1e-7 && acc < 1e-7) {
                    acc = 0.0;
                }
                if (settings.pauseLogic && port_game_ticked()) {
                    /* Pause asked for: the original frame has ended, so the logic stops here. */
                    acc = 0.0;
                    break;
                }
            }
        }
        wasOriginal = original;
#ifndef PORT_STUDIO
        /* The studio keeps the save in memory: going back on the timeline would rewrite the player's file. */
        if (port_eeprom_dirty) {
            write_save();
        }
#endif
        if (quitFrames >= 0 && windowTicks >= quitFrames) {
            running = 0;
        }
        SDL_GL_GetDrawableSize(window, &w, &h);
        prepare_overlays(&settings);
        gfx_draw_frame((const GfxTask *)tasks, (int)taskCount, w, h);
        menu_draw(window);
        SDL_GL_SwapWindow(window);
        plog_tick(port_game_mode(), port_game_level());
        if (drawn++ == crashTestFrame) {
            plog_crash_now();
        }
        {
            /* With no limit chosen, the drawn rate stops at 10 times the game's own rate (200 a second in a level,
               300 in a menu). An update is then never shorter than 0.1 of an original frame, the shortest tested. */
            int unlimitedCap = (int)(10 * 60 / port_game_retraces());
            /* The Original physics mode has no smallest update, so Unlimited stops at 1000 a second. */
            int cap = original ? 1000 : unlimitedCap;
            int limit = settings.maxFps > 0 ? settings.maxFps : cap;
            double minFrame = 1.0 / limit;
            double freq = (double)SDL_GetPerformanceFrequency();
            double spent = (double)(SDL_GetPerformanceCounter() - now) / freq;
            int interval = 1;
            int software = 1;

            settings.fpsCap = settings.maxFps == 0 ? cap : 0;
            if (drawn % 120 == 1 || refresh == 0) {
                refresh = display_refresh(window);
            }
            if (settings.vsync && refresh > 0 && !vsyncFails) {
                /* VSync alone sets the pace when the limit is the refresh rate or higher, or when it is a whole
                   fraction of it (60 on a 120 Hz display swaps every second refresh). A software limiter that runs
                   beside it drifts against the refresh and now and then costs a whole refresh. */
                double ratio = (double)refresh / limit;
                int n = (int)(ratio + 0.5);

                if (limit >= refresh - 1) {
                    software = 0;
                } else if (n >= 2 && fabs(ratio - n) < 0.03 * n) {
                    interval = n;
                    software = 0;
                }
            }
            if (!settings.vsync) {
                interval = 0;
            }
            if (pacingKey != ((settings.vsync << 16) | (interval << 8) | software)) {
                pacingKey = (settings.vsync << 16) | (interval << 8) | software;
                if (settings.vsync && interval > 1 && SDL_GL_SetSwapInterval(interval) != 0) {
                    /* The driver does not take this interval. */
                    interval = 1;
                    software = 1;
                    SDL_GL_SetSwapInterval(1);
                }
                if (interval <= 1) {
                    SDL_GL_SetSwapInterval(interval);
                }
                pacing = interval;
            }
            if (!software) {
                /* If frames come back much sooner than a refresh, the driver is not waiting for VSync. */
                if (spent < 0.4 / refresh * pacing) {
                    if (++vsyncSlack >= 30) {
                        vsyncFails = 1;
                    }
                } else {
                    vsyncSlack = 0;
                }
            } else {
                /* SDL_Delay is coarse (up to 15 ms on Windows): sleep most of the wait and spin the last 2 ms. */
                if (minFrame - spent > 0.003) {
                    SDL_Delay((Uint32)((minFrame - spent - 0.002) * 1000.0));
                }
                while ((double)(SDL_GetPerformanceCounter() - now) / freq < minFrame) {
                }
            }
        }
    }
    finish_recording();
    menu_shutdown();
    gfx_shutdown();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
