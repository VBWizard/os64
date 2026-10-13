#ifndef OS64_IMAGE_ICON_THEME_H
#define OS64_IMAGE_ICON_THEME_H

#include "os64/ui_icon.h"
#include "os64/ui_result.h"

typedef struct os64_icon_theme os64_icon_theme_t;

/* Resolve icons.conf on the system config ladder. Its `set` value names an
 * unpacked directory with a format-1 theme.conf. Missing icons in that set
 * are looked up in /etc/icons/default. The caller owns the returned theme
 * and keeps it alive while consumers hold icon leases. A theme has up to two
 * catalogs, each with a 2 MiB retained-pixel limit and a 64 KiB catalog
 * metadata limit. The theme object and its live lease-routing records share
 * a separate 64 KiB metadata limit, including allocation overhead.
 * Creation returns NO_MEMORY for allocation failure and ERROR for I/O failure
 * reading configuration or manifests; out remains NULL so the caller can retry.
 * Missing/invalid selected manifests retain the default-set fallback. Open
 * failure with successful stat is I/O; failed stat cannot prove absence in
 * this ABI and follows the missing-manifest policy. */
os64_ui_status_code_t os64_icon_theme_create(os64_icon_theme_t **out);
const os64_ui_icon_provider_t *os64_icon_theme_provider(os64_icon_theme_t *theme);
os64_ui_status_code_t os64_icon_theme_destroy(os64_icon_theme_t *theme);

/* Retained catalog pixel-cache bytes, shared across views; excludes theme
 * routing metadata and decoder scratch. This query allocates nothing. */
size_t os64_icon_theme_cache_bytes(const os64_icon_theme_t *theme);

/* Metadata describes the selected set, or the shipped default when no valid
 * selected set was found. Strings live until theme destruction. */
const char *os64_icon_theme_path(const os64_icon_theme_t *theme);
const char *os64_icon_theme_name(const os64_icon_theme_t *theme);
const char *os64_icon_theme_author(const os64_icon_theme_t *theme);

#endif
