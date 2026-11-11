#include "internal.h"

/* DOCUMENTS OF THEIR OWN (DOM § 4.5.1, DOMImplementation): what jQuery's
 * parseHTML builds its nodes in and its feature test reads, so that markup
 * a script parses runs nothing and fetches nothing until it is put in the
 * page.
 *
 * A NEW DOCUMENT LIVES IN THE PAGE'S ARENA. libhtml makes it a second
 * DOCUMENT node there (os64_html_create_document), so its nodes are the
 * page's nodes as far as every verb, hold and wrapper is concerned: a node
 * made in it and appended to the page is moved, not copied, and keeps its
 * identity, its wrapper and its listeners, which is what adopting it means
 * (DOM § 4.2.3, "adopt"). It is freed with the page, as a fragment is,
 * and charged to the same caps.
 *
 * IT HAS NO WINDOW. Its location is null, it is cookie-averse, it cannot be
 * written to, a script connected to it is marked started and never runs, a
 * handler attribute in it is not compiled, a link, a form's submission, a
 * focus or a blur in it asks nothing of the host, and a form's reset there
 * fires its event and resets no control (DEBTS.md). Its body reflects no
 * handler onto window, and scripting is off for its nodes (d_scripting).
 *
 * Native metadata records the current Document even for unwrapped detached
 * nodes. Wrapper records retain identity and creation-realm prototypes. */

static JSClassID implementation_slot;

static const os64_html_node_t *window_document(os64_dom_t *dom)
{
    return dom->document->document;
}

const os64_html_node_t *d_node_document(os64_dom_t *dom, const os64_html_node_t *node)
{
    (void)dom;
    return os64_html_node_document(node);
}

bool d_window_document(os64_dom_t *dom, const os64_html_node_t *node)
{
    return d_node_document(dom, node) == window_document(dom);
}

bool d_scripting(os64_dom_t *dom, const os64_html_node_t *node)
{
    dom = d_owner(dom, node);
    return dom->options.scripting && d_window_document(dom, node);
}

int64_t d_made_in(os64_dom_t *dom, const os64_html_node_t *node, const os64_html_node_t *document)
{
    dom = d_owner(dom, document);
    int64_t status = OS64_HTML_OK;
    if (os64_html_node_document(node) != document)
        status = os64_html_adopt(dom->document, (os64_html_node_t *)document, (os64_html_node_t *)node);
    return status;
}

JSValue d_wrap_in(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node,
                   const os64_html_node_t *document)
{
    int64_t status = d_made_in(dom, node, document);
    return status == OS64_HTML_OK ? d_wrap(dom, ctx, node) : d_html_error(ctx, status);
}

int d_document_type(os64_dom_t *dom, const os64_html_node_t *node)
{
    DValue *entry = node->kind == OS64_HTML_DOCUMENT ? d_find(dom, node) : NULL;
    return entry != NULL ? entry->document_type : D_DOCUMENT_HTML;
}

/* The DOMImplementation whose `this` this is: the registry entry of its
 * document's wrapper. NULL with a TypeError thrown. */
static DValue *implementation_of(JSContext *ctx, JSValueConst self)
{
    DValue *owner = JS_GetOpaque(self, os64_js_class_id(&implementation_slot));
    if (owner == NULL || owner->node == NULL) {
        JS_ThrowTypeError(ctx, "Expected a DOMImplementation");
        return NULL;
    }
    return owner;
}

/* Additional Documents remain arena-owned even when a wrapper was never
 * requested. Detached nodes and template associations can still name them. */
static os64_html_node_t *native_document(os64_dom_t *dom, JSContext *ctx)
{
    int64_t status;
    os64_html_node_t *document = os64_html_create_document(dom->document, &status);
    if (!document) { d_html_error(ctx, status); return NULL; }
    status = os64_html_document_keep(dom->document, document);
    if (status != OS64_HTML_OK) { d_html_error(ctx, status); return NULL; }
    return document;
}

int d_document_templates(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *document)
{
    os64_html_node_t *inert = native_document(dom, ctx);
    if (!inert) return -1;
    int64_t status = os64_html_document_templates(dom->document, (os64_html_node_t *)document, inert);
    if (status != OS64_HTML_OK) { d_html_error(ctx, status); return -1; }
    return 0;
}

static os64_html_node_t *document_made(os64_dom_t *dom, JSContext *ctx)
{
    os64_html_node_t *document = native_document(dom, ctx);
    if (!document) return NULL;
    dom->documents++;
    return d_document_templates(dom, ctx, document) == 0 ? document : NULL;
}

/* The new document's wrapper, which records what kind of document it is. */
static JSValue document_new(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *document, int type)
{
    JSValue value = d_wrap(dom, ctx, document);
    if (JS_IsException(value)) return value;
    d_find(dom, document)->document_type = (uint8_t)type;
    return value;
}

/* Created, or the refusal thrown: an element, a doctype, a text. NULL when
 * libhtml refused, the status said by d_html_error. The status is read
 * through its address, here, after the creating call in the same argument
 * list has written it: C does not order the arguments' evaluation. */
static os64_html_node_t *made(JSContext *ctx, os64_html_node_t *node, const int64_t *status)
{
    if (node == NULL) d_html_error(ctx, *status);
    return node;
}

/* createHTMLDocument(title) (DOM § 4.5.1): `<!DOCTYPE html><html><head>
 * <title>title</title></head><body></body></html>`, the title element and
 * its text only when a title was given (the empty string is one). The
 * nodes are new and none is a form control, so libhtml's own verbs build
 * the tree: nothing of the page's control state can move. */
static JSValue create_html_document(os64_dom_t *dom, JSContext *ctx, int argc, JSValueConst *argv)
{
    DString title = {0};
    bool titled = argc > 0 && !JS_IsUndefined(argv[0]);
    if (titled && !d_string(dom, ctx, argv[0], &title)) return JS_EXCEPTION;
    int64_t status = OS64_HTML_OK;
    os64_html_document_t *doc = dom->document;
    os64_html_node_t *document = NULL, *doctype = NULL, *html = NULL, *head = NULL, *body = NULL,
                     *title_element = NULL, *text = NULL;
    bool built = (document = document_made(dom, ctx)) != NULL &&
        (doctype = made(ctx, os64_html_create_doctype(doc, "html", "", "", &status), &status)) != NULL &&
        (html = made(ctx, os64_html_create_element(doc, OS64_HTML_NS_HTML, "html", &status), &status)) != NULL &&
        (head = made(ctx, os64_html_create_element(doc, OS64_HTML_NS_HTML, "head", &status), &status)) != NULL &&
        (body = made(ctx, os64_html_create_element(doc, OS64_HTML_NS_HTML, "body", &status), &status)) != NULL &&
        (!titled ||
         ((title_element = made(ctx, os64_html_create_element(doc, OS64_HTML_NS_HTML, "title", &status), &status)) != NULL &&
          (text = made(ctx, os64_html_create_text(doc, title.data, title.length, &status), &status)) != NULL));
    d_string_free(dom, &title);
    if (!built) return JS_EXCEPTION;
    status = os64_html_insert(doc, document, doctype, NULL);
    if (status == OS64_HTML_OK) status = os64_html_insert(doc, document, html, NULL);
    if (status == OS64_HTML_OK) status = os64_html_insert(doc, html, head, NULL);
    if (status == OS64_HTML_OK && titled) status = os64_html_insert(doc, head, title_element, NULL);
    if (status == OS64_HTML_OK && titled) status = os64_html_insert(doc, title_element, text, NULL);
    if (status == OS64_HTML_OK) status = os64_html_insert(doc, html, body, NULL);
    if (status != OS64_HTML_OK) return d_html_error(ctx, status);
    return document_new(dom, ctx, document, D_DOCUMENT_HTML);
}

/* A doctype's name (DOM § 4.5.1, createDocumentType): anything but ASCII
 * whitespace, U+0000 and '>', the empty name included. */
static bool doctype_name_valid(const DString *name)
{
    for (size_t i = 0; i < name->length; i++) {
        char c = name->data[i];
        if (c == '\0' || c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r')
            return false;
    }
    return true;
}

static JSValue create_document_type(os64_dom_t *dom, JSContext *ctx, DValue *owner, int argc,
                                    JSValueConst *argv)
{
    if (argc < 3) return JS_ThrowTypeError(ctx, "createDocumentType needs a name, a public id and a system id");
    DString name = {0}, public_id = {0}, system_id = {0};
    /* The name keeps a NUL as a NUL, so the rule can refuse it; the ids are
     * DOMStrings as the binding hands every other over. */
    if (!d_string_text(dom, ctx, argv[0], &name, true)) return JS_EXCEPTION;
    JSValue result = JS_EXCEPTION;
    if (d_string(dom, ctx, argv[1], &public_id) && d_string(dom, ctx, argv[2], &system_id)) {
        if (!doctype_name_valid(&name)) {
            result = d_error(ctx, "InvalidCharacterError", "A doctype's name holds no space, NUL or '>'");
        } else {
            int64_t status;
            os64_html_node_t *doctype = os64_html_create_doctype(dom->document, name.data, public_id.data,
                                                                 system_id.data, &status);
            result = doctype != NULL ? d_wrap_in(dom, ctx, doctype, owner->node) : d_html_error(ctx, status);
        }
    }
    d_string_free(dom, &system_id);
    d_string_free(dom, &public_id);
    d_string_free(dom, &name);
    return result;
}

#define NS_HTML "http://www.w3.org/1999/xhtml"
#define NS_SVG "http://www.w3.org/2000/svg"
#define NS_MATHML "http://www.w3.org/1998/Math/MathML"
#define NS_XML "http://www.w3.org/XML/1998/namespace"
#define NS_XMLNS "http://www.w3.org/2000/xmlns/"

/* DOM's "valid namespace prefix": not empty, and no ASCII whitespace, NUL,
 * '/' or '>'. */
bool d_prefix_valid(const char *text, size_t length)
{
    if (length == 0) return false;
    for (size_t i = 0; i < length; i++) {
        char c = text[i];
        if (c == '\0' || c == '/' || c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r')
            return false;
    }
    return true;
}

static bool same(const char *text, size_t length, const char *word)
{
    size_t n = os64_strlen(word);
    return length == n && os64_memcmp(text, word, n) == 0;
}

/* DOM's "validate and extract" for an element named `name` in namespace
 * `ns` (`has_ns` false: none), for createDocument and createElementNS:
 * JS_UNDEFINED with `*prefix` (NULL for none) and `*local` set, pointing
 * into `name`, whose colon is overwritten to end the prefix; or the refusal
 * thrown. The name is the standard's since 2025, which WPT's createElementNS
 * table at the pin holds, and not XML's QName: it splits at its FIRST
 * colon, the part before must be a valid namespace prefix (d_prefix_valid)
 * and everything after it, colons included, a valid element local name
 * (d_element_name_valid), else InvalidCharacterError; so `0:a`, `f:o:o` and
 * `prefix::local` are names and `a:0`, `:a` and `a:` are not. Then the
 * namespace rules, in the standard's order (a prefix with no namespace,
 * `xml` outside the XML namespace, `xmlns` outside the XMLNS one and the
 * XMLNS one without it), each a NamespaceError. Any namespace, or none,
 * makes an element (os64_html_create_element_ns). */
static JSValue element_namespace(JSContext *ctx, bool has_ns, const DString *ns, DString *name,
                                 const char **prefix, const char **local)
{
    const char *colon = os64_strchr(name->data, ':');
    size_t prefix_length = colon != NULL ? (size_t)(colon - name->data) : 0;
    const char *local_part = colon != NULL ? colon + 1 : name->data;
    bool xmlns = same(name->data, name->length, "xmlns") ||
                 (colon != NULL && same(name->data, prefix_length, "xmlns"));
    bool is_xmlns_ns = has_ns && same(ns->data, ns->length, NS_XMLNS);
    if ((colon != NULL && !d_prefix_valid(name->data, prefix_length)) || !d_element_name_valid(local_part))
        return d_error(ctx, "InvalidCharacterError", "That is not an element's qualified name");
    if ((colon != NULL && !has_ns) ||
        (colon != NULL && same(name->data, prefix_length, "xml") && !(has_ns && same(ns->data, ns->length, NS_XML))) ||
        (xmlns != is_xmlns_ns))
        return d_error(ctx, "NamespaceError", "That name and namespace do not go together");
    *prefix = NULL;
    if (colon != NULL) {
        name->data[prefix_length] = '\0';
        *prefix = name->data;
    }
    *local = local_part;
    return JS_UNDEFINED;
}

/* createDocument(namespace, qualifiedName, doctype) (DOM § 4.5.1): an XML
 * document, holding the doctype and then the element createElementNS would
 * make (element_namespace), either one absent when not asked for. */
static JSValue create_document(os64_dom_t *dom, JSContext *ctx, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "createDocument needs a namespace and a qualified name");
    /* The arguments in their order, as WebIDL converts them: a nullable
     * namespace, a name whose null is "", a DocumentType or null. */
    DString ns = {0}, name = {0};
    bool has_ns = !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0]);
    if (has_ns && !d_string(dom, ctx, argv[0], &ns)) return JS_EXCEPTION;
    if (!JS_IsNull(argv[1]) && !d_string(dom, ctx, argv[1], &name)) {
        d_string_free(dom, &ns);
        return JS_EXCEPTION;
    }
    if (has_ns && ns.length == 0) has_ns = false;
    JSValue result = JS_UNDEFINED;
    const os64_html_node_t *doctype = NULL;
    if (argc > 2 && !JS_IsNull(argv[2]) && !JS_IsUndefined(argv[2])) {
        doctype = JS_GetOpaque(argv[2], dom->node_class) != NULL ? d_node(dom, ctx, argv[2]) : NULL;
        if (doctype == NULL || doctype->kind != OS64_HTML_DOCTYPE)
            result = JS_ThrowTypeError(ctx, "createDocument's third argument is a DocumentType or null");
    }
    int type = has_ns && same(ns.data, ns.length, NS_HTML) ? D_DOCUMENT_XHTML
             : has_ns && same(ns.data, ns.length, NS_SVG) ? D_DOCUMENT_SVG : D_DOCUMENT_XML;
    const char *prefix = NULL, *local = NULL;
    if (JS_IsUndefined(result) && name.length != 0)
        result = element_namespace(ctx, has_ns, &ns, &name, &prefix, &local);
    os64_html_node_t *element = NULL, *document = NULL;
    int64_t status = OS64_HTML_OK;
    if (JS_IsUndefined(result) && name.length != 0)
        element = made(ctx, os64_html_create_element_ns(dom->document, has_ns ? ns.data : NULL, prefix, local,
                                                        &status), &status);
    if (JS_IsUndefined(result) && (name.length == 0 || element != NULL))
        document = document_made(dom, ctx);
    d_string_free(dom, &name);
    d_string_free(dom, &ns);
    if (document == NULL) return JS_IsUndefined(result) ? JS_EXCEPTION : result;
    /* The doctype may be in a tree, the window's own among them: it moves,
     * by the state-aware verb every move takes. */
    if (doctype != NULL) status = d_insert(dom, document, (os64_html_node_t *)doctype, NULL);
    if (status == OS64_HTML_OK && element != NULL) status = os64_html_insert(dom->document, document, element, NULL);
    if (status != OS64_HTML_OK) return d_html_error(ctx, status);
    return document_new(dom, ctx, document, type);
}

/* new Document() (DOM § 4.5): an empty document, XML's kind (its content
 * type is application/xml, so createElement makes elements in no namespace
 * and createCDATASection is allowed), its prototype new.target's. */
JSValue d_document_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    os64_html_node_t *document = document_made(dom, ctx);
    JSValue value = document != NULL ? d_wrap_constructed(dom, ctx, document, prototype) : JS_EXCEPTION;
    JS_FreeValue(ctx, prototype);
    if (!JS_IsException(value)) d_find(dom, document)->document_type = D_DOCUMENT_XML;
    return value;
}

enum { I_HTML_DOCUMENT, I_DOCUMENT, I_DOCUMENT_TYPE, I_HAS_FEATURE };

static JSValue implementation_method(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                                     int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DValue *owner = implementation_of(ctx, self);
    if (owner == NULL) return JS_EXCEPTION;
    if (!d_same_cluster(dom, owner->dom)) return JS_ThrowTypeError(ctx, "Foreign DOMImplementation");
    dom = owner->dom;
    switch (magic) {
    case I_HTML_DOCUMENT: return create_html_document(dom, ctx, argc, argv);
    case I_DOCUMENT: return create_document(dom, ctx, argc, argv);
    case I_DOCUMENT_TYPE: return create_document_type(dom, ctx, owner, argc, argv);
    /* hasFeature() is specified to answer true, whatever it is asked. */
    default: return JS_TRUE;
    }
}

enum { M_IMPLEMENTATION, M_DOCTYPE, M_NAME, M_PUBLIC_ID, M_SYSTEM_ID };

/* document.implementation ([SameObject], one per document, made when first
 * asked for and held by the document's wrapper), document.doctype, and a
 * DocumentType's name, publicId and systemId. */
static JSValue document_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                               int magic, JSValue *data)
{
    (void)argc; (void)argv;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *node = d_node(dom, ctx, self);
    if (node == NULL) return JS_EXCEPTION;
    dom = d_owner(dom, node);
    if (magic >= M_NAME) {
        if (node->kind != OS64_HTML_DOCTYPE) return JS_ThrowTypeError(ctx, "Expected a DocumentType");
        const char *text = magic == M_NAME ? node->name : magic == M_PUBLIC_ID ? node->public_id : node->system_id;
        return JS_NewString(ctx, text != NULL ? text : "");
    }
    if (node->kind != OS64_HTML_DOCUMENT) return JS_ThrowTypeError(ctx, "Expected a document");
    if (magic == M_DOCTYPE) {
        for (const os64_html_node_t *at = node->first_child; at != NULL; at = at->next)
            if (at->kind == OS64_HTML_DOCTYPE) return d_wrap(dom, ctx, at);
        return JS_NULL;
    }
    DValue *entry = d_find(dom, node);
    if (d_more_peek(entry)->implementation != NULL) return JS_DupValue(ctx, d_more_peek(entry)->implementation->value);
    DValueMore *more = d_more(dom, ctx, entry);
    if (more == NULL) return JS_EXCEPTION;
    JSValue object = JS_NewObjectProtoClass(ctx, dom->implementation_prototype->value,
                                            os64_js_class_id(&implementation_slot));
    DValue *held = d_retain(dom, ctx, object);
    if (held == NULL) return JS_EXCEPTION;
    JS_SetOpaque(held->value, entry);
    more->implementation = held;
    return JS_DupValue(ctx, held->value);
}

static void clear(JSRuntime *rt, JSValue value)
{
    (void)rt;
    JS_SetOpaque(value, NULL);
}

int d_implementation_classic(os64_dom_t *dom, JSContext *ctx)
{
    static const JSClassDef implementation_class = {.class_name = "DOMImplementation", .finalizer = clear};
    JSClassID id = os64_js_class_id(&implementation_slot);
    if (!JS_IsRegisteredClass(dom->engine, id) && JS_NewClass(dom->engine, id, &implementation_class) < 0)
        return -1;
    JSValue prototype = JS_NewObject(ctx);
    dom->implementation_prototype = d_retain(dom, ctx, prototype);
    if (dom->implementation_prototype == NULL) return -1;
    static const struct { const char *name; int magic, argc; } kMethods[] = {
        {"createDocumentType", I_DOCUMENT_TYPE, 3}, {"createDocument", I_DOCUMENT, 2},
        {"createHTMLDocument", I_HTML_DOCUMENT, 0}, {"hasFeature", I_HAS_FEATURE, 0},
    };
    for (size_t i = 0; i < sizeof(kMethods) / sizeof(kMethods[0]); i++)
        if (d_method(dom, ctx, prototype, kMethods[i].name, implementation_method, kMethods[i].argc,
                     kMethods[i].magic) < 0) return -1;
    JSValueConst document = dom->prototypes[D_PROTO_DOCUMENT]->value;
    JSValueConst doctype = dom->prototypes[D_PROTO_DOCTYPE]->value;
    if (d_accessor(dom, ctx, document, "implementation", document_member, M_IMPLEMENTATION, false) < 0 ||
        d_accessor(dom, ctx, document, "doctype", document_member, M_DOCTYPE, false) < 0 ||
        d_accessor(dom, ctx, doctype, "name", document_member, M_NAME, false) < 0 ||
        d_accessor(dom, ctx, doctype, "publicId", document_member, M_PUBLIC_ID, false) < 0 ||
        d_accessor(dom, ctx, doctype, "systemId", document_member, M_SYSTEM_ID, false) < 0)
        return -1;
    return 0;
}

/* The Modern Web table's row (modern.c): the interface object that names
 * DOMImplementation's prototype, which the classic surface made. */
/* createElementNS(namespace, qualifiedName, options) (DOM § 4.5): the element
 * element_namespace allows, in the document it is called on. React makes
 * every SVG element so, and Svelte every element. `options` (a customized
 * built-in's `is`) is converted, as WebIDL converts it, and changes nothing:
 * customized built-ins are not supported (custom.c). */
static JSValue create_element_ns(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                                 JSValue *data)
{
    (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (argc < 2) return JS_ThrowTypeError(ctx, "createElementNS needs a namespace and a qualified name");
    DString ns = {0}, name = {0};
    bool has_ns = !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0]);
    if (has_ns && !d_string(dom, ctx, argv[0], &ns)) return JS_EXCEPTION;
    if (!d_string(dom, ctx, argv[1], &name)) {
        d_string_free(dom, &ns);
        return JS_EXCEPTION;
    }
    if (has_ns && ns.length == 0) has_ns = false;
    JSValue result = JS_UNDEFINED;
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue is = JS_GetPropertyStr(ctx, argv[2], "is");
        if (JS_IsException(is)) result = JS_EXCEPTION;
        else if (!JS_IsUndefined(is)) {
            DString ignored = {0};
            if (!d_string(dom, ctx, is, &ignored)) result = JS_EXCEPTION;
            d_string_free(dom, &ignored);
        }
        JS_FreeValue(ctx, is);
    }
    const os64_html_node_t *document = JS_IsUndefined(result) ? d_node(dom, ctx, self) : NULL;
    if (JS_IsUndefined(result) && document == NULL) result = JS_EXCEPTION;
    else if (JS_IsUndefined(result) && document->kind != OS64_HTML_DOCUMENT)
        result = JS_ThrowTypeError(ctx, "createElementNS requires a document");
    if (JS_IsUndefined(result)) dom = d_owner(dom, document);
    const char *prefix = NULL, *local = NULL;
    if (JS_IsUndefined(result)) result = element_namespace(ctx, has_ns, &ns, &name, &prefix, &local);
    /* An HTML element with a definition is constructed (custom.c), inside
     * [CEReactions]: what its constructor queued runs before this returns. */
    if (JS_IsUndefined(result) && has_ns && same(ns.data, ns.length, NS_HTML) && prefix == NULL) {
        JSValue made;
        if (d_reactions_push(dom, ctx) < 0) result = JS_EXCEPTION;
        else {
            if (d_custom_create(dom, ctx, document, local, &made) != 0) result = made;
            if (d_reactions_pop(dom, ctx) < 0 && !JS_IsException(result)) {
                JS_FreeValue(ctx, result);
                result = JS_EXCEPTION;
            }
        }
    }
    if (JS_IsUndefined(result)) {
        int64_t status = OS64_HTML_OK;
        os64_html_node_t *element = os64_html_create_element_ns(dom->document, has_ns ? ns.data : NULL, prefix, local,
                                                                &status);
        result = element != NULL ? d_wrap_in(dom, ctx, element, document) : d_html_error(ctx, status);
    }
    d_string_free(dom, &ns);
    d_string_free(dom, &name);
    return result;
}

enum { T_IMPORT, T_ADOPT };

/* importNode(node, deep) and adoptNode(node) (DOM § 4.5): a copy of the
 * node whose node document is this one, or the node itself taken out of
 * its parent and moved here, its custom elements owed adoptedCallback when
 * it changed documents. Both are [CEReactions] (custom.c wraps them). A
 * Document is a NotSupportedError for either. `deep` is a boolean: DOM's
 * newer ImportNodeOptions dictionary is not read, so `{selfOnly: true}`
 * converts to true as a boolean. */
static JSValue document_take(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                             JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *document = d_node(dom, ctx, self);
    if (document == NULL) return JS_EXCEPTION;
    dom = d_owner(dom, document);
    if (document->kind != OS64_HTML_DOCUMENT) return JS_ThrowTypeError(ctx, "Expected a document");
    if (argc < 1) return JS_ThrowTypeError(ctx, "Missing node argument");
    int deep = magic == T_IMPORT && argc > 1 ? JS_ToBool(ctx, argv[1]) : 0;
    if (deep < 0) return JS_EXCEPTION;
    if (d_is_attr(dom, argv[0])) {
        if (magic == T_IMPORT) return d_attr_import(dom, ctx, argv[0], document);
        return d_attr_adopt(dom, ctx, argv[0], document) < 0 ? JS_EXCEPTION : JS_DupValue(ctx, argv[0]);
    }
    const os64_html_node_t *node = d_node(dom, ctx, argv[0]);
    if (node == NULL) return JS_EXCEPTION;
    if (node->kind == OS64_HTML_DOCUMENT)
        return d_error(ctx, "NotSupportedError", magic == T_IMPORT ? "A document cannot be imported" :
                                                                   "A document cannot be adopted");
    if (magic == T_IMPORT) return d_node_clone(dom, ctx, node, deep != 0, document);
    const os64_html_node_t *from = d_node_document(dom, node);
    int64_t status = os64_page_node_adopt(dom->state, (os64_html_node_t *)document, (os64_html_node_t *)node);
    if (status != OS64_HTML_OK) return d_html_error(ctx, status);
    if (from != document) {
        d_custom_adopted(dom, node, from, document);
    }
    return JS_DupValue(ctx, argv[0]);
}

/* ── DOMParser (HTML § 8.5.1), HTML only ──────────────────────────────── */

static JSClassID parser_slot;

/* `new DOMParser()`: an object of the parser's class, which carries no
 * state, with new.target's prototype (a subclass's) or DOMParser's. */
static JSValue parser_construct(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    if (d_binding(ctx) == NULL) return JS_EXCEPTION;
    JSClassID id = os64_js_class_id(&parser_slot);
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    JSValue made = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, id) : JS_NewObjectClass(ctx, (int)id);
    JS_FreeValue(ctx, prototype);
    if (!JS_IsException(made)) JS_SetOpaque(made, &parser_slot);
    return made;
}

/* parseFromString(string, type): for "text/html", a new HTML document of
 * the string, parsed with scripting off (HTML's "parse HTML from a
 * string": a script in it is born started and never runs, and noscript is
 * markup). The string is parsed in an `html` element's context, which puts
 * the head's and the body's content where a whole document's parse would,
 * and the parse's own root becomes the document's element, with what its
 * `<html>` tags gave it (`lang`, a theme's class); a doctype in it is not
 * kept, so the document is in no-quirks mode. The
 * XML types are a NotSupportedError: libhtml's XML builder is not
 * connected here (DEBTS.md). */
static JSValue parse_from_string(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                                 JSValue *data)
{
    (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (JS_GetOpaque(self, os64_js_class_id(&parser_slot)) == NULL) return JS_ThrowTypeError(ctx, "Expected a DOMParser");
    if (argc < 2) return JS_ThrowTypeError(ctx, "parseFromString needs a string and a type");
    DString text = {0}, type = {0};
    if (!d_string(dom, ctx, argv[0], &text)) return JS_EXCEPTION;
    if (!d_string(dom, ctx, argv[1], &type)) { d_string_free(dom, &text); return JS_EXCEPTION; }
    static const char *const kTypes[] = {"text/html", "text/xml", "application/xml", "application/xhtml+xml",
                                         "image/svg+xml"};
    int kind = -1;
    for (int i = 0; i < 5 && kind < 0; i++)
        if (os64_streq(type.data != NULL ? type.data : "", kTypes[i])) kind = i;
    d_string_free(dom, &type);
    JSValue result;
    if (kind < 0) result = JS_ThrowTypeError(ctx, "Not a DOMParserSupportedType");
    else if (kind > 0) result = d_error(ctx, "NotSupportedError", "DOMParser parses HTML only");
    else {
        int64_t status = OS64_HTML_OK;
        os64_html_document_t *doc = dom->document;
        os64_html_node_t *document = document_made(dom, ctx), *context = NULL, *html = NULL;
        bool built = document != NULL &&
            (context = made(ctx, os64_html_create_element(doc, OS64_HTML_NS_HTML, "html", &status), &status)) != NULL &&
            (html = made(ctx, os64_html_parse_fragment_root(doc, context, text.data != NULL ? text.data : "",
                                                            text.length, false, &status), &status)) != NULL;
        if (built && d_script_mark_tree(dom, html) < 0) { status = OS64_HTML_NO_MEMORY; built = false; }
        if (built) status = os64_html_insert(doc, document, html, NULL);
        if (built && status == OS64_HTML_OK) result = document_new(dom, ctx, document, D_DOCUMENT_HTML);
        else result = document == NULL || JS_HasException(ctx) ? JS_EXCEPTION : d_html_error(ctx, status);
    }
    d_string_free(dom, &text);
    return result;
}

/* ── XMLSerializer (DOM Parsing § 3.1) ───────────────────────────────── */

static JSClassID serializer_slot;

/* `new XMLSerializer()`: stateless as DOMParser is, of new.target's
 * prototype (a subclass's) or XMLSerializer's. */
static JSValue serializer_construct(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    if (d_binding(ctx) == NULL) return JS_EXCEPTION;
    JSClassID id = os64_js_class_id(&serializer_slot);
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    JSValue made = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, id) : JS_NewObjectClass(ctx, (int)id);
    JS_FreeValue(ctx, prototype);
    if (!JS_IsException(made)) JS_SetOpaque(made, &serializer_slot);
    return made;
}

/* serializeToString(root): the XML serialization of a node and its subtree
 * (libhtml's os64_html_serialize_xml, require well-formed false, so no
 * node is refused as unserializable); an Attr's is the empty string. A
 * root that is no Node is a TypeError, and running out of memory a
 * QuotaExceededError. */
static JSValue serialize_to_string(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                                   JSValue *data)
{
    (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (JS_GetOpaque(self, os64_js_class_id(&serializer_slot)) == NULL)
        return JS_ThrowTypeError(ctx, "Expected an XMLSerializer");
    if (argc < 1) return JS_ThrowTypeError(ctx, "serializeToString needs a node");
    DAttrView view;
    int attr = d_attr_view(dom, ctx, argv[0], &view);
    if (attr < 0) return JS_EXCEPTION;
    if (attr == 1) return JS_NewString(ctx, "");
    const os64_html_node_t *node = d_node(dom, ctx, argv[0]);
    if (node == NULL) return JS_EXCEPTION;
    dom = d_owner(dom, node);
    int64_t length = os64_html_serialize_xml(node, NULL, 0);
    if (length < 0) return d_html_error(ctx, length);
    char *buffer = (uint64_t)length >= SIZE_MAX ? NULL : d_alloc(dom, (size_t)length + 1);
    if (buffer == NULL) return d_error(ctx, "QuotaExceededError", "DOM serialization quota exceeded");
    int64_t written = os64_html_serialize_xml(node, buffer, (size_t)length + 1);
    JSValue result = written < 0 ? d_html_error(ctx, written) : JS_NewStringLen(ctx, buffer, (size_t)length);
    d_free(dom, buffer);
    return result;
}

int d_implementation_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    JSValue f = d_interface_new(ctx, "DOMImplementation", 0, NULL, dom->implementation_prototype->value,
                                JS_UNDEFINED);
    if (JS_IsException(f) ||
        JS_DefinePropertyValueStr(ctx, global, "DOMImplementation", f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0 ||
        d_method(dom, ctx, dom->prototypes[D_PROTO_DOCUMENT]->value, "createElementNS", create_element_ns, 2, 0) < 0 ||
        d_method(dom, ctx, dom->prototypes[D_PROTO_DOCUMENT]->value, "importNode", document_take, 1, T_IMPORT) < 0 ||
        d_method(dom, ctx, dom->prototypes[D_PROTO_DOCUMENT]->value, "adoptNode", document_take, 1, T_ADOPT) < 0)
        return -1;
    static const JSClassDef parser_definition = {.class_name = "DOMParser"};
    JSClassID id = os64_js_class_id(&parser_slot);
    if (!JS_IsRegisteredClass(dom->engine, id) && JS_NewClass(dom->engine, id, &parser_definition) < 0) return -1;
    JSValue prototype = JS_NewObject(ctx);
    if (d_retain(dom, ctx, prototype) == NULL) return -1;
    JS_SetClassProto(ctx, id, JS_DupValue(ctx, prototype));
    if (d_method(dom, ctx, prototype, "parseFromString", parse_from_string, 2, 0) < 0) return -1;
    JSValue parser = d_interface_new(ctx, "DOMParser", 0, parser_construct, prototype, JS_UNDEFINED);
    if (JS_IsException(parser) ||
        JS_DefinePropertyValueStr(ctx, global, "DOMParser", parser, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    static const JSClassDef serializer_definition = {.class_name = "XMLSerializer"};
    id = os64_js_class_id(&serializer_slot);
    if (!JS_IsRegisteredClass(dom->engine, id) && JS_NewClass(dom->engine, id, &serializer_definition) < 0) return -1;
    prototype = JS_NewObject(ctx);
    if (d_retain(dom, ctx, prototype) == NULL) return -1;
    JS_SetClassProto(ctx, id, JS_DupValue(ctx, prototype));
    if (d_method(dom, ctx, prototype, "serializeToString", serialize_to_string, 1, 0) < 0) return -1;
    JSValue serializer = d_interface_new(ctx, "XMLSerializer", 0, serializer_construct, prototype, JS_UNDEFINED);
    if (JS_IsException(serializer)) return -1;
    return JS_DefinePropertyValueStr(ctx, global, "XMLSerializer", serializer, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}
