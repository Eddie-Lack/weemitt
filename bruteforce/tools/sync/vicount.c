/* vicount: a video plugin for mupen64plus that counts video frames and passes everything on to a real video plugin.

   The core calls the video plugin's UpdateScreen once at each vertical interrupt (a VI, one video frame). BizHawk
   counts its frames the same way, so this count is the clock of a BizHawk movie. mupen64plus's frame callback is
   another clock: it counts graphics tasks. tools/sync/emurun.py loads this plugin with RTLD_GLOBAL and sets the real
   plugin's path with vicount_load before it attaches it; pollinput reads the count with vicount_value.

   Build: cc -shared -fPIC -O2 -I/usr/include/mupen64plus -o vicount.so vicount.c -ldl */
#define M64P_PLUGIN_PROTOTYPES 1
#include <dlfcn.h>
#include <stddef.h>
#include <xmmintrin.h>

#include "m64p_common.h"
#include "m64p_plugin.h"
#include "m64p_types.h"

static void *real;
static unsigned int vis;

#define REAL(name) ((ptr_##name)dlsym(real, #name))

/* The SSE flags flush to zero and denormals are zero, which other plugins' libraries turn on (pollinput.c). */
static void clear_flush(void) {
    _mm_setcsr(_mm_getcsr() & ~0x8040u);
}

EXPORT int CALL vicount_load(const char *path) {
    real = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    vis = 0;
    return real != NULL;
}

EXPORT unsigned int CALL vicount_value(void) {
    return vis;
}

EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle core, void *context, void (*debug)(void *, int, const char *)) {
    return REAL(PluginStartup)(core, context, debug);
}

EXPORT m64p_error CALL PluginShutdown(void) {
    return REAL(PluginShutdown)();
}

EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *type, int *version, int *api, const char **name, int *caps) {
    return REAL(PluginGetVersion)(type, version, api, name, caps);
}

EXPORT void CALL ChangeWindow(void) {
    REAL(ChangeWindow)();
}

EXPORT int CALL InitiateGFX(GFX_INFO info) {
    return REAL(InitiateGFX)(info);
}

EXPORT void CALL MoveScreen(int x, int y) {
    REAL(MoveScreen)(x, y);
}

EXPORT void CALL ProcessDList(void) {
    REAL(ProcessDList)();
}

EXPORT void CALL ProcessRDPList(void) {
    REAL(ProcessRDPList)();
}

EXPORT void CALL RomClosed(void) {
    REAL(RomClosed)();
}

EXPORT int CALL RomOpen(void) {
    int ok = REAL(RomOpen)();

    clear_flush();
    return ok;
}

EXPORT void CALL ShowCFB(void) {
    REAL(ShowCFB)();
}

EXPORT void CALL UpdateScreen(void) {
    vis++;
    REAL(UpdateScreen)();
    clear_flush();
}

EXPORT void CALL ViStatusChanged(void) {
    REAL(ViStatusChanged)();
}

EXPORT void CALL ViWidthChanged(void) {
    REAL(ViWidthChanged)();
}

EXPORT void CALL ReadScreen2(void *dest, int *width, int *height, int front) {
    REAL(ReadScreen2)(dest, width, height, front);
}

EXPORT void CALL SetRenderingCallback(void (*callback)(int)) {
    REAL(SetRenderingCallback)(callback);
}

EXPORT void CALL ResizeVideoOutput(int width, int height) {
    ptr_ResizeVideoOutput f = REAL(ResizeVideoOutput);

    if (f != NULL) {
        f(width, height);
    }
}

EXPORT void CALL FBRead(unsigned int addr) {
    ptr_FBRead f = REAL(FBRead);

    if (f != NULL) {
        f(addr);
    }
}

EXPORT void CALL FBWrite(unsigned int addr, unsigned int size) {
    ptr_FBWrite f = REAL(FBWrite);

    if (f != NULL) {
        f(addr, size);
    }
}

EXPORT void CALL FBGetFrameBufferInfo(void *p) {
    ptr_FBGetFrameBufferInfo f = REAL(FBGetFrameBufferInfo);

    if (f != NULL) {
        f(p);
    }
}
