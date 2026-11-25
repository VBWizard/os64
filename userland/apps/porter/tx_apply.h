#ifndef OS64_PORTER_TX_APPLY_H
#define OS64_PORTER_TX_APPLY_H

#include "transaction.h"

typedef struct {
    porter_volumes_t volumes;
    const porter_mount_t *home;
    int32_t pins[PORTER_MOUNTS];
} porter_tx_mounts_t;

bool porter_tx_mounts_open(porter_tx_mounts_t *mounts);
void porter_tx_mounts_close(porter_tx_mounts_t *mounts);
bool porter_tx_pin(porter_tx_mounts_t *mounts, const porter_mount_t *mount);
bool porter_tx_location(porter_tx_mounts_t *mounts, const char *volume, const char *relative,
                        porter_location_t *location);

bool porter_tx_head_path(const char *scope, char path[PORTER_PATH]);
bool porter_tx_stamp_path(const porter_mount_t *mount, const char *scope, char path[PORTER_PATH]);
bool porter_tx_metadata(porter_tx_mounts_t *mounts, const porter_transaction_t *tx, bool old_only);
bool porter_tx_set_metadata(porter_tx_mounts_t *mounts, const porter_transaction_t *tx,
                            bool forward, bool head);

bool porter_tx_scratch(porter_tx_mounts_t *mounts, const porter_transaction_t *tx,
                       uint32_t row, char role, char path[PORTER_PATH], bool ensure);
bool porter_tx_backup(porter_tx_mounts_t *mounts, const porter_transaction_t *tx,
                      bool create, bool restore);
bool porter_tx_preflight(porter_tx_mounts_t *mounts, const porter_transaction_t *tx,
                         bool forward, bool allow_other);
bool porter_tx_verify_applied(porter_tx_mounts_t *mounts, const porter_transaction_t *tx, bool forward);
bool porter_tx_apply(porter_tx_mounts_t *mounts, const porter_transaction_t *tx,
                     bool forward, bool allow_other,
                     void (*checkpoint)(const void *, const char *), const void *context);
void porter_tx_cleanup(porter_tx_mounts_t *mounts, const porter_transaction_t *tx, bool discard_backup);

#endif
