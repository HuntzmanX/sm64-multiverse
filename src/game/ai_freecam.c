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
#define AI_FREECAM_MARIO_FOCUS_HEIGHT 100.0f
#define AI_FREECAM_MAX_PITCH 0x3800

static s32 sEnabled = TRUE;
static s32 sInitialized;
static s32 sFocusMode;
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
    f32 focusDistance = AI_FREECAM_FOCUS_DISTANCE;
    s32 forward;
    s32 right;
    s32 vertical;
    s32 lookYaw;
    s32 lookPitch;
    s32 mouseX;
    s32 mouseY;
    s16 cameraYaw;

    if (ai_input_take_freecam_toggle()) {
        sEnabled = !sEnabled;
        if (!sEnabled) sFocusMode = FALSE;
        sInitialized = FALSE;
    }
    if (ai_input_take_focus_toggle() && sEnabled) sFocusMode = !sFocusMode;
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
    lookYaw = ai_input_look_yaw();
    lookPitch = ai_input_look_pitch();
    mouseX = ai_input_take_mouse_x();
    mouseY = ai_input_take_mouse_y();
    if (sFocusMode && gMarioState != NULL) {
        vec3f_set(focus, gMarioState->pos[0],
                  gMarioState->pos[1] + AI_FREECAM_MARIO_FOCUS_HEIGHT,
                  gMarioState->pos[2]);
        sViewYaw = calculate_yaw(sPosition, focus);
        sViewPitch = calculate_pitch(sPosition, focus);
    } else {
        sViewYaw += lookYaw * AI_FREECAM_YAW_SPEED - mouseX * AI_FREECAM_MOUSE_SPEED;
        sViewPitch += lookPitch * AI_FREECAM_PITCH_SPEED - mouseY * AI_FREECAM_MOUSE_SPEED;
        if (sViewPitch > AI_FREECAM_MAX_PITCH) sViewPitch = AI_FREECAM_MAX_PITCH;
        if (sViewPitch < -AI_FREECAM_MAX_PITCH) sViewPitch = -AI_FREECAM_MAX_PITCH;
    }

    // W/S follows the full view vector, including pitch. A/D remains a level
    // strafe, while Q/E supplies explicit world-vertical movement.
    sPosition[0] += (forward * coss(sViewPitch) * sins(sViewYaw)
                    + right * coss(sViewYaw)) * AI_FREECAM_MOVE_SPEED;
    sPosition[1] += (forward * sins(sViewPitch) + vertical) * AI_FREECAM_MOVE_SPEED;
    sPosition[2] += (forward * coss(sViewPitch) * coss(sViewYaw)
                    - right * sins(sViewYaw)) * AI_FREECAM_MOVE_SPEED;
    if (sFocusMode && gMarioState != NULL) {
        vec3f_set(focus, gMarioState->pos[0],
                  gMarioState->pos[1] + AI_FREECAM_MARIO_FOCUS_HEIGHT,
                  gMarioState->pos[2]);
        vec3f_get_dist_and_angle(sPosition, focus, &focusDistance, &sViewPitch, &sViewYaw);
    } else {
        vec3f_set_dist_and_angle(sPosition, focus, focusDistance, sViewPitch, sViewYaw);
    }
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
    gLakituState.focusDistance = focusDistance;
}

#else

void ai_freecam_update(UNUSED struct Camera *camera) {}
s32 ai_freecam_is_enabled(void) { return FALSE; }

#endif
