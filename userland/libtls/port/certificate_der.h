#ifndef OS64_TLS_CERTIFICATE_DER_H
#define OS64_TLS_CERTIFICATE_DER_H
#include "certificate_policy.h"

typedef struct { const unsigned char *data; size_t length; } tls_der_span;
typedef struct {
    tls_der_span subject;
    br_x509_pkey key; // Borrows bytes from the input DER.
} tls_certificate_view;
// role: 0 leaf, 1 intermediate, 2 administratively selected anchor.
tls_policy_reason os64_tls_certificate_inspect(const void *der, size_t length,
    unsigned role, const char *hostname, tls_certificate_view *view);
#endif
