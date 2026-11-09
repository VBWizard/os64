#include "internal.h"

/* THE TABLES' TREE VERBS (0f's P6b; INTERFACES_FORMS.md § P6; HTML § 4.9).
 *
 * A table's caption, head and foot, read and replaced; its tbodies and its
 * rows, a section's rows, a row's cells and a map's areas, live
 * collections made once (collection.c's table kinds, d_table_query); and
 * the verbs that make and remove them: createCaption and the rest,
 * insertRow and deleteRow on a table and a section, insertCell and
 * deleteCell on a row, and a row's and a cell's index. A verb reads the
 * collection a page reads, so the two agree on every index. Every change
 * to the tree goes through d_tree_change, the door a script's own
 * appendChild takes, and every member checks its own interface's brand.
 *
 * One row of the Modern Web table, naming nothing of window's. */

enum {
    T_CAPTION, T_CREATE_CAPTION, T_DELETE_CAPTION, T_HEAD, T_CREATE_HEAD, T_DELETE_HEAD,
    T_FOOT, T_CREATE_FOOT, T_DELETE_FOOT, T_BODIES, T_CREATE_BODY, T_ROWS, T_INSERT_ROW, T_DELETE_ROW,
    S_ROWS, S_INSERT_ROW, S_DELETE_ROW,
    W_ROW_INDEX, W_SECTION_ROW_INDEX, W_CELLS, W_INSERT_CELL, W_DELETE_CELL,
    C_CELL_INDEX, M_AREAS
};

/* The interface a member is on: the table's first, a section's, a row's,
 * a cell's, a map's, in the enumeration's order. */
static int member_kind(int member)
{
    return member <= T_DELETE_ROW ? D_PROTO_TABLE : member <= S_DELETE_ROW ? D_PROTO_TABLE_SECTION :
           member <= W_DELETE_CELL ? D_PROTO_TABLE_ROW : member == C_CELL_INDEX ? D_PROTO_TABLE_CELL : D_PROTO_MAP;
}

static bool html_is(const os64_html_node_t *node, os64_html_tag_t tag)
{
    return node != NULL && node->kind == OS64_HTML_ELEMENT && node->ns == OS64_HTML_NS_HTML && node->tag == tag;
}

static const os64_html_node_t *first_child(const os64_html_node_t *parent, os64_html_tag_t tag)
{
    for (const os64_html_node_t *child = parent->first_child; child != NULL; child = child->next)
        if (html_is(child, tag)) return child;
    return NULL;
}

/* An HTML element named `name`, wrapped in *value (which holds it); NULL
 * with *value the exception. The caller puts it in the owner's tree at
 * once, whose node document is then its own. */
static const os64_html_node_t *create(os64_dom_t *dom, JSContext *ctx, const char *name, JSValue *value)
{
    int64_t status;
    os64_html_node_t *made = os64_html_create_element(dom->document, OS64_HTML_NS_HTML, name, &status);
    if (made == NULL) {
        *value = d_html_error(ctx, status);
        return NULL;
    }
    *value = d_wrap(dom, ctx, made);
    return JS_IsException(*value) ? NULL : made;
}

/* `node`, made for the caller, put under `parent` before `before` (NULL:
 * last), and answered; the exception when it cannot be. */
static JSValue placed(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *parent,
                      const os64_html_node_t *node, const os64_html_node_t *before, JSValue value)
{
    if (d_tree_change(dom, ctx, before != NULL ? D_INSERT : D_APPEND, parent, node, before) < 0) {
        JS_FreeValue(ctx, value);
        return JS_EXCEPTION;
    }
    return value;
}

/* Take `node` out of its parent, when there is one. */
static JSValue removed(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node)
{
    if (node == NULL || node->parent == NULL) return JS_UNDEFINED;
    return d_tree_change(dom, ctx, D_REMOVE, node->parent, node, NULL) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

/* A collection of `owner`'s as a value for the page, the same object every
 * time. */
static JSValue collection_value(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *owner, unsigned kind)
{
    DQuery *query = d_table_query(dom, ctx, owner, kind);
    return query != NULL ? JS_DupValue(ctx, query->entry->value) : JS_EXCEPTION;
}

/* `owner`'s collection's nodes now; false with the exception pending. */
static bool items(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *owner, unsigned kind,
                  const os64_html_node_t *const **nodes, size_t *count)
{
    DQuery *query = d_table_query(dom, ctx, owner, kind);
    return query != NULL && d_query_items(query, ctx, nodes, count);
}

/* `node`'s place in `owner`'s collection, -1 when it is not in it. */
static JSValue index_in(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *owner, unsigned kind,
                        const os64_html_node_t *node)
{
    const os64_html_node_t *const *nodes;
    size_t count;
    if (!items(dom, ctx, owner, kind, &nodes, &count)) return JS_EXCEPTION;
    for (size_t i = 0; i < count; i++)
        if (nodes[i] == node) return JS_NewInt32(ctx, (int32_t)i);
    return JS_NewInt32(ctx, -1);
}

/* The insert verbs' `optional long index = -1`, and the delete verbs'
 * `long index`, which must be given: ToInt32's wrap. False with the
 * exception pending. */
static bool index_argument(JSContext *ctx, int argc, JSValueConst *argv, bool optional, int32_t *index)
{
    if (argc < 1 || (optional && JS_IsUndefined(argv[0]))) {
        *index = -1;
        if (optional) return true;
        JS_ThrowTypeError(ctx, "Missing index");
        return false;
    }
    return JS_ToInt32(ctx, index, argv[0]) >= 0;
}

/* insertRow on a section, insertCell on a row: a new `name` at `index` among
 * `owner`'s collection, or last for -1 or the count; IndexSizeError for
 * an index below -1 or past the count. */
static JSValue insert_at(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *owner, unsigned kind,
                         int32_t index, const char *name)
{
    const os64_html_node_t *const *nodes;
    size_t count;
    if (!items(dom, ctx, owner, kind, &nodes, &count)) return JS_EXCEPTION;
    if (index < -1 || (index >= 0 && (size_t)index > count))
        return d_error(ctx, "IndexSizeError", "The index is out of range");
    const os64_html_node_t *before = index == -1 || (size_t)index == count ? NULL : nodes[index];
    JSValue value;
    const os64_html_node_t *made = create(dom, ctx, name, &value);
    return made == NULL ? value : placed(dom, ctx, owner, made, before, value);
}

/* deleteRow and deleteCell: the `index`th of `owner`'s collection, the last
 * for -1 (nothing when there is none); IndexSizeError for an index below
 * -1 or at or past the count. */
static JSValue delete_at(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *owner, unsigned kind,
                         int32_t index)
{
    const os64_html_node_t *const *nodes;
    size_t count;
    if (!items(dom, ctx, owner, kind, &nodes, &count)) return JS_EXCEPTION;
    if (index < -1 || (index >= 0 && (size_t)index >= count))
        return d_error(ctx, "IndexSizeError", "The index is out of range");
    if (index == -1) {
        if (count == 0) return JS_UNDEFINED;
        index = (int32_t)count - 1;
    }
    return removed(dom, ctx, nodes[index]);
}

/* A table's insertRow (HTML § 4.9.1): a new row at `index` in its rows;
 * with no rows, last in its last tbody, or in a new tbody of its own; last
 * for -1 or the count, in the last row's parent. */
static JSValue insert_row(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *table, int32_t index)
{
    const os64_html_node_t *const *rows;
    size_t count;
    if (!items(dom, ctx, table, D_QUERY_TABLE_ROWS, &rows, &count)) return JS_EXCEPTION;
    if (index < -1 || (index >= 0 && (size_t)index > count))
        return d_error(ctx, "IndexSizeError", "The index is out of range");
    const os64_html_node_t *parent, *before = NULL;
    if (count == 0) {
        parent = NULL;
        for (const os64_html_node_t *child = table->first_child; child != NULL; child = child->next)
            if (html_is(child, OS64_HTML_TAG_TBODY)) parent = child;
    } else if (index == -1 || (size_t)index == count) {
        parent = rows[count - 1]->parent;
    } else {
        parent = rows[index]->parent;
        before = rows[index];
    }
    JSValue row;
    const os64_html_node_t *made = create(dom, ctx, "tr", &row);
    if (made == NULL) return row;
    if (parent != NULL) return placed(dom, ctx, parent, made, before, row);
    JSValue body;
    const os64_html_node_t *section = create(dom, ctx, "tbody", &body);
    if (section == NULL) {
        JS_FreeValue(ctx, row);
        return body;
    }
    if (d_tree_change(dom, ctx, D_APPEND, section, made, NULL) < 0 ||
        d_tree_change(dom, ctx, D_APPEND, table, section, NULL) < 0) {
        JS_FreeValue(ctx, body);
        JS_FreeValue(ctx, row);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, body);
    return row;
}

/* The first child of a table that is neither a caption nor a colgroup:
 * where a head goes. */
static const os64_html_node_t *after_captions(const os64_html_node_t *table)
{
    for (const os64_html_node_t *child = table->first_child; child != NULL; child = child->next)
        if (child->kind == OS64_HTML_ELEMENT && !html_is(child, OS64_HTML_TAG_CAPTION) &&
            !html_is(child, OS64_HTML_TAG_COLGROUP))
            return child;
    return NULL;
}

/* caption, tHead and tFoot's setters: `value` is null (undefined too, as
 * WebIDL converts it for a nullable type) or an element of the member's
 * interface (WebIDL's TypeError otherwise; a section that is not the
 * member's tag is HTML's HierarchyRequestError). The table's
 * present one goes, and the new one is put where the member's create verb
 * would put it. */
static JSValue set_part(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *table, int member,
                        JSValueConst value)
{
    os64_html_tag_t tag = member == T_CAPTION ? OS64_HTML_TAG_CAPTION :
                          member == T_HEAD ? OS64_HTML_TAG_THEAD : OS64_HTML_TAG_TFOOT;
    const os64_html_node_t *given = NULL;
    if (!JS_IsNull(value) && !JS_IsUndefined(value)) {
        given = JS_IsObject(value) ? d_node(dom, ctx, value) : NULL;
        if (given == NULL) {
            if (JS_IsObject(value) && JS_HasException(ctx)) JS_FreeValue(ctx, JS_GetException(ctx));
            return JS_ThrowTypeError(ctx, "Expected an element of the member's interface or null");
        }
        int kind = d_node_kind(given);
        if (kind != (member == T_CAPTION ? D_PROTO_TABLE_CAPTION : D_PROTO_TABLE_SECTION))
            return JS_ThrowTypeError(ctx, "Expected an element of the member's interface or null");
        if (!html_is(given, tag)) return d_error(ctx, "HierarchyRequestError", "The section is not the member's");
    }
    if (JS_IsException(removed(dom, ctx, first_child(table, tag)))) return JS_EXCEPTION;
    if (given == NULL) return JS_UNDEFINED;
    const os64_html_node_t *before = member == T_CAPTION ? table->first_child :
                                     member == T_HEAD ? after_captions(table) : NULL;
    return d_tree_change(dom, ctx, before != NULL ? D_INSERT : D_APPEND, table, given, before) < 0 ? JS_EXCEPTION
                                                                                                    : JS_UNDEFINED;
}

/* createCaption, createTHead and createTFoot: the table's present one, or
 * a new one where it goes (a caption first, a head before the first child
 * that is neither a caption nor a colgroup, a foot last). */
static JSValue create_part(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *table, int member)
{
    os64_html_tag_t tag = member == T_CREATE_CAPTION ? OS64_HTML_TAG_CAPTION :
                          member == T_CREATE_HEAD ? OS64_HTML_TAG_THEAD : OS64_HTML_TAG_TFOOT;
    const os64_html_node_t *present = first_child(table, tag);
    if (present != NULL) return d_wrap(dom, ctx, present);
    JSValue value;
    const os64_html_node_t *made = create(dom, ctx, member == T_CREATE_CAPTION ? "caption" :
                                          member == T_CREATE_HEAD ? "thead" : "tfoot", &value);
    if (made == NULL) return value;
    const os64_html_node_t *before = member == T_CREATE_CAPTION ? table->first_child :
                                     member == T_CREATE_HEAD ? after_captions(table) : NULL;
    return placed(dom, ctx, table, made, before, value);
}

/* createTBody: a new tbody after the table's last, or last when it has
 * none. */
static JSValue create_body(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *table)
{
    const os64_html_node_t *last = NULL;
    for (const os64_html_node_t *child = table->first_child; child != NULL; child = child->next)
        if (html_is(child, OS64_HTML_TAG_TBODY)) last = child;
    JSValue value;
    const os64_html_node_t *made = create(dom, ctx, "tbody", &value);
    return made == NULL ? value : placed(dom, ctx, table, made, last != NULL ? last->next : NULL, value);
}

static JSValue table_member(JSContext *ctx, JSValueConst this_value, int argc, JSValueConst *argv, int magic,
                            JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *node = d_node(dom, ctx, this_value);
    if (node == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    if (d_node_kind(node) != member_kind(magic)) return JS_ThrowTypeError(ctx, "Illegal invocation");
    switch (magic) {
    case T_CAPTION: case T_HEAD: case T_FOOT:
        if (set) return set_part(dom, ctx, node, magic, argc > 0 ? argv[0] : JS_UNDEFINED);
        return d_wrap(dom, ctx, first_child(node, magic == T_CAPTION ? OS64_HTML_TAG_CAPTION :
                                                  magic == T_HEAD ? OS64_HTML_TAG_THEAD : OS64_HTML_TAG_TFOOT));
    case T_CREATE_CAPTION: case T_CREATE_HEAD: case T_CREATE_FOOT: return create_part(dom, ctx, node, magic);
    case T_DELETE_CAPTION: return removed(dom, ctx, first_child(node, OS64_HTML_TAG_CAPTION));
    case T_DELETE_HEAD: return removed(dom, ctx, first_child(node, OS64_HTML_TAG_THEAD));
    case T_DELETE_FOOT: return removed(dom, ctx, first_child(node, OS64_HTML_TAG_TFOOT));
    case T_BODIES: return collection_value(dom, ctx, node, D_QUERY_TBODIES);
    case T_CREATE_BODY: return create_body(dom, ctx, node);
    case T_ROWS: return collection_value(dom, ctx, node, D_QUERY_TABLE_ROWS);
    case S_ROWS: return collection_value(dom, ctx, node, D_QUERY_SECTION_ROWS);
    case W_CELLS: return collection_value(dom, ctx, node, D_QUERY_CELLS);
    case M_AREAS: return collection_value(dom, ctx, node, D_QUERY_AREAS);
    case T_INSERT_ROW: case S_INSERT_ROW: case W_INSERT_CELL: {
        int32_t index;
        if (!index_argument(ctx, argc, argv, true, &index)) return JS_EXCEPTION;
        if (magic == T_INSERT_ROW) return insert_row(dom, ctx, node, index);
        return magic == S_INSERT_ROW ? insert_at(dom, ctx, node, D_QUERY_SECTION_ROWS, index, "tr")
                                     : insert_at(dom, ctx, node, D_QUERY_CELLS, index, "td");
    }
    case T_DELETE_ROW: case S_DELETE_ROW: case W_DELETE_CELL: {
        int32_t index;
        if (!index_argument(ctx, argc, argv, false, &index)) return JS_EXCEPTION;
        return delete_at(dom, ctx, node, magic == T_DELETE_ROW ? D_QUERY_TABLE_ROWS :
                                         magic == S_DELETE_ROW ? D_QUERY_SECTION_ROWS : D_QUERY_CELLS, index);
    }
    case W_ROW_INDEX: {
        /* Its place in the rows of the table that is its parent, or its
         * section's parent; -1 for a row in no table. */
        const os64_html_node_t *parent = node->parent;
        if (html_is(parent, OS64_HTML_TAG_TABLE)) return index_in(dom, ctx, parent, D_QUERY_TABLE_ROWS, node);
        if ((html_is(parent, OS64_HTML_TAG_THEAD) || html_is(parent, OS64_HTML_TAG_TBODY) ||
             html_is(parent, OS64_HTML_TAG_TFOOT)) && html_is(parent->parent, OS64_HTML_TAG_TABLE))
            return index_in(dom, ctx, parent->parent, D_QUERY_TABLE_ROWS, node);
        return JS_NewInt32(ctx, -1);
    }
    case W_SECTION_ROW_INDEX: {
        /* Its place in its parent's rows: a table's, or a section's. */
        const os64_html_node_t *parent = node->parent;
        if (html_is(parent, OS64_HTML_TAG_TABLE)) return index_in(dom, ctx, parent, D_QUERY_TABLE_ROWS, node);
        if (html_is(parent, OS64_HTML_TAG_THEAD) || html_is(parent, OS64_HTML_TAG_TBODY) ||
            html_is(parent, OS64_HTML_TAG_TFOOT))
            return index_in(dom, ctx, parent, D_QUERY_SECTION_ROWS, node);
        return JS_NewInt32(ctx, -1);
    }
    case C_CELL_INDEX:
        return html_is(node->parent, OS64_HTML_TAG_TR) ? index_in(dom, ctx, node->parent, D_QUERY_CELLS, node)
                                                       : JS_NewInt32(ctx, -1);
    default: return JS_ThrowTypeError(ctx, "Unknown table member");
    }
}

int d_tables_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    (void)global;
    static const struct { const char *name; int magic; bool writable; } kAttributes[] = {
        {"caption", T_CAPTION, true}, {"tHead", T_HEAD, true}, {"tFoot", T_FOOT, true},
        {"tBodies", T_BODIES, false}, {"rows", T_ROWS, false}, {"rows", S_ROWS, false},
        {"rowIndex", W_ROW_INDEX, false}, {"sectionRowIndex", W_SECTION_ROW_INDEX, false},
        {"cells", W_CELLS, false}, {"cellIndex", C_CELL_INDEX, false}, {"areas", M_AREAS, false},
    };
    static const struct { const char *name; int magic, argc; } kMethods[] = {
        {"createCaption", T_CREATE_CAPTION, 0}, {"deleteCaption", T_DELETE_CAPTION, 0},
        {"createTHead", T_CREATE_HEAD, 0}, {"deleteTHead", T_DELETE_HEAD, 0},
        {"createTFoot", T_CREATE_FOOT, 0}, {"deleteTFoot", T_DELETE_FOOT, 0},
        {"createTBody", T_CREATE_BODY, 0}, {"insertRow", T_INSERT_ROW, 0}, {"deleteRow", T_DELETE_ROW, 1},
        {"insertRow", S_INSERT_ROW, 0}, {"deleteRow", S_DELETE_ROW, 1},
        {"insertCell", W_INSERT_CELL, 0}, {"deleteCell", W_DELETE_CELL, 1},
    };
    for (size_t i = 0; i < sizeof(kAttributes) / sizeof(kAttributes[0]); i++)
        if (d_accessor(dom, ctx, dom->prototypes[member_kind(kAttributes[i].magic)]->value, kAttributes[i].name,
                       table_member, kAttributes[i].magic, kAttributes[i].writable) < 0)
            return -1;
    for (size_t i = 0; i < sizeof(kMethods) / sizeof(kMethods[0]); i++)
        if (d_method_named(dom, ctx, dom->prototypes[member_kind(kMethods[i].magic)]->value, kMethods[i].name,
                           table_member, kMethods[i].argc, kMethods[i].magic) < 0)
            return -1;
    return 0;
}
