#ifndef DOM_INTERNAL_H
#define DOM_INTERNAL_H

#include "dom/dom.h"
#include "os64/js_engine.h"
#include "way/origin.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "quickjs.h"

#define D_BUCKETS 256
#define D_SET 0x1000
/* The intrinsics the structured clone holds (clone.c's H_COUNT). */
#define D_CLONE_HELD 34
#include <canvas/canvas.h>
#include <dom/canvas.h>

/* Embedded consumers of libdom's document listener. Registration happens
 * during binding construction; callbacks borrow records and never enter JS. */
typedef struct DMutationConsumer {
    struct DMutationConsumer *next;
    void (*changed)(os64_dom_t *, const os64_html_change_t *);
} DMutationConsumer;
void d_mutation_register(os64_dom_t *, DMutationConsumer *,
                         void (*)(os64_dom_t *, const os64_html_change_t *));
void d_mutation_start(os64_dom_t *);
void d_mutation_stop(os64_dom_t *);
int64_t d_non_negative(const char *);
JSValue d_computed_property(os64_dom_t *,JSContext *,const os64_html_node_t *,int32_t,const char *);
JSValue d_computed_color(os64_dom_t *, JSContext *, const os64_html_node_t *);
int d_canvas_install(os64_dom_t *, JSContext *, JSValueConst);

typedef struct DValue DValue;
void d_canvas_free(DValue *);
void d_canvas_sync(DValue *);
canvas_status_t d_canvas_prepare(DValue *);
os64_geometry_status_t d_canvas_allocate(void *,size_t,void **);
void d_canvas_release(void *,void *,size_t);
void d_image_changed(os64_dom_t *,const os64_html_change_t *);
int d_canvas_images_install(os64_dom_t *,JSContext *,JSValueConst);
JSValue d_canvas_result(JSContext *,canvas_status_t);
int d_canvas_text_install(os64_dom_t *,JSContext *,JSValueConst,JSValueConst);
int d_canvas_paths_install(os64_dom_t *,JSContext *,JSValueConst);
int d_double_sequence(os64_dom_t *,JSContext *,JSValueConst,size_t,JSValue **,size_t *);
typedef struct DQuery DQuery;
typedef struct DListener DListener;
typedef struct DTimer DTimer;
/* A link in a chain of nodes on the C stack: the elements being clicked
 * (window.c's click) and the forms being submitted (window.c's submit,
 * validity.c's os64_dom_form_validate, and a host's submit event at a form
 * in os64_dom_dispatch). */
struct DClicking {
    const os64_html_node_t *node;
    const struct DClicking *next;
};
typedef struct DScript DScript;
typedef struct DRequest DRequest;
typedef struct DClone DClone;
typedef struct DMessage DMessage;
typedef struct DIdle DIdle;
typedef struct DAnimation DAnimation;
typedef struct DActiveEvent { struct DActiveEvent *previous; JSValue value; } DActiveEvent;
/* The wrappers' prototypes, one per kind of node the binding tells apart
 * (node.c's d_node_install builds the chain). An HTML element is an
 * HTML_ELEMENT, or one of the HTML kinds when it has one of its own; an
 * element in the SVG namespace is an SVG; an element of another namespace
 * is an ELEMENT. A text node is a TEXT and a comment a COMMENT, each under
 * CHARACTER_DATA, which no node is itself. ATTR is Attr's, which is a Node
 * and no tree node: no Node member is installed on it, and it shadows
 * every one with its own answer (attr.c). A kind comes after the kind it
 * inherits, which is the order the prototypes are made in. */
enum {
    D_PROTO_NODE, D_PROTO_DOCUMENT, D_PROTO_ELEMENT, D_PROTO_CHARACTER_DATA,
    D_PROTO_FRAGMENT, D_PROTO_DOCTYPE, D_PROTO_ATTR, D_PROTO_HTML_ELEMENT, D_PROTO_INPUT, D_PROTO_TEXTAREA, D_PROTO_SELECT,
    D_PROTO_BUTTON, D_PROTO_FORM, D_PROTO_IMAGE, D_PROTO_SCRIPT, D_PROTO_ANCHOR,
    D_PROTO_LINK, D_PROTO_IFRAME, D_PROTO_OPTION, D_PROTO_OPTGROUP,
    D_PROTO_TEXT, D_PROTO_COMMENT, D_PROTO_SVG, D_PROTO_TEMPLATE,
    D_PROTO_LABEL, D_PROTO_FIELDSET, D_PROTO_LEGEND, D_PROTO_OUTPUT, D_PROTO_STYLE,
    /* The element kinds whose members are reflections (0f's P6). */
    D_PROTO_META, D_PROTO_CANVAS, D_PROTO_UNKNOWN, D_PROTO_TIME, D_PROTO_DIV, D_PROTO_BODY, D_PROTO_OLIST,
    D_PROTO_HEAD, D_PROTO_DATA, D_PROTO_LI, D_PROTO_AREA, D_PROTO_BASE, D_PROTO_MAP, D_PROTO_MOD, D_PROTO_QUOTE,
    D_PROTO_TABLE_CELL, D_PROTO_TABLE_COL, D_PROTO_TABLE, D_PROTO_TABLE_ROW, D_PROTO_TABLE_SECTION, D_PROTO_ULIST,
    D_PROTO_BR, D_PROTO_DLIST, D_PROTO_DIRECTORY, D_PROTO_FONT, D_PROTO_HR, D_PROTO_HEADING, D_PROTO_HTML,
    D_PROTO_MENU, D_PROTO_PARAGRAPH, D_PROTO_PRE, D_PROTO_SPAN, D_PROTO_TABLE_CAPTION, D_PROTO_TITLE,
    D_PROTO_COUNT
};
/* A set of kinds is a uint64_t, bit `kind` for each (D_KIND): the tables
 * that put a member on some kinds' prototypes. More kinds than its bits
 * need another representation. */
_Static_assert(D_PROTO_COUNT <= 64, "a set of prototype kinds is one uint64_t");
#define D_KIND(kind) ((uint64_t)1 << (kind))
/* The kind of `node`'s wrapper: its prototype's (node.c). */
int d_node_kind(const os64_html_node_t *node);
/* A content attribute reflected as an IDL attribute (HTML § 2.6.1), by
 * one of HTML's rules: a string (null as "null", or as "" for WebIDL's
 * LegacyNullToEmptyString); a URL, read resolved, and base's href, read
 * against the document's own URL; a form's action; a boolean; an
 * enumerated one; and the integers, each with its own default and its own
 * answer to a value out of range — a non-negative long, a long, the
 * unsigned kinds, and a span's and a row span's clamps. A row is a member
 * on the kinds `on` names; d_reflection_get and d_reflection_set
 * (controls.c) read every rule. */
enum { K_STRING, K_STRING_NULL_EMPTY, K_URL, K_BASE_URL, K_FORM_ACTION, K_BOOLEAN, K_ENUM, K_LIMITED_LONG,
       K_LONG, K_UNSIGNED, K_LIMITED_UNSIGNED, K_UNSIGNED_FALLBACK, K_SPAN, K_ROW_SPAN };
typedef struct {
    const char *property, *attribute;
    uint8_t kind;
    uint64_t on;
    /* An enumerated attribute's keywords, and what it reads as missing and
     * as a value that is none of them. */
    const char *const *keywords;
    const char *missing, *invalid;
    /* An integer's default (a long's as an int32_t). */
    uint32_t number;
} DReflection;
JSValue d_reflection_get(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node, const DReflection *row);
/* HTML § 2.3.4.1, the rules for parsing integers: ASCII whitespace, a
 * sign, digits. False when there is no integer or it is past a long's
 * range (mixin.c). */
bool d_parse_integer(const char *text, int32_t *out);
/* Each setter converts its value before it reads anything of the tree. */
JSValue d_reflection_set(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node, const DReflection *row,
                         JSValueConst value);
/* The kind a kind's prototype inherits, as its interface inherits: Node's
 * for the node kinds, CharacterData's for TEXT and COMMENT, Element's for
 * HTMLElement and SVG, HTMLElement's for the HTML kinds; -1 for Node, which
 * inherits Object's. */
int d_proto_parent(int kind);
/* The event types the binding names, which are the ones on<type> names and
 * os64_dom_listens count: the ones the host dispatches; the three the
 * binding fires at window for a script's failures (report.c);
 * readystatechange, which the binding fires itself (document.c); message
 * and messageerror, which a posted message's task fires at window
 * (message.c); invalid, which constraint validation fires at a control
 * (validity.c); and online, offline, languagechange, visibilitychange and
 * prerenderingchange, whose handlers exist though nothing here ever fires
 * them (browsing.c and document.c say why). A script may listen for any
 * other type. The handler
 * properties of the ones from SCROLL on are the Modern Web arc's, installed
 * by its table's rows (d_event_handler), not with the rest.
 * READYSTATECHANGE, VISIBILITYCHANGE and PRERENDERINGCHANGE are the
 * document's alone: HTML gives them an IDL property on Document and no
 * content attribute, so no element's on<type> attribute is a handler for
 * them. */
enum {
    D_EVENT_CLICK, D_EVENT_MOUSEDOWN, D_EVENT_MOUSEUP, D_EVENT_MOUSEOVER,
    D_EVENT_MOUSEOUT, D_EVENT_MOUSEMOVE, D_EVENT_KEYDOWN, D_EVENT_KEYPRESS,
    D_EVENT_KEYUP, D_EVENT_INPUT, D_EVENT_CHANGE, D_EVENT_SUBMIT, D_EVENT_RESET,
    D_EVENT_FOCUS, D_EVENT_BLUR, D_EVENT_LOAD, D_EVENT_SCROLL, D_EVENT_RESIZE,
    D_EVENT_POPSTATE, D_EVENT_HASHCHANGE, D_EVENT_ONLINE, D_EVENT_OFFLINE,
    D_EVENT_LANGUAGECHANGE, D_EVENT_ERROR, D_EVENT_UNHANDLEDREJECTION,
    D_EVENT_REJECTIONHANDLED, D_EVENT_READYSTATECHANGE, D_EVENT_VISIBILITYCHANGE,
    D_EVENT_PRERENDERINGCHANGE, D_EVENT_DOM_CONTENT_LOADED, D_EVENT_MESSAGE, D_EVENT_MESSAGEERROR,
    D_EVENT_INVALID, D_EVENT_COUNT
};
/* The UI event interfaces (event.c, 0f's P8): UI Events' UIEvent and the
 * five that inherit it, then HTML's SubmitEvent, which inherits Event. An
 * event's record says which one it is (D_UI_NONE: none of them), and each
 * has a prototype on the binding with the Modern Web switch on. */
enum {
    D_UI_NONE, D_UI_EVENT, D_UI_MOUSE, D_UI_KEYBOARD, D_UI_FOCUS, D_UI_INPUT, D_UI_COMPOSITION,
    D_UI_SUBMIT, D_UI_KINDS
};
/* A wrapper, or another engine value the binding holds strongly. A node's
 * wrapper also carries the node's listener list, in the order listeners and
 * handler slots were first added, and the element's views (its style, its
 * classList, a link's relList, its dataset, its attributes), each made
 * once. A view's own entry holds the element it stands for as `target`; so
 * does an Attr's while it is attached (attr.c). */
struct DValue {
    JSValue value;
    DValue *next, *hash_next;
    const os64_html_node_t *node, *target;
    os64_dom_t *dom;
    DValue *style, *class_list, *rel_list, *for_list, *dataset, *attributes;
    /* An Attr's own entry: its record (attr.c). */
    struct DAttr *attr;
    /* Canvas bitmap and stable context identity belong to the element entry. */
    canvas_context_t *canvas;
    DValue *canvas_object;
    canvas_gradient_t *canvas_gradient;
    bool canvas_reset_pending;
    bool image_reported;
    DQuery *child_nodes, *children, *images, *forms, *elements;
    /* A select's options and selectedOptions, each made once ([SameObject]). */
    DQuery *options, *selected_options;
    /* A labelable element's labels, made once ([SameObject]). */
    DQuery *labels;
    /* A table's rows (`members`) and tBodies, a section's rows, a row's
     * cells and a map's areas, each made once ([SameObject], tables.c). */
    DQuery *members, *bodies;
    /* A document's styleSheets, made once ([SameObject]), and a `style` or
     * `link`'s CSSStyleSheet, made once (sheets.c). */
    DQuery *style_sheets;
    DValue *sheet;
    /* A control's, fieldset's or output's ValidityState, made once
     * ([SameObject], validity.c). */
    DValue *validity;
    /* A form's RadioNodeLists, one a name, each made once
     * (d_radio_list), as browsers answer `form.r === form.r`. */
    DQuery *radio_lists;
    /* An output's default value override (HTML § 4.10.12): its text when
     * `output_default_set`, d_alloc'd; else it has none. */
    char *output_default;
    size_t output_default_len;
    bool output_default_set;
    DListener *listeners, *last_listener;
    /* The document the node was made in, copied from or last taken out of,
     * when that is not the window's (implementation.c); NULL is the
     * window's. Read only while the node is the root of a tree no document
     * holds (d_node_document). */
    const os64_html_node_t *document;
    /* A document's DOMImplementation, made when first asked for, and what
     * kind of document it is. */
    DValue *implementation;
    uint8_t document_type;
    /* A DOMTokenList's own entry: the attribute it is over (D_TOKENS_*). */
    uint8_t tokens;
    /* A plain target's entry IN THE OBJECT (d_event_target_init): this
     * record is the first member of the object's own state, outside the
     * registry; `value` is the object itself, not a reference; its
     * listeners' callbacks are traced by the object's gc_mark and freed
     * with it. `listeners_changed`, when the class sets it, is called after
     * a listener is added or removed or a handler set on it (an
     * AbortSignal's GC clause, abort.c); nothing else reads it. */
    bool engine_held;
    void (*listeners_changed)(JSRuntime *rt, DValue *entry);
};
/* DValue.document_type: the window's document and createHTMLDocument's are
 * HTML documents; createDocument makes an XML document, whose createElement
 * makes HTML elements when it was made for the HTML namespace
 * (application/xhtml+xml) and elements in no namespace otherwise. An SVG
 * one is an XML document made for the SVG namespace, which differs from
 * the rest only in its content type (image/svg+xml). */
enum { D_DOCUMENT_HTML = 0, D_DOCUMENT_XHTML, D_DOCUMENT_XML, D_DOCUMENT_SVG };
struct DQuery {
    os64_dom_t *dom;
    const os64_html_node_t *root, *owner;
    DQuery *next;
    DValue *entry;
    char *name, *folded, *named;
    unsigned kind;
    const os64_html_node_t **nodes;
    size_t count;
    uint64_t version;
    uint64_t state_version;     /* SELECTED_OPTIONS: the control state it was read at */
    bool descendants, elements;
    /* A NodeList (as_node_list): its entries are reached by index alone,
     * with no named properties, whatever its kind. */
    bool node_list;
    /* The next of its form's RadioNodeLists (DValue.radio_lists). */
    DQuery *radio_next;
};
/* One addEventListener registration, or a target's on<type> handler slot.
 * The callback is a C-held engine value, drained with the registry. A
 * removed record keeps its place until no dispatch can be walking it. */
struct DListener {
    DListener *next;
    JSValue callback;
    char *type;
    /* Handler slots only: the content attribute's text when last looked at
     * (NULL: absent), so a changed or removed attribute is noticed. */
    char *seen;
    int kind;
    uint8_t handler;
    bool capture, once, removed;
    /* On a collectable plain target: its callback is the target's to trace
     * and free, not one of the registry's values. */
    bool engine;
    /* The script the callback is (d_callback_script); a handler compiled
     * from its attribute is the page's. */
    uint32_t script;
};
/* DListener.handler: zero for a listener, else the state of a handler slot.
 * An ERROR slot failed to compile its attribute and stays absent until the
 * attribute changes. */
enum { D_LISTENER = 0, D_SLOT_EMPTY, D_SLOT_FUNCTION, D_SLOT_UNCOMPILED, D_SLOT_ERROR };
struct DTimer {
    DTimer *next;
    uint64_t due, delay;
    uint32_t id, nesting;
    /* The callback's script (d_callback_script); a string's is the page's. */
    uint32_t script;
    bool interval;
    /* One the binding set for itself (d_timer_add), which clearTimeout and
     * clearInterval cannot reach: its id is drawn from the page's count,
     * so a page could name it. */
    bool binding;
    JSValue callback;
    char *source;
    size_t source_length;
    int argc;
    JSValue *argv;
};
struct DScript {
    DScript *next;
    const os64_html_node_t *node;
};
typedef struct {
    os64_dom_navigation_kind_t kind;
    const os64_html_node_t *node, *submitter;
    int32_t delta;
    char url[OS64_DOM_URL_MAX];
} DNavigation;
struct os64_dom {
    os64_js_runtime_t *runtime;
    JSRuntime *engine;
    os64_html_document_t *document;
    os64_page_state_t *state;
    os64_dom_options_t options;
    size_t bytes, retained;
    bool closed;
    DMutationConsumer *mutation_consumers, canvas_mutations;
    bool mutation_installed;
    size_t canvas_bytes;
    os64_dom_image_provider_t image_provider;
    JSClassID canvas_class, gradient_class, metrics_class;
    os64_dom_text_provider_t canvas_text;
    os64_dom_geometry_provider_t geometry;
    void *geometry_opaque;
    os64_dom_geometry_stats_t geometry_stats;
    JSClassID anchor_class, node_class, collection_class, event_class, style_class, token_class, map_class;
    /* Attr and NamedNodeMap (attr.c), and the map's prototype. The
     * attached Attrs, found by element and name (a table of chains that
     * doubles as it fills), and the last element walked as an array of
     * its attributes, good only while its list is as it was. */
    JSClassID attr_class, named_map_class;
    DValue *named_map_prototype;
    /* HTMLOptionsCollection's prototype (options.c), over a collection's;
     * NULL with the modern switch off. */
    DValue *options_prototype;
    /* HTMLFormControlsCollection's prototype, over a collection's, and
     * RadioNodeList's, over a NodeList's (collection.c, 0f's P3b); NULL
     * with the modern switch off. */
    DValue *controls_prototype, *radio_prototype;
    /* A form's names are being listed for its ordinary keys alone
     * (classic.c's node_names asking the engine). */
    bool listing_names;
    /* StyleSheetList's prototype (collection.c) and CSSStyleSheet's
     * (sheets.c); NULL with the modern switch off. */
    DValue *sheet_list_prototype, *sheet_prototype;
    /* Constraint validation (validity.c): ValidityState's prototype; the
     * RegExp constructor and RegExp.prototype.exec, taken before any page
     * ran, which judge a control's pattern; the native function a person's
     * submission's validation is called through (os64_dom_form_validate);
     * and the control last reported to the person, with its message, which
     * the host reads after the task (os64_dom_report_message). NULL with
     * the modern switch off. */
    DValue *validity_prototype, *regexp, *regexp_exec, *validate;
    const os64_html_node_t *reported;
    char reported_message[256];
    /* Date.prototype.getTime, taken before any page ran: valueAsDate's
     * setter reads a Date's time value with it, and anything that is no
     * Date throws its TypeError (controls.c). NULL with the modern switch
     * off. */
    DValue *date_get_time;
    /* The window's Selection (selection.c), the one getSelection answers;
     * NULL with the modern switch off. */
    DValue *selection;
    struct DAttr **attr_table;
    size_t attr_capacity, attr_attached;
    struct DAttrCursor {
        const os64_html_node_t *element;
        const os64_html_attr_t *head;
        const os64_html_attr_t **list;
        size_t capacity;
        uint64_t version;
        uint32_t count, found;
    } attr_cursor;
    /* Set while the map's own ordinary property is asked of the engine,
     * which the map's named properties then do not answer (attr.c). */
    bool attr_ordinary;
    DValue *values, *anchor, *index_guard, *window, *location, *history, *function_ctor;
    DValue *invoke, *navigator, *style_prototype, *static_list;
    const char *(*user_agent)(void *opaque);
    void *user_agent_opaque;
    os64_dom_cookies_t cookies;
    DValue *prototypes[D_PROTO_COUNT];
    DValue *buckets[D_BUCKETS];
    DQuery *queries;
    /* Listener and timer callbacks are the registry's other two lists. */
    size_t listeners_held, timers_held;
    uint32_t listen_counts[D_EVENT_COUNT];
    uint64_t attribute_mask;
    uint64_t attribute_version;
    bool attribute_scanned;
    unsigned dispatching;
    DActiveEvent *current_event;
    /* The classes of plain event targets (d_event_target), and each one's
     * resolver, NULL for a class whose opaque is its entry. */
    JSClassID target_classes[12];
    int (*target_resolvers[12])(os64_dom_t *, JSContext *, JSValueConst, bool, DValue **);
    unsigned target_class_count;
    /* EventTarget.prototype, which holds the listener methods every target
     * inherits, and Window.prototype, between the global and it (event.c,
     * d_event_install): levels of the chain with the Modern Web surface on
     * or off; its row names them. */
    DValue *event_target_prototype, *window_prototype;
    bool sweep_pending;
    DListener *invoking;
    /* The import map os64_dom_import_map is parsing, while it does
     * (importmap.c). */
    struct DImportParse *import_parse;
    DValue *invoking_entry;
    DTimer *timers;
    size_t timer_count;
    uint32_t timer_id, timer_nesting;
    /* HTML's click-in-progress flags, one link per element.click() on the
     * C stack (window.c); and a form's firing-submission-events flags, one
     * link per form whose validation or submit event is running. */
    const struct DClicking *clicking, *submitting;
    DScript *scripts[64];
    char *url;
    DNavigation navigation;
    os64_js_outcome_t report;
    /* THE VIEW (view.c): the host's provider, and the view as the last
     * os64_dom_view_step saw it, what its events are owed against. */
    os64_dom_view_provider_t view;
    os64_dom_view_t view_seen;
    /* The view the MediaQueryLists were last judged against. */
    os64_dom_view_t media_judged;
    /* getComputedStyle's provider, and the snapshot it writes into, made
     * at the first read and reused. */
    os64_dom_computed_provider_t computed;
    void *computed_opaque;
    struct os64_dom_computed *snapshot;
    JSClassID computed_class, media_class;
    DValue *computed_prototype, *media_prototype, *media_event_prototype;
    /* The page's MediaQueryLists, in the order they were made (view.c's
     * DMedia): a list is the binding's while its object lives, and while a
     * listener holds it. */
    struct DMedia *media, *media_last;
    /* HISTORY (window.c): the host's provider; the current entry's state as
     * JSON, NULL for null; its value, parsed at the first read of
     * history.state and the same object every read after (`entry_state_read`);
     * the entry's name (os64_dom_history_entry), a new one at every change
     * of the entry or its state; and whether the page asked to keep its own
     * scroll. */
    os64_dom_history_t history_host;
    char *entry_state;
    size_t entry_state_length;
    DValue *entry_state_value;
    uint64_t entry_name;
    bool entry_state_read, scroll_manual;
    DValue *popstate_prototype, *hashchange_prototype;
    /* JS_GetClassID of a plain object and of an array: the only objects
     * a state's JSON carries. */
    JSClassID plain_class, array_class;
    /* DataView's constructor and its buffer, byteOffset and byteLength
     * getters as the engine made them, for reading a buffer source
     * (encoding.c). */
    DValue *data_view[4];
    /* Web storage (storage.c): an area per os64_dom_storage_area_t, made
     * at its first touch, and the provider each is kept by. */
    struct DStorageArea *storage[OS64_DOM_STORAGE_AREAS];
    os64_dom_storage_t storage_providers[OS64_DOM_STORAGE_AREAS];
    /* performance (performance.c): its object (a plain event target), the
     * marks and measures it buffers (an engine object with no prototype,
     * never handed to a page, in the order made, and how many it holds),
     * its timing and navigation objects, the navigation entry, and the
     * entries' prototypes. The time origin is read once, as the binding is
     * made. */
    DValue *performance, *performance_entries, *performance_timing, *performance_navigation,
        *navigation_entry;
    uint32_t performance_count;
    DValue *entry_prototypes[5];
    uint64_t time_origin_us;
    int64_t time_origin_epoch_us;
    /* screen and screen.orientation (screen.c), each made once. */
    DValue *screen, *orientation;
    /* window.status's string as a script last set it, a classic member
     * (window.c); P1-W's six BarProps, `external` and `crypto`, each made
     * once (browsing.c, crypto.c). */
    DValue *status, *bars[6], *external, *crypto;
    /* The engine's typed-array `buffer` getter and ArrayBuffer's
     * `resizable` getter, taken before any page ran (crypto.c). */
    DValue *crypto_getters[2];
    /* Symbol.iterator as the engine made it, for reading an iterable a
     * page hands over (url.c). */
    DValue *iterator_symbol;
    /* DOMImplementation (implementation.c): its prototype, and how many
     * documents of their own it has made, none of which has a window. */
    DValue *implementation_prototype;
    size_t documents;
    /* A script's requests (slice 2): DOMException's prototype, the host's
     * network, the requests out with the next token, and how many engine
     * values they hold (the registry's fourth list). */
    DValue *exception_prototype, *file_prototype, *form_prototype, *progress_prototype;
    os64_dom_network_t network;
    DRequest *requests;
    uint32_t request_token;
    size_t requests_held;
    /* The last AbortSignal's number (abort.c's DSignal.serial). */
    uint64_t signal_serial;
    /* A SCRIPT'S FAILURES (report.c, MODERN_WEB.md § P2a): ErrorEvent's
     * and PromiseRejectionEvent's prototypes; the function a failure's
     * event is fired through; the intrinsic Error constructor, taken before
     * any page ran, which says where a running script is (an Error it makes
     * has the caller's place in its stack); the exceptions the wrapper caught, waiting
     * for their error event, and the reported promises a handler reached,
     * waiting for their rejectionhandled, each oldest first, with how many
     * values the two hold (the registry's fifth list); the promise whose
     * unhandledrejection is being fired and whether a handler reached it
     * meanwhile; and whether an error event is being fired, when a second
     * error is reported to no page (HTML's error reporting mode). */
    DValue *error_event_prototype, *rejection_event_prototype, *report_fire, *error_constructor;
    struct DPending *errors, *errors_last, *handled, *handled_last;
    size_t pending_held;
    const void *rejection_firing;
    bool rejection_handled, reporting_error;
    /* The rejections past the task's first failure told to the host one by
     * one so far, and those only counted (report.c's D_REJECTIONS_TOLD). */
    uint32_t rejections_told;
    uint64_t rejections_untold;
    /* THE SCRIPTS' NAMES (report.c, MUTED ERRORS): every script the host
     * runs is compiled under a name the binding gives it, `os64-script:<n>`,
     * and `n` is its place here: its address (NULL: the page's own) and
     * whether it is another site's. 0 is the page's, 1 another site's
     * whose record could not be made. The script being run
     * (os64_dom_script_name to os64_dom_script_ended); the one whose
     * callback the binding is calling (a listener's, a timer's); the last
     * such of the task; and the one whose queueMicrotask callback threw,
     * until the exception is heard: D_NO_SCRIPT for none (report.c's
     * current_script). Function.prototype's own fileName getter, and
     * WeakSet with WeakSet.prototype's add and delete, taken before any
     * page ran; the switch's row installs them, and with it off they are
     * NULL and nothing is muted, as no event fires. The set made with them
     * at the first muted rejection, which holds the muted promises taken
     * so a handler added to one later is no event, and whether one could
     * not be kept, after which no rejectionhandled fires (report.c's
     * remember_muted). */
    struct DScriptName *script_names;
    uint32_t script_name_count, script_name_cap;
    uint32_t running_script, calling_script, task_script, thrown_script;
    DValue *filename_getter, *weak_set[3], *muted_promises;
    bool muted_forgotten;
    /* P1-Da, the document's state (document.c): its readiness, moved by
     * the host's DOMContentLoaded and load dispatches; the referrer the
     * host gave, the binding's own copy (NULL for none); the script
     * element the host says is running, held; and CustomEvent's
     * prototype (event.c). */
    os64_dom_readiness_t readiness;
    char *referrer;
    const os64_html_node_t *current_script;
    DValue *custom_prototype;
    /* P2b, clone, message, idle. The intrinsics the structured clone calls,
     * taken before any page ran (clone.c's d_clone_setup), and whether this
     * binding has learned the engine's classes it tells values by (at its
     * first clone, clone.c's learn_classes). The window's
     * posted messages, oldest first, and how many values they hold (the
     * registry's sixth list), and MessageEvent's prototype (message.c,
     * event.c). The idle callbacks (idle.c): every entry in request order,
     * holes included (`idle_count` of `idle_cap`), the next to run
     * (`idle_head`), where the runnable ones end and the ones waiting for
     * the next period begin (`idle_split`), how many hold a callback (the
     * registry's seventh list) and how many of those are runnable; the last
     * handle; the open period, when it began on both clocks, whether a
     * request was made outside a period's callback since it began, and when
     * the next may begin for one made inside; and whether a period's
     * callback is running. */
    DValue *clone_held[D_CLONE_HELD];
    bool clone_classes;
    DMessage *messages, *messages_last;
    size_t messages_held;
    DValue *message_prototype;
    DAnimation *animations, *animation_last;
    size_t animation_count;
    uint32_t animation_handle;
    uint64_t animation_due;
    bool animation_running;
    DIdle *idle;
    size_t idle_count, idle_cap, idle_head, idle_split, idle_live, idle_runnable;
    uint32_t idle_handle;
    bool idle_open, idle_fresh, idle_running;
    uint64_t idle_start_ms, idle_start_us, idle_rested_ms;
    uint64_t idle_render_due; /* host rendering deadline, UINT64_MAX when unbounded */
    /* 0f's P8: each UI event interface's prototype (event.c), indexed by
     * D_UI_*; all NULL with the switch off. */
    DValue *ui_prototypes[D_UI_KINDS];
    /* Custom elements (custom.c): the registry, its definitions, the
     * reactions waiting and their queues; NULL with the switch off. */
    struct DCustomState *custom;
};
#define D_NO_SCRIPT UINT32_MAX
/* d_callback_script's answer when the runtime must stop: the exception is
 * pending, uncatchable, and the caller fails before it records anything. */
#define D_SCRIPT_FAILED (UINT32_MAX - 1)
#define D_SCRIPT_PAGE 0u
struct DScriptName {
    char *address;
    bool muted;
};
typedef struct { char *data; size_t length; } DString;

/* The caller passes its own compiled ABI identity, including callback units.
 * Native state is carried by private function data, never opaque host slots. */
os64_dom_t *d_callback(JSContext *ctx, JSValueConst *data, const char *abi);
bool d_context(os64_dom_t *dom, JSContext *ctx, const char *abi);
void *d_alloc(os64_dom_t *dom, size_t size);
void d_free(os64_dom_t *dom, void *ptr);
JSValue d_error(JSContext *ctx, const char *name, const char *message);
/* A dictionary argument (WebIDL § 3.2.17): argv[at] undefined, null or
 * missing is an empty one, any other value that is not an object a
 * TypeError. 1 an object, 0 empty, -1 with the exception pending. */
int d_dictionary(JSContext *ctx, int argc, JSValueConst *argv, int at);
JSValue d_html_error(JSContext *ctx, int64_t status);
JSValue d_page_error(JSContext *ctx, int64_t status);
DValue *d_retain(os64_dom_t *dom, JSContext *ctx, JSValue value);
/* The function `name` of `from`, taken at install, before any page ran,
 * and kept for the binding's life (the registry owns the reference
 * d_retain is given): the engine's own, whatever a page later puts there.
 * NULL, with a TypeError pending, when it is no function. */
DValue *d_intrinsic(os64_dom_t *dom, JSContext *ctx, JSValueConst from, const char *name);
/* Whether `value` is the very object `held` holds (false for NULL): the
 * brand check of a member of an object the binding makes one of. */
bool d_same_object(JSValueConst value, const DValue *held);
int d_method(os64_dom_t *dom, JSContext *ctx, JSValueConst target,
             const char *name, JSCFunctionData *fn, int argc, int magic);
int d_accessor(os64_dom_t *dom, JSContext *ctx, JSValueConst target,
               const char *name, JSCFunctionData *fn, int magic, bool writable);
/* A getter-only accessor `name` on `target`, with `flags`, answering
 * `held` (which its function holds) while the binding is open: the
 * cheapest member there is, for a read a page makes on nearly every line
 * (core.c). */
int d_held_getter(os64_dom_t *dom, JSContext *ctx, JSValueConst target, const char *name, JSValueConst held,
                  int flags);
/* d_method whose function's `name` is its own, as WebIDL names an
 * operation; the Modern Web's rows make theirs so. */
int d_method_named(os64_dom_t *dom, JSContext *ctx, JSValueConst target,
                   const char *name, JSCFunctionData *fn, int argc, int magic);
JSValue d_wrap(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node);
/* d_wrap for an interface's constructor: a node with no wrapper yet gets
 * one whose prototype is `prototype` when that is an object (new.target's,
 * WebIDL's "get prototype from constructor"), else its kind's; identity,
 * holds and the registry are d_wrap's. */
JSValue d_wrap_constructed(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node,
                           JSValueConst prototype);
/* The tree node a value wraps, or NULL with a TypeError thrown: one that
 * names an Attr when the value is one, which is a Node with no place in the
 * tree. */
const os64_html_node_t *d_node(os64_dom_t *dom, JSContext *ctx, JSValueConst value);
DValue *d_find(os64_dom_t *dom, const os64_html_node_t *node);
int d_geometry_install(os64_dom_t *dom, JSContext *ctx);
int d_node_install(os64_dom_t *dom, JSContext *ctx);
int d_event_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_timer_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_window_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
void d_event_drain(os64_dom_t *dom);
void d_event_free(os64_dom_t *dom, DValue *entry);
void d_timer_drain(os64_dom_t *dom);
void d_timer_free(os64_dom_t *dom);
/* A timer the binding sets for itself (AbortSignal.timeout, an
 * XMLHttpRequest's timeout, an idle callback's timeout): `callback` (taken)
 * is called with no arguments once, `delay` ms from now on the host's
 * clock, as one of the page's timers, though clearTimeout and
 * clearInterval cannot cancel it; only d_timer_cancel can. Its id, or 0
 * with an exception pending. Cancelling an id that has fired, or 0, does
 * nothing. */
uint32_t d_timer_add(os64_dom_t *dom, JSContext *ctx, JSValue callback, uint64_t delay);
void d_timer_cancel(os64_dom_t *dom, uint32_t id);
void d_window_free(os64_dom_t *dom);
int d_event_kind(const char *type);
/* A handler slot noticed in a content attribute must keep its place among
 * the listeners a script added before the attribute was set. */
int d_handler_attribute_set(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node,
                            const char *name);
/* Script-mode dispatch from inside a script: listeners run as nested calls,
 * their exceptions are reported and the next one runs. Answers 1 when the
 * default was not prevented, 0 when it was, -1 with an exception pending
 * when the binding refused its own storage or the runtime failed under it. */
int d_dispatch(os64_dom_t *dom, JSContext *ctx, JSValueConst event, const os64_html_node_t *node);
/* A PLAIN EVENT TARGET: an object that is neither a node nor window and
 * is an EventTarget all the same (performance, screen.orientation, a
 * MediaQueryList, an AbortSignal, an XMLHttpRequest and its upload, a
 * `new EventTarget()`), whose entry holds its listeners. A row registers
 * the object's class with d_event_target, which makes its prototype
 * inherit EventTarget.prototype, and so the three methods, when it does not
 * already. The row gives the prototype its on<type> properties with
 * d_event_handler (one of the binding's event types) or
 * d_event_handler_named (any type). Where the entry lives is the class's
 * choice, and everything else (the methods, the listener records, the
 * dispatch) is the same for all three:
 *   - the object's opaque, `resolve` NULL, a registry entry the object
 *     keeps for the page's life (performance, screen.orientation);
 *   - made on demand by a resolver, for a class whose objects come and go
 *     (a MediaQueryList): it makes the entry only when `make` (a listener
 *     added, a handler set, a dispatch), and answers a NULL entry otherwise
 *     when there is none, which a read, a removal or a null handler acts on
 *     as nothing; the class releases an entry once nothing listens on it
 *     (view.c's media_sweep);
 *   - IN THE OBJECT, `resolve` NULL, for a class a page makes one of per
 *     request (an AbortSignal, an XMLHttpRequest, a `new EventTarget()`),
 *     which would otherwise keep every one alive on the registry: the
 *     object's state BEGINS with
 *     its entry (so the opaque finds it), made with d_event_target_init;
 *     the entry is on no registry, its listeners' callbacks are the
 *     object's to trace (d_event_target_mark from its gc_mark) and free
 *     (d_event_target_finalize from its finalizer), and it goes with the
 *     garbage whatever listens on it.
 * A resolver answers 1 with the entry, 0 when `self` is not its kind, -1
 * with an exception pending. d_dispatch_plain is d_dispatch at one: the
 * path is the target alone; d_dispatch_plain_task is the same from inside
 * a host task, each listener a call and a checkpoint as the host's own
 * dispatch runs them, the outcome folded into `outcome`. */
int d_event_target(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype, JSClassID class_id,
                   int (*resolve)(os64_dom_t *dom, JSContext *ctx, JSValueConst self, bool make,
                                  DValue **entry));
int d_event_handler(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype, int kind);
int d_dispatch_plain(os64_dom_t *dom, JSContext *ctx, JSValueConst event, DValue *target);
JSValue d_event_new(os64_dom_t *dom, JSContext *ctx, const char *type, bool bubbles,
                    bool cancelable, bool trusted);
/* d_event_new with the event's prototype `prototype` when it is an object
 * (an Event interface's own, PopStateEvent's and the rest), else Event's. */
JSValue d_event_new_proto(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype, const char *type,
                          bool bubbles, bool cancelable, bool trusted);
/* `new Event(type, {bubbles, cancelable})`'s steps, the event's prototype
 * `prototype` (d_event_new_proto): what an Event interface's constructor
 * does before it reads the members of its own. */
JSValue d_event_construct(os64_dom_t *dom, JSContext *ctx, int argc, JSValueConst *argv,
                          JSValueConst prototype);
/* One event the host dispatches as one task, as os64_dom_dispatch does: at
 * `node`, at window (both NULL), or at `plain`, a plain event target's
 * entry. `make` builds the Event inside the task (its own prototype and
 * fields); the caller has already asked whether anything listens. */
os64_js_status_t d_host_dispatch(os64_dom_t *dom, const os64_html_node_t *node, DValue *plain,
                                 const char *type, JSValue (*make)(os64_dom_t *, JSContext *, void *),
                                 void *arg, bool *prevented, os64_js_outcome_t *outcome);
/* MediaQueryList's legacy addListener and removeListener: the `change`
 * listener `callback` added to or removed from `self`. */
JSValue d_listener_change(os64_dom_t *dom, JSContext *ctx, JSValueConst self, JSValueConst callback,
                          bool add);
/* A plain target's entry in the object, d_event_handler_named and
 * d_dispatch_plain_task: A PLAIN EVENT TARGET, above. */
void d_event_target_init(os64_dom_t *dom, DValue *entry, JSValueConst object);
void d_event_target_mark(JSRuntime *rt, const DValue *entry, JS_MarkFunc *mark);
void d_event_target_finalize(JSRuntime *rt, DValue *entry);
int d_event_handler_named(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype, const char *type);
int d_dispatch_plain_task(os64_dom_t *dom, JSContext *ctx, JSValueConst event, DValue *target,
                          os64_js_outcome_t *outcome);
/* 0f's P1b row (event.c): EventTarget, its constructor, and Window. */
int d_targets_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* A host task's bookkeeping (event.c): an entry's outcome folded into the
 * task's (the first failure kept, a sticky one over one that is not), and
 * the task ended with the binding's own report folded in. */
void d_outcome_fold(os64_js_outcome_t *into, const os64_js_outcome_t *from);
os64_js_status_t d_task_finish(os64_dom_t *dom, os64_js_outcome_t *outcome);
void d_report(os64_dom_t *dom, JSContext *ctx, JSValueConst error);
int d_script_mark(os64_dom_t *dom, const os64_html_node_t *node);
int d_script_mark_tree(os64_dom_t *dom, const os64_html_node_t *root);
int d_script_copy_marks(os64_dom_t *dom, const os64_html_node_t *from, const os64_html_node_t *to);
void d_script_connected(os64_dom_t *dom, const os64_html_node_t *const *scripts, size_t count);
size_t d_script_collect(const os64_html_node_t *node,
                        const os64_html_node_t **out, size_t cap);
/* TWO KINDS OF CONNECTED. d_connected is DOM's: the node's tree hangs from
 * a document, any document, a document of its own (implementation.c)
 * included; isConnected and a `form` attribute's lookup ask it.
 * d_in_window_tree is narrower: the tree hangs from the window's document,
 * which is what a script must be in to run. */
bool d_connected(const os64_html_node_t *node);
bool d_in_window_tree(const os64_dom_t *dom, const os64_html_node_t *node);
const os64_html_node_t *d_form_owner(const os64_html_node_t *node);
JSValue d_element_action(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node, int action);
void d_navigate(os64_dom_t *dom, os64_dom_navigation_kind_t kind, const os64_html_node_t *node,
                const os64_html_node_t *submitter, const char *url, int32_t delta);
int d_collection_install(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype);
void d_query_free(os64_dom_t *dom, DQuery *query);
int d_static_list_install(os64_dom_t *dom, JSContext *ctx);
int d_mixin_install(os64_dom_t *dom, JSContext *ctx);
/* A URL-valued reflection `name` on `prototype`, as script.src is: read
 * resolved against the document's base, set as written (mixin.c). The
 * attribute is `src` on a script or an iframe, else `href`. */
int d_url_reflection(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype, const char *name);
extern JSClassExoticMethods d_token_exotic, d_map_exotic;
/* A live list's index keys: 1 and *index for a canonical array index, 0
 * for any other key, -1 on an exception. */
int d_index_key(JSContext *ctx, JSAtom atom, uint32_t *index);
int d_index_names(JSContext *ctx, size_t count, JSPropertyEnum **table, uint32_t *length);
/* WebIDL's iteration from Array.prototype (and a NodeList's forEach,
 * entries, keys and values with `node_list`). */
int d_iterable(JSContext *ctx, JSValueConst prototype, bool node_list);
int d_tree_change(os64_dom_t *dom, JSContext *ctx, int op, const os64_html_node_t *parent,
            const os64_html_node_t *child, const os64_html_node_t *other);
JSValue d_select(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *root,
                 JSValueConst text, bool all);
JSValue d_selector_test(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *element,
                        JSValueConst text, bool closest);
JSValue d_attribute_collection(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *root,
                               JSValueConst text, bool by_name);
/* Whether a whitespace-separated list holds the token (ASCII case folded
 * when `fold`). */
bool d_has_token(const char *list, const char *token, size_t length, bool fold);
extern JSClassExoticMethods d_collection_exotic;
JSValue d_collection(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *root,
                     bool descendants, bool elements, const char *name);
JSValue d_children(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *root,
                   bool elements);
bool d_string(os64_dom_t *dom, JSContext *ctx, JSValueConst value, DString *out);
void d_string_free(os64_dom_t *dom, DString *string);
void d_fold(char *text);
bool d_element_name_valid(const char *text);
bool d_attribute_name_valid(const char *text);
int64_t d_insert(os64_dom_t *dom, os64_html_node_t *parent, os64_html_node_t *node,
                  os64_html_node_t *before);
int64_t d_remove(os64_dom_t *dom, os64_html_node_t *node);
/* The node after `node` in tree order among `root`'s descendants, NULL
 * past the last (node.c). */
const os64_html_node_t *d_walk_next(const os64_html_node_t *root, const os64_html_node_t *node);
int64_t d_replace(os64_dom_t *dom, os64_html_node_t *parent, os64_html_node_t *node,
                   os64_html_node_t *old);
int64_t d_replace_content(os64_dom_t *dom, os64_html_node_t *parent,
                           os64_html_node_t *replacement);
JSValue d_content_get(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node,
                      int property);
JSValue d_content_set(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node,
                      int property, JSValueConst value);

enum {
    D_PARENT, D_FIRST, D_LAST, D_PREV, D_NEXT, D_OWNER, D_CHILD_NODES, D_CHILDREN,
    D_FIRST_ELEMENT, D_LAST_ELEMENT, D_PREV_ELEMENT, D_NEXT_ELEMENT,
    D_CHILD_ELEMENT_COUNT, D_PARENT_ELEMENT, D_TYPE, D_NAME, D_TAG, D_DOCUMENT_ELEMENT, D_HEAD, D_BODY,
    D_TEXT_CONTENT, D_NODE_VALUE, D_DATA, D_INNER_HTML, D_OUTER_HTML,
    D_ID, D_CLASS, D_VALUE, D_CHECKED, D_SELECTED_INDEX, D_CONTROL_TYPE
};
enum {
    D_APPEND, D_INSERT, D_REMOVE, D_REPLACE, D_CLONE, D_GET_ID, D_GET_TAG,
    D_QUERY_ONE, D_QUERY_ALL,
    D_CREATE_ELEMENT, D_CREATE_TEXT, D_CREATE_COMMENT, D_CREATE_FRAGMENT,
    D_GET_ATTR, D_SET_ATTR, D_REMOVE_ATTR, D_HAS_ATTR, D_HAS_CHILDREN,
    /* The mixins (mixin.c): ParentNode, ChildNode, and the rest of Element. */
    D_APPEND_NODES, D_PREPEND, D_REPLACE_CHILDREN, D_BEFORE, D_AFTER, D_REPLACE_WITH, D_REMOVE_SELF,
    D_CONTAINS, D_MATCHES, D_CLOSEST, D_BY_CLASS, D_BY_NAME,
    D_ADJACENT_HTML, D_ADJACENT_ELEMENT, D_ADJACENT_TEXT,
    D_ATTRIBUTE_NAMES, D_TOGGLE_ATTR, D_HAS_ATTRS,
    /* The node row's (mixin.c d_nodes_install, behind the modern switch). */
    D_ROOT_NODE, D_NORMALIZE
};
/* OPTIONS and SELECTED_OPTIONS are a select's list of options (libpage's
 * walk, os64_page_option_next) and the ones of it that are selected; the
 * second also changes with control state, not only with the tree. LABELS
 * are the labels whose labeled control is the query's owner, walked from
 * the root of the owner's tree, that root included (d_labels). FIELDSET is
 * a fieldset's listed elements (d_listed) among its descendants. SHEETS is
 * a document's style sheets, by libpage's rule (os64_page_names_sheet),
 * whose entries are each node's CSSStyleSheet (d_sheet), not the node.
 * The table kinds (0f's P6b, d_table_query): TBODIES a table's tbody
 * children; TABLE_ROWS a table's rows, its tr children and its sections'
 * (thead, tbody, tfoot children of it), the thead's first and the tfoot's
 * last, each group in tree order (HTML § 4.9.1); SECTION_ROWS a section's
 * tr children; CELLS a row's td and th children; AREAS a map's area
 * descendants. */
enum { D_QUERY_GENERIC, D_QUERY_IMAGES, D_QUERY_FORMS, D_QUERY_CONTROLS, D_QUERY_DOCUMENT_NAMES,
       D_QUERY_CLASSES, D_QUERY_NAME, D_QUERY_OPTIONS, D_QUERY_SELECTED_OPTIONS, D_QUERY_LABELS,
       D_QUERY_FIELDSET, D_QUERY_SHEETS, D_QUERY_TBODIES, D_QUERY_TABLE_ROWS, D_QUERY_SECTION_ROWS,
       D_QUERY_CELLS, D_QUERY_AREAS };
/* The live HTMLCollection of a table kind (above) that `owner` has,
 * made once ([SameObject]) and kept on its entry; NULL with an exception
 * pending. Its object is the query's entry's value. */
DQuery *d_table_query(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *owner, unsigned kind);
/* A query's nodes as they are now, refreshed: borrowed until the query's
 * next refresh. False with an exception pending. */
bool d_query_items(DQuery *query, JSContext *ctx, const os64_html_node_t *const **nodes, size_t *count);
bool d_named_match(const os64_html_node_t *node, const char *name);
bool d_classic_match(const os64_html_node_t *root, const os64_html_node_t *node, unsigned kind);
/* One of HTML § 4.10.2's listed elements, which a fieldset's `elements`
 * lists, and a form's all but an image button: button, fieldset, input,
 * object, output, select, textarea. */
bool d_listed(const os64_html_node_t *node);
/* A form's controls as its `elements` lists them, where it looks for them
 * (classic.c): the `index`th, NULL past the last; *count, when not NULL,
 * how many there are. */
const os64_html_node_t *d_form_control(const os64_html_node_t *form, uint32_t index, uint32_t *count);
JSValue d_special_collection(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *root, unsigned kind, const char *named);
JSValue d_classic_collection(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node, unsigned kind);
int d_classic_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* NEVER SWALLOW A RUNTIME'S FAILURE. Whether the exception pending may be
 * taken (JS_GetException) and replaced, discarded or handed to a page: false
 * once the runtime has failed or its budget says stop (an interrupt at its
 * deadline, a cancel, a memory limit). The engine made that exception
 * uncatchable, and taking it makes it an ordinary value a page can catch,
 * so a page that loops on a binding call that converts it would never be
 * stopped; leave it pending and fail the call instead. Asked BEFORE taking
 * the exception, whenever what threw could have entered the engine's
 * interpreter (any JS_Call, a conversion that may run a page's code). */
bool d_exception_catchable(JSContext *ctx);
/* The binding of the context a function made without data runs in (its
 * anchor is the anchor class's prototype there); NULL, with the error
 * d_context throws, when it is closed or unavailable. */
os64_dom_t *d_binding(JSContext *ctx);
/* The Modern Web arc's surface (modern.c): the table's rows installed when
 * options.modern is set, and each name a row declares, by index across the
 * table, NULL past the last, for the harness that holds the rows to it. */
int d_modern_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
const char *d_modern_name(size_t index);
/* The table's rows' installers, each in the file its slice owns. */
int d_interfaces_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_view_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_media_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_computed_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_history_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* Location, History and Navigator as interfaces, over the objects the
 * classic install made (window.c, 0f's P7). */
int d_window_objects_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* The view's records freed, after the engine (os64_dom_free). */
void d_view_free(os64_dom_t *dom);
int d_storage_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_performance_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_url_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_encoding_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_screen_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* The screen's width and height as `screen` reports them, in CSS pixels at
 * the page's zoom, and devicePixelRatio: what the device-* media features
 * and `resolution` answer (view.c). */
void d_screen_media(const os64_dom_t *dom, double *width, double *height, double *ratio);
int d_base64_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_implementation_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* P1-W, window's low-hanging fruit (WINDOW_DOCUMENT.md § P1-W): the
 * window's members as a browsing context (browsing.c), its BarProps and
 * `external` (the same file), and `crypto` (crypto.c). */
int d_browsing_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* top, parent and length, every page's (browsing.c), asked of the window's
 * browsing context as P1-W's members are; and whether that context is a
 * secure context, the answer isSecureContext and every [SecureContext]
 * member read. */
int d_browsing_classic(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
bool d_secure_context(const os64_dom_t *context);
int d_bars_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_external_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_crypto_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_image_data_install(os64_dom_t *, JSContext *, JSValueConst);
int d_image_data_settings(JSContext *, JSValueConst);
/* Clone reads internal fields, not page-replaceable getters. The returned
 * array is borrowed; the constructor below takes its own reference. */
bool d_image_data_parts(JSValueConst, JSValue *array, uint32_t *width, uint32_t *height);
JSValue d_image_data_from_array(JSContext *, JSValueConst, uint32_t width, uint32_t height);
JSValue d_image_data_new(JSContext *, uint32_t width, uint32_t height);
/* Borrowed bytes expire on the next call into page code. */
int d_image_data_view(os64_dom_t *, JSContext *, JSValueConst, canvas_rgba8_t *);
/* The frames row (frames.c; IFRAME.md § The cluster and the window proxy):
 * the HTMLIFrameElement interface over D_PROTO_IFRAME, and its members. */
int d_frames_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* P2a, a script's failures (report.c; WINDOW_DOCUMENT.md § P2a). The row:
 * window's onerror, onunhandledrejection, onrejectionhandled and
 * reportError, document's onerror, ErrorEvent and PromiseRejectionEvent.
 * The row is the switch's; being the runtime's script host is every
 * page's (d_report_host, from os64_dom_create), so the record can say a
 * rejection went unhandled with the switch off too, though no event is
 * fired then. d_report_drain lets go of the host and the values waiting;
 * d_report_free, after the engine, the scripts' names. */
int d_report_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
bool d_report_host(os64_dom_t *dom);
void d_report_drain(os64_dom_t *dom);
void d_report_free(os64_dom_t *dom);
/* An exception the binding caught itself (d_report's): its error event is
 * fired now when a script is running, which is HTML's timing, or else left
 * for the task's next turn between calls. `script` is the script whose
 * failure it is, which decides whether it is muted. Nothing with the switch off, while an
 * error event is being fired, or after a sticky failure. */
void d_report_error(os64_dom_t *dom, JSContext *ctx, JSValueConst error, uint32_t script);
/* d_report for `script`'s failure (event.c): a nested dispatch's listener's,
 * or reportError's, whose script is the one that called it. d_report
 * itself is for failures that are the page's: a handler attribute that did
 * not compile, the binding's own refusal. */
void d_report_as(os64_dom_t *dom, JSContext *ctx, JSValueConst error, uint32_t script);
/* The script a callback handed to the binding belongs to, asked as it is
 * handed over: the name it was compiled under when that is one of the
 * binding's (read through Function.prototype's own fileName getter), else
 * the running script's, the one handing it over (a bound function, one
 * made by eval). D_SCRIPT_FAILED, with the exception pending, when the
 * getter's call found the runtime must stop: asked before the callback is
 * recorded, so the caller can fail with nothing done. */
uint32_t d_callback_script(os64_dom_t *dom, JSContext *ctx, JSValueConst function);
/* Around every call the binding makes into a callback of the page's: the
 * muting state (report.c's current_script reads it) as it stood, kept by
 * the caller, and the callback's script made the one being called, which
 * an exception the call ends in is muted by. Leaving puts all of it back,
 * so a script that dispatched and goes on is again the one whose failures
 * they are, at any depth; `top`, a task's own call with no script under
 * it, keeps the task's last-called script, which the checkpoint after the
 * call reads for a rejection no frame names. */
typedef struct { uint32_t running, calling, task, thrown; } DScripts;
DScripts d_script_enter(os64_dom_t *dom, uint32_t script);
void d_script_leave(os64_dom_t *dom, const DScripts *before, bool top);
/* Inside an open task, between its calls: the error events the exceptions
 * the wrapper caught are owed, fired at window, each named for its script. */
void d_errors_fire(os64_dom_t *dom, os64_js_outcome_t *outcome);
/* Inside an open task, before it ends: checkpoints and what each leaves
 * owed, the error events, an unhandledrejection for each rejection the
 * runtime hands over and a rejectionhandled for each reported promise a
 * handler reached, until a checkpoint leaves nothing. A rejection no
 * handler cancelled is the task's failure. */
void d_task_settle(os64_dom_t *dom, os64_js_outcome_t *outcome);
/* As a task named `task` closes: the binding's own report (d_report's first
 * error) is the outcome when that is OK, else a line told to the host, and
 * is cleared either way; and the rejections it only counted, past the ones
 * told one by one, are told as one line. */
void d_task_close(os64_dom_t *dom, const char *task, os64_js_outcome_t *outcome);
/* After a script the host ran with os64_js_run (and a string timer the
 * binding did): what it left owed, settled in a task of its own named
 * `name`, folded into `outcome`. No engine entry when nothing is owed. */
void d_script_settle(os64_dom_t *dom, const char *name, os64_js_outcome_t *outcome);
/* ErrorEvent and PromiseRejectionEvent (event.c): the interfaces, and one
 * of each made for a dispatch, trusted. */
int d_error_events_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
JSValue d_error_event_new(os64_dom_t *dom, JSContext *ctx, const char *message, const char *filename,
                          uint32_t lineno, uint32_t colno, JSValueConst error);
JSValue d_rejection_event_new(os64_dom_t *dom, JSContext *ctx, const char *type, JSValueConst promise,
                              JSValueConst reason, bool cancelable);
/* A thrown value as a line of text, read without running any of the page's
 * code (no toString, no getter): an Error's or a DOMException's name and
 * message, a primitive's own spelling, and for any other object a fixed
 * phrase. */
void d_error_text(os64_dom_t *dom, JSContext *ctx, JSValueConst value, char *out, size_t cap);
/* An Error's own `stack` into `out` (an outcome's stack_trace), read as a
 * data property so no page code runs, "" for anything else; true when it
 * was cut short. */
bool d_error_stack(JSContext *ctx, JSValueConst error, char *out, size_t cap);
/* The DOMException's own name and message (exception.c), borrowed; false
 * for a value that is not one. */
bool d_dom_exception_fields(JSValueConst value, const char **name, const char **message);
/* An interface whose objects the binding makes (WebIDL § 3.7): its class
 * registered in this runtime, its prototype made and held for the page's
 * life, and its interface object, an Illegal constructor, named on
 * window. `*out` borrows the prototype from the entry that holds it. */
int d_interface_class(os64_dom_t *dom, JSContext *ctx, JSValueConst global, JSClassID id,
                      const JSClassDef *definition, JSValue *out);
/* WebIDL's ArrayBufferView (encoding.c): the kind of typed array `value`
 * is (a JSTypedArrayEnum), D_VIEW_DATA_VIEW for a DataView, -1 for
 * anything else, and -2 with an exception pending when the engine's
 * classes could not be learned. Nothing of the value is read. */
#define D_VIEW_DATA_VIEW 12
int d_view_kind(os64_dom_t *dom, JSContext *ctx, JSValueConst value);
/* DOCUMENTS OF THEIR OWN (implementation.c). document.implementation and
 * DOMImplementation's prototype, which every page has (classic), and the
 * DocumentType members. */
int d_implementation_classic(os64_dom_t *dom, JSContext *ctx);
/* A node's node document: a document itself; else the document its tree
 * hangs from; else, for a tree no document holds, the one it was made in
 * or last left (its root's record), the window's when there is none. */
const os64_html_node_t *d_node_document(os64_dom_t *dom, const os64_html_node_t *node);
/* Whether a node's node document is the window's: a node of a document
 * with no window navigates, submits and takes focus nowhere, compiles no
 * handler, and as a document reads no cookies. */
bool d_window_document(os64_dom_t *dom, const os64_html_node_t *node);
/* Whether scripting is enabled for a node (HTML § 8.1.3.2): the page's
 * scripting, for a node of the window's document. What a fragment parse and
 * a serialization read `noscript` by. */
bool d_scripting(os64_dom_t *dom, const os64_html_node_t *node);
/* A verb is about to take `node` out of its tree: its record says which
 * document it leaves, for when it is a root. */
void d_leaving(os64_dom_t *dom, const os64_html_node_t *node);
/* A node made for, or copied from a node of, `document`: its record says
 * so, the window's leaving none. */
void d_made_in(os64_dom_t *dom, const os64_html_node_t *node, const os64_html_node_t *document);
/* The kind of document `node` is (D_DOCUMENT_*); a node that is no
 * document is the window's kind. */
int d_document_type(os64_dom_t *dom, const os64_html_node_t *node);
/* An interface object for a prototype a row made (WebIDL § 3.7): its
 * `prototype` and the prototype's `constructor` and Symbol.toStringTag
 * (`name`). `constructor` makes an instance, given new.target, and refuses
 * a call without `new` (JS_CFUNC_constructor); NULL makes an Illegal
 * constructor. `parent`, when an object, is the parent interface's object,
 * which this one inherits. JS_EXCEPTION when it could not be made. */
JSValue d_interface_new(JSContext *ctx, const char *name, int length, JSCFunction *constructor,
                        JSValueConst prototype, JSValueConst parent);
/* A string's text exactly as the engine holds it, as UTF-8 with a NUL a
 * byte like any other and, unless `scalar`, a lone surrogate as the
 * three bytes the engine writes for it (WTF-8): what JS_NewStringLen
 * reads back as the same string. `scalar` makes each lone surrogate
 * U+FFFD, WebIDL's USVString. Charged to the binding's budget like
 * d_string; false with an exception pending. */
bool d_string_text(os64_dom_t *dom, JSContext *ctx, JSValueConst value, DString *out, bool scalar);
/* The origin an address has (HTML § Origins), serialized by libway
 * (way_origin_serialize): `scheme://host` and `:port` when it is not the
 * scheme's default, for an address whose scheme gives it a tuple origin
 * (http, https, ws, wss, ftp; a blob: address its inner http(s) address's),
 * else "null", an opaque origin. False when it does not fit in cap. */
bool d_origin(const char *url, char *out, size_t cap);
/* The binding's storage (storage.c): what os64_dom_drain commits, and what
 * os64_dom_free frees. */
void d_storage_drain(os64_dom_t *dom);
void d_storage_free(os64_dom_t *dom);
/* The storage maps' steps so far (storage.c's s_steps), for the DOM host. */
uint64_t d_storage_steps(void);
/* URLSearchParams' record conversion's probes so far (url.c), for the DOM
 * host. */
uint64_t d_url_record_probes(void);
/* ── Slice 2: a script's requests (fetch.c, xhr.c, blob.c, abort.c,
 * exception.c). LIBDOM.md § Slice 2's rows is the record. ── */
int d_exception_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_abort_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_blob_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_fetch_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_xhr_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* ProgressEvent (event.c): the interface, and one made for a dispatch. */
int d_progress_event_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
JSValue d_progress_event_new(os64_dom_t *dom, JSContext *ctx, const char *type, bool computable, double loaded,
                             double total, bool trusted);
/* A DOMException named `name`: made (d_dom_exception_new) or thrown
 * (d_dom_exception, which answers JS_EXCEPTION). */
JSValue d_dom_exception_new(os64_dom_t *dom, JSContext *ctx, const char *name, const char *message);
JSValue d_dom_exception(os64_dom_t *dom, JSContext *ctx, const char *name, const char *message);
/* WebIDL's sequence<any>: `value`'s iterator walked to its end, at most
 * `max` elements (one more is a TypeError, and the iterator is closed),
 * each kept in a js_malloc'd array the caller frees with d_sequence_free.
 * The page's iterator runs here; the array is C's and no page sees it.
 * -1 with an exception pending. */
#define D_SEQUENCE_MAX 65536
int d_sequence(os64_dom_t *dom, JSContext *ctx, JSValueConst value, size_t max, JSValue **out, size_t *count);
void d_sequence_free(JSContext *ctx, JSValue *items, size_t count);
/* encoding.c's buffer sources for the other rows: 1 with the bytes a
 * BufferSource holds now (borrowed), 0 for a value that is not one, -1
 * with an exception pending; and a new Uint8Array holding a copy. */
int d_buffer_source(os64_dom_t *dom, JSContext *ctx, JSValueConst value, const uint8_t **bytes, size_t *length);
JSValue d_uint8_array(JSContext *ctx, const uint8_t *bytes, size_t length);
/* url.c's URLSearchParams, for a request body: its
 * application/x-www-form-urlencoded serialization (js_malloc'd) when
 * `value` is one; NULL and no exception when it is not one; NULL with an
 * exception pending when there was no memory. */
char *d_params_text(JSContext *ctx, JSValueConst value, size_t *length);
/* Whether `value` is a URLSearchParams (XHR's send() treats one as it
 * treats a string when it rewrites a charset). */
bool d_params_is(JSValueConst value);
/* AbortSignal (abort.c). A signal's algorithms are C's: a fetch's abort,
 * by its request's token, and a dependent signal's. */
JSValue d_signal_new(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype);
bool d_signal_is(JSValueConst value);
bool d_signal_aborted(JSValueConst signal, JSValue *reason, JSContext *ctx);
int d_signal_abort(os64_dom_t *dom, JSContext *ctx, JSValueConst signal, JSValueConst reason);
int d_signal_follow(os64_dom_t *dom, JSContext *ctx, JSValueConst dependent, JSValueConst source);
int d_signal_add_fetch(JSContext *ctx, JSValueConst signal, uint32_t token);
/* A fetch answered: its algorithm now aborts the Response whose state is
 * `answered` (NULL: there is no body to error, and the algorithm goes),
 * and the Response keeps this signal's state at *back, which this side
 * clears when it goes; d_signal_fetch_settled drops it when the body
 * settles or the Response goes, from either side's finalizer too. */
void d_signal_fetch_answered(JSContext *ctx, JSValueConst signal, uint32_t token, void *answered, void **back);
void d_signal_fetch_settled(JSRuntime *rt, void *signal, void **back);
void d_signal_remove_fetch(JSContext *ctx, JSValueConst signal, uint32_t token);
/* How many dependents a signal's weak set links (the DOM host's bound). */
size_t d_signal_dependents(JSValueConst signal);
/* The steps a signal's links took (abort.c): each source followed, each
 * link made or found a duplicate, each link a hold update walked. Not
 * synchronized: a count for
 * the DOM host to hold the work a page can size to its bound, as
 * d_request_steps is. */
extern uint64_t d_signal_steps;
/* fetch.c: a fetch's abort, the algorithm its signal runs. `answered` is
 * the Response's state once the fetch answered, else NULL. */
void d_fetch_abort(os64_dom_t *dom, JSContext *ctx, uint32_t token, void *answered, JSValueConst reason);
/* Bytes and their type (blob.c, fetch.c): js_malloc'd; `type` NULL or "" for none. */
typedef struct {
    uint8_t *bytes;
    size_t length;
    char *type;
} DBytes;
void d_bytes_free(JSContext *ctx, DBytes *b);
/* A Blob's (or File's) bytes and type, borrowed; false for anything else. */
bool d_blob_get(JSValueConst value, const uint8_t **bytes, size_t *length, const char **type);
/* A new Blob of `bytes` (taken) and `type` (copied). */
JSValue d_blob_new(os64_dom_t *dom, JSContext *ctx, DBytes *bytes);
bool d_form_data_is(JSValueConst value);
/* A FormData as multipart/form-data (HTML § 4.10.21.8), its type naming
 * the boundary. -1 with an exception pending. */
int d_form_data_encode(os64_dom_t *dom, JSContext *ctx, JSValueConst form, DBytes *out);
/* A body read back as FormData (Fetch § 5.3 "formData"): multipart or
 * urlencoded by `type`; a TypeError for anything else. */
JSValue d_form_data_parse(os64_dom_t *dom, JSContext *ctx, const uint8_t *bytes, size_t length, const char *type);
/* Fetch § 5.2 "extract a body": a string, a BufferSource, a Blob, a
 * FormData or a URLSearchParams, as bytes and a type (NULL for a
 * BufferSource's none). 1 extracted, 0 not one of those, -1 with an
 * exception pending. */
int d_body_extract(os64_dom_t *dom, JSContext *ctx, JSValueConst value, DBytes *out, bool *has_type);
/* The requests out (fetch.c, xhr.c): each a token the host's answer names,
 * the host's id for cancel, and what waits on it. */
typedef void (*DAnswer)(os64_dom_t *dom, JSContext *ctx, DRequest *request, const os64_dom_response_t *response,
                        os64_js_outcome_t *outcome);
struct DRequest {
    DRequest *next;
    uint32_t token;
    uint64_t id;
    DAnswer answer;
    JSValue held[3];            /* C-held: the registry's fourth list, drain releases them */
};
DRequest *d_request_start(os64_dom_t *dom, JSContext *ctx, const os64_dom_request_t *request, DAnswer answer);
DRequest *d_request_find(os64_dom_t *dom, uint32_t token);
/* Forgets a request: cancels it with the host when it is still out
 * (`cancel`), releases what it held and frees it. */
void d_request_end(os64_dom_t *dom, DRequest *request, bool cancel);
void d_fetch_drain(os64_dom_t *dom);
void d_fetch_free(os64_dom_t *dom);
/* WebIDL's ByteString (fetch.c): a string whose every code unit is a byte,
 * as those bytes (js_malloc'd, NUL-terminated); false with a TypeError for
 * any other. d_byte_text is the way back (isomorphic decode). */
bool d_byte_string(JSContext *ctx, JSValueConst value, char **out, size_t *length);
JSValue d_byte_text(JSContext *ctx, const char *bytes, size_t length);
bool d_same_nocase(const char *a, size_t alen, const char *b, size_t blen);
/* The steps a header list's work took (fetch.c, xhr.c): each line read
 * from a server's block, each entry placed by a sort's pass, each value
 * joined. Not synchronized: a count for the DOM host to hold the work a
 * page or a server can size to its bound, as d_storage_steps is. */
extern uint64_t d_request_steps;
/* An iterator prototype's `next` (fetch.c): WebIDL's iterator prototype
 * object defines it writable, ENUMERABLE and configurable, which
 * d_method's operations are not. */
int d_iterator_next(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype, JSCFunctionData *fn, int magic);
/* A header block (way/origin.h's shape) as a Headers object with `guard`. */
JSValue d_headers_from_block(os64_dom_t *dom, JSContext *ctx, const char *block, size_t length, int guard);
/* A minimal ReadableStream over a copy of `bytes` (fetch.c): its reader
 * hands over the whole as one Uint8Array, then done. */
JSValue d_stream_new(os64_dom_t *dom, JSContext *ctx, const uint8_t *bytes, size_t length);
/* A promise settled with `value` (taken), or rejected with the pending
 * exception when `value` is JS_EXCEPTION (blob.c). */
JSValue d_promise_settled(JSContext *ctx, JSValue value);
/* The Encoding Standard's decode of a whole body (encoding.c, TextDecoder's
 * decoders): `label` as TextDecoder reads one, UTF-8 when it is NULL or
 * names nothing this browser decodes; a BOM of the encoding taken off, an
 * invalid sequence U+FFFD. Engine memory. d_utf8_decode is UTF-8 decode. */
JSValue d_text_decode(JSContext *ctx, const char *label, const uint8_t *bytes, size_t length);
JSValue d_utf8_decode(JSContext *ctx, const uint8_t *bytes, size_t length);
/* "UTF-8 decode without BOM": a leading U+FEFF is text, not a mark (what
 * the urlencoded parser decodes names and values with). */
JSValue d_utf8_decode_without_bom(JSContext *ctx, const uint8_t *bytes, size_t length);
/* url.c: `input` parsed as a URL against the document's base URL (its
 * <base href> as the tree stands), by the API's parser: a query is UTF-8
 * whatever the document's encoding. 1 with the address (d_alloc'd, the
 * caller frees it), 0 for one that does not parse, -1 with an exception
 * pending. */
int d_url_parse(os64_dom_t *dom, JSContext *ctx, JSValueConst input, char **out);
/* ── P1-Da: the document's state reads (document.c), CustomEvent
 * (event.c). LIBDOM.md § P1-Da's rows is the record. ── */
int d_document_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_custom_event_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* ── The node row (0f's P1a and after; INTERFACES_FORMS.md): Node's
 * getRootNode, baseURI and normalize, Text's own members and Element's
 * names (mixin.c), and Attr's answers to Node's three beside them
 * (attr.c), whose shadow covers Node.prototype whole. ── */
int d_nodes_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_attr_nodes_install(os64_dom_t *dom, JSContext *ctx);
/* Node's `baseURI` for a node of a document (mixin.c): that document's base
 * URL, about:blank for one with no window. */
JSValue d_base_uri(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node);
/* The select's, option's and optgroup's members, HTMLOptionsCollection's
 * prototype and the Option factory (options.c). */
int d_options_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* The text controls' selection members and the window's Selection
 * (selection.c, 0f's P2 core). */
int d_selection_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* `len` bytes of UTF-8 in UTF-16 code units, the unit a script counts a
 * string's length in (selection.c). */
uint32_t d_utf16_length(const char *text, size_t len);
/* The controls' reflections and state: input's, textarea's, select's and
 * button's (controls.c, 0f's P3a). */
int d_controls_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* An element's child text content (DOM § 4.4): its Text children's data,
 * in order (controls.c). */
JSValue d_child_text(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node);
/* The element kinds whose members are reflections, and a title's text and
 * a body's window-reflecting handlers (elements.c, 0f's P6a). */
int d_elements_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* The tables' tree verbs and collections, and a map's areas (tables.c,
 * 0f's P6b). */
int d_tables_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* Node's comparisons, isSameNode, isEqualNode and compareDocumentPosition,
 * on Node's prototype and Attr's (compare.c). */
int d_compare_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* A label's labeled control (HTML § 4.10.4): the labelable element its
 * `for` names, when the first element of its tree with that ID is one;
 * without `for`, its first labelable descendant. NULL for none, or for a
 * node that is not an HTML label. */
const os64_html_node_t *d_label_control(const os64_html_node_t *label);
/* A labelable element's labels (controls.c's `labels`): a live NodeList,
 * made once, of the labels in its tree whose labeled control it is
 * (collection.c). */
JSValue d_labels(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *control);
/* A form's controls named `name` (collection.c): with the modern switch
 * on, its live RadioNodeList of that name, made once; with it off, a new
 * collection of them. */
JSValue d_radio_list(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *form, const char *name);
/* HTMLFormControlsCollection and RadioNodeList (collection.c, 0f's P3b). */
int d_form_collections_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* A document's styleSheets (collection.c): its live StyleSheetList, made
 * once. */
JSValue d_style_sheets(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *document);
/* StyleSheetList's prototype and interface (collection.c). */
int d_sheet_list_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* A `style` or `link`'s CSSStyleSheet (sheets.c), made once, whether or
 * not the node names a sheet now. */
JSValue d_sheet(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node);
/* document.styleSheets, StyleSheet, CSSStyleSheet and LinkStyle's `sheet`
 * (sheets.c): one row of the Modern Web table. */
int d_sheets_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* A form's constraint validation (HTML § 4.10.20.2, validity.c): every
 * invalid control it owns gets an `invalid` event, and when `report` the
 * first whose event nobody cancelled is reported to the person. 1 valid,
 * 0 invalid, -1 with an exception pending. With the modern switch off a
 * form is valid here, and the model's own judge (libpage) is the only one. */
int d_form_validate(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *form, bool report);
/* ValidityState, the controls' constraint validation members and the
 * form's, and oninvalid (validity.c): one row of the Modern Web table. */
int d_validity_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* The form's, label's, fieldset's, legend's and output's own members
 * (forms.c, 0f's P3b). */
int d_forms_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* What a DOMTokenList is over: `class` (a classList), `rel` (a relList),
 * `for` (an output's htmlFor). */
enum { D_TOKENS_CLASS, D_TOKENS_REL, D_TOKENS_FOR };
/* An output's htmlFor (mixin.c): a DOMTokenList over `for`, made once. */
JSValue d_for_list(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node);
/* A form's relList and rel (mixin.c): installed on its prototype as a
 * link's are. */
int d_form_rel_install(os64_dom_t *dom, JSContext *ctx);
/* HTMLScriptElement.supports (importmap.c). */
int d_import_map_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* HTMLOptionsCollection's indexed setter (HTML § 2.6.2.2), for the
 * collection's exotic: null or undefined removes the option at `index`, an
 * option past the end is appended after empty options fill the gap, and
 * one within it replaces the option there. 0, or -1 with an exception. */
int d_options_set_index(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *select, uint32_t index,
                        JSValueConst value);
/* getRootNode's GetRootNodeOptions converted as WebIDL converts a
 * dictionary (a page's `composed` getter runs; anything but an object,
 * undefined or null is a TypeError), for both of its receivers. With no
 * shadow trees, `composed` changes no answer. -1 with an exception. */
int d_root_options(JSContext *ctx, int argc, JSValueConst *argv);
/* The referrer copied out of the options at creation (core.c), and what
 * os64_dom_free lets go: the copy and the held running script. */
int d_document_setup(os64_dom_t *dom);
void d_document_free(os64_dom_t *dom);
/* document.body's setter (HTML § 3.1.4), every page's: `document` is the
 * receiver. */
JSValue d_body_set(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *document, JSValueConst value);
/* The document's readiness moves for a dispatch the host makes (the rule
 * is os64_dom_readiness_after's): moved, with readystatechange fired at
 * the document as one task when the Modern Web switch is on and anything
 * listens, its outcome in `outcome`. False when it did not move. */
bool d_readiness_moved(os64_dom_t *dom, const os64_html_node_t *node, const char *type,
                       os64_js_outcome_t *outcome);
/* ── P2b: clone, message, idle (clone.c, message.c, idle.c, and
 * MessageEvent in event.c). LIBDOM.md § P2b's rows is the record. ── */
/* HTML's StructuredSerializeWithTransfer, in the realm of `ctx` whose
 * binding is `dom`: the record, or NULL with the exception pending (a
 * DataCloneError, or what a getter or a conversion threw). `transfer` is
 * the transfer list, each an object (d_clone_objects), whose bytes the
 * record copies; the caller detaches them with d_clone_detach once the
 * clone has succeeded whole (structuredClone: once the copy is made;
 * postMessage: once the message is queued), so a refusal detaches none. */
DClone *d_clone_write(os64_dom_t *dom, JSContext *ctx, JSValueConst value, const JSValue *transfer,
                      size_t transfer_count);
void d_clone_detach(JSContext *ctx, const JSValue *transfer, size_t transfer_count);
/* StructuredDeserializeWithTransfer into the realm of `ctx`, whose binding
 * is `dom`: the copy, or JS_EXCEPTION with the exception pending. The
 * record's bytes move into the buffers and blobs it makes, so a record is
 * read once. */
JSValue d_clone_read(os64_dom_t *dom, JSContext *ctx, DClone *clone);
void d_clone_free(JSRuntime *rt, DClone *clone);
/* The intrinsics the pair calls, taken from `global` before any page ran:
 * once per binding, by each row that clones. */
int d_clone_setup(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* StructuredSerializeOptions at argv[at]: its transfer list (empty when
 * absent), each an object, as a sequence the caller frees with
 * d_sequence_free. -1 with an exception pending. d_clone_objects is the
 * object check alone, for a list converted otherwise. */
int d_clone_transfer(os64_dom_t *dom, JSContext *ctx, int argc, JSValueConst *argv, int at, JSValue **list,
                     size_t *count);
int d_clone_objects(JSContext *ctx, const JSValue *list, size_t count);
int d_clone_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_message_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
double d_performance_now(const os64_dom_t *dom);
int d_animation_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
void d_animation_drain(os64_dom_t *dom);
int d_idle_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* What os64_dom_drain releases (the messages' values and clones, the idle
 * callbacks), and what os64_dom_free frees (the idle entries). */
void d_message_drain(os64_dom_t *dom);
void d_idle_drain(os64_dom_t *dom);
void d_idle_free(os64_dom_t *dom);
/* MessageEvent (event.c): the interface, and one made for a dispatch:
 * trusted, not bubbling or cancelable, `data` taken, `origin` copied,
 * `source` held, lastEventId "" and its ports an empty frozen array. */
int d_message_event_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
JSValue d_message_event_new(os64_dom_t *dom, JSContext *ctx, const char *type, JSValue data, const char *origin,
                            JSValueConst source);
/* ── 0f's P8: the UI event interfaces (event.c; INTERFACES_FORMS.md § P8,
 * LIBDOM.md § Events). ── */
int d_ui_events_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* The events a script's own action fires (window.c), untrusted, as HTML
 * makes them: element.click()'s, a MouseEvent whose view is the window when
 * `node` is in the window's document; and a submission's SubmitEvent, its
 * submitter `submitter` (NULL: none). A plain Event of the type with the
 * switch off. */
JSValue d_click_event_new(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node);
JSValue d_submit_event_new(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *submitter);
/* A click's activation (window.c, which says what each half does): whether
 * an element has activation behaviour, and the halves before and after the
 * click's listeners, the second told whether the click was cancelled and
 * given what the first kept. Each -1 with an exception pending. */
typedef struct { bool checkable, checkbox, was, was_indeterminate; } DActivation;
bool d_activation_has(const os64_html_node_t *node);
int d_activation_before(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node, DActivation *saved);
int d_activation_after(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node, bool canceled,
                       const DActivation *saved);
/* A File's name and lastModified (borrowed); false for a Blob that is no
 * File, and for anything else. d_file_new is d_blob_new's File: `name`
 * (engine memory) taken with the bytes. */
bool d_blob_file(JSValueConst value, const char **name, int64_t *last_modified);
JSValue d_file_new(os64_dom_t *dom, JSContext *ctx, DBytes *bytes, char *name, int64_t last_modified);
/* The URL parser with no base, as postMessage's targetOrigin is parsed:
 * `text` made ready for it as d_url_parse makes its input (the caller still
 * frees it), then 1 with the address in `out` (OS64_DOM_URL_MAX bytes), 0
 * for one that is no absolute URL, -1 with an exception pending. */
int d_url_parse_absolute(os64_dom_t *dom, JSContext *ctx, DString *text, char *out);
/* `object`'s own accessor `name`'s getter, held in *out; -1 with an
 * exception pending (crypto.c). */
int d_take_getter(os64_dom_t *dom, JSContext *ctx, JSValueConst object, const char *name, DValue **out);
/* An element's dir attribute as its IDL attribute reads it (mixin.c): one
 * of its keywords, lowercased, or "". */
JSValue d_dir_value(JSContext *ctx, const os64_html_node_t *element);
/* A document's element, its first element child; and its head (`body`
 * false) or body, the first such child of that element when it is an HTML
 * `html`, a frameset being a body (node.c, HTML § 3.1). NULL for none. */
const os64_html_node_t *d_document_element(const os64_html_node_t *document);
const os64_html_node_t *d_document_part(const os64_html_node_t *document, bool body);
/* An object's own [Symbol.toStringTag] (classic.c), as an interface's. */
int d_string_tag(JSContext *ctx, JSValueConst object, const char *name);
/* ATTRIBUTES (attr.c). THE ONE DOOR: every attribute the binding sets or
 * removes goes through d_set_attr to libpage's verb, so that a removal
 * detaches the Attr that stood for the attribute, with its last value.
 * libhtml's status; on a refusal the attribute and its Attr are as they
 * were. */
int64_t d_set_attr(os64_dom_t *dom, const os64_html_node_t *node, const char *name,
                   const char *value, size_t length, bool remove);
/* Whether a value is an Attr of this binding. */
bool d_is_attr(os64_dom_t *dom, JSValueConst value);
/* An Attr as Node's comparisons read it: the entry that is its identity,
 * its element (NULL while detached), its qualified name, namespace (NULL:
 * none) and local name, and its value, `length` bytes. Read at the call,
 * so good until page code runs. */
typedef struct {
    const DValue *entry;
    const os64_html_node_t *element;
    const char *name, *ns, *local, *value;
    size_t length;
} DAttrView;
/* 1 with *view filled when `value` is an Attr, 0 when it is not, -1 with
 * the exception thrown. */
int d_attr_view(os64_dom_t *dom, JSContext *ctx, JSValueConst value, DAttrView *view);
/* A name's local part (DOM § 4.9.2): after the first colon of a name in a
 * namespace; the whole name of one in none, colons and all. */
const char *d_attr_local(const char *name, const char *ns);
/* Element.attributes, the Attr members and the attribute-node methods of
 * Document and Element: the classic surface, on every page. */
int d_attr_classic(os64_dom_t *dom, JSContext *ctx);
/* The NamedNodeMap interface object (the interfaces row's). */
JSValue d_attr_map_interface(os64_dom_t *dom, JSContext *ctx);
/* An Attr entry's record freed, after the engine (os64_dom_free), and the
 * table of attached ones with the walk's cursor. */
void d_attr_free(os64_dom_t *dom, DValue *entry);
void d_attr_table_free(os64_dom_t *dom);
/* The attribute walks' steps so far (attr.c's s_steps), for the DOM host. */
uint64_t d_attr_steps(void);
/* DOM's "valid namespace prefix" (implementation.c). */
bool d_prefix_valid(const char *text, size_t length);
JSValue d_style(os64_dom_t *dom, JSContext *ctx, JSValueConst self);
int d_style_install(os64_dom_t *dom, JSContext *ctx);
extern JSClassExoticMethods d_node_exotic;
/* ── Custom elements (custom.c; docs/design/pending/CUSTOM_ELEMENTS.md) ──
 * The row: CustomElementRegistry, window.customElements and the document's
 * listener. HTMLElement's constructor, which the interfaces row makes and
 * which answers Illegal constructor until a definition names its
 * new.target. */
int d_custom_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
JSValue d_html_element_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv);
/* [CEReactions] for a member written in C: push an element queue before
 * it acts, pop it after, which runs what it queued, before it returns and
 * though it threw. Both do nothing with the switch off; -1 with an
 * exception pending. */
int d_reactions_push(os64_dom_t *dom, JSContext *ctx);
int d_reactions_pop(os64_dom_t *dom, JSContext *ctx);
/* createElement's synchronous construction: 0 when `name` has no
 * definition in `document`'s registry, else 1 with *out the element (an
 * HTMLUnknownElement in the failed state when the constructor failed,
 * which is reported) or JS_EXCEPTION. */
int d_custom_create(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *document, const char *name,
                    JSValue *out);
/* A clone's elements with a definition, upgraded before the clone is
 * handed back. 0, or -1 with an exception pending. */
int d_custom_cloned(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *copy);
/* Teardown: the listener taken away with the engine's values; the
 * records and definitions' memory after the engine. */
void d_custom_free(os64_dom_t *dom);
/* THE [CEReactions] TABLE (ce_reactions.inc, written by
 * tools/ce_reactions.py from the standard's IDL): a row's interface, its
 * member, and its kind (METHOD, SETTER, EXOTIC for a special operation no
 * prototype member is, ALL_SETTERS for every setter of the interface's
 * prototype). What install made of each row is its state: WRAPPED,
 * SHARED (one member another row wrapped, reached through a mixin),
 * ABSENT (libdom has no such member), UNWRAPPABLE (an exotic path). For
 * the DOM host, which holds the members it expects to be wrapped to it, and
 * reads each row as "Interface.member" and its state as a word ("wrapped",
 * "shared", "absent", "unwrappable") so it needs none of these types. */
enum { D_CE_METHOD, D_CE_SETTER, D_CE_EXOTIC, D_CE_ALL_SETTERS };
enum { D_CE_ABSENT, D_CE_WRAPPED, D_CE_SHARED, D_CE_UNWRAPPABLE };
typedef struct {
    const char *interface, *member;
    uint8_t kind;
} DReactionRow;
size_t d_reactions_rows(void);
bool d_reactions_row_name(size_t i, char *out, size_t cap);
const char *d_reactions_row_state(const os64_dom_t *dom, size_t i);
size_t d_reactions_wrapped(const os64_dom_t *dom);
#endif
