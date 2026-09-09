#include "os64/fmt.h"
#include "os64/proc.h"
#include "os64/mem.h"
#include "os64/io.h"
#include "os64/str.h"
#include "../../libtls/port/platform_inputs.h"
#include "../../libtls/port/trust_store.h"
#include "../../libtls/test/foundation.h"
#include "../../libtls/test/trust_vectors.h"

static void report(const char *line)
{
    os64_printf("tlsinputtest: %s\n", line);
    os64_serial_log(line);
}
static int64_t read_pem(void *context, void *out, size_t capacity)
{
    size_t *at = context;
    size_t n = tls_fixture_a_pem_length - *at;
    if (n > capacity) n = capacity;
    os64_memcpy(out, tls_fixture_a_pem + *at, n); *at += n; return (int64_t)n;
}
int main(int argc, char **argv)
{
    if (argc == 2 && os64_streq(argv[1], "--license")) {
        os64_printf("%s", bearssl_test_license());
        return 0;
    }
    unsigned char previous[256], hello[256];
    size_t previous_length = 0;
    for (unsigned run = 0; run < 4; run++) {
        os64_tls_trust *trust = NULL; size_t at = 0;
        if (os64_tls_trust_load_pem(read_pem, &at, &trust, NULL) != TLS_STORE_OK) {
            report("FAIL fixture trust snapshot"); return 0x71580001;
        }
        tls_os_config cfg = {.hostname = {"example.test", 12}, .trust = trust};
        os64_tls_engine *client = NULL;
        tls_status status = os64_tls_engine_create_os(&cfg, &client);
        os64_tls_trust_free(trust);
        if (status != TLS_OK) {
            report(status == TLS_ENTROPY_UNAVAILABLE ? "FAIL random service unavailable" :
                status == TLS_BAD_TIME ? "FAIL UTC clock unavailable or outside engine range" : "FAIL client creation");
            return 0x71580002;
        }
        tls_state state = os64_tls_engine_state(client);
        tls_transfer moved = os64_tls_engine_take(client, hello, sizeof hello);
        bool pass = state.status == TLS_OK && (state.flags & TLS_SEND_CIPHER) &&
            !(state.flags & (TLS_HANDSHAKE_DONE | TLS_SEND_PLAIN | TLS_RECV_PLAIN)) &&
            moved.status == TLS_OK && moved.transferred >= 43 && hello[0] == 22 && hello[5] == 1;
        bool same = moved.transferred == previous_length;
        for (size_t i = 0; same && i < moved.transferred; i++) if (hello[i] != previous[i]) same = false;
        if (run && same) pass = false;
        os64_memcpy(previous, hello, moved.transferred); previous_length = moved.transferred;
        if (os64_tls_engine_abort(client, TLS_CANCELLED) != TLS_CANCELLED) pass = false;
        os64_tls_engine_destroy(client);
        if (!pass) { report("FAIL independent ClientHello and cancellation fixture"); return 0x71580003; }
    }
    report("PASS four real-input clients, distinct ClientHellos, retained snapshots and cleanup");
    return 0x71580000;
}
