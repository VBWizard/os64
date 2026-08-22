#include "hex_buffer.h"
#include "os64/os64.h"

#define HEX_SAVE_CHUNK 256u
#define HEX_MAPS_MAX_BYTES (1024u * 1024u)

static size_t patch_lower_bound(const hex_buffer_t *buffer, uint64_t offset,
                                bool *found)
{
    size_t lo = 0;
    size_t hi = buffer->patch_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (buffer->patches[mid].offset < offset)
            lo = mid + 1;
        else
            hi = mid;
    }
    *found = lo < buffer->patch_count &&
             buffer->patches[lo].offset == offset;
    return lo;
}

static int reserve_patches(hex_buffer_t *buffer, size_t needed)
{
    if (needed <= buffer->patch_capacity)
        return 0;
    size_t capacity = buffer->patch_capacity == 0 ? 32 : buffer->patch_capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2)
            return -1;
        capacity *= 2;
    }
    hex_patch_t *grown = os64_realloc(buffer->patches,
                                      capacity * sizeof(*grown));
    if (grown == NULL)
        return -1;
    buffer->patches = grown;
    buffer->patch_capacity = capacity;
    return 0;
}

static int reserve_undo(hex_buffer_t *buffer, size_t needed)
{
    if (needed <= buffer->undo_capacity)
        return 0;
    size_t capacity = buffer->undo_capacity == 0 ? 32 : buffer->undo_capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2)
            return -1;
        capacity *= 2;
    }
    hex_undo_t *grown = os64_realloc(buffer->undo, capacity * sizeof(*grown));
    if (grown == NULL)
        return -1;
    buffer->undo = grown;
    buffer->undo_capacity = capacity;
    return 0;
}

static int reserve_maps(hex_buffer_t *buffer, size_t needed)
{
    if (needed <= buffer->map_capacity)
        return 0;
    size_t capacity = buffer->map_capacity == 0 ? 16 : buffer->map_capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2)
            return -1;
        capacity *= 2;
    }
    hex_map_t *grown = os64_realloc(buffer->maps,
                                    capacity * sizeof(*grown));
    if (grown == NULL)
        return -1;
    buffer->maps = grown;
    buffer->map_capacity = capacity;
    return 0;
}

bool hex_buffer_is_proc_mem_path(const char *path)
{
    const char prefix[] = "/proc/";
    if (path == NULL)
        return false;
    size_t at = 0;
    while (prefix[at] != '\0' && path[at] == prefix[at])
        at++;
    if (prefix[at] != '\0')
        return false;

    // A task id — or `self`, which /proc resolves to the caller (and which
    // is the natural first thing anyone points a hex editor at). The second
    // draft of this test took digits only and silently sent /proc/self/mem
    // down the regular-file path, where mem's refused SEEK_END made the open
    // fail (review, 2026-08-22).
    size_t first_digit = at;
    while (path[at] >= '0' && path[at] <= '9')
        at++;
    if (at == first_digit && path[at] == 's' && path[at + 1] == 'e' &&
        path[at + 2] == 'l' && path[at + 3] == 'f')
        at += 4;
    return at != first_digit && path[at] == '/' && path[at + 1] == 'm' &&
           path[at + 2] == 'e' && path[at + 3] == 'm' &&
           path[at + 4] == '\0';
}

static char *proc_maps_path(const char *mem_path)
{
    if (!hex_buffer_is_proc_mem_path(mem_path))
        return NULL;
    size_t length = os64_strlen(mem_path);
    char *path = os64_malloc(length + 2); // "maps" is one byte longer than "mem"
    if (path == NULL)
        return NULL;
    for (size_t i = 0; i < length - 3; i++)
        path[i] = mem_path[i];
    path[length - 3] = 'm';
    path[length - 2] = 'a';
    path[length - 1] = 'p';
    path[length] = 's';
    path[length + 1] = '\0';
    return path;
}

static void copy_field(char *to, size_t capacity, const char *from,
                       const char *end)
{
    size_t count = (size_t)(end - from);
    if (count >= capacity)
        count = capacity - 1;
    for (size_t i = 0; i < count; i++)
        to[i] = from[i];
    to[count] = '\0';
}

static int parse_maps_text(hex_buffer_t *parsed, char *text)
{
    const char *line = text;
    while (*line != '\0') {
        const char *end = line;
        while (*end != '\0' && *end != '\n')
            end++;

        uint64_t start;
        uint64_t finish;
        if (os64_parse_range(line, &start, &finish) && finish > start &&
            finish <= (uint64_t)INT64_MAX + 1u) {
            const char *field = line;
            while (field < end && *field != ' ' && *field != '\t')
                field++;
            while (field < end && (*field == ' ' || *field == '\t'))
                field++;
            const char *permissions = field;
            while (field < end && *field != ' ' && *field != '\t')
                field++;
            const char *permissions_end = field;
            while (field < end && (*field == ' ' || *field == '\t'))
                field++;

            if (permissions_end - permissions >= 3 &&
                reserve_maps(parsed, parsed->map_count + 1) == 0) {
                hex_map_t *map = &parsed->maps[parsed->map_count++];
                *map = (hex_map_t){0};
                map->start = start;
                map->end = finish;
                copy_field(map->permissions, sizeof(map->permissions),
                           permissions, permissions + 3);
                copy_field(map->description, sizeof(map->description),
                           field, end);
            } else if (permissions_end - permissions >= 3) {
                return -1;
            }
        }
        line = *end == '\n' ? end + 1 : end;
    }
    return 0;
}

int hex_buffer_reload_maps(hex_buffer_t *buffer)
{
    if (buffer->maps_path == NULL)
        return -1;
    int64_t handle = os64_open(buffer->maps_path, "r");
    if (handle < 0)
        return -1;
    int64_t length = os64_seek((int32_t)handle, 0, OS64_SEEK_END);
    if (length < 0 || (uint64_t)length > HEX_MAPS_MAX_BYTES ||
        os64_seek((int32_t)handle, 0, OS64_SEEK_SET) != 0) {
        os64_close((int32_t)handle);
        return -1;
    }

    char *text = os64_malloc((size_t)length + 1);
    if (text == NULL) {
        os64_close((int32_t)handle);
        return -1;
    }
    size_t count = 0;
    while (count < (size_t)length) {
        int64_t n = os64_read((int32_t)handle, text + count,
                              (size_t)length - count);
        if (n <= 0)
            break;
        count += (size_t)n;
    }
    os64_close((int32_t)handle);
    text[count] = '\0';

    hex_buffer_t parsed = {0};
    int result = count == (size_t)length ? parse_maps_text(&parsed, text) : -1;
    os64_free(text);
    if (result < 0 || parsed.map_count == 0) {
        os64_free(parsed.maps);
        return -1;
    }

    os64_free(buffer->maps);
    buffer->maps = parsed.maps;
    buffer->map_count = parsed.map_count;
    buffer->map_capacity = parsed.map_capacity;
    buffer->window_loaded = false;
    return (int)buffer->map_count;
}

int hex_buffer_attach_proc_maps(hex_buffer_t *buffer, const char *mem_path)
{
    char *path = proc_maps_path(mem_path);
    if (path == NULL)
        return -1;
    os64_free(buffer->maps_path);
    buffer->maps_path = path;
    return hex_buffer_reload_maps(buffer);
}

const hex_map_t *hex_buffer_map_at(const hex_buffer_t *buffer, uint64_t offset)
{
    for (size_t i = 0; i < buffer->map_count; i++)
        if (offset >= buffer->maps[i].start && offset < buffer->maps[i].end)
            return &buffer->maps[i];
    return NULL;
}

const hex_map_t *hex_buffer_next_map(const hex_buffer_t *buffer, uint64_t offset)
{
    for (size_t i = 0; i < buffer->map_count; i++)
        if (buffer->maps[i].start > offset)
            return &buffer->maps[i];
    return NULL;
}

const hex_map_t *hex_buffer_previous_map(const hex_buffer_t *buffer,
                                         uint64_t offset)
{
    const hex_map_t *previous = NULL;
    for (size_t i = 0; i < buffer->map_count; i++) {
        if (buffer->maps[i].start >= offset)
            break;
        previous = &buffer->maps[i];
    }
    return previous;
}

static int load_window(hex_buffer_t *buffer, uint64_t offset)
{
    uint64_t start = offset & ~((uint64_t)HEX_BUFFER_WINDOW_SIZE - 1u);
    if (start > INT64_MAX)
        return -1;

    int64_t position = os64_seek(buffer->handle, (int64_t)start,
                                 OS64_SEEK_SET);
    if (position < 0 || (!buffer->address_space && position != (int64_t)start))
        return -1;

    size_t count = 0;
    for (size_t i = 0; i < sizeof(buffer->window_valid); i++)
        buffer->window_valid[i] = 0;
    while (count < sizeof(buffer->window)) {
        int64_t n = os64_read(buffer->handle, buffer->window + count,
                              sizeof(buffer->window) - count);
        if (n <= 0)
            break;
        for (size_t i = 0; i < (size_t)n; i++)
            buffer->window_valid[count + i] = 1;
        count += (size_t)n;
    }
    buffer->window_start = start;
    // An address-space window always represents the whole page. Bytes the
    // provider could not return remain invalid and render as ??; a finite
    // file stops at the actual short read as before.
    buffer->window_count = buffer->address_space
        ? sizeof(buffer->window) : count;
    buffer->window_loaded = true;
    return 0;
}

int hex_buffer_open(hex_buffer_t *buffer, const char *path, bool read_only,
                    bool address_space)
{
    *buffer = (hex_buffer_t){0};
    buffer->handle = -1;
    int64_t handle = os64_open(path, read_only ? "r" : "u");
    if (handle < 0)
        return -1;
    buffer->handle = (int32_t)handle;
    buffer->read_only = read_only;
    buffer->address_space = address_space;

    if (address_space) {
        // os64_seek takes an int64_t. Consequently the addressable byte domain
        // is 0..INT64_MAX inclusive, expressed here as its one-past bound.
        buffer->size = (uint64_t)INT64_MAX + 1u;
        return 0;
    }

    int64_t size = os64_seek(buffer->handle, 0, OS64_SEEK_END);
    if (size < 0) {
        os64_close(buffer->handle);
        buffer->handle = -1;
        return -1;
    }
    buffer->size = (uint64_t)size;
    return 0;
}

void hex_buffer_close(hex_buffer_t *buffer)
{
    if (buffer->handle >= 0)
        os64_close(buffer->handle);
    os64_free(buffer->patches);
    os64_free(buffer->undo);
    os64_free(buffer->maps);
    os64_free(buffer->maps_path);
    *buffer = (hex_buffer_t){0};
    buffer->handle = -1;
}

void hex_buffer_refresh(hex_buffer_t *buffer)
{
    buffer->window_loaded = false;
}

int hex_buffer_get(hex_buffer_t *buffer, uint64_t offset, uint8_t *value)
{
    if (offset >= buffer->size || value == NULL)
        return HEX_BUFFER_ERROR;

    bool found;
    size_t patch = patch_lower_bound(buffer, offset, &found);
    if (found) {
        *value = buffer->patches[patch].value;
        return HEX_BUFFER_OK;
    }

    if (!buffer->window_loaded || offset < buffer->window_start ||
        offset - buffer->window_start >= buffer->window_count) {
        if (load_window(buffer, offset) < 0)
            return -1;
    }
    uint64_t index = offset - buffer->window_start;
    if (index >= buffer->window_count)
        return buffer->address_space
            ? HEX_BUFFER_UNAVAILABLE : HEX_BUFFER_ERROR;
    if (!buffer->window_valid[index])
        return HEX_BUFFER_UNAVAILABLE;
    *value = buffer->window[index];
    return HEX_BUFFER_OK;
}

static int set_patch(hex_buffer_t *buffer, uint64_t offset, uint8_t original,
                     uint8_t value)
{
    bool found;
    size_t at = patch_lower_bound(buffer, offset, &found);
    if (value == original) {
        if (found) {
            os64_memmove(&buffer->patches[at], &buffer->patches[at + 1],
                         (buffer->patch_count - at - 1) * sizeof(*buffer->patches));
            buffer->patch_count--;
        }
        return 0;
    }
    if (found) {
        buffer->patches[at].value = value;
        return 0;
    }
    if (reserve_patches(buffer, buffer->patch_count + 1) < 0)
        return -1;
    os64_memmove(&buffer->patches[at + 1], &buffer->patches[at],
                 (buffer->patch_count - at) * sizeof(*buffer->patches));
    buffer->patches[at] = (hex_patch_t){offset, original, value};
    buffer->patch_count++;
    return 0;
}

int hex_buffer_edit(hex_buffer_t *buffer, uint64_t offset, uint8_t value)
{
    if (buffer->read_only || offset >= buffer->size)
        return -1;
    uint8_t before;
    // != OK, not < 0: HEX_BUFFER_UNAVAILABLE is +1, and an edit of a byte the
    // provider could not show us would carry an uninitialized "before" into
    // the undo log (review, 2026-08-22 — the /proc/<id>/mem path hits this).
    if (hex_buffer_get(buffer, offset, &before) != HEX_BUFFER_OK)
        return -1;
    if (before == value)
        return 0;

    bool found;
    size_t at = patch_lower_bound(buffer, offset, &found);
    uint8_t original = found ? buffer->patches[at].original : before;
    if (reserve_undo(buffer, buffer->undo_count + 1) < 0 ||
        (!found && value != original &&
         reserve_patches(buffer, buffer->patch_count + 1) < 0))
        return -1;

    buffer->undo[buffer->undo_count++] =
        (hex_undo_t){offset, before, value, original};
    return set_patch(buffer, offset, original, value);
}

int hex_buffer_undo(hex_buffer_t *buffer)
{
    if (buffer->undo_count == 0)
        return 0;
    hex_undo_t change = buffer->undo[--buffer->undo_count];
    if (set_patch(buffer, change.offset, change.original, change.before) < 0) {
        buffer->undo[buffer->undo_count++] = change;
        return -1;
    }
    return 1;
}

static int write_all(int32_t handle, const uint8_t *data, size_t count)
{
    size_t written = 0;
    while (written < count) {
        int64_t n = os64_write(handle, data + written, count - written);
        if (n <= 0)
            return -1;
        written += (size_t)n;
    }
    return 0;
}

int hex_buffer_save(hex_buffer_t *buffer)
{
    if (buffer->read_only)
        return -1;

    uint8_t bytes[HEX_SAVE_CHUNK];
    size_t patch = 0;
    while (patch < buffer->patch_count) {
        uint64_t start = buffer->patches[patch].offset;
        size_t count = 0;
        while (patch + count < buffer->patch_count && count < sizeof(bytes) &&
               buffer->patches[patch + count].offset == start + count) {
            bytes[count] = buffer->patches[patch + count].value;
            count++;
        }
        if (start > INT64_MAX ||
            os64_seek(buffer->handle, (int64_t)start, OS64_SEEK_SET) !=
            (int64_t)start || write_all(buffer->handle, bytes, count) < 0)
            return -1;
        patch += count;
    }
    if (os64_sync(buffer->handle) < 0)
        return -1;

    buffer->patch_count = 0;
    buffer->undo_count = 0;
    buffer->window_loaded = false;
    return 0;
}
