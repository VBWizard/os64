/* host_blob_stub.c — dom.h's blob URL entry (BLOB_URLS.md) for a host
 * test of a yonder job that carries one, without libdom's engine: an
 * entry is made by host_blob_new and counted as libdom counts it (the last
 * release frees it). The store and its takes are libdom's, tested there
 * (tools/test_dom_blob.inc); a job only holds, reads and releases. */

#include "dom/dom.h"
#include <stdlib.h>
#include <string.h>

struct os64_dom_blob {
    uint32_t refs;
    char *type;
    uint8_t *bytes;
    size_t length;
};

os64_dom_blob_t *host_blob_new(const void *bytes, size_t length, const char *type);

os64_dom_blob_t *host_blob_new(const void *bytes, size_t length, const char *type)
{
    os64_dom_blob_t *b = calloc(1, sizeof(*b));
    if (b == NULL) return NULL;
    b->refs = 1;
    const char *t = type != NULL ? type : "";
    b->type = malloc(strlen(t) + 1);
    if (b->type != NULL) memcpy(b->type, t, strlen(t) + 1);
    b->bytes = malloc(length > 0 ? length : 1);
    if (b->type == NULL || b->bytes == NULL) abort();
    if (length > 0) memcpy(b->bytes, bytes, length);
    b->length = length;
    return b;
}

os64_dom_blob_t *os64_dom_blob_hold(os64_dom_blob_t *blob)
{
    if (blob != NULL) __atomic_fetch_add(&blob->refs, 1, __ATOMIC_RELAXED);
    return blob;
}

void os64_dom_blob_release(os64_dom_blob_t *blob)
{
    if (blob == NULL || __atomic_sub_fetch(&blob->refs, 1, __ATOMIC_ACQ_REL) != 0) return;
    free(blob->type);
    free(blob->bytes);
    free(blob);
}

const uint8_t *os64_dom_blob_bytes(const os64_dom_blob_t *blob) { return blob->bytes; }
size_t os64_dom_blob_length(const os64_dom_blob_t *blob) { return blob->length; }
const char *os64_dom_blob_type(const os64_dom_blob_t *blob) { return blob->type; }
