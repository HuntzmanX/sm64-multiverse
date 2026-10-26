#ifndef AI_INPUT_H
#define AI_INPUT_H

#include <PR/ultratypes.h>
#include <PR/os_cont.h>

#ifdef __cplusplus
extern "C" {
#endif

s32 ai_input_on_key_down(s32 scancode);
s32 ai_input_on_key_up(s32 scancode);
void ai_input_on_all_keys_up(void);
void ai_input_capture_physical_pad(OSContPad *pad);

s32 ai_input_move_forward(void);
s32 ai_input_move_right(void);
s32 ai_input_move_vertical(void);
s32 ai_input_look_yaw(void);
s32 ai_input_look_pitch(void);
s32 ai_input_is_typing(void);
s32 ai_input_take_freecam_toggle(void);
void ai_input_add_mouse_delta(s32 x, s32 y);
void ai_input_set_mouse_look(s32 active);
s32 ai_input_take_mouse_x(void);
s32 ai_input_take_mouse_y(void);

void ai_input_render(void);

#ifdef __cplusplus
}
#endif

#endif
