#ifndef OS64_FONT_CONFIG_H
#define OS64_FONT_CONFIG_H

#include <stdbool.h>
#include "os64/font_provider.h"

#define OS64_FONT_PATH_CAP 256u
#define OS64_FONT_CONFIG_BYTES_MAX 8191u
#define OS64_FONT_SOURCE_BYTES_MAX (64u * 1024u * 1024u)

typedef struct {
    char face[3][OS64_FONT_PATH_CAP]; /* primary, fallback.1, fallback.2 */
    uint32_t size;
    size_t source_line[3], size_line; /* zero means a compiled/default value */
} os64_font_config_role_t;
typedef struct {
    /* Empty members inherit: bold/italic -> regular, bolditalic -> bold.
     * An entirely empty family selects the shipped DejaVu family. */
    char face[OS64_FONT_FAMILY_STYLES + OS64_FONT_CONFIG_FALLBACK_MAX][OS64_FONT_PATH_CAP];
    size_t source_line[OS64_FONT_FAMILY_STYLES + OS64_FONT_CONFIG_FALLBACK_MAX];
} os64_font_config_family_t;
/* THE WEB SETTING rides after the roles, in their shape: `web.face` is the
 * face a browser draws a page's form controls in, and `web.size` is the
 * page's default font size (CSS's `medium`). It is not a role — no window's
 * set carries it, so a program that never shows a page never loads it; a
 * browser opens it itself, at the size its page and zoom ask for
 * (os64_font_config_web_prepare). Unset, it is the shipped DejaVu Sans at 16. */
#define OS64_FONT_CONFIG_WEB ((size_t)OS64_FONT_ROLE_COUNT)
#define OS64_FONT_CONFIG_ROLES (OS64_FONT_ROLE_COUNT + 1)
typedef struct {
    os64_font_config_role_t roles[OS64_FONT_CONFIG_ROLES];
    os64_font_config_family_t families[OS64_FONT_FAMILY_COUNT];
    char path[OS64_FONT_PATH_CAP]; /* selected config; empty for no file */
} os64_font_config_t;

typedef enum {
    OS64_FONT_CONFIG_OK = 0,
    OS64_FONT_CONFIG_SYNTAX,
    OS64_FONT_CONFIG_PATH,
    OS64_FONT_CONFIG_SIZE,
    OS64_FONT_CONFIG_DUPLICATE,
    OS64_FONT_CONFIG_IO,
    OS64_FONT_CONFIG_LIMIT,
    OS64_FONT_CONFIG_NO_MEMORY,
    OS64_FONT_CONFIG_FACE,
    OS64_FONT_CONFIG_EXISTS
} os64_font_config_status_t;
typedef struct {
    os64_font_config_status_t status;
    os64_font_status_t font_status;
    size_t line, source;
    os64_font_role_t role; /* ROLE_COUNT when not specific to one role */
    bool web;              /* the Web setting's; role is then ROLE_COUNT */
} os64_font_config_error_t;

void os64_font_config_defaults(os64_font_config_t *);
/* Parse immutable bytes using conf's grammar. Out is unchanged on failure;
 * path must be the absolute location of the selected config. No file I/O. */
os64_font_config_status_t os64_font_config_decode(const char *, size_t,
    const char *path, os64_font_config_t *out, os64_font_config_error_t *);
/* Resolve fonts.conf on the system ladder. No match returns builtin roles and default web-family selections.
 * A selected file that cannot be read is an error. Out is unchanged on error. */
os64_font_config_status_t os64_font_config_read(os64_font_config_t *out,
    os64_font_config_error_t *);
/* Validate a programmatically edited config and prepare all roles; the Web
 * setting is validated, not opened (it is in no role set). No active
 * set or consumer changes. On failure *out is NULL; success owns a new set. */
os64_font_config_status_t os64_font_config_prepare(os64_text_context_t *,
    const os64_font_config_t *, os64_font_set_t **out, os64_font_config_error_t *);
/* Open the Web setting's face (and its fallbacks) at `pixel_height`, clamped
 * to 8..96, as the UI role of a new set whose other roles are builtin — the
 * shape os64_ui_font_app lends. A builtin Web face is opened at 16, the only
 * size it has. Errors name the Web setting's line with `web` set. */
os64_font_config_status_t os64_font_config_web_prepare(os64_text_context_t *,
    const os64_font_config_t *, uint32_t pixel_height, os64_font_set_t **out,
    os64_font_config_error_t *);
/* Resolve family inheritance and prepare an immutable source snapshot. Reads
 * every selected member before publishing a cache, so an unreadable style
 * refuses the candidate even if the page has not requested that style yet.
 * Error line/source identify the family setting; role is ROLE_COUNT.
 * Roles do not load these files, and Workshop's Apply does not adopt them. */
os64_font_config_status_t os64_font_config_family_prepare(os64_text_context_t *,
    const os64_font_config_t *, os64_font_family_cache_t **out,
    os64_font_config_error_t *);
/* Absolute paths (or builtin) make saved choices independent of destination.
 * Returns bytes excluding NUL, or a negative value; no partial output. */
int64_t os64_font_config_encode(const os64_font_config_t *, char *, size_t);
const char *os64_font_config_status_name(os64_font_config_status_t);

#define OS64_FONT_DISCOVERY_MAX 128u
#define OS64_FONT_DISCOVERY_SCAN_MAX 256u
typedef struct {
    char path[OS64_FONT_PATH_CAP];
    os64_font_face_info_t info; /* copied display metadata, never identity */
    os64_font_config_status_t status;
    os64_font_status_t font_status;
} os64_font_catalog_entry_t;
typedef struct {
    char path[OS64_FONT_PATH_CAP], canonical[OS64_FONT_PATH_CAP];
} os64_font_catalog_alias_t;
typedef struct {
    os64_font_catalog_entry_t entries[OS64_FONT_DISCOVERY_MAX];
    size_t count;
    bool limited, directory_unavailable;
    os64_font_catalog_alias_t aliases[OS64_FONT_CONFIG_ROLES * 3];
    size_t alias_count;
} os64_font_catalog_t;
/* Includes builtin, selected files, /home/fonts, /etc/fonts, the installation
 * target and the config's adjacent fonts/ folder. No recursion or suffix filter.
 * Identical valid file bytes share a row, preferring configured paths, then
 * personal files. Different bytes remain separate even with matching names.
 * Selected paths survive unavailable folders; aliases preserve their lookup.
 * Labels may repeat; clients disambiguate with path. Metadata fixed-width is
 * only a hint: prepare validates actual terminal advances at the chosen size.
 * At most 256 directory entries and 64 MiB of source reads across the refresh.
 * Exact deduplication retains at most 64 MiB of source buffers until return.
 * Success returns an owned catalog; individual entries may carry errors. */
os64_font_config_status_t os64_font_config_discover(os64_text_context_t *,
    const os64_font_config_t *, os64_font_catalog_t **out);
void os64_font_catalog_release(os64_font_catalog_t *);
/* Find a representative or a configured alias; -1 if absent. */
int os64_font_catalog_find(const os64_font_catalog_t *, const char *path);
/* Copy the role's primary — or the Web setting's, with OS64_FONT_CONFIG_WEB
 * as the role — into fonts/ at the top of the config ladder. The
 * staged file and complete candidate are validated before no-replace publish.
 * Existing installed names are refused. Success returns the installed path;
 * it neither publishes session choices nor saves fonts.conf. */
os64_font_config_status_t os64_font_config_install(os64_text_context_t *,
    const os64_font_config_t *, os64_font_role_t, char out[OS64_FONT_PATH_CAP],
    os64_font_config_error_t *);

#endif
