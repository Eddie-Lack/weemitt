/* nullvideo: a video plugin that draws nothing, for BizHawk's mupen64plus core (tools/sync/bizhawk/setup.sh).

   The core waits for the video plugin to raise the RDP's interrupt at the end of a display list, as every real plugin
   does. In this core no video plugin adds emulated time: GLideN64, the video plugin of BizHawk's movies, runs inside
   ProcessDList as this one does. UpdateScreen comes once at each vertical interrupt (a video frame); vicount_value
   gives the count to pollinput, as tools/sync/vicount.c does for the system's mupen64plus. */
#define M64P_PLUGIN_PROTOTYPES 1
#include <stddef.h>

#include "m64p_common.h"
#include "m64p_plugin.h"
#include "m64p_types.h"

static GFX_INFO info;
static unsigned int vis;

EXPORT unsigned int CALL vicount_value(void) {
    return vis;
}

EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle core, void *context, void (*debug)(void *, int, const char *)) {
    vis = 0;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginShutdown(void) {
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *type, int *version, int *api, const char **name, int *caps) {
    if (type) *type = M64PLUGIN_GFX;
    if (version) *version = 0x010000;
    if (api) *api = 0x020200;
    if (name) *name = "nullvideo";
    if (caps) *caps = 0;
    return M64ERR_SUCCESS;
}

EXPORT int CALL InitiateGFX(GFX_INFO gfx) {
    info = gfx;
    return 1;
}

/* The end of a display list: the RDP's interrupt (MI_INTR_DP). */
EXPORT void CALL ProcessDList(void) {
    *info.MI_INTR_REG |= 0x20;
    info.CheckInterrupts();
}

EXPORT void CALL ProcessRDPList(void) {
    *info.MI_INTR_REG |= 0x20;
    info.CheckInterrupts();
}

EXPORT void CALL UpdateScreen(void) {
    vis++;
}

EXPORT void CALL ChangeWindow(void) {}
EXPORT void CALL MoveScreen(int x, int y) {}
EXPORT void CALL RomClosed(void) {}
EXPORT int CALL RomOpen(void) {
    return 1;
}
EXPORT void CALL ShowCFB(void) {}
EXPORT void CALL ViStatusChanged(void) {}
EXPORT void CALL ViWidthChanged(void) {}
EXPORT void CALL ReadScreen2(void *dest, int *width, int *height, int front) {}
EXPORT void CALL SetRenderingCallback(void (*callback)(int)) {}
EXPORT void CALL ResizeVideoOutput(int width, int height) {}
EXPORT void CALL FBRead(unsigned int addr) {}
EXPORT void CALL FBWrite(unsigned int addr, unsigned int size) {}
EXPORT void CALL FBGetFrameBufferInfo(void *p) {}
