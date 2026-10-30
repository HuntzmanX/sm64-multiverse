#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
# Keep headless tests away from a configured live game directory.
unset SM64_NPC_RUNTIME_DIR
temporary=$(mktemp -d /tmp/sm64-npc-checks.XXXXXX)
trap 'rm -rf -- "$temporary"' EXIT
cc tools/textconv.c tools/utf8.c tools/hashtable.c -Itools -o "$temporary/textconv"
"$temporary/textconv" charmap_menu.txt include/text_menu_strings.h.in "$temporary/text_menu_strings.h"
"$temporary/textconv" charmap.txt include/text_strings.h.in "$temporary/text_strings.h"
flags=(-I"$temporary" -std=gnu99 -D_LANGUAGE_C -DVERSION_US -DNON_MATCHING -DAVOID_UB -DF3DEX_GBI_2 -DNO_SEGMENTED_MEMORY -DUSE_SYSTEM_MALLOC -Iinclude -Isrc -I.)
sources=(src/game/ai_npc.c src/game/ingame_menu.c src/game/camera.c src/game/mario_misc.c src/game/obj_behaviors.c src/game/behavior_actions.c src/game/game_init.c src/game/area.c src/game/object_list_processor.c src/pc/controller/controller_keyboard.c)
cc -fsyntax-only -Werror=implicit-function-declaration "${flags[@]}" "${sources[@]}"
cc -fsyntax-only -fno-builtin -Werror=implicit-function-declaration "${flags[@]}" -DTARGET_N64 src/game/ai_npc.c
binary="$temporary/state-tests"
# Link actual engine physics; rename the coin helper so the reward spy can coexist.
cc "${flags[@]}" -ffunction-sections -fdata-sections -Dobj_spawn_loot_yellow_coins=engine_spawn_loot_yellow_coins -c src/game/object_helpers.c -o "$temporary/object_helpers.o"
cc "${flags[@]}" -ffunction-sections -fdata-sections tests/test_ai_npc.c "$temporary/object_helpers.o" -Wl,--gc-sections -lm -o "$binary"
"$binary"
cc "${flags[@]}" -DWIDESCREEN -ffunction-sections -fdata-sections tests/test_ai_npc.c "$temporary/object_helpers.o" -Wl,--gc-sections -lm -o "$binary"
"$binary"
printf '#include "game/ai_npc.h"\n' | c++ -fsyntax-only -x c++ -D_LANGUAGE_C -Iinclude -Isrc -
python -m unittest discover -s npc_bridge -p 'test_*.py' -v
