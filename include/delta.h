#ifndef DELTA_H
#define DELTA_H

/* The port's variable frame rate. docs/delta-time.md explains the design and the rules for using this API.

   On the N64 the game's logic runs once per game frame: 20 times a second in the levels (desiredFrameRate 3) and
   30 times a second in the menus (desiredFrameRate 2). Every speed, acceleration, damping factor and timer is
   tuned per game frame. The port runs the logic once per update, and the platform layer runs one update per drawn
   frame. Speeds stay in units per original frame. Only the steps that apply them change.

   Every helper here gives the original value when deltaTime is 1 and deltaTick is 1. A run with updates of one
   original frame is therefore the same, bit for bit, as the N64. */

#include "ultra64.h"
/* port_pow, port_log1p and port_expm1: the same bits on every build (docs/port64.md, "Libm"). */
#include "portmath.h"

/* ---- The update. The frame driver (src/port/frame.c) sets these before each update. */

/* The length of this update in original frames of the current mode, 0 < deltaTime <= 1. The platform layer splits
   a longer gap into several updates. The first update of a new mode or of a restarted frameCount is always 1. */
extern f32 deltaTime;
/* 1 in an update in which an original frame ends: its discrete logic (counters, decisions, random events, sounds)
   runs then. The controller is read, and its presses act, in the first update of a frame. Over time it is 1 exactly as often as the N64 ran a frame. */
extern s32 deltaTick;
/* The time since the latest frame boundary, at the end of this update, in original frames: 0 <= deltaPhase < 1.
   It is 0 in every update when deltaTime is 1. */
extern f32 deltaPhase;
/* 1 in the first update of an original frame: the previous update was a tick, or a new mode or a restarted
   frameCount begins a frame in this update. When both deltaNewFrame and deltaTick are 1, the update is a whole
   original frame. */
extern s32 deltaNewFrame;
/* A number for the current original frame. All the updates of one frame share it, and the next frame has another. */
extern u32 deltaFrame;

/* ---- Per-frame factors over deltaTime frames. Each returns its argument unchanged when deltaTime is 1. */

/* What a per-frame factor k does over this update: k^deltaTime. For a quantity that is not a velocity: a scale, an
   alpha, a zoom, a squash amplitude, a growth. A velocity uses DeltaDamp. For k < 0 it returns k. */
f32 DeltaPow(f32 k);
/* DeltaPow for a factor that the game writes as a double constant (v *= 0.9 multiplies in double). */
f64 DeltaPowD(f64 k);
/* The fraction for a per-frame approach x += (t - x) * k: 1 - (1 - k)^deltaTime. Outside 0..1 it is k * deltaTime. */
f32 DeltaLerp(f32 k);
f64 DeltaLerpD(f64 k);
/* The divisor for a per-frame approach x -= (x - t) / d with d > 1: 1 / (1 - (1 - 1/d)^deltaTime). x / 3.0f and
   x * (1.0f / 3.0f) differ in the last bit, so a divisor needs this form instead of DeltaLerp(1.0f / 3.0f). */
f32 DeltaDiv(f32 d);
f64 DeltaDivD(f64 d);
/* The moving weight of a blend x = x * keep + t * take whose weights are separate constants: take when deltaTime
   is 1, else 1 - keep^deltaTime. Use it with DeltaPow(keep) as the other weight. */
f32 DeltaTake(f32 keep, f32 take);
f64 DeltaTakeD(f64 keep, f64 take);

/* ---- Motion. */

/* A per-frame amount scaled to this update: a velocity added to a position, an acceleration added to a velocity, an
   angle step. Multiplying by 1.0f changes no value. */
#define DT(x) ((x) * deltaTime)
/* The factor for damping a velocity by k per frame (v *= k), where the velocity moves a position by v * deltaTime:
   1 - (1 - k) * deltaTime for 0 <= k <= 1, DeltaPow(k) outside. A velocity that per-frame thrust holds steady then
   keeps the N64's speed, a speed capped every frame moves at the cap, and a coasting velocity covers the N64's
   total distance. */
f32 DeltaDamp(f32 k);
f64 DeltaDampD(f64 k);
/* The position correction for a velocity change dv that a sustained per-frame acceleration (gravity) made after
   this update's position step: x += DeltaAccelPos(dv). It is dv * (deltaTime - 1) / 2, and 0 when deltaTime is 1.
   With it, free flight under gravity passes through the N64's positions at whole frames. For an acceleration made
   before the position step, subtract it instead. Do not add it when deltaTime is 1: adding +0.0f turns -0.0f into
   +0.0f. */
f32 DeltaAccelPos(f32 dv);
/* A change measured over this update as a rate per original frame: x / deltaTime. For speeds taken from
   pos - prevPos when prevPos is written every update. */
f32 DeltaPerFrame(f32 x);

/* ---- Counters that step on ticks, read as continuous time. */

/* The offset that turns a tick counter into continuous time, for a read before this update's tick step: deltaPhase
   in a tick update, deltaPhase - 1 in the others. A read after the step adds deltaPhase instead. Both are 0 when
   deltaTime is 1. */
f32 DeltaLag(void);
/* frameCount as continuous time, for code that runs before GameStep steps frameCount: the draw path, MainGameLoop
   and all it calls, and the menus. It does not move while frameCount stands still (pause, modes 2 and 15). */
f32 FrameTime(void);
/* FrameTime() - frameCount. For SineWave, whose time argument is an s32:
   SineWave(f, phase + FrameLag(), frameCount). */
f32 FrameLag(void);
/* frameCount as continuous time, for code that runs after GameStep has stepped it (CheckForExits and its callees,
   GameOverScreen). */
f32 FrameTimeAfter(void);
/* frameCount2 as continuous time, for code that runs before GameStep steps frameCount2. */
f32 FrameTime2(void);
/* The value to draw for a quantity that changes only on ticks: cur in a tick update, else the blend from prev (its
   value before the latest tick's step) to cur by deltaPhase. Exact (cur) when deltaTime is 1. */
f32 DeltaTickLerp(f32 prev, f32 cur);

/* ---- Events. */

/* TRUE the first time it is called with this stamp in an original frame, then FALSE until the next frame. For
   one-shot code that must run once per frame in whichever update reaches it first (if (frameCount == 0 &&
   DeltaFirst(&stamp))). When every update is a whole frame it is always TRUE. A stamp is a u32 outside port_ram,
   set to 0 at start-up. */
s32 DeltaFirst(u32 *stamp);

/* ---- Whole frames. */

/* Runs the code between DeltaEnterFixed and DeltaLeaveFixed as one whole original frame: deltaTime 1, deltaTick 1,
   deltaPhase 0, deltaNewFrame 1 and a new deltaFrame. For code that simulates frames outside the update (the
   camera warm-up in RestartPlayer, the head-start loops of the level loader). Enter and leave once per simulated
   frame. Nothing changes when the update is already a whole frame at deltaTime 1. */
typedef struct DeltaSaved {
    f32 time;
    s32 tick;
    f32 phase;
    s32 newFrame;
    u32 frame;
} DeltaSaved;
void DeltaEnterFixed(DeltaSaved *saved);
void DeltaLeaveFixed(DeltaSaved *saved);

/* ---- A helper that another file implements (docs/delta-time.md, "Animation"). */

struct AnimState;
/* anim.c: the animation time to draw: anim->time moved on by anim->speed * deltaPhase and kept inside the current
   animation. anim->time itself steps only on ticks. Exactly anim->time when deltaPhase is 0. */
f32 AnimDrawTime(struct AnimState *anim);

/* ---- The frame driver (src/port/frame.c). */

/* Starts an update of dt original frames: sets deltaTime, deltaTick, deltaPhase, deltaNewFrame and deltaFrame. An
   original frame ends when the time since the latest boundary reaches one frame. A change of game mode, or a
   frameCount lower than at the start of the previous update, ends the frame at once and restarts the phase. The
   update after it is a whole original frame, whatever dt is. */
void DeltaBeginUpdate(f32 dt);
/* Clears the button presses (pad[].pressed) in an update that is not the first of its frame window. The frame driver
   reads the controller in the first update, and the presses are visible there only. */
void DeltaHoldInput(void);
/* Keeps the presses that the first update of a window left (code may clear some) for the tick. Does nothing in
   any other update. */
void DeltaKeepInput(void);
/* 1 when the button went down at the window's read of the controller, for code that runs only in the tick. In the
   first update it is pad[].pressed itself. Later in the window it is the copy that DeltaKeepInput made. Use it
   where a test of pad[].pressed sits under deltaTick or in code that only the tick calls. */
s32 DeltaTickPressed(s32 padNum, s32 button);
/* Clears the press for the rest of the window, in pad[].pressed and in the tick's copy. */
void DeltaTakePress(s32 padNum, s32 button);
/* Ends the update: records the tick and whether frameCount and frameCount2 stepped. */
void DeltaEndUpdate(void);

#endif
