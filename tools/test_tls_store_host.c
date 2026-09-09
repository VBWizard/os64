#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../userland/libtls/test/trust_test.h"
#include "../userland/libtls/test/trust_vectors.h"
#include "os64/conf.h"
#include "os64/slurp.h"

static size_t allocated, freed, calls, fail_allocation;
void *os64_malloc(size_t n)
{
    if (++calls == fail_allocation) return NULL;
    void *p = malloc(n); assert(p); allocated++; return p;
}
void os64_free(void *p) { if (p) { freed++; free(p); } }

typedef struct {
    const char *path, *bytes;
    size_t length, at, fail_at, fragment;
    bool live, refuse_open, fail_close;
    unsigned opens, closes;
} fake_file;
static fake_file files[3];
static bool config_found;
int64_t os64_conf_find(const char *name, char *path, size_t capacity)
{
    assert(!strcmp(name, "tls.conf") && capacity == TLS_STORE_PATH_MAX);
    if (!config_found) { path[0] = 0; return OS64_CONF_NO_FILE; }
    strcpy(path, files[0].path); return 0;
}
int64_t os64_open(const char *path, const char *mode)
{
    assert(!strcmp(mode, "r"));
    for (size_t i = 0; i < 3; i++) {
        fake_file *f = &files[i];
        if (strcmp(path, f->path)) continue;
        f->opens++;
        if (f->refuse_open) return -1;
        assert(!f->live); f->live = true; f->at = 0; return (int64_t)i + 3;
    }
    return -1;
}
int64_t os64_read(int32_t handle, void *buffer, size_t capacity)
{
    assert(handle >= 3 && handle <= 5);
    fake_file *f = &files[handle - 3]; assert(f->live);
    if (f->at >= f->fail_at) return -1;
    size_t n = f->length - f->at;
    if (n > capacity) n = capacity;
    if (n > f->fragment) n = f->fragment;
    if (n > f->fail_at - f->at) n = f->fail_at - f->at;
    memcpy(buffer, f->bytes + f->at, n); f->at += n; return (int64_t)n;
}
int64_t os64_close(int32_t handle)
{
    assert(handle >= 3 && handle <= 5);
    fake_file *f = &files[handle - 3]; assert(f->live);
    f->live = false; f->closes++; return f->fail_close ? -1 : 0;
}
static void reset(void)
{
    for (size_t i = 0; i < 3; i++) assert(!files[i].live);
    static const char config[] = "trust_store = /home/roots.pem\n";
    files[0] = (fake_file){.path="/home/tls.conf", .bytes=config, .length=sizeof config-1, .fail_at=SIZE_MAX, .fragment=3};
    files[1] = (fake_file){.path=TLS_STORE_DEFAULT, .bytes=tls_fixture_a_pem, .length=tls_fixture_a_pem_length, .fail_at=SIZE_MAX, .fragment=7};
    files[2] = (fake_file){.path="/home/roots.pem", .bytes=tls_fixture_b_pem, .length=tls_fixture_b_pem_length, .fail_at=SIZE_MAX, .fragment=1};
    config_found = false;
}
static const br_x509_class **retain_validator(os64_tls_trust *store)
{
    tls_validator_factory f = os64_tls_policy_factory(store);
    const br_x509_class **v;
    assert(f.create(store, "example.test", 740232, 0, &v) == TLS_OK);
    return v;
}
static void retained_root_a(const br_x509_class **v)
{
    (*v)->start_chain(v, "example.test");
    (*v)->start_cert(v, (uint32_t)tls_fixture_a_leaf_length);
    (*v)->append(v, tls_fixture_a_leaf, tls_fixture_a_leaf_length);
    (*v)->end_cert(v);
    assert(!(*v)->end_chain(v) && (*v)->get_pkey(v, NULL));
    os64_tls_policy_factory(NULL).destroy(v);
}
static void reloads(void)
{
    reset();
    os64_tls_trust *current = NULL;
    tls_store_report report;
    assert(os64_tls_trust_reload(&current, &report) == TLS_STORE_OK);
    assert(!report.config_path[0] && !strcmp(report.bundle_path, TLS_STORE_DEFAULT));
    assert(tls_trust_test_verify(current, false) && !tls_trust_test_verify(current, true));
    const br_x509_class **old = retain_validator(current);
    config_found = true;
    assert(os64_tls_trust_reload(&current, &report) == TLS_STORE_OK);
    assert(!strcmp(report.config_path, "/home/tls.conf") && !strcmp(report.bundle_path, "/home/roots.pem"));
    assert(tls_trust_test_verify(current, true) && !tls_trust_test_verify(current, false));
    retained_root_a(old);
    os64_tls_trust *saved = current;
    files[2].refuse_open = true;
    unsigned defaults_opened = files[1].opens;
    assert(os64_tls_trust_reload(&current, &report) == TLS_STORE_OPEN && current == saved);
    assert(files[1].opens == defaults_opened && !report.config_stage);
    files[2].refuse_open = false;
    files[2].fail_at = files[2].length;
    assert(os64_tls_trust_reload(&current, &report) == TLS_STORE_IO && current == saved);
    assert(!files[2].live && files[1].opens == defaults_opened);
    files[2].fail_at = SIZE_MAX; files[2].fail_close = true;
    assert(os64_tls_trust_reload(&current, &report) == TLS_STORE_IO && current == saved);
    files[2].fail_close = false;
    char *bad = malloc(files[2].length + 1); assert(bad);
    memcpy(bad, files[2].bytes, files[2].length); bad[files[2].length] = '!';
    files[2].bytes = bad; files[2].length++;
    assert(os64_tls_trust_reload(&current, &report) == TLS_STORE_FORMAT && current == saved);
    free(bad); reset(); config_found = true;
    files[0].refuse_open = true;
    assert(os64_tls_trust_reload(&current, &report) == TLS_STORE_OPEN && current == saved && report.config_stage);
    assert(!files[1].opens && !files[2].opens);
    files[0].refuse_open = false; files[0].fail_at = files[0].length;
    assert(os64_tls_trust_reload(&current, &report) == TLS_STORE_IO && current == saved && report.config_stage);
    files[0].fail_at = SIZE_MAX;
    static const char hidden[] = "trust_store=/home/roots.pem\0trust_store=bad";
    files[0].bytes = hidden; files[0].length = sizeof hidden - 1;
    assert(os64_tls_trust_reload(&current, &report) == TLS_STORE_CONFIG && current == saved && report.config_stage);
    assert(!files[1].opens && !files[2].opens);
    files[0].bytes = "# no setting\n"; files[0].length = strlen(files[0].bytes);
    assert(os64_tls_trust_reload(&current, &report) == TLS_STORE_OK);
    assert(tls_trust_test_verify(current, false));
    saved = current;
    assert(os64_tls_trust_reload_path(&current, "relative", &report) == TLS_STORE_CONFIG && current == saved);
    assert(os64_tls_trust_reload_path(&current, "/home/roots.pem", &report) == TLS_STORE_OK);
    assert(!report.config_path[0] && tls_trust_test_verify(current, true) && !tls_trust_test_verify(current, false));
    assert(os64_tls_trust_reload_path(&current, report.bundle_path, &report) == TLS_STORE_OK);
    os64_tls_trust_free(current);
    puts("PASS trust file/config selection, no fallback, replacement, old references and failure preservation");
}
static void allocations(void)
{
    reset();
    os64_tls_trust *current = NULL;
    assert(os64_tls_trust_reload(&current, NULL) == TLS_STORE_OK);
    os64_tls_trust *saved = current;
    size_t count = calls;
    config_found = true;
    assert(os64_tls_trust_reload(&current, NULL) == TLS_STORE_OK);
    count = calls - count;
    os64_tls_trust_free(current);
    for (size_t i = 1; i <= count; i++) {
        reset();
        current = NULL;
        assert(os64_tls_trust_reload(&current, NULL) == TLS_STORE_OK);
        saved = current;
        config_found = true; fail_allocation = calls + i;
        assert(os64_tls_trust_reload(&current, NULL) == TLS_STORE_NO_MEMORY && current == saved);
        fail_allocation = 0;
        assert(tls_trust_test_verify(current, false));
        os64_tls_trust_free(current);
    }
    puts("PASS trust reload allocation failures preserve the previous store and close owned handles");
}
static void report(const char *line) { puts(line); }
int main(void)
{
    assert(!tls_trust_selftest(report));
    reloads(); allocations(); reset();
    assert(allocated == freed);
    puts("TLS trust-store host tests PASS");
    return 0;
}
