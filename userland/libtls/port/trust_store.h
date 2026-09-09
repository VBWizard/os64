#ifndef OS64_TLS_TRUST_STORE_H
#define OS64_TLS_TRUST_STORE_H
#include "certificate_policy.h"

#define TLS_STORE_PATH_MAX 256u
#define TLS_STORE_PEM_MAX (2u * 1024u * 1024u)
#define TLS_STORE_LINE_MAX 1024u
#define TLS_STORE_CONFIG_MAX 8191u
#define TLS_STORE_DEFAULT "/etc/certs/roots.pem"
typedef enum {
    TLS_STORE_OK, TLS_STORE_BAD_ARGUMENT, TLS_STORE_NO_MEMORY, TLS_STORE_LIMIT,
    TLS_STORE_CONFIG, TLS_STORE_FORMAT, TLS_STORE_CERTIFICATE,
    TLS_STORE_OPEN, TLS_STORE_IO
} tls_store_status;
typedef struct {
    tls_store_status status;
    tls_policy_reason policy_reason;
    size_t line, certificates;
} tls_store_detail;
typedef struct {
    // True means failure occurred while selecting/reading/parsing config.
    bool config_stage;
    char config_path[TLS_STORE_PATH_MAX], bundle_path[TLS_STORE_PATH_MAX];
    tls_store_detail detail;
} tls_store_report;

// Synchronous borrowed reader: 0 is EOF, negative is an error, positive is
// at most capacity bytes. No callback reentry or retained destination pointer.
typedef int64_t (*tls_store_reader)(void *context, void *data, size_t capacity);
// Produces a fresh sealed snapshot on complete success; failure sets *out=NULL.
tls_store_status os64_tls_trust_load_pem(tls_store_reader read, void *context,
                                       os64_tls_trust **out, tls_store_detail *detail);
tls_store_status os64_tls_trust_config_parse(const void *text, size_t length,
                                            char *path, size_t capacity,
                                            tls_store_detail *detail);
// Caller serializes reload and factory acquisition and owns *current (or NULL).
// Failure leaves *current unchanged. The explicit-path form bypasses config
// lookup but applies the same absolute-path and whole-bundle rules.
tls_store_status os64_tls_trust_reload(os64_tls_trust **current, tls_store_report *report);
tls_store_status os64_tls_trust_reload_path(os64_tls_trust **current,
                                           const char *path, tls_store_report *report);
const char *os64_tls_store_status_name(tls_store_status status);
#endif
