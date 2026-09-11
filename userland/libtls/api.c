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
os64_tls_status_t os64_tls_input_eof(os64_tls_client *c) { return os64_tls_engine_eof((os64_tls_engine *)c); }
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

const char *os64_tls_error_description(os64_tls_status_t status,
                                      os64_tls_policy_reason_t policy, int error)
{
    // Local policy is more specific than the validator's generic trust error.
    if (status == OS64_TLS_CERTIFICATE || status == OS64_TLS_LIMIT || status == OS64_TLS_PROTOCOL) {
        switch (policy) {
        case OS64_TLS_POLICY_DER: return "malformed or unsupported certificate encoding";
        case OS64_TLS_POLICY_LIMIT: return "certificate resource limit exceeded";
        case OS64_TLS_POLICY_DUPLICATE: return "duplicate certificate extension or value";
        case OS64_TLS_POLICY_EXTENSION: return "unsupported certificate extension or restriction";
        case OS64_TLS_POLICY_CRITICAL: return "unsupported critical certificate extension";
        case OS64_TLS_POLICY_SAN: return "certificate DNS names are invalid, unsupported or do not match";
        case OS64_TLS_POLICY_EKU: return "certificate is not permitted for TLS server authentication";
        case OS64_TLS_POLICY_CA: return "certificate does not satisfy its CA or leaf role";
        case OS64_TLS_POLICY_KEY_USAGE: return "certificate key usage does not permit signing";
        case OS64_TLS_POLICY_KEY: return "certificate public key is invalid, weak or unsupported";
        case OS64_TLS_POLICY_SIGNATURE: return "certificate signature algorithm is unsupported or inconsistent";
        case OS64_TLS_POLICY_ANCHOR: return "certificate cannot be used as an installed trust anchor";
        case OS64_TLS_POLICY_SEQUENCE: return "invalid certificate validation sequence";
        case OS64_TLS_POLICY_OK: break;
        }
        // Received alerts occupy one byte above the receive base. Sent alerts
        // have a different base and must not be attributed to the server.
        if (error >= BR_ERR_RECV_FATAL_ALERT && error < BR_ERR_RECV_FATAL_ALERT + 256) {
            switch (error - BR_ERR_RECV_FATAL_ALERT) {
            case BR_ALERT_CLOSE_NOTIFY: return "server sent a fatal alert: close_notify";
            case BR_ALERT_UNEXPECTED_MESSAGE: return "server sent a fatal alert: unexpected_message";
            case BR_ALERT_BAD_RECORD_MAC: return "server sent a fatal alert: bad_record_mac";
            case BR_ALERT_RECORD_OVERFLOW: return "server sent a fatal alert: record_overflow";
            case BR_ALERT_DECOMPRESSION_FAILURE: return "server sent a fatal alert: decompression_failure";
            case BR_ALERT_HANDSHAKE_FAILURE: return "server sent a fatal alert: handshake_failure";
            case BR_ALERT_BAD_CERTIFICATE: return "server sent a fatal alert: bad_certificate";
            case BR_ALERT_UNSUPPORTED_CERTIFICATE: return "server sent a fatal alert: unsupported_certificate";
            case BR_ALERT_CERTIFICATE_REVOKED: return "server sent a fatal alert: certificate_revoked";
            case BR_ALERT_CERTIFICATE_EXPIRED: return "server sent a fatal alert: certificate_expired";
            case BR_ALERT_CERTIFICATE_UNKNOWN: return "server sent a fatal alert: certificate_unknown";
            case BR_ALERT_ILLEGAL_PARAMETER: return "server sent a fatal alert: illegal_parameter";
            case BR_ALERT_UNKNOWN_CA: return "server sent a fatal alert: unknown_ca";
            case BR_ALERT_ACCESS_DENIED: return "server sent a fatal alert: access_denied";
            case BR_ALERT_DECODE_ERROR: return "server sent a fatal alert: decode_error";
            case BR_ALERT_DECRYPT_ERROR: return "server sent a fatal alert: decrypt_error";
            case BR_ALERT_PROTOCOL_VERSION: return "server rejected the TLS version (fatal alert: protocol_version; this client supports TLS 1.2)";
            case BR_ALERT_INSUFFICIENT_SECURITY: return "server sent a fatal alert: insufficient_security";
            case BR_ALERT_INTERNAL_ERROR: return "server sent a fatal alert: internal_error";
            case BR_ALERT_USER_CANCELED: return "server sent a fatal alert: user_canceled";
            case BR_ALERT_NO_RENEGOTIATION: return "server sent a fatal alert: no_renegotiation";
            case BR_ALERT_UNSUPPORTED_EXTENSION: return "server sent a fatal alert: unsupported_extension";
            case BR_ALERT_NO_APPLICATION_PROTOCOL: return "server sent a fatal alert: no_application_protocol";
            default: return "server sent an unrecognized fatal TLS alert";
            }
        }
        switch (error) {
        case BR_ERR_X509_NOT_TRUSTED: return "certificate chain does not reach an installed trust anchor";
        case BR_ERR_X509_EXPIRED: return "certificate has expired or is not yet valid; check the system clock";
        case BR_ERR_X509_BAD_SERVER_NAME: return "certificate does not match the requested hostname";
        case BR_ERR_X509_DN_MISMATCH: return "certificate issuer does not match the next CA";
        case BR_ERR_X509_BAD_SIGNATURE: return "certificate signature verification failed";
        case BR_ERR_X509_UNSUPPORTED: return "unsupported certificate algorithm or feature";
        case BR_ERR_X509_CRITICAL_EXTENSION: return "unsupported critical certificate extension";
        case BR_ERR_X509_NOT_CA: return "certificate issuer is not a CA";
        case BR_ERR_X509_FORBIDDEN_KEY_USAGE: return "certificate key usage forbids this operation";
        case BR_ERR_X509_WEAK_PUBLIC_KEY: return "certificate public key is too weak";
        case BR_ERR_X509_LIMIT_EXCEEDED: return "certificate resource limit exceeded";
        case BR_ERR_X509_TIME_UNKNOWN: return "certificate validation time is unavailable";
        case BR_ERR_X509_EMPTY_CHAIN: return "server supplied no certificate chain";
        case BR_ERR_UNSUPPORTED_VERSION:
        case BR_ERR_BAD_VERSION: return "unsupported TLS version; this client supports TLS 1.2";
        case BR_ERR_BAD_CIPHER_SUITE: return "server selected an unsupported TLS cipher suite";
        case BR_ERR_BAD_MAC: return "TLS record authentication failed";
        case BR_ERR_BAD_SIGNATURE: return "TLS handshake signature verification failed";
        }
        if (error >= BR_ERR_SEND_FATAL_ALERT && error < BR_ERR_SEND_FATAL_ALERT + 256)
            return "client sent a fatal TLS alert";
    }
    switch (status) {
    case OS64_TLS_CERTIFICATE: return "certificate validation failed";
    case OS64_TLS_PROTOCOL: return "TLS protocol error";
    case OS64_TLS_LIMIT: return "TLS resource limit exceeded";
    case OS64_TLS_BAD_ARGUMENT: return "invalid TLS configuration or argument";
    case OS64_TLS_NO_MEMORY: return "not enough memory for TLS";
    case OS64_TLS_ENTROPY_UNAVAILABLE: return "secure randomness is unavailable";
    case OS64_TLS_BAD_TIME: return "system clock is unavailable or invalid";
    case OS64_TLS_UNSUPPORTED: return "unsupported TLS option or server identity";
    case OS64_TLS_TRUNCATED: return "connection ended without a complete TLS close";
    case OS64_TLS_TRANSPORT: return "network I/O failed";
    case OS64_TLS_TIMEOUT: return "TLS operation timed out";
    case OS64_TLS_CANCELLED: return "TLS operation cancelled";
    case OS64_TLS_OK:
    case OS64_TLS_NEED_PROGRESS:
    case OS64_TLS_CLEAN_EOF: return "no TLS error";
    }
    return "unknown TLS error";
}
