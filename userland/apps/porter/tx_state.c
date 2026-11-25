// Durable transaction records and exact snapshots of mutable head/stamp files.
#include "transaction.h"
#include "os64/os64.h"
#include "os64/slurp.h"
#include "os64/syscall.h"

static const char tx_magic[8] = "PORTERTX";

int64_t porter_claim(os64_porter_owner_t *owner)
{
    return (int64_t)os64_syscall2(SYSCALL_PORTER_LEASE, OS64_PORTER_ACQUIRE, (uint64_t)owner);
}

void porter_unclaim(void)
{
    os64_syscall2(SYSCALL_PORTER_LEASE, OS64_PORTER_RELEASE, 0);
}

bool porter_barrier(void)
{
    return (int64_t)os64_syscall0(SYSCALL_STORAGE_BARRIER) == 0;
}

bool porter_tx_mkdirs(const char *path)
{
    porter_kind_t kind;
    if (!porter_probe(path, false, &kind))
        return false;
    if (kind != PORTER_ABSENT)
        return kind == PORTER_DIR;
    char parent[PORTER_PATH];
    return porter_parent(parent, path) && porter_tx_mkdirs(parent) && os64_mkdir(path) == 0;
}

bool porter_tx_write_exact(int32_t fd, const void *data, size_t size)
{
    const uint8_t *next = data;
    while (size) {
        size_t n = size < 65536 ? size : 65536;
        // Short writes from the ext2 file path can report ENOSPC. Do
        // not retry past a failed transaction write.
        if (os64_write(fd, next, n) != (int64_t)n)
            return false;
        next += n;
        size -= n;
    }
    return true;
}

static bool metadata_temp(const char *path, const char *id, char temp[PORTER_PATH])
{
    const char *root = path;
    for (; *root; root++)
        if (os64_strlen(root) >= 9 && os64_memcmp(root, "/.porter/", 9) == 0)
            break;
    if (!*root)
        return false;
    char directory[PORTER_PATH], digest[PORTER_DIGEST], name[120];
    size_t prefix = (size_t)(root - path) + 8;
    os64_memcpy(directory, path, prefix);
    directory[prefix] = 0;
    porter_sha(path, os64_strlen(path), digest);
    os64_snprintf(name, sizeof(name), "temp-%s-%s", id, digest);
    return porter_join(temp, directory, name);
}

void porter_tx_discard_metadata_temp(const char *path, const char *id)
{
    char temp[PORTER_PATH];
    porter_kind_t kind;
    if (metadata_temp(path, id, temp) && porter_probe(temp, false, &kind) && kind == PORTER_FILE)
        os64_unlink(temp);
}

// Same-volume temporaries live in the reserved state directory so a cut
// cannot introduce a false scope name into the active heads directory.
bool porter_tx_write(const char *path, const char *id, const void *data, size_t size, bool replace)
{
    porter_kind_t kind;
    if (!porter_probe(path, false, &kind) || kind == PORTER_DIR)
        return false;
    if (kind == PORTER_FILE) {
        uint8_t *old;
        size_t length;
        if (os64_slurp(path, PORTER_METADATA, &old, &length) != OS64_SLURP_OK)
            return false;
        bool same = length == size && os64_memcmp(old, data, size) == 0;
        os64_free(old);
        if (same)
            return true;
        if (!replace)
            return false;
    }
    char tmp[PORTER_PATH];
    if (!metadata_temp(path, id, tmp) || !porter_probe(tmp, false, &kind) ||
        (kind != PORTER_ABSENT && (kind != PORTER_FILE || os64_unlink(tmp) < 0)))
        return false;
    int64_t opened = os64_open(tmp, "x");
    if (opened < 0)
        return false;
    int32_t fd = (int32_t)opened;
    bool ok = porter_tx_write_exact(fd, data, size) && os64_sync(fd) == 0;
    if (os64_close(fd) < 0)
        ok = false;
    uint64_t flags = replace ? OS64_RENAME_REQUIRE_ATOMIC_REPLACE : OS64_RENAME_NOREPLACE;
    // Require atomic replacement only for an existing destination.
    if (!porter_probe(path, false, &kind) || kind == PORTER_DIR)
        return false;
    if (kind == PORTER_ABSENT)
        flags = OS64_RENAME_NOREPLACE;
    return ok && os64_rename_with_flags(tmp, path, flags) == 0;
}

bool porter_tx_value(const char *path, porter_tx_value_t *value)
{
    *value = (porter_tx_value_t){0};
    porter_kind_t kind;
    if (!porter_probe(path, false, &kind) || kind == PORTER_DIR)
        return false;
    if (kind == PORTER_ABSENT)
        return true;
    uint8_t *data;
    size_t size;
    if (os64_slurp(path, sizeof(value->data), &data, &size) != OS64_SLURP_OK)
        return false;
    value->present = 1;
    value->size = (uint32_t)size;
    os64_memcpy(value->data, data, size);
    os64_free(data);
    return true;
}

bool porter_tx_value_equal(const porter_tx_value_t *a, const porter_tx_value_t *b)
{
    return a->present == b->present && a->size == b->size &&
           os64_memcmp(a->data, b->data, a->size) == 0;
}

void porter_tx_id_value(porter_tx_value_t *value, const char *id)
{
    *value = (porter_tx_value_t){0};
    if (id && *id) {
        value->present = 1;
        value->size = PORTER_DIGEST;
        os64_memcpy(value->data, id, PORTER_DIGEST - 1);
        value->data[PORTER_DIGEST - 1] = '\n';
    }
}

bool porter_tx_value_id(const porter_tx_value_t *value, char id[PORTER_DIGEST])
{
    *id = 0;
    if (!value->present)
        return true;
    if (value->size != PORTER_DIGEST || value->data[PORTER_DIGEST - 1] != '\n')
        return false;
    os64_memcpy(id, value->data, PORTER_DIGEST - 1);
    id[PORTER_DIGEST - 1] = 0;
    return porter_hex(id, PORTER_DIGEST - 1);
}

bool porter_tx_set_value(const char *path, const char *id, const porter_tx_value_t *value)
{
    porter_tx_value_t actual;
    if (!porter_tx_value(path, &actual))
        return false;
    if (porter_tx_value_equal(&actual, value))
        return true;
    if (!value->present)
        return os64_unlink(path) == 0;
    char parent[PORTER_PATH];
    return porter_parent(parent, path) && porter_tx_mkdirs(parent) &&
           porter_tx_write(path, id, value->data, value->size, true);
}

static bool op_path(const char *id, const char *name, char path[PORTER_PATH])
{
    return porter_hex(id, PORTER_TX_ID - 1) &&
           os64_snprintf(path, PORTER_PATH, PORTER_TX_OPERATIONS "/%s/%s", id, name) < PORTER_PATH;
}

void porter_tx_init(porter_transaction_t *tx, porter_tx_kind_t kind, const os64_porter_owner_t *owner)
{
    *tx = (porter_transaction_t){0};
    os64_memcpy(tx->h.magic, tx_magic, sizeof(tx_magic));
    tx->h.version = 1;
    tx->h.kind = kind;
    tx->h.owner = *owner;
    os64_snprintf(tx->h.id, sizeof(tx->h.id), "%016lx%016lx%016lx",
                 owner->boot[0], owner->boot[1], owner->acquisition);
}

void porter_tx_free(porter_transaction_t *tx)
{
    os64_free(tx->changes);
    os64_free(tx->stamps);
    *tx = (porter_transaction_t){0};
}

static size_t record_size(const porter_tx_header_t *h)
{
    if (h->change_count > PORTER_TX_CHANGES || h->stamp_count > PORTER_MOUNTS)
        return 0;
    return sizeof(*h) + h->change_count * sizeof(porter_tx_change_t) +
           h->stamp_count * sizeof(porter_tx_stamp_t);
}

bool porter_tx_store(porter_transaction_t *tx)
{
    char path[PORTER_PATH], parent[PORTER_PATH];
    size_t total = record_size(&tx->h);
    if (!total || total > PORTER_METADATA || !op_path(tx->h.id, "intent", path) ||
        !porter_parent(parent, path) || !porter_tx_mkdirs(parent))
        return false;
    uint8_t *raw = os64_malloc(total);
    if (!raw)
        return false;
    os64_memcpy(raw, &tx->h, sizeof(tx->h));
    size_t changes = tx->h.change_count * sizeof(*tx->changes);
    size_t stamps = tx->h.stamp_count * sizeof(*tx->stamps);
    if (changes)
        os64_memcpy(raw + sizeof(tx->h), tx->changes, changes);
    if (stamps)
        os64_memcpy(raw + sizeof(tx->h) + changes, tx->stamps, stamps);
    porter_sha(raw, total, tx->digest);
    bool ok = porter_tx_write(path, tx->h.id, raw, total, false);
    os64_free(raw);
    return ok;
}

static bool terminated(const char *text, size_t size)
{
    for (size_t i = 0; i < size; i++)
        if (!text[i])
            return true;
    return false;
}

static bool valid_identity(const porter_identity_t *identity)
{
    if (identity->kind == PORTER_FILE)
        return identity->size <= UINT32_MAX && identity->hash[PORTER_DIGEST - 1] == 0 &&
               porter_hex(identity->hash, PORTER_DIGEST - 1);
    return (identity->kind == PORTER_DIR || identity->kind == PORTER_ABSENT) &&
           !identity->size && !*identity->hash;
}

static bool valid_value(const porter_tx_value_t *value)
{
    return value->present <= 1 && value->size <= sizeof(value->data) &&
           (value->present || !value->size);
}

static bool validate_record(const porter_transaction_t *tx)
{
    const porter_tx_header_t *h = &tx->h;
    if (os64_memcmp(h->magic, tx_magic, sizeof(tx_magic)) || h->version != 1 ||
        (h->kind != PORTER_TX_INSTALL && h->kind != PORTER_TX_UNDO) ||
        !terminated(h->scope, sizeof(h->scope)) || !porter_lot(h->scope) ||
        h->target[PORTER_DIGEST - 1] || !porter_hex(h->target, PORTER_DIGEST - 1) ||
        !valid_value(&h->old_head) || !valid_value(&h->next_head) || !valid_value(&h->undo_head) ||
        h->source[PORTER_TX_ID - 1] ||
        !porter_hex(h->source, PORTER_TX_ID - 1))
        return false;
    if (os64_streq(h->scope, "system")) {
        if (h->kernel[PORTER_DIGEST - 1] || !porter_hex(h->kernel, PORTER_DIGEST - 1) ||
            h->kernel_volume[PORTER_GUID - 1] || !porter_guid(h->kernel_volume) ||
            !terminated(h->kernel_path, sizeof(h->kernel_path)) || !porter_component_path(h->kernel_path, true))
            return false;
    } else if (*h->kernel || *h->kernel_volume || *h->kernel_path) {
        return false;
    }
    if (*h->backup_path &&
        (h->backup_volume[PORTER_GUID - 1] || !porter_guid(h->backup_volume) ||
         !terminated(h->backup_path, sizeof(h->backup_path)) || !porter_component_path(h->backup_path, true)))
        return false;
    for (uint32_t i = 0; i < h->change_count; i++) {
        const porter_tx_change_t *change = &tx->changes[i];
        if (change->volume[PORTER_GUID - 1] || !porter_guid(change->volume) ||
            !terminated(change->path, sizeof(change->path)) || !porter_component_path(change->path, true) ||
            os64_streq(change->path, "/") || !valid_identity(&change->before) ||
            !valid_identity(&change->after) || porter_identity_equal(&change->before, &change->after) ||
            change->flags & ~PORTER_TX_KEEP_DIRECTORY ||
            (change->flags && (h->kind != PORTER_TX_UNDO || change->before.kind != PORTER_ABSENT ||
                               change->after.kind != PORTER_DIR)))
            return false;
    }
    for (uint32_t i = 0; i < h->stamp_count; i++) {
        const porter_tx_stamp_t *stamp = &tx->stamps[i];
        if (stamp->volume[PORTER_GUID - 1] || !porter_guid(stamp->volume) ||
            !valid_value(&stamp->old) || !valid_value(&stamp->next) || !valid_value(&stamp->undo))
            return false;
    }
    return true;
}

bool porter_tx_read(porter_transaction_t *tx, const char *id, const char *digest)
{
    *tx = (porter_transaction_t){0};
    char path[PORTER_PATH];
    if (!op_path(id, "intent", path))
        return false;
    uint8_t *raw;
    size_t size;
    if (os64_slurp(path, PORTER_METADATA, &raw, &size) != OS64_SLURP_OK)
        return false;
    porter_sha(raw, size, tx->digest);
    bool ok = (!digest || os64_streq(digest, tx->digest)) && size >= sizeof(tx->h);
    if (ok) {
        os64_memcpy(&tx->h, raw, sizeof(tx->h));
        ok = record_size(&tx->h) == size && !tx->h.id[PORTER_TX_ID - 1] && os64_streq(tx->h.id, id);
    }
    if (ok && tx->h.change_count) {
        size_t n = tx->h.change_count * sizeof(*tx->changes);
        tx->changes = os64_malloc(n);
        ok = tx->changes != NULL;
        if (ok)
            os64_memcpy(tx->changes, raw + sizeof(tx->h), n);
    }
    if (ok && tx->h.stamp_count) {
        size_t n = tx->h.stamp_count * sizeof(*tx->stamps);
        tx->stamps = os64_malloc(n);
        ok = tx->stamps != NULL;
        if (ok)
            os64_memcpy(tx->stamps, raw + size - n, n);
    }
    ok = ok && validate_record(tx);
    os64_free(raw);
    if (!ok)
        porter_tx_free(tx);
    return ok;
}

bool porter_tx_pointer(char id[PORTER_TX_ID], char digest[PORTER_DIGEST], bool *exists)
{
    porter_tx_value_t v;
    if (!porter_tx_value(PORTER_TX_POINTER, &v))
        return false;
    *exists = v.present;
    if (!v.present)
        return true;
    if (v.size != PORTER_TX_ID + PORTER_DIGEST ||
        v.data[PORTER_TX_ID - 1] != ' ' || v.data[v.size - 1] != '\n')
        return false;
    os64_memcpy(id, v.data, PORTER_TX_ID - 1);
    id[PORTER_TX_ID - 1] = 0;
    os64_memcpy(digest, v.data + PORTER_TX_ID, PORTER_DIGEST - 1);
    digest[PORTER_DIGEST - 1] = 0;
    return porter_hex(id, PORTER_TX_ID - 1) && porter_hex(digest, PORTER_DIGEST - 1);
}

bool porter_tx_publish_pointer(const porter_transaction_t *tx)
{
    char line[PORTER_TX_VALUE];
    int length = os64_snprintf(line, sizeof(line), "%s %s\n", tx->h.id, tx->digest);
    return porter_tx_write(PORTER_TX_POINTER, tx->h.id, line, (size_t)length, false);
}

bool porter_tx_marker(const porter_transaction_t *tx, const char *name, bool *exists, bool *valid)
{
    char path[PORTER_PATH], digest[PORTER_DIGEST];
    porter_tx_value_t value;
    if (!op_path(tx->h.id, name, path) || !porter_tx_value(path, &value))
        return false;
    *exists = value.present;
    *valid = *exists && porter_tx_value_id(&value, digest) && os64_streq(digest, tx->digest);
    return true;
}

bool porter_tx_publish_marker(const porter_transaction_t *tx, const char *name)
{
    char path[PORTER_PATH];
    porter_tx_value_t value;
    porter_tx_id_value(&value, tx->digest);
    return op_path(tx->h.id, name, path) &&
           porter_tx_write(path, tx->h.id, value.data, value.size, false);
}
