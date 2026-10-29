#include "internal.h"

/* THE FORM'S, LABEL'S, FIELDSET'S, LEGEND'S AND OUTPUT'S OWN (0f's P3b;
 * INTERFACES_FORMS.md § P3).
 *
 * Their reflected attributes are rows of controls.c's table, which also
 * gives a fieldset and an output their `form` and an output its `labels`.
 * Here, what is no plain attribute: a form's `length` and its `rel` and
 * `relList` (mixin.c's, as a link's); a label's labeled `control` and that
 * control's `form`; a fieldset's `type` and `elements`; a legend's `form`;
 * and an output's `value`, `defaultValue`, `type` and `htmlFor`, a
 * DOMTokenList over `for`. A form's `elements` is classic.c's, made an
 * HTMLFormControlsCollection with the switch on (collection.c).
 *
 * One row of the Modern Web table, naming nothing of window's. */

enum { F_LENGTH, F_CONTROL, F_LABEL_FORM, F_FIELDSET_TYPE, F_FIELDSET_ELEMENTS, F_LEGEND_FORM, F_OUTPUT_VALUE,
       F_OUTPUT_DEFAULT, F_OUTPUT_TYPE, F_OUTPUT_FOR };

static bool html(const os64_html_node_t *node, os64_html_tag_t tag)
{
    return node != NULL && node->kind == OS64_HTML_ELEMENT && node->ns == OS64_HTML_NS_HTML && node->tag == tag;
}

/* A fieldset's `elements` ([SameObject]): a live collection of its listed
 * descendants. A fieldset is never a form, so the entry's `elements` slot,
 * a form's, holds it. */
static JSValue fieldset_elements(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *fieldset)
{
    DValue *entry = d_find(dom, fieldset);
    if (entry == NULL) return JS_ThrowTypeError(ctx, "Missing node wrapper");
    if (entry->elements != NULL) return JS_DupValue(ctx, entry->elements->entry->value);
    JSValue value = d_special_collection(dom, ctx, fieldset, D_QUERY_FIELDSET, NULL);
    if (!JS_IsException(value)) entry->elements = JS_GetOpaque(value, dom->collection_class);
    return value;
}

/* An output's default value (HTML § 4.10.12): its override when it has
 * one, else its text. */
static JSValue output_default(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *output, DValue *entry)
{
    if (entry->output_default_set)
        return JS_NewStringLen(ctx, entry->output_default != NULL ? entry->output_default : "",
                               entry->output_default_len);
    return d_content_get(dom, ctx, output, D_TEXT_CONTENT);
}

/* Keep `text` as the output's override. False with QuotaExceededError
 * thrown when there is no room, and then the override is as it was. */
static bool keep_default(os64_dom_t *dom, JSContext *ctx, DValue *entry, const char *text, size_t length)
{
    char *copy = d_alloc(dom, length + 1);
    if (copy == NULL) {
        d_error(ctx, "QuotaExceededError", "DOM string quota exceeded");
        return false;
    }
    os64_memcpy(copy, text, length);
    copy[length] = '\0';
    d_free(dom, entry->output_default);
    entry->output_default = copy;
    entry->output_default_len = length;
    entry->output_default_set = true;
    return true;
}

/* String replace all (DOM § 4.2.6): textContent's setter, the value a
 * string first. */
static JSValue replace_text(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node, JSValueConst text)
{
    return d_content_set(dom, ctx, node, D_TEXT_CONTENT, text);
}

static JSValue output_member(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *output, int magic, bool set,
                             JSValueConst value)
{
    DValue *entry = d_find(dom, output);
    if (entry == NULL) return JS_ThrowTypeError(ctx, "Missing node wrapper");
    if (magic == F_OUTPUT_TYPE) return JS_NewString(ctx, "output");
    if (magic == F_OUTPUT_FOR) {
        if (!set) return d_for_list(dom, ctx, output);
        /* [PutForwards=value]: assigning the list sets the attribute. */
        DString text = {0};
        if (!d_string(dom, ctx, value, &text)) return JS_EXCEPTION;
        int64_t status = d_set_attr(dom, output, "for", text.data != NULL ? text.data : "", text.length, false);
        d_string_free(dom, &text);
        return status < 0 ? d_html_error(ctx, status) : JS_UNDEFINED;
    }
    if (!set)
        return magic == F_OUTPUT_VALUE ? d_content_get(dom, ctx, output, D_TEXT_CONTENT)
                                       : output_default(dom, ctx, output, entry);
    JSValue text = JS_ToString(ctx, value);
    if (JS_IsException(text)) return text;
    JSValue result = JS_UNDEFINED;
    if (magic == F_OUTPUT_VALUE) {
        /* The override is the default as it stands, then the text goes. */
        JSValue now = output_default(dom, ctx, output, entry);
        DString kept = {0};
        bool ok = !JS_IsException(now) && d_string(dom, ctx, now, &kept) &&
                  keep_default(dom, ctx, entry, kept.data != NULL ? kept.data : "", kept.length);
        JS_FreeValue(ctx, now);
        d_string_free(dom, &kept);
        result = ok ? replace_text(dom, ctx, output, text) : JS_EXCEPTION;
    } else if (!entry->output_default_set) {
        result = replace_text(dom, ctx, output, text);
    } else {
        DString given = {0};
        if (!d_string(dom, ctx, text, &given) ||
            !keep_default(dom, ctx, entry, given.data != NULL ? given.data : "", given.length))
            result = JS_EXCEPTION;
        d_string_free(dom, &given);
    }
    JS_FreeValue(ctx, text);
    return result;
}

static JSValue forms_member(JSContext *ctx, JSValueConst this_value, int argc, JSValueConst *argv, int magic,
                            JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *node = d_node(dom, ctx, this_value);
    if (node == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    JSValueConst value = argc > 0 ? argv[0] : JS_UNDEFINED;
    switch (magic) {
    case F_LENGTH:
        if (!html(node, OS64_HTML_TAG_FORM)) break;
    {
        uint32_t count = 0;
        (void)d_form_control(node, UINT32_MAX, &count);
        return JS_NewUint32(ctx, count);
    }
    case F_CONTROL: case F_LABEL_FORM: {
        if (!html(node, OS64_HTML_TAG_LABEL)) break;
        const os64_html_node_t *control = d_label_control(node);
        if (magic == F_CONTROL) return d_wrap(dom, ctx, control);
        /* A label's form is its control's form owner, when it has one. */
        return d_wrap(dom, ctx, control != NULL ? d_form_owner(control) : NULL);
    }
    case F_FIELDSET_TYPE: case F_FIELDSET_ELEMENTS:
        if (!html(node, OS64_HTML_TAG_FIELDSET)) break;
        return magic == F_FIELDSET_TYPE ? JS_NewString(ctx, "fieldset") : fieldset_elements(dom, ctx, node);
    case F_LEGEND_FORM:
        if (!html(node, OS64_HTML_TAG_LEGEND)) break;
        /* A legend's form is its fieldset parent's form owner. */
        return d_wrap(dom, ctx, html(node->parent, OS64_HTML_TAG_FIELDSET) ? d_form_owner(node->parent) : NULL);
    case F_OUTPUT_VALUE: case F_OUTPUT_DEFAULT: case F_OUTPUT_TYPE: case F_OUTPUT_FOR:
        if (!html(node, OS64_HTML_TAG_OUTPUT)) break;
        return output_member(dom, ctx, node, magic, set, value);
    default: break;
    }
    return JS_ThrowTypeError(ctx, "Illegal invocation");
}

int d_forms_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    (void)global;
    static const struct { const char *name; int magic; bool writable; int kind; } kMembers[] = {
        {"length", F_LENGTH, false, D_PROTO_FORM},
        {"control", F_CONTROL, false, D_PROTO_LABEL}, {"form", F_LABEL_FORM, false, D_PROTO_LABEL},
        {"type", F_FIELDSET_TYPE, false, D_PROTO_FIELDSET}, {"elements", F_FIELDSET_ELEMENTS, false, D_PROTO_FIELDSET},
        {"form", F_LEGEND_FORM, false, D_PROTO_LEGEND},
        {"value", F_OUTPUT_VALUE, true, D_PROTO_OUTPUT}, {"defaultValue", F_OUTPUT_DEFAULT, true, D_PROTO_OUTPUT},
        {"type", F_OUTPUT_TYPE, false, D_PROTO_OUTPUT}, {"htmlFor", F_OUTPUT_FOR, true, D_PROTO_OUTPUT},
    };
    for (size_t i = 0; i < sizeof(kMembers) / sizeof(kMembers[0]); i++)
        if (d_accessor(dom, ctx, dom->prototypes[kMembers[i].kind]->value, kMembers[i].name, forms_member,
                       kMembers[i].magic, kMembers[i].writable) < 0)
            return -1;
    return d_form_rel_install(dom, ctx);
}
