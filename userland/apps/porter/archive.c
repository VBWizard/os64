// A bounded tar.gz reader for approved incoming and local backup members.
#include "archive.h"
#include "transaction.h"
#include "os64/os64.h"
#include "bearssl_hash.h"
#include "gzip/gzip.h"
#include "../tar/tar_format.h"

#define ARCHIVE_CHUNK 65536

typedef struct {
    os64_gzip_t *gzip;
    porter_read_t read;
    void *source;
    const uint8_t *next;
    size_t remaining;
    bool eof, done;
    uint8_t buffer[ARCHIVE_CHUNK];
} archive_reader_t;

typedef struct {
    os64_gzip_encoder_t *gzip;
    int32_t fd;
    uint8_t buffer[ARCHIVE_CHUNK];
} archive_writer_t;

static uint64_t padding(uint64_t size)
{
    return (TAR_BLOCK_SIZE - size % TAR_BLOCK_SIZE) % TAR_BLOCK_SIZE;
}

uint64_t porter_tar_size(const porter_member_t *members, size_t count)
{
    uint64_t sum = 2 * TAR_BLOCK_SIZE;
    for (size_t i = 0; i < count; i++) {
        uint64_t size = members[i].identity.size;
        if (size > TAR_USTAR_SIZE_MAX)
            return 0;
        sum += TAR_BLOCK_SIZE + size + padding(size);
    }
    return sum;
}

static int64_t decode(archive_reader_t *r, void *buffer, size_t size)
{
    uint8_t *out = buffer;
    size_t left = size;
    while (left && !r->done) {
        if (!r->remaining && !r->eof) {
            int64_t n = r->read(r->source, r->buffer, sizeof(r->buffer));
            if (n < 0 || n > (int64_t)sizeof(r->buffer))
                return -1;
            r->eof = n == 0;
            r->remaining = (size_t)n;
            r->next = r->buffer;
        }
        os64_gzip_status_t status = os64_gzip_process(r->gzip, &r->next, &r->remaining,
                                                     &out, &left, r->eof);
        if (status == OS64_GZIP_DONE)
            r->done = true;
        else if ((status != OS64_GZIP_NEED_INPUT && status != OS64_GZIP_NEED_OUTPUT) ||
                 (status == OS64_GZIP_NEED_INPUT && r->eof))
            return -1;
    }
    return (int64_t)(size - left);
}

static bool zero_bytes(const uint8_t *p, size_t size)
{
    for (size_t i = 0; i < size; i++)
        if (p[i])
            return false;
    return true;
}

static bool hash_matches(const br_sha256_context *sha, const char *expected)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char value[32];
    br_sha256_out(sha, value);
    for (size_t i = 0; i < sizeof(value); i++)
        if (expected[2 * i] != hex[value[i] >> 4] ||
            expected[2 * i + 1] != hex[value[i] & 15])
            return false;
    return true;
}

static bool decode_member(archive_reader_t *r, const porter_member_t *member, uint8_t *buffer)
{
    uint8_t header[TAR_BLOCK_SIZE];
    tar_entry_t entry;
    if (decode(r, header, sizeof(header)) != (int64_t)sizeof(header) ||
        tar_header_decode(header, &entry) != TAR_FORMAT_OK ||
        entry.type != TAR_TYPE_REGULAR || entry.size != member->identity.size ||
        !os64_streq(entry.path, member->name))
        return false;
    int64_t fd = -1;
    if (*member->path && (fd = os64_open(member->path, "x")) < 0)
        return false;
    br_sha256_context sha;
    br_sha256_init(&sha);
    bool ok = true;
    for (uint64_t left = entry.size; left && ok;) {
        size_t n = left < ARCHIVE_CHUNK ? (size_t)left : ARCHIVE_CHUNK;
        ok = decode(r, buffer, n) == (int64_t)n;
        if (ok) {
            br_sha256_update(&sha, buffer, n);
            if (fd >= 0)
                ok = porter_tx_write_exact((int32_t)fd, (const char *)buffer, n);
        }
        left -= n;
    }
    size_t tail = (size_t)padding(entry.size);
    ok = ok && hash_matches(&sha, member->identity.hash) &&
         decode(r, buffer, tail) == (int64_t)tail && zero_bytes(buffer, tail);
    if (fd >= 0) {
        if (ok && os64_sync((int32_t)fd) < 0)
            ok = false;
        if (os64_close((int32_t)fd) < 0)
            ok = false;
    }
    return ok;
}

bool porter_archive_decode(porter_read_t read, void *source,
                           const porter_member_t *members, size_t count)
{
    uint64_t size = porter_tar_size(members, count);
    if (!size)
        return false;
    archive_reader_t *r = os64_malloc(sizeof(*r));
    uint8_t *buffer = os64_malloc(ARCHIVE_CHUNK);
    bool ok = r && buffer;
    if (ok) {
        *r = (archive_reader_t){.gzip = os64_gzip_create(size), .read = read, .source = source};
        ok = r->gzip != NULL;
        for (size_t i = 0; ok && i < count; i++)
            ok = decode_member(r, &members[i], buffer);
        ok = ok && decode(r, buffer, 2 * TAR_BLOCK_SIZE) == 2 * TAR_BLOCK_SIZE &&
             zero_bytes(buffer, 2 * TAR_BLOCK_SIZE) && decode(r, buffer, 1) == 0 && r->done;
        if (r->gzip)
            os64_gzip_destroy(r->gzip);
    }
    os64_free(buffer);
    os64_free(r);
    return ok;
}

static bool encode(archive_writer_t *w, const uint8_t *input, size_t size, bool finish)
{
    for (;;) {
        uint8_t *out = w->buffer;
        size_t remaining = sizeof(w->buffer);
        os64_gzip_encode_status_t status = os64_gzip_encoder_process(w->gzip, &input, &size,
                                                                     &out, &remaining, finish);
        if (!porter_tx_write_exact(w->fd, (const char *)w->buffer, sizeof(w->buffer) - remaining))
            return false;
        if (status == OS64_GZIP_ENCODE_DONE)
            return finish && !size;
        if (status == OS64_GZIP_ENCODE_NEED_INPUT)
            return !finish && !size;
        if (status != OS64_GZIP_ENCODE_NEED_OUTPUT || remaining == sizeof(w->buffer))
            return false;
    }
}

static bool encode_member(archive_writer_t *w, const porter_member_t *member, uint8_t *buffer)
{
    uint8_t header[TAR_BLOCK_SIZE];
    tar_entry_t entry = {.size = member->identity.size, .mode = 0644, .type = TAR_TYPE_REGULAR};
    os64_strcopy(entry.path, sizeof(entry.path), member->name);
    if (tar_header_encode(header, &entry) != TAR_FORMAT_OK || !encode(w, header, sizeof(header), false))
        return false;
    int64_t fd = os64_open(member->path, "r");
    if (fd < 0)
        return false;
    br_sha256_context sha;
    br_sha256_init(&sha);
    bool ok = true;
    for (uint64_t left = entry.size; left && ok;) {
        size_t wanted = left < ARCHIVE_CHUNK ? (size_t)left : ARCHIVE_CHUNK;
        int64_t n = os64_read((int32_t)fd, buffer, wanted);
        if (n <= 0 || n > (int64_t)wanted) {
            ok = false;
            break;
        }
        br_sha256_update(&sha, buffer, (size_t)n);
        ok = encode(w, buffer, (size_t)n, false);
        left -= (uint64_t)n;
    }
    ok = ok && os64_read((int32_t)fd, buffer, 1) == 0 && hash_matches(&sha, member->identity.hash);
    if (os64_close((int32_t)fd) < 0)
        ok = false;
    size_t tail = (size_t)padding(entry.size);
    os64_memset(buffer, 0, tail);
    return ok && encode(w, buffer, tail, false);
}

bool porter_archive_backup(const char *path, const porter_member_t *members, size_t count)
{
    archive_writer_t *w = os64_malloc(sizeof(*w));
    uint8_t *buffer = os64_malloc(ARCHIVE_CHUNK);
    bool ok = w && buffer;
    if (ok) {
        w->gzip = os64_gzip_encoder_create(0);
        int64_t fd = w->gzip ? os64_open(path, "x") : -1;
        ok = fd >= 0;
        w->fd = (int32_t)fd;
        for (size_t i = 0; ok && i < count; i++)
            ok = encode_member(w, &members[i], buffer);
        os64_memset(buffer, 0, 2 * TAR_BLOCK_SIZE);
        ok = ok && encode(w, buffer, 2 * TAR_BLOCK_SIZE, true) && os64_sync((int32_t)fd) == 0;
        if (fd >= 0 && os64_close((int32_t)fd) < 0)
            ok = false;
        if (w->gzip)
            os64_gzip_encoder_destroy(w->gzip);
    }
    os64_free(buffer);
    os64_free(w);
    return ok;
}

static int64_t read_file(void *context, void *buffer, size_t size)
{
    return os64_read(*(int32_t *)context, buffer, size);
}

bool porter_archive_local(const char *path, const porter_member_t *members, size_t count)
{
    int64_t opened = os64_open(path, "r");
    if (opened < 0)
        return false;
    int32_t fd = (int32_t)opened;
    bool ok = porter_archive_decode(read_file, &fd, members, count);
    return os64_close(fd) == 0 && ok;
}
