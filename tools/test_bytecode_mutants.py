#!/usr/bin/env python3
"""One deliberate break at a time to the bytecode cache (BYTECODE_CACHE.md),
each against the suite that holds its rule: a mutant is CAUGHT when the
suite fails, MISSED when it passes, which means a test is missing and gets
written. A build failure is neither.

The rules: libjs's blob is refused unless it is this engine's, whole and
the payload's own (§ 4), is read under the run's name (§ 3), and a write is
judged by the allocator; the engine's identity follows its
sources; an entry is found by its whole key — the page's partition (an
opaque page's keeps nothing), the credentials, the address — and a
companion by its entry's key and its tag, living and dying with its entry
(§ 5, SCRIPT_CACHE.md § 2-3); a face is told whether the cache holds a body; the
worker looks for a companion only for one it holds; and the window runs
through the blob, keeps a compile's, lets its queue go, and says which ran.
A script's request uses the cache under its mode, a kept answer judged by
the live CORS check (SCRIPT_CACHE.md § 5-6); a module's blob is read only
under its name (§ 9), by its fetch job for an answer the entry holds, and
kept under the entry's key, credentials and all.

The shape is test_garb_mutants.py's: each mutant is applied to the
worktree's copy of its file, the suite is run, and the file is put back
from the bytes read before the change, checked byte for byte, whatever
happens. Run it from a clean tree; the per-mutant logs are in --logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/bytecode-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

RUNTIME = 'userland/libjs/runtime/runtime.c'
# The file a mutant breaks, not a suite: mutants_parallel.py runs every
# quoted tools/ script a harness names as a suite, so it is not spelled
# that way.
PREPARE = str(pathlib.Path('tools', 'js_prepare.py'))
CACHE = 'userland/libway/cache.c'
LOAD = 'userland/libway/load.c'
JOB = 'userland/apps/yonder/script_job.c'
BYTECODE = 'userland/apps/yonder/bytecode.c'
FETCHJOB = 'userland/apps/yonder/fetch_job.c'
MODULES = 'userland/apps/yonder/modules.c'
DOMFETCH = 'userland/libdom/fetch.c'
SCRIPTS = 'userland/apps/yonder/scripts.c'
YONDER = 'userland/apps/yonder/yonder.c'
JS = 'bash tools/test_js_runtime_host.sh'
PREPARED = 'python3 tools/test_js_prepare.py'
WAY = 'bash tools/test_way_host.sh'
FETCH = 'bash tools/test_way_fetch_host.sh'
WORKER = 'bash tools/test_yonder_script_job_host.sh'
FETCHJOBS = 'bash tools/test_yonder_fetch_job_host.sh'
WINDOW = 'bash tools/test_yonder_scripts_host.sh'

# (name, file, old, new, harness). Each breaks one rule the design states.
mutants = [
    # libjs: the blob, its name, and the allocator's judgment.
    ('the-name-is-not-given-at-the-read', RUNTIME,
     'JS_ReadObjectNamed(runtime->context, payload, payload_length,\n'
     '                                              JS_READ_OBJ_BYTECODE, source_name);',
     'JS_ReadObject(runtime->context, payload, payload_length,\n'
     '                                              JS_READ_OBJ_BYTECODE);', JS),
    ('the-engine-is-not-compared', RUNTIME,
     ' ||\n        os64_memcmp(blob + BLOB_MAGIC_LENGTH, OS64_JS_ENGINE_ID, BLOB_ENGINE_LENGTH) != 0)',
     ')', JS),
    ('the-kind-is-not-compared', RUNTIME,
     '    if (kind[0] != want || kind[1] != 0 || kind[2] != 0 || kind[3] != 0)\n        return NULL;\n',
     '', JS),
    ('the-length-is-not-compared', RUNTIME,
     '    if (said != length - at) return NULL;\n', '', JS),
    ('the-digest-is-not-compared', RUNTIME,
     '    if (get_u64(kind + 12) != blob_digest(payload, (size_t)said)) return NULL;\n', '', JS),
    ('a-write-is-judged-by-its-answer', RUNTIME,
     '    if (payload == NULL || runtime->allocator.failures != failures) {',
     '    if (payload == NULL) {', JS),
    ('a-refused-write-fails-the-runtime', RUNTIME,
     '        discard_exception(runtime->context);\n        runtime->allocator.failures = failures;\n        return;',
     '        discard_exception(runtime->context);\n        return;', JS),
    ('a-compile-that-throws-is-kept', RUNTIME,
     '    if (!JS_IsException(function) && bytecode->keep) blob_keep(runtime, function, bytecode, BLOB_KIND_CLASSIC, NULL);',
     '    if (bytecode->keep) blob_keep(runtime, function, bytecode, BLOB_KIND_CLASSIC, NULL);', JS),
    # libjs: a module's blob (BYTECODE_CACHE.md § 9).
    ('a-module-blob-of-another-name-is-read', RUNTIME,
     '            os64_memcmp(blob + at + 8, name, nlen) != 0)', '            false)', JS),
    ('a-module-is-not-read-from-its-blob', RUNTIME,
     '        if (JS_VALUE_GET_TAG(value) == JS_TAG_MODULE) {\n            bytecode->from_blob = true;',
     '        if (false) {\n            bytecode->from_blob = true;', JS),
    ('a-module-compile-is-not-kept', RUNTIME,
     '            blob_keep(runtime, value, bytecode, BLOB_KIND_MODULE, name);', '            (void)name;', JS),
    ('a-module-compile-that-throws-is-kept', RUNTIME,
     '        if (!JS_IsException(value) && bytecode != NULL && bytecode->keep)',
     '        if (bytecode != NULL && bytecode->keep)', JS),
    # The engine's identity.
    ('the-identity-ignores-the-sources', PREPARE,
     "    identity.update(name.encode() + b'\\0' + str(len(data)).encode() + b'\\0' + data)\n",
     "    identity.update(name.encode())\n", PREPARED),
    # libway: the companion.
    ('a-drop-keeps-the-companion', CACHE,
     '    if (path_of(c, key, KIND_COMPANION, path, sizeof(path)))\n        (void)os64_unlink(path);\n}',
     '}', WAY),
    ('a-clear-keeps-the-companions', CACHE,
     '        if (is_kept(e.name) && os64_snprintf(path, sizeof(path), "%s/%s", c->dir, e.name) > 0 &&',
     '        if (is_kind(e.name, KIND_ENTRY) && os64_snprintf(path, sizeof(path), "%s/%s", c->dir, e.name) > 0 &&',
     WAY),
    ('the-sweep-passes-companions-by', CACHE,
     '        if (!is_kept(e.name))\n            continue;',
     '        if (!is_kind(e.name, KIND_ENTRY))\n            continue;', WAY),
    ('a-companion-is-counted-as-an-entry', CACHE,
     '            *entries += is_kind(e.name, KIND_ENTRY);', '            (*entries)++;', WAY),
    ('the-tag-is-not-checked', CACHE,
     'else if (os64_streq(line, "tag") && os64_streq(v, f->tag))', 'else if (os64_streq(line, "tag"))', WAY),
    ('the-address-is-not-checked', CACHE,
     'else if (os64_streq(line, "url") && os64_streq(v, f->key->url))', 'else if (os64_streq(line, "url"))', WAY),
    # libway: the key (SCRIPT_CACHE.md § 2-3).
    ('the-credentials-are-not-in-the-name', CACHE,
     '    h = hash(h, key->credentials ? "\\n1\\n" : "\\n0\\n");', '    h = hash(h, "\\n1\\n");', WAY),
    ('the-partition-is-not-in-the-name', CACHE,
     '    uint64_t h = hash(0xcbf29ce484222325ull, key->partition);',
     '    uint64_t h = 0xcbf29ce484222325ull;', WAY),
    ('an-opaque-page-keeps', CACHE,
     '           os64_memcmp(key->partition, kOpaque, sizeof(kOpaque) - 1) != 0;',
     '           kOpaque[0] != 0;', WAY),
    ('the-entry-key-is-not-checked', CACHE,
     "    if (!entry_parse(head, f->e, &f->len) || !key_is(f->key, f->e->partition, f->e->credentials, f->e->url) ||",
     "    if (!entry_parse(head, f->e, &f->len) || (!key_is(f->key, f->e->partition, f->e->credentials, f->e->url) && false) ||",
     WAY),
    ('whole-is-not-partitioned', LOAD,
     '    way_partition_of(&asked_for.origin, partition, sizeof(partition));',
     '    os64_strcopy(partition, sizeof(partition), "https all");', FETCH),
    # libway: whether the cache holds a body.
    ('a-body-a-304-forbids-is-held', LOAD, '        out->kept = keep;', '        out->kept = true;', FETCH),
    ('a-stale-body-is-not-held', LOAD,
     '            served = serve(out, e, kept, kept_len, WAY_SERVED_STALE);\n            out->kept = true;',
     '            served = serve(out, e, kept, kept_len, WAY_SERVED_STALE);', FETCH),
    # yonder's worker.
    ('a-body-not-held-has-its-companion-read', JOB,
     'if (s->ok && body.kept && !cancelled(ctx)', 'if (s->ok && !cancelled(ctx)', WORKER),
    ('the-key-is-not-the-text', BYTECODE, '        h ^= bytes[i];', '        h ^= (uint8_t)(bytes[i] & 0);', WORKER),
    # yonder's fetch job: a script's request through the cache
    # (SCRIPT_CACHE.md § 5-6), and a module's blob beside its entry.
    ('a-kept-reply-is-not-checked-live', FETCHJOB,
     '    if (r->tainting == WAY_TAINT_CORS && !way_cors_check(r, &resp))\n        return false;\n', '', FETCHJOBS),
    ('the-job-key-forgets-the-credentials', FETCHJOB,
     'way_credentials_included(r), c->url};', 'true, c->url};', FETCHJOBS),
    ('the-job-key-forgets-the-partition', FETCHJOB,
     '    way_partition_of(&r->origin, c->partition, sizeof(c->partition));',
     '    os64_strcopy(c->partition, sizeof(c->partition), "https all");', FETCHJOBS),
    ('a-redirected-answer-is-kept', FETCHJOB,
     ' && r->urls == 1 && head->status == 200 &&', ' && r->urls >= 1 && head->status == 200 &&', FETCHJOBS),
    ('only-if-cached-asks-on-a-miss', FETCHJOB,
     '    if (!c->have)\n        return mode == WAY_CACHE_ONLY_IF_CACHED ? CACHE_REFUSED : CACHE_MISS;',
     '    if (!c->have)\n        return CACHE_MISS;', FETCHJOBS),
    ('force-cache-minds-the-age', FETCHJOB,
     '    bool any_age = mode == WAY_CACHE_FORCE_CACHE || mode == WAY_CACHE_ONLY_IF_CACHED;',
     '    bool any_age = mode == WAY_CACHE_ONLY_IF_CACHED;', FETCHJOBS),
    ('no-cache-serves-fresh', FETCHJOB, '(mode != WAY_CACHE_NO_CACHE && way_keep_fresh(', '(way_keep_fresh(',
     FETCHJOBS),
    ('reload-reads', FETCHJOB, '    if (job->cache_mode != WAY_CACHE_RELOAD &&\n', '    if (\n', FETCHJOBS),
    ('no-store-uses-the-cache', FETCHJOB, ' && job->cache_mode != WAY_CACHE_NO_STORE &&', ' &&', FETCHJOBS),
    ('a-range-uses-the-cache', FETCHJOB, '    return !range && way_cache_enabled(', '    return way_cache_enabled(',
     FETCHJOBS),
    ('vary-is-not-matched', FETCHJOB,
     '        c->have = way_vary_matches(c->blocks.vary, c->blocks.vary_len, request_value, c);',
     '        c->have = true;', FETCHJOBS),
    ('any-fetch-reads-a-module-blob', FETCHJOB, '    if (job->module && got->ok && cached != NULL',
     '    if (got->ok && cached != NULL', FETCHJOBS),
    ('a-body-not-held-has-its-module-blob-read', FETCHJOB, 'cached != NULL && cached->held &&', 'cached != NULL &&',
     FETCHJOBS),
    ('the-module-record-forgets-the-credentials', FETCHJOB, '    bc->credentials = c->key.credentials;',
     '    bc->credentials = true;', FETCHJOBS),
    ('the-cache-mode-is-not-carried', DOMFETCH, '.cache = (uint8_t)q->cache,', '.cache = 0,', WINDOW),
    # yonder's window.
    ('the-blob-is-not-run', SCRIPTS, '            bc.blob = bytecode->blob;', '            bc.blob = NULL;', WINDOW),
    ('a-compile-is-not-kept', SCRIPTS,
     '            bc.keep = s->options.keep_bytecode != NULL;', '            bc.keep = false;', WINDOW),
    ('a-module-blob-is-not-run', MODULES, '{.blob = kept->blob, .blob_length', '{.blob = NULL, .blob_length', WINDOW),
    ('the-map-keeps-no-module-compile', MODULES, '.keep = m->net.keep != NULL};', '.keep = false};', WINDOW),
    ('a-module-fetch-is-not-said', YONDER, '    job->module = (token & YONDER_MODULE_TOKEN) != 0;\n', '', WINDOW),
    ('the-keep-job-forgets-the-credentials', YONDER, '    job->credentials = where->credentials;\n', '', WINDOW),
    ('the-queue-is-not-let-go', YONDER,
     '        s_bytecode_queued -= ((const yonder_bytecode_job_t *)job)->length;\n', '', WINDOW),
    ('the-record-does-not-say', YONDER,
     '    const char *from = yonder_scripts_task_from_bytecode(host) ? " from its bytecode" : "";',
     '    const char *from = "";', WINDOW),
]

caught, missed, unbuilt = [], [], []
for name, rel, old, new, harness in mutants:
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
                result = subprocess.run(['bash', '-c', harness], cwd=root, stdout=out, stderr=out, timeout=1800)
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

print(f'\nbytecode cache mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
