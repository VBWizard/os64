#include "tls_engine_fixture.h"
#include "../userland/libtls/upstream/samples/key-ec.h"
#include "../userland/libtls/upstream/samples/chain-ec.h"

void tls_fixture_ec(br_ssl_server_context *server, br_x509_decoder_context *decoder)
{
    if (server) br_ssl_server_init_full_ec(server, CHAIN, CHAIN_LEN, BR_KEYTYPE_EC, &EC);
    if (decoder) {
        br_x509_decoder_init(decoder, NULL, NULL);
        br_x509_decoder_push(decoder, CERT0, sizeof CERT0);
    }
}
