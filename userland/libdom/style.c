#include "internal.h"
#include "garb/garb.h"
#include "garb/values.h"

/* Inline declarations stay in the style attribute, so verbs, CSS and script
 * observe the same source. Edits replace the declarations they cover
 * (covered) instead of accumulating a new declaration on each mouse move. */
#define STYLE_MAX ((size_t)65536)
/* A member's magic is the index of the property name it stands for
 * (garb_prop_name_at), or one of these, which sit below D_SET past every
 * name's index. */
enum { S_TEXT = 0xF00, S_GET, S_SET, S_REMOVE, S_PRIORITY, S_LENGTH, S_ITEM, S_PARENT };

static bool space(unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f'; }
static bool equal(const char *a, size_t n, const char *b)
{
    if (os64_strlen(b) != n) return false;
    if (b[0] == '-' && b[1] == '-') return os64_memcmp(a,b,n) == 0;
    for (size_t i = 0; i < n; i++) {
        char c = a[i] >= 'A' && a[i] <= 'Z' ? a[i] + ('a'-'A') : a[i];
        if (c != b[i]) return false;
    }
    return true;
}
static bool budget(os64_dom_t *dom, JSContext *ctx)
{
    os64_js_outcome_t out;
    if (os64_js_check_budget(dom->runtime,OS64_JS_ABI_ID,&out) == OS64_JS_OK) return true;
    d_error(ctx,"InvalidStateError","Inline style work exceeded the script budget");
    return false;
}

/* Find separators outside CSS strings, comments and bracketed component
 * values. The CSS parser, rather than this boundary scan, judges each value. */
static size_t boundary(const char *s, size_t n, size_t at, char wanted)
{
    char quote = 0;
    unsigned depth = 0;
    for (; at < n; at++) {
        char c = s[at];
        if (c == '\\') { if (at + 1 < n) at++; continue; }
        if (quote) { if (c == quote) quote = 0; continue; }
        if (c == '/' && at + 1 < n && s[at+1] == '*') {
            at += 2;
            while (at < n && !(s[at] == '*' && at+1 < n && s[at+1] == '/')) at++;
            if (at < n) at++;
            continue;
        }
        if (c == '\'' || c == '"') quote = c;
        else if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') { if (depth) depth--; }
        else if (!depth && c == wanted) return at;
    }
    return n;
}

static int parse(os64_dom_t *dom, JSContext *ctx, const char *s, size_t n,
                  garb_parsed_t *out)
{
    garb_status_t result = garb_parse_one_declaration(s,n,out);
    if (result == GARB_NO_MEMORY || result == GARB_TOO_BIG || out->incomplete) {
        d_error(ctx,"QuotaExceededError","Inline style parse quota exceeded");return -1;
    }
    if (result != GARB_OK) return 0;
    if (garb_decl_is_custom(&out->decl)) return 1;
    garb_set_t *sets = d_alloc(dom,sizeof(*sets)*GARB_NPROPS);
    if (sets == NULL) { d_error(ctx,"QuotaExceededError","Inline style value quota exceeded");return -1; }
    garb_reading_t how = {.quirks = dom->document->quirks == OS64_HTML_QUIRKS, .modern_off = !dom->options.modern};
    int count = garb_read_declaration(out,&out->decl,how,sets);
    d_free(dom,sets);
    if (out->incomplete) {
        d_error(ctx,"QuotaExceededError","Inline style value quota exceeded");return -1;
    }
    return count > 0 || count == GARB_DECL_HELD;
}

/* What a property name stands for in the declarations: a custom property
 * its own name, case kept; any other the longhands it sets (d_longhands),
 * or, when libgarb does not know them, its name ASCII case folded. */
typedef struct {
    const char *name;
    size_t length;
    bool custom;
    int32_t n;
    int32_t props[D_LONGHANDS_MAX];
} Stands;

static bool stands_for(os64_dom_t *dom, JSContext *ctx, const char *name, size_t length, Stands *out)
{
    out->name = name;
    out->length = length;
    out->custom = length >= 2 && name[0] == '-' && name[1] == '-';
    out->n = out->custom ? 0 : d_longhands(dom,name,length,out->props);
    if (out->n < 0) { d_error(ctx,"QuotaExceededError","Inline style name quota exceeded");return false; }
    return true;
}

/* Whether a declaration of `decl` is one that setting or removing `target`
 * takes away: the same custom property; the same name, when libgarb cannot
 * say what either stands for; or longhands every one of which `target`
 * also sets (`margin` covers a `margin-top`, never the other way:
 * taking one longhand out of a shorthand would mean writing the rest back
 * as text, which DEBTS.md books with serialization). */
static bool covered(const Stands *decl, const Stands *target)
{
    if (decl->custom || target->custom)
        return decl->custom && target->custom && decl->length == target->length &&
               os64_memcmp(decl->name,target->name,decl->length) == 0;
    if (decl->n == 0 || target->n == 0) {
        char folded[128];
        if (target->length >= sizeof(folded)) return false;
        os64_memcpy(folded,target->name,target->length);
        folded[target->length] = '\0';
        d_fold(folded);
        return equal(decl->name,decl->length,folded);
    }
    for (int32_t i = 0; i < decl->n; i++) {
        bool in = false;
        for (int32_t k = 0; k < target->n && !in; k++) in = decl->props[i] == target->props[k];
        if (!in) return false;
    }
    return true;
}

static JSValue read(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node,
                     const char *name, bool priority)
{
    dom = d_owner(dom,node);
    const os64_html_attr_t *attr = os64_html_attr(node,"style");
    const char *text = attr != NULL ? attr->value : "";
    size_t len = os64_strlen(text), first = 0, last = 0;
    bool found = false, important = false;
    if (len > STYLE_MAX) return d_error(ctx,"QuotaExceededError","Inline style text quota exceeded");
    for (size_t at = 0; at < len;) {
        size_t end = boundary(text,len,at,';');
        garb_parsed_t parsed = {0};
        int valid = parse(dom,ctx,text+at,end-at,&parsed);
        if (valid < 0) { garb_free(&parsed);return JS_EXCEPTION; }
        if (valid && equal(parsed.decl.name,parsed.decl.len,name) && (!important || parsed.decl.important)) {
            size_t colon = boundary(text,end,at,':');
            first = colon < end ? colon+1 : end;
            last = parsed.decl.important ? boundary(text,end,first,'!') : end;
            while (first < last && space((unsigned char)text[first])) first++;
            while (last > first && space((unsigned char)text[last-1])) last--;
            found = true; important = parsed.decl.important;
        }
        garb_free(&parsed);
        at = end < len ? end+1 : len;
    }
    if (!budget(dom,ctx)) return JS_EXCEPTION;
    return priority ? JS_NewString(ctx,important ? "important" : "") :
           JS_NewStringLen(ctx,found ? text+first : "",found ? last-first : 0);
}

static JSValue write(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node,
                      const char *name, const char *value, const char *priority)
{
    size_t name_len = os64_strlen(name), value_len = os64_strlen(value);
    if (name_len == 0 || name_len > 128) return JS_UNDEFINED;
    for (size_t i = 0; i < name_len; i++)
        if (!(name[i] == '-' || name[i] == '_' || (name[i]>='a' && name[i]<='z') ||
              (name[i]>='A' && name[i]<='Z') || (i && name[i]>='0' && name[i]<='9'))) return JS_UNDEFINED;
    bool important = equal(priority,os64_strlen(priority),"important");
    if (*priority && !important && value_len != 0) return JS_UNDEFINED;
    dom = d_owner(dom,node);
    const os64_html_attr_t *attr = os64_html_attr(node,"style");
    const char *old = attr != NULL ? attr->value : "";
    size_t old_len = os64_strlen(old);
    if (old_len > STYLE_MAX || value_len > STYLE_MAX || old_len + value_len + name_len + 16 > STYLE_MAX)
        return d_error(ctx,"QuotaExceededError","Inline style text quota exceeded");
    // The declarations the new one takes the place of (covered): any whose
    // longhands it sets all of, its own name's among them, a shorthand's
    // longhands and narrower shorthands (`border` takes `border-top`), and
    // a name libgarb maps onto the same longhand (`margin-inline-start`
    // and `margin-left`: DEBTS.md).
    Stands target;
    if (!stands_for(dom,ctx,name,name_len,&target)) return JS_EXCEPTION;
    char *text = d_alloc(dom,old_len + value_len + name_len + 16);
    if (text == NULL) return d_error(ctx,"QuotaExceededError","Inline style edit quota exceeded");
    size_t used = 0;
    for (size_t at = 0; at < old_len;) {
        size_t end = boundary(old,old_len,at,';');
        garb_parsed_t parsed = {0};
        int valid = parse(dom,ctx,old+at,end-at,&parsed);
        Stands decl;
        bool replace = false;
        if (valid > 0 && !stands_for(dom,ctx,parsed.decl.name,parsed.decl.len,&decl)) valid = -1;
        else if (valid > 0) replace = covered(&decl,&target);
        garb_free(&parsed);
        if (valid < 0) { d_free(dom,text);return JS_EXCEPTION; }
        if (!replace && end > at) {
            os64_memcpy(text+used,old+at,end-at);used += end-at;text[used++] = ';';
        }
        at = end < old_len ? end+1 : old_len;
    }
    if (value_len != 0) {
        size_t start = used;
        os64_memcpy(text+used,name,name_len);used += name_len;text[used++] = ':';
        os64_memcpy(text+used,value,value_len);used += value_len;
        garb_parsed_t parsed = {0};
        int valid = parse(dom,ctx,text+start,used-start,&parsed);
        bool embedded_priority = parsed.decl.important;
        garb_free(&parsed);
        if (valid <= 0 || embedded_priority) { d_free(dom,text);return valid < 0 ? JS_EXCEPTION : JS_UNDEFINED; }
        if (important) { os64_memcpy(text+used,"!important",10);used += 10; }
        text[used++] = ';';
    }
    text[used] = '\0';
    if (!budget(dom,ctx)) { d_free(dom,text);return JS_EXCEPTION; }
    int64_t status = d_set_attr(dom,node,"style",text,used,false);
    d_free(dom,text);
    if (status != OS64_HTML_OK) return d_page_error(ctx,status);
    return budget(dom,ctx) ? JS_UNDEFINED : JS_EXCEPTION;
}

JSValue d_style_write(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node, const char *name,
                      const char *value)
{
    return write(dom,ctx,node,name,value,"");
}

/* THE LIST (CSSOM § The CSSStyleDeclaration Interface: the declarations,
 * by index): each valid declaration in order, a custom property by its name
 * as written and any other as the longhands it stands for (`margin: 0` is
 * margin-top, -right, -bottom and -left), each name once, at the place it
 * first appears. A name whose longhands libgarb cannot list is left out.
 * The style attribute is read again for every question, as classList reads
 * its attribute, so the text stays the one truth. The entry `want` is put
 * in `*out` as a string (none for UINT32_MAX, or one past the end); the
 * answer is the list's length, or -1 with an exception pending. */
static int64_t listed(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node, uint32_t want, JSValue *out)
{
    dom = d_owner(dom,node);
    const os64_html_attr_t *attr = os64_html_attr(node,"style");
    const char *text = attr != NULL ? attr->value : "";
    size_t len = os64_strlen(text);
    if (len > STYLE_MAX) { d_error(ctx,"QuotaExceededError","Inline style text quota exceeded");return -1; }
    // The custom names met so far, end to end in `names`: together no
    // longer than the text they came from.
    char *names = d_alloc(dom,len + 1);
    size_t *ends = d_alloc(dom,(len / 2 + 1) * sizeof(*ends));
    if (names == NULL || ends == NULL) {
        if (names != NULL) d_free(dom,names);
        if (ends != NULL) d_free(dom,ends);
        d_error(ctx,"QuotaExceededError","Inline style list quota exceeded");
        return -1;
    }
    bool seen[GARB_NPROPS];
    os64_memset(seen,0,sizeof(seen));
    int64_t count = 0;
    size_t ncustom = 0, used = 0;
    for (size_t at = 0; at < len && count >= 0;) {
        size_t end = boundary(text,len,at,';');
        garb_parsed_t parsed = {0};
        int valid = parse(dom,ctx,text+at,end-at,&parsed);
        Stands s;
        if (valid > 0 && !stands_for(dom,ctx,parsed.decl.name,parsed.decl.len,&s)) valid = -1;
        if (valid < 0) {
            count = -1;
        } else if (valid > 0 && s.custom) {
            bool met = false;
            for (size_t i = 0, from = 0; i < ncustom && !met; from = ends[i++])
                met = ends[i] - from == s.length && os64_memcmp(names + from,s.name,s.length) == 0;
            if (!met) {
                if (count == want) *out = JS_NewStringLen(ctx,s.name,s.length);
                os64_memcpy(names + used,s.name,s.length);
                used += s.length;
                ends[ncustom++] = used;
                count++;
            }
        } else if (valid > 0) {
            for (int32_t i = 0; i < s.n; i++) {
                // A longhand the Modern Web arc added is no name at all
                // with its switch off, inside a shorthand as much as alone.
                const char *longhand = garb_prop_name((garb_prop_t)s.props[i]);
                if (seen[s.props[i]] || !garb_prop_known(longhand,os64_strlen(longhand),!dom->options.modern))
                    continue;
                seen[s.props[i]] = true;
                if (count == want) *out = JS_NewString(ctx,longhand);
                count++;
            }
        }
        garb_free(&parsed);
        at = end < len ? end+1 : len;
    }
    d_free(dom,names);
    d_free(dom,ends);
    if (count >= 0 && !budget(dom,ctx)) count = -1;
    return count;
}

static DValue *style_receiver(JSContext *ctx, JSValueConst object)
{
    JSClassID id;
    DValue *entry = JS_GetAnyOpaque(object,&id);
    if (entry == NULL) { d_error(ctx,"InvalidStateError","Inline style object is closed");return NULL; }
    return d_context(entry->dom,ctx,OS64_JS_ABI_ID) ? entry : NULL;
}

/* The list's indices (WebIDL's indexed getter), read-only and live, as
 * classList's are. */
static int style_own_property(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst object, JSAtom atom)
{
    uint32_t index;
    int numeric = d_index_key(ctx,atom,&index);
    if (numeric <= 0) return numeric;
    DValue *entry = style_receiver(ctx,object);
    if (entry == NULL) return -1;
    JSValue value = JS_UNDEFINED;
    int64_t count = listed(entry->dom,ctx,entry->target,index,&value);
    if (count < 0 || JS_IsException(value)) { JS_FreeValue(ctx,value);return -1; }
    if (index >= count) return 0;
    if (desc == NULL) { JS_FreeValue(ctx,value);return 1; }
    desc->flags = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
    desc->value = value;
    desc->getter = JS_UNDEFINED;
    desc->setter = JS_UNDEFINED;
    return 1;
}

static int style_own_names(JSContext *ctx, JSPropertyEnum **table, uint32_t *length, JSValueConst object)
{
    DValue *entry = style_receiver(ctx,object);
    if (entry == NULL) return -1;
    JSValue none = JS_UNDEFINED;
    int64_t count = listed(entry->dom,ctx,entry->target,UINT32_MAX,&none);
    return count < 0 ? -1 : d_index_names(ctx,(size_t)count,table,length);
}

static int style_define(JSContext *ctx, JSValueConst object, JSAtom atom, JSValueConst value,
                        JSValueConst getter, JSValueConst setter, int flags)
{
    DValue *entry = style_receiver(ctx,object);
    if (entry == NULL) return -1;
    uint32_t index;
    int numeric = d_index_key(ctx,atom,&index);
    if (numeric < 0) return -1;
    return JS_DefineProperty(ctx,numeric ? entry->dom->index_guard->value : object,atom,
                             value,getter,setter,flags | JS_PROP_NO_EXOTIC);
}

static int style_delete(JSContext *ctx, JSValueConst object, JSAtom atom)
{
    uint32_t index;
    int numeric = d_index_key(ctx,atom,&index);
    if (numeric <= 0) return numeric < 0 ? -1 : 1;
    int present = style_own_property(ctx,NULL,object,atom);
    return present < 0 ? -1 : !present;
}

JSClassExoticMethods d_style_exotic = {
    .get_own_property = style_own_property, .get_own_property_names = style_own_names,
    .define_own_property = style_define, .delete_property = style_delete,
};

static JSValue member(JSContext *ctx, JSValueConst self, int argc,
                       JSValueConst *argv, int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx,data,OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DValue *entry = JS_GetOpaque(self,dom->style_class);
    if (entry == NULL) return JS_ThrowTypeError(ctx,"Expected an inline style object");
    if (!budget(dom,ctx)) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0; magic &= ~D_SET;
    const os64_html_node_t *node = entry->target;
    // An inline style belongs to no rule.
    if (magic == S_PARENT) return JS_NULL;
    if (magic == S_LENGTH || magic == S_ITEM) {
        // item(index) is WebIDL's unsigned long: wrapped, as ToUint32
        // wraps, and the empty string past the end (CSSOM).
        uint32_t index = UINT32_MAX;
        if (magic == S_ITEM) {
            int32_t given;
            if (argc < 1) return JS_ThrowTypeError(ctx,"item needs an index");
            if (JS_ToInt32(ctx,&given,argv[0]) < 0) return JS_EXCEPTION;
            index = (uint32_t)given;
        }
        JSValue value = JS_UNDEFINED;
        int64_t count = listed(dom,ctx,node,index,&value);
        if (count < 0) { JS_FreeValue(ctx,value);return JS_EXCEPTION; }
        if (magic == S_LENGTH) return JS_NewInt64(ctx,count);
        return JS_IsUndefined(value) ? JS_NewString(ctx,"") : value;
    }
    if (magic == S_TEXT) {
        if (!set) {
            dom = d_owner(dom,node);
    const os64_html_attr_t *attr = os64_html_attr(node,"style");
            const char *text = attr != NULL ? attr->value : "";
            size_t length = os64_strlen(text);
            if (length > STYLE_MAX) return d_error(ctx,"QuotaExceededError","Inline style text quota exceeded");
            JSValue value = JS_NewStringLen(ctx,text,length);
            if (!JS_IsException(value) && !budget(dom,ctx)) { JS_FreeValue(ctx,value);return JS_EXCEPTION; }
            return value;
        }
        DString value = {0};
        if (!d_string(dom,ctx,argc > 0 ? argv[0] : JS_UNDEFINED,&value)) return JS_EXCEPTION;
        int64_t status = value.length > STYLE_MAX ? -OS64_PAGE_REASON_TOO_LONG :
            d_set_attr(dom,node,"style",value.data,value.length,false);
        d_string_free(dom,&value);
        if (status != OS64_HTML_OK) return d_page_error(ctx,status);
        return budget(dom,ctx) ? JS_UNDEFINED : JS_EXCEPTION;
    }
    DString name = {0}, value = {0}, priority = {0};
    const char *property = magic < S_TEXT ? garb_prop_name_at(magic) : NULL;
    if (property == NULL) {
        if (argc < 1) return JS_ThrowTypeError(ctx,"Missing CSS property name");
        if (!d_string(dom,ctx,argv[0],&name)) return JS_EXCEPTION;
        if (!(name.data[0] == '-' && name.data[1] == '-')) d_fold(name.data);
        property = name.data;
    }
    JSValue result;
    if (magic == S_REMOVE) {
        result = read(dom,ctx,node,property,false);
        if (!JS_IsException(result)) {
            JSValue changed = write(dom,ctx,node,property,"","");
            if (JS_IsException(changed)) { JS_FreeValue(ctx,result);result = changed; }
        }
    } else if (!set && magic != S_SET) result = read(dom,ctx,node,property,magic == S_PRIORITY);
    else {
        int value_at = magic == S_SET ? 1 : 0;
        if (argc <= value_at) result = JS_ThrowTypeError(ctx,"Missing CSS property value");
        else if (!d_string(dom,ctx,argv[value_at],&value)) result = JS_EXCEPTION;
        else if (magic == S_SET && argc > 2 && !d_string(dom,ctx,argv[2],&priority)) result = JS_EXCEPTION;
        else result = write(dom,ctx,node,property,value.data,priority.data != NULL ? priority.data : "");
    }
    d_string_free(dom,&name);d_string_free(dom,&value);d_string_free(dom,&priority);
    return result;
}

JSValue d_style(os64_dom_t *dom, JSContext *ctx, JSValueConst self)
{
    const os64_html_node_t *node = d_node(dom,ctx,self);
    if (node == NULL) return JS_EXCEPTION;
    DValue *owner = d_find(dom,node);
    dom = owner->dom;
    os64_js_outcome_t outcome;
    ctx = os64_js_realm_context(dom->realm, OS64_JS_ABI_ID, &outcome);
    if (ctx == NULL) return JS_EXCEPTION;
    if (d_more_peek(owner)->style != NULL) return JS_DupValue(ctx,d_more_peek(owner)->style->value);
    DValueMore *more = d_more(dom,ctx,owner);
    if (more == NULL) return JS_EXCEPTION;
    JSValue object = JS_NewObjectClass(ctx,dom->style_class);
    DValue *entry = d_retain(dom,ctx,object);
    if (entry == NULL) return JS_EXCEPTION;
    entry->target = node;
    os64_html_hold(dom->document,node);
    JS_SetOpaque(object,entry);
    more->style = entry;
    return JS_DupValue(ctx,object);
}

int d_style_install(os64_dom_t *dom, JSContext *ctx)
{
    JSValue prototype = JS_NewObject(ctx);
    dom->style_prototype = d_retain(dom,ctx,prototype);
    if (dom->style_prototype == NULL) return -1;
    JS_SetClassProto(ctx,dom->style_class,JS_DupValue(ctx,prototype));
    /* Each property name libgarb reads is a member three ways (CSSOM §
     * 6.6.1): camel-cased (`marginInlineStart`, `WebkitTransform`),
     * webkit-cased for a `-webkit-` name (`webkitTransform`) and dashed as
     * written (`margin-inline-start`), shorthands and aliases as well as
     * longhands. One the Modern Web arc added goes with the switch, as its
     * name does from every declaration (`'aspectRatio' in el.style` is how
     * pages ask). */
    const char *name;
    for (int32_t i = 0; (name = garb_prop_name_at(i)) != NULL; i++) {
        size_t length = os64_strlen(name);
        if (i >= S_TEXT || length >= 128) return -1;
        if (!garb_prop_known(name,length,!dom->options.modern)) continue;
        char camel[128], webkit[128];size_t at = 0, wat = 0;
        bool upper = false, prefixed = length > 8 && os64_memcmp(name,"-webkit-",8) == 0;
        for (size_t j = 0; j < length; j++) {
            if (name[j] == '-') { upper = true;continue; }
            char c = upper && name[j] >= 'a' && name[j] <= 'z' ? name[j] - ('a'-'A') : name[j];
            upper = false;
            camel[at++] = c;
            if (prefixed) { webkit[wat] = wat == 0 && c >= 'A' && c <= 'Z' ? c + ('a'-'A') : c;wat++; }
        }
        camel[at] = '\0';webkit[wat] = '\0';
        if (d_accessor(dom,ctx,prototype,camel,member,i,true) < 0) return -1;
        if (prefixed && d_accessor(dom,ctx,prototype,webkit,member,i,true) < 0) return -1;
        if (at != length && d_accessor(dom,ctx,prototype,name,member,i,true) < 0) return -1;
        // `float` is a word JavaScript once reserved, so CSSOM names its
        // member cssFloat as well.
        if (os64_strcmp(name,"float") == 0 && d_accessor(dom,ctx,prototype,"cssFloat",member,i,true) < 0)
            return -1;
    }
    // The list (listed): its length, item() and an iterator over it; and
    // parentRule, which an inline style has none of.
    if (d_accessor(dom,ctx,prototype,"length",member,S_LENGTH,false) < 0 ||
        d_method(dom,ctx,prototype,"item",member,1,S_ITEM) < 0 ||
        d_accessor(dom,ctx,prototype,"parentRule",member,S_PARENT,false) < 0 ||
        d_iterable(ctx,prototype,false) < 0) return -1;
    if (d_accessor(dom,ctx,prototype,"cssText",member,S_TEXT,true) < 0 ||
        d_method(dom,ctx,prototype,"getPropertyValue",member,1,S_GET) < 0 ||
        d_method(dom,ctx,prototype,"getPropertyPriority",member,1,S_PRIORITY) < 0 ||
        d_method(dom,ctx,prototype,"setProperty",member,2,S_SET) < 0) return -1;
    return d_method(dom,ctx,prototype,"removeProperty",member,1,S_REMOVE);
}
