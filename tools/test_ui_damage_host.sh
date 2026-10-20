#!/usr/bin/env bash
# libui's dirty list (tools/test_ui_damage_host.c) under ASan and UBSan: the
# real ui.c and panel on a canvas in memory, with the fonts' fake engine.
set -eu
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d -t os64-ui-damage-host.XXXXXX)
trap 'case "$work" in "${TMPDIR:-/tmp}"/os64-ui-damage-host.*) rm -rf "$work";; esac' EXIT
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I userland/libos64/include -I userland/libos64 -I abi/include -I tools/fonts \
    -o "$work/ui_damage" tools/test_ui_damage_host.c tools/fonts/fake_backend.c shared/damage.c \
    userland/libos64/{ui,ui_font,ui_controls,ui_list,ui_text,font_provider,font_adopt,text,text_cache,text_decode,text_bitmap,text_draw,draw,str}.c
"$work/ui_damage"
