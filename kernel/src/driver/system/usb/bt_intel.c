// Intel AX210 wire formats: version TLVs, loader events and SFI containers.
// Wire definitions: Linux drivers/bluetooth/btintel.h and btintel.c at
// https://github.com/torvalds/linux/tree/v6.12/drivers/bluetooth .
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
            case 0x10: field = BT_INTEL_TOP; width = 4; break;
            case 0x11: field = BT_INTEL_RADIO_TOP; width = 4; break;
            case 0x12: field = BT_INTEL_CNVI; width = 4; break;
            case 0x13: field = BT_INTEL_CNVR; width = 4; break;
            case 0x1c: field = BT_INTEL_IMAGE; width = 1; break;
            case 0x1d: field = BT_INTEL_TIMESTAMP; width = 2; break;
            case 0x1f: field = BT_INTEL_BUILD; width = 4; break;
            case 0x32: field = BT_INTEL_SHA1; width = 4; break;
            case 0x2e: field = BT_INTEL_LIMITED; width = 1; break;
            case 0x2f: field = BT_INTEL_SBE; width = 1; break;
        }
        if (length < width || (version.fields & field)) goto malformed;
        version.fields |= field;
        switch (field) {
            case BT_INTEL_TOP: version.cnvi_top = read32(value); break;
            case BT_INTEL_RADIO_TOP: version.cnvr_top = read32(value); break;
            case BT_INTEL_CNVI: version.cnvi = read32(value); break;
            case BT_INTEL_CNVR: version.cnvr = read32(value); break;
            case BT_INTEL_IMAGE: version.image = value[0]; break;
            case BT_INTEL_TIMESTAMP: version.timestamp = read16(value); break;
            case BT_INTEL_BUILD: version.build = read32(value); break;
            case BT_INTEL_SHA1: version.sha1 = read32(value); break;
            case BT_INTEL_LIMITED: version.limited = value[0]; break;
            case BT_INTEL_SBE: version.sbe = value[0]; break;
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

void bt_hci_feed(bt_hci_stream_t *stream,
                 void (*event)(void *, const uint8_t *, size_t), void *context,
                 const uint8_t *data, size_t bytes)
{
    while (bytes) {
        // Filling the two-byte header first bounds the ensuing event length.
        size_t target = stream->used < 2 ? 2 : 2 + stream->bytes[1];
        size_t take = target - stream->used;
        if (take > bytes) take = bytes;
        for (size_t i = 0; i < take; i++) stream->bytes[stream->used++] = *data++;
        bytes -= take;
        if (stream->used >= 2 && stream->used == 2 + stream->bytes[1]) {
            event(context, stream->bytes, stream->used);
            stream->used = 0;
        }
    }
}

static void version_event(void *context, const uint8_t *event, size_t bytes)
{
    bt_intel_reply_t *reply = context;
    if (reply->result == BT_INTEL_WAITING) read_version_event(reply, event, bytes);
}

void bt_intel_feed(bt_hci_stream_t *stream, bt_intel_reply_t *reply,
                   const uint8_t *data, size_t bytes)
{
    bt_hci_feed(stream, version_event, reply, data, bytes);
}

static void decode_loader_event(void *context, const uint8_t *event, size_t bytes)
{
    bt_intel_events_t *e = context;
    if (event[0] == 0x0e && bytes >= 5) {
        e->credits = event[2];
        if (e->opcode && read16(event + 3) == e->opcode) {
            if (bytes < 6 || e->command_done) { e->malformed = true; return; }
            e->status = event[5];
            e->command_done = true;
            if (e->opcode == BT_INTEL_READ_VERSION) {
                read_version_event(&e->version, event, bytes);
                if (e->version.result == BT_INTEL_BAD_REPLY) e->malformed = true;
            }
        }
    } else if (event[0] == 0x0f && bytes == 6) {
        e->credits = event[3];
        if (e->opcode && read16(event + 4) == e->opcode && event[2]) {
            e->status = event[2];
            e->command_done = true;
        }
    } else if (event[0] == 0xff && bytes >= 3) {
        if (event[2] == 6 && e->watch_download) {
            // The vendor subevent identifies the secure-download result.
            // AX210 success can carry opcode zero instead of echoing fc09.
            if (bytes != 7) {
                e->malformed = true;
                return;
            }
            e->download_done = true;
            e->download_failed |= event[3] != 0 || event[6] != 0;
        } else if (event[2] == 2 && e->watch_boot) {
            if (bytes != 9) { e->malformed = true; return; }
            e->booted = true;
            e->credits = event[4];
        }
    }
}

static void loader_event(void *context, const uint8_t *event, size_t bytes)
{
    bt_intel_events_t *e = context;
    decode_loader_event(context, event, bytes);
    if (!e->failure_bytes && (e->malformed || e->download_failed ||
                             (e->command_done && e->status))) {
        e->failure_bytes = bytes;
        size_t count = bytes < sizeof(e->failure_event) ? bytes : sizeof(e->failure_event);
        for (size_t i = 0; i < count; i++) e->failure_event[i] = event[i];
    }
}

void bt_intel_feed_events(bt_hci_stream_t *stream, bt_intel_events_t *events,
                          const uint8_t *data, size_t bytes)
{
    bt_hci_feed(stream, loader_event, events, data, bytes);
}

static uint16_t firmware_id(uint32_t top)
{
    uint16_t packed = ((top & 0xfff) << 4) | ((top >> 24) & 15);
    return (packed >> 8) | (packed << 8);
}

bool bt_intel_is_ax210_bootloader(const bt_intel_reply_t *v)
{
    unsigned required = BT_INTEL_IMAGE | BT_INTEL_CNVI | BT_INTEL_TOP |
                        BT_INTEL_RADIO_TOP | BT_INTEL_SBE | BT_INTEL_LIMITED;
    return v->result == BT_INTEL_VERSION && (v->fields & required) == required &&
           v->image == 1 && ((v->cnvi >> 8) & 255) == 0x37 &&
           ((v->cnvi >> 16) & 63) == 0x17 && v->sbe <= 1 && !v->limited &&
           firmware_id(v->cnvi_top) == 0x0041 && firmware_id(v->cnvr_top) == 0x0041;
}

size_t bt_intel_sfi_group(const bt_intel_sfi_t *sfi, size_t offset)
{
    if (offset < BT_INTEL_SFI_PAYLOAD || offset >= sfi->size) return 0;
    size_t end = offset;
    do {
        if (sfi->size - end < 3) return 0;
        size_t n = 3u + sfi->data[end + 2];
        if (n > sfi->size - end) return 0;
        end += n;
    } while ((end - offset) & 3);
    return end - offset;
}

bool bt_intel_sfi_validate(const uint8_t *data, size_t size, bt_intel_sfi_t *out)
{
    *out = (bt_intel_sfi_t){0};
    if (size <= BT_INTEL_SFI_PAYLOAD || size > 2u * 1024 * 1024 ||
        data[0] != 6 || read32(data + 8) != 0x10000 ||
        data[644] != 6 || read32(data + 652) != 0x20000)
        return false;
    bt_intel_sfi_t sfi = {.data = data, .size = size};
    bool found = false;
    // Walk command records for the boot address and validate their boundaries.
    // The signed payload is passed through unchanged, never executed by os64.
    for (size_t off = BT_INTEL_SFI_PAYLOAD; off < size;) {
        if (size - off < 3 || (size_t)data[off + 2] + 3 > size - off) return false;
        if (read16(data + off) == 0xfc0e) {
            if (found || data[off + 2] != 7) return false;
            found = true;
            sfi.boot_address = read32(data + off + 3);
            sfi.build = data[off + 7];
            sfi.week = data[off + 8];
            sfi.year = data[off + 9];
        }
        off += 3u + data[off + 2];
    }
    if (!found || !sfi.boot_address) return false;
    for (size_t off = BT_INTEL_SFI_PAYLOAD; off < size;) {
        size_t group = bt_intel_sfi_group(&sfi, off);
        if (!group) return false;
        off += group;
    }
    *out = sfi;
    return true;
}

bool bt_intel_ddc_valid(const uint8_t *data, size_t size)
{
    if (!size || size > 65536) return false;
    for (size_t off = 0; off < size;) {
        size_t length = 1u + data[off];
        if (length < 3 || length > 255 || length > size - off) return false;
        off += length;
    }
    return true;
}
