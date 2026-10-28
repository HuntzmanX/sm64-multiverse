#include <ultra64.h>

#include "sm64.h"
#include "ai_bridge.h"
#include "ai_controller.h"
#include "ai_freecam.h"
#include "ai_input.h"

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
#include "gfx_dimensions.h"
#include "hud.h"
#include "ingame_menu.h"
#include "level_update.h"
#include "memory.h"
#include "object_constants.h"
#include "object_fields.h"
#include "object_list_processor.h"
#include "print.h"
#include "segment2.h"

#define AI_BRIDGE_COMMAND_FILE ".sm64-ai-command"
#define AI_BRIDGE_STATE_FILE ".sm64-ai-state.json"
#define AI_BRIDGE_STATUS_FILE ".sm64-ai-status"
#define AI_BRIDGE_MAX_PACKET_STEPS 32
#define AI_BRIDGE_MAX_STEP_FRAMES 300
#define AI_BRIDGE_MAX_TOTAL_FRAMES 900
#define AI_BRIDGE_TELEMETRY_INTERVAL 15
#define AI_BRIDGE_COMPLETE_FRAMES 60
#define AI_BRIDGE_MAX_NEARBY_OBJECTS 20
#define AI_BRIDGE_OBJECT_RADIUS 8000.0f
#define AI_BRIDGE_DIALOG_ASCII_LENGTH 80
#define AI_BRIDGE_DIALOG_ENCODED_LENGTH 96
#define AI_BRIDGE_DIALOG_HOLD_FRAMES 150
#define AI_BRIDGE_DIALOG_FADE_FRAMES 15
#define AI_BRIDGE_SIDEBAR_STEPS 7
#define AI_BRIDGE_SIDEBAR_WIDTH 118.0f
#define AI_BRIDGE_SIDEBAR_COLUMNS 18
#define AI_TECHNIQUE_BACKFLIP (1 << 0)
#define AI_TECHNIQUE_SIDE_FLIP (1 << 1)
#define AI_TECHNIQUE_LONG_JUMP (1 << 2)
#define AI_TECHNIQUE_SLIDE_JUMP (1 << 3)
#define AI_TECHNIQUE_GROUND_POUND (1 << 4)
#define AI_TECHNIQUE_DIVE (1 << 5)
#define AI_BRIDGE_ALLOWED_BUTTONS \
    (A_BUTTON | B_BUTTON | Z_TRIG | R_TRIG | U_CBUTTONS | D_CBUTTONS | L_CBUTTONS | R_CBUTTONS)

struct AiBridgeStep {
    s32 type;
    s8 stickX;
    s8 stickY;
    s16 targetId;
    s8 targetDirection;
    u8 targetSpeed;
    u8 waypointMode;
    u16 stopDistance;
    f32 waypointX;
    f32 waypointY;
    f32 waypointZ;
    u16 buttons;
    u16 frames;
    char text[AI_BRIDGE_DIALOG_ASCII_LENGTH + 1];
};

enum AiBridgeStepType {
    AI_BRIDGE_STEP_INPUT,
    AI_BRIDGE_STEP_WAIT,
    AI_BRIDGE_STEP_SAY,
    AI_BRIDGE_STEP_TARGET,
    AI_BRIDGE_STEP_WAYPOINT,
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
static s32 sDialogueStepTimer;
static s32 sDoorTransitionActive;
static s32 sDoorTransitionWarp;
static s32 sDoorTransitionSawSpawn;
static u32 sDoorTransitionSequence;
static s16 sDoorSourceId;
static s16 sDoorDestinationId;
static s16 sDoorFromLevel;
static s16 sDoorFromArea;
static s16 sDoorFromRoom;
static s16 sDoorToLevel;
static s16 sDoorToArea;
static s16 sDoorToRoom;
static u32 sTechniqueMask;
static u32 sPreviousMarioAction = ACT_UNINITIALIZED;

ALIGNED8 static const Texture sAiReadyTexture[] = {
#include "assets/ai-ready.rgba16.inc.c"
};

ALIGNED8 static const Texture sAiThinkingTexture[] = {
#include "assets/ai-thinking.rgba16.inc.c"
};

ALIGNED8 static const Texture sAiActingTexture[] = {
#include "assets/ai_acting.rgba16.inc.c"
};

ALIGNED8 static const Texture sAiErrorTexture[] = {
#include "assets/ai-error.rgba16.inc.c"
};

ALIGNED8 static const Texture sAiCancelledTexture[] = {
#include "assets/ai-cancelled.rgba16.inc.c"
};

static s32 ai_bridge_has_behavior(struct Object *object, const BehaviorScript *behavior) {
    return object->behavior == segmented_to_virtual(behavior);
}

static s32 ai_bridge_is_door_action(u32 action) {
    return action == ACT_PULLING_DOOR || action == ACT_PUSHING_DOOR
        || action == ACT_ENTERING_STAR_DOOR || action == ACT_WARP_DOOR_SPAWN;
}

static void ai_bridge_finish_door_transition(void) {
    sDoorTransitionActive = FALSE;
    sDoorTransitionSequence++;
    sDoorToLevel = gCurrLevelNum;
    sDoorToArea = gCurrAreaIndex;
    sDoorToRoom = gMarioCurrentRoom;
}

static void ai_bridge_track_door_transition(void) {
    u32 action;

    if (gMarioState == NULL || gMarioState->action == ACT_UNINITIALIZED) {
        return;
    }
    action = gMarioState->action;

    if (!sDoorTransitionActive && ai_bridge_is_door_action(action)) {
        sDoorTransitionActive = TRUE;
        sDoorTransitionWarp = (action == ACT_WARP_DOOR_SPAWN)
            || ((action == ACT_PULLING_DOOR || action == ACT_PUSHING_DOOR)
                && (gMarioState->actionArg & 4));
        sDoorTransitionSawSpawn = action == ACT_WARP_DOOR_SPAWN;
        sDoorSourceId = gMarioState->usedObj != NULL
            ? ai_controller_object_id(gMarioState->usedObj) : 0;
        sDoorDestinationId = sDoorSourceId;
        sDoorFromLevel = gCurrLevelNum;
        sDoorFromArea = gCurrAreaIndex;
        sDoorFromRoom = gMarioCurrentRoom;
    }

    if (!sDoorTransitionActive) {
        return;
    }

    if (action == ACT_WARP_DOOR_SPAWN) {
        sDoorTransitionWarp = TRUE;
        sDoorTransitionSawSpawn = TRUE;
        if (gMarioState->usedObj != NULL) {
            sDoorDestinationId = ai_controller_object_id(gMarioState->usedObj);
        }
    }

    if ((!sDoorTransitionWarp && !ai_bridge_is_door_action(action))
        || (sDoorTransitionWarp && sDoorTransitionSawSpawn
            && action != ACT_WARP_DOOR_SPAWN)) {
        ai_bridge_finish_door_transition();
    }
}

static void ai_bridge_track_techniques(void) {
    u32 action;
    s32 instructionActive;

    if (gMarioState == NULL || gMarioState->action == ACT_UNINITIALIZED) {
        return;
    }
    action = gMarioState->action;
    instructionActive = sStatus == AI_BRIDGE_EXECUTING || ai_controller_is_active();

    if (instructionActive && action != sPreviousMarioAction) {
        switch (action) {
            case ACT_BACKFLIP:
                sTechniqueMask |= AI_TECHNIQUE_BACKFLIP;
                break;
            case ACT_SIDE_FLIP:
                sTechniqueMask |= AI_TECHNIQUE_SIDE_FLIP;
                break;
            case ACT_LONG_JUMP:
                sTechniqueMask |= AI_TECHNIQUE_LONG_JUMP;
                break;
            case ACT_GROUND_POUND:
                sTechniqueMask |= AI_TECHNIQUE_GROUND_POUND;
                break;
            case ACT_DIVE:
                sTechniqueMask |= AI_TECHNIQUE_DIVE;
                break;
            case ACT_JUMP:
            case ACT_HOLD_JUMP:
                if (sPreviousMarioAction == ACT_BUTT_SLIDE
                    || sPreviousMarioAction == ACT_HOLD_BUTT_SLIDE) {
                    sTechniqueMask |= AI_TECHNIQUE_SLIDE_JUMP;
                }
                break;
        }
    }
    sPreviousMarioAction = action;
}

static void ai_bridge_write_techniques(FILE *file) {
    static const struct {
        u32 mask;
        const char *name;
    } techniques[] = {
        { AI_TECHNIQUE_BACKFLIP, "backflip" },
        { AI_TECHNIQUE_SIDE_FLIP, "side_flip" },
        { AI_TECHNIQUE_LONG_JUMP, "long_jump" },
        { AI_TECHNIQUE_SLIDE_JUMP, "slide_jump" },
        { AI_TECHNIQUE_GROUND_POUND, "ground_pound" },
        { AI_TECHNIQUE_DIVE, "dive" },
    };
    s32 first = TRUE;
    u32 i;

    fprintf(file, "  \"techniques_seen\": [");
    for (i = 0; i < sizeof(techniques) / sizeof(techniques[0]); i++) {
        if (!(sTechniqueMask & techniques[i].mask)) continue;
        fprintf(file, "%s\"%s\"", first ? "" : ", ", techniques[i].name);
        first = FALSE;
    }
    fprintf(file, "],\n");
}

static const char *ai_bridge_camera_mode_text(void) {
    if (!ai_freecam_is_enabled()) return "vanilla";
    switch (ai_freecam_get_mode()) {
        case AI_CAMERA_MODE_FOCUS: return "focus";
        case AI_CAMERA_MODE_FOLLOW: return "follow";
        case AI_CAMERA_MODE_FIRST_PERSON: return "first_person";
        default: return "free";
    }
}

static const char *ai_bridge_exclamation_box_type(s32 contents) {
    switch (contents) {
        case 0: return "wing_cap_box";
        case 1: return "metal_cap_box";
        case 2: return "vanish_cap_box";
        case 3: return "koopa_shell_box";
        case 4: return "coin_box";
        case 5: return "three_coin_box";
        case 6: return "ten_coin_box";
        case 7: return "one_up_box";
        case 8: return "star_box";
        case 9: return "running_one_up_box";
        case 10: case 11: case 12: case 13: case 14: return "star_box";
        default: return "exclamation_box";
    }
}

static const char *ai_bridge_cap_switch_type(s32 capType) {
    switch (capType) {
        case 0: return "wing_cap_switch";
        case 1: return "metal_cap_switch";
        case 2: return "vanish_cap_switch";
        default: return "cap_switch";
    }
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
    if (ai_bridge_has_behavior(object, bhvToadMessage)) return "toad";
    if (ai_bridge_has_behavior(object, bhvRacingPenguin)) return "racing_penguin";
    if (ai_bridge_has_behavior(object, bhvBreakableBox)) return "breakable_box";
    if (ai_bridge_has_behavior(object, bhvBreakableBoxSmall)) return "small_breakable_box";
    if (ai_bridge_has_behavior(object, bhvExclamationBox)) {
        return ai_bridge_exclamation_box_type(object->oBehParams2ndByte);
    }
    if (ai_bridge_has_behavior(object, bhvTree)) return "tree";
    if (ai_bridge_has_behavior(object, bhvPushableMetalBox)) return "pushable_metal_box";
    if (ai_bridge_has_behavior(object, bhvCannon)) return "cannon";
    if (ai_bridge_has_behavior(object, bhvCannonClosed)) return "closed_cannon";
    if (ai_bridge_has_behavior(object, bhvMessagePanel)
        || ai_bridge_has_behavior(object, bhvSignOnWall)) return "sign";
    if (ai_bridge_has_behavior(object, bhvPoleGrabbing)
        || ai_bridge_has_behavior(object, bhvGiantPole)) return "pole";
    if (ai_bridge_has_behavior(object, bhvCapSwitch)) {
        return ai_bridge_cap_switch_type(object->oBehParams2ndByte);
    }
    if (ai_bridge_has_behavior(object, bhvBlueCoinSwitch)) return "blue_coin_switch";
    if (ai_bridge_has_behavior(object, bhvFloorSwitchHardcodedModel)
        || ai_bridge_has_behavior(object, bhvFloorSwitchAnimatesObject)
        || ai_bridge_has_behavior(object, bhvFloorSwitchGrills)
        || ai_bridge_has_behavior(object, bhvFloorSwitchHiddenObjects)) return "purple_switch";
    if (ai_bridge_has_behavior(object, bhvRecoveryHeart)) return "recovery_heart";
    if (ai_bridge_has_behavior(object, bhvWarpPipe)) return "warp_pipe";
    if (ai_bridge_has_behavior(object, bhvKoopaShell)) return "koopa_shell";
    if (ai_bridge_has_behavior(object, bhvWingCap)) return "wing_cap";
    if (ai_bridge_has_behavior(object, bhvMetalCap)) return "metal_cap";
    if (ai_bridge_has_behavior(object, bhvVanishCap)) return "vanish_cap";
    if (ai_bridge_has_behavior(object, bhv1upWalking)
        || ai_bridge_has_behavior(object, bhv1upRunningAway)
        || ai_bridge_has_behavior(object, bhv1upSliding)
        || ai_bridge_has_behavior(object, bhv1upJumpOnApproach)) return "one_up";

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

static void ai_bridge_encode_hud_text(const char *text, u8 *encoded, s32 capacity) {
    s32 i;

    for (i = 0; text[i] != '\0' && i < capacity - 1
                && i < AI_BRIDGE_SIDEBAR_COLUMNS; i++) {
        encoded[i] = ai_bridge_dialog_char(text[i]);
    }
    encoded[i] = DIALOG_CHAR_TERMINATOR;
}

static void ai_bridge_append_button_names(char *text, s32 capacity, u16 buttons) {
    static const struct {
        u16 mask;
        const char *name;
    } names[] = {
        { A_BUTTON, " A" }, { B_BUTTON, " B" }, { Z_TRIG, " Z" }, { R_TRIG, " R" },
        { U_CBUTTONS, " CU" }, { D_CBUTTONS, " CD" },
        { L_CBUTTONS, " CL" }, { R_CBUTTONS, " CR" },
    };
    s32 i;

    for (i = 0; i < (s32) (sizeof(names) / sizeof(names[0])); i++) {
        if (buttons & names[i].mask) {
            s32 length = strlen(text);
            if (length < capacity - 1) {
                strncat(text, names[i].name, capacity - length - 1);
            }
        }
    }
}

static void ai_bridge_describe_step(const struct AiControllerDisplayStep *step,
                                    s32 current, char *text, s32 capacity) {
    const char *prefix = current ? "! " : "  ";

    if (step->type == AI_CONTROLLER_DISPLAY_TARGET) {
        struct Object *object = ai_controller_object_from_id(step->targetId);
        s32 threat;
        const char *type = object != NULL ? ai_bridge_object_type(object, &threat) : NULL;
        s32 output;
        s32 i;

        output = snprintf(text, capacity, "%s%s ", prefix,
                          step->targetDirection > 0 ? "TO" : "FLEE");
        if (type == NULL) {
            snprintf(&text[output], capacity - output, "ID %d", step->targetId);
        } else {
            for (i = 0; type[i] != '\0' && output < capacity - 1; i++) {
                char c = type[i] == '_' ? ' ' : type[i];
                text[output++] = c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c;
            }
            text[output] = '\0';
        }
        ai_bridge_append_button_names(text, capacity, step->buttons);
        return;
    }

    if (step->type == AI_CONTROLLER_DISPLAY_WAYPOINT) {
        snprintf(text, capacity, "%sTO %s", prefix,
                 step->waypointMode == AI_WAYPOINT_CAMERA ? "CAMERA" : "LANDMARK");
        return;
    }

    if (step->stickX == 0 && step->stickY == 0) {
        if (step->buttons == 0) {
            snprintf(text, capacity, "%s%s", prefix,
                     current && sDialogueStepTimer > 0 ? "SAY" : "WAIT");
        } else {
            snprintf(text, capacity, "%sPRESS", prefix);
            ai_bridge_append_button_names(text, capacity, step->buttons);
        }
        return;
    }

    if (step->stickY > 20) {
        snprintf(text, capacity, "%sFORWARD", prefix);
    } else if (step->stickY < -20) {
        snprintf(text, capacity, "%sBACK", prefix);
    } else if (step->stickX < 0) {
        snprintf(text, capacity, "%sLEFT", prefix);
    } else {
        snprintf(text, capacity, "%sRIGHT", prefix);
    }
    ai_bridge_append_button_names(text, capacity, step->buttons);
}

static void ai_bridge_render_sidebar(void) {
    struct AiControllerDisplayStep steps[AI_BRIDGE_SIDEBAR_STEPS];
    u8 encoded[32];
    char text[32];
    s32 count;
    s32 i;
    s32 right;
    s32 left;
    s32 y;

    if (!ai_input_sidebar_is_visible() || GFX_DIMENSIONS_ASPECT_RATIO < 1.5f) return;

    right = GFX_DIMENSIONS_RECT_FROM_RIGHT_EDGE(6);
    left = right - (s32) AI_BRIDGE_SIDEBAR_WIDTH;
    create_dl_translation_matrix(MENU_MTX_PUSH, left, 195.0f, 0.0f);
    create_dl_scale_matrix(MENU_MTX_NOPUSH, AI_BRIDGE_SIDEBAR_WIDTH / 130.0f, 1.7f, 1.0f);
    gDPSetEnvColor(gDisplayListHead++, 0, 0, 0, 155);
    gSPDisplayList(gDisplayListHead++, dl_draw_text_bg_box);
    gSPPopMatrix(gDisplayListHead++, G_MTX_MODELVIEW);

    gSPDisplayList(gDisplayListHead++, dl_ia_text_begin);
    gDPSetEnvColor(gDisplayListHead++, 255, 220, 70, 255);
    ai_bridge_encode_hud_text("ACTION QUEUE", encoded, sizeof(encoded));
    print_generic_string(left + 7, 180, encoded);

    count = ai_controller_get_display_steps(steps, AI_BRIDGE_SIDEBAR_STEPS);
    y = 162;
    if (count == 0) {
        const char *emptyText = sStatus == AI_BRIDGE_THINKING ? "PLANNING..." : "QUEUE EMPTY";
        gDPSetEnvColor(gDisplayListHead++, 210, 210, 210, 255);
        ai_bridge_encode_hud_text(emptyText, encoded, sizeof(encoded));
        print_generic_string(left + 7, y, encoded);
    } else {
        for (i = 0; i < count; i++) {
            ai_bridge_describe_step(&steps[i], i == 0, text, sizeof(text));
            gDPSetEnvColor(gDisplayListHead++, i == 0 ? 255 : 190,
                          i == 0 ? 220 : 190, i == 0 ? 70 : 190, 255);
            ai_bridge_encode_hud_text(text, encoded, sizeof(encoded));
            print_generic_string(left + 7, y, encoded);
            y -= 14;
        }
    }

    gDPSetEnvColor(gDisplayListHead++, 135, 135, 135, 255);
    ai_bridge_encode_hud_text("F2 HIDE", encoded, sizeof(encoded));
    print_generic_string(left + 7, 62, encoded);
    gSPDisplayList(gDisplayListHead++, dl_ia_text_end);
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
    sDialogueTimer = AI_BRIDGE_DIALOG_HOLD_FRAMES + AI_BRIDGE_DIALOG_FADE_FRAMES;
    sDialogueStepTimer = frames;
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
    s32 referenceYaw = 0;
    s32 sawReferenceYaw = FALSE;
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
        s32 targetId;
        s32 targetDirection;
        s32 targetSpeed;
        s32 stopDistance;
        s32 waypointMode;
        f32 waypointX;
        f32 waypointY;
        f32 waypointZ;
        char statusText[32];
        char encodedText[(AI_BRIDGE_DIALOG_ASCII_LENGTH * 2) + 1];

        if (strcmp(line, "END\n") == 0 || strcmp(line, "END\r\n") == 0) {
            sawEnd = TRUE;
            break;
        }

        if (sscanf(line, "REFERENCE_YAW %d", &referenceYaw) == 1) {
            if (sawReferenceYaw || stepCount != 0 || referenceYaw < -32768 || referenceYaw > 32767) {
                valid = FALSE;
                break;
            }
            sawReferenceYaw = TRUE;
        } else if (sscanf(line, "TARGET %d %d %d %d %d %d", &targetId, &targetDirection,
                          &targetSpeed, &buttons, &frames, &stopDistance) == 6) {
            if (stepCount >= AI_BRIDGE_MAX_PACKET_STEPS || targetId < 1
                || targetId > OBJECT_POOL_CAPACITY
                || (targetDirection != 1 && targetDirection != -1)
                || targetSpeed < 1 || targetSpeed > 80 || buttons < 0
                || (buttons & ~AI_BRIDGE_ALLOWED_BUTTONS) != 0 || frames < 1
                || frames > AI_BRIDGE_MAX_STEP_FRAMES || stopDistance < 0
                || stopDistance > 8000 || totalFrames + frames > AI_BRIDGE_MAX_TOTAL_FRAMES) {
                valid = FALSE;
                break;
            }
            steps[stepCount].type = AI_BRIDGE_STEP_TARGET;
            steps[stepCount].targetId = targetId;
            steps[stepCount].targetDirection = targetDirection;
            steps[stepCount].targetSpeed = targetSpeed;
            steps[stepCount].stopDistance = stopDistance;
            steps[stepCount].buttons = buttons;
            steps[stepCount].frames = frames;
            steps[stepCount].text[0] = '\0';
            stepCount++;
            totalFrames += frames;
        } else if (sscanf(line, "WAYPOINT %f %f %f %d %d %d", &waypointX,
                          &waypointY, &waypointZ, &targetSpeed, &frames,
                          &stopDistance) == 6) {
            if (stepCount >= AI_BRIDGE_MAX_PACKET_STEPS || waypointX < -32768.0f
                || waypointX > 32767.0f || waypointY < -32768.0f
                || waypointY > 32767.0f || waypointZ < -32768.0f
                || waypointZ > 32767.0f || targetSpeed < 1 || targetSpeed > 80
                || frames < 1 || frames > AI_BRIDGE_MAX_STEP_FRAMES
                || stopDistance < 1 || stopDistance > 8000
                || totalFrames + frames > AI_BRIDGE_MAX_TOTAL_FRAMES) {
                valid = FALSE;
                break;
            }
            steps[stepCount].type = AI_BRIDGE_STEP_WAYPOINT;
            steps[stepCount].waypointMode = AI_WAYPOINT_STATIC;
            steps[stepCount].waypointX = waypointX;
            steps[stepCount].waypointY = waypointY;
            steps[stepCount].waypointZ = waypointZ;
            steps[stepCount].targetSpeed = targetSpeed;
            steps[stepCount].stopDistance = stopDistance;
            steps[stepCount].frames = frames;
            steps[stepCount].text[0] = '\0';
            stepCount++;
            totalFrames += frames;
        } else if (sscanf(line, "CAMERA %d %d %d", &targetSpeed, &frames,
                          &stopDistance) == 3) {
            if (stepCount >= AI_BRIDGE_MAX_PACKET_STEPS || targetSpeed < 1
                || targetSpeed > 80 || frames < 1 || frames > AI_BRIDGE_MAX_STEP_FRAMES
                || stopDistance < 1 || stopDistance > 8000
                || totalFrames + frames > AI_BRIDGE_MAX_TOTAL_FRAMES) {
                valid = FALSE;
                break;
            }
            waypointMode = AI_WAYPOINT_CAMERA;
            steps[stepCount].type = AI_BRIDGE_STEP_WAYPOINT;
            steps[stepCount].waypointMode = waypointMode;
            steps[stepCount].waypointX = 0.0f;
            steps[stepCount].waypointY = 0.0f;
            steps[stepCount].waypointZ = 0.0f;
            steps[stepCount].targetSpeed = targetSpeed;
            steps[stepCount].stopDistance = stopDistance;
            steps[stepCount].frames = frames;
            steps[stepCount].text[0] = '\0';
            stepCount++;
            totalFrames += frames;
        } else if (sscanf(line, "INPUT %d %d %d %d", &stickX, &stickY, &buttons, &frames) == 4) {
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

    // A cancelled OpenAI request can finish just after the player replaces it.
    // Discard that stale plan until the replacement request first announces a
    // new THINKING state.
    if (sStatus == AI_BRIDGE_CANCELLED) {
        return TRUE;
    }

    ai_controller_begin_plan();
    if (sawReferenceYaw) {
        ai_controller_set_reference_yaw((s16) referenceYaw);
    }

    for (i = 0; i < stepCount; i++) {
        if (steps[i].type == AI_BRIDGE_STEP_SAY) {
            ai_bridge_show_dialogue(steps[i].text, steps[i].frames);
        }
        if (steps[i].type == AI_BRIDGE_STEP_TARGET) {
            if (!ai_controller_enqueue_target(steps[i].targetId, steps[i].targetDirection,
                                              steps[i].targetSpeed, steps[i].buttons,
                                              steps[i].frames, steps[i].stopDistance)) {
                ai_controller_clear();
                ai_bridge_set_status(AI_BRIDGE_ERROR);
                return TRUE;
            }
        } else if (steps[i].type == AI_BRIDGE_STEP_WAYPOINT) {
            if (!ai_controller_enqueue_waypoint(steps[i].waypointX, steps[i].waypointY,
                                                steps[i].waypointZ, steps[i].targetSpeed,
                                                steps[i].frames, steps[i].stopDistance,
                                                steps[i].waypointMode)) {
                ai_controller_clear();
                ai_bridge_set_status(AI_BRIDGE_ERROR);
                return TRUE;
            }
        } else if (!ai_controller_enqueue(steps[i].stickX, steps[i].stickY,
                                          steps[i].buttons, steps[i].frames)) {
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
    s32 floorType;
    f32 floorNormalY;
    s16 cameraYaw = 0;
    f32 cameraX = 0.0f;
    f32 cameraY = 0.0f;
    f32 cameraZ = 0.0f;
    f32 cameraFocusX = 0.0f;
    f32 cameraFocusY = 0.0f;
    f32 cameraFocusZ = 0.0f;
    f32 cameraHorizontalDistance = 0.0f;
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
    floorType = gMarioState->floor != NULL ? gMarioState->floor->type : -1;
    floorNormalY = gMarioState->floor != NULL ? gMarioState->floor->normal.y : 0.0f;
    if (gCamera != NULL) {
        cameraYaw = gCamera->yaw;
        cameraX = gCamera->pos[0];
        cameraY = gCamera->pos[1];
        cameraZ = gCamera->pos[2];
        cameraFocusX = gCamera->focus[0];
        cameraFocusY = gCamera->focus[1];
        cameraFocusZ = gCamera->focus[2];
        cameraHorizontalDistance = sqrtf(
            (cameraX - gMarioState->pos[0]) * (cameraX - gMarioState->pos[0])
            + (cameraZ - gMarioState->pos[2]) * (cameraZ - gMarioState->pos[2]));
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
            "  \"room\": %d,\n"
            "  \"position\": {\"x\": %.2f, \"y\": %.2f, \"z\": %.2f},\n"
            "  \"velocity\": {\"x\": %.2f, \"y\": %.2f, \"z\": %.2f},\n"
            "  \"forward_speed\": %.2f,\n"
            "  \"facing_yaw\": %d,\n"
            "  \"camera_yaw\": %d,\n"
            "  \"camera\": {\"mode\": \"%s\", "
            "\"position\": {\"x\": %.2f, \"y\": %.2f, \"z\": %.2f}, "
            "\"focus\": {\"x\": %.2f, \"y\": %.2f, \"z\": %.2f}, "
            "\"horizontal_distance\": %.2f, \"height_delta\": %.2f},\n"
            "  \"action\": %u,\n"
            "  \"grounded\": %s,\n"
            "  \"swimming\": %s,\n"
            "  \"cutscene\": %s,\n"
            "  \"dialog\": {\"active\": %s, \"id\": %d},\n"
            "  \"health\": %d,\n"
            "  \"environment\": {\"floor_height\": %.1f, \"height_above_floor\": %.1f, "
            "\"floor_type\": %d, \"floor_normal_y\": %.3f, \"touching_wall\": %s, "
            "\"water_level\": %d},\n"
            "  \"target_result\": \"%s\",\n"
            "  \"door_transition\": {\"sequence\": %u, \"source_id\": %d, "
            "\"destination_id\": %d, \"from\": {\"level\": %d, \"area\": %d, \"room\": %d}, "
            "\"to\": {\"level\": %d, \"area\": %d, \"room\": %d}},\n",
            gCurrLevelNum, gCurrAreaIndex, gMarioCurrentRoom,
            gMarioState->pos[0], gMarioState->pos[1], gMarioState->pos[2],
            gMarioState->vel[0], gMarioState->vel[1], gMarioState->vel[2],
            gMarioState->forwardVel, gMarioState->faceAngle[1], cameraYaw,
            ai_bridge_camera_mode_text(), cameraX, cameraY, cameraZ,
            cameraFocusX, cameraFocusY, cameraFocusZ, cameraHorizontalDistance,
            cameraY - gMarioState->pos[1], gMarioState->action,
            grounded ? "true" : "false", swimming ? "true" : "false",
            cutscene ? "true" : "false",
            get_dialog_id() != DIALOG_NONE ? "true" : "false", get_dialog_id(),
            gMarioState->health,
            gMarioState->floorHeight, gMarioState->pos[1] - gMarioState->floorHeight,
            floorType, floorNormalY, gMarioState->wall != NULL ? "true" : "false",
            gMarioState->waterLevel,
            ai_controller_target_result_text(),
            sDoorTransitionSequence, sDoorSourceId, sDoorDestinationId,
            sDoorFromLevel, sDoorFromArea, sDoorFromRoom,
            sDoorToLevel, sDoorToArea, sDoorToRoom);
    ai_bridge_write_techniques(file);
    fprintf(file, "  \"nearby_objects\": [\n");
    for (i = 0; i < nearbyCount; i++) {
        struct Object *object = nearby[i].object;
        s32 objectId = ai_controller_object_id(object);
        f32 dx = object->oPosX - gMarioState->pos[0];
        f32 dy = object->oPosY - gMarioState->pos[1];
        f32 dz = object->oPosZ - gMarioState->pos[2];
        f32 horizontalDistance = sqrtf(dx * dx + dz * dz);
        s16 targetYaw = atan2s(dz, dx);
        s16 relativeYaw = targetYaw - cameraYaw;
        // Mario derives movement yaw with atan2s(-stickY, stickX).
        // SM64 yaw 0 points along +Z, so X uses sine and Y uses -cosine.
        s32 stickX = sins(relativeYaw) * 80.0f;
        s32 stickY = -coss(relativeYaw) * 80.0f;
        fprintf(file,
                "    {\"id\": %d, \"type\": \"%s\", \"distance\": %.1f, "
                "\"horizontal_distance\": %.1f, \"height_delta\": %.1f, \"threat\": %s, "
                "\"position\": {\"x\": %.1f, \"y\": %.1f, \"z\": %.1f}, "
                "\"stick_toward\": {\"x\": %d, \"y\": %d}}%s\n",
                objectId, nearby[i].type, nearby[i].distance, horizontalDistance, dy,
                nearby[i].threat ? "true" : "false",
                object->oPosX, object->oPosY, object->oPosZ, stickX, stickY,
                i + 1 < nearbyCount ? "," : "");
    }
    fprintf(file, "  ]\n}\n");
    fclose(file);
    remove(path);
    rename(temporaryPath, path);
}

void ai_bridge_update(void) {
    s32 active;
    s32 cancelled = ai_controller_take_cancelled();

    ai_bridge_track_door_transition();
    ai_bridge_track_techniques();
    active = ai_controller_is_active() || sDoorTransitionActive;

    if (sDialogueTimer > 0) {
        sDialogueTimer--;
    }
    if (sDialogueStepTimer > 0) {
        sDialogueStepTimer--;
    }

    if (cancelled) {
        ai_bridge_set_status(AI_BRIDGE_CANCELLED);
        sDialogueTimer = 0;
        sDialogueStepTimer = 0;
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

    sWasActive = ai_controller_is_active() || sDoorTransitionActive;
}

s32 ai_bridge_has_pending_instruction(void) {
    return sStatus == AI_BRIDGE_THINKING || sStatus == AI_BRIDGE_EXECUTING
        || sStatus == AI_BRIDGE_COMPLETE;
}

void ai_bridge_cancel_current(void) {
    ai_controller_clear();
    sDialogueTimer = 0;
    sDialogueStepTimer = 0;
    ai_bridge_set_status(AI_BRIDGE_CANCELLED);
}

void ai_bridge_render_status(void) {
    const u8 *texture;
    s32 x = GFX_DIMENSIONS_RECT_FROM_LEFT_EDGE(22);

    switch (sStatus) {
        case AI_BRIDGE_THINKING:
            texture = sAiThinkingTexture;
            break;
        case AI_BRIDGE_EXECUTING:
            texture = sAiActingTexture;
            break;
        case AI_BRIDGE_COMPLETE:
            texture = sAiReadyTexture;
            break;
        case AI_BRIDGE_ERROR:
            texture = sAiErrorTexture;
            break;
        case AI_BRIDGE_CANCELLED:
            texture = sAiCancelledTexture;
            break;
        default:
            texture = sAiReadyTexture;
            break;
    }

    gSPDisplayList(gDisplayListHead++, dl_hud_img_begin);
    render_hud_tex_lut(x, SCREEN_HEIGHT - HUD_TOP_Y - 16, texture);
    gSPDisplayList(gDisplayListHead++, dl_hud_img_end);
    ai_bridge_render_sidebar();
}

void ai_bridge_render_dialogue(void) {
    s32 fadeAlpha = 255;

    if (sDialogueTimer <= 0 || get_dialog_id() != DIALOG_NONE) {
        return;
    }
    if (sDialogueTimer <= AI_BRIDGE_DIALOG_FADE_FRAMES) {
        fadeAlpha = sDialogueTimer * 255 / AI_BRIDGE_DIALOG_FADE_FRAMES;
    }

    create_dl_ortho_matrix();
    create_dl_translation_matrix(MENU_MTX_PUSH, 18.0f, 72.0f, 0.0f);
    create_dl_scale_matrix(MENU_MTX_NOPUSH, 2.2f, 0.7f, 1.0f);
    gDPSetEnvColor(gDisplayListHead++, 0, 0, 0, 170 * fadeAlpha / 255);
    gSPDisplayList(gDisplayListHead++, dl_draw_text_bg_box);
    gSPPopMatrix(gDisplayListHead++, G_MTX_MODELVIEW);

    gSPDisplayList(gDisplayListHead++, dl_ia_text_begin);
    gDPSetEnvColor(gDisplayListHead++, 255, 255, 255, fadeAlpha);
    print_generic_string(28, 58, sDialogueText);
    gSPDisplayList(gDisplayListHead++, dl_ia_text_end);
}

s32 ai_bridge_is_speaking(void) {
    return sDialogueTimer > 0;
}

#else

void ai_bridge_update(void) {
}

s32 ai_bridge_has_pending_instruction(void) {
    return FALSE;
}

void ai_bridge_cancel_current(void) {
}

void ai_bridge_render_status(void) {
}

void ai_bridge_render_dialogue(void) {
}

s32 ai_bridge_is_speaking(void) {
    return FALSE;
}

#endif
