#include "os64/font_config.h"
#include "os64/conf.h"
#include "os64/slurp.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "os64/fmt.h"
#include "os64/io.h"
#include "font_config_internal.h"

static const char *const role_names[OS64_FONT_CONFIG_ROLES] = { "ui", "terminal", "document", "web" };
_Static_assert(OS64_FONT_ROLE_COUNT == 3, "a new role needs a name here, before web");
static const char *const family_names[] = { "serif", "sans", "mono" };
static const char *const family_suffix[] = { "", ".bold", ".italic", ".bolditalic",
                                            ".fallback.1", ".fallback.2" };
static const char *const family_defaults[OS64_FONT_FAMILY_COUNT][OS64_FONT_FAMILY_STYLES] = {
    {"/etc/fonts/DejaVuSerif.ttf", "/etc/fonts/DejaVuSerif-Bold.ttf",
     "/etc/fonts/DejaVuSerif-Italic.ttf", "/etc/fonts/DejaVuSerif-BoldItalic.ttf"},
    {"/etc/fonts/DejaVuSans.ttf", "/etc/fonts/DejaVuSans-Bold.ttf",
     "/etc/fonts/DejaVuSans-Oblique.ttf", "/etc/fonts/DejaVuSans-BoldOblique.ttf"},
    {"/etc/fonts/DejaVuSansMono.ttf", "/etc/fonts/DejaVuSansMono-Bold.ttf",
     "/etc/fonts/DejaVuSansMono-Oblique.ttf", "/etc/fonts/DejaVuSansMono-BoldOblique.ttf"}
};

/* `role` is an index into a config's roles — the Web setting included — or
 * NO_ROLE for a problem that belongs to none of them. */
#define NO_ROLE SIZE_MAX
static os64_font_config_status_t problem(os64_font_config_error_t *e,
    os64_font_config_status_t status, size_t line, size_t role, size_t source)
{
    if (e) *e = (os64_font_config_error_t){status, OS64_FONT_OK, line, source,
        role < OS64_FONT_ROLE_COUNT ? (os64_font_role_t)role : OS64_FONT_ROLE_COUNT,
        role == OS64_FONT_CONFIG_WEB};
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
    os64_strcopy(c->roles[OS64_FONT_CONFIG_WEB].face[0], OS64_FONT_PATH_CAP,
                 "/etc/fonts/DejaVuSans.ttf");
    c->roles[OS64_FONT_CONFIG_WEB].size = 16;
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
    for (size_t r = 0; r < OS64_FONT_CONFIG_ROLES; ++r) {
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
    for (size_t f = 0; f < OS64_FONT_FAMILY_COUNT; ++f) {
        os64_font_config_family_t *family = &c->families[f];
        for (size_t s = 0; s < OS64_FONT_FAMILY_STYLES + OS64_FONT_CONFIG_FALLBACK_MAX; ++s) {
            if (!family->face[s][0]) continue;
            char path[OS64_FONT_PATH_CAP];
            if (!resolve_path(NULL, family->face[s], path))
                return problem(e, OS64_FONT_CONFIG_PATH, family->source_line[s],
                               NO_ROLE, s);
            os64_strcopy(family->face[s], OS64_FONT_PATH_CAP, path);
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
    for (size_t r = 0; key && r < OS64_FONT_CONFIG_ROLES; ++r) {
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
    for (size_t f = 0; key && f < OS64_FONT_FAMILY_COUNT; ++f) {
        for (size_t s = 0; s < OS64_FONT_FAMILY_STYLES + OS64_FONT_CONFIG_FALLBACK_MAX; ++s) {
            char name[40];
            os64_snprintf(name, sizeof(name), "family.%s%s", family_names[f], family_suffix[s]);
            if (!os64_streq_nocase(name, key)) continue;
            os64_font_config_family_t *family = &d->config->families[f];
            if (!resolve_path(d->config->path, value, family->face[s]))
                d->status = problem(d->error, OS64_FONT_CONFIG_PATH, d->line,
                                    NO_ROLE, s);
            else family->source_line[s] = d->line;
            return false;
        }
    }
    d->status = problem(d->error, OS64_FONT_CONFIG_SYNTAX, d->line,
                        NO_ROLE, 0);
    return false;
}

os64_font_config_status_t os64_font_config_decode(const char *text, size_t len,
    const char *path, os64_font_config_t *out, os64_font_config_error_t *e)
{
    problem(e, OS64_FONT_CONFIG_OK, 0, NO_ROLE, 0);
    if (!text || !out) return problem(e, OS64_FONT_CONFIG_SYNTAX, 0, NO_ROLE, 0);
    if (len > OS64_FONT_CONFIG_BYTES_MAX)
        return problem(e, OS64_FONT_CONFIG_LIMIT, 0, NO_ROLE, 0);
    os64_font_config_t c;
    os64_font_config_defaults(&c);
    if (!path || path[0] != '/' || !resolve_path(NULL, path, c.path))
        return problem(e, OS64_FONT_CONFIG_PATH, 0, NO_ROLE, 0);
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
                           OS64_FONT_CONFIG_SYNTAX, d.line, NO_ROLE, 0);
        if (d.status) return d.status;
        /* conf's callback omits empty keys as well as blank lines. An '='
         * without a key is malformed input, not an absent font choice. */
        if (!d.delivered && first < end && text[first] != '#')
            return problem(e, OS64_FONT_CONFIG_SYNTAX, d.line, NO_ROLE, 0);
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
    problem(e, OS64_FONT_CONFIG_OK, 0, NO_ROLE, 0);
    if (!out) return problem(e, OS64_FONT_CONFIG_SYNTAX, 0, NO_ROLE, 0);
    char path[OS64_FONT_PATH_CAP];
    if (os64_conf_find("fonts.conf", path, sizeof(path)) < 0) {
        os64_font_config_defaults(out);
        return OS64_FONT_CONFIG_OK;
    }
    uint8_t *bytes = NULL;
    size_t len = 0;
    os64_slurp_status_t s = os64_slurp(path, OS64_FONT_CONFIG_BYTES_MAX, &bytes, &len);
    if (s) return problem(e, read_error(s), 0, NO_ROLE, 0);
    os64_font_config_status_t status = os64_font_config_decode((const char *)bytes, len, path, out, e);
    os64_free(bytes);
    return status;
}

os64_font_config_status_t os64_font_config_prepare(os64_text_context_t *context,
    const os64_font_config_t *config, os64_font_set_t **out, os64_font_config_error_t *e)
{
    problem(e, OS64_FONT_CONFIG_OK, 0, NO_ROLE, 0);
    if (out) *out = NULL;
    if (!config || !context || !out)
        return problem(e, OS64_FONT_CONFIG_SYNTAX, 0, NO_ROLE, 0);
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
        // The provider's "no one role" (ROLE_COUNT) is the Web setting's
        // index here, so it is said as NO_ROLE: this door never opens the
        // Web face, and a failure it did not cause must not be laid on it.
        size_t r = where.role < OS64_FONT_ROLE_COUNT ? (size_t)where.role : NO_ROLE;
        size_t source = 0, line = 0;
        if (r != NO_ROLE) {
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

/* The Web setting, opened through the role door as the UI role of a set whose
 * other roles are builtin and cost nothing to prepare. */
os64_font_config_status_t os64_font_config_web_prepare(os64_text_context_t *context,
    const os64_font_config_t *config, uint32_t pixel_height, os64_font_set_t **out,
    os64_font_config_error_t *e)
{
    if (out) *out = NULL;
    if (!config) return problem(e, OS64_FONT_CONFIG_SYNTAX, 0, NO_ROLE, 0);
    os64_font_config_t c;
    os64_font_config_defaults(&c);
    os64_strcopy(c.path, OS64_FONT_PATH_CAP, config->path);
    os64_font_config_role_t *web = &c.roles[OS64_FONT_ROLE_UI];
    *web = config->roles[OS64_FONT_CONFIG_WEB];
    web->size = pixel_height < 8 ? 8 : pixel_height > 96 ? 96 : pixel_height;
    if (os64_streq(web->face[0], "builtin")) web->size = 16;
    os64_font_config_status_t status = os64_font_config_prepare(context, &c, out, e);
    if (status && e && e->role == OS64_FONT_ROLE_UI) {
        e->role = OS64_FONT_ROLE_COUNT;
        e->web = true;
    }
    return status;
}

/* Resolve the source slot as well as its path so inherited failures still
 * name the line that selected the unreadable file. */
static const char *family_path(const os64_font_config_family_t *family,
    size_t f, size_t s, size_t *origin)
{
    *origin = s;
    bool configured = false;
    for (size_t i = 0; i < OS64_FONT_FAMILY_STYLES + OS64_FONT_CONFIG_FALLBACK_MAX; ++i)
        configured |= family->face[i][0] != 0;
    if (!configured && s < OS64_FONT_FAMILY_STYLES) return family_defaults[f][s];
    if (s == 3 && !family->face[s][0]) s = 1;
    if ((s == 1 || s == 2) && !family->face[s][0]) s = 0;
    *origin = s;
    if (!s && !family->face[s][0]) return family_defaults[f][0];
    return family->face[s];
}
os64_font_config_status_t os64_font_config_family_prepare(os64_text_context_t *context,
    const os64_font_config_t *config, os64_font_family_cache_t **out,
    os64_font_config_error_t *e)
{
    problem(e, OS64_FONT_CONFIG_OK, 0, NO_ROLE, 0);
    if (out) *out = NULL;
    if (!context || !config || !out)
        return problem(e, OS64_FONT_CONFIG_SYNTAX, 0, NO_ROLE, 0);
    os64_font_config_t c = *config;
    os64_font_config_status_t status = validate(&c, e);
    if (status) return status;
    os64_font_family_spec_t specs[OS64_FONT_FAMILY_COUNT] = {0};
    enum { SOURCES = OS64_FONT_FAMILY_STYLES + OS64_FONT_CONFIG_FALLBACK_MAX,
           FILES = OS64_FONT_FAMILY_COUNT * SOURCES };
    struct { const char *path; uint8_t *bytes; size_t length; } files[FILES] = {0};
    size_t origins[OS64_FONT_FAMILY_COUNT][SOURCES] = {{0}};
    size_t count = 0, remaining = OS64_FONT_SOURCE_BYTES_MAX;
    for (size_t f = 0; f < OS64_FONT_FAMILY_COUNT; ++f) {
        for (size_t s = 0; s < SOURCES; ++s) {
            size_t origin;
            const char *path = family_path(&c.families[f], f, s, &origin);
            if (!path[0]) continue;
            size_t slot = s < OS64_FONT_FAMILY_STYLES ? s :
                OS64_FONT_FAMILY_STYLES + specs[f].fallback_count++;
            origins[f][slot] = origin;
            os64_font_source_t *dst = slot < OS64_FONT_FAMILY_STYLES ?
                &specs[f].styles[slot] : &specs[f].fallbacks[slot - OS64_FONT_FAMILY_STYLES];
            if (os64_streq(path, "builtin")) continue;
            size_t i = 0;
            while (i < count && !os64_streq(files[i].path, path)) ++i;
            if (i == count) {
                status = font_source_read(path, &remaining, &files[i].bytes, &files[i].length);
                if (status) {
                    problem(e, status, c.families[f].source_line[origin], NO_ROLE, origin);
                    goto done;
                }
                files[i].path = path; ++count;
            }
            *dst = (os64_font_source_t){OS64_FONT_SOURCE_OUTLINE, files[i].bytes, files[i].length};
        }
    }
    os64_font_family_t family;
    size_t source;
    os64_font_status_t prepared = os64_font_family_cache_create(context, specs, out, &family, &source);
    if (prepared) {
        size_t origin = family < OS64_FONT_FAMILY_COUNT && source < SOURCES ? origins[family][source] : 0;
        size_t line = family < OS64_FONT_FAMILY_COUNT && source < SOURCES ?
                      c.families[family].source_line[origin] : 0;
        status = problem(e, OS64_FONT_CONFIG_FACE, line, NO_ROLE, origin);
        if (e) e->font_status = prepared;
    }
done:
    for (size_t i = 0; i < count; ++i) os64_free(files[i].bytes);
    return status;
}

int64_t os64_font_config_encode(const os64_font_config_t *config, char *out, size_t cap)
{
    if (!config || !out || !cap) return -1;
    os64_font_config_t c = *config;
    if (validate(&c, NULL)) return -1;
    char text[OS64_FONT_CONFIG_BYTES_MAX + 1];
    size_t used = 0;
    for (size_t r = 0; r < OS64_FONT_CONFIG_ROLES; ++r) {
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
    for (size_t f = 0; f < OS64_FONT_FAMILY_COUNT; ++f)
        for (size_t s = 0; s < OS64_FONT_FAMILY_STYLES + OS64_FONT_CONFIG_FALLBACK_MAX; ++s) {
            if (!c.families[f].face[s][0]) continue;
            int n = os64_snprintf(text + used, sizeof(text) - used, "family.%s%s = %s\n",
                family_names[f], family_suffix[s], c.families[f].face[s]);
            if (n < 0 || (size_t)n >= sizeof(text) - used) return -1;
            used += (size_t)n;
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
