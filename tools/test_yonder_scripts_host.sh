#!/usr/bin/env bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d /tmp/os64-yonder-scripts.XXXXXX)
cleanup() {
    result=$?
    if [ "$result" -eq 0 ] && [ "${YONDER_SCRIPTS_KEEP:-0}" = 0 ]; then
        rm -rf "$work"
    else
        echo "Yonder script artifacts: $work" >&2
    fi
}
trap cleanup EXIT
make -C userland -j4 js-runtime-test > "$work/build.log" 2>&1
flags=(-O1 -g -std=gnu11 -Wall -Wextra -Werror -ffreestanding -fno-builtin
    -ffunction-sections -fdata-sections -fno-tree-loop-distribute-patterns
    -fno-stack-protector -DOS64_JS_TARGET
    -Iuserland/libmath/include -Iuserland/libjs/port -Iuserland/libjs/include
    -Iuserland/libdom/include -Iuserland/libhtml/include -Iuserland/libpage/include
    -Iuserland/libflow/include -Iuserland/libgarb/include -Iuserland/libway/include
    -Iuserland/libfetch/include -Iuserland/libimage/include -Iuserland/libgzip/include
    -Iuserland/libtls/include -Iuserland/libflow -Iuserland/apps/yonder
    -Iuserland/libpage/upstream/ryu -Iuserland/libos64/include -Iuserland/libos64
    -Iabi/include -Itools -Itools/fonts
    -isystem userland/obj/js/upstream -fsanitize=address,undefined -fno-sanitize-recover=all)
objects=()
for source in tools/test_yonder_scripts_host.c tools/test_js_port_calendar.c tools/test_libflow_fonts.c \
    userland/libjs/runtime/runtime.c userland/libdom/{core,node,collection,content,event,timer,window,geometry}.c \
    userland/apps/yonder/{scripts,geometry,paint,scale,agent,bar}.c \
    userland/libhtml/{core,encoding,tokenizer,tree,dom,fragment,serialize}.c \
    userland/libpage/{core,state,resolve,value,submit,encode,refresh,activate,number,range}.c \
    userland/libpage/upstream/ryu/ryu/d2s.c \
    userland/libflow/{store,attrs,style,dump,boxes,layout,flow}.c \
    userland/libgarb/{tokenize,parse,decode,dump,select,values,props,media,cascade}.c \
    userland/libway/session.c \
    userland/libos64/{str,fmt,bidi,url,arena,ui,ui_controls,ui_list,ui_text,ui_font,ui_theme,font_config,font_family,font_provider,font_adopt,text,text_cache,text_decode,text_bitmap,text_draw,draw}.c; do
    object="$work/${source//\//_}.o"
    private=()
    if [[ "$source" == userland/libdom/* ]]; then private=(-Dd_alloc=dom_fixture_private_alloc); fi
    cc "${flags[@]}" "${private[@]}" -c "$source" -o "$object"
    if [ "$source" = userland/libos64/str.c ]; then
        objcopy --localize-symbol=memcpy --localize-symbol=memmove --localize-symbol=memset \
            --localize-symbol=memcmp "$object" "$object-local"
        object="$object-local"
    fi
    objects+=("$object")
done
mapfile -t maths < <(make -s -C userland math-host-flags | python3 -c '
import shlex,sys
for line in sys.stdin:
    if line.startswith("MATH_OBJS="): print("\n".join(shlex.split(line.split("=",1)[1])[0].split()))
')
cc -fsanitize=address,undefined -pthread -Wl,-z,noexecstack,--gc-sections,--wrap=flow_layout,--wrap=os64_js_create_with_teardown \
    userland/obj/js/core.o "${maths[@]}" "${objects[@]}" -o "$work/yonder-scripts"
"$work/yonder-scripts" "$@"
