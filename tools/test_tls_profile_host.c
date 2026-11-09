#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../userland/libtls/port/client_profile.h"

int main(int argc, char **argv)
{
    (void)argv;
    br_ssl_client_context *c = malloc(sizeof *c);
    br_x509_minimal_context *x = malloc(sizeof *x);
    unsigned char *records = malloc(BR_SSL_BUFSIZE_BIDI);
    assert(c && x && records);
    os64_bearssl_client_profile(c, x, NULL, 0);
    br_ssl_engine_set_buffer(&c->eng, records, BR_SSL_BUFSIZE_BIDI, 1);
    assert(!br_ssl_client_reset(c, "example.test", 0));
    assert(br_ssl_engine_last_error(&c->eng) == BR_ERR_NO_RANDOM);

    os64_bearssl_client_profile(c, x, NULL, 0);
    assert(br_ssl_engine_get_flags(&c->eng) & BR_OPT_NO_RENEGOTIATION);
    br_ssl_engine_set_buffer(&c->eng, records, BR_SSL_BUFSIZE_BIDI, 1);
    const char *protocols[] = {"http/1.1"};
    br_ssl_engine_set_protocol_names(&c->eng, protocols, 1);
    const unsigned char seed[32] = {1, 2, 3, 4}; // Fixture-only deterministic entropy.
    br_ssl_engine_inject_entropy(&c->eng, seed, sizeof seed);
    assert(br_ssl_client_reset(c, "example.test", 0));
    size_t n;
    unsigned char *hello = br_ssl_engine_sendrec_buf(&c->eng, &n);
    assert(hello && n);
    if (argc == 1) assert(fwrite(hello, 1, n, stdout) == n);
    br_ssl_engine_sendrec_ack(&c->eng, n);
    if (argc > 1) {
        // Feed one byte at a time so record and handshake boundaries cross
        // calls. The Python driver supplies a complete controlled ServerHello.
        int ch;
        while ((ch = getchar()) != EOF) {
            unsigned char *in = br_ssl_engine_recvrec_buf(&c->eng, &n);
            if (!in || !n) break;
            *in = (unsigned char)ch;
            br_ssl_engine_recvrec_ack(&c->eng, 1);
        }
        printf("%d\n", br_ssl_engine_last_error(&c->eng));
    }
    free(records);
    free(x);
    free(c);
    return 0;
}
