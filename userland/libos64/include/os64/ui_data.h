#ifndef OS64_UI_DATA_H
#define OS64_UI_DATA_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "os64/ui_result.h"
#define OS64_UI_DATA_ROWS 100000u
#define OS64_UI_DATA_COLUMNS 16u
#define OS64_UI_DATA_TEXT 4096u
#define OS64_UI_DATA_BYTES (64u * 1024u * 1024u)
#define OS64_UI_DATA_VIEW_BYTES (16u * 1024u * 1024u)
typedef uint64_t os64_ui_data_id_t;
typedef struct { const char *bytes; size_t length; } os64_ui_data_text_t;
typedef enum { OS64_UI_DATA_TEXT_KEY, OS64_UI_DATA_UNSIGNED, OS64_UI_DATA_SIGNED } os64_ui_data_type_t;
typedef struct {
    bool present;
    union { os64_ui_data_text_t text; uint64_t u; int64_t i; } value;
    os64_ui_data_type_t type;
} os64_ui_data_key_t;
typedef struct { os64_ui_data_text_t display; os64_ui_data_key_t key; } os64_ui_data_cell_t;
typedef struct {
    uint32_t id;
    os64_ui_data_text_t title;
    os64_ui_data_type_t type;
    uint32_t width;
    bool right, sortable, required, search;
} os64_ui_data_column_t;
typedef struct {
    os64_ui_data_id_t id;
    uint32_t group;
    os64_ui_data_text_t search, icon;
    const os64_ui_data_cell_t *cells;
} os64_ui_data_row_t;
typedef struct os64_ui_data os64_ui_data_t;
typedef struct os64_ui_data_projection os64_ui_data_projection_t;
typedef struct { uint32_t column; bool descending; } os64_ui_data_sort_t;
/* Copied immutable inputs. Text uses explicit lengths and becomes internally
 * terminated; getters borrow until the owning reference is released. */
os64_ui_status_code_t os64_ui_data_create(uint64_t domain,
    const os64_ui_data_column_t *, size_t columns,
    const os64_ui_data_row_t *, size_t rows, os64_ui_data_t **out);
void os64_ui_data_retain(os64_ui_data_t *);
void os64_ui_data_release(os64_ui_data_t *);
uint64_t os64_ui_data_domain(const os64_ui_data_t *);
size_t os64_ui_data_count(const os64_ui_data_t *);
size_t os64_ui_data_columns(const os64_ui_data_t *);
size_t os64_ui_data_bytes(const os64_ui_data_t *);
const os64_ui_data_column_t *os64_ui_data_column(const os64_ui_data_t *, size_t);
const os64_ui_data_row_t *os64_ui_data_row(const os64_ui_data_t *, os64_ui_data_id_t);
const os64_ui_data_row_t *os64_ui_data_row_at(const os64_ui_data_t *, size_t);
bool os64_ui_data_schema_equal(const os64_ui_data_t *, const os64_ui_data_t *);
os64_ui_status_code_t os64_ui_data_projection_create(os64_ui_data_t *,
    const os64_ui_data_id_t *, size_t, os64_ui_data_projection_t **out);
os64_ui_status_code_t os64_ui_data_projection_identity(os64_ui_data_t *, os64_ui_data_projection_t **out);
os64_ui_status_code_t os64_ui_data_projection_sort(const os64_ui_data_projection_t *,
    os64_ui_data_sort_t, os64_ui_data_projection_t **out);
void os64_ui_data_projection_retain(os64_ui_data_projection_t *);
void os64_ui_data_projection_release(os64_ui_data_projection_t *);
os64_ui_data_t *os64_ui_data_projection_snapshot(const os64_ui_data_projection_t *);
size_t os64_ui_data_projection_count(const os64_ui_data_projection_t *);
size_t os64_ui_data_projection_bytes(const os64_ui_data_projection_t *);
os64_ui_data_sort_t os64_ui_data_projection_order(const os64_ui_data_projection_t *);
os64_ui_data_id_t os64_ui_data_projection_id(const os64_ui_data_projection_t *, size_t);
bool os64_ui_data_projection_position(const os64_ui_data_projection_t *, os64_ui_data_id_t, size_t *);

typedef struct { void *state; } os64_ui_selection_t;
typedef enum { OS64_UI_DATA_PRESERVE, OS64_UI_DATA_RESET } os64_ui_data_bind_t;
typedef enum { OS64_UI_SELECTION_CHOOSE, OS64_UI_SELECTION_FOCUS,
    OS64_UI_SELECTION_TOGGLE, OS64_UI_SELECTION_RANGE,
    OS64_UI_SELECTION_UNION, OS64_UI_SELECTION_ALL, OS64_UI_SELECTION_CLEAR,
    OS64_UI_SELECTION_CONTEXT } os64_ui_selection_command_t;
/* Initialize fresh storage; dispose before reinitializing. */
void os64_ui_selection_init(os64_ui_selection_t *);
os64_ui_status_code_t os64_ui_selection_dispose(os64_ui_selection_t *);
os64_ui_status_code_t os64_ui_selection_bind(os64_ui_selection_t *, os64_ui_data_projection_t *, os64_ui_data_bind_t);
os64_ui_status_code_t os64_ui_selection_command(os64_ui_selection_t *, os64_ui_selection_command_t, os64_ui_data_id_t);
os64_ui_data_projection_t *os64_ui_selection_projection(const os64_ui_selection_t *);
os64_ui_data_id_t os64_ui_selection_focus(const os64_ui_selection_t *);
os64_ui_data_id_t os64_ui_selection_anchor(const os64_ui_selection_t *);
size_t os64_ui_selection_count(const os64_ui_selection_t *);
size_t os64_ui_selection_bytes(const os64_ui_selection_t *);
uint64_t os64_ui_selection_revision(const os64_ui_selection_t *);
bool os64_ui_selection_contains(const os64_ui_selection_t *, os64_ui_data_id_t);
/* Enumeration in projection order; a revision mismatch returns STALE. cursor
 * starts at zero. EMPTY marks the end and clears out. */
os64_ui_status_code_t os64_ui_selection_next(const os64_ui_selection_t *, uint64_t revision,
    size_t *cursor, os64_ui_data_id_t *out);
#endif
