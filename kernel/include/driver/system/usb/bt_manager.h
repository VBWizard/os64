#ifndef OS64_BT_MANAGER_H
#define OS64_BT_MANAGER_H
#include "driver/system/usb/bt_le.h"

typedef struct {
    bool loaded, automatic, suppressed, blocked, scan_owned, dirty;
    uint64_t revision_seen, generation, next_attempt, next_save;
    uint32_t backoff_ms;
    const char *storage_error;
} bt_manager_t;

// Serialized with LE, scan and manual commands; these functions perform no I/O.
void bt_manager_observe(bt_manager_t *,const bt_le_t *,uint64_t);
void bt_manager_step(bt_manager_t *,bt_le_t *,bt_scan_t *,uint64_t);
bool bt_manager_command(bt_manager_t *,bt_le_t *,bt_scan_t *,const char *,size_t,uint64_t);
size_t bt_manager_status(const bt_manager_t *,char *,size_t);

#define BT_BOND_SLOTS 2
#define BT_BOND_RECORD_BYTES 80
void bt_bond_encode(uint8_t [BT_BOND_RECORD_BYTES],const bt_le_bond_t *,bool);
bool bt_bond_decode(bt_le_bond_t *,bool *,const uint8_t *,size_t);
// Task context; marshals through kernel mappings without the USB poll lock.
bool bt_bond_load(bt_le_bond_t *,bool *);
bool bt_bond_save(const bt_le_bond_t *,bool);
bool bt_bond_load_slot(unsigned,bt_le_bond_t *,bool *);
bool bt_bond_save_slot(unsigned,const bt_le_bond_t *,bool);
#endif
