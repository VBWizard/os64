#ifndef TLS_ENGINE_FIXTURE_H
#define TLS_ENGINE_FIXTURE_H
#include "bearssl.h"
void tls_fixture_ec(br_ssl_server_context *, br_x509_decoder_context *);
void tls_fixture_rsa(br_ssl_server_context *, br_x509_decoder_context *);
#endif
