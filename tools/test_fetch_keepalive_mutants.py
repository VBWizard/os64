#!/usr/bin/env python3
"""One deliberate break at a time to keep-alive and the presets' asking
(docs/design/pending/WIRE.md § 2), against the harnesses that hold each
rule: a mutant is CAUGHT when its harness fails, MISSED when it passes,
which means a test is missing and gets written. A build failure is neither.

Each mutant is applied to the worktree's copy of its file, the harness is
run, and the file is put back from the bytes read before the change —
checked byte for byte — whatever happens. Nothing else is touched. Run it
from a clean tree, one run at a time, or through tools/mutants_parallel.py.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/fetch-keepalive-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

FETCH_C = 'userland/libfetch/fetch.c'
HTTP_C = 'userland/libfetch/http.c'
POOL_C = 'userland/libfetch/pool.c'
TRANSPORT_C = 'userland/libfetch/transport.c'
ORIGIN_C = 'userland/libway/origin.c'
LOAD_C = 'userland/libway/load.c'
JOB_C = 'userland/apps/yonder/fetch_job.c'
AGENT_C = 'userland/apps/yonder/agent.c'
FETCH = 'tools/test_fetch_host.sh'
POOL = 'tools/test_fetch_pool_host.sh'
WAYO = 'tools/test_way_origin_host.sh'
WAYF = 'tools/test_way_fetch_host.sh'
JOB = 'tools/test_yonder_fetch_job_host.sh'
YONDER = 'tools/test_yonder_host.sh'

# (name, file, old, new, harness). Each breaks one rule the design states;
# none supplies a replacement expectation.
mutants = [
    # connection_reusable, clause by clause (RFC 9112 § 9.3)
    ('rule-length-framed', POOL_C,
     'return (f->framing == HTTP_FRAMING_LENGTH || f->framing == HTTP_FRAMING_CHUNKED) && f->consumed;',
     'return (f->framing == HTTP_FRAMING_CHUNKED) && f->consumed;', FETCH),
    ('rule-chunked', POOL_C,
     'return (f->framing == HTTP_FRAMING_LENGTH || f->framing == HTTP_FRAMING_CHUNKED) && f->consumed;',
     'return (f->framing == HTTP_FRAMING_LENGTH) && f->consumed;', FETCH),
    ('rule-read-to-close-kept', POOL_C,
     'return (f->framing == HTTP_FRAMING_LENGTH || f->framing == HTTP_FRAMING_CHUNKED) && f->consumed;',
     'return f->consumed;', FETCH),
    ('rule-bodiless', POOL_C, '    if (f->bodiless)\n        return true;', '    if (false)\n        return true;', FETCH),
    ('rule-response-close', POOL_C,
     'if (f->request_close || f->response_close || !f->request_whole)',
     'if (f->request_close || !f->request_whole)', FETCH),
    ('rule-request-close', POOL_C,
     'if (f->request_close || f->response_close || !f->request_whole)',
     'if (f->response_close || !f->request_whole)', FETCH),
    ('rule-request-whole', POOL_C,
     'if (f->request_close || f->response_close || !f->request_whole)',
     'if (f->request_close || f->response_close)', FETCH),
    ('rule-response-http11', POOL_C,
     'if (!f->request_http11 || !f->response_http11)', 'if (!f->request_http11)', FETCH),
    ('rule-transport', POOL_C,
     'if (!f->transport_ok || f->leftover)', 'if (f->leftover)', FETCH),
    ('rule-leftover', POOL_C,
     'if (!f->transport_ok || f->leftover)', 'if (!f->transport_ok)', FETCH),
    # The facts the rule is handed
    # (`bodiless` itself has no mutant here: body_prepare already frames a
    # HEAD's answer, a 204 and a 304 as empty, so `consumed` holds for them
    # with or without it; rule-bodiless proves the clause in the rule.)
    ('facts-abandoned-body', FETCH_C,
     '        x->consumed = !f->wire_unread &&',
     '        x->consumed = true || !f->wire_unread ||', FETCH),
    ('facts-205-unread', FETCH_C,
     '        f->wire_unread = f->reply.status == 205 &&', '        f->wire_unread = false &&', FETCH),
    ('facts-leftover', FETCH_C,
     '    x->leftover = f->stream.next < f->stream.have;', '    x->leftover = false;', FETCH),
    ('facts-cancelled', FETCH_C,
     "                      !(f->over && f->status != OS64_FETCH_OK) && !cancelled(f) &&",
     "                      !(f->over && f->status != OS64_FETCH_OK) &&", FETCH),
    ('facts-tls-transport', FETCH_C,
     "!cancelled(f) &&\n                      fetch_transport_reusable(&f->io);",
     "!cancelled(f);", FETCH),
    ('facts-sent-whole', FETCH_C,
     '    f->sent_whole = head_sent == os64_strlen(request) &&',
     '    f->sent_whole = true || head_sent == os64_strlen(request) ||', FETCH),
    ('redirect-closes-instead', FETCH_C,
     '        release_connection(f, false);\n        f->have_head = false;',
     '        fetch_transport_close(&f->io, false);\n        f->connected = false;\n        f->have_head = false;', FETCH),
    # What a reply says
    ('reply-http10-is-11', HTTP_C,
     "line[7] >= '1' && line[7] <= '9'", "line[7] >= '0' && line[7] <= '9'", FETCH),
    ('reply-connection-close', HTTP_C,
     "                to_lower(value[from + 4]) == 'e')\n                out->connectionClose = true;",
     "                to_lower(value[from + 4]) == 'e')\n                (void)out;", FETCH),
    ('reply-connection-unreadable', HTTP_C,
     '    if (os64_streq_nocase(name, "Connection"))\n        out->connectionClose = true;', '    (void)out;', FETCH),
    # The probe at the take, and adoption
    ('probe-plain-trusts', TRANSPORT_C,
     'return os64_read_for(io->handle, &probe, 1, 0) == OS64_ERR_TIMEOUT;',
     '(void)probe;\n        return true;', FETCH),
    ('adopt-received-kept', TRANSPORT_C, '    io->received = 0;\n', '', FETCH),
    # The pool
    ('pool-never-expires', POOL_C,
     'return idle >= (uint64_t)FETCH_POOL_IDLE_MS * now->per_second / 1000u;', 'return false && idle;', FETCH),
    ('pool-per-key-cap', POOL_C, 'if (mine >= FETCH_POOL_PER_KEY) {', 'if (mine > FETCH_POOL_PER_KEY) {', FETCH),
    ('pool-evicts-newest', POOL_C,
     'if (oldest == NULL || k->kept_at < oldest->kept_at)', 'if (oldest == NULL || k->kept_at > oldest->kept_at)', FETCH),
    ('pool-takes-oldest', POOL_C,
     '(best == NULL || k->kept_at > best->kept_at)', '(best == NULL || k->kept_at < best->kept_at)', FETCH),
    ('key-trust', POOL_C, '           a->trust == b->trust && os64_streq(a->host, b->host);',
     '           os64_streq(a->host, b->host);', FETCH),
    ('key-tls', POOL_C, 'return a->tls == b->tls && a->via_proxy == b->via_proxy',
     'return a->via_proxy == b->via_proxy', FETCH),
    ('key-proxy-is-origin', FETCH_C,
     'os64_strcopy(k->host, sizeof(k->host), f->proxy.inUse ? f->proxy.host : f->current.host);',
     'os64_strcopy(k->host, sizeof(k->host), f->current.host);', FETCH),
    ('pool-take-unlocked', POOL_C,
     '    kept_t *best = NULL;\n    os64_lock_acquire(&s_lock);', '    kept_t *best = NULL;', POOL),
    ('pool-evicted-not-closed', POOL_C,
     '    for (size_t i = 0; i < out->n; i++)\n        fetch_transport_close(&out->io[i], false);',
     '    (void)out;', POOL),
    ('pool-keeps-cancellation', POOL_C,
     '    free_slot->io.cancelled = NULL;\n    free_slot->io.cancel_ctx = NULL;\n', '', POOL),
    # The retry rule
    ('retry-never', FETCH_C,
     'return f->reused && f->io.received == 0 && !f->io.silent && !cancelled(f) && idempotent(f);',
     'return false && idempotent(f);', FETCH),
    ('retry-any-method', FETCH_C,
     'return f->reused && f->io.received == 0 && !f->io.silent && !cancelled(f) && idempotent(f);',
     'return f->reused && f->io.received == 0 && !f->io.silent && !cancelled(f) && (idempotent(f) || true);',
     FETCH),
    ('retry-fresh-too', FETCH_C,
     'return f->reused && f->io.received == 0 && !f->io.silent && !cancelled(f) && idempotent(f);',
     'return f->io.received == 0 && !f->io.silent && !cancelled(f) && idempotent(f);', FETCH),
    ('retry-after-bytes', FETCH_C,
     'return f->reused && f->io.received == 0 && !f->io.silent && !cancelled(f) && idempotent(f);',
     'return f->reused && !f->io.silent && !cancelled(f) && idempotent(f);', FETCH),
    ('retry-silence', FETCH_C,
     'return f->reused && f->io.received == 0 && !f->io.silent && !cancelled(f) && idempotent(f);',
     'return f->reused && f->io.received == 0 && !cancelled(f) && idempotent(f);', FETCH),
    ('retry-post-listed', FETCH_C,
     'static const char *const kOnce[] = {"GET", "HEAD", "OPTIONS", "PUT", "DELETE"};',
     'static const char *const kOnce[] = {"GET", "HEAD", "OPTIONS", "PUT", "DELETE", "POST"};', FETCH),
    ('retry-unsaid', FETCH_C,
     '" (retried once on a fresh connection)");', '"%s", "");', FETCH),
    # The request writer
    ('writer-no-keep-alive', HTTP_C,
     'case 3: if (!x->keep_alive) continue;', 'case 3: if (true) continue;', FETCH),
    ('writer-order-ignored', HTTP_C,
     '    const char *order = extras->order != NULL ? extras->order : "";',
     '    const char *order = "";', FETCH),
    ('writer-order-repeats', HTTP_C,
     'if (len == 0 || name_listed(order, name, name, len) ||',
     'if (len == 0 ||', FETCH),
    ('writer-tail-repeats', HTTP_C,
     'if (!name_listed(order, NULL, line.name, line.name_len) && !head_line_put(out, cap, &at, &line))',
     'if (!head_line_put(out, cap, &at, &line))', FETCH),
    ('writer-order-unjudged', HTTP_C,
     '    if (!order_clean(extras->order))\n        return false;', '    (void)order_clean;', FETCH),
    # libway and the fetch job
    ('way-order-no-cors-as-cors', ORIGIN_C,
     'return m->mode == WAY_MODE_NO_CORS ? WAY_ORDER_NO_CORS : WAY_ORDER_CORS;', 'return WAY_ORDER_CORS;', WAYO),
    ('way-order-preflight-as-request', ORIGIN_C,
     '    if (m->preflight)\n        return WAY_ORDER_PREFLIGHT;\n', '', WAYO),
    ('way-keep-alive-ignored', ORIGIN_C, '    return id != NULL && id->keep_alive;', '    return false && id;', WAYO),
    ('way-hooks-accept', LOAD_C, '    if (accept != NULL)\n        opt->accept = accept;', '    (void)accept;', WAYF),
    ('way-hooks-order', LOAD_C,
     '    opt->order = way_identity_order(hooks->identity, &hooks->metadata);', '', WAYF),
    ('way-navigation-accept', LOAD_C,
     '    if (preset != NULL) {\n        opt.accept = preset;', '    if (false) {\n        opt.accept = preset;', WAYF),
    ('way-navigation-order', LOAD_C,
     '    opt.order = way_identity_order(s->identity, &s->metadata);', '', WAYF),
    ('job-preflight-order', JOB_C,
     '    way_metadata_t m = h->preflight ? way_metadata_preflight(&h->metadata) : h->metadata;\n    opt->order',
     '    way_metadata_t m = h->metadata;\n    opt->order', JOB),
    ('job-fetch-accept', JOB_C,
     '        if (fields->accepts == 0 && preset != NULL)', '        if (false && preset != NULL)', JOB),
    # The table
    ('table-chrome-webp', AGENT_C,
     '[WAY_DEST_IMAGE] = "image/*,*/*;q=0.8"', '[WAY_DEST_IMAGE] = "image/webp,image/*,*/*;q=0.8"', YONDER),
    ('table-chrome-order', AGENT_C,
     '"Connection, Content-Length, sec-ch-ua-platform, User-Agent, sec-ch-ua, sec-ch-ua-mobile, Accept, "',
     '"Connection, Content-Length, User-Agent, sec-ch-ua-platform, sec-ch-ua, sec-ch-ua-mobile, Accept, "', YONDER),
    ('table-firefox-cors-connection', AGENT_C,
     '"User-Agent, Accept, Accept-Language, Accept-Encoding, Referer, Origin, Connection, Sec-Fetch-Dest, "',
     '"User-Agent, Accept, Accept-Language, Accept-Encoding, Connection, Referer, Origin, Sec-Fetch-Dest, "',
     YONDER),
    ('table-chrome-silent', AGENT_C,
     '.reload_cache_control = "max-age=0", .keep_alive = true,', '.reload_cache_control = "max-age=0",', YONDER),
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
                result = subprocess.run(['bash', str(root / harness)], cwd=root, stdout=out, stderr=out, timeout=900)
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

print(f'\nkeep-alive mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
