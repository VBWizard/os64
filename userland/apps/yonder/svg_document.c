#include "svg_document.h"
#include <os64/arena.h>
#include <os64/mem.h>
#include <os64/str.h>

struct yonder_svg_document {
    os64_html_document_t *tree;
    garb_parsed_t sheets[YONDER_SVG_DOCUMENT_SHEETS];
    const char *media[YONDER_SVG_DOCUMENT_SHEETS];
    size_t nsheets, cost;
};
static svg_status_t xml_verdict(xml_status_t s)
{
    switch (s) {
    case XML_DONE:
        return SVG_OK;
    case XML_NO_MEMORY:
        return SVG_NO_MEMORY;
    case XML_LIMIT:
        return SVG_LIMIT;
    case XML_CANCELLED:
        return SVG_CANCELLED;
    case XML_UNSUPPORTED:
    case XML_UNSUPPORTED_ENCODING:
        return SVG_UNSUPPORTED;
    case XML_BAD_ARGUMENT:
        return SVG_BAD_ARGUMENT;
    default:
        return SVG_MALFORMED;
    }
}
static const char *attribute(const os64_html_node_t *n, const char *name)
{
    const os64_html_attr_t *a = os64_html_attr(n, name);
    return a && !a->ns ? a->value : "";
}
static bool css_type(const char *s)
{
    if (!*s)
        return true;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        ++s;
    const char *type = "text/css";
    for (; *type; ++type, ++s) {
        char c = *s;
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        if (c != *type)
            return false;
    }
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        ++s;
    return !*s;
}
void yonder_svg_document_free(yonder_svg_document_t *d)
{
    if (!d)
        return;
    for (size_t i = 0; i < d->nsheets; ++i)
        garb_free(&d->sheets[i]);
    os64_html_document_free(d->tree);
    os64_free(d);
}
size_t yonder_svg_document_cost(const yonder_svg_document_t *d)
{
    return d ? d->cost : 0;
}
svg_status_t yonder_svg_document_parse(const void *bytes, size_t len, bool (*cancelled)(void *),
                                       void *context, yonder_svg_document_t **out,
                                       os64_html_xml_result_t *diagnostic)
{
    os64_html_xml_result_t result = {.status = XML_BAD_ARGUMENT};
    if (diagnostic)
        *diagnostic = result;
    if (!out)
        return SVG_BAD_ARGUMENT;
    *out = NULL;
    if (!bytes && len)
        return SVG_BAD_ARGUMENT;
    if (len > YONDER_SVG_DOCUMENT_BYTES) {
        if (diagnostic)
            diagnostic->status = XML_LIMIT;
        return SVG_LIMIT;
    }
    os64_html_xml_options_t options = os64_html_xml_options_default();
    options.max_arena_bytes = YONDER_SVG_DOCUMENT_TREE_BYTES;
    options.max_tree_work = 4000000;
    options.parser_limits.encoded_bytes = YONDER_SVG_DOCUMENT_BYTES;
    options.parser_limits.decoded_bytes = YONDER_SVG_DOCUMENT_BYTES;
    options.parser_limits.expanded_bytes = YONDER_SVG_DOCUMENT_BYTES;
    options.parser_limits.depth = 128;
    options.context = context;
    options.cancelled = cancelled;
    os64_html_xml_parser_t *parser = os64_html_xml_parser_new(&options, &result);
    if (!parser) {
        if (diagnostic)
            *diagnostic = result;
        return xml_verdict(result.status);
    }
    size_t consumed = 0;
    os64_html_xml_parser_feed(parser, bytes, len, &consumed);
    os64_html_document_t *tree = os64_html_xml_parser_finish(parser, &result);
    if (diagnostic)
        *diagnostic = result;
    if (!tree)
        return xml_verdict(result.status);
    const os64_html_node_t *root = os64_html_document_element(tree);
    if (!root || root->ns != OS64_HTML_NS_SVG || !os64_streq(root->name, "svg")) {
        os64_html_document_free(tree);
        return SVG_UNSUPPORTED;
    }
    yonder_svg_document_t *d = os64_calloc(1, sizeof(*d));
    if (!d) {
        os64_html_document_free(tree);
        return SVG_NO_MEMORY;
    }
    d->tree = tree;
    d->cost = sizeof(*d) + tree->arena_bytes;
    if (d->cost > YONDER_SVG_DOCUMENT_TREE_BYTES) {
        yonder_svg_document_free(d);
        return SVG_LIMIT;
    }
    svg_status_t status = SVG_OK;
    size_t style_bytes = 0, visits = 0;
    for (const os64_html_node_t *n = root; n;) {
        if (cancelled && cancelled(context)) {
            status = SVG_CANCELLED;
            break;
        }
        if (++visits > 65536) {
            status = SVG_LIMIT;
            break;
        }
        if (n->kind == OS64_HTML_ELEMENT && n->ns == OS64_HTML_NS_SVG &&
            os64_streq(n->name, "style") && css_type(attribute(n, "type"))) {
            if (d->nsheets == YONDER_SVG_DOCUMENT_SHEETS) {
                status = SVG_LIMIT;
                break;
            }
            for (const os64_html_node_t *p = n->first_child; p; p = p->next) {
                if (++visits > 65536) {
                    status = SVG_LIMIT;
                    break;
                }
                if (p->kind != OS64_HTML_TEXT)
                    continue;
                if (p->text_len > YONDER_SVG_DOCUMENT_STYLE_BYTES - style_bytes) {
                    status = SVG_LIMIT;
                    break;
                }
                style_bytes += p->text_len;
            }
            if (status != SVG_OK)
                break;
            size_t i = d->nsheets++;
            garb_status_t parsed = garb_parse_style_element(n, &d->sheets[i]);
            if (parsed != GARB_OK || d->sheets[i].incomplete) {
                status = parsed == GARB_NO_MEMORY ? SVG_NO_MEMORY : SVG_LIMIT;
                break;
            }
            d->media[i] = attribute(n, "media");
            d->cost += os64_arena_stats(d->sheets[i].arena).reserved_bytes;
            // Bound retained parses collectively; a single parse has garb's
            // own temporary cap while it is being constructed.
            if (d->cost > YONDER_SVG_DOCUMENT_TREE_BYTES) {
                status = SVG_LIMIT;
                break;
            }
        }
        if (n->first_child)
            n = n->first_child;
        else {
            while (n != root && !n->next)
                n = n->parent;
            if (n == root)
                break;
            n = n->next;
        }
    }
    if (cancelled && cancelled(context))
        status = SVG_CANCELLED;
    if (status != SVG_OK)
        yonder_svg_document_free(d);
    else
        *out = d;
    return status;
}
svg_status_t yonder_svg_document_render(const yonder_svg_document_t *d,
                                        const yonder_svg_image_view_t *view,
                                        const os64_geometry_memory_t *memory,
                                        const os64_geometry_backend_t *backend,
                                        yonder_svg_pixels_t *out)
{
    if (!out)
        return SVG_BAD_ARGUMENT;
    *out = (yonder_svg_pixels_t){0};
    if (!d || !view || !memory || !memory->alloc || !memory->free)
        return SVG_BAD_ARGUMENT;
    if (view->width > 16384 || view->height > 16384 ||
        (uint64_t)view->width * view->height > 8388608)
        return SVG_LIMIT;
    if (view->cancelled && view->cancelled(view->cancel_context))
        return SVG_CANCELLED;
    if (!view->width || !view->height)
        return SVG_OK;
    garb_sheet_in_t sheets[YONDER_SVG_DOCUMENT_SHEETS] = {0};
    for (size_t i = 0; i < d->nsheets; ++i)
        sheets[i] = (garb_sheet_in_t){.sheet = &d->sheets[i], .media = d->media[i]};
    garb_env_t env = {.width = view->width,
                      .height = view->height,
                      .dark = view->dark,
                      .reduced_motion = view->reduced_motion,
                      .scripting = false,
                      .resolution = 1};
    garb_cascade_t *cascade = garb_cascade(sheets, (int32_t)d->nsheets, d->tree, env);
    if (!cascade)
        return SVG_NO_MEMORY;
    svg_status_t status;
    if (view->cancelled && view->cancelled(view->cancel_context))
        status = SVG_CANCELLED;
    else
        status = yonder_svg_render_image_tree(d->tree, cascade, view, memory, backend, out);
    garb_cascade_free(cascade);
    return status;
}
