#include "internal.h"

/* THE ELEMENT KINDS WHOSE MEMBERS ARE REFLECTIONS (0f's P6a;
 * INTERFACES_FORMS.md § P6).
 *
 * HTML gives these element kinds interfaces of their own whose members
 * are content attributes, read and set by HTML's reflection rules
 * (controls.c's engine, d_reflection_get and d_reflection_set). kRows is
 * those members, one row per member and interface, the WebIDL shape: a
 * row's one kind is its interface, so its accessor called on an element
 * of any other is a TypeError, as WebIDL's brand check is. Beside them, a
 * title's text (its child text) and a body's window-reflecting handlers,
 * which are window's (event.c's handler property answers window's for the
 * body). The table kinds' tree verbs (rows, cells, insertRow and the
 * rest) and a map's areas are not reflections; INTERFACES_FORMS.md § P6
 * tracks them.
 *
 * One row of the Modern Web table, naming nothing of window's: the
 * interfaces row (classic.c) names the interfaces. */

static const char *const kScopes[] = {"row", "col", "rowgroup", "colgroup", NULL};
static const char *const kReferrerPolicies[] = {
    "", "no-referrer", "no-referrer-when-downgrade", "same-origin", "origin", "strict-origin",
    "origin-when-cross-origin", "strict-origin-when-cross-origin", "unsafe-url", NULL,
};

#define STRING(interface, property, attribute) \
    {property, attribute, K_STRING, D_KIND(interface), NULL, NULL, NULL, 0}
/* WebIDL's LegacyNullToEmptyString: the body's colors, the table's. */
#define NULL_EMPTY(interface, property, attribute) \
    {property, attribute, K_STRING_NULL_EMPTY, D_KIND(interface), NULL, NULL, NULL, 0}
#define BOOLEAN(interface, property, attribute) \
    {property, attribute, K_BOOLEAN, D_KIND(interface), NULL, NULL, NULL, 0}
#define NUMBER(interface, property, attribute, kind, number) \
    {property, attribute, kind, D_KIND(interface), NULL, NULL, NULL, number}

static const DReflection kRows[] = {
    STRING(D_PROTO_META, "name", "name"), STRING(D_PROTO_META, "httpEquiv", "http-equiv"),
    STRING(D_PROTO_META, "content", "content"), STRING(D_PROTO_META, "media", "media"),
    STRING(D_PROTO_META, "scheme", "scheme"),
    STRING(D_PROTO_STYLE, "media", "media"), STRING(D_PROTO_STYLE, "type", "type"),
    NUMBER(D_PROTO_CANVAS, "width", "width", K_UNSIGNED, 300),
    NUMBER(D_PROTO_CANVAS, "height", "height", K_UNSIGNED, 150),
    STRING(D_PROTO_TIME, "dateTime", "datetime"), STRING(D_PROTO_DATA, "value", "value"),
    {"cite", "cite", K_URL, D_KIND(D_PROTO_MOD), NULL, NULL, NULL, 0}, STRING(D_PROTO_MOD, "dateTime", "datetime"),
    {"cite", "cite", K_URL, D_KIND(D_PROTO_QUOTE), NULL, NULL, NULL, 0},
    BOOLEAN(D_PROTO_OLIST, "reversed", "reversed"), NUMBER(D_PROTO_OLIST, "start", "start", K_LONG, 1),
    STRING(D_PROTO_OLIST, "type", "type"), BOOLEAN(D_PROTO_OLIST, "compact", "compact"),
    BOOLEAN(D_PROTO_ULIST, "compact", "compact"), STRING(D_PROTO_ULIST, "type", "type"),
    NUMBER(D_PROTO_LI, "value", "value", K_LONG, 0), STRING(D_PROTO_LI, "type", "type"),
    BOOLEAN(D_PROTO_DLIST, "compact", "compact"), BOOLEAN(D_PROTO_DIRECTORY, "compact", "compact"),
    BOOLEAN(D_PROTO_MENU, "compact", "compact"),
    {"href", "href", K_BASE_URL, D_KIND(D_PROTO_BASE), NULL, NULL, NULL, 0}, STRING(D_PROTO_BASE, "target", "target"),
    STRING(D_PROTO_MAP, "name", "name"),
    {"href", "href", K_URL, D_KIND(D_PROTO_AREA), NULL, NULL, NULL, 0},
    STRING(D_PROTO_AREA, "alt", "alt"), STRING(D_PROTO_AREA, "coords", "coords"),
    STRING(D_PROTO_AREA, "shape", "shape"), STRING(D_PROTO_AREA, "target", "target"),
    STRING(D_PROTO_AREA, "download", "download"), STRING(D_PROTO_AREA, "ping", "ping"),
    STRING(D_PROTO_AREA, "rel", "rel"), STRING(D_PROTO_AREA, "hreflang", "hreflang"),
    STRING(D_PROTO_AREA, "type", "type"),
    {"referrerPolicy", "referrerpolicy", K_ENUM, D_KIND(D_PROTO_AREA), kReferrerPolicies, "", "", 0},
    BOOLEAN(D_PROTO_AREA, "noHref", "nohref"),
    NULL_EMPTY(D_PROTO_BODY, "text", "text"), NULL_EMPTY(D_PROTO_BODY, "link", "link"),
    NULL_EMPTY(D_PROTO_BODY, "vLink", "vlink"), NULL_EMPTY(D_PROTO_BODY, "aLink", "alink"),
    NULL_EMPTY(D_PROTO_BODY, "bgColor", "bgcolor"), STRING(D_PROTO_BODY, "background", "background"),
    STRING(D_PROTO_TABLE, "align", "align"), STRING(D_PROTO_TABLE, "border", "border"),
    STRING(D_PROTO_TABLE, "frame", "frame"), STRING(D_PROTO_TABLE, "rules", "rules"),
    STRING(D_PROTO_TABLE, "summary", "summary"), STRING(D_PROTO_TABLE, "width", "width"),
    NULL_EMPTY(D_PROTO_TABLE, "bgColor", "bgcolor"), NULL_EMPTY(D_PROTO_TABLE, "cellPadding", "cellpadding"),
    NULL_EMPTY(D_PROTO_TABLE, "cellSpacing", "cellspacing"),
    STRING(D_PROTO_TABLE_CAPTION, "align", "align"),
    NUMBER(D_PROTO_TABLE_COL, "span", "span", K_SPAN, 1), STRING(D_PROTO_TABLE_COL, "align", "align"),
    STRING(D_PROTO_TABLE_COL, "ch", "char"), STRING(D_PROTO_TABLE_COL, "chOff", "charoff"),
    STRING(D_PROTO_TABLE_COL, "vAlign", "valign"), STRING(D_PROTO_TABLE_COL, "width", "width"),
    STRING(D_PROTO_TABLE_SECTION, "align", "align"), STRING(D_PROTO_TABLE_SECTION, "ch", "char"),
    STRING(D_PROTO_TABLE_SECTION, "chOff", "charoff"), STRING(D_PROTO_TABLE_SECTION, "vAlign", "valign"),
    STRING(D_PROTO_TABLE_ROW, "align", "align"), STRING(D_PROTO_TABLE_ROW, "ch", "char"),
    STRING(D_PROTO_TABLE_ROW, "chOff", "charoff"), STRING(D_PROTO_TABLE_ROW, "vAlign", "valign"),
    NULL_EMPTY(D_PROTO_TABLE_ROW, "bgColor", "bgcolor"),
    NUMBER(D_PROTO_TABLE_CELL, "colSpan", "colspan", K_SPAN, 1),
    NUMBER(D_PROTO_TABLE_CELL, "rowSpan", "rowspan", K_ROW_SPAN, 1),
    STRING(D_PROTO_TABLE_CELL, "headers", "headers"),
    {"scope", "scope", K_ENUM, D_KIND(D_PROTO_TABLE_CELL), kScopes, "", "", 0},
    STRING(D_PROTO_TABLE_CELL, "abbr", "abbr"), STRING(D_PROTO_TABLE_CELL, "align", "align"),
    STRING(D_PROTO_TABLE_CELL, "axis", "axis"), STRING(D_PROTO_TABLE_CELL, "height", "height"),
    STRING(D_PROTO_TABLE_CELL, "width", "width"), STRING(D_PROTO_TABLE_CELL, "ch", "char"),
    STRING(D_PROTO_TABLE_CELL, "chOff", "charoff"), BOOLEAN(D_PROTO_TABLE_CELL, "noWrap", "nowrap"),
    STRING(D_PROTO_TABLE_CELL, "vAlign", "valign"), NULL_EMPTY(D_PROTO_TABLE_CELL, "bgColor", "bgcolor"),
    /* Obsolete, and still read. */
    STRING(D_PROTO_DIV, "align", "align"), STRING(D_PROTO_PARAGRAPH, "align", "align"),
    STRING(D_PROTO_HEADING, "align", "align"),
    STRING(D_PROTO_HR, "align", "align"), STRING(D_PROTO_HR, "color", "color"),
    BOOLEAN(D_PROTO_HR, "noShade", "noshade"), STRING(D_PROTO_HR, "size", "size"),
    STRING(D_PROTO_HR, "width", "width"),
    NUMBER(D_PROTO_PRE, "width", "width", K_LONG, 0), STRING(D_PROTO_BR, "clear", "clear"),
    NULL_EMPTY(D_PROTO_FONT, "color", "color"), STRING(D_PROTO_FONT, "face", "face"),
    STRING(D_PROTO_FONT, "size", "size"), STRING(D_PROTO_HTML, "version", "version"),
};

/* The members that are no plain reflection, numbered after the rows. */
enum { E_TITLE_TEXT = 256 };
_Static_assert(sizeof(kRows) / sizeof(kRows[0]) < E_TITLE_TEXT, "the rows are numbered below E_TITLE_TEXT");

static JSValue element_member(JSContext *ctx, JSValueConst this_value, int argc, JSValueConst *argv, int magic,
                              JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *node = d_node(dom, ctx, this_value);
    if (node == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    JSValueConst value = argc > 0 ? argv[0] : JS_UNDEFINED;
    uint64_t own = magic == E_TITLE_TEXT ? D_KIND(D_PROTO_TITLE) : kRows[magic].on;
    if (D_KIND(d_node_kind(node)) != own) return JS_ThrowTypeError(ctx, "Illegal invocation");
    if (magic != E_TITLE_TEXT)
        return set ? d_reflection_set(dom, ctx, node, &kRows[magic], value)
                   : d_reflection_get(dom, ctx, node, &kRows[magic]);
    if (!set) return d_child_text(dom, ctx, node);
    /* String replace all: textContent's setter, with the value a string
     * first (null is "null" here). */
    JSValue text = JS_ToString(ctx, value);
    if (JS_IsException(text)) return text;
    JSValue result = d_content_set(dom, ctx, node, D_TEXT_CONTENT, text);
    JS_FreeValue(ctx, text);
    return result;
}

int d_elements_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    (void)global;
    for (int kind = 0; kind < D_PROTO_COUNT; kind++) {
        JSValueConst prototype = dom->prototypes[kind]->value;
        for (size_t i = 0; i < sizeof(kRows) / sizeof(kRows[0]); i++)
            if (kRows[i].on == D_KIND(kind) &&
                d_accessor(dom, ctx, prototype, kRows[i].property, element_member, (int)i, true) < 0)
                return -1;
    }
    if (d_accessor(dom, ctx, dom->prototypes[D_PROTO_TITLE]->value, "text", element_member, E_TITLE_TEXT, true) <
        0)
        return -1;
    /* WindowEventHandlers, which a body has as window has them: the
     * handlers of window's events that GlobalEventHandlers (every
     * element's) does not name. */
    static const int kWindowHandlers[] = {
        D_EVENT_POPSTATE, D_EVENT_HASHCHANGE, D_EVENT_ONLINE, D_EVENT_OFFLINE, D_EVENT_LANGUAGECHANGE,
        D_EVENT_MESSAGE, D_EVENT_MESSAGEERROR, D_EVENT_UNHANDLEDREJECTION, D_EVENT_REJECTIONHANDLED,
    };
    JSValueConst body = dom->prototypes[D_PROTO_BODY]->value;
    for (size_t i = 0; i < sizeof(kWindowHandlers) / sizeof(kWindowHandlers[0]); i++)
        if (d_event_handler(dom, ctx, body, kWindowHandlers[i]) < 0) return -1;
    return 0;
}
