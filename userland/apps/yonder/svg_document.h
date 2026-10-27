#ifndef YONDER_SVG_DOCUMENT_H
#define YONDER_SVG_DOCUMENT_H
#include "svg.h"
#include "html/xml.h"

/* Independently owned, immutable XML tree and embedded styles; natural-size
 * results are memoized for the last preference pair. No page, script
 * context, network/file resolver or host stylesheet is accepted by this API.
 * Calls on a document require owner serialization. */
typedef struct yonder_svg_document yonder_svg_document_t;
#define YONDER_SVG_DOCUMENT_BYTES (4u * 1024u * 1024u)
#define YONDER_SVG_DOCUMENT_TREE_BYTES (32u * 1024u * 1024u)
#define YONDER_SVG_DOCUMENT_STYLE_BYTES (256u * 1024u)
#define YONDER_SVG_DOCUMENT_SHEETS 16u
#define YONDER_SVG_CASCADE_BYTES (32u * 1024u * 1024u)
#define YONDER_SVG_READ_BYTES (64u * 1024u * 1024u)
#define YONDER_SVG_ATTRIBUTE_BYTES (2u * 1024u * 1024u)
#define YONDER_SVG_ATTRIBUTES 128u

/* Input need not outlive the call. Failure publishes no document. XML errors
 * remain available in diagnostic; a well-formed non-SVG root is UNSUPPORTED.
 * Cancellation context is borrowed only for this call. Parsing uses libhtml/
 * libgarb's bounded OS allocations, separate from the render owner ledger. */
svg_status_t yonder_svg_document_parse(const void *, size_t, bool (*cancelled)(void *), void *,
                                       yonder_svg_document_t **,
                                       os64_html_xml_result_t *diagnostic);
/* Default object size from absolute root CSS dimensions and/or viewBox ratio,
 * with a 300x150 fallback. Measurement media use that default viewport. */
svg_status_t yonder_svg_document_size(yonder_svg_document_t *, bool dark, bool reduced_motion,
                                       bool (*cancelled)(void *), void *, uint32_t *, uint32_t *);
void yonder_svg_document_free(yonder_svg_document_t *);
/* Retained tree + stylesheet arenas + document state; excludes temporary
 * cascade/render allocations and allocator implementation overhead. */
size_t yonder_svg_document_cost(const yonder_svg_document_t *);
/* An isolated cascade is rebuilt for the supplied viewport/preferences.
 * scripting is disabled; root currentColor starts at black. CSS imports and
 * external stylesheets are not loaded. On failure pixels is empty. Pixels
 * belong to memory and can outlive the document; use yonder_svg_pixels_free. */
svg_status_t yonder_svg_document_render(const yonder_svg_document_t *,
                                        const yonder_svg_image_view_t *,
                                        const os64_geometry_memory_t *,
                                        const os64_geometry_backend_t *, yonder_svg_pixels_t *);
#endif
