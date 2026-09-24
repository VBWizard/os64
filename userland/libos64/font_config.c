#include "os64/font_config.h"
#include "os64/conf.h"
#include "os64/slurp.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "os64/fmt.h"
#include "os64/io.h"
#include "font_config_internal.h"

static const char *const role_names[] = { "ui", "terminal", "document" };

static os64_font_config_status_t problem(os64_font_config_error_t *e,
    os64_font_config_status_t status, size_t line, size_t role, size_t source)
{
    if (e) *e = (os64_font_config_error_t){status, OS64_FONT_OK, line, source,
                                         (os64_font_role_t)role};
    return status;
}

void os64_font_config_defaults(os64_font_config_t *c)
{
    if (!c) return;
    *c = (os64_font_config_t){0};
    for (size_t r = 0; r < OS64_FONT_ROLE_COUNT; ++r) {
        os64_strcopy(c->roles[r].face[0], OS64_FONT_PATH_CAP, "builtin");
        c->roles[r].size = 16;
    }
}

static size_t path_length(const char *s)
{
    size_t n = 0;
    if (s) while (n < OS64_FONT_PATH_CAP && s[n]) ++n;
    return n;
}

/* Canonical separators make duplicate checks independent of redundant '/'.
 * Dot components are refused, so resolution never consults process cwd. */
static bool resolve_path(const char *base, const char *value, char *out)
{
    size_t n = path_length(value), used = 0;
    if (!n || n == OS64_FONT_PATH_CAP) return false;
    if (value[0] == ' ' || value[n - 1] == ' ') return false;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char)value[i] < 32 || value[i] == '#') return false;
    if (os64_streq(value, "builtin")) {
        os64_strcopy(out, OS64_FONT_PATH_CAP, "builtin");
        return true;
    }
    if (value[0] != '/') {
        size_t b = path_length(base);
        if (!b || b == OS64_FONT_PATH_CAP || base[0] != '/') return false;
        while (b && base[b - 1] != '/') --b;
        if (b + n >= OS64_FONT_PATH_CAP) return false;
        os64_memcpy(out, base, b);
        used = b;
    } else out[used++] = '/';
    size_t i = value[0] == '/' ? 1 : 0;
    bool component = false;
    while (i < n) {
        if (value[i] == '/') { ++i; continue; }
        size_t start = i;
        while (i < n && value[i] != '/') ++i;
        size_t len = i - start;
        if ((len == 1 && value[start] == '.') ||
            (len == 2 && value[start] == '.' && value[start + 1] == '.')) return false;
        if (used && out[used - 1] != '/') {
            if (used + 1 >= OS64_FONT_PATH_CAP) return false;
            out[used++] = '/';
        }
        if (len >= OS64_FONT_PATH_CAP - used) return false;
        os64_memcpy(out + used, value + start, len);
        used += len;
        component = true;
    }
    if (!component || value[n - 1] == '/') return false;
    out[used] = 0;
    return true;
}

static os64_font_config_status_t validate(os64_font_config_t *c,
    os64_font_config_error_t *e)
{
    for (size_t r = 0; r < OS64_FONT_ROLE_COUNT; ++r) {
        os64_font_config_role_t *role = &c->roles[r];
        for (size_t s = 0; s < 3; ++s)
            if (path_length(role->face[s]) == OS64_FONT_PATH_CAP)
                return problem(e, OS64_FONT_CONFIG_PATH, role->source_line[s], r, s);
        if (role->size < 8 || role->size > 96 ||
            (os64_streq(role->face[0], "builtin") && role->size != 16))
            return problem(e, OS64_FONT_CONFIG_SIZE, role->size_line, r, 0);
        for (size_t s = 0; s < 3; ++s) {
            if (s && !role->face[s][0]) continue;
            char path[OS64_FONT_PATH_CAP];
            if (!resolve_path(NULL, role->face[s], path))
                return problem(e, OS64_FONT_CONFIG_PATH, role->source_line[s], r, s);
            os64_strcopy(role->face[s], sizeof(role->face[s]), path);
            for (size_t prev = 0; prev < s; ++prev)
                if (os64_streq(role->face[s], role->face[prev]))
                    return problem(e, OS64_FONT_CONFIG_DUPLICATE,
                                   role->source_line[s], r, s);
        }
    }
    return OS64_FONT_CONFIG_OK;
}

typedef struct {
    os64_font_config_t *config;
    os64_font_config_error_t *error;
    size_t line;
    bool delivered;
    os64_font_config_status_t status;
} decode_t;

static bool setting(const char *key, const char *value, void *user)
{
    decode_t *d = user;
    d->delivered = true;
    for (size_t r = 0; key && r < OS64_FONT_ROLE_COUNT; ++r) {
        const char *const suffix[] = {"face", "fallback.1", "fallback.2", "size"};
        for (size_t s = 0; s < 4; ++s) {
            char name[32];
            os64_snprintf(name, sizeof(name), "%s.%s", role_names[r], suffix[s]);
            if (!os64_streq_nocase(name, key)) continue;
            os64_font_config_role_t *role = &d->config->roles[r];
            if (s == 3) {
                uint32_t size = 0;
                bool valid = value[0] != 0;
                for (size_t i = 0; value[i]; ++i) {
                    if (value[i] < '0' || value[i] > '9' || size > 96) {
                        valid = false; break;
                    }
                    size = size * 10 + (uint32_t)(value[i] - '0');
                }
                if (!valid || size < 8 || size > 96)
                    d->status = problem(d->error, OS64_FONT_CONFIG_SIZE, d->line, r, 0);
                else { role->size = size; role->size_line = d->line; }
            } else {
                if (!resolve_path(d->config->path, value, role->face[s]))
                    d->status = problem(d->error, OS64_FONT_CONFIG_PATH, d->line, r, s);
                else role->source_line[s] = d->line;
            }
            return false;
        }
    }
    d->status = problem(d->error, OS64_FONT_CONFIG_SYNTAX, d->line,
                        OS64_FONT_ROLE_COUNT, 0);
    return false;
}

os64_font_config_status_t os64_font_config_decode(const char *text, size_t len,
    const char *path, os64_font_config_t *out, os64_font_config_error_t *e)
{
    problem(e, OS64_FONT_CONFIG_OK, 0, OS64_FONT_ROLE_COUNT, 0);
    if (!text || !out) return problem(e, OS64_FONT_CONFIG_SYNTAX, 0, OS64_FONT_ROLE_COUNT, 0);
    if (len > OS64_FONT_CONFIG_BYTES_MAX)
        return problem(e, OS64_FONT_CONFIG_LIMIT, 0, OS64_FONT_ROLE_COUNT, 0);
    os64_font_config_t c;
    os64_font_config_defaults(&c);
    if (!path || path[0] != '/' || !resolve_path(NULL, path, c.path))
        return problem(e, OS64_FONT_CONFIG_PATH, 0, OS64_FONT_ROLE_COUNT, 0);
    decode_t d = { .config = &c, .error = e };
    size_t start = 0;
    while (start < len) {
        ++d.line;
        size_t end = start;
        while (end < len && text[end] != '\n') ++end;
        size_t first = start;
        while (first < end && (text[first] == ' ' || text[first] == '\t' || text[first] == '\r')) ++first;
        d.delivered = false;
        int64_t parsed = os64_conf_parse(text + start, end - start, setting, &d);
        if (parsed < 0)
            return problem(e, parsed == OS64_CONF_NO_MEMORY ? OS64_FONT_CONFIG_NO_MEMORY :
                           OS64_FONT_CONFIG_SYNTAX, d.line, OS64_FONT_ROLE_COUNT, 0);
        if (d.status) return d.status;
        /* conf's callback omits empty keys as well as blank lines. An '='
         * without a key is malformed input, not an absent font choice. */
        if (!d.delivered && first < end && text[first] != '#')
            return problem(e, OS64_FONT_CONFIG_SYNTAX, d.line, OS64_FONT_ROLE_COUNT, 0);
        start = end + (end < len);
    }
    os64_font_config_status_t status = validate(&c, e);
    if (!status) *out = c;
    return status;
}

static os64_font_config_status_t read_error(os64_slurp_status_t s)
{
    if (s == OS64_SLURP_TOO_BIG) return OS64_FONT_CONFIG_LIMIT;
    if (s == OS64_SLURP_NO_MEMORY) return OS64_FONT_CONFIG_NO_MEMORY;
    return OS64_FONT_CONFIG_IO;
}

os64_font_config_status_t font_source_read(const char *path, size_t *remaining,
                                          uint8_t **bytes, size_t *length)
{
    *bytes = NULL; *length = 0;
    os64_dirent_t entry;
    if (os64_stat(path, &entry) < 0 || (entry.flags & OS64_DE_DIR))
        return OS64_FONT_CONFIG_IO;
    if (entry.size > OS64_FONT_FILE_MAX || entry.size > *remaining)
        return OS64_FONT_CONFIG_LIMIT;
    *remaining -= (size_t)entry.size;
    os64_slurp_status_t read = os64_slurp(path, (size_t)entry.size, bytes, length);
    if (read) return read_error(read);
    return OS64_FONT_CONFIG_OK;
}

os64_font_config_status_t os64_font_config_read(os64_font_config_t *out,
    os64_font_config_error_t *e)
{
    problem(e, OS64_FONT_CONFIG_OK, 0, OS64_FONT_ROLE_COUNT, 0);
    if (!out) return problem(e, OS64_FONT_CONFIG_SYNTAX, 0, OS64_FONT_ROLE_COUNT, 0);
    char path[OS64_FONT_PATH_CAP];
    if (os64_conf_find("fonts.conf", path, sizeof(path)) < 0) {
        os64_font_config_defaults(out);
        return OS64_FONT_CONFIG_OK;
    }
    uint8_t *bytes = NULL;
    size_t len = 0;
    os64_slurp_status_t s = os64_slurp(path, OS64_FONT_CONFIG_BYTES_MAX, &bytes, &len);
    if (s) return problem(e, read_error(s), 0, OS64_FONT_ROLE_COUNT, 0);
    os64_font_config_status_t status = os64_font_config_decode((const char *)bytes, len, path, out, e);
    os64_free(bytes);
    return status;
}

os64_font_config_status_t os64_font_config_prepare(os64_text_context_t *context,
    const os64_font_config_t *config, os64_font_set_t **out, os64_font_config_error_t *e)
{
    problem(e, OS64_FONT_CONFIG_OK, 0, OS64_FONT_ROLE_COUNT, 0);
    if (out) *out = NULL;
    if (!config || !context || !out)
        return problem(e, OS64_FONT_CONFIG_SYNTAX, 0, OS64_FONT_ROLE_COUNT, 0);
    os64_font_config_t c = *config;
    os64_font_config_status_t status = validate(&c, e);
    if (status) return status;
    os64_font_role_spec_t specs[OS64_FONT_ROLE_COUNT] = {0};
    struct { const char *path; uint8_t *bytes; size_t length; } files[9] = {0};
    size_t count = 0, remaining = OS64_FONT_SOURCE_BYTES_MAX;
    size_t indices[OS64_FONT_ROLE_COUNT][3] = {{0}};
    for (size_t r = 0; r < OS64_FONT_ROLE_COUNT; ++r) {
        specs[r].pixel_height = c.roles[r].size;
        for (size_t s = 0; s < 3; ++s) {
            const char *path = c.roles[r].face[s];
            if (s && !path[0]) continue;
            size_t index = s ? ++specs[r].fallback_count : 0;
            indices[r][index] = s;
            os64_font_source_t *source = index ? &specs[r].fallbacks[index - 1] : &specs[r].primary;
            if (os64_streq(path, "builtin")) continue;
            size_t f = 0;
            while (f < count && !os64_streq(path, files[f].path)) ++f;
            if (f == count) {
                os64_font_config_status_t read = font_source_read(path, &remaining,
                                                        &files[f].bytes, &files[f].length);
                if (read) {
                    status = problem(e, read, c.roles[r].source_line[s], r, s);
                    goto done;
                }
                files[f].path = path;
                ++count;
            }
            *source = (os64_font_source_t){OS64_FONT_SOURCE_OUTLINE, files[f].bytes, files[f].length};
        }
    }
    os64_font_problem_t where;
    os64_font_status_t prepared = os64_font_set_prepare_checked(context, specs, out, &where);
    if (prepared) {
        size_t r = where.role, source = 0, line = 0;
        if (r < OS64_FONT_ROLE_COUNT) {
            source = where.source_index < 3 ? indices[r][where.source_index] : 0;
            line = c.roles[r].source_line[source];
        }
        status = problem(e, OS64_FONT_CONFIG_FACE, line, r, source);
        if (e) e->font_status = prepared;
    }
done:
    for (size_t f = 0; f < count; ++f) os64_free(files[f].bytes);
    return status;
}

int64_t os64_font_config_encode(const os64_font_config_t *config, char *out, size_t cap)
{
    if (!config || !out || !cap) return -1;
    os64_font_config_t c = *config;
    if (validate(&c, NULL)) return -1;
    char text[4096];
    size_t used = 0;
    for (size_t r = 0; r < OS64_FONT_ROLE_COUNT; ++r) {
        int n = os64_snprintf(text + used, sizeof(text) - used, "%s.face = %s\n%s.size = %u\n",
                             role_names[r], c.roles[r].face[0], role_names[r], c.roles[r].size);
        if (n < 0 || (size_t)n >= sizeof(text) - used) return -1;
        used += (size_t)n;
        for (size_t s = 1; s < 3; ++s) if (c.roles[r].face[s][0]) {
            n = os64_snprintf(text + used, sizeof(text) - used, "%s.fallback.%u = %s\n",
                             role_names[r], (unsigned)s, c.roles[r].face[s]);
            if (n < 0 || (size_t)n >= sizeof(text) - used) return -1;
            used += (size_t)n;
        }
    }
    if (used >= cap) return -1;
    os64_memcpy(out, text, used + 1);
    return (int64_t)used;
}

const char *os64_font_config_status_name(os64_font_config_status_t status)
{
    static const char *const names[] = {"ok", "invalid setting", "unsupported path",
        "invalid size", "duplicate font", "could not read file", "limit exceeded",
        "out of memory", "font rejected", "installed name already exists"};
    return (unsigned)status < sizeof(names) / sizeof(names[0]) ? names[status] : "unknown error";
}
