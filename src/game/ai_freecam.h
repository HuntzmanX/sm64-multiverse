#ifndef AI_FREECAM_H
#define AI_FREECAM_H

#include <PR/ultratypes.h>

struct Camera;

enum AiCameraMode {
    AI_CAMERA_MODE_FREE,
    AI_CAMERA_MODE_FOCUS,
    AI_CAMERA_MODE_FOLLOW,
    AI_CAMERA_MODE_FIRST_PERSON,
    AI_CAMERA_MODE_COUNT,
};

void ai_freecam_update(struct Camera *camera);
s32 ai_freecam_is_enabled(void);
enum AiCameraMode ai_freecam_get_mode(void);

#endif
