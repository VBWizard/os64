// Canonical inventories remain readable by the preview and history code.
#include "tx_receipt.h"
#include "os64/os64.h"

static bool text(porter_receipt_text_t *r, const char *part)
{
    size_t n = os64_strlen(part);
    if (n > PORTER_METADATA - r->length)
        return false;
    size_t need = r->length + n;
    while (need > r->capacity)
        if (!porter_grow((void **)&r->data, &r->capacity, r->capacity, 1, PORTER_METADATA))
            return false;
    os64_memcpy(r->data + r->length, part, n);
    r->length = need;
    return true;
}

static bool escape_codepoint(porter_receipt_text_t *r, unsigned value)
{
    char escaped[16];
    if (value < 0x10000) {
        os64_snprintf(escaped, sizeof(escaped), "\\u%04x", value);
    } else {
        value -= 0x10000;
        os64_snprintf(escaped, sizeof(escaped), "\\u%04x\\u%04x",
                      0xd800 + (value >> 10), 0xdc00 + (value & 1023));
    }
    return text(r, escaped);
}

static bool quoted(porter_receipt_text_t *r, const char *value)
{
    if (!text(r, "\""))
        return false;
    for (const unsigned char *p = (const unsigned char *)value; *p;) {
        unsigned c = *p++;
        if (c < 32 || c == 127) {
            if (!escape_codepoint(r, c))
                return false;
        } else if (c < 128) {
            char escaped[3] = {(char)c, 0, 0};
            if (c == '"' || c == '\\') {
                escaped[0] = '\\';
                escaped[1] = (char)c;
            }
            if (!text(r, escaped))
                return false;
        } else {
            unsigned length = c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
            if (c < 0xc2 || c > 0xf4)
                return false;
            unsigned code = c & (0x7f >> length);
            for (unsigned i = 1; i < length; i++) {
                if ((*p & 0xc0) != 0x80)
                    return false;
                code = (code << 6) | (*p++ & 0x3f);
            }
            if (code < (length == 2 ? 0x80u : length == 3 ? 0x800u : 0x10000u) ||
                (code >= 0xd800 && code <= 0xdfff) || code > 0x10ffff ||
                !escape_codepoint(r, code))
                return false;
        }
    }
    return text(r, "\"");
}

static bool emit_entry(porter_receipt_text_t *r, const porter_entry_t *e,
                       const porter_mount_t *mount, const porter_volumes_t *volumes)
{
    if (!mount || !mount->persistent_ext2 || !mount->writable || r->entries == PORTER_RECORDS)
        return false;
    r->volumes[mount - volumes->mounts] = true;
    if ((r->entries++ && !text(r, ",")) ||
        !text(r, "{\"kind\":") || !quoted(r, e->identity.kind == PORTER_DIR ? "dir" : "file") ||
        !text(r, ",\"lot\":") || !quoted(r, e->lot) ||
        !text(r, ",\"path\":") || !quoted(r, e->path) ||
        !text(r, ",\"relative\":") || !quoted(r, e->relative))
        return false;
    if (e->identity.kind == PORTER_FILE) {
        char size[28];
        os64_snprintf(size, sizeof(size), "%lu", e->identity.size);
        if (!text(r, ",\"sha256\":") || !quoted(r, e->identity.hash) ||
            !text(r, ",\"size\":") || !text(r, size))
            return false;
    }
    return text(r, ",\"volume\":") && quoted(r, e->volume) && text(r, "}");
}

static int lexical(const porter_entry_t *a, const porter_entry_t *b)
{
    int order = os64_strcmp(a->lot, b->lot);
    return order ? order : os64_strcmp(a->path, b->path);
}

static bool preserve_old_entry(const porter_inventory_t *target, const porter_plan_t *plan,
                               const porter_entry_t *entry)
{
    if (plan->adoption)
        return false;
    if (!porter_has_lot(target, entry->lot))
        return true;
    if (entry->identity.kind == PORTER_DIR)
        for (size_t i = 0; i < plan->count; i++) {
            const porter_plan_item_t *row = &plan->items[i];
            if (row->action == PORTER_PRESERVE && row->where.mount &&
                os64_streq(row->where.mount->guid, entry->volume) &&
                os64_streq(row->where.relative, entry->relative))
                return true;
        }
    return false;
}

bool porter_build_receipt(porter_receipt_text_t *r, const porter_transaction_t *tx,
                          const porter_inventory_t *target, const porter_inventory_t *older,
                          const porter_plan_t *plan, const porter_volumes_t *volumes)
{
    *r = (porter_receipt_text_t){0};
    size_t old = 0, cursor = 0;
    bool ok = text(r, "{\"entries\":[");
    for (size_t i = 0; ok && (i < target->entry_count || old < older->entry_count);) {
        const porter_entry_t *entry = i < target->entry_count ? &target->entries[i] : NULL;
        const porter_entry_t *prior = old < older->entry_count ? &older->entries[old] : NULL;
        int order = !prior ? -1 : !entry ? 1 : lexical(entry, prior);
        if (order >= 0)
            old++;
        if (order > 0) {
            if (preserve_old_entry(target, plan, prior))
                ok = emit_entry(r, prior, porter_mount_by_guid(volumes, prior->volume), volumes);
            continue;
        }
        i++;
        while (cursor < plan->count && plan->items[cursor].entry != entry)
            cursor++;
        if (cursor == plan->count || !plan->items[cursor].where.mount)
            return false;
        const porter_location_t *where = &plan->items[cursor++].where;
        if (where->mount->fat)
            continue; // Unchanged boot data is a dependency in the ext2 writer.
        porter_entry_t routed = *entry;
        routed.volume = where->mount->guid;
        routed.relative = where->relative;
        ok = emit_entry(r, &routed, where->mount, volumes);
    }
    char previous[PORTER_DIGEST];
    ok = ok && porter_tx_value_id(&tx->h.undo_head, previous) &&
         text(r, "],\"operation\":") && quoted(r, tx->h.id) && text(r, ",\"previous\":") &&
         (*previous ? quoted(r, previous) : text(r, "null")) &&
         text(r, ",\"scope\":") && quoted(r, tx->h.scope) &&
         text(r, ",\"target\":") && quoted(r, tx->h.target) && text(r, ",\"version\":2}");
    if (ok)
        porter_sha(r->data, r->length, r->digest);
    return ok;
}

bool porter_publish_receipt(const porter_receipt_text_t *receipt, const char *operation)
{
    char name[PORTER_PATH], path[PORTER_PATH];
    os64_snprintf(name, sizeof(name), "%s.json", receipt->digest);
    return porter_tx_mkdirs(PORTER_TX_RECEIPTS) &&
           porter_join(path, PORTER_TX_RECEIPTS, name) &&
           porter_tx_write(path, operation, receipt->data, receipt->length, false);
}
