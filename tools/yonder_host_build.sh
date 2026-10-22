# yonder_host_build.sh — sourced by the suites that run yonder's own window
# headless, never run: it builds a binary from a test unit that includes
# yonder.c (the headless binding, tools/test_yonder_scripts_host.c, which
# fakes the OS around it) and every library the window links, under ASan
# and UBSan.
#
#   . tools/yonder_host_build.sh        (after cd to the top of the tree)
#   yonder_host_build "$work" <unit.c> <binary> [flags for the unit alone]
#
# YONDER_HOST_UNITS names more sources of the suite's own, built beside the
# unit as translation units of their own (the replay runner's file door).
#
# A failed build says so on stderr and answers 2, not only in a log nobody
# reads: a mutant harness counts any failure as its catch.

. tools/host_build.sh

yonder_host_build() {
    local work=$1 unit=$2 binary=$3
    shift 3
    if ! make -C userland -j"${HOST_BUILD_JOBS:-4}" js-runtime-test > "$work/build.log" 2>&1; then
        tail -n 20 "$work/build.log" >&2
        echo "yonder host: the JS runtime did not build (whole log: $work/build.log)" >&2
        return 2
    fi
    host_flags=(-O1 -g -std=gnu11 -Wall -Wextra -Werror -ffreestanding -fno-builtin
        -ffunction-sections -fdata-sections -fno-tree-loop-distribute-patterns
        -fno-stack-protector -DOS64_JS_TARGET
        -Iuserland/libmath/include -Iuserland/libjs/port -Iuserland/libjs/include
        -Iuserland/libdom/include -Iuserland/libhtml/include -Iuserland/libpage/include
        -Iuserland/libflow/include -Iuserland/libgarb/include -Iuserland/libway/include
        -Iuserland/libfetch/include -Iuserland/libimage/include -Iuserland/libwebp/include -Iuserland/libgzip/include
        -Iuserland/libtls/include -Iuserland/libflow -Iuserland/apps/yonder
        -Iuserland/libpage/upstream/ryu -Iuserland/libos64/include -Iuserland/libos64
        -Iabi/include -Itools -Itools/fonts
        -isystem userland/obj/js/upstream -fsanitize=address,undefined -fno-sanitize-recover=all)
    local objects=() source object private
    for source in "$unit" ${YONDER_HOST_UNITS:-} tools/test_js_port_calendar.c tools/test_libflow_fonts.c \
        userland/libjs/runtime/runtime.c userland/libdom/{core,node,collection,content,event,timer,window,geometry,classic,style,mixin,modern}.c \
        userland/apps/yonder/{scripts,geometry,paint,paint_cache,scratch,scale,warp,agent,bar,diag}.c \
        userland/libhtml/{core,encoding,tokenizer,tree,dom,fragment,serialize}.c \
        userland/libpage/{core,state,resolve,value,submit,encode,refresh,activate,number,range}.c \
        userland/libpage/upstream/ryu/ryu/d2s.c \
        userland/libflow/{store,attrs,style,motion,dump,boxes,layout,flow}.c \
        userland/libgarb/{tokenize,parse,decode,dump,select,values,props,media,cascade}.c \
        userland/libway/session.c \
        userland/libos64/{str,fmt,bidi,url,arena,ui,ui_controls,ui_list,ui_text,ui_font,ui_theme,font_config,font_family,font_provider,font_adopt,text,text_cache,text_decode,text_bitmap,text_draw,draw}.c \
        shared/damage.c; do
        object="$work/${source//\//_}.o"
        private=()
        if [[ "$source" == userland/libdom/* ]]; then private=(-Dd_alloc=dom_fixture_private_alloc); fi
        if [ "$source" = "$unit" ]; then private=("$@"); fi
        host_compile "$source" "$object" "${private[@]}"
        objects+=("$object")
    done
    host_wait || return 2
    local i
    for i in "${!objects[@]}"; do
        if [ "${objects[$i]}" = "$work/userland_libos64_str.c.o" ]; then
            objcopy --localize-symbol=memcpy --localize-symbol=memmove --localize-symbol=memset \
                --localize-symbol=memcmp "${objects[$i]}" "${objects[$i]}-local" || return 2
            objects[$i]="${objects[$i]}-local"
        fi
    done
    local maths
    mapfile -t maths < <(make -s -C userland math-host-flags | python3 -c '
import shlex,sys
for line in sys.stdin:
    if line.startswith("MATH_OBJS="): print("\n".join(shlex.split(line.split("=",1)[1])[0].split()))
')
    cc -fsanitize=address,undefined -pthread -Wl,-z,noexecstack,--gc-sections,--wrap=flow_layout,--wrap=flow_motion_tick,--wrap=os64_js_create_with_teardown,--wrap=os64_text_font_open,--wrap=os64_text_font_open_like \
        userland/obj/js/core.o "${maths[@]}" "${objects[@]}" -o "$binary" || return 2
}
