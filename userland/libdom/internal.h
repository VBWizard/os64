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
 * an element of another namespace is an ELEMENT. */
enum {
    D_PROTO_NODE, D_PROTO_DOCUMENT, D_PROTO_ELEMENT, D_PROTO_CHARACTER_DATA,
    D_PROTO_FRAGMENT, D_PROTO_HTML_ELEMENT, D_PROTO_INPUT, D_PROTO_TEXTAREA, D_PROTO_SELECT,
    D_PROTO_BUTTON, D_PROTO_FORM, D_PROTO_IMAGE, D_PROTO_SCRIPT, D_PROTO_ANCHOR,
    D_PROTO_LINK, D_PROTO_COUNT
};
/* The kind a kind's prototype inherits, as its interface inherits: Node's
 * for the node kinds, Element's for HTMLElement, HTMLElement's for the kinds
 * after it; -1 for Node, which inherits Object's. */
int d_proto_parent(int kind);
/* The event types the host dispatches, which are the ones on<type> names
 * and os64_dom_listens count. A script may listen for any other type. */
enum {
    D_EVENT_CLICK, D_EVENT_MOUSEDOWN, D_EVENT_MOUSEUP, D_EVENT_MOUSEOVER,
    D_EVENT_MOUSEOUT, D_EVENT_MOUSEMOVE, D_EVENT_KEYDOWN, D_EVENT_KEYPRESS,
    D_EVENT_KEYUP, D_EVENT_INPUT, D_EVENT_CHANGE, D_EVENT_SUBMIT, D_EVENT_RESET,
    D_EVENT_FOCUS, D_EVENT_BLUR, D_EVENT_LOAD, D_EVENT_DOM_CONTENT_LOADED,
    D_EVENT_COUNT
};
/* A wrapper, or another engine value the binding holds strongly. A node's
 * wrapper also carries the node's listener list, in the order listeners and
 * handler slots were first added, and the element's views (its style, its
 * classList, its dataset), each made once. A view's own entry holds the
 * element it stands for as `target`. */
struct DValue {
    JSValue value;
    DValue *next, *hash_next;
    const os64_html_node_t *node, *target;
    os64_dom_t *dom;
    DValue *style, *class_list, *dataset;
    DQuery *child_nodes, *children, *images, *forms, *elements;
    DListener *listeners, *last_listener;
    /* A COLLECTABLE plain target's (d_event_target_init): this record is
     * the first member of the object's own state, outside the registry;
     * `value` is the object itself, not a reference; its listeners'
     * callbacks are traced by the object's gc_mark and freed with it. */
    bool engine_held;
};
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
};
/* DListener.handler: zero for a listener, else the state of a handler slot.
 * An ERROR slot failed to compile its attribute and stays absent until the
 * attribute changes. */
enum { D_LISTENER = 0, D_SLOT_EMPTY, D_SLOT_FUNCTION, D_SLOT_UNCOMPILED, D_SLOT_ERROR };
struct DTimer {
    DTimer *next;
    uint64_t due, delay;
    uint32_t id, nesting;
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
    /* The classes of plain event targets (d_event_target). */
    JSClassID target_classes[8];
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
    /* Symbol.iterator as the engine made it, for reading an iterable a
     * page hands over (url.c). */
    DValue *iterator_symbol;
    /* A script's requests (slice 2): DOMException's prototype, the host's
     * network, the requests out with the next token, and how many engine
     * values they hold (the registry's fourth list). */
    DValue *exception_prototype, *file_prototype, *form_prototype, *progress_prototype;
    os64_dom_network_t network;
    DRequest *requests;
    uint32_t request_token;
    size_t requests_held;
};
typedef struct { char *data; size_t length; } DString;

/* The caller passes its own compiled ABI identity, including callback units.
 * Native state is carried by private function data, never opaque host slots. */
os64_dom_t *d_callback(JSContext *ctx, JSValueConst *data, const char *abi);
bool d_context(os64_dom_t *dom, JSContext *ctx, const char *abi);
void *d_alloc(os64_dom_t *dom, size_t size);
void d_free(os64_dom_t *dom, void *ptr);
JSValue d_error(JSContext *ctx, const char *name, const char *message);
JSValue d_html_error(JSContext *ctx, int64_t status);
JSValue d_page_error(JSContext *ctx, int64_t status);
DValue *d_retain(os64_dom_t *dom, JSContext *ctx, JSValue value);
int d_method(os64_dom_t *dom, JSContext *ctx, JSValueConst target,
             const char *name, JSCFunctionData *fn, int argc, int magic);
int d_accessor(os64_dom_t *dom, JSContext *ctx, JSValueConst target,
               const char *name, JSCFunctionData *fn, int magic, bool writable);
JSValue d_wrap(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node);
/* d_wrap for an interface's constructor: a node with no wrapper yet gets
 * one whose prototype is `prototype` when that is an object (new.target's,
 * WebIDL's "get prototype from constructor"), else its kind's; identity,
 * holds and the registry are d_wrap's. */
JSValue d_wrap_constructed(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node,
                           JSValueConst prototype);
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
 * is an EventTarget all the same (performance, screen.orientation), whose
 * opaque is its own registry entry, which holds its listeners. A row
 * registers the object's class and gives its prototype the three
 * EventTarget methods with d_event_target, and an on<type> property for one
 * of the binding's event types with d_event_handler. d_dispatch_plain is
 * d_dispatch at one: the path is the target alone. */
int d_event_target(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype, JSClassID class_id);
int d_event_handler(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype, int kind);
int d_dispatch_plain(os64_dom_t *dom, JSContext *ctx, JSValueConst event, DValue *target);
JSValue d_event_new(os64_dom_t *dom, JSContext *ctx, const char *type, bool bubbles,
                    bool cancelable, bool trusted);
/* d_event_new with a prototype of the caller's (a ProgressEvent's), or the
 * Event prototype when `prototype` is not an object. */
JSValue d_event_new_proto(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype, const char *type, bool bubbles,
                          bool cancelable, bool trusted);
/* A COLLECTABLE plain target (event.c): an object a page may make as often
 * as it likes, whose state begins with a DValue that is NOT on the
 * registry. Its class is registered with d_event_target as any plain
 * target's; d_event_target_init fills the record for `object`, the class's
 * gc_mark calls d_event_target_mark and its finalizer
 * d_event_target_finalize, which frees the listeners. on<type> for any type
 * is d_event_handler_named. d_dispatch_plain_task is d_dispatch_plain from
 * a host task: each listener a call and a checkpoint, as the host's own
 * dispatch runs them, the outcome folded into `outcome`. */
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
bool d_connected(const os64_dom_t *dom, const os64_html_node_t *node);
const os64_html_node_t *d_form_owner(const os64_dom_t *dom, const os64_html_node_t *node);
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
    D_ID, D_CLASS, D_VALUE, D_CHECKED, D_SELECTED_INDEX
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
bool d_classic_match(os64_dom_t *dom, const os64_html_node_t *root, const os64_html_node_t *node, unsigned kind);
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
int d_storage_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_performance_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_url_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_encoding_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_screen_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_base64_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
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
/* ── Slice 2: a script's requests (fetch.c, xhr.c, blob.c, abort.c,
 * exception.c). LIBDOM.md § Requests is the record. ── */
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
/* AbortSignal (abort.c). A signal's algorithms are C's: a fetch's abort,
 * by its request's token, and a dependent signal's. */
JSValue d_signal_new(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype);
bool d_signal_is(JSValueConst value);
bool d_signal_aborted(JSValueConst signal, JSValue *reason, JSContext *ctx);
int d_signal_abort(os64_dom_t *dom, JSContext *ctx, JSValueConst signal, JSValueConst reason);
int d_signal_follow(os64_dom_t *dom, JSContext *ctx, JSValueConst dependent, JSValueConst source);
int d_signal_add_fetch(JSContext *ctx, JSValueConst signal, uint32_t token);
void d_signal_fetch_answered(JSContext *ctx, JSValueConst signal, uint32_t token, JSValueConst response);
void d_signal_remove_fetch(JSContext *ctx, JSValueConst signal, uint32_t token);
/* fetch.c: a fetch's abort, the algorithm its signal runs. `response` is
 * the Response once the fetch answered, else undefined. */
void d_fetch_abort(os64_dom_t *dom, JSContext *ctx, uint32_t token, JSValueConst response, JSValueConst reason);
/* True when an abort could no longer reach `response`'s body: it has none,
 * it has been read, or an abort already reached it. */
bool d_response_body_settled(JSValueConst response);
/* Bytes and their type (blob.c, fetch.c): js_malloc'd, `type` "" for none. */
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
/* url.c: `input` parsed as a URL against the document's base URL, as a
 * link's href is resolved: 1 with the address (d_alloc'd, the caller frees
 * it), 0 for one that does not parse, -1 with an exception pending. */
int d_url_parse(os64_dom_t *dom, JSContext *ctx, JSValueConst input, char **out);
/* An object's own [Symbol.toStringTag] (classic.c), as an interface's. */
int d_string_tag(JSContext *ctx, JSValueConst object, const char *name);
JSValue d_style(os64_dom_t *dom, JSContext *ctx, JSValueConst self);
int d_style_install(os64_dom_t *dom, JSContext *ctx);
extern JSClassExoticMethods d_node_exotic;
#endif
