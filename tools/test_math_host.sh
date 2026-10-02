#!/bin/bash
# libmath on the host: the port's numbers, judged two ways before a guest
# ever runs them (MATH.md § Validation).
#
#   1. The pinned sources are built TWICE — by the host gcc, from the flags
#      shared.mk hands over, and by the cross compiler, as the very objects
#      libmath.so is linked from — and each set is linked into the harness
#      with no host libm. Every maths symbol must be defined in the binary
#      itself, or a call could have landed in glibc and proved nothing.
#   2. Both binaries run the floating-point state checks (state.h).
#   3. Both compute the whole corpus (corpus.h); the two outputs must be
#      bit-identical — the host reference agrees with the guest's objects.
#   4. MPFR judges every vector (tools/test_math_oracle.c) against the
#      bounds userland/libmath/UPSTREAM_REVIEW.md § Accuracy sources.
#   5. The block digests are regenerated and must match the checked-in
#      userland/tests/mathtest/expected.h, which is what the guest fixture
#      compares libmath.so against. `--regen` rewrites it instead, and only
#      once every check above has passed.
#
# `tools/test_math_host.sh dump <fn> <block>` builds the host harness and
# prints one block's vectors, to set beside the dump the guest prints when a
# digest disagrees.
#
# Needs the cross toolchain (it builds libmath.so's objects) and MPFR's
# development files.
set -eu
cd "$(git rev-parse --show-toplevel)"

regen=0
dump=""
case "${1:-}" in
    --regen) regen=1 ;;
    dump) dump="$2 $3" ;;
esac

work=$(mktemp -d)
cleanup() {
    status=$?
    if [ "$status" -eq 0 ]; then rm -rf "$work"; else echo "libmath test artifacts: $work" >&2; fi
}
trap cleanup EXIT

make -s -C userland --no-print-directory "$PWD/userland/bin/libmath.so" >/dev/null
eval "$(make -s -C userland --no-print-directory math-host-flags)"

# The host build, source by source, with each file's own flags.
mkdir -p "$work/host"
for src in $MATH_UPSTREAM_SRCS $MATH_PORT_SRCS; do
    extra=""
    case "$src" in
        */upstream/*) extra="$MATH_UPSTREAM_FLAGS" ;;
    esac
    case "$src" in
        */__rem_pio2_large.c) extra="$extra $MATH_REM_PIO2_LARGE_FLAGS" ;;
        libmath/port/sqrt.c)  extra="$extra $MATH_SQRT_FLAGS" ;;
    esac
    # shellcheck disable=SC2086 # the flag strings are word lists
    (cd userland && gcc $MATH_FLAGS $extra -c "$src" -o "$work/host/$(basename "$src").o")
done

# The cross build is the objects libmath.so was just linked from.
cross_objs=""
for o in $MATH_OBJS; do cross_objs="$cross_objs $o"; done

harness() {
    # -fno-builtin: every call in the harness is a real call to the port.
    # shellcheck disable=SC2086
    gcc -std=c11 -g -O0 -Wall -Wextra -Werror -fno-builtin -masm=intel \
        -I userland/libmath/include -I userland/tests/mathtest \
        -Wl,-z,noexecstack tools/test_math_host.c $1 -o "$2"
}
# shellcheck disable=SC2086
harness "$(echo "$work"/host/*.o)" "$work/math_host"
if [ -n "$dump" ]; then
    # shellcheck disable=SC2086
    "$work/math_host" dump $dump
    exit
fi
harness "$cross_objs" "$work/math_cross"

for bin in math_host math_cross; do
    if readelf -dW "$work/$bin" | grep -q 'NEEDED.*libm\.so'; then
        echo "FAIL $bin: linked against host libm" >&2
        exit 1
    fi
    for name in $MATH_PUBLIC_NAMES; do
        if ! nm "$work/$bin" | grep -q " T $name\$"; then
            echo "FAIL $bin: $name is not defined in the harness itself" >&2
            exit 1
        fi
    done
done
echo "PASS binding: both harnesses define all $(echo $MATH_PUBLIC_NAMES | wc -w) functions, no host libm"

# The public header against C's own prototypes: a declaration that drifts
# from the standard is a conflicting redeclaration, and the build stops.
{
    echo '#include <math.h>'
    for name in $MATH_PUBLIC_NAMES; do
        case "$name" in
            lrint) echo 'long lrint(double);' ;;
            atan2|fmax|fmin|fmod|hypot|pow) echo "double $name(double, double);" ;;
            *) echo "double $name(double);" ;;
        esac
    done
} > "$work/prototypes.c"
x86_64-elf-gcc -std=c11 -ffreestanding -nostdinc -Wall -Wextra -Werror -fsyntax-only \
    -I userland/libmath/include "$work/prototypes.c"
echo "PASS header: <math.h> declares every export with C's prototype"

"$work/math_host" state
"$work/math_cross" state

"$work/math_host" vectors > "$work/host.vectors"
"$work/math_cross" vectors > "$work/cross.vectors"
if ! cmp -s "$work/host.vectors" "$work/cross.vectors"; then
    echo "FAIL corpus: host and cross builds disagree; first differences:" >&2
    diff "$work/host.vectors" "$work/cross.vectors" | head -20 >&2
    exit 1
fi
echo "PASS corpus: $(wc -l < "$work/host.vectors") vectors bit-identical between the host and cross builds"

gcc -std=c11 -O1 -Wall -Wextra -Werror tools/test_math_oracle.c -lmpfr -lgmp -o "$work/oracle"
"$work/oracle" < "$work/host.vectors"

"$work/math_host" expected > "$work/expected.h"
expected=userland/tests/mathtest/expected.h
if [ "$regen" -eq 1 ]; then
    cp "$work/expected.h" "$expected"
    echo "WROTE $expected"
elif ! cmp -s "$work/expected.h" "$expected"; then
    echo "FAIL $expected is stale: run tools/test_math_host.sh --regen" >&2
    exit 1
else
    echo "PASS $expected matches the host build"
fi
