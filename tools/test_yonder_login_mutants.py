#!/usr/bin/env python3
"""One deliberate break at a time to the login slice — the Cookie line's
budget and which cookies go when they do not all fit, a request sent once
riding only a connection used moments ago, and the jar kept in its file —
against the harnesses that hold each rule: a mutant is CAUGHT when one of
its harnesses fails, MISSED when they all pass, which means a test is
missing and gets written. A build failure is neither.

Each mutant is applied to the worktree's copy of its file, its harnesses
are run, and the file is put back from the bytes read before the change —
checked byte for byte — whatever happens. Nothing else is touched. Run it
from a clean tree, one run at a time, or through tools/mutants_parallel.py.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/yonder-login-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

JAR_C = 'userland/libway/jar.c'
JAR_H = 'userland/libway/include/way/jar.h'
LOAD_C = 'userland/libway/load.c'
FETCH_C = 'userland/libfetch/fetch.c'
POOL_C = 'userland/libfetch/pool.c'
JOB_C = 'userland/apps/yonder/fetch_job.c'
YONDER_C = 'userland/apps/yonder/yonder.c'
DIAG_C = 'userland/apps/yonder/diag.c'
COOKIES_C = 'userland/apps/yonder/cookies.c'
SETTINGS_C = 'userland/apps/yonder/settings.c'
WAY = 'tools/test_way_host.sh'
WAYF = 'tools/test_way_fetch_host.sh'
FETCH = 'tools/test_fetch_host.sh'
POOL = 'tools/test_fetch_pool_host.sh'
JOB = 'tools/test_yonder_fetch_job_host.sh'
SCRIPTS = 'tools/test_yonder_scripts_host.sh'

# (name, file, old, new, harnesses). Each breaks one rule the slice
# states; none supplies a replacement expectation.
mutants = [
    # 1. The budget, and which cookies go
    ('budget-line-1k', JAR_H, '#define WAY_COOKIE_LINE_MAX    8192', '#define WAY_COOKIE_LINE_MAX    1024', (WAY,)),
    ('pick-oldest-kept', JAR_C, 'a->http_only != b->http_only ? a->http_only : older(b, a);',
     'a->http_only != b->http_only ? a->http_only : older(a, b);', (WAY,)),
    ('pick-httponly-ignored', JAR_C, 'a->http_only != b->http_only ? a->http_only : older(b, a);', 'older(b, a);',
     (WAY,)),
    ('pick-stops-at-misfit', JAR_C, '            first->pick = PICK_NO;\n            left++;',
     '            first->pick = PICK_NO;\n            left++;\n            room = 0;', (WAY,)),
    ('read-name-equals', JAR_C, "    if (os64_strchr(field[5], '=') != NULL || os64_strchr(field[5], ';') != NULL ||",
     "    if (os64_strchr(field[5], ';') != NULL ||", (WAY,)),
    ('read-padded', JAR_C, '    return n > 0 && (wsp(s[0]) || wsp(s[n - 1]));', '    return n > 0 && s[0] == 0x7f;', (WAY,)),
    ('write-expiry-unclamped', JAR_C, '(long)(c->expiry < FILE_EXPIRY_MAX ? c->expiry : FILE_EXPIRY_MAX)',
     '(long)c->expiry', (WAY,)),
    ('hop-line-uncapped', JAR_C, 'size_t line_cap = cap < WAY_COOKIE_LINE_MAX ? cap : WAY_COOKIE_LINE_MAX;',
     'size_t line_cap = cap;', (WAY,)),
    ('short-note-any', JAR_C, 'if (s == NULL || to == NULL || left <= s->left)',
     'if (s == NULL || to == NULL || left <= 0)', (WAY,)),
    ('leg-no-note', LOAD_C, '    way_cookies_short_note(&s->cookies_short, hop, left);', '    (void)left;', (WAYF,)),
    ('hooks-no-note', LOAD_C, '    way_cookies_short_note(&h->cookies_short, hop, left);', '    (void)left;', (WAYF,)),
    ('fetch-room-short', FETCH_C, 'const size_t room = sizeof(f->hop_fields);',
     'const size_t room = sizeof(f->hop_fields) - 4096;', (FETCH,)),
    ('job-uncapped', JOB_C, '    if (cap > WAY_COOKIE_LINE_MAX)\n        cap = WAY_COOKIE_LINE_MAX;\n', '', (JOB,)),
    ('job-no-note', JOB_C, '    way_cookies_short_note(h->cookies_short, to, left);\n', '    (void)left;\n', (JOB,)),
    ('record-verdict-dropped', YONDER_C,
     '        diag_cookies_short(g.stream.diag, &g.stream.verdict.cookies_short);\n', '', (SCRIPTS,)),
    ('record-picture-dropped', YONDER_C,
     '    diag_cookies_short(g.page.diag, &job->hooks.cookies_short);\n    Picture *pic', '    Picture *pic', (SCRIPTS,)),
    ('record-most-is-last', DIAG_C,
     'YONDER_DIAG_RECORDS_MAX) : NULL;\n    if (l != NULL && count > l->count)\n        l->count = count;\n}\n\nuint32_t',
     'YONDER_DIAG_RECORDS_MAX) : NULL;\n    if (l != NULL)\n        l->count = count;\n}\n\nuint32_t', (SCRIPTS,)),
    ('record-none-said', YONDER_C, '    if (d == NULL || s->left <= 0)\n        return;\n    char message',
     '    if (d == NULL || s->left < 0)\n        return;\n    char message', (SCRIPTS,)),
    # 2. A request sent once
    ('once-limit-ignored', FETCH_C, 'uint32_t idle_most = idempotent(f) ? FETCH_POOL_IDLE_MS : FETCH_POOL_ONCE_MS;',
     'uint32_t idle_most = FETCH_POOL_IDLE_MS;', (FETCH,)),
    ('once-always-fresh', FETCH_C, 'uint32_t idle_most = idempotent(f) ? FETCH_POOL_IDLE_MS : FETCH_POOL_ONCE_MS;',
     'uint32_t idle_most = idempotent(f) ? FETCH_POOL_IDLE_MS : 0;', (FETCH,)),
    ('once-get-too', FETCH_C, 'uint32_t idle_most = idempotent(f) ? FETCH_POOL_IDLE_MS : FETCH_POOL_ONCE_MS;',
     'uint32_t idle_most = FETCH_POOL_ONCE_MS;', (FETCH,)),
    ('pool-limit-ignored', POOL_C, '        if (same_key(&k->key, key) && !idle_past(k, &now, idle_most_ms) &&',
     '        if (same_key(&k->key, key) && !idle_past(k, &now, idle_most_ms * 0 + FETCH_POOL_IDLE_MS) &&',
     (POOL, FETCH)),
    ('pool-passed-over-closed', POOL_C, '        if (!timed || expired(k, &now)) {',
     '        if (!timed || expired(k, &now) || idle_past(k, &now, idle_most_ms)) {', (POOL,)),
    ('pool-boundary', POOL_C, '    return idle >= (uint64_t)ms * now->per_second / 1000u;',
     '    return idle > (uint64_t)ms * now->per_second / 1000u;', (POOL, FETCH)),
    # 3. The jar's file (libway)
    ('text-writes-session', JAR_C, '    if (c->expiry == INT64_MAX)\n        return false;\n    size_t n = spell_line',
     '    size_t n = spell_line', (WAY,)),
    ('text-writes-expired', JAR_C, '    if (expiry == 0 || expiry <= now)\n        return LINE_NONE;',
     '    if (expiry == 0 || expiry <= now * 0)\n        return LINE_NONE;', (WAY,)),
    ('text-writes-unreadable', JAR_C, '        c->pick = round_trips(c, now, scratch) ? PICK_MATCHES : PICK_NO;',
     '        c->pick = c->expiry != INT64_MAX && c->expiry > now && field_ok(c->value) ? PICK_MATCHES : PICK_NO;',
     (WAY,)),
    ('follow-marks-unwritable', JAR_C, 'c->changed == 0 && round_trips(c, now, line) ? PICK_MATCHES',
     'c->changed == 0 && c->expiry != INT64_MAX ? PICK_MATCHES', (WAY,)),
    ('session-over-change-unkept', JAR_C, ' || jar->cookies[same].changed != 0;', ';', (WAY,)),
    ('put-in-keeps-source-age', JAR_C, '    int64_t created = now;\n    uint64_t order = jar->next_order++, used = 0;',
     '    int64_t created = c->created != 0 ? c->created : now;\n    uint64_t order = jar->next_order++, used = 0;', (WAY,)),
    ('overlay-hides-no-memory', JAR_C, '            whole = put_in(to, c, now);', '            (void)put_in(to, c, now);', (WAY,)),
    ('text-no-httponly', JAR_C, 'c->http_only ? "#HttpOnly_" : ""', 'c->http_only ? "" : ""', (WAY,)),
    ('text-no-dot', JAR_C, 'c->host_only ? "" : ".", c->domain', '"", c->domain', (WAY,)),
    ('text-unordered', JAR_C, '    by_age(jar->cookies, idx, idx + n, n);\n', '    by_age(jar->cookies, idx, idx + n, 0);\n',
     (WAY,)),
    ('read-not-judged', JAR_C, '    if (!judged(h, h->domain, h->path, secure))\n        return LINE_REFUSED;',
     '    if (false && !judged(h, h->domain, h->path, secure))\n        return LINE_REFUSED;', (WAY,)),
    ('read-single-label', JAR_C, '    if (sub && !dotted)\n        return LINE_REFUSED;\n', '', (WAY,)),
    ('read-expired', JAR_C, '    if (expiry == 0 || expiry <= now)\n        return LINE_NONE;',
     '    if (expiry == 0 || expiry <= now * 0)\n        return LINE_NONE;', (WAY,)),
    ('read-long-line', JAR_C, 'bool whole = n < WAY_JAR_LINE_MAX && os64_memchr', 'bool whole = os64_memchr', (WAY,)),
    ('read-nul', JAR_C, "bool whole = n < WAY_JAR_LINE_MAX && os64_memchr(text + at, '\\0', n) == NULL;",
     'bool whole = n < WAY_JAR_LINE_MAX;', (WAY,)),
    ('read-expiry-digits', JAR_C, "        if (!digit(s[n]) || n == 18)", "        if (!digit(s[n]) || n == 19)", (WAY,)),
    ('read-fields-eight', JAR_C, '        if (fields == 7)\n            return LINE_REFUSED;\n', '', (WAY,)),
    ('read-change-overwritten', JAR_C, 'bool read = changed_here(jar, &c) || put_in(jar, &c, now);',
     'bool read = (changed_here(jar, &c) && false) || put_in(jar, &c, now);', (WAY,)),
    ('follow-lets-nothing-go', JAR_C, '            if (jar->cookies[i].pick == PICK_MATCHES)\n                drop(jar, i);',
     '            if (false)\n                drop(jar, i);', (WAY,)),
    ('follow-short-lets-go', JAR_C, '    if (follow && !jar->refusing && !report->short_of_memory) {',
     '    if (follow && !jar->refusing) {', (WAY,)),
    ('put-in-drops-first', JAR_C,
     '    if (insert(jar, c->name, os64_strlen(c->name), c->value, os64_strlen(c->value), c->domain, c->path) == NULL)\n        return false;',
     '    if (same >= 0)\n        jar->cookies[same].expiry = INT64_MIN;\n'
     '    if (insert(jar, c->name, os64_strlen(c->name), c->value, os64_strlen(c->value), c->domain, c->path) == NULL)\n        return false;',
     (WAY,)),
    ('put-in-limits-on-its-string', JAR_C, '    enforce_limits(jar, domain);\n    return true;',
     '    enforce_limits(jar, n->domain);\n    return true;', (WAY,)),
    ('overlay-ignores-gone', JAR_C, '        if (g->changed > through)\n            continue;\n        for',
     '        if (true)\n            continue;\n        for', (WAY,)),
    ('overlay-session-keeps-kept', JAR_C,
     '        // A session cookie in a kept one\'s place: the kept one goes.\n        for (int32_t k = 0; k < to->n; k++) {',
     '        // A session cookie in a kept one\'s place: the kept one goes.\n        for (int32_t k = 0; k < 0; k++) {',
     (WAY,)),
    ('saved-clears-later', JAR_C, '        if (jar->cookies[i].changed <= through)\n            jar->cookies[i].changed = 0;',
     '        jar->cookies[i].changed = 0;', (WAY,)),
    ('dirty-ignores-replaced', JAR_C, '    if (expiry != INT64_MAX || was_kept) {', '    if (expiry != INT64_MAX) {', (WAY,)),
    ('gone-not-noted', JAR_C, '        if (was_kept && expiry <= now)\n            gone_note(jar, &jar->cookies[same]);\n',
     '        if (false && was_kept && expiry <= now)\n            gone_note(jar, &jar->cookies[same]);\n',
     (WAY,)),
    ('refuse-hears', JAR_C,
     '    os64_lock_acquire(&jar->lock);\n    if (jar->refusing) {\n        os64_lock_release(&jar->lock);\n        return;\n    }',
     '    os64_lock_acquire(&jar->lock);', (WAY,)),
    ('refuse-sends', JAR_C,
     '    if (jar->refusing) {\n        os64_lock_release(&jar->lock);\n        return 0;\n    }\n', '', (WAY,)),
    # 3. The jar's file (yonder)
    ('write-not-coalesced', COOKIES_C, '(k->failed ? YONDER_COOKIES_RETRY_MS : YONDER_COOKIES_WRITE_MS)', '0', (SCRIPTS,)),
    ('retry-at-once', COOKIES_C, '(k->failed ? YONDER_COOKIES_RETRY_MS : YONDER_COOKIES_WRITE_MS)',
     'YONDER_COOKIES_WRITE_MS', (SCRIPTS,)),
    ('session-writes', COOKIES_C, "k->mode != YONDER_COOKIES_ON || k->path[0] == '\\0' || way_jar_changes",
     "k->path[0] == '\\0' || way_jar_changes", (SCRIPTS,)),
    ('off-keeps-file', COOKIES_C, '            (void)empty_file(k);            // a failure', '            (void)0;            // a failure',
     (SCRIPTS,)),
    ('session-start-keeps-file', COOKIES_C, '        (void)empty_file(k);                // a failure',
     '        (void)0;                // a failure', (SCRIPTS,)),
    ('session-leaves-file', COOKIES_C, '            (void)empty_file(k);\n        return;\n    }\n    if (yonder_cookies_due',
     '            (void)0;\n        return;\n    }\n    if (yonder_cookies_due', (SCRIPTS,)),
    ('folder-said-as-unreadable', COOKIES_C, '    if (entry.flags & OS64_DE_DIR) {\n',
     '    if ((entry.flags & OS64_DE_DIR) && false) {\n', (SCRIPTS,)),
    ('settings-opens-silent', SETTINGS_C, '    if (problem != NULL)\n        os64_ui_settings_report(&s.d, problem);',
     '    if (problem != NULL && false)\n        os64_ui_settings_report(&s.d, problem);', (SCRIPTS,)),
    ('clear-keeps-temps', COOKIES_C, "        return false;\n    yonder_profile_sweep(k->path);\n    way_jar_t *none",
     "        return false;\n    way_jar_t *none", (SCRIPTS,)),
    ('clear-keeps-file', COOKIES_C, '    return empty_file(k) ? had : -1;', '    return had;', (SCRIPTS,)),
    ('write-over-unreadable', COOKIES_C, '        if (!read_file(k, &text, &len))\n            return false;\n        way_jar_t *merged',
     '        (void)read_file(k, &text, &len);\n        way_jar_t *merged', (SCRIPTS,)),
    ('race-not-read-again', COOKIES_C, '    if (check && identity_of(k->path) != read) {',
     '    if (check && read != read + 0 * identity_of(k->path)) {', (SCRIPTS,)),
    ('write-not-followed', COOKIES_C, '            way_jar_follow(k->jar, out, out_len, now, NULL);\n', '', (SCRIPTS,)),
    ('look-never', COOKIES_C, "k->path[0] == '\\0' || identity_of(k->path) == k->identity)",
     "k->path[0] == '\\0' || true)", (SCRIPTS,)),
    ('no-sweep', COOKIES_C, '        yonder_profile_sweep(k->path);\n', '', (SCRIPTS,)),
    ('on-not-read', COOKIES_C, "    if (mode == YONDER_COOKIES_ON && was != YONDER_COOKIES_ON && k->path[0] != '\\0')\n        follow(k);",
     "    (void)was;", (SCRIPTS,)),
    ('flush-nothing', COOKIES_C, '    if (write_now(k))\n        k->saved = through;\n}\n\nvoid yonder_cookies_look',
     '    (void)through;\n}\n\nvoid yonder_cookies_look', (SCRIPTS,)),
    ('schedule-forgets', YONDER_C, '    deadline_offer(&d, yonder_cookies_due(s_cookies), PAST_TIMERS);\n', '', (SCRIPTS,)),
    ('turn-forgets', YONDER_C, '    yonder_cookies_turn(s_cookies, yonder_now_ms());\n', '', (SCRIPTS,)),
    ('settings-swapped', SETTINGS_C, ': s.outlive.checked ? YONDER_COOKIES_ON : YONDER_COOKIES_SESSION;',
     ': s.outlive.checked ? YONDER_COOKIES_SESSION : YONDER_COOKIES_ON;', (SCRIPTS,)),
    ('settings-default-session', SETTINGS_C,
     '    yonder_cookies_mode_t mode = YONDER_COOKIES_ON;\n    if (refused != NULL)',
     '    yonder_cookies_mode_t mode = YONDER_COOKIES_SESSION;\n    if (refused != NULL)', (SCRIPTS,)),
]

caught, missed, unbuilt = [], [], []
for name, rel, old, new, harnesses in mutants:
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
    codes = []
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            for harness in harnesses:
                try:
                    result = subprocess.run(['bash', str(root / harness)], cwd=root, stdout=out, stderr=out,
                                            timeout=900)
                    codes.append(result.returncode)
                except subprocess.TimeoutExpired:
                    codes.append('timeout')
                if codes[-1] != 0:
                    break
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
    elif any(code != 0 for code in codes):
        caught.append(name)
        verdict = 'caught'
    else:
        missed.append(name)
        verdict = 'MISSED'
    print(f'{name}: {verdict}', flush=True)

print(f'\nlogin mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
