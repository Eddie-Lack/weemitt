/* The menu for builds without a window (make headless): nothing to open, nothing to draw. */
#include "menu.h"

void menu_init(SDL_Window *window, SDL_GLContext gl, Settings *st) {}
void menu_shutdown(void) {}
int menu_handle_event(const SDL_Event *e) { return 0; }
int menu_is_open(void) { return 0; }
void menu_draw(SDL_Window *window) {}
