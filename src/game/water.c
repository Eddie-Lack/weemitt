PORT_C_BEGIN
#include "common.h"
#include "ident.h"
#include "water.h"
#include "jalloc.h"
#include "footprint.h"
#include "camera.h"
#include "maths.h"
#include "hand.h"
#include "platform.h"
#include "macros.h"
#include "debris.h"
#include "weather.h"
#include "ball.h"
#include "light.h"
#include "overlay.h"
#include "sound.h"
#include "game.h"
#include "object.h"
#include "delta.h"
#include "porthooks.h"

extern PTR32(Texture) rippleTex;
extern PTR32(Texture) wakeTex;
extern f32 DETAIL_RANGE;
extern s32 frameCount;
extern PTR32(Actor) foregroundObjects[];
extern s32 numForegroundObjects;
extern Quat vertQ;
extern Debris debris[];
extern s32 sparkleDeb;
extern f32 sparkleDist;
extern Vec3 zero;
extern f32 gravity;
extern f32 waterBouy;
s32 CheckActorForWater(Actor *aPtr, Water *ptr, s32 wake);
void WaterDoFloating(Water *ptr, Actor *aPtr, s32 doCurrent);
void WaterDoSplashes(f32 yPos, Actor *aPtr, Vec3 *plane, s16 ripple);
void CreateWaterRipple(f32 xPos, f32 yPos, f32 zPos, f32 rippleSize, s32 numRings, Vec3 *plane2, Texture *texture, s16 size);
void CreateWake(Vec3 *pos, Vec3 *vel, Platform *platform, f32 level, f32 size, Vec3 *normal);

extern s32 cameraInWater;
extern u16 watFogRange;
extern Fog originalFog;
extern u8 hubBackdropColour[][4];
extern s32 cheatActivated;
extern s16 fieldOfViewAim;
extern s32 cameraWaterFrame;
extern u8 originalAmbientLight[3];
void MusHandleSetReverb(s32 handle, s32 reverb);
void MusHandleSetVolume(s32 handle, s32 volume);

void InitWaterList(void) {
    waterList.count = 0;
    waterList.head.next = waterList.head.prev = &waterList.head;
}

void AddWater(Water *water) {
    if (water->next == NULL) {
        waterList.count++;
        water->prev = &waterList.head;
        water->next = waterList.head.next;
        waterList.head.next->prev = water;
        waterList.head.next = water;
    }
}

#define NUM_EVENT_STAMPS 16
#define WATER_ENTRY 0
#define WATER_RIPPLE 1

static struct {
    Actor *actor;
    u32 stamp;
} eventStamps[2][NUM_EVENT_STAMPS];

/* TRUE the first time an actor causes this water event in an original frame (entering the water, or sinking through
   the ripple depth). The event's damping and its sound then act once per frame, as on the N64. A floating actor that
   rests on the surface dips in and out of the water in many short updates, and damping each dip stops it before it can
   settle. When every update is a whole frame, it is always TRUE. */
static s32 FirstWaterEvent(Actor *aPtr, s32 kind) {
    static s32 next[2];
    s32 i;

    for (i = 0; i < NUM_EVENT_STAMPS; i++) {
        if (eventStamps[kind][i].actor == aPtr) {
            return DeltaFirst(&eventStamps[kind][i].stamp);
        }
    }
    eventStamps[kind][next[kind]].actor = aPtr;
    eventStamps[kind][next[kind]].stamp = deltaFrame;
    next[kind] = (next[kind] + 1) % NUM_EVENT_STAMPS;
    return TRUE;
}

void WaterDoFloating(Water *ptr, Actor *aPtr, s32 doCurrent) {
    Vec3 tempVect;
    Plane *plane;
    s32 index;
    f32 actorHeight;
    f32 actorOldHeight;
    f32 lift;
    Actor *b;
    f32 r;

    if (aPtr->phys != NULL) {
        r = aPtr->radius / aPtr->phys->bottomDiv;
        actorHeight = aPtr->pos.y - r;
        actorOldHeight = aPtr->prevPos.y - r;
    } else {
        actorHeight = aPtr->pos.y;
        actorOldHeight = aPtr->prevPos.y;
    }
    if (actorHeight < ptr->height) {
        if (aPtr->shadow != NULL && aPtr->shadow->altitude == -1.0f) {
            return;
        }
        if (aPtr->state != NULL) {
            /* The N64 decides once per frame whether an actor is in the water, from where it stands at the start of
               the frame. The state holds for the whole frame window. */
            if (deltaNewFrame) {
                if (aPtr->flags & 0x200000) {
                    aPtr->state->waterState = INWATER_FLOATING;
                } else if (ptr->height < aPtr->pos.y) {
                    aPtr->state->waterState = INWATER_PADDLING;
                } else if (aPtr->pos.y + aPtr->radius / 2.0f < ptr->height) {
                    aPtr->state->waterState = INWATER_SWIMMING;
                } else {
                    aPtr->state->waterState = INWATER_WADING;
                }
            }
            if (aPtr->state != NULL && aPtr->state->waterState == INWATER_SWIMMING && aPtr == &hand && deltaTick
                && frameCount % 10 == 0 && Random(10) < 5) {
                tempVect.x = 0.0f;
                tempVect.z = 0.0f;
                tempVect.y = 1.0f;
                AddDebris(BUBBLE, &aPtr->pos, &tempVect, NULL, NULL);
            }
        }
        /* The current, the push along the floor and the lift are accelerations per original frame. */
        if (doCurrent) {
            aPtr->vel.x += ptr->currentX * deltaTime;
            aPtr->vel.y += ptr->currentY * deltaTime;
            aPtr->vel.z += ptr->currentZ * deltaTime;
        }
        /* The water's drag acts once per original frame, in the first update of the frame window. On the N64 it
           comes before the frame's position step. */
        if (!(aPtr->flags & 0x200000)) {
            if (deltaNewFrame) {
                if (aPtr->pos.y < ptr->height) {
                    SCALE_VECTOR_2D(&aPtr->vel, 0.8);
                }
                aPtr->vel.y *= 0.85;
                if ((aPtr->flags & 0x4000) && aPtr->obj->planes[0] == NULL) {
                    aPtr->spinSpeed *= 0.9;
                }
            }
        } else {
            if (aPtr->obj != NULL) {
                if (aPtr->obj->onGround == 0 || aPtr->obj->planes[0] == NULL) {
                    if (actorOldHeight > ptr->height - 2.0f && ptr->height - 2.0f >= actorHeight
                        && FirstWaterEvent(aPtr, WATER_RIPPLE)) {
                        CreateWaterRipple(aPtr->pos.x, ptr->height, aPtr->pos.z, (s32)(aPtr->radius / 8.0f + 1.0f), 2,
                                          NULL, rippleTex, 0x400);
                        if (aPtr == &ball) {
                            PlaySample(3, &aPtr->pos, 40, 128);
                        }
                    }
                } else if (aPtr->vel.y < 0.0f && aPtr->pos.y < ptr->height) {
                    FindFlattestPlane(aPtr, &plane, &index);
                    AddToOneScaledVector(&aPtr->vel, (Vec3 *)plane, -gravity / 2.0f * deltaTime);
                }
            }
            if (ptr->height <= actorOldHeight && FirstWaterEvent(aPtr, WATER_ENTRY)) {
                if (aPtr->flags & 0x80) {
                    aPtr->lifeTimer = 1;
                    return;
                }
                if (aPtr->state != NULL) {
                    aPtr->state->enteredWater = 1;
                }
                if (aPtr == &ball) {
                    if (hand.state->mode == HAND_BOUNCE) {
                        if (ball.pos.y + ball.radius / 2.0f < ptr->height) {
                            SCALE_VECTOR(&ball.vel, 0.4);
                        } else {
                            SCALE_VECTOR(&ball.vel, 0.8);
                        }
                        return;
                    }
                    if (hand.state->mode != HAND_BALLWALK && hand.state->mode != HAND_JOINED && -ball.vel.y * 1.3 < ball.speed
                        && LENGTH_2D(&ball.vel) > 5.0f) {
                        CreateWaterRipple(ball.pos.x, ptr->height, ball.pos.z, 2.0f, 1, NULL, rippleTex, 0x400);
                        SCALE_VECTOR_2D(&ball.vel, 0.8);
                        ball.vel.y = -ball.vel.y * 0.6;
                        return;
                    }
                }
                aPtr->vel.x *= 0.7;
                aPtr->vel.y *= 0.5;
                aPtr->vel.z *= 0.7;
                return;
            }
            if (deltaNewFrame) {
                SCALE_VECTOR_2D(&aPtr->vel, 0.9);
            }
            lift = (ptr->height - actorHeight) / 5.0f + gravity / 3.0f;
            aPtr->vel.y += MIN(lift, waterBouy) * deltaTime;
        }
    } else if ((aPtr->flags & 0x200000) && actorOldHeight < ptr->height) {
        if (aPtr->state != NULL && aPtr->state->enteredWater) {
            if (aPtr == &ball || aPtr->type == BULLET) {
                b = &ball; // comparing through a copy keeps the second ball test
                if (aPtr == b && hand.state->mode == HAND_BOUNCE) {
                    ApplyAction(HAND_BOUNCE2CUP);
                } else if (aPtr->state->waterState && (aPtr->type == BULLET || hand.state->mode != HAND_BOUNCE)) {
                    aPtr->state->enteredWater = 0;
                    aPtr->vel.y *= 0.8;
                }
            } else {
                aPtr->state->enteredWater = 0;
                aPtr->vel.y *= 0.6;
            }
            if (aPtr->vel.y > 3.0f) {
                aPtr->vel.y = 3.0f;
            }
        }
    }
}

void DoForceRipple(void) {
}

void WaterDoSplashes(f32 yPos, Actor *aPtr, Vec3 *plane, s16 ripple) {
    Vec3 vect;
    f32 rippleSize;
    f32 rippleVol;
    f32 mass;

    if (aPtr->shadow != NULL && aPtr->shadow->altitude == -1.0f) {
        return;
    }
    vect.x = aPtr->pos.x;
    vect.z = aPtr->pos.z;
    vect.y = yPos;
    if (aPtr->type == DEBRIS) {
        if (!ripple) {
            return;
        }
        rippleSize = ABS(aPtr->prevVel.y / 2.0f);
        CreateWaterRipple(vect.x, vect.y, vect.z, MIN(MAX(rippleSize, 1.0f), 5.0f), 1, plane, rippleTex, 0x400);
    } else {
        if (!ripple) {
            return;
        }
        mass = aPtr->phys->mass > 10.0f ? 10.0f : aPtr->phys->mass;
        rippleSize = aPtr->prevVel.y * mass / 15.0f;
        if (!(rippleSize > 0.0f)) {
            rippleSize = -rippleSize;
        }
        rippleSize = MIN(MAX(rippleSize, 1.0f), 5.0f);
        rippleVol = rippleSize * 20.0f;
        CreateWaterRipple(vect.x, vect.y, vect.z, rippleSize, 1, plane, rippleTex, 0x400);
        if (aPtr->type != BULLET && rippleVol >= 40.0f) {
            PlaySample(aPtr->vel.y > 0.0f ? 3 : 2, &aPtr->pos, rippleVol, 128);
        }
    }
    if (ripple) {
        if (rippleSize >= 3.0f) {
            CreateSplashCircle(&vect, MIN(rippleSize + 0.0f, 5), rippleSize);
        }
    }
}

/* The draw path calls this in every update. The alpha fade and the texture coordinates are integers that step
   once per original frame, so all of it runs in ticks. */
void DeformTextures(void) {
    Actor *actor;
    Platform *platform;
    s32 j;

    if (!deltaTick) {
        return;
    }
    for (actor = actorList.head.next; actor != &actorList.head; actor = actor->next) {
        if (actor->faces && actor->faces->terrain == 12) {
            actor->alpha -= (actor->alpha - 80) / 2.5;
        }
        if (actor->inst->obj->flags & 0x20) {
            DeformTextureCoords(actor->inst->obj);
        }
    }
    for (j = 0; j < numForegroundObjects; j++) {
        actor = foregroundObjects[j];
        if (actor->inst->obj->flags & 0x20) {
            DeformTextureCoords(actor->inst->obj);
        }
    }
    for (platform = platformList.head.next; platform != &platformList.head; platform = platform->next) {
        if ((platform->flags & 0x200) && platform->pullSpeed == 0.0f &&
            (frameCount < 12 || (s32)platform->actor.inst->obj->crc < frameCount)) {
            SlideTextureCoords(&platform->actor, -platform->conveyorVel.z, -platform->conveyorVel.x);
            platform->actor.inst->obj->crc = frameCount;
        }
    }
}

void UpdateWater(void) {
    Vec3 tempVect;
    Vec3 tempVect2;
    Actor *aPtr;
    Water *ptr;
    Debris *d;
    Debris *sparkle;
    f32 waterLevel;
    s32 i;

    ProcessRain();
    CheckCameraForWater();
    if (frameCount == 0) {
        return;
    }
    for (aPtr = actorList.head.next; aPtr != &actorList.head; aPtr = aPtr->next) {
        if (deltaTick && aPtr->faces != NULL && aPtr->sparkleRate && frameCount % aPtr->sparkleRate == 0) {
            COPY_VECTOR(&tempVect, &aPtr->faces[Random(aPtr->inst->obj->mesh->numFaces)].edgeStart[Random(3)]);
            SUB_VECTORS(&tempVect2, &tempVect, &camera.pos);
            MakeUnit(&tempVect2);
            AddToOneScaledVector(&tempVect, &tempVect2, sparkleDist);
            sparkle = AddDebris(sparkleDeb, &tempVect, &zero, NULL, NULL);
            if (sparkle != NULL) {
                sparkle->sprite.anim.def.frameDelay = 0;
            }
        }
        if (aPtr->state != NULL && deltaNewFrame) {
            aPtr->state->waterState = 0;
        }
    }
    if (handControl.fakeball != NULL && deltaNewFrame) {
        handControl.fakeball->state->waterState = 0;
    }
    for (ptr = waterList.head.next; ptr != &waterList.head; ptr = ptr->next) {
        if (ptr->height != ptr->targetHeight) {
            /* The level moves a quarter of the way per original frame, at most 1 unit per frame. */
            waterLevel = (ptr->height - ptr->targetHeight) * DeltaLerp(0.25f);
            waterLevel = MIN(MAX(waterLevel, -deltaTime), deltaTime);
            ptr->height -= waterLevel;
            if (ptr->height - 25.0f < ptr->box.pos.y) {
                ptr->box.pos.y = ptr->height - 25.0f;
            }
            ptr->box.size.y = ptr->height + 25.0f - ptr->box.pos.y;
            if (ptr->actor != NULL) {
                ptr->actor->pos.y -= waterLevel;
            }
        }
        for (aPtr = actorList.head.next; aPtr != &actorList.head; aPtr = aPtr->next) {
            if (!(aPtr->flags & 0x10000) && (aPtr->flags & 0x200010)) {
                CheckActorForWater(aPtr, ptr, 1);
            }
        }
        for (i = 0; i < 120; i++) {
            if (!debris[i].active) {
                continue;
            }
            d = &debris[i];
            if (d->type == DUST || d->type == PUFF) {
                continue;
            }
            if (!IsPointWithinBox(&ptr->box, &d->pos)) {
                continue;
            }
            if (d->type == FAKEBALL) {
                if ((d->flags & 0x200080) == 0x200080 && ptr->height <= d->pos.y + d->radius
                    && d->prevPos.y + d->radius < ptr->height) {
                    d->lifeTimer = 1;
                }
                WaterDoFloating(ptr, (Actor *)d, 0);
            } else {
                if (d->type == BUBBLE && d->pos.y > ptr->height) {
                    KillDebris(d);
                }
                if (ptr->height <= d->pos.y + d->radius && d->prevPos.y + d->radius < ptr->height) {
                    if (d->type == WIND_BIT) {
                        WaterDoSplashes(ptr->height, (Actor *)d, NULL, 0);
                    } else {
                        WaterDoSplashes(ptr->height, (Actor *)d, NULL, 1);
                    }
                    KillDebris(d);
                } else if (d->pos.y - d->radius < ptr->height && ptr->height <= d->prevPos.y - d->radius) {
                    if (d->type == WIND_BIT) {
                        WaterDoSplashes(ptr->height, (Actor *)d, NULL, 0);
                    } else {
                        WaterDoSplashes(ptr->height, (Actor *)d, NULL, 1);
                    }
                    KillDebris(d);
                }
            }
        }
    }
}

void CreateWaterRipple(f32 xPos, f32 yPos, f32 zPos, f32 rippleSize, s32 numRings, Vec3 *plane2, Texture *texture, s16 size) {
    CollFace plane;
    Vec3 vect;
    Footprint *footprint2;
    s32 j;

    vect.x = xPos;
    vect.y = yPos;
    vect.z = zPos;
    for (j = 1; j <= numRings; j++) {
        footprint2 = (Footprint *)JallocAlloc(sizeof(Footprint), 1, "Footprint");
        footprint2->parent = NULL;
        if (!plane2) {
            plane.plane.x = plane.plane.z = 0.0f;
            plane.plane.y = 1.0f;
            SetupFootprint(footprint2, &vect, (Vec3 *)&plane, j * rippleSize / 2.0f, 80.0f, texture, 0x26, size);
        } else {
            SetupFootprint(footprint2, &vect, plane2, j * rippleSize / 2.0f, 80.0f, texture, 6, size);
        }
        footprint2->maxAlpha = 0;
        footprint2->width = 0.0f;
        if (texture == rippleTex) {
            footprint2->alpha = 200;
        }
        footprint2->alphaStep = 10;
        AddFootprint(footprint2);
    }
}

Water *FindTaggedWater(s32 tagNum) {
    Water *water;

    for (water = waterList.head.next; water != &waterList.head; water = water->next) {
        if (water->tag == tagNum) {
            return water;
        }
    }
    return NULL;
}

// a macro: each use emits its own rodata constants, as the ROM has
#define APPROACH(cur, target, diff) \
    diff = cur - (target);           \
    if (ABS(diff) > 1.0f) {          \
        diff /= 4.0f;                \
        cur -= diff;                 \
    }

void CheckCameraForWater(void) {
    Water *ptr;
    GameLight *light;
    s32 lastInWater;
    f32 dist;
    u8 rAim;
    u8 gAim;
    u8 bAim;
    u16 rangeAim;
    u8 aAim;
    Vec3 unused; // reserves stack space

    lastInWater = cameraInWater;
    cameraInWater = 0;
    for (ptr = waterList.head.next; ptr != &waterList.head; ptr = ptr->next) {
        if (IsPointWithinBox(&ptr->box, &camera.pos) && camera.pos.y - 0.0f < ptr->height) {
            cameraInWater = 1;
        }
    }
    if (cameraInWater) {
        rAim = (gameInfo.level == PIRATESBONUS) ? 21 : 43;
        gAim = (gameInfo.level == PIRATESBONUS) ? 104 : 208;
        bAim = (gameInfo.level == PIRATESBONUS) ? 76 : 153;
        rangeAim = watFogRange;
        aAim = 0;
    } else {
        rAim = originalFog.r;
        gAim = originalFog.g;
        bAim = originalFog.b;
        rangeAim = originalFog.min;
        aAim = (gameInfo.level == HUB7) ? hubBackdropColour[6][3] : 255;
    }
    /* The fog, the backdrop's alpha, the ambient light and the music settings are integers and messages: they step
       once per original frame. The camera's roll and the lights' targets move in every update. */
    if (deltaTick) {
        APPROACH(fog.r, rAim, dist);
        APPROACH(fog.g, gAim, dist);
        APPROACH(fog.b, bAim, dist);
        APPROACH(fog.min, (s16)rangeAim, dist);
        if (backdropList.count) {
            APPROACH(backdropList.head.next->alpha, aAim, dist);
        }
    }
    if (cameraInWater) {
        if (deltaTick) {
            MusHandleSetReverb(audioCtrl.handle, 60);
            MusHandleSetVolume(audioCtrl.handle, 55);
        }
        if (!(cheatActivated & 0x400)) {
            fieldOfViewAim = 70;
        }
        rAim = 33;
        if (deltaTick && hand.state->waterState == INWATER_SWIMMING) {
            switch (Random(100)) {
                case 1:
                    PlaySample(277, &camera.pos, 60, 128);
                    break;
                case 2:
                    PlaySample(278, &camera.pos, 60, 128);
                    break;
                case 3:
                    PlaySample(279, &camera.pos, 60, 128);
                    break;
                case 4:
                    PlaySample(232, &camera.pos, 60, 128);
                    break;
                case 5:
                    PlaySample(233, &camera.pos, 60, 128);
                    break;
            }
            rAim = 33;
        }
        gAim = 178;
        bAim = 123;
        if (!lastInWater) {
            cameraWaterFrame = -frameCount;
        }
        RotateVector2DXYZ(&cameraUpVect, &cameraUpVectAim, SineWave(20.0f, FrameTime(), cameraWaterFrame) / 10.0f, 2);
        /* target[1] turns by 0.1 per original frame, and target[0] is the new target[1] less 0.05. */
        for (light = backgroundLightList.lights.head.next; light != &backgroundLightList.lights.head; light = light->next) {
            light->target[0] = Aabs(light->target[1] + (0.1 * deltaTime - 0.05));
            light->target[1] = Aabs(light->target[1] + 0.1 * deltaTime);
        }
    } else {
        if (deltaTick) {
            MusHandleSetReverb(audioCtrl.handle, 0);
            MusHandleSetVolume(audioCtrl.handle, 80);
        }
        if (!(cheatActivated & 0x400)) {
            if (gameInfo.level >= INTRO) {
                fieldOfViewAim = 35;
            } else {
                fieldOfViewAim = 45;
            }
        }
        rAim = originalAmbientLight[0];
        gAim = originalAmbientLight[1];
        bAim = originalAmbientLight[2];
        AddScaledVector(&cameraUpVect, &cameraUpVect, DeltaPow(0.49f), &cameraUpVectAim, DeltaTake(0.49f, 0.51f));
        MakeUnit(&cameraUpVect);
        for (light = backgroundLightList.lights.head.next; light != &backgroundLightList.lights.head; light = light->next) {
            light->target[0] = light->origX;
            light->target[1] = light->origY;
        }
    }
    if (deltaTick) {
        dist = backgroundLightList.ambient.l.col[0] - rAim;
        if (ABS(dist) > 1.0f) {
            dist /= 4.0f;
            backgroundLightList.ambient.l.col[0] -= dist;
            backgroundLightList.ambient.l.colc[0] -= dist;
        }
        dist = backgroundLightList.ambient.l.col[1] - gAim;
        if (ABS(dist) > 1.0f) {
            dist /= 4.0f;
            backgroundLightList.ambient.l.col[1] -= dist;
            backgroundLightList.ambient.l.colc[1] -= dist;
        }
        dist = backgroundLightList.ambient.l.col[2] - bAim;
        if (ABS(dist) > 1.0f) {
            dist /= 4.0f;
            backgroundLightList.ambient.l.col[2] -= dist;
            backgroundLightList.ambient.l.colc[2] -= dist;
        }
    }
    camera.state->waterState = cameraInWater;
}

s32 CheckActorForWater(Actor *aPtr, Water *ptr, s32 wake) {
    s32 inWater = FALSE;

    if (IsPointWithinBoxPlusHeight(&ptr->box, &aPtr->pos, aPtr->radius)) {
        if (aPtr == &hand && hand.state->mode == HAND_JOINED) {
            return FALSE;
        }
        if (ptr->height <= aPtr->pos.y + aPtr->radius && aPtr->prevPos.y + aPtr->radius < ptr->height) {
            WaterDoSplashes(ptr->height, aPtr, NULL, 1);
            inWater = TRUE;
        } else if (aPtr->pos.y - aPtr->radius < ptr->height && ptr->height <= aPtr->prevPos.y - aPtr->radius) {
            WaterDoSplashes(ptr->height, aPtr, NULL, 1);
            inWater = TRUE;
        }
        WaterDoFloating(ptr, aPtr, !(aPtr->flags & 0x10000000));
        if (wake && aPtr->pos.y - aPtr->radius < ptr->height && ptr->height <= aPtr->pos.y + aPtr->radius) {
            CreateWake(&aPtr->pos, &aPtr->vel, aPtr->obj->platform, ptr->height,
                       LENGTH_2D(&aPtr->vel), (Vec3 *)&vertQ);
            inWater = TRUE;
        }
    }
    if (aPtr->state && aPtr->state->waterState) {
        inWater = TRUE;
    }
    return inWater;
}

void CreateWake(Vec3 *pos, Vec3 *vel, Platform *platform, f32 level, f32 size, Vec3 *normal) {
    Vec3 tempVect;
    CollFace plane;
    Footprint *footprint;
    s16 smallWake;

    smallWake = FALSE;
    /* One wake every third original frame. */
    if (!deltaTick || frameCount % 3 != 0
        || DistanceBetweenPointsSquared(pos, &camera.pos) * (port_camera_dist_scale * port_camera_dist_scale)
               > DETAIL_RANGE * 2.0f) {
        return;
    }
    if (size < 0.5) {
        smallWake = TRUE;
    }
    footprint = (Footprint *)JallocAlloc(sizeof(Footprint), 1, "Footprint");
    footprint->parent = NULL;
    AddVector2D(&tempVect, pos, vel);
    tempVect.y = level;
    *(Vec3 *)&plane = *normal;
    size = MIN(MAX(size, 1.5), 2.0);
    SetupFootprint(footprint, &tempVect, (Vec3 *)&plane, size, 15.0f, wakeTex, 6, 0x400);
    footprint->width = 0.6f;
    footprint->alpha = smallWake ? 50 : 90;
    footprint->r = 200;
    footprint->maxAlpha = 0;
    footprint->b = footprint->g = 255;
    AddFootprint(footprint);
}

INCLUDE_ASM("asm/nonmatchings/water", pad_801B6364);

INCLUDE_RODATA("asm/nonmatchings/water", pad_8010BCBC);

PORT_C_END
