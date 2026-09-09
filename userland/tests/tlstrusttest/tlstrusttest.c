#include "os64/io.h"
#include "os64/fmt.h"
#include "os64/proc.h"
#include "os64/str.h"
#include "../../libtls/test/trust_test.h"
#include "../../libtls/test/trust_vectors.h"

static void report(const char *line)
{
    os64_printf("tlstrusttest: %s\n", line);
    os64_serial_log(line);
}
static bool write_file(const char *path, const char *bytes, size_t length)
{
    int64_t fd = os64_open(path, "w");
    if (fd < 0) return false;
    size_t at = 0;
    while (at < length) {
        int64_t n = os64_write((int32_t)fd, bytes + at, length - at);
        if (n <= 0 || (uint64_t)n > length - at) break;
        at += (size_t)n;
    }
    int64_t closed = os64_close((int32_t)fd);
    return at == length && closed >= 0;
}
int main(void)
{
    if (tls_trust_selftest(report)) return 0x71570001;
    char directory[128], path[TLS_STORE_PATH_MAX];
    os64_snprintf(directory, sizeof directory, "/tmp/tlstrust-%lu", os64_taskid());
    os64_snprintf(path, sizeof path, "%s/roots.pem", directory);
    if (os64_mkdir(directory) < 0) { report("FAIL creating isolated fixture directory"); return 0x71570002; }
    os64_tls_trust *store = NULL;
    const br_x509_class **old = NULL;
    tls_validator_factory factory = os64_tls_policy_factory(NULL);
    tls_store_report diagnostic;
    bool pass = false;
    if (!write_file(path, tls_fixture_a_pem, tls_fixture_a_pem_length) ||
        os64_tls_trust_reload_path(&store, path, &diagnostic) != TLS_STORE_OK ||
        !tls_trust_test_verify(store, false) || tls_trust_test_verify(store, true)) goto done;
    factory = os64_tls_policy_factory(store);
    if (factory.create(store, "example.test", 740232, 0, &old) != TLS_OK) goto done;
    if (!write_file(path, tls_fixture_b_pem, tls_fixture_b_pem_length) ||
        os64_tls_trust_reload_path(&store, path, &diagnostic) != TLS_STORE_OK ||
        !tls_trust_test_verify(store, true) || tls_trust_test_verify(store, false)) goto done;
    (*old)->start_chain(old, "example.test");
    (*old)->start_cert(old, (uint32_t)tls_fixture_a_leaf_length);
    (*old)->append(old, tls_fixture_a_leaf, tls_fixture_a_leaf_length);
    (*old)->end_cert(old);
    if ((*old)->end_chain(old) || !(*old)->get_pkey(old, NULL)) goto done;
    os64_tls_trust *saved = store;
    if (!write_file(path, "invalid bundle\n", 15) ||
        os64_tls_trust_reload_path(&store, path, &diagnostic) != TLS_STORE_FORMAT ||
        store != saved || !tls_trust_test_verify(store, true)) goto done;
    pass = true;
done:
    factory.destroy(old);
    os64_tls_trust_free(store);
    if (os64_unlink(path) < 0) pass = false;
    if (os64_unlink(directory) < 0) pass = false;
    report(pass ? "PASS trust file replacement, old validator lifetime and failed-reload preservation" : "FAIL trust file replacement fixture");
    return pass ? 0x71570000 : 0x71570003;
}
