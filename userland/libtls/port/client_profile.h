#ifndef OS64_BEARSSL_CLIENT_PROFILE_H
#define OS64_BEARSSL_CLIENT_PROFILE_H
#include "bearssl.h"

// Configure engine algorithms; caller attaches its certificate validator.
void os64_bearssl_client_algorithms(br_ssl_client_context *client);

// Private initializer. Caller owns both contexts and keeps the anchor data
// alive. Buffers, entropy, validation time, hostname, and ALPN are supplied
// separately. This configures algorithms; certificate policy is not complete.
void os64_bearssl_client_profile(br_ssl_client_context *client,
                                br_x509_minimal_context *x509,
                                const br_x509_trust_anchor *anchors,
                                size_t anchor_count);
#endif
