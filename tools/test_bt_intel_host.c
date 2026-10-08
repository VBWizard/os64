// AX210 descriptor selection and HCI event framing, without hardware or DMA.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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
    // A completion and secure-download result may occupy the same packet.
    const uint8_t notify[] = {0x0e,4,1,9,0xfc,0, 0xff,5,6,0,9,0xfc,0,
                             0xff,7,2,0,1,0,0,0,0};
    for (unsigned split = 0; split <= sizeof(notify); split++) {
        bt_intel_events_t e = {.opcode=0xfc09,.watch_download=true,.watch_boot=true};
        bt_hci_stream_t stream = {0};
        bt_intel_feed_events(&stream,&e,notify,split);
        bt_intel_feed_events(&stream,&e,notify+split,sizeof(notify)-split);
        assert(e.command_done && e.download_done && e.booted && e.credits==1);
        assert(!e.status && !e.malformed && !e.download_failed);
    }
    bt_intel_events_t e = {.opcode=0xfc09,.watch_download=true};
    bt_hci_stream_t stream = {0};
    const uint8_t failure[] = {0xff,5,6,1,9,0xfc,0};
    bt_intel_feed_events(&stream,&e,failure,sizeof(failure));
    assert(e.download_done && e.download_failed);
    const uint8_t malformed[] = {0xff,4,6,0,9,0xfc};
    e=(bt_intel_events_t){.watch_download=true}; stream=(bt_hci_stream_t){0};
    bt_intel_feed_events(&stream,&e,malformed,sizeof(malformed));
    assert(e.malformed && !e.download_done);
    assert(e.failure_bytes==sizeof(malformed) && !memcmp(e.failure_event,malformed,sizeof(malformed)));
    // A later event cannot replace the evidence for the original rejection.
    bt_intel_feed_events(&stream,&e,notify,sizeof(notify));
    assert(e.failure_bytes==sizeof(malformed) && !memcmp(e.failure_event,malformed,sizeof(malformed)));
    // The P5's AX210 reports secure success with a zero opcode field.
    const uint8_t p5_secure_success[]={0xff,5,6,0,0,0,0};
    for (unsigned split=0; split<=sizeof(p5_secure_success); split++) {
        e=(bt_intel_events_t){.watch_download=true}; stream=(bt_hci_stream_t){0};
        bt_intel_feed_events(&stream,&e,p5_secure_success,split);
        bt_intel_feed_events(&stream,&e,p5_secure_success+split,sizeof(p5_secure_success)-split);
        assert(e.download_done && !e.download_failed && !e.malformed && !e.failure_bytes);
    }
    const uint8_t zero_opcode_failure[]={0xff,5,6,1,0,0,0};
    e=(bt_intel_events_t){.watch_download=true}; stream=(bt_hci_stream_t){0};
    bt_intel_feed_events(&stream,&e,zero_opcode_failure,sizeof(zero_opcode_failure));
    assert(e.download_done && e.download_failed && !e.malformed);
    assert(e.failure_bytes==sizeof(zero_opcode_failure));
    assert(!memcmp(e.failure_event,zero_opcode_failure,sizeof(zero_opcode_failure)));
    e=(bt_intel_events_t){0}; stream=(bt_hci_stream_t){0};
    bt_intel_feed_events(&stream,&e,p5_secure_success,sizeof(p5_secure_success));
    assert(!e.download_done && !e.malformed);
    e=(bt_intel_events_t){.opcode=0xfc09}; stream=(bt_hci_stream_t){0};
    bt_intel_feed_events(&stream,&e,rejected,sizeof(rejected));
    assert(!e.command_done); // fc05 cannot complete fc09.

    bt_intel_reply_t identity = {.result=BT_INTEL_VERSION,.image=1,
        .fields=BT_INTEL_IMAGE|BT_INTEL_CNVI|BT_INTEL_TOP|BT_INTEL_RADIO_TOP|BT_INTEL_SBE|BT_INTEL_LIMITED,
        .cnvi=0x173700,.cnvi_top=0x410,.cnvr_top=0x410,.sbe=1};
    assert(bt_intel_is_ax210_bootloader(&identity));
    identity.sbe=0; assert(bt_intel_is_ax210_bootloader(&identity));
    identity.sbe=2; assert(!bt_intel_is_ax210_bootloader(&identity)); identity.sbe=1;
    identity.cnvi_top=0x01000410; assert(!bt_intel_is_ax210_bootloader(&identity)); identity.cnvi_top=0x410;
    identity.image=3; assert(!bt_intel_is_ax210_bootloader(&identity)); identity.image=1;
    identity.limited=1; assert(!bt_intel_is_ax210_bootloader(&identity)); identity.limited=0;
    for (unsigned bit=0;bit<10;bit++) {
        if (!(identity.fields & (1u<<bit))) continue;
        bt_intel_reply_t missing=identity; missing.fields &= ~(1u<<bit);
        assert(!bt_intel_is_ax210_bootloader(&missing));
    }

    uint8_t fixture[980] = {6,0,0,0,0,0,0,0,0,0,1,0};
    fixture[644]=6; fixture[654]=2;
    const uint8_t commands[]={0x0e,0xfc,7,0,8,0x10,0,60,48,23, 2,0xfc,3,0,0,0};
    memcpy(fixture+964,commands,sizeof(commands));
    bt_intel_sfi_t sfi;
    assert(bt_intel_sfi_validate(fixture,sizeof(fixture),&sfi));
    assert(sfi.boot_address==0x100800 && sfi.build==60 && sfi.week==48 && sfi.year==23);
    assert(bt_intel_sfi_group(&sfi,964)==16 && bt_intel_sfi_group(&sfi,980)==0);
    for (unsigned n=0;n<sizeof(fixture);n++) assert(!bt_intel_sfi_validate(fixture,n,&sfi));
    fixture[966]=255; assert(!bt_intel_sfi_validate(fixture,sizeof(fixture),&sfi)); fixture[966]=7;
    fixture[654]=1; assert(!bt_intel_sfi_validate(fixture,sizeof(fixture),&sfi)); fixture[654]=2;
    fixture[974]=0x0e; // A duplicate boot record is ambiguous, even with a short body.
    assert(!bt_intel_sfi_validate(fixture,sizeof(fixture),&sfi));
    const uint8_t ddc[]={3,0x28,1,0x18,4,0x29,1,3,0};
    assert(bt_intel_ddc_valid(ddc,sizeof(ddc)));
    assert(!bt_intel_ddc_valid(ddc,0) && !bt_intel_ddc_valid(ddc,sizeof(ddc)-1));

    FILE *f=fopen("firmware/intel/ibt-0041-0041.sfi","rb"); assert(f);
    assert(!fseek(f,0,SEEK_END)); long length=ftell(f); assert(length>0);
    rewind(f); uint8_t *blob=malloc(length); assert(blob);
    assert(fread(blob,1,length,f)==(size_t)length); fclose(f);
    assert(bt_intel_sfi_validate(blob,length,&sfi));
    assert(sfi.boot_address==0x100800 && sfi.build==60 && sfi.week==48 && sfi.year==23);
    size_t offset=964; unsigned groups=0;
    while (offset<sfi.size) {
        size_t n=bt_intel_sfi_group(&sfi,offset); assert(n && !(n%4));
        offset+=n; groups++;
    }
    assert(offset==sfi.size && groups==2972);
    free(blob);
    puts("test_bt_intel_host: framing, identity, firmware container and notifications passed");
    return 0;
}
