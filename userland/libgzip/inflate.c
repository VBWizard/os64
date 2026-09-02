// inflate.c — a bounded, streaming RFC 1951 decoder.
//
// Parsing compressed data is hostile-input work. Reads below are
// transactional at the bit level: a field or Huffman symbol is removed from
// the bit reservoir only when all of it is present. Bytes may move from the
// caller's chunk into that reservoir while a symbol waits for its tail; this
// guarantees NEED_INPUT has consumed the whole offered chunk and still avoids
// reading beyond the byte that contains the symbol. A gzip or zlib trailer is
// therefore left to the container above.

#include "gzip/inflate.h"
#include "os64/mem.h"

#define INFLATE_WINDOW_SIZE 32768u
#define INFLATE_WINDOW_MASK (INFLATE_WINDOW_SIZE - 1u)
#define INFLATE_MAX_BITS    15u
#define INFLATE_MAX_SYMBOLS 320u

typedef struct {
    uint16_t count[INFLATE_MAX_BITS + 1u];
    uint16_t symbol[288];
    uint8_t max_bits;
} inflate_huffman_t;

typedef enum {
    MODE_BLOCK_HEADER,
    MODE_STORED_LENGTH,
    MODE_STORED_COPY,
    MODE_DYNAMIC_COUNTS,
    MODE_DYNAMIC_CODE_LENGTHS,
    MODE_DYNAMIC_LENGTHS,
    MODE_DYNAMIC_REPEAT,
    MODE_COMPRESSED_SYMBOL,
    MODE_LITERAL,
    MODE_LENGTH_EXTRA,
    MODE_DISTANCE_SYMBOL,
    MODE_DISTANCE_EXTRA,
    MODE_COPY,
    MODE_DONE,
    MODE_ERROR
} inflate_mode_t;

struct os64_inflate {
    inflate_mode_t mode;
    os64_inflate_status_t terminal;
    uint64_t bit_buffer;
    uint64_t output_size;
    uint64_t output_limit;
    uint32_t bit_count;
    uint32_t window_position;
    uint32_t window_have;
    uint32_t stored_remaining;
    uint32_t copy_remaining;
    uint32_t copy_distance;
    uint32_t pending_base;
    uint16_t pending_literal;
    uint8_t pending_extra;
    bool final_block;

    uint16_t dynamic_hlit;
    uint16_t dynamic_hdist;
    uint16_t dynamic_hclen;
    uint16_t dynamic_index;
    uint16_t dynamic_total;
    uint16_t repeat_value;
    uint16_t repeat_base;
    uint8_t repeat_extra;

    uint8_t code_length_lengths[19];
    uint8_t lengths[INFLATE_MAX_SYMBOLS];
    inflate_huffman_t code_length_tree;
    inflate_huffman_t literal_tree;
    inflate_huffman_t distance_tree;
    uint8_t window[INFLATE_WINDOW_SIZE];
};

static const uint16_t kLengthBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};

static const uint8_t kLengthExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

static const uint16_t kDistanceBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
    6145, 8193, 12289, 16385, 24577
};

static const uint8_t kDistanceExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static const uint8_t kCodeLengthOrder[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static void bytes_zero(void *memory, size_t length)
{
    uint8_t *bytes = memory;
    for (size_t i = 0; i < length; i++)
        bytes[i] = 0;
}

// Build the canonical-code decoding order described in RFC 1951 section
// 3.2.2. allow_empty is needed for a literal-only dynamic block: its one
// declared distance alphabet may contain no code because no distance will be
// read. Any attempt to use that empty tree is still malformed.
static bool huffman_build(inflate_huffman_t *tree, const uint8_t *lengths,
                          uint16_t symbols, bool allow_empty,
                          bool allow_single)
{
    uint16_t offsets[INFLATE_MAX_BITS + 1u];
    bytes_zero(tree, sizeof(*tree));

    for (uint16_t symbol = 0; symbol < symbols; symbol++) {
        if (lengths[symbol] > INFLATE_MAX_BITS)
            return false;
        tree->count[lengths[symbol]]++;
        if (lengths[symbol] > tree->max_bits)
            tree->max_bits = lengths[symbol];
    }

    uint16_t used = (uint16_t)(symbols - tree->count[0]);
    if (used == 0)
        return allow_empty;

    int32_t left = 1;
    for (uint32_t bits = 1; bits <= INFLATE_MAX_BITS; bits++) {
        left <<= 1;
        left -= tree->count[bits];
        if (left < 0)
            return false;
    }
    if (left != 0 && !(allow_single && used == 1 && tree->max_bits == 1))
        return false;

    offsets[1] = 0;
    for (uint32_t bits = 1; bits < INFLATE_MAX_BITS; bits++)
        offsets[bits + 1u] = (uint16_t)(offsets[bits] + tree->count[bits]);
    for (uint16_t symbol = 0; symbol < symbols; symbol++) {
        uint8_t length = lengths[symbol];
        if (length != 0)
            tree->symbol[offsets[length]++] = symbol;
    }
    return true;
}

static bool fill_bits(os64_inflate_t *stream, const uint8_t **input,
                      size_t *length, uint32_t wanted)
{
    while (stream->bit_count < wanted) {
        if (*length == 0)
            return false;
        stream->bit_buffer |= (uint64_t)*(*input)++ << stream->bit_count;
        (*length)--;
        stream->bit_count += 8;
    }
    return true;
}

static bool read_bits(os64_inflate_t *stream, const uint8_t **input,
                      size_t *length, uint32_t count, uint32_t *value)
{
    if (!fill_bits(stream, input, length, count))
        return false;
    uint64_t mask = count == 32 ? 0xffffffffu : ((1ULL << count) - 1u);
    *value = (uint32_t)(stream->bit_buffer & mask);
    stream->bit_buffer >>= count;
    stream->bit_count -= count;
    return true;
}

typedef enum {
    HUFFMAN_OK,
    HUFFMAN_NEED_INPUT,
    HUFFMAN_INVALID
} huffman_result_t;

static huffman_result_t huffman_decode(os64_inflate_t *stream,
                                       const inflate_huffman_t *tree,
                                       const uint8_t **input, size_t *length,
                                       uint16_t *symbol)
{
    if (tree->max_bits == 0)
        return HUFFMAN_INVALID;

    uint32_t code = 0;
    uint32_t first = 0;
    uint32_t index = 0;

    for (uint32_t bits = 1; bits <= tree->max_bits; bits++) {
        if (!fill_bits(stream, input, length, bits))
            return HUFFMAN_NEED_INPUT;
        code |= (uint32_t)((stream->bit_buffer >> (bits - 1u)) & 1u);

        uint32_t count = tree->count[bits];
        if (code >= first && code - first < count) {
            *symbol = tree->symbol[index + code - first];
            stream->bit_buffer >>= bits;
            stream->bit_count -= bits;
            return HUFFMAN_OK;
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return HUFFMAN_INVALID;
}

static void refuse(os64_inflate_t *stream, os64_inflate_status_t status)
{
    stream->mode = MODE_ERROR;
    stream->terminal = status;
}

static os64_inflate_status_t input_short(os64_inflate_t *stream,
                                         bool end_of_input)
{
    if (end_of_input) {
        refuse(stream, OS64_INFLATE_TRUNCATED);
        return OS64_INFLATE_TRUNCATED;
    }
    return OS64_INFLATE_NEED_INPUT;
}

static bool build_fixed_trees(os64_inflate_t *stream)
{
    uint8_t lengths[288];
    for (uint16_t i = 0; i <= 143; i++) lengths[i] = 8;
    for (uint16_t i = 144; i <= 255; i++) lengths[i] = 9;
    for (uint16_t i = 256; i <= 279; i++) lengths[i] = 7;
    for (uint16_t i = 280; i <= 287; i++) lengths[i] = 8;
    if (!huffman_build(&stream->literal_tree, lengths, 288, false, false))
        return false;
    for (uint16_t i = 0; i < 32; i++) lengths[i] = 5;
    return huffman_build(&stream->distance_tree, lengths, 32, false, false);
}

static os64_inflate_status_t emit_byte(os64_inflate_t *stream, uint8_t byte,
                                       uint8_t **output, size_t *length)
{
    if (stream->output_size >= stream->output_limit) {
        refuse(stream, OS64_INFLATE_LIMIT);
        return OS64_INFLATE_LIMIT;
    }
    if (*length == 0)
        return OS64_INFLATE_NEED_OUTPUT;

    *(*output)++ = byte;
    (*length)--;
    stream->window[stream->window_position] = byte;
    stream->window_position = (stream->window_position + 1u) & INFLATE_WINDOW_MASK;
    if (stream->window_have < INFLATE_WINDOW_SIZE)
        stream->window_have++;
    stream->output_size++;
    return OS64_INFLATE_NEED_INPUT; // internal success marker; never returned
}

os64_inflate_t *os64_inflate_create(uint64_t output_limit)
{
    os64_inflate_t *stream = os64_malloc(sizeof(*stream));
    if (stream != NULL)
        os64_inflate_reset(stream, output_limit);
    return stream;
}

void os64_inflate_reset(os64_inflate_t *stream, uint64_t output_limit)
{
    if (stream == NULL)
        return;
    bytes_zero(stream, sizeof(*stream));
    stream->mode = MODE_BLOCK_HEADER;
    stream->terminal = OS64_INFLATE_NEED_INPUT;
    stream->output_limit = output_limit;
}

void os64_inflate_destroy(os64_inflate_t *stream)
{
    os64_free(stream);
}

uint64_t os64_inflate_output_size(const os64_inflate_t *stream)
{
    return stream == NULL ? 0 : stream->output_size;
}

const char *os64_inflate_status_name(os64_inflate_status_t status)
{
    switch (status) {
        case OS64_INFLATE_NEED_INPUT:   return "needs input";
        case OS64_INFLATE_NEED_OUTPUT:  return "needs output space";
        case OS64_INFLATE_DONE:         return "done";
        case OS64_INFLATE_TRUNCATED:    return "truncated stream";
        case OS64_INFLATE_MALFORMED:    return "malformed stream";
        case OS64_INFLATE_LIMIT:        return "output limit exceeded";
        case OS64_INFLATE_BAD_ARGUMENT: return "bad argument";
    }
    return "unknown";
}

os64_inflate_status_t os64_inflate_process(os64_inflate_t *stream,
                                            const uint8_t **input,
                                            size_t *input_length,
                                            uint8_t **output,
                                            size_t *output_length,
                                            bool end_of_input)
{
    if (stream == NULL || input == NULL || input_length == NULL ||
        output == NULL || output_length == NULL ||
        (*input_length != 0 && *input == NULL) ||
        (*output_length != 0 && *output == NULL))
        return OS64_INFLATE_BAD_ARGUMENT;
    if (stream->mode == MODE_DONE)
        return OS64_INFLATE_DONE;
    if (stream->mode == MODE_ERROR)
        return stream->terminal;

    for (;;) {
        uint32_t value;
        uint16_t symbol;
        huffman_result_t decoded;

        switch (stream->mode) {
            case MODE_BLOCK_HEADER:
                if (!read_bits(stream, input, input_length, 3, &value))
                    return input_short(stream, end_of_input);
                stream->final_block = (value & 1u) != 0;
                value >>= 1;
                if (value == 0) {
                    // Stored blocks begin at the next byte boundary. Because
                    // reads take only the bytes they need, these are solely
                    // padding bits from the block-header byte.
                    stream->bit_buffer = 0;
                    stream->bit_count = 0;
                    stream->mode = MODE_STORED_LENGTH;
                } else if (value == 1) {
                    if (!build_fixed_trees(stream)) {
                        refuse(stream, OS64_INFLATE_MALFORMED);
                        return stream->terminal;
                    }
                    stream->mode = MODE_COMPRESSED_SYMBOL;
                } else if (value == 2) {
                    stream->mode = MODE_DYNAMIC_COUNTS;
                } else {
                    refuse(stream, OS64_INFLATE_MALFORMED);
                    return stream->terminal;
                }
                break;

            case MODE_STORED_LENGTH:
                if (!read_bits(stream, input, input_length, 32, &value))
                    return input_short(stream, end_of_input);
                if (((value & 0xffffu) ^ (value >> 16)) != 0xffffu) {
                    refuse(stream, OS64_INFLATE_MALFORMED);
                    return stream->terminal;
                }
                stream->stored_remaining = value & 0xffffu;
                stream->mode = MODE_STORED_COPY;
                break;

            case MODE_STORED_COPY:
                if (stream->stored_remaining == 0) {
                    if (stream->final_block) {
                        stream->mode = MODE_DONE;
                        return OS64_INFLATE_DONE;
                    }
                    stream->mode = MODE_BLOCK_HEADER;
                    break;
                }
                if (*output_length == 0)
                    return OS64_INFLATE_NEED_OUTPUT;
                if (stream->output_size >= stream->output_limit) {
                    refuse(stream, OS64_INFLATE_LIMIT);
                    return stream->terminal;
                }
                if (!read_bits(stream, input, input_length, 8, &value))
                    return input_short(stream, end_of_input);
                (void)emit_byte(stream, (uint8_t)value, output, output_length);
                stream->stored_remaining--;
                break;

            case MODE_DYNAMIC_COUNTS:
                if (!read_bits(stream, input, input_length, 14, &value))
                    return input_short(stream, end_of_input);
                stream->dynamic_hlit = (uint16_t)((value & 31u) + 257u);
                stream->dynamic_hdist = (uint16_t)(((value >> 5) & 31u) + 1u);
                stream->dynamic_hclen = (uint16_t)(((value >> 10) & 15u) + 4u);
                if (stream->dynamic_hlit > 286) {
                    refuse(stream, OS64_INFLATE_MALFORMED);
                    return stream->terminal;
                }
                bytes_zero(stream->code_length_lengths,
                           sizeof(stream->code_length_lengths));
                stream->dynamic_index = 0;
                stream->mode = MODE_DYNAMIC_CODE_LENGTHS;
                break;

            case MODE_DYNAMIC_CODE_LENGTHS:
                if (stream->dynamic_index == stream->dynamic_hclen) {
                    if (!huffman_build(&stream->code_length_tree,
                                       stream->code_length_lengths, 19,
                                       false, false)) {
                        refuse(stream, OS64_INFLATE_MALFORMED);
                        return stream->terminal;
                    }
                    stream->dynamic_total = (uint16_t)(stream->dynamic_hlit +
                                                       stream->dynamic_hdist);
                    stream->dynamic_index = 0;
                    stream->mode = MODE_DYNAMIC_LENGTHS;
                    break;
                }
                if (!read_bits(stream, input, input_length, 3, &value))
                    return input_short(stream, end_of_input);
                stream->code_length_lengths[
                    kCodeLengthOrder[stream->dynamic_index++]] = (uint8_t)value;
                break;

            case MODE_DYNAMIC_LENGTHS:
                if (stream->dynamic_index == stream->dynamic_total) {
                    if (stream->lengths[256] == 0 ||
                        !huffman_build(&stream->literal_tree, stream->lengths,
                                       stream->dynamic_hlit, false, true) ||
                        !huffman_build(&stream->distance_tree,
                                       stream->lengths + stream->dynamic_hlit,
                                       stream->dynamic_hdist, true, true)) {
                        refuse(stream, OS64_INFLATE_MALFORMED);
                        return stream->terminal;
                    }
                    stream->mode = MODE_COMPRESSED_SYMBOL;
                    break;
                }
                decoded = huffman_decode(stream, &stream->code_length_tree,
                                         input, input_length, &symbol);
                if (decoded != HUFFMAN_OK) {
                    if (decoded == HUFFMAN_NEED_INPUT)
                        return input_short(stream, end_of_input);
                    refuse(stream, OS64_INFLATE_MALFORMED);
                    return stream->terminal;
                }
                if (symbol <= 15) {
                    stream->lengths[stream->dynamic_index++] = (uint8_t)symbol;
                } else if (symbol == 16) {
                    if (stream->dynamic_index == 0) {
                        refuse(stream, OS64_INFLATE_MALFORMED);
                        return stream->terminal;
                    }
                    stream->repeat_value = stream->lengths[stream->dynamic_index - 1u];
                    stream->repeat_base = 3;
                    stream->repeat_extra = 2;
                    stream->mode = MODE_DYNAMIC_REPEAT;
                } else if (symbol == 17) {
                    stream->repeat_value = 0;
                    stream->repeat_base = 3;
                    stream->repeat_extra = 3;
                    stream->mode = MODE_DYNAMIC_REPEAT;
                } else if (symbol == 18) {
                    stream->repeat_value = 0;
                    stream->repeat_base = 11;
                    stream->repeat_extra = 7;
                    stream->mode = MODE_DYNAMIC_REPEAT;
                } else {
                    refuse(stream, OS64_INFLATE_MALFORMED);
                    return stream->terminal;
                }
                break;

            case MODE_DYNAMIC_REPEAT:
                if (!read_bits(stream, input, input_length,
                               stream->repeat_extra, &value))
                    return input_short(stream, end_of_input);
                value += stream->repeat_base;
                if (value > (uint32_t)(stream->dynamic_total -
                                       stream->dynamic_index)) {
                    refuse(stream, OS64_INFLATE_MALFORMED);
                    return stream->terminal;
                }
                while (value-- != 0)
                    stream->lengths[stream->dynamic_index++] =
                        (uint8_t)stream->repeat_value;
                stream->mode = MODE_DYNAMIC_LENGTHS;
                break;

            case MODE_COMPRESSED_SYMBOL:
                decoded = huffman_decode(stream, &stream->literal_tree,
                                         input, input_length, &symbol);
                if (decoded != HUFFMAN_OK) {
                    if (decoded == HUFFMAN_NEED_INPUT)
                        return input_short(stream, end_of_input);
                    refuse(stream, OS64_INFLATE_MALFORMED);
                    return stream->terminal;
                }
                if (symbol < 256) {
                    stream->pending_literal = symbol;
                    stream->mode = MODE_LITERAL;
                } else if (symbol == 256) {
                    if (stream->final_block) {
                        stream->bit_buffer = 0;
                        stream->bit_count = 0;
                        stream->mode = MODE_DONE;
                        return OS64_INFLATE_DONE;
                    }
                    stream->mode = MODE_BLOCK_HEADER;
                } else if (symbol <= 285) {
                    uint16_t index = (uint16_t)(symbol - 257u);
                    stream->pending_base = kLengthBase[index];
                    stream->pending_extra = kLengthExtra[index];
                    stream->mode = MODE_LENGTH_EXTRA;
                } else {
                    refuse(stream, OS64_INFLATE_MALFORMED);
                    return stream->terminal;
                }
                break;

            case MODE_LITERAL: {
                os64_inflate_status_t emitted = emit_byte(
                    stream, (uint8_t)stream->pending_literal,
                    output, output_length);
                if (emitted == OS64_INFLATE_NEED_OUTPUT ||
                    emitted == OS64_INFLATE_LIMIT)
                    return emitted;
                stream->mode = MODE_COMPRESSED_SYMBOL;
                break;
            }

            case MODE_LENGTH_EXTRA:
                if (!read_bits(stream, input, input_length,
                               stream->pending_extra, &value))
                    return input_short(stream, end_of_input);
                stream->copy_remaining = stream->pending_base + value;
                stream->mode = MODE_DISTANCE_SYMBOL;
                break;

            case MODE_DISTANCE_SYMBOL:
                decoded = huffman_decode(stream, &stream->distance_tree,
                                         input, input_length, &symbol);
                if (decoded != HUFFMAN_OK) {
                    if (decoded == HUFFMAN_NEED_INPUT)
                        return input_short(stream, end_of_input);
                    refuse(stream, OS64_INFLATE_MALFORMED);
                    return stream->terminal;
                }
                if (symbol >= 30) {
                    refuse(stream, OS64_INFLATE_MALFORMED);
                    return stream->terminal;
                }
                stream->pending_base = kDistanceBase[symbol];
                stream->pending_extra = kDistanceExtra[symbol];
                stream->mode = MODE_DISTANCE_EXTRA;
                break;

            case MODE_DISTANCE_EXTRA:
                if (!read_bits(stream, input, input_length,
                               stream->pending_extra, &value))
                    return input_short(stream, end_of_input);
                stream->copy_distance = stream->pending_base + value;
                if (stream->copy_distance == 0 ||
                    stream->copy_distance > stream->window_have) {
                    refuse(stream, OS64_INFLATE_MALFORMED);
                    return stream->terminal;
                }
                stream->mode = MODE_COPY;
                break;

            case MODE_COPY:
                while (stream->copy_remaining != 0) {
                    if (*output_length == 0)
                        return OS64_INFLATE_NEED_OUTPUT;
                    uint32_t source = (stream->window_position -
                                       stream->copy_distance) &
                                      INFLATE_WINDOW_MASK;
                    os64_inflate_status_t emitted = emit_byte(
                        stream, stream->window[source], output, output_length);
                    if (emitted == OS64_INFLATE_LIMIT)
                        return emitted;
                    stream->copy_remaining--;
                }
                stream->mode = MODE_COMPRESSED_SYMBOL;
                break;

            case MODE_DONE:
                return OS64_INFLATE_DONE;
            case MODE_ERROR:
                return stream->terminal;
        }
    }
}
