#ifndef OS64_HEX_BUFFER_H
#define OS64_HEX_BUFFER_H

// Windowed byte editor engine. It knows files, offsets, dirty bytes and undo;
// it knows nothing about terminals or pixels. A future GUI gets the same
// editing semantics by putting a different view over this object.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HEX_BUFFER_WINDOW_SIZE 4096u

enum {
    HEX_BUFFER_OK = 0,
    HEX_BUFFER_UNAVAILABLE = 1,
    HEX_BUFFER_ERROR = -1
};

typedef struct {
    uint64_t offset;
    uint8_t original;
    uint8_t value;
} hex_patch_t;

typedef struct {
    uint64_t offset;
    uint8_t before;
    uint8_t after;
    uint8_t original;
} hex_undo_t;

typedef struct {
    uint64_t start;
    uint64_t end;       // exclusive, exactly as /proc/<pid>/maps prints it
    char permissions[4];
    char description[32];
} hex_map_t;

typedef struct {
    int32_t handle;
    bool read_only;
    bool address_space;
    bool window_loaded;
    uint64_t size;
    uint64_t window_start;
    size_t window_count;
    uint8_t window[HEX_BUFFER_WINDOW_SIZE];
    uint8_t window_valid[HEX_BUFFER_WINDOW_SIZE];
    hex_patch_t *patches;
    size_t patch_count;
    size_t patch_capacity;
    hex_undo_t *undo;
    size_t undo_count;
    size_t undo_capacity;
    hex_map_t *maps;
    size_t map_count;
    size_t map_capacity;
    char *maps_path;
} hex_buffer_t;

int hex_buffer_open(hex_buffer_t *buffer, const char *path, bool read_only,
                    bool address_space);
void hex_buffer_close(hex_buffer_t *buffer);
int hex_buffer_get(hex_buffer_t *buffer, uint64_t offset, uint8_t *value);
int hex_buffer_edit(hex_buffer_t *buffer, uint64_t offset, uint8_t value);
int hex_buffer_undo(hex_buffer_t *buffer);
int hex_buffer_save(hex_buffer_t *buffer);
void hex_buffer_refresh(hex_buffer_t *buffer);

bool hex_buffer_is_proc_mem_path(const char *path);
int hex_buffer_attach_proc_maps(hex_buffer_t *buffer, const char *mem_path);
int hex_buffer_reload_maps(hex_buffer_t *buffer);
const hex_map_t *hex_buffer_map_at(const hex_buffer_t *buffer, uint64_t offset);
const hex_map_t *hex_buffer_next_map(const hex_buffer_t *buffer, uint64_t offset);
const hex_map_t *hex_buffer_previous_map(const hex_buffer_t *buffer,
                                         uint64_t offset);

static inline bool hex_buffer_modified(const hex_buffer_t *buffer)
{
    return buffer->patch_count != 0;
}

#endif
