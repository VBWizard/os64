// Compare the selected inventory, local ownership and actual mounted bytes.
#include "porter.h"
#include "os64/os64.h"
#include "os64/slurp.h"

#define PLAN_ROWS (4u * PORTER_RECORDS)
#define INDEX_LIMIT (8u * PORTER_RECORDS)
#define HISTORY_LIMIT 128

typedef struct {
    const porter_mount_t *mount;
    char path[PORTER_PATH];
    size_t row, short_row, long_row;
    const porter_entry_t *owner;
    bool selected_below, foreign, foreign_file, foreign_below, scanned, parents_checked;
} path_key_t;

typedef struct {
    path_key_t *keys;
    size_t capacity, count;
} paths_t;

typedef struct {
    porter_plan_t *plan;
    const porter_inventory_t *target;
    const porter_config_t *config;
    const porter_volumes_t *volumes;
    porter_inventory_t current;
    paths_t paths;
    bool old_valid;
    size_t scope_count;
} planner_t;

static size_t path_hash(const porter_mount_t *mount, const char *path)
{
    size_t h = (uintptr_t)mount;
    for (; *path; path++) {
        unsigned char c = (unsigned char)*path;
        if (mount->fat && c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        h = (h ^ c) * 1099511628211ULL;
    }
    return h ^ (h >> 32);
}

static path_key_t *slot(paths_t *paths, const porter_mount_t *mount, const char *path)
{
    size_t i = path_hash(mount, path) & (paths->capacity - 1);
    while (paths->keys[i].mount &&
           (paths->keys[i].mount != mount || !porter_folded_equal(paths->keys[i].path, path, mount->fat)))
        i = (i + 1) & (paths->capacity - 1);
    return &paths->keys[i];
}

// Callers keep row numbers across insertions because growth moves the table.
static path_key_t *path_key(paths_t *paths, const porter_mount_t *mount, const char *path, bool add)
{
    if (!mount || (!add && !paths->capacity))
        return NULL;
    if (add && paths->count >= paths->capacity / 2) {
        size_t n = paths->capacity ? paths->capacity * 2 : 64;
        if (n > INDEX_LIMIT)
            return NULL;
        path_key_t *keys = os64_malloc(n * sizeof(*keys));
        if (!keys)
            return NULL;
        os64_memset(keys, 0, n * sizeof(*keys));
        paths_t old = *paths;
        paths->keys = keys;
        paths->capacity = n;
        for (size_t i = 0; i < old.capacity; i++)
            if (old.keys[i].mount)
                *slot(paths, old.keys[i].mount, old.keys[i].path) = old.keys[i];
        os64_free(old.keys);
    }
    path_key_t *key = slot(paths, mount, path);
    if (!key->mount) {
        if (!add)
            return NULL;
        key->mount = mount;
        os64_strcopy(key->path, sizeof(key->path), path);
        paths->count++;
    }
    return key;
}

static porter_plan_item_t *add_item(porter_plan_t *plan)
{
    if (!porter_grow((void **)&plan->items, &plan->capacity, plan->count, sizeof(*plan->items), PLAN_ROWS))
        return NULL;
    porter_plan_item_t *item = &plan->items[plan->count++];
    *item = (porter_plan_item_t){0};
    return item;
}

static void conflict(porter_plan_item_t *item, const char *reason)
{
    item->action = PORTER_CONFLICT;
    if (!item->reason)
        item->reason = reason;
}

static void block(porter_plan_t *plan, const char *reason)
{
    if (!plan->blocker)
        plan->blocker = reason;
}

static bool same_place(const porter_location_t *where, const porter_entry_t *old)
{
    return where->mount && os64_streq(where->mount->guid, old->volume) &&
           porter_folded_equal(where->relative, old->relative, where->mount->fat);
}

static bool recorded_place(const planner_t *p, const porter_entry_t *entry,
                           porter_location_t *where, const char **why)
{
    const porter_mount_t *mount = porter_mount_by_guid(p->volumes, entry->volume);
    char path[PORTER_PATH];
    if (mount && porter_join(path, mount->where, entry->relative) &&
        porter_locate(p->volumes, path, where, why) && same_place(where, entry))
        return true;
    if (!*why)
        *why = "recorded destination volume or path is unavailable";
    return false;
}

typedef enum { READ_MISSING, READ_VALID, READ_INVALID } read_result_t;

static read_result_t read_id(const char *path, bool fat, char digest[PORTER_DIGEST])
{
    porter_kind_t kind;
    if (!porter_probe(path, fat, &kind))
        return READ_INVALID;
    if (kind == PORTER_ABSENT)
        return READ_MISSING;
    uint8_t *raw;
    size_t size;
    if (kind != PORTER_FILE || os64_slurp(path, PORTER_DIGEST, &raw, &size) != OS64_SLURP_OK)
        return READ_INVALID;
    bool ok = size == PORTER_DIGEST && raw[64] == '\n';
    if (ok) {
        raw[64] = 0;
        ok = porter_hex((char *)raw, 64);
    }
    if (ok)
        os64_strcopy(digest, PORTER_DIGEST, (char *)raw);
    os64_free(raw);
    return ok ? READ_VALID : READ_INVALID;
}

static bool read_receipt(porter_inventory_t *out, const char *id, const char *scope)
{
    char path[PORTER_PATH];
    os64_snprintf(path, sizeof(path), PORTER_STATE "/receipts/%s.json", id);
    uint8_t *raw;
    size_t size;
    if (os64_slurp(path, PORTER_METADATA, &raw, &size) != OS64_SLURP_OK)
        return false;
    bool ok = porter_parse_inventory(out, (char *)raw, size, true) &&
              os64_streq(out->digest, id) && os64_streq(out->scope, scope);
    if (!ok)
        porter_release_inventory(out);
    return ok;
}

static bool check_stamps(const planner_t *p, const porter_inventory_t *receipt, const char *scope)
{
    const porter_volumes_t *volumes = p->volumes;
    bool checked[PORTER_MOUNTS] = {0};
    for (size_t i = 0; receipt && i < receipt->entry_count; i++) {
        const porter_mount_t *m = porter_mount_by_guid(volumes, receipt->entries[i].volume);
        if (!m || (size_t)(m - volumes->mounts) >= sizeof(checked) / sizeof(*checked))
            return false;
        checked[m - volumes->mounts] = true;
    }
    for (size_t i = 0; i < volumes->count; i++) {
        const porter_mount_t *m = &volumes->mounts[i];
        if (!m->disk)
            continue;
        char path[PORTER_PATH], relative[PORTER_PATH], stamp[PORTER_DIGEST];
        os64_snprintf(relative, sizeof(relative), "/.porter/stamps/%s", scope);
        if (!porter_join(path, m->where, relative))
            return false;
        read_result_t result = read_id(path, m->fat, stamp);
        if (checked[i] ? result != READ_VALID || !os64_streq(receipt->digest, stamp)
                       : result != READ_MISSING)
            return false;
    }
    return true;
}

static bool index_receipt(planner_t *p, const porter_inventory_t *receipt, bool foreign)
{
    for (size_t i = 0; i < receipt->entry_count; i++) {
        const porter_entry_t *entry = &receipt->entries[i];
        const porter_mount_t *mount = porter_mount_by_guid(p->volumes, entry->volume);
        if (!mount)
            continue;
        path_key_t *k = path_key(&p->paths, mount, entry->relative, true);
        if (!k || k->owner || k->foreign)
            return false;
        k->owner = foreign ? NULL : entry;
        k->foreign = foreign;
        k->foreign_file = foreign && entry->identity.kind == PORTER_FILE;
        if (foreign) {
            char path[PORTER_PATH], parent[PORTER_PATH];
            os64_strcopy(path, sizeof(path), entry->relative);
            while (porter_parent(parent, path)) {
                k = path_key(&p->paths, mount, parent, true);
                if (!k)
                    return false;
                k->foreign_below = true;
                os64_strcopy(path, sizeof(path), parent);
            }
        }
    }
    return true;
}

static bool load_heads(planner_t *p, bool baseline, bool adopt)
{
    const porter_mount_t *home = porter_mount_at(p->volumes, PORTER_STATE);
    porter_kind_t kind;
    if (!home || !home->disk || !home->writable || !os64_streq(home->where, "/home") ||
        porter_mount_at(p->volumes, PORTER_STATE "/heads") != home ||
        porter_mount_at(p->volumes, PORTER_STATE "/receipts") != home ||
        !porter_probe(PORTER_STATE "/operation", home->fat, &kind)) {
        block(p->plan, "persistent Porter state volume is unavailable");
        return false;
    }
    if (kind != PORTER_ABSENT) {
        block(p->plan, "a Porter operation needs recovery before preview");
        return false;
    }

    char path[PORTER_PATH], id[PORTER_DIGEST];
    os64_snprintf(path, sizeof(path), PORTER_STATE "/heads/%s", p->target->scope);
    read_result_t result = read_id(path, home->fat, id);
    bool loaded = result == READ_VALID && read_receipt(&p->current, id, p->target->scope);
    bool clean = check_stamps(p, loaded ? &p->current : NULL, p->target->scope);
    p->old_valid = loaded && clean;
    if (loaded) {
        os64_strcopy(p->plan->installed, sizeof(p->plan->installed), p->current.target);
        os64_strcopy(p->plan->receipt, sizeof(p->plan->receipt), p->current.digest);
    }
    if (adopt && !baseline)
        block(p->plan, "--adopt requires a baseline publication");
    p->plan->adoption = baseline && (adopt || (result == READ_MISSING && clean));
    if (!p->old_valid && !p->plan->adoption)
        block(p->plan, result == READ_MISSING && clean
                          ? "first adoption requires a baseline publication"
                          : "receipt or volume stamp is missing or inconsistent; re-adopt a baseline");

    if (p->old_valid && !index_receipt(p, &p->current, false)) {
        block(p->plan, "local receipt has duplicate or excessive destinations");
        return false;
    }
    if (!porter_probe(PORTER_STATE "/heads", home->fat, &kind))
        return false;
    if (kind == PORTER_ABSENT)
        return true;
    if (kind != PORTER_DIR)
        return false;
    int64_t h = os64_opendir(PORTER_STATE "/heads");
    if (h < 0)
        return false;
    os64_dirent_t e;
    int64_t rc;
    bool ok = true;
    while ((rc = os64_readdir((int32_t)h, &e)) == 1) {
        if (os64_streq(e.name, ".") || os64_streq(e.name, "..") ||
            os64_streq(e.name, p->target->scope))
            continue;
        if (++p->scope_count > HISTORY_LIMIT || (e.flags & (OS64_DE_DIR | OS64_DE_MOUNT)) ||
            !porter_lot(e.name)) {
            ok = false;
            break;
        }
        porter_inventory_t other = {0};
        os64_snprintf(path, sizeof(path), PORTER_STATE "/heads/%s", e.name);
        ok = read_id(path, home->fat, id) == READ_VALID && read_receipt(&other, id, e.name) &&
             index_receipt(p, &other, true);
        porter_release_inventory(&other);
        if (!ok)
            break;
    }
    return os64_close((int32_t)h) == 0 && rc == 0 && ok;
}

static bool legal_sfn_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || os64_strchr("!#$%&'()-@^_`{}~", c);
}

static bool short_name(const char *name, bool *numeric_tail)
{
    const char *dot = os64_strchr(name, '.');
    size_t base = dot ? (size_t)(dot - name) : os64_strlen(name);
    if (!base || base > 8 || (dot && os64_strlen(dot + 1) > 3))
        return false;
    const char *tilde = NULL;
    for (const char *s = name; *s; s++) {
        if (s != dot && !legal_sfn_char((unsigned char)*s))
            return false;
        if (*s == '~' && (size_t)(s - name) < base)
            tilde = s;
    }
    *numeric_tail = tilde && tilde > name && tilde < name + base - 1;
    for (const char *s = tilde ? tilde + 1 : name + base; *numeric_tail && s < name + base; s++)
        *numeric_tail = *s >= '0' && *s <= '9';
    return true;
}

static bool alias_candidates(planner_t *p, const porter_mount_t *mount, const char *parent,
                             const char *name, size_t row)
{
    bool tail = false;
    bool short_form = short_name(name, &tail);
    if (short_form && !tail)
        return true;
    path_key_t *dir = path_key(&p->paths, mount, parent, true);
    if (!dir)
        return false;
    size_t *same = short_form ? &dir->short_row : &dir->long_row;
    size_t other = short_form ? dir->long_row : dir->short_row;
    if (!*same)
        *same = row;
    if (other) {
        const char *why = "proposed FAT long name and numeric short alias share a directory";
        conflict(&p->plan->items[row - 1], why);
        conflict(&p->plan->items[other - 1], why);
    }
    return true;
}

static bool register_selected(planner_t *p, size_t row, const porter_entry_t *entry)
{
    porter_plan_item_t *item = &p->plan->items[row - 1];
    const porter_mount_t *m = item->where.mount;
    const char *relative = item->where.relative;
    path_key_t *key = path_key(&p->paths, m, relative, true);
    if (!key)
        return false;
    if (key->foreign || (key->foreign_below && entry->identity.kind == PORTER_FILE))
        conflict(item, "destination overlaps another scope's receipt");
    if (key->owner && key->owner != porter_find(&p->current, entry->lot, entry->path))
        conflict(item, "destination is already owned by a different logical entry");
    if (key->row) {
        conflict(item, "selected entries resolve to the same destination");
        conflict(&p->plan->items[key->row - 1], "selected entries resolve to the same destination");
    } else {
        key->row = row;
    }
    if (key->selected_below && entry->identity.kind == PORTER_FILE)
        conflict(item, "selected file also serves as a directory");
    char path[PORTER_PATH], parent[PORTER_PATH];
    os64_strcopy(path, sizeof(path), relative);
    while (porter_parent(parent, path)) {
        key = path_key(&p->paths, m, parent, true);
        if (!key)
            return false;
        key->selected_below = true;
        if (key->foreign_file || (key->owner && key->owner->identity.kind == PORTER_FILE))
            conflict(item, "a destination directory is recorded as a file");
        if (key->row && p->plan->items[key->row - 1].after.kind == PORTER_FILE) {
            conflict(item, "selected parent is a file");
            conflict(&p->plan->items[key->row - 1], "selected file also serves as a directory");
        }
        if (m->fat && !alias_candidates(p, m, parent, porter_basename(path), row))
            return false;
        os64_strcopy(path, sizeof(path), parent);
    }
    return true;
}

static bool coupled_destination(const porter_location_t *where)
{
    const char *roots[] = {"/bin", "/lib", "/boot", "/etc", "/tests", "/husk.rc"};
    char key[PORTER_PATH];
    os64_strcopy(key, sizeof(key), where->relative);
    if (where->mount->fat)
        for (char *c = key; *c; c++)
            if (*c >= 'A' && *c <= 'Z')
                *c += 'a' - 'A';
    if (os64_streq(where->mount->where, "/home"))
        return false;
    for (size_t i = 0; i < sizeof(roots) / sizeof(*roots); i++)
        if (porter_under(key, roots[i]))
            return true;
    return false;
}

static bool show_recorded_route(planner_t *p, const porter_entry_t *entry)
{
    porter_plan_item_t *recorded = add_item(p->plan);
    if (!recorded)
        return false;
    recorded->action = PORTER_PRESERVE;
    recorded->reason = "recorded location for comparison";
    recorded->expected = entry->identity;
    const char *why = NULL;
    if (!recorded_place(p, entry, &recorded->where, &why))
        os64_snprintf(recorded->where.path, sizeof(recorded->where.path), "%s:%s",
                      entry->volume, entry->relative);
    return true;
}

static bool selected_entry(planner_t *p, const porter_entry_t *entry)
{
    porter_plan_item_t *item = add_item(p->plan);
    if (!item)
        return false;
    size_t row = p->plan->count;
    item->entry = entry;
    item->after = entry->identity;
    const porter_entry_t *old = p->old_valid
                              ? porter_find(&p->current, entry->lot, entry->path) : NULL;
    if (old)
        item->expected = old->identity;
    const char *dir = porter_route(p->config, entry->lot,
                                   entry->identity.kind == PORTER_FILE ? entry->path : "");
    const char *why = NULL;
    char path[PORTER_PATH];
    if (!dir || !porter_join(path, dir, entry->path)) {
        os64_snprintf(item->where.path, sizeof(item->where.path), "%s/%s", entry->lot, entry->path);
        conflict(item, dir ? "resolved path exceeds the Os64 path limit" : "lot has no effective route");
        return !old || show_recorded_route(p, old);
    }
    if (!porter_locate(p->volumes, path, &item->where, &why)) {
        os64_strcopy(item->where.path, sizeof(item->where.path), path);
        conflict(item, why);
        return !old || show_recorded_route(p, old);
    }
    if (porter_log_contains(item->where.mount, item->where.path, item->where.relative)) {
        conflict(item, "destination is reserved");
        return !old || same_place(&item->where, old) || show_recorded_route(p, old);
    }
    if (!register_selected(p, row, entry))
        return false;
    if (os64_streq(item->where.relative, "/") || porter_protected(p->config, p->volumes, &item->where))
        conflict(item, "destination is reserved");
    if (!p->target->kernel && coupled_destination(&item->where))
        conflict(item, "independent scope routes into the coupled system");
    if (entry->identity.kind == PORTER_FILE && item->where.mount->fat &&
        entry->identity.size > UINT32_MAX)
        conflict(item, "file exceeds the FAT size limit");
    if (old && !same_place(&item->where, old)) {
        conflict(item, "route changed; undo the recorded install before moving this entry");
        return show_recorded_route(p, old);
    }
    if (!porter_inspect(&item->where, &item->before))
        conflict(item, "cannot verify actual destination bytes");
    if (item->action == PORTER_CONFLICT)
        return true;
    if (item->before.kind != PORTER_ABSENT && item->before.kind != item->after.kind) {
        conflict(item, "actual file and target directory or actual directory and target file");
    } else if (p->plan->adoption) {
        if (item->before.kind == PORTER_ABSENT) {
            item->action = PORTER_ADD;
        } else if (porter_identity_equal(&item->before, &item->after)) {
            item->action = PORTER_ADOPT;
        } else {
            item->action = PORTER_REPLACE;
            item->reason = "local bytes differ; adoption would back them up and replace them";
        }
    } else if (old && !porter_identity_equal(&item->before, &old->identity)) {
        conflict(item, "local drift from the recorded identity");
    } else if (!old && item->before.kind != PORTER_ABSENT) {
        conflict(item, "unmanaged destination already exists");
    } else if (!old) {
        item->action = PORTER_ADD;
    } else if (porter_identity_equal(&item->before, &item->after)) {
        item->action = PORTER_UNCHANGED;
    } else {
        item->action = PORTER_REPLACE;
    }
    return true;
}

static bool omitted_entry(planner_t *p, const porter_entry_t *entry)
{
    if (porter_find(p->target, entry->lot, entry->path))
        return true;
    porter_plan_item_t *item = add_item(p->plan);
    if (!item)
        return false;
    item->expected = entry->identity;
    const char *why = NULL;
    bool located = recorded_place(p, entry, &item->where, &why);
    if (!located)
        os64_strcopy(item->where.path, sizeof(item->where.path), entry->relative);
    if (p->plan->adoption || !porter_has_lot(p->target, entry->lot)) {
        item->action = PORTER_PRESERVE;
        item->reason = p->plan->adoption ? "adoption does not retire files" : "lot is outside this target";
        return true;
    }
    if (!located || porter_protected(p->config, p->volumes, &item->where) ||
        (!p->target->kernel && coupled_destination(&item->where))) {
        conflict(item, located ? "recorded destination is now reserved" : why);
        return true;
    }
    path_key_t *key = path_key(&p->paths, item->where.mount, item->where.relative, false);
    if (!key)
        return false;
    if (key->row || key->selected_below) {
        conflict(item, "retirement would overlap the selected destinations");
        return true;
    }
    key->row = p->plan->count;
    if (!porter_inspect(&item->where, &item->before) ||
        !porter_identity_equal(&item->before, &entry->identity))
        conflict(item, "local drift at a proposed retirement");
    else
        item->action = PORTER_RETIRE;
    return true;
}

static bool implied_parent(planner_t *p, const porter_mount_t *mount, const char *relative)
{
    if (os64_streq(relative, "/"))
        return true;
    path_key_t *k = path_key(&p->paths, mount, relative, false);
    if (!k)
        return false;
    if (k->parents_checked)
        return true;
    k->parents_checked = true;
    char parent[PORTER_PATH];
    if (!porter_parent(parent, relative) || !implied_parent(p, mount, parent))
        return false;
    if (k->row)
        return true; // An explicit destination already describes this directory.
    char path[PORTER_PATH];
    porter_kind_t kind;
    if (!porter_join(path, mount->where, relative) ||
        !porter_probe(path, mount->fat, &kind))
        return false;
    if (kind == PORTER_DIR)
        return true;
    porter_plan_item_t *item = add_item(p->plan);
    if (!item)
        return false;
    k->row = p->plan->count;
    item->where.mount = mount;
    os64_strcopy(item->where.relative, sizeof(item->where.relative), relative);
    os64_strcopy(item->where.path, sizeof(item->where.path), path);
    item->before.kind = kind;
    item->after.kind = PORTER_DIR;
    if (k->owner || k->foreign || kind != PORTER_ABSENT ||
        porter_protected(p->config, p->volumes, &item->where) ||
        (!p->target->kernel && coupled_destination(&item->where))) {
        conflict(item, "required directory conflicts with local ownership or a reserved path");
    } else {
        item->action = PORTER_ADD;
        item->reason = "directory required by selected destinations";
    }
    return true;
}

static bool add_implied_parents(planner_t *p)
{
    size_t count = p->plan->count;
    for (size_t i = 0; i < count; i++) {
        const porter_plan_item_t *item = &p->plan->items[i];
        char parent[PORTER_PATH];
        if (item->where.mount && *item->where.relative && item->after.kind != PORTER_ABSENT &&
            !porter_log_contains(item->where.mount, item->where.path, item->where.relative) &&
            porter_parent(parent, item->where.relative) &&
            !implied_parent(p, item->where.mount, parent))
            return false;
    }
    return true;
}

static bool mark_downgrades(planner_t *p)
{
    size_t possible = 0;
    for (size_t i = 0; i < p->plan->count; i++)
        if (p->plan->items[i].action == PORTER_REPLACE)
            possible++;
    char id[PORTER_DIGEST];
    if (p->plan->adoption || !p->old_valid || !p->current.previous || !possible)
        return true;
    os64_strcopy(id, sizeof(id), p->current.previous);
    for (size_t n = 0; *id && possible; n++) {
        porter_inventory_t prior = {0};
        if (n == HISTORY_LIMIT || !read_receipt(&prior, id, p->target->scope))
            return false;
        for (size_t i = 0; i < p->plan->count; i++) {
            porter_plan_item_t *item = &p->plan->items[i];
            if (item->action != PORTER_REPLACE)
                continue;
            const porter_entry_t *entry = porter_find(&prior, item->entry->lot, item->entry->path);
            if (entry && same_place(&item->where, entry) &&
                porter_identity_equal(&entry->identity, &item->after)) {
                item->action = PORTER_DOWNGRADE;
                item->reason = "target bytes were previously recorded at this location";
                possible--;
            }
        }
        os64_strcopy(id, sizeof(id), prior.previous ? prior.previous : "");
        porter_release_inventory(&prior);
    }
    return true;
}

static void preserve_directories(planner_t *p, const porter_mount_t *mount, const char *relative)
{
    char path[PORTER_PATH], parent[PORTER_PATH];
    os64_strcopy(path, sizeof(path), relative);
    do {
        path_key_t *k = path_key(&p->paths, mount, path, false);
        if (k && k->row) {
            porter_plan_item_t *item = &p->plan->items[k->row - 1];
            if (item->action == PORTER_RETIRE && item->before.kind == PORTER_DIR) {
                item->action = PORTER_PRESERVE;
                item->reason = "directory contains entries outside this retirement";
            }
        }
        if (!porter_parent(parent, path))
            break;
        os64_strcopy(path, sizeof(path), parent);
    } while (true);
}

static bool scan_directory(planner_t *p, const porter_mount_t *mount, const char *relative)
{
    path_key_t *key = path_key(&p->paths, mount, relative, true);
    if (!key)
        return false;
    if (key->scanned)
        return true;
    key->scanned = true;
    char dir[PORTER_PATH];
    porter_kind_t kind;
    if (!porter_join(dir, mount->where, relative))
        return false;
    if (porter_log_contains(mount, dir, relative))
        return true;
    if (!porter_probe(dir, mount->fat, &kind))
        return false;
    if (kind == PORTER_ABSENT)
        return true;
    if (kind != PORTER_DIR)
        return false;
    int64_t h = os64_opendir(dir);
    if (h < 0)
        return false;
    os64_dirent_t e;
    int64_t rc;
    bool ok = true;
    while ((rc = os64_readdir((int32_t)h, &e)) == 1) {
        if (os64_streq(e.name, ".") || os64_streq(e.name, ".."))
            continue;
        char child[PORTER_PATH];
        if (!porter_join(child, relative, e.name) || (e.flags & OS64_DE_MOUNT)) {
            preserve_directories(p, mount, relative);
            continue;
        }
        path_key_t *k = path_key(&p->paths, mount, child, false);
        if (!k || !k->row || p->plan->items[k->row - 1].action != PORTER_RETIRE)
            preserve_directories(p, mount, relative);
        if (k && (k->row || k->owner || k->foreign || k->selected_below))
            continue;
        char path[PORTER_PATH];
        if (!porter_join(path, mount->where, child)) {
            ok = false;
            break;
        }
        if (porter_log_contains(mount, path, child))
            continue;
        porter_plan_item_t *item = add_item(p->plan);
        if (!item) {
            ok = false;
            break;
        }
        item->where.mount = mount;
        os64_strcopy(item->where.path, sizeof(item->where.path), path);
        os64_strcopy(item->where.relative, sizeof(item->where.relative), child);
        item->action = PORTER_UNMANAGED;
        item->before.kind = (e.flags & OS64_DE_DIR) ? PORTER_DIR : PORTER_FILE;
        item->before.size = e.size;
        item->reason = "local entry outside the selected ownership";
    }
    return os64_close((int32_t)h) == 0 && rc == 0 && ok;
}

static bool scan_neighbors(planner_t *p)
{
    size_t count = p->plan->count;
    for (size_t i = 0; i < count; i++) {
        const porter_plan_item_t *item = &p->plan->items[i];
        if (!item->where.mount || !*item->where.relative || item->action == PORTER_PRESERVE)
            continue;
        // Directory scans append rows and can move the plan's storage.
        porter_location_t where = item->where;
        if (porter_log_contains(where.mount, where.path, where.relative))
            continue;
        bool directory = item->before.kind == PORTER_DIR;
        char parent[PORTER_PATH];
        if (porter_parent(parent, where.relative) &&
            !scan_directory(p, where.mount, parent))
            return false;
        if (directory && !scan_directory(p, where.mount, where.relative))
            return false;
    }
    size_t entry = 0;
    for (size_t i = 0; i < p->target->lot_count; i++) {
        const char *lot = p->target->lots[i];
        if (entry < p->target->entry_count && os64_streq(lot, p->target->entries[entry].lot)) {
            do {
                entry++;
            } while (entry < p->target->entry_count && os64_streq(lot, p->target->entries[entry].lot));
            continue;
        }
        const char *dir = porter_route(p->config, lot, "");
        porter_location_t where;
        const char *why;
        if (!dir || !porter_locate(p->volumes, dir, &where, &why)) {
            block(p->plan, "an empty selected lot has no available destination");
            continue;
        }
        if (!scan_directory(p, where.mount, where.relative))
            return false;
    }
    return true;
}

static void check_kernel(planner_t *p)
{
    porter_plan_t *plan = p->plan;
    if (!p->target->kernel) {
        plan->kernel_status = "independent scope";
        return;
    }
    for (size_t i = 0; i < plan->count; i++) {
        const porter_plan_item_t *item = &plan->items[i];
        if (item->entry && os64_streq(item->entry->lot, "boot") &&
            os64_streq(item->entry->path, "os64_kernel") && item->before.kind == PORTER_FILE &&
            item->after.kind == PORTER_FILE) {
            os64_strcopy(plan->staged_kernel, sizeof(plan->staged_kernel), item->before.hash);
            break;
        }
    }
    const porter_mount_t *root = porter_mount_at(p->volumes, "/");
    bool rescue = root && root->fat;
    bool main_inactive = rescue;
    for (size_t i = 0; rescue && i < plan->count; i++) {
        const porter_plan_item_t *item = &plan->items[i];
        if (item->where.mount == root && item->entry && !os64_streq(item->entry->lot, "boot"))
            main_inactive = false;
    }
    if (rescue && !main_inactive)
        block(plan, "route the runtime to the inactive main volume from the FAT lifeboat");
    if (!porter_boot_digest(plan->booted_kernel)) {
        plan->kernel_status = "unknown booted kernel identity";
        block(plan, "running kernel cannot prove compatibility with the system target");
    } else if (!os64_streq(plan->booted_kernel, p->target->kernel)) {
        plan->kernel_status = "different booted kernel; system update requires the lifeboat and a reboot";
        if (!main_inactive)
            block(plan, "preview this kernel-coupled switch from the lifeboat against the inactive main system");
    } else if (!os64_streq(plan->staged_kernel, p->target->kernel)) {
        plan->kernel_status = "matches booted kernel; staged kernel differs. This operation will reboot the machine.";
    } else {
        plan->kernel_status = "target matches booted and staged kernel";
    }
}

static bool add_bytes(uint64_t *value, uint64_t amount)
{
    if (amount > UINT64_MAX - *value)
        return false;
    *value += amount;
    return true;
}

static bool estimate(planner_t *p)
{
    porter_plan_t *plan = p->plan;
    plan->spaces = os64_malloc(p->volumes->count * sizeof(*plan->spaces));
    if (!plan->spaces)
        return false;
    porter_location_t archive;
    const char *why = NULL;
    bool backup_ready = porter_locate(p->volumes, p->config->backup, &archive, &why) &&
                        !os64_streq(archive.relative, "/") &&
                        !porter_log_contains(archive.mount, archive.path, archive.relative);
    const porter_mount_t *backup_mount = backup_ready ? archive.mount : NULL;
    if (backup_mount) {
        porter_kind_t kind;
        backup_ready = porter_probe(archive.path, backup_mount->fat, &kind) && kind != PORTER_FILE;
    }
    for (size_t m = 0; m < p->volumes->count; m++) {
        porter_space_t space = {.mount = &p->volumes->mounts[m]};
        for (size_t i = 0; i < plan->count; i++) {
            const porter_plan_item_t *it = &plan->items[i];
            switch (it->action) {
            case PORTER_ADD: case PORTER_ADOPT: case PORTER_REPLACE:
            case PORTER_DOWNGRADE: case PORTER_RETIRE:
                if (it->where.mount != space.mount || porter_identity_equal(&it->before, &it->after))
                    break;
                if (it->after.kind != PORTER_ABSENT) {
                    uint64_t block_size = space.mount->block_size;
                    // Allow one allocation unit for creating the staged name,
                    // plus the payload rounded up to an allocation boundary.
                    uint64_t bytes = it->after.size;
                    uint64_t slack = block_size + (block_size - bytes % block_size) % block_size;
                    if (!add_bytes(&space.stage, bytes) || !add_bytes(&space.stage, slack) ||
                        (it->after.kind == PORTER_FILE && !add_bytes(&plan->transfer_bytes, bytes)))
                        return false;
                }
                if (it->before.kind == PORTER_FILE) {
                    // Allow tar padding/header and gzip expansion for incompressible originals.
                    uint64_t padded = ((it->before.size + 511) & ~UINT64_C(511)) + 512;
                    if (it->before.size >= UINT64_MAX - 1024 ||
                        !add_bytes(&plan->backup_bytes, padded) ||
                        !add_bytes(&plan->backup_bytes, padded / 100 + 64))
                        return false;
                }
                break;
            default:
                break;
            }
        }
        if (space.stage)
            plan->spaces[plan->space_count++] = space;
    }
    if (plan->backup_bytes) {
        if (!add_bytes(&plan->backup_bytes, 2048))
            return false;
        if (!backup_ready) {
            block(plan, "backup directory is unavailable");
        } else {
            size_t i = 0;
            while (i < plan->space_count && plan->spaces[i].mount != backup_mount)
                i++;
            if (i == plan->space_count)
                plan->spaces[plan->space_count++] = (porter_space_t){.mount = backup_mount};
            plan->spaces[i].backup = plan->backup_bytes;
            if (backup_mount->fat && plan->backup_bytes > UINT32_MAX)
                block(plan, "estimated backup archive exceeds the FAT size limit");
        }
    }
    for (size_t i = 0; i < plan->space_count; i++) {
        porter_space_t *s = &plan->spaces[i];
        if (s->mount->space_known && (s->stage > s->mount->free_bytes ||
            s->backup > s->mount->free_bytes - s->stage))
            block(plan, "estimated staging and backup bytes exceed available space");
    }
    return true;
}

bool porter_preview(porter_plan_t *plan, const porter_inventory_t *target, bool baseline,
                    bool adopt, const porter_config_t *config, const porter_volumes_t *volumes)
{
    *plan = (porter_plan_t){0};
    os64_strcopy(plan->target, sizeof(plan->target), target->digest);
    planner_t p = {.plan = plan, .target = target, .config = config, .volumes = volumes};
    bool ok = load_heads(&p, baseline, adopt);
    if (!ok)
        block(plan, "cannot verify local scope ownership");
    for (size_t i = 0; i < target->lot_count; i++)
        if (!porter_route(config, target->lots[i], ""))
            block(plan, "a selected lot has no effective default route");
    for (size_t i = 0; ok && i < target->entry_count; i++)
        ok = selected_entry(&p, &target->entries[i]);
    if (ok)
        ok = add_implied_parents(&p);
    for (size_t i = 0; ok && p.old_valid && i < p.current.entry_count; i++)
        ok = omitted_entry(&p, &p.current.entries[i]);
    if (ok && !mark_downgrades(&p))
        block(plan, "cannot verify older receipt identities");
    if (ok && !scan_neighbors(&p))
        block(plan, "cannot enumerate local neighbors of selected destinations");
    check_kernel(&p);
    if (ok)
        ok = estimate(&p);
    for (size_t i = 0; i < plan->count; i++) {
        if (plan->items[i].action == PORTER_CONFLICT)
            plan->conflicts++;
    }
    porter_release_inventory(&p.current);
    os64_free(p.paths.keys);
    if (!ok)
        block(plan, "preview could not build a complete plan");
    return ok;
}

void porter_release_plan(porter_plan_t *plan)
{
    os64_free(plan->items);
    os64_free(plan->spaces);
    *plan = (porter_plan_t){0};
}
