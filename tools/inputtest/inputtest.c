/* Checks the input latch (src/port/inputlatch.c) and the event mapping (src/port/inputevent.c) without the game.
   The latch test models the window loop: drawn frames of changing length, split into updates of at most one
   original frame, one game read per original frame. A button goes down for one original frame at a spacing. */
#include <stdio.h>
#include <string.h>
#include "../../src/port/inputevent.h"
#include "../../src/port/inputlatch.h"

static int failures;

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            printf("FAIL line %d: ", __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
            failures++; \
        } \
    } while (0)

static void latch_reset(void) {
    port_buttons_latched = port_buttons_read = port_buttons_up = 0;
}

/* The old rule, for comparison: no release tracking. */
static unsigned short old_latched, old_read;
static unsigned short old_update_read(unsigned short b, int read) {
    old_latched |= b & ~old_read;
    if (read) {
        old_read = b | old_latched;
        old_latched = 0;
        return old_read;
    }
    return 0xffff;
}

/* One run. jitter is the drawn frame's length error (0.05: 1.05 and 0.95 original frames). Smooth mode reads at the
   first update of a window. taps: the button is pressed for one drawn frame, `spacing` original frames apart.
   mode 0: the state is sampled once per drawn frame (the keyboard state). mode 1: the press is an event that
   makes the update see it down for one update (tapPending), and the state stays up. */
static void run(double jitter, int spacing, int mode, int *pressesOut, int *seenNew, int *seenOld) {
    unsigned seed = 1;
    double pending = 0.0, window = 0.0;
    long frame = 0;
    int presses = 0;
    int prevDown = 0;
    unsigned short prevNew = 0, prevOld = 0;

    latch_reset();
    old_latched = old_read = 0;
    *seenNew = *seenOld = 0;
    for (frame = 0; frame < 400 && presses < 100;) {
        double len = 1.0;
        unsigned short state, tap = 0;
        int down = frame >= 20 && (frame - 20) % spacing == 0;

        seed = seed * 1103515245u + 12345u;
        if (jitter > 0.0) {
            len = 1.0 * (1.0 + jitter * ((double)((seed >> 16) & 0x7fff) / 16383.5 - 1.0));
        }
        state = (mode == 0 && down) ? 0x20 : 0;
        if (mode == 1 && down) {
            tap = 0x20;
        }
        if (down && !prevDown) {
            presses++;
        }
        prevDown = down;
        pending = len;
        while (pending > 1e-7) {
            double step = pending > 1.0 ? 1.0 : pending;
            int read = window < 1e-7;
            unsigned short b = state | tap;
            unsigned short w, o;

            tap = 0;
            pending -= step;
            port_input_update(b);
            o = old_update_read(b, read);
            if (read) {
                w = port_input_read(b);
                if ((w & 0x20) && !(prevNew & 0x20)) {
                    (*seenNew)++;
                }
                prevNew = w;
                if ((o & 0x20) && !(prevOld & 0x20)) {
                    (*seenOld)++;
                }
                prevOld = o;
            }
            window += step;
            if (window > 1.0 - 1e-7) {
                window = 0.0;
                frame++;
            }
        }
    }
    *pressesOut = presses;
}

static void test_latch(void) {
    static const double jitters[] = {0.0, 0.05, 0.1, 0.3};
    int j, spacing, mode;

    for (mode = 0; mode < 2; mode++) {
        for (j = 0; j < 4; j++) {
            for (spacing = 2; spacing <= 4; spacing++) {
                int n, newSeen, oldSeen;

                run(jitters[j], spacing, mode, &n, &newSeen, &oldSeen);
                printf("latch mode %d jitter %.2f spacing %d: %d presses, new rule %d seen, old rule %d seen\n", mode,
                       jitters[j], spacing, n, newSeen, oldSeen);
                CHECK(newSeen >= oldSeen, "the new rule saw fewer presses");
                if (jitters[j] == 0.0) {
                    CHECK(newSeen == n && oldSeen == n, "at whole frames every press must be seen");
                }
            }
        }
    }
}

static void test_events(void) {
    Settings st;
    SDL_Event e;

    settings_defaults(&st);
    memset(&e, 0, sizeof(e));
    e.type = SDL_KEYDOWN;
    e.key.keysym.scancode = (SDL_Scancode)st.keys[IN_L][0];
    CHECK(input_event_buttons(&st, &e, 1, 0, 0) == BTN_L, "L key down");
    CHECK(input_event_buttons(&st, &e, 0, 0, 0) == 0, "keyboard off (menu open)");
    e.key.repeat = 1;
    CHECK(input_event_buttons(&st, &e, 1, 0, 0) == 0, "key repeat");
    e.key.repeat = 0;
    e.type = SDL_KEYUP;
    CHECK(input_event_buttons(&st, &e, 1, 0, 0) == 0, "key up");
    e.type = SDL_KEYDOWN;
    e.key.keysym.scancode = SDL_SCANCODE_F9;
    CHECK(input_event_buttons(&st, &e, 1, 0, 0) == 0, "unbound key");

    memset(&e, 0, sizeof(e));
    e.type = SDL_CONTROLLERBUTTONDOWN;
    e.cbutton.button = SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
    CHECK(input_event_buttons(&st, &e, 0, 1, 0) == BTN_L, "pad left shoulder");
    CHECK(input_event_buttons(&st, &e, 0, 0, 0) == 0, "pad button, pad not in use");
    e.type = SDL_CONTROLLERAXISMOTION;
    e.caxis.axis = SDL_CONTROLLER_AXIS_TRIGGERLEFT;
    e.caxis.value = 10000;
    CHECK(input_event_buttons(&st, &e, 0, 1, 0) == 0, "trigger below the threshold");
    e.caxis.value = 20000;
    CHECK(input_event_buttons(&st, &e, 0, 1, 0) == BTN_Z, "trigger crosses");
    e.caxis.value = 30000;
    CHECK(input_event_buttons(&st, &e, 0, 1, 0) == 0, "trigger goes on");
    e.caxis.value = 0;
    CHECK(input_event_buttons(&st, &e, 0, 1, 0) == 0, "trigger released");
    e.caxis.value = 20000;
    CHECK(input_event_buttons(&st, &e, 0, 1, 0) == BTN_Z, "trigger crosses again");

    memset(&e, 0, sizeof(e));
    e.type = SDL_JOYBUTTONDOWN;
    e.jbutton.button = 4;
    CHECK(input_event_buttons(&st, &e, 0, 0, 1) == BTN_L, "joystick button 4 is L");
    CHECK(input_event_buttons(&st, &e, 0, 1, 0) == 0, "joystick button, joystick not in use");
    e.type = SDL_JOYHATMOTION;
    e.jhat.hat = 0;
    e.jhat.value = SDL_HAT_UP;
    CHECK(input_event_buttons(&st, &e, 0, 0, 1) == BTN_DUP, "hat up");
    e.jhat.value = SDL_HAT_UP | SDL_HAT_RIGHT;
    CHECK(input_event_buttons(&st, &e, 0, 0, 1) == BTN_DRIGHT, "hat up and right: only right is new");
    e.jhat.value = SDL_HAT_CENTERED;
    CHECK(input_event_buttons(&st, &e, 0, 0, 1) == 0, "hat centred");
}

int main(void) {
    test_latch();
    test_events();
    printf(failures ? "%d FAILED\n" : "all input checks passed\n", failures);
    return failures != 0;
}
