#include "internal.h"
#include "os64/fmt.h"

/* WEB STORAGE (HTML § 12.2): localStorage and sessionStorage, the Storage
 * interface over a map of strings, kept per origin by the host's provider.
 * dom.h has the contract; this file is the map, the store's bytes, the
 * provider's protocol and the Storage object's exotic shape (WebIDL's
 * legacy platform object with named properties).
 *
 * WHAT A PAGE SEES IS ITS MAP, AND THE STORE IS THE TRUTH. An area loads
 * the store at its first touch in a task when the store has moved since it
 * last loaded it, and keeps a CHANGE LIST beside its map: every key this
 * page set or removed since its last commit, the last word on each. A load
 * replays the change list over what it read, so a page never loses sight
 * of its own writes, and a commit hands over the change list alone, which
 * the host merges onto the store as the store then stands: two windows on
 * one origin lose nothing of each other's but the same key written by
 * both, where the later commit wins. clear() is a removal of every key the
 * page could see, so it cannot take away a key another window added after.
 *
 * An area's map and change list are not the binding's budget's: each is
 * held to the quota (OS64_DOM_STORAGE_QUOTA) and to OS64_DOM_STORAGE_KEYS
 * keys, and freed with the binding. A setItem past either throws; a change
 * list that would pass either is committed early instead, since what it
 * holds is the page's own writes and removals. The keys' cap bounds what
 * the quota cannot: a key of one character costs the quota two bytes and
 * the heap a record, and a page could otherwise make millions of them. */

#define MAGIC "os64 storage 1\n"
#define MAGIC_LENGTH (sizeof(MAGIC) - 1)
/* Keys an area may hold; one more is QuotaExceededError. */
#define OS64_DOM_STORAGE_KEYS 65536
/* A store read longer than this is not one this binding wrote: every image
 * it writes is held to the quota, which no key can make three times larger
 * in UTF-8 than in UTF-16, plus each record's own few bytes. */
#define IMAGE_MAX ((size_t)3 * OS64_DOM_STORAGE_QUOTA + (size_t)OS64_DOM_STORAGE_KEYS * 48 + MAGIC_LENGTH)
/* The first load's room; a bigger store is asked for again with its own. */
#define LOAD_FIRST (16 * 1024)
#define LOAD_TRIES 4

static JSClassID storage_slot;

typedef struct {
    char *bytes;            /* the key, then the value: one allocation */
    size_t key_length, value_length;
    uint32_t hash;
    bool removed;           /* a change list's: the key was removed */
} DItem;

/* The items in the order their keys were first set, which is key(n)'s
 * order, and an open-addressed index over them: a slot holds an item's
 * position plus one, 0 when empty, and there are at least twice as many
 * slots as items can be. A removal shifts the items after it, so the index
 * is rebuilt then; nothing else moves an item. */
typedef struct {
    DItem *items;
    size_t count, cap;
    uint32_t *slots;
    size_t nslots;
    size_t used;            /* the quota's count: two bytes per UTF-16 unit
                               of every key and value it holds */
} DMap;

typedef struct DStorageArea {
    os64_dom_t *dom;
    os64_dom_storage_area_t which;
    DMap map, changes;
    DValue *object;
    char origin[OS64_URL_SCHEME_MAX + OS64_URL_HOST_MAX + 16];
    /* `memory`: the area keeps its map for the page's life and asks no
     * provider (an opaque origin, or a store that failed). `loaded`: the map
     * holds what the store held at `identity`. `asked`: this task has asked
     * the provider already. */
    bool memory, loaded, asked, committed;
    uint64_t identity, committed_at;
} DStorageArea;

/* ── The map ─────────────────────────────────────────────────────────── */

static uint32_t hash_of(const char *key, size_t length)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; i++) hash = (hash ^ (unsigned char)key[i]) * 16777619u;
    return hash;
}

/* The UTF-16 code units of WTF-8 text: two for a four-byte sequence, one
 * for any other. */
static size_t units(const char *text, size_t length)
{
    size_t count = 0;
    for (size_t at = 0; at < length; at++) {
        unsigned char c = (unsigned char)text[at];
        if ((c & 0xc0) != 0x80) count += c >= 0xf0 ? 2 : 1;
    }
    return count;
}

static size_t item_cost(size_t key_length, const char *key, size_t value_length, const char *value)
{
    return 2 * (units(key, key_length) + (value != NULL ? units(value, value_length) : 0));
}

/* The slot that holds `key`, or the empty slot where it would go. */
static size_t map_slot(const DMap *map, const char *key, size_t length, uint32_t hash)
{
    size_t mask = map->nslots - 1, at = hash & mask;
    for (;; at = (at + 1) & mask) {
        uint32_t held = map->slots[at];
        if (held == 0) return at;
        const DItem *item = &map->items[held - 1];
        if (item->hash == hash && item->key_length == length && os64_memcmp(item->bytes, key, length) == 0)
            return at;
    }
}

static ptrdiff_t map_find(const DMap *map, const char *key, size_t length, uint32_t hash)
{
    if (map->count == 0) return -1;
    uint32_t held = map->slots[map_slot(map, key, length, hash)];
    return held != 0 ? (ptrdiff_t)held - 1 : -1;
}

static void map_index(DMap *map)
{
    os64_memset(map->slots, 0, map->nslots * sizeof(*map->slots));
    for (size_t i = 0; i < map->count; i++) {
        const DItem *item = &map->items[i];
        map->slots[map_slot(map, item->bytes, item->key_length, item->hash)] = (uint32_t)i + 1;
    }
}

/* Room for `more` items beyond those held, so the inserts that follow
 * cannot fail. */
static bool map_reserve_n(DMap *map, size_t more)
{
    if (more <= map->cap - map->count) return true;
    if (more > (size_t)OS64_DOM_STORAGE_KEYS * 4 - map->count) return false;
    size_t cap = map->cap != 0 ? map->cap : 8;
    while (cap < map->count + more) cap *= 2;
    size_t nslots = 16;
    while (nslots < cap * 2) nslots *= 2;
    DItem *items = os64_malloc(cap * sizeof(*items));
    uint32_t *slots = items != NULL ? os64_malloc(nslots * sizeof(*slots)) : NULL;
    if (slots == NULL) {
        os64_free(items);
        return false;
    }
    if (map->count != 0) os64_memcpy(items, map->items, map->count * sizeof(*items));
    os64_free(map->items);
    os64_free(map->slots);
    map->items = items;
    map->cap = cap;
    map->slots = slots;
    map->nslots = nslots;
    map_index(map);
    return true;
}

static bool map_reserve(DMap *map)
{
    return map_reserve_n(map, 1);
}

/* A new item's bytes: the key, then the value (none for a removal). */
static bool item_make(DItem *item, const char *key, size_t key_length, const char *value, size_t value_length)
{
    size_t length = key_length + (value != NULL ? value_length : 0);
    char *bytes = os64_malloc(length != 0 ? length : 1);
    if (bytes == NULL) return false;
    os64_memcpy(bytes, key, key_length);
    if (value != NULL && value_length != 0) os64_memcpy(bytes + key_length, value, value_length);
    *item = (DItem){bytes, key_length, value != NULL ? value_length : 0, hash_of(key, key_length), value == NULL};
    return true;
}

/* A removal (a change list's) costs its key: the list holds it until the
 * commit. */
static size_t cost_of(const DItem *item)
{
    return item_cost(item->key_length, item->bytes, item->value_length,
                     item->removed ? NULL : item->bytes + item->key_length);
}

/* Puts a made item in its key's place, or at the end for a new key; the
 * map has room (map_reserve). */
static void map_put(DMap *map, DItem item)
{
    size_t slot = map_slot(map, item.bytes, item.key_length, item.hash);
    map->used += cost_of(&item);
    if (map->slots[slot] == 0) {
        map->items[map->count++] = item;
        map->slots[slot] = (uint32_t)map->count;
        return;
    }
    DItem *old = &map->items[map->slots[slot] - 1];
    map->used -= cost_of(old);
    os64_free(old->bytes);
    *old = item;
}

static void map_remove(DMap *map, size_t at)
{
    map->used -= cost_of(&map->items[at]);
    os64_free(map->items[at].bytes);
    os64_memmove(map->items + at, map->items + at + 1, (map->count - at - 1) * sizeof(*map->items));
    map->count--;
    map_index(map);
}

static void map_free(DMap *map)
{
    for (size_t i = 0; i < map->count; i++) os64_free(map->items[i].bytes);
    os64_free(map->items);
    os64_free(map->slots);
    *map = (DMap){0};
}

/* One change applied to a map: a removal takes the key out, a set puts it
 * in. False on no memory, with the map as it was. */
static bool map_apply(DMap *map, const char *key, size_t key_length, const char *value, size_t value_length)
{
    if (value == NULL) {
        ptrdiff_t at = map_find(map, key, key_length, hash_of(key, key_length));
        if (at >= 0) map_remove(map, (size_t)at);
        return true;
    }
    DItem item;
    if (!map_reserve(map) || !item_make(&item, key, key_length, value, value_length)) return false;
    map_put(map, item);
    return true;
}

/* ── The store's bytes ───────────────────────────────────────────────── */

/* A decimal length at `*at`, ended by `stop`; false for anything else. */
static bool read_length(const char *bytes, size_t length, size_t *at, char stop, size_t *out)
{
    size_t value = 0, digits = 0;
    while (*at < length && bytes[*at] >= '0' && bytes[*at] <= '9') {
        if (++digits > 12) return false;
        value = value * 10 + (size_t)(bytes[*at] - '0');
        (*at)++;
    }
    if (digits == 0 || *at >= length || bytes[*at] != stop) return false;
    (*at)++;
    *out = value;
    return true;
}

bool os64_dom_storage_walk(const void *records, size_t length,
                           bool (*each)(void *opaque, const char *key, size_t key_length,
                                        const char *value, size_t value_length),
                           void *opaque)
{
    const char *bytes = records;
    if (length < MAGIC_LENGTH || os64_memcmp(bytes, MAGIC, MAGIC_LENGTH) != 0) return false;
    size_t at = MAGIC_LENGTH;
    while (at < length) {
        char kind = bytes[at++];
        size_t key_length, value_length = 0;
        if (kind != '+' && kind != '-') return false;
        if (kind == '+' ? !read_length(bytes, length, &at, ' ', &key_length) ||
                              !read_length(bytes, length, &at, '\n', &value_length)
                        : !read_length(bytes, length, &at, '\n', &key_length))
            return false;
        if (key_length > length - at || value_length > length - at - key_length ||
            length - at - key_length - value_length < 1 || bytes[at + key_length + value_length] != '\n')
            return false;
        const char *key = bytes + at;
        const char *value = kind == '+' ? key + key_length : NULL;
        at += key_length + value_length + 1;
        if (each != NULL && !each(opaque, key, key_length, value, value_length)) return true;
    }
    return true;
}

typedef struct {
    DMap *map;
    bool sets_only, failed;
} Loading;

static bool load_record(void *opaque, const char *key, size_t key_length, const char *value, size_t value_length)
{
    Loading *loading = opaque;
    if (loading->sets_only && value == NULL) {
        loading->failed = true;
        return false;
    }
    if (!map_apply(loading->map, key, key_length, value, value_length)) {
        loading->failed = true;
        return false;
    }
    return true;
}

/* An image's records into an empty map. False when it is not in the
 * format, holds a removal, or there was no memory. */
static bool map_read(DMap *map, const void *image, size_t length)
{
    if (length == 0) return true;
    Loading loading = {map, true, false};
    return os64_dom_storage_walk(image, length, load_record, &loading) && !loading.failed;
}

/* A map as records: its sets, and with `changes` its removals too.
 * os64_malloc'd, NULL on no memory or past what a length can hold. */
static char *map_write(const DMap *map, size_t *length)
{
    size_t need = MAGIC_LENGTH;
    char number[48];
    for (size_t i = 0; i < map->count; i++) {
        const DItem *item = &map->items[i];
        int n = item->removed ? os64_snprintf(number, sizeof(number), "-%lu\n", (unsigned long)item->key_length)
                              : os64_snprintf(number, sizeof(number), "+%lu %lu\n",
                                              (unsigned long)item->key_length,
                                              (unsigned long)item->value_length);
        size_t record = (size_t)n + item->key_length + item->value_length + 1;
        if (record > SIZE_MAX - need) return NULL;
        need += record;
    }
    char *out = os64_malloc(need);
    if (out == NULL) return NULL;
    os64_memcpy(out, MAGIC, MAGIC_LENGTH);
    size_t at = MAGIC_LENGTH;
    for (size_t i = 0; i < map->count; i++) {
        const DItem *item = &map->items[i];
        int n = item->removed ? os64_snprintf(out + at, need - at, "-%lu\n", (unsigned long)item->key_length)
                              : os64_snprintf(out + at, need - at, "+%lu %lu\n",
                                              (unsigned long)item->key_length,
                                              (unsigned long)item->value_length);
        at += (size_t)n;
        os64_memcpy(out + at, item->bytes, item->key_length + item->value_length);
        at += item->key_length + item->value_length;
        out[at++] = '\n';
    }
    *length = need;
    return out;
}

os64_dom_storage_status_t os64_dom_storage_merge(const void *image, size_t image_length,
                                                 const void *changes, size_t changes_length,
                                                 void **out, size_t *out_length)
{
    if (out != NULL) *out = NULL;
    if (out_length != NULL) *out_length = 0;
    if (out == NULL || out_length == NULL || (image == NULL && image_length != 0) || changes == NULL)
        return OS64_DOM_STORAGE_FAILED;
    DMap map = {0};
    if (!map_read(&map, image, image_length)) {
        map_free(&map);
        return OS64_DOM_STORAGE_FAILED;
    }
    size_t was_used = map.used, was_count = map.count;
    Loading applying = {&map, false, false};
    if (!os64_dom_storage_walk(changes, changes_length, load_record, &applying) || applying.failed) {
        map_free(&map);
        return OS64_DOM_STORAGE_FAILED;
    }
    if ((map.used > OS64_DOM_STORAGE_QUOTA && map.used > was_used) ||
        (map.count > OS64_DOM_STORAGE_KEYS && map.count > was_count)) {
        map_free(&map);
        return OS64_DOM_STORAGE_FULL;
    }
    os64_dom_storage_status_t status = OS64_DOM_STORAGE_OK;
    if (map.count != 0) {
        *out = map_write(&map, out_length);
        if (*out == NULL) status = OS64_DOM_STORAGE_FAILED;
    }
    map_free(&map);
    return status;
}

/* ── The area and its provider ───────────────────────────────────────── */

static uint64_t now_ms(const os64_dom_t *dom)
{
    return dom->options.now_ms != NULL ? dom->options.now_ms(dom->options.host_opaque) : 0;
}

static bool kept(const os64_dom_t *dom, const DStorageArea *area)
{
    return !area->memory && dom->storage_providers[area->which].load != NULL;
}

/* The area, made at its first touch: an opaque origin keeps its map in
 * memory alone. NULL on no memory. */
static DStorageArea *area_of(os64_dom_t *dom, os64_dom_storage_area_t which)
{
    if (dom->storage[which] != NULL) return dom->storage[which];
    DStorageArea *area = os64_calloc(1, sizeof(*area));
    if (area == NULL) return NULL;
    area->dom = dom;
    area->which = which;
    if (!d_origin(dom->url != NULL ? dom->url : "about:blank", area->origin, sizeof(area->origin)))
        os64_strcopy(area->origin, sizeof(area->origin), "null");
    area->memory = os64_streq(area->origin, "null");
    dom->storage[which] = area;
    return area;
}

static bool only_sets(void *opaque, const char *key, size_t key_length, const char *value, size_t value_length)
{
    (void)key; (void)key_length; (void)value_length;
    *(bool *)opaque = value != NULL;
    return value != NULL;
}

/* Whether bytes are an image: in the format, and nothing but sets. */
static bool image_valid(const void *image, size_t length)
{
    bool sets = true;
    return length == 0 || (os64_dom_storage_walk(image, length, only_sets, &sets) && sets);
}

/* The store read into a fresh map, this page's changes replayed over it,
 * and the map replaced. False with the map as it was: on no memory, or a
 * store still growing past its room at the last asking (both tried again
 * at the next task), or when the store could not be read or was not in
 * the format (`*broken`, and the area keeps to memory). */
static bool area_load(DStorageArea *area, bool *broken)
{
    os64_dom_t *dom = area->dom;
    const os64_dom_storage_t *provider = &dom->storage_providers[area->which];
    *broken = false;
    size_t room = LOAD_FIRST;
    char *image;
    int64_t length;
    /* A store that grows between the asking and the reading is asked
     * again with the room it answered, a few times. */
    for (int tries = 1;; tries++) {
        image = os64_malloc(room);
        if (image == NULL) return false;
        length = provider->load(provider->opaque, area->origin, image, room);
        if (length < 0 || (uint64_t)length > IMAGE_MAX) {
            os64_free(image);
            *broken = true;
            return false;
        }
        if ((uint64_t)length <= room) break;
        os64_free(image);
        if (tries == LOAD_TRIES) return false;
        room = (size_t)length;
    }
    if (!image_valid(image, (size_t)length)) {
        os64_free(image);
        *broken = true;
        return false;
    }
    DMap map = {0};
    bool read = map_read(&map, image, (size_t)length);
    os64_free(image);
    for (size_t i = 0; read && i < area->changes.count; i++) {
        const DItem *change = &area->changes.items[i];
        read = map_apply(&map, change->bytes, change->key_length,
                         change->removed ? NULL : change->bytes + change->key_length, change->value_length);
    }
    if (!read) {
        map_free(&map);
        return false;
    }
    map_free(&area->map);
    area->map = map;
    return true;
}

/* The area a script is about to read or change, brought up to the store
 * at the first touch in a task. NULL on no memory, with the exception
 * thrown. */
static DStorageArea *area_touch(os64_dom_t *dom, JSContext *ctx, os64_dom_storage_area_t which)
{
    DStorageArea *area = area_of(dom, which);
    if (area == NULL) {
        d_error(ctx, "QuotaExceededError", "Storage quota exceeded");
        return NULL;
    }
    if (!kept(dom, area) || area->asked) return area;
    area->asked = true;
    const os64_dom_storage_t *provider = &dom->storage_providers[which];
    uint64_t identity = provider->identity != NULL ? provider->identity(provider->opaque, area->origin) : 0;
    if (area->loaded && identity == area->identity) return area;
    bool broken;
    if (area_load(area, &broken)) {
        area->loaded = true;
        area->identity = identity;
    } else if (broken) {
        area->memory = true;
        map_free(&area->changes);
    }
    return area;
}

/* The change list handed to the provider, and forgotten: the page sees
 * the store as it then stands at its next touch, which may hold another
 * window's changes merged in. A store that could not be written leaves the
 * area to memory; one that was full drops the changes, so the page reads
 * back what the store kept. Changes that could not be written down for
 * want of memory wait for the next commit. */
static void area_commit(DStorageArea *area)
{
    os64_dom_t *dom = area->dom;
    if (!kept(dom, area) || area->changes.count == 0) return;
    size_t length;
    char *changes = map_write(&area->changes, &length);
    if (changes == NULL) return;
    const os64_dom_storage_t *provider = &dom->storage_providers[area->which];
    os64_dom_storage_status_t status = provider->commit != NULL
        ? provider->commit(provider->opaque, area->origin, changes, length) : OS64_DOM_STORAGE_FAILED;
    os64_free(changes);
    area->committed = true;
    area->committed_at = now_ms(dom);
    map_free(&area->changes);
    if (status == OS64_DOM_STORAGE_FAILED) area->memory = true;
    else area->loaded = false;
}

static uint64_t area_due(const DStorageArea *area)
{
    if (area == NULL || !kept(area->dom, area) || area->changes.count == 0) return UINT64_MAX;
    if (!area->committed) return 0;
    return area->committed_at > UINT64_MAX - OS64_DOM_STORAGE_COMMIT_MS
        ? UINT64_MAX - 1 : area->committed_at + OS64_DOM_STORAGE_COMMIT_MS;
}

void os64_dom_set_storage(os64_dom_t *dom, os64_dom_storage_area_t area, const os64_dom_storage_t *storage)
{
    if (dom == NULL || (unsigned)area >= OS64_DOM_STORAGE_AREAS) return;
    if (storage != NULL) dom->storage_providers[area] = *storage;
    else os64_memset(&dom->storage_providers[area], 0, sizeof(dom->storage_providers[area]));
    if (dom->storage[area] != NULL) {
        dom->storage[area]->loaded = false;
        dom->storage[area]->asked = false;
    }
}

void os64_dom_storage_settle(os64_dom_t *dom)
{
    if (dom == NULL) return;
    uint64_t now = now_ms(dom);
    for (int i = 0; i < OS64_DOM_STORAGE_AREAS; i++) {
        DStorageArea *area = dom->storage[i];
        if (area == NULL) continue;
        area->asked = false;
        if (area_due(area) <= now) area_commit(area);
    }
}

uint64_t os64_dom_storage_due(const os64_dom_t *dom)
{
    if (dom == NULL) return UINT64_MAX;
    uint64_t due = UINT64_MAX;
    for (int i = 0; i < OS64_DOM_STORAGE_AREAS; i++) {
        uint64_t one = area_due(dom->storage[i]);
        if (one < due) due = one;
    }
    return due;
}

void os64_dom_storage_forget(os64_dom_t *dom)
{
    if (dom == NULL) return;
    for (int i = 0; i < OS64_DOM_STORAGE_AREAS; i++) {
        DStorageArea *area = dom->storage[i];
        if (area == NULL) continue;
        map_free(&area->map);
        map_free(&area->changes);
        area->loaded = area->asked = false;
    }
}

void d_storage_drain(os64_dom_t *dom)
{
    for (int i = 0; i < OS64_DOM_STORAGE_AREAS; i++)
        if (dom->storage[i] != NULL) area_commit(dom->storage[i]);
}

void d_storage_free(os64_dom_t *dom)
{
    for (int i = 0; i < OS64_DOM_STORAGE_AREAS; i++) {
        DStorageArea *area = dom->storage[i];
        if (area == NULL) continue;
        map_free(&area->map);
        map_free(&area->changes);
        os64_free(area);
        dom->storage[i] = NULL;
    }
}

/* ── The Storage interface ───────────────────────────────────────────── */

/* Room in the change list for `count` more records costing `cost`. A list
 * that would pass the quota or the keys' cap is committed now rather than
 * when it is due: a page that sets and removes key after key in one task
 * would otherwise hold every key it removed until its next commit. */
static void changes_room(DStorageArea *area, size_t count, size_t cost)
{
    const DMap *changes = &area->changes;
    if (changes->count != 0 && kept(area->dom, area) &&
        (count > OS64_DOM_STORAGE_KEYS - (changes->count < OS64_DOM_STORAGE_KEYS ? changes->count
                                                                                  : OS64_DOM_STORAGE_KEYS) ||
         cost > OS64_DOM_STORAGE_QUOTA - (changes->used < OS64_DOM_STORAGE_QUOTA ? changes->used
                                                                                 : OS64_DOM_STORAGE_QUOTA)))
        area_commit(area);
}

/* A change, made to the map and remembered for the commit together, or
 * not at all: false on no memory, with neither touched. */
static bool area_change(DStorageArea *area, const char *key, size_t key_length, const char *value,
                        size_t value_length)
{
    changes_room(area, map_find(&area->changes, key, key_length, hash_of(key, key_length)) < 0 ? 1 : 0,
                 item_cost(key_length, key, value_length, value));
    bool remember = kept(area->dom, area);
    DItem in_map = {0}, in_changes = {0};
    if (value != NULL && (!map_reserve(&area->map) || !item_make(&in_map, key, key_length, value, value_length)))
        return false;
    if (remember && (!map_reserve(&area->changes) ||
                     !item_make(&in_changes, key, key_length, value, value_length))) {
        os64_free(in_map.bytes);
        return false;
    }
    if (value != NULL) map_put(&area->map, in_map);
    else {
        ptrdiff_t at = map_find(&area->map, key, key_length, hash_of(key, key_length));
        if (at >= 0) map_remove(&area->map, (size_t)at);
    }
    if (remember) map_put(&area->changes, in_changes);
    return true;
}

/* clear(): a removal of each key the page can see, remembered for the
 * commit, so a key another window adds before it is not taken with them.
 * Every removal is made before any is kept: false on no memory, with
 * nothing changed. */
static bool area_clear(DStorageArea *area)
{
    size_t count = area->map.count;
    if (count == 0) return true;
    size_t cost = 0;
    for (size_t i = 0; i < count; i++) {
        const DItem *item = &area->map.items[i];
        cost += item_cost(item->key_length, item->bytes, 0, NULL);
    }
    changes_room(area, count, cost);
    if (kept(area->dom, area)) {
        DItem *made = os64_malloc(count * sizeof(*made));
        if (made == NULL || !map_reserve_n(&area->changes, count)) {
            os64_free(made);
            return false;
        }
        for (size_t i = 0; i < count; i++) {
            const DItem *item = &area->map.items[i];
            if (!item_make(&made[i], item->bytes, item->key_length, NULL, 0)) {
                while (i != 0) os64_free(made[--i].bytes);
                os64_free(made);
                return false;
            }
        }
        for (size_t i = 0; i < count; i++) map_put(&area->changes, made[i]);
        os64_free(made);
    }
    map_free(&area->map);
    return true;
}

static DStorageArea *receiver(JSContext *ctx, JSValueConst object)
{
    DStorageArea *area = JS_GetOpaque(object, os64_js_class_id(&storage_slot));
    if (area == NULL) {
        JS_ThrowTypeError(ctx, "Expected a Storage");
        return NULL;
    }
    return d_context(area->dom, ctx, OS64_JS_ABI_ID) ? area : NULL;
}

/* A property key as the string it names: 1 (with `out`), 0 for a Symbol,
 * -1 with an exception pending. */
static int key_text(os64_dom_t *dom, JSContext *ctx, JSAtom atom, DString *out)
{
    JSValue key = JS_AtomToValue(ctx, atom);
    if (JS_IsException(key)) return -1;
    if (JS_IsSymbol(key)) { JS_FreeValue(ctx, key); return 0; }
    bool ok = d_string_text(dom, ctx, key, out, false);
    JS_FreeValue(ctx, key);
    return ok ? 1 : -1;
}

/* WebIDL's named property visibility: a stored key names a property of
 * the object only when the object's prototype chain has none by that name
 * (Storage is not [LegacyOverrideBuiltIns]), so `localStorage.getItem` is
 * always the method. 1 visible, 0 not, -1 on an exception. */
static int visible(JSContext *ctx, JSValueConst object, JSAtom atom)
{
    JSValue prototype = JS_GetPrototype(ctx, object);
    if (JS_IsException(prototype)) return -1;
    int shadowed = JS_IsNull(prototype) ? 0 : JS_HasProperty(ctx, prototype, atom);
    JS_FreeValue(ctx, prototype);
    return shadowed < 0 ? -1 : !shadowed;
}

/* The item a property key names, visible or not: its index in the touched
 * area's map, -1 when there is none, -2 on an exception. */
static ptrdiff_t named(DStorageArea *area, JSContext *ctx, JSAtom atom, bool *symbol)
{
    DString key = {0};
    int is = key_text(area->dom, ctx, atom, &key);
    *symbol = is == 0;
    if (is <= 0) return is < 0 ? -2 : -1;
    DStorageArea *touched = area_touch(area->dom, ctx, area->which);
    ptrdiff_t at = touched == NULL ? -2 : map_find(&area->map, key.data, key.length, hash_of(key.data, key.length));
    d_string_free(area->dom, &key);
    return at;
}

static JSValue item_value(JSContext *ctx, const DItem *item)
{
    return JS_NewStringLen(ctx, item->bytes + item->key_length, item->value_length);
}

static int storage_own_property(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst object, JSAtom atom)
{
    DStorageArea *area = receiver(ctx, object);
    if (area == NULL) return -1;
    bool symbol;
    ptrdiff_t at = named(area, ctx, atom, &symbol);
    if (at < 0) return at == -1 ? 0 : -1;
    int shown = visible(ctx, object, atom);
    if (shown <= 0) return shown;
    if (desc != NULL) {
        /* The map may have moved under a getter run by `visible`. */
        if ((size_t)at >= area->map.count) return 0;
        JSValue value = item_value(ctx, &area->map.items[at]);
        if (JS_IsException(value)) return -1;
        *desc = (JSPropertyDescriptor){.flags = JS_PROP_C_W_E, .value = value,
                                       .getter = JS_UNDEFINED, .setter = JS_UNDEFINED};
    }
    return 1;
}

static int storage_own_names(JSContext *ctx, JSPropertyEnum **table, uint32_t *length, JSValueConst object)
{
    DStorageArea *area = receiver(ctx, object);
    if (area == NULL || area_touch(area->dom, ctx, area->which) == NULL) return -1;
    size_t count = area->map.count;
    JSPropertyEnum *names = count != 0 ? js_malloc(ctx, count * sizeof(*names)) : NULL;
    if (count != 0 && names == NULL) return -1;
    uint32_t used = 0;
    for (size_t i = 0; i < count && i < area->map.count; i++) {
        const DItem *item = &area->map.items[i];
        JSAtom atom = JS_NewAtomLen(ctx, item->bytes, item->key_length);
        int shown = atom == JS_ATOM_NULL ? -1 : visible(ctx, object, atom);
        if (shown < 0) {
            if (atom != JS_ATOM_NULL) JS_FreeAtom(ctx, atom);
            while (used != 0) JS_FreeAtom(ctx, names[--used].atom);
            js_free(ctx, names);
            return -1;
        }
        if (!shown) { JS_FreeAtom(ctx, atom); continue; }
        names[used].is_enumerable = true;
        names[used++].atom = atom;
    }
    *table = names;
    *length = used;
    return 0;
}

/* setItem's steps (HTML § 12.2.1), the named setter's too: the same value
 * is no change, and a value that would pass the quota changes nothing. */
static JSValue set_item(DStorageArea *area, JSContext *ctx, const DString *key, const DString *value)
{
    if (area_touch(area->dom, ctx, area->which) == NULL) return JS_EXCEPTION;
    ptrdiff_t at = map_find(&area->map, key->data, key->length, hash_of(key->data, key->length));
    size_t old = 0;
    if (at >= 0) {
        const DItem *item = &area->map.items[at];
        if (item->value_length == value->length &&
            os64_memcmp(item->bytes + item->key_length, value->data, value->length) == 0)
            return JS_UNDEFINED;
        old = cost_of(item);
    } else if (area->map.count >= OS64_DOM_STORAGE_KEYS) {
        return d_error(ctx, "QuotaExceededError", "Storage holds as many keys as it may");
    }
    size_t cost = item_cost(key->length, key->data, value->length, value->data);
    if (area->map.used - old > OS64_DOM_STORAGE_QUOTA || cost > OS64_DOM_STORAGE_QUOTA - (area->map.used - old))
        return d_error(ctx, "QuotaExceededError", "Storage quota exceeded");
    if (!area_change(area, key->data, key->length, value->data, value->length))
        return d_error(ctx, "QuotaExceededError", "Storage quota exceeded");
    return JS_UNDEFINED;
}

static JSValue remove_item(DStorageArea *area, JSContext *ctx, const DString *key)
{
    if (area_touch(area->dom, ctx, area->which) == NULL) return JS_EXCEPTION;
    if (map_find(&area->map, key->data, key->length, hash_of(key->data, key->length)) < 0) return JS_UNDEFINED;
    if (!area_change(area, key->data, key->length, NULL, 0))
        return d_error(ctx, "QuotaExceededError", "Storage quota exceeded");
    return JS_UNDEFINED;
}

/* WebIDL's [[DefineOwnProperty]] for a string key: a data descriptor is
 * the named setter given its value; an accessor, or a descriptor with
 * neither value nor writable, is refused. A Symbol is an ordinary property
 * of the object. */
static int storage_define(JSContext *ctx, JSValueConst object, JSAtom atom, JSValueConst value,
                          JSValueConst getter, JSValueConst setter, int flags)
{
    DStorageArea *area = receiver(ctx, object);
    if (area == NULL) return -1;
    DString key = {0};
    int is = key_text(area->dom, ctx, atom, &key);
    if (is < 0) return -1;
    if (is == 0) return JS_DefineProperty(ctx, object, atom, value, getter, setter, flags | JS_PROP_NO_EXOTIC);
    if ((flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET)) || !(flags & (JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE))) {
        d_string_free(area->dom, &key);
        if (flags & JS_PROP_THROW) {
            JS_ThrowTypeError(ctx, "A Storage item is a string, not an accessor");
            return -1;
        }
        return 0;
    }
    DString text = {0};
    int result = -1;
    if (d_string_text(area->dom, ctx, (flags & JS_PROP_HAS_VALUE) ? value : JS_UNDEFINED, &text, false)) {
        JSValue done = set_item(area, ctx, &key, &text);
        result = JS_IsException(done) ? -1 : 1;
        d_string_free(area->dom, &text);
    }
    d_string_free(area->dom, &key);
    return result;
}

/* WebIDL's [[Set]]: an assignment to the object itself under a string key
 * is the named setter, whatever the prototype has by that name. A Symbol,
 * or an assignment through an object that inherits from this one, is the
 * ordinary [[Set]], which goes on up the prototype chain to the receiver. */
static int storage_set(JSContext *ctx, JSValueConst object, JSAtom atom, JSValueConst value,
                       JSValueConst receiver_object, int flags)
{
    DStorageArea *area = receiver(ctx, object);
    if (area == NULL) return -1;
    DString key = {0};
    int is = key_text(area->dom, ctx, atom, &key);
    if (is < 0) return -1;
    if (is == 1 && JS_IsObject(receiver_object) &&
        JS_VALUE_GET_PTR(receiver_object) == JS_VALUE_GET_PTR(object)) {
        DString text = {0};
        int result = -1;
        if (d_string_text(area->dom, ctx, value, &text, false)) {
            result = JS_IsException(set_item(area, ctx, &key, &text)) ? -1 : 1;
            d_string_free(area->dom, &text);
        }
        d_string_free(area->dom, &key);
        return result;
    }
    if (is == 1) d_string_free(area->dom, &key);
    JSValue prototype = JS_GetPrototype(ctx, object);
    if (JS_IsException(prototype)) return -1;
    if (JS_IsNull(prototype)) {
        /* No chain to walk: the receiver gets an own data property. */
        return JS_DefineProperty(ctx, receiver_object, atom, value, JS_UNDEFINED, JS_UNDEFINED,
                                 JS_PROP_C_W_E | JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE |
                                 JS_PROP_HAS_ENUMERABLE | JS_PROP_HAS_CONFIGURABLE | (flags & JS_PROP_THROW));
    }
    int result = JS_SetPropertyInternal(ctx, prototype, atom, JS_DupValue(ctx, value), receiver_object, flags);
    JS_FreeValue(ctx, prototype);
    return result;
}

/* WebIDL's [[Delete]]: a visible named property is the deleter's
 * (removeItem); anything else is ordinary, and a property that is not
 * there deletes. */
static int storage_delete(JSContext *ctx, JSValueConst object, JSAtom atom)
{
    DStorageArea *area = receiver(ctx, object);
    if (area == NULL) return -1;
    bool symbol;
    ptrdiff_t at = named(area, ctx, atom, &symbol);
    if (at == -2) return -1;
    if (at < 0) return 1;
    int shown = visible(ctx, object, atom);
    if (shown <= 0) return shown < 0 ? -1 : 1;
    DString key = {0};
    if (key_text(area->dom, ctx, atom, &key) < 0) return -1;
    JSValue done = remove_item(area, ctx, &key);
    d_string_free(area->dom, &key);
    return JS_IsException(done) ? -1 : 1;
}

static JSClassExoticMethods storage_exotic = {
    .get_own_property = storage_own_property, .get_own_property_names = storage_own_names,
    .define_own_property = storage_define, .delete_property = storage_delete,
    .set_property = storage_set,
};

enum { S_LENGTH, S_KEY, S_GET, S_SET, S_REMOVE, S_CLEAR, S_LOCAL, S_SESSION };

static JSValue storage_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                              JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    magic &= ~D_SET;
    if (magic == S_LOCAL || magic == S_SESSION) {
        os64_dom_storage_area_t which = magic == S_LOCAL ? OS64_DOM_STORAGE_LOCAL : OS64_DOM_STORAGE_SESSION;
        DStorageArea *area = area_of(dom, which);
        if (area == NULL) return d_error(ctx, "QuotaExceededError", "Storage quota exceeded");
        if (area->object != NULL) return JS_DupValue(ctx, area->object->value);
        JSValue object = JS_NewObjectClass(ctx, (int)os64_js_class_id(&storage_slot));
        DValue *held = d_retain(dom, ctx, object);
        if (held == NULL) return JS_EXCEPTION;
        JS_SetOpaque(object, area);
        area->object = held;
        return JS_DupValue(ctx, object);
    }
    DStorageArea *area = receiver(ctx, self);
    if (area == NULL) return JS_EXCEPTION;
    static const int kNeeds[] = {0, 1, 1, 2, 1, 0};
    if (argc < kNeeds[magic])
        return JS_ThrowTypeError(ctx, "Storage's method needs %d argument%s", kNeeds[magic],
                                 kNeeds[magic] == 1 ? "" : "s");
    if (magic == S_KEY) {
        /* key(n): an unsigned long, so n is taken modulo 2^32. */
        uint32_t index;
        if (JS_ToUint32(ctx, &index, argv[0]) < 0) return JS_EXCEPTION;
        if (area_touch(dom, ctx, area->which) == NULL) return JS_EXCEPTION;
        if (index >= area->map.count) return JS_NULL;
        const DItem *item = &area->map.items[index];
        return JS_NewStringLen(ctx, item->bytes, item->key_length);
    }
    if (magic == S_LENGTH || magic == S_CLEAR) {
        if (area_touch(dom, ctx, area->which) == NULL) return JS_EXCEPTION;
        if (magic == S_LENGTH) return JS_NewInt64(ctx, (int64_t)area->map.count);
        if (!area_clear(area)) return d_error(ctx, "QuotaExceededError", "Storage quota exceeded");
        return JS_UNDEFINED;
    }
    DString key = {0}, value = {0};
    if (!d_string_text(dom, ctx, argv[0], &key, false)) return JS_EXCEPTION;
    if (magic == S_SET && !d_string_text(dom, ctx, argv[1], &value, false)) {
        d_string_free(dom, &key);
        return JS_EXCEPTION;
    }
    JSValue result = JS_UNDEFINED;
    if (magic == S_SET) result = set_item(area, ctx, &key, &value);
    else if (magic == S_REMOVE) result = remove_item(area, ctx, &key);
    else if (area_touch(dom, ctx, area->which) == NULL) result = JS_EXCEPTION;
    else {
        ptrdiff_t at = map_find(&area->map, key.data, key.length, hash_of(key.data, key.length));
        result = at < 0 ? JS_NULL : item_value(ctx, &area->map.items[at]);
    }
    d_string_free(dom, &key);
    d_string_free(dom, &value);
    return result;
}

static void storage_finalizer(JSRuntime *rt, JSValue value)
{
    (void)rt;
    JS_SetOpaque(value, NULL);
}

/* The row: `Storage`, an interface that constructs nothing, and window's
 * localStorage and sessionStorage, each its area's one object, made at its
 * first read. */
int d_storage_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    JSClassID id = os64_js_class_id(&storage_slot);
    static const JSClassDef definition = {.class_name = "Storage", .finalizer = storage_finalizer,
                                          .exotic = &storage_exotic};
    if (!JS_IsRegisteredClass(dom->engine, id) && JS_NewClass(dom->engine, id, &definition) < 0) return -1;
    JSValue prototype = JS_NewObject(ctx);
    if (JS_IsException(prototype)) return -1;
    DValue *held = d_retain(dom, ctx, prototype);
    if (held == NULL) return -1;
    JS_SetClassProto(ctx, id, JS_DupValue(ctx, prototype));
    static const struct { const char *name; int magic, argc; } kMethods[] = {
        {"key", S_KEY, 1}, {"getItem", S_GET, 1}, {"setItem", S_SET, 2},
        {"removeItem", S_REMOVE, 1}, {"clear", S_CLEAR, 0},
    };
    if (d_accessor(dom, ctx, prototype, "length", storage_member, S_LENGTH, false) < 0) return -1;
    for (size_t i = 0; i < sizeof(kMethods) / sizeof(kMethods[0]); i++)
        if (d_method(dom, ctx, prototype, kMethods[i].name, storage_member, kMethods[i].argc, kMethods[i].magic) < 0)
            return -1;
    JSValue f = d_interface_new(ctx, "Storage", 0, NULL, prototype, JS_UNDEFINED);
    if (JS_IsException(f) ||
        JS_DefinePropertyValueStr(ctx, global, "Storage", f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    return d_accessor(dom, ctx, global, "localStorage", storage_member, S_LOCAL, false) < 0 ||
           d_accessor(dom, ctx, global, "sessionStorage", storage_member, S_SESSION, false) < 0 ? -1 : 0;
}
