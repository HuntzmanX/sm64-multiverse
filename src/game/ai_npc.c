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
#include "camera.h"
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
#define HISTORY_MESSAGES 32
#define UI_LINES 768
#define CHAT_TIMEOUT 3600

struct ChatMessage { s32 mario; char text[241]; };
/* Wire action IDs are shared with npc_bridge/protocol.py. */
enum { NPC_NONE, NPC_FOLLOW, NPC_FLEE, NPC_APPROACH, NPC_STOP,
       NPC_HOSTILE, NPC_FRIENDLY, NPC_COINS, NPC_CANNON };
struct Npc {
    struct Object *obj;
    s32 kind, coins, mode, frames, pending, amount, chatOpacity;
    u32 seen;
    char key[128], token[80];
    s32 historyCount;
    struct ChatMessage history[HISTORY_MESSAGES];
};
static struct Npc sNpcs[NPC_COUNT];
static struct Npc *sChat, *sOffer;
static s32 sOpen, sWaiting, sShift[2], sKeys[512];
static u32 sStarted, sRequest, sSession;
static char sToken[80], sText[INPUT_LIMIT + 1], sReply[241], sSubmitted[INPUT_LIMIT + 1];
static s32 sScroll, sStage, sPlayerShown, sNpcShown, sEventSequence;
static f32 sFrameBlend, sFrameTarget[3];
static u32 sFrameTimer;
static s32 sNativeDialog = -1, sNativePage = -1;
static struct Object *sNativeObject;
static char sNativeText[1024], sNativeToken[80];
static u32 sNativeFrame;
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
static void append_message(s32 mario, const char *text) {
    struct ChatMessage *m;
    if (!sChat || !text[0]) return;
    if (sChat->historyCount == HISTORY_MESSAGES) {
        memmove(sChat->history, sChat->history + 1,
                sizeof(sChat->history[0]) * (HISTORY_MESSAGES - 1));
        sChat->historyCount--;
    }
    m = &sChat->history[sChat->historyCount++];
    m->mario = mario;
    strncpy(m->text, text, sizeof(m->text)-1);
    m->text[sizeof(m->text)-1] = 0;
    sScroll = 0;
}
static void clear_native_dialogue(void) {
    char path[512];
    path_for(path, sizeof(path), ".sm64-npc-native.json");
    remove(path);
    sNativeDialog = sNativePage = -1;
    sNativeObject = NULL;
}
void ai_npc_native_page(s32 dialog, s32 start, s32 lines, const u8 *encoded, s32 visible) {
#if defined(VERSION_US)
    struct Npc *npc = NULL;
    char text[1024], part[48], token[80], path[512], temp[520];
    s32 i, j = 0, line = 0, c, samePage;
    FILE *file;
    if (!visible || sOpen || !gMarioState || !gCurrentArea || !encoded || start < 0
        || dialog < 0 || lines < 1 || lines > 12) {
        if (sNativeDialog >= 0) clear_native_dialogue();
        return;
    }
    for (i = 0; i < NPC_COUNT; i++) {
        if (sNpcs[i].obj && (sNpcs[i].obj == gMarioState->interactObj
            || sNpcs[i].obj == gMarioState->usedObj)
            && (sNpcs[i].obj->activeFlags & ACTIVE_FLAG_ACTIVE)) { npc = &sNpcs[i]; break; }
    }
    if (!npc) { if (sNativeDialog >= 0) clear_native_dialogue(); return; }
    for (i = start; i < start + 2048 && j < (s32)sizeof(text)-1; i++) {
        c = encoded[i]; part[0] = 0;
        if (c == DIALOG_CHAR_TERMINATOR) break;
        if (c == DIALOG_CHAR_NEWLINE) { if (++line >= lines) break; strcpy(part, " "); }
        else if (c <= 9) { part[0] = '0' + c; part[1] = 0; }
        else if (c <= 0x23) { part[0] = 'A' + c - 10; part[1] = 0; }
        else if (c <= 0x3D) { part[0] = 'a' + c - 0x24; part[1] = 0; }
        else if (c >= 0x54 && c <= 0x58) snprintf(part, sizeof(part), "%c button", "ABCZR"[c-0x54]);
        else switch (c) {
            case 0x3E: strcpy(part, "'"); break;
            case 0x3F: case DIALOG_CHAR_PERIOD: strcpy(part, "."); break;
            case DIALOG_CHAR_COMMA: strcpy(part, ","); break;
            case 0x50: strcpy(part, "up"); break;
            case 0x51: strcpy(part, "down"); break;
            case 0x52: strcpy(part, "left"); break;
            case 0x53: strcpy(part, "right"); break;
            case DIALOG_CHAR_SPACE: strcpy(part, " "); break;
            case 0x9F: strcpy(part, "-"); break;
            case DIALOG_CHAR_SLASH: strcpy(part, "/"); break;
            case DIALOG_CHAR_MULTI_THE: strcpy(part, "the"); break;
            case DIALOG_CHAR_MULTI_YOU: strcpy(part, "you"); break;
            case DIALOG_CHAR_STAR_COUNT: snprintf(part, sizeof(part), "%d", gDialogVariable); break;
            case 0xE1: strcpy(part, "("); break;
            case 0xE2: strcpy(part, ")("); break;
            case 0xE3: strcpy(part, ")"); break;
            case 0xE4: strcpy(part, "+"); break;
            case 0xE5: strcpy(part, "and"); break;
            case 0xE6: strcpy(part, ":"); break;
            case 0xF2: strcpy(part, "!"); break;
            case 0xF3: strcpy(part, "%"); break;
            case 0xF4: strcpy(part, "?"); break;
            case 0xF5: case 0xF6: part[0] = 34; part[1] = 0; break;
            case 0xF8: strcpy(part, "..."); break;
            case 0xF9: strcpy(part, "coin"); break;
            case 0xFA: case 0xFD: strcpy(part, "star"); break;
            case 0xFB: strcpy(part, "times"); break;
            default: strcpy(part, " "); break;
        }
        if (j + strlen(part) >= sizeof(text)) break;
        memcpy(text + j, part, strlen(part)); j += strlen(part);
    }
    text[j] = 0;
    samePage = sNativeObject == npc->obj && sNativeDialog == dialog && sNativePage == start
        && !strcmp(sNativeText, text);
    if (!j || (samePage && gGlobalTimer - sNativeFrame < 30)) return;
    if (samePage) strcpy(token, sNativeToken); /* Refresh liveness without a new playback request. */
    else {
        if (!sSession) sSession = (u32)time(NULL) ^ (u32)clock();
        snprintf(token, sizeof(token), "%u-%u-%u", sSession, ++sRequest, gGlobalTimer);
    }
    path_for(path, sizeof(path), ".sm64-npc-native.json");
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    file = fopen(temp, "w"); if (!file) return;
    fprintf(file, "{\"id\":\"%s\",\"source\":\"default\",\"language\":\"us\",\"kind\":\"%s\",\"dialog_id\":%d,\"page\":%d,\"text\":",
            token, npc->kind == 1 ? "toad" : "bobomb_buddy", dialog, start);
    json_string(file, text); fputs("}\n", file);
    if (fclose(file) != 0) return;
    remove(path); if (rename(temp, path) != 0) return;
    sNativeObject = npc->obj; sNativeDialog = dialog; sNativePage = start;
    strcpy(sNativeText, text); strcpy(sNativeToken, token); sNativeFrame = gGlobalTimer;
#else
    (void)dialog; (void)start; (void)lines; (void)encoded; (void)visible;
#endif
}
static void clear_request(void) {
    char path[512];
    path_for(path, sizeof(path), ".sm64-npc-request.json");
    remove(path);
}
static void cancel(void) {
    /* Release chat and invalidate in-flight replies. Callers discard pending
     * actions on cancellation; a completed NPC turn keeps its accepted action. */
    clear_request();
    if (live(sChat) && sChat->kind == 1) sChat->obj->oOpacity = sChat->chatOpacity;
    sOpen = sWaiting = 0;
    sChat = NULL;
    sText[0] = 0;
    sToken[0] = 0;
}
void ai_npc_reset(void) {
    cancel();
    clear_native_dialogue();
    sOffer = NULL;
    memset(sNpcs, 0, sizeof(sNpcs));
    sFrameBlend = 0;
}
static void open_chat(struct Npc *n) {
    if (!live(n) || distance(n->obj) > CHAT_RANGE || !ready()) return;
    clear_native_dialogue();
    sChat = n;
    n->chatOpacity = n->obj->oOpacity;
    sOpen = 1;
    sWaiting = 0;
    sText[0] = 0;
    sReply[0] = 0;
    sScroll = sStage = 0;
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
               "\"protocol_version\":3,\"history_count\":%d,\"dialog_id\":%d,\"coin_budget\":%d,\"mode\":\"%s\",\"can_open_cannon\":%s,\"player_text\":",
            sToken, sChat->key, sChat->kind == 1 ? "toad" : "bobomb_buddy", gCurrSaveFileNum,
            gCurrLevelNum, gCurrAreaIndex, gCurrCourseNum, gCurrActNum, gMarioState->numStars,
            sChat->historyCount,
            sChat->kind == 1 ? (s32)sChat->obj->oToadMessageDialogId :
                (cannon_capable(sChat) ? (gCurrCourseNum == COURSE_BOB ? DIALOG_105 : DIALOG_106)
                                      : (s32)sChat->obj->oBehParams2ndByte),
            10 - sChat->coins, sActions[sChat->mode], cannon_capable(sChat) ? "true" : "false");
    json_string(f, sText);
    fputs("}\n", f);
    if (fclose(f) != 0) { strcpy(sStatus, "REQUEST WRITE FAILED"); return; }
    remove(path);
    if (rename(temp, path) != 0) { strcpy(sStatus, "REQUEST RENAME FAILED"); return; }
    strcpy(sSubmitted, sText);
    sText[0] = 0;
    sPlayerShown = sNpcShown = sEventSequence = sStage = 0;
    sScroll = 0;
    sWaiting = 1;
    sStarted = gGlobalTimer;
    strcpy(sStatus, "THINKING   ESC CANCEL");
}
static void poll_event(void) {
    char path[512], token[80], line[256], temp[520];
    s32 sequence, speaker;
    FILE *f;
    if (!sWaiting) return;
    path_for(path, sizeof(path), ".sm64-npc-event");
    f = fopen(path, "r");
    if (!f) return;
    if (!fgets(token, sizeof(token), f) || !fgets(line, sizeof(line), f)
        || sscanf(line, "%d %d", &sequence, &speaker) != 2
        || !fgets(line, sizeof(line), f)) { fclose(f); return; }
    fclose(f);
    token[strcspn(token, "\r\n")] = 0;
    line[strcspn(line, "\r\n")] = 0;
    if (strcmp(token, sToken) || sequence < 1 || speaker < 1 || speaker > 4) return;
    if (sequence > sEventSequence) {
        if (speaker == 1 && !sPlayerShown) {
            append_message(TRUE, sSubmitted); sPlayerShown = 1; sStage = 1;
        } else if (speaker == 2 && !sNpcShown) {
            if (!sPlayerShown) { append_message(TRUE, sSubmitted); sPlayerShown = 1; }
            append_message(FALSE, line); sNpcShown = 1; sStage = 2;
        } else if (speaker >= 3) append_message(speaker == 3, line);
        sEventSequence = sequence;
        sStarted = gGlobalTimer;
    }
    remove(path);
    path_for(path, sizeof(path), ".sm64-npc-event-ack");
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    f = fopen(temp, "w");
    if (!f) return;
    fprintf(f, "%s\n%d\n", token, sequence);
    if (fclose(f) == 0) { remove(path); rename(temp, path); }
}
static void poll_reply(void) {
    char path[512], token[80], text[244];
    int action, amount, endConversation = 0;
    FILE *f;
    if (!sWaiting) return;
    if (gGlobalTimer - sStarted > CHAT_TIMEOUT) {
        sWaiting = 0;
        sToken[0] = 0;
        strcpy(sText, sSubmitted);
        clear_request();
        strcpy(sStatus, "BRIDGE TIMEOUT   ENTER RETRY   ESC CLOSE");
        return;
    }
    path_for(path, sizeof(path), ".sm64-npc-response");
    f = fopen(path, "r");
    if (!f) return;
    if (!fgets(token, sizeof(token), f) || !fgets(text, sizeof(text), f)
        || sscanf(text, "%d %d %d", &action, &amount, &endConversation) < 2
        || !fgets(text, sizeof(text), f)) { fclose(f); return; }
    fclose(f);
    token[strcspn(token, "\r\n")] = 0;
    if (strcmp(token, sToken) != 0) return;
    remove(path);
    if (action < NPC_NONE || action > NPC_CANNON || amount < 0 || amount > 3
        || (endConversation != 0 && endConversation != 1)
        || (action == NPC_CANNON && !cannon_capable(sChat))
        || (action != NPC_COINS && amount != 0)
        || (action == NPC_COINS && amount < 1)) {
        strcpy(sStatus, "INVALID REPLY   ENTER RETRY"); sWaiting = 0; return;
    }
    text[strcspn(text, "\r\n")] = 0;
    strncpy(sReply, text, sizeof(sReply)-1);
    sReply[sizeof(sReply)-1] = 0;
    if (!sPlayerShown) append_message(TRUE, sSubmitted);
    if (!sNpcShown) append_message(FALSE, sReply);
    sWaiting = 0;
    sStage = 0;
    /* Execute from the NPC's behaviour callback, where gCurrentObject is correct. */
    strcpy(sChat->token, sToken);
    sChat->pending = action;
    sChat->amount = amount;
    if (action == NPC_NONE) {
        result(sChat, action, "Spoke; no action requested", 0);
        strcpy(sStatus, "ENTER SEND   ESC CLOSE");
    }
    /* The companion publishes completion only after the final voice has ended.
     * Actions always release chat controls; a goodbye can close without an action. */
    if (endConversation || action != NPC_NONE) cancel();
}
void ai_npc_update(void) {
    if (sNativeDialog >= 0 && (get_dialog_id() == DIALOG_NONE || sOpen)) clear_native_dialogue();
    if (sOpen && (!live(sChat) || !ready() || distance(sChat->obj) > CHAT_RANGE + 100)) {
        if (sChat) sChat->pending = 0;
        cancel();
    }
    if (sOffer && ready()) {
        if (!sOpen) open_chat(sOffer);
        sOffer = NULL;
    }
    if (sOpen) {
        /* Neutral chat controls must not count as idle time toward sleep. */
        if (gMarioState->action == ACT_START_SLEEPING || gMarioState->action == ACT_SLEEPING
            || gMarioState->action == ACT_WAKING_UP) {
            set_mario_action(gMarioState, ACT_IDLE, 0);
        }
        if (gMarioState->action == ACT_IDLE) {
            gMarioState->actionTimer = 0;
            if (gMarioState->actionState == 3) gMarioState->actionState = 0;
        }
    }
    poll_event();
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
        sChat->pending = 0;
        cancel();
        return 1;
    }
    if (key == 0x148 || key == 0x149) { sScroll += key == 0x149 ? 16 : 3; return 1; }
    if (key == 0x150 || key == 0x151) { sScroll -= key == 0x151 ? 16 : 3; if (sScroll < 0) sScroll = 0; return 1; }
    if (key == 0x147) { sScroll = UI_LINES; return 1; }
    if (key == 0x14F) { sScroll = 0; return 1; }
    if (key == 28 && !repeated) { if (!sWaiting && !sChat->pending) submit(); return 1; }
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
    f32 d, nextX, nextZ, floorY, nativeGravity, nativeWallRadius;
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
    /* object_step (used by Buddies) subtracts positive gravity, whereas
     * cur_obj_move_standard adds signed gravity. Keep our settings local to
     * this step so returning to the native behavior preserves its physics. */
    nativeGravity = obj->oGravity;
    nativeWallRadius = obj->oWallHitboxRadius;
    obj->oGravity = -2.5f;
    obj->oWallHitboxRadius = 50.0f;
    cur_obj_update_floor_and_walls();
    cur_obj_move_standard(-78);
    obj->oGravity = nativeGravity;
    obj->oWallHitboxRadius = nativeWallRadius;
    obj->oInteractStatus = 0;
    if (kind == 1) obj->oOpacity = 255;
    if (n->mode == NPC_APPROACH && d < 160) {
        n->mode = 0; obj->oForwardVel = 0;
        if (kind == 1) { obj->oOpacity = 81; obj->oToadMessageState = 0;
                         obj->oToadMessageRecentlyTalked = FALSE; }
    }
    /* A hostile NPC shoves Mario on contact, with the normal invincibility window. */
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
struct UiLine { s32 mario, heading; char text[97]; };
static struct UiLine sLines[UI_LINES];
static s32 sLineCount;

static void ui_line(s32 mario, s32 heading, const char *text, s32 length) {
    struct UiLine *line;
    if (sLineCount >= UI_LINES) return;
    line = &sLines[sLineCount++];
    line->mario = mario; line->heading = heading;
    if (length > 96) length = 96;
    memcpy(line->text, text, length); line->text[length] = 0;
}
static void wrap_message(s32 mario, const char *text, s32 columns) {
    s32 length, split;
    const char *name = mario ? "MARIO" : (sChat->kind == 1 ? "TOAD" : "BOB-OMB BUDDY");
    ui_line(mario, 1, name, strlen(name));
    while (*text) {
        length = strlen(text);
        if (length > columns) {
            length = columns;
            for (split = length; split > columns / 2; split--)
                if (text[split] == ' ') { length = split; break; }
        }
        ui_line(mario, 0, text, length);
        text += length;
        while (*text == ' ') text++;
    }
    ui_line(-1, 0, "", 0);
}
static void build_lines(s32 columns) {
    s32 i;
    char dots[4] = "...";
    sLineCount = 0;
    for (i = 0; i < sChat->historyCount; i++)
        wrap_message(sChat->history[i].mario, sChat->history[i].text, columns);
    if (sWaiting && sStage < 2) {
        dots[1 + (gGlobalTimer / 12) % 3] = 0;
        wrap_message(sStage == 0, dots, columns);
    }
}
static void ui_box(f32 x, f32 y, f32 width, f32 height, u8 r, u8 g, u8 b, u8 alpha) {
    create_dl_translation_matrix(MENU_MTX_PUSH, x, y, 0);
    create_dl_scale_matrix(MENU_MTX_NOPUSH, width / 130.0f, height / 80.0f, 1);
    gDPSetEnvColor(gDisplayListHead++, r, g, b, alpha);
    gSPDisplayList(gDisplayListHead++, dl_draw_text_bg_box);
    gSPPopMatrix(gDisplayListHead++, G_MTX_MODELVIEW);
}
static void ui_text(s16 x, s16 y, const char *text, s32 maxChars) {
    u8 encoded[256];
    s32 j = 0;
    while (*text && j < maxChars && j < 254) encoded[j++] = glyph(*text++);
    encoded[j] = DIALOG_CHAR_TERMINATOR;
    print_generic_string(x, y, encoded);
}
static void ui_arrow(f32 x, f32 y, s32 up) {
    Vtx *v = alloc_display_list(sizeof(Vtx) * 3);
    s32 i;
    if (!v) return;
    memset(v, 0, sizeof(Vtx) * 3);
    for (i = 0; i < 3; i++) {
        v[i].v.ob[0] = x + (i == 0 ? 0 : i == 1 ? 12 : 6);
        v[i].v.ob[1] = y + ((i == 2) == up ? 9 : 0);
        v[i].v.cn[0] = v[i].v.cn[1] = v[i].v.cn[2] = v[i].v.cn[3] = 255;
    }
    gDPSetEnvColor(gDisplayListHead++, 255, 220, 100, 255);
    gSPVertex(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(v), 3, 0);
    if (up) { gSP1Triangle(gDisplayListHead++, 0, 1, 2, 0); }
    else { gSP1Triangle(gDisplayListHead++, 0, 2, 1, 0); }
}
void ai_npc_scroll(s32 direction) {
    if (!sOpen) return;
    sScroll += direction * 3;
    if (sScroll < 0) sScroll = 0;
    if (sScroll > UI_LINES) sScroll = UI_LINES;
}
void ai_npc_pointer_down(s32 x, s32 y, s32 width, s32 height) {
    f32 ux, uy, right = GFX_DIMENSIONS_FROM_RIGHT_EDGE(0.0f) * 2;
    f32 left = GFX_DIMENSIONS_FROM_LEFT_EDGE(0.0f) * 2;
    if (!sOpen || width <= 0 || height <= 0) return;
    ux = left + (right - left) * x / width;
    uy = 480.0f * (height - y) / height;
    if (ux < right - 48 || ux > right - 8) return;
    if (uy >= 426 && uy <= 466) ai_npc_scroll(4);
    if (uy >= 96 && uy <= 120) ai_npc_scroll(-4);
}
void ai_npc_frame_camera(Vec3f pos, Vec3f focus, f32 fov) {
    f32 dx, dy, dz, length, horizontal, offset, framed[3];
    s32 i, active = sOpen && live(sChat);
    if (sFrameTimer != gGlobalTimer) {
        sFrameTimer = gGlobalTimer;
        sFrameBlend += ((active ? 1.0f : 0.0f) - sFrameBlend) * 0.16f;
        if (!active && sFrameBlend < 0.002f) sFrameBlend = 0;
    }
    if (active) {
        sFrameTarget[0] = (gMarioState->pos[0] + sChat->obj->oPosX) * 0.5f;
        sFrameTarget[1] = (gMarioState->pos[1] + sChat->obj->oPosY) * 0.5f + 85;
        sFrameTarget[2] = (gMarioState->pos[2] + sChat->obj->oPosZ) * 0.5f;
    }
    if (!sFrameBlend) return;
    dx = sFrameTarget[0] - pos[0]; dy = sFrameTarget[1] - pos[1]; dz = sFrameTarget[2] - pos[2];
    horizontal = sqrtf(dx*dx + dz*dz); length = sqrtf(dx*dx + dy*dy + dz*dz);
    if (horizontal < 1 || length < 1) return;
    if (fov < 10 || fov > 100) fov = 45;
    offset = length * tanf(fov * 0.00872664626f) * GFX_DIMENSIONS_ASPECT_RATIO / 3.0f;
    framed[0] = sFrameTarget[0] - dz / horizontal * offset;
    framed[1] = sFrameTarget[1];
    framed[2] = sFrameTarget[2] + dx / horizontal * offset;
    for (i = 0; i < 3; i++) focus[i] += (framed[i] - focus[i]) * sFrameBlend;
}
void ai_npc_render(void) {
    f32 screenLeft = GFX_DIMENSIONS_FROM_LEFT_EDGE(0.0f) * 2;
    f32 right = GFX_DIMENSIONS_FROM_RIGHT_EDGE(0.0f) * 2;
    f32 width = (right - screenLeft) / 3, left = right - width, x, y;
    s32 columns = (width - 44) / 8, i, index, start, rows = 19, length, offset;
    char position[64];
    const char *status;
    if (!sOpen || !live(sChat)) return;
    if (columns < 12) columns = 12;
    if (columns > 90) columns = 90;
    build_lines(columns);
    if (sScroll > sLineCount - rows) sScroll = sLineCount - rows;
    if (sScroll < 0) sScroll = 0;
    start = sLineCount - rows - sScroll;
    if (start < 0) start = 0;
    create_dl_ortho_matrix();
    /* A 2x logical canvas gives half-size conversation UI without changing 3D resolution. */
    create_dl_scale_matrix(MENU_MTX_PUSH, 0.5f, 0.5f, 1);
    ui_box(left, 480, width, 480, 13, 17, 24, 245);
    ui_box(left, 480, width, 56, 26, 33, 44, 255);
    ui_box(left+12, 72, width-24, 46, 32, 42, 52, 255);
    for (i = 0; i < rows && start + i < sLineCount; i++) {
        index = start + i; y = 402 - i * 16;
        if (sLines[index].mario < 0) continue;
        x = left + (sLines[index].mario ? 24 : 12);
        ui_box(x, y+16, width-36, 16, sLines[index].mario ? 38 : 27,
               sLines[index].mario ? 62 : 36, sLines[index].mario ? 67 : 47, 255);
    }
    ui_arrow(right-32, 442, 1); ui_arrow(right-32, 102, 0);
    gSPDisplayList(gDisplayListHead++, dl_ia_text_begin);
    gDPSetEnvColor(gDisplayListHead++, 255, 220, 100, 255);
    ui_text(left+12, 460, sChat->kind == 1 ? "TOAD" : "BOB-OMB BUDDY", columns);
    gDPSetEnvColor(gDisplayListHead++, 165, 180, 195, 255);
    ui_text(left+12, 440, "CONVERSATION", columns);
    for (i = 0; i < rows && start + i < sLineCount; i++) {
        index = start + i; y = 402 - i * 16;
        if (sLines[index].mario < 0) continue;
        x = left + (sLines[index].mario ? 28 : 16);
        if (sLines[index].heading) { gDPSetEnvColor(gDisplayListHead++, 255, 220, 100, 255); }
        else { gDPSetEnvColor(gDisplayListHead++, 235, 240, 245, 255); }
        ui_text(x, y, sLines[index].text, columns);
    }
    status = sWaiting ? (sStage == 0 ? "PREPARING MARIO VOICE" : sStage == 1 ? "THINKING..." : "SPEAKING...") : "ENTER: SEND";
    if (!sWaiting && strstr(sStatus, "TIMEOUT")) status = "TIMED OUT: ENTER TO RETRY";
    gDPSetEnvColor(gDisplayListHead++, 255, 220, 100, 255);
    ui_text(left+12, 84, status, columns);
    gDPSetEnvColor(gDisplayListHead++, 235, 240, 245, 255);
    length = strlen(sText); offset = length > columns*2 ? length-columns*2 : 0;
    ui_text(left+18, 54, sText[0] ? sText+offset : "Type a message...", columns);
    if (length-offset > columns) ui_text(left+18, 38, sText+offset+columns, columns);
    snprintf(position, sizeof(position), "UP/DOWN: SCROLL  ESC: CLOSE");
    gDPSetEnvColor(gDisplayListHead++, 165, 180, 195, 255);
    ui_text(left+12, 10, position, columns+2);
    gSPDisplayList(gDisplayListHead++, dl_ia_text_end);
    gSPPopMatrix(gDisplayListHead++, G_MTX_MODELVIEW);
}

#else
void ai_npc_native_page(UNUSED s32 dialog, UNUSED s32 start, UNUSED s32 lines,
                        UNUSED const u8 *encoded, UNUSED s32 visible) {}
void ai_npc_reset(void) {}
void ai_npc_update(void) {}
void ai_npc_render(void) {}
void ai_npc_scroll(UNUSED s32 direction) {}
void ai_npc_pointer_down(UNUSED s32 x, UNUSED s32 y, UNUSED s32 w, UNUSED s32 h) {}
void ai_npc_frame_camera(UNUSED Vec3f pos, UNUSED Vec3f focus, UNUSED f32 fov) {}
void ai_npc_filter_controller(void) {}
s32 ai_npc_key_down(UNUSED s32 key) { return 0; }
s32 ai_npc_key_up(UNUSED s32 key) { return 0; }
void ai_npc_keys_up(void) {}
s32 ai_npc_tick(UNUSED struct Object *obj, UNUSED s32 kind) { return 0; }
void ai_npc_offer_reply(UNUSED struct Object *obj) {}
#endif
