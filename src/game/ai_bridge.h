#ifndef AI_BRIDGE_H
#define AI_BRIDGE_H

#include <PR/ultratypes.h>

enum AiBridgeStatus {
    AI_BRIDGE_READY,
    AI_BRIDGE_THINKING,
    AI_BRIDGE_EXECUTING,
    AI_BRIDGE_COMPLETE,
    AI_BRIDGE_ERROR,
    AI_BRIDGE_CANCELLED,
};

/** Poll for a command packet and publish a small Mario state snapshot. */
void ai_bridge_update(void);

/** Return TRUE while an instruction may still produce more actions. */
s32 ai_bridge_has_pending_instruction(void);

/** Stop current playback and reject its next late-arriving plan packet. */
void ai_bridge_cancel_current(void);

/** Render the current bridge state with fixed HUD-font labels. */
void ai_bridge_render_status(void);

/** Render queued Mario speech with the native dialog font. */
void ai_bridge_render_dialogue(void);

#endif // AI_BRIDGE_H
