#ifndef AI_BRIDGE_H
#define AI_BRIDGE_H

enum AiBridgeStatus {
    AI_BRIDGE_READY,
    AI_BRIDGE_THINKING,
    AI_BRIDGE_EXECUTING,
    AI_BRIDGE_COMPLETE,
    AI_BRIDGE_ERROR,
};

/** Poll for a command packet and publish a small Mario state snapshot. */
void ai_bridge_update(void);

/** Render the current bridge state in the lower-left corner of the HUD. */
void ai_bridge_render_status(void);

#endif // AI_BRIDGE_H
