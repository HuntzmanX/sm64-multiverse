/* Headless tests of the actual state machine and object_helpers movement.
 * Collision queries use a flat test floor; rendering uses stubs. */
#include <assert.h>
#include <unistd.h>
#include "../src/game/ai_npc.c"

static struct MarioState mario;
static struct Object marioObj, npc;
static struct Area area;
static struct Controller controller;
struct MarioState *gMarioState = &mario;
struct Area *gCurrentArea = &area;
struct Controller *gPlayer1Controller = &controller;
struct Object *gCurrentObject = &npc;
struct WarpTransition gWarpTransition;
s32 gDialogVariable = 7;
s16 sCurrPlayMode, gCurrSaveFileNum = 1, gCurrLevelNum = 6, gCurrAreaIndex = 1;
s16 gCurrCourseNum, gCurrActNum = 1;
u32 gGlobalTimer = 10;
f32 gSineTable[0x1400];
#ifdef WIDESCREEN
struct GfxDimensions gfx_current_dimensions = {1280, 720, 16.0f / 9.0f};
#endif
static int spawnedCoins, cannonUnlocks, knockbacks;
static struct Surface surface;
s16 get_dialog_id(void) { return DIALOG_NONE; }
void save_file_set_cannon_unlocked(void) { cannonUnlocks++; }
void obj_spawn_loot_yellow_coins(struct Object *obj, s32 count, f32 unused) {
    assert(obj->oNumLootCoins == count);
    spawnedCoins += count;
    obj->oNumLootCoins = 0;
}
s16 atan2s(f32 y, f32 x) { return 0; }
f32 find_floor(f32 x, f32 y, f32 z, struct Surface **floor) { *floor = &surface; return 0; }
s32 find_wall_collisions(struct WallCollisionData *data) { return 0; }
f32 find_water_level(f32 x, f32 z) { return FLOOR_LOWER_LIMIT; }
s32 drop_and_set_mario_action(struct MarioState *m, u32 action, u32 arg) { knockbacks++; return 1; }
u32 set_mario_action(struct MarioState *m, u32 action, u32 arg) {
    m->action = action; m->actionArg = arg;
    m->actionState = m->actionTimer = 0;
    return TRUE;
}

/* Record UI draw calls without game textures to exercise layout and half scaling. */
static Gfx drawBuffer[8192];
Gfx *gDisplayListHeadInChunk = drawBuffer;
Gfx *gDisplayListEndInChunk = drawBuffer + 8192;
Gfx dl_ia_text_begin[1], dl_ia_text_end[1], dl_draw_text_bg_box[1];
static u8 vertexArena[1024];
static s32 vertexUsed, textDraws, scaleCalls, translateCalls;
static f32 firstScale, panelLeft;
Gfx **alloc_next_dl(void) { assert(!"UI display list overflow"); return &gDisplayListHeadInChunk; }
void *alloc_display_list(u32 size) {
    void *ptr = vertexArena + vertexUsed;
    vertexUsed += (size+7) & ~7;
    assert(vertexUsed <= sizeof(vertexArena));
    return ptr;
}
void create_dl_ortho_matrix(void) {}
void create_dl_translation_matrix(s8 op, f32 x, f32 y, f32 z) {
    if (translateCalls++ == 0) panelLeft = x;
}
void create_dl_scale_matrix(s8 op, f32 x, f32 y, f32 z) {
    if (scaleCalls++ == 0) { firstScale = x; assert(x == y && x == 0.5f); }
}
void print_generic_string(s16 x, s16 y, const u8 *text) {
    s32 i;
    f32 right = GFX_DIMENSIONS_FROM_RIGHT_EDGE(0.0f) * 2;
    assert(x >= panelLeft && x < right && y >= 0 && y <= 464);
    for (i = 0; text[i] != DIALOG_CHAR_TERMINATOR; i++) assert(i < 255);
    textDraws++;
}
static void tick(void) { ai_npc_tick(&npc, 2); }
static void key(int scan) { ai_npc_key_down(scan); ai_npc_key_up(scan); }
static void fixture(void) {
    s32 i;
    ai_npc_reset(); ai_npc_keys_up();
    memset(&npc, 0, sizeof(npc)); memset(&mario, 0, sizeof(mario));
    memset(&controller, 0, sizeof(controller));
    mario.marioObj = &marioObj; mario.action = ACT_IDLE;
    npc.activeFlags = ACTIVE_FLAG_ACTIVE;
    npc.oGravity = 2.5f; /* Native Buddy object_step convention. */
    surface.normal.y = 1;
    for (i = 0; i < 0x1400; i++) gSineTable[i] = sinf(i * 6.28318530718f / 0x1000);
    npc.oPosX = 200;
    npc.oBobombBuddyRole = BOBOMB_BUDDY_ROLE_ADVICE;
    spawnedCoins = cannonUnlocks = knockbacks = 0;
    tick();
}
static void type_hello(void) { key(35); key(18); key(38); key(38); key(24); }
static void reply_ending(const char *token, int action, int count, int end) {
    FILE *f = fopen(".sm64-npc-response", "w"); assert(f);
    fprintf(f, "%s\n%d %d %d\nHello Mario!\n", token, action, count, end); fclose(f);
    gGlobalTimer += (3 - gGlobalTimer % 3) % 3;
    tick(); poll_reply();
}
static void reply(const char *token, int action, int count) { reply_ending(token, action, count, 0); }
static void reveal(const char *token, s32 sequence, s32 speaker, const char *text) {
    FILE *f = fopen(".sm64-npc-event", "w"); assert(f);
    fprintf(f, "%s\n%d %d\n%s\n", token, sequence, speaker, text); fclose(f);
    tick(); poll_event();
}
static void check_ui_and_timing(void) {
    char token[80], ack[128], packet[2048];
    FILE *f;
    s32 i;
    f32 pos[3] = {0, 85, 1000}, focus[3], ndc, previous;
    fixture(); key(20); type_hello(); key(28); strcpy(token, sToken);
    assert(!sText[0] && sChat->historyCount == 0 && sStage == 0);
    f = fopen(".sm64-npc-request.json", "r"); assert(f);
    packet[fread(packet, 1, sizeof(packet)-1, f)] = 0; fclose(f);
    assert(strstr(packet, "\"protocol_version\":3") && strstr(packet, "\"history_count\":0"));
    build_lines(30); assert(sLineCount == 3 && !strcmp(sLines[0].text, "MARIO"));
    reveal("0-0-0", 1, 1, "old"); assert(sChat->historyCount == 0);
    reveal(token, 1, 1, "ignored in favour of actual player input");
    assert(sChat->historyCount == 1 && sStage == 1);
    assert(sChat->history[0].mario && !strcmp(sChat->history[0].text, "hello"));
    f = fopen(".sm64-npc-event-ack", "r"); assert(f);
    assert(fgets(ack, sizeof(ack), f)); ack[strcspn(ack, "\r\n")] = 0;
    assert(!strcmp(ack, token)); assert(fgets(ack, sizeof(ack), f) && atoi(ack) == 1); fclose(f);
    reveal(token, 1, 1, "duplicate"); assert(sChat->historyCount == 1);
    build_lines(30); assert(!strcmp(sLines[sLineCount-3].text, "BOB-OMB BUDDY"));
    reveal(token, 2, 2, "Hello Mario!");
    assert(sChat->historyCount == 2 && sStage == 2 && sWaiting && !sChat->pending);
    assert(!sChat->history[1].mario && !strcmp(sChat->history[1].text, "Hello Mario!"));
    reply(token, NPC_FOLLOW, 0);
    assert(!sWaiting && !sOpen && !sChat && sNpcs[0].historyCount == 2 && sNpcs[0].pending == NPC_FOLLOW);
    tick(); assert(sNpcs[0].mode == NPC_FOLLOW);

    fixture(); key(20); type_hello(); key(28); strcpy(token, sToken);
    reveal(token, 1, 3, "Old Mario message"); reveal(token, 2, 4, "Old NPC reply");
    assert(sChat->historyCount == 2 && !sPlayerShown && !sNpcShown && sStage == 0);
    reveal(token, 3, 1, "hello"); reveal(token, 4, 2, "New reply");
    assert(sChat->historyCount == 4 && sPlayerShown && sNpcShown);
    for (i = 0; i < 40; i++) append_message(i % 2, "A long message that should wrap neatly at a word boundary in the side panel.");
    assert(sChat->historyCount == HISTORY_MESSAGES);
    build_lines(30); assert(sLineCount < UI_LINES);
    for (i = 0; i < sLineCount; i++) assert(strlen(sLines[i].text) <= 30);
    key(0x148); assert(sScroll == 3); key(0x150); assert(sScroll == 0);
    ai_npc_scroll(-100); assert(sScroll == 0);
    ai_npc_pointer_down(980, 80, 1000, 1000); assert(sScroll == 12);
    ai_npc_pointer_down(980, 780, 1000, 1000); assert(sScroll == 0);
    key(0x147); assert(sScroll == UI_LINES); key(0x14F); assert(sScroll == 0);
    scaleCalls = translateCalls = textDraws = vertexUsed = 0;
    ai_npc_render();
    assert(firstScale == 0.5f && textDraws >= 10 && textDraws <= 26);
    assert(fabsf(panelLeft - (GFX_DIMENSIONS_FROM_RIGHT_EDGE(0.0f)*2 -
           (GFX_DIMENSIONS_FROM_RIGHT_EDGE(0.0f)-GFX_DIMENSIONS_FROM_LEFT_EDGE(0.0f))*2/3)) < 1);
    key(0x147); ai_npc_render(); assert(sScroll > 0 && sScroll < UI_LINES);

    /* At steady state the pair's midpoint projects to x=1/3; camera position is untouched. */
    fixture(); npc.oPosX = 0; key(20);
    for (i = 0; i < 80; i++) {
        gGlobalTimer++; tick();
        focus[0] = focus[2] = 0; focus[1] = 85;
        ai_npc_frame_camera(pos, focus, 45);
    }
    assert(pos[0] == 0 && pos[1] == 85 && pos[2] == 1000);
    ndc = -focus[0] / (1000 * tanf(45 * 0.00872664626f) * GFX_DIMENSIONS_ASPECT_RATIO);
    assert(fabsf(ndc + 1.0f/3) < 0.001f);
    key(1); previous = focus[0];
    for (i = 0; i < 80; i++) {
        gGlobalTimer++; focus[0] = focus[2] = 0; focus[1] = 85;
        ai_npc_frame_camera(pos, focus, 45);
        assert(focus[0] <= previous); previous = focus[0];
    }
    assert(sFrameBlend == 0 && focus[0] == 0);
    remove(".sm64-npc-event"); remove(".sm64-npc-event-ack");
}
static void check_conversation_sleep(void) {
    u32 sleepActions[] = { ACT_START_SLEEPING, ACT_SLEEPING, ACT_WAKING_UP };
    s32 i;
    fixture(); key(20);
    mario.actionTimer = 10; mario.actionState = 3;
    ai_npc_update();
    assert(sOpen && mario.action == ACT_IDLE && !mario.actionTimer && !mario.actionState);
    for (i = 0; i < 3; i++) {
        mario.action = sleepActions[i];
        ai_npc_update();
        assert(mario.action == ACT_IDLE && !mario.actionTimer && !mario.actionState);
    }
    mario.action = ACT_WALKING; mario.actionTimer = 42;
    ai_npc_update(); assert(mario.action == ACT_WALKING && mario.actionTimer == 42);
    key(1);
    mario.action = ACT_IDLE; mario.actionTimer = 10; mario.actionState = 3;
    ai_npc_update(); assert(mario.actionTimer == 10 && mario.actionState == 3);
    mario.action = ACT_SLEEPING;
    ai_npc_update(); assert(mario.action == ACT_SLEEPING);
    key(20); ai_npc_update();
    assert(sOpen && mario.action == ACT_IDLE);
}
static void check_grounded_movement(void) {
    s32 kinds[] = { 1, 2 }, modes[] = { NPC_FOLLOW, NPC_FLEE, NPC_APPROACH, NPC_HOSTILE };
    s32 k, a, i;
    for (k = 0; k < 2; k++) for (a = 0; a < 4; a++) {
        fixture();
        sNpcs[0].kind = kinds[k];
        npc.oGravity = kinds[k] == 2 ? 2.5f : 0;
        npc.oWallHitboxRadius = 17;
        mario.pos[2] = 600;
        sNpcs[0].pending = modes[a];
        for (i = 0; i < 120; i++) {
            ai_npc_tick(&npc, kinds[k]);
            assert(npc.oPosY == 0 && npc.oVelY == 0);
            assert(npc.oGravity == (kinds[k] == 2 ? 2.5f : 0));
            assert(npc.oWallHitboxRadius == 17);
        }
        assert(npc.oPosZ != 0); /* Real engine movement, not a no-op stub. */
        /* An elevated NPC falls back to the floor rather than accelerating up. */
        npc.oPosY = 60;
        sNpcs[0].pending = NPC_FOLLOW;
        ai_npc_tick(&npc, kinds[k]);
        assert(npc.oPosY < 60 && npc.oVelY < 0);
        for (i = 0; i < 20; i++) ai_npc_tick(&npc, kinds[k]);
        assert(npc.oPosY == 0 && npc.oVelY == 0);
        sNpcs[0].pending = NPC_STOP;
        ai_npc_tick(&npc, kinds[k]);
        assert(!sNpcs[0].mode && npc.oGravity == (kinds[k] == 2 ? 2.5f : 0));
        sNpcs[0].pending = NPC_FOLLOW;
        ai_npc_tick(&npc, kinds[k]);
        sNpcs[0].frames = 1;
        ai_npc_tick(&npc, kinds[k]);
        assert(!sNpcs[0].mode && npc.oGravity == (kinds[k] == 2 ? 2.5f : 0));
    }
}
static void check_npc_conversation_end(void) {
    char token[80], receipt[1024];
    FILE *f;
    fixture(); key(20); type_hello(); key(28); strcpy(token, sToken);
    reveal(token, 1, 1, "hello"); reveal(token, 2, 2, "Goodbye, Mario!");
    assert(sOpen && sWaiting && !sNpcs[0].pending && !sNpcs[0].mode);
    reply_ending(token, NPC_NONE, 0, 1);
    assert(!sOpen && !sWaiting && !sChat && !sNpcs[0].pending);
    assert(sNpcs[0].historyCount == 2 && !sNpcs[0].mode);
    assert(access(".sm64-npc-request.json", F_OK) != 0);
    f = fopen(".sm64-npc-result.json", "r"); assert(f);
    receipt[fread(receipt, 1, sizeof(receipt)-1, f)] = 0; fclose(f);
    assert(strstr(receipt, token) && strstr(receipt, "\"action\":\"none\""));
    controller.buttonDown = A_BUTTON; ai_npc_filter_controller(); assert(controller.buttonDown == A_BUTTON);
    key(20); assert(sOpen && sChat->historyCount == 2);
    type_hello(); key(28); strcpy(token, sToken);
    reply(token, NPC_NONE, 0); assert(sOpen && !sWaiting && !sNpcs[0].pending);
    type_hello(); key(28); strcpy(token, sToken);
    reply_ending(token, NPC_NONE, 0, 2); assert(sOpen && !sWaiting && strstr(sStatus, "INVALID"));

    /* Even Toad waits for the final completion packet before attacking. */
    fixture(); sNpcs[0].kind = 1;
    npc.oOpacity = 81; key(20); type_hello(); key(28); strcpy(token, sToken);
    reveal(token, 1, 2, "That's it! Put 'em up!");
    npc.oPosX = 50; tick(); assert(sOpen && !knockbacks);
    reply_ending(token, NPC_HOSTILE, 0, 1); assert(!sOpen && !knockbacks);
    ai_npc_tick(&npc, 1); assert(knockbacks == 1 && sNpcs[0].mode == NPC_HOSTILE);
    ai_npc_tick(&npc, 1); assert(knockbacks == 1);
    key(20); type_hello(); key(28); strcpy(token, sToken);
    reply_ending(token, NPC_FRIENDLY, 0, 1);
    ai_npc_tick(&npc, 1); assert(!sNpcs[0].mode && npc.oOpacity == 81);

    fixture(); key(20); type_hello(); key(28); strcpy(token, sToken);
    reveal(token, 1, 2, "I'll follow!"); key(1);
    reply_ending(token, NPC_FOLLOW, 0, 1); tick(); assert(!sOpen && !sNpcs[0].mode);
}
static void check_original_dialogue_voice(void) {
    u8 encoded[] = { ASCII_TO_DIALOG('H'), ASCII_TO_DIALOG('i'), DIALOG_CHAR_SPACE,
        0x54, 0xF2, DIALOG_CHAR_NEWLINE, ASCII_TO_DIALOG('G'), ASCII_TO_DIALOG('o'),
        DIALOG_CHAR_SPACE, DIALOG_CHAR_MULTI_THE, DIALOG_CHAR_SPACE, DIALOG_CHAR_MULTI_YOU,
        DIALOG_CHAR_SPACE, DIALOG_CHAR_STAR_COUNT, DIALOG_CHAR_TERMINATOR };
    char packet[2048], before[2048];
    FILE *file;
    u32 counter;
    struct Object other;
    fixture(); mario.interactObj = &npc;
    ai_npc_native_page(4, 0, 1, encoded, TRUE);
    file = fopen(".sm64-npc-native.json", "r"); assert(file);
    packet[fread(packet, 1, sizeof(packet)-1, file)] = 0; fclose(file);
    assert(strstr(packet, "\"source\":\"default\"") && strstr(packet, "\"kind\":\"bobomb_buddy\""));
    assert(strstr(packet, "\"text\":\"Hi A button!\""));
    strcpy(before, packet); counter = sRequest;
    ai_npc_native_page(4, 0, 1, encoded, TRUE);
    assert(sRequest == counter); /* Repeated render frames do not request more audio. */
    gGlobalTimer += 30; tick();
    ai_npc_native_page(4, 0, 1, encoded, TRUE);
    assert(sRequest == counter); /* Heartbeats retain the page request ID. */
    ai_npc_native_page(4, 6, 2, encoded, TRUE);
    file = fopen(".sm64-npc-native.json", "r"); assert(file);
    packet[fread(packet, 1, sizeof(packet)-1, file)] = 0; fclose(file);
    assert(strcmp(packet, before) && strstr(packet, "\"page\":6"));
    assert(strstr(packet, "\"text\":\"Go the you 7\""));
    ai_npc_native_page(4, 6, 2, encoded, FALSE);
    assert(access(".sm64-npc-native.json", F_OK) != 0);
    mario.interactObj = &other; mario.usedObj = NULL;
    ai_npc_native_page(4, 0, 1, encoded, TRUE);
    assert(access(".sm64-npc-native.json", F_OK) != 0); /* Signs/other speakers are not this NPC. */
    mario.interactObj = NULL; mario.usedObj = &npc;
    ai_npc_native_page(4, 0, 1, encoded, TRUE);
    assert(access(".sm64-npc-native.json", F_OK) == 0);
    key(20); assert(sOpen && access(".sm64-npc-native.json", F_OK) != 0);
    key(1); ai_npc_native_page(4, 0, 1, encoded, TRUE);
    ai_npc_reset(); assert(access(".sm64-npc-native.json", F_OK) != 0);
}
int main(void) {
    char temp[] = "/tmp/sm64-npc-test-XXXXXX", token[80];
    assert(mkdtemp(temp)); assert(chdir(temp) == 0);

    fixture();
    key(20); assert(sOpen); type_hello(); assert(strcmp(sText, "hello") == 0);
    controller.buttonDown = controller.buttonPressed = A_BUTTON;
    controller.stickMag = 64; ai_npc_filter_controller();
    assert(!controller.buttonDown && !controller.buttonPressed && !controller.stickMag);
    key(28); assert(sWaiting); strcpy(token, sToken);
    reply("0-0-0", NPC_COINS, 3); assert(sWaiting && !spawnedCoins);
    reply(token, NPC_COINS, 3); assert(!sWaiting && !sOpen && sNpcs[0].pending == NPC_COINS);
    assert(!spawnedCoins); /* Execute only in the object's own callback. */
    assert(access(".sm64-npc-request.json", F_OK) != 0);
    tick(); assert(spawnedCoins == 3);
    tick(); assert(spawnedCoins == 3); /* Never execute a response twice. */

    fixture(); key(20); type_hello(); key(28); strcpy(token, sToken);
    key(1); assert(!sOpen && !sWaiting); assert(access(".sm64-npc-request.json", F_OK) != 0);
    reply(token, NPC_COINS, 3); tick(); assert(!spawnedCoins);

    fixture(); key(20); type_hello(); key(28); strcpy(token, sToken);
    ai_npc_reset(); reply(token, NPC_COINS, 3); tick(); assert(!spawnedCoins);

    fixture(); key(20); type_hello(); key(28); strcpy(token, sToken);
    reply(token, NPC_CANNON, 0); assert(!cannonUnlocks && !sChat->pending);
    assert(strstr(sStatus, "INVALID"));

    fixture(); npc.oBobombBuddyRole = BOBOMB_BUDDY_ROLE_CANNON;
    key(20); type_hello(); key(28); strcpy(token, sToken);
    reply(token, NPC_CANNON, 0); tick(); assert(!sOpen && cannonUnlocks == 1);

    fixture(); ai_npc_offer_reply(&npc); ai_npc_update(); assert(sOpen);
    key(1); assert(!sOpen);

    fixture(); key(20); type_hello(); key(28);
    gGlobalTimer += CHAT_TIMEOUT + 3; tick(); poll_reply(); assert(!sWaiting && !sToken[0]);
    key(1); assert(!sOpen);

    fixture(); key(20); type_hello(); key(28); strcpy(token, sToken);
    reply(token, NPC_HOSTILE, 0); npc.oPosX = 50; tick();
    assert(knockbacks == 1 && mario.hurtCounter == 4 && mario.invincTimer == 60);
    tick(); assert(knockbacks == 1);
    key(20); tick(); assert(sOpen && knockbacks == 1);
    fixture(); ai_npc_reset();
    npc.oOpacity = 81; npc.oToadMessageState = 0;
    ai_npc_tick(&npc, 1); key(20); ai_npc_tick(&npc, 1);
    assert(npc.oOpacity == 255); key(1);
    assert(npc.oOpacity == 81 && npc.oToadMessageState == 0);

    check_ui_and_timing();
    check_conversation_sleep();
    check_grounded_movement();
    check_npc_conversation_end();
    check_original_dialogue_voice();
    puts("Game-side NPC tests passed: engine gravity and grounded movement, UI history, scroll, voice reveals, camera framing, input, stale replies, cancellation, single execution, capabilities, timeout, hostility.");
    ai_npc_reset();
    remove(".sm64-npc-response"); remove(".sm64-npc-result.json");
    assert(chdir("/") == 0); rmdir(temp);
    return 0;
}
