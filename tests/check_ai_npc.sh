#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
flags=(-std=gnu99 -D_LANGUAGE_C -DVERSION_US -DNON_MATCHING -DAVOID_UB -DF3DEX_GBI_2 -DNO_SEGMENTED_MEMORY -DUSE_SYSTEM_MALLOC -Iinclude -Isrc -I.)
sources=(src/game/ai_npc.c src/game/mario_misc.c src/game/behavior_actions.c src/game/game_init.c src/game/area.c src/game/object_list_processor.c src/pc/controller/controller_keyboard.c)
cc -fsyntax-only -Werror=implicit-function-declaration "${flags[@]}" "${sources[@]}"
cc -fsyntax-only -fno-builtin -Werror=implicit-function-declaration "${flags[@]}" -DTARGET_N64 src/game/ai_npc.c
binary=$(mktemp /tmp/sm64-npc-tests.XXXXXX)
trap 'rm -f "$binary"' EXIT
cc "${flags[@]}" -ffunction-sections -fdata-sections tests/test_ai_npc.c -Wl,--gc-sections -lm -o "$binary"
"$binary"
python -m unittest discover -s npc_bridge -p 'test_*.py' -v
