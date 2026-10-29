/* Headless tests of the actual game-side state machine. Engine functions below
 * are stubs; rendering and full game physics require the user's extracted assets. */
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
struct WarpTransition gWarpTransition;
s16 sCurrPlayMode, gCurrSaveFileNum = 1, gCurrLevelNum = 6, gCurrAreaIndex = 1;
s16 gCurrCourseNum, gCurrActNum = 1;
u32 gGlobalTimer = 10;
f32 gSineTable[0x1400];
static int spawnedCoins, cannonUnlocks, knockbacks;
static struct Surface surface;
s16 get_dialog_id(void) { return DIALOG_NONE; }
void save_file_set_cannon_unlocked(void) { cannonUnlocks++; }
void obj_spawn_loot_yellow_coins(struct Object *obj, s32 count, f32 unused) {
    assert(obj->oNumLootCoins == count);
    spawnedCoins += count;
    obj->oNumLootCoins = 0;
}
s16 approach_s16_symmetric(s16 value, s16 target, s16 increment) { return target; }
s16 atan2s(f32 y, f32 x) { return 0; }
f32 find_floor(f32 x, f32 y, f32 z, struct Surface **floor) { *floor = &surface; return 0; }
void cur_obj_update_floor_and_walls(void) {}
void cur_obj_move_standard(s16 degrees) {}
s32 drop_and_set_mario_action(struct MarioState *m, u32 action, u32 arg) { knockbacks++; return 1; }

static void tick(void) { ai_npc_tick(&npc, 2); }
static void key(int scan) { ai_npc_key_down(scan); ai_npc_key_up(scan); }
static void fixture(void) {
    ai_npc_reset(); ai_npc_keys_up();
    memset(&npc, 0, sizeof(npc)); memset(&mario, 0, sizeof(mario));
    memset(&controller, 0, sizeof(controller));
    mario.marioObj = &marioObj; mario.action = ACT_IDLE;
    npc.activeFlags = ACTIVE_FLAG_ACTIVE;
    npc.oPosX = 200;
    npc.oBobombBuddyRole = BOBOMB_BUDDY_ROLE_ADVICE;
    spawnedCoins = cannonUnlocks = knockbacks = 0;
    tick();
}
static void type_hello(void) { key(35); key(18); key(38); key(38); key(24); }
static void reply(const char *token, int action, int count) {
    FILE *f = fopen(".sm64-npc-response", "w"); assert(f);
    fprintf(f, "%s\n%d %d\nHello Mario!\n", token, action, count); fclose(f);
    gGlobalTimer += (3 - gGlobalTimer % 3) % 3;
    tick(); poll_reply();
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
    reply(token, NPC_COINS, 3); assert(!sWaiting && sChat->pending == NPC_COINS);
    tick(); assert(!spawnedCoins); /* Wait for the player to close the chat. */
    key(1); tick(); assert(spawnedCoins == 3);
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
    reply(token, NPC_CANNON, 0); key(1); tick(); assert(cannonUnlocks == 1);

    fixture(); ai_npc_offer_reply(&npc); ai_npc_update(); assert(sOpen);
    key(1); assert(!sOpen);

    fixture(); key(20); type_hello(); key(28);
    gGlobalTimer += 1803; tick(); poll_reply(); assert(!sWaiting && !sToken[0]);
    key(1); assert(!sOpen);

    fixture(); key(20); type_hello(); key(28); strcpy(token, sToken);
    reply(token, NPC_HOSTILE, 0); key(1); npc.oPosX = 50; tick();
    assert(knockbacks == 1 && mario.hurtCounter == 4 && mario.invincTimer == 60);
    tick(); assert(knockbacks == 1);
    key(20); tick(); assert(sOpen && knockbacks == 1);
    fixture(); ai_npc_reset();
    npc.oOpacity = 81; npc.oToadMessageState = 0;
    ai_npc_tick(&npc, 1); key(20); ai_npc_tick(&npc, 1);
    assert(npc.oOpacity == 255); key(1);
    assert(npc.oOpacity == 81 && npc.oToadMessageState == 0);

    puts("Game-side NPC tests passed: input, stale replies, cancellation, single execution, capabilities, timeout, hostility.");
    ai_npc_reset();
    remove(".sm64-npc-response"); remove(".sm64-npc-result.json");
    assert(chdir("/") == 0); rmdir(temp);
    return 0;
}
