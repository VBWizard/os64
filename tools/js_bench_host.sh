#!/usr/bin/env bash
# js_bench_host.sh — the JS benchmark host twin: /bin/js as os64 builds it,
# run on the host, so libjs's engine can be timed against stock QuickJS
# (~/src/quickjs, the same release) on one machine.
#
#   tools/js_bench_host.sh OUT [--glibc-malloc] [--glibc-string] [--core OBJ]
#       builds OUT, a host program taking /bin/js's own arguments
#
# What is os64's and what is the host's: the engine core
# (userland/obj/js/core.o), libjs's runtime and Intl, libmath and libos64's
# allocator, string, formatting, date and argument code are the very objects
# the cross compiler built for os64, with os64's flags; the runner (js.c),
# compiled here as the program's body, and tools/js_bench_host_shim.c, which
# answers os64's system calls through Linux, are the host's. So a timing
# difference against stock qjs is libjs's port, not a recompile of it.
#
# The switches are the experiments, each replacing one thing so a timing
# against the plain twin says what that thing costs:
#   --glibc-malloc   glibc's allocator in place of libos64's heap
#   --glibc-string   glibc's memcpy/memcmp/strlen family in place of libos64's
#   --core OBJ       another build of the engine core (an `ld -r` of it, as
#                    core.mk links core.o), e.g. one compiled with other flags
# (tools/js_bench_host_glibc.c holds the glibc stand-ins.)
#
# The objects must exist: build userland/bin/libjs.so (and with it libos64
# and libmath) in this tree first.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
out=${1:?usage: tools/js_bench_host.sh OUT [--glibc-malloc] [--glibc-string] [--core OBJ]}
shift
obj=userland/obj
core="$obj/js/core.o"
glibc_malloc=0
glibc_string=0
while [ $# -gt 0 ]; do
    case "$1" in
        --glibc-malloc) glibc_malloc=1 ;;
        --glibc-string) glibc_string=1 ;;
        --core) core=${2:?--core needs an object}; shift ;;
        *) echo "js_bench_host: unknown switch $1" >&2; exit 2 ;;
    esac
    shift
done
for need in "$core" "$obj/js/runtime/runtime.o" "$obj/js/intl/intl.o" "$obj/pic/libos64/heap.c.o"; do
    [ -f "$need" ] || { echo "js_bench_host: $need is not built (make userland/bin/libjs.so first)" >&2; exit 2; }
done
work=$(mktemp -d -t os64-js-bench.XXXXXX)
trap 'rm -rf "$work"' EXIT
mapfile -t maths < <(make -s -C userland --no-print-directory math-host-flags 2>/dev/null | python3 -I -c '
import shlex, sys
for line in sys.stdin:
    if line.startswith("MATH_OBJS="):
        print("\n".join(shlex.split(line.split("=", 1)[1])[0].split()))
')
[ "${#maths[@]}" -gt 0 ] || { echo "js_bench_host: libmath's objects are not listed" >&2; exit 2; }
flags=(-O2 -std=gnu11 -fno-builtin -ffreestanding -DOS64_JS_TARGET
       -I userland/libjs/include -I userland/libos64/include -I abi/include)
cc "${flags[@]}" -Dmain=js_main -c userland/apps/js/js.c -o "$work/js.o"
printf 'int js_main(int, char **);\nint main(int argc, char **argv) { return js_main(argc, argv); }\n' > "$work/main.c"
cc -O2 -c "$work/main.c" -o "$work/main.o"
cc -O2 -std=gnu11 -I userland/libos64/include -I abi/include -c tools/js_bench_host_shim.c -o "$work/shim.o"
# The core carries its own memcpy family (libjs/port/memory.c, each a call
# to libos64's os64_ routine, as in libjs.so); libos64's copies of those four
# names are made local so the two do not collide. With --glibc-string the
# os64_ routines themselves are weakened, so glibc's stand-ins are linked.
string_syms=(os64_memcpy os64_memmove os64_memset os64_memcmp os64_memchr os64_strlen os64_strcmp os64_strchr os64_strrchr)
weaken=()
[ "$glibc_string" = 1 ] && for s in "${string_syms[@]}"; do weaken+=(--weaken-symbol="$s"); done
objcopy --localize-symbol=memcpy --localize-symbol=memmove --localize-symbol=memset \
    --localize-symbol=memcmp "${weaken[@]}" "$obj/pic/libos64/str.c.o" "$work/str.o"
# date.c holds the calendar (pure) and the two clock calls, which are os64
# system calls; those two are weakened so the shim's answer is the one
# linked.
objcopy --weaken-symbol=os64_time --weaken-symbol=os64_set_time "$obj/pic/libos64/date.c.o" "$work/date.o"
# With --glibc-string the core's memcpy family is made local as well: the
# stand-ins call memcpy and its kin, which must reach glibc's, not the
# core's wrappers, which would call the stand-ins back.
if [ "$glibc_string" = 1 ]; then
    objcopy --localize-symbol=memcpy --localize-symbol=memmove --localize-symbol=memset \
        --localize-symbol=memcmp "$core" "$work/core.o"
    core="$work/core.o"
fi
os64_objs=("$core" "$obj"/js/runtime/*.o "$obj/js/intl/intl.o" "${maths[@]}"
           "$work/str.o" "$work/date.o" "$obj"/pic/libos64/{fmt,args}.c.o)
variant=()
[ "$glibc_malloc" = 1 ] && variant+=(-DJSB_GLIBC_MALLOC) || os64_objs+=("$obj/pic/libos64/heap.c.o")
[ "$glibc_string" = 1 ] && variant+=(-DJSB_GLIBC_STRING)
cc -O2 -std=gnu11 "${variant[@]}" -c tools/js_bench_host_glibc.c -o "$work/glibc.o"
# An os64 system call run on Linux is a different call by the same number,
# so a linked function that makes one, other than those the shim replaces,
# stops the build by name.
for o in "${os64_objs[@]}"; do
    objdump -d --no-show-raw-insn "$o" | awk -v o="$o" '
        /^[0-9a-f]+ <.*>:$/ { f = $2 }
        /\tsyscall/ && f != "<os64_time>:" && f != "<os64_set_time>:" { print "js_bench_host: " o " " f " makes an os64 system call"; bad = 1 }
        END { exit bad }'
done
cc -Wl,-z,noexecstack ${JSB_EXTRA_LINK:-} -o "$out" "$work/main.o" "$work/js.o" "$work/shim.o" "$work/glibc.o" "${os64_objs[@]}"
echo "js_bench_host: built $out"
