#include "platform_inputs.h"
#include "os64/date.h"
#include "os64/io.h"
#include "os64/proc.h"     // os64_micros — the clock a session's age is told by

bool os64_tls_entropy(void *out, size_t length)
{
    int64_t opened = os64_open("/dev/random", "r");
    if (opened < 0) return false;
    int32_t handle = (int32_t)opened;
    unsigned char *bytes = out;
    size_t at = 0;
    while (at < length) {
        int64_t n = os64_read(handle, bytes + at, length - at);
        if (n <= 0 || (uint64_t)n > length - at) break;
        at += (size_t)n;
    }
    int64_t closed = os64_close(handle);
    return at == length && closed >= 0;
}

static tls_status entropy(void *context, unsigned char *out, size_t length)
{
    (void)context;
    return os64_tls_entropy(out, length) ? TLS_OK : TLS_ENTROPY_UNAVAILABLE;
}

// A kept session's hold on the store it was earned under, and the clock its
// age and lifetime are told by: microseconds since boot, which only move
// forward and do not jump when the wall clock is set.
static bool trust_retain(const void *trust) { return os64_tls_trust_retain((os64_tls_trust *)trust); }
static void trust_release(const void *trust) { os64_tls_trust_free((os64_tls_trust *)trust); }
static uint64_t now_ms(void *context)
{
    (void)context;
    return (uint64_t)os64_micros() / 1000u;
}

tls_status os64_tls_engine_create_os(const tls_os_config *config, os64_tls_engine **out)
{
    if (!out) return TLS_BAD_ARGUMENT;
    *out = NULL;
    if (!config || !config->trust) return TLS_BAD_ARGUMENT;
    os64_time_t now;
    if (os64_time(&now) < 0) return TLS_BAD_TIME;
    tls_engine_config engine = {
        .hostname = config->hostname, .alpn = config->alpn,
        .alpn_count = config->alpn_count, .epoch = now.epoch,
        .entropy = entropy, .validator = os64_tls_policy_factory(config->trust), .protocol = config->protocol,
        .session = {.port = config->port, .trust = config->trust, .retain = trust_retain,
                    .release = trust_release, .now_ms = now_ms}
    };
    return os64_tls_engine_create(&engine, out);
}
