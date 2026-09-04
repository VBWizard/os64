// deflate.c — a bounded, streaming RFC 1951 encoder.
//
// Fixed Huffman codes keep the format engine small, while a hash-chained
// 32 KiB LZ77 search captures the repetition that dominates logs. Input is
// collected in format-sized blocks rather than caller-sized blocks, so a pipe
// delivering one byte at a time produces the same stream as a regular file.

#include "gzip/deflate.h"
#include "os64/mem.h"
#include "os64/str.h"

#define DEFLATE_WINDOW_SIZE  32768u
#define DEFLATE_WINDOW_MASK  (DEFLATE_WINDOW_SIZE - 1u)
#define DEFLATE_BLOCK_SIZE   32768u
#define DEFLATE_HASH_BITS    13u
#define DEFLATE_HASH_SIZE    (1u << DEFLATE_HASH_BITS)
#define DEFLATE_CHAIN_LIMIT  128u
#define DEFLATE_MIN_MATCH    3u
#define DEFLATE_MAX_MATCH    258u
#define DEFLATE_PENDING_SIZE 40000u
#define DEFLATE_NO_POSITION  UINT64_MAX

// A fixed-code literal costs at most nine bits. Add the block header, EOB,
// one carried partial byte from the preceding block, and final byte rounding.
_Static_assert(DEFLATE_PENDING_SIZE >=
               (DEFLATE_BLOCK_SIZE * 9u + 3u + 7u + 7u + 7u) / 8u,
               "pending buffer holds the worst fixed-Huffman block");

typedef enum {
    DEFLATE_MODE_COLLECT,
    DEFLATE_MODE_EMIT,
    DEFLATE_MODE_DONE
} deflate_mode_t;

struct os64_deflate {
    deflate_mode_t mode;
    uint64_t position;
    uint64_t output_size;
    uint64_t bit_buffer;
    uint64_t head[DEFLATE_HASH_SIZE];
    uint32_t previous_distance[DEFLATE_WINDOW_SIZE];
    size_t block_size;
    size_t pending_size;
    size_t pending_position;
    uint32_t bit_count;
    bool final_pending;
    uint8_t block[DEFLATE_BLOCK_SIZE];
    uint8_t window[DEFLATE_WINDOW_SIZE];
    uint8_t pending[DEFLATE_PENDING_SIZE];
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

static uint32_t reverse_bits(uint32_t value, uint32_t count)
{
    uint32_t reversed = 0;
    while (count-- != 0) {
        reversed = (reversed << 1) | (value & 1u);
        value >>= 1;
    }
    return reversed;
}

static void write_bits(os64_deflate_t *stream, uint32_t value, uint32_t count)
{
    stream->bit_buffer |= (uint64_t)value << stream->bit_count;
    stream->bit_count += count;
    while (stream->bit_count >= 8) {
        stream->pending[stream->pending_size++] =
            (uint8_t)stream->bit_buffer;
        stream->bit_buffer >>= 8;
        stream->bit_count -= 8;
    }
}

static void write_fixed_symbol(os64_deflate_t *stream, uint16_t symbol)
{
    uint32_t code;
    uint32_t bits;
    if (symbol <= 143) {
        code = 0x30u + symbol;
        bits = 8;
    } else if (symbol <= 255) {
        code = 0x190u + symbol - 144u;
        bits = 9;
    } else if (symbol <= 279) {
        code = symbol - 256u;
        bits = 7;
    } else {
        code = 0xc0u + symbol - 280u;
        bits = 8;
    }
    write_bits(stream, reverse_bits(code, bits), bits);
}

static uint32_t hash_bytes(const uint8_t *bytes)
{
    uint32_t value = (uint32_t)bytes[0] * 251u;
    value = (value ^ bytes[1]) * 251u;
    value ^= bytes[2];
    return value & (DEFLATE_HASH_SIZE - 1u);
}

static uint8_t source_byte(const os64_deflate_t *stream,
                           uint64_t block_start, uint64_t position)
{
    if (position >= block_start)
        return stream->block[(size_t)(position - block_start)];
    return stream->window[position & DEFLATE_WINDOW_MASK];
}

static void insert_position(os64_deflate_t *stream, uint64_t block_start,
                            size_t offset)
{
    uint64_t position = block_start + offset;
    stream->window[position & DEFLATE_WINDOW_MASK] = stream->block[offset];
    stream->previous_distance[position & DEFLATE_WINDOW_MASK] = 0;

    if (offset + DEFLATE_MIN_MATCH > stream->block_size)
        return;

    uint32_t hash = hash_bytes(stream->block + offset);
    uint64_t previous = stream->head[hash];
    if (previous != DEFLATE_NO_POSITION &&
        position - previous <= DEFLATE_WINDOW_SIZE)
        stream->previous_distance[position & DEFLATE_WINDOW_MASK] =
            (uint32_t)(position - previous);
    stream->head[hash] = position;
}

static size_t find_match(const os64_deflate_t *stream, uint64_t block_start,
                         size_t offset, uint32_t *distance_out)
{
    size_t available = stream->block_size - offset;
    if (available < DEFLATE_MIN_MATCH)
        return 0;
    if (available > DEFLATE_MAX_MATCH)
        available = DEFLATE_MAX_MATCH;

    uint64_t position = block_start + offset;
    uint64_t candidate = stream->head[hash_bytes(stream->block + offset)];
    size_t best = 0;
    uint32_t best_distance = 0;

    for (uint32_t searched = 0;
         candidate != DEFLATE_NO_POSITION && candidate < position &&
         position - candidate <= DEFLATE_WINDOW_SIZE &&
         searched < DEFLATE_CHAIN_LIMIT;
         searched++) {
        size_t length = 0;
        while (length < available &&
               source_byte(stream, block_start, candidate + length) ==
                   stream->block[offset + length])
            length++;
        if (length >= DEFLATE_MIN_MATCH && length > best) {
            best = length;
            best_distance = (uint32_t)(position - candidate);
            if (best == available)
                break;
        }

        uint32_t previous =
            stream->previous_distance[candidate & DEFLATE_WINDOW_MASK];
        if (previous == 0 || previous > candidate)
            break;
        candidate -= previous;
    }

    *distance_out = best_distance;
    return best;
}

static void write_match(os64_deflate_t *stream, size_t length,
                        uint32_t distance)
{
    uint32_t length_code = 0;
    while (length_code + 1u < 29u &&
           length >= kLengthBase[length_code + 1u])
        length_code++;
    write_fixed_symbol(stream, (uint16_t)(257u + length_code));
    uint32_t length_extra = kLengthExtra[length_code];
    if (length_extra != 0)
        write_bits(stream, (uint32_t)length - kLengthBase[length_code],
                   length_extra);

    uint32_t distance_code = 0;
    while (distance_code + 1u < 30u &&
           distance >= kDistanceBase[distance_code + 1u])
        distance_code++;
    write_bits(stream, reverse_bits(distance_code, 5), 5);
    uint32_t distance_extra = kDistanceExtra[distance_code];
    if (distance_extra != 0)
        write_bits(stream, distance - kDistanceBase[distance_code],
                   distance_extra);
}

static void generate_block(os64_deflate_t *stream, bool final)
{
    stream->pending_size = 0;
    stream->pending_position = 0;
    stream->final_pending = final;

    // BTYPE=01 selects the fixed tables. DEFLATE writes the least-significant
    // bit first, so BFINAL occupies bit zero of this three-bit field.
    write_bits(stream, (final ? 1u : 0u) | 2u, 3);

    uint64_t block_start = stream->position;
    size_t offset = 0;
    while (offset < stream->block_size) {
        uint32_t distance = 0;
        size_t length = find_match(stream, block_start, offset, &distance);
        if (length >= DEFLATE_MIN_MATCH) {
            write_match(stream, length, distance);
            for (size_t i = 0; i < length; i++)
                insert_position(stream, block_start, offset + i);
            offset += length;
        } else {
            write_fixed_symbol(stream, stream->block[offset]);
            insert_position(stream, block_start, offset);
            offset++;
        }
    }
    write_fixed_symbol(stream, 256);

    stream->position += stream->block_size;
    stream->block_size = 0;
    if (final && stream->bit_count != 0) {
        stream->pending[stream->pending_size++] =
            (uint8_t)stream->bit_buffer;
        stream->bit_buffer = 0;
        stream->bit_count = 0;
    }
    stream->mode = DEFLATE_MODE_EMIT;
}

os64_deflate_t *os64_deflate_create(void)
{
    os64_deflate_t *stream = os64_malloc(sizeof(*stream));
    if (stream != NULL)
        os64_deflate_reset(stream);
    return stream;
}

void os64_deflate_reset(os64_deflate_t *stream)
{
    if (stream == NULL)
        return;
    os64_memset(stream, 0, sizeof(*stream));
    os64_memset(stream->head, 0xff, sizeof(stream->head));
    stream->mode = DEFLATE_MODE_COLLECT;
}

void os64_deflate_destroy(os64_deflate_t *stream)
{
    os64_free(stream);
}

uint64_t os64_deflate_input_size(const os64_deflate_t *stream)
{
    if (stream == NULL)
        return 0;
    return stream->position + stream->block_size;
}

uint64_t os64_deflate_output_size(const os64_deflate_t *stream)
{
    return stream == NULL ? 0 : stream->output_size;
}

const char *os64_deflate_status_name(os64_deflate_status_t status)
{
    switch (status) {
        case OS64_DEFLATE_NEED_INPUT:  return "needs input";
        case OS64_DEFLATE_NEED_OUTPUT: return "needs output space";
        case OS64_DEFLATE_DONE:        return "done";
        case OS64_DEFLATE_BAD_ARGUMENT: return "bad argument";
    }
    return "unknown";
}

os64_deflate_status_t os64_deflate_process(os64_deflate_t *stream,
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
        return OS64_DEFLATE_BAD_ARGUMENT;
    if (stream->mode == DEFLATE_MODE_DONE)
        return OS64_DEFLATE_DONE;

    for (;;) {
        if (stream->mode == DEFLATE_MODE_EMIT) {
            size_t available = stream->pending_size - stream->pending_position;
            size_t amount = available < *output_length ? available :
                                                         *output_length;
            if (amount != 0) {
                os64_memcpy(*output,
                            stream->pending + stream->pending_position,
                            amount);
                *output += amount;
                *output_length -= amount;
                stream->pending_position += amount;
                stream->output_size += amount;
            }
            if (stream->pending_position != stream->pending_size)
                return OS64_DEFLATE_NEED_OUTPUT;
            if (stream->final_pending) {
                stream->mode = DEFLATE_MODE_DONE;
                return OS64_DEFLATE_DONE;
            }
            stream->mode = DEFLATE_MODE_COLLECT;
        }

        size_t capacity = DEFLATE_BLOCK_SIZE - stream->block_size;
        size_t amount = *input_length < capacity ? *input_length : capacity;
        if (amount != 0) {
            os64_memcpy(stream->block + stream->block_size, *input, amount);
            stream->block_size += amount;
            *input += amount;
            *input_length -= amount;
        }

        if (stream->block_size == DEFLATE_BLOCK_SIZE) {
            generate_block(stream, end_of_input && *input_length == 0);
            continue;
        }
        if (end_of_input && *input_length == 0) {
            generate_block(stream, true);
            continue;
        }
        return OS64_DEFLATE_NEED_INPUT;
    }
}
