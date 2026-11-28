#ifndef AX210_BOOT_H
#define AX210_BOOT_H

#include "driver/net/ax210_fw.h"

// Typhoon Peak Gen3 boot ABI from the iwx reference pinned in WIFI_W1.md.
#define AX210_BOOT_PAGE 4096u
#define AX210_RX_COUNT 512u
#define AX210_COMMAND_COUNT 256u

typedef struct __attribute__((packed)) {
    uint16_t version, size;
    uint32_t config;
    uint64_t peripheral_info, completion_head, transfer_tail, completion_tail, transfer_head;
    uint16_t completion_index_count, transfer_index_count;
    uint64_t transfer_ring, completion_ring;
    uint16_t transfer_ring_size, completion_ring_size;
    uint16_t doorbell_vectors[2], msi_vectors[2];
    uint8_t header_footer_sizes[4];
    uint16_t message_flags, peripheral_msi;
    uint64_t scratch;
    uint32_t scratch_size, reserved;
} ax210_gen3_context_t;

typedef struct __attribute__((packed)) {
    uint16_t mac, version, size_dwords, reserved;
    uint32_t flags, reserved2;
    uint64_t pnvm_address;
    uint32_t pnvm_bytes, pnvm_reserved;
    uint8_t monitor[16];
    uint64_t rx_free_ring;
    uint32_t rx_reserved;
    uint8_t power_and_reserved[64];
    uint64_t umac[AX210_FW_MAX_SECTIONS];
    uint64_t lmac[AX210_FW_MAX_SECTIONS];
    uint64_t paging[AX210_FW_MAX_SECTIONS];
} ax210_gen3_scratch_t;

typedef struct {
    uint16_t token;
    uint16_t reserved[3];
    uint64_t address;
} ax210_rx_transfer_t;

typedef struct {
    uint32_t reserved;
    uint16_t token;
    uint8_t flags, padding[25];
} ax210_rx_completion_t;

typedef struct __attribute__((packed)) {
    uint16_t count, length;
    uint64_t address;
    uint8_t reserved[244];
} ax210_command_tfd_t;

typedef struct {
    uint8_t opcode, group, index, queue;
    uint16_t length;
    uint8_t reserved, version;
    uint8_t payload[56];
} ax210_command_packet_t;

typedef struct {
    _Alignas(AX210_BOOT_PAGE) ax210_gen3_context_t context;
    _Alignas(AX210_BOOT_PAGE) ax210_gen3_scratch_t scratch;
    _Alignas(AX210_BOOT_PAGE) uint32_t peripheral_info[AX210_BOOT_PAGE / 4];
    _Alignas(AX210_BOOT_PAGE) uint16_t completion_head;
    _Alignas(AX210_BOOT_PAGE) ax210_rx_transfer_t rx_free[AX210_RX_COUNT];
    _Alignas(AX210_BOOT_PAGE) ax210_rx_completion_t rx_used[AX210_RX_COUNT];
    _Alignas(AX210_BOOT_PAGE) ax210_command_tfd_t command_ring[AX210_COMMAND_COUNT];
    _Alignas(AX210_BOOT_PAGE) ax210_command_packet_t command_payloads[AX210_COMMAND_COUNT];
} ax210_boot_dma_t;

_Static_assert(sizeof(ax210_gen3_context_t) == 104, "Gen3 context length");
_Static_assert(offsetof(ax210_gen3_context_t, transfer_ring) == 52, "Gen3 ring offset");
_Static_assert(offsetof(ax210_gen3_context_t, scratch) == 88, "Gen3 scratch offset");
_Static_assert(sizeof(ax210_gen3_scratch_t) == 1660, "Gen3 scratch length");
_Static_assert(offsetof(ax210_gen3_scratch_t, rx_free_ring) == 48, "Gen3 free ring offset");
_Static_assert(offsetof(ax210_gen3_scratch_t, umac) == 124, "Gen3 image table offset");
_Static_assert(offsetof(ax210_gen3_scratch_t, pnvm_address) == 16, "Gen3 PNVM config offset");
_Static_assert(sizeof(ax210_rx_transfer_t) == 16, "Gen3 RX transfer descriptor");
_Static_assert(sizeof(ax210_rx_completion_t) == 32, "Gen3 RX completion descriptor");
_Static_assert(sizeof(ax210_command_tfd_t) == 256, "Gen3 command TFD");
_Static_assert(offsetof(ax210_command_tfd_t, address) == 4, "Gen3 command TB address");
_Static_assert(sizeof(ax210_command_packet_t) == 64, "Gen3 command buffer stride");
_Static_assert(offsetof(ax210_command_packet_t, payload) == 8, "Gen3 wide command header");

typedef struct {
    uint16_t status, flags;
    uint32_t lmac_major, lmac_minor, umac_major, umac_minor, sku[3];
} ax210_alive_t;

typedef struct {
    uint8_t opcode, group, index, queue;
    bool failed;
    ax210_fw_span_t payload;
} ax210_notification_t;

typedef struct {
    uint32_t flags, sku_flags, tx_chains, rx_chains, lar_enabled;
    uint16_t version;
    uint8_t hw_addresses;
    uint16_t channels, valid_channels;
} ax210_nvm_info_t;

typedef enum {
    AX210_RX_EMPTY,
    AX210_RX_PACKET,
    AX210_RX_MALFORMED,
} ax210_rx_result_t;

void ax210_gen3_set_rings(ax210_boot_dma_t *dma, uint64_t control_phys, uint64_t rx_phys,
                         uint16_t mac_revision);
bool ax210_gen3_command(ax210_boot_dma_t *dma, uint64_t control_phys, uint8_t index,
                        uint8_t group, uint8_t opcode, const void *payload, size_t length);
ax210_rx_result_t ax210_decode_rx(const uint8_t *buffer, size_t capacity, ax210_notification_t *packet);
bool ax210_decode_alive(const ax210_notification_t *packet, ax210_alive_t *alive);
bool ax210_decode_pnvm_status(const ax210_notification_t *packet, uint32_t *status);
bool ax210_decode_nvm_info(const ax210_notification_t *packet, ax210_nvm_info_t *info);

#endif
