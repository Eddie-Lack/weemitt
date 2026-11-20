/* What the renderer needs from the program around it: a log, the environment, a clock and the OpenGL functions.
   gfx_host.c gives them without SDL; the window program and the library pass the OpenGL loader. */
#ifndef GFX_HOST_H
#define GFX_HOST_H

#include <stdint.h>

void gfx_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* getenv, for the renderer's test variables. NULL in the library (PORT_LIB): a host's environment does not change
   the picture. */
const char *gfx_getenv(const char *name);
uint64_t gfx_ticks(void);
uint64_t gfx_ticks_per_second(void);
/* The function that finds an OpenGL function by name (SDL_GL_GetProcAddress in the window program). Set it before
   gfx_init. */
void gfx_set_loader(void *(*load)(const char *name));
void *gfx_gl_proc(const char *name);

#endif
