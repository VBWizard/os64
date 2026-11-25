#ifndef OS64_PORTER_TRANSACTION_H
#define OS64_PORTER_TRANSACTION_H

#include "porter.h"
#include "os64/storage.h"

#define PORTER_TX_ID 49
#define PORTER_TX_OPERATIONS PORTER_STATE "/operations"
#define PORTER_TX_POINTER PORTER_STATE "/operation"
#define PORTER_TX_RECEIPTS PORTER_STATE "/receipts"
#define PORTER_TX_HEADS PORTER_STATE "/heads"
#define PORTER_TX_VALUE 128
#define PORTER_TX_CHANGES (2 * PORTER_RECORDS)
#define PORTER_TX_KEEP_DIRECTORY 1u

typedef enum { PORTER_TX_INSTALL = 1, PORTER_TX_UNDO = 2 } porter_tx_kind_t;

typedef struct {
    uint32_t present, size;
    char data[PORTER_TX_VALUE];
} porter_tx_value_t;

typedef struct {
    char volume[PORTER_GUID], path[PORTER_PATH];
    porter_identity_t before, after;
    uint32_t flags;
} porter_tx_change_t;

typedef struct {
    char volume[PORTER_GUID];
    porter_tx_value_t old, next, undo;
} porter_tx_stamp_t;

typedef struct {
    char magic[8];
    uint32_t version, kind, change_count, stamp_count;
    os64_porter_owner_t owner;
    char id[PORTER_TX_ID], source[PORTER_TX_ID];
    char scope[PORTER_LOT], target[PORTER_DIGEST], kernel[PORTER_DIGEST];
    char kernel_volume[PORTER_GUID], kernel_path[PORTER_PATH];
    char backup_volume[PORTER_GUID], backup_path[PORTER_PATH];
    porter_tx_value_t old_head, next_head, undo_head;
} porter_tx_header_t;

typedef struct {
    porter_tx_header_t h;
    porter_tx_change_t *changes;
    porter_tx_stamp_t *stamps;
    char digest[PORTER_DIGEST];
} porter_transaction_t;

typedef void (*porter_report_t)(void *context, bool error, const char *message);

void porter_tx_init(porter_transaction_t *tx, porter_tx_kind_t kind, const os64_porter_owner_t *owner);
void porter_tx_free(porter_transaction_t *tx);
bool porter_tx_store(porter_transaction_t *tx);
bool porter_tx_read(porter_transaction_t *tx, const char *id, const char *digest);
bool porter_tx_pointer(char id[PORTER_TX_ID], char digest[PORTER_DIGEST], bool *exists);
bool porter_tx_publish_pointer(const porter_transaction_t *tx);
bool porter_tx_marker(const porter_transaction_t *tx, const char *name, bool *exists, bool *valid);
bool porter_tx_publish_marker(const porter_transaction_t *tx, const char *name);

bool porter_tx_mkdirs(const char *path);
bool porter_tx_write_exact(int32_t fd, const void *data, size_t size);
bool porter_tx_write(const char *path, const char *id, const void *data, size_t size, bool replace);
bool porter_tx_value(const char *path, porter_tx_value_t *value);
void porter_tx_discard_metadata_temp(const char *path, const char *id);
bool porter_tx_value_equal(const porter_tx_value_t *a, const porter_tx_value_t *b);
void porter_tx_id_value(porter_tx_value_t *value, const char *id);
bool porter_tx_value_id(const porter_tx_value_t *value, char id[PORTER_DIGEST]);
bool porter_tx_set_value(const char *path, const char *id, const porter_tx_value_t *value);

int64_t porter_claim(os64_porter_owner_t *owner);
void porter_unclaim(void);
bool porter_barrier(void);

bool porter_tx_install(const char *host, uint16_t port, const porter_inventory_t *target,
                       bool baseline, bool adopt, const porter_plan_t *approved,
                       const porter_config_t *approved_config, porter_report_t report, void *context);
void porter_tx_notice(porter_report_t report, void *context);
bool porter_tx_history(const char *scope, porter_report_t report, void *context);
bool porter_tx_undo(const char *scope, bool approved, porter_report_t report, void *context);
bool porter_tx_recover(porter_report_t report, void *context);

#endif
