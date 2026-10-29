#ifndef AI_NPC_H
#define AI_NPC_H
#include <PR/ultratypes.h>
#ifdef __cplusplus
extern "C" {
#endif
struct Object;
/* PC companion only. The N64 build supplies no-op implementations. */
void ai_npc_reset(void);
void ai_npc_update(void);
void ai_npc_render(void);
void ai_npc_filter_controller(void);
s32 ai_npc_key_down(s32 key);
s32 ai_npc_key_up(s32 key);
void ai_npc_keys_up(void);
s32 ai_npc_tick(struct Object *obj, s32 kind);
void ai_npc_offer_reply(struct Object *obj);
#ifdef __cplusplus
}
#endif
#endif
