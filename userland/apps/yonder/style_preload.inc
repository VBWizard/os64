// Document-owned style preloads. A staged or measured stylesheet table borrows
// this account; response bytes remain here until a live consumer completes.

static bool style_request_read(const os64_html_node_t *node, StyleRequest *r)
{
    *r = (StyleRequest){.mode = WAY_MODE_NO_CORS, .credentials = WAY_CREDENTIALS_INCLUDE};
    if (node == NULL)
        return true;
    const os64_html_attr_t *cors = os64_html_attr(node, "crossorigin");
    if (cors != NULL) {
        r->mode = WAY_MODE_CORS;
        r->credentials = os64_streq_nocase(cors->value, "use-credentials")
                         ? WAY_CREDENTIALS_INCLUDE : WAY_CREDENTIALS_SAME_ORIGIN;
    }
    static const char *const policies[] = {
        "strict-origin-when-cross-origin", "no-referrer", "no-referrer-when-downgrade",
        "same-origin", "origin", "strict-origin", "origin-when-cross-origin", "unsafe-url"
    };
    const os64_html_attr_t *policy = os64_html_attr(node, "referrerpolicy");
    for (size_t i = 0; policy != NULL && i < sizeof(policies) / sizeof(*policies); i++)
        if (os64_streq_nocase(policy->value, policies[i]))
            r->policy = (way_policy_t)i;
    const os64_html_attr_t *integrity = os64_html_attr(node, "integrity");
    if (integrity != NULL && integrity->value[0] != '\0') {
        r->integrity = copy_text(integrity->value);
        if (r->integrity == NULL)
            return false;
    }
    return true;
}

static bool style_request_same(const StyleRequest *a, const StyleRequest *b)
{
    return a->mode == b->mode && a->credentials == b->credentials && a->policy == b->policy &&
           os64_streq(a->integrity != NULL ? a->integrity : "", b->integrity != NULL ? b->integrity : "");
}

static void style_job_request(Page *p, const StyleRequest *r, const char *url, yonder_sheet_job_t *job)
{
    const os64_html_document_t *doc = page_doc(p);
    job->kind = YONDER_JOB_SHEET;
    job->agent = g.way.agent;
    os64_strcopy(job->url, sizeof(job->url), url);
    if (doc->charset != NULL)
        os64_strcopy(job->environment, sizeof(job->environment), doc->charset);
    job->quirks = doc->quirks == OS64_HTML_QUIRKS;
    way_origin_of(p->way.url, p->document, &job->request.origin);
    job->request.mode = r->mode;
    job->request.credentials = r->credentials;
    job->request.policy = r->policy;
    os64_strcopy(job->request.method, sizeof(job->request.method), "GET");
    job->hooks.jar = g.way.jar;
    job->hooks.cache = g.cache;
    os64_strcopy(job->hooks.referrer, sizeof(job->hooks.referrer), p->way.url);
    hooks_ask(&job->hooks, p->way.url, WAY_DEST_STYLE);
    job->keep = s_diag.keep;
}

static bool style_rel(const os64_html_node_t *node, const char *wanted)
{
    const os64_html_attr_t *rel = os64_html_attr(node, "rel");
    const char *at = rel != NULL ? rel->value : "";
    size_t len = os64_strlen(wanted);
    while (*at) {
        while (*at == ' ' || (*at >= '\t' && *at <= '\r')) at++;
        const char *start = at;
        while (*at && *at != ' ' && !(*at >= '\t' && *at <= '\r')) at++;
        if ((size_t)(at - start) != len) continue;
        bool same = true;
        for (size_t i = 0; same && i < len; i++) same = (start[i] | 0x20) == wanted[i];
        if (same) return true;
    }
    return false;
}

static bool preload_request_attribute(const char *name)
{
    static const char *const names[] = {
        "href", "rel", "as", "crossorigin", "referrerpolicy", "integrity", "media", "type"
    };
    for (size_t i = 0; name != NULL && i < sizeof(names) / sizeof(*names); i++)
        if (os64_streq(name, names[i])) return true;
    return false;
}

// Invalidate event generations using the document's existing mutation log.
// Retained responses remain available to a compatible stylesheet consumer.
static bool style_request_changed(const Preloads *map, const os64_html_node_t *node, uint64_t version,
                                   const os64_html_change_entry_t *changes, int64_t n)
{
    if (n < 0 || os64_html_shadow_including_root(node) != map->doc->document) return true;
    for (const os64_html_change_entry_t *c = changes; c != NULL; c = c->next) {
        if (c->version <= version) continue;
        if (c->kind == OS64_HTML_CHANGE_ATTR && c->attr_ns == NULL &&
            ((c->target == node && preload_request_attribute(c->attr_name)) ||
             (c->target->tag == OS64_HTML_TAG_BASE && os64_streq(c->attr_name, "href")))) return true;
        for (size_t k = 0; k < c->nremoved; k++)
            for (const os64_html_node_t *at = node; at != NULL; at = os64_html_shadow_including_parent(at))
                if (at == c->removed[k]) return true;
    }
    return false;
}

static void preloads_sync(Preloads *map)
{
    const os64_html_change_entry_t *changes = NULL;
    int64_t n = os64_html_changes(map->reader, &changes);
    for (int32_t i = 0; i < map->count; i++) {
        StylePreload *entry = &map->entries[i];
        if (entry->active && style_request_changed(map, entry->node, entry->version, changes, n))
            entry->active = false;
        if (entry->consumer_active &&
            style_request_changed(map, entry->consumer, entry->consumer_version, changes, n))
            entry->consumer_active = false;
    }
}

static bool preload_consumer_valid(void *ctx)
{
    StylePreload *entry = ctx;
    preloads_sync(entry->owner);
    return entry->consumer_active;
}

static bool preload_event_valid(void *ctx)
{
    StylePreload *entry = ctx;
    preloads_sync(entry->owner);
    return entry->active;
}

static void preloads_tell(Page *p)
{
    if (p->preloads == NULL || p->measuring) return;
    preloads_sync(p->preloads);
    yonder_scripts_t *host = page_scripts(p);
    if (host == NULL) host = scripts_for(p->serial);
    for (int32_t i = 0; i < p->preloads->count; i++) {
        StylePreload *entry = &p->preloads->entries[i];
        if (entry->active && entry->settled && !entry->told)
            entry->told = yonder_scripts_owe_guarded(host, entry->node,
                           entry->response == NULL || !entry->response->fetched, preload_event_valid, entry);
    }
}

static void preload_refused(Page *p, const char *why)
{
    os64_debug_log(why);
    yonder_diag_fact(p == &g.stream.page ? g.stream.diag : p->diag, "style preload", why);
}

static void preloads_free(Page *p)
{
    Preloads *map = p->preloads;
    if (map == NULL) return;
    for (int32_t i = 0; i < map->count; i++) {
        StylePreload *entry = &map->entries[i];
        if (entry->id != 0 && g.pool != NULL) os64_work_cancel(g.pool, entry->id);
        yonder_sheet_release(NULL, entry->response);
        os64_html_release(map->doc, entry->node);
        if (entry->consumer != NULL) os64_html_release(map->doc, entry->consumer);
        os64_free(entry->url);
        os64_free(entry->request.integrity);
    }
    os64_html_reader_free(map->reader);
    os64_free(map);
    p->preloads = NULL;
}

static Preloads *preloads_get(Page *p)
{
    Page *owner = p->reuse_sheets != NULL ? p->reuse_sheets : p;
    if (owner->preloads == NULL) {
        Preloads *map = os64_calloc(1, sizeof(*map));
        if (map == NULL) return NULL;
        map->doc = page_doc(p);
        map->document = ++g.pages_made;
        map->reader = os64_html_reader_new((os64_html_document_t *)map->doc, os64_html_version(map->doc));
        if (map->reader == NULL) { os64_free(map); return NULL; }
        owner->preloads = map;
    }
    return p->preloads = owner->preloads;
}

static void preloads_start(Page *p)
{
    if (p->measuring || !p->modern || page_doc(p) == NULL) return;
    if (p->preloads != NULL) preloads_sync(p->preloads);
    const os64_html_document_t *doc = page_doc(p);
    garb_env_t view = css_view(g.view.bounds.w, g.view.bounds.h, g.zoom);
    view.scripting = p->scripting;
    for (const os64_html_node_t *node = doc->document; node != NULL;
         node = os64_html_shadow_including_next(doc->document, node)) {
        if (node->kind != OS64_HTML_ELEMENT || node->ns != OS64_HTML_NS_HTML ||
            node->tag != OS64_HTML_TAG_LINK || !style_rel(node, "preload")) continue;
        const os64_html_attr_t *as = os64_html_attr(node, "as"), *href = os64_html_attr(node, "href");
        if (as == NULL || !os64_streq_nocase(as->value, "style") || href == NULL || !href->value[0]) continue;
        const os64_html_attr_t *media = os64_html_attr(node, "media"), *type = os64_html_attr(node, "type");
        if (media != NULL && !garb_media_text_matches(media->value, view)) continue;
        if (type != NULL && type->value[0]) {
            char mime[WAY_MIME_ESSENCE_MAX], charset[WAY_MIME_CHARSET_MAX];
            if (!way_mime_parse(type->value, os64_strlen(type->value), mime, sizeof(mime), charset, sizeof(charset)) ||
                !os64_streq(mime, "text/css")) continue;
        }
        Preloads *map = preloads_get(p);
        if (map == NULL) { preload_refused(p, "style preload: no memory for document state"); break; }
        bool known = false;
        for (int32_t i = 0; i < map->count; i++)
            known |= map->entries[i].node == node && map->entries[i].active;
        if (known) continue;
        if (map->count == STYLE_PRELOAD_MAX) {
            if (!map->refused) preload_refused(p, "style preload: document entry limit reached");
            map->refused = true;
            break;
        }
        char base[OS64_FETCH_URL_MAX];
        if (!os64_page_base_in(doc, p->way.url, base, sizeof(base))) continue;
        char resolved[OS64_FETCH_URL_MAX];
        if (!os64_page_url_absolute(base, href->value, resolved, sizeof(resolved))) continue;
        StylePreload *entry = &map->entries[map->count];
        if (!style_request_read(node, &entry->request) || (entry->url = copy_text(resolved)) == NULL) {
            os64_free(entry->request.integrity);
            os64_memset(entry, 0, sizeof(*entry));
            preload_refused(p, "style preload: no memory for request identity");
            break;
        }
        entry->owner = map;
        entry->node = node;
        os64_html_hold(doc, node);
        entry->version = os64_html_version(doc);
        entry->active = true;
        int32_t index = map->count++;
        yonder_sheet_job_t *job = g.pool != NULL ? os64_calloc(1, sizeof(*job)) : NULL;
        if (job != NULL) {
            style_job_request(p, &entry->request, resolved, job);
            job->preload = true;
            job->page = map->document;
            job->index = index;
            os64_work_t work = {yonder_sheet_run, yonder_sheet_release, job, SHEET_RESERVE};
            entry->id = os64_work_submit(g.pool, &work);
            if (entry->id == 0) yonder_sheet_release(job, NULL);
        }
        if (entry->id == 0) {
            entry->settled = true;
            preload_refused(p, "style preload: request could not be submitted");
        }
    }
    preloads_tell(p);
}

static void sheet_owe(Page *p, Sheet *sh, bool failed);

static void preload_parse(Page *p, Sheet *sh, StylePreload *entry)
{
    if (sh->id != 0 || !entry->settled) return;
    yonder_sheet_job_t *job = g.pool != NULL ? os64_calloc(1, sizeof(*job)) : NULL;
    if (job == NULL) goto refused;
    const yonder_sheet_t *response = entry->response;
    style_job_request(p, &sh->request, response != NULL ? response->url : entry->url, job);
    job->page = p->serial;
    job->index = (int32_t)(sh - p->sheets);
    job->input_present = true;
    if (response != NULL) {
        job->input_fetched = response->fetched;
        job->input_usable = response->usable;
        job->input_len = response->kept_len;
        os64_strcopy(job->input_label, sizeof(job->input_label), response->label);
        job->input = os64_malloc(job->input_len != 0 ? job->input_len : 1);
        if (job->input == NULL) { yonder_sheet_release(job, NULL); goto refused; }
        os64_memcpy(job->input, response->kept, job->input_len);
    }
    os64_work_t work = {yonder_sheet_run, yonder_sheet_release, job, SHEET_RESERVE};
    sh->id = os64_work_submit(g.pool, &work);
    if (sh->id != 0) return;
    yonder_sheet_release(job, NULL);
refused:
    sh->waiting = false;
    if (sh->holds) p->sheets_waiting--;
    sheet_owe(p, sh, true);
}

static bool preload_join(Page *p, Sheet *sh, const char *url)
{
    if (p->preloads == NULL) return false;
    for (int32_t i = p->preloads->count - 1; i >= 0; i--) {
        StylePreload *entry = &p->preloads->entries[i];
        if (entry->consumed || !os64_streq(entry->url, url) ||
            entry->request.mode != sh->request.mode || entry->request.credentials != sh->request.credentials ||
            entry->request.policy != sh->request.policy ||
            (sh->request.integrity != NULL &&
             !os64_streq(entry->request.integrity != NULL ? entry->request.integrity : "", sh->request.integrity)) ||
            (entry->consumer != NULL && (!entry->consumer_active || entry->consumer != sh->node))) continue;
        if (entry->consumer == NULL && sh->node != NULL) {
            entry->consumer = sh->node;
            entry->consumer_active = true;
            entry->consumer_version = os64_html_version(p->preloads->doc);
            os64_html_hold(p->preloads->doc, sh->node);
        }
        // Imports have no element identity across restages, so they cannot
        // claim an entry whose pending consumer must survive those restages.
        if (sh->node == NULL) continue;
        sh->preload = i + 1;
        sh->waiting = true;
        if (sh->holds) p->sheets_waiting++;
        preload_parse(p, sh, entry);
        return true;
    }
    return false;
}

static void coming_show(void);

static void preload_arrived(const yonder_sheet_job_t *job, yonder_sheet_t *got)
{
    Page *p = g.coming.active && g.coming.page.preloads != NULL &&
              g.coming.page.preloads->document == job->page ? &g.coming.page
            : g.page.preloads != NULL && g.page.preloads->document == job->page ? &g.page
            : g.stream.active && g.stream.page.preloads != NULL &&
              g.stream.page.preloads->document == job->page ? &g.stream.page : NULL;
    if (p == NULL || p->preloads == NULL || job->index < 0 || job->index >= p->preloads->count) return;
    Preloads *map = p->preloads;
    StylePreload *entry = &map->entries[job->index];
    if (entry->settled) return;
    entry->id = 0;
    entry->settled = true;
    if (got != NULL && got->kept_len <= STYLE_PRELOAD_BYTES - map->bytes) {
        entry->response = os64_malloc(sizeof(*entry->response));
        if (entry->response != NULL) {
            *entry->response = *got;
            map->bytes += got->kept_len;
            got->kept = NULL;
            got->kept_len = 0;
            os64_memset(&got->parsed, 0, sizeof(got->parsed));
        }
    }
    if (entry->response == NULL) preload_refused(p, "style preload: response retention failed or exceeded document limit");
    preloads_tell(p);
    for (int32_t i = 0; i < p->nsheets; i++)
        if (p->sheets[i].preload == job->index + 1 && p->sheets[i].waiting)
            preload_parse(p, &p->sheets[i], entry);
    if (p == &g.coming.page && p->sheets_waiting == 0) coming_show();
}

static void preload_consumed(Page *p, Sheet *sh)
{
    if (sh->preload <= 0 || p->preloads == NULL) return;
    StylePreload *entry = &p->preloads->entries[sh->preload - 1];
    entry->consumed = true;
    // Keep the acquisition verdict for an owed event after releasing its body.
    if (entry->response != NULL) {
        p->preloads->bytes -= entry->response->kept_len;
        os64_free(entry->response->kept);
        entry->response->kept = NULL;
        entry->response->kept_len = 0;
    }
}
