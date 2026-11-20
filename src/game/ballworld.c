/* Copied from the same author's Ballworld mod for the N64 game. Keep it in sync with the mod's file:
   replace this copy as a whole. The copy differs in three places, which the mod's own copy should take too: the
   PORT_C_BEGIN and PORT_C_END lines, the cast of JallocAlloc's result, and PTR32 in the declaration of blastTex. */
/* Ballworld's game logic: every level is played as the ball alone. The N64 build and the PC port both compile
 * this file. It uses only the game's own functions and data. */
/* weemitt compiles the game as C++: the file's functions and variables keep their C names. */
#ifdef PORT
PORT_C_BEGIN
#endif
#include "common.h"
#include "actor.h"
#include "ball.h"
#include "hand.h"
#include "camera.h"
#include "controller.h"
#include "game.h"
#include "bonus.h"
#include "platform.h"
#include "enemy.h"
#include "footprint.h"
#include "debris.h"
#include "quake.h"
#include "jalloc.h"
#include "sound.h"
#include "overlay.h"
#include "cameo.h"
#include "puzzle.h"
#include "maths.h"
#include "ballworld.h"

/* The PC port can run the logic more often than the N64's 20 times a second (weemitt's docs/delta-time.md). Its
 * decisions and counters run only in the update that ends an original frame (a tick), and its motion is scaled by
 * the update's share of a frame. On the N64 every call is a whole frame. */
#ifdef PORT
#include "delta.h"
#define TICK deltaTick
#define SCALED(x) DT(x)
/* A press of the button in this frame, as the tick sees it (the port clears pad[].pressed after the first update). */
#define PRESSED(button) DeltaTickPressed(ActiveController, button)
#else
#define TICK 1
#define SCALED(x) (x)
#define PRESSED(button) pad[ActiveController].pressed[button]
#endif

/* weemitt keeps each pointer of the game's memory as a 32-bit word, a PTR32 (its docs/port64.md). On the N64 it is
 * a plain pointer. */
#ifndef PTR32
#define PTR32(T) T *
#endif

extern s32 controlMethod;
extern s32 cheatActivated;
extern u8 ActiveController;
extern u8 gamePaused;
extern s16 levelExitTimer;
extern s16 worldExitTimer;
extern s16 newLevExitTimer;
extern PTR32(Texture) blastTex;
extern f32 gravity;
extern s32 spellDuration;
extern Actor plaster[];
extern s32 ballStatic;
extern Stat stats[];
void ActivateSpell(Actor *actor, s16 spell, s16 duration);
Platform *TagToFirstPlatform(s32 tag);
void PlaySpellSample(void);
s32 GameBeingPlayed(void);
void FindFlattestPlane(Actor *actor, Plane **plane, s32 *planeNum);

u8 ballworldOn;

/* Game frames between R and the change of form. Glover's spin takes about as long before its first change. */
#define TRANSFORM_DELAY 8
/* HandTransformBall sets transformState to -20; MoveBall counts it up to 0. Glover's spin takes another R press
 * while it is at -15 or above (MaintainAction, HAND_SPIN). */
#define TRANSFORM_REPEAT (-15)


/* The pound: the ball's version of Glover's slam. Z in the air starts it. As Glover's (MaintainAction, HAND_SLAM),
 * the ball stops for SLAM_HOLD game frames, then falls at SLAM_SPEED, and its speed across the ground halves each
 * frame. The landing is the slam. */
#define SLAM_HOLD 3
#define SLAM_SPEED 14.0f

/* The mode's state. It is one struct because the assembler (gloveworld's tools/asn64.py) aligns only the
 * uninitialized variables larger than 4 bytes; a lone s16 could land on an odd address. */
static struct {
    s16 slamTime;       /* game frames since Z started the pound */
    s8 transformDelay;  /* game frames left before the change of form */
    u8 pounding;        /* 1 from Z in the air to the landing */
    u8 slamShown;       /* 1 from a pound to the next frame's input: the glove's mode shows a slam landing */
    f32 lift;           /* the rotor potion's lift, as handControl.rotorSpeed is Glover's */
    u8 thrown;          /* 1 while BallworldThrownBegin holds handControl.releaseTimer */
    u8 healthHeld;      /* 1 while Mend holds player.health at BALL_HEALTH */
    u8 fire;            /* 1 from Z on the pachinko's tray to the end of the frame */
    u8 riding;          /* 1 from Z on the pachinko's low tray to the shot: the ball rides the tray */
    u8 savedRelease;    /* handControl.releaseTimer before that */
} bw;

static void StartPound(void);
static void PachinkoInput(void);
static void PachinkoFrame(void);

/* 1 in the levels that are played: not in the menus, the title's fly-through, the present screen or the intro
 * and outro. Two levels keep their own set-up, in which the glove is only an anchor and is not seen: the Carnival
 * bonus (a cannon) and the Space boss's last part (a turret). */
static s32 InPlay(void) {
    return ballworldOn && GameBeingPlayed() && gameInfo.level < PRESENT && gameInfo.level != CARNIVALBONUS
           && gameInfo.level != SPACEBOSS3;
}

/* 1 while the ball is under the player's control: it is in play, the hidden glove rides it, and no death or
 * restart is running. The glove rides the ball in three modes: joined, and the two of a jump (bounce, and back to
 * the cup). After a pound it shows the slam landing (HAND_SLAM2ST) until the next frame's input. */
static s32 BallControlled(void) {
    s16 mode = hand.state->mode;

    return InPlay() && controlMethod == CONTROL_BALL
           && (mode == HAND_JOINED || mode == HAND_BOUNCE || mode == HAND_BOUNCE2CUP
               || (mode == HAND_SLAM2ST && bw.slamShown))
           && !handControl.deathTimer && !ballControl.loseTimer && ballControl.state != BALL_BURST;
}

/* R changes the ball into its next form, in the order of the game's table (nextBallState), as Glover's spin does.
 * The limits are those of MaintainAction's HAND_JOINED case: no change as the enemy ball (cheat bit 0x80), with
 * the lava ball's cancer, or as a large snowball. */
static void Transform(void) {
    Pad *p = &pad[ActiveController];

    /* Glover's spin starts only from the joined mode (MaintainAction, HAND_JOINED). A change of form that R has
     * started waits through a jump or a pause: its countdown stops until the ball is joined again. */
    if (!BallControlled() || hand.state->mode != HAND_JOINED || gamePaused || levelExitTimer) {
        return;
    }
    if (ballControl.transformState > 0) {
        if (--bw.transformDelay <= 0) {
            HandTransformBall();
        }
    } else if (ballControl.transformState < 0) {
        if (ballControl.transformState >= TRANSFORM_REPEAT && PRESSED(BUTTON_SHOULDER_RIGHT)) {
            HandTransformBall();
        }
    } else if (p->button[BUTTON_SHOULDER_RIGHT] && !(cheatActivated & 0x80) && !ballControl.cancerTimer
               && ballControl.snow <= 0.9) {
        ballControl.transformState = 30;
        bw.transformDelay = TRANSFORM_DELAY;
        PlaySpellSample();
    }
}

void BallworldInputFrame(void) {
    if (!ballworldOn || !TICK) {
        return;
    }
    /* The slam landing has been seen by this frame's puzzles; the glove goes back to riding the ball before
     * MoveHand runs. */
    if (bw.slamShown) {
        bw.slamShown = 0;
        if (hand.state->mode == HAND_SLAM2ST) {
            hand.state->mode = HAND_JOINED;
        }
    }
    Transform();
    PachinkoInput();
    StartPound();
}

/* CheckHandPlatform's slam case: a slam on a platform whose path waits for one (a first point of -1 or -2, and no
 * lever link) starts the platform. */
static void PressSlamButton(Platform *platform) {
    if (platform == NULL || platform->path == NULL || (platform->path->moving != -1 && platform->path->moving != -2)
        || platform->link != NULL) {
        return;
    }
    if (platform->path->moving == -1) {
        platform->pathStopFlags |= 2;
        platform->pathStopFlags &= ~0x800;
    }
    platform->path->moving = 1;
}

/* CreateBlastCircle (hand.c), centered on the ball: the ring of a slam. UpdateBlastCircle then stuns or kills the
 * enemies that it reaches. */
static void BlastCircle(void) {
    Plane *plane;
    s32 index;
    Footprint *footprint;
    s32 blastTime;

    if (ball.obj->planes[0] == NULL) {
        return;
    }
    FindFlattestPlane(&ball, &plane, &index);
    footprint = (Footprint *)JallocAlloc(sizeof(Footprint), 1, "Footprnt");
    footprint->parent = ball.obj->platform;
    blastTime = player.powerups[HERCULES] ? 15 : 10;
    if (SetupFootprint(footprint, &ball.pos, (Vec3 *)plane, blastTime / 2 + 1, blastTime, blastTex, 6, 0x800)) {
        footprint->pos.y -= ball.radius / 1.1;
        footprint->alpha = 150;
        footprint->maxAlpha = 0;
        footprint->width = blastTime / 10;
        footprint->alphaStep = footprint->alpha / (f32)blastTime;
        footprint->b = footprint->g = footprint->r = 255;
        AddFootprint(footprint);
        handControl.footprint = footprint;
        handControl.blastPos = footprint->pos;
        handControl.blastSize = blastTime;
        StartEarthquakeAtPoint(&ball.pos, 10, blastTime * 100, blastTime * 25, 1, 14);
    }
}

/* A slam landing: what MaintainAction's HAND_SLAM2ST case does, for the ball. The glove's mode shows the landing
 * until the next frame's input. That makes the slam conditions of the puzzles (HAND_JUST_ON with value 2) and of
 * the Fear boss (DoFortressBoss) see it. */
static void Pound(void) {
    Vec3 pos;

    pos = ball.pos;
    pos.y -= 10.0f;
    PlaySample(166, &ball.pos, 100, 128);
    CreateDustCircle(&pos, 8, 4.0f);
    BlastCircle();
    PressSlamButton(ball.obj->platform);
    hand.state->mode = HAND_SLAM2ST;
    handControl.actionTime = 1;
    hand.obj->wasOnGround = 0;
    bw.slamShown = 1;
}

/* An enemy that a falling ball hits is hit as by Glover's slam (CheckActorCollisions, HAND_SLAM). */
static void SlamEnemies(void) {
    s32 j;
    Enemy *e;

    for (j = 0; j < numberOfEnemies; j++) {
        e = &enemies[j];
        if (!(e->actor.flags & 0x4000000) || e->actor.active == 2) {
            continue;
        }
        e->brain.flags |= 8;
        if (!(e->brain.flags & 0x20)) {
            NMEHitByBlastRing(e);
        }
        if ((e->brain.flags & 1) || e->brain.spellTimer[2]) {
            KillEnemy(e);
        }
    }
}

/* Carnival 1's pachinko machine. Glover stands on the plunger (platform 100) or on one of two buttons (101, 102)
 * while the ball waits on the tray (103). A slam on the plunger raises the tray, and a second slam fires the ball
 * into the machine. The buttons slide the raised tray. The puzzles test HAND_ON and HAND_JUST_ON with value 2. The
 * ball alone cannot be in two places. While the ball sits on the tray (puzzle variable 10 is 1 or 3), Z presses the
 * plunger, and on the raised tray the stick slides it. Just before the puzzles run, the glove's platform fields show
 * the plunger or the button. MaintainAction copies the ball's platform into them again after the puzzles. The
 * rubber ball bounces off the tray, so the ball rides the tray from the first Z to the shot. */
#define PACHINKO_LOW 1
#define PACHINKO_READY 3
#define TAG_PLUNGER 100
#define TAG_BUTTON_A 101
#define TAG_BUTTON_B 102
#define TAG_TRAY 103
#define AIM_STICK 30
#define TRAY_REACH 35.0f
#define TRAY_HEIGHT 70.0f
#define TRAY_TOP 16.0f

/* The tray, while the ball rides it, or rests or bounces in it. */
static Platform *PachinkoTray(void) {
    Platform *tray;
    f32 height;

    if (gameInfo.level != CARNIVAL1 || (puzzleVal[10] != PACHINKO_LOW && puzzleVal[10] != PACHINKO_READY)) {
        return NULL;
    }
    tray = TagToFirstPlatform(TAG_TRAY);
    if (tray == NULL || bw.riding) {
        return tray;
    }
    height = ball.pos.y - tray->actor.pos.y;
    if (height > 0.0f && height < TRAY_HEIGHT
        && DistanceBetweenPoints2DSquared(&ball.pos, &tray->actor.pos) < TRAY_REACH * TRAY_REACH) {
        return tray;
    }
    return NULL;
}

/* The ball rides the tray from the first Z to the shot, while it is in play. */
static void PachinkoStopRiding(void) {
    if (gameInfo.level != CARNIVAL1 || puzzleVal[10] == 0 || !BallControlled()) {
        bw.riding = 0;
    }
}

/* In the frame's input: Z presses the plunger. The ball keeps still on the raised tray. */
static void PachinkoInput(void) {
    if (!BallControlled() || PachinkoTray() == NULL) {
        return;
    }
    if (puzzleVal[10] == PACHINKO_READY) {
        ball.vel.x = ball.vel.z = 0.0f;
    }
    if (PRESSED(BUTTON_Z)) {
        bw.fire = 1;
    }
}

/* Just before the puzzles: the platform fields that the puzzles read. Z, from the frame before, shows a slam on the
 * plunger, and the stick shows the glove on a button. */
static void PachinkoPuzzles(void) {
    Platform *tray;
    Platform *p;
    s16 x = pad[ActiveController].stickX;

    PachinkoStopRiding();
    tray = BallControlled() ? PachinkoTray() : NULL;
    if (tray == NULL) {
        bw.fire = 0;
        return;
    }
    if (bw.riding) {
        ball.obj->platform = tray;
    }
    if (bw.fire) {
        bw.fire = 0;
        p = TagToFirstPlatform(TAG_PLUNGER);
        if (p != NULL) {
            PlaySample(166, &ball.pos, 100, 128);
            bw.riding = puzzleVal[10] == PACHINKO_LOW;
            ball.obj->platform = tray;
            hand.obj->prevPlatform = tray;
            hand.obj->platform = p;
            hand.state->mode = HAND_SLAM2ST;
            handControl.actionTime = 1;
            bw.slamShown = 1;
        }
        return;
    }
    if (puzzleVal[10] == PACHINKO_READY && (x <= -AIM_STICK || x >= AIM_STICK)) {
        p = TagToFirstPlatform(x < 0 ? TAG_BUTTON_A : TAG_BUTTON_B);
        if (p != NULL) {
            hand.obj->platform = p;
        }
    }
}

/* After the ball moves, in every update: the ball rides the tray. */
static void PachinkoFrame(void) {
    Platform *tray;

    PachinkoStopRiding();
    if (!bw.riding || (tray = TagToFirstPlatform(TAG_TRAY)) == NULL) {
        return;
    }
    ball.pos.x = tray->actor.pos.x;
    ball.pos.y = tray->actor.pos.y + TRAY_TOP + ball.radius;
    ball.pos.z = tray->actor.pos.z;
    ball.vel = tray->actor.vel;
    ball.obj->platform = tray;
}

void BallworldBeforePuzzles(void) {
    if (!ballworldOn || !TICK || gamePaused) {
        return;
    }
    if (!InPlay()) {
        bw.riding = 0;
        return;
    }
    PachinkoPuzzles();
}

/* Z in the air starts a pound. It runs after ProcessInput, in the frame's input. */
static void StartPound(void) {
    if (bw.pounding || bw.fire || !BallControlled() || ball.obj->onGround || !PRESSED(BUTTON_Z)) {
        return;
    }
    bw.pounding = 1;
    bw.slamTime = 0;
    PlaySample(168, &ball.pos, 100, 128);
}

/* Runs before DoBoss, after this frame's movement and collisions. While the ball pounds it carries the glove's slam
 * flag (actor flag 0x200): CheckActorCollisions then breaks the platforms that a slam breaks. */
static void PoundFrame(void) {
    if (!BallControlled()) {
        bw.pounding = 0;
        ball.flags &= ~0x200;
        return;
    }
    if (!bw.pounding) {
        return;
    }
    if (!ball.obj->onGround) {
        ball.vel.y = (bw.slamTime < SLAM_HOLD) ? 0.0f : -SLAM_SPEED;
        ball.vel.x /= 2.0f;
        ball.vel.z /= 2.0f;
        bw.slamTime++;
        ball.flags |= 0x200;
        SlamEnemies();
        return;
    }
    SlamEnemies();
    Pound();
    bw.pounding = 0;
    ball.flags &= ~0x200;
}

/* The ball's versions of Glover's potions. The game keeps each one's time in player.powerups as before, and
 * activates the ball's own forms (bowling ball and the others) and the speed-up as it always did. */

/* An enemy can take a spell: it has an enemy record and is alive (ActivateSpell, BulletHitActor). */
static s32 SpellTarget(Enemy *e) {
    return e->actor.state != NULL && e->actor.state->index != -1 && e->actor.active != 0 && e->actor.active != 2;
}

/* Stun, death and frog are spells that Glover casts with R. The ball has R for its forms. A spell that it holds
 * goes to each enemy that it touches, as the spell bullet would give it. Strength (Hercules) makes each touch hit
 * the enemy as a slam's blast ring does. */
static void TouchEnemies(void) {
    s32 j;
    s16 spell = player.spell;
    Enemy *e;

    for (j = 0; j < numberOfEnemies; j++) {
        e = &enemies[j];
        if (!(e->actor.flags & 0x4000000) || !SpellTarget(e)) {
            continue;
        }
        if (spell >= 0 && spell <= 2 && e->brain.spellTimer[spell] == 0) {
            ActivateSpell(&e->actor, spell, spell == 1 ? 50 : spellDuration);
        }
        if (player.powerups[HERCULES] && !(e->brain.flags & 0x20) && e->brain.spellTimer[0] == 0) {
            NMEHitByBlastRing(e);
        }
    }
}

/* Rotor blades: Glover flies; the ball floats. While A is held in the air the lift grows toward a little more
 * than gravity, and without A it dies away, as Glover's rotor speed does (MaintainAction, HAND_ROTOR). A tap of A
 * on the ground still jumps. The lift is a share of gravity. */
#define LIFT_START 0.9f
#define LIFT_MAX 1.15f
#define LIFT_RISE_MAX 3.0f

static void Float(void) {
    if (!player.powerups[ROTORBLADES] || ball.obj->onGround || bw.pounding) {
        bw.lift = 0.0f;
        return;
    }
    if (TICK) {
        bw.lift *= 0.95;
        if (pad[ActiveController].button[BUTTON_A]) {
            /* The ball has no rotor to spin up: the lift starts near gravity's pull. */
            bw.lift = MIN(gravity * LIFT_MAX, MAX(gravity * LIFT_START, bw.lift + gravity * 0.1));
        }
        bw.lift = MAX(gravity * 0.25, bw.lift);
    }
    if (ball.vel.y < LIFT_RISE_MAX) {
        ball.vel.y += SCALED(bw.lift);
    }
}

/* Sticky fingers (suction): Glover walks on walls; the ball holds on to what it touches. Touching a surface, the
 * ball does not fall: it stays on slopes. It climbs a steep surface that it rolls against. */
#define CLIMB_SPEED 2.5f

static void Stick(void) {
    s32 i;
    Plane *plane;

    if (!player.powerups[SUCTION] || ball.obj->planes[0] == NULL) {
        return;
    }
    ball.vel.y += SCALED(gravity);
    for (i = 0; i < 4; i++) {
        plane = ball.obj->planes[i];
        if (plane != NULL && plane->y < 0.7f && plane->x * ball.vel.x + plane->z * ball.vel.z < 0.0f
            && pad[ActiveController].mag > 20.0f) {
            ball.vel.y = MAX(ball.vel.y, CLIMB_SPEED);
        }
    }
}

static void PotionFrame(void) {
    if (!BallControlled()) {
        bw.lift = 0.0f;
        return;
    }
    if (TICK) {
        TouchEnemies();
    }
    Float();
    Stick();
}

void BallworldBeforeBoss(void) {
    if (!ballworldOn || gamePaused) {
        return;
    }
    if (TICK) {
        PoundFrame();
    }
    PotionFrame();
    PachinkoFrame();
}

s32 BallworldSkipAction(s32 action) {
    if (!ballworldOn || !InPlay() || controlMethod != CONTROL_BALL) {
        return FALSE;
    }
    /* The rotor potion makes Glover fly (ActivateCollectable). The ball floats instead (Float), and the glove keeps
     * riding it. */
    if (action == HAND_ROTOR) {
        return TRUE;
    }
    /* A hit makes Glover let go of the ball he holds (an enemy's touch, HurtBall, a bullet, a Sucker). The glove
     * then falls back with the camera on it until Ballworld joins it again. The ball takes the hit alone. A floating
     * ball in deep water makes Glover stand on it (HAND_BALLWALK); the glove keeps riding it instead. */
    if (action == HAND_BALLWALK) {
        return TRUE;
    }
    return action == HAND_RELEASE && BallControlled();
}

s32 BallworldSkipRelease(void) {
    if (!ballworldOn || !InPlay() || controlMethod != CONTROL_BALL) {
        return FALSE;
    }
    /* MaintainAction lets go of a floating ball in deep water (ReleaseBall) before Glover stands on it. The glove
     * keeps riding the ball, with the speed and size of the joined mode, which MaintainAction sets only away from
     * deep water. */
    ball.phys->moveSpeed = BallJoinedMaxSpeed();
    hand.obj->spheres[0].targetRadius = ball.obj->spheres[0].targetRadius + 0.05;
    return TRUE;
}

/* A teleport whose flags take only Glover (flag 2, as two in Atlantis 3A) takes the ball while the glove rides it
 * too (flag 8, CheckActorForTeleport). A puzzle can change a teleport's flags. This runs every frame. */
static void OpenTeleports(void) {
    Platform *p;

    for (p = platformList.head.next; p != &platformList.head; p = p->next) {
        if (p->teleport != NULL && (p->teleport->flags & 2) && !(p->teleport->flags & 8)) {
            p->teleport->flags |= 8;
        }
    }
}

/* Glover's health is not used: a hit on him hurts the ball. His hearts are hidden, and the health that garibs give
 * (every tenth garib, and the special ones, while health is below 3) mends a bandage of the ball instead. Health
 * is kept at BALL_HEALTH for that test. */
#define BALL_HEALTH 2

static void Mend(void) {
    if (player.health > BALL_HEALTH && ballControl.numPlasters > 0) {
        SubActor(&plaster[--ballControl.numPlasters]);
        PlaySample(147, &ball.pos, 100, 128);
    }
    player.health = BALL_HEALTH;
    bw.healthHeld = 1;
    stats[HEALTH_STATS].alpha = stats[HEALTH_STATS].alphaTarget = 0;
}

/* Glover's health is his own again in a level that keeps its set-up (the Space boss's turret), and when the mode
 * is turned off (BallworldLeave). */
static void GiveHealthBack(void) {
    bw.healthHeld = 0;
    if (player.health == BALL_HEALTH) {
        player.health = 3;
    }
    stats[HEALTH_STATS].alphaTarget = 255;
}

void BallworldLeave(void) {
    if (bw.healthHeld) {
        GiveHealthBack();
    }
}

static void Unmend(void) {
    if (bw.healthHeld && GameBeingPlayed() && !InPlay()) {
        GiveHealthBack();
    }
}

/* 1 while a cameo (a scripted scene, such as a boss's entrance) runs. A cameo moves and animates the glove and waits
 * for its animations to end. MainGameLoop updates the cameos only while cameoList.count is set; a record left from
 * an earlier level keeps its old active count. */
static s32 CameoRunning(void) {
    Cameo *c;

    if (!cameoList.count) {
        return FALSE;
    }
    for (c = cameoList.head.next; c != &cameoList.head; c = c->next) {
        if (c->active) {
            return TRUE;
        }
    }
    return FALSE;
}

/* A ball that the game keeps away from the glove (ballStatic: left behind in another level, stored in the hub, or
 * frozen) has no gravity and no collision (MoveBall). The player is the ball: Ballworld takes it as the retail
 * "control ball" cheat does (ActivateCheat, case 5). */
static void TakeBall(void) {
    if (!ballStatic) {
        return;
    }
    ballStatic = 0;
    ball.flags = defaultBehaviour[ballControl.type].flags;
    ball.pos = hand.pos;
}

/* What the Prehistoric Bonus level does every frame (DoPrehistoricBonus), in every level. SetControlToBall hides
 * the glove, joins it to the ball and points the camera at the ball. While handControl.noControlTimer runs (a
 * checkpoint restart, a hint sign) it only keeps the control and the camera from switching; the glove is still
 * hidden and joined. Ballworld does nothing while a death, a level exit or a cameo runs, or while the glove is in a
 * scripted state (HAND_NOTHING, as at the Space control room's joystick): these move and animate the glove (the
 * end post's sequence lets the ball go and waits for the glove's animation to end). The glove stays hidden. */
void BallworldEndFrame(void) {
    if (!ballworldOn || !TICK || gamePaused) {
        return;
    }
    Unmend();
    if (!InPlay()) {
        return;
    }
    /* Glover and his hearts are not seen, also while Ballworld waits. A level and its cameos start with him shown. */
    hand.alpha = 0;
    stats[HEALTH_STATS].alpha = stats[HEALTH_STATS].alphaTarget = 0;
    if (handControl.deathTimer || levelExitTimer || worldExitTimer || newLevExitTimer || CameoRunning()
        || hand.state->mode == HAND_NOTHING) {
        return;
    }
    OpenTeleports();
    Mend();
    TakeBall();
    /* A floating ball in deep water makes Glover stand on it (MaintainAction); Ballworld skips that, and brings the
     * glove back from any other way into it. */
    if (hand.state->mode == HAND_BALLWALK && controlMethod == CONTROL_BALL && !ballControl.loseTimer
        && ballControl.state != BALL_BURST) {
        ApplyAction(HAND_JOINED);
    }
    SetControlToBall();
}

void BallworldHandHurt(void) {
    if (!ballworldOn) {
        return;
    }
    if (InPlay() && controlMethod == CONTROL_BALL && !handControl.deathTimer && !player.hurtTimer) {
        HurtBall();
    }
}

/* A boss that takes only a thrown ball tests "ball.link == NULL || handControl.releaseTimer", and IsHandHoldingBall
 * is false while releaseTimer is set. The ball alone is never thrown. While such a boss's code runs, the ball
 * counts as thrown: releaseTimer is 1, and its old value comes back afterward. MaintainAction, which counts
 * releaseTimer down and lets the ball go at 0, never sees the 1. */
void BallworldThrownBegin(void) {
    if (!ballworldOn) {
        return;
    }
    if (!BallControlled() || bw.thrown) {
        return;
    }
    bw.thrown = 1;
    bw.savedRelease = handControl.releaseTimer;
    handControl.releaseTimer = 1;
}

void BallworldThrownEnd(void) {
    if (!bw.thrown) {
        return;
    }
    bw.thrown = 0;
    handControl.releaseTimer = bw.savedRelease;
}

/* The Carnival boss's weapons fall when the ball, not held, hits its platforms (DoCarnivalBoss). The Prehistoric
 * boss's arena makes the glove let go of a held ball (DoPrehistBoss). */
void BallworldBossBegin(void) {
    if (!ballworldOn) {
        return;
    }
    if (gameInfo.level == CARNIVALBOSS || gameInfo.level == PREHISTORICBOSS) {
        BallworldThrownBegin();
    }
}

void BallworldBossEnd(void) {
    BallworldThrownEnd();
}

#ifdef PORT
PORT_C_END
#endif
