#include <SDL2/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"

/* The file names an input by these words. */
static const char *const inputNames[IN_COUNT] = {
    "A", "B", "Z", "Start", "L", "R",
    "C-Up", "C-Down", "C-Left", "C-Right",
    "D-Up", "D-Down", "D-Left", "D-Right",
    "Stick-Up", "Stick-Down", "Stick-Left", "Stick-Right",
};

static const int defaultKeys[IN_COUNT][KEY_SLOTS] = {
    [IN_A] = {SDL_SCANCODE_SPACE, 0},
    [IN_B] = {SDL_SCANCODE_X, 0},
    [IN_Z] = {SDL_SCANCODE_Z, SDL_SCANCODE_LSHIFT},
    [IN_START] = {SDL_SCANCODE_RETURN, 0},
    [IN_L] = {SDL_SCANCODE_Q, 0},
    [IN_R] = {SDL_SCANCODE_E, 0},
    [IN_CUP] = {SDL_SCANCODE_I, 0},
    [IN_CDOWN] = {SDL_SCANCODE_K, 0},
    [IN_CLEFT] = {SDL_SCANCODE_J, 0},
    [IN_CRIGHT] = {SDL_SCANCODE_L, 0},
    [IN_DUP] = {SDL_SCANCODE_T, 0},
    [IN_DDOWN] = {SDL_SCANCODE_G, 0},
    [IN_DLEFT] = {SDL_SCANCODE_F, 0},
    [IN_DRIGHT] = {SDL_SCANCODE_H, 0},
    [IN_STICK_UP] = {SDL_SCANCODE_UP, SDL_SCANCODE_W},
    [IN_STICK_DOWN] = {SDL_SCANCODE_DOWN, SDL_SCANCODE_S},
    [IN_STICK_LEFT] = {SDL_SCANCODE_LEFT, SDL_SCANCODE_A},
    [IN_STICK_RIGHT] = {SDL_SCANCODE_RIGHT, SDL_SCANCODE_D},
};

/* The debug pad's keys. They avoid the game's own keys (above), F9 to F11 and Escape. */
static const int debugKeys[IN_COUNT] = {
    [IN_A] = SDL_SCANCODE_1,
    [IN_B] = SDL_SCANCODE_2,
    [IN_Z] = SDL_SCANCODE_3,
    [IN_START] = SDL_SCANCODE_4,
    [IN_L] = SDL_SCANCODE_5,
    [IN_R] = SDL_SCANCODE_6,
    [IN_CUP] = SDL_SCANCODE_7,
    [IN_CDOWN] = SDL_SCANCODE_8,
    [IN_CLEFT] = SDL_SCANCODE_9,
    [IN_CRIGHT] = SDL_SCANCODE_0,
    [IN_DUP] = SDL_SCANCODE_LEFTBRACKET,
    [IN_DDOWN] = SDL_SCANCODE_RIGHTBRACKET,
    [IN_DLEFT] = SDL_SCANCODE_MINUS,
    [IN_DRIGHT] = SDL_SCANCODE_EQUALS,
    [IN_STICK_UP] = SDL_SCANCODE_HOME,
    [IN_STICK_DOWN] = SDL_SCANCODE_END,
    [IN_STICK_LEFT] = SDL_SCANCODE_DELETE,
    [IN_STICK_RIGHT] = SDL_SCANCODE_PAGEDOWN,
};

int settings_debug_key(int input) {
    return input >= 0 && input < IN_COUNT ? debugKeys[input] : 0;
}

const char *settings_scancode_name(int scancode) {
    const char *name = SDL_GetScancodeName((SDL_Scancode)scancode);

    return name != NULL && name[0] != 0 ? name : "?";
}

#define PAD_AX(axis, dir) (PAD_AXIS_BASE + (axis) * 2 + (dir))
#define PAD_BTN(button) ((button) + 1)

static const int defaultPads[IN_COUNT][KEY_SLOTS] = {
    [IN_A] = {PAD_BTN(SDL_CONTROLLER_BUTTON_A), 0},
    [IN_B] = {PAD_BTN(SDL_CONTROLLER_BUTTON_B), PAD_BTN(SDL_CONTROLLER_BUTTON_X)},
    [IN_Z] = {PAD_AX(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1), 0},
    [IN_START] = {PAD_BTN(SDL_CONTROLLER_BUTTON_START), 0},
    [IN_L] = {PAD_BTN(SDL_CONTROLLER_BUTTON_LEFTSHOULDER), 0},
    [IN_R] = {PAD_BTN(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER), PAD_AX(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1)},
    [IN_CUP] = {PAD_AX(SDL_CONTROLLER_AXIS_RIGHTY, 0), PAD_BTN(SDL_CONTROLLER_BUTTON_Y)},
    [IN_CDOWN] = {PAD_AX(SDL_CONTROLLER_AXIS_RIGHTY, 1), 0},
    [IN_CLEFT] = {PAD_AX(SDL_CONTROLLER_AXIS_RIGHTX, 0), 0},
    [IN_CRIGHT] = {PAD_AX(SDL_CONTROLLER_AXIS_RIGHTX, 1), 0},
    [IN_DUP] = {PAD_BTN(SDL_CONTROLLER_BUTTON_DPAD_UP), 0},
    [IN_DDOWN] = {PAD_BTN(SDL_CONTROLLER_BUTTON_DPAD_DOWN), 0},
    [IN_DLEFT] = {PAD_BTN(SDL_CONTROLLER_BUTTON_DPAD_LEFT), 0},
    [IN_DRIGHT] = {PAD_BTN(SDL_CONTROLLER_BUTTON_DPAD_RIGHT), 0},
    /* The left stick drives the stick analog. A binding here adds a full tilt. */
};

#define JOY_BTN(n) ((n) + 1)
#define JOY_AX(n, dir) (JOY_AXIS_BASE + (n) * 2 + (dir))
#define JOY_HAT(n, dir) (JOY_HAT_BASE + (n) * 4 + (dir))

/* The raphnet N64 adapter lists the buttons in this order. Hat 0 also drives the D-pad. */
static const int defaultJoys[IN_COUNT][KEY_SLOTS] = {
    [IN_A] = {JOY_BTN(0), 0},
    [IN_B] = {JOY_BTN(1), 0},
    [IN_Z] = {JOY_BTN(2), 0},
    [IN_START] = {JOY_BTN(3), 0},
    [IN_L] = {JOY_BTN(4), 0},
    [IN_R] = {JOY_BTN(5), 0},
    [IN_CUP] = {JOY_BTN(6), 0},
    [IN_CDOWN] = {JOY_BTN(7), 0},
    [IN_CLEFT] = {JOY_BTN(8), 0},
    [IN_CRIGHT] = {JOY_BTN(9), 0},
    [IN_DUP] = {JOY_BTN(10), JOY_HAT(0, JOY_HAT_UP)},
    [IN_DDOWN] = {JOY_BTN(11), JOY_HAT(0, JOY_HAT_DOWN)},
    [IN_DLEFT] = {JOY_BTN(12), JOY_HAT(0, JOY_HAT_LEFT)},
    [IN_DRIGHT] = {JOY_BTN(13), JOY_HAT(0, JOY_HAT_RIGHT)},
};

static const char *const buttonNames[SDL_CONTROLLER_BUTTON_MAX] = {
    "A", "B", "X", "Y", "Back", "Guide", "Start", "Left stick click", "Right stick click",
    "Left shoulder", "Right shoulder", "D-pad up", "D-pad down", "D-pad left", "D-pad right",
    "Misc", "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad",
};

static const char *const axisNames[SDL_CONTROLLER_AXIS_MAX][2] = {
    {"Left stick left", "Left stick right"}, {"Left stick up", "Left stick down"},
    {"Right stick left", "Right stick right"}, {"Right stick up", "Right stick down"},
    {"Left trigger", "Left trigger"}, {"Right trigger", "Right trigger"},
};

const char *settings_input_name(int input) {
    return input >= 0 && input < IN_COUNT ? inputNames[input] : "?";
}

void settings_defaults(Settings *st) {
    memset(st, 0, sizeof(*st));
    st->physicsMode = PHYSICS_ORIGINAL;
    st->vsync = 1;
    st->maxFps = 0;
    settings_default_keys(st);
    settings_default_pads(st);
    settings_default_joys(st);
    st->stickDead = STICK_DEAD_DEFAULT;
    st->stickRange = STICK_RANGE_DEFAULT;
    st->cameraFollow = 1.0f;
    st->volume = VOLUME_DEFAULT;
    st->musicVolume = 100;
    st->effectsVolume = 100;
    st->pauseCheats = 1;
    st->debugRange = DEBUG_RANGE_DEFAULT;
    st->collisionTypes = 0x1ff;
    st->stepKey = STEP_KEY_DEFAULT;
    st->pauseKey = PAUSE_KEY_DEFAULT;
    st->renderScale = RENDER_SCALE_DEFAULT;
    st->msaa = MSAA_DEFAULT;
    st->texFilter = TEXFILTER_N64;
    st->widescreen = WIDESCREEN_OFF;
    st->debugPad = DEBUGPAD_OFF;
    st->timerScale = TIMER_SCALE_DEFAULT;
}

void settings_default_pads(Settings *st) {
    memcpy(st->pads, defaultPads, sizeof(st->pads));
}

void settings_default_joys(Settings *st) {
    memcpy(st->joys, defaultJoys, sizeof(st->joys));
}

static int joy_code_valid(int c) {
    return c >= 0 && c < JOY_HAT_BASE + JOY_HAT_MAX * 4;
}

const char *settings_joy_name(int code) {
    static char text[4][24];
    static int next;
    static const char *const hatDirs[4] = {"Up", "Down", "Left", "Right"};
    char *out = text[next++ & 3];

    if (code >= JOY_HAT_BASE && code < JOY_HAT_BASE + JOY_HAT_MAX * 4) {
        snprintf(out, 24, "Hat %d %s", (code - JOY_HAT_BASE) / 4, hatDirs[(code - JOY_HAT_BASE) & 3]);
    } else if (code >= JOY_AXIS_BASE && code < JOY_AXIS_BASE + JOY_AXIS_MAX * 2) {
        snprintf(out, 24, "Axis %d%c", (code - JOY_AXIS_BASE) / 2, (code & 1) ? '+' : '-');
    } else if (code >= 1 && code <= JOY_BUTTON_MAX) {
        snprintf(out, 24, "Button %d", code - 1);
    } else {
        return "(none)";
    }
    return out;
}

int settings_joy_code_from_button(int button) {
    return button >= 0 && button < JOY_BUTTON_MAX ? JOY_BTN(button) : 0;
}

int settings_joy_code_from_axis(int axis, int v) {
    if (axis < 0 || axis >= JOY_AXIS_MAX || (v > -20000 && v < 20000)) {
        return 0;
    }
    return JOY_AX(axis, v > 0);
}

int settings_joy_code_from_hat(int hat, int value) {
    int dir;

    if (hat < 0 || hat >= JOY_HAT_MAX) {
        return 0;
    }
    dir = value == SDL_HAT_UP ? JOY_HAT_UP : value == SDL_HAT_DOWN ? JOY_HAT_DOWN
        : value == SDL_HAT_LEFT ? JOY_HAT_LEFT : value == SDL_HAT_RIGHT ? JOY_HAT_RIGHT : -1;
    return dir < 0 ? 0 : JOY_HAT(hat, dir);
}

static int joy_code_down(SDL_Joystick *joy, int code) {
    if (code >= JOY_HAT_BASE && code < JOY_HAT_BASE + JOY_HAT_MAX * 4) {
        static const int mask[4] = {SDL_HAT_UP, SDL_HAT_DOWN, SDL_HAT_LEFT, SDL_HAT_RIGHT};

        return (SDL_JoystickGetHat(joy, (code - JOY_HAT_BASE) / 4) & mask[(code - JOY_HAT_BASE) & 3]) != 0;
    }
    if (code >= JOY_AXIS_BASE && code < JOY_AXIS_BASE + JOY_AXIS_MAX * 2) {
        int v = SDL_JoystickGetAxis(joy, (code - JOY_AXIS_BASE) / 2);

        return (code & 1) ? v > 16000 : v < -16000;
    }
    if (code >= 1 && code <= JOY_BUTTON_MAX) {
        return SDL_JoystickGetButton(joy, code - 1) != 0;
    }
    return 0;
}

int settings_joy_down(const Settings *st, void *joy, int input) {
    int s;

    for (s = 0; s < KEY_SLOTS; s++) {
        if (joy_code_down((SDL_Joystick *)joy, st->joys[input][s])) {
            return 1;
        }
    }
    return 0;
}

const char *settings_pad_name(int code) {
    if (code >= PAD_AXIS_BASE && code < PAD_AXIS_BASE + SDL_CONTROLLER_AXIS_MAX * 2) {
        return axisNames[(code - PAD_AXIS_BASE) / 2][(code - PAD_AXIS_BASE) & 1];
    }
    if (code >= 1 && code <= SDL_CONTROLLER_BUTTON_MAX) {
        return buttonNames[code - 1];
    }
    return "(none)";
}

int settings_pad_code_from_axis(int axis, int v) {
    if (axis < 0 || axis >= SDL_CONTROLLER_AXIS_MAX || (v > -20000 && v < 20000)) {
        return 0;
    }
    return PAD_AX(axis, v > 0);
}

static int pad_code_down(SDL_GameController *pad, int code) {
    if (code >= PAD_AXIS_BASE && code < PAD_AXIS_BASE + SDL_CONTROLLER_AXIS_MAX * 2) {
        int v = SDL_GameControllerGetAxis(pad, (SDL_GameControllerAxis)((code - PAD_AXIS_BASE) / 2));

        return (code & 1) ? v > 16000 : v < -16000;
    }
    if (code >= 1 && code <= SDL_CONTROLLER_BUTTON_MAX) {
        return SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)(code - 1)) != 0;
    }
    return 0;
}

int settings_pad_down(const Settings *st, void *pad, int input) {
    int s;

    for (s = 0; s < KEY_SLOTS; s++) {
        if (pad_code_down((SDL_GameController *)pad, st->pads[input][s])) {
            return 1;
        }
    }
    return 0;
}

/* The stick is read as one round control. The dead zone is a circle (stickDead percent of the travel), so a
   stick that rests off centre or slips sideways gives no sideways drift. From the dead zone to stickRange percent
   the length grows in a straight line from 6 to 80. The game ignores up to 5 on each axis and clamps the rest at
   79, so the first value that moves the glove is 6 and full tilt is the N64's 80. The direction is kept. */
void settings_stick(const Settings *st, int rawX, int rawY, int *outX, int *outY) {
    double fx = rawX / 32768.0;
    double fy = rawY / 32768.0;
    double len = sqrt(fx * fx + fy * fy);
    double dead = st->stickDead / 100.0;
    double range = st->stickRange / 100.0;
    double t, mag;

    *outX = 0;
    *outY = 0;
    if (range < dead + 0.05) {
        range = dead + 0.05;
    }
    if (len <= dead || len == 0.0) {
        return;
    }
    t = (len - dead) / (range - dead);
    if (t > 1.0) {
        t = 1.0;
    }
    mag = 6.0 + t * (80.0 - 6.0);
    *outX = (int)lround(fx / len * mag);
    *outY = (int)lround(fy / len * mag);
}

void settings_default_keys(Settings *st) {
    memcpy(st->keys, defaultKeys, sizeof(st->keys));
}

void settings_path(char *out, int size) {
    char *pref = SDL_GetPrefPath("weemitt", "Glover");

    snprintf(out, size, "%ssettings.ini", pref ? pref : "");
    if (pref == NULL) {
        out[0] = 0;
    } else {
        SDL_free(pref);
    }
}

int settings_input_down(const Settings *st, const unsigned char *keys, int input) {
    int s;

    for (s = 0; s < KEY_SLOTS; s++) {
        int k = st->keys[input][s];

        if (k > 0 && k < SDL_NUM_SCANCODES && keys[k]) {
            return 1;
        }
    }
    return 0;
}

static int input_by_name(const char *name) {
    int i;

    for (i = 0; i < IN_COUNT; i++) {
        if (!strcmp(name, inputNames[i])) {
            return i;
        }
    }
    return -1;
}

void settings_load(Settings *st, const char *path) {
    FILE *f;
    char line[256];

    settings_defaults(st);
    if (path == NULL || path[0] == 0 || (f = fopen(path, "r")) == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *key = line;
        char *val = line;
        int in;

        /* The line is "name=value". The end of line and a comment's first character stop the scan. */
        while (*val != 0 && *val != '=' && *val != '\n' && *val != '\r') {
            val++;
        }
        if (*val != '=' || line[0] == '#' || line[0] == '[') {
            continue;
        }
        *val++ = 0;
        if (!strcmp(key, "physics_mode")) {
            st->physicsMode = atoi(val) == PHYSICS_SMOOTH ? PHYSICS_SMOOTH : PHYSICS_ORIGINAL;
        } else if (!strcmp(key, "vsync")) {
            st->vsync = atoi(val) != 0;
        } else if (!strcmp(key, "max_fps")) {
            int v = atoi(val);

            st->maxFps = v <= 0 ? 0 : v < FPS_LIMIT_MIN ? FPS_LIMIT_MIN : v > FPS_LIMIT_MAX ? 0 : v;
        } else if (!strcmp(key, "ballworld")) {
            st->ballworld = atoi(val) != 0;
        } else if (!strcmp(key, "easy_ball")) {
            st->easyBall = atoi(val) != 0;
        } else if (!strcmp(key, "master_volume")) {
            int v = atoi(val);

            st->volume = v < 0 ? 0 : v > 100 ? 100 : v;
        } else if (!strcmp(key, "music_volume")) {
            int v = atoi(val);

            st->musicVolume = v < 0 ? 0 : v > 100 ? 100 : v;
        } else if (!strcmp(key, "effects_volume")) {
            int v = atoi(val);

            st->effectsVolume = v < 0 ? 0 : v > 100 ? 100 : v;
        } else if (!strcmp(key, "mute_unfocused")) {
            st->muteUnfocused = atoi(val) != 0;
        } else if (!strcmp(key, "skip_intro")) {
            st->skipIntro = atoi(val) != 0;
        } else if (!strcmp(key, "infinite_lives")) {
            st->infiniteLives = atoi(val) != 0;
        } else if (!strcmp(key, "infinite_health")) {
            st->infiniteHealth = atoi(val) != 0;
        } else if (!strcmp(key, "unlock_all")) {
            st->unlockAll = atoi(val) != 0;
        } else if (!strcmp(key, "level_select")) {
            st->levelSelect = atoi(val) != 0;
        } else if (!strcmp(key, "pause_cheats")) {
            st->pauseCheats = atoi(val) != 0;
        } else if (!strcmp(key, "camera_follow")) {
            double v = atof(val);

            st->cameraFollow = v < 1.0 ? 1.0f : v > 3.0 ? 3.0f : (float)v;
        } else if (!strcmp(key, "log_file")) {
            st->logFile = atoi(val) != 0;
        } else if (!strcmp(key, "log_verbose")) {
            st->logVerbose = atoi(val) != 0;
        } else if (!strcmp(key, "record_input")) {
            st->recordInput = atoi(val) != 0;
        } else if (!strcmp(key, "debug_range")) {
            int v = atoi(val);

            st->debugRange = v < DEBUG_RANGE_MIN ? DEBUG_RANGE_MIN : v > DEBUG_RANGE_MAX ? DEBUG_RANGE_MAX : v;
        } else if (!strcmp(key, "collision_types")) {
            st->collisionTypes = (int)(strtol(val, NULL, 0) & 0x1ff);
        } else if (!strcmp(key, "step_key")) {
            st->stepKey = atoi(val) > 0 ? atoi(val) : 0;
        } else if (!strcmp(key, "pause_key")) {
            st->pauseKey = atoi(val) > 0 ? atoi(val) : 0;
        } else if (!strcmp(key, "render_scale")) {
            int v = atoi(val);

            st->renderScale = v < RENDER_SCALE_MIN ? RENDER_SCALE_MIN : v > RENDER_SCALE_MAX ? RENDER_SCALE_MAX : v;
        } else if (!strcmp(key, "msaa")) {
            int v = atoi(val);

            st->msaa = v >= 8 ? 8 : v >= 4 ? 4 : v >= 2 ? 2 : 0;
        } else if (!strcmp(key, "texture_filter")) {
            int v = atoi(val);

            st->texFilter = v == TEXFILTER_BILINEAR || v == TEXFILTER_NEAREST ? v : TEXFILTER_N64;
        } else if (!strcmp(key, "widescreen")) {
            int v = atoi(val);

            st->widescreen = v == WIDESCREEN_STRETCH || v == WIDESCREEN_HORPLUS ? v : WIDESCREEN_OFF;
        } else if (!strcmp(key, "stick_dead")) {
            int v = atoi(val);

            st->stickDead = v < 0 ? 0 : v > 50 ? 50 : v;
        } else if (!strcmp(key, "stick_range")) {
            int v = atoi(val);

            st->stickRange = v < 50 ? 50 : v > 100 ? 100 : v;
        } else if (!strncmp(key, "pad.", 4) && (in = input_by_name(key + 4)) >= 0) {
            int s;

            for (s = 0; s < KEY_SLOTS; s++) {
                int c = atoi(val);

                st->pads[in][s] = c >= 0 && c < PAD_AXIS_BASE + SDL_CONTROLLER_AXIS_MAX * 2 ? c : 0;
                while (*val != 0 && *val != '|') {
                    val++;
                }
                if (*val == '|') {
                    val++;
                }
            }
        } else if (!strncmp(key, "joy.", 4) && (in = input_by_name(key + 4)) >= 0) {
            int s;

            for (s = 0; s < KEY_SLOTS; s++) {
                int c = atoi(val);

                st->joys[in][s] = joy_code_valid(c) ? c : 0;
                while (*val != 0 && *val != '|') {
                    val++;
                }
                if (*val == '|') {
                    val++;
                }
            }
        } else if (!strncmp(key, "key.", 4) && (in = input_by_name(key + 4)) >= 0) {
            /* Keys are SDL scancode numbers, 0 for an unbound slot, separated by a bar. */
            int s;

            for (s = 0; s < KEY_SLOTS; s++) {
                int sc = atoi(val);

                st->keys[in][s] = sc > 0 && sc < SDL_NUM_SCANCODES ? sc : 0;
                while (*val != 0 && *val != '|') {
                    val++;
                }
                if (*val == '|') {
                    val++;
                }
            }
        }
    }
    fclose(f);
}

int settings_save(const Settings *st, const char *path) {
    FILE *f;
    int i;
    int s;

    if (path == NULL || path[0] == 0 || (f = fopen(path, "w")) == NULL) {
        return -1;
    }
    fprintf(f, "# weemitt settings. The menu (Escape) writes this file.\n# Keys are SDL scancode numbers (4 is A, 44 is Space); 0 means no key.\n[graphics]\nphysics_mode=%d\nvsync=%d\nmax_fps=%d\n[enhancements]\neasy_ball=%d\nballworld=%d\ncamera_follow=%.2f\n[audio]\nmaster_volume=%d\n[developer]\nlog_file=%d\nlog_verbose=%d\nrecord_input=%d\n[controller]\nstick_dead=%d\nstick_range=%d\n[keys]\n",
            st->physicsMode, st->vsync, st->maxFps, st->easyBall, st->ballworld, (double)st->cameraFollow, st->volume, st->logFile, st->logVerbose, st->recordInput, st->stickDead, st->stickRange);
    fprintf(f, "[play]\nmusic_volume=%d\neffects_volume=%d\nmute_unfocused=%d\nskip_intro=%d\ninfinite_lives=%d\ninfinite_health=%d\nunlock_all=%d\nlevel_select=%d\npause_cheats=%d\n",
            st->musicVolume, st->effectsVolume, st->muteUnfocused, st->skipIntro, st->infiniteLives, st->infiniteHealth,
            st->unlockAll, st->levelSelect, st->pauseCheats);
    for (i = 0; i < IN_COUNT; i++) {
        fprintf(f, "key.%s=", inputNames[i]);
        for (s = 0; s < KEY_SLOTS; s++) {
            int k = st->keys[i][s];

            fprintf(f, "%s%d", s ? "|" : "", k);
        }
        fputc('\n', f);
    }
    fprintf(f, "[image]\n# render_scale: 50 to 400 (percent). msaa: 0, 2, 4 or 8. texture_filter: 0 N64, 1 bilinear, 2 nearest.\n"
               "# widescreen: 0 off (bars), 1 stretch, 2 extend the view (Hor+).\n"
               "render_scale=%d\nmsaa=%d\ntexture_filter=%d\nwidescreen=%d\n",
            st->renderScale, st->msaa, st->texFilter, st->widescreen);
    fprintf(f, "[pads]\n# 0 none, 1 + SDL button, 100 + 2 * SDL axis (+1 for the positive direction).\n");
    for (i = 0; i < IN_COUNT; i++) {
        fprintf(f, "pad.%s=", inputNames[i]);
        for (s = 0; s < KEY_SLOTS; s++) {
            fprintf(f, "%s%d", s ? "|" : "", st->pads[i][s]);
        }
        fputc('\n', f);
    }
    fprintf(f, "[joys]\n# A joystick without an SDL mapping. 0 none, 1 + button, 100 + 2 * axis (+1 for positive), 200 + 4 * hat + direction (0 up, 1 down, 2 left, 3 right).\n");
    for (i = 0; i < IN_COUNT; i++) {
        fprintf(f, "joy.%s=", inputNames[i]);
        for (s = 0; s < KEY_SLOTS; s++) {
            fprintf(f, "%s%d", s ? "|" : "", st->joys[i][s]);
        }
        fputc('\n', f);
    }
    fprintf(f, "[debug]\n# SDL scancodes of the frame step keys (0 for none) and the range of the overlays in game units.\nstep_key=%d\npause_key=%d\ndebug_range=%d\n# The kinds of collision lines shown, as bits: floor 1, wall 2, ceiling 4, platform 8, water 16, hurts 32,\n# glove 64, ball 128, enemy 256.\ncollision_types=0x%x\n",
            st->stepKey, st->pauseKey, st->debugRange, (unsigned int)st->collisionTypes);
    return fclose(f);
}
