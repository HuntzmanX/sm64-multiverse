#include <ultra64.h>

#include "sm64.h"
#include "ai_controller.h"

struct AiControllerStep {
    s8 stickX;
    s8 stickY;
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

#define AI_CONTROLLER_DEBUG_COMBO (Z_TRIG | R_TRIG | B_BUTTON)

s32 ai_controller_enqueue(s8 stickX, s8 stickY, u16 buttons, u16 frames) {
    struct AiControllerStep *step;

    if (frames == 0 || sQueueCount >= AI_CONTROLLER_QUEUE_CAPACITY) {
        return FALSE;
    }

    step = &sQueue[sQueueTail];
    step->stickX = stickX;
    step->stickY = stickY;
    step->buttons = buttons & VALID_BUTTONS;
    step->frames = frames;

    sQueueTail = (sQueueTail + 1) % AI_CONTROLLER_QUEUE_CAPACITY;
    sQueueCount++;
    return TRUE;
}

void ai_controller_clear(void) {
    sQueueHead = 0;
    sQueueTail = 0;
    sQueueCount = 0;
    sFramesRemaining = 0;
}

s32 ai_controller_is_active(void) {
    return sFramesRemaining != 0 || sQueueCount != 0;
}

u32 ai_controller_queued_steps(void) {
    return sQueueCount;
}

static s32 ai_controller_begin_next_step(void) {
    if (sQueueCount == 0) {
        return FALSE;
    }

    sCurrentStep = sQueue[sQueueHead];
    sQueueHead = (sQueueHead + 1) % AI_CONTROLLER_QUEUE_CAPACITY;
    sQueueCount--;
    sFramesRemaining = sCurrentStep.frames;
    return TRUE;
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
        return;
    }

    // Temporary development trigger. Hold Z + R, then press B.
    if (!ai_controller_is_active()
        && (physicalButtons & AI_CONTROLLER_DEBUG_COMBO) == AI_CONTROLLER_DEBUG_COMBO
        && (physicalButtonsPressed & B_BUTTON)) {
        ai_controller_queue_debug_sequence();
    }

    if (sFramesRemaining == 0 && !ai_controller_begin_next_step()) {
        return;
    }

    pad->stick_x = sCurrentStep.stickX;
    pad->stick_y = sCurrentStep.stickY;
    pad->button = sCurrentStep.buttons;

    sFramesRemaining--;
}
