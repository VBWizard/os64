#ifndef OS64_PORTER_H
#define OS64_PORTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PORTER_PATH       256
#define PORTER_LOT        32
#define PORTER_DIGEST     65
#define PORTER_GUID       37
#define PORTER_RECORDS    16384
#define PORTER_MOUNTS     128
#define PORTER_METADATA   (16u * 1024u * 1024u)
#define PORTER_STATE      "/home/.porter"
#define PORTER_LOG_DIR    "/var/porter"

typedef enum { PORTER_ABSENT, PORTER_DIR, PORTER_FILE } porter_kind_t;

typedef struct {
    porter_kind_t kind;
    uint64_t size;
    char hash[PORTER_DIGEST];
} porter_identity_t;

typedef struct {
    const char *lot, *path;
    porter_identity_t identity;
    // Present in receipts. "relative" begins at the volume's root.
    const char *volume, *relative;
} porter_entry_t;

typedef struct {
    const char *scope, *kernel, *previous, *target;
    const char **lots;
    porter_entry_t *entries;
    size_t lot_count, entry_count;
    char *data;
    char digest[PORTER_DIGEST];
} porter_inventory_t;

typedef struct {
    char scope[PORTER_LOT], target[PORTER_DIGEST];
} porter_selection_t;

typedef struct {
    char id[33], name[334], published_at[48];
    bool baseline;
    porter_selection_t *targets;
    size_t count;
} porter_publication_t;

typedef struct {
    char where[PORTER_PATH], guid[PORTER_GUID];
    bool disk, writable, fat;
    uint64_t free_bytes, block_size;
    bool space_known;
} porter_mount_t;

typedef struct {
    porter_mount_t *mounts;
    size_t count;
} porter_volumes_t;

typedef struct {
    const porter_mount_t *mount;
    char path[PORTER_PATH], relative[PORTER_PATH];
} porter_location_t;

typedef struct {
    char lot[PORTER_LOT], suffix[PORTER_LOT], directory[PORTER_PATH];
} porter_rule_t;

typedef struct {
    porter_rule_t *rules;
    size_t count;
    char backup[PORTER_PATH];
} porter_config_t;

typedef enum {
    PORTER_UNCHANGED, PORTER_ADD, PORTER_ADOPT, PORTER_REPLACE, PORTER_DOWNGRADE,
    PORTER_RETIRE, PORTER_PRESERVE, PORTER_UNMANAGED, PORTER_CONFLICT
} porter_action_t;

typedef struct {
    porter_action_t action;
    const porter_entry_t *entry;
    porter_location_t where;
    porter_identity_t expected, before, after;
    const char *reason;
} porter_plan_item_t;

typedef struct {
    const porter_mount_t *mount;
    uint64_t stage, backup;
} porter_space_t;

typedef struct {
    porter_plan_item_t *items;
    porter_space_t *spaces;
    size_t count, capacity, space_count, conflicts;
    uint64_t transfer_bytes, backup_bytes;
    const char *blocker;
    const char *kernel_status;
    char booted_kernel[PORTER_DIGEST], staged_kernel[PORTER_DIGEST];
    char installed[PORTER_DIGEST], receipt[PORTER_DIGEST], target[PORTER_DIGEST];
    bool adoption;
} porter_plan_t;

bool porter_grow(void **array, size_t *cap, size_t used, size_t element, size_t limit);
bool porter_component_path(const char *path, bool absolute);
bool porter_lot(const char *name);
bool porter_hex(const char *text, size_t digits);
bool porter_guid(const char *name);
bool porter_folded_equal(const char *a, const char *b, bool fold);
bool porter_under(const char *path, const char *root);
bool porter_join(char out[PORTER_PATH], const char *left, const char *right);
bool porter_parent(char out[PORTER_PATH], const char *path);
const char *porter_basename(const char *path);
void porter_sha(const void *data, size_t size, char hash[PORTER_DIGEST]);
bool porter_identity_equal(const porter_identity_t *a, const porter_identity_t *b);

bool porter_parse_inventory(porter_inventory_t *out, char *data, size_t count, bool receipt);
void porter_release_inventory(porter_inventory_t *inventory);
const porter_entry_t *porter_find(const porter_inventory_t *inventory,
                                  const char *lot, const char *path);
bool porter_has_lot(const porter_inventory_t *inventory, const char *lot);
bool porter_parse_publication(porter_publication_t *out, char *line, size_t size);
bool porter_catalog(const char *host, uint16_t port,
                    bool (*visit)(const porter_publication_t *, void *), void *context);
bool porter_get_manifest(const char *host, uint16_t port, const porter_selection_t *selection,
                         porter_inventory_t *inventory);

bool porter_load_volumes(porter_volumes_t *volumes);
bool porter_load_config(porter_config_t *config, const porter_volumes_t *volumes);
const char *porter_route(const porter_config_t *config, const char *lot, const char *path);
const porter_mount_t *porter_mount_at(const porter_volumes_t *volumes, const char *path);
const porter_mount_t *porter_mount_by_guid(const porter_volumes_t *volumes, const char *guid);
bool porter_locate(const porter_volumes_t *volumes, const char *path, porter_location_t *out,
                   const char **failure);
bool porter_log_contains(const porter_mount_t *mount, const char *path, const char *relative);
bool porter_protected(const porter_config_t *config, const porter_volumes_t *volumes,
                      const porter_location_t *where);
bool porter_probe(const char *path, bool fat, porter_kind_t *kind);
bool porter_inspect(const porter_location_t *where, porter_identity_t *id);
bool porter_hash_file(const char *path, uint64_t expected, char hash[PORTER_DIGEST]);
bool porter_boot_digest(char hash[PORTER_DIGEST]);

// Plan construction reads live files, mounted volumes, immutable history
// and its per-volume stamps. The frontend owns rendering and approval.
bool porter_preview(porter_plan_t *plan, const porter_inventory_t *target, bool baseline,
                    bool force_adopt, const porter_config_t *config, const porter_volumes_t *volumes);
void porter_release_plan(porter_plan_t *plan);

#endif
