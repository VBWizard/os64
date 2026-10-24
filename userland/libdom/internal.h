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
typedef struct DValue DValue;
typedef struct DQuery DQuery;
typedef struct DListener DListener;
typedef struct DTimer DTimer;
typedef struct DScript DScript;
typedef struct DRequest DRequest;
typedef struct DActiveEvent { struct DActiveEvent *previous; JSValue value; } DActiveEvent;
/* The wrappers' prototypes, one per kind of node the binding tells apart
 * (node.c's d_node_install builds the chain). An HTML element is an
 * HTML_ELEMENT, or one of the kinds after it when it has one of its own;
 * an element of another namespace is an ELEMENT. ATTR is Attr's, which is
 * a Node and no tree node: no Node member is installed on it, and it
 * shadows every one with its own answer (attr.c). */
enum {
    D_PROTO_NODE, D_PROTO_DOCUMENT, D_PROTO_ELEMENT, D_PROTO_CHARACTER_DATA,
    D_PROTO_FRAGMENT, D_PROTO_DOCTYPE, D_PROTO_ATTR, D_PROTO_HTML_ELEMENT, D_PROTO_INPUT, D_PROTO_TEXTAREA, D_PROTO_SELECT,
    D_PROTO_BUTTON, D_PROTO_FORM, D_PROTO_IMAGE, D_PROTO_SCRIPT, D_PROTO_ANCHOR,
    D_PROTO_LINK, D_PROTO_COUNT
};
/* The kind a kind's prototype inherits, as its interface inherits: Node's
 * for the node kinds, Element's for HTMLElement, HTMLElement's for the kinds
 * after it; -1 for Node, which inherits Object's. */
int d_proto_parent(int kind);
/* The event types the binding names, which are the ones on<type> names and
 * os64_dom_listens count: the ones the host dispatches, the three the
 * binding fires at window for a script's failures (report.c), and online,
 * offline and languagechange, whose window handlers exist though nothing
 * here ever fires them (browsing.c says why). A script may listen for any
 * other type. The handler properties of the ones from SCROLL on are the
 * Modern Web arc's, installed by its table's rows (d_event_handler), not
 * with the rest. */
enum {
    D_EVENT_CLICK, D_EVENT_MOUSEDOWN, D_EVENT_MOUSEUP, D_EVENT_MOUSEOVER,
    D_EVENT_MOUSEOUT, D_EVENT_MOUSEMOVE, D_EVENT_KEYDOWN, D_EVENT_KEYPRESS,
    D_EVENT_KEYUP, D_EVENT_INPUT, D_EVENT_CHANGE, D_EVENT_SUBMIT, D_EVENT_RESET,
    D_EVENT_FOCUS, D_EVENT_BLUR, D_EVENT_LOAD, D_EVENT_SCROLL, D_EVENT_RESIZE,
    D_EVENT_POPSTATE, D_EVENT_HASHCHANGE, D_EVENT_ONLINE, D_EVENT_OFFLINE,
    D_EVENT_LANGUAGECHANGE, D_EVENT_ERROR, D_EVENT_UNHANDLEDREJECTION,
    D_EVENT_REJECTIONHANDLED, D_EVENT_DOM_CONTENT_LOADED,
    D_EVENT_COUNT
};
/* A wrapper, or another engine value the binding holds strongly. A node's
 * wrapper also carries the node's listener list, in the order listeners and
 * handler slots were first added, and the element's views (its style, its
 * classList, its dataset, its attributes), each made once. A view's own
 * entry holds the element it stands for as `target`; so does an Attr's
 * while it is attached (attr.c). */
struct DValue {
    JSValue value;
    DValue *next, *hash_next;
    const os64_html_node_t *node, *target;
    os64_dom_t *dom;
    DValue *style, *class_list, *dataset, *attributes;
    /* An Attr's own entry: its record (attr.c). */
    struct DAttr *attr;
    DQuery *child_nodes, *children, *images, *forms, *elements;
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
 * (application/xhtml+xml) and elements in no namespace otherwise. */
enum { D_DOCUMENT_HTML = 0, D_DOCUMENT_XHTML, D_DOCUMENT_XML };
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
    bool descendants, elements;
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
    uint32_t attribute_mask;
    uint64_t attribute_version;
    bool attribute_scanned;
    unsigned dispatching;
    DActiveEvent *current_event;
    /* The classes of plain event targets (d_event_target), and each one's
     * resolver, NULL for a class whose opaque is its entry. */
    JSClassID target_classes[8];
    int (*target_resolvers[8])(os64_dom_t *, JSContext *, JSValueConst, bool, DValue **);
    unsigned target_class_count;
    bool sweep_pending;
    DListener *invoking;
    DValue *invoking_entry;
    DTimer *timers;
    size_t timer_count;
    uint32_t timer_id, timer_nesting;
    /* HTML's click-in-progress flags, one link per element.click() on the
     * C stack (window.c). */
    const struct DClicking *clicking;
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
};
#define D_NO_SCRIPT UINT32_MAX
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
 * XMLHttpRequest's timeout): `callback` (taken) is called with no
 * arguments once, `delay` ms from now on the host's clock, as one of the
 * page's timers. Its id, or 0 with an exception pending. Cancelling an id
 * that has fired, or 0, does nothing. */
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
 * MediaQueryList, an AbortSignal, an XMLHttpRequest and its upload), whose
 * entry holds its listeners. A row registers the object's class and gives
 * its prototype the three EventTarget methods with d_event_target, and an
 * on<type> property with d_event_handler (one of the binding's event
 * types) or d_event_handler_named (any type). Where the entry lives is the
 * class's choice, and everything else (the methods, the listener records,
 * the dispatch) is the same for all three:
 *   - the object's opaque, `resolve` NULL, a registry entry the object
 *     keeps for the page's life (performance, screen.orientation);
 *   - made on demand by a resolver, for a class whose objects come and go
 *     (a MediaQueryList): it makes the entry only when `make` (a listener
 *     added, a handler set, a dispatch), and answers a NULL entry otherwise
 *     when there is none, which a read, a removal or a null handler acts on
 *     as nothing; the class releases an entry once nothing listens on it
 *     (view.c's media_sweep);
 *   - IN THE OBJECT, `resolve` NULL, for a class a page makes one of per
 *     request (an AbortSignal, an XMLHttpRequest), which would otherwise
 *     keep every one alive on the registry: the object's state BEGINS with
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
    D_ATTRIBUTE_NAMES, D_TOGGLE_ATTR, D_HAS_ATTRS
};
enum { D_QUERY_GENERIC, D_QUERY_IMAGES, D_QUERY_FORMS, D_QUERY_CONTROLS, D_QUERY_DOCUMENT_NAMES,
       D_QUERY_CLASSES, D_QUERY_NAME };
bool d_named_match(const os64_html_node_t *node, const char *name);
bool d_classic_match(const os64_html_node_t *root, const os64_html_node_t *node, unsigned kind);
JSValue d_special_collection(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *root, unsigned kind, const char *named);
JSValue d_classic_collection(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node, unsigned kind);
int d_classic_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
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
 * made by eval). And the script whose callback the binding is about to
 * call (D_NO_SCRIPT once the call is over): what an exception the call
 * ends in is muted by. */
uint32_t d_callback_script(os64_dom_t *dom, JSContext *ctx, JSValueConst function);
void d_script_calling(os64_dom_t *dom, uint32_t script);
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
#endif
