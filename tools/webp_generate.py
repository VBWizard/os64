#!/usr/bin/env python3
"""Generate the private WebP adaptation and embedded notice from pinned inputs."""
import argparse
import difflib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'userland/libwebp'

def alpha_source():
    original = (BASE/'upstream/src/dec/alpha_dec.c').read_text()
    text = original
    edits = {
        '  int rsrv;\n': '',
        '  rsrv = (data[0] >> 6) & 0x03;\n': '',
        '      dec->filter >= WEBP_FILTER_LAST ||\n'
        '      dec->pre_processing > ALPHA_PREPROCESSED_LEVELS ||\n'
        '      rsrv != 0)': '      dec->filter >= WEBP_FILTER_LAST)',
    }
    for old, new in edits.items():
        assert text.count(old) == 1, old
        text = text.replace(old, new)
    patch = ''.join(difflib.unified_diff(original.splitlines(True), text.splitlines(True),
                    fromfile='upstream/src/dec/alpha_dec.c', tofile='generated/alpha_dec.c'))
    assert patch == (BASE/'port/alpha_dec.patch').read_text(), 'unrecorded adaptation'
    return text

def generate(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    (directory/'alpha_dec.c').write_text(alpha_source())
    notice = (ROOT/'license/libwebp-LICENSE').read_text()
    (directory/'webp_license.h').write_text('static const char webp_license[] =\n' +
        '\n'.join(json.dumps(line, ensure_ascii=True) for line in notice.splitlines(True)) + ';\n')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    generate(parser.parse_args().output)
