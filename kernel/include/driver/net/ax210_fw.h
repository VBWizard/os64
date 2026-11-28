#ifndef AX210_FW_H
#define AX210_FW_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define AX210_FW_API 77
#define AX210_FW_MAX_SECTIONS 64  // Entries in each Gen3 DRAM address table.

typedef struct {
    const uint8_t *data;
    size_t length;
} ax210_fw_span_t;

typedef enum {
    AX210_FW_LMAC,
    AX210_FW_UMAC,
    AX210_FW_PAGING,
    AX210_FW_GROUP_COUNT,
} ax210_fw_group_t;

typedef struct {
    ax210_fw_span_t sections[AX210_FW_MAX_SECTIONS];
    size_t count;
} ax210_fw_sections_t;

typedef struct {
    uint32_t api, build, revision;
    bool fragmented_pnvm;
    ax210_fw_span_t loader;
    ax210_fw_sections_t groups[AX210_FW_GROUP_COUNT];
} ax210_fw_image_t;

typedef struct {
    uint32_t version;
    ax210_fw_sections_t image;
} ax210_pnvm_t;

typedef enum {
    AX210_FW_OK,
    AX210_FW_BAD_HEADER,
    AX210_FW_BAD_TLV,
    AX210_FW_WRONG_API,
    AX210_FW_BAD_LAYOUT,
    AX210_FW_NO_MATCH,
} ax210_fw_result_t;

// The views borrow immutable firmware bytes. Output is usable on AX210_FW_OK.
ax210_fw_result_t ax210_fw_parse(const void *data, size_t length, ax210_fw_image_t *image);
ax210_fw_result_t ax210_pnvm_select(const void *data, size_t length, const uint32_t sku[3],
                                   uint16_t mac_type, uint16_t rf_type, ax210_pnvm_t *pnvm);

extern const uint8_t ax210_ucode_start[], ax210_ucode_end[];
extern const uint8_t ax210_pnvm_start[], ax210_pnvm_end[];

#endif
