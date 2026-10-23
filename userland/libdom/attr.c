#include "internal.h"

/* ELEMENT.ATTRIBUTES (DOM § 4.9.1, NamedNodeMap; § 4.9.2, Attr): the
 * surface jQuery 1.x reads at load (its focusin probe is
 * `e.attributes[name].expando`), and with it the attribute-node methods of
 * Document and Element, whose verbs the map's are. Classic surface, on
 * every page; the two interface objects are the interfaces row's.
 *
 * AN ATTR IS A VIEW OF ITS ELEMENT, NOT A TREE NODE. libhtml's tree holds an
 * element's attributes as a list of names and values on the element, never
 * as nodes, so an Attr is what `style`, `classList` and `dataset` are: a
 * registry entry with no `node`, whose `target` is the element it stands
 * for, given a name (DAttr). The binding keeps an element's Attrs in a
 * table by element and qualified name, one per name, each made the first
 * time it is asked for, so the same object comes back for as long as the
 * attribute exists. The name is the identity because libhtml's is: its
 * verbs find an attribute by qualified name, first match, and no verb
 * makes two of one name.
 *
 * NOTHING IS HELD ACROSS PAGE CODE. An attached Attr's value is read from
 * libhtml by name at every read, never through a pointer kept from an
 * earlier one: libhtml retires an attribute's bytes when its value changes.
 * The map trusts no snapshot: `length`, `item(i)` and every indexed or
 * named property answer the element's list as it is at the call, so a
 * getter that removes an attribute in the middle of a walk is seen by the
 * walk's next step, which is the standard's answer too. The last element
 * walked is kept as an array of its attributes, used only while its list
 * is provably the one copied (the walk's cursor, below), so `length` and
 * the i-th attribute are a step each, and a whole walk over n attributes,
 * forwards or back, is linear in n.
 *
 * DETACHMENT HAPPENS AT THE ONE DOOR. An attribute leaves its element by a
 * removal, and every attribute the binding sets or removes goes through
 * d_set_attr: removeAttribute, toggleAttribute, a dataset delete,
 * removeAttributeNode and removeNamedItem among them. The parser removes
 * none (it sets an element's attributes as it makes the element, and a
 * second html or body tag only adds the ones the element lacks), and
 * libpage keeps a control's state beside the attribute list, never by
 * removing from it; so the door sees every removal, copies the attribute's
 * last value into the Attr (binding-allocated, charged before the removal
 * is made), and clears its `target`. A detached Attr keeps and takes a
 * value of its own and answers ownerElement null. Its entry is in the
 * binding's registry until os64_dom_free, like every entry, and charged to
 * the binding's budget: a page that makes them without end meets
 * QuotaExceededError, not a leak.
 *
 * THE LAZY BACKSTOP names the invariant if it ever stops being true: an
 * attached Attr whose name libhtml no longer finds on its element reads as
 * detached with an empty value, and the host is told (options.broken, which
 * yonder writes as `FAILED page: an attribute left its element outside
 * libdom`). What it cannot see is an attribute removed and set again
 * outside the door, which reads as the same attribute with its new value.
 *
 * MOVING ONE. setAttributeNode takes a detached Attr onto an element (its
 * stored value set through the door, the Attr made the element's), answers
 * the Attr it replaced, now detached, or null; an Attr of
 * another element is InUseAttributeError, and one of this element is
 * answered as it is.
 *
 * A NAMESPACE GOES WITH THE NAME AND NO FURTHER. libhtml's verbs set an
 * attribute by qualified name, keeping the namespace of one that is there
 * and giving a new one none. So an Attr in a namespace (createAttributeNS,
 * or one an SVG element's `xlink:href` stood for) is set onto an element
 * only where the element holds that name in that namespace already, and
 * one in none only where the element does not hold its name in some
 * namespace; anything else is a NotSupportedError, since the tree would
 * hold the attribute under another namespace than the Attr says it has. */

static JSClassID attr_slot, map_slot;

/* An Attr's record, on its own entry (DValue.attr) and, while attached, in
 * the binding's table of attached Attrs (os64_dom.attr_table), chained by
 * `hash_next`. */
struct DAttr {
    struct DAttr *hash_next;
    DValue *entry;
    /* The qualified name and the namespace (NULL: none), both the
     * binding's copies, fixed for the Attr's life. */
    char *name, *ns;
    /* While detached: its value (NULL reads ""), and its node document,
     * NULL for the window's. While attached the element says both. */
    char *value;
    size_t length;
    const os64_html_node_t *document;
    /* Its childNodes, an empty NodeList made when first asked for. */
    DValue *child_nodes;
};
typedef struct DAttr DAttr;

static const char kBroken[] = "an attribute left its element outside libdom";

/* Steps taken along an element's attribute list, the cursor's array and the
 * table's chains, for the DOM host, which holds each kind of whole walk over
 * an element's attributes to a number of steps linear in them. */
static uint64_t s_steps;

uint64_t d_attr_steps(void)
{
    return s_steps;
}

static char *copy_bytes(os64_dom_t *dom, const char *text, size_t length)
{
    char *copy = d_alloc(dom, length + 1);
    if (copy == NULL) return NULL;
    os64_memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static bool ns_equal(const char *a, const char *b)
{
    return a == NULL || b == NULL ? a == b : os64_strcmp(a, b) == 0;
}

/* A name's local part (DOM § 4.9.2): after the first colon of a name in a
 * namespace; the whole name of one in none, colons and all. */
static const char *local_of(const char *name, const char *ns)
{
    const char *colon = ns != NULL ? os64_strchr(name, ':') : NULL;
    return colon != NULL ? colon + 1 : name;
}

static const os64_html_node_t *window_document(os64_dom_t *dom)
{
    return dom->document->document;
}

/* An HTML element's attribute names are folded as a script names them,
 * as getAttribute folds them (LIBDOM.md § Documents of their own says
 * where that reads an XML document's elements as an HTML one's). */
static bool folds(const os64_html_node_t *element)
{
    return element->ns == OS64_HTML_NS_HTML;
}

/* The attribute a namespace and local name name (DOM's "get an attribute
 * by namespace and local name"), NULL for none. */
static const os64_html_attr_t *attr_ns(const os64_html_node_t *element, const char *ns, const char *local)
{
    for (const os64_html_attr_t *a = element->attrs; a != NULL; a = a->next)
        if (ns_equal(a->ns, ns) && os64_strcmp(local_of(a->name, a->ns), local) == 0) return a;
    return NULL;
}

/* THE TABLE OF ATTACHED ATTRS, by element and qualified name: what makes
 * finding an attribute's Attr a step or two however many Attrs an element
 * has, so a walk over all of an element's attributes stays linear. Made at
 * install; it doubles when it holds twice its slots, and keeps its size
 * when there is no room to double, its chains growing longer. */
static size_t record_slot(const os64_dom_t *dom, const os64_html_node_t *element, const char *name)
{
    uint64_t key = (uint64_t)(uintptr_t)element * 0x9E3779B97F4A7C15ull;
    for (const unsigned char *at = (const unsigned char *)name; *at != 0; at++)
        key = (key ^ *at) * 0x100000001B3ull;
    return (size_t)(key ^ (key >> 29)) & (dom->attr_capacity - 1);
}

static DAttr *record_of(os64_dom_t *dom, const os64_html_node_t *element, const char *name)
{
    for (DAttr *rec = dom->attr_table[record_slot(dom, element, name)]; rec != NULL; rec = rec->hash_next) {
        s_steps++;
        if (rec->entry->target == element && os64_strcmp(rec->name, name) == 0) return rec;
    }
    return NULL;
}

static void record_add(os64_dom_t *dom, DAttr *rec)
{
    if (dom->attr_attached >= dom->attr_capacity * 2) {
        size_t capacity = dom->attr_capacity * 2;
        DAttr **table = capacity <= SIZE_MAX / sizeof(*table) ? d_alloc(dom, capacity * sizeof(*table)) : NULL;
        if (table != NULL) {
            DAttr **old = dom->attr_table;
            size_t old_capacity = dom->attr_capacity;
            dom->attr_table = table;
            dom->attr_capacity = capacity;
            for (size_t i = 0; i < old_capacity; i++)
                for (DAttr *at = old[i], *next; at != NULL; at = next) {
                    next = at->hash_next;
                    size_t slot = record_slot(dom, at->entry->target, at->name);
                    at->hash_next = table[slot];
                    table[slot] = at;
                }
            d_free(dom, old);
        }
    }
    size_t slot = record_slot(dom, rec->entry->target, rec->name);
    rec->hash_next = dom->attr_table[slot];
    dom->attr_table[slot] = rec;
    dom->attr_attached++;
}

static void record_remove(os64_dom_t *dom, DAttr *rec)
{
    for (DAttr **at = &dom->attr_table[record_slot(dom, rec->entry->target, rec->name)]; *at != NULL;
         at = &(*at)->hash_next)
        if (*at == rec) {
            *at = rec->hash_next;
            rec->hash_next = NULL;
            dom->attr_attached--;
            return;
        }
}

/* A new Attr, detached, with no value: its record and its object, the
 * object held by the registry. NULL with QuotaExceededError thrown. */
static DAttr *attr_make(os64_dom_t *dom, JSContext *ctx, const char *name, const char *ns)
{
    DAttr *rec = d_alloc(dom, sizeof(*rec));
    if (rec != NULL) {
        rec->name = copy_bytes(dom, name, os64_strlen(name));
        rec->ns = ns != NULL ? copy_bytes(dom, ns, os64_strlen(ns)) : NULL;
    }
    if (rec == NULL || rec->name == NULL || (ns != NULL && rec->ns == NULL)) {
        if (rec != NULL) { d_free(dom, rec->name); d_free(dom, rec->ns); d_free(dom, rec); }
        d_error(ctx, "QuotaExceededError", "DOM attribute quota exceeded");
        return NULL;
    }
    JSValue object = JS_NewObjectClass(ctx, (int)dom->attr_class);
    DValue *entry = JS_IsException(object) ? NULL : d_retain(dom, ctx, object);
    if (entry == NULL) {
        d_free(dom, rec->name);
        d_free(dom, rec->ns);
        d_free(dom, rec);
        return NULL;
    }
    entry->dom = dom;
    entry->attr = rec;
    rec->entry = entry;
    JS_SetOpaque(object, entry);
    return rec;
}

/* Onto the element, reading its value from the element from now on. */
static void attach(os64_dom_t *dom, DAttr *rec, const os64_html_node_t *element)
{
    rec->entry->target = element;
    record_add(dom, rec);
    os64_html_hold(dom->document, element);
    d_free(dom, rec->value);
    rec->value = NULL;
    rec->length = 0;
    rec->document = NULL;
}

/* Off it, keeping `value` (the record's from now on) and the document the
 * element was in. The element's own wrapper holds it (an Attr is reached
 * only through its element's wrapper or its map), so letting go of the
 * Attr's hold reclaims nothing. */
static void detach(os64_dom_t *dom, DAttr *rec, char *value, size_t length)
{
    record_remove(dom, rec);
    const os64_html_node_t *element = rec->entry->target;
    const os64_html_node_t *document = d_node_document(dom, element);
    rec->document = document != window_document(dom) ? document : NULL;
    rec->entry->target = NULL;
    os64_html_release(dom->document, element);
    d_free(dom, rec->value);
    rec->value = value;
    rec->length = length;
}

/* THE BACKSTOP: an attached Attr whose name its element no longer has was
 * removed by something other than the door. It reads as detached with an
 * empty value from here on, and the host hears of the broken rule. */
static void attr_live(os64_dom_t *dom, DAttr *rec)
{
    const os64_html_node_t *element = rec->entry->target;
    if (element == NULL || os64_html_attr(element, rec->name) != NULL) return;
    detach(dom, rec, NULL, 0);
    if (dom->options.broken != NULL) dom->options.broken(dom->options.host_opaque, kBroken);
}

/* The Attr standing for an attribute `element` has: the one made for its
 * name, else a new one. */
static JSValue attr_get(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *element,
                        const os64_html_attr_t *a)
{
    DAttr *rec = record_of(dom, element, a->name);
    if (rec == NULL) {
        rec = attr_make(dom, ctx, a->name, a->ns);
        if (rec == NULL) return JS_EXCEPTION;
        attach(dom, rec, element);
    }
    return JS_DupValue(ctx, rec->entry->value);
}

int64_t d_set_attr(os64_dom_t *dom, const os64_html_node_t *node, const char *name,
                   const char *value, size_t length, bool remove)
{
    DAttr *rec = remove ? record_of(dom, node, name) : NULL;
    const os64_html_attr_t *a = rec != NULL ? os64_html_attr(node, name) : NULL;
    if (rec != NULL && a == NULL) {
        attr_live(dom, rec);
        rec = NULL;
    }
    /* The value it leaves with is copied before it goes (the removal
     * retires libhtml's bytes), and the removal refused when there is no
     * room to copy it. */
    size_t kept_length = a != NULL ? os64_strlen(a->value) : 0;
    char *kept = a != NULL ? copy_bytes(dom, a->value, kept_length) : NULL;
    if (a != NULL && kept == NULL) return OS64_HTML_NO_MEMORY;
    int64_t status = os64_page_node_set_attr(dom->state, node, name, value, length, remove);
    if (status < 0) {
        d_free(dom, kept);
        return status;
    }
    if (rec != NULL) detach(dom, rec, kept, kept_length);
    return status;
}

bool d_is_attr(os64_dom_t *dom, JSValueConst value)
{
    return dom->attr_class != 0 && JS_GetOpaque(value, dom->attr_class) != NULL;
}

/* An Attr receiver's record, after the backstop has looked at it. NULL with
 * a TypeError (or the closed binding's error) thrown. */
static DAttr *attr_receiver(JSContext *ctx, JSValueConst self)
{
    DValue *entry = JS_GetOpaque(self, os64_js_class_id(&attr_slot));
    if (entry == NULL || entry->attr == NULL) {
        JS_ThrowTypeError(ctx, "Expected an Attr");
        return NULL;
    }
    if (!d_context(entry->dom, ctx, OS64_JS_ABI_ID)) return NULL;
    attr_live(entry->dom, entry->attr);
    return entry->attr;
}

/* Its value: the element's, read by name, while attached. */
static JSValue attr_value(JSContext *ctx, const DAttr *rec)
{
    const os64_html_node_t *element = rec->entry->target;
    if (element != NULL) return JS_NewString(ctx, os64_html_attr(element, rec->name)->value);
    return JS_NewStringLen(ctx, rec->value != NULL ? rec->value : "", rec->length);
}

/* DOM's "set an existing attribute value": the element's attribute through
 * the door while attached, else the Attr's own. `text` is converted before
 * the Attr is looked at, since converting it runs page code that may move
 * the Attr. */
static JSValue attr_value_set(os64_dom_t *dom, JSContext *ctx, DAttr *rec, DString *text)
{
    attr_live(dom, rec);
    const os64_html_node_t *element = rec->entry->target;
    if (element != NULL) {
        int64_t status = d_set_attr(dom, element, rec->name, text->data, text->length, false);
        return status < 0 ? d_html_error(ctx, status) : JS_UNDEFINED;
    }
    d_free(dom, rec->value);
    rec->value = text->data;
    rec->length = text->length;
    text->data = NULL;
    return JS_UNDEFINED;
}

/* A value a Node member takes as a node: a tree node of this document or an
 * Attr, or (`nullable`) null. 0, or -1 with the TypeError thrown. */
static int node_argument(os64_dom_t *dom, JSContext *ctx, JSValueConst value, bool nullable)
{
    if (d_is_attr(dom, value) || (nullable && JS_IsNull(value))) return 0;
    return d_node(dom, ctx, value) != NULL ? 0 : -1;
}

/* ── Attr's members, and Node's as an Attr answers them ───────────────── */

enum {
    A_NAME, A_LOCAL_NAME, A_PREFIX, A_NAMESPACE, A_VALUE, A_OWNER_ELEMENT, A_SPECIFIED,
    A_NODE_TYPE, A_NODE_VALUE, A_TEXT_CONTENT, A_NO_NODE, A_OWNER_DOCUMENT, A_CHILD_NODES,
    A_CONNECTED, A_APPEND, A_INSERT, A_REPLACE, A_REMOVE_CHILD, A_CLONE, A_HAS_CHILDREN, A_CONTAINS
};

static JSValue empty_child_nodes(os64_dom_t *dom, JSContext *ctx, DAttr *rec)
{
    if (rec->child_nodes == NULL) {
        JSValue list = JS_NewObjectProto(ctx, dom->static_list->value);
        if (JS_IsException(list)) return list;
        if (JS_DefinePropertyValueStr(ctx, list, "length", JS_NewUint32(ctx, 0), 0) < 0) {
            JS_FreeValue(ctx, list);
            return JS_EXCEPTION;
        }
        rec->child_nodes = d_retain(dom, ctx, list);
        if (rec->child_nodes == NULL) return JS_EXCEPTION;
    }
    return JS_DupValue(ctx, rec->child_nodes->value);
}

/* cloneNode: a detached Attr of the same name, namespace and value, in the
 * original's node document (DOM § 4.4, "clone a node"). */
static JSValue attr_clone(os64_dom_t *dom, JSContext *ctx, DAttr *rec)
{
    const os64_html_node_t *element = rec->entry->target;
    const char *value = element != NULL ? os64_html_attr(element, rec->name)->value :
                        rec->value != NULL ? rec->value : "";
    size_t length = element != NULL ? os64_strlen(value) : rec->length;
    char *copy = copy_bytes(dom, value, length);
    if (copy == NULL) return d_error(ctx, "QuotaExceededError", "DOM attribute quota exceeded");
    const os64_html_node_t *document = element != NULL ? d_node_document(dom, element) : rec->document;
    DAttr *made = attr_make(dom, ctx, rec->name, rec->ns);
    if (made == NULL) { d_free(dom, copy); return JS_EXCEPTION; }
    made->value = copy;
    made->length = length;
    made->document = document != window_document(dom) ? document : NULL;
    return JS_DupValue(ctx, made->entry->value);
}

static JSValue attr_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                           int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    JSValueConst arg = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (set) {
        /* The value converted first (it may run page code), then the Attr
         * looked at: `value` is a DOMString, and nodeValue and textContent
         * are nullable ones, null and undefined the empty string. */
        if (JS_GetOpaque(self, dom->attr_class) == NULL) return JS_ThrowTypeError(ctx, "Expected an Attr");
        DString text = {0};
        bool empty = magic != A_VALUE && (JS_IsNull(arg) || JS_IsUndefined(arg));
        if (!empty && !d_string(dom, ctx, arg, &text)) return JS_EXCEPTION;
        DAttr *rec = attr_receiver(ctx, self);
        JSValue result = rec == NULL ? JS_EXCEPTION : attr_value_set(dom, ctx, rec, &text);
        d_string_free(dom, &text);
        return result;
    }
    DAttr *rec = attr_receiver(ctx, self);
    if (rec == NULL) return JS_EXCEPTION;
    const os64_html_node_t *element = rec->entry->target;
    switch (magic) {
    case A_NAME: return JS_NewString(ctx, rec->name);
    case A_LOCAL_NAME: return JS_NewString(ctx, local_of(rec->name, rec->ns));
    case A_PREFIX: {
        const char *local = local_of(rec->name, rec->ns);
        return local == rec->name ? JS_NULL : JS_NewStringLen(ctx, rec->name, (size_t)(local - rec->name) - 1);
    }
    case A_NAMESPACE: return rec->ns != NULL ? JS_NewString(ctx, rec->ns) : JS_NULL;
    case A_VALUE: case A_NODE_VALUE: case A_TEXT_CONTENT: return attr_value(ctx, rec);
    case A_OWNER_ELEMENT: return element != NULL ? d_wrap(dom, ctx, element) : JS_NULL;
    case A_SPECIFIED: return JS_TRUE;
    case A_NODE_TYPE: return JS_NewInt32(ctx, 2);
    case A_NO_NODE: return JS_NULL;
    case A_OWNER_DOCUMENT:
        return d_wrap(dom, ctx, element != NULL ? d_node_document(dom, element) :
                                rec->document != NULL ? rec->document : window_document(dom));
    case A_CHILD_NODES: return empty_child_nodes(dom, ctx, rec);
    case A_CONNECTED: case A_HAS_CHILDREN: return JS_FALSE;
    case A_APPEND: case A_INSERT: case A_REPLACE:
        /* DOM's pre-insert and replace refuse an Attr as the parent before
         * looking at anything else; the arguments are WebIDL's Nodes. */
        if (argc < (magic == A_APPEND ? 1 : 2)) return JS_ThrowTypeError(ctx, "Missing node argument");
        if (node_argument(dom, ctx, argv[0], false) < 0 ||
            (magic != A_APPEND && node_argument(dom, ctx, argv[1], magic == A_INSERT) < 0))
            return JS_EXCEPTION;
        return d_error(ctx, "HierarchyRequestError", "An Attr has no children");
    case A_REMOVE_CHILD:
        if (argc < 1) return JS_ThrowTypeError(ctx, "Missing node argument");
        if (node_argument(dom, ctx, argv[0], false) < 0) return JS_EXCEPTION;
        return d_error(ctx, "NotFoundError", "Node is not a child of this parent");
    case A_CLONE: {
        int deep = JS_ToBool(ctx, arg);
        if (deep < 0) return JS_EXCEPTION;
        /* Converting `deep` runs no page code a boolean could not. */
        return attr_clone(dom, ctx, rec);
    }
    case A_CONTAINS:
        if (JS_IsNull(arg)) return JS_FALSE;
        if (node_argument(dom, ctx, arg, false) < 0) return JS_EXCEPTION;
        return JS_NewBool(ctx, JS_VALUE_GET_PTR(arg) == JS_VALUE_GET_PTR(self));
    default: return JS_ThrowTypeError(ctx, "Unknown Attr member");
    }
}

/* ── The attribute-node methods: Document's and Element's, and the map's ── */

enum {
    M_CREATE, M_CREATE_NS, M_GET, M_GET_NS, M_SET, M_SET_NS, M_REMOVE_NODE,
    M_ITEM, M_REMOVE_NAMED, M_REMOVE_NAMED_NS, M_LENGTH, M_ATTRIBUTES
};

#define NS_XML "http://www.w3.org/XML/1998/namespace"
#define NS_XMLNS "http://www.w3.org/2000/xmlns/"

static bool same(const char *text, size_t length, const char *word)
{
    size_t n = os64_strlen(word);
    return length == n && os64_memcmp(text, word, n) == 0;
}

/* createAttribute(localName): a name setAttribute would take, folded in an
 * HTML document; createAttributeNS(namespace, qualifiedName): DOM's
 * "validate and extract" for an attribute, split at the first colon as
 * createDocument splits an element's name (implementation.c), the local
 * part an attribute's name, then the namespace rules, each a
 * NamespaceError. Either is born detached, in this document, valued "". */
static JSValue create_attribute(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *document,
                                int magic, int argc, JSValueConst *argv)
{
    if (argc < (magic == M_CREATE ? 1 : 2)) return JS_ThrowTypeError(ctx, "Missing attribute name");
    DString ns = {0}, name = {0};
    bool has_ns = magic == M_CREATE_NS && !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0]);
    if (has_ns && !d_string(dom, ctx, argv[0], &ns)) return JS_EXCEPTION;
    if (!d_string(dom, ctx, argv[magic == M_CREATE ? 0 : 1], &name)) {
        d_string_free(dom, &ns);
        return JS_EXCEPTION;
    }
    if (has_ns && ns.length == 0) has_ns = false;
    JSValue result = JS_UNDEFINED;
    if (magic == M_CREATE) {
        if (!d_attribute_name_valid(name.data))
            result = d_error(ctx, "InvalidCharacterError", "Invalid attribute name");
        else if (d_document_type(dom, document) == D_DOCUMENT_HTML) d_fold(name.data);
    } else {
        const char *colon = os64_strchr(name.data, ':');
        size_t prefix = colon != NULL ? (size_t)(colon - name.data) : 0;
        const char *local = colon != NULL ? colon + 1 : name.data;
        bool xmlns = same(name.data, name.length, "xmlns") || (colon != NULL && same(name.data, prefix, "xmlns"));
        bool is_xmlns_ns = has_ns && same(ns.data, ns.length, NS_XMLNS);
        if ((colon != NULL && !d_prefix_valid(name.data, prefix)) || !d_attribute_name_valid(local))
            result = d_error(ctx, "InvalidCharacterError", "That is not an attribute's qualified name");
        else if ((colon != NULL && !has_ns) ||
                 (colon != NULL && same(name.data, prefix, "xml") && !(has_ns && same(ns.data, ns.length, NS_XML))) ||
                 xmlns != is_xmlns_ns)
            result = d_error(ctx, "NamespaceError", "That name and namespace do not go together");
    }
    DAttr *rec = JS_IsUndefined(result) ? attr_make(dom, ctx, name.data, has_ns ? ns.data : NULL) : NULL;
    d_string_free(dom, &name);
    d_string_free(dom, &ns);
    if (rec == NULL) return JS_EXCEPTION;
    rec->document = document != window_document(dom) ? document : NULL;
    return JS_DupValue(ctx, rec->entry->value);
}

/* setAttributeNode(attr), setAttributeNodeNS, setNamedItem and
 * setNamedItemNS (DOM § 4.9, "set an attribute"). */
static JSValue set_attribute_node(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *element,
                                  JSValueConst value)
{
    DAttr *rec = d_is_attr(dom, value) ? attr_receiver(ctx, value) : NULL;
    if (rec == NULL) {
        if (!JS_HasException(ctx)) JS_ThrowTypeError(ctx, "Expected an Attr");
        return JS_EXCEPTION;
    }
    const os64_html_node_t *at = rec->entry->target;
    if (at != NULL && at != element)
        return d_error(ctx, "InUseAttributeError", "That Attr belongs to another element");
    if (at == element) return JS_DupValue(ctx, value);
    /* The attribute it replaces is the one of its namespace and local name;
     * libhtml's verb sets the one of its qualified name, keeping that one's
     * namespace or giving a new one none. Unless those agree, the tree would
     * hold the attribute under another name or namespace than the Attr's. */
    const os64_html_attr_t *old = attr_ns(element, rec->ns, local_of(rec->name, rec->ns));
    if (old != os64_html_attr(element, rec->name) || (old == NULL && rec->ns != NULL))
        return d_error(ctx, "NotSupportedError",
                       "This element cannot hold that Attr under both its name and its namespace");
    /* Everything that can be refused is asked for before the tree changes:
     * the Attr the old attribute goes as, its last value, and the handler
     * slot an on<type> name takes. */
    JSValue previous = old != NULL ? attr_get(dom, ctx, element, old) : JS_NULL;
    if (JS_IsException(previous)) return previous;
    DAttr *gone = old != NULL ? record_of(dom, element, old->name) : NULL;
    size_t kept_length = gone != NULL ? os64_strlen(old->value) : 0;
    char *kept = gone != NULL ? copy_bytes(dom, old->value, kept_length) : NULL;
    if (gone != NULL && kept == NULL) {
        JS_FreeValue(ctx, previous);
        return d_error(ctx, "QuotaExceededError", "DOM attribute quota exceeded");
    }
    if (d_handler_attribute_set(dom, ctx, element, rec->name) < 0) {
        d_free(dom, kept);
        JS_FreeValue(ctx, previous);
        return JS_EXCEPTION;
    }
    int64_t status = d_set_attr(dom, element, rec->name, rec->value != NULL ? rec->value : "", rec->length, false);
    if (status < 0) {
        d_free(dom, kept);
        JS_FreeValue(ctx, previous);
        return d_html_error(ctx, status);
    }
    if (gone != NULL) detach(dom, gone, kept, kept_length);
    attach(dom, rec, element);
    return previous;
}

/* removeAttributeNode(attr): this element's own, removed and answered,
 * detached; anything else NotFoundError. */
static JSValue remove_attribute_node(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *element,
                                     JSValueConst value)
{
    DAttr *rec = d_is_attr(dom, value) ? attr_receiver(ctx, value) : NULL;
    if (rec == NULL) {
        if (!JS_HasException(ctx)) JS_ThrowTypeError(ctx, "Expected an Attr");
        return JS_EXCEPTION;
    }
    if (rec->entry->target != element) return d_error(ctx, "NotFoundError", "That Attr is not this element's");
    int64_t status = d_set_attr(dom, element, rec->name, NULL, 0, true);
    return status < 0 ? d_html_error(ctx, status) : JS_DupValue(ctx, value);
}

/* The attribute a script's qualified name names on `element`: folded for
 * an HTML element, first match. */
static const os64_html_attr_t *attr_named(const os64_html_node_t *element, DString *name)
{
    if (folds(element)) d_fold(name->data);
    return os64_html_attr(element, name->data);
}

/* getNamedItemNS and friends' (namespace, localName): the namespace's null
 * and "" are none. 0, or -1 with the exception pending. */
static int ns_arguments(os64_dom_t *dom, JSContext *ctx, int argc, JSValueConst *argv,
                        DString *ns, DString *local)
{
    if (argc < 2) { JS_ThrowTypeError(ctx, "Missing namespace and local name"); return -1; }
    if (!JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0]) && !d_string(dom, ctx, argv[0], ns)) return -1;
    if (!d_string(dom, ctx, argv[1], local)) { d_string_free(dom, ns); return -1; }
    if (ns->data != NULL && ns->length == 0) d_string_free(dom, ns);
    return 0;
}

/* The verbs, on an element (Element's methods, and the map's, whose
 * element is its own). */
static JSValue element_verb(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *element, int magic,
                            int argc, JSValueConst *argv)
{
    if (magic == M_SET || magic == M_SET_NS) {
        if (argc < 1) return JS_ThrowTypeError(ctx, "Missing Attr argument");
        return set_attribute_node(dom, ctx, element, argv[0]);
    }
    if (magic == M_REMOVE_NODE) {
        if (argc < 1) return JS_ThrowTypeError(ctx, "Missing Attr argument");
        return remove_attribute_node(dom, ctx, element, argv[0]);
    }
    if (magic == M_GET_NS || magic == M_REMOVE_NAMED_NS) {
        DString ns = {0}, local = {0};
        if (ns_arguments(dom, ctx, argc, argv, &ns, &local) < 0) return JS_EXCEPTION;
        const os64_html_attr_t *a = attr_ns(element, ns.data, local.data);
        d_string_free(dom, &ns);
        d_string_free(dom, &local);
        if (a == NULL)
            return magic == M_GET_NS ? JS_NULL : d_error(ctx, "NotFoundError", "No such attribute");
        if (magic == M_GET_NS) return attr_get(dom, ctx, element, a);
        /* The name is the binding's own copy before the removal retires
         * libhtml's bytes. */
        DString name = {0};
        name.length = os64_strlen(a->name);
        name.data = copy_bytes(dom, a->name, name.length);
        if (name.data == NULL) return d_error(ctx, "QuotaExceededError", "DOM attribute quota exceeded");
        JSValue gone = attr_get(dom, ctx, element, a);
        int64_t status = JS_IsException(gone) ? OS64_HTML_OK : d_set_attr(dom, element, name.data, NULL, 0, true);
        d_string_free(dom, &name);
        if (status < 0) { JS_FreeValue(ctx, gone); return d_html_error(ctx, status); }
        return gone;
    }
    /* M_GET and M_REMOVE_NAMED: a qualified name. */
    if (argc < 1) return JS_ThrowTypeError(ctx, "Missing attribute name");
    DString name = {0};
    if (!d_string(dom, ctx, argv[0], &name)) return JS_EXCEPTION;
    const os64_html_attr_t *a = attr_named(element, &name);
    JSValue result;
    if (a == NULL) {
        result = magic == M_GET ? JS_NULL : d_error(ctx, "NotFoundError", "No such attribute");
    } else {
        result = attr_get(dom, ctx, element, a);
        int64_t status = magic == M_REMOVE_NAMED && !JS_IsException(result)
            ? d_set_attr(dom, element, name.data, NULL, 0, true) : OS64_HTML_OK;
        if (status < 0) { JS_FreeValue(ctx, result); result = d_html_error(ctx, status); }
    }
    d_string_free(dom, &name);
    return result;
}

/* The element's NamedNodeMap, made the first time it is asked for. */
static JSValue attributes_of(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *element)
{
    DValue *owner = d_find(dom, element);
    if (owner == NULL) return JS_ThrowTypeError(ctx, "Missing node wrapper");
    if (owner->attributes != NULL) return JS_DupValue(ctx, owner->attributes->value);
    JSValue object = JS_NewObjectClass(ctx, (int)dom->named_map_class);
    DValue *entry = JS_IsException(object) ? NULL : d_retain(dom, ctx, object);
    if (entry == NULL) return JS_EXCEPTION;
    entry->dom = dom;
    entry->target = element;
    os64_html_hold(dom->document, element);
    JS_SetOpaque(object, entry);
    owner->attributes = entry;
    return JS_DupValue(ctx, object);
}

static JSValue node_verb(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                         int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *node = d_node(dom, ctx, self);
    if (node == NULL) return JS_EXCEPTION;
    if (magic == M_CREATE || magic == M_CREATE_NS) {
        if (node->kind != OS64_HTML_DOCUMENT) return JS_ThrowTypeError(ctx, "Creation requires a document");
        return create_attribute(dom, ctx, node, magic, argc, argv);
    }
    if (node->kind != OS64_HTML_ELEMENT) return JS_ThrowTypeError(ctx, "Attribute method requires an element");
    if (magic == M_ATTRIBUTES) return attributes_of(dom, ctx, node);
    return element_verb(dom, ctx, node, magic, argc, argv);
}

/* ── NamedNodeMap ─────────────────────────────────────────────────────── */

static DValue *map_receiver(JSContext *ctx, JSValueConst object)
{
    DValue *entry = JS_GetOpaque(object, os64_js_class_id(&map_slot));
    if (entry == NULL) {
        JS_ThrowTypeError(ctx, "Expected a NamedNodeMap");
        return NULL;
    }
    return d_context(entry->dom, ctx, OS64_JS_ABI_ID) ? entry : NULL;
}

/* THE WALK'S CURSOR. An element's attributes are a list, so the i-th is i
 * steps from the first, and its length a walk of it all; a walk over them
 * all (an iteration, which reads `length` at every step, a countdown from
 * `length`, Object.keys, for..in) would cost n² steps in one call. So the
 * binding keeps the last element walked as an array of its attributes,
 * with their count: `length` and the i-th are a step, either way round.
 * The array is good only while the element's list is the one it copied:
 * the same element, libhtml's version unmoved, the list starting where it
 * did and its last attribute still the last (a parser's late attribute on
 * html or body goes on the end without moving the version). Nothing is
 * trusted across page code: every use asks again, and the array is made
 * again when the answer is no. It is charged to the binding's budget; with
 * no room for it, the list is walked as it is. */
static bool cursor_good(const os64_dom_t *dom, const os64_html_node_t *element)
{
    const struct DAttrCursor *c = &dom->attr_cursor;
    return c->element == element && c->head == element->attrs &&
           c->version == os64_html_version(dom->document) &&
           (c->count == 0 || c->list[c->count - 1]->next == NULL);
}

/* The cursor made for `element`, or false when there is no room for it. */
static bool cursor_for(os64_dom_t *dom, const os64_html_node_t *element)
{
    if (cursor_good(dom, element)) return true;
    struct DAttrCursor *c = &dom->attr_cursor;
    c->element = NULL;
    size_t count = 0;
    for (const os64_html_attr_t *a = element->attrs; a != NULL && count < UINT32_MAX; a = a->next) {
        count++;
        s_steps++;
    }
    if (count > c->capacity) {
        size_t capacity = c->capacity * 2 > count ? c->capacity * 2 : count < 16 ? 16 : count;
        const os64_html_attr_t **list = capacity <= SIZE_MAX / sizeof(*list) ?
            d_alloc(dom, capacity * sizeof(*list)) : NULL;
        if (list == NULL) return false;
        d_free(dom, c->list);
        c->list = list;
        c->capacity = capacity;
    }
    size_t i = 0;
    for (const os64_html_attr_t *a = element->attrs; i < count; a = a->next) c->list[i++] = a;
    c->count = (uint32_t)count;
    c->found = 0;
    c->element = element;
    c->head = element->attrs;
    c->version = os64_html_version(dom->document);
    return true;
}

/* The index-th attribute. */
static const os64_html_attr_t *attr_at(os64_dom_t *dom, const os64_html_node_t *element, uint32_t index)
{
    if (cursor_for(dom, element)) return index < dom->attr_cursor.count ? dom->attr_cursor.list[index] : NULL;
    const os64_html_attr_t *a = element->attrs;
    for (; a != NULL && index != 0; index--, s_steps++) a = a->next;
    return a;
}

static uint32_t attr_count(os64_dom_t *dom, const os64_html_node_t *element)
{
    if (cursor_for(dom, element)) return dom->attr_cursor.count;
    uint32_t count = 0;
    for (const os64_html_attr_t *a = element->attrs; a != NULL && count < UINT32_MAX; a = a->next, s_steps++)
        count++;
    return count;
}

/* Whether the map lists a name (DOM § 4.9.1's supported property names):
 * an HTML element's names with an ASCII uppercase letter are not listed,
 * since its named getter folds and could never answer them. libhtml holds
 * no two attributes of one name, so no name is listed twice. */
static bool name_listed(const os64_html_node_t *element, const char *name, size_t length)
{
    for (size_t i = 0; folds(element) && i < length; i++)
        if (name[i] >= 'A' && name[i] <= 'Z') return false;
    return true;
}

/* The attribute a listed property name names, NULL for none: looked for
 * in the cursor outward from the last one found, a step after it and a
 * step before it in turn, so names asked for in the list's order, either
 * way round, cost a step or two each. */
static const os64_html_attr_t *listed(os64_dom_t *dom, const os64_html_node_t *element, const char *name,
                                      size_t length)
{
    if (!name_listed(element, name, length)) return NULL;
    if (!cursor_for(dom, element)) {
        for (const os64_html_attr_t *a = element->attrs; a != NULL; a = a->next, s_steps++)
            if (os64_strlen(a->name) == length && os64_memcmp(a->name, name, length) == 0) return a;
        return NULL;
    }
    struct DAttrCursor *c = &dom->attr_cursor;
    if (c->count == 0) return NULL;
    uint32_t from = c->found < c->count ? c->found : 0;
    for (uint32_t d = 0; d <= c->count / 2; d++)
        for (int side = 0; side < 2; side++) {
            if (side == 1 && (d == 0 || 2 * d == c->count)) continue;
            uint32_t i = side == 0 ? (from + d) % c->count : (from + c->count - d) % c->count;
            const os64_html_attr_t *a = c->list[i];
            s_steps++;
            if (os64_strlen(a->name) == length && os64_memcmp(a->name, name, length) == 0) {
                c->found = i;
                return a;
            }
        }
    return NULL;
}

/* A property key's text, as the engine holds it: 1, 0 for a Symbol, -1
 * with an exception pending. The caller frees it with JS_FreeCString. */
static int key_text(JSContext *ctx, JSAtom atom, const char **text, size_t *length)
{
    JSValue key = JS_AtomToValue(ctx, atom);
    if (JS_IsException(key)) return -1;
    if (!JS_IsString(key)) { JS_FreeValue(ctx, key); return 0; }
    *text = JS_ToCStringLen(ctx, length, key);
    JS_FreeValue(ctx, key);
    return *text != NULL ? 1 : -1;
}

/* WebIDL's named property visibility: a listed name is a property of the
 * map only when its prototype chain has none by that name, so
 * `attributes.item` is always the method. 1, 0, or -1 on an exception. */
static int visible(JSContext *ctx, JSValueConst object, JSAtom atom)
{
    JSValue prototype = JS_GetPrototype(ctx, object);
    if (JS_IsException(prototype)) return -1;
    int shadowed = JS_IsNull(prototype) ? 0 : JS_HasProperty(ctx, prototype, atom);
    JS_FreeValue(ctx, prototype);
    return shadowed < 0 ? -1 : !shadowed;
}

/* The attribute a non-index key names visibly: 1 (with *found), 0, -1.
 * The prototype chain is asked first, as a collection asks it, so a read of
 * the map's own members (`length`, `item`) costs no walk of the list; and
 * since asking it may run page code (a Proxy prototype's `has`) that changes
 * the element's attributes, the list is read after it. */
static int named(os64_dom_t *dom, JSContext *ctx, JSValueConst object, const os64_html_node_t *element,
                 JSAtom atom, const os64_html_attr_t **found)
{
    const char *text;
    size_t length;
    int is = key_text(ctx, atom, &text, &length);
    if (is <= 0) return is;
    int shown = visible(ctx, object, atom);
    *found = shown > 0 ? listed(dom, element, text, length) : NULL;
    JS_FreeCString(ctx, text);
    return shown < 0 ? -1 : *found != NULL;
}

static int map_own_property(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst object, JSAtom atom)
{
    DValue *entry = map_receiver(ctx, object);
    if (entry == NULL) return -1;
    const os64_html_node_t *element = entry->target;
    uint32_t index;
    int numeric = d_index_key(ctx, atom, &index);
    if (numeric < 0) return -1;
    const os64_html_attr_t *a = NULL;
    if (numeric) {
        a = attr_at(entry->dom, element, index);
        if (a == NULL) return 0;
    } else {
        int found = named(entry->dom, ctx, object, element, atom, &a);
        if (found <= 0) return found;
    }
    if (desc != NULL) {
        JSValue value = attr_get(entry->dom, ctx, element, a);
        if (JS_IsException(value)) return -1;
        /* No setter and no deleter; the names are not enumerable
         * ([LegacyUnenumerableNamedProperties]). */
        *desc = (JSPropertyDescriptor){.flags = JS_PROP_CONFIGURABLE | (numeric ? JS_PROP_ENUMERABLE : 0),
                                       .value = value, .getter = JS_UNDEFINED, .setter = JS_UNDEFINED};
    }
    return 1;
}

static int map_own_names(JSContext *ctx, JSPropertyEnum **table, uint32_t *length, JSValueConst object)
{
    DValue *entry = map_receiver(ctx, object);
    if (entry == NULL) return -1;
    const os64_html_node_t *element = entry->target;
    uint32_t count = attr_count(entry->dom, element);
    if ((size_t)count * 2 > SIZE_MAX / sizeof(**table)) {
        d_error(ctx, "QuotaExceededError", "Too many attributes");
        return -1;
    }
    JSPropertyEnum *names = count != 0 ? js_malloc(ctx, (size_t)count * 2 * sizeof(*names)) : NULL;
    if (count != 0 && names == NULL) return -1;
    /* The indices, then every listed name that is not an index itself, as
     * the list stands now; each name's visibility after, since asking it
     * may run page code, and the names are the list as it was asked for. */
    uint32_t used = 0, i = 0;
    for (const os64_html_attr_t *a = element->attrs; a != NULL && i < count; a = a->next, i++) {
        names[used].is_enumerable = true;
        names[used].atom = JS_NewAtomUInt32(ctx, i);
        if (names[used].atom == JS_ATOM_NULL) goto fail;
        used++;
    }
    uint32_t indices = used;
    i = 0;
    for (const os64_html_attr_t *a = element->attrs; a != NULL && i < count; a = a->next, i++) {
        if (!name_listed(element, a->name, os64_strlen(a->name))) continue;
        JSAtom atom = JS_NewAtom(ctx, a->name);
        if (atom == JS_ATOM_NULL) goto fail;
        uint32_t ignored;
        int numeric = d_index_key(ctx, atom, &ignored);
        if (numeric != 0) {
            JS_FreeAtom(ctx, atom);
            if (numeric < 0) goto fail;
            continue;
        }
        names[used].is_enumerable = false;
        names[used++].atom = atom;
    }
    uint32_t kept = indices;
    for (uint32_t k = indices; k < used; k++) {
        int shown = visible(ctx, object, names[k].atom);
        if (shown < 0) {
            for (uint32_t r = k; r < used; r++) JS_FreeAtom(ctx, names[r].atom);
            used = kept;
            goto fail;
        }
        if (!shown) JS_FreeAtom(ctx, names[k].atom);
        else names[kept++] = names[k];
    }
    *table = names;
    *length = kept;
    return 0;
fail:
    while (used != 0) JS_FreeAtom(ctx, names[--used].atom);
    js_free(ctx, names);
    return -1;
}

/* WebIDL's [[DefineOwnProperty]] for a map with no setters: an index, or a
 * name the map lists, is refused (the engine says how, against a
 * non-extensible object); anything else is an ordinary property. */
static int map_define(JSContext *ctx, JSValueConst object, JSAtom atom, JSValueConst value,
                      JSValueConst getter, JSValueConst setter, int flags)
{
    DValue *entry = map_receiver(ctx, object);
    if (entry == NULL) return -1;
    uint32_t index;
    int numeric = d_index_key(ctx, atom, &index);
    if (numeric < 0) return -1;
    bool refused = numeric != 0;
    if (!refused) {
        const char *text;
        size_t length;
        int is = key_text(ctx, atom, &text, &length);
        if (is < 0) return -1;
        if (is > 0) {
            refused = listed(entry->dom, entry->target, text, length) != NULL;
            JS_FreeCString(ctx, text);
        }
    }
    return JS_DefineProperty(ctx, refused ? entry->dom->index_guard->value : object, atom, value, getter,
                             setter, flags | JS_PROP_NO_EXOTIC);
}

/* [[Delete]]: an index the map has, or a visible name, cannot go (there is
 * no deleter); anything else is ordinary and already gone. */
static int map_delete(JSContext *ctx, JSValueConst object, JSAtom atom)
{
    int present = map_own_property(ctx, NULL, object, atom);
    return present < 0 ? -1 : !present;
}

static JSClassExoticMethods map_exotic = {
    .get_own_property = map_own_property, .get_own_property_names = map_own_names,
    .define_own_property = map_define, .delete_property = map_delete,
};

static JSValue map_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                          int magic, JSValue *data)
{
    if (d_callback(ctx, data, OS64_JS_ABI_ID) == NULL) return JS_EXCEPTION;
    DValue *entry = map_receiver(ctx, self);
    if (entry == NULL) return JS_EXCEPTION;
    const os64_html_node_t *element = entry->target;
    if (magic == M_LENGTH) return JS_NewUint32(ctx, attr_count(entry->dom, element));
    if (magic == M_ITEM) {
        if (argc < 1) return JS_ThrowTypeError(ctx, "Missing index");
        uint32_t index;
        if (JS_ToUint32(ctx, &index, argv[0]) < 0) return JS_EXCEPTION;
        /* The conversion may have run page code: the list is walked after. */
        const os64_html_attr_t *a = attr_at(entry->dom, element, index);
        return a != NULL ? attr_get(entry->dom, ctx, element, a) : JS_NULL;
    }
    return element_verb(entry->dom, ctx, element, magic, argc, argv);
}

static void attr_finalizer(JSRuntime *rt, JSValue value)
{
    (void)rt;
    JS_SetOpaque(value, NULL);
}

int d_attr_classic(os64_dom_t *dom, JSContext *ctx)
{
    dom->attr_capacity = 64;
    dom->attr_table = d_alloc(dom, dom->attr_capacity * sizeof(*dom->attr_table));
    if (dom->attr_table == NULL) {
        dom->attr_capacity = 0;
        d_error(ctx, "QuotaExceededError", "DOM attribute table quota exceeded");
        return -1;
    }
    dom->attr_class = os64_js_class_id(&attr_slot);
    dom->named_map_class = os64_js_class_id(&map_slot);
    static const JSClassDef attr_definition = {.class_name = "Attr", .finalizer = attr_finalizer};
    static const JSClassDef map_definition = {.class_name = "NamedNodeMap", .finalizer = attr_finalizer,
                                              .exotic = &map_exotic};
    if ((!JS_IsRegisteredClass(dom->engine, dom->attr_class) &&
         JS_NewClass(dom->engine, dom->attr_class, &attr_definition) < 0) ||
        (!JS_IsRegisteredClass(dom->engine, dom->named_map_class) &&
         JS_NewClass(dom->engine, dom->named_map_class, &map_definition) < 0))
        return -1;
    /* Attr.prototype: Attr's own members, then every Node member as an Attr
     * answers it (tools/test_dom_attributes.inc holds the shadow to cover
     * Node.prototype whole), then EventTarget's, an Attr being a plain
     * event target whose path is itself. */
    JSValueConst attr = dom->prototypes[D_PROTO_ATTR]->value;
    JS_SetClassProto(ctx, dom->attr_class, JS_DupValue(ctx, attr));
    static const struct { const char *name; int magic; bool writable; } kAccessors[] = {
        {"namespaceURI", A_NAMESPACE, false}, {"prefix", A_PREFIX, false},
        {"localName", A_LOCAL_NAME, false}, {"name", A_NAME, false}, {"value", A_VALUE, true},
        {"ownerElement", A_OWNER_ELEMENT, false}, {"specified", A_SPECIFIED, false},
        {"nodeType", A_NODE_TYPE, false}, {"nodeName", A_NAME, false},
        {"nodeValue", A_NODE_VALUE, true}, {"textContent", A_TEXT_CONTENT, true},
        {"parentNode", A_NO_NODE, false}, {"parentElement", A_NO_NODE, false},
        {"firstChild", A_NO_NODE, false}, {"lastChild", A_NO_NODE, false},
        {"previousSibling", A_NO_NODE, false}, {"nextSibling", A_NO_NODE, false},
        {"ownerDocument", A_OWNER_DOCUMENT, false}, {"childNodes", A_CHILD_NODES, false},
        {"isConnected", A_CONNECTED, false},
    };
    static const struct { const char *name; int magic, argc; } kMethods[] = {
        {"appendChild", A_APPEND, 1}, {"insertBefore", A_INSERT, 2}, {"replaceChild", A_REPLACE, 2},
        {"removeChild", A_REMOVE_CHILD, 1}, {"cloneNode", A_CLONE, 0},
        {"hasChildNodes", A_HAS_CHILDREN, 0}, {"contains", A_CONTAINS, 1},
    };
    for (size_t i = 0; i < sizeof(kAccessors) / sizeof(kAccessors[0]); i++)
        if (d_accessor(dom, ctx, attr, kAccessors[i].name, attr_member, kAccessors[i].magic,
                       kAccessors[i].writable) < 0) return -1;
    for (size_t i = 0; i < sizeof(kMethods) / sizeof(kMethods[0]); i++)
        if (d_method(dom, ctx, attr, kMethods[i].name, attr_member, kMethods[i].argc, kMethods[i].magic) < 0)
            return -1;
    if (d_event_target(dom, ctx, attr, dom->attr_class, NULL) < 0) return -1;
    /* NamedNodeMap.prototype. */
    JSValue map = JS_NewObject(ctx);
    dom->named_map_prototype = d_retain(dom, ctx, map);
    if (dom->named_map_prototype == NULL) return -1;
    JS_SetClassProto(ctx, dom->named_map_class, JS_DupValue(ctx, map));
    static const struct { const char *name; int magic, argc; } kMap[] = {
        {"item", M_ITEM, 1}, {"getNamedItem", M_GET, 1}, {"getNamedItemNS", M_GET_NS, 2},
        {"setNamedItem", M_SET, 1}, {"setNamedItemNS", M_SET_NS, 1},
        {"removeNamedItem", M_REMOVE_NAMED, 1}, {"removeNamedItemNS", M_REMOVE_NAMED_NS, 2},
    };
    if (d_accessor(dom, ctx, map, "length", map_member, M_LENGTH, false) < 0) return -1;
    for (size_t i = 0; i < sizeof(kMap) / sizeof(kMap[0]); i++)
        if (d_method(dom, ctx, map, kMap[i].name, map_member, kMap[i].argc, kMap[i].magic) < 0) return -1;
    if (d_iterable(ctx, map, false) < 0) return -1;
    /* Element's and Document's. */
    JSValueConst element = dom->prototypes[D_PROTO_ELEMENT]->value;
    JSValueConst document = dom->prototypes[D_PROTO_DOCUMENT]->value;
    static const struct { const char *name; int magic, argc; bool on_document; } kVerbs[] = {
        {"createAttribute", M_CREATE, 1, true}, {"createAttributeNS", M_CREATE_NS, 2, true},
        {"getAttributeNode", M_GET, 1, false}, {"getAttributeNodeNS", M_GET_NS, 2, false},
        {"setAttributeNode", M_SET, 1, false}, {"setAttributeNodeNS", M_SET_NS, 1, false},
        {"removeAttributeNode", M_REMOVE_NODE, 1, false},
    };
    for (size_t i = 0; i < sizeof(kVerbs) / sizeof(kVerbs[0]); i++)
        if (d_method(dom, ctx, kVerbs[i].on_document ? document : element, kVerbs[i].name, node_verb,
                     kVerbs[i].argc, kVerbs[i].magic) < 0) return -1;
    return d_accessor(dom, ctx, element, "attributes", node_verb, M_ATTRIBUTES, false);
}

JSValue d_attr_map_interface(os64_dom_t *dom, JSContext *ctx)
{
    return d_interface_new(ctx, "NamedNodeMap", 0, NULL, dom->named_map_prototype->value, JS_UNDEFINED);
}

void d_attr_table_free(os64_dom_t *dom)
{
    d_free(dom, dom->attr_table);
    dom->attr_table = NULL;
    d_free(dom, dom->attr_cursor.list);
    dom->attr_cursor.list = NULL;
    dom->attr_cursor.element = NULL;
    dom->attr_capacity = dom->attr_attached = 0;
}

void d_attr_free(os64_dom_t *dom, DValue *entry)
{
    DAttr *rec = entry->attr;
    if (rec == NULL) return;
    d_free(dom, rec->name);
    d_free(dom, rec->ns);
    d_free(dom, rec->value);
    d_free(dom, rec);
    entry->attr = NULL;
}
