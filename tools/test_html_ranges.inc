/* LIVE RANGES (docs/design/pending/RANGE.md § 3, § 4) against a reference
 * model. The model mirrors the tree as child arrays and runs every mutation
 * as the DOM Standard writes it — replace is "adopt, remove the old child,
 * insert", a fragment's insertion removes each child then inserts them —
 * with each live range's steps applied at the model's own indexes, which it
 * reads off its arrays. A random walk applies each operation to libhtml and
 * to the model and compares every boundary, and the tree's shape, after
 * every step; text carries characters past the BMP, so UTF-16 offsets and
 * the snapping of a surrogate pair are exercised too. */

typedef struct MN {
    HNode *real;
    struct MN *parent;
    struct MN **kids;
    int nk, capk;
    char *text;
    size_t tlen;
    int kind;
} MN;

#define RR_NODES 4096
#define RR_RANGES_MAX 1000
static MN *rr_all[RR_NODES];
static int rr_n;
static struct { MN *node; uint32_t off; } rr_b[RR_RANGES_MAX][2];
static os64_html_range_t *rr_r[RR_RANGES_MAX];
static int rr_k;
static uint32_t rr_seed;
static os64_html_document_t *rr_doc;
/* How many of each kind of step changed the tree: the walk's coverage. */
static unsigned rr_did[9];

static uint32_t rr_rand(void)
{
    rr_seed ^= rr_seed << 13;
    rr_seed ^= rr_seed >> 17;
    rr_seed ^= rr_seed << 5;
    return rr_seed;
}

static MN *rr_find(const HNode *real)
{
    for (int i = 0; i < rr_n; i++)
        if (rr_all[i]->real == real)
            return rr_all[i];
    return NULL;
}

static MN *rr_new(HNode *real)
{
    MN *m = calloc(1, sizeof(*m));
    m->real = real;
    /* Held for the walk: a removed subtree nobody holds is reclaimed, and
     * the model must be able to hand any node it knows back to libhtml. */
    os64_html_hold(rr_doc, real);
    m->kind = real->kind;
    if (real->text_len) {
        m->text = malloc(real->text_len);
        memcpy(m->text, real->text, real->text_len);
    }
    m->tlen = real->text_len;
    rr_all[rr_n++] = m;
    return m;
}

static void rr_link(MN *p, MN *c, int at)
{
    if (p->nk == p->capk) {
        p->capk = p->capk ? p->capk * 2 : 4;
        p->kids = realloc(p->kids, (size_t)p->capk * sizeof(*p->kids));
    }
    memmove(p->kids + at + 1, p->kids + at, (size_t)(p->nk - at) * sizeof(*p->kids));
    p->kids[at] = c;
    p->nk++;
    c->parent = p;
}

static MN *rr_import(HNode *real)
{
    MN *m = rr_new(real);
    for (HNode *c = real->first_child; c; c = c->next)
        rr_link(m, rr_import(c), m->nk);
    return m;
}

static int rr_index(const MN *n)
{
    for (int i = 0; i < n->parent->nk; i++)
        if (n->parent->kids[i] == n)
            return i;
    return -1;
}

/* UTF-16 units of `n` bytes of UTF-8, written here apart from libhtml's. */
static uint32_t rr_units(const char *s, size_t n)
{
    uint32_t u = 0;
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        size_t w = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        u += w == 4 ? 2 : 1;
        i += w;
    }
    return u;
}

/* The byte where unit `unit` falls, snapped down to its character's start. */
static size_t rr_byte(const char *s, size_t n, uint32_t unit)
{
    uint32_t u = 0;
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        size_t w = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        uint32_t cu = w == 4 ? 2 : 1;
        if (u + cu > unit)
            return i;
        u += cu;
        i += w;
    }
    return n;
}

static bool rr_data(const MN *n)
{
    return n->kind == OS64_HTML_TEXT || n->kind == OS64_HTML_COMMENT || n->kind == OS64_HTML_PI;
}

static uint32_t rr_length(const MN *n)
{
    if (n->kind == OS64_HTML_DOCTYPE)
        return 0;
    return rr_data(n) ? rr_units(n->text, n->tlen) : (uint32_t)n->nk;
}

static bool rr_within(const MN *n, const MN *of)
{
    for (; n; n = n->parent)
        if (n == of)
            return true;
    return false;
}

/* DOM § 4.2.3, remove: the live range steps, then the unlink. */
static void rr_remove(MN *n)
{
    MN *p = n->parent;
    if (!p)
        return;
    int i = rr_index(n);
    for (int r = 0; r < rr_k; r++)
        for (int e = 0; e < 2; e++) {
            if (rr_within(rr_b[r][e].node, n)) {
                rr_b[r][e].node = p;
                rr_b[r][e].off = (uint32_t)i;
            } else if (rr_b[r][e].node == p && rr_b[r][e].off > (uint32_t)i) {
                rr_b[r][e].off--;
            }
        }
    memmove(p->kids + i, p->kids + i + 1, (size_t)(p->nk - i - 1) * sizeof(*p->kids));
    p->nk--;
    n->parent = NULL;
}

/* DOM § 4.2.3, insert one node before `before` (NULL appends). */
static void rr_insert_one(MN *n, MN *p, MN *before)
{
    rr_remove(n);
    int at = before ? rr_index(before) : p->nk;
    if (before)
        for (int r = 0; r < rr_k; r++)
            for (int e = 0; e < 2; e++)
                if (rr_b[r][e].node == p && rr_b[r][e].off > (uint32_t)at)
                    rr_b[r][e].off++;
    rr_link(p, n, at);
}

static void rr_insert(MN *n, MN *p, MN *before)
{
    /* DOM's pre-insert: a node inserted before itself goes before its next
     * sibling. */
    if (before == n) {
        int i = rr_index(n);
        before = i + 1 < p->nk ? p->kids[i + 1] : NULL;
    }
    if (n->kind != OS64_HTML_FRAGMENT) {
        rr_insert_one(n, p, before);
        return;
    }
    int k = n->nk;
    MN **kids = malloc((size_t)(k ? k : 1) * sizeof(*kids));
    if (k)
        memcpy(kids, n->kids, (size_t)k * sizeof(*kids));
    for (int i = 0; i < k; i++)
        rr_remove(kids[i]);
    for (int i = 0; i < k; i++)
        rr_insert_one(kids[i], p, before);
    free(kids);
}

/* DOM § 4.2.3, replace a child: the reference child, adopt, remove, insert. */
static void rr_replace(MN *p, MN *node, MN *old)
{
    int i = rr_index(old);
    MN *ref = i + 1 < p->nk ? p->kids[i + 1] : NULL;
    if (ref == node) {
        int j = rr_index(node);
        ref = j + 1 < p->nk ? p->kids[j + 1] : NULL;
    }
    if (node->kind == OS64_HTML_FRAGMENT) {
        if (old->parent)
            rr_remove(old);
        rr_insert(node, p, ref);
        return;
    }
    rr_remove(node);
    if (old->parent)
        rr_remove(old);
    rr_insert_one(node, p, ref);
}

/* DOM § 4.10, replace data, offsets snapped as libhtml snaps them. */
static void rr_replace_data(MN *n, uint32_t off, uint32_t count, const char *s, size_t len)
{
    uint32_t units = rr_units(n->text, n->tlen);
    if (count > units - off)
        count = units - off;
    size_t from = rr_byte(n->text, n->tlen, off), to = rr_byte(n->text, n->tlen, off + count);
    off = rr_units(n->text, from);
    count = rr_units(n->text + from, to - from);
    uint32_t added = rr_units(s, len);
    for (int r = 0; r < rr_k; r++)
        for (int e = 0; e < 2; e++)
            if (rr_b[r][e].node == n && rr_b[r][e].off > off) {
                if (rr_b[r][e].off <= off + count)
                    rr_b[r][e].off = off;
                else
                    rr_b[r][e].off = rr_b[r][e].off - count + added;
            }
    char *t = malloc(n->tlen - (to - from) + len + 1);
    if (from)
        memcpy(t, n->text, from);
    if (len)
        memcpy(t + from, s, len);
    if (n->tlen - to)
        memcpy(t + from + len, n->text + to, n->tlen - to);
    n->tlen = n->tlen - (to - from) + len;
    free(n->text);
    n->text = t;
}

/* DOM § 4.11, split a Text node; `tail` is what libhtml made. */
static void rr_split(MN *n, uint32_t off, HNode *tail_real)
{
    size_t from = rr_byte(n->text, n->tlen, off);
    off = rr_units(n->text, from);
    MN *tail = rr_new(tail_real);
    if (n->parent) {
        MN *p = n->parent;
        int i = rr_index(n);
        rr_insert_one(tail, p, i + 1 < p->nk ? p->kids[i + 1] : NULL);
        for (int r = 0; r < rr_k; r++)
            for (int e = 0; e < 2; e++) {
                if (rr_b[r][e].node == n && rr_b[r][e].off > off) {
                    rr_b[r][e].node = tail;
                    rr_b[r][e].off -= off;
                } else if (rr_b[r][e].node == p && rr_b[r][e].off == (uint32_t)i + 1) {
                    rr_b[r][e].off++;
                }
            }
    }
    rr_replace_data(n, off, rr_units(n->text, n->tlen) - off, "", 0);
}

/* DOM § 4.4, normalize, as the standard writes it. */
static MN *rr_next(MN *x, MN *root, bool kids)
{
    if (kids && x->nk)
        return x->kids[0];
    while (x != root) {
        int i = rr_index(x);
        if (i + 1 < x->parent->nk)
            return x->parent->kids[i + 1];
        x = x->parent;
    }
    return NULL;
}

static void rr_normalize(MN *node)
{
    for (MN *x = node->nk ? node->kids[0] : NULL; x;) {
        if (x->kind != OS64_HTML_TEXT) {
            x = rr_next(x, node, true);
            continue;
        }
        uint32_t length = rr_units(x->text, x->tlen);
        if (length == 0) {
            MN *next = rr_next(x, node, false);
            rr_remove(x);
            x = next;
            continue;
        }
        int i = rr_index(x);
        MN *p = x->parent;
        size_t more = 0;
        for (int j = i + 1; j < p->nk && p->kids[j]->kind == OS64_HTML_TEXT; j++)
            more += p->kids[j]->tlen;
        if (i + 1 < p->nk && p->kids[i + 1]->kind == OS64_HTML_TEXT) {
            char *data = malloc(more ? more : 1);
            size_t at = 0;
            for (int j = i + 1; j < p->nk && p->kids[j]->kind == OS64_HTML_TEXT; j++) {
                if (p->kids[j]->tlen)
                    memcpy(data + at, p->kids[j]->text, p->kids[j]->tlen);
                at += p->kids[j]->tlen;
            }
            rr_replace_data(x, length, 0, data, more);
            free(data);
            for (int j = i + 1; j < p->nk && p->kids[j]->kind == OS64_HTML_TEXT; j++) {
                MN *cur = p->kids[j];
                for (int r = 0; r < rr_k; r++)
                    for (int e = 0; e < 2; e++) {
                        if (rr_b[r][e].node == cur) {
                            rr_b[r][e].node = x;
                            rr_b[r][e].off += length;
                        } else if (rr_b[r][e].node == p && rr_b[r][e].off == (uint32_t)j) {
                            rr_b[r][e].node = x;
                            rr_b[r][e].off = length;
                        }
                    }
                length += rr_units(cur->text, cur->tlen);
            }
            while (i + 1 < p->nk && p->kids[i + 1]->kind == OS64_HTML_TEXT)
                rr_remove(p->kids[i + 1]);
        }
        x = rr_next(x, node, false);
    }
}

/* Every boundary libhtml holds equals the model's, and so does every model
 * node's parent and place. */
static bool rr_compare(const char *what, unsigned step)
{
    for (int r = 0; r < rr_k; r++)
        for (int e = 0; e < 2; e++) {
            const os64_html_node_t *node;
            uint32_t off;
            os64_html_range_get(rr_r[r], e, &node, &off);
            if (node != rr_b[r][e].node->real || off != rr_b[r][e].off) {
                check(false, "ranges: step %u (%s): range %d's %s is (%p, %u), the model says (%p, %u)", step,
                      what, r, e ? "end" : "start", (const void *)node, off,
                      (const void *)rr_b[r][e].node->real, rr_b[r][e].off);
                return false;
            }
        }
    for (int i = 0; i < rr_n; i++) {
        MN *m = rr_all[i];
        const HNode *want = m->parent ? m->parent->real : NULL;
        if (m->real->parent != want || (m->parent && m->parent->kids[rr_index(m)] != m)) {
            check(false, "ranges: step %u (%s): the model's tree parted from libhtml's at a kind-%d node "
                  "(libhtml's parent %p, kind %d; the model's %p)", step, what, m->kind,
                  (const void *)m->real->parent, m->real->parent ? (int)m->real->parent->kind : -1,
                  (const void *)want);
            return false;
        }
    }
    return true;
}

static const char *const rr_texts[] = {"", "a", "hello", "x\xF0\x9F\x98\x80y", "\xF0\x9F\x98\x80\xF0\x9F\x98\x80",
                                       "caf\xC3\xA9", "\xE6\x97\xA5\xE6\x9C\xAC"};

static MN *rr_pick(void)
{
    return rr_all[rr_rand() % (uint32_t)rr_n];
}

static void rr_place(int r, int e)
{
    for (int tries = 0; tries < 8; tries++) {
        MN *m = rr_pick();
        if (m->kind == OS64_HTML_DOCTYPE)
            continue;
        uint32_t len = rr_length(m), off = rr_rand() % (len + 1);
        if (os64_html_range_set(rr_r[r], e, m->real, off) != OS64_HTML_OK)
            continue;
        rr_b[r][e].node = m;
        rr_b[r][e].off = rr_data(m) ? rr_units(m->text, rr_byte(m->text, m->tlen, off)) : off;
        return;
    }
}

/* Runs `steps` random mutations over a page holding `ranges` live ranges;
 * answers mutations a second. */
static double t_ranges_walk(uint32_t seed, int ranges, unsigned steps)
{
    os64_html_document_t *doc =
        parse("<body><div id=a>one<b>two</b>three</div><p>\xF0\x9F\x98\x80</p><ul><li>x</li><li>y</li></ul></body>");
    rr_n = 0;
    rr_k = ranges;
    rr_seed = seed;
    rr_doc = doc;
    rr_import(doc->document);
    for (int i = 0; i < 12; i++) {
        int64_t why = 0;
        HNode *pool = (i % 3 == 0) ? os64_html_create_element(doc, OS64_HTML_NS_HTML, "span", &why)
                    : (i % 3 == 1) ? os64_html_create_text(doc, rr_texts[i % 7], strlen(rr_texts[i % 7]), &why)
                                   : os64_html_create_fragment(doc, &why);
        rr_new(pool);
    }
    for (int r = 0; r < ranges; r++) {
        rr_r[r] = os64_html_range_new(doc);
        rr_b[r][0].node = rr_b[r][1].node = rr_find(doc->document);
        rr_b[r][0].off = rr_b[r][1].off = 0;
        rr_place(r, 0);
        rr_place(r, 1);
    }
    bool ok = rr_compare("setup", 0);
    clock_t began = clock();
    unsigned done = 0;
    for (unsigned step = 1; ok && step <= steps; step++) {
        MN *a = rr_pick(), *b = rr_pick(), *c = rr_pick();
        int64_t why = OS64_HTML_OK;
        const char *what = "?";
        uint64_t version = os64_html_version(doc);
        unsigned kind = rr_rand() % 9;
        switch (kind) {
        case 0:
            what = "remove";
            if (a->parent && os64_html_remove(doc, a->real) == OS64_HTML_OK)
                rr_remove(a);
            break;
        case 1:
        case 2: {
            what = "insert";
            MN *before = b->nk && rr_rand() % 3 ? b->kids[rr_rand() % (uint32_t)b->nk] : NULL;
            if (os64_html_insert(doc, b->real, a->real, before ? before->real : NULL) == OS64_HTML_OK)
                rr_insert(a, b, before);
            break;
        }
        case 3:
            what = "replace";
            if (c->parent && os64_html_replace(doc, c->parent->real, a->real, c->real) == OS64_HTML_OK)
                rr_replace(c->parent, a, c);
            break;
        case 4: {
            what = "set_text";
            const char *t = rr_texts[rr_rand() % 7];
            if (rr_data(a) && os64_html_set_text(doc, a->real, t, strlen(t)) == OS64_HTML_OK)
                rr_replace_data(a, 0, rr_length(a), t, strlen(t));
            break;
        }
        case 5: {
            what = "replace_data";
            const char *t = rr_texts[rr_rand() % 7];
            if (!rr_data(a))
                break;
            uint32_t len = rr_length(a), off = rr_rand() % (len + 1), count = rr_rand() % (len + 2);
            if (os64_html_replace_data(doc, a->real, off, count, t, strlen(t)) == OS64_HTML_OK)
                rr_replace_data(a, off, count, t, strlen(t));
            break;
        }
        case 6: {
            what = "split_text";
            if (a->kind != OS64_HTML_TEXT || rr_n >= RR_NODES - 1)
                break;
            uint32_t off = rr_rand() % (rr_length(a) + 1);
            HNode *tail = NULL;
            if (os64_html_split_text(doc, a->real, off, &tail) == OS64_HTML_OK)
                rr_split(a, off, tail);
            break;
        }
        case 7:
            what = "normalize";
            if (os64_html_normalize(doc, a->real) == OS64_HTML_OK)
                rr_normalize(a);
            break;
        default:
            what = "set a range";
            if (ranges)
                rr_place((int)(rr_rand() % (uint32_t)ranges), (int)(rr_rand() % 2));
            break;
        }
        (void)why;
        done++;
        if (os64_html_version(doc) != version)
            rr_did[kind]++;
        ok = rr_compare(what, step);
    }
    double seconds = (double)(clock() - began) / CLOCKS_PER_SEC;
    for (int r = 0; r < ranges; r++)
        os64_html_range_free(rr_r[r]);
    for (int i = 0; i < rr_n; i++) {
        os64_html_release(doc, rr_all[i]->real);
        free(rr_all[i]->kids);
        free(rr_all[i]->text);
        free(rr_all[i]);
    }
    rr_n = 0;
    os64_html_document_free(doc);
    return seconds > 0 ? done / seconds : 0;
}

static void t_ranges(void)
{
    for (uint32_t seed = 1; seed <= 24 && !checks_failed; seed++)
        (void)t_ranges_walk(0x52a00000u + seed, (int)(seed % 6) + 1, 3000);
    check(!checks_failed, "ranges: every boundary follows the standard's steps across random mutation");
    printf("ranges walk: remove %u, insert %u, replace %u, set_text %u, replace_data %u, split %u, "
           "normalize %u\n", rr_did[0], rr_did[1] + rr_did[2], rr_did[3], rr_did[4], rr_did[5], rr_did[6],
           rr_did[7]);
    check(rr_did[0] && rr_did[1] + rr_did[2] && rr_did[3] && rr_did[4] && rr_did[5] && rr_did[6] && rr_did[7],
          "ranges: the walk made every kind of change");
}

/* libhtml alone: `ranges` live ranges in one subtree, and a node inserted
 * before a child of another subtree and removed again, `steps` times.
 * Answers mutations a second. */
static double t_ranges_mutations(int ranges, unsigned steps)
{
    os64_html_document_t *doc = parse("<body><div id=a><p>one</p><p>two</p><p>three</p></div>"
                                      "<ul id=b><li>x</li><li>y</li><li>z</li></ul></body>");
    HNode *a = id_in(doc, "a"), *b = id_in(doc, "b");
    os64_html_range_t **held = calloc((size_t)(ranges ? ranges : 1), sizeof(*held));
    for (int r = 0; r < ranges; r++) {
        held[r] = os64_html_range_new(doc);
        os64_html_range_set(held[r], 0, a, (uint32_t)(r % 3));
        os64_html_range_set(held[r], 1, a->last_child->first_child, (uint32_t)(r % 5));
    }
    int64_t why = 0;
    HNode *li = os64_html_create_element(doc, OS64_HTML_NS_HTML, "li", &why);
    os64_html_hold(doc, li);
    clock_t began = clock();
    for (unsigned i = 0; i < steps; i++) {
        os64_html_insert(doc, b, li, b->first_child->next);
        os64_html_remove(doc, li);
    }
    double seconds = (double)(clock() - began) / CLOCKS_PER_SEC;
    os64_html_release(doc, li);
    for (int r = 0; r < ranges; r++)
        os64_html_range_free(held[r]);
    free(held);
    os64_html_document_free(doc);
    return seconds > 0 ? 2.0 * steps / seconds : 0;
}

/* RANGE.md § 3: 1, 100 and 1,000 live ranges. */
static void t_ranges_cost(void)
{
    static const int counts[] = {0, 1, 100, 1000};
    for (size_t i = 0; i < 4; i++)
        printf("ranges cost: %d live ranges: libhtml %.0f mutations a second; with the reference model "
               "and its checks %.0f\n", counts[i], t_ranges_mutations(counts[i], 20000),
               counts[i] ? t_ranges_walk(0x52b00001u, counts[i], 2000) : 0.0);
}
