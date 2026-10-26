#include "internal.h"

/* SELECTIONS (0f's P2; INTERFACES_FORMS.md). Two kinds:
 *
 * A text control's (HTML § 4.10.20): selectionStart, selectionEnd,
 * selectionDirection, setSelectionRange(), select() and setRangeText() on
 * input and textarea, over libpage's record of it
 * (os64_page_node_selection), which the host keeps in step with the
 * field's caret, in UTF-16 code units; a set that moves it queues the
 * select event. An input of a type without one (checkbox, email,
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

/* The select event a set queued: fired at the element as a task of its own,
 * trusted, bubbling and not cancelable. */
static JSValue select_fired(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                            JSValue *data)
{
    (void)self; (void)argc; (void)argv; (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *node = d_node(dom, ctx, data[1]);
    if (node == NULL) return JS_EXCEPTION;
    JSValue event = d_event_new(dom, ctx, "select", true, false, true);
    if (JS_IsException(event)) return event;
    int result = d_dispatch(dom, ctx, event, node);
    JS_FreeValue(ctx, event);
    return result < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

/* HTML's "set the selection range" ends by queuing an element task to fire
 * select when the set moved the selection, in extent or direction: a
 * binding timer due at once, which the host runs as the task it is.
 * `before` and the record as it now stands are compared. -1 with an
 * exception pending when it could not be queued. */
static int select_queue(os64_dom_t *dom, JSContext *ctx, JSValueConst element, const os64_html_node_t *node,
                        uint32_t start, uint32_t end, os64_page_direction_t direction)
{
    uint32_t now_start, now_end;
    os64_page_direction_t now_direction;
    if (os64_page_node_selection(dom->state, node, &now_start, &now_end, &now_direction) != 0 ||
        (now_start == start && now_end == end && now_direction == direction))
        return 0;
    JSValue fn_data[2] = {dom->anchor->value, (JSValue)element};
    JSValue fired = JS_NewCFunctionData(ctx, select_fired, 0, 0, 2, fn_data);
    if (JS_IsException(fired)) return -1;
    return d_timer_add(dom, ctx, fired, 0) == 0 ? -1 : 0;
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
    uint32_t was_start = start, was_end = end;
    os64_page_direction_t was_direction = direction;
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
    if (status < 0) return no_selection(ctx);
    return select_queue(dom, ctx, this_value, self, was_start, was_end, was_direction) < 0 ? JS_EXCEPTION
                                                                                           : JS_UNDEFINED;
}

/* The byte offset in `len` bytes of UTF-8 of the `units`th UTF-16 code
 * unit. An offset between the two halves of a pair, which UTF-8 cannot
 * split, is taken past the character. */
static size_t byte_of_unit(const char *text, size_t len, uint32_t units)
{
    size_t at = 0;
    for (uint32_t seen = 0; at < len && seen < units;) {
        unsigned char c = (unsigned char)text[at];
        size_t width = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        seen += width == 4 ? 2 : 1;
        at += width;
    }
    return at < len ? at : len;
}

/* `len` bytes of UTF-8 in UTF-16 code units. */
static uint32_t units_of(const char *text, size_t len)
{
    uint32_t units = 0;
    for (size_t i = 0; i < len; i++)
        if (((unsigned char)text[i] & 0xC0) != 0x80) units += (unsigned char)text[i] >= 0xF0 ? 2 : 1;
    return units;
}

enum { M_SELECT, M_START, M_END, M_PRESERVE };

/* setRangeText(replacement[, start, end[, selectionMode]]) (HTML § 4.10.20):
 * the replacement spliced into the value at [start, end), or at the
 * selection with one argument; the value made dirty, as a script's value
 * set makes it; the selection put where the mode says. No input event
 * fires: a script did this. */
static JSValue range_text(JSContext *ctx, JSValueConst this_value, int argc, JSValueConst *argv, int magic,
                          JSValue *data)
{
    (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *self = d_node(dom, ctx, this_value);
    if (self == NULL) return JS_EXCEPTION;
    /* WebIDL's two overloads: one argument, or three or four. */
    if (argc < 1 || argc == 2) return JS_ThrowTypeError(ctx, "setRangeText takes a replacement, or one, a start and an end");
    bool ranged = argc >= 3;
    uint32_t start = 0, end = 0;
    int mode = M_PRESERVE;
    /* Left to right, as WebIDL converts them: a conversion may run a page's
     * script, which may change what the next one answers. */
    size_t added_len = 0;
    const char *added = JS_ToCStringLen(ctx, &added_len, argv[0]);
    if (added == NULL) return JS_EXCEPTION;
    if (ranged && (JS_ToUint32(ctx, &start, argv[1]) < 0 || JS_ToUint32(ctx, &end, argv[2]) < 0)) {
        JS_FreeCString(ctx, added);
        return JS_EXCEPTION;
    }
    if (argc >= 4 && !JS_IsUndefined(argv[3])) {
        const char *name = JS_ToCString(ctx, argv[3]);
        if (name == NULL) {
            JS_FreeCString(ctx, added);
            return JS_EXCEPTION;
        }
        mode = os64_streq(name, "select") ? M_SELECT : os64_streq(name, "start") ? M_START
             : os64_streq(name, "end") ? M_END : os64_streq(name, "preserve") ? M_PRESERVE : -1;
        JS_FreeCString(ctx, name);
        if (mode < 0) {
            JS_FreeCString(ctx, added);
            return JS_ThrowTypeError(ctx, "The selection mode is not select, start, end or preserve");
        }
    }
    /* Every conversion is done, so the control is read as it stands. */
    uint32_t sel_start, sel_end;
    os64_page_direction_t direction;
    int64_t read = os64_page_node_selection(dom->state, self, &sel_start, &sel_end, &direction);
    const char *value = NULL;
    size_t value_len = 0;
    if (read == 0) read = os64_page_node_value(dom->state, self, &value, &value_len);
    if (read < 0) {
        JS_FreeCString(ctx, added);
        return read == -OS64_PAGE_REASON_NO_MEMORY ? no_room(ctx) : no_selection(ctx);
    }
    if (!ranged) {
        start = sel_start;
        end = sel_end;
    } else if (start > end) {
        JS_FreeCString(ctx, added);
        return d_error(ctx, "IndexSizeError", "The start is past the end");
    }
    uint32_t length = units_of(value, value_len);
    if (start > length) start = length;
    if (end > length) end = length;
    size_t from = byte_of_unit(value, value_len, start), to = byte_of_unit(value, value_len, end);
    size_t made_len = from + added_len + (value_len - to);
    char *made = js_malloc(ctx, made_len + 1);
    if (made == NULL) {
        JS_FreeCString(ctx, added);
        return JS_EXCEPTION;
    }
    os64_memcpy(made, value, from);
    os64_memcpy(made + from, added, added_len);
    os64_memcpy(made + from + added_len, value + to, value_len - to);
    made[made_len] = '\0';
    uint32_t new_end = start + units_of(added, added_len);
    JS_FreeCString(ctx, added);
    int64_t set = os64_page_node_set_value(dom->state, self, made, made_len);
    js_free(ctx, made);
    if (set < 0) return set == -OS64_PAGE_REASON_NO_MEMORY ? no_room(ctx) : no_selection(ctx);
    /* What the closing "set the selection range" compares with: the
     * selection as the new value left it. */
    uint32_t was_start = 0, was_end = 0;
    os64_page_direction_t was_direction = OS64_PAGE_DIRECTION_NONE;
    (void)os64_page_node_selection(dom->state, self, &was_start, &was_end, &was_direction);
    switch (mode) {
    case M_SELECT: sel_start = start; sel_end = new_end; break;
    case M_START: sel_start = sel_end = start; break;
    case M_END: sel_start = sel_end = new_end; break;
    default: {
        /* "preserve": a selection past the replaced text moves with it, and
         * one inside it snaps to the new text's ends. */
        int64_t delta = (int64_t)(new_end - start) - (int64_t)(end - start);
        if (sel_start > end) sel_start = (uint32_t)((int64_t)sel_start + delta);
        else if (sel_start > start) sel_start = start;
        if (sel_end > end) sel_end = (uint32_t)((int64_t)sel_end + delta);
        else if (sel_end > start) sel_end = new_end;
        break;
    }
    }
    int64_t status = os64_page_node_set_selection(dom->state, self, sel_start, sel_end, OS64_PAGE_DIRECTION_NONE);
    if (status == -OS64_PAGE_REASON_NO_MEMORY) return no_room(ctx);
    if (status < 0) return no_selection(ctx);
    return select_queue(dom, ctx, this_value, self, was_start, was_end, was_direction) < 0 ? JS_EXCEPTION
                                                                                           : JS_UNDEFINED;
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
            d_method(dom, ctx, prototype, "select", control_member, 0, X_SELECT) < 0 ||
            d_method(dom, ctx, prototype, "setRangeText", range_text, 1, 0) < 0)
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
