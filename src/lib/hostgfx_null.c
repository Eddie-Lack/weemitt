/* glover_host_gfx in a build without src/gfx (RENDERER=null): GLOVER_ERR_UNSUPPORTED. */
#include "lib.h"

int lib_host_interp;

void lib_host_record(const PortFrameTask *tasks, int count, int cut) {
    (void)tasks;
    (void)count;
    (void)cut;
}

void lib_host_shutdown(void) {
}

GLOVER_API int32_t glover_host_gfx(Glover *g, GloverHostGfx *h) {
    (void)g;
    (void)h;
    return GLOVER_ERR_UNSUPPORTED;
}
