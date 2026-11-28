#include "driver/net/ax210_boot.h"
#include "memcpy.h"

#define AX210_SCRATCH_4K_RX      (1u << 16)
#define AX210_SCRATCH_REPLY_RX  (1u << 17)
#define AX210_SCRATCH_256B_TX   (3u << 18)

void ax210_gen3_set_rings(ax210_boot_dma_t *dma, uint64_t control_phys, uint64_t rx_phys,
                         uint16_t mac_revision)
{
    uint64_t info = control_phys + offsetof(ax210_boot_dma_t, peripheral_info);
    dma->context.peripheral_info = info;
    // The firmware accesses these dummy tail slots in the info page.
    dma->context.transfer_tail = info + AX210_BOOT_PAGE / 2;
    dma->context.completion_tail = info + 3 * AX210_BOOT_PAGE / 4;
    dma->context.completion_head = control_phys + offsetof(ax210_boot_dma_t, completion_head);
    dma->context.transfer_ring = control_phys + offsetof(ax210_boot_dma_t, command_ring);
    dma->context.completion_ring = control_phys + offsetof(ax210_boot_dma_t, rx_used);
    dma->context.transfer_ring_size = 8 - 3; // log2(256) - 3 for the TFD ring.
    dma->context.completion_ring_size = 9;  // log2(512) for the RX ring.
    dma->context.scratch = control_phys + offsetof(ax210_boot_dma_t, scratch);
    dma->context.scratch_size = sizeof(dma->scratch);
    dma->scratch.mac = mac_revision;
    dma->scratch.size_dwords = sizeof(dma->scratch) / 4;
    dma->scratch.flags = AX210_SCRATCH_4K_RX | AX210_SCRATCH_REPLY_RX | AX210_SCRATCH_256B_TX;
    dma->scratch.rx_free_ring = control_phys + offsetof(ax210_boot_dma_t, rx_free);
    for (unsigned i = 0; i < AX210_RX_COUNT; ++i) {
        dma->rx_free[i].token = i;
        dma->rx_free[i].address = rx_phys + i * AX210_BOOT_PAGE;
    }
}

bool ax210_gen3_command(ax210_boot_dma_t *dma, uint64_t control_phys, uint8_t index,
                        uint8_t group, uint8_t opcode, const void *payload, size_t length)
{
    ax210_command_packet_t *packet = &dma->command_payloads[index];
    if (length > sizeof(packet->payload))
        return false;
    *packet = (ax210_command_packet_t) {
        .opcode = opcode, .group = group, .index = index, .length = length,
    };
    memcpy(packet->payload, payload, length);
    dma->command_ring[index] = (ax210_command_tfd_t) {
        .count = 1,
        .length = offsetof(ax210_command_packet_t, payload) + length,
        .address = control_phys + offsetof(ax210_boot_dma_t, command_payloads)
                   + index * sizeof(ax210_command_packet_t),
    };
    return true;
}

static uint16_t read16(const uint8_t *p)
{
    return p[0] | (uint16_t)p[1] << 8;
}

static uint32_t read32(const uint8_t *p)
{
    return read16(p) | (uint32_t)read16(p + 2) << 16;
}

ax210_rx_result_t ax210_decode_rx(const uint8_t *buffer, size_t capacity, ax210_notification_t *packet)
{
    if (capacity < 8)
        return AX210_RX_MALFORMED;
    uint32_t flags = read32(buffer);
    if (flags == 0x55550000 || (flags == 0 && read32(buffer + 4) == 0))
        return AX210_RX_EMPTY;
    uint32_t length = flags & 0x3fff; // Includes the four-byte command header.
    if (length < 4 || length > capacity - 4)
        return AX210_RX_MALFORMED;

    *packet = (ax210_notification_t) {
        .opcode = buffer[4], .group = buffer[5] & ~0x40u,
        .index = buffer[6], .queue = buffer[7], .failed = (buffer[5] & 0x40u) != 0,
        .payload = { buffer + 8, length - 4 },
    };
    return AX210_RX_PACKET;
}

bool ax210_decode_alive(const ax210_notification_t *packet, ax210_alive_t *alive)
{
    // The pinned image advertises ALIVE v6, which carries three SKU words.
    if (packet->payload.length != 144)
        return false;
    const uint8_t *payload = packet->payload.data;
    *alive = (ax210_alive_t) {
        .status = read16(payload),
        .flags = read16(payload + 2),
        .lmac_major = read32(payload + 4), .lmac_minor = read32(payload + 8),
        .umac_major = read32(payload + 100), .umac_minor = read32(payload + 104),
        .sku = { read32(payload + 116), read32(payload + 120), read32(payload + 124) },
    };
    return true;
}

bool ax210_decode_pnvm_status(const ax210_notification_t *packet, uint32_t *status)
{
    if (packet->payload.length != 4)
        return false;
    *status = read32(packet->payload.data);
    return true;
}

bool ax210_decode_nvm_info(const ax210_notification_t *packet, ax210_nvm_info_t *info)
{
    // NVM_GET_INFO v4 has 110 fixed channel entries following the header.
    if (packet->payload.length != 28 + 110 * 4)
        return false;
    const uint8_t *p = packet->payload.data;
    uint32_t channels = read32(p + 24);
    if (channels > 110)
        return false;
    uint16_t valid_channels = 0;
    for (uint32_t i = 0; i < channels; ++i)
        valid_channels += (read32(p + 28 + i * 4) & 1u) != 0;
    *info = (ax210_nvm_info_t) {
        .flags = read32(p), .version = read16(p + 4), .hw_addresses = p[7],
        .sku_flags = read32(p + 8), .tx_chains = read32(p + 12), .rx_chains = read32(p + 16),
        .lar_enabled = read32(p + 20), .channels = channels, .valid_channels = valid_channels,
    };
    return true;
}
