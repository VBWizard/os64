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
 * WHICH DOCUMENT A NODE BELONGS TO is the document its tree hangs from, and
 * a tree that hangs from none (a node made and not yet inserted, or one
 * taken out) remembers the document it was made in or last left on its
 * root's wrapper (DValue.document). Every verb that takes a node out of a
 * tree records first the document it leaves (d_leaving), and every node a
 * document of its own makes or a clone copies is recorded as it is made
 * (d_made_in), so a root that has a wrapper always says. One that has none (a child innerHTML replaced,
 * never handed to a script) says nothing, and reads as the window's: a
 * script reaches it only through a descendant it held before. While no
 * document of its own exists every node is the window's, and none of this
 * runs. */

static JSClassID implementation_slot;

static const os64_html_node_t *window_document(os64_dom_t *dom)
{
    return dom->document->document;
}

const os64_html_node_t *d_node_document(os64_dom_t *dom, const os64_html_node_t *node)
{
    if (node->kind == OS64_HTML_DOCUMENT) return node;
    if (dom->documents == 0) return window_document(dom);
    const os64_html_node_t *root = node;
    while (root->parent != NULL) root = root->parent;
    if (root->kind == OS64_HTML_DOCUMENT) return root;
    DValue *entry = d_find(dom, root);
    return entry != NULL && entry->document != NULL ? entry->document : window_document(dom);
}

bool d_window_document(os64_dom_t *dom, const os64_html_node_t *node)
{
    return dom->documents == 0 || d_node_document(dom, node) == window_document(dom);
}

bool d_scripting(os64_dom_t *dom, const os64_html_node_t *node)
{
    return dom->options.scripting && d_window_document(dom, node);
}

void d_made_in(os64_dom_t *dom, const os64_html_node_t *node, const os64_html_node_t *document)
{
    DValue *entry = d_find(dom, node);
    if (entry != NULL) entry->document = document != window_document(dom) ? document : NULL;
}

/* Only a node with a wrapper keeps a record, so one without costs a lookup
 * and no walk to its root. */
void d_leaving(os64_dom_t *dom, const os64_html_node_t *node)
{
    if (dom->documents == 0 || node->parent == NULL) return;
    DValue *entry = d_find(dom, node);
    if (entry == NULL) return;
    const os64_html_node_t *document = d_node_document(dom, node);
    entry->document = document != window_document(dom) ? document : NULL;
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

/* A new DOCUMENT node, counted from the moment it exists, so that from then
 * on nodes are asked which document they are in: one may be moved into it
 * before it has a wrapper. NULL with the refusal thrown. */
static os64_html_node_t *document_made(os64_dom_t *dom, JSContext *ctx)
{
    int64_t status;
    os64_html_node_t *document = os64_html_create_document(dom->document, &status);
    if (document == NULL) {
        d_html_error(ctx, status);
        return NULL;
    }
    dom->documents++;
    return document;
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
            result = doctype != NULL ? d_wrap(dom, ctx, doctype) : d_html_error(ctx, status);
            if (!JS_IsException(result)) d_made_in(dom, doctype, owner->node);
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

/* The namespace an element named `name` in namespace `ns` (`has_ns` false:
 * none) is made in, for createDocument and createElementNS: JS_UNDEFINED
 * with *space set, or the refusal thrown. The name is DOM's "validate and
 * extract" as the standard has had it since 2025, which WPT's
 * createElementNS table at the pin holds, and not XML's QName: the name
 * splits at its FIRST colon, the part before it must be a valid namespace
 * prefix (d_prefix_valid) and everything after it, colons included, a valid
 * element local name (d_element_name_valid), else InvalidCharacterError; so
 * `0:a`, `f:o:o` and `prefix::local` are names and `a:0`, `:a` and `a:` are
 * not. Then the namespace rules, in the standard's order (a prefix with no
 * namespace, `xml` outside the XML namespace, `xmlns` outside the XMLNS one
 * and the XMLNS one without it), each a NamespaceError. Then its namespace
 * must be one the page's tree holds, HTML, SVG or MathML: an element in no
 * namespace or another is a NotSupportedError, since libhtml has nowhere to
 * put it. */
static JSValue element_namespace(JSContext *ctx, bool has_ns, const DString *ns, const DString *name,
                                 os64_html_ns_t *space)
{
    const char *colon = os64_strchr(name->data, ':');
    size_t prefix = colon != NULL ? (size_t)(colon - name->data) : 0;
    const char *local = colon != NULL ? colon + 1 : name->data;
    bool xmlns = same(name->data, name->length, "xmlns") || (colon != NULL && same(name->data, prefix, "xmlns"));
    bool is_xmlns_ns = has_ns && same(ns->data, ns->length, NS_XMLNS);
    if ((colon != NULL && !d_prefix_valid(name->data, prefix)) || !d_element_name_valid(local))
        return d_error(ctx, "InvalidCharacterError", "That is not an element's qualified name");
    if ((colon != NULL && !has_ns) ||
        (colon != NULL && same(name->data, prefix, "xml") && !(has_ns && same(ns->data, ns->length, NS_XML))) ||
        (xmlns != is_xmlns_ns))
        return d_error(ctx, "NamespaceError", "That name and namespace do not go together");
    if (has_ns && same(ns->data, ns->length, NS_HTML)) *space = OS64_HTML_NS_HTML;
    else if (has_ns && same(ns->data, ns->length, NS_SVG)) *space = OS64_HTML_NS_SVG;
    else if (has_ns && same(ns->data, ns->length, NS_MATHML)) *space = OS64_HTML_NS_MATHML;
    else return d_error(ctx, "NotSupportedError",
                        "An element in no namespace, or in one other than HTML, SVG and MathML, "
                        "is one this browser's documents cannot hold");
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
    os64_html_ns_t space = OS64_HTML_NS_HTML;
    int type = has_ns && same(ns.data, ns.length, NS_HTML) ? D_DOCUMENT_XHTML
             : has_ns && same(ns.data, ns.length, NS_SVG) ? D_DOCUMENT_SVG : D_DOCUMENT_XML;
    if (JS_IsUndefined(result) && name.length != 0)
        result = element_namespace(ctx, has_ns, &ns, &name, &space);
    os64_html_node_t *element = NULL, *document = NULL;
    int64_t status = OS64_HTML_OK;
    if (JS_IsUndefined(result) && name.length != 0)
        element = made(ctx, os64_html_create_element(dom->document, space, name.data, &status), &status);
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

enum { I_HTML_DOCUMENT, I_DOCUMENT, I_DOCUMENT_TYPE, I_HAS_FEATURE };

static JSValue implementation_method(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                                     int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DValue *owner = implementation_of(ctx, self);
    if (owner == NULL) return JS_EXCEPTION;
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
    if (entry->implementation != NULL) return JS_DupValue(ctx, entry->implementation->value);
    JSValue object = JS_NewObjectProtoClass(ctx, dom->implementation_prototype->value,
                                            os64_js_class_id(&implementation_slot));
    DValue *held = d_retain(dom, ctx, object);
    if (held == NULL) return JS_EXCEPTION;
    JS_SetOpaque(held->value, entry);
    entry->implementation = held;
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
 * there are no custom elements. */
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
    os64_html_ns_t space = OS64_HTML_NS_HTML;
    if (JS_IsUndefined(result)) result = element_namespace(ctx, has_ns, &ns, &name, &space);
    if (JS_IsUndefined(result)) {
        int64_t status = OS64_HTML_OK;
        os64_html_node_t *element = os64_html_create_element(dom->document, space, name.data, &status);
        result = element != NULL ? d_wrap(dom, ctx, element) : d_html_error(ctx, status);
        if (!JS_IsException(result)) d_made_in(dom, element, document);
    }
    d_string_free(dom, &ns);
    d_string_free(dom, &name);
    return result;
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
 * the head's and the body's content where a whole document's parse would;
 * a doctype in it is not kept, so the document is in no-quirks mode. The
 * XML types are a NotSupportedError: there is no XML parser (DEBTS.md). */
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
        os64_html_node_t *document = document_made(dom, ctx), *html = NULL, *fragment = NULL;
        bool built = document != NULL &&
            (html = made(ctx, os64_html_create_element(doc, OS64_HTML_NS_HTML, "html", &status), &status)) != NULL &&
            (status = os64_html_insert(doc, document, html, NULL)) == OS64_HTML_OK &&
            (fragment = made(ctx, os64_html_parse_fragment(doc, html, text.data != NULL ? text.data : "", text.length,
                                                           false, &status), &status)) != NULL;
        if (built && d_script_mark_tree(dom, fragment) < 0) { status = OS64_HTML_NO_MEMORY; built = false; }
        if (built) status = os64_html_insert(doc, html, fragment, NULL);
        if (built && status == OS64_HTML_OK) result = document_new(dom, ctx, document, D_DOCUMENT_HTML);
        else result = document == NULL || JS_HasException(ctx) ? JS_EXCEPTION : d_html_error(ctx, status);
    }
    d_string_free(dom, &text);
    return result;
}

int d_implementation_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    JSValue f = d_interface_new(ctx, "DOMImplementation", 0, NULL, dom->implementation_prototype->value,
                                JS_UNDEFINED);
    if (JS_IsException(f) ||
        JS_DefinePropertyValueStr(ctx, global, "DOMImplementation", f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0 ||
        d_method(dom, ctx, dom->prototypes[D_PROTO_DOCUMENT]->value, "createElementNS", create_element_ns, 2, 0) < 0)
        return -1;
    static const JSClassDef parser_definition = {.class_name = "DOMParser"};
    JSClassID id = os64_js_class_id(&parser_slot);
    if (!JS_IsRegisteredClass(dom->engine, id) && JS_NewClass(dom->engine, id, &parser_definition) < 0) return -1;
    JSValue prototype = JS_NewObject(ctx);
    if (d_retain(dom, ctx, prototype) == NULL) return -1;
    JS_SetClassProto(ctx, id, JS_DupValue(ctx, prototype));
    if (d_method(dom, ctx, prototype, "parseFromString", parse_from_string, 2, 0) < 0) return -1;
    JSValue parser = d_interface_new(ctx, "DOMParser", 0, parser_construct, prototype, JS_UNDEFINED);
    if (JS_IsException(parser)) return -1;
    return JS_DefinePropertyValueStr(ctx, global, "DOMParser", parser, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}
