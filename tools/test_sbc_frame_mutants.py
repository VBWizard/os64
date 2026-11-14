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
sbc_frame_samples rather than by 128 (every accepted setting has 128); and the bit-rate search stepping past a
rate (`lo = mid + 2`), since each bitpool is reached by a run of several
rates and the encoder never reads the rate again, so the bitpool it lands
on is the same.

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
    ('null-settings-accepted', FRAME, '\tif (settings == NULL)\n\t\treturn false;\n', ''),
    ('44100-refused', FRAME, 'if (settings->rate != 44100 && settings->rate != 48000)',
     'if (settings->rate != 48000)'),
    ('48000-refused', FRAME, 'if (settings->rate != 44100 && settings->rate != 48000)',
     'if (settings->rate != 44100)'),
    ('any-rate', FRAME, 'if (settings->rate != 44100 && settings->rate != 48000)', 'if (false)'),
    ('any-channels', FRAME, 'settings->channels != 2 || ', ''),
    ('any-blocks', FRAME, 'settings->blocks != 16 || ', ''),
    ('any-subbands', FRAME, ' || settings->subbands != 8)', ')'),
    ('least-bitpool-refused', FRAME, 'settings->bitpool >= SBC_BITPOOL_MIN', 'settings->bitpool > SBC_BITPOOL_MIN'),
    ('bitpool-1-accepted', FRAME, 'settings->bitpool >= SBC_BITPOOL_MIN', 'settings->bitpool >= 1'),
    ('most-bitpool-refused', FRAME, 'settings->bitpool <= SBC_BITPOOL_MAX', 'settings->bitpool < SBC_BITPOOL_MAX'),
    ('bitpool-one-past-the-most', FRAME, 'settings->bitpool <= SBC_BITPOOL_MAX', 'settings->bitpool <= SBC_BITPOOL_MAX + 1'),
    # sbc_frame.c: the arithmetic.
    ('refused-settings-have-a-length', FRAME,
     '\tif (!sbc_settings_valid(settings))\n\t\treturn 0;\n\tuint32_t subbands', '\tuint32_t subbands'),
    ('join-bits-forgotten', FRAME, 'settings->joint ? subbands : 0', '0'),
    ('join-bits-always', FRAME, 'settings->joint ? subbands : 0', 'subbands'),
    ('one-channel-of-scale-factors', FRAME, '(4 * subbands * settings->channels) / 8', '(4 * subbands) / 8'),
    ('a-three-byte-header', FRAME, 'return 4 + (4 * subbands', 'return 3 + (4 * subbands'),
    ('samples-per-channel-doubled', FRAME, 'return (uint32_t)settings->blocks * settings->subbands;',
     'return (uint32_t)settings->blocks * settings->subbands * settings->channels;'),
    ('refused-settings-have-samples', FRAME,
     '\tif (!sbc_settings_valid(settings))\n\t\treturn 0;\n\treturn (uint32_t)settings->blocks', '\treturn (uint32_t)settings->blocks'),
    ('bit-rate-in-bytes', FRAME, '8ull * sbc_frame_bytes(settings)', '1ull * sbc_frame_bytes(settings)'),
    ('bit-rate-at-48000', FRAME, '* settings->rate;', '* 48000u;'),
    ('refused-settings-have-a-bit-rate', FRAME,
     '\tif (!sbc_settings_valid(settings))\n\t\treturn 0;\n\tuint64_t bits', '\tuint64_t bits'),
    # sbc_port.c: begin.
    ('begin-keeps-the-old-encoder', PORT, '\ts_ready = false;\n\tif (!sbc_settings_valid(settings))',
     '\tif (!sbc_settings_valid(settings))'),
    ('begin-skips-validation', PORT, '\tif (!sbc_settings_valid(settings))\n\t\treturn false;\n\n\tuint8_t', '\n\tuint8_t'),
    ('rates-swapped', PORT, 'settings->rate == 48000 ? SBC_sf48000 : SBC_sf44100',
     'settings->rate == 48000 ? SBC_sf44100 : SBC_sf48000'),
    ('modes-swapped', PORT, 'settings->joint ? SBC_JOINT_STEREO : SBC_STEREO',
     'settings->joint ? SBC_STEREO : SBC_JOINT_STEREO'),
    ('allocations-swapped', PORT, 'settings->snr ? SBC_SNR : SBC_LOUDNESS', 'settings->snr ? SBC_LOUDNESS : SBC_SNR'),
    ('search-stops-short', PORT, '\t\t\thi = mid;\n', '\t\t\thi = mid - 1;\n'),
    ('search-looks-for-more', PORT, 'if (s_params.s16BitPool >= settings->bitpool)', 'if (s_params.s16BitPool > settings->bitpool)'),
    ('no-final-settle', PORT, '\t}\n\tsettle(lo);\n', '\t}\n'),
    ('bitpool-unchecked', PORT, '\tif (s_params.s16BitPool != settings->bitpool)\n\t\treturn false;\n', ''),
    ('guard-not-laid', PORT, '\t\ts_frame[SBC_PORT_FRAME_ROOM + i] = SBC_PORT_GUARD_FILL;\n\ts_ready = true;',
     '\t\t;\n\ts_ready = true;'),
    ('begin-never-ready', PORT, '\ts_ready = true;\n\treturn true;', '\treturn true;'),
    # sbc_port.c: encode.
    ('encode-without-begin', PORT, '\tif (!s_ready)\n\t\treturn false;\n', ''),
    ('null-pcm-encoded', PORT, 'if (pcm == NULL || out == NULL || ', 'if (out == NULL || '),
    ('capacity-two-short-accepted', PORT, 'capacity < s_frame_bytes)', 'capacity + 1 < s_frame_bytes)'),
    ('capacity-must-exceed', PORT, 'capacity < s_frame_bytes)', 'capacity <= s_frame_bytes)'),
    ('length-unchecked', PORT, '\tif (length != s_frame_bytes)\n\t\treturn fail();\n', ''),
    ('guard-unread', PORT, '\t\tif (s_frame[SBC_PORT_FRAME_ROOM + i] != SBC_PORT_GUARD_FILL)\n\t\t\treturn fail();\n',
     '\t\t;\n'),
    ('failure-forgotten', PORT, 'static bool fail(void)\n{\n\ts_ready = false;\n', 'static bool fail(void)\n{\n'),
    ('copy-short', PORT, '\tfor (uint32_t i = 0; i < length; i++)\n\t\tout[i]', '\tfor (uint32_t i = 1; i < length; i++)\n\t\tout[i]'),
    ('copy-last-byte-lost', PORT, '\tfor (uint32_t i = 0; i < length; i++)\n\t\tout[i]', '\tfor (uint32_t i = 0; i + 1 < length; i++)\n\t\tout[i]'),
]

caught, missed, unbuilt = [], [], []
for name, target, old, new in mutants:
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
