#include "tls/transport.h"
#include "os64/os64.h"
#include "../../libtls/test/trust_vectors.h"

static int64_t pem(void *context, void *out, size_t capacity)
{
    size_t *at = context, n = tls_fixture_a_pem_length - *at;
    if (n > capacity) n = capacity;
    os64_memcpy(out, tls_fixture_a_pem + *at, n); *at += n; return (int64_t)n;
}
static unsigned char pattern(size_t n) { return (unsigned char)(n * 13u); }
int main(int argc, char **argv)
{
    if (argc != 4) {
        os64_printf("usage: tlstransportprobe HOST PORT good|stall|truncated|badname\n"); return 1;
    }
    bool stall = os64_streq(argv[3], "stall"), truncated = os64_streq(argv[3], "truncated");
    bool badname = os64_streq(argv[3], "badname"), pass = false;
    if (!stall && !truncated && !badname && !os64_streq(argv[3], "good")) return 1;
    char address[128]; os64_snprintf(address, sizeof address, "tcp!%s!%s", argv[1], argv[2]);
    int64_t handle = os64_dial(address);
    if (handle < 0) return 2;
    os64_tls_trust *trust = NULL; os64_tls_transport *transport = NULL;
    size_t at = 0; os64_tls_store_detail_t detail;
    os64_tls_status_t status = OS64_TLS_BAD_ARGUMENT;
    if (os64_tls_trust_load_pem(pem, &at, &trust, &detail) != OS64_TLS_STORE_OK) goto done;
    os64_tls_config_t cfg = {.hostname = badname ? (os64_tls_name_t){"wrong.test",10} : (os64_tls_name_t){"example.test",12}, .trust = trust};
    os64_tls_transport_limits_t limits = {stall ? 200 : 10000, 2000};
    os64_ticks_t clock;
    if (os64_ticks(&clock) < 0 || !clock.per_second) goto done;
    uint64_t started = clock.ticks;
    status = os64_tls_transport_create(&cfg, (int32_t)handle, &limits, &transport);
    os64_tls_trust_free(trust); trust = NULL;
    if (status != OS64_TLS_OK) goto done;
    handle = -1;
    unsigned char bytes[257]; size_t sent = 0, received = 0;
    const size_t goal = 65536;
    while (os64_ticks(&clock) == 0 && clock.ticks - started < 20u * clock.per_second) {
        os64_tls_state_t state = os64_tls_transport_state(transport);
        status = state.status;
        if (status != OS64_TLS_OK) break;
        if ((state.flags & OS64_TLS_HANDSHAKE_DONE) && !stall && !badname) {
            if (sent < goal) {
                size_t n = goal - sent; if (n > sizeof bytes) n = sizeof bytes;
                for (size_t i = 0; i < n; i++) bytes[i] = pattern(sent + i);
                os64_tls_transfer_t moved = os64_tls_transport_write(transport, bytes, n);
                sent += moved.transferred;
                if (moved.status != OS64_TLS_OK && moved.status != OS64_TLS_NEED_PROGRESS) { status = moved.status; break; }
                os64_tls_transport_flush(transport);
            }
            os64_tls_transfer_t moved = os64_tls_transport_read(transport, bytes, sizeof bytes);
            for (size_t i = 0; i < moved.transferred; i++) if (bytes[i] != pattern(received + i)) goto done;
            received += moved.transferred;
            if (received > goal) goto done;
            if (moved.status != OS64_TLS_OK && moved.status != OS64_TLS_NEED_PROGRESS) { status = moved.status; break; }
            if (sent == goal && received == goal) os64_tls_transport_begin_close(transport);
        }
        status = os64_tls_transport_step(transport, 10);
        if (status != OS64_TLS_OK && status != OS64_TLS_NEED_PROGRESS) break;
    }
    pass = stall ? status == OS64_TLS_TIMEOUT : badname ? status == OS64_TLS_CERTIFICATE :
           truncated ? status == OS64_TLS_TRUNCATED : status == OS64_TLS_CLEAN_EOF && sent == goal && received == goal;
    if (stall) pass &= os64_ticks(&clock) == 0 && clock.ticks - started >= clock.per_second / 5 &&
                       clock.ticks - started <= 2u * clock.per_second;
done:
    os64_tls_trust_free(trust); os64_tls_transport_free(transport);
    if (handle >= 0) os64_close((int32_t)handle);
    os64_printf("tlstransportprobe: %s %s (%s)\n", pass ? "PASS" : "FAIL", argv[3], os64_tls_status_name(status));
    char report[96]; os64_snprintf(report, sizeof report, "TLSTRANSPORT %s %s (%s)",
        pass ? "PASS" : "FAIL", argv[3], os64_tls_status_name(status));
    os64_serial_log(report);
    return pass ? 0 : 3;
}
