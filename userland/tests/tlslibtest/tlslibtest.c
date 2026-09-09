#include "tls/tls.h"
#include "os64/fmt.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "../../libtls/test/trust_vectors.h"

static int64_t read_pem(void *context, void *out, size_t capacity)
{
    size_t *at = context;
    size_t n = tls_fixture_a_pem_length - *at;
    if (n > capacity) n = capacity;
    os64_memcpy(out, tls_fixture_a_pem + *at, n); *at += n; return (int64_t)n;
}
static int fail(const char *why)
{
    os64_printf("tlslibtest: FAIL %s\n", why);
    os64_serial_log(why);
    return 0x71590001;
}
int main(int argc, char **argv)
{
    if (argc == 2 && os64_streq(argv[1], "--license")) {
        os64_printf("%s", os64_tls_license()); return 0;
    }
    os64_tls_client *client = (void *)1;
    if (os64_tls_client_create(NULL, &client) != OS64_TLS_BAD_ARGUMENT || client ||
        os64_tls_state(NULL).status != OS64_TLS_BAD_ARGUMENT ||
        !os64_streq(os64_tls_status_name(OS64_TLS_ENTROPY_UNAVAILABLE), "ENTROPY_UNAVAILABLE") ||
        !os64_streq(os64_tls_status_name((os64_tls_status_t)-1), "UNKNOWN") ||
        !os64_tls_license()[0]) return fail("public arguments and diagnostics");
    os64_tls_free(NULL);
    for (unsigned run = 0; run < 3; run++) {
        size_t at = 0;
        os64_tls_trust *trust = NULL;
        os64_tls_store_detail_t detail;
        if (os64_tls_trust_load_pem(read_pem, &at, &trust, &detail) != OS64_TLS_STORE_OK)
            return fail("public PEM snapshot");
        os64_tls_store_report_t report;
        os64_tls_trust *saved = trust;
        if (os64_tls_trust_reload_path(&trust, "relative.pem", &report) != OS64_TLS_STORE_CONFIG || trust != saved) {
            os64_tls_trust_free(trust); return fail("failed reload preserves snapshot");
        }
        os64_tls_config_t cfg = {.hostname = {"example.test", 12}, .trust = trust};
        os64_tls_status_t status = os64_tls_client_create(&cfg, &client);
        os64_tls_trust_free(trust);
        if (status != OS64_TLS_OK) return fail(os64_tls_status_name(status));
        unsigned char hello[512]; size_t n = 0;
        bool pass = os64_tls_state(client).flags & OS64_TLS_SEND_CIPHER;
        pass &= os64_tls_take_ciphertext(client, NULL, 0).transferred == 0;
        pass &= os64_tls_write_plaintext(client, "x", 1).transferred == 0;
        pass &= os64_tls_read_plaintext(client, hello, sizeof hello).transferred == 0;
        while (n < sizeof hello && (os64_tls_state(client).flags & OS64_TLS_SEND_CIPHER)) {
            os64_tls_transfer_t moved = os64_tls_take_ciphertext(client, hello + n, 1);
            if (moved.transferred != 1) { pass = false; break; }
            n++;
        }
        pass &= n >= 43 && hello[0] == 22 && hello[5] == 1;
        pass &= !(os64_tls_state(client).flags & (OS64_TLS_HANDSHAKE_DONE | OS64_TLS_SEND_PLAIN | OS64_TLS_RECV_PLAIN));
        pass &= os64_tls_flush(client) == OS64_TLS_OK;
        os64_tls_status_t terminal;
        if (!run) terminal = os64_tls_abort(client, OS64_TLS_TIMEOUT);
        else if (run == 1) terminal = os64_tls_begin_close(client);
        else terminal = os64_tls_transport_eof(client);
        pass &= terminal == (!run ? OS64_TLS_TIMEOUT : run == 1 ? OS64_TLS_CANCELLED : OS64_TLS_TRUNCATED);
        pass &= os64_tls_abort(client, OS64_TLS_TRANSPORT) == terminal;
        pass &= os64_tls_feed_ciphertext(client, "x", 1).status == terminal;
        pass &= os64_tls_state(client).status == terminal;
        os64_tls_free(client);
        if (!pass) return fail("public byte progress and terminal state");
    }
    os64_printf("tlslibtest: PASS shared API, real inputs, trust ownership and byte operations\n");
    os64_serial_log("PASS shared API, real inputs, trust ownership and byte operations");
    return 0x71590000;
}
