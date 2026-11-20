/* The in-game menu: a menu bar with category windows, drawn with Dear ImGui over the game's frame. */
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
extern "C" {
#include "frame.h"
#include "../gfx/gfx.h"
}
#include "log.h"
#include "menu.h"
#include "record.h"
#include "settings.h"
#ifdef PORT_STUDIO
#include "studio.h"
#endif

namespace {

bool menuOpen;
bool settingsDirty;
SDL_Window *gameWindow;
Settings *cfg;
char cfgPath[512];

struct Windows {
    bool graphics, audio, controls, enhancements, cheats, developer, log, studioLevel, studioStates;
} win;

/* Rebinding: which input and key slot wait for a key press (-1: none). */
int rebindInput = -1;
int rebindSlot;
bool rebindPad;  /* the wait is for a controller button, not a key */

/* Developer, frame step keys: which of them waits for a key press (0: none, 1: pause, 2: step). */
int rebindDev;

/* Hidden test option: WEEMITT_MENU_TEST=N opens the menu with every window after N frames, saves the next frame to
   weemitt-menu.ppm (or the file named by WEEMITT_MENU_SHOT) and quits. */
long testFrames = -1;
long frameCount;

void save_settings() {
    if (settingsDirty) {
        settingsDirty = false;
        if (settings_save(cfg, cfgPath) != 0) {
            fprintf(stderr, "weemitt: cannot write %s\n", cfgPath);
        }
    }
}

void set_open(bool open) {
    menuOpen = open;
    rebindDev = 0;
    rebindInput = -1;
    rebindPad = false;
    if (!open) {
        save_settings();
    }
}

void bind_key(int input, int slot, int scancode) {
    int i, s;

    /* A key does one thing: take it away from any other input. */
    for (i = 0; i < IN_COUNT; i++) {
        for (s = 0; s < KEY_SLOTS; s++) {
            if (cfg->keys[i][s] == scancode) {
                cfg->keys[i][s] = 0;
            }
        }
    }
    cfg->keys[input][slot] = scancode;
    settingsDirty = true;
}

void placeholder(const char *label, const char *note = "Coming later") {
    ImGui::BeginDisabled();
    bool off = false;
    ImGui::Checkbox(label, &off);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", note);
    }
}

void placeholder_slider(const char *label, int value, int lo, int hi) {
    ImGui::BeginDisabled();
    ImGui::SliderInt(label, &value, lo, hi);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Coming later");
    }
}

/* The Graphics window's picture options. The renderer takes them from the settings each frame (platform.c). */
void image_options() {
    static const char *const filters[] = {"N64 (three-point)", "Bilinear", "Nearest"};
    static const char *const wides[] = {"Off (4:3 with bars)", "Stretch", "Extend view (Hor+)"};
    const int maxSamples = gfx_max_samples();
    int scale = cfg->renderScale;
    char text[40];

    if (ImGui::SliderInt("Resolution scale", &scale, RENDER_SCALE_MIN, RENDER_SCALE_MAX, "%d%%",
                         ImGuiSliderFlags_AlwaysClamp)) {
        cfg->renderScale = scale;
        settingsDirty = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The 3D picture is drawn at this share of the window's size, then scaled to the window.\n"
                          "Above 100%% it is smoother when scaled down. Below 100%% it is faster. Very large values\n"
                          "need much video memory: the renderer lowers the anti-aliasing when the GPU refuses.");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##scale")) {
        cfg->renderScale = RENDER_SCALE_DEFAULT;
        settingsDirty = true;
    }

    snprintf(text, sizeof(text), cfg->msaa ? "%dx" : "Off", cfg->msaa);
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::BeginCombo("Anti-aliasing (MSAA)", text)) {
        static const int levels[] = {0, 2, 4, 8};

        for (int i = 0; i < 4; i++) {
            bool usable = levels[i] == 0 || levels[i] <= maxSamples;

            snprintf(text, sizeof(text), levels[i] ? "%dx" : "Off", levels[i]);
            if (!usable) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Selectable(text, cfg->msaa == levels[i])) {
                cfg->msaa = levels[i];
                settingsDirty = true;
            }
            if (!usable) {
                ImGui::EndDisabled();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Smooths the edges of polygons. Up to %dx on this GPU.\n"
                          "Cut-out textures (leaves, fences) keep their N64 edges: only polygon edges are smoothed.",
                          maxSamples);
    }

    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::Combo("Texture filtering", &cfg->texFilter, filters, 3)) {
        settingsDirty = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("N64: the console's three-point filter, which is the original look.\n"
                          "Bilinear: the usual four-point filter. Nearest: no filtering, sharp texels.");
    }

    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::Combo("Widescreen", &cfg->widescreen, wides, 3)) {
        settingsDirty = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Off: the picture keeps its 4:3 shape with bars at the sides.\n"
                          "Stretch: the 4:3 picture is stretched over the window.\n"
                          "Extend view: the 3D view widens to the window's shape. The HUD, menus and\n"
                          "texture rectangles stay 4:3 in the middle. The game still skips objects that lie\n"
                          "outside its own 4:3 view, so some pop in and out at the sides.");
    }
}

void graphics_window() {
    if (!ImGui::Begin("Graphics", &win.graphics, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::SeparatorText("Frame rate");
    {
        static const char *const modes[] = {"Smooth (frame-rate independent)", "Original (interpolated)"};
        int mode = cfg->physicsMode == PHYSICS_ORIGINAL ? 1 : 0;

        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::Combo("Physics", &mode, modes, 2)) {
            cfg->physicsMode = mode ? PHYSICS_ORIGINAL : PHYSICS_SMOOTH;
            settingsDirty = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Smooth: the game logic runs in short steps, once per drawn frame.\n"
                              "Original: the logic runs as on the N64, 20 steps a second in a level and 30 in a menu\n"
                              "(glitches that need whole steps, such as wall clips, work). The picture is drawn at the\n"
                              "frame rate below and blends the two latest steps. This adds up to one step of delay.");
        }
    }
    bool vsync = cfg->vsync != 0;
    if (ImGui::Checkbox("VSync", &vsync)) {
        cfg->vsync = vsync;
        SDL_GL_SetSwapInterval(vsync ? 1 : 0);
        settingsDirty = true;
    }
    /* The slider runs from the lowest limit to one step past the highest, which stands for no limit. */
    int fps = cfg->maxFps > 0 ? cfg->maxFps : FPS_LIMIT_MAX + 1;
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::SliderInt("Frame rate limit", &fps, FPS_LIMIT_MIN, FPS_LIMIT_MAX + 1,
                         fps > FPS_LIMIT_MAX ? "Unlimited" : "%d FPS", ImGuiSliderFlags_AlwaysClamp)) {
        cfg->maxFps = fps > FPS_LIMIT_MAX ? 0 : fps;
        settingsDirty = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The picture is drawn at this rate with either Physics choice.\n"
                          "Smooth: the logic follows any rate from 20 to 240 FPS. Levels originally ran at 20 frames a\n"
                          "second, menus at 30. Unlimited stops at 10 times that rate (200 in a level, 300 in a menu).\n"
                          "Original: Unlimited stops at 1000 FPS.");
    }
    ImGui::SeparatorText("Image");
    image_options();
    ImGui::End();
}

void audio_window() {
    if (!ImGui::Begin("Audio", &win.audio, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    int volume = cfg->volume;
    if (ImGui::SliderInt("Master volume", &volume, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
        cfg->volume = volume;
        settingsDirty = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("100%% is the game's own level.");
    }
    int music = cfg->musicVolume;
    if (ImGui::SliderInt("Music volume", &music, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
        cfg->musicVolume = music;
        settingsDirty = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The songs, as a share of Master volume. 100%% is the game's own balance.");
    }
    int effects = cfg->effectsVolume;
    if (ImGui::SliderInt("Effects volume", &effects, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
        cfg->effectsVolume = effects;
        settingsDirty = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The sound effects, as a share of Master volume. 100%% is the game's own balance.");
    }
    bool mute = cfg->muteUnfocused != 0;
    if (ImGui::Checkbox("Mute when the window loses focus", &mute)) {
        cfg->muteUnfocused = mute;
        settingsDirty = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The game keeps running. Only the sound is silent while another window is active.");
    }
    ImGui::End();
}

void key_button(int input, int slot) {
    char label[64];
    int key = cfg->keys[input][slot];
    bool waiting = rebindInput == input && rebindSlot == slot;

    snprintf(label, sizeof(label), "%s##%d_%d", waiting ? "Press a key..." : key > 0 ? SDL_GetScancodeName((SDL_Scancode)key) : "(none)",
             input, slot);
    if (ImGui::Button(label, ImVec2(130.0f, 0.0f))) {
        rebindInput = input;
        rebindSlot = slot;
        rebindPad = false;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Click, then press a key. Escape cancels. Right-click clears the key.");
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        cfg->keys[input][slot] = 0;
        settingsDirty = true;
        rebindInput = -1;
    }
}

void bind_joy(int input, int slot, int code) {
    /* A joystick control does one thing: take it away from any other input. */
    for (int i = 0; i < IN_COUNT; i++) {
        for (int s = 0; s < KEY_SLOTS; s++) {
            if (cfg->joys[i][s] == code) {
                cfg->joys[i][s] = 0;
            }
        }
    }
    cfg->joys[input][slot] = code;
    settingsDirty = true;
}

void bind_pad(int input, int slot, int code) {
    /* A controller button does one thing: take it away from any other input. */
    for (int i = 0; i < IN_COUNT; i++) {
        for (int s = 0; s < KEY_SLOTS; s++) {
            if (cfg->pads[i][s] == code) {
                cfg->pads[i][s] = 0;
            }
        }
    }
    cfg->pads[input][slot] = code;
    settingsDirty = true;
}

void pad_button(int input, int slot, bool joy) {
    char label[64];
    int (*binds)[KEY_SLOTS] = joy ? cfg->joys : cfg->pads;
    int code = binds[input][slot];
    bool waiting = rebindPad && rebindInput == input && rebindSlot == slot;

    snprintf(label, sizeof(label), "%s##p%d_%d", waiting ? "Press a button..." : joy ? settings_joy_name(code) : settings_pad_name(code), input, slot);
    if (ImGui::Button(label, ImVec2(150.0f, 0.0f))) {
        rebindInput = input;
        rebindSlot = slot;
        rebindPad = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(joy ? "Click, then press a button, move a stick or axis, or press the hat.\n"
                                "Escape cancels. Right-click clears the binding."
                              : "Click, then press a controller button or move a trigger or stick.\n"
                                "Escape cancels. Right-click clears the binding.");
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        binds[input][slot] = 0;
        settingsDirty = true;
        rebindInput = -1;
    }
}

void input_groups(bool pad, bool joy = false) {
    static const struct { const char *title; int first, last; } groups[] = {
        {"Buttons", IN_A, IN_R},
        {"C buttons", IN_CUP, IN_CRIGHT},
        {"D-pad", IN_DUP, IN_DRIGHT},
        {"Control stick", IN_STICK_UP, IN_STICK_RIGHT},
    };
    for (const auto &g : groups) {
        ImGui::SeparatorText(pad && g.first == IN_STICK_UP ? (joy ? "Control stick (extra, axes 0 and 1 are analog)" : "Control stick (extra, the left stick is analog)") : g.title);
        if (ImGui::BeginTable(g.title, 3, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("input", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            for (int i = g.first; i <= g.last; i++) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(settings_input_name(i));
                for (int s = 0; s < KEY_SLOTS; s++) {
                    ImGui::TableNextColumn();
                    if (pad) {
                        pad_button(i, s, joy);
                    } else {
                        key_button(i, s);
                    }
                }
            }
            ImGui::EndTable();
        }
    }
}

void controls_window() {
    if (!ImGui::Begin("Controls", &win.controls, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginTabBar("controls_tabs")) {
        if (ImGui::BeginTabItem("Keyboard")) {
            ImGui::TextUnformatted("Each input has two keys.");
            if (ImGui::Button("Reset to defaults")) {
                settings_default_keys(cfg);
                rebindInput = -1;
                settingsDirty = true;
            }
            input_groups(false);
            ImGui::EndTabItem();
        }
        /* WEEMITT_MENU_TAB=controller (test option) opens the window on this tab. */
        const char *tab = getenv("WEEMITT_MENU_TAB");
        bool pick = testFrames >= 0 && frameCount == testFrames && tab && !strcmp(tab, "controller");

        if (ImGui::BeginTabItem("Controller", NULL, pick ? ImGuiTabItemFlags_SetSelected : 0)) {
            char devName[128];
            int kind = port_input_device(devName, sizeof(devName));
            bool joy = kind == PORT_DEVICE_JOYSTICK;

            if (kind == PORT_DEVICE_NONE) {
                ImGui::TextUnformatted("Device: none connected");
            } else {
                ImGui::Text("Device: %s", devName);
                ImGui::Text("Type: %s", joy ? "Joystick (no SDL mapping)" : "Game controller");
            }
            ImGui::TextUnformatted(joy ? "Each input has two bindings. Axes 0 and 1 move the glove."
                                       : "Each input has two bindings. The left stick moves the glove.");
            if (ImGui::Button("Reset to defaults")) {
                settings_default_pads(cfg);
                settings_default_joys(cfg);
                cfg->stickDead = STICK_DEAD_DEFAULT;
                cfg->stickRange = STICK_RANGE_DEFAULT;
                rebindInput = -1;
                settingsDirty = true;
            }
            ImGui::SeparatorText(joy ? "Stick (axes 0 and 1)" : "Left stick");
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::SliderInt("Dead zone", &cfg->stickDead, 0, 50, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
                settingsDirty = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("How far the stick must move before the glove moves.");
            }
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::SliderInt("Full tilt at", &cfg->stickRange, 50, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
                settingsDirty = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("How far the stick must move to reach the N64 stick's full range.\n"
                                  "Lower it if your stick does not reach the rim.");
            }
            input_groups(true, joy);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void enhancements_window() {
    if (!ImGui::Begin("Enhancements", &win.enhancements, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted("The game draws at any frame rate. Graphics, Physics chooses how its logic runs.");
    ImGui::Separator();
    bool easy = cfg->easyBall != 0;
    if (ImGui::Checkbox("Easy ball controls", &easy)) {
        cfg->easyBall = easy;
        port_set_easy_ball(easy);
        settingsDirty = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The N64 game reverses the stick while the glove stands on the ball, except on Easy.\n"
                          "With this on, every difficulty steers the ball the way Easy does.");
    }
    bool ballworld = cfg->ballworld != 0;
    if (ImGui::Checkbox("Ballworld", &ballworld)) {
        cfg->ballworld = ballworld;
        port_set_ballworld(ballworld);
        settingsDirty = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Play every level as the ball alone.");
    }
    if (port_save_locked) {
        ImGui::TextDisabled("Playing with the other save file's mode: progress is not saved until you restart.");
    } else if (ballworld != (port_save_ballworld != 0)) {
        ImGui::TextDisabled("Takes effect at the next level. Saves follow it at the next start.");
    }
    float follow = cfg->cameraFollow;
    if (ImGui::SliderFloat("Camera follow speed", &follow, 1.0f, 3.0f, "%.1fx", ImGuiSliderFlags_AlwaysClamp)) {
        cfg->cameraFollow = follow;
        port_set_camera_follow(follow);
        settingsDirty = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("How fast the camera catches up with the glove and turns behind it.\n"
                          "1.0x is the original game.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##camfollow")) {
        cfg->cameraFollow = 1.0f;
        port_set_camera_follow(1.0f);
        settingsDirty = true;
    }
    bool skip = cfg->skipIntro != 0;
    if (ImGui::Checkbox("Skip intro videos", &skip)) {
        cfg->skipIntro = skip;
        port_set_skip_intro(skip);
        settingsDirty = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("At start-up, skips the logo scene and the fly-through and goes to the title menu,\n"
                          "as a press of Start would. It acts on the next start. The Space demo has neither scene.");
    }
    ImGui::End();
}

/* One cheat option: the box shows the game's cheat bits as they are now, so a change in the pause menu shows here.
   A click turns the cheats on or off at once and keeps them on in later levels. locked holds the box on. */
void cheat_box(const char *label, int firstBit, int bits, int *setting, const char *tip, bool locked = false) {
    unsigned long now = (unsigned long)port_get_cheat_bits();
    unsigned long mask = (unsigned long)bits << firstBit;
    bool on = locked || (now & mask) == mask;

    if (locked) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Checkbox(label, &on)) {
        for (int i = 0; i < 32; i++) {
            if ((mask >> i) & 1) {
                port_set_cheat_bit(i, on);
            }
        }
        *setting = on;
        settingsDirty = true;
    }
    if (locked) {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tip);
    }
}

/* The names of the levels that "Go to level" starts: the game's level numbers 0 to 41. */
const char *const levelNames[] = {
    "Hub 1", "Hub 2", "Hub 3", "Hub 4", "Hub 5", "Hub 6", "Hub 7", "Hub 8", "Cave", "Assault course",
    "Atlantis 1", "Atlantis 2", "Atlantis 3", "Atlantis boss", "Atlantis bonus",
    "Carnival 1", "Carnival 2", "Carnival 3", "Carnival boss", "Carnival bonus",
    "Pirates 1", "Pirates 2", "Pirates 3", "Pirates boss", "Pirates bonus",
    "Prehistoric 1", "Prehistoric 2", "Prehistoric 3", "Prehistoric boss", "Prehistoric bonus",
    "Fear 1", "Fear 2", "Fear 3", "Fear boss", "Fear bonus",
    "Space 1", "Space 2", "Space 3", "Space boss 1", "Space boss 2 (cut scene)", "Space boss 3", "Space bonus",
};

#ifdef PORT_STUDIO
/* The studio bar: the timeline and its controls, at the bottom of the window. */
void studio_bar() {
    StudioInfo in;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    char text[160];
    int frame;
    int length;

    studio_info(&in);
    frame = (int)in.frame;
    length = (int)(in.length > 0 ? in.length : 1);
    ImGui::SetNextWindowPos(ImVec2(8.0f, ds.y - 104.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(ds.x - 16.0f, 96.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.8f);
    ImGui::Begin("Studio", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderInt("##timeline", &frame, 0, length, "frame %d")) {
        studio_request_seek(frame);
    }
    if (ImGui::Button("|<")) {
        studio_request_seek(0);
    }
    ImGui::SameLine();
    if (ImGui::Button("<<")) {
        studio_request_seek(in.frame - 20);
    }
    ImGui::SameLine();
    if (ImGui::Button("<")) {
        studio_request_seek(in.frame - 1);
    }
    ImGui::SameLine();
    if (ImGui::Button(in.paused ? "Play on from here" : "Pause")) {
        studio_set_paused(!in.paused);
    }
    ImGui::SameLine();
    if (ImGui::Button(">")) {
        studio_step_forward();
    }
    ImGui::SameLine();
    if (ImGui::Button(">>")) {
        studio_request_seek(in.frame + 20);
    }
    ImGui::SameLine();
    if (ImGui::Button(">|")) {
        studio_request_seek(in.length);
    }
    ImGui::SameLine();
    if (in.frame >= in.length) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Play the recording")) {
        studio_play_recording();
    }
    if (in.frame >= in.length) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    snprintf(text, sizeof(text), "Frame %ld of %ld (%.2f s). %ld saved states, %.0f MB.", in.frame, in.length,
             in.frame * port_game_retraces() / 60.0, in.states, in.megabytes);
    ImGui::TextUnformatted(text);
    ImGui::TextDisabled("%s", in.paused ? studio_controls_help() : "All C buttons together: pause.");
    ImGui::End();
}

/* The points and the search: a window at the top right while the studio bar shows. */
void studio_search_window() {
    static float radius = 40.0f;
    static int workers = 0;
    static int limits = 2;
    static const char *const limitNames[] = {"none", "loose", "strict"};
    const StudioPoint *points;
    int npoints = studio_points(&points);
    long start = studio_start_frame();
    StudioSearchInfo si;
    StudioInfo in;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    int i;

    studio_info(&in);
    studio_search_info(&si);
    ImGui::SetNextWindowPos(ImVec2(ds.x - 360.0f, menuOpen ? 30.0f : 8.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Points and search", NULL, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    if (ImGui::Button("Start here")) {
        studio_mark_start();
    }
    ImGui::SameLine();
    if (start >= 0) {
        ImGui::Text("Start: frame %ld", start);
    } else {
        ImGui::TextDisabled("No start point");
    }
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("radius", &radius, 5.0f, 300.0f, "%.0f");
    if (ImGui::Button("Point: glove here")) {
        studio_add_place(0, radius);
    }
    ImGui::SameLine();
    if (ImGui::Button("Point: ball here")) {
        studio_add_place(1, radius);
    }
    if (ImGui::Button("Point: the level is done")) {
        studio_add_level_end();
    }
    for (i = 0; i < npoints; i++) {
        ImGui::PushID(i);
        if (ImGui::SmallButton("x")) {
            studio_remove_point(i);
        }
        ImGui::SameLine();
        if (points[i].place) {
            ImGui::Text("%s %d: %s at %.0f %.0f %.0f, radius %.0f (frame %ld)", i + 1 == npoints ? "End" : "Point", i + 1,
                        points[i].ofBall ? "ball" : "glove", points[i].at[0], points[i].at[1], points[i].at[2],
                        points[i].radius, points[i].frame);
        } else {
            ImGui::Text("%s %d: the level is done (frame %ld)", i + 1 == npoints ? "End" : "Point", i + 1,
                        points[i].frame);
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderInt("workers (0: all)", &workers, 0, 32);
    ImGui::SetNextItemWidth(120.0f);
    ImGui::Combo("input limits", &limits, limitNames, 3);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("What a runner can play.\nnone: any input on any frame.\nloose: presses and gaps of at least 2 "
                          "frames, the stick changes by at most 96 a frame.\nstrict: at least 3 frames, at most 48 a "
                          "frame.");
    }
    if (!si.running) {
        if (ImGui::Button("Search")) {
            studio_start_search(workers, 0.0, limitNames[limits]);
        }
    } else if (ImGui::Button("Stop the search")) {
        studio_stop_search();
    }
    if (si.message[0]) {
        ImGui::TextWrapped("%s", si.message);
    }
    if (si.haveBest) {
        if (si.bestFrame >= 0) {
            long bestCost = si.bestFrame + (long)si.bestLoads * STUDIO_LOAD_FRAMES;

            ImGui::Text("Best: the end at frame %ld", si.bestFrame);
            if (si.bestLoads > 0) {
                ImGui::SameLine();
                ImGui::Text("after %d load%s (%ld with the loads)", si.bestLoads, si.bestLoads == 1 ? "" : "s",
                            bestCost);
            }
            if (si.firstFrame >= 0) {
                long firstCost = si.firstFrame + (long)si.firstLoads * STUDIO_LOAD_FRAMES;

                ImGui::Text("First result: %ld, %+ld frames with the loads", firstCost, bestCost - firstCost);
            }
        } else {
            ImGui::Text("Not reached yet (progress %.1f)", si.progress);
        }
        if (ImGui::Button("Take the result")) {
            studio_take_result();
        }
        ImGui::SameLine();
    } else if (si.running) {
        ImGui::TextDisabled("Searching...");
    }
    if (studio_can_undo_take() && ImGui::Button("Undo the take")) {
        studio_undo_take();
    }
    if (si.folder[0]) {
        ImGui::TextDisabled("%s", si.folder);
    }
    (void)in;
    ImGui::End();
}

void studio_level_window() {
    static int levelPick = 0;
    bool canGo = port_goto_possible() != 0;

    if (!ImGui::Begin("Start a level", &win.studioLevel, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("The level starts at once, as the level select does (medium difficulty, save slot 6). "
                       "The timeline starts again from there.");
    ImGui::SetNextItemWidth(200.0f);
    ImGui::Combo("Level", &levelPick, levelNames, (int)(sizeof(levelNames) / sizeof(levelNames[0])), 12);
    ImGui::SameLine();
    if (!canGo) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Start")) {
        studio_goto_level(levelPick);
    }
    if (!canGo) {
        ImGui::EndDisabled();
    }
    if (!canGo) {
        ImGui::TextDisabled("This works in a level and in the title menu.");
    }
    ImGui::End();
}

/* State files in the folder studio/states/ of the preference folder: a name box to save, a list to load from. */
void studio_states_window() {
    static char name[101];
    static char message[700];
    static char names[256][128];
    static int count = -1;
    static int selected = -1;
    /* Whether a state file has the name in the box. It is checked again when the name changes or a file is saved. */
    static char checkedName[sizeof(name)] = "";
    static int nameExists = -1;
    const char *folder = studio_states_folder();
    char path[1024];
    StudioInfo in;
    int i;

    if (!ImGui::Begin("States", &win.studioStates, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    if (count < 0 || ImGui::IsWindowAppearing()) {
        count = studio_list_states(names, 256);
        selected = -1;
    }
    studio_info(&in);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 420.0f);
    ImGui::TextUnformatted("A state holds the game's memory at one frame: do not share it. Only this build of the "
                           "studio loads it.");
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##name", "name", name, sizeof(name));
    ImGui::SameLine();
    if (ImGui::Button("Save state")) {
        if (studio_state_path(name, path, sizeof(path), message, sizeof(message)) == 0
            && studio_save_state(path, message, sizeof(message)) == 0) {
            snprintf(message, sizeof(message), "Saved frame %ld as %s.", in.frame, name);
            count = studio_list_states(names, 256);
            selected = -1;
            nameExists = -1;
        }
    }
    if (nameExists < 0 || strcmp(checkedName, name) != 0) {
        snprintf(checkedName, sizeof(checkedName), "%s", name);
        nameExists = studio_state_exists(name);
    }
    if (nameExists) {
        ImGui::TextDisabled("Save state replaces the state of that name.");
    }
    ImGui::Separator();
    if (ImGui::BeginListBox("##states", ImVec2(320.0f, 160.0f))) {
        for (i = 0; i < count; i++) {
            if (ImGui::Selectable(names[i], selected == i)) {
                selected = i;
            }
        }
        ImGui::EndListBox();
    }
    if (selected < 0) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Load state") && selected >= 0) {
        if (studio_listed_state_path(names[selected], path, sizeof(path)) == 0
            && studio_load_state(path, message, sizeof(message)) == 0) {
            snprintf(message, sizeof(message), "Loaded %s. The timeline starts again from it, paused.", names[selected]);
        }
    }
    if (selected < 0) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh the list")) {
        count = studio_list_states(names, 256);
        selected = -1;
        nameExists = -1;
    }
    if (message[0]) {
        ImGui::TextUnformatted(message);
    }
    ImGui::TextDisabled("%s", folder != NULL ? folder : "No folder for the states.");
    ImGui::PopTextWrapPos();
    ImGui::End();
}
#endif


void cheats_window() {
    if (!ImGui::Begin("Cheats", &win.cheats, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    bool demo = port_space_demo() != 0;

    ImGui::TextUnformatted("These use the game's own cheats.");
    bool pauseList = cfg->pauseCheats != 0;
    if (ImGui::Checkbox("Cheat list in the pause menu (R or Z)", &pauseList)) {
        cfg->pauseCheats = pauseList;
        port_set_pause_cheats(pauseList);
        settingsDirty = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("In the game's pause menu, R or Z opens a list of all the cheats to switch with A.\n"
                          "Off: R and Z do nothing there, as in the original game.");
    }
    ImGui::Separator();
    cheat_box("Infinite lives", 3, 1, &cfg->infiniteLives,
              demo ? "The Space demo keeps this on." : "The game's INFINITE LIVES cheat: losing a life costs nothing.", demo);
    cheat_box("Infinite health", 6, 1, &cfg->infiniteHealth,
              "The game's INFINITE ENERGY cheat: hits do not take hearts. Falls, drowning and crushing still kill.");
    cheat_box("Unlock all levels", 24, 3, &cfg->unlockAll,
              "The game's OPEN PORTALS and OPEN LEVELS cheats: every portal and bonus level in the hub is open.\n"
              "The save is not changed.");
    cheat_box("Level select", 23, 1, &cfg->levelSelect,
              "The game's LEVEL SELECT cheat: the title menu shows the level select.");
    ImGui::SeparatorText("Go to level");
    static int levelPick = 0;
    ImGui::SetNextItemWidth(200.0f);
    ImGui::Combo("Level", &levelPick, levelNames, (int)(sizeof(levelNames) / sizeof(levelNames[0])), 12);
    ImGui::SameLine();
    bool canGo = port_goto_possible() != 0;
    if (!canGo) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Go")) {
        port_goto_level(levelPick);
    }
    if (!canGo) {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Starts the level at once, as the level select does (medium difficulty, save slot 6).\n"
                          "The current level is left without saving. Your save slots are not used.\n"
                          "It works in a level and in the title menu, not in the Space demo.");
    }
    ImGui::End();
}

bool showFps;

/* The name of a key for the Developer window ("(none)" for 0). */
const char *key_name(int scancode) {
    const char *name = scancode > 0 ? SDL_GetScancodeName((SDL_Scancode)scancode) : NULL;

    return name != NULL && name[0] != 0 ? name : "(none)";
}

void dev_key_button(const char *label, int which, int *key) {
    char text[64];

    snprintf(text, sizeof(text), "%s##devkey%d", rebindDev == which ? "Press a key..." : key_name(*key), which);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(90.0f);
    if (ImGui::Button(text, ImVec2(130.0f, 0.0f))) {
        rebindDev = which;
        rebindInput = -1;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Click, then press a key. Escape cancels. Right-click clears the key.");
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        *key = 0;
        settingsDirty = true;
        rebindDev = 0;
    }
}

/* One debug action: a button that puts the debug control's input into pad[4] for the next game frames. The game's
   own code then does the work. */
void debug_action_button(const char *label, int action, const char *tip, bool inLevel) {
    bool ok = inLevel && !port_debug_busy();

    ImGui::BeginDisabled(!ok);
    if (ImGui::Button(label)) {
        port_debug_action(action);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s%s", tip, inLevel ? "" : "\nOnly in a level.");
    }
}

/* The debug keys of one or two inputs as "5" or "9 / 0". */
void debug_keys(char *out, int size, int a, int b = -1) {
    if (b < 0) {
        snprintf(out, size, "%s", settings_scancode_name(settings_debug_key(a)));
    } else {
        snprintf(out, size, "%s / %s", settings_scancode_name(settings_debug_key(a)), settings_scancode_name(settings_debug_key(b)));
    }
}

/* Developer, Game debug: the debug code that the retail game still holds (docs/menu.md). */
void game_debug_section() {
    static const char *const padChoices[] = {"Off", "Second controller", "Keyboard (debug keys)"};
    static const char *const playbackNames[] = {"Off", "Record", "Play back"};
    char devName[128];
    long autoPlay = port_debug_autoplayback();
    long timerMode = port_debug_timer_mode();
    bool inLevel = port_debug_in_level() != 0;
    bool onTitle = port_debug_on_title() != 0;
    bool openWarning = false;

    /* The section is closed until opened; a menu test (WEEMITT_MENU_TEST) opens it. */
    if (testFrames >= 0) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    }
    if (!ImGui::CollapsingHeader("Game debug")) {
        return;
    }
    ImGui::TextWrapped("The game still holds the team's debug code. It reads a fifth controller, pad[4], that the "
                       "retail game never fills. Nothing here changes the game while the debug controller is Off "
                       "and no button below is used.");
    ImGui::SetNextItemWidth(220.0f);
    ImGui::Combo("Debug controller", &cfg->debugPad, padChoices, 3);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("What fills pad[4], once per game frame, in the same read as controller 1.\n"
                          "Second controller: the second game controller or joystick, with the bindings of the first.\n"
                          "Keyboard: the debug keys in the table below (the menu must be closed).\n"
                          "Not saved: the controller is Off at every start.");
    }
    if (cfg->debugPad == DEBUGPAD_SECOND) {
        int kind = port_debug_device(devName, sizeof(devName));

        if (kind == PORT_DEVICE_NONE) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "No second controller found. Plug one in.");
        } else {
            ImGui::TextDisabled("Using %s", devName);
        }
    } else if (cfg->debugPad == DEBUGPAD_KEYBOARD) {
        ImGui::TextDisabled("Close the menu (Escape) to use the debug keys.");
    }
    ImGui::TextDisabled("The debug pad is not part of an input recording (.rec).");

    if (testFrames >= 0) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    }
    if (ImGui::TreeNode("Debug controls")) {
        if (ImGui::BeginTable("debugcontrols", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            struct Row {
                const char *input;
                int a, b;
                const char *effect;
            };
            const Row rows[] = {
                {"C-left, C-right", IN_CLEFT, IN_CRIGHT, "Move Glover (and the ball, if he holds it) to the previous or next platform."},
                {"L", IN_L, -1, "Mark every level and every garib as done in the current save slot. The game may save this."},
                {"Z", IN_Z, -1, "Cycle the timing bars: off, mode 1, mode 2 (1 and 2 both show them)."},
                {"Stick, B, A", IN_B, IN_A, "Move the helper box on x and z, up and down. (The stick keys are Home, End, Delete and Page Down.)"},
                {"Z held with stick, B, A", IN_Z, -1, "Change the size of the helper box."},
                {"D-pad left, right", IN_DLEFT, IN_DRIGHT, "Play the previous or next of the 61 songs."},
                {"D-pad right held, down held", IN_DRIGHT, IN_DDOWN, "Turn the background ambient light."},
                {"D-pad up held", IN_DUP, -1, "Make a ring of puffs at Glover."},
                {"R", IN_R, -1, "Toggle inertia: the hand's acceleration is divided by 3, or multiplied back."},
                {"A held, then B", IN_A, IN_B, "Quit to the menu. In record mode it first writes the recording."},
                {"C-up held, C-down held", IN_CUP, IN_CDOWN, "Push Glover (or the ball, when he holds it) up or down."},
                {"Start", IN_START, -1, "Hurt the ball."},
                {"Z, on the title menu", IN_Z, -1, "Cycle the recording mode: off, record, play back."},
            };

            ImGui::TableSetupColumn("Input");
            ImGui::TableSetupColumn("Debug key");
            ImGui::TableSetupColumn("Effect", ImGuiTableColumnFlags_WidthFixed, 340.0f);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
                char keys[64];

                debug_keys(keys, sizeof(keys), rows[i].a, rows[i].b);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(rows[i].input);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(keys);
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", rows[i].effect);
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("The stick and the B and A keys move the helper box all the time, as on the debug ROM.");
        ImGui::TreePop();
    }

    ImGui::TextUnformatted("Actions (no second controller needed):");
    debug_action_button("Previous platform", PORT_DBG_PREV_PLATFORM, "C-left on pad[4] for one game frame.", inLevel);
    ImGui::SameLine();
    debug_action_button("Next platform", PORT_DBG_NEXT_PLATFORM, "C-right on pad[4] for one game frame.", inLevel);
    ImGui::SameLine();
    debug_action_button("Hurt ball", PORT_DBG_HURT, "Start on pad[4] for one game frame: the game calls HurtBall.", inLevel);
    debug_action_button("Previous song", PORT_DBG_PREV_SONG, "D-pad left on pad[4] for one game frame.", inLevel);
    ImGui::SameLine();
    debug_action_button("Next song", PORT_DBG_NEXT_SONG,
                        "D-pad right on pad[4] for one game frame. The same press also turns the ambient light a\n"
                        "little, as on the debug ROM.", inLevel);
    ImGui::SameLine();
    debug_action_button("Toggle inertia", PORT_DBG_INERTIA, "R on pad[4] for one game frame.", inLevel);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", port_debug_inertia() ? "(inertia on)" : "(inertia off)");
    debug_action_button("Quit to menu", PORT_DBG_QUIT,
                        "A held for one game frame, then A and B: the game leaves the level. In record mode it\n"
                        "writes the recording first.", inLevel);
    ImGui::SameLine();
    ImGui::BeginDisabled(!inLevel || port_debug_busy());
    if (ImGui::Button("Mark all levels done")) {
        openWarning = true;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("L on pad[4] for one game frame. Asks first.%s", inLevel ? "" : "\nOnly in a level.");
    }
    if (openWarning) {
        ImGui::OpenPopup("Mark all levels done?");
    }
    if (ImGui::BeginPopupModal("Mark all levels done?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("This marks every level and every garib as done in the current save slot.");
        ImGui::TextUnformatted("The game may write it to the save file. It cannot be undone here.");
        if (ImGui::Button("Mark them")) {
            port_debug_action(PORT_DBG_MARK_LEVELS);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::SeparatorText("Timing bars");
    debug_action_button("Timing bars", PORT_DBG_TIMING, "Z on pad[4] for one game frame: timerMode goes 0, 1, 2, 0.", inLevel);
    ImGui::SameLine();
    ImGui::TextDisabled("mode %ld (%s)", timerMode, timerMode ? "shown" : "off");
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderInt("Bar scale", &cfg->timerScale, 1, 64, "%dx", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The bars show the game's own time, from the system clock, at 20000 clock ticks to the pixel\n"
                          "(0.43 ms). The PC does a frame's work in a few milliseconds, so the bars are\n"
                          "multiplied by this number. At 16x, one pixel is 0.027 ms.");
    }
    ImGui::TextWrapped("One bar row for each timer, from the top: Main (0), GRAPHICS (1), CONTROL (2), NEAR (3). "
                       "The sound and RSP timers (7 to 9) stay empty. The long lines mark one and two retraces "
                       "(16.7 and 33.3 ms) at the chosen scale. The bars show the latest game frame.");

    ImGui::SeparatorText("Recordings");
    ImGui::Text("Recording mode: %s", autoPlay >= 0 && autoPlay <= 2 ? playbackNames[autoPlay] : "?");
    ImGui::SameLine();
    ImGui::BeginDisabled(!onTitle || port_debug_busy());
    if (ImGui::Button("Cycle")) {
        port_debug_action(PORT_DBG_AUTOPLAY);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Z on pad[4] for one game frame. The title menu reads it: off, record, play back.%s",
                          onTitle ? "" : "\nOnly while the title menu shows.");
    }
    {
        char folder[512];

        port_record_folder(folder, sizeof(folder));
        ImGui::TextWrapped("Record: press Cycle once on the title menu, then start a level. To save the "
                           "recording, hold A and press B on the debug pad (the Quit to menu button). The file "
                           "is LEVELNAME.moo in %s", folder[0] ? folder : "(no save folder)");
    }
    ImGui::TextWrapped("Play back: the game plays the demo that the ROM holds for the level you start. A level "
                       "without one stops with \"LEVEL UNPLAYABLE - NO MOO FILE\".");
    ImGui::TextWrapped("The game's debug messages (PRINTF) go to the log. Turn on Write a log file, or open the "
                       "Log window.");
}

void developer_window() {
    /* The window grows with its content, up to the height of the display. Then it scrolls. */
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, ImGui::GetIO().DisplaySize.y - 50.0f));
    if (!ImGui::Begin("Developer", &win.developer, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::Checkbox("Show FPS", &showFps);
    ImGui::Text("%.1f FPS, %.2f ms a frame", ImGui::GetIO().Framerate,
                ImGui::GetIO().Framerate > 0.0f ? 1000.0f / ImGui::GetIO().Framerate : 0.0f);
    if (cfg->fpsCap > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Unlimited is capped at %d FPS%s", cfg->fpsCap,
                           cfg->physicsMode == PHYSICS_ORIGINAL ? "" : " (10 times the game's rate)");
    }
    ImGui::SeparatorText("Log");
    bool logging = plog_file_active() != 0;
    if (ImGui::Checkbox("Write a log file", &logging)) {
        char path[512];

        plog_path(path, sizeof(path));
        cfg->logFile = logging && plog_set_file(path) == 0;
        if (!logging) {
            plog_set_file(NULL);
        }
        settingsDirty = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Writes the game's messages, the program's errors and a crash report to the log file.\n"
                          "A Windows build has no console, so this file is where they can be read.");
    }
    char logPathText[512];
    plog_path(logPathText, sizeof(logPathText));
    ImGui::TextWrapped("%s", logPathText[0] ? logPathText : "(no save folder)");
    if (ImGui::Button("Open folder")) {
        plog_open_folder();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Log window", &win.log);
    bool verbose = cfg->logVerbose != 0;
    if (ImGui::Checkbox("Verbose", &verbose)) {
        cfg->logVerbose = verbose;
        plog_set_verbose(verbose);
        settingsDirty = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Also logs the frame rate, the game mode and the level once a second.");
    }
    ImGui::SeparatorText("Record input");
    bool recording = cfg->recordInput != 0;
    if (ImGui::Checkbox("Record input (from the next start)", &recording)) {
        cfg->recordInput = recording;
        settingsDirty = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Writes the length of every update and every controller value the game reads to a\n"
                          "weemitt-DATE-TIME.rec file, from the start of the program. A developer can replay the file\n"
                          "to see the same run. The file holds your save and no data from the ROM.");
    }
    if (rec_active()) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Recording to:");
        ImGui::TextWrapped("%s", rec_path());
    } else {
        char folder[512];

        rec_folder(folder, sizeof(folder));
        ImGui::TextWrapped("Folder: %s", folder[0] ? folder : "(no save folder)");
        if (recording) {
            ImGui::TextUnformatted("Recording starts the next time the program starts.");
        }
    }
    if (ImGui::Button("Open folder##rec")) {
        plog_open_folder();
    }
    ImGui::SeparatorText("Frame step");
    bool paused = cfg->pauseLogic != 0;
    if (ImGui::Checkbox("Pause game logic", &paused)) {
        cfg->pauseLogic = paused;
        cfg->stepRequests = 0;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Stops the game's logic. The picture keeps drawing and the menu keeps working.\n"
                          "In the Smooth physics mode the pause starts when the original frame in progress ends.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!paused);
    if (ImGui::Button("Step") && cfg->stepRequests < 4) {
        cfg->stepRequests++;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Runs exactly one original frame of the game's logic (one update of the length 1).\n"
                          "The frame reads the controller as it is now. Works with both physics modes.");
    }
    dev_key_button("Pause key", 1, &cfg->pauseKey);
    dev_key_button("Step key", 2, &cfg->stepKey);
    ImGui::TextDisabled("%s pauses and resumes. %s runs one frame (it pauses first).", key_name(cfg->pauseKey),
                        key_name(cfg->stepKey));
    ImGui::SeparatorText("Overlays");
    bool collision = cfg->showCollision != 0;
    if (ImGui::Checkbox("Show collision", &collision)) {
        cfg->showCollision = collision;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Draws the faces that the game collides with, near the glove, as lines over the 3D view,\n"
                          "and the collision spheres of the glove, the ball and the enemies near them.");
    }
    ImGui::Indent();
    bool depth = cfg->collisionDepth != 0;
    if (ImGui::Checkbox("Hide lines behind surfaces", &depth)) {
        cfg->collisionDepth = depth;
    }
    {
        /* One box per kind of line, in its own colour. */
        static const struct {
            const char *name;
            unsigned int bit;
            ImVec4 color;
        } kinds[] = {
            {"floor", PORT_COLL_FLOOR, ImVec4(0.24f, 0.9f, 0.31f, 1.0f)},
            {"wall", PORT_COLL_WALL, ImVec4(1.0f, 0.59f, 0.12f, 1.0f)},
            {"ceiling", PORT_COLL_CEILING, ImVec4(0.86f, 0.31f, 0.94f, 1.0f)},
            {"platform", PORT_COLL_PLATFORM, ImVec4(0.27f, 0.55f, 1.0f, 1.0f)},
            {"water", PORT_COLL_WATER, ImVec4(0.16f, 0.9f, 0.94f, 1.0f)},
            {"hurts", PORT_COLL_HURT, ImVec4(1.0f, 0.2f, 0.2f, 1.0f)},
            {"glove", PORT_COLL_HAND, ImVec4(1.0f, 0.92f, 0.24f, 1.0f)},
            {"ball", PORT_COLL_BALL, ImVec4(1.0f, 1.0f, 1.0f, 1.0f)},
            {"enemy", PORT_COLL_ENEMY, ImVec4(1.0f, 0.43f, 0.43f, 1.0f)},
        };
        unsigned int types = (unsigned int)cfg->collisionTypes;
        float x0 = ImGui::GetCursorPosX();

        for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
            if (k % 3 != 0) {
                ImGui::SameLine(x0 + 90.0f * (float)(k % 3));
            }
            ImGui::PushStyleColor(ImGuiCol_Text, kinds[k].color);
            ImGui::PushStyleColor(ImGuiCol_CheckMark, kinds[k].color);
            if (ImGui::CheckboxFlags(kinds[k].name, &types, kinds[k].bit)) {
                cfg->collisionTypes = (int)types;
                settingsDirty = true;
            }
            ImGui::PopStyleColor(2);
        }
        if (ImGui::SmallButton("All##colltypes")) {
            cfg->collisionTypes = PORT_COLL_ALL;
            settingsDirty = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("None##colltypes")) {
            cfg->collisionTypes = 0;
            settingsDirty = true;
        }
    }
    ImGui::Unindent();
    bool names = cfg->showNames != 0;
    if (ImGui::Checkbox("Show object names", &names)) {
        cfg->showNames = names;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Writes a name at each actor near the glove: the enemy type, or the model's name from the\n"
                          "level file for platforms, pick-ups and the rest.");
    }
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::SliderInt("Overlay range", &cfg->debugRange, DEBUG_RANGE_MIN, DEBUG_RANGE_MAX, "%d units",
                         ImGuiSliderFlags_AlwaysClamp)) {
        settingsDirty = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("How far from the glove the collision lines and the names reach, in game units.");
    }
    game_debug_section();
    ImGui::End();
}

bool logAutoScroll = true;

void log_window() {
    ImGui::SetNextWindowSize(ImVec2(520, 260), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Log", &win.log)) {
        ImGui::End();
        return;
    }
    if (ImGui::Button("Clear")) {
        plog_ring_clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &logAutoScroll);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", plog_file_active() ? "also written to the log file" : "log file off");
    ImGui::Separator();
    ImGui::BeginChild("lines", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    unsigned total = plog_ring_total();
    unsigned first = total > LOG_RING_LINES ? total - LOG_RING_LINES : 0;
    char line[LOG_LINE_SIZE];
    for (unsigned n = first; n < total; n++) {
        if (plog_ring_get(n, line, sizeof(line))) {
            ImGui::TextUnformatted(line);
        }
    }
    if (logAutoScroll) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

void menu_bar() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("Settings")) {
        ImGui::MenuItem("Graphics", NULL, &win.graphics);
        ImGui::MenuItem("Audio", NULL, &win.audio);
        ImGui::MenuItem("Controls", NULL, &win.controls);
        ImGui::EndMenu();
    }
#ifdef PORT_STUDIO
    /* The Enhancements and Cheats change the game's logic, which the timeline's recorded input would not replay. */
    if (ImGui::BeginMenu("Studio")) {
        if (ImGui::MenuItem("Pause", "all C buttons", studio_paused() != 0)) {
            studio_set_paused(!studio_paused());
        }
        ImGui::MenuItem("Start a level", NULL, &win.studioLevel);
        ImGui::MenuItem("Save and load states", NULL, &win.studioStates);
        ImGui::EndMenu();
    }
#else
    if (ImGui::BeginMenu("Enhancements")) {
        ImGui::MenuItem("Enhancements", NULL, &win.enhancements);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Cheats")) {
        ImGui::MenuItem("Cheats", NULL, &win.cheats);
        ImGui::EndMenu();
    }
#endif
    if (ImGui::BeginMenu("Developer")) {
        ImGui::MenuItem("Developer tools", NULL, &win.developer);
        ImGui::MenuItem("Log window", NULL, &win.log);
        ImGui::EndMenu();
    }
    ImGui::TextDisabled("   Esc: close the menu");
    ImGui::EndMainMenuBar();
}

/* The colour of an actor's name by kind. */
ImU32 label_color(int kind) {
    switch (kind) {
        case PORT_DEBUG_HAND: return IM_COL32(255, 235, 60, 255);
        case PORT_DEBUG_BALL: return IM_COL32(255, 255, 255, 255);
        case PORT_DEBUG_ENEMY: return IM_COL32(255, 120, 120, 255);
        case PORT_DEBUG_PLATFORM: return IM_COL32(120, 170, 255, 255);
        case PORT_DEBUG_COLLECTABLE: return IM_COL32(120, 255, 140, 255);
    }
    return IM_COL32(220, 220, 220, 255);
}

/* Developer, Show object names: a label at each actor near the glove that is on the screen. The position is
   projected with the camera of the latest drawn frame, so the labels follow the picture. */
void draw_object_names() {
    enum { MAX_LABELS = 160, MAX_SHOWN = 100 };
    static PortDebugLabel labels[MAX_LABELS];
    GfxSceneView view;
    ImVec2 size = ImGui::GetIO().DisplaySize;
    ImDrawList *list = ImGui::GetBackgroundDrawList();
    ImVec2 placed[MAX_SHOWN];
    int shown = 0;
    long n;

    if (!gfx_scene_view(&view)) {
        return;
    }
    n = port_debug_labels(labels, MAX_LABELS, (float)cfg->debugRange);
    for (long i = 0; i < n && shown < MAX_SHOWN; i++) {
        float sx, sy, depth;
        ImVec2 box;
        ImVec2 at;
        const char *text = labels[i].name;
        ImVec2 textSize = ImGui::CalcTextSize(text);

        if (!gfx_scene_project(&view, labels[i].pos[0], labels[i].pos[1], labels[i].pos[2], &sx, &sy, &depth)) {
            continue;
        }
        /* Only the picture's own area counts as the screen. */
        if (sx < view.frameX || sx > view.frameX + view.frameW || sy < view.frameY || sy > view.frameY + view.frameH) {
            continue;
        }
        at = ImVec2(sx * size.x, sy * size.y);
        box = ImVec2(at.x - textSize.x * 0.5f, at.y - textSize.y - 6.0f);
        /* A label that would lie on one already drawn moves down, to the next free row. */
        for (int tries = 0; tries < 6; tries++) {
            bool clash = false;

            for (int k = 0; k < shown; k++) {
                if (fabsf(placed[k].x - box.x) < textSize.x + 8.0f && fabsf(placed[k].y - box.y) < textSize.y + 2.0f) {
                    clash = true;
                    break;
                }
            }
            if (!clash) {
                break;
            }
            box.y += textSize.y + 3.0f;
        }
        placed[shown++] = box;
        list->AddCircleFilled(at, 2.5f, label_color(labels[i].kind));
        list->AddRectFilled(ImVec2(box.x - 3.0f, box.y - 1.0f), ImVec2(box.x + textSize.x + 3.0f, box.y + textSize.y + 1.0f),
                            IM_COL32(0, 0, 0, 160), 2.0f);
        list->AddText(box, label_color(labels[i].kind), text);
    }
}

void save_test_shot(int w, int h) {
#ifdef _WIN32
#define GL_CALL __attribute__((stdcall))
#else
#define GL_CALL
#endif
    typedef void (GL_CALL *ReadPixels)(int, int, int, int, unsigned, unsigned, void *);
    ReadPixels rp = (ReadPixels)SDL_GL_GetProcAddress("glReadPixels");
    const char *path = getenv("WEEMITT_MENU_SHOT");
    unsigned char *px = (unsigned char *)malloc((size_t)w * h * 3);
    FILE *f = fopen(path ? path : "weemitt-menu.ppm", "wb");

    if (rp && px && f) {
        rp(0, 0, w, h, 0x1907 /* GL_RGB */, 0x1401 /* GL_UNSIGNED_BYTE */, px);
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; y--) {
            fwrite(px + (size_t)y * w * 3, 3, (size_t)w, f);
        }
    }
    if (f) fclose(f);
    free(px);
}

}  // namespace

extern "C" {

void menu_init(SDL_Window *window, SDL_GLContext gl, Settings *st) {
    gameWindow = window;
    cfg = st;
    settings_path(cfgPath, sizeof(cfgPath));
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 4.0f;
    ImGui_ImplSDL2_InitForOpenGL(window, gl);
    ImGui_ImplOpenGL3_Init("#version 330 core");
    plog_ring_enable();
    const char *t = getenv("WEEMITT_MENU_TEST");
    if (t) {
        testFrames = atol(t);
    }
}

void menu_shutdown(void) {
    save_settings();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

int menu_handle_event(const SDL_Event *e) {
    if (rebindInput >= 0 && rebindPad) {
        char devName[8];
        bool joy = port_input_device(devName, sizeof(devName)) == PORT_DEVICE_JOYSTICK;
        int code = 0;

        if (joy) {
            if (e->type == SDL_JOYBUTTONDOWN) {
                code = settings_joy_code_from_button(e->jbutton.button);
            } else if (e->type == SDL_JOYAXISMOTION) {
                code = settings_joy_code_from_axis(e->jaxis.axis, e->jaxis.value);
            } else if (e->type == SDL_JOYHATMOTION) {
                code = settings_joy_code_from_hat(e->jhat.hat, e->jhat.value);
            }
        } else if (e->type == SDL_CONTROLLERBUTTONDOWN) {
            code = e->cbutton.button + 1;
        } else if (e->type == SDL_CONTROLLERAXISMOTION) {
            code = settings_pad_code_from_axis(e->caxis.axis, e->caxis.value);
        }
        if (code > 0) {
            if (joy) {
                bind_joy(rebindInput, rebindSlot, code);
            } else {
                bind_pad(rebindInput, rebindSlot, code);
            }
            rebindInput = -1;
            rebindPad = false;
            return 1;
        }
    }
    if (rebindDev != 0 && e->type == SDL_KEYDOWN && !e->key.repeat) {
        if (e->key.keysym.scancode != SDL_SCANCODE_ESCAPE) {
            int *mine = rebindDev == 1 ? &cfg->pauseKey : &cfg->stepKey;
            int *other = rebindDev == 1 ? &cfg->stepKey : &cfg->pauseKey;

            /* A key does one thing: the other frame step key gives it up. */
            if (*other == (int)e->key.keysym.scancode) {
                *other = 0;
            }
            *mine = e->key.keysym.scancode;
            settingsDirty = true;
        }
        rebindDev = 0;
        return 1;
    }
    if (e->type == SDL_KEYDOWN && !e->key.repeat) {
        SDL_Scancode sc = e->key.keysym.scancode;

        if (rebindInput >= 0) {
            if (sc != SDL_SCANCODE_ESCAPE && !rebindPad) {
                bind_key(rebindInput, rebindSlot, sc);
            }
            rebindInput = -1;
            rebindPad = false;
            return 1;
        }
        if (sc == SDL_SCANCODE_ESCAPE) {
            set_open(!menuOpen);
            return 1;
        }
    }
#ifdef PORT_STUDIO
    if (!menuOpen && studio_paused()) {
        /* The studio bar shows while paused: the mouse works on it. */
        ImGui_ImplSDL2_ProcessEvent(e);
        return (e->type == SDL_MOUSEBUTTONDOWN || e->type == SDL_MOUSEBUTTONUP || e->type == SDL_MOUSEMOTION
                || e->type == SDL_MOUSEWHEEL) && ImGui::GetIO().WantCaptureMouse;
    }
#endif
    if (menuOpen) {
        ImGui_ImplSDL2_ProcessEvent(e);
        /* Keys that act on the window (F11, Alt+Enter) still reach the platform layer. */
        return e->type != SDL_JOYDEVICEADDED && e->type != SDL_JOYDEVICEREMOVED && e->type != SDL_CONTROLLERDEVICEADDED
               && e->type != SDL_CONTROLLERDEVICEREMOVED && e->type != SDL_KEYDOWN && e->type != SDL_KEYUP && e->type != SDL_QUIT && e->type != SDL_WINDOWEVENT;
    }
    return 0;
}

int menu_is_open(void) {
    return menuOpen;
}

void menu_draw(SDL_Window *window) {
    frameCount++;
    if (testFrames >= 0 && frameCount == testFrames) {
        /* WEEMITT_MENU_WINDOWS=none keeps the menu closed and =developer opens only the Developer window. */
        const char *which = getenv("WEEMITT_MENU_WINDOWS");
        /* WEEMITT_OVERLAYS=collision,depth,names,pause turns the Developer window's tools on. */
        const char *tools = getenv("WEEMITT_OVERLAYS");

        if (which == NULL || strcmp(which, "none") != 0) {
            set_open(true);
            if (which != NULL && !strcmp(which, "developer")) {
                win.developer = true;
            } else {
                win.graphics = win.audio = win.controls = win.enhancements = win.cheats = win.developer = win.log = true;
                showFps = true;
            }
        }
        if (tools != NULL) {
            cfg->showCollision = strstr(tools, "collision") != NULL;
            cfg->collisionDepth = strstr(tools, "depth") != NULL;
            cfg->showNames = strstr(tools, "names") != NULL;
            cfg->pauseLogic = strstr(tools, "pause") != NULL;
        }
        plog_printf("menu test: the log window shows the latest lines");
    }
#ifdef PORT_STUDIO
    if (!menuOpen && !showFps && !rec_active() && !cfg->showNames && !cfg->pauseLogic && testFrames < 0
        && !studio_paused()) {
        return;
    }
#else
    if (!menuOpen && !showFps && !rec_active() && !cfg->showNames && !cfg->pauseLogic && testFrames < 0) {
        return;
    }
#endif
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    if (menuOpen) {
        menu_bar();
        /* Fresh windows start in a tidy cascade below the bar. */
        ImGui::SetNextWindowPos(ImVec2(20, 40), ImGuiCond_FirstUseEver);
        if (win.graphics) graphics_window();
        ImGui::SetNextWindowPos(ImVec2(20, 262), ImGuiCond_FirstUseEver);
        if (win.audio) audio_window();
        ImGui::SetNextWindowPos(ImVec2(480, 40), ImGuiCond_FirstUseEver);
        if (win.controls) controls_window();
        ImGui::SetNextWindowPos(ImVec2(20, 410), ImGuiCond_FirstUseEver);
        if (win.enhancements) enhancements_window();
        ImGui::SetNextWindowPos(ImVec2(20, 560), ImGuiCond_FirstUseEver);
        if (win.cheats) cheats_window();
        ImGui::SetNextWindowPos(ImVec2(250, 40), ImGuiCond_FirstUseEver);
        if (win.developer) developer_window();
        if (win.log) log_window();
#ifdef PORT_STUDIO
        if (win.studioLevel) studio_level_window();
        if (win.studioStates) studio_states_window();
#endif
    }
#ifdef PORT_STUDIO
    if (menuOpen || studio_paused()) {
        studio_bar();
        studio_search_window();
    }
#endif
    if (showFps) {
        char text[48];

        snprintf(text, sizeof(text), cfg->fpsCap > 0 ? "%.0f FPS (cap %d)" : "%.0f FPS", ImGui::GetIO().Framerate, cfg->fpsCap);
        ImGui::GetForegroundDrawList()->AddText(ImVec2(8, menuOpen ? 28.0f : 6.0f), IM_COL32(255, 255, 0, 255), text);
    }
    if (cfg->showNames) {
        draw_object_names();
    }
    if (cfg->pauseLogic) {
        char text[96];
        ImVec2 size = ImGui::GetIO().DisplaySize;
        float top = menuOpen ? 26.0f : 4.0f;
        ImVec2 textSize;

        snprintf(text, sizeof(text), "PAUSED   %s: step   %s: resume", key_name(cfg->stepKey), key_name(cfg->pauseKey));
        textSize = ImGui::CalcTextSize(text);
        ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2((size.x - textSize.x) * 0.5f - 6.0f, top),
                                                      ImVec2((size.x + textSize.x) * 0.5f + 6.0f, top + textSize.y + 4.0f),
                                                      IM_COL32(0, 0, 0, 170), 3.0f);
        ImGui::GetForegroundDrawList()->AddText(ImVec2((size.x - textSize.x) * 0.5f, top + 2.0f), IM_COL32(255, 200, 60, 255), text);
    }
    if (rec_active()) {
        /* The marker sits at the right edge, so it never covers the FPS text. */
        float x = ImGui::GetIO().DisplaySize.x - 44.0f;

        ImGui::GetForegroundDrawList()->AddCircleFilled(ImVec2(x, menuOpen ? 36.0f : 14.0f), 5.0f, IM_COL32(230, 30, 30, 255));
        ImGui::GetForegroundDrawList()->AddText(ImVec2(x + 10.0f, menuOpen ? 28.0f : 6.0f), IM_COL32(255, 255, 255, 255), "REC");
    }
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (testFrames >= 0 && frameCount == testFrames + 3) {
        int w, h;
        SDL_GL_GetDrawableSize(window, &w, &h);
        save_test_shot(w, h);
        SDL_Event q;
        q.type = SDL_QUIT;
        SDL_PushEvent(&q);
    }
}

}  // extern "C"
