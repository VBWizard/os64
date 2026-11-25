// Porter metadata is fetched over the same listener as the legacy valet.
#include "porter.h"
#include "os64/os64.h"

#define REQUEST_MS 30000
#define WIRE_CHUNK 4096

typedef struct {
    int32_t fd;
    uint64_t started;
    size_t pos, length;
    char buf[WIRE_CHUNK];
} wire_t;

static uint64_t remaining(const wire_t *w)
{
    uint64_t elapsed = (os64_micros() - w->started) / 1000;
    return elapsed < REQUEST_MS ? REQUEST_MS - elapsed : 0;
}

static bool request(wire_t *wire, const char *host, uint16_t port, const char *line)
{
    char address[PORTER_PATH];
    if (!host || !*host || os64_strchr(host, '!') ||
        os64_snprintf(address, sizeof(address), "tcp!%s!%u", host, (unsigned)port) >= (int)sizeof(address))
        return false;
    int64_t opened = os64_dial(address);
    if (opened < 0)
        return false;
    *wire = (wire_t){.fd = (int32_t)opened, .started = os64_micros()};
    size_t len = os64_strlen(line);
    while (len) {
        int64_t n = os64_write_for(wire->fd, line, len, remaining(wire));
        if (n <= 0) {
            os64_close(wire->fd);
            return false;
        }
        line += n;
        len -= (size_t)n;
    }
    return true;
}

static int refill(wire_t *w)
{
    int64_t n = os64_read_for(w->fd, w->buf, sizeof(w->buf), remaining(w));
    w->pos = 0;
    w->length = n > 0 ? (size_t)n : 0;
    return n < 0 ? -1 : (int)n;
}

static bool line(wire_t *w, char **out, size_t *cap, size_t *length, size_t limit)
{
    *length = 0;
    for (;;) {
        if (w->pos == w->length && refill(w) <= 0)
            return false;
        char c = w->buf[w->pos++];
        if (!porter_grow((void **)out, cap, *length, sizeof(char), limit + 1))
            return false;
        if (c == '\n') {
            (*out)[*length] = 0;
            return true;
        }
        if (!c || *length == limit)
            return false;
        (*out)[(*length)++] = c;
    }
}

static bool complete(wire_t *w)
{
    return w->pos == w->length && refill(w) == 0;
}

static bool read_body(wire_t *w, char *dest, size_t length)
{
    while (length) {
        if (w->pos == w->length && refill(w) <= 0)
            return false;
        size_t n = w->length - w->pos;
        if (n > length)
            n = length;
        os64_memcpy(dest, w->buf + w->pos, n);
        dest += n;
        w->pos += n;
        length -= n;
    }
    return true;
}

bool porter_catalog(const char *host, uint16_t port,
                    bool (*visit)(const porter_publication_t *, void *), void *context)
{
    wire_t wire;
    if (!request(&wire, host, port, "CATALOG\n"))
        return false;
    char *row = NULL;
    size_t cap = 0, length, records = 0;
    bool ok = false;
    if (line(&wire, &row, &cap, &length, 128) && os64_streq(row, "OK")) {
        while (line(&wire, &row, &cap, &length, PORTER_METADATA)) {
            if (os64_streq(row, ".")) {
                ok = complete(&wire);
                break;
            }
            if (++records > PORTER_RECORDS)
                break;
            porter_publication_t pub;
            bool valid = porter_parse_publication(&pub, row, length) && visit(&pub, context);
            os64_free(pub.targets);
            if (!valid)
                break;
        }
    }
    os64_free(row);
    os64_close(wire.fd);
    return ok;
}

bool porter_get_manifest(const char *host, uint16_t port, const porter_selection_t *selection,
                         porter_inventory_t *inventory)
{
    *inventory = (porter_inventory_t){0};
    char query[80];
    os64_snprintf(query, sizeof(query), "MANIFEST %s\n", selection->target);
    wire_t wire;
    if (!request(&wire, host, port, query))
        return false;
    char *header = NULL;
    size_t cap = 0, length;
    uint64_t count;
    bool ok = false;
    if (line(&wire, &header, &cap, &length, 128) && length > 3 &&
        os64_memcmp(header, "OK ", 3) == 0 && os64_parse_u64(header + 3, &count) &&
        count > 0 && count <= PORTER_METADATA) {
        char *data = os64_malloc((size_t)count);
        if (data && read_body(&wire, data, (size_t)count) && complete(&wire)) {
            ok = porter_parse_inventory(inventory, data, (size_t)count, false) &&
                 os64_streq(inventory->digest, selection->target) &&
                 os64_streq(inventory->scope, selection->scope);
            data = NULL; // parser owns bytes even when validation fails
        }
        os64_free(data);
    }
    os64_free(header);
    os64_close(wire.fd);
    if (!ok)
        porter_release_inventory(inventory);
    return ok;
}
