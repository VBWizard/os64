// Shared install, history, single undo and offline recovery engine.
#include "tx_apply.h"
#include "tx_receipt.h"
#include "archive.h"
#include "os64/os64.h"

typedef struct {
    porter_report_t emit;
    void *context;
} reporter_t;

static bool problem(const reporter_t *r, const char *message)
{
    r->emit(r->context, true, message);
    return false;
}

static void announce(const reporter_t *r, const char *message)
{
    r->emit(r->context, false, message);
}

static void checkpoint(const void *context, const char *name)
{
#ifdef PORTER_TEST_INTERRUPTS
    const reporter_t *r = context;
    const char *cut = os64_getenv("PORTER_TEST_CUT");
    if (cut && os64_streq(cut, name)) {
        char message[96];
        os64_snprintf(message, sizeof(message), "Porter test cut %s", name);
        announce(r, message);
        os64_sleep(120000);
    }
    const char *pause = os64_getenv("PORTER_TEST_PAUSE");
    if (pause && os64_streq(pause, name)) {
        char message[96];
        os64_snprintf(message, sizeof(message), "Porter test pause %s", name);
        announce(r, message);
        os64_dirent_t e;
        for (unsigned i = 0; i < 1200 && os64_stat("/home/porter-test-release", &e) < 0; i++)
            os64_sleep(100);
    }
#else
    (void)context;
    (void)name;
#endif
}

static bool claim(os64_porter_owner_t *owner, const reporter_t *r)
{
    int64_t status = porter_claim(owner);
    if (!status)
        return true;
    if (status == OS64_PORTER_BUSY)
        return problem(r, "another Porter writer is active");
    if (status == OS64_PORTER_UNSEEDED)
        return problem(r, "the kernel entropy pool is not ready for a writer lease");
    return problem(r, "this kernel does not provide the Porter writer lease and checked storage barrier");
}

static bool empty_operation(const reporter_t *r)
{
    char id[PORTER_TX_ID], digest[PORTER_DIGEST];
    bool exists;
    if (!porter_tx_pointer(id, digest, &exists))
        return problem(r, "cannot validate the operation pointer");
    return !exists || problem(r, "a Porter operation is pending; run porter recover");
}

static bool prepare(const reporter_t *r, porter_transaction_t *tx)
{
    if (!porter_tx_store(tx) || !porter_barrier())
        return problem(r, "could not persist the operation intent");
    checkpoint(r, "before-pointer");
    if (!porter_tx_publish_pointer(tx) || !porter_barrier())
        return problem(r, "could not persist the operation pointer");
    checkpoint(r, "prepared");
    return true;
}

static bool arm(const reporter_t *r, const porter_transaction_t *tx)
{
    if (!porter_tx_publish_marker(tx, "armed") || !porter_barrier())
        return problem(r, "could not make the backup and mutation authority durable");
    checkpoint(r, "armed");
    return true;
}

static bool retire_pointer(const reporter_t *r, const porter_transaction_t *tx)
{
    char id[PORTER_TX_ID], digest[PORTER_DIGEST];
    bool exists;
    if (!porter_tx_pointer(id, digest, &exists) || !exists || !os64_streq(id, tx->h.id) ||
        !os64_streq(digest, tx->digest))
        return problem(r, "operation pointer changed; recovery evidence was retained");
    checkpoint(r, "before-pointer-unlink");
    if (os64_unlink(PORTER_TX_POINTER) < 0)
        return problem(r, "cannot retire the operation pointer");
    checkpoint(r, "after-pointer-unlink");
    if (!porter_barrier())
        return problem(r, "operation pointer retirement was not confirmed durable");
    checkpoint(r, "retired");
    return true;
}

static bool settle(porter_tx_mounts_t *mounts, const porter_transaction_t *tx, const reporter_t *r,
                   bool next, bool publish_completion, bool discard_backup)
{
    if (!porter_tx_set_metadata(mounts, tx, next, false) || !porter_barrier())
        return problem(r, "could not make the destination and volume stamps durable");
    checkpoint(r, "stamps");
    if (publish_completion) {
        if (!porter_tx_publish_marker(tx, "complete") || !porter_barrier())
            return problem(r, "could not confirm the completion record");
        checkpoint(r, "complete");
    }
    if (!porter_tx_set_metadata(mounts, tx, next, true) || !porter_barrier())
        return problem(r, "could not make the resulting history head durable");
    checkpoint(r, "head");
    if (!retire_pointer(r, tx))
        return false;
    porter_tx_cleanup(mounts, tx, discard_backup);
    return true;
}

static bool same_plan(const porter_plan_t *a, const porter_plan_t *b,
                      const porter_config_t *old_cfg, const porter_config_t *cfg)
{
    if (a->blocker || b->blocker || a->conflicts || b->conflicts ||
        a->adoption != b->adoption ||
        !os64_streq(a->target, b->target) || !os64_streq(a->receipt, b->receipt) ||
        !os64_streq(a->booted_kernel, b->booted_kernel) || !os64_streq(a->staged_kernel, b->staged_kernel) ||
        !os64_streq(old_cfg->backup, cfg->backup))
        return false;
    // Neighbor listings are informational. A redirect into an unmanaged
    // /home file can grow while the approved write decisions stay the same.
    size_t i = 0, j = 0;
    for (;;) {
        while (i < a->count && a->items[i].action == PORTER_UNMANAGED)
            i++;
        while (j < b->count && b->items[j].action == PORTER_UNMANAGED)
            j++;
        if (i == a->count || j == b->count)
            return i == a->count && j == b->count;
        const porter_plan_item_t *x = &a->items[i++], *y = &b->items[j++];
        if (x->action != y->action || x->entry != y->entry ||
            !os64_streq(x->where.path, y->where.path) || !os64_streq(x->where.relative, y->where.relative) ||
            (x->where.mount != NULL) != (y->where.mount != NULL) ||
            (x->where.mount && !os64_streq(x->where.mount->guid, y->where.mount->guid)) ||
            !porter_identity_equal(&x->expected, &y->expected) ||
            !porter_identity_equal(&x->before, &y->before) ||
            !porter_identity_equal(&x->after, &y->after))
            return false;
    }
}

static bool mutable_row(const porter_plan_item_t *row)
{
    return (row->action == PORTER_ADD || row->action == PORTER_REPLACE ||
            row->action == PORTER_DOWNGRADE || row->action == PORTER_RETIRE) &&
           !porter_identity_equal(&row->before, &row->after);
}

static bool make_changes(porter_transaction_t *tx, porter_tx_mounts_t *mounts,
                         const porter_plan_t *plan)
{
    size_t cap = 0;
    for (size_t i = 0; i < plan->count; i++) {
        const porter_plan_item_t *row = &plan->items[i];
        if (!mutable_row(row))
            continue;
        if (!porter_tx_pin(mounts, row->where.mount) ||
            row->before.size > UINT32_MAX || row->after.size > UINT32_MAX ||
            !porter_grow((void **)&tx->changes, &cap, tx->h.change_count, sizeof(*tx->changes),
                         PORTER_TX_CHANGES))
            return false;
        porter_tx_change_t *change = &tx->changes[tx->h.change_count++];
        *change = (porter_tx_change_t){.before = row->before, .after = row->after};
        os64_strcopy(change->volume, sizeof(change->volume), row->where.mount->guid);
        os64_strcopy(change->path, sizeof(change->path), row->where.relative);
    }
    return true;
}

static bool set_backup_path(porter_transaction_t *tx, porter_tx_mounts_t *m, const porter_config_t *cfg)
{
    bool needed = false;
    for (uint32_t i = 0; i < tx->h.change_count; i++)
        needed |= tx->changes[i].before.kind == PORTER_FILE;
    if (!needed)
        return true;
    char name[72], path[PORTER_PATH];
    os64_snprintf(name, sizeof(name), "%s.tar.gz", tx->h.id);
    const char *why;
    porter_location_t where;
    if (!porter_join(path, cfg->backup, name) || !porter_locate(&m->volumes, path, &where, &why) ||
        !porter_tx_pin(m, where.mount) || porter_under(where.relative, "/.porter") ||
        porter_log_contains(where.mount, where.path, where.relative))
        return false;
    os64_strcopy(tx->h.backup_volume, sizeof(tx->h.backup_volume), where.mount->guid);
    os64_strcopy(tx->h.backup_path, sizeof(tx->h.backup_path), where.relative);
    return true;
}

static bool snapshot_stamps(porter_transaction_t *tx, porter_tx_mounts_t *m,
                            const porter_receipt_text_t *receipt)
{
    size_t cap = 0;
    for (size_t i = 0; i < m->volumes.count; i++) {
        const porter_mount_t *mount = &m->volumes.mounts[i];
        if (!mount->disk)
            continue;
        char path[PORTER_PATH];
        porter_tx_value_t old;
        if (!porter_tx_stamp_path(mount, tx->h.scope, path) ||
            porter_mount_at(&m->volumes, path) != mount || !porter_tx_value(path, &old))
            return false;
        if (!old.present && !receipt->volumes[i])
            continue;
        if (!porter_tx_pin(m, mount) ||
            !porter_grow((void **)&tx->stamps, &cap, tx->h.stamp_count, sizeof(*tx->stamps), PORTER_MOUNTS))
            return false;
        porter_tx_stamp_t *stamp = &tx->stamps[tx->h.stamp_count++];
        *stamp = (porter_tx_stamp_t){.old = old};
        os64_strcopy(stamp->volume, sizeof(stamp->volume), mount->guid);
        if (receipt->volumes[i])
            stamp->next = tx->h.next_head;
        if (tx->h.undo_head.present)
            stamp->undo = old;
    }
    return true;
}

static bool make_install_record(porter_transaction_t *tx, porter_tx_mounts_t *m,
                                const porter_inventory_t *target, const porter_plan_t *plan,
                                const porter_config_t *cfg, porter_receipt_text_t *receipt)
{
    os64_strcopy(tx->h.scope, sizeof(tx->h.scope), target->scope);
    os64_strcopy(tx->h.target, sizeof(tx->h.target), target->digest);
    os64_strcopy(tx->h.source, sizeof(tx->h.source), tx->h.id);
    if (target->kernel) {
        const porter_entry_t *kernel = porter_find(target, "boot", "os64_kernel");
        for (size_t i = 0; i < plan->count && kernel; i++) {
            const porter_plan_item_t *row = &plan->items[i];
            if (row->entry == kernel && row->where.mount) {
                os64_strcopy(tx->h.kernel, sizeof(tx->h.kernel), target->kernel);
                os64_strcopy(tx->h.kernel_volume, sizeof(tx->h.kernel_volume), row->where.mount->guid);
                os64_strcopy(tx->h.kernel_path, sizeof(tx->h.kernel_path), row->where.relative);
                break;
            }
        }
        if (!*tx->h.kernel)
            return false;
    }
    char path[PORTER_PATH], current[PORTER_DIGEST];
    if (!porter_tx_head_path(target->scope, path) || !porter_tx_value(path, &tx->h.old_head) ||
        (!plan->adoption && (!porter_tx_value_id(&tx->h.old_head, current) ||
                             !os64_streq(current, plan->receipt))))
        return false;
    if (!plan->adoption)
        tx->h.undo_head = tx->h.old_head;
    porter_inventory_t prior = {0};
    bool ok = (!tx->h.undo_head.present || porter_read_receipt(&prior, plan->receipt, target->scope)) &&
              porter_build_receipt(receipt, tx, target, &prior, plan, &m->volumes);
    porter_release_inventory(&prior);
    if (!ok)
        return false;
    porter_tx_id_value(&tx->h.next_head, receipt->digest);
    return snapshot_stamps(tx, m, receipt) && make_changes(tx, m, plan) && set_backup_path(tx, m, cfg);
}

static bool stage_incoming(porter_tx_mounts_t *m, const porter_transaction_t *tx,
                           const char *host, uint16_t port, const porter_inventory_t *target,
                           const porter_plan_t *plan)
{
    size_t count = 0;
    for (uint32_t i = 0; i < tx->h.change_count; i++)
        if (tx->changes[i].after.kind == PORTER_FILE)
            count++;
    if (!count)
        return true;
    porter_member_t *members = os64_malloc(count * sizeof(*members));
    size_t *indices = os64_malloc(count * sizeof(*indices));
    bool ok = members && indices;
    size_t member = 0;
    uint32_t row = 0;
    for (size_t i = 0; ok && i < plan->count; i++) {
        const porter_plan_item_t *item = &plan->items[i];
        if (!mutable_row(item))
            continue;
        uint32_t index = row++;
        if (item->after.kind != PORTER_FILE)
            continue;
        if (!item->entry || member == count) {
            ok = false;
            break;
        }
        porter_member_t *v = &members[member];
        *v = (porter_member_t){.identity = item->after};
        indices[member++] = (size_t)(item->entry - target->entries);
        int n = os64_snprintf(v->name, sizeof(v->name), "%s/%s", item->entry->lot, item->entry->path);
        ok = n > 0 && n < (int)sizeof(v->name) &&
             porter_tx_scratch(m, tx, index, 'a', v->path, true);
    }
    ok = ok && member == count && porter_get_archive(host, port, target->digest, indices, members, count);
    os64_free(indices);
    os64_free(members);
    return ok;
}

static bool receipt_matches(const porter_tx_value_t *head, const char *scope)
{
    char id[PORTER_DIGEST];
    if (!porter_tx_value_id(head, id))
        return false;
    if (!*id)
        return true;
    porter_inventory_t receipt;
    if (!porter_read_receipt(&receipt, id, scope))
        return false;
    porter_release_inventory(&receipt);
    return true;
}

static bool selected_before_matches(porter_tx_mounts_t *m, const porter_plan_t *plan)
{
    for (size_t i = 0; i < plan->count; i++) {
        const porter_plan_item_t *row = &plan->items[i];
        if (!row->entry)
            continue;
        const char *reason;
        porter_location_t where;
        porter_identity_t actual;
        if (!porter_locate(&m->volumes, row->where.path, &where, &reason) ||
            !os64_streq(where.mount->guid, row->where.mount->guid) ||
            !os64_streq(where.relative, row->where.relative) ||
            !porter_inspect(&where, &actual) || !porter_identity_equal(&actual, &row->before))
            return false;
    }
    return true;
}

static bool run_install(const reporter_t *r, porter_tx_mounts_t *m, porter_transaction_t *tx,
                        const char *host, uint16_t port, const porter_inventory_t *target,
                        const porter_plan_t *plan, const porter_receipt_text_t *receipt)
{
    if (!prepare(r, tx))
        return false;
    if (!stage_incoming(m, tx, host, port, target, plan))
        return problem(r, "selected archive failed verification or staging; recover the prepared operation");
    if (!porter_tx_backup(m, tx, true, false) || !porter_publish_receipt(receipt, tx->h.id) ||
        !receipt_matches(&tx->h.next_head, tx->h.scope))
        return problem(r, "cannot verify the backup or write the receipt; recover the prepared operation");
    checkpoint(r, "staged");
    if (!selected_before_matches(m, plan) || !porter_tx_preflight(m, tx, false, false) ||
        !porter_tx_metadata(m, tx, true))
        return problem(r, "the approved before-state changed; the operation was not armed");
    if (!arm(r, tx))
        return false;
    if (!porter_tx_apply(m, tx, true, false, checkpoint, r))
        return problem(r, "destination operation failed; run porter recover");
    checkpoint(r, "destinations");
    return settle(m, tx, r, true, true, false);
}

bool porter_tx_install(const char *host, uint16_t port, const porter_inventory_t *target,
                       bool baseline, bool adopt, const porter_plan_t *approved,
                       const porter_config_t *approved_cfg, porter_report_t emit, void *context)
{
    const reporter_t r = {emit, context};
    os64_porter_owner_t owner;
    if (!claim(&owner, &r))
        return false;
    porter_tx_mounts_t mounts;
    porter_plan_t plan = {0};
    porter_config_t cfg = {0};
    porter_receipt_text_t receipt = {0};
    porter_transaction_t tx;
    porter_tx_init(&tx, PORTER_TX_INSTALL, &owner);
    bool ok = false;
    if (!porter_tx_mounts_open(&mounts))
        problem(&r, "persistent /home on NVMe ext2 is required for Porter state");
    else if (!empty_operation(&r))
        ok = false;
    else if (!porter_load_config(&cfg, &mounts.volumes) ||
             !porter_preview_ext2(&plan, target, baseline, adopt, &cfg, &mounts.volumes) ||
             !same_plan(approved, &plan, approved_cfg, &cfg))
        problem(&r, "the approved plan changed; preview and approve it again");
    else if (target->kernel && (!os64_streq(target->kernel, plan.booted_kernel) ||
                               !os64_streq(target->kernel, plan.staged_kernel)))
        problem(&r, "this system update would change the kernel; it requires the kernel update slice");
    else {
        bool changes = false;
        for (size_t i = 0; i < plan.count; i++)
            changes |= mutable_row(&plan.items[i]);
        if (!changes && !plan.adoption && os64_streq(plan.installed, plan.target)) {
            announce(&r, "the verified target is already installed");
            ok = true;
        } else if (!make_install_record(&tx, &mounts, target, &plan, &cfg, &receipt)) {
            problem(&r, "cannot record the install on persistent NVMe ext2 volumes");
        } else {
            ok = run_install(&r, &mounts, &tx, host, port, target, &plan, &receipt);
            if (ok)
                announce(&r, "install committed");
        }
    }
    porter_tx_free(&tx);
    os64_free(receipt.data);
    porter_release_plan(&plan);
    os64_free(cfg.rules);
    porter_tx_mounts_close(&mounts);
    porter_unclaim();
    return ok;
}

static bool active_install(porter_tx_mounts_t *mounts, const char *scope,
                           porter_transaction_t *install, const reporter_t *r)
{
    char path[PORTER_PATH], head[PORTER_DIGEST];
    porter_tx_value_t value;
    if (!porter_tx_head_path(scope, path) || !porter_tx_value(path, &value) ||
        !porter_tx_value_id(&value, head))
        return problem(r, "cannot read the active history head");
    if (!*head)
        return problem(r, "there is no active history for this scope");
    porter_inventory_t receipt = {0};
    bool ok = porter_read_receipt(&receipt, head, scope);
    if (ok)
        ok = receipt.operation && porter_tx_read(install, receipt.operation, NULL);
    bool exists, valid;
    ok = ok && install->h.kind == PORTER_TX_INSTALL && os64_streq(install->h.scope, scope) &&
         os64_streq(install->h.target, receipt.target) &&
         porter_tx_value_equal(&value, &install->h.next_head) &&
         porter_tx_marker(install, "complete", &exists, &valid) && valid;
    for (size_t i = 0; ok && i < receipt.entry_count; i++) {
        const porter_entry_t *e = &receipt.entries[i];
        porter_location_t where;
        porter_identity_t actual;
        ok = porter_tx_location(mounts, e->volume, e->relative, &where) &&
             porter_inspect(&where, &actual) && porter_identity_equal(&e->identity, &actual);
        if (!ok)
            problem(r, "actual contents drifted from the active receipt");
    }
    porter_release_inventory(&receipt);
    return ok || problem(r, "cannot verify a completed install record for the active receipt");
}

static bool undo_kernel_matches(const porter_transaction_t *tx, const porter_volumes_t *volumes)
{
    if (!*tx->h.kernel)
        return true;
    char booted[PORTER_DIGEST], path[PORTER_PATH];
    const porter_mount_t *mount = porter_mount_by_guid(volumes, tx->h.kernel_volume);
    const char *why;
    porter_location_t where;
    porter_identity_t staged;
    return mount && porter_boot_digest(booted) && os64_streq(booted, tx->h.kernel) &&
           porter_join(path, mount->where, tx->h.kernel_path) &&
           porter_locate(volumes, path, &where, &why) && where.mount == mount &&
           porter_inspect(&where, &staged) && staged.kind == PORTER_FILE &&
           os64_streq(staged.hash, tx->h.kernel);
}

static void use_install_for_undo(porter_transaction_t *tx, porter_transaction_t *install)
{
    os64_strcopy(tx->h.scope, sizeof(tx->h.scope), install->h.scope);
    os64_strcopy(tx->h.target, sizeof(tx->h.target), install->h.target);
    os64_strcopy(tx->h.source, sizeof(tx->h.source), install->h.id);
    os64_strcopy(tx->h.kernel, sizeof(tx->h.kernel), install->h.kernel);
    os64_strcopy(tx->h.kernel_volume, sizeof(tx->h.kernel_volume), install->h.kernel_volume);
    os64_strcopy(tx->h.kernel_path, sizeof(tx->h.kernel_path), install->h.kernel_path);
    os64_strcopy(tx->h.backup_volume, sizeof(tx->h.backup_volume), install->h.backup_volume);
    os64_strcopy(tx->h.backup_path, sizeof(tx->h.backup_path), install->h.backup_path);
    tx->h.old_head = install->h.next_head;
    tx->h.next_head = install->h.undo_head;
    tx->h.change_count = install->h.change_count;
    tx->h.stamp_count = install->h.stamp_count;
    tx->changes = install->changes;
    tx->stamps = install->stamps;
    install->changes = NULL;
    install->stamps = NULL;
    for (uint32_t i = 0; i < tx->h.stamp_count; i++) {
        tx->stamps[i].old = tx->stamps[i].next;
        tx->stamps[i].next = tx->stamps[i].undo;
    }
}

static bool keep_foreign_directories(porter_transaction_t *tx)
{
    porter_kind_t kind;
    if (!porter_probe(PORTER_TX_HEADS, false, &kind))
        return false;
    if (kind != PORTER_DIR)
        return kind == PORTER_ABSENT;
    int64_t dir = os64_opendir(PORTER_TX_HEADS);
    if (dir < 0)
        return false;
    bool ok = true;
    int64_t n;
    os64_dirent_t e;
    while ((n = os64_readdir((int32_t)dir, &e)) == 1) {
        if (os64_streq(e.name, ".") || os64_streq(e.name, "..") || os64_streq(e.name, tx->h.scope))
            continue;
        char path[PORTER_PATH], head[PORTER_DIGEST];
        porter_tx_value_t v;
        porter_inventory_t foreign;
        if (!porter_lot(e.name) || !porter_tx_head_path(e.name, path) ||
            !porter_tx_value(path, &v) || !porter_tx_value_id(&v, head) || !*head ||
            !porter_read_receipt(&foreign, head, e.name)) {
            ok = false;
            break;
        }
        for (size_t i = 0; i < foreign.entry_count; i++) {
            const porter_entry_t *entry = &foreign.entries[i];
            if (entry->identity.kind != PORTER_DIR)
                continue;
            for (uint32_t j = 0; j < tx->h.change_count; j++) {
                porter_tx_change_t *change = &tx->changes[j];
                if (change->before.kind == PORTER_ABSENT && change->after.kind == PORTER_DIR &&
                    os64_streq(change->volume, entry->volume) && os64_streq(change->path, entry->relative))
                    change->flags |= PORTER_TX_KEEP_DIRECTORY;
            }
        }
        porter_release_inventory(&foreign);
    }
    return os64_close((int32_t)dir) == 0 && n == 0 && ok;
}

bool porter_tx_undo(const char *scope, bool approved, porter_report_t emit, void *context)
{
    const reporter_t r = {emit, context};
    os64_porter_owner_t owner;
    if (approved && !claim(&owner, &r))
        return false;
    if (!approved)
        owner = (os64_porter_owner_t){0};
    porter_transaction_t install = {0}, tx;
    porter_tx_init(&tx, PORTER_TX_UNDO, &owner);
    porter_tx_mounts_t mounts;
    bool ok = false;
    if (!porter_tx_mounts_open(&mounts))
        problem(&r, "persistent /home on NVMe ext2 is required for Porter state");
    else if (empty_operation(&r) && active_install(&mounts, scope, &install, &r)) {
        use_install_for_undo(&tx, &install);
        if (!undo_kernel_matches(&tx, &mounts.volumes))
            problem(&r, "booted or staged kernel changed; this system undo requires the kernel update slice");
        else if (!porter_tx_preflight(&mounts, &tx, true, false) || !porter_tx_metadata(&mounts, &tx, true) ||
            !receipt_matches(&tx.h.next_head, scope) || !keep_foreign_directories(&tx))
            problem(&r, "current files or ownership differ from the active receipt");
        else if (!porter_tx_backup(&mounts, &tx, false, false))
            problem(&r, "cannot verify the original backup");
        else {
            char message[160];
            os64_snprintf(message, sizeof(message), "Undo target %s in scope %s (%u destination changes)",
                          tx.h.target, tx.h.scope, tx.h.change_count);
            announce(&r, message);
            if (!approved)
                problem(&r, "use porter undo --yes to approve the displayed undo");
            else if (prepare(&r, &tx)) {
                if (!porter_tx_backup(&mounts, &tx, false, true) ||
                    !porter_tx_preflight(&mounts, &tx, true, false) ||
                    !porter_tx_metadata(&mounts, &tx, true))
                    problem(&r, "cannot stage undo or verify its before-state; run porter recover");
                else if (arm(&r, &tx)) {
                    if (!porter_tx_apply(&mounts, &tx, false, false, checkpoint, &r))
                        problem(&r, "undo stopped while restoring destinations; run porter recover");
                    else {
                        checkpoint(&r, "destinations");
                        ok = settle(&mounts, &tx, &r, true, true, false);
                    }
                }
            }
        }
    }
    if (ok)
        announce(&r, "undo committed");
    porter_tx_free(&install);
    porter_tx_free(&tx);
    porter_tx_mounts_close(&mounts);
    if (approved)
        porter_unclaim();
    return ok;
}

static void report_operation(const reporter_t *r, const porter_transaction_t *tx, const char *outcome)
{
    char message[200];
    os64_snprintf(message, sizeof(message), "Pending %s for %s target %s; %s",
                  tx->h.kind == PORTER_TX_INSTALL ? "install" : "undo", tx->h.scope, tx->h.target, outcome);
    announce(r, message);
}

static bool recover_record(porter_tx_mounts_t *m, porter_transaction_t *tx, const reporter_t *r)
{
    bool armed_exists, armed, complete_exists, complete;
    if (!porter_tx_marker(tx, "armed", &armed_exists, &armed) ||
        !porter_tx_marker(tx, "complete", &complete_exists, &complete) ||
        (armed_exists && !armed) || (!armed && complete_exists))
        return problem(r, "damaged operation records; evidence was retained");
    bool installing = tx->h.kind == PORTER_TX_INSTALL;
    if (!armed) {
        report_operation(r, tx, "discard prepared scratch");
        if (!retire_pointer(r, tx))
            return false;
        porter_tx_cleanup(m, tx, installing);
        return true;
    }
    if (!porter_tx_metadata(m, tx, false))
        return problem(r, "unexpected history head or volume stamp; evidence was retained");
    if (complete) {
        report_operation(r, tx, "reconcile the completed operation");
        if (!porter_tx_verify_applied(m, tx, installing) ||
            !receipt_matches(&tx->h.next_head, tx->h.scope))
            return problem(r, "completed operation has unexpected destination or receipt contents");
        return settle(m, tx, r, true, false, false);
    }

    report_operation(r, tx, installing ? "roll back the install" : "finish the undo");
    if (!porter_tx_preflight(m, tx, false, true))
        return problem(r, "unexpected destination contents; recovery will preserve them");
    if (!porter_tx_backup(m, tx, false, true))
        return problem(r, "cannot verify and stage the recorded backup");
    if (!porter_tx_apply(m, tx, false, true, NULL, NULL))
        return problem(r, "recovery stopped while restoring destinations");
    return settle(m, tx, r, !installing, false, installing);
}

bool porter_tx_recover(porter_report_t emit, void *context)
{
    const reporter_t r = {emit, context};
    os64_porter_owner_t owner;
    if (!claim(&owner, &r))
        return false;
    porter_tx_mounts_t mounts;
    porter_transaction_t tx = {0};
    char id[PORTER_TX_ID], digest[PORTER_DIGEST];
    bool exists, ok = false;
    if (!porter_tx_mounts_open(&mounts))
        problem(&r, "persistent /home on NVMe ext2 is required for recovery");
    else if (!porter_tx_pointer(id, digest, &exists))
        problem(&r, "operation pointer is damaged; evidence was retained");
    else if (!exists) {
        announce(&r, "no Porter operation is pending");
        ok = true;
    } else if (!porter_tx_read(&tx, id, digest))
        problem(&r, "the operation intent is missing or damaged; evidence was retained");
    else {
        ok = recover_record(&mounts, &tx, &r);
        if (ok)
            announce(&r, "recovery complete");
    }
    porter_tx_free(&tx);
    porter_tx_mounts_close(&mounts);
    porter_unclaim();
    return ok;
}

static bool was_active(char (*chain)[PORTER_DIGEST], size_t count, const char *id)
{
    for (size_t i = 0; i < count; i++)
        if (os64_streq(chain[i], id))
            return true;
    return false;
}

void porter_tx_notice(porter_report_t emit, void *context)
{
    const reporter_t r = {emit, context};
    porter_kind_t kind;
    if (!porter_probe(PORTER_TX_POINTER, false, &kind) || kind == PORTER_ABSENT)
        return;
    char id[PORTER_TX_ID], digest[PORTER_DIGEST];
    bool pending;
    if (!porter_tx_pointer(id, digest, &pending))
        problem(&r, "the operation pointer could not be verified");
    else if (pending) {
        porter_transaction_t tx;
        if (porter_tx_read(&tx, id, digest)) {
            bool complete_exists, complete, armed_exists, armed;
            bool valid = porter_tx_marker(&tx, "complete", &complete_exists, &complete) &&
                         porter_tx_marker(&tx, "armed", &armed_exists, &armed);
            const char *outcome = !valid || (armed_exists && !armed) || (!armed && complete_exists)
                ? "inspect damaged records" : !armed ? "discard prepared scratch"
                : complete ? "reconcile completion"
                : tx.h.kind == PORTER_TX_INSTALL ? "roll back the install" : "finish the undo";
            char message[200];
            os64_snprintf(message, sizeof(message), "Pending %s for %s target %s; %s",
                          tx.h.kind == PORTER_TX_INSTALL ? "install" : "undo",
                          tx.h.scope, tx.h.target, outcome);
            problem(&r, message);
            porter_tx_free(&tx);
        } else {
            problem(&r, "the pending operation intent could not be verified");
        }
    }
}

bool porter_tx_history(const char *scope, porter_report_t emit, void *context)
{
    const reporter_t r = {emit, context};
    porter_tx_notice(emit, context);
    char path[PORTER_PATH], head[PORTER_DIGEST];
    porter_tx_value_t value;
    if (!porter_tx_head_path(scope, path) || !porter_tx_value(path, &value) ||
        !porter_tx_value_id(&value, head))
        return problem(&r, "cannot verify this scope's active head");
    char (*chain)[PORTER_DIGEST] = os64_malloc(PORTER_RECORDS * PORTER_DIGEST);
    if (!chain)
        return problem(&r, "cannot allocate history view");
    bool ok = true;
    size_t count = 0;
    char message[190];
    if (!*head)
        announce(&r, "no active receipt for this scope");
    while (*head) {
        porter_inventory_t receipt;
        if (count == PORTER_RECORDS || was_active(chain, count, head) ||
            !porter_read_receipt(&receipt, head, scope)) {
            ok = false;
            break;
        }
        os64_snprintf(message, sizeof(message), "%s %s target %s", count ? "UNDOABLE" : "ACTIVE",
                      head, receipt.target);
        announce(&r, message);
        os64_strcopy(chain[count++], PORTER_DIGEST, head);
        os64_strcopy(head, sizeof(head), receipt.previous ? receipt.previous : "");
        porter_release_inventory(&receipt);
    }
    porter_kind_t kind;
    if (ok && porter_probe(PORTER_TX_RECEIPTS, false, &kind) && kind == PORTER_DIR) {
        int64_t dir = os64_opendir(PORTER_TX_RECEIPTS);
        os64_dirent_t entry;
        int64_t next = -1;
        while (dir >= 0 && (next = os64_readdir((int32_t)dir, &entry)) == 1) {
            if (os64_strlen(entry.name) != 69 || !os64_streq(entry.name + 64, ".json"))
                continue;
            entry.name[64] = 0;
            if (!porter_hex(entry.name, 64) || was_active(chain, count, entry.name))
                continue;
            porter_inventory_t receipt;
            if (!porter_read_receipt(&receipt, entry.name, scope))
                continue;
            bool show = !receipt.operation;
            porter_transaction_t archived;
            if (receipt.operation && porter_tx_read(&archived, receipt.operation, NULL)) {
                bool exists, valid;
                show = porter_tx_marker(&archived, "complete", &exists, &valid) && valid;
                porter_tx_free(&archived);
            }
            if (show) {
                os64_snprintf(message, sizeof(message), "INACTIVE %s target %s", entry.name, receipt.target);
                announce(&r, message);
            }
            porter_release_inventory(&receipt);
        }
        if (dir >= 0)
            ok = os64_close((int32_t)dir) == 0 && next == 0;
        else
            ok = false;
    }
    os64_free(chain);
    return ok || problem(&r, "history view is incomplete");
}
