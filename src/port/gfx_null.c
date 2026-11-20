/* A renderer that draws nothing, for builds and tests without the real one (src/gfx). */
#include <stddef.h>
#include "gfx.h"
#include "gfx_host.h"

int gfx_init(void) {
    return 0;
}

void gfx_set_loader(void *(*load)(const char *name)) {
}

void gfx_set_options(const GfxOptions *options) {
}

void gfx_set_drawable_size(int drawableW, int drawableH) {
}

int gfx_max_samples(void) {
    return 0;
}

void gfx_set_framebuffers(unsigned int cfb0, unsigned int cfb1) {
}

void gfx_set_memory_base(uintptr_t base) {
}

void gfx_set_memory_range(unsigned int lo, unsigned int hi) {
}

void gfx_draw_frame(const GfxTask *tasks, int count, int drawableW, int drawableH) {
}

unsigned char *gfx_read_frame(int *w, int *h) {
    *w = 0;
    *h = 0;
    return NULL;
}

unsigned char *gfx_read_window(int *w, int *h) {
    *w = 0;
    *h = 0;
    return NULL;
}

void gfx_shutdown(void) {
}

void gfx_interp_record(const GfxTask *tasks, int count, int cut) {
}

void gfx_interp_set(float t) {
}

void gfx_interp_set_volatile(unsigned int lo, unsigned int hi) {
}

void gfx_interp_stats(GfxInterpStats *s) {
    s->total = s->blended = s->moved = s->loose = s->noPartner = s->cut = s->rects = s->rectsBlended = 0;
}

int gfx_scene_view(GfxSceneView *v) {
    v->valid = 0;
    return 0;
}

int gfx_scene_project(const GfxSceneView *v, float x, float y, float z, float *sx, float *sy, float *depth) {
    return 0;
}

void gfx_debug_lines(const GfxDebugLine *lines, int count, int depth) {
}
