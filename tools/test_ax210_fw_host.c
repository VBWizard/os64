#include "driver/net/ax210_fw.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_le32(uint8_t *bytes, uint32_t value)
{
    for (size_t i = 0; i < 4; ++i)
        bytes[i] = (uint8_t)(value >> (i * 8));
}

static size_t tlv_size(const uint8_t *header)
{
    size_t length = 0;
    for (size_t i = 0; i < 4; ++i)
        length |= (size_t)header[4 + i] << (i * 8);
    return 8 + ((length + 3) & ~(size_t)3);
}

static void test_mutated_tlvs(const uint8_t *original, size_t length, size_t start)
{
    ax210_fw_image_t image;
    ax210_pnvm_t pnvm;
    const uint32_t sku[3] = { 0x610d0, 0, 0 };
    uint8_t *mutated = malloc(length);
    assert(mutated != NULL);
    memcpy(mutated, original, length);

    // Exercise record types, lengths, and section/SKU tags throughout the
    // pinned inputs. The original input alone determines iteration bounds.
    static const uint8_t masks[] = { 1, 0x80, 0xff };
    for (size_t offset = start; offset < length;) {
        size_t record_size = tlv_size(original + offset);
        assert(record_size <= length - offset);
        size_t prefix = record_size < 20 ? record_size : 20;
        for (size_t field = 0; field < prefix; ++field) {
            for (size_t mask = 0; mask < sizeof(masks); ++mask) {
                mutated[offset + field] ^= masks[mask];
                if (start == 0)
                    (void)ax210_pnvm_select(mutated, length, sku, 0x42, 0x10d, &pnvm);
                else
                    (void)ax210_fw_parse(mutated, length, &image);
                mutated[offset + field] = original[offset + field];
            }
        }
        offset += record_size;
    }
    free(mutated);
}

static void test_ucode(void)
{
    ax210_fw_image_t image;
    size_t length = (uintptr_t)ax210_ucode_end - (uintptr_t)ax210_ucode_start;
    assert(length == 1594276);
    assert(ax210_fw_parse(ax210_ucode_start, length, &image) == AX210_FW_OK);
    assert(image.api == 77 && image.build == 0xf92b5fed && image.revision == 0);
    assert(image.loader.length == 13944);
    assert(image.groups[AX210_FW_LMAC].count == 15);
    assert(image.groups[AX210_FW_UMAC].count == 16);
    assert(image.groups[AX210_FW_PAGING].count == 23);
    assert(image.groups[AX210_FW_LMAC].sections[0].length == 1656);
    assert(image.groups[AX210_FW_UMAC].sections[0].length == 1656);
    assert(image.groups[AX210_FW_PAGING].sections[0].length == 1656);

    assert(ax210_fw_parse(ax210_ucode_start, 87, &image) == AX210_FW_BAD_HEADER);
    assert(ax210_fw_parse(ax210_ucode_start, length - 1, &image) == AX210_FW_BAD_TLV);
    uint8_t *mutated = malloc(length);
    assert(mutated != NULL);
    memcpy(mutated, ax210_ucode_start, length);
    put_le32(mutated + 92, UINT32_MAX);
    assert(ax210_fw_parse(mutated, length, &image) == AX210_FW_BAD_TLV);
    memcpy(mutated, ax210_ucode_start, length);
    // The first two TLVs are a 12-byte probe record and a 12-byte version.
    put_le32(mutated + 116, AX210_FW_API + 1);
    assert(ax210_fw_parse(mutated, length, &image) == AX210_FW_WRONG_API);
    free(mutated);
}

static void test_pnvm(void)
{
    ax210_pnvm_t pnvm;
    size_t length = (uintptr_t)ax210_pnvm_end - (uintptr_t)ax210_pnvm_start;
    assert(length == 55020);
    static const uint32_t sku_values[] = { 0x610d0, 0x510d0, 0x4510d0, 0x4610d0 };
    static const size_t expected_lengths[] = { 11996, 12048, 12048, 12000 };
    for (size_t i = 0; i < sizeof(sku_values) / sizeof(*sku_values); ++i) {
        const uint32_t sku[3] = { sku_values[i], 0, 0 };
        assert(ax210_pnvm_select(ax210_pnvm_start, length, sku, 0x42, 0x10d, &pnvm) == AX210_FW_OK);
        assert(pnvm.version == 0xce1a5094 && pnvm.image.count == 2);
        assert(pnvm.image.sections[0].length == 1656);
        assert(pnvm.image.sections[1].length == expected_lengths[i]);
        assert(ax210_pnvm_select(ax210_pnvm_start, length, sku, 0x43, 0x10d, &pnvm) == AX210_FW_NO_MATCH);
    }
    const uint32_t sku[3] = { sku_values[0], 0, 0 };
    assert(ax210_pnvm_select(ax210_pnvm_start, length - 1, sku, 0x42, 0x10d, &pnvm) == AX210_FW_BAD_TLV);
    uint8_t *mutated = malloc(length);
    assert(mutated != NULL);
    memcpy(mutated, ax210_pnvm_start, length);
    put_le32(mutated + 4, 4);
    assert(ax210_pnvm_select(mutated, length, sku, 0x42, 0x10d, &pnvm) == AX210_FW_BAD_LAYOUT);
    free(mutated);
}

int main(void)
{
    test_ucode();
    test_pnvm();
    test_mutated_tlvs(ax210_ucode_start, (uintptr_t)ax210_ucode_end - (uintptr_t)ax210_ucode_start, 88);
    test_mutated_tlvs(ax210_pnvm_start, (uintptr_t)ax210_pnvm_end - (uintptr_t)ax210_pnvm_start, 0);
    puts("AX210 firmware and PNVM checks passed");
    return 0;
}
