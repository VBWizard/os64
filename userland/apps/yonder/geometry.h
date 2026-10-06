#ifndef YONDER_GEOMETRY_H
#define YONDER_GEOMETRY_H
#include <dom/dom.h>
#include <flow/flow.h>

/* Read an already-current layout. Tree/model/document outlive the call; no
 * boxes are retained in the returned numeric snapshot. A partial parser tree
 * uses the same door after its host builds layout at the current view size. */
bool yonder_geometry_snapshot(const os64_html_document_t *document,
    const flow_tree_t *tree, const os64_html_node_t *node, int32_t width,
    int32_t height, uint32_t zoom, flow_point_t scroll, os64_dom_geometry_t *out);

/* Resolve an image-map region in unscaled CSS image-content coordinates. */
const os64_html_node_t *yonder_image_map_hit(const os64_html_document_t *document,
    const os64_html_node_t *image, double x, double y);
#endif
