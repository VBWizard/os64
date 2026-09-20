#include "font_config_internal.h"
#include "os64/conf.h"
#include "os64/io.h"
#include "os64/proc.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "os64/fmt.h"
#include "os64/signal.h"

static os64_font_config_status_t install(os64_text_context_t *context,
    const os64_font_config_t *config, os64_font_role_t role,
    char out[OS64_FONT_PATH_CAP], os64_font_config_error_t *error)
{
    if (error) *error = (os64_font_config_error_t){.role = role};
    if (!context || !config || !out || role >= OS64_FONT_ROLE_COUNT)
        return OS64_FONT_CONFIG_PATH;
    char valid[4096];
    if (os64_font_config_encode(config, valid, sizeof(valid)) < 0) return OS64_FONT_CONFIG_PATH;
    const char *source = config->roles[role].face[0];
    if (source[0] != '/') return OS64_FONT_CONFIG_PATH;
    const char *name = source;
    for (const char *p = source; *p; ++p) if (*p == '/') name = p + 1;
    if (!*name || *name == '.') return OS64_FONT_CONFIG_PATH;
    char folder[OS64_FONT_PATH_CAP], path[OS64_FONT_PATH_CAP], temp[OS64_FONT_PATH_CAP];
    if (os64_conf_target("fonts", folder, sizeof(folder)) < 0) return OS64_FONT_CONFIG_IO;
    int n = os64_snprintf(path, sizeof(path), "%s/%s", folder, name);
    if (n < 0 || (size_t)n >= sizeof(path)) return OS64_FONT_CONFIG_PATH;
    static uint64_t sequence;
    uint64_t seq = __atomic_fetch_add(&sequence, 1, __ATOMIC_RELAXED);
    n = os64_snprintf(temp, sizeof(temp), "%s/.%lu.%lu.install", folder,
                      (unsigned long)os64_taskid(), (unsigned long)seq);
    if (n < 0 || (size_t)n >= sizeof(temp)) return OS64_FONT_CONFIG_PATH;
    os64_dirent_t entry;
    if (os64_stat(path, &entry) == 0) return OS64_FONT_CONFIG_EXISTS;
    size_t remaining = OS64_FONT_FILE_MAX, length;
    uint8_t *bytes = NULL;
    os64_font_config_status_t status = font_source_read(source, &remaining, &bytes, &length);
    if (status) return status;
    (void)os64_mkdir(folder);
    int64_t fd = os64_open(temp, "w");
    if (fd < 0) { os64_free(bytes); return OS64_FONT_CONFIG_IO; }
    size_t written = 0;
    while (written < length) {
        int64_t took = os64_write((int32_t)fd, bytes + written, length - written);
        if (took == OS64_INTERRUPTED) continue;
        if (took <= 0 || (uint64_t)took > length - written) { status = OS64_FONT_CONFIG_IO; break; }
        written += (size_t)took;
    }
    if (!status && os64_sync((int32_t)fd) != 0) status = OS64_FONT_CONFIG_IO;
    os64_close((int32_t)fd);
    os64_free(bytes);
    if (!status) {
        os64_font_config_t candidate = *config;
        os64_strcopy(candidate.roles[role].face[0], OS64_FONT_PATH_CAP, temp);
        os64_font_set_t *set = NULL;
        status = os64_font_config_prepare(context, &candidate, &set, error);
        os64_font_set_release(set);
    }
    if (!status && os64_rename_with_flags(temp, path, OS64_RENAME_NOREPLACE) != 0)
        status = os64_stat(path, &entry) == 0 ? OS64_FONT_CONFIG_EXISTS : OS64_FONT_CONFIG_IO;
    if (status) os64_unlink(temp);
    else os64_strcopy(out, OS64_FONT_PATH_CAP, path);
    if (error && !error->status) error->status = status;
    return status;
}

/* Keep the diagnostic coherent for early I/O/path refusals as well as provider
 * failures, which carry their more specific role/source details. */
os64_font_config_status_t os64_font_config_install(os64_text_context_t *context,
    const os64_font_config_t *config, os64_font_role_t role,
    char out[OS64_FONT_PATH_CAP], os64_font_config_error_t *error)
{
    os64_font_config_status_t status = install(context, config, role, out, error);
    if (error) error->status = status;
    return status;
}
