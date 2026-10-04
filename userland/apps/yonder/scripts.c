#include "scripts.h"
#include "os64/fmt.h"
#include "os64/mem.h"
#include "os64/str.h"

#define SCRIPT_COUNT_MAX 4096
#define SCRIPT_SOURCE_MAX ((size_t)4 * 1024 * 1024)

struct yonder_scripts {
    os64_html_document_t *doc;
    os64_page_state_t *state;
    os64_js_runtime_t *runtime;
    os64_dom_t *dom;
    void (*alert)(void *, const char *, size_t);
    void *opaque;
    char url[OS64_JS_SOURCE_NAME_CAP];
    size_t count, next;
    os64_html_node_t *nodes[];
};

static os64_html_node_t *after(os64_html_node_t *node)
{
    if (node->first_child != NULL)
        return node->first_child;
    while (node != NULL && node->next == NULL)
        node = node->parent;
    return node != NULL ? node->next : NULL;
}

static bool classic(const os64_html_node_t *n)
{
    if (n->kind != OS64_HTML_ELEMENT || n->ns != OS64_HTML_NS_HTML ||
        !os64_streq(n->name, "script") || os64_html_attr(n, "src") != NULL)
        return false;
    const os64_html_attr_t *type = os64_html_attr(n, "type");
    if (type == NULL || type->value == NULL || type->value[0] == '\0')
        return true;
    return os64_streq(type->value, "text/javascript") ||
           os64_streq(type->value, "application/javascript") ||
           os64_streq(type->value, "text/ecmascript") ||
           os64_streq(type->value, "application/ecmascript");
}

yonder_scripts_t *yonder_scripts_new(os64_html_document_t *doc,
                                     os64_page_state_t *state, const char *url,
                                     void (*alert)(void *, const char *, size_t),
                                     void *opaque)
{
    if (doc == NULL || state == NULL || os64_page_state_document(state) != doc)
        return NULL;
    size_t count = 0;
    for (os64_html_node_t *n = doc->document; n != NULL; n = after(n))
        if (classic(n) && ++count > SCRIPT_COUNT_MAX)
            return NULL;
    yonder_scripts_t *s = os64_calloc(1, sizeof(*s) + count * sizeof(*s->nodes));
    if (s == NULL)
        return NULL;
    s->doc = doc;
    s->state = state;
    s->alert = alert;
    s->opaque = opaque;
    os64_strcopy(s->url, sizeof(s->url), url);
    for (os64_html_node_t *n = doc->document; n != NULL; n = after(n))
        if (classic(n)) {
            os64_html_hold(doc, n);
            s->nodes[s->count++] = n;
        }
    return s;
}

bool yonder_scripts_pending(const yonder_scripts_t *s)
{
    return s != NULL && s->next < s->count;
}

static void retire(yonder_scripts_t *s)
{
    s->next = s->count;
    os64_dom_drain(s->dom);
    os64_js_destroy(s->runtime);
    os64_dom_free(s->dom);
    s->runtime = NULL;
    s->dom = NULL;
    // Queue identities remain valid even when a preceding script detaches
    // them. Teardown releases each queue hold after the engine is gone.
    for (size_t i = 0; i < s->count; i++)
        if (s->nodes[i] != NULL) {
            os64_html_release(s->doc, s->nodes[i]);
            s->nodes[i] = NULL;
        }
}

bool yonder_scripts_step(yonder_scripts_t *s, os64_js_outcome_t *out)
{
    if (out == NULL)
        return false;
    os64_memset(out, 0, sizeof(*out));
    os64_html_node_t *node = NULL;
    while (yonder_scripts_pending(s)) {
        node = s->nodes[s->next++];
        const os64_html_node_t *root = node;
        while (root->parent != NULL)
            root = root->parent;
        if (root == s->doc->document && classic(node))
            break;
        node = NULL;
    }
    if (node == NULL)
        return false;
    char name[OS64_JS_SOURCE_NAME_CAP];
    os64_snprintf(name, sizeof(name), "%.*s#inline-%lu", (int)(sizeof(name) - 32),
                  s->url, (unsigned long)s->next);
    if (s->runtime == NULL) {
        /* A fixture turn has a finite one-second deadline and 4096 jobs.
         * Stack/heap/source keep the measured libdom target profile. */
        os64_js_config_t config = {os64_js_default_limits()};
        config.limits.execution_ms = 1000;
        config.limits.jobs_per_turn = 4096;
        if (os64_js_create(&config, OS64_JS_ABI_ID, &s->runtime, out) != OS64_JS_OK)
            goto failed;
        os64_dom_options_t options = os64_dom_default_options();
        options.alert = s->alert;
        options.alert_opaque = s->opaque;
        s->dom = os64_dom_create(s->runtime, s->doc, s->state, &options, out);
        if (s->dom == NULL || os64_js_install_output(s->runtime, 1,
                                                   OS64_JS_OUTPUT_CONSOLE_LOG, out) != OS64_JS_OK)
            goto failed;
    }
    size_t length = 0;
    for (const os64_html_node_t *n = node->first_child; n != NULL; n = n->next)
        if (n->kind == OS64_HTML_TEXT) {
            if (n->text_len > SCRIPT_SOURCE_MAX - length) {
                out->status = OS64_JS_LIMIT;
                out->limit = OS64_JS_LIMIT_SOURCE;
                os64_strcopy(out->message, sizeof(out->message), "Inline script exceeds the source limit.");
                goto failed;
            }
            length += n->text_len;
        }
    char *source = os64_malloc(length + 1);
    if (source == NULL) {
        out->status = OS64_JS_HOST_FAILURE;
        os64_strcopy(out->message, sizeof(out->message), "No memory for the inline script.");
        goto failed;
    }
    size_t at = 0;
    for (const os64_html_node_t *n = node->first_child; n != NULL; n = n->next)
        if (n->kind == OS64_HTML_TEXT) {
            os64_memcpy(source + at, n->text, n->text_len);
            at += n->text_len;
        }
    source[length] = '\0';
    os64_js_status_t status = os64_js_run(s->runtime, source, length, name, out);
    os64_free(source);
    if (status == OS64_JS_OK || status == OS64_JS_EXCEPTION || status == OS64_JS_UNHANDLED_REJECTION)
        return true;
failed:
    os64_strcopy(out->source_name, sizeof(out->source_name), name);
    retire(s);
    return true;
}

void yonder_scripts_free(yonder_scripts_t *s)
{
    if (s == NULL)
        return;
    retire(s);
    os64_free(s);
}
