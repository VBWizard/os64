#!/usr/bin/env python3
"""One deliberate break at a time to yonder's data: pictures
(docs/design/pending/DATA_URL_PICTURES_BACKPORT.md): the catalog that admits
one, the feed that hands the worker its own copy of the address, the worker
that decodes it without a fetch, the release that frees the copy on every
path, a sheet's data: background keyed as the markup's is, and the record
that names a long one short, and libpage keeping a picture's data: address
whole past OS64_URL_REF_MAX — against the scripted host's data_picture_cases
(tools/test_yonder_data_pictures.inc) and libpage's state_long_data_pictures
(tools/test_libpage_state.inc). A mutant is CAUGHT when they fail,
MISSED when they pass, which means a test is missing and gets written. A
build failure is neither.

The shape is test_yonder_corners_mutants.py's: each mutant is applied to the
worktree's copy of its file, the cases are run, and the file is put back
from the bytes read before the change, checked byte for byte, whatever
happens. Run it from a clean tree, and never beside another harness that
edits the same tree; the per-mutant logs are in --logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/data-pictures-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

YONDER = 'userland/apps/yonder/yonder.c'
PICTURE = 'userland/apps/yonder/picture.c'
RESOLVE = 'userland/libpage/resolve.c'
CASES = 'bash tools/test_libpage_host.sh && bash tools/test_yonder_scripts_host.sh'
TIMEOUT = 1800

# (name, file, old, new). Each breaks one rule the design states.
mutants = [
    # The model: a picture's data: address at any length, a link's not.
    ('a-long-data-picture-is-refused', RESOLVE,
     '    resolve_ref(page, ref, out, true);',
     '    resolve_ref(page, ref, out, false);'),
    ('a-long-data-link-is-taken', RESOLVE,
     '    resolve_ref(page, ref, out, false);\n}',
     '    resolve_ref(page, ref, out, true);\n}'),
    ('a-long-data-picture-keeps-its-case', RESOLVE,
     '            os64_memcpy(whole, "data", 4);\n            out->url = whole;',
     '            out->url = whole;'),
    # The catalog.
    ('the-catalog-refuses-data', YONDER,
     '                     data_address(url, len);',
     '                     (false && data_address(url, len));'),
    ('a-key-is-a-prefix', YONDER,
     '        if (pic->url_len == n && os64_memcmp(pic->url, url, n) == 0)',
     '        if (os64_memcmp(pic->url, url, n) == 0)'),
    # The feed.
    ('the-job-gets-no-copy', YONDER,
     '        if (data_address(pic->url, pic->url_len)) {\n            job->data_url = copy_text(pic->url);',
     '        if (false && data_address(pic->url, pic->url_len)) {\n            job->data_url = copy_text(pic->url);'),
    ('a-failed-copy-leaks-its-job', YONDER,
     '                yonder_picture_release(job, NULL);\n                picture_failed(p, pic->url, NULL, "no memory for its address");',
     '                picture_failed(p, pic->url, NULL, "no memory for its address");'),
    ('a-refused-job-leaks-its-address', YONDER,
     '            yonder_picture_release(job, NULL);\n            picture_failed(p, pic->url, NULL, "the workers would not take it");',
     '            os64_free(job);\n            picture_failed(p, pic->url, NULL, "the workers would not take it");'),
    # The worker and its release.
    ('the-worker-fetches-data', PICTURE,
     '    if (j->data_url != NULL) {',
     '    if (false && j->data_url != NULL) {'),
    ('an-empty-address-is-unknown', PICTURE,
     '        } else if (len == 0) {',
     '        } else if (false && len == 0) {'),
    ('the-release-keeps-the-address', PICTURE,
     '        os64_free(((yonder_picture_job_t *)job)->data_url);',
     '        (void)0;'),
    # A sheet's data: picture.
    ('a-sheet-data-picture-resolves', YONDER,
     '    if (!data_address(s, len))\n        return false;',
     '    if (true || !data_address(s, len))\n        return false;'),
    ('a-sheet-key-keeps-its-fragment', YONDER,
     '    *n = hash != NULL ? (size_t)(hash - s) : len;',
     '    *n = hash != NULL && false ? (size_t)(hash - s) : len;'),
    ('a-sheet-key-keeps-its-case', YONDER,
     '    key[n] = \'\\0\';\n    os64_memcpy(key, "data", 4);',
     '    key[n] = \'\\0\';\n    (void)0;'),
    ('the-painter-misses-a-sheet-data-picture', YONDER,
     '        if (os64_memcmp(at, "data", 4) == 0)\n            return picture_find_span(p, at, n);',
     '        if (os64_memcmp(at, "data", 4) == 0)\n            return -1;'),
    ('the-painter-misses-a-folded-one', YONDER,
     '        int32_t k = picture_find(p, key);',
     '        int32_t k = -1;'),
    # The record.
    ('a-refused-data-picture-is-a-missing-scheme', YONDER,
     '                             os64_streq(scheme, "file") || os64_streq(scheme, "data"));',
     '                             os64_streq(scheme, "file"));'),
    ('a-long-address-is-named-whole', YONDER,
     '    if (!data_address(url, n) || n <= 200)\n        return url;',
     '    if (true || !data_address(url, n) || n <= 200)\n        return url;'),
]


caught, missed, unbuilt = [], [], []
for name, rel, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / rel
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {rel}', file=sys.stderr)
        unbuilt.append(name)
        continue
    log = args.logs / f'{name}.log'
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            try:
                result = subprocess.run(['bash', '-c', CASES], cwd=root, stdout=out, stderr=out,
                                        timeout=TIMEOUT)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
    finally:
        path.write_bytes(original)
        if path.read_bytes() != original:
            print(f'{name}: {rel} WAS NOT RESTORED', file=sys.stderr)
            sys.exit(2)
    body = log.read_text(errors='replace')
    # A compiler's error, not a sanitizer's "runtime error:", which is a catch.
    built = re.search(r'\.(c|h|inc):\d+:\d+: error:', body) is None and 'undefined reference' not in body
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

print(f'\ndata: pictures mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
