/* See inputevent.h. */
#include "inputevent.h"

#define AXIS_PRESS 16000 /* the same threshold as settings.c uses for a held axis */

static const unsigned short buttonOf[IN_DRIGHT + 1] = {
    [IN_A] = BTN_A, [IN_B] = BTN_B, [IN_Z] = BTN_Z, [IN_START] = BTN_START, [IN_L] = BTN_L, [IN_R] = BTN_R,
    [IN_CUP] = BTN_CUP, [IN_CDOWN] = BTN_CDOWN, [IN_CLEFT] = BTN_CLEFT, [IN_CRIGHT] = BTN_CRIGHT,
    [IN_DUP] = BTN_DUP, [IN_DDOWN] = BTN_DDOWN, [IN_DLEFT] = BTN_DLEFT, [IN_DRIGHT] = BTN_DRIGHT,
};

/* Which side of its threshold each axis was on at its latest event: -1, 0 or 1. */
static signed char padAxisSide[SDL_CONTROLLER_AXIS_MAX];
static signed char joyAxisSide[JOY_AXIS_MAX];
static unsigned char joyHat[JOY_HAT_MAX];

unsigned short input_button(int input) {
    return input >= 0 && input <= IN_DRIGHT ? buttonOf[input] : 0;
}

/* The buttons of the inputs that have the code in one of their two slots. */
static unsigned short match(const int (*table)[KEY_SLOTS], int code) {
    unsigned short b = 0;
    int i;
    int s;

    if (code == 0) {
        return 0;
    }
    for (i = 0; i <= IN_DRIGHT; i++) {
        for (s = 0; s < KEY_SLOTS; s++) {
            if (table[i][s] == code) {
                b |= buttonOf[i];
            }
        }
    }
    return b;
}

/* Moves an axis to value v. Returns the code of the direction that it just passed the threshold in, or 0. */
static int axis_press(signed char *side, int base, int axis, int v) {
    int now = v > AXIS_PRESS ? 1 : v < -AXIS_PRESS ? -1 : 0;
    int was = *side;

    *side = (signed char)now;
    return now != 0 && now != was ? base + axis * 2 + (now > 0) : 0;
}

unsigned short input_event_buttons(const Settings *st, const SDL_Event *e, int keyboard, int pad, int joy) {
    int code;

    switch (e->type) {
    case SDL_KEYDOWN:
        return keyboard && !e->key.repeat ? match(st->keys, e->key.keysym.scancode) : 0;
    case SDL_CONTROLLERBUTTONDOWN:
        return pad ? match(st->pads, e->cbutton.button + 1) : 0;
    case SDL_CONTROLLERAXISMOTION:
        if (e->caxis.axis >= SDL_CONTROLLER_AXIS_MAX) {
            return 0;
        }
        code = axis_press(&padAxisSide[e->caxis.axis], PAD_AXIS_BASE, e->caxis.axis, e->caxis.value);
        return pad ? match(st->pads, code) : 0;
    case SDL_JOYBUTTONDOWN:
        return joy && e->jbutton.button < JOY_BUTTON_MAX ? match(st->joys, e->jbutton.button + 1) : 0;
    case SDL_JOYAXISMOTION:
        if (e->jaxis.axis >= JOY_AXIS_MAX) {
            return 0;
        }
        code = axis_press(&joyAxisSide[e->jaxis.axis], JOY_AXIS_BASE, e->jaxis.axis, e->jaxis.value);
        return joy ? match(st->joys, code) : 0;
    case SDL_JOYHATMOTION: {
        static const unsigned char dirMask[4] = {SDL_HAT_UP, SDL_HAT_DOWN, SDL_HAT_LEFT, SDL_HAT_RIGHT};
        unsigned char added;
        unsigned short b = 0;
        int d;

        if (e->jhat.hat >= JOY_HAT_MAX) {
            return 0;
        }
        added = e->jhat.value & ~joyHat[e->jhat.hat];
        joyHat[e->jhat.hat] = e->jhat.value;
        for (d = 0; joy && d < 4; d++) {
            if (added & dirMask[d]) {
                b |= match(st->joys, JOY_HAT_BASE + e->jhat.hat * 4 + d);
            }
        }
        return b;
    }
    default:
        return 0;
    }
}
