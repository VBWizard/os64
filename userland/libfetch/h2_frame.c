// h2_frame.c — see h2_frame.h.
#include "h2_frame.h"

#include "os64/str.h"

const uint8_t h2_preface[H2_PREFACE_LEN] = {'P', 'R', 'I', ' ', '*', ' ', 'H', 'T', 'T', 'P', '/', '2',
                                            '.', '0', '\r', '\n', '\r', '\n', 'S', 'M', '\r', '\n', '\r', '\n'};

const char *h2_error_name(uint32_t code)
{
    static const char *const kNames[] = {
        "NO_ERROR", "PROTOCOL_ERROR", "INTERNAL_ERROR", "FLOW_CONTROL_ERROR", "SETTINGS_TIMEOUT",
        "STREAM_CLOSED", "FRAME_SIZE_ERROR", "REFUSED_STREAM", "CANCEL", "COMPRESSION_ERROR",
        "CONNECT_ERROR", "ENHANCE_YOUR_CALM", "INADEQUATE_SECURITY", "HTTP_1_1_REQUIRED",
    };
    return code < sizeof(kNames) / sizeof(kNames[0]) ? kNames[code] : "UNKNOWN_ERROR";
}

const char *h2_setting_name(uint32_t identifier)
{
    static const char *const kNames[] = {
        NULL, "HEADER_TABLE_SIZE", "ENABLE_PUSH", "MAX_CONCURRENT_STREAMS",
        "INITIAL_WINDOW_SIZE", "MAX_FRAME_SIZE", "MAX_HEADER_LIST_SIZE",
    };
    if (identifier == 0 || identifier >= sizeof(kNames) / sizeof(kNames[0]))
        return "UNKNOWN_SETTING";
    return kNames[identifier];
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void h2_frame_header_read(const uint8_t bytes[H2_FRAME_HEADER_LEN], h2_frame_header_t *header)
{
    header->length = (uint32_t)bytes[0] << 16 | (uint32_t)bytes[1] << 8 | bytes[2];
    header->type = bytes[3];
    header->flags = bytes[4];
    header->stream = get32(bytes + 5) & H2_STREAM_MOST;
}

void h2_frame_header_write(uint8_t bytes[H2_FRAME_HEADER_LEN], const h2_frame_header_t *header)
{
    bytes[0] = (uint8_t)(header->length >> 16);
    bytes[1] = (uint8_t)(header->length >> 8);
    bytes[2] = (uint8_t)header->length;
    bytes[3] = header->type;
    bytes[4] = header->flags;
    put32(bytes + 5, header->stream & H2_STREAM_MOST);
}

static bool broke(h2_fault_t *fault, uint32_t error, h2_rule_t rule, bool stream)
{
    *fault = (h2_fault_t){.error = error, .rule = rule, .stream = stream};
    return false;
}

bool h2_frame_header_check(const h2_frame_header_t *header, uint32_t max_frame_size, h2_fault_t *fault)
{
    if (header->length > max_frame_size)
        return broke(fault, H2_ERR_FRAME_SIZE, H2_RULE_TOO_LONG, false);
    return true;
}

// The priority fields: a dependency with its exclusive bit, and a weight.
static void priority_read(const uint8_t *p, h2_frame_t *frame)
{
    uint32_t word = get32(p);
    frame->has_priority = true;
    frame->exclusive = (word & ~H2_STREAM_MOST) != 0;
    frame->depends_on = word & H2_STREAM_MOST;
    frame->weight = p[4];
}

// Padding is a length byte at the payload's front and that many bytes at
// its end, with the frame's fixed fields and its data between. The two
// halves are taken off apart, because the fixed fields are judged between
// them: a frame too short to hold its fixed fields has not the length its
// type needs (FRAME_SIZE_ERROR), and one that holds them but whose padding
// is longer than what is left for the data is PROTOCOL_ERROR (§§ 6.1, 6.2,
// 6.6).
static bool pad_length(const uint8_t **data, uint32_t *len, uint32_t *pad, h2_fault_t *fault)
{
    if (*len < 1)
        return broke(fault, H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, false);
    *pad = (*data)[0];
    *data += 1;
    *len -= 1;
    return true;
}

static bool pad_drop(uint32_t *len, uint32_t pad, h2_fault_t *fault)
{
    if (pad > *len)
        return broke(fault, H2_ERR_PROTOCOL, H2_RULE_PADDING, false);
    *len -= pad;
    return true;
}

bool h2_frame_parse(const h2_frame_header_t *header, const uint8_t *payload, uint32_t max_frame_size,
                    h2_frame_t *frame, h2_fault_t *fault)
{
    *frame = (h2_frame_t){.kind = H2_FRAME_UNKNOWN, .type = header->type, .flags = header->flags,
                          .stream = header->stream};
    *fault = (h2_fault_t){0};
    if (!h2_frame_header_check(header, max_frame_size, fault))
        return false;
    const uint8_t *data = payload;
    uint32_t len = header->length, pad = 0;
    bool on_stream = header->stream != 0;
    bool padded = (header->flags & H2_FLAG_PADDED) != 0;

    switch (header->type) {
    case H2_FRAME_DATA:
        if (!on_stream)
            return broke(fault, H2_ERR_PROTOCOL, H2_RULE_NEEDS_STREAM, false);
        frame->flow_len = len;
        if (padded && (!pad_length(&data, &len, &pad, fault) || !pad_drop(&len, pad, fault)))
            return false;
        break;
    case H2_FRAME_HEADERS:
        if (!on_stream)
            return broke(fault, H2_ERR_PROTOCOL, H2_RULE_NEEDS_STREAM, false);
        if (padded && !pad_length(&data, &len, &pad, fault))
            return false;
        if (header->flags & H2_FLAG_PRIORITY) {
            if (len < 5)
                return broke(fault, H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, false);
            priority_read(data, frame);
            data += 5;
            len -= 5;
        }
        if (!pad_drop(&len, pad, fault))
            return false;
        break;
    case H2_FRAME_PRIORITY:
        if (!on_stream)
            return broke(fault, H2_ERR_PROTOCOL, H2_RULE_NEEDS_STREAM, false);
        // § 6.3: the one wrong length that is the stream's error.
        if (len != 5)
            return broke(fault, H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, true);
        priority_read(data, frame);
        len = 0;
        break;
    case H2_FRAME_RST_STREAM:
        if (!on_stream)
            return broke(fault, H2_ERR_PROTOCOL, H2_RULE_NEEDS_STREAM, false);
        if (len != 4)
            return broke(fault, H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, false);
        frame->value = get32(data);
        len = 0;
        break;
    case H2_FRAME_SETTINGS:
        if (on_stream)
            return broke(fault, H2_ERR_PROTOCOL, H2_RULE_NEEDS_CONNECTION, false);
        if ((header->flags & H2_FLAG_ACK) && len != 0)
            return broke(fault, H2_ERR_FRAME_SIZE, H2_RULE_SETTINGS_ACK, false);
        if (len % 6 != 0)
            return broke(fault, H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, false);
        break;
    case H2_FRAME_PUSH_PROMISE:
        if (!on_stream)
            return broke(fault, H2_ERR_PROTOCOL, H2_RULE_NEEDS_STREAM, false);
        if (padded && !pad_length(&data, &len, &pad, fault))
            return false;
        if (len < 4)
            return broke(fault, H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, false);
        frame->value = get32(data) & H2_STREAM_MOST;
        data += 4;
        len -= 4;
        if (!pad_drop(&len, pad, fault))
            return false;
        break;
    case H2_FRAME_PING:
        if (on_stream)
            return broke(fault, H2_ERR_PROTOCOL, H2_RULE_NEEDS_CONNECTION, false);
        if (len != 8)
            return broke(fault, H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, false);
        break;
    case H2_FRAME_GOAWAY:
        if (on_stream)
            return broke(fault, H2_ERR_PROTOCOL, H2_RULE_NEEDS_CONNECTION, false);
        if (len < 8)
            return broke(fault, H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, false);
        frame->last_stream = get32(data) & H2_STREAM_MOST;
        frame->value = get32(data + 4);
        data += 8;
        len -= 8;
        break;
    case H2_FRAME_WINDOW_UPDATE:
        if (len != 4)
            return broke(fault, H2_ERR_FRAME_SIZE, H2_RULE_LENGTH, false);
        frame->value = get32(data) & H2_STREAM_MOST;
        // § 6.9: nothing added is the stream's error on a stream and the
        // connection's on the connection.
        if (frame->value == 0)
            return broke(fault, H2_ERR_PROTOCOL, H2_RULE_WINDOW_ZERO, on_stream);
        len = 0;
        break;
    case H2_FRAME_CONTINUATION:
        if (!on_stream)
            return broke(fault, H2_ERR_PROTOCOL, H2_RULE_NEEDS_STREAM, false);
        break;
    default:
        return true;            // § 4.1: a type we do not know is ignored
    }
    frame->kind = header->type;
    frame->data = data;
    frame->data_len = len;
    return true;
}

uint32_t h2_settings_count(const h2_frame_t *frame)
{
    return frame->kind == H2_FRAME_SETTINGS ? frame->data_len / 6 : 0;
}

h2_setting_t h2_settings_at(const h2_frame_t *frame, uint32_t i)
{
    const uint8_t *p = frame->data + (size_t)i * 6;
    return (h2_setting_t){.identifier = (uint16_t)(p[0] << 8 | p[1]), .value = get32(p + 2)};
}

bool h2_setting_check(h2_setting_t setting, h2_fault_t *fault)
{
    *fault = (h2_fault_t){0};
    switch (setting.identifier) {
    case H2_SETTING_ENABLE_PUSH:
        if (setting.value > 1)
            return broke(fault, H2_ERR_PROTOCOL, H2_RULE_SETTING_VALUE, false);
        break;
    case H2_SETTING_INITIAL_WINDOW_SIZE:
        if (setting.value > H2_STREAM_MOST)
            return broke(fault, H2_ERR_FLOW_CONTROL, H2_RULE_SETTING_VALUE, false);
        break;
    case H2_SETTING_MAX_FRAME_SIZE:
        if (setting.value < H2_FRAME_SIZE_LEAST || setting.value > H2_FRAME_SIZE_MOST)
            return broke(fault, H2_ERR_PROTOCOL, H2_RULE_SETTING_VALUE, false);
        break;
    default:
        break;
    }
    return true;
}

// ── A field block across frames ─────────────────────────────────────────

void h2_block_init(h2_block_t *block, uint8_t *buffer, size_t room)
{
    *block = (h2_block_t){.buffer = buffer, .room = room};
}

static bool block_take(h2_block_t *block, const h2_frame_t *frame, h2_fault_t *fault)
{
    if (frame->data_len > block->room - block->len) {
        block->open = false;
        return broke(fault, H2_ERR_ENHANCE_YOUR_CALM, H2_RULE_BLOCK_TOO_LARGE, false);
    }
    if (frame->data_len != 0)
        os64_memcpy(block->buffer + block->len, frame->data, frame->data_len);
    block->len += frame->data_len;
    return true;
}

h2_block_step_t h2_block_feed(h2_block_t *block, const h2_frame_t *frame, h2_fault_t *fault)
{
    *fault = (h2_fault_t){0};
    if (block->open) {
        if (frame->kind != H2_FRAME_CONTINUATION || frame->stream != block->first.stream) {
            block->open = false;
            broke(fault, H2_ERR_PROTOCOL, H2_RULE_BLOCK_INTERRUPTED, false);
            return H2_BLOCK_FAULT;
        }
    } else if (frame->kind == H2_FRAME_CONTINUATION) {
        broke(fault, H2_ERR_PROTOCOL, H2_RULE_BLOCK_NOT_OPEN, false);
        return H2_BLOCK_FAULT;
    } else if (frame->kind == H2_FRAME_HEADERS || frame->kind == H2_FRAME_PUSH_PROMISE) {
        block->first = *frame;
        block->first.data = NULL;
        block->first.data_len = 0;
        block->len = 0;
        block->open = true;
    } else
        return H2_BLOCK_NOT_MINE;
    if (!block_take(block, frame, fault))
        return H2_BLOCK_FAULT;
    if (!(frame->flags & H2_FLAG_END_HEADERS))
        return H2_BLOCK_MORE;
    block->open = false;
    return H2_BLOCK_DONE;
}

// ── Writers ─────────────────────────────────────────────────────────────

static uint8_t *head(uint8_t *out, uint32_t length, uint8_t type, uint8_t flags, uint32_t stream)
{
    h2_frame_header_write(out, &(h2_frame_header_t){.length = length, .type = type, .flags = flags,
                                                    .stream = stream});
    return out + H2_FRAME_HEADER_LEN;
}

size_t h2_write_settings(uint8_t *out, size_t room, const h2_setting_t *settings, uint32_t count)
{
    if (count > H2_FRAME_SIZE_LEAST / 6)
        return 0;
    size_t total = H2_FRAME_HEADER_LEN + (size_t)count * 6;
    if (total > room)
        return 0;
    uint8_t *p = head(out, count * 6, H2_FRAME_SETTINGS, 0, 0);
    for (uint32_t i = 0; i < count; i++, p += 6) {
        p[0] = (uint8_t)(settings[i].identifier >> 8);
        p[1] = (uint8_t)settings[i].identifier;
        put32(p + 2, settings[i].value);
    }
    return total;
}

size_t h2_write_settings_ack(uint8_t *out, size_t room)
{
    if (room < H2_FRAME_HEADER_LEN)
        return 0;
    head(out, 0, H2_FRAME_SETTINGS, H2_FLAG_ACK, 0);
    return H2_FRAME_HEADER_LEN;
}

size_t h2_write_window_update(uint8_t *out, size_t room, uint32_t stream, uint32_t increment)
{
    if (room < H2_FRAME_HEADER_LEN + 4 || stream > H2_STREAM_MOST || increment == 0 || increment > H2_STREAM_MOST)
        return 0;
    put32(head(out, 4, H2_FRAME_WINDOW_UPDATE, 0, stream), increment);
    return H2_FRAME_HEADER_LEN + 4;
}

size_t h2_write_ping(uint8_t *out, size_t room, bool ack, const uint8_t opaque[8])
{
    if (room < H2_FRAME_HEADER_LEN + 8)
        return 0;
    os64_memcpy(head(out, 8, H2_FRAME_PING, ack ? H2_FLAG_ACK : 0, 0), opaque, 8);
    return H2_FRAME_HEADER_LEN + 8;
}

size_t h2_write_rst_stream(uint8_t *out, size_t room, uint32_t stream, uint32_t error)
{
    if (room < H2_FRAME_HEADER_LEN + 4 || stream == 0 || stream > H2_STREAM_MOST)
        return 0;
    put32(head(out, 4, H2_FRAME_RST_STREAM, 0, stream), error);
    return H2_FRAME_HEADER_LEN + 4;
}

size_t h2_write_goaway(uint8_t *out, size_t room, uint32_t last_stream, uint32_t error, const uint8_t *debug,
                       uint32_t debug_len)
{
    if (last_stream > H2_STREAM_MOST || debug_len > H2_FRAME_SIZE_LEAST - 8)
        return 0;
    size_t total = H2_FRAME_HEADER_LEN + 8 + (size_t)debug_len;
    if (total > room)
        return 0;
    uint8_t *p = head(out, 8 + debug_len, H2_FRAME_GOAWAY, 0, 0);
    put32(p, last_stream);
    put32(p + 4, error);
    if (debug_len != 0)
        os64_memcpy(p + 8, debug, debug_len);
    return total;
}

static void priority_write(uint8_t *p, uint32_t depends_on, bool exclusive, uint8_t weight)
{
    put32(p, depends_on | (exclusive ? ~H2_STREAM_MOST : 0));
    p[4] = weight;
}

size_t h2_write_priority(uint8_t *out, size_t room, uint32_t stream, uint32_t depends_on, bool exclusive,
                         uint8_t weight)
{
    if (room < H2_FRAME_HEADER_LEN + 5 || stream == 0 || stream > H2_STREAM_MOST || depends_on > H2_STREAM_MOST)
        return 0;
    priority_write(head(out, 5, H2_FRAME_PRIORITY, 0, stream), depends_on, exclusive, weight);
    return H2_FRAME_HEADER_LEN + 5;
}

// How much of a block the HEADERS frame carries, the priority fields having
// taken their five bytes of it first.
static size_t first_share(size_t block_len, uint32_t max_frame_size, bool with_priority)
{
    size_t most = max_frame_size - (with_priority ? 5u : 0u);
    return block_len < most ? block_len : most;
}

size_t h2_headers_size(size_t block_len, uint32_t max_frame_size, bool with_priority)
{
    if (max_frame_size < H2_FRAME_SIZE_LEAST || max_frame_size > H2_FRAME_SIZE_MOST)
        return 0;
    size_t first = first_share(block_len, max_frame_size, with_priority);
    size_t rest = block_len - first;
    size_t frames = 1 + (rest + max_frame_size - 1) / max_frame_size;
    return frames * H2_FRAME_HEADER_LEN + (with_priority ? 5u : 0u) + block_len;
}

size_t h2_write_headers(uint8_t *out, size_t room, uint32_t stream, const uint8_t *block, size_t block_len,
                        bool end_stream, const h2_priority_t *priority, uint32_t max_frame_size)
{
    size_t total = h2_headers_size(block_len, max_frame_size, priority != NULL);
    if (total == 0 || total > room || stream == 0 || stream > H2_STREAM_MOST ||
        (priority != NULL && priority->depends_on > H2_STREAM_MOST))
        return 0;
    size_t share = first_share(block_len, max_frame_size, priority != NULL);
    uint8_t flags = (end_stream ? H2_FLAG_END_STREAM : 0) | (priority != NULL ? H2_FLAG_PRIORITY : 0) |
                    (share == block_len ? H2_FLAG_END_HEADERS : 0);
    uint8_t *p = head(out, (uint32_t)(share + (priority != NULL ? 5u : 0u)), H2_FRAME_HEADERS, flags, stream);
    if (priority != NULL) {
        priority_write(p, priority->depends_on, priority->exclusive, priority->weight);
        p += 5;
    }
    if (share != 0)
        os64_memcpy(p, block, share);
    p += share;
    for (size_t at = share; at < block_len; at += share, p += share) {
        share = block_len - at < max_frame_size ? block_len - at : max_frame_size;
        p = head(p, (uint32_t)share, H2_FRAME_CONTINUATION, at + share == block_len ? H2_FLAG_END_HEADERS : 0,
                 stream);
        os64_memcpy(p, block + at, share);
    }
    return total;
}

size_t h2_write_data(uint8_t *out, size_t room, uint32_t stream, const uint8_t *data, size_t len, uint32_t most,
                     bool end_stream, size_t *taken)
{
    *taken = 0;
    if (stream == 0 || stream > H2_STREAM_MOST || most > H2_FRAME_SIZE_MOST)
        return 0;
    size_t share = len < most ? len : most;
    if (H2_FRAME_HEADER_LEN + share > room)
        return 0;
    uint8_t *p = head(out, (uint32_t)share, H2_FRAME_DATA, end_stream && share == len ? H2_FLAG_END_STREAM : 0,
                      stream);
    if (share != 0)
        os64_memcpy(p, data, share);
    *taken = share;
    return H2_FRAME_HEADER_LEN + share;
}
