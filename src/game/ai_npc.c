#include <ultra64.h>
#include "sm64.h"
#include "ai_npc.h"

#ifndef TARGET_N64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "area.h"
#include "behavior_data.h"
#include "dialog_ids.h"
#include "engine/math_util.h"
#include "engine/surface_collision.h"
#include "game_init.h"
#include "gfx_dimensions.h"
#include "ingame_menu.h"
#include "interaction.h"
#include "level_update.h"
#include "memory.h"
#include "mario.h"
#include "surface_terrains.h"
#include "object_constants.h"
#include "object_helpers.h"
#include "object_list_processor.h"
#include "save_file.h"
#include "segment2.h"

#define NPC_COUNT 32
#define CHAT_RANGE 650.0f
#define INPUT_LIMIT 120
/* Wire action IDs are shared with npc_bridge/protocol.py. */
enum { NPC_NONE, NPC_FOLLOW, NPC_FLEE, NPC_APPROACH, NPC_STOP,
       NPC_HOSTILE, NPC_FRIENDLY, NPC_COINS, NPC_CANNON };
struct Npc {
    struct Object *obj;
    s32 kind, coins, mode, frames, pending, amount, chatOpacity;
    u32 seen;
    char key[128], token[80];
};
static struct Npc sNpcs[NPC_COUNT];
static struct Npc *sChat, *sOffer;
static s32 sOpen, sWaiting, sShift[2], sKeys[512];
static u32 sStarted, sRequest, sSession;
static char sToken[80], sText[INPUT_LIMIT + 1], sReply[241];
static char sStatus[96] = "ENTER SEND   ESC CLOSE";
static const char *sActions[] = { "none", "follow", "flee", "approach", "stop",
                                 "hostile", "friendly", "give_coins", "open_cannon" };

static void path_for(char *out, size_t size, const char *name) {
    const char *dir = getenv("SM64_NPC_RUNTIME_DIR");
    if (dir && dir[0]) snprintf(out, size, "%s/%s", dir, name);
    else snprintf(out, size, "%s", name);
}
static s32 live(struct Npc *n) {
    return n && n->obj && gGlobalTimer - n->seen < 3
        && (n->obj->activeFlags & ACTIVE_FLAG_ACTIVE);
}
static f32 distance(struct Object *obj) {
    f32 dx = obj->oPosX - gMarioState->pos[0];
    f32 dy = obj->oPosY - gMarioState->pos[1];
    f32 dz = obj->oPosZ - gMarioState->pos[2];
    return sqrtf(dx*dx + dy*dy + dz*dz);
}
static s32 ready(void) {
    return gCurrentArea && gMarioState && gMarioState->marioObj
        && sCurrPlayMode == 0 && !gWarpTransition.isActive
        && get_dialog_id() == DIALOG_NONE
        && (gMarioState->action & ACT_GROUP_MASK) != ACT_GROUP_CUTSCENE;
}
static s32 cannon_capable(struct Npc *n) {
    return n->kind == 2 && n->obj->oBobombBuddyRole == BOBOMB_BUDDY_ROLE_CANNON;
}
static void json_string(FILE *f, const char *text) {
    fputc('"', f);
    for (; *text; text++) {
        unsigned char c = (unsigned char)*text;
        if (c == '"' || c == '\\') fputc('\\', f);
        if (c >= 32 && c < 127) fputc(c, f);
    }
    fputc('"', f);
}
static void result(struct Npc *n, s32 action, const char *outcome, s32 coins) {
    char path[512], temp[520];
    FILE *f;
    path_for(path, sizeof(path), ".sm64-npc-result.json");
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    f = fopen(temp, "w");
    if (!f) return;
    fprintf(f, "{\"id\":\"%s\",\"npc\":\"%s\",\"action\":\"%s\",\"coins\":%d,\"result\":",
            n->token, n->key, sActions[action], coins);
    json_string(f, outcome);
    fputs("}\n", f);
    if (fclose(f) == 0) { remove(path); rename(temp, path); }
}
static void cancel(void) {
    /* Invalidates in-flight replies; no queued action survives a cancelled chat. */
    char path[512];
    path_for(path, sizeof(path), ".sm64-npc-request.json");
    remove(path);
    if (live(sChat) && sChat->kind == 1) sChat->obj->oOpacity = sChat->chatOpacity;
    sOpen = sWaiting = 0;
    sChat = NULL;
    sText[0] = 0;
    sToken[0] = 0;
}
void ai_npc_reset(void) {
    cancel();
    sOffer = NULL;
    memset(sNpcs, 0, sizeof(sNpcs));
}
static void open_chat(struct Npc *n) {
    if (!live(n) || distance(n->obj) > CHAT_RANGE || !ready()) return;
    sChat = n;
    n->chatOpacity = n->obj->oOpacity;
    sOpen = 1;
    sWaiting = 0;
    sText[0] = 0;
    sReply[0] = 0;
    strcpy(sStatus, "ENTER SEND   ESC CLOSE");
}
void ai_npc_offer_reply(struct Object *obj) {
    s32 i;
    for (i = 0; i < NPC_COUNT; i++)
        if (sNpcs[i].obj == obj) { sOffer = &sNpcs[i]; break; }
}
static void submit(void) {
    char path[512], temp[520];
    FILE *f;
    s32 i, nonspace = 0;
    if (!live(sChat) || sWaiting) return;
    for (i = 0; sText[i]; i++) if (sText[i] != ' ') nonspace = 1;
    if (!nonspace) return;
    if (!sSession) sSession = (u32)time(NULL) ^ (u32)clock();
    snprintf(sToken, sizeof(sToken), "%u-%u-%u", sSession, ++sRequest, gGlobalTimer);
    path_for(path, sizeof(path), ".sm64-npc-request.json");
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    f = fopen(temp, "w");
    if (!f) { strcpy(sStatus, "CANNOT WRITE REQUEST FILE"); return; }
    fprintf(f, "{\"id\":\"%s\",\"npc\":\"%s\",\"kind\":\"%s\",\"save\":%d,"
               "\"level\":%d,\"area\":%d,\"course\":%d,\"act\":%d,\"stars\":%d,"
               "\"dialog_id\":%d,\"coin_budget\":%d,\"mode\":\"%s\",\"can_open_cannon\":%s,\"player_text\":",
            sToken, sChat->key, sChat->kind == 1 ? "toad" : "bobomb_buddy", gCurrSaveFileNum,
            gCurrLevelNum, gCurrAreaIndex, gCurrCourseNum, gCurrActNum, gMarioState->numStars,
            sChat->kind == 1 ? (s32)sChat->obj->oToadMessageDialogId :
                (cannon_capable(sChat) ? (gCurrCourseNum == COURSE_BOB ? DIALOG_105 : DIALOG_106)
                                      : (s32)sChat->obj->oBehParams2ndByte),
            10 - sChat->coins, sActions[sChat->mode], cannon_capable(sChat) ? "true" : "false");
    json_string(f, sText);
    fputs("}\n", f);
    if (fclose(f) != 0) { strcpy(sStatus, "REQUEST WRITE FAILED"); return; }
    remove(path);
    if (rename(temp, path) != 0) { strcpy(sStatus, "REQUEST RENAME FAILED"); return; }
    sWaiting = 1;
    sStarted = gGlobalTimer;
    strcpy(sStatus, "THINKING   ESC CANCEL");
}
static void poll_reply(void) {
    char path[512], token[80], text[244];
    int action, amount;
    FILE *f;
    if (!sWaiting || gGlobalTimer % 3) return;
    if (gGlobalTimer - sStarted > 1800) {
        sWaiting = 0;
        sToken[0] = 0;
        strcpy(sStatus, "BRIDGE TIMEOUT   ENTER RETRY   ESC CLOSE");
        return;
    }
    path_for(path, sizeof(path), ".sm64-npc-response");
    f = fopen(path, "r");
    if (!f) return;
    if (!fgets(token, sizeof(token), f) || !fgets(text, sizeof(text), f)
        || sscanf(text, "%d %d", &action, &amount) != 2
        || !fgets(text, sizeof(text), f)) { fclose(f); return; }
    fclose(f);
    token[strcspn(token, "\r\n")] = 0;
    if (strcmp(token, sToken) != 0) return;
    remove(path);
    if (action < NPC_NONE || action > NPC_CANNON || amount < 0 || amount > 3
        || (action == NPC_HOSTILE && sChat->kind != 2)
        || (action == NPC_CANNON && !cannon_capable(sChat))
        || (action != NPC_COINS && amount != 0)
        || (action == NPC_COINS && amount < 1)) {
        strcpy(sStatus, "INVALID REPLY   ENTER RETRY"); sWaiting = 0; return;
    }
    text[strcspn(text, "\r\n")] = 0;
    strncpy(sReply, text, sizeof(sReply)-1);
    sReply[sizeof(sReply)-1] = 0;
    sWaiting = 0;
    sText[0] = 0;
    /* Execute from the NPC's behaviour callback, where gCurrentObject is correct. */
    strcpy(sChat->token, sToken);
    sChat->pending = action;
    sChat->amount = amount;
    if (action == NPC_NONE) {
        result(sChat, action, "Spoke; no action requested", 0);
        strcpy(sStatus, "ENTER SEND   ESC CLOSE");
    } else {
        strcpy(sStatus, "ESC CLOSE TO LET THEM ACT");
    }
}
void ai_npc_update(void) {
    if (sOpen && (!live(sChat) || !ready() || distance(sChat->obj) > CHAT_RANGE + 100)) {
        if (sChat) sChat->pending = 0;
        cancel();
    }
    if (sOffer && ready()) {
        if (!sOpen) open_chat(sOffer);
        sOffer = NULL;
    }
    poll_reply();
}
void ai_npc_filter_controller(void) {
    if (sOpen && gPlayer1Controller) {
        gPlayer1Controller->rawStickX = gPlayer1Controller->rawStickY = 0;
        gPlayer1Controller->stickX = gPlayer1Controller->stickY = gPlayer1Controller->stickMag = 0;
        gPlayer1Controller->buttonDown = gPlayer1Controller->buttonPressed = 0;
    }
}
static char key_char(s32 key) {
    s32 shift = sShift[0] || sShift[1];
    if (key >= 2 && key <= 13) return (shift ? "!@#$%^&*()_+" : "1234567890-=")[key-2];
    if (key >= 16 && key <= 25) return (shift ? "QWERTYUIOP" : "qwertyuiop")[key-16];
    if (key >= 30 && key <= 38) return (shift ? "ASDFGHJKL" : "asdfghjkl")[key-30];
    if (key >= 44 && key <= 50) return (shift ? "ZXCVBNM" : "zxcvbnm")[key-44];
    if (key == 57) return ' ';
    if (key == 39) return shift ? ':' : ';';
    if (key == 40) return shift ? '"' : '\'';
    if (key == 51) return shift ? '<' : ',';
    if (key == 52) return shift ? '>' : '.';
    if (key == 53) return shift ? '?' : '/';
    return 0;
}
s32 ai_npc_key_down(s32 key) {
    s32 repeated = 0, i, length;
    char c;
    struct Npc *nearest = NULL;
    f32 best = CHAT_RANGE;
    if (key >= 0 && key < 512) { repeated = sKeys[key]; sKeys[key] = 1; }
    if (key == 42) sShift[0] = 1;
    if (key == 54) sShift[1] = 1;
    if (!sOpen) {
        if (key != 20 || repeated || !ready()) return 0;
        for (i = 0; i < NPC_COUNT; i++) {
            if (live(&sNpcs[i]) && distance(sNpcs[i].obj) < best) {
                best = distance(sNpcs[i].obj); nearest = &sNpcs[i];
            }
        }
        if (nearest) { open_chat(nearest); return 1; }
        return 0;
    }
    if (key == 1 && !repeated) {
        if (sWaiting) { sChat->pending = 0; cancel(); }
        else {
            if (sChat->kind == 1) sChat->obj->oOpacity = sChat->chatOpacity;
            sOpen = 0; sChat = NULL; sText[0] = 0;
        }
        return 1;
    }
    if (sWaiting || (sChat && sChat->pending)) return 1;
    if (key == 28 && !repeated) { submit(); return 1; }
    length = strlen(sText);
    if (key == 14 && length) sText[length - 1] = 0;
    else if ((c = key_char(key)) && length < INPUT_LIMIT) {
        sText[length] = c; sText[length+1] = 0;
    }
    return 1;
}
s32 ai_npc_key_up(s32 key) {
    if (key >= 0 && key < 512) sKeys[key] = 0;
    if (key == 42) sShift[0] = 0;
    if (key == 54) sShift[1] = 0;
    return sOpen;
}
void ai_npc_keys_up(void) {
    memset(sKeys, 0, sizeof(sKeys));
    sShift[0] = sShift[1] = 0;
}

static void execute(struct Npc *n) {
    s32 action = n->pending, count;
    const char *outcome = "Action started";
    n->pending = 0;
    if (action == NPC_COINS) {
        count = n->amount;
        if (count > 10 - n->coins) count = 10 - n->coins;
        if (count > 0) {
            s32 oldLoot = n->obj->oNumLootCoins;
            n->obj->oNumLootCoins = count;
            obj_spawn_loot_yellow_coins(n->obj, count, 12.0f);
            n->obj->oNumLootCoins = oldLoot;
            n->coins += count;
        }
        result(n, action, count ? "Coins spawned" : "No coins left", count);
        return;
    }
    if (action == NPC_CANNON) {
        if (cannon_capable(n)) {
            save_file_set_cannon_unlocked();
            outcome = "Course cannon unlocked";
        } else outcome = "Cannot operate a cannon";
    } else {
        n->obj->oForwardVel = 0;
        n->mode = (action == NPC_STOP || action == NPC_FRIENDLY) ? 0 : action;
        n->frames = 900;
        if (n->kind == 1 && !n->mode) {
            n->obj->oOpacity = 81;
            n->obj->oToadMessageState = 0;
            n->obj->oToadMessageRecentlyTalked = FALSE;
        }
    }
    result(n, action, outcome, 0);
}
s32 ai_npc_tick(struct Object *obj, s32 kind) {
    struct Npc *n = NULL;
    s32 i, moving;
    s16 yaw;
    f32 d, nextX, nextZ, floorY;
    struct Surface *floor;
    for (i = 0; i < NPC_COUNT; i++) if (sNpcs[i].obj == obj) { n = &sNpcs[i]; break; }
    if (!n) {
        for (i = 0; i < NPC_COUNT; i++) if (!sNpcs[i].obj) {
            n = &sNpcs[i]; n->obj = obj; n->kind = kind;
            snprintf(n->key, sizeof(n->key), "%d:%d:%d:%d:%.0f:%.0f:%.0f:%u",
                     gCurrSaveFileNum, gCurrLevelNum, gCurrAreaIndex, kind,
                     obj->oPosX, obj->oPosY, obj->oPosZ, obj->oBehParams);
            break;
        }
    }
    if (!n) return 0;
    n->seen = gGlobalTimer;
    if (n->pending && !sOpen && ready()) execute(n);
    if (sOpen && sChat == n) {
        obj->oForwardVel = 0;
        obj->oInteractStatus = 0;
        if (kind == 1) obj->oOpacity = 255;
        return 1;
    }
    if (!n->mode || !ready()) return 0;
    if (--n->frames <= 0) {
        n->mode = 0; obj->oForwardVel = 0;
        if (kind == 1) { obj->oOpacity = 81; obj->oToadMessageState = 0; }
        return 0;
    }
    /* Chatting NPC holds position while replying. Other NPCs keep their state. */
    d = distance(obj);
    yaw = atan2s(gMarioState->pos[2] - obj->oPosZ, gMarioState->pos[0] - obj->oPosX);
    if (n->mode == NPC_FLEE) yaw += 0x8000;
    obj->oMoveAngleYaw = approach_s16_symmetric(obj->oMoveAngleYaw, yaw, 0x600);
    obj->oFaceAngleYaw = obj->oMoveAngleYaw;
    moving = !(sOpen && sChat == n) && (n->mode == NPC_FLEE ? d < 900 : d > 140);
    obj->oForwardVel = moving ? (n->mode == NPC_HOSTILE ? 14 : 8) : 0;
    /* Refuse steps over ledges or sharply different floor heights. */
    nextX = obj->oPosX + sins(obj->oMoveAngleYaw) * 50;
    nextZ = obj->oPosZ + coss(obj->oMoveAngleYaw) * 50;
    floorY = find_floor(nextX, obj->oPosY + 80, nextZ, &floor);
    if (!floor || floor->type == SURFACE_BURNING || floor->type == SURFACE_DEATH_PLANE
        || fabsf(floorY - obj->oPosY) > 100) obj->oForwardVel = 0;
    obj->oGravity = 2.5f;
    obj->oWallHitboxRadius = 50.0f;
    cur_obj_update_floor_and_walls();
    cur_obj_move_standard(-78);
    obj->oInteractStatus = 0;
    if (kind == 1) obj->oOpacity = 255;
    if (n->mode == NPC_APPROACH && d < 160) {
        n->mode = 0; obj->oForwardVel = 0;
        if (kind == 1) { obj->oOpacity = 81; obj->oToadMessageState = 0;
                         obj->oToadMessageRecentlyTalked = FALSE; }
    }
    /* A hostile Buddy shoves Mario on contact, with the normal invincibility window. */
    if (n->mode == NPC_HOSTILE && d < 130 && !sOpen
        && gMarioState->invincTimer == 0 && !(gMarioState->action & ACT_FLAG_INVULNERABLE)
        && !(gMarioState->flags & (MARIO_VANISH_CAP | MARIO_METAL_CAP))) {
        gMarioState->interactObj = obj;
        gMarioState->hurtCounter += 4;
        gMarioState->invincTimer = 60;
        gMarioState->forwardVel = -24;
        gMarioState->faceAngle[1] = yaw + 0x8000;
        drop_and_set_mario_action(gMarioState, ACT_BACKWARD_GROUND_KB, 1);
    }
    return 1;
}
static u8 glyph(char c) {
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return ASCII_TO_DIALOG(c);
    switch (c) {
        case '\'': return 0x3E; case '.': return 0x3F; case ',': return DIALOG_CHAR_COMMA;
        case '-': return 0x9F; case ':': return 0xE6; case '!': return 0xF2; case '?': return 0xF4;
        default: return DIALOG_CHAR_SPACE;
    }
}
static void draw_text(s16 x, s16 y, const char *text, s32 columns, s32 maxLines) {
    u8 encoded[512];
    s32 j = 0, col = 0, line = 1;
    while (*text && j < 508) {
        if (col >= columns) {
            if (++line > maxLines) break;
            encoded[j++] = DIALOG_CHAR_NEWLINE; col = 0;
        }
        encoded[j++] = glyph(*text++); col++;
    }
    encoded[j] = DIALOG_CHAR_TERMINATOR;
    print_generic_string(x, y, encoded);
}
void ai_npc_render(void) {
    f32 left = GFX_DIMENSIONS_FROM_LEFT_EDGE(0.0f);
    f32 width = GFX_DIMENSIONS_FROM_RIGHT_EDGE(0.0f) - left;
    s32 columns = (s32)((width - 24) / 8.0f), length, offset;
    if (!sOpen || !live(sChat)) return;
    create_dl_ortho_matrix();
    create_dl_translation_matrix(MENU_MTX_PUSH, left, 154.0f, 0);
    create_dl_scale_matrix(MENU_MTX_NOPUSH, width / 130.0f, 1.9f, 1);
    gDPSetEnvColor(gDisplayListHead++, 0, 0, 0, 220);
    gSPDisplayList(gDisplayListHead++, dl_draw_text_bg_box);
    gSPPopMatrix(gDisplayListHead++, G_MTX_MODELVIEW);
    gSPDisplayList(gDisplayListHead++, dl_ia_text_begin);
    gDPSetEnvColor(gDisplayListHead++, 255, 220, 100, 255);
    draw_text(left+12, 137, sChat->kind == 1 ? "TOAD" : "BOB-OMB BUDDY", columns, 1);
    gDPSetEnvColor(gDisplayListHead++, 255, 255, 255, 255);
    draw_text(left+12, 119, sReply[0] ? sReply : "What would you like to say?", columns, 6);
    gDPSetEnvColor(gDisplayListHead++, 255, 220, 100, 255);
    draw_text(left+12, 27, sStatus, columns, 1);
    gDPSetEnvColor(gDisplayListHead++, 255, 255, 255, 255);
    length = strlen(sText); offset = length > columns-2 ? length-(columns-2) : 0;
    draw_text(left+12, 10, sText[0] ? sText+offset : ">", columns, 1);
    gSPDisplayList(gDisplayListHead++, dl_ia_text_end);
}
#else
void ai_npc_reset(void) {}
void ai_npc_update(void) {}
void ai_npc_render(void) {}
void ai_npc_filter_controller(void) {}
s32 ai_npc_key_down(UNUSED s32 key) { return 0; }
s32 ai_npc_key_up(UNUSED s32 key) { return 0; }
void ai_npc_keys_up(void) {}
s32 ai_npc_tick(UNUSED struct Object *obj, UNUSED s32 kind) { return 0; }
void ai_npc_offer_reply(UNUSED struct Object *obj) {}
#endif
