#include "platform_inputs.h"
#include "os64/date.h"
#include "os64/io.h"

static tls_status entropy(void *context, unsigned char *out, size_t length)
{
    (void)context;
    int64_t opened = os64_open("/dev/random", "r");
    if (opened < 0) return TLS_ENTROPY_UNAVAILABLE;
    int32_t handle = (int32_t)opened;
    size_t at = 0;
    while (at < length) {
        int64_t n = os64_read(handle, out + at, length - at);
        if (n <= 0 || (uint64_t)n > length - at) break;
        at += (size_t)n;
    }
    int64_t closed = os64_close(handle);
    return at == length && closed >= 0 ? TLS_OK : TLS_ENTROPY_UNAVAILABLE;
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
        .entropy = entropy, .validator = os64_tls_policy_factory(config->trust)
    };
    return os64_tls_engine_create(&engine, out);
}
