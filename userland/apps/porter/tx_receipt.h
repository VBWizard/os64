#ifndef OS64_PORTER_TX_RECEIPT_H
#define OS64_PORTER_TX_RECEIPT_H

#include "transaction.h"

typedef struct {
    char *data;
    size_t length, capacity, entries;
    bool volumes[PORTER_MOUNTS];
    char digest[PORTER_DIGEST];
} porter_receipt_text_t;

bool porter_build_receipt(porter_receipt_text_t *receipt, const porter_transaction_t *tx,
                          const porter_inventory_t *target, const porter_inventory_t *older,
                          const porter_plan_t *plan, const porter_volumes_t *volumes);
bool porter_publish_receipt(const porter_receipt_text_t *receipt, const char *operation);

#endif
