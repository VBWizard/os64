#ifndef OS64_DIRECTORY_H
#define OS64_DIRECTORY_H
#include "os64/ui_data.h"
#include "os64/dirent.h"
#define OS64_DIRECTORY_PATH 256u
#define OS64_DIRECTORY_ROWS 10000u
#define OS64_DIRECTORY_FILTER 128u
#define OS64_DIRECTORY_PLACES 32u
typedef struct os64_directory os64_directory_t;
typedef struct { os64_ui_data_id_t id; os64_dirent_t entry; } os64_directory_entry_t;
typedef struct { char label[80], path[OS64_DIRECTORY_PATH]; } os64_directory_place_t;
/* Labels and paths are copied into this result. The first favorite_count
 * items are Root and Home, except that Home is omitted when it resolves to /. */
typedef struct { size_t count, favorite_count; bool unavailable, incomplete; os64_directory_place_t items[OS64_DIRECTORY_PLACES]; } os64_directory_places_t;
os64_ui_status_code_t os64_directory_path(const char *base,const char *input,char out[OS64_DIRECTORY_PATH]);
os64_ui_status_code_t os64_directory_join(const char *base,const char *name,char out[OS64_DIRECTORY_PATH]);
/* Resolve HOME lexically; missing or unusable HOME selects /home without
 * testing whether that directory is mounted or readable. */
os64_ui_status_code_t os64_directory_home(char out[OS64_DIRECTORY_PATH]);
/* Previous raw listing is immutable and may be shared across a worker handoff.
 * E3 objects produced by data/project have serialized, separate lifetimes. */
os64_ui_status_code_t os64_directory_read(const char *path,uint64_t domain,const os64_directory_t *previous,
    bool (*cancelled)(void *),void *cancel_user,os64_directory_t **out,int64_t *io_error);
void os64_directory_retain(os64_directory_t *);
void os64_directory_release(os64_directory_t *);
const char *os64_directory_location(const os64_directory_t *);
uint64_t os64_directory_domain(const os64_directory_t *);
size_t os64_directory_count(const os64_directory_t *);
size_t os64_directory_bytes(const os64_directory_t *);
bool os64_directory_incomplete(const os64_directory_t *);
const os64_directory_entry_t *os64_directory_entry(const os64_directory_t *,os64_ui_data_id_t);
const os64_directory_entry_t *os64_directory_at(const os64_directory_t *,size_t);
os64_ui_status_code_t os64_directory_data(const os64_directory_t *,os64_ui_data_t **out);
os64_ui_status_code_t os64_directory_project(const os64_directory_t *,os64_ui_data_t *,const char *filter,
    bool hidden,os64_ui_data_sort_t,os64_ui_data_projection_t **out);
/* Call off the UI thread. Does not create mounts or fail directory browsing. */
void os64_directory_places(os64_directory_places_t *out);
#endif
