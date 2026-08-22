// Host-side test for the presentation-independent hex editor engine.
//
// Build and run:
//   gcc -Wall -Wextra -Werror -I userland/libos64/include -I abi/include
//       -I userland/common userland/common/hex_buffer.c
//       tools/test_hex_buffer_host.c -o /tmp/os64_hex_buffer_test
//   /tmp/os64_hex_buffer_test

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "hex_buffer.h"
#include "os64/syscall_numbers.h"

#define FAKE_SIZE UINT64_C(0x100000100)

typedef struct {
    uint64_t offset;
    uint8_t value;
} stored_byte_t;

static uint64_t fake_position;
static uint64_t fake_maps_position;
static stored_byte_t stored[32];
static size_t stored_count;
static int sync_count;
static int failures;
static bool fake_sparse;

static const char fake_maps_text[] =
    "0000000000001000-0000000000002000\tr--\tprivate\n"
    "0000000000002000-0000000000003000\trw-\tprivate,cow\n"
    "0000000000005000-0000000000006000\tr-x\tprivate,lib\n";

#define EXPECT(condition) do {                                                \
    if (!(condition)) {                                                       \
        printf("FAIL line %d: %s\n", __LINE__, #condition);                  \
        failures++;                                                           \
    }                                                                         \
} while (0)

static uint8_t generated_byte(uint64_t offset)
{
    for (size_t i = 0; i < stored_count; i++)
        if (stored[i].offset == offset)
            return stored[i].value;
    return (uint8_t)(offset ^ (offset >> 32));
}

int64_t os64_open(const char *path, const char *mode)
{
    if (strcmp(path, "/proc/42/maps") == 0) {
        EXPECT(strcmp(mode, "r") == 0);
        fake_maps_position = 0;
        return 4;
    }
    fake_sparse = strcmp(path, "sparse.mem") == 0 ||
                  strcmp(path, "/proc/42/mem") == 0;
    EXPECT(strcmp(mode, "u") == 0 || strcmp(mode, "r") == 0);
    fake_position = 0;
    return 3;
}

int64_t os64_close(int32_t handle)
{
    EXPECT(handle == 3 || handle == 4);
    return 0;
}

int64_t os64_seek(int32_t handle, int64_t offset, int32_t whence)
{
    EXPECT(handle == 3 || handle == 4);
    if (handle == 4) {
        int64_t base = whence == OS64_SEEK_SET ? 0 :
                       whence == OS64_SEEK_CUR ? (int64_t)fake_maps_position :
                       whence == OS64_SEEK_END
                           ? (int64_t)(sizeof(fake_maps_text) - 1) : -1;
        if (base < 0 || (offset < 0 && base < -offset))
            return -1;
        fake_maps_position = (uint64_t)(base + offset);
        return (int64_t)fake_maps_position;
    }
    if (fake_sparse && whence == OS64_SEEK_END)
        return -1;
    int64_t base = whence == OS64_SEEK_SET ? 0 :
                   whence == OS64_SEEK_CUR ? (int64_t)fake_position :
                   whence == OS64_SEEK_END ? (int64_t)FAKE_SIZE : -1;
    if (base < 0 || (offset < 0 && base < -offset))
        return -1;
    fake_position = (uint64_t)(base + offset);
    return (int64_t)fake_position;
}

int64_t os64_read(int32_t handle, void *data, size_t count)
{
    EXPECT(handle == 3 || handle == 4);
    if (handle == 4) {
        size_t length = sizeof(fake_maps_text) - 1;
        if (fake_maps_position >= length)
            return 0;
        if (count > length - fake_maps_position)
            count = length - fake_maps_position;
        memcpy(data, fake_maps_text + fake_maps_position, count);
        fake_maps_position += count;
        return (int64_t)count;
    }
    if (fake_sparse) {
        uint64_t page = fake_position & ~UINT64_C(0xfff);
        if (page != UINT64_C(0x2000))
            return -1;
        size_t remaining = (size_t)(UINT64_C(0x3000) - fake_position);
        if (count > remaining)
            count = remaining;
    }
    if (fake_position >= FAKE_SIZE)
        return 0;
    if (count > FAKE_SIZE - fake_position)
        count = (size_t)(FAKE_SIZE - fake_position);
    uint8_t *bytes = data;
    for (size_t i = 0; i < count; i++)
        bytes[i] = generated_byte(fake_position + i);
    fake_position += count;
    return (int64_t)count;
}

int64_t os64_write(int32_t handle, const void *data, size_t count)
{
    EXPECT(handle == 3);
    const uint8_t *bytes = data;
    for (size_t i = 0; i < count; i++) {
        size_t slot = stored_count;
        for (size_t j = 0; j < stored_count; j++)
            if (stored[j].offset == fake_position + i)
                slot = j;
        if (slot == stored_count)
            stored_count++;
        stored[slot] = (stored_byte_t){fake_position + i, bytes[i]};
    }
    fake_position += count;
    return (int64_t)count;
}

int64_t os64_sync(int32_t handle)
{
    EXPECT(handle == 3);
    sync_count++;
    return 0;
}

void *os64_malloc(size_t size) { return malloc(size); }
void *os64_realloc(void *pointer, size_t size) { return realloc(pointer, size); }
void os64_free(void *pointer) { free(pointer); }
size_t os64_strlen(const char *text) { return strlen(text); }
void *os64_memmove(void *to, const void *from, size_t count)
{
    return memmove(to, from, count);
}

bool os64_parse_range(const char *text, uint64_t *lo, uint64_t *hi)
{
    char *end;
    uint64_t first = strtoull(text, &end, 16);
    if (end == text || *end != '-')
        return false;
    const char *second_text = end + 1;
    uint64_t second = strtoull(second_text, &end, 16);
    if (end == second_text)
        return false;
    if (lo != NULL) *lo = first;
    if (hi != NULL) *hi = second;
    return true;
}

int main(void)
{
    hex_buffer_t buffer;
    EXPECT(hex_buffer_open(&buffer, "huge.bin", false, false) == 0);
    EXPECT(buffer.size == FAKE_SIZE);

    uint64_t high = UINT64_C(0x100000020);
    uint8_t original = generated_byte(high);
    uint8_t value = 0;
    EXPECT(hex_buffer_get(&buffer, high, &value) == 0);
    EXPECT(value == original);

    EXPECT(hex_buffer_edit(&buffer, high + 5, 0xa5) == 0);
    EXPECT(hex_buffer_edit(&buffer, high, 0x11) == 0);
    EXPECT(hex_buffer_edit(&buffer, high + 1, 0x22) == 0);
    EXPECT(buffer.patch_count == 3);
    EXPECT(hex_buffer_get(&buffer, high, &value) == 0 && value == 0x11);

    EXPECT(hex_buffer_undo(&buffer) == 1);
    EXPECT(buffer.patch_count == 2);
    EXPECT(hex_buffer_get(&buffer, high + 1, &value) == 0);
    EXPECT(value == generated_byte(high + 1));
    EXPECT(hex_buffer_edit(&buffer, high + 1, 0x33) == 0);

    EXPECT(hex_buffer_save(&buffer) == 0);
    EXPECT(sync_count == 1);
    EXPECT(!hex_buffer_modified(&buffer));
    EXPECT(generated_byte(high) == 0x11);
    EXPECT(generated_byte(high + 1) == 0x33);
    EXPECT(generated_byte(high + 5) == 0xa5);
    EXPECT(hex_buffer_get(&buffer, high, &value) == 0 && value == 0x11);
    hex_buffer_close(&buffer);

    EXPECT(hex_buffer_open(&buffer, "huge.bin", true, false) == 0);
    EXPECT(hex_buffer_edit(&buffer, high, 0xff) < 0);
    EXPECT(hex_buffer_save(&buffer) < 0);
    hex_buffer_close(&buffer);

    EXPECT(hex_buffer_open(&buffer, "sparse.mem", true, true) == 0);
    EXPECT(buffer.address_space);
    EXPECT(buffer.size == (uint64_t)INT64_MAX + 1u);
    EXPECT(hex_buffer_get(&buffer, UINT64_C(0x1000), &value) ==
           HEX_BUFFER_UNAVAILABLE);
    EXPECT(hex_buffer_get(&buffer, UINT64_C(0x202a), &value) == HEX_BUFFER_OK);
    EXPECT(value == generated_byte(UINT64_C(0x202a)));
    EXPECT(hex_buffer_get(&buffer, UINT64_C(0x3000), &value) ==
           HEX_BUFFER_UNAVAILABLE);
    hex_buffer_close(&buffer);

    EXPECT(hex_buffer_is_proc_mem_path("/proc/42/mem"));
    EXPECT(!hex_buffer_is_proc_mem_path("/proc/42/maps"));
    EXPECT(!hex_buffer_is_proc_mem_path("/proc/nope/mem"));
    EXPECT(hex_buffer_open(&buffer, "/proc/42/mem", true, true) == 0);
    EXPECT(hex_buffer_attach_proc_maps(&buffer, "/proc/42/mem") == 3);
    EXPECT(buffer.map_count == 3);
    const hex_map_t *map = hex_buffer_map_at(&buffer, UINT64_C(0x202a));
    EXPECT(map != NULL && map->start == UINT64_C(0x2000));
    EXPECT(strcmp(map->permissions, "rw-") == 0);
    EXPECT(strcmp(map->description, "private,cow") == 0);
    map = hex_buffer_next_map(&buffer, UINT64_C(0x202a));
    EXPECT(map != NULL && map->start == UINT64_C(0x5000));
    map = hex_buffer_previous_map(&buffer, UINT64_C(0x2000));
    EXPECT(map != NULL && map->start == UINT64_C(0x1000));
    EXPECT(hex_buffer_reload_maps(&buffer) == 3);
    EXPECT(hex_buffer_get(&buffer, UINT64_C(0x202a), &value) == HEX_BUFFER_OK);
    hex_buffer_close(&buffer);

    if (failures == 0)
        puts("hex_buffer host test: PASS");
    return failures == 0 ? 0 : 1;
}
