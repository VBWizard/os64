#include "trust_store.h"
#include "os64/conf.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/slurp.h"
#include "os64/str.h"

static tls_store_status path_copy(char *out, const char *path)
{
    if (!path || *path != '/') return TLS_STORE_CONFIG;
    size_t n = 0;
    while (path[n]) {
        if (n == TLS_STORE_PATH_MAX - 1) return TLS_STORE_LIMIT;
        if ((unsigned char)path[n] < 32 || (unsigned char)path[n] == 127) return TLS_STORE_CONFIG;
        n++;
    }
    os64_memcpy(out, path, n + 1);
    return TLS_STORE_OK;
}
static int64_t read_file(void *context, void *data, size_t capacity)
{
    return os64_read(*(int32_t *)context, data, capacity);
}
static tls_store_status load(os64_tls_trust **current, tls_store_report *report)
{
    report->config_stage = false;
    int64_t opened = os64_open(report->bundle_path, "r");
    if (opened < 0) return report->detail.status = TLS_STORE_OPEN;
    int32_t handle = (int32_t)opened;
    os64_tls_trust *fresh = NULL;
    tls_store_status status = os64_tls_trust_load_pem(read_file, &handle, &fresh, &report->detail);
    if (os64_close(handle) < 0 && status == TLS_STORE_OK) status = TLS_STORE_IO;
    if (status == TLS_STORE_OK) {
        os64_tls_trust *old = *current;
        *current = fresh;
        os64_tls_trust_free(old);
    } else os64_tls_trust_free(fresh);
    return report->detail.status = status;
}
tls_store_status os64_tls_trust_reload_path(os64_tls_trust **current,
                                           const char *path, tls_store_report *report)
{
    // A caller may reuse the selected path from its previous report.
    char selected[TLS_STORE_PATH_MAX];
    tls_store_status status = path_copy(selected, path);
    tls_store_report local;
    if (!report) report = &local;
    os64_memset(report, 0, sizeof *report);
    if (!current) return report->detail.status = TLS_STORE_BAD_ARGUMENT;
    if (status != TLS_STORE_OK) return report->detail.status = status;
    os64_memcpy(report->bundle_path, selected, os64_strlen(selected) + 1);
    return load(current, report);
}
tls_store_status os64_tls_trust_reload(os64_tls_trust **current, tls_store_report *report)
{
    tls_store_report local;
    if (!report) report = &local;
    os64_memset(report, 0, sizeof *report);
    report->config_stage = true;
    if (!current) return report->detail.status = TLS_STORE_BAD_ARGUMENT;
    int64_t resolved = os64_conf_find("tls.conf", report->config_path, sizeof report->config_path);
    uint8_t *config = NULL;
    size_t length = 0;
    if (resolved == 0) {
        os64_slurp_status_t result = os64_slurp(report->config_path, TLS_STORE_CONFIG_MAX, &config, &length);
        if (result != OS64_SLURP_OK) {
            tls_store_status status = result == OS64_SLURP_NO_MEMORY ? TLS_STORE_NO_MEMORY :
                result == OS64_SLURP_TOO_BIG ? TLS_STORE_LIMIT :
                result == OS64_SLURP_NO_FILE ? TLS_STORE_OPEN : TLS_STORE_IO;
            return report->detail.status = status;
        }
    } else if (resolved == OS64_CONF_NO_FILE) report->config_path[0] = 0;
    else return report->detail.status = TLS_STORE_IO;
    tls_store_status status = os64_tls_trust_config_parse(config, length,
        report->bundle_path, sizeof report->bundle_path, &report->detail);
    os64_free(config);
    if (status != TLS_STORE_OK) return status;
    return load(current, report);
}
