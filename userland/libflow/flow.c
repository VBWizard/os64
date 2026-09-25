// flow.c — the door: a document, its model and a width in, a laid-out
// tree out (LAYOUT.md § The one door).

#include "internal.h"

struct flow_tree {
    FStyles *styles;
    FBoxes *boxes;
    FLayout *layout;
};

void flow_free(flow_tree_t *tree)
{
    if (tree == NULL)
        return;
    f_layout_free(tree->layout);
    f_boxes_free(tree->boxes);
    f_style_free(tree->styles);
    os64_free(tree);
}

flow_tree_t *flow_layout(const os64_html_document_t *doc, const os64_page_t *model,
                         int32_t width, const flow_env_t *env)
{
    if (doc == NULL || env == NULL || env->text == NULL || env->fonts == NULL || width < 0)
        return NULL;
    flow_tree_t *tree = os64_calloc(1, sizeof(*tree));
    if (tree == NULL)
        return NULL;
    tree->styles = f_style_build(doc, model, env);
    tree->boxes = tree->styles != NULL ? f_boxes_build(doc, model, tree->styles, env) : NULL;
    tree->layout = tree->boxes != NULL ? f_layout(tree->boxes, doc, model, env, width) : NULL;
    if (tree->layout == NULL) {
        flow_free(tree);
        return NULL;
    }
    return tree;
}

bool flow_incomplete(const flow_tree_t *tree)
{
    return tree != NULL && tree->layout->incomplete;
}

int32_t flow_height(const flow_tree_t *tree)
{
    return tree != NULL ? (int32_t)((tree->layout->height + 63) / 64) : 0;
}

int32_t flow_width(const flow_tree_t *tree)
{
    return tree != NULL ? (int32_t)((tree->layout->width + 63) / 64) : 0;
}

int64_t flow_dump(const flow_tree_t *tree, char *out, size_t cap)
{
    if (tree == NULL) {
        if (cap > 0)
            out[0] = '\0';
        return 0;
    }
    return f_layout_dump(tree->layout, out, cap);
}
