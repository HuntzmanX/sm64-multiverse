#ifndef AI_NPC_H
#define AI_NPC_H
#include <PR/ultratypes.h>
#ifdef __cplusplus
extern "C" {
#endif
struct Object;
/* PC companion only. The N64 build supplies no-op implementations. */
void ai_npc_reset(void);
void ai_npc_native_page(s32 dialog, s32 start, s32 lines, const u8 *encoded, s32 visible);
void ai_npc_update(void);
void ai_npc_render(void);
void ai_npc_frame_camera(f32 pos[3], f32 focus[3], f32 fov);
void ai_npc_scroll(s32 direction);
void ai_npc_pointer_down(s32 x, s32 y, s32 width, s32 height);
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
