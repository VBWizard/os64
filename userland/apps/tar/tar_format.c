// tar_format.c — encode and decode the fixed 512-byte POSIX ustar header.

#include "tar_format.h"

#define TAR_NAME_OFFSET       0
#define TAR_NAME_SIZE       100
#define TAR_MODE_OFFSET     100
#define TAR_MODE_SIZE         8
#define TAR_UID_OFFSET      108
#define TAR_UID_SIZE          8
#define TAR_GID_OFFSET      116
#define TAR_GID_SIZE          8
#define TAR_FILESIZE_OFFSET 124
#define TAR_FILESIZE_SIZE    12
#define TAR_MTIME_OFFSET    136
#define TAR_MTIME_SIZE       12
#define TAR_CHECKSUM_OFFSET 148
#define TAR_CHECKSUM_SIZE     8
#define TAR_TYPE_OFFSET     156
#define TAR_MAGIC_OFFSET    257
#define TAR_MAGIC_SIZE        6
#define TAR_VERSION_OFFSET  263
#define TAR_PREFIX_OFFSET   345
#define TAR_PREFIX_SIZE     155

static size_t text_length(const char *text)
{
    size_t length = 0;
    while (text != NULL && text[length] != '\0')
        length++;
    return length;
}

static size_t field_length(const uint8_t *field, size_t capacity)
{
    size_t length = 0;
    while (length < capacity && field[length] != 0)
        length++;
    return length;
}

static void fill(uint8_t *destination, uint8_t value, size_t length)
{
    for (size_t i = 0; i < length; i++)
        destination[i] = value;
}

static void copy(uint8_t *destination, const uint8_t *source, size_t length)
{
    for (size_t i = 0; i < length; i++)
        destination[i] = source[i];
}

static bool bytes_equal(const uint8_t *first, const char *second, size_t length)
{
    for (size_t i = 0; i < length; i++)
        if (first[i] != (uint8_t)second[i])
            return false;
    return true;
}

static bool field_is_zero(const uint8_t *field, size_t length)
{
    for (size_t i = 0; i < length; i++)
        if (field[i] != 0)
            return false;
    return true;
}

static bool put_octal(uint8_t *field, size_t length, uint64_t value)
{
    if (length < 2)
        return false;

    size_t digits = length - 1;
    uint64_t remaining = value;
    for (size_t i = 0; i < digits; i++) {
        size_t position = digits - i - 1;
        field[position] = (uint8_t)('0' + (remaining & 7u));
        remaining >>= 3;
    }
    field[digits] = 0;
    return remaining == 0;
}

static bool get_octal(const uint8_t *field, size_t length, uint64_t *value)
{
    if (length == 0 || (field[0] & 0x80u) != 0)
        return false;

    size_t position = 0;
    while (position < length && field[position] == ' ')
        position++;

    uint64_t parsed = 0;
    bool saw_digit = false;
    for (; position < length; position++) {
        uint8_t byte = field[position];
        if (byte == 0 || byte == ' ')
            break;
        if (byte < '0' || byte > '7')
            return false;
        if (parsed > (UINT64_MAX - (uint64_t)(byte - '0')) / 8u)
            return false;
        parsed = parsed * 8u + (uint64_t)(byte - '0');
        saw_digit = true;
    }

    for (; position < length; position++)
        if (field[position] != 0 && field[position] != ' ')
            return false;

    if (!saw_digit)
        return false;
    *value = parsed;
    return true;
}

static uint64_t header_checksum(const uint8_t block[TAR_BLOCK_SIZE])
{
    uint64_t sum = 0;
    for (size_t i = 0; i < TAR_BLOCK_SIZE; i++) {
        if (i >= TAR_CHECKSUM_OFFSET &&
            i < TAR_CHECKSUM_OFFSET + TAR_CHECKSUM_SIZE)
            sum += (uint8_t)' ';
        else
            sum += block[i];
    }
    return sum;
}

static bool split_name(const char *path, size_t *prefix_length,
                       size_t *name_offset, size_t *name_length)
{
    size_t length = text_length(path);
    if (length == 0 || length >= TAR_PATH_CAP)
        return false;

    if (length <= TAR_NAME_SIZE) {
        *prefix_length = 0;
        *name_offset = 0;
        *name_length = length;
        return true;
    }

    // Prefer the rightmost separator that fits both ustar fields. The slash
    // belongs to neither field; the decoder supplies it between them.
    for (size_t separator = length; separator > 0; separator--) {
        if (path[separator - 1] != '/')
            continue;
        size_t prefix = separator - 1;
        size_t name = length - separator;
        if (prefix > 0 && prefix <= TAR_PREFIX_SIZE &&
            name > 0 && name <= TAR_NAME_SIZE) {
            *prefix_length = prefix;
            *name_offset = separator;
            *name_length = name;
            return true;
        }
    }
    return false;
}

bool tar_block_is_zero(const uint8_t block[TAR_BLOCK_SIZE])
{
    return field_is_zero(block, TAR_BLOCK_SIZE);
}

tar_format_result_t tar_header_encode(uint8_t block[TAR_BLOCK_SIZE],
                                      const tar_entry_t *entry)
{
    size_t prefix_length;
    size_t name_offset;
    size_t name_length;
    if (entry == NULL || entry->path[0] == '\0')
        return TAR_FORMAT_EMPTY_NAME;
    if (!split_name(entry->path, &prefix_length, &name_offset, &name_length))
        return TAR_FORMAT_PATH_TOO_LONG;

    fill(block, 0, TAR_BLOCK_SIZE);
    copy(block + TAR_NAME_OFFSET,
         (const uint8_t *)entry->path + name_offset, name_length);
    if (prefix_length != 0)
        copy(block + TAR_PREFIX_OFFSET, (const uint8_t *)entry->path,
             prefix_length);

    if (!put_octal(block + TAR_MODE_OFFSET, TAR_MODE_SIZE, entry->mode) ||
        !put_octal(block + TAR_UID_OFFSET, TAR_UID_SIZE, 0) ||
        !put_octal(block + TAR_GID_OFFSET, TAR_GID_SIZE, 0) ||
        !put_octal(block + TAR_FILESIZE_OFFSET, TAR_FILESIZE_SIZE,
                   entry->size) ||
        !put_octal(block + TAR_MTIME_OFFSET, TAR_MTIME_SIZE, entry->mtime))
        return TAR_FORMAT_BAD_NUMBER;

    block[TAR_TYPE_OFFSET] = (uint8_t)entry->type;
    copy(block + TAR_MAGIC_OFFSET, (const uint8_t *)"ustar\0", TAR_MAGIC_SIZE);
    copy(block + TAR_VERSION_OFFSET, (const uint8_t *)"00", 2);
    copy(block + 265, (const uint8_t *)"os64", 4);
    copy(block + 297, (const uint8_t *)"os64", 4);

    uint64_t checksum = header_checksum(block);
    if (!put_octal(block + TAR_CHECKSUM_OFFSET, TAR_CHECKSUM_SIZE - 1,
                   checksum))
        return TAR_FORMAT_BAD_NUMBER;
    block[TAR_CHECKSUM_OFFSET + 6] = 0;
    block[TAR_CHECKSUM_OFFSET + 7] = ' ';
    return TAR_FORMAT_OK;
}

tar_format_result_t tar_header_decode(const uint8_t block[TAR_BLOCK_SIZE],
                                      tar_entry_t *entry)
{
    uint64_t recorded_checksum;
    if (entry == NULL ||
        !get_octal(block + TAR_CHECKSUM_OFFSET, TAR_CHECKSUM_SIZE,
                   &recorded_checksum))
        return TAR_FORMAT_BAD_CHECKSUM;
    if (recorded_checksum != header_checksum(block))
        return TAR_FORMAT_BAD_CHECKSUM;

    bool ustar = bytes_equal(block + TAR_MAGIC_OFFSET, "ustar", 5);
    if (!ustar && !field_is_zero(block + TAR_MAGIC_OFFSET, TAR_MAGIC_SIZE))
        return TAR_FORMAT_BAD_MAGIC;

    size_t name_length = field_length(block + TAR_NAME_OFFSET, TAR_NAME_SIZE);
    size_t prefix_length = ustar
        ? field_length(block + TAR_PREFIX_OFFSET, TAR_PREFIX_SIZE) : 0;
    if (name_length == 0)
        return TAR_FORMAT_EMPTY_NAME;

    size_t path_length = prefix_length + (prefix_length != 0 ? 1u : 0u) +
                         name_length;
    if (path_length >= TAR_PATH_CAP)
        return TAR_FORMAT_PATH_TOO_LONG;

    size_t out = 0;
    if (prefix_length != 0) {
        for (size_t i = 0; i < prefix_length; i++)
            entry->path[out++] = (char)block[TAR_PREFIX_OFFSET + i];
        entry->path[out++] = '/';
    }
    for (size_t i = 0; i < name_length; i++)
        entry->path[out++] = (char)block[TAR_NAME_OFFSET + i];
    entry->path[out] = '\0';

    uint64_t mode;
    if (!get_octal(block + TAR_MODE_OFFSET, TAR_MODE_SIZE, &mode) ||
        mode > UINT32_MAX ||
        !get_octal(block + TAR_FILESIZE_OFFSET, TAR_FILESIZE_SIZE,
                   &entry->size) ||
        !get_octal(block + TAR_MTIME_OFFSET, TAR_MTIME_SIZE, &entry->mtime))
        return TAR_FORMAT_BAD_NUMBER;
    entry->mode = (uint32_t)mode;
    entry->type = (char)block[TAR_TYPE_OFFSET];
    if (entry->type == '\0')
        entry->type = TAR_TYPE_REGULAR;
    return TAR_FORMAT_OK;
}

static tar_format_result_t normalize_path(const char *input,
                                          char out[TAR_PATH_CAP],
                                          bool extraction)
{
    if (input == NULL || input[0] == '\0')
        return TAR_FORMAT_EMPTY_NAME;
    if (extraction && input[0] == '/')
        return TAR_FORMAT_PATH_TOO_LONG;

    size_t length = 0;
    const char *cursor = input;
    while (*cursor != '\0') {
        while (*cursor == '/')
            cursor++;
        if (*cursor == '\0')
            break;

        const char *component = cursor;
        while (*cursor != '\0' && *cursor != '/')
            cursor++;
        size_t component_length = (size_t)(cursor - component);

        if (component_length == 1 && component[0] == '.')
            continue;
        if (component_length == 2 && component[0] == '.' &&
            component[1] == '.') {
            if (extraction)
                return TAR_FORMAT_PATH_TOO_LONG;
            if (length != 0) {
                while (length > 0 && out[length - 1] != '/')
                    length--;
                if (length > 0)
                    length--;
            }
            continue;
        }

        size_t separator = length != 0 ? 1u : 0u;
        if (length + separator + component_length >= TAR_PATH_CAP)
            return TAR_FORMAT_PATH_TOO_LONG;
        if (separator != 0)
            out[length++] = '/';
        for (size_t i = 0; i < component_length; i++)
            out[length++] = component[i];
    }

    if (length == 0)
        out[length++] = '.';
    out[length] = '\0';
    return TAR_FORMAT_OK;
}

tar_format_result_t tar_member_path(const char *source,
                                    char out[TAR_PATH_CAP])
{
    return normalize_path(source, out, false);
}

tar_format_result_t tar_extract_path(const char *member,
                                     char out[TAR_PATH_CAP])
{
    return normalize_path(member, out, true);
}

const char *tar_format_error(tar_format_result_t result)
{
    switch (result) {
    case TAR_FORMAT_OK:            return "no error";
    case TAR_FORMAT_PATH_TOO_LONG: return "unsafe or overlong path";
    case TAR_FORMAT_BAD_CHECKSUM:  return "bad header checksum";
    case TAR_FORMAT_BAD_MAGIC:     return "unrecognized header magic";
    case TAR_FORMAT_BAD_NUMBER:    return "invalid numeric field";
    case TAR_FORMAT_EMPTY_NAME:    return "empty member name";
    }
    return "unknown format error";
}
