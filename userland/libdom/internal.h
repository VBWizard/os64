#ifndef DOM_INTERNAL_H
#define DOM_INTERNAL_H

#include "dom/dom.h"
#include "os64/js_engine.h"
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
 * and os64_dom_listens count. A script may listen for any other type. The
 * handler properties of the ones from SCROLL on are the Modern Web arc's,
 * installed by its table's rows (d_handler_install), not with the rest. */
enum {
    D_EVENT_CLICK, D_EVENT_MOUSEDOWN, D_EVENT_MOUSEUP, D_EVENT_MOUSEOVER,
    D_EVENT_MOUSEOUT, D_EVENT_MOUSEMOVE, D_EVENT_KEYDOWN, D_EVENT_KEYPRESS,
    D_EVENT_KEYUP, D_EVENT_INPUT, D_EVENT_CHANGE, D_EVENT_SUBMIT, D_EVENT_RESET,
    D_EVENT_FOCUS, D_EVENT_BLUR, D_EVENT_LOAD, D_EVENT_SCROLL, D_EVENT_RESIZE,
    D_EVENT_POPSTATE, D_EVENT_HASHCHANGE, D_EVENT_DOM_CONTENT_LOADED,
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
     * and whether the page asked to keep its own scroll. */
    os64_dom_history_t history_host;
    char *entry_state;
    size_t entry_state_length;
    DValue *entry_state_value;
    bool entry_state_read, scroll_manual;
    DValue *popstate_prototype, *hashchange_prototype;
    /* JS_GetClassID of a plain object and of an array: the only objects
     * a state's JSON carries. */
    JSClassID plain_class, array_class;
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
 * `node`, at window (both NULL), or at `lone`, a target that is no node.
 * `make` builds the Event inside the task (its own prototype and fields);
 * the caller has already asked whether anything listens. */
os64_js_status_t d_host_dispatch(os64_dom_t *dom, const os64_html_node_t *node, DValue *lone,
                                 const char *type, JSValue (*make)(os64_dom_t *, JSContext *, void *),
                                 void *arg, bool *prevented, os64_js_outcome_t *outcome);
/* The on<type> property of a handler type (D_EVENT_*) on `target`. */
int d_handler_install(os64_dom_t *dom, JSContext *ctx, JSValueConst target, int kind);
/* EventTarget's three methods on `target`, a prototype whose objects
 * target_entry resolves. */
int d_target_install(os64_dom_t *dom, JSContext *ctx, JSValueConst target);
/* MediaQueryList's legacy addListener and removeListener: the `change`
 * listener `callback` added to or removed from `self`. */
JSValue d_listener_change(os64_dom_t *dom, JSContext *ctx, JSValueConst self, JSValueConst callback,
                          bool add);
/* Whether `self` is a MediaQueryList (view.c): 1 with its target record in
 * *entry, made the first time it is asked for, 0 when it is something
 * else, -1 with an exception pending when the record could not be made. */
int d_media_target(os64_dom_t *dom, JSContext *ctx, JSValueConst self, DValue **entry);
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
int d_view_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_media_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_computed_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
int d_history_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
/* An interface object `name` on `global` for `prototype`, made as the
 * interface objects row makes its own (WebIDL § 3.7): the prototype's
 * `constructor` and Symbol.toStringTag, the object's [[Prototype]]
 * `parent` when that is an object, and `construct` its constructor, made
 * constructor-only so it is never just called (it finds its binding with
 * d_binding), or an Illegal constructor when NULL. */
int d_interface_install(JSContext *ctx, JSValueConst global, const char *name, JSValueConst prototype,
                        JSValueConst parent, JSCFunction *construct);
/* The view's records freed, after the engine (os64_dom_free). */
void d_view_free(os64_dom_t *dom);
JSValue d_style(os64_dom_t *dom, JSContext *ctx, JSValueConst self);
int d_style_install(os64_dom_t *dom, JSContext *ctx);
extern JSClassExoticMethods d_node_exotic;
#endif
