#include "driver/net/ax210_fw.h"
#include <stdbool.h>

// Public wire definitions from OpenBSD if_iwxreg.h. See WIFI_W1.md for the
// exact reference revision. Unknown TLVs still have to pass framing checks.
#define FW_MAGIC                 0x0a4c5749u
#define FW_HEADER_SIZE           88u
#define FW_TLV_SEC_RT            19u
#define FW_TLV_CAPABILITIES      30u
#define FW_TLV_VERSION           36u
#define FW_TLV_IML               52u
#define FW_TLV_HW_TYPE           58u
#define FW_TLV_PNVM_VERSION      62u
#define FW_TLV_PNVM_SKU          64u
#define FW_CPU_SEPARATOR         0xffffccccu
#define FW_PAGING_SEPARATOR      0xaaaabbbbu
#define PNVM_LEGACY_SEPARATOR    0xddddeeeeu

typedef struct {
    uint32_t type;
    ax210_fw_span_t value;
} firmware_tlv_t;

static uint16_t read_le16(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | (uint16_t)bytes[1] << 8;
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return read_le16(bytes) | (uint32_t)read_le16(bytes + 2) << 16;
}

static ax210_fw_result_t next_tlv(ax210_fw_span_t *remaining, firmware_tlv_t *tlv)
{
    if (remaining->length < 8)
        return AX210_FW_BAD_TLV;

    size_t length = read_le32(remaining->data + 4);
    size_t padding = (4 - (length & 3)) & 3;
    size_t available = remaining->length - 8;
    if (length > available || padding > available - length)
        return AX210_FW_BAD_TLV;

    tlv->type = read_le32(remaining->data);
    tlv->value = (ax210_fw_span_t){ remaining->data + 8, length };
    size_t consumed = 8 + length + padding;
    remaining->data += consumed;
    remaining->length -= consumed;
    return AX210_FW_OK;
}

ax210_fw_result_t ax210_fw_parse(const void *data, size_t length, ax210_fw_image_t *image)
{
    const uint8_t *header = data;
    *image = (ax210_fw_image_t){ 0 };
    if (length < FW_HEADER_SIZE || read_le32(header) != 0 || read_le32(header + 4) != FW_MAGIC)
        return AX210_FW_BAD_HEADER;

    ax210_fw_group_t group = AX210_FW_LMAC;
    ax210_fw_span_t remaining = { header + FW_HEADER_SIZE, length - FW_HEADER_SIZE };
    while (remaining.length != 0) {
        firmware_tlv_t tlv;
        ax210_fw_result_t result = next_tlv(&remaining, &tlv);
        if (result != AX210_FW_OK)
            return result;

        const uint8_t *value = tlv.value.data;
        size_t value_length = tlv.value.length;
        switch (tlv.type) {
        case FW_TLV_VERSION:
            if (value_length != 12 || image->api != 0)
                return AX210_FW_BAD_LAYOUT;
            image->api = read_le32(value);
            if (image->api != AX210_FW_API)
                return AX210_FW_WRONG_API;
            image->build = read_le32(value + 4);
            image->revision = read_le32(value + 8);
            break;
        case FW_TLV_IML:
            if (value_length == 0 || image->loader.data != NULL)
                return AX210_FW_BAD_LAYOUT;
            image->loader = tlv.value;
            break;
        case FW_TLV_CAPABILITIES:
            if (value_length != 8)
                return AX210_FW_BAD_LAYOUT;
            if (read_le32(value) == 1)
                image->fragmented_pnvm = (read_le32(value + 4) & 1u) != 0;
            break;
        case FW_TLV_SEC_RT: {
            if (value_length <= 4)
                return AX210_FW_BAD_LAYOUT;
            uint32_t offset = read_le32(value);
            ax210_fw_sections_t *sections = &image->groups[group];
            if (offset == FW_CPU_SEPARATOR || offset == FW_PAGING_SEPARATOR) {
                if (sections->count == 0 ||
                    (offset == FW_CPU_SEPARATOR && group != AX210_FW_LMAC) ||
                    (offset == FW_PAGING_SEPARATOR && group != AX210_FW_UMAC))
                    return AX210_FW_BAD_LAYOUT;
                ++group;
                break;
            }
            if (sections->count == AX210_FW_MAX_SECTIONS)
                return AX210_FW_BAD_LAYOUT;
            // Gen3 consumes the payload; the firmware's leading offset is
            // a container field and must not be copied into a DMA section.
            sections->sections[sections->count++] =
                (ax210_fw_span_t){ value + 4, value_length - 4 };
            break;
        }
        default:
            break;
        }
    }

    if (image->api == 0)
        return AX210_FW_WRONG_API;
    if (image->loader.data == NULL || group != AX210_FW_PAGING ||
        image->groups[AX210_FW_PAGING].count == 0)
        return AX210_FW_BAD_LAYOUT;
    return AX210_FW_OK;
}

ax210_fw_result_t ax210_pnvm_select(const void *data, size_t length, const uint32_t sku[3],
                                   uint16_t mac_type, uint16_t rf_type, ax210_pnvm_t *pnvm)
{
    *pnvm = (ax210_pnvm_t){ 0 };
    ax210_fw_span_t remaining = { data, length };
    bool sku_match = false;
    bool hw_match = false;
    bool found = false;
    ax210_pnvm_t candidate = { 0 };

    // Each SKU TLV opens a candidate. Hardware ID and data belong to it
    // until the next SKU, regardless of the ordering of TLVs within it.
    while (remaining.length != 0) {
        firmware_tlv_t tlv;
        ax210_fw_result_t result = next_tlv(&remaining, &tlv);
        if (result != AX210_FW_OK)
            return result;

        const uint8_t *value = tlv.value.data;
        size_t value_length = tlv.value.length;
        if (tlv.type == FW_TLV_PNVM_SKU) {
            if (value_length != 12)
                return AX210_FW_BAD_LAYOUT;
            if (sku_match && hw_match) {
                if (found || candidate.image.count == 0 || candidate.version == 0)
                    return AX210_FW_BAD_LAYOUT;
                *pnvm = candidate;
                found = true;
            }
            sku_match = read_le32(value) == sku[0] && read_le32(value + 4) == sku[1] &&
                        read_le32(value + 8) == sku[2];
            hw_match = false;
            candidate = (ax210_pnvm_t){ 0 };
            continue;
        }
        if (!sku_match)
            continue;

        switch (tlv.type) {
        case FW_TLV_HW_TYPE:
            if (value_length < 4)
                return AX210_FW_BAD_LAYOUT;
            hw_match |= read_le16(value) == mac_type && read_le16(value + 2) == rf_type;
            break;
        case FW_TLV_PNVM_VERSION:
            if (value_length != 4 || candidate.version != 0)
                return AX210_FW_BAD_LAYOUT;
            candidate.version = read_le32(value);
            break;
        case FW_TLV_SEC_RT:
            if (value_length < 4)
                return AX210_FW_BAD_LAYOUT;
            if (read_le32(value) == PNVM_LEGACY_SEPARATOR)
                break;
            if (value_length == 4 || candidate.image.count == AX210_FW_MAX_SECTIONS)
                return AX210_FW_BAD_LAYOUT;
            candidate.image.sections[candidate.image.count++] =
                (ax210_fw_span_t){ value + 4, value_length - 4 };
            break;
        default:
            break;
        }
    }

    if (sku_match && hw_match) {
        if (found || candidate.image.count == 0 || candidate.version == 0)
            return AX210_FW_BAD_LAYOUT;
        *pnvm = candidate;
        found = true;
    }
    return found ? AX210_FW_OK : AX210_FW_NO_MATCH;
}
