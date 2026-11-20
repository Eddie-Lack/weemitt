PORT_C_BEGIN
#include "common.h"
#include "cheat.h"
#include "ident.h"
#include "controller.h"
#include "game.h"
#include "ball.h"
#include "hand.h"
#include "sound.h"
#include "text.h"
#include "macros.h"
#include "collect.h"
#include "maths.h"
#include "actor.h"
#include "banks.h"
#include "debug.h"
#include "overlay.h"
#include "bonus.h"
#include "decompress.h"
#include "spell.h"
#include "delta.h"
#include "spacedemo.h"

extern u8 ActiveController;
extern u8 cheatTempString[];
extern s32 controlMethod;
extern u8 gamePaused;
extern u8 cheatMsgCount;
extern s32 cheatCount;
extern s32 cheatTextPause;
extern s32 frameCount;
void SubText(struct Text *text);
extern s32 cheatAlphaVal;
extern PTR32(struct Text) cheatText;

typedef struct CheatString {
    char c[8];
} CheatString;

extern char cheatMsg[];
extern CheatString cheatButtonPressed;
void SetFont(u8 type);
void MusHandleStop(s32 handle, s32 speed);

extern s32 cheatSfx;
extern s32 cheatSfxCount;
struct Text *NEW_TEXT(s16 x, s16 y, u8 r, u8 g, u8 b, u8 a, f32 sx, f32 sy, u8 oneShot, char *str, u8 centre);

u32 UpdateCRC(u8 *ptr);

extern s32 cheatActivated;
extern u8 ballEnemyType;
extern PTR32(char) nmeObjectName[];
extern Actor plaster[4];
extern SpriteAnim garibAnimation[];
extern Vec3 cameraUpVectAim;
extern f32 gravity;
extern s16 fieldOfViewAim;
void InitSpriteAnimation(GameSprite *sprite, SpriteAnim *temp, s16 lifespan);
void FlashScreen(s16 r, s16 g, s16 b, s16 fadeVal, s16 fadeStep);

s32 CheckForCheats(void) {
    s8 cheat;

    /* The cheat entry reads presses and counts the sound's original frames: it runs in ticks. */
    if (!deltaTick) {
        return FALSE;
    }
    if (cheatSfxCount && --cheatSfxCount == 0) {
        MusHandleStop(cheatSfx, 0);
    }
    if (DeltaTickPressed(ActiveController, BUTTON_C_LEFT) || DeltaTickPressed(ActiveController, BUTTON_C_RIGHT) || DeltaTickPressed(ActiveController, BUTTON_C_UP) ||
        DeltaTickPressed(ActiveController, BUTTON_C_DOWN)) {
        cheatTempString[cheatMsgCount++] = FindCheatButton();
        cheatSfx = PlaySampleNot3D(cheatSampleData[cheatTempString[cheatMsgCount - 1]].id, 100, 128,
                                   cheatSampleData[cheatTempString[cheatMsgCount - 1]].pitch);
        cheatSfxCount = 12;
        cheat = CompareCheatString();
        if (cheat != -1) {
            ActivateCheat(cheat, 1);
            return TRUE;
        }
    }
    if (cheatMsgCount == 8 || DeltaTickPressed(ActiveController, BUTTON_Z)) {
        cheatMsgCount = 0;
        memset(cheatTempString, 0xFF, 8);
    }
    return FALSE;
}

s32 FindCheatButton(void) {
    if (DeltaTickPressed(ActiveController, BUTTON_C_LEFT)) {
        return 2;
    } else if (DeltaTickPressed(ActiveController, BUTTON_C_RIGHT)) {
        return 3;
    } else if (DeltaTickPressed(ActiveController, BUTTON_C_UP)) {
        return 1;
    } else if (DeltaTickPressed(ActiveController, BUTTON_C_DOWN)) { // keeps the last test a branch instead of sltu
        return 0;
    } else {
        return 0;
    }
}

s8 CompareCheatString(void) {
    s32 i;
    s32 j;
    u8 matches;

    for (i = 0; i < 26; i++) {
        matches = 0;
        for (j = 0; j < 8; j++) {
            if (cheatTempString[j] == cheatInfo[i].code[j]) {
                matches++;
            }
        }
        if (matches == 8) {
            return i;
        }
    }
    return -1;
}

void ActivateCheat(u8 num, u8 show) {
    char buf[64];
    GaribGroup *group;
    Garib *garib;
    s32 j;
    u8 cheatOn = FALSE;

    if (!CheatAllowed(num)) {
        return;
    }
    if (cheatInfo[num].type == 0) {
        if (cheatActivated & (1 << num)) {
            cheatActivated &= ~(1 << num);
        } else {
            cheatActivated |= 1 << num;
            cheatOn = TRUE;
        }
    }
    cheatCount = 0;
    switch (num) {
        case 4:
            if (cheatOn) {
                ActivateCollectable(POWERBALL, -1);
            } else {
                DeactivateCollectable(POWERBALL);
            }
            break;
        case 5:
            if (cheatOn) {
                ballStatic = 0;
                ball.flags = defaultBehaviour[ballControl.type].flags;
                ball.pos = hand.pos;
            } else {
                SetControlToHand();
            }
            break;
        case 7:
            if (cheatOn) {
                switch (gameInfo.world) {
                    case ATLANTIS:
                        ballEnemyType = 10;
                        break;
                    case CARNIVAL:
                        ballEnemyType = 28;
                        break;
                    case PREHISTORIC:
                        ballEnemyType = 7;
                        break;
                    case FEAR:
                        ballEnemyType = 43;
                        break;
                    case SPACE:
                        ballEnemyType = 25;
                        break;
                    case HUB:
                    case PIRATES:
                        ballEnemyType = 8;
                        break;
                }
                DeactivateCollectable(CRYSTALBALL);
                DeactivateCollectable(SNOWBALL);
                FindObject(&ball.inst, UpdateCRC((u8 *)nmeObjectName[ballEnemyType]), nmeObjectName[ballEnemyType], 1);
                AddObjectsSpritesToSpriteList(ball.inst->obj);
                ball.scale.x = ball.scale.y = ball.scale.z = ball.obj->spheres[0].radius *
                                                             enemyStats[ballEnemyType].scale /
                                                             defaultBehaviour[ballEnemyType].radius;
                ball.obj->spheres[0].targetRadius = defaultBehaviour[ballEnemyType].radius;
                for (j = 0; j < 4; j++) {
                    plaster[j].alpha = 0;
                }
            } else {
                RemoveObjectSprites(ball.inst->obj, 1);
                FindObject(&ball.inst, UpdateCRC((u8 *)"gball.ndo"), "gball.ndo", 1);
                ball.scale.x = ball.scale.y = ball.scale.z = ball.obj->spheres[0].radius * 0.05 / 9.0;
                ball.obj->spheres[0].targetRadius = defaultBehaviour[ballControl.type].radius;
                for (j = 0; j < 4; j++) {
                    plaster[j].alpha = 100;
                }
            }
            break;
        case 8:
            if (cheatOn) {
                gravity /= 2.0f;
            } else {
                gravity *= 2.0f;
            }
            break;
        case 9:
            if (!(cheatActivated & 0x80)) {
                if (cheatOn) {
                    ball.obj->spheres[0].targetRadius = 20.0f;
                } else {
                    ball.obj->spheres[0].targetRadius = defaultBehaviour[ballControl.type].radius;
                }
            }
            break;
        case 10:
            if (cheatOn) {
                fieldOfViewAim = 120;
            } else {
                fieldOfViewAim = 45;
            }
            break;
        case 11:
            RotateVector2DXYZ(&cameraUpVectAim, &cameraUpVectAim, -PI / 4, 2);
            break;
        case 12:
            RotateVector2DXYZ(&cameraUpVectAim, &cameraUpVectAim, PI / 4, 2);
            break;
        case 13:
            for (group = garibGroupList.head.prev; group != &garibGroupList.head; group = group->prev) {
                for (garib = group->garibs.head.prev; garib != &group->garibs.head; garib = garib->prev) {
                    if (garib->type == NORMAL_GARIB) {
                        if (cheatOn) {
                            InitSpriteAnimation(&garib->sprite, &garibAnimation[3], 0);
                        } else {
                            InitSpriteAnimation(&garib->sprite, &garibAnimation[0], 0);
                        }
                    }
                }
            }
            break;
        case 14:
            cheatOn = FALSE;
            if (player.powerups[DEATH]) {
                DeactivateCollectable(DEATH);
            } else {
                cheatOn = TRUE;
                ActivateCollectable(DEATH, 1000);
            }
            break;
        case 15:
            cheatOn = FALSE;
            if (player.powerups[FROGGY]) {
                DeactivateCollectable(FROGGY);
            } else {
                cheatOn = TRUE;
                ActivateCollectable(FROGGY, 1000);
            }
            break;
        case 16:
            cheatOn = FALSE;
            if (player.powerups[HERCULES]) {
                DeactivateCollectable(HERCULES);
            } else {
                cheatOn = TRUE;
                ActivateCollectable(HERCULES, 1000);
            }
            break;
        case 17:
            cheatOn = FALSE;
            if (player.powerups[SPEEDUP]) {
                player.powerups[SPEEDUP] = 1;
            } else {
                cheatOn = TRUE;
                ActivateCollectable(SPEEDUP, 1000);
            }
            break;
        case 18:
            cheatOn = FALSE;
            if (player.powerups[BOOMERANGBALL]) {
                DeactivateCollectable(BOOMERANGBALL);
            } else {
                cheatOn = TRUE;
                ActivateCollectable(BOOMERANGBALL, 1000);
            }
            break;
        case 19:
            cheatOn = FALSE;
            if (player.powerups[VANISHBALL]) {
                DeactivateCollectable(VANISHBALL);
            } else {
                cheatOn = TRUE;
                ActivateCollectable(VANISHBALL, 1000);
            }
            break;
        case 20:
            cheatOn = TRUE;
            if (handControl.spellTimer[2] > 50) {
                cheatOn = FALSE;
                handControl.spellTimer[2] = 50;
            } else {
                ActivateSpellHand(2);
                handControl.spellTimer[2] = 1000;
            }
            break;
        case 22:
            for (j = 0; j < 26; j++) {
                if ((cheatActivated & (1 << j)) && cheatInfo[j].type == 0) {
                    ActivateCheat(j, 0);
                }
            }
            cameraUpVectAim.x = 0.0f;
            cameraUpVectAim.z = 0.0f;
            cameraUpVectAim.y = 1.0f;
            break;
    }
    FlashScreen(255, 255, 255, 255, -30);
    if (show) {
        if (cheatText != NULL) {
            SubText(cheatText);
        }
        SetFont(0);
        if (cheatInfo[num].type == 1) {
            cheatText = NEW_TEXT(320, 120, 255, 255, 255, 255, 1.0f, 1.0f, 0, cheatInfo[num].msg, 0);
        } else {
            if (cheatOn) {
                sprintf(buf, "%s ON!", cheatInfo[num].msg);
            } else {
                sprintf(buf, "%s OFF!", cheatInfo[num].msg);
            }
            cheatText = NEW_TEXT(320, 120, 255, 255, 255, 255, 1.0f, 1.0f, 0, buf, 0);
        }
        PlaySampleNot3D(142, 100, 128, 128);
    }
}

void UpdateCheatText(void) {
    s32 x;

    SetFont(0);
    /* The message's position is an integer, and cheatCount counts original frames: they step in ticks. */
    if (deltaTick) {
        x = cheatText->x + GetStringPixelLength(cheatText->str, cheatText->scaleX, cheatText->scaleY) / 2;
        if (x > 160) {
            cheatText->x -= MIN(MAX(1, (x - 160) / 2), 50);
        } else {
            if (cheatCount++ > cheatTextPause) {
                cheatText->x -= (cheatCount - cheatTextPause) * 3;
            }
            if (cheatText->x < -480) {
                SubText(cheatText);
                cheatText = NULL;
            }
        }
    }
    SetFont(1);
    NEW_TEXT(112, 200 - cheatCount * 2, 255, 255, 255, MIN(510 - cheatCount * cheatAlphaVal, 255), 1.0f, 1.0f, 1,
             cheatButtonPressed.c, 0);
    if (deltaTick && (frameCount & 1) && cheatCount >= 3) {
        AddStarBurst(Random(60) + 130, 200 - cheatCount * 2, 2, SDT_STAR);
    }
}

void PrintCheatString(void) {
    s32 i = 0; // the early initialisation matches the original register use

    if (gamePaused || gameInfo.level == FLYTHRU2) {
        for (i = 0; i < 8; i++) {
            switch (cheatTempString[i]) {
                case 2:
                    cheatMsg[i] = 0xAA;
                    break;
                case 3:
                    cheatMsg[i] = 0xAB;
                    break;
                case 1:
                    cheatMsg[i] = 0xAC;
                    break;
                case 0:
                    cheatMsg[i] = 0xAD;
                    break;
                default:
                    cheatMsg[i] = ' ';
                    break;
            }
        }
        cheatMsg[i] = 0;
        if (gameInfo.level == FLYTHRU2) {
            return;
        }
    }
    if (gamePaused) {
        SetFont(1);
        NEW_TEXT(112, 200, 255, 255, 255, 255, 1.0f, 1.0f, 1, cheatMsg, 0);
    }
    if (!cheatCount && cheatMsgCount) {
        cheatMsgCount = 0;
        cheatButtonPressed = *(CheatString *)cheatMsg;
        memset(cheatTempString, 0xFF, 8);
    }
}

s32 CheatAllowed(s32 cheat) {
    switch (cheat) {
        case 1:
            if (gameInfo.level == CAVE) {
                return FALSE;
            }
            break;
        case 5:
            if (ballControl.enemy || handControl.attachedTo || gameInfo.level == CAVE || ballStatic) {
                return FALSE;
            }
            break;
        case 18:
        case 19:
            if (ballStatic || controlMethod == CONTROL_BALL) {
                return FALSE;
            }
            break;
        case 14:
        case 15:
            if (gameInfo.level == ATLANTISBOSS || gameInfo.level == CARNIVALBOSS || gameInfo.level == PIRATESBOSS
                || gameInfo.level == PREHISTORICBOSS || gameInfo.level == FEARBOSS || gameInfo.level == SPACEBOSS1
                || gameInfo.level == SPACEBOSS3) {
                return FALSE;
            }
            break;
    }
    return TRUE;
}

/* Port: the pause menu's cheat list. R or Z in the pause menu opens a panel beside it with every cheat. */

#define CHEAT_COUNT 26
#define CHEAT_ROWS 7
#define CHEAT_PANE_X 261
#define CHEAT_PANE_Y 116
#define CHEAT_PANE_W 114
#define CHEAT_PANE_H 160
#define CHEAT_GROW_STEPS 6

void DestroyOverlay(Overlay *overlay);
extern s32 frameCount2;

static Overlay cheatPane;
static s32 cheatMenuOpen;
static s32 cheatMenuSel;
static s32 cheatMenuTop;
static s32 cheatMenuGrow;
static s32 cheatMenuWait;

/* The Space demo starts with the infinite lives cheat on (spacedemo.c): the list leaves it and "all off" alone. The
   cheats that the port menu holds on (frame.c) are locked too. */
static s32 CheatMenuLocked(s32 num) {
    return (spaceDemo && (num == 3 || num == 22)) || ((portCheatHold >> num) & 1);
}

/* The state a cheat shows. The spell cheats follow the spell itself: a spell that ended or came from a pick-up
   leaves the cheat's bit out of step. */
static s32 CheatMenuOn(s32 num) {
    switch (num) {
        case 14:
            return player.powerups[DEATH] != 0;
        case 15:
            return player.powerups[FROGGY] != 0;
        case 16:
            return player.powerups[HERCULES] != 0;
        case 17:
            return player.powerups[SPEEDUP] != 0;
        case 18:
            return player.powerups[BOOMERANGBALL] != 0;
        case 19:
            return player.powerups[VANISHBALL] != 0;
        case 20:
            return handControl.spellTimer[2] > 50;
    }
    return (cheatActivated & (1 << num)) != 0;
}

/* 1 when R or Z in the pause menu opens the cheat list (Cheats window of the Escape menu). Outside port_ram. */
s32 cheatMenuEnabled = 1;

/* The cheat codes work in the same places as the pause menu's code entry. */
s32 CheatMenuAvailable(void) {
    return cheatMenuEnabled && !IsBonusLevel() && gameInfo.level != SPACEBOSS3;
}

s32 CheatMenuIsOpen(void) {
    return cheatMenuOpen;
}

void CheatMenuOpen(void) {
    FindAndSetupOverlay(&cheatPane, "tippane.bmp", 0, 0, 0, 1);
    cheatPane.x = CHEAT_PANE_X;
    cheatPane.y = CHEAT_PANE_Y;
    cheatPane.scaleX = cheatPane.scaleY = 0;
    cheatPane.alpha = 200;
    cheatPane.r = 0;
    cheatPane.g = 200;
    cheatPane.b = 0;
    AddOverlay(&cheatPane);
    cheatMenuOpen = TRUE;
    cheatMenuGrow = 0;
    cheatMenuWait = 1;
    cheatMenuSel = MIN(cheatMenuSel, CHEAT_COUNT - 1);
    cheatMenuTop = MIN(cheatMenuTop, cheatMenuSel);
    /* The code entry's buttons are ignored while the list is open: start it afresh. */
    memset(cheatTempString, 0xFF, 8);
    cheatMsgCount = 0;
    PlaySampleNot3D(181, 100, 128, 128);
}

void CheatMenuClose(void) {
    if (cheatMenuOpen) {
        DestroyOverlay(&cheatPane);
        cheatMenuOpen = FALSE;
    }
}

static void CheatMenuUseButton(s32 button) {
    pad[ActiveController].latch[button] = 1;
    pad[ActiveController].sincePress[button] = 99999;
}

/* Called once per update while the pause menu is open. step is 1 in the update that ends an original frame. */
void CheatMenuUpdate(s32 step, u8 alpha) {
    char buf[8];
    char *state;
    s32 i;
    s32 row;
    s32 grow;
    s32 on;
    s32 x;
    s16 y;
    f32 scale;
    f32 fgrow;
    u8 a;
    u8 r;
    u8 g;
    u8 b;

    if (step) {
        if (cheatMenuGrow < CHEAT_GROW_STEPS) {
            cheatMenuGrow++;
        }
        if (DeltaTickPressed(ActiveController, BUTTON_Z) || DeltaTickPressed(ActiveController, BUTTON_SHOULDER_RIGHT)
            || DeltaTickPressed(ActiveController, BUTTON_B)) {
            CheatMenuUseButton(BUTTON_Z);
            CheatMenuUseButton(BUTTON_SHOULDER_RIGHT);
            CheatMenuUseButton(BUTTON_B);
            PlaySampleNot3D(181, 100, 128, 128);
            CheatMenuClose();
            return;
        }
        if (DeltaTickPressed(ActiveController, BUTTON_A)) {
            CheatMenuUseButton(BUTTON_A);
            if (CheatAllowed(cheatMenuSel) && !CheatMenuLocked(cheatMenuSel)) {
                ActivateCheat(cheatMenuSel, 1);
            } else {
                PlaySampleNot3D(179, 100, 128, 128);
            }
        }
        if (pad[ActiveController].stickY >= 51 || DeltaTickPressed(ActiveController, BUTTON_DPAD_UP)) {
            if (!cheatMenuWait && cheatMenuSel > 0) {
                cheatMenuSel--;
                PlaySampleNot3D(179, 100, 128, 128);
            }
            cheatMenuWait = 1;
        } else if (pad[ActiveController].stickY < -50 || DeltaTickPressed(ActiveController, BUTTON_DPAD_DOWN)) {
            if (!cheatMenuWait && cheatMenuSel < CHEAT_COUNT - 1) {
                cheatMenuSel++;
                PlaySampleNot3D(179, 100, 128, 128);
            }
            cheatMenuWait = 1;
        } else {
            cheatMenuWait = 0;
        }
        if (cheatMenuSel < cheatMenuTop) {
            cheatMenuTop = cheatMenuSel;
        }
        if (cheatMenuSel >= cheatMenuTop + CHEAT_ROWS) {
            cheatMenuTop = cheatMenuSel - CHEAT_ROWS + 1;
        }
    }
    /* The pane grows from its centre by one step per original frame. */
    fgrow = cheatMenuGrow < CHEAT_GROW_STEPS ? (cheatMenuGrow + deltaPhase) / CHEAT_GROW_STEPS : 1.0f;
    cheatPane.scaleX = CHEAT_PANE_W * 32 * fgrow;
    cheatPane.scaleY = CHEAT_PANE_H * 32 * fgrow;
    if (cheatMenuGrow < CHEAT_GROW_STEPS) {
        return;
    }
    SetFont(0);
    NEW_TEXT(CHEAT_PANE_X, 48, 255, 255, 255, alpha, 0.7f, 0.7f, 1, "CHEATS", 1);
    SetFont(1);
    y = 64;
    for (row = 0; row < CHEAT_ROWS; row++) {
        i = cheatMenuTop + row;
        on = CheatMenuOn(i);
        r = g = b = 255;
        if (cheatInfo[i].type != 0) {
            state = "GO";
        } else {
            state = (char *)(on ? "ON" : "OFF");
        }
        if (!CheatAllowed(i) || CheatMenuLocked(i)) {
            state = "--";
            r = g = b = 140;
        } else if (cheatInfo[i].type == 0 && on) {
            r = 130;
            b = 130;
        }
        a = (i == cheatMenuSel && !(frameCount2 & 4)) ? alpha / 2 : alpha;
        scale = 0.65f;
        if (i == cheatMenuSel) {
            b = 120;
        }
        NEW_TEXT(CHEAT_PANE_X - CHEAT_PANE_W / 2 + 5, y, r, g, b, a, scale, scale, 1, cheatInfo[i].msg, 0);
        x = CHEAT_PANE_X + CHEAT_PANE_W / 2 - 5 - GetStringPixelLength(state, scale, scale);
        NEW_TEXT(x, y, r, g, b, a, scale, scale, 1, state, 0);
        y += 16;
    }
    sprintf(buf, "%d/%d", cheatMenuSel + 1, CHEAT_COUNT);
    NEW_TEXT(CHEAT_PANE_X, 184, 255, 255, 255, alpha, 0.6f, 0.6f, 1, buf, 1);
}

INCLUDE_ASM("asm/nonmatchings/cheat", pad_801BC70C);

PORT_C_END
