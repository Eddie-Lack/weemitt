/* Presses as SDL reports them. The window loop reads the keyboard and the controller as a state once per drawn
   frame. A key or button that goes down and up between two reads leaves no trace in that state. The events do. */
#ifndef PORT_INPUTEVENT_H
#define PORT_INPUTEVENT_H

#include <SDL2/SDL.h>
#include "settings.h"

/* N64 controller buttons (OSContPad.button). */
enum {
    BTN_A = 0x8000, BTN_B = 0x4000, BTN_Z = 0x2000, BTN_START = 0x1000,
    BTN_DUP = 0x0800, BTN_DDOWN = 0x0400, BTN_DLEFT = 0x0200, BTN_DRIGHT = 0x0100,
    BTN_L = 0x0020, BTN_R = 0x0010, BTN_CUP = 0x0008, BTN_CDOWN = 0x0004, BTN_CLEFT = 0x0002, BTN_CRIGHT = 0x0001,
};

/* The N64 button for an input (IN_A to IN_DRIGHT), 0 for the stick directions. */
unsigned short input_button(int input);
/* The buttons that a press event drives through the current bindings: a key going down (not a repeat), a game
   controller button or a raw joystick button going down, an axis moving past the same threshold that the state
   read uses, or a hat direction coming on. keyboard, pad and joy say which devices count. Other events and
   releases give 0. The function remembers each axis and hat, so give it every event of the devices. */
unsigned short input_event_buttons(const Settings *st, const SDL_Event *e, int keyboard, int pad, int joy);

#endif
