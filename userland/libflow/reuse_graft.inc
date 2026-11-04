// New arena records and retained runs stay private until the subtree copy
// succeeds. A failed copy consumes budget and follows the normal refusal path.
typedef struct {
    FBox *box;
    int64_t x, y, w, h, baseline, content_h, clearance, origin_x, origin_y;
    int64_t border[4], padding[4];
    bool has_baseline;
    FLine *lines, *last_line;
    FFrag *marker;
    os64_text_run_t *ellipsis;
    FReuseCall call;
} FGraftBox;

static int64_t graft_add(L *l, int64_t value, int64_t delta)
{
    int64_t result;
    if (__builtin_add_overflow(value, delta, &result)) {
        l->reuse_stats->refused[FLOW_REUSE_TRANSLATION]++;
        fail(l); return 0;
    }
    return result;
}

static bool graft_run(L *l, os64_text_run_t *run)
{
    if (!run) return true;
    if (os64_text_run_retain(run) != OS64_FONT_OK) {
        l->reuse_stats->refused[FLOW_REUSE_RETAIN]++;
        fail(l); return false;
    }
    return keep_run(l, run); // Consumes the new reference, including on failure.
}

static FFrag *graft_fragment(L *l, const FStyles *styles, FBox *box,
                              const FFrag *old, int64_t dx, int64_t dy)
{
    FFrag *fr = alloc(l, sizeof(*fr));
    if (!fr) return NULL;
    *fr = *old; fr->next = NULL;
    fr->item = old->item ? f_reuse_pointer(l->reuse, old->item) : NULL;
    if (old->item && !fr->item) { fail(l); return NULL; }
    fr->style = fr->item ? fr->item->style : box->style;
    fr->text = fr->item ? fr->item->text : box->marker;
    Paint paint = paint_for(l, styles, fr->node,
                           fr->kind == FF_ATOMIC ? fr->node : NULL);
    fr->link = fr->kind == FF_ATOMIC && fr->item->link >= 0 ? fr->item->link : paint.link;
    fr->x = graft_add(l, old->x, dx); fr->y = graft_add(l, old->y, dy);
    fr->baseline = graft_add(l, old->baseline, dy);
    if (l->failed || !graft_run(l, fr->run)) return NULL;
    return fr;
}

static void graft_box(L *l, const FStyles *styles, FBox *box, FGraftBox *staged,
                       size_t *count, int64_t dx, int64_t dy)
{
    FReusePair *pair = f_reuse_pair(l->reuse, box);
    if (!pair || !pair->equal || !pair->ordinary) { fail(l); return; }
    const FBox *old = pair->old;
    FGraftBox *s = &staged[(*count)++];
    *s = (FGraftBox){.box = box, .w = old->w, .h = old->h,
        .content_h = old->content_h, .clearance = old->clearance,
        .has_baseline = old->has_baseline, .ellipsis = old->ellipsis, .call = *old->reuse_call};
    s->call.box = box;
    s->x = graft_add(l, old->x, dx); s->y = graft_add(l, old->y, dy);
    s->baseline = old->has_baseline ? graft_add(l, old->baseline, dy) : old->baseline;
    s->origin_x = graft_add(l, old->origin_x, dx); s->origin_y = graft_add(l, old->origin_y, dy);
    os64_memcpy(s->border, old->border, sizeof(s->border));
    os64_memcpy(s->padding, old->padding, sizeof(s->padding));
    if (l->failed || !graft_run(l, s->ellipsis)) return;
    for (const FLine *ln = old->lines; ln && !l->failed; ln = ln->next) {
        FLine *copy = alloc(l, sizeof(*copy));
        if (!copy) return;
        copy->x = graft_add(l, ln->x, dx); copy->y = graft_add(l, ln->y, dy);
        copy->baseline = graft_add(l, ln->baseline, dy);
        copy->w = ln->w; copy->h = ln->h;
        for (const FFrag *fr = ln->frags; fr && !l->failed; fr = fr->next) {
            FFrag *made = graft_fragment(l, styles, box, fr, dx, dy);
            if (!made) return;
            if (copy->last_frag) copy->last_frag->next = made; else copy->frags = made;
            copy->last_frag = made;
        }
        for (const FSpan *sp = ln->spans; sp && !l->failed; sp = sp->next) {
            FSpan *made = alloc(l, sizeof(*made));
            if (!made) return;
            *made = *sp; made->next = NULL;
            made->inl = f_reuse_pointer(l->reuse, sp->inl);
            if (!made->inl) { fail(l); return; }
            made->x0 = graft_add(l, sp->x0, dx); made->x1 = graft_add(l, sp->x1, dx);
            made->top = graft_add(l, sp->top, dy); made->bottom = graft_add(l, sp->bottom, dy);
            if (copy->last_span) copy->last_span->next = made; else copy->spans = made;
            copy->last_span = made;
        }
        if (s->last_line) s->last_line->next = copy; else s->lines = copy;
        s->last_line = copy;
    }
    if (old->marker_frag && !l->failed)
        s->marker = graft_fragment(l, styles, box, old->marker_frag, dx, dy);
    for (FBox *c = box->first; c && !l->failed; c = c->next)
        graft_box(l, styles, c, staged, count, dx, dy);
}

static bool graft_try(L *l, const FStyles *styles, FBox *b, int64_t cbx,
                       int64_t cbw, Cursor *cur, GraftProbe *probe)
{
    if (l->failed) return false;
    FReusePair *p = f_reuse_pair(l->reuse, b);
    if (!p || !p->equal || !p->ordinary || sized_later(b)) return false;
    flow_reuse_reason_t reason = FLOW_REUSE_CALL_INPUTS;
    const FReuseCall *old = p->old->reuse_call, *fresh = b->reuse_call;
    if (!old || !old->valid || old->cbw != cbw || old->cbh != fresh->cbh ||
        old->forced_w != fresh->forced_w || old->pm_pos != cur->pm.pos || old->pm_neg != cur->pm.neg ||
        old->first_set != cursor_placed(cur) || old->bfc != fresh->bfc ||
        old->parent_align != fresh->parent_align) goto miss;
    reason = FLOW_REUSE_MARKER;
    if (probe->outside_marker) goto miss;
    int64_t query_y = probe->origin;
    reason = FLOW_REUSE_TRANSLATION;
    if (old->has_queries && __builtin_add_overflow(probe->origin, old->float_min, &query_y)) goto miss;
    reason = FLOW_REUSE_FLOATS;
    if (!old->float_free || probe->pending || b->clearance ||
        (old->has_queries && query_y < probe->bottom)) goto miss;
    int64_t dx, dy, next_y, first = cur->first;
    reason = FLOW_REUSE_TRANSLATION;
    if (__builtin_sub_overflow(cbx, p->old->origin_x, &dx) ||
        __builtin_sub_overflow(cur->y, p->old->origin_y, &dy) ||
        __builtin_add_overflow(cur->y, old->delta_y, &next_y) ||
        (!old->first_set && old->result_first_set &&
         __builtin_add_overflow(cur->y, old->first_delta, &first))) goto miss;
    if (p->subtree_count > l->reuse->staging_left / sizeof(FGraftBox)) {
        l->reuse_stats->refused[FLOW_REUSE_BUDGET]++; fail(l); b->unfinished = true; return false;
    }
    size_t peak = l->reuse_stats->metadata_bytes + l->reuse->bytes + p->subtree_count * sizeof(FGraftBox);
    if (peak > l->reuse_stats->metadata_peak) l->reuse_stats->metadata_peak = peak;
    FGraftBox *staged = os64_calloc(p->subtree_count, sizeof(*staged));
    if (!staged) { l->reuse_stats->refused[FLOW_REUSE_NO_MEMORY]++; fail(l); b->unfinished = true; return false; }
    size_t count = 0;
    graft_box(l, styles, b, staged, &count, dx, dy);
    if (!l->failed) {
        for (size_t i = 0; i < count; ++i) {
            FGraftBox *s = &staged[i]; FBox *d = s->box;
            d->x = s->x; d->y = s->y; d->w = s->w; d->h = s->h;
            d->baseline = s->baseline; d->has_baseline = s->has_baseline;
            d->content_h = s->content_h; d->clearance = s->clearance;
            d->origin_x = s->origin_x; d->origin_y = s->origin_y;
            os64_memcpy(d->border, s->border, sizeof(d->border));
            os64_memcpy(d->padding, s->padding, sizeof(d->padding));
            d->lines = s->lines; d->last_line = s->last_line; d->marker_frag = s->marker;
            d->ellipsis = s->ellipsis; *d->reuse_call = s->call;
            d->placed = true; d->unfinished = false; d->laid++;
        }
        cur->y = next_y; cur->pm = (Margins){old->out_pos, old->out_neg}; cur->first = first;
        if (old->has_queries) {
            probe->minimum = probe->queries ? min64(probe->minimum, query_y) : query_y;
            probe->queries = true;
        }
        l->out->reused += count;
    } else b->unfinished = true;
    os64_free(staged);
    return !l->failed;
miss:
    l->reuse_stats->refused[reason]++;
    return false;
}
