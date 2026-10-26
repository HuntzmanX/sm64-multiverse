#include <ultra64.h>

#include "sm64.h"
#include "ai_bridge.h"
#include "ai_controller.h"

#ifndef TARGET_N64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "area.h"
#include "camera.h"
#include "level_update.h"

#define AI_BRIDGE_COMMAND_FILE ".sm64-ai-command"
#define AI_BRIDGE_STATE_FILE ".sm64-ai-state.json"
#define AI_BRIDGE_STATUS_FILE ".sm64-ai-status"
#define AI_BRIDGE_MAX_PACKET_STEPS 32
#define AI_BRIDGE_MAX_STEP_FRAMES 300
#define AI_BRIDGE_MAX_TOTAL_FRAMES 900
#define AI_BRIDGE_TELEMETRY_INTERVAL 15
#define AI_BRIDGE_COMPLETE_FRAMES 60
#define AI_BRIDGE_ALLOWED_BUTTONS \
    (A_BUTTON | B_BUTTON | Z_TRIG | R_TRIG | U_CBUTTONS | D_CBUTTONS | L_CBUTTONS | R_CBUTTONS)

struct AiBridgeStep {
    s8 stickX;
    s8 stickY;
    u16 buttons;
    u16 frames;
};

static enum AiBridgeStatus sStatus = AI_BRIDGE_READY;
static s32 sWasActive;
static s32 sTelemetryTimer;
static s32 sCompleteTimer;

static const char *ai_bridge_path(const char *environmentName, const char *fallback) {
    const char *value = getenv(environmentName);
    return value != NULL && value[0] != '\0' ? value : fallback;
}

static const char *ai_bridge_status_text(void) {
    switch (sStatus) {
        case AI_BRIDGE_THINKING:
            return "THINKING";
        case AI_BRIDGE_EXECUTING:
            return "EXECUTING";
        case AI_BRIDGE_COMPLETE:
            return "COMPLETE";
        case AI_BRIDGE_ERROR:
            return "ERROR";
        default:
            return "READY";
    }
}

static void ai_bridge_write_status(void) {
    const char *path = ai_bridge_path("SM64_AI_STATUS_FILE", AI_BRIDGE_STATUS_FILE);
    char temporaryPath[512];
    FILE *file;

    if (snprintf(temporaryPath, sizeof(temporaryPath), "%s.tmp", path) >= (s32) sizeof(temporaryPath)) {
        return;
    }

    file = fopen(temporaryPath, "w");
    if (file == NULL) {
        return;
    }

    fprintf(file, "%s\n", ai_bridge_status_text());
    fclose(file);
    remove(path);
    rename(temporaryPath, path);
}

static void ai_bridge_set_status(enum AiBridgeStatus status) {
    if (sStatus == status) {
        return;
    }

    sStatus = status;
    sCompleteTimer = status == AI_BRIDGE_COMPLETE ? AI_BRIDGE_COMPLETE_FRAMES : 0;
    ai_bridge_write_status();
}

static s32 ai_bridge_parse_status(const char *text, enum AiBridgeStatus *status) {
    if (strcmp(text, "THINKING") == 0) {
        *status = AI_BRIDGE_THINKING;
    } else if (strcmp(text, "READY") == 0) {
        *status = AI_BRIDGE_READY;
    } else if (strcmp(text, "ERROR") == 0) {
        *status = AI_BRIDGE_ERROR;
    } else {
        return FALSE;
    }

    return TRUE;
}

static s32 ai_bridge_load_packet(void) {
    const char *path = ai_bridge_path("SM64_AI_COMMAND_FILE", AI_BRIDGE_COMMAND_FILE);
    struct AiBridgeStep steps[AI_BRIDGE_MAX_PACKET_STEPS];
    enum AiBridgeStatus packetStatus = AI_BRIDGE_READY;
    char line[256];
    s32 stepCount = 0;
    s32 totalFrames = 0;
    s32 sawEnd = FALSE;
    s32 valid = TRUE;
    s32 i;
    FILE *file = fopen(path, "r");

    if (file == NULL) {
        return FALSE;
    }

    if (fgets(line, sizeof(line), file) == NULL || strcmp(line, "SM64AI1\n") != 0) {
        valid = FALSE;
    }

    while (valid && fgets(line, sizeof(line), file) != NULL) {
        s32 stickX;
        s32 stickY;
        s32 buttons;
        s32 frames;
        char statusText[32];

        if (strcmp(line, "END\n") == 0 || strcmp(line, "END\r\n") == 0) {
            sawEnd = TRUE;
            break;
        }

        if (sscanf(line, "INPUT %d %d %d %d", &stickX, &stickY, &buttons, &frames) == 4) {
            if (stepCount >= AI_BRIDGE_MAX_PACKET_STEPS || stickX < -80 || stickX > 80
                || stickY < -80 || stickY > 80 || frames < 1 || frames > AI_BRIDGE_MAX_STEP_FRAMES
                || buttons < 0 || (buttons & ~AI_BRIDGE_ALLOWED_BUTTONS) != 0
                || totalFrames + frames > AI_BRIDGE_MAX_TOTAL_FRAMES) {
                valid = FALSE;
                break;
            }

            steps[stepCount].stickX = stickX;
            steps[stepCount].stickY = stickY;
            steps[stepCount].buttons = buttons;
            steps[stepCount].frames = frames;
            stepCount++;
            totalFrames += frames;
        } else if (sscanf(line, "WAIT %d", &frames) == 1) {
            if (stepCount >= AI_BRIDGE_MAX_PACKET_STEPS || frames < 1
                || frames > AI_BRIDGE_MAX_STEP_FRAMES
                || totalFrames + frames > AI_BRIDGE_MAX_TOTAL_FRAMES) {
                valid = FALSE;
                break;
            }

            steps[stepCount].stickX = 0;
            steps[stepCount].stickY = 0;
            steps[stepCount].buttons = 0;
            steps[stepCount].frames = frames;
            stepCount++;
            totalFrames += frames;
        } else if (sscanf(line, "STATUS %31s", statusText) == 1) {
            if (!ai_bridge_parse_status(statusText, &packetStatus)) {
                valid = FALSE;
            }
        } else {
            valid = FALSE;
        }
    }

    fclose(file);
    remove(path);

    if (!valid || !sawEnd) {
        ai_bridge_set_status(AI_BRIDGE_ERROR);
        return TRUE;
    }

    if (stepCount == 0) {
        ai_bridge_set_status(packetStatus);
        return TRUE;
    }

    for (i = 0; i < stepCount; i++) {
        if (!ai_controller_enqueue(steps[i].stickX, steps[i].stickY, steps[i].buttons, steps[i].frames)) {
            ai_controller_clear();
            ai_bridge_set_status(AI_BRIDGE_ERROR);
            return TRUE;
        }
    }

    ai_bridge_set_status(AI_BRIDGE_EXECUTING);
    return TRUE;
}

static void ai_bridge_write_telemetry(void) {
    const char *path = ai_bridge_path("SM64_AI_STATE_FILE", AI_BRIDGE_STATE_FILE);
    char temporaryPath[512];
    FILE *file;
    s32 grounded;
    s32 swimming;
    s32 cutscene;
    s16 cameraYaw = 0;

    if (gCurrentArea == NULL || gMarioState == NULL || gMarioState->action == ACT_UNINITIALIZED) {
        return;
    }

    if (snprintf(temporaryPath, sizeof(temporaryPath), "%s.tmp", path) >= (s32) sizeof(temporaryPath)) {
        return;
    }

    grounded = (gMarioState->action & (ACT_FLAG_AIR | ACT_FLAG_SWIMMING | ACT_FLAG_METAL_WATER)) == 0;
    swimming = (gMarioState->action & (ACT_FLAG_SWIMMING | ACT_FLAG_METAL_WATER)) != 0;
    cutscene = (gMarioState->action & ACT_GROUP_MASK) == ACT_GROUP_CUTSCENE;
    if (gCamera != NULL) {
        cameraYaw = gCamera->yaw;
    }

    file = fopen(temporaryPath, "w");
    if (file == NULL) {
        return;
    }

    fprintf(file,
            "{\n"
            "  \"level\": %d,\n"
            "  \"area\": %d,\n"
            "  \"position\": {\"x\": %.2f, \"y\": %.2f, \"z\": %.2f},\n"
            "  \"velocity\": {\"x\": %.2f, \"y\": %.2f, \"z\": %.2f},\n"
            "  \"forward_speed\": %.2f,\n"
            "  \"facing_yaw\": %d,\n"
            "  \"camera_yaw\": %d,\n"
            "  \"action\": %u,\n"
            "  \"grounded\": %s,\n"
            "  \"swimming\": %s,\n"
            "  \"cutscene\": %s,\n"
            "  \"health\": %d\n"
            "}\n",
            gCurrLevelNum, gCurrAreaIndex,
            gMarioState->pos[0], gMarioState->pos[1], gMarioState->pos[2],
            gMarioState->vel[0], gMarioState->vel[1], gMarioState->vel[2],
            gMarioState->forwardVel, gMarioState->faceAngle[1], cameraYaw, gMarioState->action,
            grounded ? "true" : "false", swimming ? "true" : "false",
            cutscene ? "true" : "false", gMarioState->health);
    fclose(file);
    remove(path);
    rename(temporaryPath, path);
}

void ai_bridge_update(void) {
    s32 active = ai_controller_is_active();

    if (sWasActive && !active && sStatus == AI_BRIDGE_EXECUTING) {
        ai_bridge_set_status(AI_BRIDGE_COMPLETE);
    } else if (active && sStatus == AI_BRIDGE_READY) {
        ai_bridge_set_status(AI_BRIDGE_EXECUTING);
    }

    if (sStatus == AI_BRIDGE_COMPLETE && sCompleteTimer > 0 && --sCompleteTimer == 0) {
        ai_bridge_set_status(AI_BRIDGE_READY);
    }

    if (!active && gCurrentArea != NULL) {
        ai_bridge_load_packet();
    }

    if (++sTelemetryTimer >= AI_BRIDGE_TELEMETRY_INTERVAL) {
        sTelemetryTimer = 0;
        ai_bridge_write_telemetry();
    }

    sWasActive = ai_controller_is_active();
}

void ai_bridge_render_status(void) {
    // Status remains available through .sm64-ai-status. The colorful HUD text
    // renderer is deliberately avoided here: on the PC renderer, changing the
    // label while a command begins can produce an invalid texture display list.
}

#else

void ai_bridge_update(void) {
}

void ai_bridge_render_status(void) {
}

#endif
