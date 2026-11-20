PORT_C_BEGIN
#include "common.h"
#include "libc.h"
#include "macros.h"
#include "ident.h"
#include "platform.h"
#include "jalloc.h"
#include "water.h"
#include "sound.h"
#include "maths.h"
#include "camera.h"
#include "hand.h"
#include "footprint.h"
#include "debris.h"
#include "bullet.h"
#include "game.h"
#include "ball.h"
#include "blur.h"
#include "actor.h"
#include "collect.h"
#include "controller.h"
#include "weather.h"
#include "bonus.h"
#include "collision.h"
#include "enemy.h"
#include "spell.h"
#include "delta.h"

extern f32 gravity;
extern Debris debris[120];
void DestroyAndFreeActor(Actor *actor);
extern f32 confineV;

void WaterDoFloating(Water *ptr, Actor *aPtr, s32 doCurrent);

void CheckForExit(Platform *platform);
void UpdateFan(Platform *platform);
void FlashScreen(u8 r, u8 g, u8 b, s16 fadeVal, s16 fadeStep);
extern Quat zeroQuat;
extern Vec3 zero;
extern s32 frameCount;
void QuaternionToMatrix(Quat *squat, Matrix *dmatrix);

extern f64 D_8010894C; // 0.6, followed by padding that the linker no longer adds
extern s32 cheatActivated;
extern s32 controlMethod;
extern s32 ballWithHand;
extern f32 levelHandScale;
void DisableControl(s16 time);

/* collision.c and footprint.c rebuild a platform's collision faces when lastUpdate is older than frameCount.
   frameCount holds one value through a frame window, but platforms move in every update. In an update that is not a
   whole frame, this marks the faces old, and the next collision query rebuilds them. At frameCount 0 there is no
   older value: (u16)-1 would keep the faces until frameCount passed 65535. */
static void MarkPlatformFacesOld(Platform *platform) {
    if (!(deltaNewFrame && deltaTick) && frameCount > 0) {
        platform->lastUpdate = frameCount - 1;
    }
}

/* A path platform that reaches a point in a frame window of several updates. On the N64 it jumps onto the point at
   the start of the frame, and the jump is the rest of a frame's travel. Here it moves there through the window:
   start is where it was, relative to its parent, and frame is the window's deltaFrame. */
typedef struct PathArrival {
    PTR32(Platform) platform;
    Vec3 start;
    u32 frame;
} PathArrival;

#define NUM_PATH_ARRIVALS 64

static PathArrival pathArrivals[NUM_PATH_ARRIVALS];

/* The arrival of this platform in the current frame window, or NULL. */
static PathArrival *FindPathArrival(Platform *platform) {
    PathArrival *arrival;

    for (arrival = pathArrivals; arrival < pathArrivals + NUM_PATH_ARRIVALS; arrival++) {
        if (arrival->platform == platform && arrival->frame == deltaFrame) {
            return arrival;
        }
    }
    return NULL;
}

/* Puts an arriving platform at its share of the way from the start to the point: the share of the frame that has
   passed at the end of this update. In the tick it is on the point. */
static void MoveToPathPoint(Platform *platform, PathArrival *arrival, Vec3 *nextPoint) {
    Vec3 pos;

    if (deltaTick) {
        pos = *nextPoint;
        arrival->platform = NULL;
    } else {
        SUB_VECTORS(&pos, nextPoint, &arrival->start);
        SCALE_VECTOR(&pos, deltaPhase);
        ADD_TO_VECTOR(&pos, &arrival->start);
    }
    if (platform->parent != NULL) {
        ADD_VECTORS(&platform->actor.pos, &pos, &platform->parent->actor.pos);
    } else {
        platform->actor.pos = pos;
    }
}

/* Starts a platform's arrival at a point in a window of several updates. FALSE when no record is free: the
   platform then jumps onto the point, as on the N64. */
static s32 StartPathArrival(Platform *platform, Vec3 *nextPoint) {
    PathArrival *arrival;

    for (arrival = pathArrivals; arrival < pathArrivals + NUM_PATH_ARRIVALS; arrival++) {
        if (arrival->platform == NULL || arrival->frame != deltaFrame) {
            arrival->platform = platform;
            arrival->frame = deltaFrame;
            if (platform->parent != NULL) {
                SUB_VECTORS(&arrival->start, &platform->actor.pos, &platform->parent->actor.pos);
            } else {
                arrival->start = platform->actor.pos;
            }
            MoveToPathPoint(platform, arrival, nextPoint);
            return TRUE;
        }
    }
    return FALSE;
}

void UpdatePlatforms(void) {
    Vec3 tempVect2;
    Vec3 tempVect;
    Platform *platPtr1;
    Platform *platPtr2;
    Platform *platform;
    Debris *d;
    Bullet *bullet;
    s32 i;
    s32 x;

    /* Other code (puzzles, bosses) may have moved a platform since its faces were last built. */
    for (platform = platformList.head.next; platform != &platformList.head; platform = platform->next) {
        MarkPlatformFacesOld(platform);
    }
    platPtr1 = platformList.head.next;
    i = 0;
    while (i < platformList.count) {
        i++;
        if (platPtr1->flags & 2) {
            if (platPtr1->destruct != NULL) {
                if (platPtr1->tag == 116 && gameInfo.level == SPACE2A) {
                    FlashScreen(220, 100, 100, 255, -30);
                    for (x = 0; x < 20; x++) {
                        bullet = CreateNewBullet(FLAME_BULLET);
                        bullet->actor.pos = platPtr1->actor.pos;
                        bullet->actor.vel.x = Random(40) - 20;
                        bullet->actor.vel.y = Random(40) - 20;
                        bullet->actor.vel.z = Random(40) - 20;
                        InitBulletSpritesOrMesh(bullet, 0, 0);
                        AddBullet(bullet);
                        bullet->sprites[0].anim.def.startScale = Random(256) + 212;
                        bullet->sprites[0].anim.def.endScale = Random(56) + 256;
                        bullet->sprites[1].anim.def.startScale = Random(256) + 212;
                        bullet->sprites[1].anim.def.endScale = Random(56) + 256;
                        PlaySampleNot3D(99, 100, 128, 128);
                    }
                }
                if (platPtr1->destruct->sampleId) {
                    PlaySample(platPtr1->destruct->sampleId, &platPtr1->actor.pos, platPtr1->destruct->sampleVolume, platPtr1->destruct->samplePitch);
                }
                for (x = 0; x < platPtr1->destruct->numDebris; x++) {
                    tempVect = platPtr1->actor.pos;
                    tempVect.x += (f32)Random((u32)(2.0f * platPtr1->extent.x)) - platPtr1->extent.x;
                    tempVect.y += (f32)Random((u32)(2.0f * platPtr1->extent.y)) - platPtr1->extent.y;
                    tempVect.z += (f32)Random((u32)(2.0f * platPtr1->extent.z)) - platPtr1->extent.z;
                    SUB_VECTORS(&tempVect2, &tempVect, &platPtr1->actor.pos);
                    SCALE_VECTOR(&tempVect2, 0.2);
                    tempVect2.y = Random(10) + 5;
                    d = AddDebris(DESTRUCTIBLE_BIT, &tempVect, &tempVect2, NULL, NULL);
                    if (d != NULL) {
                        d->flags |= 0x10000;
                        d->inst = platPtr1->destruct->inst;
                        SCALE_VECTOR(&d->scale, (f32)(Random(6) + 6) / 10.0f);
                        d->rotVel.x = (f32)Random(10) / 5.0f;
                        d->rotVel.y = (f32)Random(10) / 5.0f;
                    }
                    if (!(platPtr1->flags & 0x80000)) {
                        tempVect2.x = (f32)(Random(101) - 50) / 10.0f;
                        tempVect2.z = (f32)(Random(101) - 50) / 10.0f;
                        tempVect2.y = (f32)Random(101) / 10.0f;
                        AddDebris(PLATBIT, &tempVect, &tempVect2, NULL, NULL);
                    }
                }
            }
            if (!(platPtr1->flags & 0x80000)) {
                CreateDustCircleScaled(&platPtr1->actor.pos, 5, 8.0f, 2.0f);
            }
            KillPlatform(platPtr1, 1);
            platPtr2 = platformList.head.next;
            i = 0;
        } else {
            platPtr2 = platPtr1->next;
        }
        platPtr1 = platPtr2;
    }
    for (platform = platformList.head.next; platform != &platformList.head; platform = platform->next) {
        UpdatePlatform(platform);
        /* The shake of a crumbling platform takes a new random offset once per original frame. Between ticks the
           platform keeps it. */
        if (deltaTick && platform->crumble != NULL && platform->crumble->timer
            && (platform->crumble->gravity == 0.0f || platform->crumble->fallTime >= platform->crumble->timer)) {
            platform->actor.pos.x = platform->crumble->origPos.x + (f32)(Random(25) - 13) / 10.0;
            platform->actor.pos.z = platform->crumble->origPos.z + (f32)(Random(25) - 13) / 10.0;
        }
        if (platform->motion != NULL && !(platform->actor.flags & 0x4000)) {
            GetQuaternionFromXYZRot(&platform->actor.rotQuat, &platform->motion->rot);
        }
        MarkPlatformFacesOld(platform);
    }
    UpdateRopeBridges();
}

void InitPlatformList(void) {
    s32 i;

    platformList.count = 0;
    platformList.head.next = platformList.head.prev = &platformList.head;
    for (i = 0; i < NUM_PATH_ARRIVALS; i++) {
        pathArrivals[i].platform = NULL;
    }
}

void AddPlatform(Platform *platform) {
    if (platform->next == NULL) {
        platformList.count++;
        platform->next = &platformList.head;
        platform->prev = platformList.head.prev;
        platformList.head.prev->next = platform;
        platformList.head.prev = platform;
    }
}

void SubPlatform(Platform *platform) {
    if (platform->next != NULL) {
        platform->prev->next = platform->next;
        platform->next->prev = platform->prev;
        platform->next = NULL;
        platformList.count--;
    }
}

void KillPlatform(Platform *platform, s32 killKids) {
    Footprint *footprint;
    Footprint *footprint2;
    Actor *actPtr;
    Platform *ptr1;
    Debris *d;
    s32 i;
    s32 numChecked;

    numChecked = 0;
    for (i = 0; i < NUM_PATH_ARRIVALS; i++) {
        if (pathArrivals[i].platform == platform) {
            pathArrivals[i].platform = NULL;
        }
    }
    if (platform->sample.handle) {
        StopContinuousSample(&platform->sample);
    }
    if (platform->motion != NULL && platform->motion->sample.handle) {
        StopContinuousSample(&platform->motion->sample);
    }
    if (platform->orbit != NULL && platform->orbit->sample.handle) {
        StopContinuousSample(&platform->orbit->sample);
    }
    if (platform->crumble != NULL && platform->crumble->sample.handle) {
        StopContinuousSample(&platform->crumble->sample);
    }
    for (footprint = footprintList.head.next; footprint != &footprintList.head; footprint = footprint2) {
        footprint2 = footprint->next;
        if (footprint->parent == platform) {
            if (footprint == handControl.footprint) {
                RotateVectorByQuaternion(&footprint->pos, &footprint->pos, &footprint->parent->actor.rotQuat);
                ADD_TO_VECTOR(&footprint->pos, &footprint->parent->actor.pos);
                footprint->normal = *footprint->normalPtr;
                footprint->parent = NULL;
                footprint->normalPtr = NULL;
            } else {
                RemoveFootprint(footprint);
            }
        }
    }
    for (actPtr = actorList.head.next; actPtr != &actorList.head; actPtr = actPtr->next) {
        if (actPtr->obj != NULL) {
            if (actPtr->obj->platform == platform) {
                actPtr->obj->platform = NULL;
            }
            if (actPtr->obj->prevPlatform == platform) {
                actPtr->obj->prevPlatform = NULL;
            }
            if (actPtr->obj->contactPlatform == platform) {
                actPtr->obj->contactPlatform = NULL;
            }
            if (actPtr->obj->prevContactPlatform == platform) {
                actPtr->obj->prevContactPlatform = NULL;
            }
        }
    }
    for (i = 0; i < 120; i++) {
        d = &debris[i];
        if (debris[i].active && debris[i].obj != NULL) {
            if (d->obj->platform == platform) {
                d->obj->platform = NULL;
            }
            if (d->obj->prevPlatform == platform) {
                d->obj->prevPlatform = NULL;
            }
            if (d->obj->contactPlatform == platform) {
                d->obj->contactPlatform = NULL;
            }
            if (d->obj->prevContactPlatform == platform) {
                d->obj->prevContactPlatform = NULL;
            }
        }
    }
    DestroyAndFreeActor(&platform->actor);
    if (killKids) {
        ptr1 = platformList.head.next;
        while (numChecked < platformList.count) {
            numChecked++;
            if (ptr1->parent == platform) {
                KillPlatform(ptr1, 1);
                ptr1 = platformList.head.next;
                numChecked = 0;
            } else {
                ptr1 = ptr1->next;
            }
        }
    }
    SubPlatform(platform);
    if (platform->path != NULL) {
        for (i = 0; i < 20; i++) {
            JallocFree(&platform->path->points[i]);
        }
        JallocFree(&platform->path);
    }
    JallocFree(&platform->scale);
    JallocFree(&platform->fan);
    JallocFree(&platform->teleport);
    JallocFree(&platform->catapult);
    JallocFree(&platform->restartGate);
    JallocFree(&platform->motion);
    JallocFree(&platform->crumble);
    JallocFree(&platform->orbit);
    JallocFree(&platform->link);
    JallocFree(&platform->confine);
    JallocFree(&platform->confine);
    JallocFree(&platform->push);
    JallocFree(&platform->actor.faces);
    JallocFree(&platform->destruct);
    JallocFree(&platform->blur);
    JallocFree(&platform);
}

/* TRUE when a scale that moves from start towards end has reached or passed end. */
static s32 ScalePassed(f32 scale, f32 start, f32 end) {
    if (end >= start) {
        return scale >= end;
    }
    return scale <= end;
}

void ScalePlatform(Platform *platform) {
    s32 done;
    s32 j;

    if (platform->scale == NULL) {
        return;
    }
    done = 0;
    if (platform->scale->time) {
        /* count counts the original frames of the wait before each grow or shrink. */
        if (platform->scale->count > 1) {
            if (deltaTick) {
                platform->scale->count--;
            }
        } else if (platform->scale->count < -1) {
            if (deltaTick) {
                platform->scale->count++;
            }
        } else if (platform->scale->count != 0) {
            for (j = 0; j < 3; j++) {
                if (platform->scale->count == 1) {
                    (&platform->actor.scale.x)[j] +=
                        (platform->scale->max[j] - platform->scale->min[j]) / platform->scale->time * deltaTime;
                } else {
                    (&platform->actor.scale.x)[j] -=
                        (platform->scale->max[j] - platform->scale->min[j]) / platform->scale->time * deltaTime;
                }
                /* A step of part of a frame can pass the end of the range by more than 0.0001. The scale then also
                   arrives when it has passed the end it moves towards. */
                if (ABS((&platform->actor.scale.x)[j] - platform->scale->max[j]) <= 0.0001
                    || (platform->scale->count == 1
                        && ScalePassed((&platform->actor.scale.x)[j], platform->scale->min[j],
                                       platform->scale->max[j]))) {
                    done++;
                    (&platform->actor.scale.x)[j] = platform->scale->max[j];
                } else if (ABS((&platform->actor.scale.x)[j] - platform->scale->min[j]) <= 0.0001
                           || (platform->scale->count == -1
                               && ScalePassed((&platform->actor.scale.x)[j], platform->scale->max[j],
                                              platform->scale->min[j]))) {
                    done++;
                    (&platform->actor.scale.x)[j] = platform->scale->min[j];
                }
            }
        }
        if (done == 3) {
            platform->scale->count = -platform->scale->count * (platform->scale->repeat + 1);
        }
    }
}

void CrumblePlatform(Platform *platform) {
    f32 dv;

    if (platform->crumble == NULL || platform->crumble->timer <= 0) {
        return;
    }
    if (platform->crumble->timer > platform->crumble->shakeTime + 400 ||
        handControl.checkpoint->restartGate->restartTimer) {
        platform->actor.pos = platform->crumble->origPos;
        platform->actor.alpha = 100;
        ZERO_VECTOR(&platform->actor.vel);
        platform->flags &= ~4;
        platform->crumble->timer = 0;
        return;
    }
    /* The timer counts original frames. */
    if (platform->crumble->timer > platform->crumble->shakeTime) {
        platform->actor.alpha = 0;
        platform->flags |= 4;
        if (platform->crumble->sample.handle) {
            StopContinuousSample(&platform->crumble->sample);
        }
    }
    if (deltaTick) {
        platform->crumble->timer++;
    }
    if (platform->crumble->timer > platform->crumble->fallTime) {
        dv = -gravity * platform->crumble->gravity * deltaTime;
        platform->actor.vel.y += dv;
        /* ConfinePlatform moves the platform by the new speed. With this the fall passes through the N64's heights
           at whole frames. */
        if (deltaTime != 1.0f) {
            platform->actor.pos.y -= DeltaAccelPos(dv);
        }
    }
    if (platform->crumble->sample.id) {
        if (platform->crumble->timer > platform->crumble->fallTime) {
            /* The volume is an integer: the fade steps once per original frame. */
            if (deltaTick) {
                platform->crumble->sample.volume -= 60;
            }
            if (platform->crumble->sample.volume <= 0) {
                StopContinuousSample(&platform->crumble->sample);
                return;
            }
        } else if (!platform->crumble->sample.handle) {
            PlayContinuousSample(&platform->crumble->sample, platform->crumble->sample.id, platform->crumble->sample.baseVolume,
                                 &platform->actor.pos, platform->crumble->sample.pitch);
        }
        UpdateContinuousSample(&platform->crumble->sample);
    }
}

void MovePlatform(Platform *platform, Vec3 *nextPoint) {
    Vec3 pos;
    Vec3 distance;
    PathPoint *pt;
    Platform *p;
    f32 dist;
    f32 brake;
    f32 friction;
    PathArrival *arrival;
    f32 carry;
    s32 moving = TRUE;

    /* ConfinePlatform moves the platform by vel * deltaTime. */
    friction = DeltaDamp(platform->actor.friction);
    SCALE_VECTOR(&platform->actor.vel, friction);
    if (platform->path != NULL && platform->path->numPoints >= 2) {
        /* The path logic is one original frame of the N64's: the waits (moving counts frames), the arrival at a
           point (within one frame's travel), and the push towards the next point (a frame's acceleration at once).
           It runs in the first update of each frame window. The other updates of the window move the platform on at
           the speed it set, and at the end of the window the platform is where the N64 has it. */
        if (deltaNewFrame) {
            /* The original frame began carry frames before the start of this update, during the previous update. The
               logic works from where the platform was at the start of the frame. The platform then covers the carry
               at its new speed. */
            carry = deltaTick ? 0.0f : deltaPhase - deltaTime;
            if (carry > 0.0f) {
                AddToOneScaledVector(&platform->actor.pos, &platform->actor.vel, -carry);
            }
            if (platform->path->moving >= -9 && platform->path->moving <= -4) {
                if (ball.obj->platform == platform) {
                    platform->path->moving = -4;
                } else if (--platform->path->moving == -10) {
                    platform->path->moving = 1;
                }
            }
            if (platform->path->moving >= -15 && platform->path->moving <= -10) {
                if (hand.obj->platform == platform) {
                    platform->path->moving = -10;
                } else if (--platform->path->moving == -16) {
                    platform->path->moving = 1;
                }
            }
            if (platform->path->moving > 0) {
                if (--platform->path->moving == 0) {
                    platform->path->target =
                        (platform->path->target + platform->path->numPoints + platform->path->dir) % platform->path->numPoints;
                }
            } else if (platform->path->moving == 0
                       && (!(platform->flags & 0x1000) || platform->motion->rot.y == platform->motion->targetRot[1]
                           || (platform->flags & 0x2000))) {
                if (platform->parent != NULL) {
                    SUB_VECTORS(&pos, &platform->actor.pos, &platform->parent->actor.pos);
                } else {
                    pos = platform->actor.pos;
                }
                SUB_VECTORS(&distance, nextPoint, &pos);
                dist = DOT_PRODUCT(&distance, &distance);
                if (dist <= platform->actor.speed * platform->actor.speed) {
                    if (!(platform->flags & 0x2000)) {
                        moving = FALSE;
                        if (deltaTick || !StartPathArrival(platform, nextPoint)) {
                            if (platform->parent != NULL) {
                                ADD_VECTORS(&platform->actor.pos, nextPoint, &platform->parent->actor.pos);
                            } else {
                                platform->actor.pos = *nextPoint;
                            }
                        }
                        platform->actor.vel = zero;
                        platform->actor.speed = 0.0f;
                    }
                    platform->path->moving = platform->path->points[platform->path->target]->time;
                    platform->path->current = platform->path->target;
                    pt = platform->path->points[platform->path->current];
                    if (pt->sampleId != 0) {
                        PlaySample(pt->sampleId, &platform->actor.pos, pt->sampleVolume, pt->samplePitch);
                    }
                    if (platform->flags & 0x1000) {
                        pt = platform->path->points[(platform->path->target + platform->path->numPoints + platform->path->dir)
                                                    % platform->path->numPoints];
                        platform->motion->targetRot[1] =
                            Aabs(atan2(platform->actor.pos.x - pt->pos.x, platform->actor.pos.z - pt->pos.z));
                    }
                    if ((platform->flags & 0x20)
                        && (platform->path->current == 0 || platform->path->current == platform->path->numPoints - 1)) {
                        platform->path->dir = -platform->path->dir;
                    }
                    if ((platform->pathStopFlags & 2) || (platform->path->current == 0 && (platform->pathStopFlags & 1))) {
                        platform->pathStopFlags |= 0x800;
                    }
                }
                if (moving) {
                    if (platform->pathStopFlags & 0x800) {
                        platform->actor.speed = 0.0f;
                        ZERO_VECTOR(&platform->actor.vel);
                    } else {
                        if (platform->flags & 0x2000) {
                            platform->actor.vel.x -= platform->path->accel * sinf(platform->motion->rot.y);
                            platform->actor.vel.y += platform->path->accel * sinf(platform->motion->rot.x);
                            platform->actor.vel.z -= platform->path->accel * cosf(platform->motion->rot.y);
                        } else {
                            brake = (platform->path->accel + platform->actor.speed) * platform->actor.speed
                                    / (2.0f * platform->path->accel);
                            MakeUnit(&distance);
                            if (dist < brake * brake) {
                                if (platform->path->accel < platform->actor.speed) {
                                    AddToOneScaledVector(&platform->actor.vel, &distance, -platform->path->accel);
                                }
                            } else {
                                AddToOneScaledVector(&platform->actor.vel, &distance, platform->path->accel);
                            }
                        }
                        platform->actor.speed = LENGTH(&platform->actor.vel);
                        if (platform->maxSpeed < platform->actor.speed) {
                            SCALE_VECTOR(&platform->actor.vel, platform->maxSpeed / platform->actor.speed);
                            platform->actor.speed = platform->maxSpeed;
                        }
                    }
                }
            } else if (platform->path->moving == -100) {
                platform->path->moving--;
            }
            if (carry > 0.0f && FindPathArrival(platform) == NULL) {
                AddToOneScaledVector(&platform->actor.pos, &platform->actor.vel, carry);
            }
        } else if ((arrival = FindPathArrival(platform)) != NULL) {
            MoveToPathPoint(platform, arrival, nextPoint);
        }
    } else {
        platform->actor.speed = LENGTH(&platform->actor.vel);
    }
    if (platform->link != NULL && platform->link->moveToSpinScale != 0.0f) {
        for (p = FindTaggedPlatform(&platformList.head, platform->link->moveToSpinTag); p != NULL;
             p = FindTaggedPlatform(p, platform->link->moveToSpinTag)) {
            p->motion->speed = DOT_PRODUCT(&platform->actor.vel, &platform->link->moveToSpinDir) * platform->link->moveToSpinScale;
        }
    }
}

void PlatformInherit(Platform *platform, Vec3 *nextPoint) {
    Vec3 tempVect;
    Platform *plat;
    s32 k;
    s32 i;

    platform->actor.platPrevPos = platform->actor.pos;
    for (plat = platform->parent; plat != NULL; plat = plat->parent) {
        for (i = 0; i < 3; i++) {
            (&platform->actor.pos.x)[i] += (&plat->actor.pos.x)[i] - (&plat->actor.prevPos.x)[i];
        }
    }
    plat = platform->parent;
    platform->actor.prevPos = platform->actor.pos;
    if (platform->motion != NULL) {
        platform->motion->oldRot = platform->motion->rot;
    }
    if (platform->path != NULL && platform->path->numPoints >= 2) {
        *nextPoint = platform->path->points[platform->path->target]->pos;
    }
    if (plat == NULL || plat->motion == NULL) {
        return;
    }
    if (plat->motion->rot.y - plat->motion->oldRot.y != 0.0f) {
        RotateVector2D(&platform->actor.vel, &platform->actor.vel, plat->motion->rot.y - plat->motion->oldRot.y);
    } else if (plat->motion->rot.x - plat->motion->oldRot.x != 0.0f) {
        RotateVector2DXYZ(&platform->actor.vel, &platform->actor.vel, plat->motion->rot.x - plat->motion->oldRot.x, 0);
    } else if (plat->motion->rot.z - plat->motion->oldRot.z != 0.0f) {
        RotateVector2DXYZ(&platform->actor.vel, &platform->actor.vel, plat->motion->rot.z - plat->motion->oldRot.z, 2);
    }
    k = 10;
    for (i = 0; i < 3; i++) {
        if ((platform->flags & 0x40) && plat->motion != NULL
            && (&plat->motion->rot.x)[i] != (&plat->motion->oldRot.x)[i]) {
            (&platform->motion->rot.x)[i] =
                Aabs((&platform->motion->rot.x)[i] + (&plat->motion->rot.x)[i] - (&plat->motion->oldRot.x)[i]);
            platform->motion->targetRot[i] =
                Aabs((&platform->motion->rot.x)[i] + (&plat->motion->rot.x)[i] - (&plat->motion->oldRot.x)[i]);
        }
        if (plat->motion->active[i]) {
            k = i;
        }
    }
    if (k != 10) {
        SUB_VECTORS(&tempVect, &platform->actor.pos, &plat->actor.pos);
        RotateVector2DXYZ(&tempVect, &tempVect, (&plat->motion->rot.x)[k] - (&plat->motion->oldRot.x)[k], k);
        ADD_VECTORS(&platform->actor.pos, &plat->actor.pos, &tempVect);
        if (platform->path != NULL && platform->path->numPoints >= 2) {
            RotateVector2DXYZ(nextPoint, &platform->path->points[platform->path->target]->pos, (&plat->motion->rot.x)[k], k);
        }
    }
}

/* TRUE when the angle d is less than one rotation step of speed (per original frame) in this update. */
static s32 WithinStep(f32 d, f32 speed) {
    f32 step = speed * deltaTime;

    return step > 0.0f ? d < step : d < -step;
}

void SpinPlatform(Platform *platform) {
    Quat tempQ;
    Quat tempQ2;
    Quat tempQ3;
    Quat spin;
    f32 cur;
    f32 target;
    f32 d;
    s32 i;

    if (platform->actor.flags & 0x4000) {
        /* spinAxis and spinSpeed make a rotation per original frame. */
        spin = *(Quat *)&platform->actor.spinAxis;
        spin.w *= deltaTime;
        GetQuaternionFromRotation(&tempQ, &spin);
        QuaternionMultiply(&platform->actor.rotQuat, &tempQ, &platform->actor.rotQuat);
        platform->actor.spinSpeed *= DeltaDamp(platform->motion->damping);
    }
    if (platform->flags & 0x10000) {
        GetRotationFromQuaternion(&tempQ2, &platform->actor.rotQuat);
        if (platform->actor.rotQuat.w > 0.99995) {
            platform->actor.rotQuat = zeroQuat;
            platform->actor.spinSpeed = 0.0f;
        } else {
            if (tempQ2.w > platform->motion->rotLimit) {
                platform->actor.spinSpeed = 0.0f;
            }
            /* The platform turns back by the share accel / rotLimit of its tilt per original frame: an approach. */
            if (deltaTime == 1.0f) {
                tempQ2.w = -(platform->motion->accel * tempQ2.w) / platform->motion->rotLimit;
            } else {
                tempQ2.w = -tempQ2.w * DeltaLerp(platform->motion->accel / platform->motion->rotLimit);
            }
            GetQuaternionFromRotation(&tempQ3, &tempQ2);
            QuaternionMultiply(&platform->actor.rotQuat, &tempQ3, &platform->actor.rotQuat);
            GetRotationFromQuaternion(&tempQ2, &platform->actor.rotQuat);
        }
    }
    if (platform->motion == NULL || platform->motion->speed == 0.0f) {
        return;
    }
    for (i = 0; i < 3; i++) {
        if (!platform->motion->active[i]) {
            continue;
        }
        /* timer counts original frames: the pauses, and the frames of turning before the next pause test. */
        if (platform->motion->timer > 0) {
            if (deltaTick) {
                platform->motion->timer--;
                if (platform->motion->pauseType == 1 && platform->motion->timer == 0) {
                    platform->motion->timer = -2;
                }
            }
        } else if (platform->motion->timer == 0) {
            if (platform->motion->pauseType) {
                cur = Aabs((&platform->motion->rot.x)[i]);
                if (platform->motion->speed < 0.0f) {
                    target = Aabs(TWOPI - platform->motion->pauseIndex * TWOPI / platform->motion->pauseType + platform->motion->baseRot);
                } else {
                    target = Aabs(platform->motion->pauseIndex * TWOPI / platform->motion->pauseType + platform->motion->baseRot);
                }
                d = FindShortestAngle(cur, target);
                if (WithinStep(d, platform->motion->speed)) {
                    platform->motion->timer = platform->motion->pause;
                    platform->motion->rotVel[i] = 0;
                    if (d < 0.0001) {
                        (&platform->motion->rot.x)[i] = target;
                    }
                    if ((platform->spinStopFlags & 2) || (platform->motion->pauseIndex == 0 && (platform->spinStopFlags & 1))) {
                        platform->motion->speed = 0.0f;
                        (&platform->motion->rot.x)[i] = target;
                    }
                    platform->motion->pauseIndex = (platform->motion->pauseIndex + 1) % platform->motion->pauseType;
                    if (platform->motion->sample.id == 0 && platform->motion->stopSampleId) {
                        PlaySample(platform->motion->stopSampleId, &platform->actor.pos, platform->motion->stopSampleVolume,
                                   platform->motion->stopSamplePitch);
                    }
                }
            } else if (platform->motion->flip != 0.0f) {
                if (platform->motion->speed < 0.0f) {
                    cur = Aabs((&platform->motion->rot.x)[i] - platform->motion->baseRot);
                } else {
                    cur = Aabs(platform->motion->baseRot - (&platform->motion->rot.x)[i]);
                }
                d = FindShortestAngle(cur, platform->motion->flip);
                if (WithinStep(d, platform->motion->speed)
                    || (d = FindShortestAngle(TWOPI - platform->motion->flip, cur),
                        WithinStep(d, platform->motion->speed))) {
                    platform->motion->timer = platform->motion->pause;
                    platform->motion->speed = -platform->motion->speed;
                    platform->motion->rotVel[i] = 0;
                    if ((platform->spinStopFlags & 2) || (platform->motion->pauseIndex == 1 && (platform->spinStopFlags & 1))) {
                        platform->motion->speed = 0.0f;
                    }
                    StopContinuousSample(&platform->motion->sample);
                    if (platform->motion->stopSampleId) {
                        PlaySample(platform->motion->stopSampleId, &platform->actor.pos, platform->motion->stopSampleVolume,
                                   platform->motion->stopSamplePitch);
                    }
                }
            }
        }
        if (platform->motion->timer <= 0) {
            if (platform->motion->timer < 0) {
                if (platform->motion->timer > -10) {
                    if (deltaTick) {
                        platform->motion->timer++;
                    }
                    platform->motion->rotVel[i] = platform->motion->speed;
                } else if (platform->path != NULL && platform->path->current == -platform->motion->timer / 10
                           && platform->path->target == platform->path->current) {
                    platform->motion->timer = 1;
                }
            } else {
                platform->motion->rotVel[i] = platform->motion->speed;
            }
        }
    }
}

void OrbitPlatform(Platform *platform) {
    f32 d[2];
    Platform *plat = platform->parent;
    s32 k;
    f32 dist;
    f32 cur;
    f32 ang;
    f32 diff;
    f32 flipDiff;
    s32 axis;
    s32 i;

    if (platform->orbit == NULL) {
        return;
    }
    if (platform->orbit->timer) {
        if (platform->orbit->sample.handle) {
            UpdateContinuousSample(&platform->orbit->sample);
            /* The volume is an integer: the fade steps once per original frame. */
            if (deltaTick) {
                platform->orbit->sample.volume -= 60;
            }
            if (platform->orbit->sample.volume <= 0) {
                StopContinuousSample(&platform->orbit->sample);
                if (platform->orbit->stopSampleId) {
                    PlaySample(platform->orbit->stopSampleId, &platform->actor.pos, platform->orbit->stopSampleVolume,
                               platform->orbit->stopSamplePitch);
                }
            }
        }
    } else if (platform->orbit->sample.id) {
        if (!platform->orbit->sample.handle) {
            PlayContinuousSample(&platform->orbit->sample, platform->orbit->sample.id,
                                 platform->orbit->sample.baseVolume, &platform->actor.pos,
                                 platform->orbit->sample.pitch);
        } else {
            UpdateContinuousSample(&platform->orbit->sample);
        }
    }
    axis = 10;
    if (platform->orbit->speed == 0.0f) {
        return;
    }
    k = 0;
    for (i = 0; i < 3; i++) {
        if (!platform->orbit->axis[i]) {
            d[k] = (&platform->actor.pos.x)[i] - (&platform->orbit->center.x)[i];
            if (plat != NULL) {
                d[k] -= (&plat->actor.pos.x)[i];
            }
            k++;
        } else {
            axis = i;
        }
    }
    dist = sqrtf(d[0] * d[0] + d[1] * d[1]);
    if (frameCount == 0) {
        cur = platform->orbit->angle = platform->orbit->startAngle = atan2(d[1], d[0]);
    } else {
        cur = platform->orbit->startAngle;
    }
    /* timer counts original frames, as in SpinPlatform. */
    if (platform->orbit->timer > 0) {
        if (deltaTick) {
            platform->orbit->timer--;
            if (platform->orbit->pauseType == 1 && platform->orbit->timer == 0) {
                platform->orbit->timer = -2;
            }
        }
    } else if (platform->orbit->timer == 0) {
        if (platform->orbit->axis[1]) {
            if (platform->orbit->speed < 0.0f) {
                ang = Aabs(cur - platform->orbit->angle);
            } else {
                ang = Aabs(platform->orbit->angle - cur);
            }
        } else if (platform->orbit->speed > 0.0f) {
            ang = Aabs(cur - platform->orbit->angle);
        } else {
            ang = Aabs(platform->orbit->angle - cur);
        }
        if (platform->orbit->pauseType) {
            diff = FindShortestAngle(ang, platform->orbit->pauseIndex * TWOPI / platform->orbit->pauseType);
            if (WithinStep(diff, platform->orbit->speed)) {
                platform->orbit->timer = platform->orbit->pause;
                platform->actor.vel = zero;
                platform->orbit->pauseIndex = (platform->orbit->pauseIndex + 1) % platform->orbit->pauseType;
                if ((platform->orbitStopFlags & 2) || (platform->orbit->pauseIndex == 1 && (platform->orbitStopFlags & 1))) {
                    platform->orbit->speed = 0.0f;
                }
            }
        } else if (platform->orbit->flip != 0.0f) {
            flipDiff = FindShortestAngle(ang, platform->orbit->flip);
            if (WithinStep(flipDiff, platform->orbit->speed)
                || (diff = FindShortestAngle(TWOPI - platform->orbit->flip, ang),
                    WithinStep(diff, platform->orbit->speed))) {
                platform->orbit->timer = platform->orbit->pause;
                platform->orbit->speed = -platform->orbit->speed;
                platform->actor.vel = zero;
                if ((platform->orbitStopFlags & 2) || (platform->orbit->pauseIndex == 1 && (platform->orbitStopFlags & 1))) {
                    platform->orbit->speed = 0.0f;
                }
            }
        }
    }
    if (platform->orbit->timer <= 0) {
        if (platform->orbit->timer < 0 && deltaTick) {
            platform->orbit->timer++;
        }
        if (axis == 1) {
            platform->orbit->startAngle = platform->orbit->startAngle - platform->orbit->speed * deltaTime;
        } else {
            platform->orbit->startAngle = platform->orbit->startAngle + platform->orbit->speed * deltaTime;
        }
        platform->orbit->startAngle = Aabs(platform->orbit->startAngle);
        d[0] = dist * __cosf(platform->orbit->startAngle);
        d[1] = dist * __sinf(platform->orbit->startAngle);
        /* vel is the step to the next point of the orbit, over deltaTime: a speed per original frame.
           ConfinePlatform moves the platform by vel * deltaTime. */
        k = 0;
        if (plat != NULL) {
            for (i = 0; i < 3; i++) {
                if (!platform->orbit->axis[i]) {
                    (&platform->actor.vel.x)[i] =
                        DeltaPerFrame(d[k++] + (&platform->orbit->center.x)[i] - (&platform->actor.prevPos.x)[i]
                                      + (&plat->actor.pos.x)[i]);
                }
            }
            return;
        }
        for (i = 0; i < 3; i++) {
            if (!platform->orbit->axis[i]) {
                (&platform->actor.vel.x)[i] =
                    DeltaPerFrame((&platform->orbit->center.x)[i] - (&platform->actor.prevPos.x)[i] + d[k++]);
            }
        }
    }
}

void ConfinePlatform(Platform *platform) {
    Vec3 pos;
    Vec3 *plane;
    Platform *plat;
    Platform *altPlatform;
    f32 size;
    f32 alt;
    f32 fallVelY;
    s32 falling;
    s32 i;

    altPlatform = platform->parent;
    if (platform->actor.flags & 1) {
        size = platform->extent.y + confineV;
        pos.y = platform->actor.pos.y;
        pos.x = platform->actor.pos.x + platform->extent.x - 0.1;
        pos.z = platform->actor.pos.z + platform->extent.z - 0.1;
        alt = Altitude(&pos, &plane, &plat, 0);
        falling = 0;
        if (alt > size || alt == -1.0f) {
            pos.x = platform->actor.pos.x - platform->extent.x + 0.1;
            alt = Altitude(&pos, &plane, &plat, 0);
            if (alt > size || alt == -1.0f) {
                pos.z = platform->actor.pos.z - platform->extent.z + 0.1;
                alt = Altitude(&pos, &plane, &plat, 0);
                if (alt > size || alt == -1.0f) {
                    pos.x = platform->actor.pos.x + platform->extent.x - 0.1;
                    alt = Altitude(&pos, &plane, &plat, 0);
                    if (alt > size || alt == -1.0f) {
                        fallVelY = platform->actor.vel.y;
                        platform->actor.vel.y -= gravity * 1.5 * deltaTime;
                        falling = 1;
                    }
                }
            }
        }
        /* On the ground the vertical speed reverses and drops to 0.7 of its size, once per original frame. Between
           ticks the platform moves on at its speed. */
        if (!falling && deltaTick) {
            platform->actor.vel.y = -platform->actor.vel.y * 0.7;
        }
        /* The fall passes through the N64's heights at whole frames. */
        if (falling && deltaTime != 1.0f) {
            platform->actor.pos.y -= DeltaAccelPos(platform->actor.vel.y - fallVelY);
        }
    }
    for (i = 0; i < 3; i++) {
        (&platform->actor.pos.x)[i] += (&platform->actor.vel.x)[i] * deltaTime;
    }
    if (platform->confine != NULL) {
        for (i = 0; i < 3; i++) {
            if (platform->confine->size[i] >= 0.0f) {
                (&pos.x)[i] = (&platform->actor.pos.x)[i];
                if (altPlatform != NULL) {
                    (&pos.x)[i] -= (&altPlatform->actor.pos.x)[i];
                }
                if ((&pos.x)[i] < platform->confine->min[i]) {
                    (&pos.x)[i] = platform->confine->min[i];
                    (&platform->actor.vel.x)[i] = 0.0f;
                } else if ((&pos.x)[i] > platform->confine->min[i] + platform->confine->size[i]) {
                    (&pos.x)[i] = platform->confine->min[i] + platform->confine->size[i];
                    (&platform->actor.vel.x)[i] = 0.0f;
                }
                (&platform->actor.pos.x)[i] = (&pos.x)[i];
                if (altPlatform != NULL) {
                    (&platform->actor.pos.x)[i] += (&altPlatform->actor.pos.x)[i];
                }
            }
        }
    }
}

void RotatePlatform(Platform *platform) {
    Vec3 tempVect;
    Vec3 tempVect2;
    Vec3 tempVect3;
    Actor *actor;
    Platform *platPtr;
    f32 lastVel;
    f32 d;
    f32 acc;
    f32 maxRot;
    s32 i;
    s32 t;

    actor = NULL;
    if (platform->motion == NULL) {
        return;
    }
    lastVel = 0.0f;
    if (platform->flags & 0x800) {
        actor = &ball;
    } else if (platform->flags & 0x400) {
        actor = &hand;
    }
    if (actor != NULL) {
        platform->motion->targetRot[1] =
            Aabs(atan2(actor->pos.x - platform->actor.pos.x, actor->pos.z - platform->actor.pos.z));
    }
    for (i = 0; i < 3; i++) {
        if (platform->motion->rotVel[i] != 0.0f) {
            lastVel = platform->motion->rotVel[i];
        }
        if (platform->flags & 0x10) {
            if (platform->motion->rotVel[i] > platform->motion->maxRotVel) {
                platform->motion->rotVel[i] = platform->motion->maxRotVel;
            }
            if (platform->motion->rotVel[i] < -platform->motion->maxRotVel) {
                platform->motion->rotVel[i] = -platform->motion->maxRotVel;
            }
        }
        if (platform->motion->seekTarget[i] == 1) {
            if (platform->motion->snapCount >= 2) {
                platform->motion->targetRot[i] =
                    Aabs((s32)(platform->motion->snapCount *
                               Aabs((&platform->motion->rot.x)[i] - platform->motion->baseRot + PI / platform->motion->snapCount) /
                               TWOPI) *
                             PI * 2 / platform->motion->snapCount +
                         platform->motion->baseRot);
            }
            d = FindShortestAngle((&platform->motion->rot.x)[i], platform->motion->targetRot[i]);
            if (i == 2 && (platform->flags & 0x2000)) {
                acc = platform->motion->bankScale * 0.002;
            } else {
                acc = platform->motion->accel;
            }
            /* acc is an angular acceleration per original frame. The windows and the speed test below use it as an
               angle and a speed, which keep their units. */
            if (acc != 0.0f) {
                if (ABS(d) <= 0.01) {
                    (&platform->motion->rot.x)[i] = platform->motion->targetRot[i];
                    platform->motion->rotVel[i] *= DeltaDampD(0.8);
                } else if ((&platform->motion->rot.x)[i] < platform->motion->targetRot[i] - acc) {
                    if (PI - acc < platform->motion->targetRot[i] - (&platform->motion->rot.x)[i]) {
                        platform->motion->rotVel[i] -= MIN(d, acc) * deltaTime;
                    } else {
                        platform->motion->rotVel[i] += MIN(d, acc) * deltaTime;
                    }
                } else if (platform->motion->targetRot[i] + acc < (&platform->motion->rot.x)[i]) {
                    if (acc + PI < (&platform->motion->rot.x)[i] - platform->motion->targetRot[i]) {
                        platform->motion->rotVel[i] += MIN(d, acc) * deltaTime;
                    } else {
                        platform->motion->rotVel[i] -= MIN(d, acc) * deltaTime;
                    }
                } else if (!(platform->flags & 0x10)) {
                    if (platform->motion->damping == 1.0f || ABS(platform->motion->rotVel[i]) <= acc) {
                        (&platform->motion->rot.x)[i] = platform->motion->targetRot[i];
                        platform->motion->rotVel[i] = 0;
                    }
                }
            }
            /* A tipping platform's turn grows by 1.3 per original frame. */
            if (platform->motion->tipAngle != 0.0f && platform->motion->tipAngle < d) {
                platform->motion->rotVel[i] *= DeltaDampD(1.3);
                platform->motion->accel = 0.0f;
            }
        }
        /* idleCount stays per update. Its only reader, UpdateRopeBridges, runs after every platform's RotatePlatform
           and tests idleCount >= 2, which three steps per call always pass. */
        platform->motion->idleCount++;
        if (platform->motion->pendulumLength != 0.0f && platform->motion->active[i] != 0) {
            /* The blur trail takes a new point once per original frame. */
            if (platform->blur != NULL && deltaTick) {
                tempVect.x = 0.0f;
                tempVect.y = -platform->extent.y;
                tempVect.z = 0.0f;
                RotateVector2DXYZ(&tempVect, &tempVect, (&platform->motion->rot.x)[i], i);
                tempVect2 = tempVect;
                SCALE_VECTOR(&tempVect2, platform->blur->innerScale);
                ADD_TO_VECTOR(&tempVect, &platform->actor.pos);
                ADD_TO_VECTOR(&tempVect2, &platform->actor.pos);
                SUB_VECTORS(&tempVect3, &tempVect, &camera.pos);
                MakeUnit(&tempVect3);
                AddToOneScaledVector(&tempVect, &tempVect3, 2.0f);
                AddToOneScaledVector(&tempVect2, &tempVect3, 2.0f);
                IncrementBlurFX(&platform->blur->blur, &tempVect, &tempVect2);
            }
            platform->motion->rotVel[i] -= FindShortestAngleSigned((&platform->motion->rot.x)[i], 0.0f)
                                           / platform->motion->pendulumLength * deltaTime;
        }
        (&platform->motion->rot.x)[i] += platform->motion->rotVel[i] * deltaTime;
        (&platform->motion->rot.x)[i] = Aabs((&platform->motion->rot.x)[i]);
        if (platform->motion->rotLimit > 0.0f && platform->motion->active[i] == 1) {
            d = ((&platform->motion->rot.x)[i] > PI) ? (&platform->motion->rot.x)[i] - TWOPI : (&platform->motion->rot.x)[i];
            d = (d < -PI) ? d + TWOPI : d;
            if (platform->motion->limitFlags & 1) {
                maxRot = platform->motion->rotLimit;
            } else {
                maxRot = platform->motion->tipAngle * 4.0f / 5.0f;
            }
            if (platform->motion->baseRot + maxRot < d) {
                (&platform->motion->rot.x)[i] = platform->motion->baseRot + maxRot;
                platform->motion->rotVel[i] = 0;
                if (platform->motion->tipAngle != 0.0f && maxRot == platform->motion->rotLimit) {
                    platform->flags &= ~0x10;
                    platform->motion->seekTarget[i] = 0;
                    if (platform->motion->sample.id == 0 && platform->motion->stopSampleId) {
                        PlaySample(platform->motion->stopSampleId, &platform->actor.pos, platform->motion->stopSampleVolume,
                                   platform->motion->stopSamplePitch);
                    }
                }
            }
            if (platform->motion->limitFlags & 2) {
                maxRot = platform->motion->rotLimit;
            } else {
                maxRot = platform->motion->tipAngle * 3.0f / 5.0f;
            }
            if (d < platform->motion->baseRot - maxRot - 0.0001) {
                (&platform->motion->rot.x)[i] = platform->motion->baseRot - maxRot;
                platform->motion->rotVel[i] = 0;
                if (platform->motion->tipAngle != 0.0f && maxRot == platform->motion->rotLimit) {
                    platform->flags &= ~0x10;
                    platform->motion->seekTarget[i] = 0;
                    if (platform->motion->sample.id == 0 && platform->motion->stopSampleId) {
                        PlaySample(platform->motion->stopSampleId, &platform->actor.pos, platform->motion->stopSampleVolume,
                                   platform->motion->stopSamplePitch);
                    }
                }
            }
        }
        if (i == 2 && (platform->flags & 0x2000)) {
            platform->motion->rotVel[2] = 0;
        } else {
            platform->motion->rotVel[i] *= DeltaDamp(platform->motion->damping);
        }
        /* A turn slower than 0.0005 per original frame stops, tested in ticks. A turn that starts from rest gains
           its speed over the updates of a frame, and in a single update it can still be below the limit. */
        if (deltaTick && ABS(platform->motion->rotVel[i]) <= 0.0005) {
            platform->motion->rotVel[i] = 0.0f;
        }
        if (platform->link != NULL) {
            if (platform->link->spinToMoveScale != 0.0f) {
                for (platPtr = FindTaggedPlatform(&platformList.head, platform->link->spinToMoveTag); platPtr != NULL;
                     platPtr = FindTaggedPlatform(platPtr, platform->link->spinToMoveTag)) {
                    platPtr->actor.vel = platform->link->vel;
                    ADD_TO_VECTOR(&platPtr->actor.vel, &platPtr->link->velOffset);
                    SCALE_VECTOR(&platPtr->actor.vel, platform->motion->rotVel[i] * platform->link->spinToMoveScale);
                }
            }
            if (platform->link->spinToSpinScale != 0.0f) {
                for (platPtr = FindTaggedPlatform(&platformList.head, platform->link->spinToSpinTag); platPtr != NULL;
                     platPtr = FindTaggedPlatform(platPtr, platform->link->spinToSpinTag)) {
                    platPtr->motion->speed = platform->motion->rotVel[i] * platform->link->spinToSpinScale;
                }
            }
        }
    }
    if (platform->flags & 0x2000) {
        t = platform->path->target;
        if (t != platform->path->current && platform->maxSpeed != 0.0f) {
            SUB_VECTORS(&tempVect, &platform->actor.pos, &platform->path->points[t]->pos);
            platform->motion->targetRot[1] = Aabs(atan2(tempVect.x, tempVect.z));
            platform->motion->targetRot[0] = Aabs(atan2(-tempVect.y, LENGTH_2D(&tempVect)));
            /* The bank follows the turn per original frame. oldRot is the angle at the start of this update. */
            d = DeltaPerFrame(FindShortestAngleSigned(platform->motion->rot.y, platform->motion->oldRot.y));
            if (ABS(d) >= 0.018) {
                platform->motion->targetRot[2] = Aabs(d * platform->motion->bankScale);
            } else {
                platform->motion->targetRot[2] = 0.0f;
            }
        } else {
            platform->motion->targetRot[1] = platform->motion->rot.y;
            platform->motion->targetRot[2] = 0.0f;
        }
    }
    if (lastVel == 0.0f) {
        if (platform->motion->sample.handle != 0) {
            UpdateContinuousSample(&platform->motion->sample);
            /* The volume is an integer: the fade steps once per original frame. */
            if (deltaTick) {
                platform->motion->sample.volume -= 60;
            }
            if (platform->motion->sample.volume <= 0) {
                StopContinuousSample(&platform->motion->sample);
                if (platform->motion->stopSampleId) {
                    PlaySample(platform->motion->stopSampleId, &platform->actor.pos, platform->motion->stopSampleVolume,
                               platform->motion->stopSamplePitch);
                }
            }
        }
    } else if (platform->motion->sample.id != 0) {
        if (platform->motion->sample.handle == 0) {
            PlayContinuousSample(&platform->motion->sample, platform->motion->sample.id,
                                 platform->motion->sample.baseVolume, &platform->actor.pos, platform->motion->sample.pitch);
        } else {
            UpdateContinuousSample(&platform->motion->sample);
        }
    }
}

void UpdatePlatform(Platform *platform) {
    Vec3 nextPoint;
    Vec3 d;
    f32 dist;

    if (platform->sample.id) {
        /* d is the platform's step over the latest update. The sound plays above a speed of 0.5 per original
           frame. */
        SUB_VECTORS(&d, &platform->actor.pos, &platform->actor.prevPos);
        dist = DOT_PRODUCT(&d, &d);
        if ((!platform->crumble || platform->crumble->timer > platform->crumble->fallTime) &&
            (dist > 0.25 * deltaTime * deltaTime || (dist > 0.0f && !platform->push))) {
            if (!platform->sample.handle) {
                if (platform->actor.alpha) {
                    PlayContinuousSample(&platform->sample, platform->sample.id, platform->sample.baseVolume,
                                         &platform->actor.pos, platform->sample.pitch);
                }
            } else {
                platform->sample.volume = platform->sample.baseVolume;
                UpdateContinuousSample(&platform->sample);
            }
        } else if (platform->sample.handle) {
            /* The volume is an integer: the fade steps once per original frame. */
            if (deltaTick) {
                platform->sample.volume -= 60;
            }
            if (platform->sample.volume <= 0) {
                StopContinuousSample(&platform->sample);
            } else {
                UpdateContinuousSample(&platform->sample);
            }
        }
    }
    if (platform->actor.sparkleRate && platform->lastUpdate < frameCount) {
        RecalculatePlatformInfo(platform);
    }
    if (platform->exitType) {
        CheckForExit(platform);
    }
    if (platform->actor.flags & 0x200000) {
        FloatPlatform(platform);
    }
    UpdateFan(platform);
    CrumblePlatform(platform);
    ScalePlatform(platform);
    PlatformInherit(platform, &nextPoint);
    MovePlatform(platform, &nextPoint);
    SpinPlatform(platform);
    OrbitPlatform(platform);
    ConfinePlatform(platform);
    RotatePlatform(platform);
    CheckRestartGate(platform);
}

void FloatPlatform(Platform *platform) {
    Water *ptr;

    for (ptr = waterList.head.next; ptr != &waterList.head; ptr = ptr->next) {
        if (IsPointWithinBox(&ptr->box, &platform->actor.pos)) {
            platform->actor.vel.y -= gravity * deltaTime;
            WaterDoFloating(ptr, &platform->actor, 0);
            platform->actor.speed =
                LENGTH(&platform->actor.vel);
            if (platform->actor.speed > platform->maxSpeed) {
                SCALE_VECTOR(&platform->actor.vel, platform->maxSpeed / platform->actor.speed);
                platform->actor.speed = platform->maxSpeed;
            }
        }
    }
}

void InitPlatformInfo(Platform *platform, f32 offsetY) {
    CollFace face;
    Mesh *mesh;
    f32 d;
    s32 i;
    s32 j;

    if (platform->flags & 0x84) {
        return;
    }
    CalculatePlatformInfo(platform, offsetY);
    mesh = platform->actor.inst->obj->mesh;
    if (platform->scale != NULL) {
        *(Vec3 *)platform->scale->min = platform->actor.scale;
    }
    for (i = 0; i < mesh->numFaces; i++) {
        for (j = 0; j < 3; j++) {
            face.edgeStart[j].x = mesh->verts[mesh->faces[i][j]].x;
            face.edgeStart[j].y = mesh->verts[mesh->faces[i][j]].y;
            face.edgeStart[j].z = mesh->verts[mesh->faces[i][j]].z;
            face.edgeStart[j].x *= platform->actor.scale.x;
            face.edgeStart[j].y *= platform->actor.scale.y;
            face.edgeStart[j].z *= platform->actor.scale.z;
            d = LENGTH(&face.edgeStart[j]);
            if (platform->actor.radius < d) {
                platform->actor.radius = d;
            }
            if (platform->extent.x < ABS(face.edgeStart[j].x)) {
                platform->extent.x = ABS(face.edgeStart[j].x);
            }
            if (platform->extent.y < ABS(face.edgeStart[j].y)) {
                platform->extent.y = ABS(face.edgeStart[j].y);
            }
            if (platform->extent.z < ABS(face.edgeStart[j].z)) {
                platform->extent.z = ABS(face.edgeStart[j].z);
            }
        }
    }
    for (i = 0; i < mesh->numFaces; i++) {
        CheckForInnerEdges(mesh->numFaces, platform->actor.faces, i);
    }
}

void CalculatePlatformInfo(Platform *platform, f32 offsetY) {
    s16 *idx;
    Mesh *mesh = platform->actor.inst->obj->mesh;
    Vec3 e1;
    Vec3 e2;
    Matrix matrix;
    CollFace *plane;
    f32 x;
    f32 y;
    f32 z;
    s32 i;
    s32 j;

    if (platform->flags & 0x84) {
        return;
    }
    platform->lastUpdate = frameCount;
    QuaternionToMatrix(&platform->actor.rotQuat, &matrix);
    for (i = 0; i < mesh->numFaces; i++) {
        plane = &platform->actor.faces[i];
        for (j = 0; j < 3; j++) {
            // Integer arithmetic with the offset first gives the ROM operand order.
            idx = PORT_W2P(s16, i * sizeof(*mesh->faces) + (u32)mesh->faces);
            x = platform->actor.scale.x * mesh->verts[idx[j]].x;
            y = platform->actor.scale.y * mesh->verts[mesh->faces[i][j]].y + offsetY;
            z = platform->actor.scale.z * mesh->verts[mesh->faces[i][j]].z;
            plane->edgeStart[j].x = matrix.m[0][0] * x + matrix.m[1][0] * y + matrix.m[2][0] * z;
            plane->edgeStart[j].y = matrix.m[0][1] * x + matrix.m[1][1] * y + matrix.m[2][1] * z;
            plane->edgeStart[j].z = matrix.m[0][2] * x + matrix.m[1][2] * y + matrix.m[2][2] * z;
        }
        SUB_VECTORS(&e1, &plane->edgeStart[1], &plane->edgeStart[0]);
        SUB_VECTORS(&e2, &plane->edgeStart[2], &plane->edgeStart[0]);
        CrossProduct((Vec3 *)&plane->plane, &e1, &e2);
        MakeUnit((Vec3 *)&plane->plane);
        for (j = 0; j < 3; j++) {
            ADD_TO_VECTOR(&plane->edgeStart[j], &platform->actor.pos);
        }
        CalculatePlaneInfoWithEdgeLens(plane);
    }
}

void RecalculatePlatformInfo(Platform *platform) {
    s16 *idx;
    Mesh *mesh = platform->actor.inst->obj->mesh;
    Vec3 e1;
    Vec3 e2;
    Matrix matrix;
    CollFace *plane;
    f32 x;
    f32 y;
    f32 z;
    s32 i;
    s32 j;

    if (platform->flags & 0x20084) {
        return;
    }
    platform->lastUpdate = frameCount;
    if (platform->motion == NULL && platform->actor.rotQuat.w == 1.0f) {
        for (i = 0; i < mesh->numFaces; i++) {
            plane = &platform->actor.faces[i];
            for (j = 0; j < 3; j++) {
                plane->edgeStart[j].x = mesh->verts[mesh->faces[i][j]].x;
                plane->edgeStart[j].y = mesh->verts[mesh->faces[i][j]].y;
                plane->edgeStart[j].z = mesh->verts[mesh->faces[i][j]].z;
                plane->edgeStart[j].x *= platform->actor.scale.x;
                plane->edgeStart[j].y *= platform->actor.scale.y;
                plane->edgeStart[j].z *= platform->actor.scale.z;
                plane->edgeStart[j].x += platform->actor.pos.x;
                plane->edgeStart[j].y += platform->actor.pos.y;
                plane->edgeStart[j].z += platform->actor.pos.z;
            }
            plane->plane.d = -DOT_PRODUCT(&plane->edgeStart[0], &plane->plane);
        }
    } else {
        QuaternionToMatrix(&platform->actor.rotQuat, &matrix);
        for (i = 0; i < mesh->numFaces; i++) {
            plane = &platform->actor.faces[i];
            for (j = 0; j < 3; j++) {
                // Integer arithmetic with the offset first gives the ROM operand order.
                idx = PORT_W2P(s16, i * sizeof(*mesh->faces) + (u32)mesh->faces);
                x = platform->actor.scale.x * mesh->verts[idx[j]].x;
                y = platform->actor.scale.y * mesh->verts[mesh->faces[i][j]].y;
                z = platform->actor.scale.z * mesh->verts[mesh->faces[i][j]].z;
                plane->edgeStart[j].x = matrix.m[0][0] * x + matrix.m[1][0] * y + matrix.m[2][0] * z;
                plane->edgeStart[j].y = matrix.m[0][1] * x + matrix.m[1][1] * y + matrix.m[2][1] * z;
                plane->edgeStart[j].z = matrix.m[0][2] * x + matrix.m[1][2] * y + matrix.m[2][2] * z;
            }
            SUB_VECTORS(&e1, &plane->edgeStart[1], &plane->edgeStart[0]);
            SUB_VECTORS(&e2, &plane->edgeStart[2], &plane->edgeStart[0]);
            CrossProduct((Vec3 *)&plane->plane, &e1, &e2);
            MakeUnit((Vec3 *)&plane->plane);
            for (j = 0; j < 3; j++) {
                plane->edgeStart[j].x += platform->actor.pos.x;
                plane->edgeStart[j].y += platform->actor.pos.y;
                plane->edgeStart[j].z += platform->actor.pos.z;
            }
            CalculatePlaneInfo(plane);
        }
    }
}

Platform *TagToFirstPlatform(s32 tag) {
    Platform *ptr;

    for (ptr = platformList.head.next; ptr != &platformList.head; ptr = ptr->next) {
        if (ptr->tag == tag) {
            return ptr;
        }
    }
    return NULL;
}

Platform *FindTaggedPlatform(Platform *startPlatform, s32 tag) {
    Platform *platformPtr;

    for (platformPtr = startPlatform->next; platformPtr != &platformList.head; platformPtr = platformPtr->next) {
        if (platformPtr->tag == tag) {
            return platformPtr;
        }
    }
    return NULL;
}

void UpdateRopeBridges(void) {
    Vec3 rotvel[100];
    Vec3 angle;
    Platform *platform1;
    Platform *platform2;
    s32 x;
    s32 j;
    s32 k;

    for (x = 0; x < 10; x++) {
        if (ropeBridge[x].stiffness == 0.0f) {
            break;
        }
        for (j = ropeBridge[x].first; j <= ropeBridge[x].last; j++) {
            rotvel[j - ropeBridge[x].first] = zero;
        }
        /* The springs between the planks add to their turn speeds: an acceleration per original frame. */
        for (j = ropeBridge[x].first; j <= ropeBridge[x].last; j++) {
            platform1 = TagToFirstPlatform(j);
            if (j > ropeBridge[x].first) {
                platform2 = TagToFirstPlatform(j - 1);
                for (k = 0; k < 3; k++) {
                    (&angle.x)[k] = FindShortestAngleSigned((&platform1->motion->rot.x)[k], (&platform2->motion->rot.x)[k]);
                }
                AddOneScaledVector(&rotvel[j - 1 - ropeBridge[x].first], &rotvel[j - 1 - ropeBridge[x].first], &angle,
                                   ropeBridge[x].stiffness * deltaTime);
            }
            if (j < ropeBridge[x].last) {
                platform2 = TagToFirstPlatform(j + 1);
                for (k = 0; k < 3; k++) {
                    (&angle.x)[k] = FindShortestAngleSigned((&platform1->motion->rot.x)[k], (&platform2->motion->rot.x)[k]);
                }
                AddOneScaledVector(&rotvel[j + 1 - ropeBridge[x].first], &rotvel[j + 1 - ropeBridge[x].first], &angle,
                                   ropeBridge[x].stiffness * deltaTime);
            }
        }
        if (ropeBridge[x].dampEnds == 1) {
            SCALE_VECTOR(&rotvel[0], 0.075);
        }
        platform1 = TagToFirstPlatform(ropeBridge[x].first);
        ADD_TO_VECTOR((Vec3 *)platform1->motion->rotVel, &rotvel[0]);
        if (ropeBridge[x].dampEnds == 1) {
            SCALE_VECTOR(&rotvel[ropeBridge[x].last - ropeBridge[x].first], 0.075);
        }
        platform1 = TagToFirstPlatform(ropeBridge[x].last);
        ADD_TO_VECTOR((Vec3 *)platform1->motion->rotVel, &rotvel[ropeBridge[x].last - ropeBridge[x].first]);
        for (j = ropeBridge[x].first + 1; j <= ropeBridge[x].last - 1; j++) {
            platform1 = TagToFirstPlatform(j);
            if (platform1->motion->idleCount >= 2) {
                ADD_TO_VECTOR((Vec3 *)platform1->motion->rotVel, &rotvel[j - ropeBridge[x].first]);
            }
        }
    }
}

void CheckRestartGate(Platform *platform) {
    PlatformRestartGate *restartGate = platform->restartGate;
    Vec3 tempVect;
    u8 i;

    if (restartGate) {
        platform->actor.scale.x = platform->actor.scale.y = platform->actor.scale.z =
            platform->actor.scale.x - (platform->actor.scale.x - restartGate->targetScale) * DeltaLerp(0.125f);
        if (restartGate->untouched && DistanceBetweenPoints2D(&ball.pos, &platform->actor.pos) < platform->actor.radius &&
            ball.pos.y <= platform->actor.pos.y && platform->actor.pos.y < ball.prevPos.y) {
            tempVect.x = 8.0f;
            tempVect.y = 0.0f;
            tempVect.z = 0.0f;
            RotateVector2D(&tempVect, &tempVect, platform->motion->rot.y);
            for (i = 0; i < 6; i++) {
                RotateVector2D(&tempVect, &tempVect, 1.0f);
                AddPuff(&platform->actor.pos, &tempVect, 2.0f);
            }
            restartGate->untouched = 0;
            restartGate->targetScale = 0.0f;
            handControl.checkpoint = platform;
            restartGate->cameraPos = camera.pos;
            PlaySample(280, &ball.pos, 100, 128);
        }
    }
}

/* The frame windows in which RestartPlayer last ran its set-up, the glove's jump, the ball's launch and the end of
   the restart (DeltaFirst). */
static u32 restartSetUpFrame;
static u32 restartJumpFrame;
static u32 restartLaunchFrame;
static u32 restartEndFrame;
/* Port-side: how many restarts RestartPlayer has begun (the library's GloverBodies.restarts). Outside the game's
   memory. */
u32 portRestarts;

void RestartPlayer(void) {
    Vec3 tempVect;
    PlatformRestartGate *restartGate;
    DeltaSaved saved;
    s32 step;
    s32 j;
    f32 size;
    f64 scale;
    f64 d;

    if (gameInfo.mode != 1 && handControl.checkpoint != NULL) {
        restartGate = handControl.checkpoint->restartGate;
        /* restartTimer counts original frames. Each step that a value of it starts runs once, in the first update
           that sees the value. */
        if (restartGate->restartTimer == 65 && DeltaFirst(&restartSetUpFrame)) {
            portRestarts++;
            if (gameInfo.world != HUB && gameInfo.level != WAYROOM) {
                player.hurtTimer = 70;
            }
            cheatActivated &= ~0x20;
            SetControlToHand();
            if (handControl.samples[0].handle != 0) {
                StopContinuousSample(&handControl.samples[0]);
            }
            ApplyAction(HAND_IDLE);
            hand.obj->spheres[0].radius = hand.obj->spheres[0].targetRadius = 13.0f;
            cameraInfo.focus = &handControl.checkpoint->actor.pos;
            cameraInfo.actor.pos = *cameraInfo.focus;
            cameraInfo.focusVel = &hand.vel;
            cameraInfo.overrideTimer = 0;
            if (controlMethod == CONTROL_CAMERA) {
                DeactivateCameraControlMode();
            }
            ballControl.hitFlags = 0;
            ballControl.loseTimer = 0;
            handControl.deathTimer = 0;
            hand.link = NULL;
            if (gameInfo.level != PREHISTORICBOSS && gameInfo.level != FEARBOSS && gameInfo.level != SPACEBOSS1 &&
                gameInfo.level != SPACEBOSS3 && gameInfo.level != ATLANTISBOSS && gameInfo.level != PIRATESBOSS &&
                gameInfo.level != CARNIVALBOSS) {
                for (j = 0; j < numberOfEnemies; j++) {
                    RestartEnemy(&enemies[j], enemies[j].brain.startPos.x, enemies[j].brain.startPos.y,
                                 enemies[j].brain.startPos.z, enemies[j].brain.tag, enemies[j].brain.startRot,
                                 enemies[j].actor.type);
                }
            }
            player.castSpellType = 15;
            player.castSpell = 15;
            cameraInfo.fallTime = 0;
            if (gameInfo.level == SPACEBOSS3) {
                hand.pos = handControl.checkpoint->actor.pos;
                restartGate->restartTimer = 0;
                hand.rot.y = hand.rotAim.y = Aabs(handControl.checkpoint->restartGate->yaw + 3.141592654);
                hand.scale.x = 0.5f;
                hand.scale.y = 0.5f;
                hand.scale.z = 0.5f;
                handControl.checkpoint->actor.alpha = 0;
                /* Each pass is one original frame of the camera. */
                for (j = 0; j < 30; j++) {
                    DeltaEnterFixed(&saved);
                    UpdateActor(&camera);
                    UpdateActorPositionDependencies();
                    MoveCam();
                    DeltaLeaveFixed(&saved);
                }
                return;
            }
            RemoveHandAndBallBullets();
            for (j = 0; j < 3; j++) {
                if (handControl.spellTimer[j] != 0) {
                    DeactivateSpellHand(j);
                    handControl.spellTimer[j] = 0;
                }
            }
            if (ballWithHand) {
                LoseBall(10);
                ballControl.loseTimer = 0;
            }
            handControl.morphStep = 0;
            if (ballControl.cancerTimer != 0) {
                ballControl.cancerTimer = 1;
            }
            hand.flags &= ~0x4000;
            if (ballWithHand) {
                ZERO_VECTOR(&ball.scale);
            }
            ballControl.state = BALL_NORMAL;
            ZERO_VECTOR(&hand.scale);
            if (ballWithHand) {
                ball.obj->spheres[0].radius = ball.obj->spheres[0].targetRadius = 0.0f;
                ball.radius = 0.0f;
                ball.pos = handControl.checkpoint->actor.pos;
                handControl.noControlTimer = 75;
                ball.pos.y += 100.0f;
            } else {
                handControl.noControlTimer = 45;
            }
            hand.pos = handControl.checkpoint->actor.pos;
            ballControl.releaseTimer = 0;
            handControl.releaseTimer = 0;
            hand.pos.y -= levelHandScale * 12.0 / 0.05;
            DisableControl(65);
            camera.pos = restartGate->cameraPos;
            cameraInfo.actor.pos = hand.pos;
            ZERO_VECTOR(&camera.vel);
            cameraInfo.sideOffsetVel = 0.0f;
            cameraInfo.sideOffset = 0.0f;
            cameraInfo.actor.pos.y += cameraInfo.distance / 11.0f;
            restartGate->targetScale = levelHandScale / 0.05 * 0.8;
            hand.rot.y = hand.rotAim.y = restartGate->yaw;
            for (j = 0; j < 20; j++) {
                DeltaEnterFixed(&saved);
                UpdateActor(&camera);
                UpdateActorPositionDependencies();
                MoveCam();
                DeltaLeaveFixed(&saved);
            }
            ballControl.fallTime = 0;
            handControl.fallTime = 0;
            ResetHand(0);
        } else if (gameInfo.level == SPACEBOSS3) {
            return;
        }
        if (restartGate->restartTimer > 0) {
            /* restartTimer steps in ticks. Its last step ends the restart and resets the glove's sphere. On the N64
               that step comes before the frame's physics: it runs in the first update of the frame window. */
            step = restartGate->restartTimer == 1 ? DeltaFirst(&restartEndFrame) : deltaTick;
            ballControl.releaseTimer = 0;
            handControl.releaseTimer = 0;
            tempVect.x = 8.0f;
            tempVect.y = 0.0f;
            tempVect.z = 0.0f;
            RotateVector2D(&tempVect, &tempVect, handControl.checkpoint->motion->rot.y);
            if (restartGate->restartTimer == 35 || (restartGate->restartTimer == 10 && ballWithHand)) {
                size = 2.0f;
            } else if (restartGate->restartTimer == 36 || (restartGate->restartTimer == 11 && ballWithHand)) {
                size = 1.0f;
            } else {
                size = 0.5f;
            }
            if (step && (size >= 1.0f || (frameCount & 1))) {
                for (j = 0; j < 6; j++) {
                    RotateVector2D(&tempVect, &tempVect, 1.0f);
                    AddPuff(&handControl.checkpoint->actor.pos, &tempVect, size);
                }
            }
            if (restartGate->restartTimer == 35 && DeltaFirst(&restartJumpFrame)) {
                if (!ballWithHand && (gameInfo.cameoActive == 0 || gameInfo.level == ATLANTISBONUS)) {
                    cameraInfo.focus = &hand.pos;
                    cameraInfo.focusVel = &hand.vel;
                    cameraInfo.followSpeed = 5.0f;
                }
                ApplyAction(HAND_JUMP);
                hand.vel.y = gravity * 5.0f + 3.0f;
                hand.rotVel.z = 1.257f;
                hand.rot.z = hand.rotAim.z = 0.0f;
            }
            if (restartGate->restartTimer < 36) {
                for (j = 0; j < 15; j++) {
                    DeactivateCollectable(j);
                }
                d = 0.05; // a double variable keeps 0.05 in one register for both uses
                scale = levelHandScale / d;
                tempVect.x = tempVect.y = tempVect.z = d;
                hand.scale = tempVect;
                size = 9.0f;
                SCALE_VECTOR(&hand.scale, scale);
                if (ballWithHand) {
                    if (restartGate->restartTimer == 10 && DeltaFirst(&restartLaunchFrame)) {
                        ResetBall();
                        ball.pos = handControl.checkpoint->actor.pos;
                        GiveActorRandomQRotVel(&ball);
                        ball.spinSpeed = 1.2f;
                        ball.vel.y = gravity * 6.0f + 2.0f;
                        PlaySample(93, &ball.pos, 100, 100);
                        if (gameInfo.cameoActive == 0) {
                            cameraInfo.focus = &ball.pos;
                            cameraInfo.focusVel = &ball.vel;
                            cameraInfo.followSpeed = 5.0f;
                        }
                    }
                    if (restartGate->restartTimer < 11) {
                        ball.obj->spheres[0].radius = ball.obj->spheres[0].targetRadius = size * levelHandScale / 0.05;
                    } else {
                        ball.pos = handControl.checkpoint->actor.pos;
                        ball.pos.y += 100.0f;
                        ball.pos.x += 100.0f;
                        ball.vel = zero;
                        ball.spinSpeed = 0.0f;
                    }
                }
            } else {
                handControl.actionLock = 2;
                if (ballWithHand) {
                    ball.obj->spheres[0].radius = ball.obj->spheres[0].targetRadius = 0.0f;
                    ball.pos = handControl.checkpoint->actor.pos;
                    ZERO_VECTOR(&ball.vel);
                    ball.pos.y += 100.0f;
                    ball.pos.x += 100.0f;
                }
                ZERO_VECTOR(&hand.scale);
                hand.pos.y = handControl.checkpoint->actor.pos.y - levelHandScale * 12.0 / 0.05;
                hand.vel = zero;
            }
            if (step && --restartGate->restartTimer == 0) {
                if (gameInfo.cameoActive == 0) {
                    cameraInfo.focus = &hand.pos;
                }
                if (ballWithHand) {
                    ball.obj->spheres[0].radius = ball.obj->spheres[0].targetRadius = levelHandScale * 9.0 / 0.05;
                }
                restartGate->targetScale = 0.0f;
                restartGate->untouched = 0;
                hand.scale.x = hand.scale.y = hand.scale.z = 0.05f;
                SCALE_VECTOR(&hand.scale, levelHandScale / 0.05);
                hand.obj->spheres[0].radius = hand.obj->spheres[0].targetRadius = 13.0f;
                hand.shadow->shadowSize = hand.obj->spheres[0].radius * D_8010894C;
                ResetHand(0);
            }
            if (restartGate->restartTimer == 10) {
                restartGate->targetScale = 0.0f;
            }
        }
    }
}

INCLUDE_RODATA("asm/nonmatchings/platform", D_8010894C);

PORT_C_END
