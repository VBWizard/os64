#ifndef OS64_IMAGE_ICONS_H
#define OS64_IMAGE_ICONS_H

#include <stdint.h>
#include "os64/ui_icon.h"
#include "os64/ui_result.h"

#define OS64_ICON_CATALOG_PIXEL_BUDGET (2u * 1024u * 1024u)
#define OS64_ICON_CATALOG_RECORD_LIMIT 256u
#define OS64_ICON_FILE_CAP (64u * 1024u)

typedef struct os64_icon_catalog os64_icon_catalog_t;

/* The app owns the catalog and keeps it alive through all provider leases.
 * root is an absolute asset directory. A new catalog selects a new generation. */
os64_ui_status_code_t os64_icon_catalog_create(const char *root,
    uint64_t generation, os64_icon_catalog_t **out);
const os64_ui_icon_provider_t *os64_icon_catalog_provider(os64_icon_catalog_t *catalog);
os64_ui_status_code_t os64_icon_catalog_destroy(os64_icon_catalog_t *catalog);

/* Refresh drops cached results when no leases remain, permitting assets
 * installed after an earlier miss to be loaded by this catalog. */
os64_ui_status_code_t os64_icon_catalog_refresh(os64_icon_catalog_t *catalog);

typedef struct {
    uint32_t records;
    uint32_t leases;
    uint32_t retained_bytes;
    uint64_t file_attempts;
} os64_icon_catalog_stats_t;
void os64_icon_catalog_stats(const os64_icon_catalog_t *catalog,
    os64_icon_catalog_stats_t *out);

#endif
