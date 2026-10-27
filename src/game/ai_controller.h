#ifndef AI_CONTROLLER_H
#define AI_CONTROLLER_H

#include <PR/os_cont.h>
#include <PR/ultratypes.h>

#define AI_CONTROLLER_QUEUE_CAPACITY 64
#define AI_CONTROLLER_DISPLAY_INPUT 0
#define AI_CONTROLLER_DISPLAY_TARGET 1

struct Object;

struct AiControllerDisplayStep {
    s8 type;
    s8 stickX;
    s8 stickY;
    s16 targetId;
    s8 targetDirection;
    u8 targetSpeed;
    u16 buttons;
    u16 frames;
};

/**
 * Add one controller state to the end of the AI input queue.
 *
 * Stick values use the native N64 range. The supplied buttons and stick
 * position are held for the requested number of game frames.
 *
 * Returns TRUE when the step was queued, or FALSE when the step was invalid
 * or the queue was full.
 */
s32 ai_controller_enqueue(s8 stickX, s8 stickY, u16 buttons, u16 frames);

/** Track an object-pool ID live, moving toward (1) or away (-1) from it. */
s32 ai_controller_enqueue_target(s16 targetId, s8 direction, u8 speed, u16 buttons,
                                 u16 frames, u16 stopDistance);

/** Return a stable, protocol-safe ID for a live object. */
s16 ai_controller_object_id(struct Object *object);

/** Resolve a stable ID for HUD display; the result may no longer be active. */
struct Object *ai_controller_object_from_id(s16 objectId);

/** Anchor queued camera-relative stick values to the telemetry snapshot yaw. */
void ai_controller_set_reference_yaw(s16 yaw);

/** Clear stale target feedback as a newly accepted plan begins. */
void ai_controller_begin_plan(void);

/** Stop playback immediately and discard every queued input step. */
void ai_controller_clear(void);

/** Returns TRUE while a step is playing or waiting in the queue. */
s32 ai_controller_is_active(void);

/** Return and clear the physical-Start emergency cancellation flag. */
s32 ai_controller_take_cancelled(void);

/** Returns the number of steps waiting after the currently playing step. */
u32 ai_controller_queued_steps(void);

/** Copy the active step followed by queued steps for the widescreen HUD. */
s32 ai_controller_get_display_steps(struct AiControllerDisplayStep *steps, s32 capacity);

/** Human-readable result of the most recent tracked target step. */
const char *ai_controller_target_result_text(void);

/**
 * Apply the current queued step to player one's native controller data.
 * Call once per game frame after hardware input has been read.
 */
void ai_controller_update(OSContPad *pad);

#endif // AI_CONTROLLER_H
