// Bounded readers for the Porter v1 manifest, catalog and receipt formats.
#include "porter.h"
#include "os64/os64.h"
#include "bearssl_hash.h"

bool porter_grow(void **array, size_t *cap, size_t used, size_t element, size_t limit)
{
    if (used < *cap)
        return true;
    if (used >= limit)
        return false;
    size_t n = *cap ? *cap * 2 : 16;
    if (n > limit)
        n = limit;
    void *next = os64_realloc(*array, n * element);
    if (!next)
        return false;
    *array = next;
    *cap = n;
    return true;
}

bool porter_component_path(const char *path, bool absolute)
{
    if (!path || (*path == '/') != absolute || os64_strlen(path) >= PORTER_PATH)
        return false;
    if (absolute && !*++path)
        return true;
    while (*path) {
        const char *first = path;
        while (*path && *path != '/') {
            unsigned char c = *path++;
            if (c < 32 || c == 127 || c == '\\' || c == ':')
                return false;
        }
        size_t n = (size_t)(path - first);
        if (!n || (n == 1 && first[0] == '.') || (n == 2 && first[0] == '.' && first[1] == '.'))
            return false;
        if (!*path)
            return true;
        path++;
    }
    return false;
}

bool porter_lot(const char *name)
{
    if (!name || *name < 'a' || *name > 'z')
        return false;
    size_t length = 0;
    for (; *name; name++, length++)
        if (!(*name >= 'a' && *name <= 'z') && !(*name >= '0' && *name <= '9')
            && *name != '_' && *name != '-')
            return false;
    return length < PORTER_LOT;
}

static int nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

bool porter_hex(const char *text, size_t digits)
{
    if (!text || os64_strlen(text) != digits)
        return false;
    for (size_t i = 0; i < digits; i++)
        if (nibble(text[i]) < 0)
            return false;
    return true;
}

bool porter_guid(const char *name)
{
    if (!name || os64_strlen(name) != PORTER_GUID - 1)
        return false;
    for (size_t i = 0; i < PORTER_GUID - 1; i++) {
        bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? name[i] != '-' : nibble(name[i]) < 0)
            return false;
    }
    return true;
}

void porter_sha(const void *data, size_t size, char hash[PORTER_DIGEST])
{
    static const char digits[] = "0123456789abcdef";
    br_sha256_context ctx;
    unsigned char value[32];
    br_sha256_init(&ctx);
    br_sha256_update(&ctx, data, size);
    br_sha256_out(&ctx, value);
    for (size_t i = 0; i < sizeof(value); i++) {
        hash[i * 2] = digits[value[i] >> 4];
        hash[i * 2 + 1] = digits[value[i] & 15];
    }
    hash[64] = 0;
}

bool porter_identity_equal(const porter_identity_t *a, const porter_identity_t *b)
{
    return a->kind == b->kind &&
           (a->kind != PORTER_FILE || (a->size == b->size && os64_streq(a->hash, b->hash)));
}

typedef struct { char *p, *end; } json_t;

static bool take(json_t *j, char c)
{
    if (j->p == j->end || *j->p != c)
        return false;
    j->p++;
    return true;
}

static bool literal(json_t *j, const char *text)
{
    size_t n = os64_strlen(text);
    if ((size_t)(j->end - j->p) < n || os64_memcmp(j->p, text, n))
        return false;
    j->p += n;
    return true;
}

static bool unit(json_t *j, uint32_t *value)
{
    if (j->end - j->p < 4)
        return false;
    *value = 0;
    for (unsigned i = 0; i < 4; i++) {
        int n = nibble(*j->p++);
        if (n < 0)
            return false;
        *value = (*value << 4) | (unsigned)n;
    }
    return true;
}

static char *utf8(char *p, uint32_t value)
{
    if (value < 0x80) {
        *p++ = (char)value;
        return p;
    }
    if (value >= 0x10000) {
        *p++ = (char)(0xf0 | (value >> 18));
        *p++ = (char)(0x80 | ((value >> 12) & 63));
    } else if (value >= 0x800) {
        *p++ = (char)(0xe0 | (value >> 12));
    } else {
        *p++ = (char)(0xc0 | (value >> 6));
        *p++ = (char)(0x80 | (value & 63));
        return p;
    }
    *p++ = (char)(0x80 | ((value >> 6) & 63));
    *p++ = (char)(0x80 | (value & 63));
    return p;
}

static char *string(json_t *j)
{
    if (!take(j, '"'))
        return NULL;
    char *first = j->p, *dest = first;
    while (j->p < j->end) {
        unsigned char c = *j->p++;
        if (c == '"') {
            *dest = 0;
            return first;
        }
        if (c < 32 || c >= 127)
            return NULL;
        if (c != '\\') {
            *dest++ = (char)c;
            continue;
        }
        if (j->p == j->end)
            return NULL;
        c = *j->p++;
        if (c == '\\' || c == '"' || c == '/') {
            *dest++ = (char)c;
            continue;
        }
        if (c != 'u') {
            const char *escapes = "bfnrt";
            const char *decoded = "\b\f\n\r\t";
            const char *escaped = os64_strchr(escapes, c);
            if (!c || !escaped)
                return NULL;
            *dest++ = decoded[escaped - escapes];
            continue;
        }
        uint32_t value, low;
        if (!unit(j, &value) || !value)
            return NULL;
        if (value >= 0xd800 && value <= 0xdbff) {
            if (!literal(j, "\\u") || !unit(j, &low) || low < 0xdc00 || low > 0xdfff)
                return NULL;
            value = 0x10000 + ((value - 0xd800) << 10) + low - 0xdc00;
        } else if (value >= 0xdc00 && value <= 0xdfff) {
            return NULL;
        }
        dest = utf8(dest, value);
    }
    return NULL;
}

static bool key(json_t *j, const char *name)
{
    const char *actual = string(j);
    return actual && os64_streq(actual, name) && take(j, ':');
}

static bool next(json_t *j, const char *name)
{
    return take(j, ',') && key(j, name);
}

static bool number(json_t *j, uint64_t *value)
{
    if (j->p == j->end || *j->p < '0' || *j->p > '9')
        return false;
    *value = 0;
    if (take(j, '0'))
        return true;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
        unsigned digit = *j->p++ - '0';
        if (*value > (UINT64_MAX - digit) / 10)
            return false;
        *value = *value * 10 + digit;
    }
    return true;
}

static bool skip(json_t *j, unsigned depth)
{
    uint64_t value;
    if (depth >= 16 || j->p == j->end)
        return false;
    char c = *j->p;
    if (c == '"')
        return string(j) != NULL;
    if (c == '{' || c == '[') {
        char end = c == '{' ? '}' : ']';
        j->p++;
        if (take(j, end))
            return true;
        do {
            if ((c == '{' && (!string(j) || !take(j, ':'))) || !skip(j, depth + 1))
                return false;
            if (take(j, end))
                return true;
        } while (take(j, ','));
        return false;
    }
    return literal(j, "true") || literal(j, "false") || literal(j, "null") || number(j, &value);
}

static bool read_entry(json_t *j, porter_entry_t *e, bool receipt)
{
    if (!take(j, '{') || !key(j, "kind"))
        return false;
    const char *kind = string(j);
    if (!kind || !(os64_streq(kind, "file") || os64_streq(kind, "dir")))
        return false;
    e->identity.kind = os64_streq(kind, "file") ? PORTER_FILE : PORTER_DIR;
    if (!next(j, "lot") || !porter_lot(e->lot = string(j)) ||
        !next(j, "path") || !porter_component_path(e->path = string(j), false))
        return false;
    if (receipt && (!next(j, "relative") ||
                    !porter_component_path(e->relative = string(j), true) ||
                    os64_streq(e->relative, "/")))
        return false;
    if (e->identity.kind == PORTER_FILE) {
        const char *hash;
        if (!next(j, "sha256") || !porter_hex(hash = string(j), 64) ||
            !next(j, "size") || !number(j, &e->identity.size) ||
            e->identity.size > 8589934591ULL)
            return false;
        os64_strcopy(e->identity.hash, sizeof(e->identity.hash), hash);
    }
    if (receipt && (!next(j, "volume") || !porter_guid(e->volume = string(j))))
        return false;
    return take(j, '}');
}

static int compare(const porter_entry_t *entry, const char *lot, const char *path)
{
    int order = os64_strcmp(entry->lot, lot);
    return order ? order : os64_strcmp(entry->path, path);
}

const porter_entry_t *porter_find(const porter_inventory_t *inv, const char *lot, const char *path)
{
    size_t lo = 0, hi = inv->entry_count;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        int c = compare(&inv->entries[m], lot, path);
        if (!c)
            return &inv->entries[m];
        if (c < 0)
            lo = m + 1;
        else
            hi = m;
    }
    return NULL;
}

bool porter_has_lot(const porter_inventory_t *inv, const char *lot)
{
    size_t lo = 0, hi = inv->lot_count;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        int c = os64_strcmp(inv->lots[m], lot);
        if (!c)
            return true;
        if (c < 0)
            lo = m + 1;
        else
            hi = m;
    }
    return false;
}

static bool read_entries(json_t *j, porter_inventory_t *inv, bool receipt)
{
    size_t cap = 0;
    if (!key(j, "entries") || !take(j, '['))
        return false;
    if (take(j, ']'))
        return true;
    do {
        if (!porter_grow((void **)&inv->entries, &cap, inv->entry_count,
                         sizeof(*inv->entries), PORTER_RECORDS))
            return false;
        porter_entry_t *entry = &inv->entries[inv->entry_count];
        *entry = (porter_entry_t){0};
        if (!read_entry(j, entry, receipt) ||
            (inv->entry_count && compare(entry - 1, entry->lot, entry->path) >= 0))
            return false;
        inv->entry_count++;
        if (take(j, ']'))
            return true;
    } while (take(j, ','));
    return false;
}

static bool read_lots(json_t *j, porter_inventory_t *inv)
{
    size_t cap = 0;
    if (!next(j, "lots") || !take(j, '['))
        return false;
    do {
        const char *lot = string(j);
        if (!porter_lot(lot) || (inv->lot_count && os64_strcmp(inv->lots[inv->lot_count - 1], lot) >= 0) ||
            !porter_grow((void **)&inv->lots, &cap, inv->lot_count, sizeof(*inv->lots), PORTER_RECORDS))
            return false;
        inv->lots[inv->lot_count++] = lot;
        if (take(j, ']'))
            return true;
    } while (take(j, ','));
    return false;
}

bool porter_parse_inventory(porter_inventory_t *out, char *data, size_t count, bool receipt)
{
    *out = (porter_inventory_t){.data = data};
    porter_sha(data, count, out->digest);
    json_t j = {data, data + count};
    uint64_t version;
    if (!take(&j, '{') || !read_entries(&j, out, receipt))
        return false;
    if (receipt) {
        if (!next(&j, "previous") || (!literal(&j, "null") && !porter_hex(out->previous = string(&j), 64)))
            return false;
    } else {
        if (!next(&j, "kernel"))
            return false;
        if (!literal(&j, "null") &&
            (!take(&j, '{') || !key(&j, "elf_sha256") || !porter_hex(out->kernel = string(&j), 64)
             || !take(&j, '}')))
            return false;
        if (!read_lots(&j, out))
            return false;
    }
    if (!next(&j, "scope") || !porter_lot(out->scope = string(&j)))
        return false;
    if (receipt && (!next(&j, "target") || !porter_hex(out->target = string(&j), 64)))
        return false;
    if (!next(&j, "version") || !number(&j, &version) || version != 1 ||
        !take(&j, '}') || j.p != j.end)
        return false;
    if (receipt)
        return true;
    if (os64_streq(out->scope, "system")) {
        const porter_entry_t *kernel = porter_find(out, "boot", "os64_kernel");
        if (!out->kernel || !kernel || kernel->identity.kind != PORTER_FILE ||
            !os64_streq(out->kernel, kernel->identity.hash))
            return false;
    } else if (out->kernel) {
        return false;
    }
    for (size_t i = 0; i < out->entry_count; i++) {
        const porter_entry_t *e = &out->entries[i];
        if (!porter_has_lot(out, e->lot) ||
            os64_strlen(e->lot) + 1 + os64_strlen(e->path) + (e->identity.kind == PORTER_DIR) >= PORTER_PATH)
            return false;
        char parent[PORTER_PATH];
        if (porter_parent(parent, e->path)) {
            const porter_entry_t *p = porter_find(out, e->lot, parent);
            if (!p || p->identity.kind != PORTER_DIR)
                return false;
        }
    }
    return true;
}

void porter_release_inventory(porter_inventory_t *inv)
{
    os64_free(inv->data);
    os64_free(inv->entries);
    os64_free(inv->lots);
    *inv = (porter_inventory_t){0};
}

static bool copy_field(char *out, size_t cap, const char *in)
{
    if (!in || !*in || os64_strlen(in) >= cap)
        return false;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++)
        if (*p < 32 || *p == 127)
            return false;
    return os64_strcopy(out, cap, in) < cap;
}

bool porter_parse_publication(porter_publication_t *out, char *line, size_t size)
{
    *out = (porter_publication_t){0};
    json_t j = {line, line + size};
    uint64_t version;
    size_t cap = 0;
    const char *id;
    if (!take(&j, '{') || !key(&j, "baseline"))
        return false;
    if (literal(&j, "true"))
        out->baseline = true;
    else if (!literal(&j, "false"))
        return false;
    if (!next(&j, "branch") || !skip(&j, 0) || !next(&j, "dirty") || !skip(&j, 0) ||
        !next(&j, "id") || !porter_hex(id = string(&j), 32) ||
        !copy_field(out->id, sizeof(out->id), id) ||
        !next(&j, "input_rules") || !skip(&j, 0) ||
        !next(&j, "name") || !copy_field(out->name, sizeof(out->name), string(&j)) ||
        !next(&j, "published_at") || !copy_field(out->published_at, sizeof(out->published_at), string(&j)) ||
        !next(&j, "revision") || !skip(&j, 0) || !next(&j, "targets") || !take(&j, '{'))
        return false;

    do {
        const char *scope = string(&j), *target;
        if (!porter_lot(scope) || !take(&j, ':') || !porter_hex(target = string(&j), 64) ||
            (out->count && os64_strcmp(out->targets[out->count - 1].scope, scope) >= 0) ||
            !porter_grow((void **)&out->targets, &cap, out->count, sizeof(*out->targets), PORTER_RECORDS))
            return false;
        porter_selection_t *slot = &out->targets[out->count++];
        os64_strcopy(slot->scope, sizeof(slot->scope), scope);
        os64_strcopy(slot->target, sizeof(slot->target), target);
        if (take(&j, '}'))
            return next(&j, "toolchain") && skip(&j, 0) && next(&j, "version") &&
                   number(&j, &version) && version == 1 && take(&j, '}') && j.p == j.end;
    } while (take(&j, ','));
    return false;
}
