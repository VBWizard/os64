// Apply recorded ext2 identities and replay them without contacting the valet.
#include "tx_apply.h"
#include "archive.h"
#include "os64/os64.h"

bool porter_tx_mounts_open(porter_tx_mounts_t *m)
{
    *m = (porter_tx_mounts_t){0};
    for (size_t i = 0; i < PORTER_MOUNTS; i++)
        m->pins[i] = -1;
    if (!porter_load_volumes(&m->volumes))
        return false;
    m->home = porter_mount_at(&m->volumes, PORTER_STATE);
    return m->home && os64_streq(m->home->where, "/home") && porter_tx_pin(m, m->home) &&
           porter_mount_at(&m->volumes, PORTER_TX_OPERATIONS) == m->home &&
           porter_mount_at(&m->volumes, PORTER_TX_HEADS) == m->home &&
           porter_mount_at(&m->volumes, PORTER_TX_RECEIPTS) == m->home;
}

void porter_tx_mounts_close(porter_tx_mounts_t *m)
{
    for (size_t i = 0; i < m->volumes.count; i++)
        if (m->pins[i] >= 0)
            os64_close(m->pins[i]);
    os64_free(m->volumes.mounts);
}

bool porter_tx_pin(porter_tx_mounts_t *m, const porter_mount_t *mount)
{
    if (!mount || !mount->writable || !mount->persistent_ext2)
        return false;
    size_t i = (size_t)(mount - m->volumes.mounts);
    if (i >= m->volumes.count)
        return false;
    if (m->pins[i] >= 0)
        return true;
    int64_t dir = os64_opendir(mount->where);
    if (dir < 0)
        return false;
    m->pins[i] = (int32_t)dir;
    return true;
}

bool porter_tx_location(porter_tx_mounts_t *m, const char *volume, const char *relative,
                        porter_location_t *location)
{
    const porter_mount_t *mount = porter_mount_by_guid(&m->volumes, volume);
    char path[PORTER_PATH];
    const char *why;
    return porter_tx_pin(m, mount) && porter_join(path, mount->where, relative) &&
           porter_mount_at(&m->volumes, path) == mount &&
           porter_locate(&m->volumes, path, location, &why) &&
           location->mount == mount && os64_streq(location->relative, relative);
}

bool porter_tx_head_path(const char *scope, char path[PORTER_PATH])
{
    return porter_join(path, PORTER_TX_HEADS, scope);
}

bool porter_tx_stamp_path(const porter_mount_t *mount, const char *scope, char path[PORTER_PATH])
{
    char root[PORTER_PATH];
    return porter_join(root, mount->where, ".porter/stamps") && porter_join(path, root, scope);
}

static bool known_value(const char *path, const porter_tx_value_t *old,
                        const porter_tx_value_t *next, bool old_only)
{
    porter_tx_value_t value;
    return porter_tx_value(path, &value) &&
           (porter_tx_value_equal(&value, old) || (!old_only && porter_tx_value_equal(&value, next)));
}

static bool stamp_location(porter_tx_mounts_t *m, const porter_tx_stamp_t *stamp, const char *scope,
                           char path[PORTER_PATH])
{
    const porter_mount_t *mount = porter_mount_by_guid(&m->volumes, stamp->volume);
    return porter_tx_pin(m, mount) && porter_tx_stamp_path(mount, scope, path) &&
           porter_mount_at(&m->volumes, path) == mount;
}

bool porter_tx_metadata(porter_tx_mounts_t *m, const porter_transaction_t *tx, bool old_only)
{
    char path[PORTER_PATH];
    if (!porter_tx_head_path(tx->h.scope, path) ||
        porter_mount_at(&m->volumes, path) != m->home ||
        !known_value(path, &tx->h.old_head, &tx->h.next_head, old_only))
        return false;
    for (uint32_t i = 0; i < tx->h.stamp_count; i++) {
        const porter_tx_stamp_t *stamp = &tx->stamps[i];
        if (!stamp_location(m, stamp, tx->h.scope, path) ||
            !known_value(path, &stamp->old, &stamp->next, old_only))
            return false;
    }
    return true;
}

bool porter_tx_set_metadata(porter_tx_mounts_t *m, const porter_transaction_t *tx,
                            bool forward, bool head)
{
    char path[PORTER_PATH];
    if (head)
        return porter_tx_head_path(tx->h.scope, path) &&
               known_value(path, &tx->h.old_head, &tx->h.next_head, false) &&
               porter_tx_set_value(path, tx->h.id, forward ? &tx->h.next_head : &tx->h.old_head);
    for (uint32_t i = 0; i < tx->h.stamp_count; i++) {
        const porter_tx_stamp_t *stamp = &tx->stamps[i];
        if (!stamp_location(m, stamp, tx->h.scope, path) ||
            !known_value(path, &stamp->old, &stamp->next, false) ||
            !porter_tx_set_value(path, tx->h.id, forward ? &stamp->next : &stamp->old))
            return false;
    }
    return true;
}

static bool scratch_directory(const porter_mount_t *mount, const porter_transaction_t *tx,
                              char path[PORTER_PATH])
{
    char root[PORTER_PATH];
    return porter_join(root, mount->where, ".porter/stage") && porter_join(path, root, tx->h.id);
}

bool porter_tx_scratch(porter_tx_mounts_t *m, const porter_transaction_t *tx,
                       uint32_t row, char role, char path[PORTER_PATH], bool ensure)
{
    const porter_mount_t *mount = porter_mount_by_guid(&m->volumes, tx->changes[row].volume);
    char dir[PORTER_PATH], name[24];
    os64_snprintf(name, sizeof(name), "%c%lu", role, (unsigned long)row);
    return porter_tx_pin(m, mount) && scratch_directory(mount, tx, dir) &&
           porter_join(path, dir, name) && porter_mount_at(&m->volumes, path) == mount &&
           (!ensure || porter_tx_mkdirs(dir));
}

static bool remove_file(const char *path)
{
    porter_kind_t kind;
    return porter_probe(path, false, &kind) &&
           (kind == PORTER_ABSENT || (kind == PORTER_FILE && os64_unlink(path) == 0));
}

static bool restore_member(porter_tx_mounts_t *m, const porter_transaction_t *tx,
                           uint32_t row, porter_member_t *member)
{
    const porter_tx_change_t *change = &tx->changes[row];
    porter_location_t destination;
    porter_identity_t actual;
    if (!porter_tx_location(m, change->volume, change->path, &destination) ||
        !porter_inspect(&destination, &actual))
        return false;
    if (porter_identity_equal(&actual, &change->before))
        return true; // Verify the archive member without staging another copy.

    char staged[PORTER_PATH];
    if (!porter_tx_scratch(m, tx, row, 'b', staged, true))
        return false;
    os64_dirent_t e;
    char digest[PORTER_DIGEST];
    if (os64_stat(staged, &e) == 0 && !(e.flags & (OS64_DE_DIR | OS64_DE_MOUNT)) &&
        e.size == change->before.size && porter_hash_file(staged, e.size, digest) &&
        os64_streq(digest, change->before.hash))
        return true; // A verified staged copy can survive an interrupted undo.
    if (!remove_file(staged))
        return false;
    os64_strcopy(member->path, sizeof(member->path), staged);
    return true;
}

bool porter_tx_backup(porter_tx_mounts_t *m, const porter_transaction_t *tx,
                      bool create, bool restore)
{
    size_t count = 0;
    for (uint32_t i = 0; i < tx->h.change_count; i++)
        if (tx->changes[i].before.kind == PORTER_FILE)
            count++;
    if (!count)
        return !*tx->h.backup_path;

    porter_location_t archive;
    if (!*tx->h.backup_path || !porter_tx_location(m, tx->h.backup_volume, tx->h.backup_path, &archive))
        return false;
    porter_member_t *members = os64_malloc(count * sizeof(*members));
    if (!members)
        return false;
    bool ok = true;
    size_t row = 0;
    for (uint32_t i = 0; ok && i < tx->h.change_count; i++) {
        const porter_tx_change_t *change = &tx->changes[i];
        if (change->before.kind != PORTER_FILE)
            continue;
        porter_member_t *member = &members[row++];
        *member = (porter_member_t){.identity = change->before};
        os64_snprintf(member->name, sizeof(member->name), "backup/%lu", (unsigned long)i);
        if (create) {
            porter_location_t source;
            ok = porter_tx_location(m, change->volume, change->path, &source);
            if (ok)
                os64_strcopy(member->path, sizeof(member->path), source.path);
        } else if (restore) {
            ok = restore_member(m, tx, i, member);
        }
    }
    if (ok && create) {
        char parent[PORTER_PATH];
        ok = porter_parent(parent, archive.path) && porter_tx_mkdirs(parent) &&
             porter_archive_backup(archive.path, members, count);
        for (size_t i = 0; i < count; i++)
            members[i].path[0] = 0;
    }
    if (ok)
        ok = porter_archive_local(archive.path, members, count);
    os64_free(members);
    return ok;
}

static int directory_empty(const char *path)
{
    int64_t dir = os64_opendir(path);
    if (dir < 0)
        return -1;
    os64_dirent_t e;
    int64_t n;
    while ((n = os64_readdir((int32_t)dir, &e)) == 1)
        if (!os64_streq(e.name, ".") && !os64_streq(e.name, ".."))
            break;
    return os64_close((int32_t)dir) == 0 ? (int)n : -1;
}

static const porter_identity_t *desired_identity(const porter_tx_change_t *c, bool forward)
{
    return forward ? &c->after : &c->before;
}

static bool path_identity(porter_tx_mounts_t *m, const porter_tx_change_t *c,
                          porter_location_t *where, porter_identity_t *actual)
{
    return porter_tx_location(m, c->volume, c->path, where) && porter_inspect(where, actual) &&
           !porter_under(where->relative, "/.porter") &&
           !porter_log_contains(where->mount, where->path, where->relative);
}

bool porter_tx_preflight(porter_tx_mounts_t *m, const porter_transaction_t *tx,
                         bool forward, bool allow_other)
{
    for (uint32_t i = 0; i < tx->h.change_count; i++) {
        const porter_tx_change_t *c = &tx->changes[i];
        porter_location_t where;
        porter_identity_t actual;
        if (!path_identity(m, c, &where, &actual))
            return false;
        const porter_identity_t *wanted = desired_identity(c, forward);
        const porter_identity_t *other = desired_identity(c, !forward);
        if (!porter_identity_equal(&actual, wanted) &&
            !(allow_other && porter_identity_equal(&actual, other)))
            return false;
    }
    return true;
}

static bool apply_one(porter_tx_mounts_t *m, const porter_transaction_t *tx,
                      uint32_t row, bool forward, bool allow_done)
{
    const porter_tx_change_t *c = &tx->changes[row];
    const porter_identity_t *wanted = desired_identity(c, forward);
    const porter_identity_t *other = desired_identity(c, !forward);
    porter_location_t where;
    porter_identity_t actual;
    if (!path_identity(m, c, &where, &actual))
        return false;
    if (allow_done && porter_identity_equal(&actual, wanted))
        return true;
    if (!porter_identity_equal(&actual, other))
        return false;
    if (wanted->kind == PORTER_DIR)
        return os64_mkdir(where.path) == 0;
    if (wanted->kind == PORTER_ABSENT) {
        if (other->kind == PORTER_DIR) {
            if (!forward && (c->flags & PORTER_TX_KEEP_DIRECTORY))
                return true;
            int empty = directory_empty(where.path);
            if (empty != 0)
                return !forward && empty > 0;
        }
        return os64_unlink(where.path) == 0;
    }

    char staged[PORTER_PATH];
    if (!porter_tx_scratch(m, tx, row, forward ? 'a' : 'b', staged, false))
        return false;
    uint64_t flags = actual.kind == PORTER_ABSENT ? OS64_RENAME_NOREPLACE : OS64_RENAME_REQUIRE_ATOMIC_REPLACE;
    return os64_rename_with_flags(staged, where.path, flags) == 0;
}

static unsigned depth(const char *path)
{
    unsigned n = 0;
    for (; *path; path++)
        if (*path == '/')
            n++;
    return n;
}

bool porter_tx_verify_applied(porter_tx_mounts_t *m, const porter_transaction_t *tx, bool forward)
{
    for (uint32_t i = 0; i < tx->h.change_count; i++) {
        const porter_tx_change_t *c = &tx->changes[i];
        const porter_identity_t *wanted = desired_identity(c, forward);
        porter_location_t where;
        porter_identity_t actual;
        if (!path_identity(m, c, &where, &actual))
            return false;
        if (porter_identity_equal(&actual, wanted))
            continue;
        if (forward || wanted->kind != PORTER_ABSENT || actual.kind != PORTER_DIR ||
            c->after.kind != PORTER_DIR ||
            (!(c->flags & PORTER_TX_KEEP_DIRECTORY) && directory_empty(where.path) <= 0))
            return false;
    }
    return true;
}

bool porter_tx_apply(porter_tx_mounts_t *m, const porter_transaction_t *tx,
                     bool forward, bool allow_done,
                     void (*checkpoint)(const void *, const char *), const void *context)
{
    unsigned deepest = 0;
    for (uint32_t i = 0; i < tx->h.change_count; i++) {
        unsigned level = depth(tx->changes[i].path);
        if (level > deepest)
            deepest = level;
    }
    for (unsigned level = 1; level <= deepest; level++)
        for (uint32_t i = 0; i < tx->h.change_count; i++) {
            const porter_tx_change_t *c = &tx->changes[i];
            if (desired_identity(c, forward)->kind == PORTER_DIR && depth(c->path) == level &&
                !apply_one(m, tx, i, forward, allow_done))
                return false;
        }
    for (uint32_t i = 0; i < tx->h.change_count; i++) {
        const porter_tx_change_t *c = &tx->changes[i];
        if (c->before.kind != PORTER_FILE && c->after.kind != PORTER_FILE)
            continue;
        if (!apply_one(m, tx, i, forward, allow_done))
            return false;
        if (checkpoint) {
            checkpoint(context, "first-file");
            checkpoint = NULL;
        }
    }
    for (unsigned level = deepest; level; level--)
        for (uint32_t i = 0; i < tx->h.change_count; i++) {
            const porter_tx_change_t *c = &tx->changes[i];
            if (desired_identity(c, forward)->kind == PORTER_ABSENT &&
                desired_identity(c, !forward)->kind == PORTER_DIR && depth(c->path) == level &&
                !apply_one(m, tx, i, forward, allow_done))
                return false;
        }
    return porter_tx_verify_applied(m, tx, forward);
}

void porter_tx_cleanup(porter_tx_mounts_t *m, const porter_transaction_t *tx, bool discard_backup)
{
    char meta[PORTER_PATH];
    if (porter_tx_head_path(tx->h.scope, meta))
        porter_tx_discard_metadata_temp(meta, tx->h.id);
    for (uint32_t i = 0; i < tx->h.stamp_count; i++)
        if (stamp_location(m, &tx->stamps[i], tx->h.scope, meta))
            porter_tx_discard_metadata_temp(meta, tx->h.id);
    bool used[PORTER_MOUNTS] = {0};
    for (uint32_t i = 0; i < tx->h.change_count; i++) {
        const porter_mount_t *mount = porter_mount_by_guid(&m->volumes, tx->changes[i].volume);
        if (!mount)
            continue;
        used[mount - m->volumes.mounts] = true;
        char path[PORTER_PATH];
        if (porter_tx_scratch(m, tx, i, 'a', path, false))
            remove_file(path);
        if (porter_tx_scratch(m, tx, i, 'b', path, false))
            remove_file(path);
    }
    for (size_t i = 0; i < m->volumes.count; i++) {
        char path[PORTER_PATH];
        if (used[i] && scratch_directory(&m->volumes.mounts[i], tx, path))
            os64_unlink(path); // An unexpected entry keeps the directory in place.
    }
    if (discard_backup && *tx->h.backup_path) {
        porter_location_t archive;
        if (porter_tx_location(m, tx->h.backup_volume, tx->h.backup_path, &archive))
            remove_file(archive.path);
    }
}
