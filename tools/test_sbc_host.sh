#!/bin/bash
# Prove the kernel's SBC encoder on the host (docs/design/pending/AUDIO_A2DP.md
# § Part E § Proof): kernel/ports/sbc built with the port's own switches, under
# ASan and UBSan, beside kernel/src/audio/sbc_frame.c; then every stream
# decoded by FFmpeg and held against FFmpeg's own encoder at the same
# settings. tools/test_sbc_host.c says what each step asserts.
#
# The full run FAILS when ffmpeg is missing; it does not skip. It is the only
# proof the first third-party code in ring 0 has, so it may not pass by not
# running. --selftest (below) needs no ffmpeg.
#
# Its builds and files go in kernel/obj/host-sbc/, overwritten each run and
# removed by `make -C kernel clean`.
#
#   tools/test_sbc_host.sh              everything
#   tools/test_sbc_host.sh --selftest   the builds and the encoder's own
#                                       checks, without FFmpeg's half (what
#                                       tools/test_sbc_frame_mutants.py runs)

set -eu
cd "$(git rev-parse --show-toplevel)"

fail() { echo "FAIL test_sbc_host: $*" >&2; exit 1; }
mode=${1:-}
case $mode in "" | --selftest) ;; *) fail "unknown argument $mode" ;; esac

# The full run needs FFmpeg and says so before building anything; --selftest
# does not touch it.
if [ "$mode" != --selftest ]; then
	command -v ffmpeg >/dev/null 2>&1 ||
		fail "ffmpeg is not installed. This suite is the SBC encoder's proof and does not skip: install ffmpeg (6.1, with the sbc encoder, the sbc decoder and the raw sbc format) and run it again."
	ffmpeg -hide_banner -encoders 2>/dev/null | grep -q ' sbc ' || fail "this ffmpeg has no sbc encoder"
	ffmpeg -hide_banner -decoders 2>/dev/null | grep -q ' sbc ' || fail "this ffmpeg has no sbc decoder"
	ffmpeg -hide_banner -formats 2>/dev/null | grep -q ' DE sbc ' || fail "this ffmpeg cannot read and write raw sbc"
	echo "     $(ffmpeg -hide_banner -version | head -n 1)"
fi

out=kernel/obj/host-sbc
mkdir -p "$out"
P=kernel/ports/sbc
upstream=$(grep -o 'ports/sbc/upstream/[^ ]*\.c' $P/sources.mk | sed 's|^|kernel/|')
[ -n "$upstream" ] || fail "no upstream sources named in $P/sources.mk"

# Upstream as the kernel builds it (sbc.mk): its switches, -O2,
# -fno-strict-aliasing, warnings shown and not fatal. UBSan's shift-base
# check is off for these files alone: the fixed-point DCT doubles negative
# values with a left shift (README.md § UBSan says where, and why it is
# benign under GCC). Every other check stands.
UP_FLAGS="-std=gnu11 -g -O2 -fno-strict-aliasing -Wall -Wextra -include $P/port/sbc_switches.h -I $P/port/compat -I $P/upstream/include"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
UP_SAN="$SAN -fno-sanitize=shift-base"
# Ours as the kernel builds its own sources: -O0, every warning fatal. The
# kernel's headers are reached for quoted includes only, so that its math.h
# does not stand in for the host's.
INC="-iquote kernel/include -iquote $P/port -iquote $P/upstream/include"
OUR_FLAGS="-std=gnu11 -g -O0 -Wall -Wextra -Werror $INC"
# The kernel's code-generation flags, for the timing build only.
MACHINE="-mno-red-zone -mno-mmx -mno-sse -mno-sse2"

up_san_objs=""
up_fast_objs=""
for src in $upstream; do
	base=$(basename "$src" .c)
	cc $UP_FLAGS $UP_SAN -c "$src" -o "$out/$base.san.o"
	cc $UP_FLAGS $MACHINE -c "$src" -o "$out/$base.fast.o"
	up_san_objs="$up_san_objs $out/$base.san.o"
	up_fast_objs="$up_fast_objs $out/$base.fast.o"
done

# The proof: the boundary with its test hook.
cc $OUR_FLAGS $SAN -DSBC_PORT_TEST_HOOKS -c $P/port/sbc_port.c -o "$out/sbc_port.hooks.o"
cc $OUR_FLAGS $SAN -c kernel/src/audio/sbc_frame.c -o "$out/sbc_frame.san.o"
cc $OUR_FLAGS $SAN -DSBC_PORT_TEST_HOOKS -O1 -c tools/test_sbc_host.c -o "$out/test.hooks.o"
cc $SAN "$out/test.hooks.o" "$out/sbc_port.hooks.o" "$out/sbc_frame.san.o" $up_san_objs -lm -o "$out/test_sbc"

# The hand-made failure: a frame room smaller than a 115-byte frame.
cc $OUR_FLAGS $SAN -DSBC_PORT_FRAME_ROOM=100 -c $P/port/sbc_port.c -o "$out/sbc_port.guard.o"
cc $OUR_FLAGS $SAN -O1 -c tools/test_sbc_host.c -o "$out/test.plain.o"
cc $SAN "$out/test.plain.o" "$out/sbc_port.guard.o" "$out/sbc_frame.san.o" $up_san_objs -lm -o "$out/test_sbc_guard"

# The bit-rate search capped below what bitpool 51 needs.
cc $OUR_FLAGS $SAN -DSBC_PORT_RATE_CEILING_KBPS=200u -c $P/port/sbc_port.c -o "$out/sbc_port.ceiling.o"
cc $SAN "$out/test.plain.o" "$out/sbc_port.ceiling.o" "$out/sbc_frame.san.o" $up_san_objs -lm -o "$out/test_sbc_ceiling"

# The timing: no sanitizers, the port at the kernel's flags (upstream -O2,
# ours -O0, both without SSE).
cc $OUR_FLAGS $MACHINE -c $P/port/sbc_port.c -o "$out/sbc_port.fast.o"
cc $OUR_FLAGS $MACHINE -c kernel/src/audio/sbc_frame.c -o "$out/sbc_frame.fast.o"
cc -std=gnu11 -O2 -Wall -Wextra -Werror $INC -c tools/test_sbc_host.c -o "$out/test.fast.o"
cc "$out/test.fast.o" "$out/sbc_port.fast.o" "$out/sbc_frame.fast.o" $up_fast_objs -lm -o "$out/test_sbc_speed"

t="$out/test_sbc"
"$t" selftest
"$out/test_sbc_guard" guard
"$out/test_sbc_ceiling" ceiling
"$out/test_sbc_speed" speed
if [ "$mode" = --selftest ]; then
	echo "PASS test_sbc_host --selftest (FFmpeg's half not run)"
	exit 0
fi

# FFmpeg's encoder chooses joint stereo below 180 kbit/s or above 420, plain
# stereo between, and 8 subbands at or below 420 with its default delay
# (13 ms, which also gives 16 blocks); -q:a then sets the bitpool. So -b:a
# picks the mode and -q:a the bitpool, and `headers` checks that its stream
# says exactly the settings under test. It has no SNR allocation: an SNR
# setting is held against FFmpeg's loudness stream at everything else equal.
#
# FFmpeg 6.1's -xerror ends a decode at the first frame it cannot decode but
# still exits 0, and without it a bad frame is dropped in silence; either
# way the PCM comes out short. So a decode fails here on a nonzero exit, on
# anything FFmpeg prints at -loglevel error, and, in `compare`, on a frame
# count out that is not the count in.
ffm() {
	local log
	log=$(ffmpeg -hide_banner -nostdin -loglevel error -xerror "$@" 2>&1) || { echo "$log" >&2; return 1; }
	[ -z "$log" ] || { echo "$log" >&2; return 1; }
}
decode() {   # decode IN.sbc OUT.pcm FRAMES: false unless every frame decoded
	ffm -f sbc -i "$1" -f s16le -c:a pcm_s16le -y "$2" || return 1
	[ "$(stat -c %s "$2")" = $(($3 * 128 * 4)) ]
}

# The decode check must be able to fail: one flipped scale-factor bit (the
# frame's CRC no longer holds) in an otherwise good stream.
frames=$("$t" signal music 48000 "$out/in.pcm")
"$t" encode 48000 joint loudness 51 "$out/in.pcm" "$out/ours.sbc" > /dev/null
decode "$out/ours.sbc" "$out/ours.pcm" "$frames" || fail "a good stream did not decode"
"$t" flip "$out/ours.sbc" "$out/bad.sbc" $((115 * 10 + 6))
if decode "$out/bad.sbc" "$out/bad.pcm" "$frames" 2> /dev/null; then
	fail "a stream with a bad frame decoded as if whole"
fi
echo "PASS decode check: a frame whose CRC fails is caught"

settings="
44100 joint loudness 8
44100 joint loudness 19
44100 joint loudness 35
44100 joint loudness 53
44100 joint loudness 128
44100 joint loudness 250
44100 stereo loudness 8
44100 stereo loudness 35
44100 stereo loudness 53
44100 stereo loudness 250
48000 joint loudness 8
48000 joint loudness 19
48000 joint loudness 33
48000 joint loudness 51
48000 joint loudness 53
48000 joint loudness 128
48000 joint loudness 250
48000 stereo loudness 8
48000 stereo loudness 33
48000 stereo loudness 51
48000 stereo loudness 250
48000 joint snr 51
44100 stereo snr 35
48000 stereo snr 250
"
echo "$settings" | while read -r rate mode alloc bitpool; do
	[ -n "$rate" ] || continue
	case $mode in joint) bit_rate=100000 ;; stereo) bit_rate=300000 ;; esac
	echo "     $rate Hz, $mode, $alloc, bitpool $bitpool"
	for signal in sweep noise square music silence; do
		frames=$("$t" signal "$signal" "$rate" "$out/in.pcm")
		ours=$("$t" encode "$rate" "$mode" "$alloc" "$bitpool" "$out/in.pcm" "$out/ours.sbc")
		[ "$ours" = "$frames" ] || fail "$signal: we made $ours frames of $frames"
		[ "$("$t" headers "$rate" "$mode" "$alloc" "$bitpool" "$out/ours.sbc")" = "$frames" ] ||
			fail "$signal: our stream does not read back as $frames frames"
		ffm -f s16le -ar "$rate" -ac 2 -i "$out/in.pcm" -c:a sbc -b:a "$bit_rate" -q:a "$bitpool" \
			-f sbc -y "$out/ffmpeg.sbc" || fail "$signal: FFmpeg's encoder failed"
		[ "$("$t" headers "$rate" "$mode" loudness "$bitpool" "$out/ffmpeg.sbc")" = "$frames" ] ||
			fail "$signal: FFmpeg's stream is not $frames frames"
		decode "$out/ours.sbc" "$out/ours.pcm" "$frames" ||
			fail "$signal: FFmpeg's decoder did not decode every frame of our stream"
		decode "$out/ffmpeg.sbc" "$out/ffmpeg.pcm" "$frames" ||
			fail "$signal: FFmpeg's decoder did not decode every frame of its own stream"
		"$t" compare "$signal" "$out/in.pcm" "$out/ours.pcm" "$out/ffmpeg.pcm" "$frames"
	done
done
echo "PASS test_sbc_host"
