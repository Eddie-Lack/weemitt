/* The game's start-up and its frame. The platform layer (platform.c) calls port_game_init once and then
   port_game_update for each update. On the N64 the scheduler woke three threads once every desiredFrameRate
   video retraces (main.c); here one update runs their work in their priority order: the controller, the graphics,
   the game. */
PORT_C_BEGIN
#include "port.h"
#include "common.h"
#include "nnsched.h"
#include "game.h"
#include "frame.h"
#include "save.h"
#include "camera.h"
#include "actor.h"
#include "delta.h"
#include "spacedemo.h"
#include "ballworld.h"
#include "controller.h"
#include "display.h"
#include "cheat.h"
#include "porthooks.h"

extern MESG32 tlistPointers[2][10];
extern u8 draw_buffer;
extern u8 currentTask;
extern u8 desiredFrameRate;
extern u8 newDesiredFrameRate;
extern s32 osTvType;
extern u16 cfb_16_a[];
extern u16 cfb_16_b[];
extern u8 dontClearScreen;

void MainProc(void *arg);
void ControllerStep(void);
void GameStep(void);
void GraphicsStep(void);

u8 *port_rom;
u32 port_rom_size;
u16 port_buttons;
u32 port_controller_reads; /* how many times the game has read the controller (the recorder, platform.c, looks at it) */
s8 port_stick_x;
s8 port_stick_y;

extern s32 levelSelectCount;
extern s32 cheatActivated;
extern s16 currentSaveSlot;
extern s32 frameCount;
extern s32 frameCount2;
extern s32 ballLeftBehind;
extern s32 currentScreen;
extern s32 booting;
void FreeBackdropList(void);
void FreeLevel(void);
void StartNewGame(void);
void StopDrawing(char *name);
void StartNewLevel(s32 num);
void memclear(void *p, s32 size);

void (*port_pre_update_hook)(void);
void (*port_platforms_hook)(void);
void (*port_free_level_hook)(void);
void (*port_post_update_hook)(void);
void (*port_view_hook)(Mtx *projection, Mtx *view, u16 *perspNorm);
void (*port_draw_mark_hook)(s32 kind, struct Actor *actor);
s32 (*port_camera_hook)(void);
s32 (*port_no_shatter_hook)(void *face);
s32 (*port_point_sound_off_hook)(void);
f32 port_camera_dist_scale = 1.0f;

/* 1 after port_game_init started a level directly, until the game first enters the menus. */
static s32 levelStartedDirectly;

void port_set_space_demo(s32 on) {
    spaceDemo = on;
}

/* Set from the menu's Easy ball controls option. */
void port_set_easy_ball(s32 on) {
    easyBallControls = on;
}

/* Set from the menu's Camera follow speed option. */
void port_set_camera_follow(float speed) {
    cameraFollowSpeed = speed < 1.0f ? 1.0f : speed > 3.0f ? 3.0f : speed;
}

void port_set_camera_dist_scale(float scale) {
    port_camera_dist_scale = scale;
}

float port_get_camera_dist_scale(void) {
    return port_camera_dist_scale;
}

/* Ballworld: the requested value (menu, settings.ini or --ballworld) and the mode that the open save file belongs to.
   The game's switch ballworldOn takes the requested value when a level starts. While the two differ, the save file
   is not written. */
extern u8 ballworldOn;
static s32 ballworldRequested;
s32 port_save_ballworld;
s32 port_save_locked;

void port_set_ballworld(s32 on) {
    ballworldRequested = on != 0;
}

s32 port_get_ballworld(void) {
    return ballworldRequested;
}

void port_ballworld_latch(void) {
    if (ballworldOn && !ballworldRequested) {
        /* Glover gets back the health that the mode held at 2. */
        BallworldLeave();
    }
    ballworldOn = ballworldRequested;
    /* Once a level has run in the other file's mode, the game's copy of the save holds that mode's progress. The lock
       stays until the program restarts. */
    if (ballworldOn != port_save_ballworld) {
        port_save_locked = 1;
    }
}

s32 port_get_easy_ball(void) {
    return easyBallControls;
}

float port_get_camera_follow(void) {
    return cameraFollowSpeed;
}

/* ---- Options that play with the game (menu: Enhancements and Cheats). All are off by default and then change
   nothing. */

extern u8 gamePaused;
extern u8 pauseMode;
extern s32 autoPlayback;
extern PTR32(u16) recordKeys;
extern PTR32(u16) rkp;
extern s16 nextKeyFrame;
extern s16 exitCount;
extern u16 playbackButtons;
void JallocFree(void *ptr);

/* The game's own cheat bits that the options hold on (cheat.h). The cheat list of the pause menu leaves them alone. */
s32 portCheatHold;
static s32 skipIntro;
static s32 introStage; /* 0: no intro to skip; 1: the presentation comes; 2: the fly-through comes */
static s32 introPulse;
static s32 gotoLevel = -1;

void port_set_cheat_hold(s32 mask) {
    portCheatHold = mask;
}

s32 port_get_cheat_bits(void) {
    return cheatActivated;
}

/* Turns one cheat on or off now, and holds it so that a new level or the code entry does not undo it. */
void port_set_cheat_bit(s32 num, s32 on) {
    if (on) {
        cheatActivated |= 1 << num;
        portCheatHold |= 1 << num;
    } else {
        cheatActivated &= ~(1 << num);
        portCheatHold &= ~(1 << num);
    }
}

s32 port_space_demo(void) {
    return spaceDemo;
}

void port_set_pause_cheats(s32 on) {
    extern s32 cheatMenuEnabled;

    cheatMenuEnabled = on != 0;
}

void port_set_skip_intro(s32 on) {
    skipIntro = on;
}

/* The Start button that skips the start-up screens, as a player's press does. The presentation and the fly-through
   both end on a press (the controller code for a recorded demo, DoFlythruScreen). The press comes every other
   original frame, so each one is a fresh press, until the screen starts to leave. platform.c adds the buttons to
   the update's, which the recorder then sees. */
u16 port_intro_skip_buttons(void) {
    if (!skipIntro || introStage == 0 || spaceDemo) {
        return 0;
    }
    if (deltaTick) {
        introPulse ^= 1;
    }
    if (introStage == 1 && gameInfo.level == FLYTHRU) {
        introStage = 2;
    }
    if (gameInfo.mode == 1 && currentScreen == 1) {
        introStage = 0;
        return 0;
    }
    if (gameInfo.mode == 4 && (gameInfo.level == PRESENT || gameInfo.level == FLYTHRU) && !(gameInfo.exitFlags & 1)
        && introPulse) {
        return 0x1000;
    }
    return 0;
}

/* The title menu has set itself up, or a level is running. These are the places a level can be left for another one. */
s32 port_goto_possible(void) {
    return !spaceDemo && (gameInfo.mode == 4 || (gameInfo.mode == 1 && currentScreen != 0 && levelSelectCount > 0));
}

void KillTitle(void);

/* Starts a level the way the level select does (save slot 6, medium difficulty), from a level or from the title
   menu. The cheats stay as they are. */
static void StartLevelNow(s32 level) {
    /* A recorded demo (the start-up scene, the title's demos) plays from keys that its level loaded. The mode that
       the title menu's debug Z chose stays: the new level then records or plays back, as a level from the menu. */
    if (autoPlayback && gameInfo.mode != 1) {
        JallocFree(&recordKeys);
        rkp = NULL;
        nextKeyFrame = -1;
        exitCount = 0;
        playbackButtons = 0;
        autoPlayback = 0;
    }
    CheatMenuClose();
    pauseMode = 0;
    gamePaused = 0;
    currentSaveSlot = 6;
    memclear(&saveSlot[6], sizeof(SaveSlot));
    saveSlot[currentSaveSlot].difficultyLevel = DIFFICULTY_MEDIUM;
    cameraInfo.overrideTimer = 0;
    if (gameInfo.mode == 1) {
        /* As DemoKeyRun does: the title frees its own objects. */
        StopDrawing("goto");
        memset(cfb_16_a, 0, 0x25800);
        memset(cfb_16_b, 0, 0x25800);
        KillTitle();
    } else {
        levelSelectCount = 0;
        FreeBackdropList();
        frameCount = frameCount2 = 0;
        FreeLevel();
    }
    StartNewGame();
    StopDrawing("levelselec");
    ballLeftBehind = -1;
    StartNewLevel(level);
}

/* The next update starts the level. A level the menu does not offer does nothing. */
void port_goto_level(s32 level) {
    if (level >= 0 && level < PRESENT) {
        gotoLevel = level;
    }
}

/* The number of levels: port_game_init takes 0 to port_level_count() - 1, and port_goto_level 0 to
   port_goto_level_count() - 1. */
int port_level_count(void) {
    return NUM_LEVELS;
}

int port_goto_level_count(void) {
    return PRESENT;
}

/* Starts the game. With a level number, it then starts that level the way the level select does, in save slot 6
   (the one the level select uses), at medium difficulty. */
void port_game_init(s32 level) {
    port_load_ram();
    port_set_rom_symbols();
    port_ballworld_latch();
    osTvType = OS_TV_NTSC;
    MainProc(NULL);
    if (spaceDemo) {
        SpaceDemoStart();
        return;
    }
    if (level < 0) {
        introStage = 1;
        return;
    }
    levelSelectCount = 0;
    cheatActivated = 0;
    currentSaveSlot = 6;
    memclear(&saveSlot[6], sizeof(SaveSlot));
    saveSlot[currentSaveSlot].difficultyLevel = DIFFICULTY_MEDIUM;
    cameraInfo.overrideTimer = 0;
    FreeBackdropList();
    frameCount = frameCount2 = 0;
    FreeLevel();
    StartNewGame();
    StopDrawing("levelselec");
    ballLeftBehind = -1;
    /* Level 40 (SPACEBOSS3) calls ResetHand before ResetBall, and ResetHand writes through ball.phys. On the N64
       an earlier level has always set ball.phys. This is the value ResetBall gives it. */
    ball.phys = &defaultBehaviour[0];
    StartNewLevel(level);
    levelStartedDirectly = TRUE;
}

extern s32 autoPlayback;
extern u32 rseed;

/* One update of the game: the controller, the game and the graphics, for dt original game frames (0 < dt <= 1,
   see delta.h). deltaTick is 1 when an original frame ends during this update. The scheduler used to change the
   frame rate when it sent a frame's message: desiredFrameRate takes the value the game asked for. Returns the
   number of display lists the update built, or -1 when it built none. */
s32 port_game_update(f32 dt, PortFrameTask *tasks, s32 max) {
    s32 count = 0;
    s32 i;
    u32 seed;

    if (portCheatHold) {
        cheatActivated |= portCheatHold;
    }
    if (gotoLevel >= 0) {
        if (port_goto_possible()) {
            StartLevelNow(gotoLevel);
        }
        gotoLevel = -1;
    }
    if (port_pre_update_hook != NULL) {
        port_pre_update_hook();
    }
    DeltaBeginUpdate(dt);
    port_input_update(port_buttons);
    port_debug_pad_update();
    if (deltaTick) {
        desiredFrameRate = newDesiredFrameRate ? newDesiredFrameRate : desiredFrameRate;
    }
    /* The game reads the controller once per original frame, as on the N64: its button edges, hold times and
       recorded input count original frames. The N64 reads it at the start of a frame, and the whole frame moves
       with the new stick and buttons. Here the first update of a frame window reads it, and the presses act in
       that update. The window's other updates see none (DeltaHoldInput). Code that runs only in the tick gets
       them from DeltaTickPressed. */
    if (deltaNewFrame) {
        ControllerStep();
    }
    DeltaHoldInput();
    /* The N64's threads run in priority order when the frame's message arrives: the controller, then the
       graphics, then the game. The graphics therefore show the state before this update's game logic. The draw
       path calls Random. Outside a tick its calls leave the game's random sequence as it was. */
    currentTask = 1;
    seed = rseed;
    GraphicsStep();
    if (!deltaTick) {
        rseed = seed;
    }
    if (dontClearScreen) {
        count = -1;
    } else {
        /* CleanupAndSendDisplayList(0, 0x40) swapped draw_buffer: the lists just built are in the other buffer. */
        for (i = 1; i < currentTask && i < 10 && count < max; i++) {
            NNScTask *t = (NNScTask *)tlistPointers[draw_buffer ^ 1][i];
            tasks[count].addr = (u32)t->list.t.data_ptr;
            tasks[count].size = t->list.t.data_size;
            tasks[count].sprite2d = t->list.t.ucode == (u64 *)gspSprite2DTextStart;
            count++;
        }
    }
    /* Without a sound device the sound player runs on the game's clock. It gets this update's time before the
       game logic runs. With a device this does nothing. */
    musAudioAdvance(dt * (f32)port_game_retraces());
    GameStep();
    if (port_post_update_hook != NULL) {
        port_post_update_hook();
    }
    /* On the N64 the way into a level always passes the title menu, which leaves currentScreen at 1 and booting at
       0. A level started directly skips the menu. Only the menus read the two, so they get those values when the
       game first enters the menus. Otherwise DoMenu replays the start-up screens and skips InitDebris, and the
       level's debris are drawn from freed memory. */
    if (levelStartedDirectly && gameInfo.mode == 1) {
        levelStartedDirectly = FALSE;
        if (currentScreen == 0) {
            currentScreen = 1;
            booting = 0;
        }
    }
    DeltaKeepInput();
    DeltaEndUpdate();
    return count;
}

/* One original game frame, as on the N64. */
s32 port_game_frame(PortFrameTask *tasks, s32 max) {
    return port_game_update(1.0f, tasks, max);
}

/* 1 when the update that just ran ended an original game frame. */
s32 port_game_ticked(void) {
    return deltaTick;
}

/* 1 while a recorded demo plays (the attract mode). Its recorded input only matches the original frame rate. */
s32 port_game_demo(void) {
    return autoPlayback == 2;
}

/* How long one game frame lasts on the N64, in video retraces (60 a second). */
s32 port_game_retraces(void) {
    u8 rate = newDesiredFrameRate ? newDesiredFrameRate : desiredFrameRate;

    return rate ? rate : 1;
}

/* The game's two dynamic buffers (display list, matrices and vertices of a frame), which it rebuilds in every frame.
   The addresses are Ptr32 words (ptr32.h), the form that the display lists hold. */
extern Dynamic dynamic[2];

void port_game_dynamic_range(unsigned int *lo, unsigned int *hi) {
    *lo = PORT_P2W(&dynamic[0]);
    *hi = PORT_P2W(&dynamic[2]);
}

/* 1 when the frame just built does not continue the one before it, so that a picture must not blend the two: the
   game mode or the level changed, or the frame counter went back (a level or a screen started again). Call it
   once for each game frame. */
int port_game_cut(void) {
    static s32 prevMode = -1;
    static s32 prevLevel = -1;
    static s32 prevCount = -1;
    int cut = gameInfo.mode != prevMode || gameInfo.level != prevLevel || frameCount < prevCount;

    prevMode = gameInfo.mode;
    prevLevel = gameInfo.level;
    prevCount = frameCount;
    return cut;
}

void port_game_framebuffers(u32 *a, u32 *b) {
    *a = PORT_P2W(cfb_16_a);
    *b = PORT_P2W(cfb_16_b);
}

s32 port_game_mode(void) {
    return gameInfo.mode;
}

s32 port_game_level(void) {
    return gameInfo.level;
}

extern u32 rseed;

/* One line of game state for comparing runs (platform.c --trace): the frame counter, the mode and level, the
   random seed, and the positions of the glove, the ball and the camera, with each float as its bits. */
void port_game_trace(char *buf, s32 size) {
    union {
        f32 f;
        u32 i;
    } v[9];
    s32 i;

    v[0].f = hand.pos.x;
    v[1].f = hand.pos.y;
    v[2].f = hand.pos.z;
    v[3].f = ball.pos.x;
    v[4].f = ball.pos.y;
    v[5].f = ball.pos.z;
    v[6].f = camera.pos.x;
    v[7].f = camera.pos.y;
    v[8].f = camera.pos.z;
    i = snprintf(buf, size, "fc %d mode %d level %d rseed %08x", frameCount, gameInfo.mode, gameInfo.level, rseed);
    snprintf(buf + i, size - i, " hand %08x %08x %08x ball %08x %08x %08x cam %08x %08x %08x", v[0].i,
             v[1].i, v[2].i, v[3].i, v[4].i, v[5].i, v[6].i, v[7].i, v[8].i);
}

/* The positions of the glove, the ball and the camera, as port_game_trace prints them (the library's
   glover_bodies). */
void port_game_bodies(float out[9]) {
    out[0] = hand.pos.x;
    out[1] = hand.pos.y;
    out[2] = hand.pos.z;
    out[3] = ball.pos.x;
    out[4] = ball.pos.y;
    out[5] = ball.pos.z;
    out[6] = camera.pos.x;
    out[7] = camera.pos.y;
    out[8] = camera.pos.z;
}

/* One line per update for studying the glove and the ball (platform.c --trace-updates): the frame window, the
   glove's action, and the velocities and positions as decimal numbers. */
extern u8 ActiveController;

void port_game_trace_update(char *buf, s32 size) {
    /* Outside a level (the menus) the glove has no state. */
    if (hand.state == NULL) {
        snprintf(buf, size, "fc %d new %d tick %d dt %.4f mode %d (no glove state)", frameCount, (int)deltaNewFrame,
                 (int)deltaTick, (double)deltaTime, gameInfo.mode);
        return;
    }
    snprintf(buf, size,
             "fc %d new %d tick %d dt %.4f mode %d prev %d link %d hold B %d Z %d | ball vel %.3f %.3f %.3f speed %.3f pos %.2f %.2f %.2f | hand vel %.3f %.3f %.3f pos %.2f %.2f %.2f",
             frameCount, (int)deltaNewFrame, (int)deltaTick, (double)deltaTime, hand.state->mode, hand.state->prevMode,
             ball.link == &hand, pad[ActiveController].holdTime[BUTTON_B], pad[ActiveController].holdTime[BUTTON_Z],
             ball.vel.x, ball.vel.y, ball.vel.z, sqrt(ball.vel.x * ball.vel.x + ball.vel.z * ball.vel.z), ball.pos.x,
             ball.pos.y, ball.pos.z, hand.vel.x, hand.vel.y, hand.vel.z, hand.pos.x, hand.pos.y, hand.pos.z);
}

PORT_C_END
