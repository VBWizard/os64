// A call observes queries in its inherited exclusion space. Inner BFCs have
// independent spaces; their queries do not constrain an enclosing replay.
typedef struct GraftProbe {
    struct GraftProbe *previous;
    Space *space;
    int64_t origin, minimum, bottom;
    bool queries, pending, outside_marker;
} GraftProbe;

static void graft_query(L *l, int64_t y)
{
    GraftProbe *p = l->graft;
    if (!p || p->space != l->space) return;
    int64_t raw = y - (l->space ? l->space->rel_y : 0);
    p->minimum = p->queries ? min64(p->minimum, raw) : raw;
    p->queries = true;
}

static void graft_enter(L *l, GraftProbe *p, FBox *b, int64_t x, int64_t w,
                         const Cursor *cur, int64_t forced, bool pending)
{
    Space *sp = l->space;
    *p = (GraftProbe){.previous = l->graft, .space = sp,
        .origin = cur->y - (sp ? sp->rel_y : 0),
        .bottom = sp && sp->any ? max64(sp->bottom[0], sp->bottom[1]) : INT64_MIN,
        .pending = pending, .outside_marker = l->nmarkers > l->marker_floor};
    b->origin_x = x; b->origin_y = cur->y;
    *b->reuse_call = (FReuseCall){.box = b, .cbw = w, .cbh = definite_height(b->parent),
        .forced_w = forced, .pm_pos = cur->pm.pos, .pm_neg = cur->pm.neg,
        .first_set = cursor_placed(cur), .bfc = f_bfc_root(b),
        .parent_align = b->parent ? b->parent->style->text_align : FLOW_ALIGN_LEFT};
    l->graft = p;
    // clearance ran before this replay boundary. Its no-op comparison is
    // still a query whose position a future float could influence.
    if (b->style->clear != FLOW_CLEAR_NONE)
        graft_query(l, cur->y + margins_sum(margins_join(cur->pm, margin_of(len(b->style->margin[FLOW_TOP], w)))));
}

static void graft_leave(L *l, GraftProbe *p, FBox *b, const Cursor *cur)
{
    FReuseCall *c = b->reuse_call;
    c->delta_y = cur->y - b->origin_y;
    c->out_pos = cur->pm.pos; c->out_neg = cur->pm.neg;
    c->result_first_set = cursor_placed(cur);
    c->first_delta = !c->first_set && c->result_first_set ? cur->first - b->origin_y : 0;
    c->has_queries = p->queries;
    c->float_min = p->queries ? p->minimum - p->origin : 0;
    c->float_free = !p->pending && !b->clearance && (!p->queries || p->minimum >= p->bottom);
    c->valid = b->placed && !b->unfinished && !l->failed && b->laid == 1 && !p->outside_marker;
    if (p->queries && p->previous && p->previous->space == p->space) {
        GraftProbe *up = p->previous;
        up->minimum = up->queries ? min64(up->minimum, p->minimum) : p->minimum;
        up->queries = true;
    }
}
