#include <ultra64.h>

#include "sm64.h"
#include "ai_freecam.h"

#ifndef TARGET_N64

#include "ai_input.h"
#include "camera.h"
#include "engine/math_util.h"
#include "level_update.h"

#define AI_FREECAM_MOVE_SPEED 45.0f
#define AI_FREECAM_YAW_SPEED 0x280
#define AI_FREECAM_PITCH_SPEED 0x180
#define AI_FREECAM_MOUSE_SPEED 0x20
#define AI_FREECAM_FOCUS_DISTANCE 1000.0f
#define AI_FREECAM_MAX_PITCH 0x3800

static s32 sEnabled = TRUE;
static s32 sInitialized;
static struct Camera *sLastCamera;
static Vec3f sPosition;
static s16 sViewYaw;
static s16 sViewPitch;

s32 ai_freecam_is_enabled(void) {
    return sEnabled;
}

static void ai_freecam_initialize(struct Camera *camera) {
    vec3f_copy(sPosition, gLakituState.pos);
    sViewYaw = calculate_yaw(gLakituState.pos, gLakituState.focus);
    sViewPitch = calculate_pitch(gLakituState.pos, gLakituState.focus);
    sLastCamera = camera;
    sInitialized = TRUE;
}

void ai_freecam_update(struct Camera *camera) {
    Vec3f focus;
    s32 forward;
    s32 right;
    s32 vertical;
    s16 cameraYaw;

    if (ai_input_take_freecam_toggle()) {
        sEnabled = !sEnabled;
        sInitialized = FALSE;
    }
    if (!sEnabled || camera == NULL || camera->cutscene != 0 || sCurrPlayMode != 0) {
        ai_input_take_mouse_x();
        ai_input_take_mouse_y();
        sInitialized = FALSE;
        return;
    }
    if (!sInitialized || sLastCamera != camera) ai_freecam_initialize(camera);

    forward = ai_input_move_forward();
    right = ai_input_move_right();
    vertical = ai_input_move_vertical();
    sViewYaw += ai_input_look_yaw() * AI_FREECAM_YAW_SPEED
              + ai_input_take_mouse_x() * AI_FREECAM_MOUSE_SPEED;
    sViewPitch += ai_input_look_pitch() * AI_FREECAM_PITCH_SPEED
                - ai_input_take_mouse_y() * AI_FREECAM_MOUSE_SPEED;
    if (sViewPitch > AI_FREECAM_MAX_PITCH) sViewPitch = AI_FREECAM_MAX_PITCH;
    if (sViewPitch < -AI_FREECAM_MAX_PITCH) sViewPitch = -AI_FREECAM_MAX_PITCH;

    sPosition[0] += (forward * sins(sViewYaw) + right * coss(sViewYaw)) * AI_FREECAM_MOVE_SPEED;
    sPosition[1] += vertical * AI_FREECAM_MOVE_SPEED;
    sPosition[2] += (forward * coss(sViewYaw) - right * sins(sViewYaw)) * AI_FREECAM_MOVE_SPEED;
    vec3f_set_dist_and_angle(sPosition, focus, AI_FREECAM_FOCUS_DISTANCE, sViewPitch, sViewYaw);
    cameraYaw = sViewYaw + 0x8000;

    vec3f_copy(camera->pos, sPosition);
    vec3f_copy(camera->focus, focus);
    camera->yaw = cameraYaw;
    camera->nextYaw = cameraYaw;
    vec3f_copy(gLakituState.pos, sPosition);
    vec3f_copy(gLakituState.curPos, sPosition);
    vec3f_copy(gLakituState.goalPos, sPosition);
    vec3f_copy(gLakituState.focus, focus);
    vec3f_copy(gLakituState.curFocus, focus);
    vec3f_copy(gLakituState.goalFocus, focus);
    gLakituState.yaw = cameraYaw;
    gLakituState.nextYaw = cameraYaw;
    gLakituState.oldYaw = cameraYaw;
    gLakituState.oldPitch = sViewPitch;
    gLakituState.focusDistance = AI_FREECAM_FOCUS_DISTANCE;
}

#else

void ai_freecam_update(UNUSED struct Camera *camera) {}
s32 ai_freecam_is_enabled(void) { return FALSE; }

#endif
