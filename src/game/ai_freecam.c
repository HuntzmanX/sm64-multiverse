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
#define AI_FIRST_PERSON_EYE_HEIGHT 120.0f
#define AI_FIRST_PERSON_VIEW_DISTANCE 1000.0f

static s32 sEnabled = TRUE;
static s32 sInitialized;
static enum AiCameraMode sMode = AI_CAMERA_MODE_FREE;
static struct Camera *sLastCamera;
static Vec3f sPosition;
static Vec3f sFollowOffset;
static s16 sViewYaw;
static s16 sViewPitch;
static s16 sFirstPersonYawOffset;
static s16 sFirstPersonPitch;
static s32 sMarioHidden;

s32 ai_freecam_is_enabled(void) {
    return sEnabled;
}

enum AiCameraMode ai_freecam_get_mode(void) {
    return sMode;
}

static void ai_freecam_show_mario(void) {
    if (sMarioHidden && gMarioState != NULL && gMarioState->marioObj != NULL) {
        gMarioState->marioObj->header.gfx.node.flags &= ~GRAPH_RENDER_INVISIBLE;
    }
    sMarioHidden = FALSE;
}

static void ai_freecam_hide_mario(void) {
    if (gMarioState != NULL && gMarioState->marioObj != NULL) {
        gMarioState->marioObj->header.gfx.node.flags |= GRAPH_RENDER_INVISIBLE;
        sMarioHidden = TRUE;
    }
}

static void ai_freecam_capture_follow_offset(void) {
    if (gMarioState == NULL) return;
    sFollowOffset[0] = sPosition[0] - gMarioState->pos[0];
    sFollowOffset[1] = sPosition[1] - gMarioState->pos[1];
    sFollowOffset[2] = sPosition[2] - gMarioState->pos[2];
}

static void ai_freecam_initialize(struct Camera *camera) {
    vec3f_copy(sPosition, gLakituState.pos);
    sViewYaw = calculate_yaw(gLakituState.pos, gLakituState.focus);
    sViewPitch = calculate_pitch(gLakituState.pos, gLakituState.focus);
    if (sMode == AI_CAMERA_MODE_FOLLOW) ai_freecam_capture_follow_offset();
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
    s32 modeToggle;
    s16 cameraYaw;

    if (ai_input_take_freecam_toggle()) {
        sEnabled = !sEnabled;
        if (!sEnabled) {
            sMode = AI_CAMERA_MODE_FREE;
            ai_freecam_show_mario();
        }
        sInitialized = FALSE;
    }
    modeToggle = ai_input_take_focus_toggle();
    if (!sEnabled || camera == NULL || camera->cutscene != 0 || sCurrPlayMode != 0) {
        ai_input_take_mouse_x();
        ai_input_take_mouse_y();
        ai_freecam_show_mario();
        sInitialized = FALSE;
        return;
    }
    if (!sInitialized || sLastCamera != camera) ai_freecam_initialize(camera);
    if (modeToggle) {
        enum AiCameraMode oldMode = sMode;
        sMode = (sMode + 1) % AI_CAMERA_MODE_COUNT;
        if (oldMode == AI_CAMERA_MODE_FIRST_PERSON) {
            ai_freecam_show_mario();
            if (gMarioState != NULL) {
                vec3f_set(focus, gMarioState->pos[0],
                          gMarioState->pos[1] + AI_FREECAM_MARIO_FOCUS_HEIGHT,
                          gMarioState->pos[2]);
                vec3f_set_dist_and_angle(focus, sPosition, AI_FREECAM_FOCUS_DISTANCE,
                                         0x1000, sViewYaw + 0x8000);
                sViewYaw = calculate_yaw(sPosition, focus);
                sViewPitch = calculate_pitch(sPosition, focus);
            }
        }
        if (sMode == AI_CAMERA_MODE_FOLLOW) ai_freecam_capture_follow_offset();
        if (sMode == AI_CAMERA_MODE_FIRST_PERSON) {
            sFirstPersonYawOffset = 0;
            sFirstPersonPitch = 0;
        }
    }

    forward = ai_input_move_forward();
    right = ai_input_move_right();
    vertical = ai_input_move_vertical();
    lookYaw = ai_input_look_yaw();
    lookPitch = ai_input_look_pitch();
    mouseX = ai_input_take_mouse_x();
    mouseY = ai_input_take_mouse_y();
    if (sMode == AI_CAMERA_MODE_FIRST_PERSON && gMarioState != NULL) {
        sFirstPersonYawOffset += lookYaw * AI_FREECAM_YAW_SPEED - mouseX * AI_FREECAM_MOUSE_SPEED;
        sFirstPersonPitch += lookPitch * AI_FREECAM_PITCH_SPEED - mouseY * AI_FREECAM_MOUSE_SPEED;
        if (sFirstPersonPitch > AI_FREECAM_MAX_PITCH) sFirstPersonPitch = AI_FREECAM_MAX_PITCH;
        if (sFirstPersonPitch < -AI_FREECAM_MAX_PITCH) sFirstPersonPitch = -AI_FREECAM_MAX_PITCH;

        sViewYaw = gMarioState->faceAngle[1] + sFirstPersonYawOffset;
        sViewPitch = sFirstPersonPitch;
        vec3f_set(sPosition, gMarioState->pos[0],
                  gMarioState->pos[1] + AI_FIRST_PERSON_EYE_HEIGHT,
                  gMarioState->pos[2]);
        vec3f_set_dist_and_angle(sPosition, focus, AI_FIRST_PERSON_VIEW_DISTANCE,
                                 sViewPitch, sViewYaw);
        ai_freecam_hide_mario();
        focusDistance = AI_FIRST_PERSON_VIEW_DISTANCE;
    } else if ((sMode == AI_CAMERA_MODE_FOCUS || sMode == AI_CAMERA_MODE_FOLLOW)
               && gMarioState != NULL) {
        if (sMode == AI_CAMERA_MODE_FOLLOW) {
            vec3f_set(sPosition,
                      gMarioState->pos[0] + sFollowOffset[0],
                      gMarioState->pos[1] + sFollowOffset[1],
                      gMarioState->pos[2] + sFollowOffset[2]);
        }
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

    if (sMode != AI_CAMERA_MODE_FIRST_PERSON) {
        // W/S follows the full view vector, including pitch. A/D remains a level
        // strafe, while Q/E supplies explicit world-vertical movement.
        sPosition[0] += (forward * coss(sViewPitch) * sins(sViewYaw)
                        + right * coss(sViewYaw)) * AI_FREECAM_MOVE_SPEED;
        sPosition[1] += (forward * sins(sViewPitch) + vertical) * AI_FREECAM_MOVE_SPEED;
        sPosition[2] += (forward * coss(sViewPitch) * coss(sViewYaw)
                        - right * sins(sViewYaw)) * AI_FREECAM_MOVE_SPEED;
    }
    if ((sMode == AI_CAMERA_MODE_FOCUS || sMode == AI_CAMERA_MODE_FOLLOW)
        && gMarioState != NULL) {
        vec3f_set(focus, gMarioState->pos[0],
                  gMarioState->pos[1] + AI_FREECAM_MARIO_FOCUS_HEIGHT,
                  gMarioState->pos[2]);
        vec3f_get_dist_and_angle(sPosition, focus, &focusDistance, &sViewPitch, &sViewYaw);
        if (sMode == AI_CAMERA_MODE_FOLLOW) ai_freecam_capture_follow_offset();
    } else if (sMode != AI_CAMERA_MODE_FIRST_PERSON) {
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
enum AiCameraMode ai_freecam_get_mode(void) { return AI_CAMERA_MODE_FREE; }

#endif
