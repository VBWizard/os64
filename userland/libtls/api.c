#include "port/platform_inputs.h"
#include "tls_license.h"

os64_tls_status_t os64_tls_client_create(const os64_tls_config_t *config, os64_tls_client **out)
{
    if (!out) return OS64_TLS_BAD_ARGUMENT;
    *out = NULL;
    if (!config) return OS64_TLS_BAD_ARGUMENT;
    tls_os_config inputs = {.hostname = config->hostname, .alpn = config->alpn,
        .alpn_count = config->alpn_count, .trust = config->trust};
    os64_tls_engine *engine = NULL;
    tls_status status = os64_tls_engine_create_os(&inputs, &engine);
    *out = (os64_tls_client *)engine;
    return status;
}
void os64_tls_free(os64_tls_client *c) { os64_tls_engine_destroy((os64_tls_engine *)c); }
os64_tls_state_t os64_tls_state(os64_tls_client *c) { return os64_tls_engine_state((os64_tls_engine *)c); }
os64_tls_transfer_t os64_tls_feed_ciphertext(os64_tls_client *c, const void *p, size_t n)
{ return os64_tls_engine_feed((os64_tls_engine *)c, p, n); }
os64_tls_transfer_t os64_tls_take_ciphertext(os64_tls_client *c, void *p, size_t n)
{ return os64_tls_engine_take((os64_tls_engine *)c, p, n); }
os64_tls_transfer_t os64_tls_write_plaintext(os64_tls_client *c, const void *p, size_t n)
{ return os64_tls_engine_write((os64_tls_engine *)c, p, n); }
os64_tls_transfer_t os64_tls_read_plaintext(os64_tls_client *c, void *p, size_t n)
{ return os64_tls_engine_read((os64_tls_engine *)c, p, n); }
os64_tls_status_t os64_tls_flush(os64_tls_client *c) { return os64_tls_engine_flush((os64_tls_engine *)c); }
os64_tls_status_t os64_tls_begin_close(os64_tls_client *c) { return os64_tls_engine_close((os64_tls_engine *)c); }
os64_tls_status_t os64_tls_transport_eof(os64_tls_client *c) { return os64_tls_engine_eof((os64_tls_engine *)c); }
os64_tls_status_t os64_tls_abort(os64_tls_client *c, os64_tls_status_t reason)
{ return os64_tls_engine_abort((os64_tls_engine *)c, reason); }
const char *os64_tls_license(void) { return tls_license; }
const char *os64_tls_status_name(os64_tls_status_t status)
{
    switch (status) {
    case OS64_TLS_OK: return "OK";
    case OS64_TLS_NEED_PROGRESS: return "NEED_PROGRESS";
    case OS64_TLS_CLEAN_EOF: return "CLEAN_EOF";
    case OS64_TLS_BAD_ARGUMENT: return "BAD_ARGUMENT";
    case OS64_TLS_NO_MEMORY: return "NO_MEMORY";
    case OS64_TLS_LIMIT: return "LIMIT";
    case OS64_TLS_ENTROPY_UNAVAILABLE: return "ENTROPY_UNAVAILABLE";
    case OS64_TLS_BAD_TIME: return "BAD_TIME";
    case OS64_TLS_CERTIFICATE: return "CERTIFICATE";
    case OS64_TLS_UNSUPPORTED: return "UNSUPPORTED";
    case OS64_TLS_PROTOCOL: return "PROTOCOL";
    case OS64_TLS_TRUNCATED: return "TRUNCATED";
    case OS64_TLS_TRANSPORT: return "TRANSPORT";
    case OS64_TLS_TIMEOUT: return "TIMEOUT";
    case OS64_TLS_CANCELLED: return "CANCELLED";
    }
    return "UNKNOWN";
}
