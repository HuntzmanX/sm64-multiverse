#include <ultra64.h>

#include "sm64.h"
#include "ai_bridge.h"
#include "ai_controller.h"

#ifndef TARGET_N64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "area.h"
#include "behavior_data.h"
#include "camera.h"
#include "dialog_ids.h"
#include "engine/math_util.h"
#include "game_init.h"
#include "ingame_menu.h"
#include "level_update.h"
#include "memory.h"
#include "object_constants.h"
#include "object_fields.h"
#include "object_list_processor.h"
#include "segment2.h"

#define AI_BRIDGE_COMMAND_FILE ".sm64-ai-command"
#define AI_BRIDGE_STATE_FILE ".sm64-ai-state.json"
#define AI_BRIDGE_STATUS_FILE ".sm64-ai-status"
#define AI_BRIDGE_MAX_PACKET_STEPS 32
#define AI_BRIDGE_MAX_STEP_FRAMES 300
#define AI_BRIDGE_MAX_TOTAL_FRAMES 900
#define AI_BRIDGE_TELEMETRY_INTERVAL 15
#define AI_BRIDGE_COMPLETE_FRAMES 60
#define AI_BRIDGE_MAX_NEARBY_OBJECTS 12
#define AI_BRIDGE_OBJECT_RADIUS 4000.0f
#define AI_BRIDGE_DIALOG_ASCII_LENGTH 80
#define AI_BRIDGE_DIALOG_ENCODED_LENGTH 96
#define AI_BRIDGE_ALLOWED_BUTTONS \
    (A_BUTTON | B_BUTTON | Z_TRIG | R_TRIG | U_CBUTTONS | D_CBUTTONS | L_CBUTTONS | R_CBUTTONS)

struct AiBridgeStep {
    s32 type;
    s8 stickX;
    s8 stickY;
    u16 buttons;
    u16 frames;
    char text[AI_BRIDGE_DIALOG_ASCII_LENGTH + 1];
};

enum AiBridgeStepType {
    AI_BRIDGE_STEP_INPUT,
    AI_BRIDGE_STEP_WAIT,
    AI_BRIDGE_STEP_SAY,
};

struct AiBridgeNearbyObject {
    struct Object *object;
    const char *type;
    f32 distance;
    s32 threat;
};

static enum AiBridgeStatus sStatus = AI_BRIDGE_READY;
static s32 sWasActive;
static s32 sTelemetryTimer;
static s32 sCompleteTimer;
static u8 sDialogueText[AI_BRIDGE_DIALOG_ENCODED_LENGTH];
static s32 sDialogueTimer;

static s32 ai_bridge_has_behavior(struct Object *object, const BehaviorScript *behavior) {
    return object->behavior == segmented_to_virtual(behavior);
}

static const char *ai_bridge_object_type(struct Object *object, s32 *threat) {
    *threat = FALSE;

    if (ai_bridge_has_behavior(object, bhvRedCoin)) return "red_coin";
    if (ai_bridge_has_behavior(object, bhvYellowCoin)
        || ai_bridge_has_behavior(object, bhvMovingYellowCoin)
        || ai_bridge_has_behavior(object, bhvOneCoin)
        || ai_bridge_has_behavior(object, bhvSingleCoinGetsSpawned)
        || ai_bridge_has_behavior(object, bhvTemporaryYellowCoin)) return "yellow_coin";
    if (ai_bridge_has_behavior(object, bhvMovingBlueCoin)
        || ai_bridge_has_behavior(object, bhvBlueCoinSliding)
        || ai_bridge_has_behavior(object, bhvBlueCoinJumping)
        || ai_bridge_has_behavior(object, bhvMrIBlueCoin)) return "blue_coin";
    if (ai_bridge_has_behavior(object, bhvStar)) return "star";
    if (ai_bridge_has_behavior(object, bhvDoor)
        || ai_bridge_has_behavior(object, bhvDoorWarp)
        || ai_bridge_has_behavior(object, bhvStarDoor)) return "door";
    if (ai_bridge_has_behavior(object, bhvBobombBuddy)
        || ai_bridge_has_behavior(object, bhvBobombBuddyOpensCannon)) return "bobomb_buddy";

    *threat = TRUE;
    if (ai_bridge_has_behavior(object, bhvGoomba)) return "goomba";
    if (ai_bridge_has_behavior(object, bhvBobomb)) return "bobomb";
    if (ai_bridge_has_behavior(object, bhvKoopa)) return "koopa";
    if (ai_bridge_has_behavior(object, bhvChainChomp)) return "chain_chomp";
    if (ai_bridge_has_behavior(object, bhvPiranhaPlant)) return "piranha_plant";
    if (ai_bridge_has_behavior(object, bhvBoo)) return "boo";
    if (ai_bridge_has_behavior(object, bhvFlyGuy)) return "fly_guy";
    if (ai_bridge_has_behavior(object, bhvChuckya)) return "chuckya";
    if (ai_bridge_has_behavior(object, bhvSpindrift)) return "spindrift";
    if (ai_bridge_has_behavior(object, bhvScuttlebug)) return "scuttlebug";
    if (ai_bridge_has_behavior(object, bhvSnufit)) return "snufit";
    if (ai_bridge_has_behavior(object, bhvSkeeter)) return "skeeter";

    *threat = FALSE;
    return NULL;
}

static void ai_bridge_insert_nearby_object(struct AiBridgeNearbyObject *nearby, s32 *count,
                                            struct Object *object, const char *type,
                                            f32 distance, s32 threat) {
    s32 index = *count;
    s32 i;

    if (index < AI_BRIDGE_MAX_NEARBY_OBJECTS) {
        (*count)++;
    } else if (distance >= nearby[index - 1].distance) {
        return;
    } else {
        index--;
    }

    for (i = index; i > 0 && distance < nearby[i - 1].distance; i--) {
        nearby[i] = nearby[i - 1];
    }
    nearby[i].object = object;
    nearby[i].type = type;
    nearby[i].distance = distance;
    nearby[i].threat = threat;
}

static s32 ai_bridge_decode_hex(const char *hex, char *text, s32 capacity) {
    s32 length = strlen(hex);
    s32 i;

    if (length == 0 || (length & 1) != 0 || length / 2 >= capacity) {
        return FALSE;
    }
    for (i = 0; i < length; i += 2) {
        unsigned int value;
        if (sscanf(&hex[i], "%2x", &value) != 1 || value == 0 || value > 0x7F) {
            return FALSE;
        }
        text[i / 2] = value;
    }
    text[length / 2] = '\0';
    return TRUE;
}

static u8 ai_bridge_dialog_char(char c) {
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
        return ASCII_TO_DIALOG(c);
    }
    switch (c) {
        case '\'': return 0x3E;
        case '.': return 0x3F;
        case ',': return DIALOG_CHAR_COMMA;
        case ' ': return DIALOG_CHAR_SPACE;
        case '-': return 0x9F;
        case ':': return 0xE6;
        case '!': return 0xF2;
        case '%': return 0xF3;
        case '?': return 0xF4;
        default: return DIALOG_CHAR_SPACE;
    }
}

static void ai_bridge_show_dialogue(const char *text, s32 frames) {
    const char *prefix = "MARIO: ";
    s32 output = 0;
    s32 column = 0;
    s32 i;

    for (i = 0; prefix[i] != '\0' && output < AI_BRIDGE_DIALOG_ENCODED_LENGTH - 1; i++) {
        sDialogueText[output++] = ai_bridge_dialog_char(prefix[i]);
        column++;
    }
    for (i = 0; text[i] != '\0' && output < AI_BRIDGE_DIALOG_ENCODED_LENGTH - 1; i++) {
        if (column >= 34) {
            sDialogueText[output++] = DIALOG_CHAR_NEWLINE;
            column = 0;
            if (text[i] == ' ') continue;
        }
        sDialogueText[output++] = ai_bridge_dialog_char(text[i]);
        column++;
    }
    sDialogueText[output] = DIALOG_CHAR_TERMINATOR;
    sDialogueTimer = frames;
}

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
        case AI_BRIDGE_CANCELLED:
            return "CANCELLED";
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
    s32 sayCount = 0;
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
        char encodedText[(AI_BRIDGE_DIALOG_ASCII_LENGTH * 2) + 1];

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
            steps[stepCount].type = AI_BRIDGE_STEP_INPUT;
            steps[stepCount].text[0] = '\0';
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
            steps[stepCount].type = AI_BRIDGE_STEP_WAIT;
            steps[stepCount].text[0] = '\0';
            stepCount++;
            totalFrames += frames;
        } else if (sscanf(line, "SAY %d %160s", &frames, encodedText) == 2) {
            if (stepCount != 0 || sayCount != 0 || frames < 1
                || frames > AI_BRIDGE_MAX_STEP_FRAMES
                || totalFrames + frames > AI_BRIDGE_MAX_TOTAL_FRAMES
                || !ai_bridge_decode_hex(encodedText, steps[stepCount].text,
                                         sizeof(steps[stepCount].text))) {
                valid = FALSE;
                break;
            }
            steps[stepCount].stickX = 0;
            steps[stepCount].stickY = 0;
            steps[stepCount].buttons = 0;
            steps[stepCount].frames = frames;
            steps[stepCount].type = AI_BRIDGE_STEP_SAY;
            stepCount++;
            sayCount++;
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
        if (steps[i].type == AI_BRIDGE_STEP_SAY) {
            ai_bridge_show_dialogue(steps[i].text, steps[i].frames);
        }
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
    struct AiBridgeNearbyObject nearby[AI_BRIDGE_MAX_NEARBY_OBJECTS];
    s32 nearbyCount = 0;
    s32 listIndex;
    s32 i;

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

    for (listIndex = 0; listIndex < NUM_OBJ_LISTS; listIndex++) {
        struct ObjectNode *list = &gObjectLists[listIndex];
        struct ObjectNode *node;
        for (node = list->next; node != list; node = node->next) {
            struct Object *object = (struct Object *) node;
            const char *type;
            s32 threat;
            f32 dx;
            f32 dy;
            f32 dz;
            f32 distance;

            if (object == gMarioObject || !(object->activeFlags & ACTIVE_FLAG_ACTIVE)
                || (object->activeFlags & ACTIVE_FLAG_IN_DIFFERENT_ROOM)) {
                continue;
            }
            type = ai_bridge_object_type(object, &threat);
            if (type == NULL) continue;
            dx = object->oPosX - gMarioState->pos[0];
            dy = object->oPosY - gMarioState->pos[1];
            dz = object->oPosZ - gMarioState->pos[2];
            distance = sqrtf(dx * dx + dy * dy + dz * dz);
            if (distance <= AI_BRIDGE_OBJECT_RADIUS) {
                ai_bridge_insert_nearby_object(nearby, &nearbyCount, object, type, distance, threat);
            }
        }
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
            "  \"health\": %d,\n"
            "  \"nearby_objects\": [\n",
            gCurrLevelNum, gCurrAreaIndex,
            gMarioState->pos[0], gMarioState->pos[1], gMarioState->pos[2],
            gMarioState->vel[0], gMarioState->vel[1], gMarioState->vel[2],
            gMarioState->forwardVel, gMarioState->faceAngle[1], cameraYaw, gMarioState->action,
            grounded ? "true" : "false", swimming ? "true" : "false",
            cutscene ? "true" : "false", gMarioState->health);
    for (i = 0; i < nearbyCount; i++) {
        struct Object *object = nearby[i].object;
        f32 dx = object->oPosX - gMarioState->pos[0];
        f32 dz = object->oPosZ - gMarioState->pos[2];
        s16 targetYaw = atan2s(dz, dx);
        s16 relativeYaw = targetYaw - cameraYaw;
        s32 stickX = coss(relativeYaw) * 80.0f;
        s32 stickY = -sins(relativeYaw) * 80.0f;
        fprintf(file,
                "    {\"type\": \"%s\", \"distance\": %.1f, \"threat\": %s, "
                "\"position\": {\"x\": %.1f, \"y\": %.1f, \"z\": %.1f}, "
                "\"stick_toward\": {\"x\": %d, \"y\": %d}}%s\n",
                nearby[i].type, nearby[i].distance, nearby[i].threat ? "true" : "false",
                object->oPosX, object->oPosY, object->oPosZ, stickX, stickY,
                i + 1 < nearbyCount ? "," : "");
    }
    fprintf(file, "  ]\n}\n");
    fclose(file);
    remove(path);
    rename(temporaryPath, path);
}

void ai_bridge_update(void) {
    s32 active = ai_controller_is_active();
    s32 cancelled = ai_controller_take_cancelled();

    if (sDialogueTimer > 0) {
        sDialogueTimer--;
    }

    if (cancelled) {
        ai_bridge_set_status(AI_BRIDGE_CANCELLED);
        sDialogueTimer = 0;
    } else if (sWasActive && !active && sStatus == AI_BRIDGE_EXECUTING) {
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

void ai_bridge_render_dialogue(void) {
    if (sDialogueTimer <= 0 || get_dialog_id() != DIALOG_NONE) {
        return;
    }

    create_dl_ortho_matrix();
    create_dl_translation_matrix(MENU_MTX_PUSH, 18.0f, 72.0f, 0.0f);
    create_dl_scale_matrix(MENU_MTX_NOPUSH, 2.2f, 0.7f, 1.0f);
    gDPSetEnvColor(gDisplayListHead++, 0, 0, 0, 170);
    gSPDisplayList(gDisplayListHead++, dl_draw_text_bg_box);
    gSPPopMatrix(gDisplayListHead++, G_MTX_MODELVIEW);

    gSPDisplayList(gDisplayListHead++, dl_ia_text_begin);
    gDPSetEnvColor(gDisplayListHead++, 255, 255, 255, 255);
    print_generic_string(28, 58, sDialogueText);
    gSPDisplayList(gDisplayListHead++, dl_ia_text_end);
}

#else

void ai_bridge_update(void) {
}

void ai_bridge_render_status(void) {
}

void ai_bridge_render_dialogue(void) {
}

#endif
