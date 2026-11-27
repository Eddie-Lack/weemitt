/* The Space demo: a build that starts in Space 1 and joins Space 1, 2 and 3 into one run. A level's exit portal
   works as a door: the glove that brings the ball to it goes straight into the next level, without the level-done
   spin and the score screen. After Space 3 a screen shows the time of each level and their sum. The glove has the
   easy difficulty's ball controls (controller.c), and lives do not run out (the game's own cheat 3, INFINITE LIVES).

   The game calls in at four places: CheckForExit (the door), GameStep's end-of-level mode (the next level),
   GameStep's menu mode (a run that quits from the pause menu starts again) and the screen of times, which has its
   own mode. */
PORT_C_BEGIN
#include "common.h"
#include "texture.h"
#include "game.h"
#include "overlay.h"
#include "frontend.h"
#include "save.h"
#include "hand.h"
#include "ball.h"
#include "actor.h"
#include "debris.h"
#include "sound.h"
#include "banks.h"
#include "crc.h"
#include "text.h"
#include "jalloc.h"
#include "controller.h"
#include "camera.h"
#include "platform.h"
#include "maths.h"
#include "delta.h"
#include "spacedemo.h"

/* game.c's WorldInfo. */
typedef struct WorldInfo {
    /* 0x00 */ s8 objBank;
    /* 0x01 */ s8 texBank;
    /* 0x02 */ s8 firstLevel;
    /* 0x03 */ u8 secondLevel;
    /* 0x04 */ s8 lastLevel;
    /* 0x05 */ u8 extraLevels[0x3];
    /* 0x08 */ char backdrop[0x10];
} WorldInfo;

extern WorldInfo worldInfo[];
extern Player player;
extern s32 cheatActivated;
extern s16 currentSaveSlot;
extern s32 levelSelectCount;
extern s32 frameCount;
extern s32 frameCount2;
extern s32 ballLeftBehind;
extern u8 newDesiredFrameRate;
extern s16 farPlaneDist;
extern u8 ActiveController;
extern Overlay timePicOL;
extern PTR32(struct Actor) foregroundObjects[];
extern s32 numForegroundObjects;
void LoadTextureBank(s32 num);
void LoadObjectBank(s32 num);
void PrepareSong(u8 song);
void FreeScreen(void);
void StartNewLevel(s32 num);
struct Text *NEW_TEXT(s16 x, s16 y, u8 r, u8 g, u8 b, u8 a, f32 sx, f32 sy, u8 oneShot, char *str, u8 centre);
void SetFont(u8 type);
void DisableControl(s16 time);
s16 GetStringPixelLength(char *str, f32 sx, f32 sy);
void memclear(void *p, s32 size);

#define FIRST_LEVEL SPACE2A
#define NUM_LEVELS 3

s32 spaceDemo;
s32 easyBallControls;
f32 cameraFollowSpeed = 1.0f;

/* The original frames each level took, from its start to the glove reaching its exit. */
static s32 levelFrames[NUM_LEVELS];

/* The rows of the screen of times: the three levels and the total. */
#define NUM_ROWS 4

typedef struct ResultRow {
    s32 delay; /* original frames before the row starts to slide in */
    f32 offset; /* how far the row still is from its place, in screen units */
    s32 landed;
} ResultRow;

static ResultRow rows[NUM_ROWS];
static const s16 rowY[NUM_ROWS] = { 58, 96, 134, 184 };
static s32 resultsCount;
static s32 resultsLeaving;

/* The level start of the level select, in its save slot, at medium difficulty. */
static void StartRun(void) {
    levelSelectCount = 0;
    currentSaveSlot = 6;
    memclear(&saveSlot[6], sizeof(SaveSlot));
    saveSlot[currentSaveSlot].difficultyLevel = DIFFICULTY_MEDIUM;
    cameraInfo.overrideTimer = 0;
    frameCount = frameCount2 = 0;
    StartNewGame();
    cheatActivated = 8;
    StopDrawing("spacedemo");
    ballLeftBehind = -1;
    memclear(levelFrames, sizeof(levelFrames));
    newDesiredFrameRate = 3;
    StartNewLevel(FIRST_LEVEL);
}

/* Starts the demo after MainProc, in place of the title screens. */
void SpaceDemoStart(void) {
    FreeBackdropList();
    FreeLevel();
    StartRun();
}

/* Starts the run again from Space 1, after the screen of times or when the game goes back to the menus. The level
   is already freed. */
void SpaceDemoRestart(void) {
    StartRun();
}

/* The glove has brought the ball to the level's exit (CheckForExit, in a tick). The level's time stops here. */
void SpaceDemoDoor(struct Platform *platform) {
    levelFrames[gameInfo.level - FIRST_LEVEL] = player.levelFrames;
    PlaySample(Random(4) + 269, &hand.pos, 100, 128);
    PlaySample(165, &hand.pos, 100, 140);
    CreateStarCircle(&ball.pos, 5, 19);
    DisableControl(10000);
    ExitLevel(LEVEL_EXIT, 1, 1);
}

/* The level has faded out and is freed (CheckForExits). gameInfo.level is the next one. */
void SpaceDemoNextLevel(void) {
    if (gameInfo.prevLevel < FIRST_LEVEL + NUM_LEVELS - 1) {
        StartNewLevel(gameInfo.level);
        return;
    }
    gameInfo.mode = SPACE_DEMO_RESULTS_MODE;
    resultsCount = 0;
    resultsLeaving = FALSE;
}

/* A time as minutes, seconds and hundredths. An original frame in a level is a twentieth of a second. */
static void FormatTime(char *str, s32 frames) {
    sprintf(str, "%d:%02d.%02d", frames / 1200, frames / 20 % 60, frames % 20 * 5);
}

static void ShowRow(s32 i, char *label, s32 frames, u8 r, u8 g, u8 b) {
    char str[16];
    s32 x = rows[i].offset;

    NEW_TEXT(48 + x, rowY[i], r, g, b, 255, 1.0f, 1.0f, 1, label, 0);
    FormatTime(str, frames);
    NEW_TEXT(272 + x - GetStringPixelLength(str, 1.0f, 1.0f), rowY[i], r, g, b, 255, 1.0f, 1.0f, 1, str, 0);
}

/* The screen of times. It works like the end-of-level screen: a backdrop, swinging boxes behind the rows, texts
   made again in every update, and a fade out on A. */
void SpaceDemoResults(void) {
    Backdrop *backdrop;
    char label[16];
    s32 i;
    s32 total;
    f32 rot;
    u8 glow;
    /* resultsCount and the rows' delays count original frames. The set-up runs in the screen's first update. */
    s32 step = deltaTick || resultsCount == 0;

    if (resultsCount == 0 && !resultsLeaving) {
        gameInfo.exitFlags &= ~3;
        newDesiredFrameRate = 2;
        gameInfo.fade = 300;
        fog.on = fog.r = fog.g = fog.b = 0;
        farPlaneDist = 1000;
        LoadTextureBank(LEVEND2_TEX_BANK);
        LoadObjectBank(70);
        FadeScreenIn();
        backdrop = (Backdrop *)JallocAlloc(sizeof(Backdrop), 1, "Backdrop");
        AddBackdrop(backdrop);
        FindTexture(&backdrop->tex, UpdateCRC((u8 *)worldInfo[SPACE].backdrop), 1);
        backdrop->r = backdrop->g = backdrop->b = backdrop->alpha = 255;
        backdrop->scaleX = 0x400;
        backdrop->scaleY = 0x400;
        InitDebris();
        FindSetupAndAddOverlay(&timePicOL, "eolcl01.bmp", 160, 24, 2, 1);
        timePicOL.frames = (SpriteFrames *)&spriteFrameList[EOL_CLOCK_ANIM];
        /* The boxes stand where the end-of-level screen's boxes stand behind texts at these heights: 0.51
           screen units per box unit, centred on height 128.5. */
        for (i = 0; i < NUM_ROWS; i++) {
            initFGObject("titleboxb.ndo", 0.0f, (128.5f - rowY[i] - 12.0f) / 0.51f, 500.0f, 0.0f, 1.5f, 0.0f,
                         i == NUM_ROWS - 1 ? 2.4f : 2.2f, i == NUM_ROWS - 1 ? 1.0f : 0.8f, 1.4f, 255);
            initFGObject("titleboxbf.ndo", 0.0f, (128.5f - rowY[i] - 12.0f) / 0.51f, 500.0f, 0.0f, 1.5f, 0.0f,
                         i == NUM_ROWS - 1 ? 2.4f : 2.2f, i == NUM_ROWS - 1 ? 1.0f : 0.8f, 1.4f, 128);
            rows[i].delay = 10 + i * 12;
            rows[i].offset = (i & 1) ? 320.0f : -320.0f;
            rows[i].landed = FALSE;
        }
        InitScreenDebris();
        SetFont(0);
        PrepareSong(HASBRO_TRACK);
        StartDrawing("spacedemo");
    }
    HandleScreenDebris();
    for (i = 0; i < NUM_ROWS; i++) {
        if (rows[i].delay != 0) {
            if (step) {
                rows[i].delay--;
            }
            continue;
        }
        if (!rows[i].landed) {
            rows[i].offset -= rows[i].offset * DeltaLerp(0.25f);
            if (ABS(rows[i].offset) < 1.0f) {
                rows[i].offset = 0.0f;
                rows[i].landed = TRUE;
                PlaySampleNot3D(140, 100, 128, 128);
            }
        }
    }
    for (i = 0; i < NUM_ROWS; i++) {
        rot = rows[i].offset / 100.0f;
        rot = rot < -1.64f ? -1.64f : rot > 1.5f ? 1.5f : rot;
        foregroundObjects[i * 2]->rot.y = foregroundObjects[i * 2 + 1]->rot.y = rot;
        if (rows[i].delay != 0 || rot <= -1.63f || rot >= 1.49f) {
            foregroundObjects[i * 2]->alpha = foregroundObjects[i * 2 + 1]->alpha = 0;
        } else {
            foregroundObjects[i * 2]->alpha = 255;
            foregroundObjects[i * 2 + 1]->alpha = 128;
        }
        foregroundObjects[i * 2]->rot.z = foregroundObjects[i * 2 + 1]->rot.z =
            SineWave(15.0f, resultsCount + DeltaLag(), i * 60) / 9.0f;
    }
    for (i = 0; i < numForegroundObjects; i++) {
        GetQuaternionFromXYZRot(&foregroundObjects[i]->rotQuat, &foregroundObjects[i]->rot);
    }
    total = 0;
    for (i = 0; i < NUM_LEVELS; i++) {
        total += levelFrames[i];
        if (rows[i].delay == 0) {
            sprintf(label, "SPACE %d", i + 1);
            ShowRow(i, label, levelFrames[i], 255, 255, 255);
        }
    }
    if (rows[NUM_ROWS - 1].delay == 0) {
        glow = SineWave(4.0f, resultsCount + DeltaLag(), 0) * 64.0f + 191.0f;
        ShowRow(NUM_ROWS - 1, "TOTAL", total, glow, glow, 128);
        if (step && rows[NUM_ROWS - 1].landed && resultsCount & 1 && resultsCount < 120) {
            AddStarBurst(160 + Random(161) - 80, rowY[NUM_ROWS - 1] + Random(11), Random(3) + 3, SDT_STAR2);
        }
    }
    if (rows[NUM_ROWS - 1].landed && !resultsLeaving) {
        NEW_TEXT(160, 230, 255, 255, 255, SineWave(30.0f, resultsCount + DeltaLag(), 0) * 100.0f + 155.0f, 0.6f, 0.6f, 1,
                 "PRESS A TO PLAY AGAIN", 1);
    }
    if (step) {
        resultsCount++;
    }
    if (!resultsLeaving && rows[NUM_ROWS - 1].landed && pad[ActiveController].pressed[BUTTON_A]) {
        resultsLeaving = TRUE;
        FadeScreenOut();
    }
    if (resultsLeaving && ScreenFadedOut()) {
        SubOverlay(&timePicOL);
        FreeFGObjects();
        FreeScreen();
        resultsCount = 0;
        resultsLeaving = FALSE;
        SpaceDemoRestart();
    }
}

PORT_C_END
