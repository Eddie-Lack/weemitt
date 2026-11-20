PORT_C_BEGIN
#include "common.h"
#include "collision.h"
#include "ident.h"
#include "cameo.h"
#include "jalloc.h"
#include "game.h"
#include "actor.h"
#include "hand.h"
#include "camera.h"
#include "weather.h"
#include "debris.h"
#include "maths.h"
#include "sound.h"
#include "libc.h"
#include "controller.h"
#include "delta.h"

void DisableControl(s16 time);

extern s32 controlMethod;

extern PTR32(Vec3) cameoCamPos;
extern PTR32(Vec3) cameoCamVel;
extern Vec3 camTarget;
extern Vec3 zero;
extern s16 worldExitTimer;
void AnimateActor(Actor *actor, s32 animNum, s32 loop, s32 queue, f32 speed);

/* The share of an original frame that an item's motion covers in this update. An item first runs in a tick (time
   1), and the updates of its first frame before that tick did not move it. That tick moves it by a whole frame. */
static f32 CameoShare(CameoItem *instruction) {
    return instruction->time == 1 ? 1.0f : deltaTime;
}

/* Pushes the camera sideways by a ROTATE_ACTOR item's acceleration per original frame, scaled to this update. */
static void PushCameoCamera(CameoItem *instruction) {
    Vec3 tempVect;
    f32 share = CameoShare(instruction);

    tempVect.x = __sinf(camera.rot.y - 1.570796327) * instruction->u.pos.x * share;
    tempVect.y = 0.0f;
    tempVect.z = __cosf(camera.rot.y - 1.570796327) * instruction->u.pos.x * share;
    ADD_TO_VECTOR(&camera.vel, &tempVect);
}

/* Moves the point of a MOVE_ACTOR item towards u.pos at the item's speed per original frame, scaled to this update.
   Then it sets what follows the point: the camera's target and rotation and the enemy's shadow. Returns TRUE when
   the point has arrived. */
static s32 MoveCameoPoint(CameoItem *instruction, Vec3 *pos) {
    Vec3 tempVect;
    Actor *actor;
    Platform *platform;
    s32 arrived;
    f32 dist;
    f32 step;

    if (ABS(instruction->u.pos.x - 0.1) > 0.05) {
        SUB_VECTORS(&tempVect, &instruction->u.pos, pos);
    } else {
        tempVect.x = tempVect.z = 0.0f;
        tempVect.y = instruction->u.pos.y - pos->y;
    }
    dist = LENGTH(&tempVect);
    step = instruction->speed * CameoShare(instruction);
    arrived = dist <= step;
    if (arrived) {
        if (ABS(instruction->u.pos.x - 0.1) > 0.05) {
            *pos = instruction->u.pos;
        } else {
            pos->y = instruction->u.pos.y;
        }
    } else {
        AddToOneScaledVector(pos, &tempVect, step / dist);
    }
    if (instruction->target == -2) {
        SetVector2D(&cameraInfo.actor.pos, &camTarget);
        cameraInfo.actor.pos.y = cameraInfo.targetY = cameraInfo.focus->y + cameraInfo.targetOffsetY;
        if (gameInfo.level == SPACEBOSS3) {
            cameraInfo.actor.pos = camTarget;
        }
    }
    CalcCameraRot();
    if (instruction->target > 0) {
        actor = &enemies[instruction->target - 1].actor;
        if (actor->flags & 0x100) {
            actor->shadow->altitude = Altitude(&actor->pos, PORT_OUT(Vec3, &actor->shadow->floor), &platform, actor->obj);
        }
    }
    return arrived;
}

/* The point that a MOVE_ACTOR item moves, as ProcessCameoInstruction picks it, without the writes it makes there.
   NULL for an item that moves nothing. */
static Vec3 *CameoMovePoint(CameoItem *instruction) {
    switch (instruction->target) {
        case -7:
            return &enemies[1].actor.pos;
        case -6:
            return NULL;
        case -5:
        case -4:
            return &handControl.orbiter3.pos;
        case -2:
            return &camTarget;
        case -1:
            return &camera.pos;
        case 0:
            return &hand.pos;
        default:
            return &enemies[instruction->target - 1].actor.pos;
    }
}

/* The point that a TIE_ACTOR_TO_SPHERE item holds on a sphere, as ProcessCameoInstruction picks it. */
static Vec3 *CameoTiePoint(CameoItem *instruction) {
    switch (instruction->target) {
        case -2:
            return &camTarget;
        case -1:
            return &camera.pos;
        case 0:
            return &hand.pos;
        default:
            return &enemies[instruction->target - 1].actor.pos;
    }
}

/* The motion of a running cameo between ticks. An item takes part once a tick has run it (time 2 or more): the
   tick sets up its camera target first. Nothing here changes an item's time or done flag. */
static void MoveCameo(Cameo *cameo) {
    CameoItem *instruction;
    Actor *actor;
    Vec3 *pos;
    s32 k;

    for (k = 0; k < cameo->count; k++) {
        instruction = cameo->items[k];
        if (instruction->time < 2) {
            continue;
        }
        switch (instruction->type) {
            case CAMEO_INSTRUCTION_TARGET_CAMERA:
                if (instruction->target > 0 && instruction->u.pos.x != 0.0f) {
                    cameraInfo.actor.pos = *cameraInfo.focus;
                }
                break;
            case CAMEO_INSTRUCTION_ROTATE_ACTOR:
                if (instruction->target == -1) {
                    PushCameoCamera(instruction);
                }
                break;
            case CAMEO_INSTRUCTION_MOVE_ACTOR:
                pos = CameoMovePoint(instruction);
                if (pos != NULL) {
                    MoveCameoPoint(instruction, pos);
                }
                break;
            case CAMEO_INSTRUCTION_TIE_ACTOR_TO_SPHERE:
                if (instruction->u.args[0] == 0) {
                    actor = &hand;
                } else {
                    actor = &enemies[instruction->u.args[0] - 1].actor;
                }
                pos = CameoTiePoint(instruction);
                COPY_VECTOR(pos, &actor->obj->spheres[instruction->u.args[1]].pos);
                break;
        }
    }
}

void InitCameoList(void) {
    cameoList.head.next = cameoList.head.prev = &cameoList.head;
    cameoList.count = 0;
}

void AddCameo(Cameo *cameo) {
    if (cameo->next == NULL) {
        cameo->next = &cameoList.head;
        cameo->prev = cameoList.head.prev;
        cameo->prev->next = cameo;
        cameo->next->prev = cameo;
        cameoList.count++;
    }
}

void StartCameo(s32 num) {
    Cameo *cameo;
    s32 j;

    if (gameInfo.level == HUB1 && (gameInfo.prevLevel == 8 || gameInfo.prevLevel == 9)) {
        return;
    }
    if (!controlMethod) {
        DeactivateCameraControlMode();
    }
    cameo = cameoList.head.next;
    for (j = 0; j < num; j++) {
        cameo = cameo->next;
    }
    cameo->active = TRUE;
    for (j = 0; j < cameo->count; j++) {
        cameo->items[j]->time = 0;
        if (cameo->items[j]->trigger == -1) {
            cameo->items[j]->time = 1;
        }
    }
}

void UpdateCameos(void) {
    Cameo *cameo;
    s32 num = 0;

    /* A cameo's script steps once per original frame, in ticks: the item times, the triggers, the waits, the spawns,
       the sounds, the random choices and cameoActive. Between ticks only what the items move runs. */
    if (!deltaTick) {
        for (cameo = cameoList.head.next; cameo != &cameoList.head; cameo = cameo->next) {
            if (cameo->active) {
                MoveCameo(cameo);
            }
        }
        return;
    }
    for (cameo = cameoList.head.next; cameo != &cameoList.head; cameo = cameo->next) {
        if (cameo->active) {
            UpdateCameo(cameo);
            num++;
        }
    }
    if (num) {
        gameInfo.cameoActive = TRUE;
    } else {
        gameInfo.cameoActive = FALSE;
    }
}

void UpdateCameo(Cameo *cameo) {
    s32 k;
    s32 j;
    s32 num = 0;

    for (k = 0; k < cameo->count; k++) {
        cameo->items[k]->done = 0;
        if (cameo->items[k]->time) {
            ProcessCameoInstruction(cameo, cameo->items[k]);
            if (!cameo->items[k]->done) {
                num++;
            } else if (gameInfo.level != SPACEBOSS3) {
                for (j = 0; j < cameo->count; j++) {
                    if ((cameo->items[j]->trigger < -1 && cameo->items[j]->trigger == -cameo->active) ||
                        (cameo->items[j]->trigger >= 0 && cameo->items[cameo->items[j]->trigger]->done)) {
                        num++;
                        cameo->items[j]->time = 1;
                    }
                }
            }
        }
    }
    for (j = 0; j < cameo->count; j++) {
        if ((cameo->items[j]->trigger < -1 && cameo->items[j]->trigger == -cameo->active) ||
            (cameo->items[j]->trigger >= 0 && cameo->items[cameo->items[j]->trigger]->done)) {
            num++;
            cameo->items[j]->time = 1;
        }
    }
    if (num == 0) {
        cameo->active = 0;
    } else {
        cameo->active++;
        DisableControl(2);
    }
}

void ProcessCameoInstruction(Cameo *cameo, CameoItem *instruction) {
    Vec3 tempVect;
    Actor *actor;
    Vec3 *pos;
    Debris *d;
    s32 x;

    switch (instruction->type) {
        case CAMEO_INSTRUCTION_LIGHTNING:
            rain.lightning = instruction->u.args[0] + Random(instruction->u.args[1]);
            PlaySampleNot3D(248, 200, 128, 128);
            break;
        case CAMEO_INSTRUCTION_ANIMATION:
            if (instruction->target == 0) {
                actor = &hand;
                handControl.actionLock = 2;
            } else {
                actor = &enemies[instruction->target - 1].actor;
            }
            if (instruction->time == 1) {
                actor->anim->loop = 0;
                AnimateActor(actor, instruction->u.args[0], (u8)instruction->u.args[1], 0, instruction->speed);
                if (instruction->duration == 0) {
                    instruction->done = 1;
                }
            } else if (instruction->duration == -1 && actor->anim->finishedAnim != 0) {
                instruction->done = 1;
            }
            break;
        case CAMEO_INSTRUCTION_TARGET_CAMERA:
            cameraInfo.sideOffsetEnabled = 0;
            switch (instruction->target) {
                case -1:
                    cameoCamPos = cameraInfo.focus;
                    cameoCamVel = cameraInfo.focusVel;
                    cameraInfo.focus = &instruction->u.pos;
                    cameraInfo.focusVel = &zero;
                    break;
                case 0:
                    cameraInfo.focus = &hand.pos;
                    cameraInfo.focusVel = &hand.vel;
                    break;
                default:
                    cameraInfo.focus = &enemies[instruction->target - 1].actor.pos;
                    cameraInfo.focusVel = &enemies[instruction->target - 1].actor.vel;
                    if (instruction->u.pos.x != 0.0f) {
                        cameraInfo.actor.pos = *cameraInfo.focus;
                    }
                    break;
            }
            break;
        case CAMEO_INSTRUCTION_ROTATE_ACTOR:
            if (instruction->target == -1) {
                PushCameoCamera(instruction);
            } else {
                if (instruction->target == 0) {
                    actor = &hand;
                } else {
                    actor = &enemies[instruction->target - 1].actor;
                }
                if (instruction->u.pos.x >= 0.0f) {
                    actor->rotAim.y = instruction->u.pos.x;
                } else {
                    actor->rotAim.x = -instruction->u.pos.x;
                }
                if (instruction->duration == -1 && actor->rot.y == actor->rotAim.y) {
                    instruction->done = 1;
                }
            }
            break;
        case CAMEO_INSTRUCTION_DISABLE_ACTOR:
            enemies[instruction->target - 1].brain.flags |= 0x100;
            break;
        case CAMEO_INSTRUCTION_MOVE_ACTOR:
            switch (instruction->target) {
                case -7:
                    pos = &enemies[1].actor.pos;
                    AddDebris(POPB, &instruction->u.pos, &zero, NULL, NULL);
                    CreateStarCircle(&instruction->u.pos, 5, 19);
                    for (x = 0; x < 10; x++) {
                        tempVect.x = Random(11) - 5;
                        tempVect.z = Random(11) - 5;
                        tempVect.y = Random(6) + 3;
                        d = AddDebris(WIND_BIT, &instruction->u.pos, &tempVect, NULL, NULL);
                        if (d != NULL) {
                            d->lifeTimer = 5;
                            d->info->friction = 0.6f;
                            d->friction = 0.6f;
                        }
                    }
                    break;
                case -6:
                    worldExitTimer = 1;
                    gameInfo.exitPlatform = NULL;
                    return;
                case -5:
                    enemies[(s32)instruction->u.pos.x - 1].actor.flags &= ~0x11;
                    handControl.orbiter3.link = &enemies[(s32)instruction->u.pos.x - 1].actor;
                    enemies[(s32)instruction->u.pos.x - 1].actor.anim->time = 184.0f;
                    enemies[(s32)instruction->u.pos.x - 1].actor.anim->speed = 0;
                    enemies[(s32)instruction->u.pos.x - 1].actor.anim->current = 5;
                    pos = &handControl.orbiter3.pos;
                    break;
                case -4:
                    enemies[(s32)instruction->u.pos.x - 1].actor.flags |= 0x11;
                    handControl.orbiter3.link = &hand;
                    pos = &handControl.orbiter3.pos;
                    CreateStarCircle(pos, 5, 19);
                    break;
                case -2:
                    cameraInfo.sideOffsetEnabled = 0;
                    if (instruction->time == 1) {
                        camTarget = *cameraInfo.focus;
                        cameraInfo.focusVel = &zero;
                        cameraInfo.focus = &camTarget;
                    }
                    pos = &camTarget;
                    break;
                case -1:
                    pos = &camera.pos;
                    cameraInfo.mode = 2;
                    break;
                case 0:
                    pos = &hand.pos;
                    break;
                default:
                    pos = &enemies[instruction->target - 1].actor.pos;
                    break;
            }
            if (MoveCameoPoint(instruction, pos) && instruction->duration == -1) {
                instruction->done = 1;
                if (instruction->target == -1) {
                    cameraInfo.mode = 1;
                }
            }
            break;
        case CAMEO_INSTRUCTION_TIE_ACTOR_TO_SPHERE:
            if (instruction->u.args[0] == 0) {
                actor = &hand;
            } else {
                actor = &enemies[instruction->u.args[0] - 1].actor;
            }
            switch (instruction->target) {
                case -2:
                    if (instruction->time == 1) {
                        camTarget = *cameraInfo.focus;
                        cameraInfo.focusVel = &zero;
                        cameraInfo.focus = &camTarget;
                    }
                    pos = &camTarget;
                    break;
                case -1:
                    pos = &camera.pos;
                    cameraInfo.mode = 2;
                    break;
                case 0:
                    pos = &hand.pos;
                    hand.link = actor;
                    handControl.dropTimer = 0;
                    break;
                default:
                    pos = &enemies[instruction->target - 1].actor.pos;
                    enemies[instruction->target - 1].actor.link = actor;
                    break;
            }
            COPY_VECTOR(pos, &actor->obj->spheres[instruction->u.args[1]].pos);
            break;
    }
    if (instruction->time == instruction->duration || (instruction->duration < -1 && cameo->items[-instruction->duration]->done != 0)) {
        instruction->done = 1;
        if (instruction->type == CAMEO_INSTRUCTION_TARGET_CAMERA) {
            if (instruction->target == -1) {
                cameraInfo.focus = cameoCamPos;
                cameraInfo.focusVel = cameoCamVel;
            }
            cameraInfo.sideOffsetEnabled = 1;
        } else if (instruction->type == CAMEO_INSTRUCTION_MOVE_ACTOR && instruction->target == -1) {
            cameraInfo.mode = 1;
        } else if (instruction->type == CAMEO_INSTRUCTION_MOVE_ACTOR && instruction->target == -2) {
            cameraInfo.sideOffsetEnabled = 1;
        } else if (instruction->type == CAMEO_INSTRUCTION_TIE_ACTOR_TO_SPHERE) {
        } else if (instruction->type == CAMEO_INSTRUCTION_DISABLE_ACTOR) {
            enemies[instruction->target - 1].brain.flags &= ~0x100;
        }
    }
    if (instruction->duration != 0) {
        instruction->time++;
    } else {
        instruction->time = 0;
    }
    if (instruction->done) {
        instruction->time = 0;
    }
}

PORT_C_END
