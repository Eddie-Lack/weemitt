/* Loads the OpenGL functions the renderer uses (gfx_gl.h). */
#include "gfx_host.h"

#include "gfx_gl.h"

#define GFX_GL_DEFINE(ret, name, args) gfx_PFN_##name gfx_gl##name;
GFX_GL_FUNCTIONS(GFX_GL_DEFINE)
#undef GFX_GL_DEFINE

int gfx_gl_load(void) {
    int missing = 0;

#define GFX_GL_FETCH(ret, name, args) \
    gfx_gl##name = (gfx_PFN_##name)gfx_gl_proc("gl" #name); \
    if (gfx_gl##name == NULL) { \
        gfx_log("gfx: OpenGL function gl%s is missing", #name); \
        missing++; \
    }
    GFX_GL_FUNCTIONS(GFX_GL_FETCH)
#undef GFX_GL_FETCH

    return missing ? -1 : 0;
}
