#!/usr/bin/env python3
"""One deliberate break at a time to the SBC encoder's code that is OURS
(kernel/src/audio/sbc_frame.c: the settings and the frame arithmetic;
kernel/ports/sbc/port/sbc_port.c: the boundary), against
`tools/test_sbc_host.sh --selftest`: a mutant is CAUGHT when the suite fails,
MISSED when it passes, which means a test is missing and gets written. A
build failure is neither. Upstream's files are not mutated: they are not
ours to change, and FFmpeg's half of the suite is what holds them.

Not here, because no accepted setting can tell them apart from the code as
written: the rounding up in sbc_frame_bytes (16 blocks and 8 join bits make
every frame a whole number of bytes), and sbc_bit_rate's division by
sbc_frame_samples rather than by 128 (every accepted setting has 128).

The shape is test_audio_stream_mutants.py's: each mutant is applied to the
tree's copy of the file, the suite is run, and the file is put back from the
bytes read before the change, checked byte for byte, whatever happens. Run it
from a clean tree, and never beside another harness that edits the same tree;
the per-mutant logs are in --logs. Under tools/mutants_parallel.py it needs
`--with kernel`: the runner's copies do not carry the kernel's sources
unless asked.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/sbc-frame-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

FRAME = 'kernel/src/audio/sbc_frame.c'
PORT = 'kernel/ports/sbc/port/sbc_port.c'
CASES = 'bash tools/test_sbc_host.sh --selftest'

mutants = [
    # sbc_frame.c: what the boundary accepts.
    (FRAME, 'null-settings-accepted', '\tif (settings == NULL)\n\t\treturn false;\n', ''),
    (FRAME, '44100-refused', 'if (settings->rate != 44100 && settings->rate != 48000)',
     'if (settings->rate != 48000)'),
    (FRAME, '48000-refused', 'if (settings->rate != 44100 && settings->rate != 48000)',
     'if (settings->rate != 44100)'),
    (FRAME, 'any-rate', 'if (settings->rate != 44100 && settings->rate != 48000)', 'if (false)'),
    (FRAME, 'any-channels', 'settings->channels != 2 || ', ''),
    (FRAME, 'any-blocks', 'settings->blocks != 16 || ', ''),
    (FRAME, 'any-subbands', ' || settings->subbands != 8)', ')'),
    (FRAME, 'least-bitpool-refused', 'settings->bitpool >= SBC_BITPOOL_MIN', 'settings->bitpool > SBC_BITPOOL_MIN'),
    (FRAME, 'bitpool-1-accepted', 'settings->bitpool >= SBC_BITPOOL_MIN', 'settings->bitpool >= 1'),
    (FRAME, 'most-bitpool-refused', 'settings->bitpool <= SBC_BITPOOL_MAX', 'settings->bitpool < SBC_BITPOOL_MAX'),
    (FRAME, 'bitpool-unbounded', 'settings->bitpool <= SBC_BITPOOL_MAX', 'settings->bitpool <= 255'),
    # sbc_frame.c: the arithmetic.
    (FRAME, 'refused-settings-have-a-length',
     '\tif (!sbc_settings_valid(settings))\n\t\treturn 0;\n\tuint32_t subbands', '\tuint32_t subbands'),
    (FRAME, 'join-bits-forgotten', 'settings->joint ? subbands : 0', '0'),
    (FRAME, 'join-bits-always', 'settings->joint ? subbands : 0', 'subbands'),
    (FRAME, 'one-channel-of-scale-factors', '(4 * subbands * settings->channels) / 8', '(4 * subbands) / 8'),
    (FRAME, 'a-three-byte-header', 'return 4 + (4 * subbands', 'return 3 + (4 * subbands'),
    (FRAME, 'samples-per-channel-doubled', 'return (uint32_t)settings->blocks * settings->subbands;',
     'return (uint32_t)settings->blocks * settings->subbands * settings->channels;'),
    (FRAME, 'refused-settings-have-samples',
     '\tif (!sbc_settings_valid(settings))\n\t\treturn 0;\n\treturn (uint32_t)settings->blocks', '\treturn (uint32_t)settings->blocks'),
    (FRAME, 'bit-rate-in-bytes', '8ull * sbc_frame_bytes(settings)', '1ull * sbc_frame_bytes(settings)'),
    (FRAME, 'bit-rate-at-48000', '* settings->rate;', '* 48000u;'),
    (FRAME, 'refused-settings-have-a-bit-rate',
     '\tif (!sbc_settings_valid(settings))\n\t\treturn 0;\n\tuint64_t bits', '\tuint64_t bits'),
    # sbc_port.c: begin.
    (PORT, 'begin-keeps-the-old-encoder', '\ts_ready = false;\n\tif (!sbc_settings_valid(settings))',
     '\tif (!sbc_settings_valid(settings))'),
    (PORT, 'begin-skips-validation', '\tif (!sbc_settings_valid(settings))\n\t\treturn false;\n\n\tuint8_t', '\n\tuint8_t'),
    (PORT, 'rates-swapped', 'settings->rate == 48000 ? SBC_sf48000 : SBC_sf44100',
     'settings->rate == 48000 ? SBC_sf44100 : SBC_sf48000'),
    (PORT, 'modes-swapped', 'settings->joint ? SBC_JOINT_STEREO : SBC_STEREO',
     'settings->joint ? SBC_STEREO : SBC_JOINT_STEREO'),
    (PORT, 'allocations-swapped', 'settings->snr ? SBC_SNR : SBC_LOUDNESS', 'settings->snr ? SBC_LOUDNESS : SBC_SNR'),
    (PORT, 'search-stops-short', '\t\t\thi = mid;\n', '\t\t\thi = mid - 1;\n'),
    (PORT, 'search-overshoots', '\t\t\tlo = mid + 1;\n', '\t\t\tlo = mid + 2;\n'),
    (PORT, 'search-looks-for-more', 'if (s_params.s16BitPool >= settings->bitpool)', 'if (s_params.s16BitPool > settings->bitpool)'),
    (PORT, 'no-final-settle', '\t}\n\tsettle(lo);\n', '\t}\n'),
    (PORT, 'bitpool-unchecked', '\tif (s_params.s16BitPool != settings->bitpool)\n\t\treturn false;\n', ''),
    (PORT, 'guard-not-laid', '\t\ts_frame[SBC_PORT_FRAME_ROOM + i] = SBC_PORT_GUARD_FILL;\n\ts_ready = true;',
     '\t\t;\n\ts_ready = true;'),
    (PORT, 'begin-never-ready', '\ts_ready = true;\n\treturn true;', '\treturn true;'),
    # sbc_port.c: encode.
    (PORT, 'encode-without-begin', '\tif (!s_ready)\n\t\treturn false;\n', ''),
    (PORT, 'null-pcm-encoded', 'if (pcm == NULL || out == NULL || ', 'if (out == NULL || '),
    (PORT, 'capacity-unchecked', ' || capacity < s_frame_bytes)', ')'),
    (PORT, 'capacity-must-exceed', 'capacity < s_frame_bytes)', 'capacity <= s_frame_bytes)'),
    (PORT, 'length-unchecked', '\tif (length != s_frame_bytes)\n\t\treturn fail();\n', ''),
    (PORT, 'guard-unread', '\t\tif (s_frame[SBC_PORT_FRAME_ROOM + i] != SBC_PORT_GUARD_FILL)\n\t\t\treturn fail();\n',
     '\t\t;\n'),
    (PORT, 'failure-forgotten', 'static bool fail(void)\n{\n\ts_ready = false;\n', 'static bool fail(void)\n{\n'),
    (PORT, 'copy-short', '\tfor (uint32_t i = 0; i < length; i++)\n\t\tout[i]', '\tfor (uint32_t i = 1; i < length; i++)\n\t\tout[i]'),
    (PORT, 'copy-last-byte-lost', '\tfor (uint32_t i = 0; i < length; i++)\n\t\tout[i]', '\tfor (uint32_t i = 0; i + 1 < length; i++)\n\t\tout[i]'),
]

caught, missed, unbuilt = [], [], []
for target, name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / target
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {target}', file=sys.stderr)
        unbuilt.append(name)
        continue
    log = args.logs / f'{name}.log'
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            try:
                result = subprocess.run(['bash', '-c', CASES], cwd=root, stdout=out, stderr=out, timeout=300)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
    finally:
        path.write_bytes(original)
        if path.read_bytes() != original:
            print(f'{name}: {target} WAS NOT RESTORED', file=sys.stderr)
            sys.exit(2)
    body = log.read_text(errors='replace')
    # A compiler's error, not a sanitizer's "runtime error:", which is a catch.
    built = re.search(r'\.(c|h):\d+:\d+: error:', body) is None and 'undefined reference' not in body
    if not built:
        unbuilt.append(name)
        verdict = 'did not build'
    elif code != 0:
        caught.append(name)
        verdict = 'caught'
    else:
        missed.append(name)
        verdict = 'MISSED'
    print(f'{name}: {verdict}', flush=True)

print(f'\nsbc mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
