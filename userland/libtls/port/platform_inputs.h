#ifndef OS64_TLS_PLATFORM_INPUTS_H
#define OS64_TLS_PLATFORM_INPUTS_H
#include "certificate_policy.h"

typedef struct {
    tls_name hostname;
    const tls_name *alpn;
    size_t alpn_count;
    os64_tls_trust *trust;
} tls_os_config;

// Private constructor using /dev/random, UTC time and the certificate policy.
// Inputs and sealed trust are borrowed during the call; the engine retains
// its own snapshot reference. Failure sets *out=NULL. No transport is opened.
tls_status os64_tls_engine_create_os(const tls_os_config *config, os64_tls_engine **out);
#endif
