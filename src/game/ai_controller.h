#ifndef AI_CONTROLLER_H
#define AI_CONTROLLER_H

#include <PR/os_cont.h>
#include <PR/ultratypes.h>

#define AI_CONTROLLER_QUEUE_CAPACITY 64

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

/** Stop playback immediately and discard every queued input step. */
void ai_controller_clear(void);

/** Returns TRUE while a step is playing or waiting in the queue. */
s32 ai_controller_is_active(void);

/** Returns the number of steps waiting after the currently playing step. */
u32 ai_controller_queued_steps(void);

/**
 * Apply the current queued step to player one's native controller data.
 * Call once per game frame after hardware input has been read.
 */
void ai_controller_update(OSContPad *pad);

#endif // AI_CONTROLLER_H
