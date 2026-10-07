// AX210 descriptor selection and HCI event framing, without hardware or DMA.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "driver/system/usb/bt_intel.h"

static bt_intel_reply_t decode(const uint8_t *event, size_t bytes)
{
    bt_hci_stream_t stream = {0};
    bt_intel_reply_t reply = {0};
    bt_intel_feed(&stream, &reply, event, bytes);
    return reply;
}

int main(void)
{
    // P5's reported configuration: HCI plus seven isochronous alternate settings.
    uint8_t cfg[200] = {9,2,200,0,2,1,0,0x80,50,
        9,4,0,0,3,0xe0,1,1,0,
        7,5,0x81,3,64,0,1, 7,5,2,2,64,0,1, 7,5,0x82,2,64,0,1};
    const uint8_t packets[] = {0,9,17,25,33,49,63};
    for (unsigned i = 0; i < 7; i++) {
        uint8_t alternate[] = {9,4,1,i,2,0xe0,1,1,0,
            7,5,3,1,packets[i],0,1, 7,5,0x83,1,packets[i],0,1};
        memcpy(cfg + 39 + i * 23, alternate, sizeof(alternate));
    }
    bt_intel_usb_t usb;
    assert(bt_intel_find_usb(cfg, sizeof(cfg), &usb));
    assert(usb.configuration == 1 && usb.interrupt_in.address == 0x81);
    assert(usb.bulk_in.address == 0x82 && usb.bulk_out.address == 2);
    assert(usb.interrupt_in.packet_bytes == 64 && usb.interrupt_in.interval == 1);
    for (unsigned n = 0; n < sizeof(cfg); n++)
        assert(!bt_intel_find_usb(cfg, n, &usb));
    const struct {unsigned offset; uint8_t invalid;} bad[] = {
        {5,0}, {9,0}, {13,2}, {14,3}, {18,6}, {20,0x80}, {20,0x91},
        {21,1}, {22,0}, {22,65}, {23,1}, {24,0}, {27,0x82},
        {29,63}, {34,0x81}, {41,0},
    };
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        uint8_t saved = cfg[bad[i].offset];
        cfg[bad[i].offset] = bad[i].invalid;
        assert(!bt_intel_find_usb(cfg, sizeof(cfg), &usb));
        cfg[bad[i].offset] = saved;
    }
    // An audio endpoint's interval does not affect the HCI interface selection.
    cfg[199] = 255;
    assert(bt_intel_find_usb(cfg, sizeof(cfg), &usb));

    const uint8_t version[] = {0x0e,38,1,5,0xfc,0,
        0x12,4,0x41,0,0x41,0, 0x13,4,0x42,0,0x42,0,
        0x1c,1,3, 0x1d,2,48,23, 0x1f,4,0x3c,0x26,1,0,
        0x32,4,0x58,0xc5,0xba,0x23, 0xee,1,0xff};
    assert(sizeof(version) == 40);
    for (unsigned split = 0; split <= sizeof(version); split++) {
        bt_hci_stream_t stream = {0};
        bt_intel_reply_t reply = {0};
        bt_intel_feed(&stream, &reply, version, split);
        if (split < sizeof(version)) assert(reply.result == BT_INTEL_WAITING);
        bt_intel_feed(&stream, &reply, version + split, sizeof(version) - split);
        assert(reply.result == BT_INTEL_VERSION && reply.image == 3);
        assert(reply.fields == 63 && reply.cnvi == 0x410041 && reply.cnvr == 0x420042);
        assert(reply.timestamp == 0x1730 && reply.build == 75324 && reply.sha1 == 0x23bac558);
    }
    for (unsigned n = 0; n < sizeof(version); n++)
        assert(decode(version, n).result == BT_INTEL_WAITING);

    // Unrelated events, including zero-payload ones, may precede the reply.
    uint8_t combined[64] = {0xff,0, 0x0e,4,1,3,0x0c,0};
    memcpy(combined + 8, version, sizeof(version));
    assert(decode(combined, 8 + sizeof(version)).result == BT_INTEL_VERSION);
    const uint8_t rejected[] = {0x0e,4,1,5,0xfc,0x12};
    assert(decode(rejected, sizeof(rejected)).result == BT_INTEL_COMMAND_ERROR);
    const uint8_t status[] = {0x0f,4,0x0c,1,5,0xfc};
    assert(decode(status, sizeof(status)).status == 0x0c);
    const uint8_t accepted[] = {0x0f,4,0,1,5,0xfc};
    assert(decode(accepted, sizeof(accepted)).result == BT_INTEL_WAITING);
    const uint8_t missing_status[] = {0x0e,3,1,5,0xfc};
    assert(decode(missing_status, sizeof(missing_status)).result == BT_INTEL_BAD_REPLY);
    const uint8_t missing_image[] = {0x0e,4,1,5,0xfc,0};
    assert(decode(missing_image, sizeof(missing_image)).result == BT_INTEL_BAD_REPLY);
    const uint8_t short_image[] = {0x0e,6,1,5,0xfc,0,0x1c,0};
    assert(decode(short_image, sizeof(short_image)).result == BT_INTEL_BAD_REPLY);
    const uint8_t overrun[] = {0x0e,7,1,5,0xfc,0,0x1c,2,1};
    assert(decode(overrun, sizeof(overrun)).result == BT_INTEL_BAD_REPLY);
    const uint8_t partial_tlv[] = {0x0e,5,1,5,0xfc,0,0x1c};
    assert(decode(partial_tlv, sizeof(partial_tlv)).result == BT_INTEL_BAD_REPLY);
    const uint8_t duplicate[] = {0x0e,10,1,5,0xfc,0,0x1c,1,1,0x1c,1,3};
    assert(decode(duplicate, sizeof(duplicate)).result == BT_INTEL_BAD_REPLY);

    // Exact-MPS and maximum-size events must finish without a trailing short
    // USB packet. Unknown TLVs fill out the event without changing its meaning.
    for (unsigned bytes = 64; bytes <= BT_HCI_EVENT_BYTES; bytes += 193) {
        uint8_t large[BT_HCI_EVENT_BYTES] = {0x0e,bytes-2,1,5,0xfc,0,0x1c,1,1,0xee,bytes-11};
        for (unsigned chunk = 1; chunk <= 64; chunk++) {
            bt_hci_stream_t stream = {0};
            bt_intel_reply_t reply = {0};
            for (unsigned off = 0; off < bytes; off += chunk) {
                unsigned count = bytes - off < chunk ? bytes - off : chunk;
                bt_intel_feed(&stream, &reply, large + off, count);
            }
            assert(reply.result == BT_INTEL_VERSION && reply.image == 1 && !stream.used);
        }
    }
    puts("test_bt_intel_host: all checks passed");
    return 0;
}
