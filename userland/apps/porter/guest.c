// Effective routes, mounted-volume identity and read-only file probes.
#include "porter.h"
#include "os64/os64.h"
#include "os64/slurp.h"
#include "bearssl_hash.h"

#define MAX_RULES 256
#define SMALL_TEXT (128u * 1024u)

const char *porter_basename(const char *path)
{
    const char *base = path;
    for (; *path; path++)
        if (*path == '/')
            base = path + 1;
    return base;
}

bool porter_parent(char out[PORTER_PATH], const char *path)
{
    size_t n = (size_t)(porter_basename(path) - path);
    if (!n || !path[n])
        return false;
    size_t length = n == 1 ? 1 : n - 1;
    os64_memcpy(out, path, length);
    out[length] = 0;
    return true;
}

bool porter_join(char out[PORTER_PATH], const char *left, const char *right)
{
    if (*right == '/')
        right++;
    if (!*right)
        return os64_strcopy(out, PORTER_PATH, left) < PORTER_PATH;
    int n = os64_snprintf(out, PORTER_PATH, "%s%s%s", left,
                           os64_streq(left, "/") ? "" : "/", right);
    return n > 0 && n < PORTER_PATH;
}

bool porter_under(const char *path, const char *root)
{
    size_t n = os64_strlen(root);
    return n && os64_strlen(path) >= n && os64_memcmp(path, root, n) == 0 &&
           (os64_streq(root, "/") || !path[n] || path[n] == '/');
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + 'a' - 'A') : c;
}

bool porter_folded_equal(const char *a, const char *b, bool fold)
{
    if (!fold)
        return os64_streq(a, b);
    while (*a && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return lower(*a) == lower(*b);
}

static bool overlaps(const char *a, const char *b, bool fold)
{
    char first[PORTER_PATH], second[PORTER_PATH];
    if (os64_strcopy(first, sizeof(first), a) >= sizeof(first) ||
        os64_strcopy(second, sizeof(second), b) >= sizeof(second))
        return true;
    if (fold) {
        for (char *p = first; *p; p++)
            *p = lower(*p);
        for (char *p = second; *p; p++)
            *p = lower(*p);
    }
    return porter_under(first, second) || porter_under(second, first);
}

static unsigned fields(char *line, char **out, unsigned limit)
{
    unsigned n = 0;
    while (*line) {
        while (*line == ' ' || *line == '\t' || *line == '\r')
            line++;
        if (!*line)
            break;
        if (n == limit)
            return limit + 1;
        out[n++] = line;
        while (*line && *line != ' ' && *line != '\t' && *line != '\r')
            line++;
        if (*line)
            *line++ = 0;
    }
    return n;
}

bool porter_load_volumes(porter_volumes_t *out)
{
    *out = (porter_volumes_t){0};
    uint8_t *raw;
    size_t size, cap = 0;
    if (os64_slurp("/sys/mounts", SMALL_TEXT, &raw, &size) != OS64_SLURP_OK)
        return false;
    char *line = os64_strchr((char *)raw, '\n');
    bool ok = line && raw[0] == '#';
    if (line)
        line++;
    while (ok && *line) {
        char *end = os64_strchr(line, '\n');
        if (!end) {
            ok = false;
            break;
        }
        *end = 0;
        char *part[12];
        if (fields(line, part, 12) != 12 ||
            !porter_grow((void **)&out->mounts, &cap, out->count, sizeof(*out->mounts), PORTER_MOUNTS)) {
            ok = false;
            break;
        }
        os64_unescape_field(part[0]);
        porter_mount_t *mount = &out->mounts[out->count];
        *mount = (porter_mount_t){0};
        mount->fat = os64_streq(part[1], "fat");
        mount->disk = mount->fat || os64_streq(part[1], "ext2");
        mount->writable = os64_streq(part[6], "rw");
        mount->persistent_ext2 = os64_streq(part[1], "ext2") && os64_strlen(part[2]) >= 5 &&
                                os64_memcmp(part[2], "nvme", 4) == 0;
        for (char *p = part[5]; *p; p++)
            *p = lower(*p);
        if (!porter_component_path(part[0], true) || (mount->disk && !porter_guid(part[5]))) {
            ok = false;
            break;
        }
        os64_strcopy(mount->where, sizeof(mount->where), part[0]);
        os64_strcopy(mount->guid, sizeof(mount->guid), part[5]);
        mount->space_known = os64_parse_u64(part[9], &mount->free_bytes);
        if (!os64_parse_u64(part[7], &mount->block_size) ||
            !mount->block_size || mount->block_size > 65536)
            mount->block_size = 65536; // FAT reports free bytes without its cluster size.
        for (size_t i = 0; i < out->count; i++)
            if (os64_streq(out->mounts[i].where, mount->where) ||
                (mount->disk && out->mounts[i].disk &&
                 os64_streq(out->mounts[i].guid, mount->guid)))
                ok = false;
        out->count++;
        line = end + 1;
    }
    os64_free(raw);
    if (ok && porter_mount_at(out, "/"))
        return true;
    os64_free(out->mounts);
    *out = (porter_volumes_t){0};
    return false;
}

const porter_mount_t *porter_mount_at(const porter_volumes_t *volumes, const char *path)
{
    const porter_mount_t *best = NULL;
    for (size_t i = 0; i < volumes->count; i++) {
        const porter_mount_t *m = &volumes->mounts[i];
        if (porter_under(path, m->where) &&
            (!best || os64_strlen(m->where) > os64_strlen(best->where)))
            best = m;
    }
    return best;
}

const porter_mount_t *porter_mount_by_guid(const porter_volumes_t *volumes, const char *guid)
{
    for (size_t i = 0; i < volumes->count; i++)
        if (volumes->mounts[i].disk && os64_streq(volumes->mounts[i].guid, guid))
            return &volumes->mounts[i];
    return NULL;
}

static char *trim(char *text)
{
    while (*text == ' ' || *text == '\t')
        text++;
    char *end = text + os64_strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        *--end = 0;
    return text;
}

static bool route_line(porter_config_t *cfg, char *key, const char *value, size_t *cap)
{
    char *suffix = os64_strchr(key, '/');
    if (suffix) {
        *suffix++ = 0;
        if (*suffix++ != '*' || *suffix != '.' || !suffix[1] ||
            os64_strlen(suffix) >= PORTER_LOT || !porter_component_path(suffix, false) ||
            os64_strchr(suffix, '/'))
            return false;
    }
    if (!porter_lot(key) || !porter_component_path(value, true))
        return false;
    if (!suffix)
        suffix = "";
    for (size_t i = 0; i < cfg->count; i++) {
        porter_rule_t *r = &cfg->rules[i];
        if (os64_streq(r->lot, key) && porter_folded_equal(r->suffix, suffix, true)) {
            os64_strcopy(r->directory, sizeof(r->directory), value);
            return true;
        }
    }
    if (!porter_grow((void **)&cfg->rules, cap, cfg->count, sizeof(*cfg->rules), MAX_RULES))
        return false;
    porter_rule_t *r = &cfg->rules[cfg->count++];
    os64_strcopy(r->lot, sizeof(r->lot), key);
    os64_strcopy(r->suffix, sizeof(r->suffix), suffix);
    os64_strcopy(r->directory, sizeof(r->directory), value);
    return true;
}

static bool config_file(porter_config_t *cfg, size_t *cap, const char *path, bool required, bool fat)
{
    os64_dirent_t e;
    if (os64_stat(path, &e) < 0) {
        porter_kind_t kind;
        return !required && porter_probe(path, fat, &kind) && kind == PORTER_ABSENT;
    }
    uint8_t *raw;
    size_t n;
    if ((e.flags & OS64_DE_DIR) ||
        os64_slurp(path, SMALL_TEXT, &raw, &n) != OS64_SLURP_OK)
        return false;
    enum { NONE, DESTINATIONS, BACKUPS } section = NONE;
    char *line = (char *)raw;
    bool ok = n == os64_strlen(line);
    while (ok && *line) {
        char *next = os64_strchr(line, '\n');
        if (next)
            *next++ = 0;
        char *text = trim(line);
        if (os64_streq(text, "[destinations]"))
            section = DESTINATIONS;
        else if (os64_streq(text, "[backups]"))
            section = BACKUPS;
        else if (*text && *text != '#') {
            char *equals = os64_strchr(text, '=');
            if (!equals) {
                ok = false;
            } else {
                *equals++ = 0;
                char *key = trim(text), *value = trim(equals);
                if (section == DESTINATIONS)
                    ok = route_line(cfg, key, value, cap);
                else if (section == BACKUPS && os64_streq(key, "directory")) {
                    ok = porter_component_path(value, true);
                    if (ok)
                        os64_strcopy(cfg->backup, sizeof(cfg->backup), value);
                } else
                    ok = section == BACKUPS && os64_streq(key, "compression") && os64_streq(value, "gzip");
            }
        }
        if (!next)
            break;
        line = next;
    }
    os64_free(raw);
    return ok;
}

bool porter_load_config(porter_config_t *cfg, const porter_volumes_t *volumes)
{
    *cfg = (porter_config_t){0};
    size_t cap = 0;
    const porter_mount_t *home = porter_mount_at(volumes, "/home/porter.conf");
    if (config_file(cfg, &cap, "/etc/porter.conf", true, false) &&
        config_file(cfg, &cap, "/home/porter.conf", false, home && home->fat) && *cfg->backup)
        return true;
    os64_free(cfg->rules);
    *cfg = (porter_config_t){0};
    return false;
}

const char *porter_route(const porter_config_t *cfg, const char *lot, const char *path)
{
    const char *base = porter_basename(path);
    size_t n = os64_strlen(base), best = 0;
    const char *result = NULL;
    for (size_t i = 0; i < cfg->count; i++) {
        const porter_rule_t *r = &cfg->rules[i];
        size_t suffix = os64_strlen(r->suffix);
        if (!os64_streq(lot, r->lot) || suffix > n ||
            (suffix && !porter_folded_equal(base + n - suffix, r->suffix, true)))
            continue;
        if (!result || suffix > best) {
            result = r->directory;
            best = suffix;
        }
    }
    return result;
}

static bool fat_name(const char *name)
{
    size_t n = os64_strlen(name);
    if (!n || name[n - 1] == '.' || name[n - 1] == ' ')
        return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (*p >= 127 || os64_strchr("\"*<>?|", *p))
            return false;
    return true;
}

// A FAT stat may echo the requested short alias. Readdir is the source
// of directory-entry spellings. It also confirms absence after a failed stat.
static int directory_name(const char *parent, const char *name, bool fat, char *canonical)
{
    int64_t opened = os64_opendir(parent);
    if (opened < 0)
        return -1;
    os64_dirent_t e;
    int64_t rc;
    while ((rc = os64_readdir((int32_t)opened, &e)) == 1) {
        if (!porter_folded_equal(e.name, name, fat))
            continue;
        if (canonical && os64_strcopy(canonical, PORTER_PATH, e.name) >= PORTER_PATH)
            rc = -1;
        break;
    }
    return os64_close((int32_t)opened) == 0 ? (int)rc : -1;
}

bool porter_locate(const porter_volumes_t *volumes, const char *path, porter_location_t *out,
                   const char **failure)
{
    *out = (porter_location_t){0};
    os64_strcopy(out->path, sizeof(out->path), path);
    *failure = "invalid destination path";
    if (!porter_component_path(path, true))
        return false;
    const porter_mount_t *m = porter_mount_at(volumes, path);
    if (m && os64_streq(m->where, "/") && porter_under(path, "/fat") && m->fat) {
        // The FAT boot volume is the root when booted from the lifeboat.
        path += 4;
        if (!*path)
            path = "/";
    } else if (m && os64_streq(m->where, "/") &&
               (porter_under(path, "/fat") || porter_under(path, "/home") || porter_under(path, "/ext2"))) {
        *failure = "destination mount is unavailable";
        return false;
    }
    if (!m || !m->disk || !m->writable) {
        *failure = "destination requires a writable FAT or ext2 volume";
        return false;
    }
    out->mount = m;
    os64_strcopy(out->path, sizeof(out->path), m->where);
    const char *component = path + (os64_streq(m->where, "/") ? 1 : os64_strlen(m->where));
    bool parent_missing = false;
    while (*component) {
        if (*component == '/')
            component++;
        const char *end = os64_strchr(component, '/');
        size_t length = end ? (size_t)(end - component) : os64_strlen(component);
        char name[PORTER_PATH], candidate[PORTER_PATH];
        os64_memcpy(name, component, length);
        name[length] = 0;
        if (m->fat && !fat_name(name)) {
            *failure = "name is ambiguous or unsupported on FAT";
            return false;
        }
        if (!porter_join(candidate, out->path, name))
            return false;
        os64_dirent_t e;
        if (!parent_missing && os64_stat(candidate, &e) == 0) {
            if ((e.flags & OS64_DE_MOUNT) || (end && !(e.flags & OS64_DE_DIR))) {
                *failure = "destination crosses a mount or has a file as a parent";
                return false;
            }
            if (m->fat) {
                char canonical[PORTER_PATH];
                int match = directory_name(out->path, name, true, canonical);
                if (match != 1) {
                    os64_strcopy(out->path, sizeof(out->path), candidate);
                    *failure = match == 0 ? "destination names an existing FAT short-name alias"
                                           : "cannot verify the FAT destination spelling";
                    return false;
                }
                if (!porter_join(candidate, out->path, canonical))
                    return false;
            }
        } else if (!parent_missing) {
            if (directory_name(out->path, name, m->fat, NULL) != 0) {
                os64_strcopy(out->path, sizeof(out->path), candidate);
                *failure = "destination cannot be inspected";
                return false;
            }
            parent_missing = true;
        }
        os64_strcopy(out->path, sizeof(out->path), candidate);
        if (porter_mount_at(volumes, out->path) != m) {
            *failure = "destination spelling crosses a mount";
            return false;
        }
        if (!end)
            break;
        component = end;
    }
    const char *relative = out->path + (os64_streq(m->where, "/") ? 0 : os64_strlen(m->where));
    os64_strcopy(out->relative, sizeof(out->relative), *relative ? relative : "/");
    *failure = NULL;
    return true;
}

static bool other_boot_root(const porter_mount_t *mount)
{
    return os64_streq(mount->where, "/ext2") || os64_streq(mount->where, "/fat");
}

static bool under_log_directory(const char *path, bool fat)
{
    size_t len = sizeof(PORTER_LOG_DIR) - 1;
    char prefix[sizeof(PORTER_LOG_DIR)];
    if (os64_strlen(path) < len || (path[len] && path[len] != '/'))
        return false;
    os64_memcpy(prefix, path, len);
    prefix[len] = 0;
    return porter_folded_equal(prefix, PORTER_LOG_DIR, fat);
}

bool porter_log_contains(const porter_mount_t *mount, const char *path, const char *relative)
{
    return under_log_directory(path, mount->fat) ||
           (other_boot_root(mount) && under_log_directory(relative, mount->fat));
}

bool porter_protected(const porter_config_t *cfg, const porter_volumes_t *volumes,
                      const porter_location_t *where)
{
    bool fat = where->mount->fat;
    const char *rel = where->relative;
    // Reserve the active log directory and its equivalent on either boot root.
    if (overlaps(where->path, PORTER_LOG_DIR, fat) ||
        (other_boot_root(where->mount) && overlaps(rel, PORTER_LOG_DIR, fat)) ||
        overlaps(rel, "/.porter", fat) || overlaps(rel, "/.os64get-tmp", fat) ||
        overlaps(where->path, PORTER_STATE, fat) || overlaps(where->path, "/home/porter.conf", fat) ||
        overlaps(where->path, "/tmp/os64get", fat) ||
        (fat && (overlaps(rel, "/efi", true) || overlaps(rel, "/boot/limine", true))))
        return true;
    porter_location_t archive;
    const char *cause;
    return overlaps(where->path, cfg->backup, fat) ||
           (porter_locate(volumes, cfg->backup, &archive, &cause) && archive.mount == where->mount &&
            overlaps(rel, archive.relative, fat));
}

static bool hash_handle(int32_t handle, uint64_t expected, char hash[PORTER_DIGEST])
{
    br_sha256_context sha;
    br_sha256_init(&sha);
    unsigned char digest[32];
    unsigned char *buf = os64_malloc(65536);
    if (!buf)
        return false;
    uint64_t total = 0;
    int64_t n;
    while ((n = os64_read(handle, buf, 65536)) > 0) {
        if ((uint64_t)n > expected - total)
            break;
        br_sha256_update(&sha, buf, (size_t)n);
        total += (uint64_t)n;
    }
    os64_free(buf);
    if (n != 0 || total != expected)
        return false;
    static const char hex[] = "0123456789abcdef";
    br_sha256_out(&sha, digest);
    for (size_t i = 0; i < sizeof(digest); i++) {
        hash[2 * i] = hex[digest[i] >> 4];
        hash[2 * i + 1] = hex[digest[i] & 15];
    }
    hash[64] = 0;
    return true;
}

bool porter_hash_file(const char *path, uint64_t expected, char hash[PORTER_DIGEST])
{
    int64_t h = os64_open(path, "r");
    if (h < 0)
        return false;
    bool ok = hash_handle((int32_t)h, expected, hash);
    return os64_close((int32_t)h) == 0 && ok;
}

bool porter_probe(const char *path, bool fat, porter_kind_t *kind)
{
    *kind = PORTER_ABSENT;
    os64_dirent_t e;
    if (os64_stat(path, &e) < 0) {
        char dir[PORTER_PATH], previous[PORTER_PATH];
        os64_strcopy(dir, sizeof(dir), path);
        // Walk up a missing tail until the first readable directory can
        // confirm the missing component. A failed read stays a conflict.
        do {
            os64_strcopy(previous, sizeof(previous), dir);
            if (!porter_parent(dir, previous))
                return false;
        } while (os64_stat(dir, &e) < 0);
        return (e.flags & OS64_DE_DIR) && directory_name(dir, porter_basename(previous), fat, NULL) == 0;
    }
    *kind = (e.flags & OS64_DE_DIR) ? PORTER_DIR : PORTER_FILE;
    return true;
}

bool porter_inspect(const porter_location_t *where, porter_identity_t *id)
{
    *id = (porter_identity_t){0};
    os64_dirent_t before, after;
    if (os64_stat(where->path, &before) < 0)
        return porter_probe(where->path, where->mount->fat, &id->kind) && id->kind == PORTER_ABSENT;
    if (before.flags & OS64_DE_MOUNT)
        return false;
    if (before.flags & OS64_DE_DIR) {
        id->kind = PORTER_DIR;
        return true;
    }
    id->kind = PORTER_FILE;
    id->size = before.size;
    return porter_hash_file(where->path, id->size, id->hash) &&
           os64_stat(where->path, &after) == 0 && before.flags == after.flags &&
           before.size == after.size && before.mtime == after.mtime && os64_streq(before.name, after.name);
}

bool porter_boot_digest(char hash[PORTER_DIGEST])
{
    os64_dirent_t e;
    const char *path = "/sys/boot-kernel";
    return os64_stat(path, &e) == 0 && !(e.flags & OS64_DE_DIR) && e.size &&
           porter_hash_file(path, e.size, hash);
}
