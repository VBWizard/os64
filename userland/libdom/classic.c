#include "internal.h"
#include "os64/fmt.h"

/* The classic page surface is backed by the same tree and control state as
 * verbs and layout. Named lookup stays live instead of installing aliases
 * that survive the element which supplied their name. */
static const os64_html_node_t *next_node(const os64_html_node_t *node, const os64_html_node_t *root)
{
    if (node->first_child != NULL) return node->first_child;
    while (node != root && node->next == NULL) node = node->parent;
    return node != root ? node->next : NULL;
}

static bool contains(const char *text, const char *word)
{
    size_t len = os64_strlen(word);
    for (; *text; text++) {
        size_t i = 0;
        while (i < len && text[i] && text[i] == word[i]) i++;
        if (i == len) return true;
    }
    return false;
}

/* The tree a form's controls and a document's names are looked for in:
 * the document the node is connected to (any document, d_connected), else
 * the node's own subtree. */
static const os64_html_node_t *dom_root(const os64_html_node_t *node)
{
    const os64_html_node_t *root = node;
    while (root->parent != NULL) root = root->parent;
    return root->kind == OS64_HTML_DOCUMENT ? root : node;
}

bool d_named_match(const os64_html_node_t *node, const char *name)
{
    if (node->kind != OS64_HTML_ELEMENT || node->ns != OS64_HTML_NS_HTML || *name == '\0') return false;
    const os64_html_attr_t *id = os64_html_attr(node, "id"), *named = os64_html_attr(node, "name");
    return (id != NULL && os64_streq(id->value, name)) || (named != NULL && os64_streq(named->value, name));
}

bool d_listed(const os64_html_node_t *node)
{
    if (node->kind != OS64_HTML_ELEMENT || node->ns != OS64_HTML_NS_HTML) return false;
    switch (node->tag) {
    case OS64_HTML_TAG_BUTTON: case OS64_HTML_TAG_FIELDSET: case OS64_HTML_TAG_INPUT: case OS64_HTML_TAG_OBJECT:
    case OS64_HTML_TAG_OUTPUT: case OS64_HTML_TAG_SELECT: case OS64_HTML_TAG_TEXTAREA:
        return true;
    default: return false;
    }
}

static bool image_button(const os64_html_node_t *node)
{
    const os64_html_attr_t *type = node->tag == OS64_HTML_TAG_INPUT ? os64_html_attr(node, "type") : NULL;
    return type != NULL && os64_streq_nocase(type->value, "image");
}

bool d_classic_match(const os64_html_node_t *root,
                      const os64_html_node_t *node, unsigned kind)
{
    if (node->kind != OS64_HTML_ELEMENT || node->ns != OS64_HTML_NS_HTML) return false;
    switch (kind) {
    case D_QUERY_IMAGES: return node->tag == OS64_HTML_TAG_IMG;
    case D_QUERY_FORMS: return node->tag == OS64_HTML_TAG_FORM;
    case D_QUERY_DOCUMENT_NAMES:
        return node->tag == OS64_HTML_TAG_FORM || node->tag == OS64_HTML_TAG_IMG;
    case D_QUERY_CONTROLS:
        /* A form's listed elements (HTML § 4.10.2) it owns, but an image
         * button, which a form's `elements` leaves out and a fieldset's
         * does not. */
        return d_listed(node) && !image_button(node) && d_form_owner(node) == root;
    case D_QUERY_FIELDSET: return d_listed(node);
    default: return true;
    }
}

JSValue d_classic_collection(os64_dom_t *dom, JSContext *ctx,
                              const os64_html_node_t *node, unsigned kind)
{
    DValue *entry = d_find(dom, node);
    DQuery **slot = kind == D_QUERY_IMAGES ? &entry->images : kind == D_QUERY_FORMS ? &entry->forms : &entry->elements;
    if (*slot != NULL) return JS_DupValue(ctx, (*slot)->entry->value);
    const os64_html_node_t *root = kind == D_QUERY_CONTROLS ? dom_root(node) : node;
    JSValue value = d_special_collection(dom, ctx, root, kind, NULL);
    if (!JS_IsException(value)) {
        *slot = JS_GetOpaque(value, dom->collection_class);
        // The search covers the document so controls with form= participate.
        // Its owner remains separate from the traversal root.
        if (kind == D_QUERY_CONTROLS) (*slot)->owner = node;
        // With the modern switch on, an HTMLFormControlsCollection.
        if (kind == D_QUERY_CONTROLS && dom->controls_prototype != NULL &&
            JS_SetPrototype(ctx, value, dom->controls_prototype->value) < 0) {
            JS_FreeValue(ctx, value);
            return JS_EXCEPTION;
        }
    }
    return value;
}

const os64_html_node_t *d_form_control(const os64_html_node_t *form, uint32_t index, uint32_t *count)
{
    const os64_html_node_t *search = dom_root(form), *found = NULL;
    uint32_t seen = 0;
    for (const os64_html_node_t *at = search; at != NULL; at = next_node(at, search))
        if (d_classic_match(form, at, D_QUERY_CONTROLS)) {
            if (seen == index) found = at;
            seen++;
            if (found != NULL && count == NULL) break;
        }
    if (count != NULL) *count = seen;
    return found;
}

static int named_property(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst object, JSAtom atom)
{
    JSClassID id;
    DValue *entry = JS_GetAnyOpaque(object, &id);
    if (entry == NULL || !d_context(entry->dom, ctx, OS64_JS_ABI_ID)) return -1;
    const os64_html_node_t *root = entry->node;
    unsigned kind;
    if (root->kind == OS64_HTML_DOCUMENT) kind = D_QUERY_DOCUMENT_NAMES;
    else if (root->tag == OS64_HTML_TAG_FORM && root->ns == OS64_HTML_NS_HTML) kind = D_QUERY_CONTROLS;
    else return 0;
    /* A form's indexed getter (HTML § 4.10.3), with the modern switch on:
     * form[i] is its `elements`' i-th, asked before its prototype is, as
     * WebIDL asks an index. */
    uint32_t index;
    int numeric = kind == D_QUERY_CONTROLS && entry->dom->controls_prototype != NULL
        ? d_index_key(ctx, atom, &index) : 0;
    if (numeric < 0) return -1;
    if (numeric) {
        const os64_html_node_t *found = d_form_control(root, index, NULL);
        if (found == NULL) return 0;
        if (desc != NULL) {
            JSValue value = d_wrap(entry->dom, ctx, found);
            if (JS_IsException(value)) return -1;
            *desc = (JSPropertyDescriptor){.flags = JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE,
                .value = value, .getter = JS_UNDEFINED, .setter = JS_UNDEFINED};
        }
        return 1;
    }
    /* A form is [LegacyOverrideBuiltIns] with the modern switch on: a
     * control's name hides the prototype's member of that name (an input
     * named `submit` is form.submit), as every browser has it. */
    if (!(kind == D_QUERY_CONTROLS && entry->dom->controls_prototype != NULL)) {
        JSValue prototype = JS_GetPrototype(ctx, object);
        if (JS_IsException(prototype)) return -1;
        int reserved = JS_IsNull(prototype) ? 0 : JS_HasProperty(ctx, prototype, atom);
        JS_FreeValue(ctx, prototype);
        if (reserved != 0) return reserved < 0 ? -1 : 0;
    }
    JSValue key = JS_AtomToValue(ctx, atom);
    if (JS_IsException(key)) return -1;
    if (!JS_IsString(key)) { JS_FreeValue(ctx, key); return 0; }
    const char *name = JS_ToCString(ctx, key);
    JS_FreeValue(ctx, key);
    if (name == NULL) return -1;
    const os64_html_node_t *search = dom_root(root);
    const os64_html_node_t *first = NULL;
    size_t count = 0;
    for (const os64_html_node_t *at = search->first_child; at != NULL; at = next_node(at, search))
        if (d_classic_match(root, at, kind) && d_named_match(at, name)) {
            if (first == NULL) first = at;
            count++;
        }
    if (count == 0) { JS_FreeCString(ctx, name); return 0; }
    JSValue value = JS_UNDEFINED;
    if (desc != NULL) {
        if (count == 1) value = d_wrap(entry->dom, ctx, first);
        else if (kind == D_QUERY_CONTROLS) value = d_radio_list(entry->dom, ctx, root, name);
        else {
            value = d_special_collection(entry->dom, ctx, search, kind, name);
            if (!JS_IsException(value)) ((DQuery *)JS_GetOpaque(value, entry->dom->collection_class))->owner = root;
        }
    }
    JS_FreeCString(ctx, name);
    if (JS_IsException(value)) return -1;
    if (desc != NULL) *desc = (JSPropertyDescriptor){.flags = JS_PROP_CONFIGURABLE,
        .value = value, .getter = JS_UNDEFINED, .setter = JS_UNDEFINED};
    return 1;
}

/* A form whose indices and names are its controls': the modern switch's
 * (HTML § 4.10.3, a legacy platform object with no setter). */
static DValue *modern_form(JSValueConst object)
{
    JSClassID id;
    DValue *entry = JS_GetAnyOpaque(object, &id);
    return entry != NULL && entry->node != NULL && entry->node->kind == OS64_HTML_ELEMENT &&
                   entry->node->ns == OS64_HTML_NS_HTML && entry->node->tag == OS64_HTML_TAG_FORM &&
                   entry->dom->controls_prototype != NULL ? entry : NULL;
}

/* A form's own names: its controls' indices, then each id and name they
 * carry, once, not enumerable ([LegacyUnenumerableNamedProperties]). Any
 * other node has none of its own here. The engine lists an object's
 * ordinary keys beside these and does not merge them, so a name the form
 * already has as one (an expando made before the control came) is left
 * to the engine's list. */
static int node_names(JSContext *ctx, JSPropertyEnum **table, uint32_t *length, JSValueConst object)
{
    *table = NULL;
    *length = 0;
    DValue *entry = modern_form(object);
    /* The ask below, for the ordinary keys alone. */
    if (entry != NULL && entry->dom->listing_names) return 0;
    if (entry == NULL || !d_context(entry->dom, ctx, OS64_JS_ABI_ID)) return entry == NULL ? 0 : -1;
    uint32_t count = 0;
    (void)d_form_control(entry->node, UINT32_MAX, &count);
    if (count > (UINT32_MAX - 1) / 3 || (size_t)count * 3 > SIZE_MAX / sizeof(JSPropertyEnum)) {
        d_error(ctx, "QuotaExceededError", "Too many form controls");
        return -1;
    }
    JSPropertyEnum *own = NULL;
    uint32_t own_count = 0;
    entry->dom->listing_names = true;
    int listed = JS_GetOwnPropertyNames(ctx, &own, &own_count, object, JS_GPN_STRING_MASK);
    entry->dom->listing_names = false;
    if (listed < 0) return -1;
    JSPropertyEnum *entries = count != 0 ? js_malloc(ctx, (size_t)count * 3 * sizeof(*entries)) : NULL;
    uint32_t n = 0;
    if (count != 0 && entries == NULL) goto fail;
    for (uint32_t i = 0; i < count; i++) {
        entries[n].is_enumerable = true;
        entries[n].atom = JS_NewAtomUInt32(ctx, i);
        if (entries[n].atom == JS_ATOM_NULL) goto fail;
        n++;
    }
    for (uint32_t i = 0; i < count; i++) {
        const os64_html_node_t *control = d_form_control(entry->node, i, NULL);
        const char *const names[] = {"id", "name"};
        for (int k = 0; control != NULL && k < 2; k++) {
            const os64_html_attr_t *attr = os64_html_attr(control, names[k]);
            if (attr == NULL || attr->value[0] == '\0') continue;
            JSAtom atom = JS_NewAtom(ctx, attr->value);
            if (atom == JS_ATOM_NULL) goto fail;
            /* A name an index spells (`name=0`) is that index's key. */
            bool seen = false;
            for (uint32_t s = 0; s < n && !seen; s++) seen = entries[s].atom == atom;
            for (uint32_t s = 0; s < own_count && !seen; s++) seen = own[s].atom == atom;
            if (seen) {
                JS_FreeAtom(ctx, atom);
                continue;
            }
            entries[n].is_enumerable = false;
            entries[n].atom = atom;
            n++;
        }
    }
    JS_FreePropertyEnum(ctx, own, own_count);
    *table = entries;
    *length = n;
    return 0;
fail:
    while (n != 0) JS_FreeAtom(ctx, entries[--n].atom);
    js_free(ctx, entries);
    JS_FreePropertyEnum(ctx, own, own_count);
    return -1;
}

/* A form's index or name is not a node's to redefine or delete: refused,
 * as a nonextensible object refuses one (a TypeError where the caller
 * asked for one); anything else is an ordinary property. A form has no
 * indexed setter, so any index at all is refused a definition, one it has
 * no control for yet included (WebIDL § 3.9.3), or a property defined
 * there would shadow its control once it had one; only one it has is
 * refused a delete. */
static int node_define(JSContext *ctx, JSValueConst object, JSAtom atom, JSValueConst value, JSValueConst getter,
                       JSValueConst setter, int flags)
{
    DValue *entry = modern_form(object);
    uint32_t index;
    int numeric = entry != NULL ? d_index_key(ctx, atom, &index) : 0;
    int named = numeric == 0 && entry != NULL ? named_property(ctx, NULL, object, atom) : 0;
    if (numeric < 0 || named < 0) return -1;
    return JS_DefineProperty(ctx, numeric || named ? entry->dom->index_guard->value : object, atom, value, getter,
                             setter, flags | JS_PROP_NO_EXOTIC);
}

static int node_delete(JSContext *ctx, JSValueConst object, JSAtom atom)
{
    int named = modern_form(object) != NULL ? named_property(ctx, NULL, object, atom) : 0;
    return named < 0 ? -1 : !named;
}

JSClassExoticMethods d_node_exotic = {
    .get_own_property = named_property, .get_own_property_names = node_names,
    .define_own_property = node_define, .delete_property = node_delete,
};

enum { C_IMAGES, C_FORMS, C_ELEMENTS, C_STYLE, C_TITLE, C_NAME, C_SRC, C_WIDTH, C_HEIGHT,
       C_CAPTURE, C_IMAGE, C_NAVIGATOR, C_USER_AGENT, C_APP_NAME, C_APP_VERSION, C_PLATFORM, C_CURRENT_EVENT, C_LEGACY_ARGUMENTS,
       C_COOKIE };

/* The room a document.cookie read starts with, doubled until the whole
 * answer fits: a page's cookies are rarely more, and a jar may hold far more
 * (thousands of 4096-byte cookies, RFC 6265 § 6.1), which only the DOM's
 * own allocation budget bounds. */
#define COOKIE_READ_FIRST (16 * 1024)

static JSValue classic(JSContext *ctx, JSValueConst self, int argc,
                        JSValueConst *argv, int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    if (magic == C_CAPTURE) return JS_UNDEFINED; // event propagation is already enabled
    if (magic == C_COOKIE) {
        const os64_html_node_t *node = d_node(dom, ctx, self);
        if (node == NULL) return JS_EXCEPTION;
        if (node->kind != OS64_HTML_DOCUMENT) return JS_ThrowTypeError(ctx, "cookie requires a document");
        /* A document with no window is cookie-averse (HTML § 3.1.3), as
         * every document is when the host gives no provider. */
        if (!d_window_document(dom, node)) return set ? JS_UNDEFINED : JS_NewString(ctx, "");
        if (set) {
            DString text = {0};
            if (!d_string(dom, ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &text)) return JS_EXCEPTION;
            if (dom->cookies.set != NULL) dom->cookies.set(dom->cookies.opaque, text.data, text.length);
            d_string_free(dom, &text);
            return JS_UNDEFINED;
        }
        if (dom->cookies.get == NULL) return JS_NewString(ctx, "");
        for (size_t cap = COOKIE_READ_FIRST; cap != 0 && cap <= SIZE_MAX / 2; cap *= 2) {
            char *buffer = d_alloc(dom, cap);
            if (buffer == NULL) return d_error(ctx, "QuotaExceededError", "DOM cookie quota exceeded");
            bool whole = true;
            size_t n = dom->cookies.get(dom->cookies.opaque, buffer, cap, &whole);
            if (!whole) {
                d_free(dom, buffer);
                continue;
            }
            JSValue result = JS_NewStringLen(ctx, buffer, n < cap ? n : cap - 1);
            d_free(dom, buffer);
            return result;
        }
        return d_error(ctx, "QuotaExceededError", "DOM cookie quota exceeded");
    }
    if (magic == C_NAVIGATOR) return JS_DupValue(ctx, dom->navigator->value);
    if (magic == C_LEGACY_ARGUMENTS) {
        os64_js_outcome_t out;
        if (os64_js_check_budget(dom->runtime,OS64_JS_ABI_ID,&out) != OS64_JS_OK)
            return d_error(ctx,"InvalidStateError","Legacy arguments requires a live budget");
        JSValue value = JS_GetLegacyFunctionArguments(ctx,self);
        if (JS_IsException(value)) return value;
        if (os64_js_check_budget(dom->runtime,OS64_JS_ABI_ID,&out) != OS64_JS_OK) {
            JS_FreeValue(ctx,value);
            return d_error(ctx,"InvalidStateError","Legacy arguments exceeded the script budget");
        }
        return value;
    }
    if (magic == C_CURRENT_EVENT) return dom->current_event != NULL ? JS_DupValue(ctx,dom->current_event->value) : JS_UNDEFINED;
    if (magic >= C_USER_AGENT && magic <= C_PLATFORM) {
        if (!JS_IsObject(self) || JS_VALUE_GET_PTR(self) != JS_VALUE_GET_PTR(dom->navigator->value))
            return JS_ThrowTypeError(ctx, "Navigator getter requires navigator");
        const char *agent = dom->user_agent != NULL ? dom->user_agent(dom->user_agent_opaque) : NULL;
        if (agent == NULL) agent = "yonder/1.0 (os64)";
        if (magic == C_USER_AGENT) return JS_NewString(ctx, agent);
        if (magic == C_APP_NAME) return JS_NewString(ctx, "Netscape");
        if (magic == C_APP_VERSION) {
            const char *slash = os64_strchr(agent, '/');
            return JS_NewString(ctx, slash != NULL ? slash + 1 : "");
        }
        const char *platform = contains(agent,"Windows") || contains(agent,"Win98") ? "Win32" :
            contains(agent,"iPhone") ? "iPhone" : contains(agent,"Macintosh") ? "MacIntel" : "OS64 x86_64";
        return JS_NewString(ctx, platform);
    }
    if (magic == C_IMAGE) {
        uint32_t width = 0, height = 0;
        if ((argc > 0 && JS_ToUint32(ctx, &width, argv[0]) < 0) ||
            (argc > 1 && JS_ToUint32(ctx, &height, argv[1]) < 0)) return JS_EXCEPTION;
        int64_t status;
        os64_html_node_t *node = os64_html_create_element(dom->document, OS64_HTML_NS_HTML, "img", &status);
        if (node == NULL) return d_html_error(ctx, status);
        JSValue value = d_wrap(dom, ctx, node);
        if (JS_IsException(value)) return value;
        char number[16];
        for (int i = 0; i < argc && i < 2; i++) {
            os64_snprintf(number,sizeof(number),"%u",i == 0 ? width : height);
            status = d_set_attr(dom,node,i == 0 ? "width" : "height",number,os64_strlen(number),false);
            if (status != OS64_HTML_OK) { JS_FreeValue(ctx,value); return d_page_error(ctx,status); }
        }
        return value;
    }
    const os64_html_node_t *node = d_node(dom, ctx, self);
    if (node == NULL) return JS_EXCEPTION;
    if (magic == C_IMAGES || magic == C_FORMS) {
        if (node->kind != OS64_HTML_DOCUMENT) return JS_ThrowTypeError(ctx,"Expected a document");
        return d_classic_collection(dom,ctx,node,magic == C_IMAGES ? D_QUERY_IMAGES : D_QUERY_FORMS);
    }
    if (magic == C_ELEMENTS) {
        if (node->tag != OS64_HTML_TAG_FORM) return JS_ThrowTypeError(ctx,"Expected a form");
        return d_classic_collection(dom,ctx,node,D_QUERY_CONTROLS);
    }
    if (node->kind != OS64_HTML_ELEMENT) return JS_ThrowTypeError(ctx,"Expected an element");
    if (magic == C_STYLE) return d_style(dom,ctx,self);
    if ((magic == C_SRC || magic == C_WIDTH || magic == C_HEIGHT) &&
        (node->ns != OS64_HTML_NS_HTML || node->tag != OS64_HTML_TAG_IMG))
        return JS_ThrowTypeError(ctx,"Image property requires an HTML image");
    const char *name = magic == C_TITLE ? "title" : magic == C_NAME ? "name" : magic == C_SRC ? "src" :
                       magic == C_WIDTH ? "width" : "height";
    if (set) {
        DString text = {0};
        if (magic == C_WIDTH || magic == C_HEIGHT) {
            uint32_t number;
            if (argc < 1 || JS_ToUint32(ctx,&number,argv[0]) < 0) return JS_EXCEPTION;
            char digits[16];os64_snprintf(digits,sizeof(digits),"%u",number);
            int64_t status = d_set_attr(dom,node,name,digits,os64_strlen(digits),false);
            return status == OS64_HTML_OK ? JS_UNDEFINED : d_page_error(ctx,status);
        }
        if (!d_string(dom,ctx,argc > 0 ? argv[0] : JS_UNDEFINED,&text)) return JS_EXCEPTION;
        int64_t status = d_set_attr(dom,node,name,text.data,text.length,false);
        d_string_free(dom,&text);
        return status == OS64_HTML_OK ? JS_UNDEFINED : d_page_error(ctx,status);
    }
    const os64_html_attr_t *attr = os64_html_attr(node,name);
    const char *text = attr != NULL ? attr->value : "";
    if (magic == C_SRC && attr != NULL) {
        char absolute[OS64_DOM_URL_MAX];
        if (os64_dom_resolve(dom->document,dom->url,text,absolute,sizeof(absolute))) return JS_NewString(ctx,absolute);
    }
    if (magic == C_WIDTH || magic == C_HEIGHT) {
        uint64_t number = 0;
        while (*text >= '0' && *text <= '9') {
            number = number * 10 + (unsigned)(*text++ - '0');
            if (number > UINT32_MAX) return JS_NewInt32(ctx,0);
        }
        return JS_NewUint32(ctx,(uint32_t)number);
    }
    return JS_NewString(ctx,text);
}

void os64_dom_set_user_agent(os64_dom_t *dom, const char *(*provider)(void *), void *opaque)
{
    if (dom == NULL || dom->closed) return;
    dom->user_agent = provider;
    dom->user_agent_opaque = opaque;
}

void os64_dom_set_cookies(os64_dom_t *dom, const os64_dom_cookies_t *cookies)
{
    if (dom == NULL || dom->closed) return;
    if (cookies != NULL) dom->cookies = *cookies;
    else os64_memset(&dom->cookies, 0, sizeof(dom->cookies));
}

void os64_dom_set_global_miss(os64_dom_t *dom, void (*heard)(void *opaque, const char *name),
                              void *opaque)
{
    os64_js_outcome_t outcome;
    JSContext *ctx = dom != NULL && !dom->closed ? os64_js_context(dom->runtime, OS64_JS_ABI_ID, &outcome)
                                                 : NULL;
    if (ctx != NULL)
        JS_SetGlobalMissHandler(ctx, heard, opaque);
}

/* An interface object: there to be named, never to be called. */
static JSValue illegal_constructor(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

/* `new DocumentFragment()` (DOM § 4.7): an empty fragment of the window's
 * document. Made as a JS_CFUNC_constructor, so the engine refuses every
 * call that is not a construct call before this runs (WebIDL: an
 * interface's constructor is never just called), whatever `this` the
 * caller chose; a function made that way carries no data, so it finds its
 * binding through its context (d_binding). The fragment's prototype is
 * new.target's `prototype`, so `class F extends DocumentFragment` makes an
 * F, and DocumentFragment's when that is not an object (WebIDL's "get
 * prototype from constructor"; there is one realm), read before the
 * fragment is made, since reading it may run a page's getter. */
static JSValue fragment_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return JS_EXCEPTION;
    int64_t status;
    os64_html_node_t *fragment = os64_html_create_fragment(dom->document, &status);
    JSValue made = fragment != NULL ? d_wrap_constructed(dom, ctx, fragment, prototype)
                                    : d_html_error(ctx, status);
    JS_FreeValue(ctx, prototype);
    return made;
}

/* `new Text(data)` and `new Comment(data)` (DOM § 4.11, § 4.14): a node of
 * the window's document holding `data`, "" when it is undefined. As the
 * fragment's constructor, a construct call only, its prototype read from
 * new.target first, and the argument converted after (WebIDL converts in
 * that order, and either may run a page's script). `magic` is the kind. */
static JSValue character_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv,
                                     int magic)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return JS_EXCEPTION;
    DString data = {0};
    if (argc > 0 && !JS_IsUndefined(argv[0]) && !d_string(dom, ctx, argv[0], &data)) {
        JS_FreeValue(ctx, prototype);
        return JS_EXCEPTION;
    }
    int64_t status;
    const char *text = data.data != NULL ? data.data : "";
    os64_html_node_t *node = magic == D_PROTO_TEXT
        ? os64_html_create_text(dom->document, text, data.length, &status)
        : os64_html_create_comment(dom->document, text, data.length, &status);
    d_string_free(dom, &data);
    JSValue made = node != NULL ? d_wrap_constructed(dom, ctx, node, prototype) : d_html_error(ctx, status);
    JS_FreeValue(ctx, prototype);
    return made;
}

/* An interface object's [[Prototype]] set to its parent interface's. The
 * engine's JS_SetPrototype takes its reference to the new prototype before
 * it copies a shape the object shares, and keeps it when that copy runs out
 * of memory, which leaves the parent alive past the runtime. Giving the
 * object a shape of its own first (`length`'s flags changed and changed
 * back, either of which fails cleanly) leaves the set nothing to
 * allocate. `length` is the interface's: its constructor's required
 * arguments, 0 for one that constructs nothing. */
static int interface_parent(JSContext *ctx, JSValueConst f, JSValueConst parent, int length)
{
    if (JS_DefinePropertyValueStr(ctx, f, "length", JS_NewInt32(ctx, length),
                                  JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE) < 0 ||
        JS_DefinePropertyValueStr(ctx, f, "length", JS_NewInt32(ctx, length), JS_PROP_CONFIGURABLE) < 0)
        return -1;
    return JS_SetPrototype(ctx, f, parent) < 0 ? -1 : 0;
}

/* Node's constants (DOM § 4.4), on the interface object and its prototype,
 * as WebIDL puts every constant: what `n.nodeType === Node.ELEMENT_NODE`
 * reads. */
static int node_constants(JSContext *ctx, JSValueConst target)
{
    static const struct { const char *name; int value; } kConstants[] = {
        {"ELEMENT_NODE", 1}, {"ATTRIBUTE_NODE", 2}, {"TEXT_NODE", 3}, {"CDATA_SECTION_NODE", 4},
        {"ENTITY_REFERENCE_NODE", 5}, {"ENTITY_NODE", 6}, {"PROCESSING_INSTRUCTION_NODE", 7},
        {"COMMENT_NODE", 8}, {"DOCUMENT_NODE", 9}, {"DOCUMENT_TYPE_NODE", 10},
        {"DOCUMENT_FRAGMENT_NODE", 11}, {"NOTATION_NODE", 12},
        {"DOCUMENT_POSITION_DISCONNECTED", 0x01}, {"DOCUMENT_POSITION_PRECEDING", 0x02},
        {"DOCUMENT_POSITION_FOLLOWING", 0x04}, {"DOCUMENT_POSITION_CONTAINS", 0x08},
        {"DOCUMENT_POSITION_CONTAINED_BY", 0x10}, {"DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC", 0x20},
    };
    for (unsigned i = 0; i < sizeof(kConstants) / sizeof(kConstants[0]); i++)
        if (JS_DefinePropertyValueStr(ctx, target, kConstants[i].name, JS_NewInt32(ctx, kConstants[i].value),
                                      JS_PROP_ENUMERABLE) < 0)
            return -1;
    return 0;
}

/* Symbol.toStringTag as a key, read off the global's Symbol before any page
 * has run; JS_ATOM_NULL when it cannot be read. */
static JSAtom string_tag_key(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue symbol = JS_GetPropertyStr(ctx, global, "Symbol");
    JSValue tag = JS_GetPropertyStr(ctx, symbol, "toStringTag");
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, symbol);
    JSAtom key = JS_IsSymbol(tag) ? JS_ValueToAtom(ctx, tag) : JS_ATOM_NULL;
    JS_FreeValue(ctx, tag);
    return key;
}

/* An interface prototype object's own `[Symbol.toStringTag]`: its
 * interface's name, configurable and neither writable nor enumerable, as
 * WebIDL § 3.7.3 has it. Object.prototype.toString reads it, so
 * `String(document.body)` is "[object HTMLElement]". */
static int string_tag(JSContext *ctx, JSValueConst prototype, JSAtom key, const char *name)
{
    JSValue text = JS_NewString(ctx, name);
    if (JS_IsException(text)) return -1;
    return JS_DefinePropertyValue(ctx, prototype, key, text, JS_PROP_CONFIGURABLE) < 0 ? -1 : 0;
}

/* The interface objects for the prototypes the binding has (WebIDL § 3.7):
 * `Element.prototype` is what every element inherits, so `el instanceof
 * Element` holds and a page may patch the prototype, as the Wayback
 * Machine's player does. One interface per prototype: an interface with no
 * prototype of its own here (most HTML elements have only HTMLElement's,
 * and SVG's subclasses only SVGElement's) is not installed, since aliasing
 * it to a broader one would make `instanceof` lie, and the census says what
 * is missing (INTERFACES_FORMS.md). Each prototype's `constructor` is its
 * interface, and each interface object inherits its parent's
 * (`Object.getPrototypeOf(HTMLElement) === Element`), as its prototype
 * inherits its parent prototype (d_proto_parent); Node's is
 * Function.prototype. An interface whose standard gives it a constructor
 * the binding can honour constructs (DocumentFragment, Text, Comment); the
 * rest are Illegal constructors, as WebIDL makes
 * an interface without one (`new Document()`, which makes an XML document,
 * is one of these here). Node's constants come with Node, and each
 * prototype's Symbol.toStringTag with its interface. NamedNodeMap, whose
 * objects are no nodes, comes after them, over attr.c's prototype. The
 * Modern Web table's row (modern.c), so all of it exists only while
 * options.modern is set. */
int d_interfaces_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const struct { const char *name; unsigned proto; } kInterfaces[] = {
        {"Node", D_PROTO_NODE}, {"CharacterData", D_PROTO_CHARACTER_DATA},
        {"Text", D_PROTO_TEXT}, {"Comment", D_PROTO_COMMENT},
        {"DocumentFragment", D_PROTO_FRAGMENT}, {"Document", D_PROTO_DOCUMENT},
        {"DocumentType", D_PROTO_DOCTYPE}, {"Attr", D_PROTO_ATTR},
        {"Element", D_PROTO_ELEMENT}, {"SVGElement", D_PROTO_SVG}, {"HTMLElement", D_PROTO_HTML_ELEMENT},
        {"HTMLFormElement", D_PROTO_FORM}, {"HTMLInputElement", D_PROTO_INPUT},
        {"HTMLTextAreaElement", D_PROTO_TEXTAREA}, {"HTMLSelectElement", D_PROTO_SELECT},
        {"HTMLOptionElement", D_PROTO_OPTION}, {"HTMLOptGroupElement", D_PROTO_OPTGROUP},
        {"HTMLTemplateElement", D_PROTO_TEMPLATE},
        {"HTMLButtonElement", D_PROTO_BUTTON}, {"HTMLImageElement", D_PROTO_IMAGE},
        {"HTMLScriptElement", D_PROTO_SCRIPT}, {"HTMLAnchorElement", D_PROTO_ANCHOR},
        {"HTMLLinkElement", D_PROTO_LINK}, {"HTMLLabelElement", D_PROTO_LABEL},
        {"HTMLFieldSetElement", D_PROTO_FIELDSET}, {"HTMLLegendElement", D_PROTO_LEGEND},
        {"HTMLOutputElement", D_PROTO_OUTPUT}, {"HTMLStyleElement", D_PROTO_STYLE},
        {"HTMLMetaElement", D_PROTO_META}, {"HTMLCanvasElement", D_PROTO_CANVAS},
        {"HTMLUnknownElement", D_PROTO_UNKNOWN}, {"HTMLTimeElement", D_PROTO_TIME},
        {"HTMLDivElement", D_PROTO_DIV}, {"HTMLBodyElement", D_PROTO_BODY},
        {"HTMLOListElement", D_PROTO_OLIST}, {"HTMLHeadElement", D_PROTO_HEAD},
        {"HTMLDataElement", D_PROTO_DATA}, {"HTMLLIElement", D_PROTO_LI}, {"HTMLAreaElement", D_PROTO_AREA},
        {"HTMLBaseElement", D_PROTO_BASE}, {"HTMLMapElement", D_PROTO_MAP}, {"HTMLModElement", D_PROTO_MOD},
        {"HTMLQuoteElement", D_PROTO_QUOTE}, {"HTMLTableCellElement", D_PROTO_TABLE_CELL},
        {"HTMLTableColElement", D_PROTO_TABLE_COL}, {"HTMLTableElement", D_PROTO_TABLE},
        {"HTMLTableRowElement", D_PROTO_TABLE_ROW}, {"HTMLTableSectionElement", D_PROTO_TABLE_SECTION},
        {"HTMLUListElement", D_PROTO_ULIST}, {"HTMLBRElement", D_PROTO_BR}, {"HTMLDListElement", D_PROTO_DLIST},
        {"HTMLDirectoryElement", D_PROTO_DIRECTORY}, {"HTMLFontElement", D_PROTO_FONT},
        {"HTMLHRElement", D_PROTO_HR}, {"HTMLHeadingElement", D_PROTO_HEADING},
        {"HTMLHtmlElement", D_PROTO_HTML}, {"HTMLMenuElement", D_PROTO_MENU},
        {"HTMLParagraphElement", D_PROTO_PARAGRAPH}, {"HTMLPreElement", D_PROTO_PRE},
        {"HTMLSpanElement", D_PROTO_SPAN}, {"HTMLTableCaptionElement", D_PROTO_TABLE_CAPTION},
        {"HTMLTitleElement", D_PROTO_TITLE},
    };
    /* HTMLIFrameElement's is the frames row's (frames.c), with its members:
     * IFRAME.md's HTMLIFrameElement is one row. */
    _Static_assert(sizeof(kInterfaces) / sizeof(kInterfaces[0]) == D_PROTO_COUNT - 1,
                   "an interface object for each of the binding's prototypes but the frames row's");
    /* Each interface object made so far, by prototype kind, borrowed from
     * the global that holds it; the table names a parent before its
     * children. */
    JSValue made[D_PROTO_COUNT];
    for (unsigned k = 0; k < D_PROTO_COUNT; k++) made[k] = JS_UNDEFINED;
    JSAtom tag = string_tag_key(ctx);
    if (tag == JS_ATOM_NULL) return -1;
    int result = 0;
    for (unsigned i = 0; result == 0 && i < sizeof(kInterfaces) / sizeof(kInterfaces[0]); i++) {
        unsigned proto = kInterfaces[i].proto;
        JSValueConst prototype = dom->prototypes[proto]->value;
        int parent = d_proto_parent((int)proto);
        result = -1;
        if (parent >= 0 && JS_IsUndefined(made[parent])) break;
        JSValue f = proto == D_PROTO_FRAGMENT
            ? JS_NewCFunction2(ctx, fragment_constructor, kInterfaces[i].name, 0, JS_CFUNC_constructor, 0)
            : proto == D_PROTO_TEXT || proto == D_PROTO_COMMENT
            ? JS_NewCFunctionMagic(ctx, character_constructor, kInterfaces[i].name, 0, JS_CFUNC_constructor_magic,
                                   (int)proto)
            : JS_NewCFunction2(ctx, illegal_constructor, kInterfaces[i].name, 0, JS_CFUNC_constructor_or_func, 0);
        if (JS_IsException(f)) break;
        if (JS_SetConstructor(ctx, f, prototype) < 0 ||
            string_tag(ctx, prototype, tag, kInterfaces[i].name) < 0 ||
            (parent >= 0 && interface_parent(ctx, f, made[parent], 0) < 0) ||
            (proto == D_PROTO_NODE && (node_constants(ctx, f) < 0 || node_constants(ctx, prototype) < 0))) {
            JS_FreeValue(ctx, f);
            break;
        }
        made[proto] = f;
        if (JS_DefinePropertyValueStr(ctx, global, kInterfaces[i].name, f,
                                      JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
            break;
        result = 0;
    }
    JS_FreeAtom(ctx, tag);
    if (result < 0) return -1;
    /* NamedNodeMap's objects are no nodes, so its prototype is attr.c's. */
    JSValue map = d_attr_map_interface(dom, ctx);
    if (JS_IsException(map)) return -1;
    if (JS_DefinePropertyValueStr(ctx, global, "NamedNodeMap", map, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    /* The lists the nodes hand out, each over the prototype its objects
     * already have (0f's P1b): a NodeList's (collection.c's static list
     * prototype, which childNodes and getElementsByName have too), an
     * HTMLCollection's (every other live collection), classList's and
     * relList's DOMTokenList, dataset's DOMStringMap. None constructs. */
    JSValue collection = JS_GetClassProto(ctx, dom->collection_class);
    JSValue tokens = JS_GetClassProto(ctx, dom->token_class);
    JSValue strings = JS_GetClassProto(ctx, dom->map_class);
    const struct { const char *name; JSValueConst prototype; } kLists[] = {
        {"NodeList", dom->static_list->value}, {"HTMLCollection", collection},
        {"DOMTokenList", tokens}, {"DOMStringMap", strings},
    };
    for (size_t i = 0; result == 0 && i < sizeof(kLists) / sizeof(kLists[0]); i++) {
        JSValue f = d_interface_new(ctx, kLists[i].name, 0, NULL, kLists[i].prototype, JS_UNDEFINED);
        if (JS_IsException(f) ||
            JS_DefinePropertyValueStr(ctx, global, kLists[i].name, f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
            result = -1;
    }
    JS_FreeValue(ctx, collection);
    JS_FreeValue(ctx, tokens);
    JS_FreeValue(ctx, strings);
    return result;
}

JSValue d_interface_new(JSContext *ctx, const char *name, int length, JSCFunction *constructor,
                        JSValueConst prototype, JSValueConst parent)
{
    JSValue f = constructor != NULL
        ? JS_NewCFunction2(ctx, constructor, name, length, JS_CFUNC_constructor, 0)
        : JS_NewCFunction2(ctx, illegal_constructor, name, 0, JS_CFUNC_constructor_or_func, 0);
    if (JS_IsException(f)) return f;
    JSAtom tag = string_tag_key(ctx);
    bool made = tag != JS_ATOM_NULL && JS_SetConstructor(ctx, f, prototype) >= 0 &&
                string_tag(ctx, prototype, tag, name) >= 0 &&
                (!JS_IsObject(parent) || interface_parent(ctx, f, parent, constructor != NULL ? length : 0) >= 0);
    if (tag != JS_ATOM_NULL) JS_FreeAtom(ctx, tag);
    if (made) return f;
    JS_FreeValue(ctx, f);
    return JS_EXCEPTION;
}

int d_interface_class(os64_dom_t *dom, JSContext *ctx, JSValueConst global, JSClassID id,
                      const JSClassDef *definition, JSValue *out)
{
    if (!JS_IsRegisteredClass(dom->engine, id) && JS_NewClass(dom->engine, id, definition) < 0) return -1;
    JSValue prototype = JS_NewObject(ctx);
    DValue *held = d_retain(dom, ctx, prototype);
    if (held == NULL) return -1;
    JS_SetClassProto(ctx, id, JS_DupValue(ctx, prototype));
    JSValue f = d_interface_new(ctx, definition->class_name, 0, NULL, prototype, JS_UNDEFINED);
    if (JS_IsException(f) ||
        JS_DefinePropertyValueStr(ctx, global, definition->class_name, f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    *out = held->value;
    return 0;
}

int d_classic_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    if (d_style_install(dom, ctx) < 0) return -1;
    JSValue object = JS_NewObject(ctx);
    dom->navigator = d_retain(dom,ctx,object);
    if (dom->navigator == NULL) return -1;
    const char *names[] = {"userAgent","appName","appVersion","platform"};
    for (unsigned i = 0; i < 4; i++)
        if (d_accessor(dom,ctx,object,names[i],classic,C_USER_AGENT + (int)i,false) < 0) return -1;
    if (d_accessor(dom,ctx,global,"navigator",classic,C_NAVIGATOR,false) < 0 ||
        d_accessor(dom,ctx,global,"event",classic,C_CURRENT_EVENT,false) < 0) return -1;
    JSValueConst doc = dom->prototypes[D_PROTO_DOCUMENT]->value;
    JSValueConst el = dom->prototypes[D_PROTO_ELEMENT]->value;
    JSValueConst form = dom->prototypes[D_PROTO_FORM]->value;
    JSValueConst image = dom->prototypes[D_PROTO_IMAGE]->value;
    if (d_accessor(dom,ctx,doc,"images",classic,C_IMAGES,false) < 0 ||
        d_accessor(dom,ctx,doc,"forms",classic,C_FORMS,false) < 0 ||
        d_accessor(dom,ctx,form,"elements",classic,C_ELEMENTS,false) < 0 ||
        d_accessor(dom,ctx,el,"style",classic,C_STYLE,false) < 0 ||
        d_accessor(dom,ctx,dom->prototypes[D_PROTO_HTML_ELEMENT]->value,"title",classic,C_TITLE,true) < 0 ||
        d_accessor(dom,ctx,doc,"cookie",classic,C_COOKIE,true) < 0 ||
        d_accessor(dom,ctx,image,"src",classic,C_SRC,true) < 0 ||
        d_accessor(dom,ctx,image,"width",classic,C_WIDTH,true) < 0 ||
        d_accessor(dom,ctx,image,"height",classic,C_HEIGHT,true) < 0 ||
        d_method(dom,ctx,doc,"captureEvents",classic,1,C_CAPTURE) < 0 ||
        d_method(dom,ctx,doc,"releaseEvents",classic,1,C_CAPTURE) < 0 ||
        d_implementation_classic(dom,ctx) < 0 || d_attr_classic(dom,ctx) < 0) return -1;
    const unsigned named_kinds[] = {D_PROTO_IMAGE,D_PROTO_FORM,D_PROTO_INPUT,
        D_PROTO_TEXTAREA,D_PROTO_SELECT,D_PROTO_BUTTON};
    for (unsigned i = 0; i < sizeof(named_kinds)/sizeof(named_kinds[0]); i++)
        if (d_accessor(dom,ctx,dom->prototypes[named_kinds[i]]->value,
                       "name",classic,C_NAME,true) < 0) return -1;
    JSValue constructor = JS_NewCFunctionData(ctx,classic,2,C_IMAGE,1,&dom->anchor->value);
    if (JS_IsException(constructor)) return -1;
    if (!JS_SetConstructorBit(ctx,constructor,true) || JS_SetConstructor(ctx,constructor,image) < 0) {
        JS_FreeValue(ctx,constructor);return -1;
    }
    if (JS_DefinePropertyValueStr(ctx,global,"Image",constructor,JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) return -1;
    JSValue function = JS_GetPropertyStr(ctx,global,"Function");
    if (JS_IsException(function)) return -1;
    JSValue function_prototype = JS_GetPropertyStr(ctx,function,"prototype");
    JS_FreeValue(ctx,function);
    if (JS_IsException(function_prototype)) return -1;
    JSAtom legacy_name = JS_NewAtom(ctx,"arguments");
    if (legacy_name == JS_ATOM_NULL) { JS_FreeValue(ctx,function_prototype);return -1; }
    JSValue legacy_getter = JS_NewCFunctionData(ctx,classic,0,C_LEGACY_ARGUMENTS,1,&dom->anchor->value);
    int installed = JS_IsException(legacy_getter) ? -1 :
        JS_DefinePropertyGetSet(ctx,function_prototype,legacy_name,
            legacy_getter,JS_UNDEFINED,JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx,legacy_name);
    JS_FreeValue(ctx,function_prototype);
    if (installed < 0) return -1;
    JSValue event = JS_GetPropertyStr(ctx,global,"Event");
    if (JS_IsException(event)) return -1;
    int result = JS_DefinePropertyValueStr(ctx,event,"MOUSEMOVE",JS_NewInt32(ctx,16),JS_PROP_ENUMERABLE);
    JS_FreeValue(ctx,event);
    return result;
}

int d_string_tag(JSContext *ctx, JSValueConst object, const char *name)
{
    JSAtom key = string_tag_key(ctx);
    if (key == JS_ATOM_NULL) return -1;
    int result = string_tag(ctx, object, key, name);
    JS_FreeAtom(ctx, key);
    return result;
}
