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

/* The intersection observations' batch (os64_dom_intersections_provider_t)
 * answered from an already-current layout, laid out at `zoom` and shown at
 * `scroll`: one walk of the boxes for every asked element's bounds, then
 * each target's clip chain (flow_clip_parent) up to its root. False when
 * the layout is incomplete or there is no memory to answer. */
bool yonder_intersections(const os64_html_document_t *document, const flow_tree_t *tree,
    os64_dom_intersection_t *items, size_t count, uint32_t zoom, flow_point_t scroll);

/* Resolve an image-map region in unscaled CSS image-content coordinates. */
const os64_html_node_t *yonder_image_map_hit(const os64_html_document_t *document,
    const os64_html_node_t *image, double x, double y);
#endif
