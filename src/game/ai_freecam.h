#ifndef AI_FREECAM_H
#define AI_FREECAM_H

#include <PR/ultratypes.h>

struct Camera;

void ai_freecam_update(struct Camera *camera);
s32 ai_freecam_is_enabled(void);

#endif
