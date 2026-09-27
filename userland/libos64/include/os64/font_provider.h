#ifndef OS64_FONT_PROVIDER_H
#define OS64_FONT_PROVIDER_H
#include <stdbool.h>

/* F2.5's in-memory provider boundary. FONT_PROVIDER.md defines ownership,
 * row geometry, F5 resolution responsibilities and consumer transactions. */
#include "os64/text.h"

#define OS64_FONT_CONFIG_FALLBACK_MAX 2u
#define OS64_FONT_ROLE_FONTS_MAX 4u

typedef enum {
    OS64_FONT_ROLE_UI = 0,
    OS64_FONT_ROLE_TERMINAL,
    OS64_FONT_ROLE_DOCUMENT,
    OS64_FONT_ROLE_COUNT
} os64_font_role_t;

typedef enum {
    OS64_FONT_SOURCE_BUILTIN = 0,
    OS64_FONT_SOURCE_OUTLINE
} os64_font_source_kind_t;

typedef struct {
    os64_font_source_kind_t kind;
    const uint8_t *bytes; /* borrowed for prepare; builtin requires NULL/0 */
    size_t length;
} os64_font_source_t;

typedef struct {
    uint32_t pixel_height; /* zero selects 16; otherwise 8..96 */
    os64_font_source_t primary;
    os64_font_source_t fallbacks[OS64_FONT_CONFIG_FALLBACK_MAX];
    size_t fallback_count;
} os64_font_role_spec_t;

typedef struct os64_font_set os64_font_set_t;
typedef struct {
    os64_text_context_t *text;
    os64_text_font_t *const *fonts; /* borrowed ordered F2 handles */
    size_t font_count;
    os64_font_face_info_t primary;
    uint64_t identity; /* set identity, unique within text; not session generation */
    int32_t baseline_px, row_height_px;
    int32_t cell_width_px; /* terminal only; zero for the other roles */
} os64_font_role_view_t;

/* F2 options and lifetime rules, with NULL backend selecting the production
 * FreeType backend. Memory callbacks remain required. Applications can use
 * this entry without directly linking/calling the backend library. */
os64_font_status_t os64_font_context_create(const os64_text_options_t *,
    os64_text_context_t **out);

/* NULL specs or three zero-initialized specs select builtin/16 for all roles.
 * Open all roles atomically, copy outline bytes, append implicit builtin
 * fallback where absent, and validate the terminal primary against ASCII.
 * No I/O, configuration resolution or live adoption occurs here. Failure
 * clears out; existing sets/runs remain usable. Storage uses the F2 budget.
 * Output storage must not alias inputs. */
os64_font_status_t os64_font_set_prepare(os64_text_context_t *,
    const os64_font_role_spec_t specs[OS64_FONT_ROLE_COUNT], os64_font_set_t **out);
typedef struct {
    os64_font_role_t role; /* ROLE_COUNT when no particular role failed */
    size_t source_index; /* 0 primary, 1/2 configured fallback, 3 implicit builtin;
                         * SIZE_MAX when no particular source failed */
} os64_font_problem_t;
/* F5 uses this form to associate a failed source with its configuration line.
 * Success clears problem to ROLE_COUNT/SIZE_MAX; problem may be NULL. */
os64_font_status_t os64_font_set_prepare_checked(os64_text_context_t *,
    const os64_font_role_spec_t specs[OS64_FONT_ROLE_COUNT], os64_font_set_t **out,
    os64_font_problem_t *problem);
os64_font_status_t os64_font_set_retain(os64_font_set_t *);
void os64_font_set_release(os64_font_set_t *); /* NULL is a no-op */
os64_font_status_t os64_font_set_view(const os64_font_set_t *, os64_font_role_t,
    os64_font_role_view_t *out);

typedef enum {
    OS64_FONT_FAMILY_SERIF = 0,
    OS64_FONT_FAMILY_SANS,
    OS64_FONT_FAMILY_MONO,
    OS64_FONT_FAMILY_COUNT
} os64_font_family_t;
#define OS64_FONT_FAMILY_STYLES 4u /* regular, bold, italic, bolditalic */
#define OS64_FONT_FAMILY_CACHE_MAX 32u
typedef struct {
    const char *name; /* borrowed bytes; need not be NUL-terminated */
    uint32_t len;
} os64_font_family_name_t;
typedef struct {
    const os64_font_family_name_t *names; /* page order; matching uses generic */
    uint32_t count;
    os64_font_family_t generic; /* the generic tail, separate from named faces */
} os64_font_family_list_t;
typedef struct {
    os64_font_source_t styles[OS64_FONT_FAMILY_STYLES];
    os64_font_source_t fallbacks[OS64_FONT_CONFIG_FALLBACK_MAX];
    size_t fallback_count;
} os64_font_family_spec_t;
typedef struct os64_font_family_cache os64_font_family_cache_t;
/* Copy resolved sources into the context's budget, deduplicating identical
 * input spans. No I/O. Keeps context destruction BUSY even before first open.
 * Zero specs select bitmap faces; outline styles accept 1..FONT_PIXEL_MAX.
 * On failure *out is NULL; error indices identify a family and a source
 * (styles 0..3, configured fallbacks 4..5), or COUNT/SIZE_MAX for storage. */
os64_font_status_t os64_font_family_cache_create(os64_text_context_t *,
    const os64_font_family_spec_t [OS64_FONT_FAMILY_COUNT],
    os64_font_family_cache_t **out, os64_font_family_t *family, size_t *source);
void os64_font_family_cache_destroy(os64_font_family_cache_t *);
/* Serialized with the text context. The returned list is borrowed until the
 * next open on this cache (including a failed open), or cache destruction.
 * Lay out runs before asking again: runs retain fonts independently of LRU
 * eviction. At most 32 entries are held; a LIMIT evicts oldest entries and
 * retries. Fonts retained by runs can still exhaust the engine's face limit.
 * Failure clears out. Named families are carried but not matched. */
os64_font_status_t os64_font_family_open(os64_font_family_cache_t *,
    const os64_font_family_list_t *, bool bold, bool italic,
    uint32_t pixel_height, os64_font_role_view_t *out);

/* All set/context/run calls need caller serialization. A view is borrowed until
 * set release. Do not release its font handles. A retained set keeps its context
 * BUSY; a run can outlive a set, but must be released before context destruction.
 * Source/path deduplication and input diagnostics belong to F5. */
#endif
