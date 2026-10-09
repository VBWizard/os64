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

static const os64_html_node_t *dom_root(os64_dom_t *dom, const os64_html_node_t *node)
{
    const os64_html_node_t *root = node;
    while (root->parent != NULL) root = root->parent;
    return root == dom->document->document ? root : node;
}

bool d_named_match(const os64_html_node_t *node, const char *name)
{
    if (node->kind != OS64_HTML_ELEMENT || node->ns != OS64_HTML_NS_HTML || *name == '\0') return false;
    const os64_html_attr_t *id = os64_html_attr(node, "id"), *named = os64_html_attr(node, "name");
    return (id != NULL && os64_streq(id->value, name)) || (named != NULL && os64_streq(named->value, name));
}

bool d_classic_match(os64_dom_t *dom, const os64_html_node_t *root,
                      const os64_html_node_t *node, unsigned kind)
{
    if (node->kind != OS64_HTML_ELEMENT || node->ns != OS64_HTML_NS_HTML) return false;
    switch (kind) {
    case D_QUERY_IMAGES: return node->tag == OS64_HTML_TAG_IMG;
    case D_QUERY_FORMS: return node->tag == OS64_HTML_TAG_FORM;
    case D_QUERY_DOCUMENT_NAMES:
        return node->tag == OS64_HTML_TAG_FORM || node->tag == OS64_HTML_TAG_IMG;
    case D_QUERY_CONTROLS:
        return (node->tag == OS64_HTML_TAG_INPUT || node->tag == OS64_HTML_TAG_SELECT ||
                node->tag == OS64_HTML_TAG_TEXTAREA || node->tag == OS64_HTML_TAG_BUTTON) &&
               d_form_owner(dom, node) == root;
    default: return true;
    }
}

JSValue d_classic_collection(os64_dom_t *dom, JSContext *ctx,
                              const os64_html_node_t *node, unsigned kind)
{
    DValue *entry = d_find(dom, node);
    DQuery **slot = kind == D_QUERY_IMAGES ? &entry->images : kind == D_QUERY_FORMS ? &entry->forms : &entry->elements;
    if (*slot != NULL) return JS_DupValue(ctx, (*slot)->entry->value);
    const os64_html_node_t *root = kind == D_QUERY_CONTROLS ? dom_root(dom, node) : node;
    JSValue value = d_special_collection(dom, ctx, root, kind, NULL);
    if (!JS_IsException(value)) {
        *slot = JS_GetOpaque(value, dom->collection_class);
        // The search covers the document so controls with form= participate.
        // Its owner remains separate from the traversal root.
        if (kind == D_QUERY_CONTROLS) (*slot)->owner = node;
    }
    return value;
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
    JSValue prototype = JS_GetPrototype(ctx, object);
    if (JS_IsException(prototype)) return -1;
    int reserved = JS_IsNull(prototype) ? 0 : JS_HasProperty(ctx, prototype, atom);
    JS_FreeValue(ctx, prototype);
    if (reserved != 0) return reserved < 0 ? -1 : 0;
    JSValue key = JS_AtomToValue(ctx, atom);
    if (JS_IsException(key)) return -1;
    if (!JS_IsString(key)) { JS_FreeValue(ctx, key); return 0; }
    const char *name = JS_ToCString(ctx, key);
    JS_FreeValue(ctx, key);
    if (name == NULL) return -1;
    const os64_html_node_t *search = dom_root(entry->dom, root);
    const os64_html_node_t *first = NULL;
    size_t count = 0;
    for (const os64_html_node_t *at = search->first_child; at != NULL; at = next_node(at, search))
        if (d_classic_match(entry->dom, root, at, kind) && d_named_match(at, name)) {
            if (first == NULL) first = at;
            count++;
        }
    if (count == 0) { JS_FreeCString(ctx, name); return 0; }
    JSValue value = JS_UNDEFINED;
    if (desc != NULL) {
        value = count == 1 ? d_wrap(entry->dom, ctx, first) : d_special_collection(entry->dom, ctx, search, kind, name);
        if (!JS_IsException(value) && count > 1) {
            DQuery *query = JS_GetOpaque(value, entry->dom->collection_class);
            query->owner = root;
        }
    }
    JS_FreeCString(ctx, name);
    if (JS_IsException(value)) return -1;
    if (desc != NULL) *desc = (JSPropertyDescriptor){.flags = JS_PROP_CONFIGURABLE,
        .value = value, .getter = JS_UNDEFINED, .setter = JS_UNDEFINED};
    return 1;
}

JSClassExoticMethods d_node_exotic = {.get_own_property = named_property};

enum { C_IMAGES, C_FORMS, C_ELEMENTS, C_STYLE, C_TITLE, C_NAME, C_SRC, C_WIDTH, C_HEIGHT,
       C_CAPTURE, C_IMAGE, C_NAVIGATOR, C_USER_AGENT, C_APP_NAME, C_APP_VERSION, C_PLATFORM, C_CURRENT_EVENT, C_LEGACY_ARGUMENTS };

static JSValue classic(JSContext *ctx, JSValueConst self, int argc,
                        JSValueConst *argv, int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    if (magic == C_CAPTURE) return JS_UNDEFINED; // event propagation is already enabled
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
            status = os64_page_node_set_attr(dom->state,node,i == 0 ? "width" : "height",number,os64_strlen(number),false);
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
            int64_t status = os64_page_node_set_attr(dom->state,node,name,digits,os64_strlen(digits),false);
            return status == OS64_HTML_OK ? JS_UNDEFINED : d_page_error(ctx,status);
        }
        if (!d_string(dom,ctx,argc > 0 ? argv[0] : JS_UNDEFINED,&text)) return JS_EXCEPTION;
        int64_t status = os64_page_node_set_attr(dom->state,node,name,text.data,text.length,false);
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

/* The interface objects for the prototypes the binding has (WebIDL § 3.7):
 * `Element.prototype` is what every element inherits, so `el instanceof
 * Element` holds and a page may patch the prototype, as the Wayback
 * Machine's player does. An interface with no prototype of its own here
 * (HTMLAnchorElement, Text) is not installed: aliasing it to a broader one
 * would make `instanceof` lie, and the census says what is missing. Each
 * prototype's `constructor` is its most specific interface, the last
 * named for it. */
static int interfaces_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const struct { const char *name; unsigned proto; } kInterfaces[] = {
        {"Node", D_PROTO_NODE}, {"CharacterData", D_PROTO_CHARACTER_DATA},
        {"DocumentFragment", D_PROTO_FRAGMENT}, {"Document", D_PROTO_DOCUMENT},
        {"Element", D_PROTO_ELEMENT}, {"HTMLElement", D_PROTO_ELEMENT},
        {"HTMLFormElement", D_PROTO_FORM}, {"HTMLInputElement", D_PROTO_INPUT},
        {"HTMLTextAreaElement", D_PROTO_TEXTAREA}, {"HTMLSelectElement", D_PROTO_SELECT},
        {"HTMLButtonElement", D_PROTO_BUTTON}, {"HTMLImageElement", D_PROTO_IMAGE},
    };
    for (unsigned i = 0; i < sizeof(kInterfaces) / sizeof(kInterfaces[0]); i++) {
        JSValue f = JS_NewCFunction2(ctx, illegal_constructor, kInterfaces[i].name, 0,
                                     JS_CFUNC_constructor_or_func, 0);
        if (JS_IsException(f)) return -1;
        if (JS_SetConstructor(ctx, f, dom->prototypes[kInterfaces[i].proto]->value) < 0) {
            JS_FreeValue(ctx, f);
            return -1;
        }
        if (JS_DefinePropertyValueStr(ctx, global, kInterfaces[i].name, f,
                                      JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
            return -1;
    }
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
        d_accessor(dom,ctx,el,"title",classic,C_TITLE,true) < 0 ||
        d_accessor(dom,ctx,image,"src",classic,C_SRC,true) < 0 ||
        d_accessor(dom,ctx,image,"width",classic,C_WIDTH,true) < 0 ||
        d_accessor(dom,ctx,image,"height",classic,C_HEIGHT,true) < 0 ||
        d_method(dom,ctx,doc,"captureEvents",classic,1,C_CAPTURE) < 0 ||
        d_method(dom,ctx,doc,"releaseEvents",classic,1,C_CAPTURE) < 0) return -1;
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
    return result < 0 ? -1 : interfaces_install(dom,ctx,global);
}
