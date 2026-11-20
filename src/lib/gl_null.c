/* The renderer's calls in a build without the renderer (make lib64-stub): each returns GLOVER_ERR_UNSUPPORTED. */
#include "lib.h"

int lib_widescreen;

GLOVER_API int32_t glover_gl_init(Glover *g, GloverGlLoader load, void *user) {
    (void)g;
    (void)load;
    (void)user;
    return GLOVER_ERR_UNSUPPORTED;
}

GLOVER_API int32_t glover_gl_options(Glover *g, const GloverGlOptions *o) {
    (void)g;
    (void)o;
    return GLOVER_ERR_UNSUPPORTED;
}

GLOVER_API int32_t glover_gl_draw(Glover *g, uint32_t fbo, int32_t w, int32_t h) {
    (void)g;
    (void)fbo;
    (void)w;
    (void)h;
    return GLOVER_ERR_UNSUPPORTED;
}

GLOVER_API int32_t glover_gl_read_frame(Glover *g, uint8_t *rgba, uint32_t bytes, int32_t *w, int32_t *h) {
    (void)g;
    (void)rgba;
    (void)bytes;
    (void)w;
    (void)h;
    return GLOVER_ERR_UNSUPPORTED;
}

GLOVER_API int32_t glover_gl_shutdown(Glover *g) {
    (void)g;
    return GLOVER_ERR_UNSUPPORTED;
}

GLOVER_API int32_t glover_gl_draw_layer(Glover *g, GloverGlLayer *l) {
    (void)g;
    (void)l;
    return GLOVER_ERR_UNSUPPORTED;
}

GLOVER_API int32_t glover_gl_composite(Glover *g, uint32_t fbo, int32_t w, int32_t h, float bias, uint32_t flags) {
    (void)g;
    (void)fbo;
    (void)w;
    (void)h;
    (void)bias;
    (void)flags;
    return GLOVER_ERR_UNSUPPORTED;
}
