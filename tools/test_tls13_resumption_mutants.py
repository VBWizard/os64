#!/usr/bin/env python3
"""One deliberate break at a time to TLS 1.3 session resumption
(docs/design/completed/TLS13.md § Resumption), against the engine harness
that holds each rule: a mutant is CAUGHT when the harness fails, MISSED when
it passes, which means a test is missing and gets written. A build failure
is neither.

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
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/tls13-resumption-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

HANDSHAKE_C = 'userland/libtls/port/tls13_handshake.c'
CACHE_C = 'userland/libtls/port/session_cache.c'
CLIENT_C = 'userland/libtls/port/client_engine.c'
ENGINE = 'tools/test_tls13_engine_host.py'

# (name, file, old, new). Each breaks one rule the design states; none
# supplies a replacement expectation.
mutants = [
    # The hello
    ('hello-says-nothing', HANDSHAKE_C, 'if (e->res.advertise) { x=extension(&w,45);', 'if (false) { x=extension(&w,45);'),
    ('hello-age-unmasked', HANDSHAKE_C, 'uint32_t age=s->age_ms+s->age_add;', 'uint32_t age=s->age_ms;'),
    ('binder-over-the-binders-length', HANDSHAKE_C,
     'br_multihash_update(&partial,hello_bytes,truncated);', 'br_multihash_update(&partial,hello_bytes,truncated+2);'),
    ('binder-key-label', HANDSHAKE_C, '"res binder"', '"ext binder"'),
    ('binder-from-no-psk', HANDSHAKE_C,
     'tls13_extract(suite,zeros,e->res.offer->psk,suite->hash_len,early);', 'tls13_extract(suite,zeros,zeros,suite->hash_len,early);'),
    ('retry-keeps-an-offer-of-another-hash', HANDSHAKE_C,
     'if (!suite || (e->suite && e->suite->hash_len!=suite->hash_len)) return NULL;', 'if (!suite) return NULL;'),
    # The answer
    ('accepts-an-identity-not-offered', HANDSHAKE_C,
     'if (identity || !issued || issued->hash_len!=suite->hash_len) return TLS13_ERR_PSK;',
     'if (identity>1 || !issued || issued->hash_len!=suite->hash_len) return TLS13_ERR_PSK;'),
    ('accepts-another-hash', HANDSHAKE_C,
     'if (identity || !issued || issued->hash_len!=suite->hash_len) return TLS13_ERR_PSK;',
     'if (identity || !issued) return TLS13_ERR_PSK;'),
    ('accepts-what-it-did-not-offer', HANDSHAKE_C, '} else if (type==41 && !retry && e->offered) {', '} else if (type==41 && !retry) {'),
    ('early-secret-without-the-psk', HANDSHAKE_C,
     'tls13_extract(suite,zeros,psk?psk:zeros,suite->hash_len,secret);', 'tls13_extract(suite,zeros,zeros,suite->hash_len,secret);'),
    ('resumed-still-waits-for-a-certificate', HANDSHAKE_C,
     'e->phase=e->resumed?WAIT_FINISHED:WAIT_CERTIFICATE;', 'e->phase=WAIT_CERTIFICATE;'),
    ('resumed-never-said', HANDSHAKE_C, 'e->resumed=psk!=NULL;', 'e->resumed=false;'),
    # Tickets
    ('resumption-secret-label', HANDSHAKE_C, '"res master"', '"exp master"'),
    ('ticket-psk-label', HANDSHAKE_C, '"resumption",e->ticket_nonce', '"resumptioX",e->ticket_nonce'),
    ('ticket-psk-without-its-nonce', HANDSHAKE_C, 'e->ticket_nonce,e->nonce_len,', 'e->ticket_nonce,0,'),
    ('ticket-nonce-copied-to-its-start', HANDSHAKE_C,
     'os64_memcpy(e->ticket_nonce+(e->nonce_len-e->stream_left),p+at,count);', 'os64_memcpy(e->ticket_nonce,p+at,count);'),
    ('ticket-copied-to-its-start', HANDSHAKE_C,
     'os64_memcpy(e->res.arriving->ticket+(e->ticket_len-e->stream_left),p+at,count);',
     'os64_memcpy(e->res.arriving->ticket,p+at,count);'),
    ('ticket-of-any-length-kept', HANDSHAKE_C, 'e->res.arriving && n<=TLS13_TICKET_MAX;', 'e->res.arriving && n<=65535;'),
    ('ticket-of-lifetime-zero-kept', HANDSHAKE_C, 'if (s && e->ticket_keep && e->ticket_lifetime) {', 'if (s && e->ticket_keep) {'),
    ('ticket-lifetime-not-held-to-seven-days', HANDSHAKE_C,
     'uint32_t lifetime=e->ticket_lifetime<TLS13_TICKET_LIFETIME_MAX?e->ticket_lifetime:TLS13_TICKET_LIFETIME_MAX;',
     'uint32_t lifetime=e->ticket_lifetime;'),
    ('ticket-age-add-lost', HANDSHAKE_C, 's->age_add=e->ticket_age_add;', 's->age_add=0;'),
    # The cache
    ('cache-offers-twice', CACHE_C, '        strike(&gone, newest);\n        found = true;', '        found = true;'),
    ('cache-key-without-port', CACHE_C, 'return e->used && e->port == key->port && e->trust == key->trust &&', 'return e->used && e->trust == key->trust &&'),
    ('cache-key-without-trust', CACHE_C, 'return e->used && e->port == key->port && e->trust == key->trust &&', 'return e->used && e->port == key->port &&'),
    ('cache-key-without-host', CACHE_C,
     'e->trust == key->trust &&\n           os64_streq(e->hostname, key->hostname);', 'e->trust == key->trust;'),
    ('cache-three-a-key', CACHE_C, 'if (of_key >= TLS_SESSION_PER_KEY)', 'if (of_key > TLS_SESSION_PER_KEY)'),
    ('cache-puts-out-the-newest', CACHE_C, 'if (!oldest || e->serial < oldest->serial) oldest = e;', 'if (!oldest || e->serial > oldest->serial) oldest = e;'),
    ('cache-puts-out-the-keys-newest', CACHE_C,
     'if (!oldest_of_key || e->serial < oldest_of_key->serial) oldest_of_key = e;',
     'if (!oldest_of_key || e->serial > oldest_of_key->serial) oldest_of_key = e;'),
    ('cache-takes-the-oldest', CACHE_C, 'if (!newest || e->serial > newest->serial) newest = e;', 'if (!newest || e->serial < newest->serial) newest = e;'),
    ('cache-lifetime-one-second-long', CACHE_C, '(now_ms - e->kept_at) / 1000u >= e->lifetime;', '(now_ms - e->kept_at) / 1000u > e->lifetime;'),
    ('cache-keeps-without-its-hold', CACHE_C, 'if (key->retain && !key->retain(key->trust)) return;', 'if (key->retain) key->retain(key->trust);'),
    ('cache-never-lets-a-hold-go', CACHE_C,
     'if (e->release) { out->trust[out->count] = e->trust; out->release[out->count++] = e->release; }', '(void)out;'),
    ('cache-age-not-told', CACHE_C, 'out->age_ms = (uint32_t)(now_ms - newest->kept_at);', 'out->age_ms = 0;'),
    ('cache-keeps-lifetime-zero', CACHE_C, "if (!key || !key->hostname || !session || !lifetime ||", "if (!key || !key->hostname || !session ||"),
    # The wrapper
    ('no-port-resumes-anyway', CLIENT_C, 'if (cfg->session.port && cfg->session.now_ms) {', 'if (cfg->session.now_ms) {'),
    ('resumed-flag-never-raised', CLIENT_C, 'result.flags |= OS64_TLS_RESUMED;', 'result.flags |= 0;'),
    ('tickets-never-filed', CLIENT_C,
     'os64_tls_session_put(&key, session, lifetime, c->session.now_ms(c->session.context));',
     '(void)key; (void)session; (void)lifetime;'),
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
                result = subprocess.run(['python3', str(root / ENGINE)], cwd=root, stdout=out, stderr=out, timeout=900)
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

print(f'\nTLS 1.3 resumption mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
