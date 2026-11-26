/* The in-game menu (Dear ImGui), opened with Escape. The platform layer calls these from its window loop.
   The interface is plain C. Headless builds link menu_null.c, which does nothing. */
#ifndef PORT_MENU_H
#define PORT_MENU_H

#include <SDL2/SDL.h>
#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The input device in use, which the platform layer provides. The name is copied into name (empty for none). */
enum { PORT_DEVICE_NONE, PORT_DEVICE_CONTROLLER, PORT_DEVICE_JOYSTICK };
int port_input_device(char *name, int size);
/* The same for the second device, which the debug pad uses when it is set to the second controller. */
int port_debug_device(char *name, int size);

/* The menu edits st, which the platform layer reads, and saves it to settings.ini. */
void menu_init(SDL_Window *window, SDL_GLContext gl, Settings *st);
void menu_shutdown(void);
/* Gives the menu an event. Returns 1 when the menu used it and the platform layer should ignore it. */
int menu_handle_event(const SDL_Event *e);
/* True while the menu is open: the game gets no keyboard input. */
int menu_is_open(void);
/* Draws the menu over the finished game frame. Call it before the buffer swap. */
void menu_draw(SDL_Window *window);
/* Shows a line of text at the top of the window for a few seconds, also while the menu is closed. */
void menu_notice(const char *text);
/* The settings changed outside the menu (the window's size and place, full screen): save them with the menu's. */
void menu_settings_changed(void);
/* Why there is no sound, for the Audio window (empty when the sound works or was turned off). The platform layer
   provides it. */
const char *port_sound_problem(void);
/* Full screen: on or off, and the choice is kept in the settings. The platform layer provides them. */
int port_fullscreen(void);
void port_set_fullscreen(int on);

#ifdef __cplusplus
}
#endif

#endif
