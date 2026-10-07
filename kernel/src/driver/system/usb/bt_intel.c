// Intel's Read Version uses opcode fc05 with parameter ff and a TLV reply.
// Wire definitions: Linux drivers/bluetooth/btintel.h and btintel.c at
// https://github.com/torvalds/linux/tree/master/drivers/bluetooth .
// This module decodes bytes; transport, timing and DMA belong to xhci.c.
#include "driver/system/usb/bt_intel.h"

static uint16_t read16(const uint8_t *p)
{
    return p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read32(const uint8_t *p)
{
    return read16(p) | ((uint32_t)read16(p + 2) << 16);
}

bool bt_intel_find_usb(const uint8_t *cfg, size_t bytes, bt_intel_usb_t *out)
{
    *out = (bt_intel_usb_t){0};
    if (bytes < 9 || cfg[0] != 9 || cfg[1] != 2 ||
        read16(cfg + 2) != bytes || !cfg[5])
        return false;
    bt_intel_usb_t usb = {.configuration = cfg[5]};
    bool selected = false, found = false;
    unsigned endpoints = 0;
    for (size_t off = 9; off < bytes; off += cfg[off]) {
        if (bytes - off < 2 || cfg[off] < 2 || cfg[off] > bytes - off)
            return false;
        const uint8_t *d = cfg + off;
        if (d[1] == 4) {
            if (d[0] < 9) return false;
            selected = d[2] == 0 && d[3] == 0;
            if (selected) {
                if (found || d[4] != 3 || d[5] != 0xe0 || d[6] != 1 || d[7] != 1)
                    return false;
                found = true;
            }
        } else if (d[1] == 5) {
            if (d[0] < 7) return false;
            if (!selected) continue;
            if (!(d[2] & 15) || (d[2] & 0x70)) return false;
            uint16_t packet = read16(d + 4);
            if (!packet || packet > 64) return false;
            bt_usb_endpoint_t *ep;
            if ((d[3] & 3) == 3 && (d[2] & 0x80) && d[6]) {
                ep = &usb.interrupt_in;
            } else if ((d[3] & 3) == 2 &&
                       (packet == 8 || packet == 16 || packet == 32 || packet == 64)) {
                ep = (d[2] & 0x80) ? &usb.bulk_in : &usb.bulk_out;
            } else {
                return false;
            }
            if (ep->address) return false;
            *ep = (bt_usb_endpoint_t){d[2], (uint8_t)packet, d[6]};
            endpoints++;
        }
    }
    if (!found || endpoints != 3 || !usb.interrupt_in.address ||
        !usb.bulk_in.address || !usb.bulk_out.address ||
        usb.interrupt_in.address == usb.bulk_in.address)
        return false;
    *out = usb;
    return true;
}

// Command Complete: event header, command credits, opcode, status, then TLVs.
// Unknown TLVs can be skipped; known values require their complete width.
static void read_version_event(bt_intel_reply_t *reply, const uint8_t *event, size_t bytes)
{
    if (event[0] == 0x0f) { // Command Status can reject a command without a completion.
        if (bytes == 6 && read16(event + 4) == BT_INTEL_READ_VERSION && event[2]) {
            reply->status = event[2];
            reply->result = BT_INTEL_COMMAND_ERROR;
        }
        return;
    }
    if (event[0] != 0x0e || bytes < 5 || read16(event + 3) != BT_INTEL_READ_VERSION)
        return;
    if (bytes < 6) {
        reply->result = BT_INTEL_BAD_REPLY;
        return;
    }
    if (event[5]) {
        reply->status = event[5];
        reply->result = BT_INTEL_COMMAND_ERROR;
        return;
    }
    bt_intel_reply_t version = {0};
    for (size_t off = 6; off < bytes;) {
        if (bytes - off < 2 || event[off + 1] > bytes - off - 2)
            goto malformed;
        uint8_t type = event[off], length = event[off + 1];
        const uint8_t *value = event + off + 2;
        unsigned field = 0, width = 0;
        switch (type) {
            case 0x12: field = BT_INTEL_CNVI; width = 4; break;
            case 0x13: field = BT_INTEL_CNVR; width = 4; break;
            case 0x1c: field = BT_INTEL_IMAGE; width = 1; break;
            case 0x1d: field = BT_INTEL_TIMESTAMP; width = 2; break;
            case 0x1f: field = BT_INTEL_BUILD; width = 4; break;
            case 0x32: field = BT_INTEL_SHA1; width = 4; break;
        }
        if (length < width || (version.fields & field)) goto malformed;
        version.fields |= field;
        switch (field) {
            case BT_INTEL_CNVI: version.cnvi = read32(value); break;
            case BT_INTEL_CNVR: version.cnvr = read32(value); break;
            case BT_INTEL_IMAGE: version.image = value[0]; break;
            case BT_INTEL_TIMESTAMP: version.timestamp = read16(value); break;
            case BT_INTEL_BUILD: version.build = read32(value); break;
            case BT_INTEL_SHA1: version.sha1 = read32(value); break;
        }
        off += 2 + length;
    }
    if (!(version.fields & BT_INTEL_IMAGE)) goto malformed;
    version.result = BT_INTEL_VERSION;
    *reply = version;
    return;
malformed:
    reply->result = BT_INTEL_BAD_REPLY;
}

void bt_intel_feed(bt_hci_stream_t *stream, bt_intel_reply_t *reply,
                   const uint8_t *data, size_t bytes)
{
    while (bytes && reply->result == BT_INTEL_WAITING) {
        // Filling the two-byte header first bounds the ensuing event length.
        size_t target = stream->used < 2 ? 2 : 2 + stream->bytes[1];
        size_t take = target - stream->used;
        if (take > bytes) take = bytes;
        for (size_t i = 0; i < take; i++) stream->bytes[stream->used++] = *data++;
        bytes -= take;
        if (stream->used >= 2 && stream->used == 2 + stream->bytes[1]) {
            read_version_event(reply, stream->bytes, stream->used);
            stream->used = 0;
        }
    }
}
