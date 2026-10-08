#include "internal.h"
#include "garb/garb.h"
#include "garb/values.h"

/* Inline declarations stay in the style attribute, so verbs, CSS and script
 * observe the same source. Edits replace matching declarations instead of
 * accumulating a new declaration on each mouse move. */
#define STYLE_MAX ((size_t)65536)
enum { S_TEXT = GARB_NPROPS, S_GET, S_SET, S_REMOVE, S_PRIORITY };

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
    int count = garb_read_declaration(out,&out->decl,dom->document->quirks == OS64_HTML_QUIRKS,sets);
    d_free(dom,sets);
    if (out->incomplete) {
        d_error(ctx,"QuotaExceededError","Inline style value quota exceeded");return -1;
    }
    return count > 0 || count == GARB_DECL_HELD;
}

static JSValue read(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node,
                     const char *name, bool priority)
{
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
    const os64_html_attr_t *attr = os64_html_attr(node,"style");
    const char *old = attr != NULL ? attr->value : "";
    size_t old_len = os64_strlen(old);
    if (old_len > STYLE_MAX || value_len > STYLE_MAX || old_len + value_len + name_len + 16 > STYLE_MAX)
        return d_error(ctx,"QuotaExceededError","Inline style text quota exceeded");
    char *text = d_alloc(dom,old_len + value_len + name_len + 16);
    if (text == NULL) return d_error(ctx,"QuotaExceededError","Inline style edit quota exceeded");
    size_t used = 0;
    for (size_t at = 0; at < old_len;) {
        size_t end = boundary(old,old_len,at,';');
        garb_parsed_t parsed = {0};
        int valid = parse(dom,ctx,old+at,end-at,&parsed);
        bool replace = valid > 0 && equal(parsed.decl.name,parsed.decl.len,name);
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
    int64_t status = os64_page_node_set_attr(dom->state,node,"style",text,used,false);
    d_free(dom,text);
    if (status != OS64_HTML_OK) return d_page_error(ctx,status);
    return budget(dom,ctx) ? JS_UNDEFINED : JS_EXCEPTION;
}

static JSValue member(JSContext *ctx, JSValueConst self, int argc,
                       JSValueConst *argv, int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx,data,OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DValue *entry = JS_GetOpaque(self,dom->style_class);
    if (entry == NULL) return JS_ThrowTypeError(ctx,"Expected an inline style object");
    if (!budget(dom,ctx)) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0; magic &= ~D_SET;
    const os64_html_node_t *node = entry->style_target;
    if (magic == S_TEXT) {
        if (!set) {
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
            os64_page_node_set_attr(dom->state,node,"style",value.data,value.length,false);
        d_string_free(dom,&value);
        if (status != OS64_HTML_OK) return d_page_error(ctx,status);
        return budget(dom,ctx) ? JS_UNDEFINED : JS_EXCEPTION;
    }
    DString name = {0}, value = {0}, priority = {0};
    const char *property = magic < GARB_NPROPS ? garb_prop_name((garb_prop_t)magic) : NULL;
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
    if (owner->style != NULL) return JS_DupValue(ctx,owner->style->value);
    JSValue object = JS_NewObjectClass(ctx,dom->style_class);
    DValue *entry = d_retain(dom,ctx,object);
    if (entry == NULL) return JS_EXCEPTION;
    entry->style_target = node;
    os64_html_hold(dom->document,node);
    JS_SetOpaque(object,entry);
    owner->style = entry;
    return JS_DupValue(ctx,object);
}

int d_style_install(os64_dom_t *dom, JSContext *ctx)
{
    JSValue prototype = JS_NewObject(ctx);
    dom->style_prototype = d_retain(dom,ctx,prototype);
    if (dom->style_prototype == NULL) return -1;
    JS_SetClassProto(ctx,dom->style_class,JS_DupValue(ctx,prototype));
    for (int i = 0; i < GARB_NPROPS; i++) {
        const char *name = garb_prop_name((garb_prop_t)i);
        char camel[128];size_t at = 0;
        for (size_t j = 0; name[j] && at+1 < sizeof(camel); j++) {
            if (name[j] == '-' && name[j+1]) camel[at++] = name[++j] - ('a'-'A');
            else camel[at++] = name[j];
        }
        camel[at] = '\0';
        if (d_accessor(dom,ctx,prototype,camel,member,i,true) < 0) return -1;
    }
    if (d_accessor(dom,ctx,prototype,"cssText",member,S_TEXT,true) < 0 ||
        d_method(dom,ctx,prototype,"getPropertyValue",member,1,S_GET) < 0 ||
        d_method(dom,ctx,prototype,"getPropertyPriority",member,1,S_PRIORITY) < 0 ||
        d_method(dom,ctx,prototype,"setProperty",member,2,S_SET) < 0) return -1;
    return d_method(dom,ctx,prototype,"removeProperty",member,1,S_REMOVE);
}
