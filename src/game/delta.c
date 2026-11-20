/* The port's frame timing and the helpers for the frame-rate rewrite (include/delta.h, docs/delta-time.md). None
   of this state is in port_ram: the game's memory keeps the N64's layout. */
PORT_C_BEGIN
#include "common.h"
#include "delta.h"
#include "game.h"
#include "controller.h"

extern s32 frameCount;
extern s32 frameCount2;
extern u8 desiredFrameRate;
extern u8 newDesiredFrameRate;

f32 deltaTime = 1.0f;
s32 deltaTick = 1;
f32 deltaPhase = 0.0f;
s32 deltaNewFrame = 1;
u32 deltaFrame = 1;

/* The time since the latest frame boundary, in original frames of the current mode. */
static f64 phase;
/* The source of frame numbers. DeltaEnterFixed takes numbers from it too. */
static u32 frameNumbers = 1;
static s32 lastTick = 1;
/* The game mode and frameCount at the start of the previous update. -1 makes the first update a tick. */
static s32 lastMode = -1;
static s32 lastFrameCount;
/* frameCount and frameCount2 at the end of the latest tick, and whether that tick stepped each by one. */
static s32 tickCount;
static s32 tickCount2;
static s32 countMoving;
static s32 count2Moving;

f32 DeltaPow(f32 k) {
    if (deltaTime == 1.0f || k < 0.0f) {
        return k;
    }
    return (f32)port_pow(k, deltaTime);
}

f64 DeltaPowD(f64 k) {
    if (deltaTime == 1.0f || k < 0.0) {
        return k;
    }
    return port_pow(k, deltaTime);
}

f32 DeltaLerp(f32 k) {
    return DeltaLerpD(k);
}

f64 DeltaLerpD(f64 k) {
    if (deltaTime == 1.0f) {
        return k;
    }
    if (k < 0.0 || k > 1.0) {
        return k * deltaTime;
    }
    if (k == 1.0) {
        return 1.0;
    }
    /* 1 - (1 - k)^dt, without the loss of precision of 1 - pow() for small k. */
    return -port_expm1(deltaTime * port_log1p(-k));
}

f32 DeltaDiv(f32 d) {
    return DeltaDivD(d);
}

f64 DeltaDivD(f64 d) {
    if (deltaTime == 1.0f) {
        return d;
    }
    if (d <= 1.0) {
        return d / deltaTime;
    }
    return 1.0 / -port_expm1(deltaTime * port_log1p(-1.0 / d));
}

f32 DeltaTake(f32 keep, f32 take) {
    if (deltaTime == 1.0f) {
        return take;
    }
    return 1.0f - DeltaPow(keep);
}

f64 DeltaTakeD(f64 keep, f64 take) {
    if (deltaTime == 1.0f) {
        return take;
    }
    return 1.0 - DeltaPowD(keep);
}

f32 DeltaDamp(f32 k) {
    return DeltaDampD(k);
}

f64 DeltaDampD(f64 k) {
    if (deltaTime == 1.0f) {
        return k;
    }
    if (k < 0.0 || k > 1.0) {
        return DeltaPowD(k);
    }
    return 1.0 - (1.0 - k) * deltaTime;
}

f32 DeltaAccelPos(f32 dv) {
    if (deltaTime == 1.0f) {
        return 0.0f;
    }
    return dv * (deltaTime - 1.0f) * 0.5f;
}

f32 DeltaPerFrame(f32 x) {
    return x / deltaTime;
}

f32 DeltaLag(void) {
    return deltaTick ? deltaPhase : deltaPhase - 1.0f;
}

f32 FrameLag(void) {
    if (countMoving && frameCount == tickCount) {
        return DeltaLag();
    }
    return 0.0f;
}

f32 FrameTime(void) {
    return (f32)frameCount + FrameLag();
}

f32 FrameTimeAfter(void) {
    if (deltaTick ? frameCount == tickCount + 1 : countMoving && frameCount == tickCount) {
        return (f32)frameCount + deltaPhase;
    }
    return (f32)frameCount;
}

f32 FrameTime2(void) {
    if (count2Moving && frameCount2 == tickCount2) {
        return (f32)frameCount2 + DeltaLag();
    }
    return (f32)frameCount2;
}

f32 DeltaTickLerp(f32 prev, f32 cur) {
    if (deltaTick) {
        return cur;
    }
    return prev + (cur - prev) * deltaPhase;
}

s32 DeltaFirst(u32 *stamp) {
    if (*stamp == deltaFrame) {
        return FALSE;
    }
    *stamp = deltaFrame;
    return TRUE;
}

void DeltaEnterFixed(DeltaSaved *saved) {
    saved->time = deltaTime;
    saved->tick = deltaTick;
    saved->phase = deltaPhase;
    saved->newFrame = deltaNewFrame;
    saved->frame = deltaFrame;
    deltaTime = 1.0f;
    deltaTick = 1;
    deltaPhase = 0.0f;
    deltaNewFrame = 1;
    deltaFrame = ++frameNumbers;
}

void DeltaLeaveFixed(DeltaSaved *saved) {
    deltaTime = saved->time;
    deltaTick = saved->tick;
    deltaPhase = saved->phase;
    deltaNewFrame = saved->newFrame;
    deltaFrame = saved->frame;
}

void DeltaBeginUpdate(f32 dt) {
    s32 restart = gameInfo.mode != lastMode || frameCount < lastFrameCount;

    lastMode = gameInfo.mode;
    lastFrameCount = frameCount;
    deltaNewFrame = lastTick || restart;
    if (deltaNewFrame) {
        deltaFrame = ++frameNumbers;
    }
    if (restart) {
        /* A new mode or a restarted frameCount begins at a frame boundary. Its first update is a whole original
           frame: its set-up runs, and its motion gets a whole frame, as at deltaTime 1. Otherwise all its motion
           would lag its discrete logic and the input by the part of a frame that this update is short. */
        phase = 0.0;
        deltaTime = 1.0f;
        deltaTick = 1;
    } else {
        deltaTime = dt;
        phase += dt;
        if (phase >= 1.0 - 1e-6) {
            phase = phase - 1.0 > 0.0 ? phase - 1.0 : 0.0;
            deltaTick = 1;
        } else {
            deltaTick = 0;
        }
    }
    /* The frame driver applies a new frame rate at a tick. The time left over is measured in the old mode's
       frames; it becomes the same time in the new mode's frames. */
    if (deltaTick && newDesiredFrameRate != 0 && desiredFrameRate != 0 && newDesiredFrameRate != desiredFrameRate) {
        phase = phase * desiredFrameRate / newDesiredFrameRate;
    }
    deltaPhase = phase;
}

/* The window's presses (pad[].pressed) that are left at the end of its first update, for code that runs only in the
   tick. */
static u8 tickPresses[4][sizeof(pad[0].pressed)];

void DeltaHoldInput(void) {
    s32 i;
    s32 j;

    if (deltaNewFrame) {
        return;
    }
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 14; j++) {
            pad[i].pressed[j] = 0;
        }
    }
}

void DeltaKeepInput(void) {
    s32 i;

    if (!deltaNewFrame || deltaTick) {
        return;
    }
    for (i = 0; i < 4; i++) {
        memcpy(tickPresses[i], pad[i].pressed, sizeof(tickPresses[i]));
    }
}

s32 DeltaTickPressed(s32 padNum, s32 button) {
    if (deltaNewFrame) {
        return pad[padNum].pressed[button];
    }
    return tickPresses[padNum][button];
}

void DeltaTakePress(s32 padNum, s32 button) {
    pad[padNum].pressed[button] = 0;
    tickPresses[padNum][button] = 0;
}

void DeltaEndUpdate(void) {
    if (deltaTick) {
        countMoving = frameCount == tickCount + 1;
        count2Moving = frameCount2 == tickCount2 + 1;
        tickCount = frameCount;
        tickCount2 = frameCount2;
    }
    lastTick = deltaTick;
}

PORT_C_END
