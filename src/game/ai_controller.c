#include <ultra64.h>

#include "sm64.h"
#include "ai_controller.h"
#include "camera.h"
#include "behavior_data.h"
#include "engine/math_util.h"
#include "level_update.h"
#include "model_ids.h"
#include "object_constants.h"
#include "object_helpers.h"
#include "object_list_processor.h"

enum AiControllerStepType {
    AI_CONTROLLER_STEP_INPUT,
    AI_CONTROLLER_STEP_TARGET,
};

enum AiControllerTargetResult {
    AI_TARGET_NONE,
    AI_TARGET_TRACKING,
    AI_TARGET_ARRIVED,
    AI_TARGET_FLED,
    AI_TARGET_LOST,
    AI_TARGET_STUCK,
    AI_TARGET_TIMEOUT,
};

struct AiControllerStep {
    s8 type;
    s8 stickX;
    s8 stickY;
    s16 targetId;
    s8 targetDirection;
    u8 targetSpeed;
    u16 stopDistance;
    u16 buttons;
    u16 frames;
};

static struct AiControllerStep sQueue[AI_CONTROLLER_QUEUE_CAPACITY];
static struct AiControllerStep sCurrentStep;
static u32 sQueueHead;
static u32 sQueueTail;
static u32 sQueueCount;
static u16 sFramesRemaining;
static u16 sPreviousPhysicalButtons;
static s32 sCancelled;
static s32 sHasReferenceYaw;
static s16 sReferenceCameraYaw;
static s32 sTargetResult;
static f32 sTargetLastMarioX;
static f32 sTargetLastMarioZ;
static f32 sTargetLastDistance;
static u16 sTargetStuckFrames;
static u16 sTargetMarkerFrames;
static f32 sInputLastMarioX;
static f32 sInputLastMarioZ;
static u16 sInputElapsedFrames;
static u16 sInputStationaryFrames;

#ifdef USE_SYSTEM_MALLOC
/*
 * PC builds allocate objects individually rather than defining gObjectPool.
 * Keep a compact pointer registry so telemetry and TARGET commands can still
 * share stable IDs. Deallocated object nodes are reused by the engine, which
 * also makes the ID stable when a pool slot is recycled.
 */
static struct Object *sObjectRegistry[OBJECT_POOL_CAPACITY];
#endif

#define AI_CONTROLLER_DEBUG_COMBO (Z_TRIG | R_TRIG | B_BUTTON)
#define AI_CONTROLLER_INPUT_SETTLE_GRACE 12
#define AI_CONTROLLER_INPUT_STATIONARY_FRAMES 6
#define AI_CONTROLLER_INPUT_STATIONARY_DISTANCE 1.0f

s16 ai_controller_object_id(struct Object *object) {
    if (object == NULL) return 0;

#ifdef USE_SYSTEM_MALLOC
    s32 i;

    for (i = 0; i < OBJECT_POOL_CAPACITY; i++) {
        if (sObjectRegistry[i] == object) return i + 1;
    }
    for (i = 0; i < OBJECT_POOL_CAPACITY; i++) {
        if (sObjectRegistry[i] == NULL
            || !(sObjectRegistry[i]->activeFlags & ACTIVE_FLAG_ACTIVE)) {
            sObjectRegistry[i] = object;
            return i + 1;
        }
    }
    return 0;
#else
    if (object < gObjectPool || object >= gObjectPool + OBJECT_POOL_CAPACITY) return 0;
    return (s16) (object - gObjectPool) + 1;
#endif
}

static struct Object *ai_controller_object_from_id(s16 objectId) {
    if (objectId < 1 || objectId > OBJECT_POOL_CAPACITY) return NULL;
#ifdef USE_SYSTEM_MALLOC
    return sObjectRegistry[objectId - 1];
#else
    return &gObjectPool[objectId - 1];
#endif
}

s32 ai_controller_enqueue(s8 stickX, s8 stickY, u16 buttons, u16 frames) {
    struct AiControllerStep *step;

    if (frames == 0 || sQueueCount >= AI_CONTROLLER_QUEUE_CAPACITY) {
        return FALSE;
    }

    step = &sQueue[sQueueTail];
    step->type = AI_CONTROLLER_STEP_INPUT;
    step->stickX = stickX;
    step->stickY = stickY;
    step->buttons = buttons & VALID_BUTTONS;
    step->frames = frames;

    sQueueTail = (sQueueTail + 1) % AI_CONTROLLER_QUEUE_CAPACITY;
    sQueueCount++;
    return TRUE;
}

s32 ai_controller_enqueue_target(s16 targetId, s8 direction, u8 speed, u16 buttons,
                                 u16 frames, u16 stopDistance) {
    struct AiControllerStep *step;

    if (frames == 0 || targetId < 1 || targetId > OBJECT_POOL_CAPACITY
        || (direction != 1 && direction != -1) || speed == 0 || speed > 80
        || sQueueCount >= AI_CONTROLLER_QUEUE_CAPACITY) {
        return FALSE;
    }

    step = &sQueue[sQueueTail];
    step->type = AI_CONTROLLER_STEP_TARGET;
    step->stickX = 0;
    step->stickY = 0;
    step->targetId = targetId;
    step->targetDirection = direction;
    step->targetSpeed = speed;
    step->stopDistance = stopDistance;
    step->buttons = buttons & VALID_BUTTONS;
    step->frames = frames;

    sQueueTail = (sQueueTail + 1) % AI_CONTROLLER_QUEUE_CAPACITY;
    sQueueCount++;
    return TRUE;
}

void ai_controller_set_reference_yaw(s16 yaw) {
    sReferenceCameraYaw = yaw;
    sHasReferenceYaw = TRUE;
}

void ai_controller_begin_plan(void) {
    sTargetResult = AI_TARGET_NONE;
}

void ai_controller_clear(void) {
    sQueueHead = 0;
    sQueueTail = 0;
    sQueueCount = 0;
    sFramesRemaining = 0;
    sHasReferenceYaw = FALSE;
    sTargetResult = AI_TARGET_NONE;
}

s32 ai_controller_is_active(void) {
    return sFramesRemaining != 0 || sQueueCount != 0;
}

s32 ai_controller_take_cancelled(void) {
    s32 cancelled = sCancelled;
    sCancelled = FALSE;
    return cancelled;
}

u32 ai_controller_queued_steps(void) {
    return sQueueCount;
}

const char *ai_controller_target_result_text(void) {
    switch (sTargetResult) {
        case AI_TARGET_TRACKING: return "tracking";
        case AI_TARGET_ARRIVED: return "arrived";
        case AI_TARGET_FLED: return "clear";
        case AI_TARGET_LOST: return "target_lost";
        case AI_TARGET_STUCK: return "stuck";
        case AI_TARGET_TIMEOUT: return "timeout";
        default: return "none";
    }
}

static s32 ai_controller_begin_next_step(void) {
    if (sQueueCount == 0) {
        return FALSE;
    }

    sCurrentStep = sQueue[sQueueHead];
    sQueueHead = (sQueueHead + 1) % AI_CONTROLLER_QUEUE_CAPACITY;
    sQueueCount--;
    sFramesRemaining = sCurrentStep.frames;
    sInputElapsedFrames = 0;
    sInputStationaryFrames = 0;
    if (gMarioState != NULL) {
        sInputLastMarioX = gMarioState->pos[0];
        sInputLastMarioZ = gMarioState->pos[2];
    }
    if (sCurrentStep.type == AI_CONTROLLER_STEP_TARGET) {
        sTargetResult = AI_TARGET_TRACKING;
        sTargetStuckFrames = 0;
        sTargetMarkerFrames = 0;
        sTargetLastDistance = 1000000.0f;
        if (gMarioState != NULL) {
            sTargetLastMarioX = gMarioState->pos[0];
            sTargetLastMarioZ = gMarioState->pos[2];
        }
    }
    return TRUE;
}

static struct Object *ai_controller_target_object(void) {
    struct Object *target = ai_controller_object_from_id(sCurrentStep.targetId);
    if (target == NULL) return NULL;
    if (!(target->activeFlags & ACTIVE_FLAG_ACTIVE)
        || (target->activeFlags & ACTIVE_FLAG_IN_DIFFERENT_ROOM)) return NULL;
    return target;
}

static s32 ai_controller_apply_target_step(OSContPad *pad) {
    struct Object *target = ai_controller_target_object();
    f32 dx;
    f32 dz;
    f32 distance;
    f32 movedX;
    f32 movedZ;
    s16 targetYaw;
    s16 relativeYaw;

    if (target == NULL || gMarioState == NULL || gCamera == NULL) {
        sTargetResult = (sTargetLastDistance <= sCurrentStep.stopDistance + 200.0f)
            ? AI_TARGET_ARRIVED : AI_TARGET_LOST;
        return TRUE;
    }

    dx = target->oPosX - gMarioState->pos[0];
    dz = target->oPosZ - gMarioState->pos[2];
    distance = sqrtf(dx * dx + dz * dz);
    sTargetLastDistance = distance;

    if ((sCurrentStep.targetDirection > 0 && distance <= sCurrentStep.stopDistance)
        || (sCurrentStep.targetDirection < 0 && distance >= sCurrentStep.stopDistance)) {
        sTargetResult = sCurrentStep.targetDirection > 0 ? AI_TARGET_ARRIVED : AI_TARGET_FLED;
        return TRUE;
    }

    if (++sTargetStuckFrames >= 30) {
        movedX = gMarioState->pos[0] - sTargetLastMarioX;
        movedZ = gMarioState->pos[2] - sTargetLastMarioZ;
        if (sqrtf(movedX * movedX + movedZ * movedZ) < 20.0f) {
            sTargetResult = AI_TARGET_STUCK;
            return TRUE;
        }
        sTargetLastMarioX = gMarioState->pos[0];
        sTargetLastMarioZ = gMarioState->pos[2];
        sTargetStuckFrames = 0;
    }

    targetYaw = atan2s(dz, dx);
    if (sCurrentStep.targetDirection < 0) targetYaw += 0x8000;
    relativeYaw = targetYaw - gCamera->yaw;
    pad->stick_x = sins(relativeYaw) * sCurrentStep.targetSpeed;
    pad->stick_y = -coss(relativeYaw) * sCurrentStep.targetSpeed;
    pad->button = sCurrentStep.buttons;

    if (++sTargetMarkerFrames >= 10) {
        spawn_object(target, MODEL_NONE, bhvSparkleSpawn);
        sTargetMarkerFrames = 0;
    }
    return FALSE;
}

static s32 ai_controller_input_has_settled(void) {
    f32 movedX;
    f32 movedZ;
    f32 stationaryDistanceSquared = AI_CONTROLLER_INPUT_STATIONARY_DISTANCE
        * AI_CONTROLLER_INPUT_STATIONARY_DISTANCE;

    /*
     * Only shorten ordinary movement. Neutral inputs are intentional waits or
     * button-release windows, while button-bearing inputs must run for their
     * requested duration so presses are never swallowed.
     */
    if (gMarioState == NULL || sCurrentStep.type != AI_CONTROLLER_STEP_INPUT
        || (sCurrentStep.stickX == 0 && sCurrentStep.stickY == 0)
        || sCurrentStep.buttons != 0) {
        return FALSE;
    }

    movedX = gMarioState->pos[0] - sInputLastMarioX;
    movedZ = gMarioState->pos[2] - sInputLastMarioZ;
    sInputLastMarioX = gMarioState->pos[0];
    sInputLastMarioZ = gMarioState->pos[2];
    sInputElapsedFrames++;

    if (movedX * movedX + movedZ * movedZ <= stationaryDistanceSquared) {
        sInputStationaryFrames++;
    } else {
        sInputStationaryFrames = 0;
    }

    return sInputElapsedFrames >= AI_CONTROLLER_INPUT_SETTLE_GRACE
        && sInputStationaryFrames >= AI_CONTROLLER_INPUT_STATIONARY_FRAMES;
}

static void ai_controller_apply_world_locked_stick(OSContPad *pad) {
    f32 cosine;
    f32 sine;
    s16 delta;
    s32 stickX;
    s32 stickY;

    if (gCamera == NULL || (sCurrentStep.stickX == 0 && sCurrentStep.stickY == 0)) return;
    if (!sHasReferenceYaw) {
        sReferenceCameraYaw = gCamera->yaw;
        sHasReferenceYaw = TRUE;
    }

    // Controller sticks are camera-relative. Rotate the queued vector by the
    // camera delta so its world-space direction stays fixed while the human
    // moves the spectator camera.
    delta = sReferenceCameraYaw - gCamera->yaw;
    cosine = coss(delta);
    sine = sins(delta);
    // Preserve atan2s(-stickY, stickX) + cameraYaw as the camera rotates.
    stickX = sCurrentStep.stickX * cosine - sCurrentStep.stickY * sine;
    stickY = sCurrentStep.stickY * cosine + sCurrentStep.stickX * sine;
    if (stickX > 80) stickX = 80;
    if (stickX < -80) stickX = -80;
    if (stickY > 80) stickY = 80;
    if (stickY < -80) stickY = -80;
    pad->stick_x = stickX;
    pad->stick_y = stickY;
}

static void ai_controller_queue_debug_sequence(void) {
    ai_controller_clear();

    // Run forward, jump, keep moving through the jump, then come to rest.
    ai_controller_enqueue(0, 80, 0, 60);
    ai_controller_enqueue(0, 80, A_BUTTON, 4);
    ai_controller_enqueue(0, 80, 0, 26);
    ai_controller_enqueue(0, 0, 0, 15);
}

void ai_controller_update(OSContPad *pad) {
    u16 physicalButtons;
    u16 physicalButtonsPressed;

    if (pad == NULL) {
        return;
    }

    // Always inspect the real controller first so START remains an emergency
    // escape even while queued input has exclusive control of Mario.
    physicalButtons = pad->button;
    physicalButtonsPressed = physicalButtons & (physicalButtons ^ sPreviousPhysicalButtons);
    sPreviousPhysicalButtons = physicalButtons;

    if (ai_controller_is_active() && (physicalButtonsPressed & START_BUTTON)) {
        ai_controller_clear();
        sCancelled = TRUE;
        return;
    }

    // Temporary development trigger. Hold Z + R, then press B.
    if (!ai_controller_is_active()
        && (physicalButtons & AI_CONTROLLER_DEBUG_COMBO) == AI_CONTROLLER_DEBUG_COMBO
        && (physicalButtonsPressed & B_BUTTON)) {
        ai_controller_queue_debug_sequence();
    }

    while (TRUE) {
        if (sFramesRemaining == 0 && !ai_controller_begin_next_step()) return;

        if (sCurrentStep.type == AI_CONTROLLER_STEP_TARGET) {
            if (ai_controller_apply_target_step(pad)) {
                sFramesRemaining = 0;
                if (sTargetResult == AI_TARGET_LOST || sTargetResult == AI_TARGET_STUCK) {
                    sQueueHead = 0;
                    sQueueTail = 0;
                    sQueueCount = 0;
                    sHasReferenceYaw = FALSE;
                    return;
                }
                if (sQueueCount == 0) sHasReferenceYaw = FALSE;
                continue;
            }
        } else {
            pad->stick_x = sCurrentStep.stickX;
            pad->stick_y = sCurrentStep.stickY;
            pad->button = sCurrentStep.buttons;
            ai_controller_apply_world_locked_stick(pad);
            if (ai_controller_input_has_settled()) {
                sFramesRemaining = 0;
                if (sQueueCount == 0) {
                    sHasReferenceYaw = FALSE;
                    return;
                }
                continue;
            }
        }

        if (--sFramesRemaining == 0) {
            if (sCurrentStep.type == AI_CONTROLLER_STEP_TARGET
                && sTargetResult == AI_TARGET_TRACKING) {
                sTargetResult = AI_TARGET_TIMEOUT;
            }
            if (sQueueCount == 0) sHasReferenceYaw = FALSE;
        }
        return;
    }
}
