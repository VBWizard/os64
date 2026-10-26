#include "internal.h"

/* SELECTIONS (0f's P2, its core; INTERFACES_FORMS.md). Two kinds:
 *
 * A text control's (HTML § 4.10.6): selectionStart, selectionEnd,
 * selectionDirection, setSelectionRange() and select() on input and
 * textarea, over libpage's record of it (os64_page_node_selection), in
 * UTF-16 code units. An input of a type without one (checkbox, email,
 * number...) answers null and throws InvalidStateError on a set, as HTML
 * says. React's selection tracking asks the focused field for these, and
 * without them fell through to window.getSelection().
 *
 * The page's (the Selection API): window.getSelection() and
 * document.getSelection() answer the window's one Selection. yonder keeps
 * no selection of the page's text a script could see, so it holds none:
 * rangeCount 0, type "None", every node null. It answers the reads a page
 * makes before it acts on a selection; what would change one (addRange,
 * collapse, extend) is not here, so a page that calls it finds it missing.
 *
 * One row of the Modern Web table, naming getSelection and Selection. */

enum { X_START, X_END, X_DIRECTION, X_SET_RANGE, X_SELECT };
enum { W_GET, Y_ANCHOR_NODE, Y_ANCHOR_OFFSET, Y_FOCUS_NODE, Y_FOCUS_OFFSET, Y_COLLAPSED, Y_RANGE_COUNT, Y_TYPE,
       Y_DIRECTION, Y_GET_RANGE, Y_REMOVE_ALL, Y_TO_STRING };

static const char *const kDirections[] = {"none", "forward", "backward"};

/* A direction's name read as HTML reads it: "forward" and "backward", and
 * anything else "none". */
static os64_page_direction_t direction_of(const DString *name)
{
    const char *text = name->data != NULL ? name->data : "";
    if (os64_streq(text, "forward")) return OS64_PAGE_DIRECTION_FORWARD;
    if (os64_streq(text, "backward")) return OS64_PAGE_DIRECTION_BACKWARD;
    return OS64_PAGE_DIRECTION_NONE;
}

static JSValue no_selection(JSContext *ctx)
{
    return d_error(ctx, "InvalidStateError", "This control has no text selection");
}

/* Reading or keeping the selection ran out of the page's state budget. */
static JSValue no_room(JSContext *ctx)
{
    return d_error(ctx, "QuotaExceededError", "DOM state quota exceeded");
}

static JSValue control_member(JSContext *ctx, JSValueConst this_value, int argc, JSValueConst *argv,
                              int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    const os64_html_node_t *self = d_node(dom, ctx, this_value);
    if (self == NULL) return JS_EXCEPTION;
    uint32_t start, end;
    os64_page_direction_t direction;
    int64_t read = os64_page_node_selection(dom->state, self, &start, &end, &direction);
    if (read == -OS64_PAGE_REASON_NO_MEMORY) return no_room(ctx);
    bool has = read == 0;
    if (!set && magic != X_SET_RANGE && magic != X_SELECT) {
        if (!has) return JS_NULL;
        return magic == X_START ? JS_NewUint32(ctx, start) : magic == X_END ? JS_NewUint32(ctx, end)
                                                                 : JS_NewString(ctx, kDirections[direction]);
    }
    /* The arguments are converted before the control is asked again: a
     * conversion may run a page's script. */
    uint32_t a = 0, b = 0;
    DString name = {0};
    bool named = false;
    JSValueConst value = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (magic == X_START || magic == X_END) {
        if (JS_ToUint32(ctx, magic == X_START ? &a : &b, value) < 0) return JS_EXCEPTION;
    } else if (magic == X_DIRECTION) {
        if (!d_string(dom, ctx, value, &name)) return JS_EXCEPTION;
        named = true;
    } else if (magic == X_SET_RANGE) {
        if (argc < 2) return JS_ThrowTypeError(ctx, "setSelectionRange needs a start and an end");
        if (JS_ToUint32(ctx, &a, argv[0]) < 0 || JS_ToUint32(ctx, &b, argv[1]) < 0) return JS_EXCEPTION;
        if (argc > 2 && !JS_IsUndefined(argv[2])) {
            if (!d_string(dom, ctx, argv[2], &name)) return JS_EXCEPTION;
            named = true;
        }
    }
    read = os64_page_node_selection(dom->state, self, &start, &end, &direction);
    if (read == -OS64_PAGE_REASON_NO_MEMORY) {
        d_string_free(dom, &name);
        return no_room(ctx);
    }
    has = read == 0;
    if (!has) {
        d_string_free(dom, &name);
        /* select() on a control without a selection does nothing, as HTML
         * says; every other change throws. */
        return magic == X_SELECT ? JS_UNDEFINED : no_selection(ctx);
    }
    switch (magic) {
    case X_START: start = a; if (end < start) end = start; break;
    case X_END: end = b; break;
    case X_DIRECTION: direction = direction_of(&name); break;
    case X_SET_RANGE: start = a; end = b; direction = named ? direction_of(&name) : OS64_PAGE_DIRECTION_NONE; break;
    default: start = 0; end = UINT32_MAX; direction = OS64_PAGE_DIRECTION_NONE; break;
    }
    d_string_free(dom, &name);
    int64_t status = os64_page_node_set_selection(dom->state, self, start, end, direction);
    if (status == -OS64_PAGE_REASON_NO_MEMORY) return no_room(ctx);
    return status < 0 ? no_selection(ctx) : JS_UNDEFINED;
}

/* ── The page's Selection ─────────────────────────────────────────────── */

static JSClassID selection_slot;

static JSValue selection_member(JSContext *ctx, JSValueConst this_value, int argc, JSValueConst *argv,
                                int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (magic == W_GET) return JS_DupValue(ctx, dom->selection->value);
    if (JS_GetOpaque(this_value, os64_js_class_id(&selection_slot)) == NULL)
        return JS_ThrowTypeError(ctx, "Expected a Selection");
    switch (magic) {
    case Y_ANCHOR_NODE: case Y_FOCUS_NODE: return JS_NULL;
    case Y_ANCHOR_OFFSET: case Y_FOCUS_OFFSET: case Y_RANGE_COUNT: return JS_NewInt32(ctx, 0);
    case Y_COLLAPSED: return JS_TRUE;
    case Y_TYPE: return JS_NewString(ctx, "None");
    case Y_DIRECTION: return JS_NewString(ctx, "none");
    case Y_GET_RANGE: {
        uint32_t index;
        if (argc < 1) return JS_ThrowTypeError(ctx, "Missing index");
        if (JS_ToUint32(ctx, &index, argv[0]) < 0) return JS_EXCEPTION;
        return d_error(ctx, "IndexSizeError", "The selection holds no range");
    }
    case Y_REMOVE_ALL: return JS_UNDEFINED;
    case Y_TO_STRING: return JS_NewString(ctx, "");
    default: return JS_ThrowTypeError(ctx, "Unknown Selection member");
    }
}

int d_selection_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const struct { const char *name; int magic; bool writable; } kControl[] = {
        {"selectionStart", X_START, true}, {"selectionEnd", X_END, true}, {"selectionDirection", X_DIRECTION, true},
    };
    const int kinds[] = {D_PROTO_INPUT, D_PROTO_TEXTAREA};
    for (size_t k = 0; k < 2; k++) {
        JSValueConst prototype = dom->prototypes[kinds[k]]->value;
        for (size_t i = 0; i < sizeof(kControl) / sizeof(kControl[0]); i++)
            if (d_accessor(dom, ctx, prototype, kControl[i].name, control_member, kControl[i].magic,
                           kControl[i].writable) < 0)
                return -1;
        if (d_method(dom, ctx, prototype, "setSelectionRange", control_member, 2, X_SET_RANGE) < 0 ||
            d_method(dom, ctx, prototype, "select", control_member, 0, X_SELECT) < 0)
            return -1;
    }
    static const JSClassDef definition = {.class_name = "Selection"};
    JSClassID id = os64_js_class_id(&selection_slot);
    if (!JS_IsRegisteredClass(dom->engine, id) && JS_NewClass(dom->engine, id, &definition) < 0) return -1;
    JSValue prototype = JS_NewObject(ctx);
    if (d_retain(dom, ctx, prototype) == NULL) return -1;
    JS_SetClassProto(ctx, id, JS_DupValue(ctx, prototype));
    static const struct { const char *name; int magic; } kReads[] = {
        {"anchorNode", Y_ANCHOR_NODE}, {"anchorOffset", Y_ANCHOR_OFFSET}, {"focusNode", Y_FOCUS_NODE},
        {"focusOffset", Y_FOCUS_OFFSET}, {"isCollapsed", Y_COLLAPSED}, {"rangeCount", Y_RANGE_COUNT},
        {"type", Y_TYPE}, {"direction", Y_DIRECTION},
    };
    for (size_t i = 0; i < sizeof(kReads) / sizeof(kReads[0]); i++)
        if (d_accessor(dom, ctx, prototype, kReads[i].name, selection_member, kReads[i].magic, false) < 0) return -1;
    if (d_method(dom, ctx, prototype, "getRangeAt", selection_member, 1, Y_GET_RANGE) < 0 ||
        d_method(dom, ctx, prototype, "removeAllRanges", selection_member, 0, Y_REMOVE_ALL) < 0 ||
        d_method(dom, ctx, prototype, "empty", selection_member, 0, Y_REMOVE_ALL) < 0 ||
        d_method(dom, ctx, prototype, "toString", selection_member, 0, Y_TO_STRING) < 0)
        return -1;
    JSValue selection = JS_NewObjectClass(ctx, (int)id);
    if (JS_IsException(selection)) return -1;
    JS_SetOpaque(selection, &selection_slot);
    dom->selection = d_retain(dom, ctx, selection);
    if (dom->selection == NULL) return -1;
    JSValue f = d_interface_new(ctx, "Selection", 0, NULL, prototype, JS_UNDEFINED);
    if (JS_IsException(f) ||
        JS_DefinePropertyValueStr(ctx, global, "Selection", f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    return d_method(dom, ctx, global, "getSelection", selection_member, 0, W_GET) < 0 ||
                   d_method(dom, ctx, dom->prototypes[D_PROTO_DOCUMENT]->value, "getSelection", selection_member, 0,
                            W_GET) < 0
               ? -1 : 0;
}
