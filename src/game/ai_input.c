#include <ultra64.h>

#include "sm64.h"
#include "ai_input.h"

#ifndef TARGET_N64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dialog_ids.h"
#include "ai_freecam.h"
#include "game_init.h"
#include "ingame_menu.h"
#include "memory.h"
#include "segment2.h"

#define AI_INPUT_REQUEST_FILE ".sm64-ai-request"
#define AI_INPUT_MAX_LENGTH 120
#define AI_INPUT_ENCODED_LENGTH 160

#define KEY_ESCAPE    0x01
#define KEY_BACKSPACE 0x0E
#define KEY_TAB       0x0F
#define KEY_ENTER     0x1C
#define KEY_LEFTSHIFT 0x2A
#define KEY_RIGHTSHIFT 0x36
#define KEY_F1        0x3B
#define KEY_UP        0x148
#define KEY_LEFT      0x14B
#define KEY_RIGHT     0x14D
#define KEY_DOWN      0x150

static s32 sTyping;
static s32 sShiftDown;
static s32 sFreecamToggle;
static s32 sKeys[0x200];
static char sText[AI_INPUT_MAX_LENGTH + 1];
static s32 sTextLength;
static s32 sSentTimer;
static s32 sMouseX;
static s32 sMouseY;
static s32 sMouseLook;
static s8 sPhysicalStickX;
static s8 sPhysicalStickY;
static u16 sPhysicalButtons;
static u8 sDisplayText[AI_INPUT_ENCODED_LENGTH];

static const char *ai_input_request_path(void) {
    const char *path = getenv("SM64_AI_REQUEST_FILE");
    return path != NULL && path[0] != '\0' ? path : AI_INPUT_REQUEST_FILE;
}

static char ai_input_scancode_char(s32 key, s32 shifted) {
    static const char normal[] = "1234567890-=";
    static const char upper[] = "!@#$%^&*()_+";

    if (key >= 0x02 && key <= 0x0D) return shifted ? upper[key - 0x02] : normal[key - 0x02];
    if (key >= 0x10 && key <= 0x19) return (shifted ? "QWERTYUIOP" : "qwertyuiop")[key - 0x10];
    if (key >= 0x1E && key <= 0x26) return (shifted ? "ASDFGHJKL" : "asdfghjkl")[key - 0x1E];
    if (key >= 0x2C && key <= 0x32) return (shifted ? "ZXCVBNM" : "zxcvbnm")[key - 0x2C];
    if (key == 0x39) return ' ';
    if (key == 0x27) return shifted ? ':' : ';';
    if (key == 0x28) return shifted ? '"' : '\'';
    if (key == 0x33) return shifted ? '<' : ',';
    if (key == 0x34) return shifted ? '>' : '.';
    if (key == 0x35) return shifted ? '?' : '/';
    return '\0';
}

static u8 ai_input_dialog_char(char c) {
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

static void ai_input_build_display(void) {
    const char *prefix = sSentTimer > 0 ? "SENT: " : "MARIO> ";
    s32 output = 0;
    s32 column = 0;
    s32 i;

    for (i = 0; prefix[i] != '\0' && output < AI_INPUT_ENCODED_LENGTH - 1; i++) {
        sDisplayText[output++] = ai_input_dialog_char(prefix[i]);
        column++;
    }
    for (i = 0; sText[i] != '\0' && output < AI_INPUT_ENCODED_LENGTH - 2; i++) {
        if (column >= 36) {
            sDisplayText[output++] = DIALOG_CHAR_NEWLINE;
            column = 0;
        }
        sDisplayText[output++] = ai_input_dialog_char(sText[i]);
        column++;
    }
    if (sTyping && output < AI_INPUT_ENCODED_LENGTH - 1) {
        sDisplayText[output++] = ai_input_dialog_char('_');
    }
    sDisplayText[output] = DIALOG_CHAR_TERMINATOR;
}

static void ai_input_submit(void) {
    const char *path;
    char temporaryPath[512];
    FILE *file;

    if (sTextLength == 0) return;
    path = ai_input_request_path();
    if (snprintf(temporaryPath, sizeof(temporaryPath), "%s.tmp", path) >= (s32) sizeof(temporaryPath)) return;
    file = fopen(temporaryPath, "w");
    if (file == NULL) return;
    fprintf(file, "%s\n", sText);
    fclose(file);
    remove(path);
    rename(temporaryPath, path);
    sSentTimer = 60;
    sTyping = FALSE;
}

s32 ai_input_on_key_down(s32 key) {
    char c;
    s32 wasDown = FALSE;
    if (key >= 0 && key < (s32) (sizeof(sKeys) / sizeof(sKeys[0]))) wasDown = sKeys[key];
    if (key >= 0 && key < (s32) (sizeof(sKeys) / sizeof(sKeys[0]))) sKeys[key] = TRUE;
    if (key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT) sShiftDown = TRUE;

    if (!sTyping && key == KEY_TAB && !wasDown) {
        sTyping = TRUE;
        sTextLength = 0;
        sText[0] = '\0';
        sMouseLook = FALSE;
        sMouseX = 0;
        sMouseY = 0;
        return TRUE;
    }
    if (!sTyping && key == KEY_F1 && !wasDown) {
        sFreecamToggle = TRUE;
        return TRUE;
    }
    if (sTyping) {
        if (key == KEY_ESCAPE || (key == KEY_TAB && !wasDown)) {
            sTyping = FALSE;
        } else if (key == KEY_ENTER) {
            ai_input_submit();
        } else if (key == KEY_BACKSPACE && sTextLength > 0) {
            sText[--sTextLength] = '\0';
        } else if ((c = ai_input_scancode_char(key, sShiftDown)) != '\0'
                   && sTextLength < AI_INPUT_MAX_LENGTH) {
            sText[sTextLength++] = c;
            sText[sTextLength] = '\0';
        }
        return TRUE;
    }

    return ai_freecam_is_enabled()
        && (key == 0x11 || key == 0x1E || key == 0x1F || key == 0x20
            || key == KEY_UP || key == KEY_DOWN || key == KEY_LEFT || key == KEY_RIGHT);
}

s32 ai_input_on_key_up(s32 key) {
    if (key >= 0 && key < (s32) (sizeof(sKeys) / sizeof(sKeys[0]))) sKeys[key] = FALSE;
    if (key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT) {
        sShiftDown = sKeys[KEY_LEFTSHIFT] || sKeys[KEY_RIGHTSHIFT];
    }
    return sTyping || key == KEY_TAB || key == KEY_F1
        || (ai_freecam_is_enabled()
            && (key == 0x11 || key == 0x1E || key == 0x1F || key == 0x20
                || key == KEY_UP || key == KEY_DOWN || key == KEY_LEFT || key == KEY_RIGHT));
}

void ai_input_on_all_keys_up(void) {
    memset(sKeys, 0, sizeof(sKeys));
    sShiftDown = FALSE;
    sMouseLook = FALSE;
    sMouseX = 0;
    sMouseY = 0;
}

void ai_input_capture_physical_pad(OSContPad *pad) {
    if (pad == NULL) return;
    sPhysicalStickX = pad->stick_x;
    sPhysicalStickY = pad->stick_y;
    sPhysicalButtons = pad->button;
    if (sTyping) {
        pad->stick_x = 0;
        pad->stick_y = 0;
        pad->button &= START_BUTTON;
    } else if (ai_freecam_is_enabled()) {
        pad->stick_x = 0;
        pad->stick_y = 0;
        pad->button &= ~(U_CBUTTONS | D_CBUTTONS | L_CBUTTONS | R_CBUTTONS);
    }
}

s32 ai_input_move_forward(void) {
    if (sTyping) return 0;
    return sKeys[0x11] - sKeys[0x1F] + (sPhysicalStickY > 30) - (sPhysicalStickY < -30);
}

s32 ai_input_move_right(void) {
    if (sTyping) return 0;
    return sKeys[0x20] - sKeys[0x1E] + (sPhysicalStickX > 30) - (sPhysicalStickX < -30);
}

s32 ai_input_look_yaw(void) {
    if (sTyping) return 0;
    return sKeys[KEY_RIGHT] - sKeys[KEY_LEFT]
        + !!(sPhysicalButtons & R_CBUTTONS) - !!(sPhysicalButtons & L_CBUTTONS);
}

s32 ai_input_look_pitch(void) {
    if (sTyping) return 0;
    return sKeys[KEY_UP] - sKeys[KEY_DOWN]
        + !!(sPhysicalButtons & U_CBUTTONS) - !!(sPhysicalButtons & D_CBUTTONS);
}
s32 ai_input_is_typing(void) { return sTyping; }

s32 ai_input_take_freecam_toggle(void) {
    s32 toggle = sFreecamToggle;
    sFreecamToggle = FALSE;
    return toggle;
}

void ai_input_add_mouse_delta(s32 x, s32 y) {
    if (!sTyping && sMouseLook) {
        sMouseX += x;
        sMouseY += y;
    }
}

void ai_input_set_mouse_look(s32 active) {
    sMouseLook = active && !sTyping;
    if (!sMouseLook) {
        sMouseX = 0;
        sMouseY = 0;
    }
}

s32 ai_input_take_mouse_x(void) {
    s32 value = sMouseX;
    sMouseX = 0;
    return value;
}

s32 ai_input_take_mouse_y(void) {
    s32 value = sMouseY;
    sMouseY = 0;
    return value;
}

void ai_input_render(void) {
    if (sSentTimer > 0) sSentTimer--;
    if ((!sTyping && sSentTimer <= 0) || get_dialog_id() != DIALOG_NONE) return;
    ai_input_build_display();
    create_dl_ortho_matrix();
    create_dl_translation_matrix(MENU_MTX_PUSH, 12.0f, 48.0f, 0.0f);
    create_dl_scale_matrix(MENU_MTX_NOPUSH, 2.45f, 0.6f, 1.0f);
    gDPSetEnvColor(gDisplayListHead++, 0, 0, 0, 190);
    gSPDisplayList(gDisplayListHead++, dl_draw_text_bg_box);
    gSPPopMatrix(gDisplayListHead++, G_MTX_MODELVIEW);
    gSPDisplayList(gDisplayListHead++, dl_ia_text_begin);
    gDPSetEnvColor(gDisplayListHead++, 255, 255, 255, 255);
    print_generic_string(20, 36, sDisplayText);
    gSPDisplayList(gDisplayListHead++, dl_ia_text_end);
}

#else

s32 ai_input_on_key_down(UNUSED s32 scancode) { return FALSE; }
s32 ai_input_on_key_up(UNUSED s32 scancode) { return FALSE; }
void ai_input_on_all_keys_up(void) {}
void ai_input_capture_physical_pad(UNUSED OSContPad *pad) {}
s32 ai_input_move_forward(void) { return 0; }
s32 ai_input_move_right(void) { return 0; }
s32 ai_input_look_yaw(void) { return 0; }
s32 ai_input_look_pitch(void) { return 0; }
s32 ai_input_is_typing(void) { return FALSE; }
s32 ai_input_take_freecam_toggle(void) { return FALSE; }
void ai_input_add_mouse_delta(UNUSED s32 x, UNUSED s32 y) {}
void ai_input_set_mouse_look(UNUSED s32 active) {}
s32 ai_input_take_mouse_x(void) { return 0; }
s32 ai_input_take_mouse_y(void) { return 0; }
void ai_input_render(void) {}

#endif
