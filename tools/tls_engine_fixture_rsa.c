#include "tls_engine_fixture.h"
#include "../userland/libtls/upstream/samples/key-rsa.h"
#include "../userland/libtls/upstream/samples/chain-rsa.h"

void tls_fixture_rsa(br_ssl_server_context *server, br_x509_decoder_context *decoder)
{
    if (server) br_ssl_server_init_full_rsa(server, CHAIN, CHAIN_LEN, &RSA);
    if (decoder) {
        br_x509_decoder_init(decoder, NULL, NULL);
        br_x509_decoder_push(decoder, CERT0, sizeof CERT0);
    }
}
