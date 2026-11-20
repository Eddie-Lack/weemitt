/* A camera set by the host (glover.h, docs/host-world.md). src/port/hostworld.c holds the view; its hooks build
   the game's projection and view from it, and move the game's camera to it. */
#include <string.h>
#include "lib.h"
#include "hostworld.h"

typedef struct ViewCall {
    const GloverView *view;
    int32_t rc;
} ViewCall;

static void call_set_view(void *arg) {
    ViewCall *c = (ViewCall *)arg;
    HwView v;

    if (c->view == NULL) {
        c->rc = hw_set_view(NULL);
        return;
    }
    /* The host's aspect is in the projection: the renderer's wider view would widen the picture a second time. */
    if ((c->view->flags & GLOVER_VIEW_DRAW) && lib_widescreen == 2) {
        c->rc = GLOVER_ERR_ARG;
        return;
    }
    v.flags = c->view->flags;
    memcpy(v.eye, c->view->eye, sizeof(v.eye));
    memcpy(v.at, c->view->at, sizeof(v.at));
    memcpy(v.up, c->view->up, sizeof(v.up));
    v.fovy = c->view->fovy;
    v.aspect = c->view->aspect;
    v.nearZ = c->view->nearZ;
    v.farZ = c->view->farZ;
    c->rc = hw_set_view(&v);
}

GLOVER_API int32_t glover_set_view(Glover *g, const GloverView *v) {
    ViewCall c;
    int32_t rc;

    if (v != NULL && (v->size < sizeof(GloverView)
                      || (v->flags & ~(GLOVER_VIEW_DRAW | GLOVER_VIEW_STEER | GLOVER_VIEW_TEST_GAME)))) {
        return GLOVER_ERR_ARG;
    }
    c.view = v;
    c.rc = GLOVER_OK;
    rc = enter(g, call_set_view, &c);
    return rc != GLOVER_OK ? rc : c.rc;
}
