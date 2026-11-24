// cascade.c — CSS Cascading and Inheritance Level 4 for the author origin,
// with Custom Properties 1 and Environment Variables 1 (garb/cascade.h).
//
// Build: every style rule of every sheet whose @media holds, its selectors
// filed in a RULE HASH by the rightmost compound's id, class or element
// name (the browsers' own trick: an element tries only the rules that could
// possibly match it), its declarations read once; every @keyframes, the
// last of each name kept and its keyframes read once; and every
// @font-face, read whole.
//
// WHAT A CASCADE READS IS ITS OWN. A sheet keeps each rule's block as the
// component values its parse made, and a cascade reads them — the blocks,
// the selectors, the values — into one arena of its own for all its
// sheets (`reads`), held to the selected read cap, and freed with the
// cascade. Nothing is written into a sheet, so a sheet is the same after
// any number of cascades as before the first, and every cascade starts
// from the whole budget (GARB.md § The cost).
//
// Then, per element in document order: the rules that match, weighed by
// §6's order — importance, then specificity (a rule matching through
// several selectors takes the most specific), then order of appearance —
// with the `style` attribute above every sheet at the same importance; the
// winners' var() and env() replaced against the custom properties the
// element inherits and declares, and so are those of the keyframes its
// winners name.

#include "values_internal.h"
#include "garb/cascade.h"
#include "garb/select.h"
#include "os64/fmt.h"
#include "os64/mem.h"
#include "os64/str.h"

// How deep @media, @supports and @layer may nest, and var() substitution
// may recurse, before the rest is ignored and the cascade marked incomplete.
#define NEST_MAX 16
#define VAR_DEPTH_MAX 32
// What one custom property's value may grow to once its own var()s are
// replaced: the shape that makes `--a: var(--b) var(--b)` ten levels deep a
// billion values long is cut here.
#define VAR_VALUES_MAX 4096

// ── What the build keeps ────────────────────────────────────────────────

typedef struct {
    bool important;
    bool custom;                // --name: `value` kept as written
    bool pending;               // holds var() or env(): read per element
    garb_decl_t decl;
    garb_set_t *sets;           // read: the longhands it sets
    int32_t nsets;
} Decl;

typedef struct {
    garb_selectors_t *sel;
    // Its selector as written: the sheet's, read only while the sheets
    // the cascade was built from are alive (garb_cascade_compare).
    const garb_value_t *prelude;
    int32_t nprelude;
    Decl *decls;
    int32_t ndecls;
    uint32_t order;
    int32_t sheet;              // which input sheet it came from
} Rule;

// A selector filed in the rule hash. It names its rule by INDEX: the rule
// array grows, and moves, while the sheets are still being read.
typedef struct {
    int32_t rule;
    int32_t sel;
    int32_t next;               // the next entry in the same bucket, or -1
} Entry;

// A bucket: entries whose key is the same string.
typedef struct {
    const char *key;
    size_t len;
    int32_t head;
} Bucket;

typedef struct {
    Bucket *b;
    uint32_t cap, n;
} Table;

// Where a rule's selectors are filed for one kind of target: an element,
// or one of its pseudo-elements (GENERATED_CONTENT.md § 3), keyed by the
// rightmost compound's id, else a class, else its type, else universal.
typedef struct {
    Table ids, classes, types;
    int32_t universal;
} Index;

// The targets a cascade files for: an element's own rules, ::before's and
// ::after's, a shadow host's from its own shadow tree (`:host`), and a
// slotted element's from a slot's tree (`::slotted()`), the last two part
// of the element's own winners (SHADOW_DOM.md § 6). A rule for another
// pseudo-element is read and not filed.
enum { TARGET_ELEMENT, TARGET_BEFORE, TARGET_AFTER, TARGET_HOST, TARGET_SLOTTED, TARGETS };

// The custom properties an element sees: its own, then those of the element
// it inherits from (inherit_from).
typedef struct CustomMap CustomMap;

typedef struct {
    const char *name;
    size_t len;
    // As declared (kEmptyValue for a declaration of nothing), or NULL: the
    // guaranteed-invalid value.
    const garb_value_t *value;
    int32_t nvalue;
    int state;                  // 0 as declared, 1 being replaced, 2 replaced, 3 invalid
    // The declaration it came from, which a recompute builds it from again
    // (its value is replaced in place once read: custom_value); and whether
    // its value is made from the map it inherits (var() or env() in it, or
    // `inherit` or `unset`).
    const Decl *decl;
    bool reads_up;
} Custom;

struct CustomMap {
    const CustomMap *parent;
    Custom *own;
    int32_t n;
};

// A declaration holding var() among an element's winners, with the
// longhands it won (THE RECOMPUTE): what a new custom property map makes
// again without matching.
typedef struct {
    const Decl *d;
    garb_prop_t *props, *produced;
    int32_t nprops, nproduced;
} PendDecl;

// A @keyframes rule kept (Animations 1 § 3): its name as written, and its
// blocks in order, each with its offsets and its declarations read once —
// with a var() left to be read per element, as a rule's are.
typedef struct {
    double *offsets;            // each in [0, 1]
    int32_t noffsets;
    Decl *decls;
    int32_t ndecls;
    bool easing;                // decls[0] is the block's own timing function
} KeyBlock;

typedef struct {
    const char *name;
    size_t len;
    KeyBlock *blocks;
    int32_t nblocks;
    int32_t sheet;              // which input sheet it came from
    bool pending;               // a declaration holds var(): read per element
    // Read against no element: the answer for one whose winners do not
    // name it, its var()s dropped.
    garb_keyframe_t *frames;
    int32_t nframes;
} Keyframes;

// An element's own reading of a rule that holds var().
typedef struct {
    const Keyframes *rule;
    garb_keyframe_t *frames;
    int32_t nframes;
} OwnFrames;

typedef struct {
    const os64_html_node_t *node;
    garb_style_t style;
    // Its ::before's and ::after's winners (TARGET_BEFORE - 1, TARGET_AFTER
    // - 1), none when no rule for it matched.
    garb_style_t pseudo[TARGET_AFTER];
    const CustomMap *customs;
    OwnFrames *frames;
    int32_t nframes;
    // The tree it is in, as the cascade sees it (tree_place): the scope
    // whose sheets apply there, -1 for none. `placed` once it is set.
    int32_t scope;
    // `dirty_suffix`: marked by a walk of siblings that marks each with
    // its subtree (mark_siblings), where every later walk of siblings stops,
    // the rest being marked as far; `alone_suffix`: marked by one that marks
    // each alone (mark_run), where a later walk that marks alone stops.
    bool placed, needs_substitution, dirty, dirty_suffix, alone_suffix;
    // THE RECOMPUTE: its custom property map is one of its own (its rules
    // declare custom properties), not the one it inherits taken whole; a
    // pseudo-element's winners hold var(); the update that last changed its
    // map by value (`moved`), which owes what inherits from it a recompute;
    // and its winners var() made, with the longhands each won.
    bool own_customs, pseudo_vars;
    uint64_t moved;
    const PendDecl *pend;
    int32_t npend;
    // The element it inherited from when last styled (inherit_from), which
    // a change of its `slot` can move.
    const os64_html_node_t *from;
    // What :empty answered at it when the cascade last styled it or asked at
    // a change, kept while the rules hold :empty (empty_turned).
    bool empty;
    // Its attributes changed in the update being made (mark_attribute): its
    // record counts as restyled even where its winners come out the same,
    // since what it lays out may read the attribute itself.
    bool attr_touched;
    // Its `class` and `id` values when the cascade last styled it (the
    // document's own strings: a change retires the old one, and the
    // cascade's pin keeps it), NULL when absent: what a change of either is
    // compared with (attribute_keys).
    const char *klass, *idv;
    // Its back and front indexes as the cascade last counted them,
    // saturated at the bounds the rules need (index_walk): among
    // its element siblings, and among those of its type; 0 for never
    // counted.
    uint8_t back, back_type, front, front_type;
    // The update that last counted its children again from the end and
    // from the start (index_turns), and walked them for an unbounded
    // position form (position_walks).
    uint64_t back_walked, front_walked, unbounded_walked;
    // Where the batch moved its parent's children's indexes, for the
    // unbounded walks: valid in update `walk_update` (WALK_*).
    uint64_t walk_update;
    uint8_t walk_flags;
    // Dirty for its own winners alone (mark_element): whether its
    // descendants are owed a recompute too is known once it has had one
    // (restyle).
    bool element_only;
    uint64_t changed;
    // The visit epoch that last visited it for its :has() answers
    // (has_visit, visit_epoch).
    uint64_t visited;
    // A digest of the rules that matched the element itself (match_digest).
    uint64_t matched;
    // Its `style` attribute's declarations as last read for its winners
    // (inline_decls), none for no attribute: what an inline style write is
    // compared with before the update for it (garb_cascade_inline_changes).
    const Decl *inl;
    int32_t ninl;
} Slot;

typedef struct {
    const garb_parsed_t *sheet;
    uint64_t serial;            // the sheet's parse (garb_parsed_t.serial), 0 for none: the kept read's key
    const char *media;
    garb_import_t via;
    bool imported;
    int32_t parent;
} CascadeInput;

// The most keys one change of an attribute names (attribute_keys).
#define CHANGE_KEYS 64
// The most custom property names one update's recompute keeps (THE
// RECOMPUTE): past it, none is known, and no element prunes it.
#define RECOMPUTE_NAMES 16
static const char *attr_value(const os64_html_node_t *el, const char *name);
// A reader's key in the index (reader_keys): sorted by kind, then the
// folded name's hash, then the folded name, so a change finds the readers
// of its key by a search. A reader that reads every key is under the kind
// GARB_KEYS_ANY.
typedef struct {
    uint32_t hash;
    uint8_t kind;
    const char *name;
    size_t len;
    int32_t reader;
} ReaderKey;
#define GARB_KEYS_ANY 3
// The keys read at an ancestor too (garb_key_t.up), a bit each.
enum { UP_DISABLED = 1, UP_LANG = 2, UP_DIR = 4 };
// THE READ (KEPT_READ.md; cascade.h § THE KEPT READ): everything build()
// makes before it touches an element, owned here and shared by every
// cascade built from it, which copies these fields into its own at build
// (read_install), and by any caller holding it (garb_cascade_read_hold).
// Refcounted; NEVER WRITTEN after build() has made it: what
// a cascade adds later (attribute parses, its own skipped records, its
// match marks) goes into the cascade's `self` and fields, and an array a
// cascade may append to is handed over with no spare room, so an append
// copies. `heap` is its arena for everything but its declarations, which
// are in `reads` (or in `heap`, when `reads` could not be made: such a
// read is never taken, `keep` false, nor is one that came up short).
// `heap_bytes` is its heap's size when made, which a cascade's retained
// budget counts as it counted the same bytes when they lay in `self` (the
// declarations' arena was never counted); `digest` its raw hash when
// made, under `check_read` (0 otherwise), compared when it is taken.
typedef struct garb_read {
    uint32_t refs;
    garb_parsed_t heap, reads;
    bool keep;
    size_t heap_bytes;
    uint64_t digest;
    // The key, beyond the inputs and scopes below.
    const os64_html_document_t *doc;
    garb_env_t env;
    os64_html_quirks_t quirks;
    size_t state_bytes, read_bytes;
    // What it made, as build() left the fields.
    CascadeInput *inputs;
    int32_t ninputs;
    garb_scope_in_t *given;
    int32_t ngiven;
    const os64_html_node_t **roots;
    int32_t nroots;
    uint8_t *applies;
    int32_t *root_slots;
    uint32_t root_slotcap;
    Rule *rules;
    int32_t nrules;
    Keyframes *keyframes;
    int32_t nkeyframes;
    bool keyframes_sorted;
    Entry *entries;
    int32_t nentries;
    Index index[TARGETS];
    garb_skip_t *skips;
    int32_t nskips;
    uint32_t skips_lost;
    garb_font_face_t *faces;
    int32_t nfaces;
    uint32_t faces_lost, allocs_failed;
    garb_anchor_t *anchors;
    int32_t nanchors, nbare;
    garb_anchor_t *subjects;
    int32_t nsubjects, nbare_subjects;
    garb_style_reader_t *style_readers;
    int32_t nstyle_readers;
    bool style_bare;
    garb_reader_t *readers;
    int32_t nreaders, nbare_readers, nplace_readers;
    uint16_t reads_any;
    bool readers_short;
    ReaderKey *reader_keys;
    int32_t nreader_keys;
    uint8_t up_keys;
    garb_selector_dependencies_t dependencies;
    garb_update_refusal_t untrackable;
    bool incomplete;
} Read;

// A cascade's mark on each rule of its read (consider): the match list it
// last went into (`session`, one for each list cascade_element starts) and
// the consultation, and where in that list, so one rule matched through two
// selectors is counted once, for an element and its pseudo-element apart,
// and once in each tree that styles it. A mark from an earlier list never
// stands for this one: an update may restyle one element twice (a parent
// marked alone whose winners changed marks its subtree again), and the
// second list must hold every rule. The cascade's own, indexed as the
// read's rules are: a mark on the shared rule would be another cascade's to
// overwrite.
typedef struct {
    const os64_html_node_t *seen;
    uint64_t session;
    int32_t pass;
    int32_t slot;
} RuleSeen;

struct garb_cascade {
    uint32_t refs;
    CascadeInput *inputs;
    int32_t ninputs;
    const os64_html_document_t *doc;
    os64_html_pin_t pin;
    os64_html_reader_t *reader;
    uint64_t generation, updates, version;
    garb_selector_dependencies_t dependencies;
    bool update_failed, updating;
    size_t update_attribute_bytes;
    size_t initial_state_bytes, retained_budget;
    garb_parsed_t self;         // the cascade's own arena, in a parse's shape
    // Its read (Read), shared, released with the cascade; where alloc and
    // grow carve (`heap`: the read's arena while build() reads, `self`
    // after); its marks on the read's rules (RuleSeen); why the build read
    // again, or that it took a read; and, under `check_read`, whether the
    // read it took had been written since it was made.
    Read *read;
    garb_parsed_t *heap;
    RuleSeen *seen;
    uint64_t match_session;
    garb_read_refusal_t read_refusal;
    bool read_written;
    garb_env_t env;
    garb_cascade_limits_t limits;
    os64_html_quirks_t quirks;
    bool incomplete;
    Rule *rules;
    int32_t nrules, rulecap;
    // The @keyframes rules: each as it is read, then, once every sheet is
    // (keyframes_settle), one per name — the last of it — sorted by name.
    Keyframes *keyframes;
    int32_t nkeyframes, keyframecap;
    bool keyframes_sorted;
    Entry *entries;
    int32_t nentries, entrycap;
    Index index[TARGETS];
    Slot *slots;                // open addressing, keyed by node
    uint32_t slotcap, slotcount;
    const os64_html_node_t **dirty_nodes;
    size_t ndirty, dirtycap;
    garb_parsed_t *attrs;       // `style` attributes' parses, freed with the cascade
    int32_t nattrs, attrcap;
    // What the build passed over because this library does not read it
    // (garb_cascade_skips). Its storage never marks the cascade
    // incomplete: the tally is an instrument, and running out of room for
    // it changes nothing the page gets.
    garb_skip_t *skips;
    int32_t nskips, skipcap;
    uint32_t skips_lost;
    // The @font-face rules read (garb_cascade_font_faces).
    garb_font_face_t *faces;
    int32_t nfaces, facecap;
    uint32_t faces_lost;
    // How many allocations have failed, so a reader can tell whether its
    // own did (add_face).
    uint32_t allocs_failed;
    // The scopes as the caller gave them, for garb_cascade_matches_scoped
    // (`given`, `ngiven`, the lists copied); and as the cascade uses them:
    // each distinct root once (`roots`), and which sheets apply in each
    // (`applies`, a bit per (scope, input), imports included). Without
    // scopes the document's root is the one scope, every sheet applying.
    garb_scope_in_t *given;
    int32_t ngiven;
    const os64_html_node_t **roots;
    int32_t nroots;
    uint8_t *applies;
    // Each root's scope found by hashing it (scope_of_root): open
    // addressing over `root_slotcap` slots, a power of two at least twice
    // the roots, each the index in `roots` or -1. A page can have
    // thousands of component roots, and every host and every shadow
    // root's child asks.
    int32_t *root_slots;
    uint32_t root_slotcap;
    // Whether the document has had a shadow root: the walks cross into
    // shadow trees and an update recomputes in one ordered walk.
    bool shadowed;
    // The anchored :has() compounds of its rules (`nbare` of them bare).
    // What each answered at an element when last asked is bit i of that
    // element's row of `has_words`: `has_width` words a slot, the row at the
    // slot's own index, moved with it when the table grows or a removal
    // backshifts, so a slot's record can be rebuilt (restyle) without
    // touching its bits. None when its rules keep it from updates at birth
    // (`untrackable`); a cascade that comes up short later, or has no
    // reader, keeps rows it will never compare.
    // `has_scratch` is one row, for an update's compare; `bare_work` what
    // the bare anchors cost the latest update. `untrackable` names what
    // kept the cascade from a change reader at birth (a counted :has()),
    // and `refusal` why the latest update answered false.
    garb_anchor_t *anchors;
    int32_t nanchors, nbare;
    garb_anchor_t *subjects;
    int32_t nsubjects, nbare_subjects;
    // The style readers of every rule (garb_cascade_style_may_read), up to
    // GARB_STYLE_READERS; `style_bare`: one narrows to no element, there
    // were more than that, or a declaration reads attr(style), so any
    // inline write may change what matches.
    garb_style_reader_t *style_readers;
    int32_t nstyle_readers;
    bool style_bare;
    // The readers of every rule (select.h § A READER), `nbare_readers` of
    // them bare, the first `nplace_readers` those reading more than keys
    // (the ones a change of children asks, readers_at), and what any of
    // them reads;
    // `readers_short`: there was no memory to keep them, so every ask
    // passes every reader with the widest reach.
    garb_reader_t *readers;
    int32_t nreaders, nbare_readers, nplace_readers;
    uint16_t reads_any;
    bool readers_short;
    // The readers' keys, for a change of an attribute (reader_keys), and
    // the keys some reader reads at an ancestor too (UP_*).
    ReaderKey *reader_keys;
    int32_t nreader_keys;
    uint8_t up_keys;
    // THE BATCH, as an update reads it before it marks (batch_take): the
    // nodes its changes of children took out and put in, and the elements
    // whose attributes it changed, each sorted by address; and whether a
    // change of children happened under something the batch took out, or
    // outside the document, so a removed subtree may no longer show what it
    // held.
    const os64_html_node_t **batch_removed, **batch_added, **batch_attrs;
    size_t nbatch_removed, nbatch_added, nbatch_attrs, batch_cap[3];
    bool batch_detached;
    // The subject :has() compounds the batch's changes so far may turn
    // (subjects_live), a bit each, and those the change being marked may;
    // `visit_epoch` moves on at each update and whenever the first set
    // grows, so the climbs that follow visit again what they pass.
    uint64_t *subjects_union, *subjects_now;
    uint32_t subject_width;
    // The keys of the change of an attribute being marked (attribute_keys):
    // `change_unknown` where they cannot be named, `change_up` where one is
    // read at an ancestor too.
    garb_key_t change_keys[CHANGE_KEYS];
    int32_t nchange_keys;
    bool change_unknown, change_up;
    uint64_t visit_epoch;
    garb_reader_work_t reader_work;
    uint32_t has_width;
    uint64_t *has_words, *has_scratch;
    garb_bare_work_t bare_work;
    garb_has_subject_work_t has_subjects;
    garb_back_work_t back_work;
    garb_update_marks_t marks;
    // THE RECOMPUTE, in the update being made: whether any element's
    // custom property map moved, and the names whose values may have
    // changed with it (`names_all` past the room: then none is known).
    bool any_moved, names_all;
    struct { const char *name; size_t len; } moved_names[RECOMPUTE_NAMES];
    int32_t nmoved_names;
    garb_update_refusal_t untrackable, refusal;
    // THE CALLER'S MEASURE (limits.clock; garb_cascade_build_cost,
    // garb_cascade_broad_trigger): the fresh build's parts, and in an
    // update the mark being made (`mark_now`), what the change being marked
    // has marked by mark (`mark_by`), and the change that marked the most
    // (`broad`, kept as the trigger when the update is refused broad).
    garb_build_cost_t cost;
    garb_mark_t mark_now;
    uint32_t mark_by[GARB_MARKS];
    garb_broad_trigger_t broad;
    const os64_html_change_entry_t *broad_best;     // the change marking the most, this update only
    bool broad_set;
};

// Whether input `sheet` applies in scope `scope` (-1: none does).
static bool applies_in(const garb_cascade_t *c, int32_t scope, int32_t sheet)
{
    if (scope < 0 || sheet < 0)
        return false;
    size_t bit = (size_t)scope * (size_t)c->ninputs + (size_t)sheet;
    return (c->applies[bit / 8] >> (bit % 8)) & 1;
}

// How this cascade reads a declaration: in its document's mode, knowing the
// properties its environment knows (garb_env_t.modern_off).
static garb_reading_t reading(const garb_cascade_t *c)
{
    return (garb_reading_t){.quirks = c->quirks == OS64_HTML_QUIRKS, .modern_off = c->env.modern_off};
}

// Carved from `heap`: the read's arena while build() reads, the
// cascade's own after (garb_cascade.heap).
static void *alloc(garb_cascade_t *c, size_t n)
{
    void *p = os64_arena_calloc(c->heap->arena, 1, n);
    if (p == NULL) {
        c->incomplete = true;
        c->allocs_failed++;
    }
    return p;
}

// Grows an arena list by doubling.
static bool grow(garb_cascade_t *c, void **list, int32_t n, int32_t *cap, size_t size)
{
    if (n < *cap)
        return true;
    int32_t bigger = *cap > 0 ? *cap * 2 : 16;
    void *next = os64_arena_alloc(c->heap->arena, (size_t)bigger * size);
    if (next == NULL) {
        c->incomplete = true;
        c->allocs_failed++;
        return false;
    }
    if (n > 0)
        os64_memcpy(next, *list, (size_t)n * size);
    *list = next;
    *cap = bigger;
    return true;
}

// ── The rule hash ───────────────────────────────────────────────────────

static uint32_t hash_bytes(const char *s, size_t n, bool fold)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (fold && ch >= 'A' && ch <= 'Z')
            ch = (char)(ch - 'A' + 'a');
        h = (h ^ (uint8_t)ch) * 16777619u;
    }
    return h;
}

static bool same_key(const char *a, size_t alen, const char *b, size_t blen, bool fold)
{
    if (alen != blen)
        return false;
    for (size_t i = 0; i < alen; i++) {
        char x = a[i], y = b[i];
        if (fold) {
            if (x >= 'A' && x <= 'Z')
                x = (char)(x - 'A' + 'a');
            if (y >= 'A' && y <= 'Z')
                y = (char)(y - 'A' + 'a');
        }
        if (x != y)
            return false;
    }
    return true;
}

static Bucket *bucket(garb_cascade_t *c, Table *t, const char *key, size_t len, bool fold,
                      bool create)
{
    if (t->cap == 0) {
        if (!create)
            return NULL;
        t->cap = 256;
        t->b = alloc(c, t->cap * sizeof(Bucket));
        if (t->b == NULL) {
            t->cap = 0;
            return NULL;
        }
    }
    if (create && (t->n + 1) * 2 > t->cap) {
        Table bigger = {0};
        bigger.cap = t->cap * 2;
        bigger.b = alloc(c, bigger.cap * sizeof(Bucket));
        if (bigger.b == NULL)
            return NULL;
        for (uint32_t i = 0; i < t->cap; i++) {
            if (t->b[i].key == NULL)
                continue;
            uint32_t h = hash_bytes(t->b[i].key, t->b[i].len, fold) & (bigger.cap - 1);
            while (bigger.b[h].key != NULL)
                h = (h + 1) & (bigger.cap - 1);
            bigger.b[h] = t->b[i];
        }
        bigger.n = t->n;
        *t = bigger;
    }
    uint32_t h = hash_bytes(key, len, fold) & (t->cap - 1);
    while (t->b[h].key != NULL) {
        if (same_key(t->b[h].key, t->b[h].len, key, len, fold))
            return &t->b[h];
        h = (h + 1) & (t->cap - 1);
    }
    if (!create)
        return NULL;
    t->b[h].key = key;
    t->b[h].len = len;
    t->b[h].head = -1;
    t->n++;
    return &t->b[h];
}

static void file_selector(garb_cascade_t *c, int32_t rule, int32_t sel, int32_t target)
{
    if (!grow(c, (void **)&c->entries, c->nentries, &c->entrycap, sizeof(Entry)))
        return;
    garb_selector_key_t key = garb_selector_key(c->rules[rule].sel, sel);
    bool quirky = c->quirks == OS64_HTML_QUIRKS;
    Index *ix = &c->index[target];
    Bucket *b = NULL;
    if (key.id != NULL)
        b = bucket(c, &ix->ids, key.id, key.id_len, quirky, true);
    else if (key.class_name != NULL)
        b = bucket(c, &ix->classes, key.class_name, key.class_len, quirky, true);
    else if (key.type != NULL)
        b = bucket(c, &ix->types, key.type, key.type_len, true, true);
    int32_t *head = b != NULL ? &b->head : &ix->universal;
    if (b == NULL && (key.id != NULL || key.class_name != NULL || key.type != NULL))
        return;                         // no memory to file it
    Entry *e = &c->entries[c->nentries];
    e->rule = rule;
    e->sel = sel;
    e->next = *head;
    *head = c->nentries++;
}

// ── What the build passed over ──────────────────────────────────────────

// The functions some reader in values.c or props.c accepts, by name: a
// declaration this library dropped that names any OTHER function is
// counted as asking for that function. One it dropped naming only these
// was a value written wrong, which is the page's, not a gap.
static const char *const kFunctionsRead[] = {
    "calc", "min", "max", "clamp", "rgb", "rgba", "hsl", "hsla", "hwb", "url", "var", "env",
    "linear-gradient", "radial-gradient", "conic-gradient", "repeating-linear-gradient",
    "repeating-radial-gradient", "repeating-conic-gradient", "image-set", "minmax",
    "fit-content", "repeat", "rect", NULL,
};

// The prefixed property names every engine reads (Compatibility § 3.4.1,
// § 3.4.4 and § 3.4.6, and CSS Overflow 4 § 5.1.1's -webkit-line-clamp):
// no one engine's dialect but the web's, so one this library does not read
// is a gap like a standard name's. § 3.4.2's -webkit-text-size-adjust is
// not here: it is a phone's, and engines on a desktop pass it over too.
static const char *const kPrefixedRead[] = {
    "-webkit-align-items", "-webkit-align-content", "-webkit-align-self",
    "-webkit-animation", "-webkit-animation-name", "-webkit-animation-duration",
    "-webkit-animation-timing-function", "-webkit-animation-iteration-count",
    "-webkit-animation-direction", "-webkit-animation-play-state", "-webkit-animation-delay",
    "-webkit-animation-fill-mode", "-webkit-backface-visibility", "-webkit-background-clip",
    "-webkit-background-origin", "-webkit-background-size", "-webkit-border-bottom-left-radius",
    "-webkit-border-bottom-right-radius", "-webkit-border-top-left-radius",
    "-webkit-border-top-right-radius", "-webkit-border-radius", "-webkit-box-shadow",
    "-webkit-box-sizing", "-webkit-flex", "-webkit-flex-basis", "-webkit-flex-direction",
    "-webkit-flex-flow", "-webkit-flex-grow", "-webkit-flex-shrink", "-webkit-flex-wrap",
    "-webkit-filter", "-webkit-justify-content", "-webkit-mask", "-webkit-mask-box-image",
    "-webkit-mask-box-image-outset", "-webkit-mask-box-image-repeat",
    "-webkit-mask-box-image-slice", "-webkit-mask-box-image-source",
    "-webkit-mask-box-image-width", "-webkit-mask-clip", "-webkit-mask-composite",
    "-webkit-mask-image", "-webkit-mask-origin", "-webkit-mask-position", "-webkit-mask-repeat",
    "-webkit-mask-size", "-webkit-order", "-webkit-perspective", "-webkit-perspective-origin",
    "-webkit-transform-origin", "-webkit-transform-style", "-webkit-transform",
    "-webkit-transition-delay", "-webkit-transition-duration", "-webkit-transition-property",
    "-webkit-transition-timing-function", "-webkit-transition",
    "-webkit-box-align", "-webkit-box-flex", "-webkit-box-ordinal-group", "-webkit-box-orient",
    "-webkit-box-pack", "-webkit-text-fill-color", "-webkit-text-stroke-color",
    "-webkit-text-stroke-width", "-webkit-text-stroke", "-webkit-line-clamp", NULL,
};

static bool prefixed_read(const char *name, size_t len)
{
    for (int k = 0; kPrefixedRead[k] != NULL; k++)
        if (ieq(name, len, kPrefixedRead[k]))
            return true;
    return false;
}

// The skipped records made the cascade's own before it writes one: while
// they are still its read's (a count raised, a name added after build()),
// they are copied into its arena first, so the read is never written.
static bool skips_own(garb_cascade_t *c)
{
    if (c->read == NULL || c->skips == NULL || c->skips != c->read->skips)
        return true;
    int32_t cap = c->nskips > 0 ? c->nskips : 1;
    garb_skip_t *own = os64_arena_alloc(c->heap->arena, (size_t)cap * sizeof(garb_skip_t));
    if (own == NULL)
        return false;
    os64_memcpy(own, c->skips, (size_t)c->nskips * sizeof(garb_skip_t));
    c->skips = own;
    c->skipcap = cap;
    return true;
}

static void skipped(garb_cascade_t *c, garb_skip_kind_t kind, const char *name, size_t len)
{
    // Any other prefixed name is one engine's dialect, which every other
    // engine passes over too: what a page asks of a browser is the
    // standard name.
    if (len == 0 || (name[0] == '-' && !(kind == GARB_SKIP_PROPERTY && prefixed_read(name, len))))
        return;
    if (!skips_own(c)) {
        c->skips_lost++;
        return;
    }
    // ieq folds its first argument; the kept copy is already lower case.
    for (int32_t i = 0; i < c->nskips; i++)
        if (c->skips[i].kind == kind && c->skips[i].len == len && ieq(name, len, c->skips[i].name)) {
            if (c->skips[i].count != UINT32_MAX)
                c->skips[i].count++;
            return;
        }
    if (c->nskips == c->skipcap) {
        // Doubled, up to the cap: a copy made from a read has room for
        // exactly what it held (skips_own), not a power of two.
        int32_t bigger = c->skipcap > 0 ? c->skipcap * 2 : 16;
        if (bigger > GARB_SKIPS_MAX && c->skipcap < GARB_SKIPS_MAX)
            bigger = GARB_SKIPS_MAX;
        garb_skip_t *next = bigger <= GARB_SKIPS_MAX
            ? os64_arena_alloc(c->heap->arena, (size_t)bigger * sizeof(garb_skip_t)) : NULL;
        if (next == NULL) {
            c->skips_lost++;
            return;
        }
        if (c->nskips > 0)
            os64_memcpy(next, c->skips, (size_t)c->nskips * sizeof(garb_skip_t));
        c->skips = next;
        c->skipcap = bigger;
    }
    char *copy = os64_arena_alloc(c->heap->arena, len + 1);
    if (copy == NULL) {
        c->skips_lost++;
        return;
    }
    // Names are compared without case, as CSS compares them, and kept in
    // lower case so one name is spelled one way wherever it is reported.
    for (size_t i = 0; i < len; i++)
        copy[i] = name[i] >= 'A' && name[i] <= 'Z' ? (char)(name[i] + 32) : name[i];
    copy[len] = '\0';
    c->skips[c->nskips++] = (garb_skip_t){kind, copy, len, 1};
}

static bool function_read(const garb_value_t *v)
{
    for (int k = 0; kFunctionsRead[k] != NULL; k++)
        if (ieq(v->text, v->len, kFunctionsRead[k]))
            return true;
    return false;
}

// The functions a dropped declaration names that no reader here accepts,
// at any depth.
static void skipped_functions(garb_cascade_t *c, const garb_value_t *v, int32_t n, int depth)
{
    for (int32_t i = 0; i < n && depth < GARB_DEPTH_MAX; i++) {
        if (v[i].kind == GARB_FUNCTION && !function_read(&v[i]))
            skipped(c, GARB_SKIP_FUNCTION, v[i].text, v[i].len);
        if (v[i].kind == GARB_FUNCTION || v[i].kind == GARB_BLOCK)
            skipped_functions(c, v[i].children, v[i].nchildren, depth + 1);
    }
}

// A value of a property this library reads, written `<property>: <words>`:
// what a page asked of the property and did not get.
static void skipped_value(garb_cascade_t *c, const char *prop, size_t plen, const garb_value_t *v,
                          int32_t n)
{
    char name[128];
    size_t at = 0;
    for (size_t i = 0; i < plen && at + 1 < sizeof(name); i++)
        name[at++] = prop[i];
    for (const char *s = ": "; *s != '\0' && at + 1 < sizeof(name); s++)
        name[at++] = *s;
    for (int32_t i = 0; i < n; i++) {
        if (v[i].kind == GARB_WHITESPACE)
            continue;
        // A prefixed word is one engine's dialect, as most prefixed names
        // are (kPrefixedRead);
        // so is a control character, which no keyword holds and old IE's
        // hacks (`display: none\9`) write on purpose.
        if (v[i].len != 0 && v[i].text[0] == '-')
            return;
        for (uint32_t k = 0; k < v[i].len; k++)
            if ((unsigned char)v[i].text[k] < 0x20)
                return;
        if (name[at - 1] != ' ' && at + 1 < sizeof(name))
            name[at++] = ' ';
        for (uint32_t k = 0; k < v[i].len && at + 1 < sizeof(name); k++)
            name[at++] = v[i].text[k];
    }
    skipped(c, GARB_SKIP_VALUE, name, at);
}

// Whether a value is bare words and nothing else: `inline flex`, a keyword
// this library does not read. One that holds a number, a string or a
// function is a value written wrong far more often than a gap.
static bool only_words(const garb_value_t *v, int32_t n)
{
    bool any = false;
    for (int32_t i = 0; i < n; i++) {
        if (v[i].kind == GARB_IDENT)
            any = true;
        else if (v[i].kind != GARB_WHITESPACE)
            return false;
    }
    return any;
}

// ── @font-face (Fonts 4 § 4) ────────────────────────────────────────────

// `text` copied into the cascade's arena, NUL after it; NULL on no memory.
static const char *copy_text(garb_cascade_t *c, const char *text, size_t len)
{
    char *copy = alloc(c, len + 1);
    if (copy != NULL && len > 0)
        os64_memcpy(copy, text, len);
    return copy;
}

// The value with white space taken off its ends: [*i, *n).
static void trim(const garb_value_t *v, int32_t *i, int32_t *n)
{
    while (*i < *n && v[*i].kind == GARB_WHITESPACE)
        (*i)++;
    while (*n > *i && v[*n - 1].kind == GARB_WHITESPACE)
        (*n)--;
}

// A <family-name> in [i, n): one string, or identifiers joined by single
// spaces — none of them a CSS-wide keyword or `default`, and not one
// identifier alone that is a generic family's keyword or a system font's,
// which must be quoted to name a face (Fonts 4 § 2.1.1). Copied into the arena at its own
// length, `*len` set: NULL for a name written wrong, or memory refused.
static const char *family_name(garb_cascade_t *c, const garb_value_t *v, int32_t i, int32_t n, size_t *len)
{
    trim(v, &i, &n);
    if (i >= n)
        return NULL;
    if (v[i].kind == GARB_STRING) {
        *len = v[i].len;
        return i + 1 == n ? copy_text(c, v[i].text, v[i].len) : NULL;
    }
    size_t need = 0;
    int32_t words = 0, first = i;
    for (int32_t k = i; k < n; k++) {
        if (v[k].kind == GARB_WHITESPACE)
            continue;
        if (v[k].kind != GARB_IDENT || garb_family_word_reserved(v[k].text, v[k].len))
            return NULL;
        need += (words++ > 0) + v[k].len;
    }
    if (words == 1 && garb_generic_family(v[first].text, v[first].len))
        return NULL;
    if (words == 1 && garb_system_font(v[first].text, v[first].len))
        return NULL;
    char *out = alloc(c, need + 1);
    if (out == NULL)
        return NULL;
    size_t at = 0;
    for (int32_t k = i; k < n; k++) {
        if (v[k].kind == GARB_WHITESPACE)
            continue;
        if (at > 0)
            out[at++] = ' ';
        os64_memcpy(out + at, v[k].text, v[k].len);
        at += v[k].len;
    }
    *len = need;
    return out;
}

// format()'s argument, whole (Fonts 4 § 4.3): one of the format keywords,
// or strings separated by commas — the older grammar pages still write.
// Its first value, the hint; NULL for anything else.
static const garb_value_t *format_hint(const garb_value_t *v, int32_t n)
{
    static const char *const kFormats[] = {"collection", "embedded-opentype", "opentype", "svg",
                                           "truetype", "woff", "woff2", NULL};
    int32_t i = 0;
    trim(v, &i, &n);
    if (i + 1 == n && v[i].kind == GARB_IDENT) {
        for (int k = 0; kFormats[k] != NULL; k++)
            if (ieq(v[i].text, v[i].len, kFormats[k]))
                return &v[i];
        return NULL;
    }
    const garb_value_t *first = NULL;
    bool want_string = true;
    for (; i < n; i++) {
        if (v[i].kind == GARB_WHITESPACE)
            continue;
        if (want_string ? v[i].kind != GARB_STRING : v[i].kind != GARB_COMMA)
            return NULL;
        if (first == NULL)
            first = &v[i];
        want_string = !want_string;
    }
    return want_string ? NULL : first;
}

// One `src` entry in [i, n): url() [format()] — tech() drops it — or
// local(<family-name>). False for one written wrong, or with tech().
static bool face_src(garb_cascade_t *c, const garb_value_t *v, int32_t i, int32_t n, garb_face_src_t *out)
{
    trim(v, &i, &n);
    if (i >= n)
        return false;
    os64_memset(out, 0, sizeof(*out));
    const garb_value_t *first = &v[i];
    if (first->kind == GARB_FUNCTION && ieq(first->text, first->len, "local")) {
        if (i + 1 != n)
            return false;
        out->kind = GARB_FACE_LOCAL;
        out->text = family_name(c, first->children, 0, first->nchildren, &out->len);
        return out->text != NULL;
    }
    out->kind = GARB_FACE_URL;
    if (first->kind == GARB_URL) {
        out->text = first->text;
        out->len = first->len;
    } else if (first->kind == GARB_FUNCTION && ieq(first->text, first->len, "url")) {
        int32_t a = 0, b = first->nchildren;
        trim(first->children, &a, &b);
        if (b != a + 1 || first->children[a].kind != GARB_STRING)
            return false;
        out->text = first->children[a].text;
        out->len = first->children[a].len;
    } else {
        return false;
    }
    for (i++; i < n; i++) {
        if (v[i].kind == GARB_WHITESPACE)
            continue;
        if (v[i].kind != GARB_FUNCTION)
            return false;
        if (ieq(v[i].text, v[i].len, "tech"))
            return false;
        if (!ieq(v[i].text, v[i].len, "format") || out->format != NULL)
            return false;
        const garb_value_t *hint = format_hint(v[i].children, v[i].nchildren);
        if (hint == NULL)
            return false;
        out->format = copy_text(c, hint->text, hint->len);
        out->format_len = hint->len;
        if (out->format == NULL)
            return false;
    }
    out->text = copy_text(c, out->text, out->len);
    return out->text != NULL;
}

// A hex digit's value, or -1.
static int hexval(char ch)
{
    return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
         : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
}

// One <urange> in [i, n): the tokenizer makes no such token, so its text is
// the representations of the tokens that spell it, joined (Syntax 3,
// "consume the value of a unicode-range descriptor"), none written with
// an escape, since that reads the source's own characters: `u` or `U`,
// `+`, one to six hex digits and `?`s — the `?`s, all trailing, as 0 below and F
// above — or two runs of hex digits about a `-`. False for anything else,
// or a range past U+10FFFF or backwards.
static bool urange(const garb_value_t *v, int32_t i, int32_t n, garb_unicode_range_t *out)
{
    trim(v, &i, &n);
    char text[64];
    size_t len = 0;
    for (; i < n; i++) {
        const garb_value_t *t = &v[i];
        size_t need = t->len + (t->kind == GARB_DIMENSION ? t->unit_len : 0);
        if (t->kind == GARB_WHITESPACE || t->escaped || len + need >= sizeof(text) ||
            (t->kind != GARB_IDENT && t->kind != GARB_NUMBER && t->kind != GARB_DIMENSION &&
             t->kind != GARB_DELIM))
            return false;
        os64_memcpy(text + len, t->text, t->len);
        len += t->len;
        if (t->kind == GARB_DIMENSION) {
            os64_memcpy(text + len, t->unit, t->unit_len);
            len += t->unit_len;
        }
    }
    if (len < 3 || (text[0] != 'u' && text[0] != 'U') || text[1] != '+')
        return false;
    size_t k = 2;
    uint32_t lo = 0, hi = 0;
    int digits = 0, wild = 0;
    for (; k < len && digits + wild < 6; k++) {
        int h = hexval(text[k]);
        if (h >= 0 && wild == 0) {
            lo = lo * 16 + (uint32_t)h;
            hi = hi * 16 + (uint32_t)h;
            digits++;
        } else if (text[k] == '?') {
            lo = lo * 16;
            hi = hi * 16 + 15;
            wild++;
        } else {
            break;
        }
    }
    if (digits + wild == 0)
        return false;
    if (wild > 0) {
        if (k != len)
            return false;
    } else if (k < len) {
        if (text[k] != '-' || ++k == len)
            return false;
        hi = 0;
        int more = 0;
        for (; k < len; k++, more++) {
            int h = hexval(text[k]);
            if (h < 0 || more == 6)
                return false;
            hi = hi * 16 + (uint32_t)h;
        }
    }
    if (hi > 0x10FFFF || lo > hi)
        return false;
    *out = (garb_unicode_range_t){lo, hi};
    return true;
}

// The comma-separated parts of a declaration's value: each part's [a, b)
// handed to `each` in turn; false when one is refused (`forgiving` skips
// it instead) — and answers how many were taken.
typedef bool (*PartFn)(garb_cascade_t *c, const garb_value_t *v, int32_t a, int32_t b, void *out, int32_t k);

static int32_t parts(garb_cascade_t *c, const garb_decl_t *d, PartFn each, void *out, int32_t cap,
                     bool forgiving)
{
    int32_t taken = 0, a = 0;
    for (int32_t i = 0; i <= d->nvalue; i++) {
        if (i < d->nvalue && d->value[i].kind != GARB_COMMA)
            continue;
        if (taken < cap && each(c, d->value, a, i, out, taken))
            taken++;
        else if (!forgiving)
            return -1;
        a = i + 1;
    }
    return taken;
}

static bool src_part(garb_cascade_t *c, const garb_value_t *v, int32_t a, int32_t b, void *out, int32_t k)
{
    return face_src(c, v, a, b, &((garb_face_src_t *)out)[k]);
}

static bool range_part(garb_cascade_t *c, const garb_value_t *v, int32_t a, int32_t b, void *out, int32_t k)
{
    (void)c;
    return urange(v, a, b, &((garb_unicode_range_t *)out)[k]);
}

// A face's weight: `normal` (400), `bold` (700) or a number from 1 to
// 1000, a fraction kept (matching does not round); two make a range, in
// either order (Fonts 4 § 4.5). `auto` selects as `normal` (§ 4.4: it
// only spares a variable face's axis its clamp, and no axis is read here).
static bool face_weight(const garb_decl_t *d, float *lo, float *hi)
{
    double w[2];
    int32_t k = 0;
    for (int32_t i = 0; i < d->nvalue; i++) {
        const garb_value_t *t = &d->value[i];
        if (t->kind == GARB_WHITESPACE)
            continue;
        if (k == 2)
            return false;
        if (t->kind == GARB_NUMBER && t->number >= 1 && t->number <= 1000)
            w[k++] = t->number;
        else if (t->kind == GARB_IDENT && ieq(t->text, t->len, "normal"))
            w[k++] = 400;
        else if (t->kind == GARB_IDENT && ieq(t->text, t->len, "bold"))
            w[k++] = 700;
        else if (t->kind == GARB_IDENT && ieq(t->text, t->len, "auto") && d->nvalue == 1)
            w[0] = 400, w[1] = 400, k = 2;
        else
            return false;
    }
    if (k == 0)
        return false;
    if (k == 1)
        w[1] = w[0];
    *lo = (float)(w[0] < w[1] ? w[0] : w[1]);
    *hi = (float)(w[0] < w[1] ? w[1] : w[0]);
    return true;
}

// An angle in degrees, from a dimension in one of the four angle units.
static bool angle_deg(const garb_value_t *t, double *deg)
{
    static const struct {
        const char *unit;
        double per;
    } kUnits[] = {{"deg", 1}, {"rad", 57.29577951308232}, {"grad", 0.9}, {"turn", 360}};
    if (t->kind != GARB_DIMENSION)
        return false;
    for (size_t k = 0; k < sizeof(kUnits) / sizeof(kUnits[0]); k++)
        if (ieq(t->unit, t->unit_len, kUnits[k].unit)) {
            *deg = t->number * kUnits[k].per;
            return true;
        }
    return false;
}

// A face's style: `normal`, `italic`, or `oblique` with up to two angles
// after it, each between -90 and 90 degrees (Fonts 4 § 4.4) — and `auto`,
// which selects as `normal`, as the weight's does.
static bool face_style(const garb_decl_t *d, garb_face_style_t *out)
{
    int32_t i = 0, n = d->nvalue;
    trim(d->value, &i, &n);
    if (i >= n || d->value[i].kind != GARB_IDENT)
        return false;
    const garb_value_t *t = &d->value[i];
    if ((ieq(t->text, t->len, "normal") || ieq(t->text, t->len, "auto")) && i + 1 == n) {
        *out = GARB_FACE_NORMAL;
        return true;
    }
    if (ieq(t->text, t->len, "italic") && i + 1 == n) {
        *out = GARB_FACE_ITALIC;
        return true;
    }
    if (!ieq(t->text, t->len, "oblique"))
        return false;
    int32_t angles = 0;
    for (i++; i < n; i++) {
        double deg;
        if (d->value[i].kind == GARB_WHITESPACE)
            continue;
        if (++angles > 2 || !angle_deg(&d->value[i], &deg) || deg < -90 || deg > 90)
            return false;
    }
    *out = GARB_FACE_OBLIQUE;
    return true;
}

// An @font-face rule, read whole and kept (garb_cascade_font_faces). Each
// descriptor's last valid declaration is the one kept; one written wrong
// is dropped, as a property's is. A face any allocation failed for is
// dropped whole and counted lost: half a source list is not the rule.
static void add_face(garb_cascade_t *c, garb_parsed_t *owner, const garb_rule_t *r, int32_t sheet)
{
    garb_item_t *items;
    int32_t n;
    if (!r->has_block)
        return;
    // The rule has no prelude (Fonts 4 § 4.1): one written with one is no
    // rule at all.
    for (int32_t i = 0; i < r->nprelude; i++)
        if (r->prelude[i].kind != GARB_WHITESPACE)
            return;
    // Past the cap, a rule is counted and nothing of it read or kept.
    if (c->nfaces == GARB_FONT_FACES_MAX) {
        c->faces_lost++;
        return;
    }
    if (!garb_items_of(owner, r->block, r->nblock, &items, &n)) {
        c->incomplete = true;
        c->faces_lost++;
        return;
    }
    uint32_t failed = c->allocs_failed;
    garb_font_face_t face = {NULL, 0, NULL, 0, 400, 400, GARB_FACE_NORMAL, NULL, 0, sheet};
    for (int32_t i = 0; i < n; i++) {
        const garb_decl_t *d = &items[i].decl;
        if (items[i].kind != GARB_ITEM_DECL)
            continue;
        if (ieq(d->name, d->len, "font-family")) {
            size_t len;
            const char *name = family_name(c, d->value, 0, d->nvalue, &len);
            if (name != NULL)
                face.family = name, face.family_len = len;
        } else if (ieq(d->name, d->len, "src")) {
            int32_t most = 1;
            for (int32_t k = 0; k < d->nvalue; k++)
                most += d->value[k].kind == GARB_COMMA;
            garb_face_src_t *src = alloc(c, (size_t)most * sizeof(*src));
            int32_t got = src != NULL ? parts(c, d, src_part, src, most, true) : 0;
            if (got > 0)
                face.src = src, face.nsrc = got;
        } else if (ieq(d->name, d->len, "font-weight")) {
            (void)face_weight(d, &face.weight_min, &face.weight_max);
        } else if (ieq(d->name, d->len, "font-style")) {
            (void)face_style(d, &face.style);
        } else if (ieq(d->name, d->len, "unicode-range")) {
            int32_t most = 1;
            for (int32_t k = 0; k < d->nvalue; k++)
                most += d->value[k].kind == GARB_COMMA;
            garb_unicode_range_t *ranges = alloc(c, (size_t)most * sizeof(*ranges));
            int32_t got = ranges != NULL ? parts(c, d, range_part, ranges, most, false) : -1;
            if (got > 0)
                face.ranges = ranges, face.nranges = got;
        }
    }
    if (c->allocs_failed != failed) {
        c->faces_lost++;
        return;
    }
    if (face.family == NULL || face.nsrc == 0)
        return;
    if (!grow(c, (void **)&c->faces, c->nfaces, &c->facecap, sizeof(garb_font_face_t))) {
        c->faces_lost++;
        return;
    }
    c->faces[c->nfaces++] = face;
}

// ── Reading the sheets ──────────────────────────────────────────────────

static bool supports(garb_reading_t how, garb_parsed_t *owner, const garb_value_t *v, int32_t n);

// Whether a value reads the `style` attribute's text: attr(style) anywhere
// in it, a function's arguments included.
static bool values_read_style(const garb_value_t *v, int32_t n)
{
    for (int32_t i = 0; i < n; i++) {
        if (v[i].kind == GARB_FUNCTION && v[i].len == 4 && (v[i].text[0] | 0x20) == 'a' &&
            (v[i].text[1] | 0x20) == 't' && (v[i].text[2] | 0x20) == 't' && (v[i].text[3] | 0x20) == 'r') {
            const garb_value_t *a = v[i].children;
            int32_t k = 0;
            while (k < v[i].nchildren && a[k].kind == GARB_WHITESPACE)
                k++;
            if (k < v[i].nchildren && a[k].kind == GARB_IDENT && a[k].len == 5) {
                bool style = true;
                for (int32_t j = 0; j < 5; j++)
                    style = style && (a[k].text[j] | 0x20) == "style"[j];
                if (style)
                    return true;
            }
        }
        if (values_read_style(v[i].children, v[i].nchildren))
            return true;
    }
    return false;
}

// Reads one declaration for a rule: kept raw when custom or pending, else
// its longhands, else dropped (invalid, or a property not read here).
static bool read_decl(garb_cascade_t *c, garb_parsed_t *owner, const garb_decl_t *d, Decl *out)
{
    os64_memset(out, 0, sizeof(*out));
    // `content: attr(style)` reads the attribute an inline style write
    // changes (garb_cascade_reads_style_attribute).
    if (values_read_style(d->value, d->nvalue)) {
        c->dependencies.style_attribute = true;
        c->style_bare = true;
    }
    out->decl = *d;
    out->important = d->important;
    if (garb_decl_is_custom(d)) {
        out->custom = true;
        return true;
    }
    garb_prop_t props[GARB_SETS_MAX];
    if (garb_decl_has_var(d)) {
        if (prop_longhands(d->name, d->len, c->env.modern_off, props) == 0) {
            skipped(c, GARB_SKIP_PROPERTY, d->name, d->len);
            return false;
        }
        out->pending = true;
        return true;
    }
    garb_set_t sets[GARB_SETS_MAX];
    int32_t n = garb_read_declaration(owner, d, reading(c), sets);
    if (n <= 0) {
        // Which it was is asked only of what was dropped: the property
        // table is a walk.
        if (prop_longhands(d->name, d->len, c->env.modern_off, props) == 0)
            skipped(c, GARB_SKIP_PROPERTY, d->name, d->len);
        else if (only_words(d->value, d->nvalue))
            skipped_value(c, d->name, d->len, d->value, d->nvalue);
        else
            skipped_functions(c, d->value, d->nvalue, 0);
        return false;
    }
    // Read, but laid out as something else (props.c's kApproximated): asked
    // for, and not given, as surely as a value dropped.
    for (int32_t k = 0; k < n; k++) {
        const char *word = garb_set_approximation(&sets[k]);
        if (word != NULL) {
            const char *prop = garb_prop_name(sets[k].prop);
            garb_value_t one = {.kind = GARB_IDENT, .text = word, .len = (uint32_t)os64_strlen(word)};
            skipped_value(c, prop, os64_strlen(prop), &one, 1);
        }
    }
    out->sets = alloc(c, (size_t)n * sizeof(garb_set_t));
    if (out->sets == NULL)
        return false;
    os64_memcpy(out->sets, sets, (size_t)n * sizeof(garb_set_t));
    out->nsets = n;
    return true;
}

static void add_style_rule(garb_cascade_t *c, garb_parsed_t *owner, const garb_rule_t *r,
                           uint32_t *order, int32_t sheet)
{
    garb_selectors_t *sel = garb_selectors_parse(owner, r->prelude, r->nprelude, c->quirks,
                                                 c->env.modern_off);
    if (sel == NULL)
        return;                         // an invalid selector drops the rule
    garb_item_t *items;
    int32_t nitems;
    (void)garb_items_of(owner, r->block, r->nblock, &items, &nitems);
    if (!grow(c, (void **)&c->rules, c->nrules, &c->rulecap, sizeof(Rule)))
        return;
    Rule *rule = &c->rules[c->nrules];
    os64_memset(rule, 0, sizeof(*rule));
    rule->sel = sel;
    rule->prelude = r->prelude;
    rule->nprelude = r->nprelude;
    rule->order = (*order)++;
    rule->sheet = sheet;
    rule->decls = alloc(c, (size_t)(nitems > 0 ? nitems : 1) * sizeof(Decl));
    if (rule->decls == NULL)
        return;
    // Nested rules (CSS Nesting) are booked: only the declarations count.
    for (int32_t i = 0; i < nitems; i++)
        if (items[i].kind == GARB_ITEM_DECL &&
            read_decl(c, owner, &items[i].decl, &rule->decls[rule->ndecls]))
            rule->ndecls++;
    if (rule->ndecls == 0)
        return;
    int32_t index = c->nrules++;
    for (int32_t s = 0; s < garb_selectors_count(sel); s++) {
        garb_pseudo_t pseudo = garb_selector_pseudo(sel, s);
        // A host's pseudo-elements from its shadow tree (`:host::before`)
        // are booked in DEBTS: only `:host` itself is filed for it.
        if (garb_selector_names_host(sel, s)) {
            if (pseudo == GARB_PSEUDO_NONE)
                file_selector(c, index, s, TARGET_HOST);
        } else if (pseudo == GARB_PSEUDO_SLOTTED)
            file_selector(c, index, s, TARGET_SLOTTED);
        else if (pseudo == GARB_PSEUDO_NONE)
            file_selector(c, index, s, TARGET_ELEMENT);
        else if (pseudo == GARB_PSEUDO_BEFORE)
            file_selector(c, index, s, TARGET_BEFORE);
        else if (pseudo == GARB_PSEUDO_AFTER)
            file_selector(c, index, s, TARGET_AFTER);
    }
}

static bool at(const garb_rule_t *r, const char *name)
{
    return r->at && ieq(r->name, r->len, name);
}

// ── @keyframes (Animations 1 § 3, Animations 2 § 3.2) ──────────────────

// The words a <custom-ident> naming keyframes may not be.
static const char *const kNotKeyframesName[] = {"none", "inherit", "initial", "unset", "revert",
                                                "revert-layer", "default", NULL};

// A @keyframes rule's name: one identifier that is none of those, or one
// string that is not empty — as written either way.
static bool keyframes_name(const garb_rule_t *r, const char **name, size_t *len)
{
    const garb_value_t *t = NULL;
    for (int32_t i = 0; i < r->nprelude; i++) {
        if (r->prelude[i].kind == GARB_WHITESPACE)
            continue;
        if (t != NULL)
            return false;
        t = &r->prelude[i];
    }
    if (t == NULL)
        return false;
    if (t->kind == GARB_IDENT) {
        for (int k = 0; kNotKeyframesName[k] != NULL; k++)
            if (ieq(t->text, t->len, kNotKeyframesName[k]))
                return false;
    } else if (t->kind != GARB_STRING || t->len == 0) {
        return false;
    }
    *name = t->text;
    *len = t->len;
    return true;
}

// A keyframe block's offsets: `from`, `to` or a percentage in [0%, 100%],
// comma-separated, as fractions; 0 for a selector anything else is
// written in, which drops the block.
static int32_t key_offsets(garb_cascade_t *c, const garb_rule_t *r, double **out)
{
    int32_t most = 1;
    for (int32_t i = 0; i < r->nprelude; i++)
        most += r->prelude[i].kind == GARB_COMMA;
    double *at_ = alloc(c, (size_t)most * sizeof(double));
    if (at_ == NULL)
        return 0;
    int32_t n = 0;
    bool want = true;           // an offset next, not a comma
    for (int32_t i = 0; i < r->nprelude; i++) {
        const garb_value_t *t = &r->prelude[i];
        if (t->kind == GARB_WHITESPACE)
            continue;
        if (!want) {
            if (t->kind != GARB_COMMA)
                return 0;
            want = true;
            continue;
        }
        if (t->kind == GARB_IDENT && ieq(t->text, t->len, "from"))
            at_[n] = 0;
        else if (t->kind == GARB_IDENT && ieq(t->text, t->len, "to"))
            at_[n] = 1;
        else if (t->kind == GARB_PERCENTAGE && t->number >= 0 && t->number <= 100)
            at_[n] = t->number / 100;
        else
            return 0;
        n++;
        want = false;
    }
    if (want)
        return 0;               // empty, or ending in a comma
    *out = at_;
    return n;
}

// Whether a keyframe drops a declaration of `name`: every longhand of
// Animations 1 is dropped there but its timing function, which is the
// keyframe's own easing (§ 3, § 3.1).
static bool key_dropped(const garb_cascade_t *c, const garb_decl_t *d, bool *easing)
{
    garb_prop_t props[GARB_SETS_MAX];
    int32_t n = prop_longhands(d->name, d->len, c->env.modern_off, props);
    *easing = n == 1 && props[0] == GARB_ANIMATION_TIMING_FUNCTION;
    for (int32_t k = 0; k < n && !*easing; k++)
        if (props[k] >= GARB_ANIMATION_NAME && props[k] <= GARB_ANIMATION_FILL_MODE)
            return true;
    return false;
}

// A @keyframes rule, kept; the last of a name in cascade order wins whole
// (Animations 1 § 3), once every sheet is read (keyframes_settle). Its
// blocks' declarations are read as a style rule's are, and a block's last
// timing function kept apart from them as its easing; `!important` ones,
// and the custom properties a keyframe would set (PILE3.md § Animations,
// Booked), are not kept.
static void add_keyframes(garb_cascade_t *c, garb_parsed_t *owner, const garb_rule_t *r,
                          int32_t sheet)
{
    const char *name;
    size_t len;
    if (!r->has_block || !keyframes_name(r, &name, &len))
        return;
    garb_item_t *inner;
    int32_t ninner;
    (void)garb_rules_of(owner, r->block, r->nblock, &inner, &ninner);
    KeyBlock *blocks = alloc(c, (size_t)(ninner > 0 ? ninner : 1) * sizeof(KeyBlock));
    if (blocks == NULL)
        return;
    int32_t nblocks = 0;
    bool pending = false;
    for (int32_t i = 0; i < ninner; i++) {
        if (inner[i].kind != GARB_ITEM_RULE || inner[i].rule->at || !inner[i].rule->has_block)
            continue;
        const garb_rule_t *kr = inner[i].rule;
        KeyBlock *b = &blocks[nblocks];
        os64_memset(b, 0, sizeof(*b));
        if ((b->noffsets = key_offsets(c, kr, &b->offsets)) == 0)
            continue;
        garb_item_t *items;
        int32_t nitems;
        (void)garb_items_of(owner, kr->block, kr->nblock, &items, &nitems);
        b->decls = alloc(c, (size_t)(nitems > 0 ? nitems : 1) * sizeof(Decl));
        if (b->decls == NULL)
            return;
        // The easing goes first: one of each block's is kept, the last.
        Decl easing = {0};
        bool has_easing = false;
        for (int32_t j = 0; j < nitems; j++) {
            const garb_decl_t *d = &items[j].decl;
            bool is_easing;
            if (items[j].kind != GARB_ITEM_DECL || d->important || garb_decl_is_custom(d) ||
                key_dropped(c, d, &is_easing))
                continue;
            Decl read;
            if (!read_decl(c, owner, d, &read))
                continue;
            pending |= read.pending;
            if (is_easing) {
                easing = read;
                has_easing = true;
            } else {
                b->decls[b->ndecls++] = read;
            }
        }
        if (has_easing) {
            os64_memmove(b->decls + 1, b->decls, (size_t)b->ndecls * sizeof(Decl));
            b->decls[0] = easing;
            b->ndecls++;
            b->easing = true;
        }
        nblocks++;
    }
    if (!grow(c, (void **)&c->keyframes, c->nkeyframes, &c->keyframecap, sizeof(Keyframes)))
        return;
    c->keyframes[c->nkeyframes++] = (Keyframes){name, len, blocks, nblocks, sheet, pending, NULL, 0};
}

// A sheet's rules, `items`, joined to the cascade. `owner` keeps what is
// read of them, through every helper below it: the cascade's reading of
// its sheets (`reads`), never the sheet's own parse.
static void add_rules(garb_cascade_t *c, garb_parsed_t *owner, const garb_item_t *items,
                      int32_t n, uint32_t *order, int depth, int32_t sheet)
{
    if (depth > NEST_MAX) {
        c->incomplete = true;
        return;
    }
    for (int32_t i = 0; i < n; i++) {
        if (items[i].kind != GARB_ITEM_RULE)
            continue;
        const garb_rule_t *r = items[i].rule;
        if (!r->at) {
            if (r->has_block)
                add_style_rule(c, owner, r, order, sheet);
            continue;
        }
        // The grouping rules whose contents join the sheet when they hold.
        // @layer's contents are cascaded as if unlayered (booked: layers).
        // @keyframes is kept apart (add_keyframes). Every other at-rule is
        // passed over here, @import among them: its sheet is the face's to
        // fetch, and joins at its place (GARB.md, G4).
        bool group = at(r, "media") ? garb_media_matches(r->prelude, r->nprelude, c->env)
                   : at(r, "supports") ? supports(reading(c), owner, r->prelude, r->nprelude)
                   : at(r, "layer") && r->has_block;
        // What is read elsewhere is not passed over: @import's sheet is the
        // face's, @charset was the decoder's.
        if (at(r, "font-face"))
            add_face(c, owner, r, sheet);
        else if (at(r, "keyframes") || at(r, "-webkit-keyframes"))
            add_keyframes(c, owner, r, sheet);
        else if (!at(r, "media") && !at(r, "supports") && !at(r, "layer") && !at(r, "import") &&
                 !at(r, "charset"))
            skipped(c, GARB_SKIP_AT_RULE, r->name, r->len);
        if (!group || !r->has_block)
            continue;
        garb_item_t *inner;
        int32_t ninner;
        (void)garb_rules_of(owner, r->block, r->nblock, &inner, &ninner);
        add_rules(c, owner, inner, ninner, order, depth + 1, sheet);
    }
}

// ── @supports (Conditional Rules 3 § 2) ─────────────────────────────────

typedef struct {
    const garb_value_t *v;
    int32_t n, i;
} SCur;

static const garb_value_t *speek(SCur *s)
{
    while (s->i < s->n && s->v[s->i].kind == GARB_WHITESPACE)
        s->i++;
    return s->i < s->n ? &s->v[s->i] : NULL;
}

static bool sword(const garb_value_t *t, const char *w)
{
    return t != NULL && t->kind == GARB_IDENT && ieq(t->text, t->len, w);
}

static int supports_condition(garb_reading_t how, garb_parsed_t *owner, SCur *s);

// Whether component values hold no closing bracket with nothing open, no
// bad string and no bad URL, at any depth (a function's arguments and a
// block's contents included), and, at the top only, no `;` and no `!` (a
// trailing `!important` is the declaration's, not its value's: the parser
// has taken it off). The parser bounds the depth this walks
// (GARB_DEPTH_MAX).
static bool declaration_tokens(const garb_value_t *v, int32_t n, bool top)
{
    for (int32_t k = 0; k < n; k++) {
        garb_kind_t kind = v[k].kind;
        bool bang = kind == GARB_DELIM && v[k].len == 1 && v[k].text[0] == '!';
        if ((top && (kind == GARB_SEMICOLON || bang)) || kind == GARB_UNMATCHED || kind == GARB_BAD_STRING ||
            kind == GARB_BAD_URL)
            return false;
        if ((kind == GARB_FUNCTION || kind == GARB_BLOCK) &&
            !declaration_tokens(v[k].children, v[k].nchildren, false))
            return false;
    }
    return true;
}

// Whether a custom property's value is a <declaration-value> (Syntax 3
// § 8.2): declaration_tokens over its value.
static bool declaration_value(const garb_decl_t *d)
{
    return declaration_tokens(d->value, d->nvalue, true);
}

// A declaration, as @supports and CSS.supports ask after one: supported if
// its property is read here, its value fits (a custom property's when it
// is a <declaration-value>), and what it says is laid out as it says — a
// value read but laid out as something else is not supported, or a page
// would be told not to use the fallback it wrote for that.
static bool supports_declaration(garb_reading_t how, garb_parsed_t *owner, const garb_decl_t *d)
{
    if (garb_decl_is_custom(d))
        return declaration_value(d);
    if (garb_decl_has_var(d)) {
        garb_prop_t props[GARB_SETS_MAX];
        return prop_longhands(d->name, d->len, how.modern_off, props) > 0;
    }
    garb_set_t sets[GARB_SETS_MAX];
    int32_t read = garb_read_declaration(owner, d, how, sets);
    for (int32_t k = 0; k < read; k++)
        if (garb_set_approximated(&sets[k]))
            return false;
    return read > 0;
}

// A condition's answer is three-valued, because the grammar can fail where
// a test is merely false: S_INVALID, which makes the whole condition false
// (a `not` before it included), or false or true. Conditional 3 § 2.
enum { S_INVALID = -1, S_FALSE = 0, S_TRUE = 1 };

// ( condition ), ( declaration ), selector( … ): <supports-in-parens>. A
// parenthesis or a function that is neither is <general-enclosed>, valid
// and false; anything else (nothing, a bare word, another bracket) is not
// one at all.
static int supports_in_parens(garb_reading_t how, garb_parsed_t *owner, SCur *s)
{
    const garb_value_t *t = speek(s);
    if (t == NULL)
        return S_INVALID;
    s->i++;
    if (t->kind == GARB_FUNCTION && ieq(t->text, t->len, "selector")) {
        return garb_selectors_parse(owner, t->children, t->nchildren,
                                    how.quirks ? OS64_HTML_QUIRKS : OS64_HTML_NO_QUIRKS, how.modern_off) != NULL
                   ? S_TRUE : S_FALSE;
    }
    if (t->kind == GARB_FUNCTION)
        return S_FALSE;
    if (t->kind != GARB_BLOCK || t->open != '(')
        return S_INVALID;
    SCur inner = {t->children, t->nchildren, 0};
    const garb_value_t *first = speek(&inner);
    if (first != NULL && (first->kind == GARB_BLOCK || sword(first, "not") ||
                          first->kind == GARB_FUNCTION)) {
        // Not a condition after all is a parenthesis of anything: false.
        int r = supports_condition(how, owner, &inner);
        return r == S_TRUE && speek(&inner) == NULL ? S_TRUE : S_FALSE;
    }
    garb_item_t *items;
    int32_t n;
    if (!garb_items_of(owner, t->children, t->nchildren, &items, &n) || n != 1 ||
        items[0].kind != GARB_ITEM_DECL)
        return S_FALSE;
    return supports_declaration(how, owner, &items[0].decl) ? S_TRUE : S_FALSE;
}

static int supports_condition(garb_reading_t how, garb_parsed_t *owner, SCur *s)
{
    if (sword(speek(s), "not")) {
        s->i++;
        int r = supports_in_parens(how, owner, s);
        return r == S_INVALID ? S_INVALID : r == S_TRUE ? S_FALSE : S_TRUE;
    }
    int r = supports_in_parens(how, owner, s);
    if (r == S_INVALID)
        return S_INVALID;
    const garb_value_t *t = speek(s);
    bool ands = sword(t, "and"), ors = sword(t, "or");
    while ((ands && sword(speek(s), "and")) || (ors && sword(speek(s), "or"))) {
        s->i++;
        int next = supports_in_parens(how, owner, s);
        if (next == S_INVALID)
            return S_INVALID;
        r = ands ? (r == S_TRUE && next == S_TRUE ? S_TRUE : S_FALSE)
                 : (r == S_TRUE || next == S_TRUE ? S_TRUE : S_FALSE);
    }
    return r;
}

static bool supports(garb_reading_t how, garb_parsed_t *owner, const garb_value_t *v, int32_t n)
{
    SCur s = {v, n, 0};
    int r = supports_condition(how, owner, &s);
    return r == S_TRUE && speek(&s) == NULL;
}

// One condition text judged (garb_supports): 1 or 0, or -1 when the parse
// had no memory. A text past the parser's depth or size answers 0, as the
// same condition in a sheet does (its rule is read short and does not hold).
static int32_t supports_text(const char *text, size_t len, garb_reading_t how)
{
    garb_parsed_t parsed = {0};
    garb_status_t status = garb_parse_values(text, len, &parsed);
    int32_t r = status == GARB_NO_MEMORY ? -1
              : status != GARB_OK || parsed.incomplete ? 0
              : supports(how, &parsed, parsed.values, parsed.nvalues) ? 1 : 0;
    garb_free(&parsed);
    return r;
}

int32_t garb_supports(const char *text, size_t len, garb_reading_t how)
{
    int32_t r = supports_text(text, len, how);
    if (r != 0)
        return r;
    // The second form: the text in parentheses, which is how a bare
    // declaration ("display: grid") is a condition.
    if (len > SIZE_MAX - 3)
        return -1;
    char *wrapped = os64_malloc(len + 3);
    if (wrapped == NULL)
        return -1;
    wrapped[0] = '(';
    if (len != 0)
        os64_memcpy(wrapped + 1, text, len);
    wrapped[len + 1] = ')';
    wrapped[len + 2] = '\0';
    r = supports_text(wrapped, len + 2, how);
    os64_free(wrapped);
    return r;
}

int32_t garb_supports_property(const char *property, size_t plen, const char *value, size_t vlen,
                               garb_reading_t how)
{
    // The name (§ 6.1): a custom property's is a string test, `--` and
    // anything after it, so its value is read under a stand-in name; any
    // other must be a property's name, all of it as written, read as one.
    bool custom = plen >= 2 && property[0] == '-' && property[1] == '-';
    const char *name = custom ? "--x" : property;
    size_t nlen = custom ? 3 : plen;
    if (nlen > SIZE_MAX - 3 || vlen > SIZE_MAX - 3 - nlen)
        return -1;
    char *text = os64_malloc(nlen + vlen + 3);
    if (text == NULL)
        return -1;
    os64_memcpy(text, name, nlen);
    text[nlen] = ':';
    if (vlen != 0)
        os64_memcpy(text + nlen + 1, value, vlen);
    text[nlen + 1 + vlen] = '\0';
    garb_parsed_t parsed = {0};
    garb_status_t status = garb_parse_one_declaration(text, nlen + 1 + vlen, &parsed);
    const garb_decl_t *d = &parsed.decl;
    int32_t r;
    if (status == GARB_NO_MEMORY)
        r = -1;
    else if (status != GARB_OK || parsed.incomplete || d->important)
        r = 0;      // a value is the property's grammar alone, never `!important`
    else if (custom)
        r = garb_decl_is_custom(d) && supports_declaration(how, &parsed, d) ? 1 : 0;
    else
        r = d->len == plen && !garb_decl_is_custom(d) && supports_declaration(how, &parsed, d) ? 1 : 0;
    garb_free(&parsed);
    os64_free(text);
    return r;
}

// ── @import (Cascade 4 § 2.1) ───────────────────────────────────────────

// `@import <url> [layer | layer(…)]? [supports(…)]? <media-query-list>?`:
// false for a prelude that is not one.
static bool read_import(const garb_rule_t *r, garb_import_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    VCur c = {r->prelude, r->nprelude, 0};
    const garb_value_t *t = vc_peek(&c);
    garb_val_t url;
    if (t != NULL && t->kind == GARB_STRING) {
        out->url = t->text;
        out->len = t->len;
        c.i++;
    } else if (vc_url(&c, &url)) {
        out->url = url.text;
        out->len = url.len;
    } else {
        return false;
    }
    // Layers are cascaded as unlayered (GARB.md § Booked): the name is
    // passed over.
    t = vc_peek(&c);
    if (t != NULL && ((t->kind == GARB_IDENT && ieq(t->text, t->len, "layer")) ||
                      (t->kind == GARB_FUNCTION && ieq(t->text, t->len, "layer"))))
        c.i++;
    t = vc_peek(&c);
    if (t != NULL && t->kind == GARB_FUNCTION && ieq(t->text, t->len, "supports")) {
        out->has_supports = true;
        out->supports = t->children;
        out->nsupports = t->nchildren;
        c.i++;
    }
    (void)vc_peek(&c);
    out->media = c.i < c.n ? &c.v[c.i] : NULL;
    out->nmedia = c.n - c.i;
    return true;
}

int32_t garb_sheet_imports(const garb_parsed_t *sheet, garb_import_t *out, int32_t cap)
{
    int32_t n = 0;
    for (int32_t i = 0; sheet != NULL && i < sheet->nitems; i++) {
        if (sheet->items[i].kind != GARB_ITEM_RULE)
            continue;
        const garb_rule_t *r = sheet->items[i].rule;
        if (at(r, "charset") || (at(r, "layer") && !r->has_block))
            continue;
        if (!at(r, "import"))
            break;
        garb_import_t one;
        if (r->has_block || !read_import(r, &one))
            continue;
        if (n < cap)
            out[n] = one;
        n++;
    }
    return n;
}

// Whether sheet `s` of the list applies: its element's media, and for an
// imported one its @import's media and supports() and its importer's.
static bool sheet_applies(garb_cascade_t *c, const garb_sheet_in_t *sheets, int32_t n,
                          int32_t s, int depth)
{
    const garb_sheet_in_t *in = &sheets[s];
    if (in->sheet == NULL || !garb_media_text_matches(in->media, c->env))
        return false;
    if (in->via == NULL)
        return true;
    if (depth > NEST_MAX || in->parent <= s || in->parent >= n) {
        c->incomplete = true;
        return false;
    }
    const garb_import_t *im = in->via;
    if (!garb_media_matches(im->media, im->nmedia, c->env))
        return false;
    if (im->has_supports) {
        // supports()'s argument is a condition or a declaration: read as
        // the inside of a parenthesis, which is where both may stand, into
        // the cascade's own arena: a cascade never writes to a sheet.
        garb_value_t paren = {0};
        paren.kind = GARB_BLOCK;
        paren.open = '(';
        paren.children = (garb_value_t *)im->supports;
        paren.nchildren = im->nsupports;
        if (!supports(reading(c), &c->self, &paren, 1))
            return false;
    }
    return sheet_applies(c, sheets, n, in->parent, depth + 1);
}

// ── Where an element's answer is kept ───────────────────────────────────

static uint32_t hash_ptr(const void *p)
{
    uintptr_t v = (uintptr_t)p;
    v ^= v >> 17;
    v *= 0xed5ad4bbu;
    v ^= v >> 11;
    return (uint32_t)v;
}

// The document pin predates removals: an address cannot be recycled while
// its slot exists. Removed slots are erased before rotating that pin.
static bool slots_grow(garb_cascade_t *c)
{
    if (c->slotcap > UINT32_MAX / 2) return false;
    uint32_t cap = c->slotcap ? c->slotcap * 2 : 64;
    Slot *slots = os64_calloc(cap, sizeof(*slots));
    if (!slots) return false;
    uint32_t w = c->has_width;
    uint64_t *words = w ? os64_calloc((size_t)cap * w, sizeof(*words)) : NULL;
    if (w && !words) { os64_free(slots); return false; }
    for (uint32_t i = 0; i < c->slotcap; ++i) if (c->slots[i].node) {
        uint32_t at = hash_ptr(c->slots[i].node) & (cap - 1);
        while (slots[at].node) at = (at + 1) & (cap - 1);
        slots[at] = c->slots[i];
        for (uint32_t k = 0; k < w; k++) words[(size_t)at * w + k] = c->has_words[(size_t)i * w + k];
    }
    os64_free(c->slots);
    os64_free(c->has_words);
    c->slots = slots; c->slotcap = cap; c->has_words = words;
    return true;
}
// The slot's row of anchor bits (has_words); NULL while there is none.
static uint64_t *has_row(const garb_cascade_t *c, const Slot *s)
{
    return c->has_width ? c->has_words + (size_t)(s - c->slots) * c->has_width : NULL;
}
static Slot *slot_for(const garb_cascade_t *c, const os64_html_node_t *node, bool create)
{
    if (!c->slotcap) return NULL;
    uint32_t h = hash_ptr(node) & (c->slotcap - 1);
    while (c->slots[h].node) {
        if (c->slots[h].node == node) return &c->slots[h];
        h = (h + 1) & (c->slotcap - 1);
    }
    if (!create) return NULL;
    garb_cascade_t *write = (garb_cascade_t *)c;
    if (c->slotcount >= c->slotcap / 2) {
        if (!slots_grow(write)) { write->incomplete = true; return NULL; }
        h = hash_ptr(node) & (c->slotcap - 1);
        while (c->slots[h].node) h = (h + 1) & (c->slotcap - 1);
    }
    Slot *s = &write->slots[h];
    s->node = node; ++write->slotcount;
    return s;
}
static void slot_remove(garb_cascade_t *c, const os64_html_node_t *node)
{
    Slot *s = slot_for(c, node, false);
    if (!s) return;
    uint32_t hole = (uint32_t)(s - c->slots), mask = c->slotcap - 1;
    // Backshift the cluster: stopping at an empty slot remains a valid
    // lookup rule without accumulating tombstones after repeated moves.
    for (uint32_t at = (hole + 1) & mask; c->slots[at].node; at = (at + 1) & mask) {
        uint32_t home = hash_ptr(c->slots[at].node) & mask;
        if (((at - home) & mask) >= ((at - hole) & mask)) {
            c->slots[hole] = c->slots[at];
            for (uint32_t k = 0; k < c->has_width; k++)
                c->has_words[(size_t)hole * c->has_width + k] = c->has_words[(size_t)at * c->has_width + k];
            hole = at;
        }
    }
    c->slots[hole] = (Slot){0}; --c->slotcount;
    for (uint32_t k = 0; k < c->has_width; k++) c->has_words[(size_t)hole * c->has_width + k] = 0;
}

// What each anchor answers at `x`, into `out` (bit i % 64 of word i / 64
// for anchors[i]); nothing for a cascade that will take no update (no row
// of bits, or short), which never compares them. Filled in for every
// element when it is styled, one test per anchor, which is why
// garb_anchor_matches tries an anchor's filters before its :has(): a test
// costs a compare at an element its filters turn away (DEBTS: indexing the
// anchors by their filters, for sheets with thousands).
static void anchor_bits(const garb_cascade_t *c, const os64_html_node_t *x, uint64_t *out)
{
    if (out == NULL || c->incomplete) return;
    for (uint32_t k = 0; k < c->has_width; k++) out[k] = 0;
    for (int32_t i = 0; i < c->nanchors; i++)
        if (garb_anchor_matches(&c->anchors[i], x)) out[i / 64] |= (uint64_t)1 << (i % 64);
}

// ── var() and env() (Custom Properties 1 § 3, Environment Variables 1) ─

typedef struct {
    garb_cascade_t *c;
    const CustomMap *map;
    int depth;
    int32_t budget;             // values left to produce: the billion-laughs cut
} Sub;

static const garb_value_t *custom_value(Sub *s, const char *name, size_t len, int32_t *n);

typedef struct {
    garb_value_t *v;
    int32_t n, cap;
} Vec;

static bool push(Sub *s, Vec *out, const garb_value_t *v)
{
    if (--s->budget < 0)
        return false;
    if (out->n == out->cap) {
        int32_t cap = out->cap ? out->cap * 2 : 16;
        garb_value_t *next = os64_arena_alloc(s->c->heap->arena, (size_t)cap * sizeof(*next));
        if (next == NULL) {
            s->c->incomplete = true;
            return false;
        }
        if (out->n > 0)
            os64_memcpy(next, out->v, (size_t)out->n * sizeof(*next));
        out->v = next;
        out->cap = cap;
    }
    out->v[out->n++] = *v;
    return true;
}

static bool substitute(Sub *s, const garb_value_t *in, int32_t n, Vec *out);

// A var() or env(): its name, and what stands in its place — the value
// named, else its fallback, else nothing (invalid).
static bool substitute_fn(Sub *s, const garb_value_t *f, Vec *out)
{
    bool env = ieq(f->text, f->len, "env");
    int32_t i = 0;
    while (i < f->nchildren && f->children[i].kind == GARB_WHITESPACE)
        i++;
    if (i == f->nchildren || f->children[i].kind != GARB_IDENT)
        return false;
    const garb_value_t *name = &f->children[i++];
    if (!env && !(name->len >= 2 && name->text[0] == '-' && name->text[1] == '-'))
        return false;
    while (i < f->nchildren && f->children[i].kind == GARB_WHITESPACE)
        i++;
    bool has_fallback = i < f->nchildren && f->children[i].kind == GARB_COMMA;
    if (i < f->nchildren && !has_fallback)
        return false;
    if (!env) {
        int32_t vn = 0;
        const garb_value_t *v = custom_value(s, name->text, name->len, &vn);
        if (v != NULL) {
            for (int32_t k = 0; k < vn; k++)
                if (!push(s, out, &v[k]))
                    return false;
            return true;
        }
    }
    // No such variable (env() names none on this machine): the fallback.
    if (!has_fallback)
        return false;
    return substitute(s, f->children + i + 1, f->nchildren - i - 1, out);
}

static bool substitute(Sub *s, const garb_value_t *in, int32_t n, Vec *out)
{
    if (++s->depth > VAR_DEPTH_MAX) {
        s->c->incomplete = true;
        return false;
    }
    bool ok = true;
    for (int32_t k = 0; k < n && ok; k++) {
        const garb_value_t *v = &in[k];
        if (v->kind == GARB_FUNCTION && (ieq(v->text, v->len, "var") || ieq(v->text, v->len, "env"))) {
            ok = substitute_fn(s, v, out);
        } else if (v->kind == GARB_FUNCTION || v->kind == GARB_BLOCK) {
            Vec kids = {0};
            garb_value_t copy = *v;
            ok = substitute(s, v->children, v->nchildren, &kids);
            copy.children = kids.v;
            copy.nchildren = kids.n;
            ok = ok && push(s, out, &copy);
        } else {
            ok = push(s, out, v);
        }
    }
    s->depth--;
    return ok;
}

// A custom property's value as the element sees it, its own var()s
// replaced; NULL for the guaranteed-invalid value (undeclared, or in a
// cycle, or its replacement failed).
// A custom property declared with nothing (`--on: ;`) holds the EMPTY value,
// which is a value: var() substitutes nothing for it, where the
// guaranteed-invalid value (NULL) takes the fallback. The "space toggle"
// idiom is exactly that difference, so the empty one is this, never NULL.
static const garb_value_t kEmptyValue[1];

static const garb_value_t *custom_value(Sub *s, const char *name, size_t len, int32_t *n)
{
    for (const CustomMap *m = s->map; m != NULL; m = m->parent)
        for (int32_t k = m->n - 1; k >= 0; k--) {
            Custom *cu = &m->own[k];
            if (cu->len != len || os64_memcmp(cu->name, name, len) != 0)
                continue;
            if (cu->state == 1 || cu->state == 3 || cu->value == NULL)
                return NULL;            // a cycle, or invalid
            if (cu->state == 0) {
                cu->state = 1;
                Vec out = {0};
                Sub inner = {s->c, m, s->depth, VAR_VALUES_MAX};
                bool ok = substitute(&inner, cu->value, cu->nvalue, &out);
                if (ok) {
                    cu->value = out.n > 0 ? out.v : kEmptyValue;
                    cu->nvalue = out.n;
                }
                cu->state = ok ? 2 : 3;
                if (!ok)
                    return NULL;
            }
            *n = cu->nvalue;
            return cu->value;
        }
    return NULL;
}

// ── An animation's keyframes, read ──────────────────────────────────────

// The longhands a keyframe's declaration sets, into `out`: its own when it
// holds no var(); else, for an element (`own`), its value with each var()
// replaced against `customs` and then read — and nothing when that fails,
// since a keyframe's invalid value is as if that keyframe did not set the
// property (Animations 1 § 3) — or nothing at all without one.
static int32_t key_sets(garb_cascade_t *c, const Decl *d, const CustomMap *customs, bool own,
                        garb_set_t *out)
{
    if (!d->pending) {
        os64_memcpy(out, d->sets, (size_t)d->nsets * sizeof(garb_set_t));
        return d->nsets;
    }
    if (!own)
        return 0;
    Sub s = {c, customs, 0, VAR_VALUES_MAX};
    Vec v = {0};
    if (!substitute(&s, d->decl.value, d->decl.nvalue, &v))
        return 0;
    garb_decl_t read = d->decl;
    read.value = v.v;
    read.nvalue = v.n;
    int32_t n = garb_read_declaration(&c->self, &read, reading(c), out);
    return n > 0 ? n : 0;
}

// A stable sort of `n` items `size` bytes each, by `cmp`: a merge through
// `tmp`, room for n more — n log n, however many a page's CSS makes.
static void merge_sort(void *v, void *tmp, int32_t n, size_t size,
                       int (*cmp)(const void *, const void *))
{
    if (n < 2)
        return;
    int32_t h = n / 2;
    char *p = v, *t = tmp;
    merge_sort(p, t, h, size, cmp);
    merge_sort(p + (size_t)h * size, t, n - h, size, cmp);
    int32_t i = 0, j = h, k = 0;
    while (i < h && j < n) {
        const char *a = p + (size_t)i * size, *b = p + (size_t)j * size;
        bool right = cmp(b, a) < 0;
        os64_memcpy(t + (size_t)k++ * size, right ? b : a, size);
        right ? j++ : i++;
    }
    while (i < h)
        os64_memcpy(t + (size_t)k++ * size, p + (size_t)i++ * size, size);
    while (j < n)
        os64_memcpy(t + (size_t)k++ * size, p + (size_t)j++ * size, size);
    os64_memcpy(p, t, (size_t)n * size);
}

static int name_cmp(const char *a, size_t la, const char *b, size_t lb)
{
    int c = os64_memcmp(a, b, la < lb ? la : lb);
    return c != 0 ? c : (la > lb) - (la < lb);
}

// An easing as keyframes are grouped by it (Chrome's answer): a keyword
// curve by its name — `ease` is another easing than cubic-bezier(0.25,
// 0.1, 0.25, 1); cubic-bezier() by its four numbers, each compared as the
// number it is and not as printed (garb_val_dump rounds to six places,
// which made cubic-bezier(0, 1e-7, 1, 1) and (0, 2e-7, 1, 1) one); and
// steps by their count and where they jump, however written — step-start
// and step-end are steps(1, start) and steps(1, end), and jump-start and
// jump-end are start and end.
typedef struct {
    const char *word;
    double n[4];
    const char *jump;
} Easing;

static Easing easing_canon(const garb_val_t *v)
{
    Easing e = {v->keyword != NULL ? v->keyword : "", {0, 0, 0, 0}, ""};
    if (v->kind == GARB_V_KEYWORD && (os64_streq(e.word, "step-start") || os64_streq(e.word, "step-end"))) {
        e.jump = e.word + 5;
        e.word = "steps";
        e.n[0] = 1;
    } else if (v->kind == GARB_V_FUNCTION && os64_streq(e.word, "steps")) {
        e.n[0] = v->nitems > 0 ? v->items[0].number : 0;
        e.jump = v->nitems > 1 && v->items[1].keyword != NULL ? v->items[1].keyword : "end";
        if (os64_streq(e.jump, "jump-start") || os64_streq(e.jump, "jump-end"))
            e.jump += 5;
    } else if (v->kind == GARB_V_FUNCTION) {
        for (int32_t i = 0; i < v->nitems && i < 4; i++)
            e.n[i] = v->items[i].number;
    }
    return e;
}

// Two easings in a total order, equal when they are one easing
// (easing_canon); no easing sorts first.
static int easing_cmp(const garb_val_t *a, const garb_val_t *b)
{
    if (a == NULL || b == NULL)
        return (a != NULL) - (b != NULL);
    Easing x = easing_canon(a), y = easing_canon(b);
    int k = os64_strcmp(x.word, y.word);
    if (k != 0)
        return k;
    for (int i = 0; i < 4; i++)
        if (x.n[i] != y.n[i])
            return x.n[i] < y.n[i] ? -1 : 1;
    return os64_strcmp(x.jump, y.jump);
}

// One block at one of its offsets: the offset, the block's easing, the
// block, and where it was written among the rule's.
typedef struct {
    double offset;
    const garb_val_t *easing;
    int32_t block;
    int32_t order;
} KeyAt;

static int key_at_cmp(const void *a, const void *b)
{
    const KeyAt *x = a, *y = b;
    if (x->offset != y->offset)
        return x->offset < y->offset ? -1 : 1;
    int t = easing_cmp(x->easing, y->easing);
    return t != 0 ? t : (x->order > y->order) - (x->order < y->order);
}

// A keyframe: the run of KeyAt that makes it, and where its first block was
// written.
typedef struct {
    int32_t first, count, order;
    double offset;
} KeyRun;

static int key_run_cmp(const void *a, const void *b)
{
    const KeyRun *x = a, *y = b;
    if (x->offset != y->offset)
        return x->offset < y->offset ? -1 : 1;
    return (x->order > y->order) - (x->order < y->order);
}

// A rule's keyframes, read for an element (`own`, its custom properties
// `customs`) or for none: Animations 2 § 3.2's groups — the blocks at one
// offset with one easing, each longhand's last declaration among them —
// sorted by offset, a tie in the order each was first written; each kept
// declaration with where it was written. Two blocks at one offset, one
// giving an easing equal to the element's and one giving none, are two
// keyframes here, since the element's easing is not this library's to
// know: libflow, which knows it, makes them one by those places. Grouped by
// sorting, not by searching the groups so far: a rule may have as many
// offsets as a page can write. Into the cascade's arena; how many.
static int32_t build_frames(garb_cascade_t *c, const Keyframes *k, const CustomMap *customs,
                            bool own, garb_keyframe_t **out)
{
    *out = NULL;
    int32_t n = 0;
    for (int32_t b = 0; b < k->nblocks; b++)
        n += k->blocks[b].noffsets;
    if (n == 0)
        return 0;
    // Each block's easing, read once: the timing function's first item, in
    // the arena.
    const garb_val_t **easings = os64_calloc((size_t)k->nblocks, sizeof(*easings));
    KeyAt *at = os64_malloc((size_t)n * 2 * sizeof(*at));
    KeyRun *runs = os64_malloc((size_t)n * 2 * sizeof(*runs));
    garb_set_t *win = os64_malloc((size_t)GARB_NPROPS * sizeof(*win));
    uint64_t *written = os64_malloc((size_t)GARB_NPROPS * sizeof(*written));
    int32_t *stamp = os64_calloc((size_t)GARB_NPROPS, sizeof(*stamp));
    garb_prop_t *props = os64_malloc((size_t)GARB_NPROPS * sizeof(*props));
    bool ok = easings != NULL && at != NULL && runs != NULL && win != NULL &&
              written != NULL && stamp != NULL && props != NULL;
    garb_set_t sets[GARB_SETS_MAX];
    int32_t m = 0;
    for (int32_t b = 0; ok && b < k->nblocks; b++) {
        const KeyBlock *kb = &k->blocks[b];
        if (kb->easing && key_sets(c, &kb->decls[0], customs, own, sets) == 1 &&
            sets[0].value.kind != GARB_V_WIDE) {
            garb_val_t *e = alloc(c, sizeof(*e));
            if (e != NULL)
                *e = sets[0].value.kind == GARB_V_LAYERS ? sets[0].value.items[0] : sets[0].value;
            easings[b] = e;
        }
        for (int32_t o = 0; o < kb->noffsets; o++, m++)
            at[m] = (KeyAt){kb->offsets[o], easings[b], b, m};
    }
    // Runs of one offset and one easing, each a keyframe; then sorted by
    // offset, a tie in the order first written.
    int32_t nruns = 0;
    if (ok) {
        merge_sort(at, at + n, n, sizeof(*at), key_at_cmp);
        for (int32_t i = 0; i < n;) {
            int32_t j = i + 1;
            while (j < n && at[j].offset == at[i].offset && easing_cmp(at[j].easing, at[i].easing) == 0)
                j++;
            runs[nruns++] = (KeyRun){i, j - i, at[i].order, at[i].offset};
            i = j;
        }
        merge_sort(runs, runs + nruns, nruns, sizeof(*runs), key_run_cmp);
    }
    garb_keyframe_t *frames = ok ? alloc(c, (size_t)nruns * sizeof(*frames)) : NULL;
    for (int32_t r = 0; frames != NULL && r < nruns; r++) {
        // Each longhand's last value among the run's blocks, in the order
        // written, and the longhands in their table's order.
        int32_t np = 0;
        for (int32_t i = runs[r].first; i < runs[r].first + runs[r].count; i++) {
            const KeyBlock *kb = &k->blocks[at[i].block];
            for (int32_t d = kb->easing ? 1 : 0; d < kb->ndecls; d++) {
                int32_t ns = key_sets(c, &kb->decls[d], customs, own, sets);
                for (int32_t s2 = 0; s2 < ns; s2++) {
                    garb_prop_t p = sets[s2].prop;
                    win[p] = sets[s2];
                    written[p] = (uint64_t)(uint32_t)at[i].order << 32 | (uint32_t)d;
                    if (stamp[p] != r + 1) {
                        stamp[p] = r + 1;
                        props[np++] = p;
                    }
                }
            }
        }
        for (int32_t i = 1; i < np; i++) {      // at most GARB_NPROPS
            garb_prop_t x = props[i];
            int32_t j = i - 1;
            while (j >= 0 && props[j] > x) {
                props[j + 1] = props[j];
                j--;
            }
            props[j + 1] = x;
        }
        garb_set_t *kept = np > 0 ? alloc(c, (size_t)np * sizeof(*kept)) : NULL;
        uint64_t *order = np > 0 ? alloc(c, (size_t)np * sizeof(*order)) : NULL;
        if (order == NULL)
            kept = NULL;
        for (int32_t i = 0; kept != NULL && i < np; i++) {
            kept[i] = win[props[i]];
            order[i] = written[props[i]];
        }
        frames[r] = (garb_keyframe_t){.offset = runs[r].offset, .easing = at[runs[r].first].easing,
                                      .sets = kept, .order = kept != NULL ? order : NULL,
                                      .nsets = kept != NULL ? np : 0, .sheet = k->sheet};
    }
    os64_free(written);
    os64_free(easings);
    os64_free(at);
    os64_free(runs);
    os64_free(win);
    os64_free(stamp);
    os64_free(props);
    if (!ok || frames == NULL)
        c->incomplete = true;
    *out = frames;
    return frames != NULL ? nruns : 0;
}

static int keyframes_cmp(const void *a, const void *b)
{
    const Keyframes *x = a, *y = b;
    return name_cmp(x->name, x->len, y->name, y->len);
}

// One rule per name, the last of it in cascade order, sorted by name for
// keyframes_named's search. With no room to sort, the rules stay as read
// and are searched from the last.
static void keyframes_settle(garb_cascade_t *c)
{
    int32_t n = c->nkeyframes;
    Keyframes *tmp = n > 1 ? os64_malloc((size_t)n * sizeof(*tmp)) : NULL;
    if (n > 1 && tmp == NULL) {
        c->incomplete = true;
        return;
    }
    merge_sort(c->keyframes, tmp, n, sizeof(*tmp), keyframes_cmp);
    os64_free(tmp);
    int32_t kept = 0;
    for (int32_t i = 0; i < n; i++) {
        if (i + 1 < n && keyframes_cmp(&c->keyframes[i], &c->keyframes[i + 1]) == 0)
            continue;           // a later one of this name follows
        c->keyframes[kept++] = c->keyframes[i];
    }
    c->nkeyframes = kept;
    c->keyframes_sorted = true;
}

static const Keyframes *keyframes_named(const garb_cascade_t *c, const char *name, size_t len)
{
    if (c->keyframes_sorted) {
        int32_t lo = 0, hi = c->nkeyframes;
        while (lo < hi) {
            int32_t mid = lo + (hi - lo) / 2;
            int x = name_cmp(c->keyframes[mid].name, c->keyframes[mid].len, name, len);
            if (x == 0)
                return &c->keyframes[mid];
            if (x < 0)
                lo = mid + 1;
            else
                hi = mid;
        }
        return NULL;
    }
    for (int32_t i = c->nkeyframes - 1; i >= 0; i--)
        if (name_cmp(c->keyframes[i].name, c->keyframes[i].len, name, len) == 0)
            return &c->keyframes[i];
    return NULL;
}

static int own_frames_cmp(const void *a, const void *b)
{
    uintptr_t x = (uintptr_t)((const OwnFrames *)a)->rule, y = (uintptr_t)((const OwnFrames *)b)->rule;
    return (x > y) - (x < y);
}

// An element that animates — by its winners' names or by those it
// inherits (animation_names) — reads, against its own custom properties,
// each named rule that holds var(), once however often it is named; the
// rest it shares. Sorted by rule for garb_keyframes' search.
static void own_frames(garb_cascade_t *c, Slot *slot, const garb_val_t *names,
                       const CustomMap *customs)
{
    const garb_val_t *items = names->kind == GARB_V_LAYERS ? names->items : names;
    int32_t n = names->kind == GARB_V_LAYERS ? names->nitems : 1, m = 0;
    OwnFrames *own = n > 0 ? alloc(c, (size_t)n * sizeof(*own)) : NULL;
    for (int32_t i = 0; own != NULL && i < n; i++) {
        const Keyframes *k = items[i].kind == GARB_V_STRING
                           ? keyframes_named(c, items[i].text, items[i].len) : NULL;
        if (k != NULL && k->pending)
            own[m++] = (OwnFrames){k, NULL, 0};
    }
    if (m == 0)
        return;
    OwnFrames *tmp = m > 1 ? os64_malloc((size_t)m * sizeof(*tmp)) : NULL;
    if (m > 1 && tmp == NULL) {
        c->incomplete = true;
        return;
    }
    merge_sort(own, tmp, m, sizeof(*own), own_frames_cmp);
    os64_free(tmp);
    int32_t u = 0;
    for (int32_t i = 0; i < m; i++)
        if (u == 0 || own[u - 1].rule != own[i].rule)
            own[u++] = own[i];
    for (int32_t i = 0; i < u; i++)
        own[i].nframes = build_frames(c, own[i].rule, customs, true, &own[i].frames);
    slot->frames = own;
    slot->nframes = u;
}

static bool is_el(const os64_html_node_t *n);

// The element `x` inherits from: its flat parent (a slot, for an element
// assigned to one; a host, for a shadow root's child), or, for an element
// in no flat tree (a host's unassigned child, a slot's own child while the
// slot has nodes), its parent. NULL at the top.
static const os64_html_node_t *inherit_from(const garb_cascade_t *c, const os64_html_node_t *x)
{
    const os64_html_node_t *up = c->shadowed ? os64_html_flat_parent(c->doc, x) : NULL;
    if (up == NULL)
        up = x->parent;
    return is_el(up) ? up : NULL;
}

// The animation names element `el` computes from its winner `v`: the
// winner's own, or for `inherit` the one of the element it inherits from
// (inherit_from), as far up as `inherit`
// goes — whose rules with var() are then built against `el`'s custom
// properties, since `el` animates with them. NULL for none: no winner, or
// another wide keyword (animation-name does not inherit).
static const garb_val_t *animation_names(const garb_cascade_t *c, const os64_html_node_t *el,
                                         const garb_val_t *v)
{
    while (v != NULL && v->kind == GARB_V_WIDE && os64_streq(v->keyword, "inherit")) {
        el = inherit_from(c, el);
        const Slot *up = el != NULL ? slot_for(c, el, false) : NULL;
        v = NULL;
        for (int32_t i = 0; up != NULL && i < up->style.n; i++)
            if (up->style.sets[i].prop == GARB_ANIMATION_NAME)
                v = &up->style.sets[i].value;
    }
    return v != NULL && v->kind != GARB_V_WIDE ? v : NULL;
}

// ── Per element ─────────────────────────────────────────────────────────

typedef struct Match Match;

// A rule that matched, at its heaviest selector, and the context it came
// through (Consult.context).
struct Match {
    Rule *rule;
    uint32_t spec;
    int32_t context;
};
// The rules that matched, in order, each with the specificity and the
// context it came through, folded into 64 bits (restyle compares them).
static uint64_t match_digest(const Match *m, int32_t n)
{
    uint64_t h = 0x9e3779b97f4a7c15u ^ (uint64_t)n;
    for (int32_t k = 0; k < n; k++) {
        uint64_t v = (uint64_t)(uintptr_t)m[k].rule ^ ((uint64_t)m[k].spec << 32) ^ (uint32_t)m[k].context;
        h ^= v + 0x9e3779b97f4a7c15u + (h << 6) + (h >> 2);
        h *= 0xbf58476d1ce4e5b9u;
        h ^= h >> 31;
    }
    return h;
}

// One declaration's place in the cascade: importance, then context (CSS
// Cascade 5 § 6.1: between trees, the normal declaration from the tree
// earlier in shadow-including order wins, and the important one from the
// later), then (inline above every rule) specificity, then order.
typedef struct {
    const Decl *d;
    bool important, inline_;
    uint32_t spec, order;
    int32_t sheet;              // -1 for the style attribute
    int32_t context;
} Applied;

// Declaration ordering is temporary. Reuse its storage within one build
// or update and release it when that pass ends; a retained
// cascade does not keep this scratch allocation between mutations.
#define APPLIED_LOCAL_COUNT 32u
typedef struct {
    // Short declaration lists avoid the heap; larger ones share a buffer.
    Applied local[APPLIED_LOCAL_COUNT];
    Applied *items;
    size_t capacity;
    // The longhands each declaration holding var() set, while an element is
    // cascaded (THE RECOMPUTE's PendDecl, copied out at its exact size).
    garb_prop_t *produced;
    uint8_t *nproduced;
    size_t produced_cap;
} AppliedScratch;

static bool before(const Applied *a, const Applied *b)
{
    if (a->important != b->important)
        return !a->important;
    if (a->context != b->context)
        return a->important ? a->context < b->context : a->context > b->context;
    if (a->inline_ != b->inline_)
        return !a->inline_;
    if (a->spec != b->spec)
        return a->spec < b->spec;
    return a->order < b->order;
}

// One consultation of an index for the element being styled: which target,
// under which scope's sheets, in which context, and the element the rules'
// subject compound is matched against — the styled element, or for
// `::slotted()` the slot it is assigned to. The context is the tree's place
// in shadow-including order among the trees that style the element
// (cascade_element): its own tree 0, each slot's tree on its forwarding
// chain 1, 2, …, outward to inward, and a host's own shadow tree last.
typedef struct {
    int32_t target, scope, context;
    const os64_html_node_t *styled, *keyed;
} Consult;

static void consider(garb_cascade_t *c, Match **m, int32_t *n, int32_t *cap, Rule *r,
                     uint32_t spec, const Consult *q)
{
    int32_t pass = q->target + TARGETS * q->context;
    // No marks only where nothing is matched (build's failure path).
    if (c->seen == NULL)
        return;
    RuleSeen *mark = &c->seen[r - c->rules];
    if (mark->seen == q->styled && mark->session == c->match_session && mark->pass == pass) {
        if (spec > (*m)[mark->slot].spec)
            (*m)[mark->slot].spec = spec;
        return;
    }
    if (!grow(c, (void **)m, *n, cap, sizeof(Match)))
        return;
    mark->seen = q->styled;
    mark->session = c->match_session;
    mark->pass = pass;
    mark->slot = *n;
    (*m)[(*n)++] = (Match){r, spec, q->context};
}

static void try_bucket(garb_cascade_t *c, int32_t head, const Consult *q, Match **m,
                       int32_t *n, int32_t *cap)
{
    for (int32_t e = head; e >= 0; e = c->entries[e].next) {
        Entry *en = &c->entries[e];
        Rule *r = &c->rules[en->rule];
        if (!applies_in(c, q->scope, r->sheet))
            continue;
        bool hit = q->target == TARGET_HOST ? garb_selector_matches_host(r->sel, en->sel, q->keyed)
                 : q->target == TARGET_SLOTTED
                     ? garb_selector_matches_slotted(r->sel, en->sel, q->keyed, q->styled)
                     : garb_selector_matches(r->sel, en->sel, q->keyed);
        if (hit)
            consider(c, m, n, cap, r, garb_selector_specificity(r->sel, en->sel), q);
    }
}

static void try_classes(garb_cascade_t *c, Index *ix, const Consult *q, Match **m, int32_t *n,
                        int32_t *cap)
{
    const os64_html_attr_t *a = os64_html_attr(q->keyed, "class");
    if (a == NULL || a->value == NULL || ix->classes.n == 0)
        return;
    const char *v = a->value;
    bool quirky = c->quirks == OS64_HTML_QUIRKS;
    for (size_t i = 0, len = os64_strlen(v); i < len;) {
        while (i < len && (v[i] == ' ' || v[i] == '\t' || v[i] == '\n' || v[i] == '\f' || v[i] == '\r'))
            i++;
        size_t start = i;
        while (i < len && !(v[i] == ' ' || v[i] == '\t' || v[i] == '\n' || v[i] == '\f' || v[i] == '\r'))
            i++;
        if (i == start)
            continue;
        Bucket *b = bucket(c, &ix->classes, v + start, i - start, quirky, false);
        if (b != NULL)
            try_bucket(c, b->head, q, m, n, cap);
    }
}

// Every rule `q` finds, each once, at its heaviest selector, added after
// the `n` matches already in `m`; answers the new count. A scope with no
// sheets finds nothing.
static int32_t matching(garb_cascade_t *c, const Consult *q, Match **m, int32_t n, int32_t *cap)
{
    if (q->scope < 0)
        return n;
    Index *ix = &c->index[q->target];
    const os64_html_node_t *el = q->keyed;
    const os64_html_attr_t *id = os64_html_attr(el, "id");
    if (id != NULL && id->value != NULL && ix->ids.n > 0) {
        Bucket *b = bucket(c, &ix->ids, id->value, os64_strlen(id->value),
                           c->quirks == OS64_HTML_QUIRKS, false);
        if (b != NULL)
            try_bucket(c, b->head, q, m, &n, cap);
    }
    try_classes(c, ix, q, m, &n, cap);
    if (el->name != NULL && ix->types.n > 0) {
        Bucket *b = bucket(c, &ix->types, el->name, os64_strlen(el->name), true, false);
        if (b != NULL)
            try_bucket(c, b->head, q, m, &n, cap);
    }
    try_bucket(c, ix->universal, q, m, &n, cap);
    return n;
}

// Whether any rule was filed for `target`.
static bool targeted(const garb_cascade_t *c, int32_t target)
{
    const Index *ix = &c->index[target];
    return ix->ids.n > 0 || ix->classes.n > 0 || ix->types.n > 0 || ix->universal >= 0;
}

// The scope whose root is `root`, -1 when no scope names it.
static int32_t scope_of_root(const garb_cascade_t *c, const os64_html_node_t *root)
{
    if (c->root_slotcap == 0)
        return -1;
    for (uint32_t h = hash_ptr(root) & (c->root_slotcap - 1); c->root_slots[h] >= 0;
         h = (h + 1) & (c->root_slotcap - 1))
        if (c->roots[c->root_slots[h]] == root)
            return c->root_slots[h];
    return -1;
}

// Adds `root` as scope `index` (scopes_take, which sized the slots).
static void scope_add_root(garb_cascade_t *c, const os64_html_node_t *root, int32_t index)
{
    uint32_t h = hash_ptr(root) & (c->root_slotcap - 1);
    while (c->root_slots[h] >= 0)
        h = (h + 1) & (c->root_slotcap - 1);
    c->root_slots[h] = index;
    c->roots[index] = root;
}

// The scope of the tree element `x` is in (Slot.scope): its parent's, when
// that is an element already placed, or else found by climbing to its root.
static int32_t tree_place(const garb_cascade_t *c, const os64_html_node_t *x)
{
    const Slot *up = is_el(x->parent) ? slot_for(c, x->parent, false) : NULL;
    if (up != NULL && up->placed)
        return up->scope;
    const os64_html_node_t *root = x;
    while (root->parent != NULL)
        root = root->parent;
    return scope_of_root(c, root);
}

// The `style` attribute's declarations, read into a Decl list.
static int32_t inline_decls(garb_cascade_t *c, const os64_html_node_t *el, Decl **out)
{
    const os64_html_attr_t *a = os64_html_attr(el, "style");
    if (a == NULL || a->value == NULL || a->value[0] == '\0')
        return 0;
    if ((size_t)c->nattrs >= c->limits.attributes) {
        c->incomplete = true;
        return 0;
    }
    if (!grow(c, (void **)&c->attrs, c->nattrs, &c->attrcap, sizeof(garb_parsed_t)))
        return 0;
    garb_parsed_t *p = &c->attrs[c->nattrs];
    size_t cap = c->limits.attribute_bytes;
    if (c->updating && cap > c->limits.state_bytes - c->update_attribute_bytes)
        cap = c->limits.state_bytes - c->update_attribute_bytes;
    if (garb_parse_block_limited(a->value, os64_strlen(a->value), cap, p) != GARB_OK) {
        garb_free(p);
        c->incomplete = true;           // a style the page gave was not read
        return 0;
    }
    c->nattrs++;
    if (c->updating) c->update_attribute_bytes += os64_arena_stats(p->arena).reserved_bytes;
    Decl *d = alloc(c, (size_t)(p->nitems > 0 ? p->nitems : 1) * sizeof(Decl));
    if (d == NULL)
        return 0;
    int32_t n = 0;
    for (int32_t i = 0; i < p->nitems; i++)
        if (p->items[i].kind == GARB_ITEM_DECL && read_decl(c, p, &p->items[i].decl, &d[n]))
            n++;
    *out = d;
    return n;
}

// Presentation values live in the cascade's bounded arena. A temporary CSS
// value parse is released after copying, instead of retaining a 4 KiB arena
// for each small fill/opacity attribute in an icon collection.
static garb_value_t *attribute_values(garb_cascade_t *c, const garb_value_t *v, int32_t n)
{
    if (n <= 0)
        return NULL;
    garb_value_t *out = alloc(c, (size_t)n * sizeof(*out));
    if (!out)
        return NULL;
    for (int32_t i = 0; i < n; ++i) {
        out[i] = v[i];
        if (v[i].text) {
            char *text = alloc(c, (size_t)v[i].len + 1);
            if (!text)
                return NULL;
            os64_memcpy(text, v[i].text, v[i].len);
            text[v[i].len] = 0;
            out[i].text = text;
        }
        if (v[i].unit) {
            char *unit = alloc(c, (size_t)v[i].unit_len + 1);
            if (!unit)
                return NULL;
            os64_memcpy(unit, v[i].unit, v[i].unit_len);
            unit[v[i].unit_len] = 0;
            out[i].unit = unit;
        }
        if (v[i].nchildren) {
            out[i].children = attribute_values(c, v[i].children, v[i].nchildren);
            if (!out[i].children)
                return NULL;
        }
    }
    return out;
}

static int32_t svg_attributes(garb_cascade_t *c, const os64_html_node_t *el, Decl **out)
{
    if (el->ns != OS64_HTML_NS_SVG || c->env.modern_off)
        return 0;
    static const char *const names[] = {"fill",
                                        "stroke",
                                        "stop-color", "stop-opacity",
                                        "fill-opacity",
                                        "stroke-opacity",
                                        "fill-rule",
                                        "clip-path",
                                        "clip-rule",
                                        "stroke-width",
                                        "stroke-linecap",
                                        "stroke-linejoin",
                                        "stroke-miterlimit",
                                        "stroke-dasharray",
                                        "stroke-dashoffset",
                                        "vector-effect",
                                        "color-interpolation",
                                        "color",
                                        "opacity",
                                        "display",
                                        "visibility",
                                        "overflow",
                                        "width",
                                        "height", "x", "y", "cx", "cy", "r", "rx", "ry"};
    Decl *d = NULL;
    int32_t n = 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        const os64_html_attr_t *a = os64_html_attr(el, names[i]);
        if (!a || !a->value || !*a->value)
            continue;
        if (!d) {
            d = alloc(c, sizeof(names) / sizeof(names[0]) * sizeof(*d));
            if (!d)
                return 0;
        }
        garb_parsed_t temporary = {0};
        if (garb_parse_values_limited(a->value, os64_strlen(a->value),
                                      c->limits.attribute_bytes, &temporary) != GARB_OK ||
            temporary.incomplete) {
            garb_free(&temporary);
            c->incomplete = true;
            break;
        }
        int32_t count = temporary.nvalues;
        garb_value_t *values = attribute_values(c, temporary.values, count);
        garb_free(&temporary);
        if (count && !values)
            break;
        // SVG width/height accept unitless user coordinates. This conversion
        // is specific to presentation attributes, not CSS declarations.
        if (os64_streq(names[i], "width") || os64_streq(names[i], "height")) {
            int32_t first = 0, last = count;
            while (first < last && values[first].kind == GARB_WHITESPACE)
                ++first;
            while (last > first && values[last - 1].kind == GARB_WHITESPACE)
                --last;
            if (last == first + 1 && values[first].kind == GARB_NUMBER) {
                values[first].kind = GARB_DIMENSION;
                values[first].unit = "px";
                values[first].unit_len = 2;
            }
        }
        garb_decl_t attr = {
            .name = names[i], .len = os64_strlen(names[i]), .value = values, .nvalue = count};
        if (read_decl(c, &c->self, &attr, &d[n]))
            ++n;
    }
    *out = d;
    return n;
}

// A custom property entry made from declaration `d` over the map it
// inherits: as declared, `initial` the guaranteed-invalid value, `inherit`
// and `unset` the inherited value (a custom property inherits, Cascade 4
// § 7.3).
static void custom_entry(garb_cascade_t *c, Custom *cu, const Decl *source, const CustomMap *inherited)
{
    const garb_decl_t *d = &source->decl;
    cu->name = d->name;
    cu->len = d->len;
    cu->decl = source;
    cu->reads_up = garb_decl_has_var(d);
    cu->value = d->nvalue > 0 ? d->value : kEmptyValue;
    cu->nvalue = d->nvalue;
    cu->state = 0;
    bool one_ident = d->nvalue == 1 && d->value[0].kind == GARB_IDENT;
    if (one_ident && ieq(d->value[0].text, d->value[0].len, "initial"))
        cu->value = NULL;
    if (one_ident && (ieq(d->value[0].text, d->value[0].len, "inherit") ||
                      ieq(d->value[0].text, d->value[0].len, "unset"))) {
        Sub up = {c, inherited, 0, VAR_VALUES_MAX};
        int32_t pn = 0;
        cu->value = custom_value(&up, d->name, d->len, &pn);
        cu->nvalue = pn;
        cu->state = 2;
        cu->reads_up = true;
    }
}

// The longhands declaration `d`, holding var() or env(), sets against the
// custom property map `customs`, into `sets`: replaced, then read; a value
// that fails either way is invalid AT COMPUTED-VALUE TIME, and each
// longhand acts as `unset` (Custom Properties 1 § 3.1) rather than falling
// back to the declaration it overrode.
static int32_t pending_sets(garb_cascade_t *c, const Decl *d, const CustomMap *customs,
                            garb_set_t sets[GARB_SETS_MAX])
{
    Sub sub = {c, customs, 0, VAR_VALUES_MAX};
    Vec out = {0};
    int32_t n = -1;
    if (substitute(&sub, d->decl.value, d->decl.nvalue, &out)) {
        garb_decl_t read = d->decl;
        read.value = out.v;
        read.nvalue = out.n;
        n = garb_read_declaration(&c->self, &read, reading(c), sets);
    }
    if (n <= 0) {
        garb_prop_t props[GARB_SETS_MAX];
        int32_t np = prop_longhands(d->decl.name, d->decl.len, c->env.modern_off, props);
        for (int32_t k = 0; k < np; k++) {
            sets[k].prop = props[k];
            os64_memset(&sets[k].value, 0, sizeof(sets[k].value));
            sets[k].value.kind = GARB_V_WIDE;
            sets[k].value.keyword = "unset";
        }
        n = np;
    }
    return n;
}

// `el`'s winners for `target`: its own, from its rules, its `style`
// attribute and its SVG presentation attributes — and, for a shadow host,
// its shadow tree's `:host` rules, and for a slotted element each slot's
// tree's `::slotted()` rules; or one of its pseudo-element's, from that
// pseudo-element's rules alone, its custom properties inherited from `el`
// and its own kept for its own reading. Its own comes first: it places
// the element (Slot.scope), which its pseudo-elements read.
//
// The contexts (Consult.context) follow the trees' roots in shadow-including
// order: the element's own tree; then the tree of each slot it is assigned
// to, outward to inward along the forwarding chain, each slot's host lying
// inside the tree before it; and last a host's own shadow tree, whose root
// follows the host and so everything above it.
static void cascade_element(garb_cascade_t *c, const os64_html_node_t *el, int32_t target,
                            const CustomMap *inherited, Match **m, int32_t *mcap, AppliedScratch *scratch)
{
    Slot *slot = slot_for(c, el, true);
    if (slot == NULL)
        return;
    bool own = target == TARGET_ELEMENT;
    if (own) {
        slot->customs = inherited;
        slot->scope = tree_place(c, el);
        slot->placed = true;
        slot->empty = c->dependencies.parent_state && garb_element_empty(el);
        slot->klass = attr_value(el, "class");
        slot->idv = attr_value(el, "id");
    }
    Consult q = {target, slot->scope, 0, el, el};
    c->match_session++;
    int32_t nm = matching(c, &q, m, 0, mcap);
    if (own && c->shadowed) {
        int32_t context = 0;
        if (targeted(c, TARGET_SLOTTED))
            for (const os64_html_node_t *s = os64_html_assigned_slot(c->doc, el); s != NULL;
                 s = os64_html_assigned_slot(c->doc, s)) {
                const Slot *placed = slot_for(c, s, false);
                Consult slotted = {TARGET_SLOTTED,
                                   placed != NULL && placed->placed ? placed->scope : tree_place(c, s),
                                   ++context, el, s};
                nm = matching(c, &slotted, m, nm, mcap);
            }
        if (el->shadow_root != NULL && targeted(c, TARGET_HOST)) {
            Consult host = {TARGET_HOST, scope_of_root(c, el->shadow_root), context + 1, el, el};
            nm = matching(c, &host, m, nm, mcap);
        }
    }
    if (own)
        slot->matched = match_digest(*m, nm);
    Decl *inl = NULL;
    int32_t ninl = own ? inline_decls(c, el, &inl) : 0;
    if (own) {
        slot->inl = inl;
        slot->ninl = ninl;
    }
    Decl *attrs = NULL;
    int32_t nattrs = own ? svg_attributes(c, el, &attrs) : 0;
    int32_t total = ninl + nattrs;
    for (int32_t k = 0; k < nm; k++)
        total += (*m)[k].rule->ndecls;
    if (total == 0)
        return;
    size_t capacity = scratch->items ? scratch->capacity : APPLIED_LOCAL_COUNT;
    if ((size_t)total > capacity) {
        Applied *more = os64_realloc(scratch->items, (size_t)total * sizeof(*more));
        if (more == NULL) {
            c->incomplete = true;
            return;
        }
        scratch->items = more;
        scratch->capacity = (size_t)total;
    }
    Applied *ap = scratch->items ? scratch->items : scratch->local;
    int32_t na = 0;
    for (int32_t j = 0; j < nattrs; ++j)
        ap[na++] = (Applied){&attrs[j], false, false, 0, 0, -1, 0};
    for (int32_t k = 0; k < nm; k++)
        for (int32_t j = 0; j < (*m)[k].rule->ndecls; j++) {
            const Decl *d = &(*m)[k].rule->decls[j];
            ap[na++] = (Applied){d, d->important, false, (*m)[k].spec, (*m)[k].rule->order,
                                 (*m)[k].rule->sheet, (*m)[k].context};
        }
    for (int32_t j = 0; j < ninl; j++)
        ap[na++] = (Applied){&inl[j], inl[j].important, true, 0, (uint32_t)j, -1, 0};
    // Insertion sort into ascending precedence; the last write wins.
    for (int32_t i = 1; i < na; i++) {
        Applied x = ap[i];
        int32_t j = i - 1;
        while (j >= 0 && before(&x, &ap[j])) {
            ap[j + 1] = ap[j];
            j--;
        }
        ap[j + 1] = x;
    }
    // Custom properties first: every other value may name one.
    int32_t ncustom = 0;
    for (int32_t i = 0; i < na; i++)
        ncustom += ap[i].d->custom;
    const CustomMap *customs = inherited;
    if (ncustom > 0) {
        CustomMap *mine = alloc(c, sizeof(*mine));
        Custom *list = alloc(c, (size_t)ncustom * sizeof(Custom));
        if (mine != NULL && list != NULL) {
            mine->parent = inherited;
            mine->own = list;
            for (int32_t i = 0; i < na; i++) {
                if (!ap[i].d->custom)
                    continue;
                const garb_decl_t *d = &ap[i].d->decl;
                // A later declaration of the same name replaces an earlier.
                int32_t k = 0;
                while (k < mine->n && !(mine->own[k].len == d->len &&
                                       os64_memcmp(mine->own[k].name, d->name, d->len) == 0))
                    k++;
                if (k == mine->n)
                    mine->n++;
                custom_entry(c, &mine->own[k], ap[i].d, inherited);
            }
            customs = mine;
            if (target == TARGET_ELEMENT) {
                slot->customs = mine;
                slot->own_customs = true;
            }
        }
    }
    // The winners, one per longhand, and the declaration each came from;
    // and, for the element's own, the longhands each declaration holding
    // var() set, in its order among them (`produced`, for recompute).
    garb_set_t win[GARB_NPROPS];
    int32_t from[GARB_NPROPS], src[GARB_NPROPS];
    bool have[GARB_NPROPS], important[GARB_NPROPS];
    os64_memset(have, 0, sizeof(have));
    int32_t npend = 0, jpend = 0;
    for (int32_t i = 0; own && i < na; i++) npend += ap[i].d->pending && !ap[i].d->custom;
    if ((size_t)npend > scratch->produced_cap) {
        garb_prop_t *more = os64_realloc(scratch->produced, (size_t)npend * GARB_SETS_MAX * sizeof(*more));
        if (more != NULL) scratch->produced = more;
        uint8_t *counts = more != NULL ? os64_realloc(scratch->nproduced, (size_t)npend) : NULL;
        if (counts != NULL) scratch->nproduced = counts;
        if (more == NULL || counts == NULL) {
            c->incomplete = true;
            return;
        }
        scratch->produced_cap = (size_t)npend;
    }
    garb_prop_t *produced = scratch->produced;
    uint8_t *nproduced = scratch->nproduced;
    for (int32_t i = 0; i < na; i++) {
        const Decl *d = ap[i].d;
        if (d->custom)
            continue;
        if (!d->pending) {
            for (int32_t k = 0; k < d->nsets; k++) {
                win[d->sets[k].prop] = d->sets[k];
                from[d->sets[k].prop] = ap[i].sheet;
                src[d->sets[k].prop] = i;
                important[d->sets[k].prop] = ap[i].important;
                have[d->sets[k].prop] = true;
            }
            continue;
        }
        if (own)
            slot->needs_substitution = true;
        else
            slot->pseudo_vars = true;
        garb_set_t sets[GARB_SETS_MAX];
        int32_t n = pending_sets(c, d, customs, sets);
        if (own) {
            for (int32_t k = 0; k < n; k++) produced[(size_t)jpend * GARB_SETS_MAX + k] = sets[k].prop;
            nproduced[jpend++] = (uint8_t)n;
        }
        for (int32_t k = 0; k < n; k++) {
            win[sets[k].prop] = sets[k];
            from[sets[k].prop] = ap[i].sheet;
            src[sets[k].prop] = i;
            important[sets[k].prop] = ap[i].important;
            have[sets[k].prop] = true;
        }
    }
    int32_t nwin = 0;
    for (int p = 0; p < GARB_NPROPS; p++)
        nwin += have[p];
    if (nwin == 0)
        return;
    garb_set_t *kept = alloc(c, (size_t)nwin * sizeof(garb_set_t));
    int32_t *kept_from = alloc(c, (size_t)nwin * sizeof(int32_t));
    bool *kept_important = alloc(c, (size_t)nwin * sizeof(bool));
    if (kept == NULL || kept_from == NULL || kept_important == NULL)
        return;
    int32_t k = 0;
    for (int p = 0; p < GARB_NPROPS; p++)
        if (have[p]) {
            kept_from[k] = from[p];
            kept_important[k] = important[p];
            kept[k++] = win[p];
        }
    garb_style_t *into = own ? &slot->style : &slot->pseudo[target - 1];
    into->sets = kept;
    into->sheet = kept_from;
    into->important = kept_important;
    into->n = nwin;
    // Its winners var() made, each declaration with the longhands it won,
    // for a recompute (recompute_winners).
    if (own && npend > 0) {
        // Sized exactly: the declarations that won a longhand, the longhands
        // they won, and what each set.
        int32_t np = 0, nprops = 0, nset = 0;
        for (int32_t i = 0, j = 0; i < na; i++) {
            if (!ap[i].d->pending || ap[i].d->custom) continue;
            int32_t won = 0;
            for (int q = 0; q < GARB_NPROPS; q++) won += have[q] && src[q] == i;
            if (won > 0) { np++; nprops += won; nset += nproduced[j]; }
            j++;
        }
        PendDecl *pend = np > 0 ? alloc(c, (size_t)np * sizeof(*pend)) : NULL;
        garb_prop_t *props = np > 0 ? alloc(c, (size_t)(nprops + nset) * sizeof(*props)) : NULL;
        if (np > 0 && (pend == NULL || props == NULL)) {
            c->incomplete = true;
            return;
        }
        int32_t k = 0, used = 0;
        for (int32_t i = 0, j = 0; i < na && k < np; i++) {
            if (!ap[i].d->pending || ap[i].d->custom) continue;
            int32_t jj = j++;
            int32_t won = 0;
            for (int q = 0; q < GARB_NPROPS; q++) won += have[q] && src[q] == i;
            if (won == 0) continue;
            PendDecl *pd = &pend[k++];
            *pd = (PendDecl){ap[i].d, props + used, props + used + won, 0, nproduced[jj]};
            for (int q = 0; q < GARB_NPROPS; q++)
                if (have[q] && src[q] == i) pd->props[pd->nprops++] = (garb_prop_t)q;
            os64_memcpy(pd->produced, produced + (size_t)jj * GARB_SETS_MAX, (size_t)pd->nproduced * sizeof(garb_prop_t));
            used += won + pd->nproduced;
        }
        slot->pend = pend;
        slot->npend = np;
    }
    // A pseudo-element's animations read the frames libflow asks for with
    // its element (garb_keyframes): a rule holding var() is read against the
    // element's custom properties, and only when the element names it too
    // (DEBTS.md).
    if (!own)
        return;
    const garb_val_t *names = animation_names(c, el, have[GARB_ANIMATION_NAME]
                                                     ? &win[GARB_ANIMATION_NAME].value : NULL);
    if (names != NULL)
        own_frames(c, slot, names, customs);
}

// ── The door ────────────────────────────────────────────────────────────

static bool is_el(const os64_html_node_t *n)
{
    return n != NULL && n->kind == OS64_HTML_ELEMENT;
}

// The node after `x` in preorder over `root`'s shadow-including descendants
// — a host's shadow root and its tree before the host's children, which
// puts every element after the element it inherits from (inherit_from) —
// skipping `x`'s own when `children` is false. NULL after the last.
static const os64_html_node_t *si_next(const os64_html_node_t *x, const os64_html_node_t *root,
                                       bool children)
{
    if (children && x->shadow_root != NULL)
        return x->shadow_root;
    if (children && x->first_child != NULL)
        return x->first_child;
    while (x != NULL && x != root) {
        const os64_html_node_t *host = os64_html_shadow_host(x);
        if (host != NULL) {
            // A shadow tree done: its host's children are next.
            if (host->first_child != NULL)
                return host->first_child;
            x = host;
            continue;
        }
        if (x->next != NULL)
            return x->next;
        x = x->parent;
    }
    return NULL;
}

// Element `x`'s winners and its pseudo-elements', its custom properties
// taken from the element it inherits from, which the walks calling this
// (si_next's order) have already styled; its pseudo-elements take its own.
static void cascade_whole(garb_cascade_t *c, const os64_html_node_t *x, Match **m, int32_t *mcap,
                          AppliedScratch *scratch)
{
    const os64_html_node_t *from = inherit_from(c, x);
    const Slot *up = from != NULL ? slot_for(c, from, false) : NULL;
    cascade_element(c, x, TARGET_ELEMENT, up != NULL ? up->customs : NULL, m, mcap, scratch);
    Slot *self = slot_for(c, x, false);
    if (self != NULL) self->from = from;
    for (int32_t t = TARGET_BEFORE; t <= TARGET_AFTER; t++)
        if (targeted(c, t))
            cascade_element(c, x, t, self != NULL ? self->customs : NULL, m, mcap, scratch);
}

static void set_applies(garb_cascade_t *c, int32_t scope, int32_t sheet)
{
    size_t bit = (size_t)scope * (size_t)c->ninputs + (size_t)sheet;
    c->applies[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

// The scopes, as given (for garb_cascade_matches_scoped) and as used: each
// distinct root once, and a bit for each sheet applying there, an imported
// sheet's bit following its importer's.
static bool scopes_take(garb_cascade_t *c, const garb_sheet_in_t *sheets, int32_t n,
                        const garb_scope_in_t *scopes, int32_t nscopes)
{
    if (scopes == NULL || nscopes < 0)
        nscopes = 0;
    if (nscopes > 0) {
        c->given = alloc(c, (size_t)nscopes * sizeof(*c->given));
        if (c->given == NULL)
            return false;
        for (int32_t i = 0; i < nscopes; i++) {
            c->given[i] = (garb_scope_in_t){scopes[i].root, NULL, 0};
            if (scopes[i].sheets == NULL || scopes[i].n <= 0)
                continue;
            int32_t *list = alloc(c, (size_t)scopes[i].n * sizeof(*list));
            if (list == NULL)
                return false;
            os64_memcpy(list, scopes[i].sheets, (size_t)scopes[i].n * sizeof(*list));
            c->given[i].sheets = list;
            c->given[i].n = scopes[i].n;
        }
        c->ngiven = nscopes;
    }
    int32_t most = nscopes > 0 ? nscopes : 1;
    c->root_slotcap = 4;
    while (c->root_slotcap < (uint32_t)most * 2)
        c->root_slotcap *= 2;
    c->roots = alloc(c, (size_t)most * sizeof(*c->roots));
    c->root_slots = alloc(c, (size_t)c->root_slotcap * sizeof(*c->root_slots));
    c->applies = alloc(c, ((size_t)most * (size_t)(n > 0 ? n : 1) + 7) / 8);
    if (c->roots == NULL || c->root_slots == NULL || c->applies == NULL) {
        c->root_slotcap = 0;
        return false;
    }
    for (uint32_t h = 0; h < c->root_slotcap; h++)
        c->root_slots[h] = -1;
    if (nscopes == 0) {
        scope_add_root(c, c->doc->document, c->nroots++);
        for (int32_t s = 0; s < n; s++)
            set_applies(c, 0, s);
        return true;
    }
    for (int32_t i = 0; i < nscopes; i++) {
        if (c->given[i].root == NULL)
            continue;
        int32_t r = scope_of_root(c, c->given[i].root);
        if (r < 0) {
            r = c->nroots++;
            scope_add_root(c, c->given[i].root, r);
        }
        for (int32_t k = 0; k < c->given[i].n; k++)
            if (c->given[i].sheets[k] >= 0 && c->given[i].sheets[k] < n)
                set_applies(c, r, c->given[i].sheets[k]);
    }
    // An importer follows the sheets it imports, so its bit is set first.
    for (int32_t s = n - 1; s >= 0; s--)
        if (sheets[s].via != NULL && sheets[s].parent > s && sheets[s].parent < n)
            for (int32_t r = 0; r < c->nroots; r++)
                if (applies_in(c, r, sheets[s].parent))
                    set_applies(c, r, s);
    return true;
}

static garb_cascade_t *build(const garb_sheet_in_t *sheets, int32_t n,
                             const garb_scope_in_t *scopes, int32_t nscopes,
                             const os64_html_document_t *doc, garb_env_t env,
                             garb_cascade_limits_t limits);

garb_cascade_t *garb_cascade(const garb_sheet_in_t *sheets, int32_t n,
                             const os64_html_document_t *doc, garb_env_t env)
{
    return garb_cascade_scoped(sheets, n, NULL, 0, doc, env, NULL);
}

garb_cascade_t *garb_cascade_limited(const garb_sheet_in_t *sheets, int32_t n,
                                     const os64_html_document_t *doc, garb_env_t env,
                                     garb_cascade_limits_t limits)
{
    return build(sheets, n, NULL, 0, doc, env, limits);
}

garb_cascade_t *garb_cascade_scoped(const garb_sheet_in_t *sheets, int32_t n,
                                    const garb_scope_in_t *scopes, int32_t nscopes,
                                    const os64_html_document_t *doc, garb_env_t env,
                                    const garb_cascade_limits_t *limits)
{
    return build(sheets, n, scopes, nscopes, doc, env,
                 limits != NULL ? *limits
                                : (garb_cascade_limits_t){GARB_ARENA_MAX, GARB_CASCADE_READ_MAX,
                                                          GARB_ARENA_MAX, SIZE_MAX, NULL, NULL, false});
}

// ── The kept read (KEPT_READ.md) ─────────────────────────────────────

static bool env_equal(garb_env_t a, garb_env_t b)
{
    return a.width == b.width && a.height == b.height && a.dark == b.dark &&
           a.reduced_motion == b.reduced_motion && a.scripting == b.scripting && a.modern_off == b.modern_off &&
           a.device_width == b.device_width && a.device_height == b.device_height && a.resolution == b.resolution;
}

static void read_release(Read *r)
{
    if (r == NULL || --r->refs != 0)
        return;
    garb_free(&r->heap);
    garb_free(&r->reads);
    os64_free(r);
}

// Whether read `r` may be taken for this build, and if not, the first key
// member that differs (cascade.h § THE KEPT READ). The sheets are compared
// by their parse serials, the imports and media as garb_cascade_matches_
// scoped compares them, the scopes' roots by address. A read only ever
// hashes and compares its roots' addresses (scope_of_root) and never
// follows one, and a read is taken only when the caller's scopes, whose
// roots are live now, equal its own root by root: so even an address a
// freed root handed on maps to the same sheets, and the read is equal by
// content (KEPT_READ.md, condition 3).
static garb_read_refusal_t read_key(const Read *r, const garb_sheet_in_t *sheets, int32_t n,
                                    const garb_scope_in_t *scopes, int32_t nscopes,
                                    const os64_html_document_t *doc, garb_env_t env,
                                    garb_cascade_limits_t limits)
{
    if (r == NULL) return GARB_READ_NOT_OFFERED;
    if (!r->keep) return GARB_READ_NOT_KEEPABLE;
    if (r->doc != doc) return GARB_READ_DOCUMENT;
    if (r->state_bytes != limits.state_bytes || r->read_bytes != limits.read_bytes) return GARB_READ_LIMITS;
    if (r->quirks != doc->quirks) return GARB_READ_QUIRKS;
    if (!env_equal(r->env, env)) return GARB_READ_ENVIRONMENT;
    if (r->ninputs != n) return GARB_READ_SHEETS;
    for (int32_t i = 0; i < n; ++i) {
        const CascadeInput *a = &r->inputs[i];
        const garb_sheet_in_t *b = &sheets[i];
        uint64_t serial = b->sheet != NULL ? b->sheet->serial : 0;
        // The serial says the parse is the same one; the address too, since
        // the read keeps the giver's inputs, which garb_cascade_matches_
        // scoped compares by address at every later layout.
        if (a->serial != serial || a->sheet != b->sheet || a->parent != b->parent || a->imported != (b->via != NULL) ||
            ((a->media == NULL) != (b->media == NULL)) || (a->media && !os64_streq(a->media, b->media)))
            return GARB_READ_SHEETS;
        if (a->imported && (a->via.url != b->via->url || a->via.len != b->via->len ||
            a->via.media != b->via->media || a->via.nmedia != b->via->nmedia ||
            a->via.has_supports != b->via->has_supports || a->via.supports != b->via->supports ||
            a->via.nsupports != b->via->nsupports))
            return GARB_READ_SHEETS;
    }
    if (scopes == NULL || nscopes < 0)
        nscopes = 0;
    if (r->ngiven != nscopes) return GARB_READ_SCOPES;
    for (int32_t i = 0; i < nscopes; ++i) {
        const garb_scope_in_t *a = &r->given[i], *b = &scopes[i];
        int32_t bn = b->sheets != NULL && b->n > 0 ? b->n : 0;
        if (a->root != b->root || a->n != bn ||
            (bn > 0 && os64_memcmp(a->sheets, b->sheets, (size_t)bn * sizeof(*a->sheets)) != 0))
            return GARB_READ_SCOPES;
    }
    return GARB_READ_KEPT;
}

// The read's fields between it and a cascade. A cascade takes them with no
// spare room in any array it might append to, so an append copies
// (skips_own copies the skipped records before any write).
static void read_install(garb_cascade_t *c, const Read *r)
{
    c->inputs = r->inputs; c->ninputs = r->ninputs;
    c->given = r->given; c->ngiven = r->ngiven;
    c->roots = r->roots; c->nroots = r->nroots;
    c->applies = r->applies; c->root_slots = r->root_slots; c->root_slotcap = r->root_slotcap;
    c->rules = r->rules; c->nrules = c->rulecap = r->nrules;
    c->keyframes = r->keyframes; c->nkeyframes = c->keyframecap = r->nkeyframes;
    c->keyframes_sorted = r->keyframes_sorted;
    c->entries = r->entries; c->nentries = c->entrycap = r->nentries;
    os64_memcpy(c->index, r->index, sizeof(c->index));
    c->skips = r->skips; c->nskips = c->skipcap = r->nskips; c->skips_lost = r->skips_lost;
    c->faces = r->faces; c->nfaces = c->facecap = r->nfaces; c->faces_lost = r->faces_lost;
    c->allocs_failed = r->allocs_failed;
    c->anchors = r->anchors; c->nanchors = r->nanchors; c->nbare = r->nbare;
    c->subjects = r->subjects; c->nsubjects = r->nsubjects; c->nbare_subjects = r->nbare_subjects;
    c->style_readers = r->style_readers; c->nstyle_readers = r->nstyle_readers; c->style_bare = r->style_bare;
    c->readers = r->readers; c->nreaders = r->nreaders; c->nbare_readers = r->nbare_readers;
    c->nplace_readers = r->nplace_readers;
    c->reads_any = r->reads_any; c->readers_short = r->readers_short;
    c->reader_keys = r->reader_keys; c->nreader_keys = r->nreader_keys; c->up_keys = r->up_keys;
    c->dependencies = r->dependencies;
    c->untrackable = r->untrackable;
    c->incomplete = r->incomplete;
}

static void read_save(Read *r, const garb_cascade_t *c)
{
    r->inputs = c->inputs; r->ninputs = c->ninputs;
    r->given = c->given; r->ngiven = c->ngiven;
    r->roots = c->roots; r->nroots = c->nroots;
    r->applies = c->applies; r->root_slots = c->root_slots; r->root_slotcap = c->root_slotcap;
    r->rules = c->rules; r->nrules = c->nrules;
    r->keyframes = c->keyframes; r->nkeyframes = c->nkeyframes; r->keyframes_sorted = c->keyframes_sorted;
    r->entries = c->entries; r->nentries = c->nentries;
    os64_memcpy(r->index, c->index, sizeof(r->index));
    r->skips = c->skips; r->nskips = c->nskips; r->skips_lost = c->skips_lost;
    r->faces = c->faces; r->nfaces = c->nfaces; r->faces_lost = c->faces_lost;
    r->allocs_failed = c->allocs_failed;
    r->anchors = c->anchors; r->nanchors = c->nanchors; r->nbare = c->nbare;
    r->subjects = c->subjects; r->nsubjects = c->nsubjects; r->nbare_subjects = c->nbare_subjects;
    r->style_readers = c->style_readers; r->nstyle_readers = c->nstyle_readers; r->style_bare = c->style_bare;
    r->readers = c->readers; r->nreaders = c->nreaders; r->nbare_readers = c->nbare_readers;
    r->nplace_readers = c->nplace_readers;
    r->reads_any = c->reads_any; r->readers_short = c->readers_short;
    r->reader_keys = c->reader_keys; r->nreader_keys = c->nreader_keys; r->up_keys = c->up_keys;
    r->dependencies = c->dependencies;
    r->untrackable = c->untrackable;
}

static uint64_t fnv(uint64_t h, const void *bytes, size_t n)
{
    const unsigned char *b = bytes;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
#define FNV_START 14695981039346656037ull

static uint64_t table_raw(uint64_t h, const Table *t)
{
    return t->b != NULL ? fnv(h, t->b, (size_t)t->cap * sizeof(*t->b)) : h;
}

static uint64_t text_hash(uint64_t h, const char *s, size_t n)
{
    h = fnv(h, &n, sizeof(n));
    return s != NULL ? fnv(h, s, n) : h;
}

// THE READ'S RAW HASH: the bytes of every array the read made, as they
// lie (addresses and padding included, all fixed once made), so a write
// made into one after build() shows as a different hash (check_read;
// garb_cascade_read_written). What those arrays point to beyond them, the
// selectors the anchors and readers name and the text declarations hold,
// is the sheets' own parse, which the read borrows and does not hash.
static uint64_t read_digest(const Read *r)
{
    uint64_t h = fnv(FNV_START, r->rules, (size_t)r->nrules * sizeof(*r->rules));
    for (int32_t i = 0; i < r->nrules; i++) {
        const Rule *u = &r->rules[i];
        h = fnv(h, u->decls, (size_t)u->ndecls * sizeof(*u->decls));
        for (int32_t d = 0; d < u->ndecls; d++)
            h = fnv(h, u->decls[d].sets, (size_t)u->decls[d].nsets * sizeof(*u->decls[d].sets));
    }
    h = fnv(h, r->entries, (size_t)r->nentries * sizeof(*r->entries));
    h = fnv(h, r->index, sizeof(r->index));
    for (int t = 0; t < TARGETS; t++)
        h = table_raw(table_raw(table_raw(h, &r->index[t].ids), &r->index[t].classes), &r->index[t].types);
    h = fnv(h, r->keyframes, (size_t)r->nkeyframes * sizeof(*r->keyframes));
    for (int32_t k = 0; k < r->nkeyframes; k++) {
        h = fnv(h, r->keyframes[k].blocks, (size_t)r->keyframes[k].nblocks * sizeof(*r->keyframes[k].blocks));
        h = fnv(h, r->keyframes[k].frames, (size_t)r->keyframes[k].nframes * sizeof(*r->keyframes[k].frames));
        for (int32_t b = 0; b < r->keyframes[k].nblocks; b++) {
            const KeyBlock *kb = &r->keyframes[k].blocks[b];
            h = fnv(h, kb->offsets, (size_t)kb->noffsets * sizeof(*kb->offsets));
            h = fnv(h, kb->decls, (size_t)kb->ndecls * sizeof(*kb->decls));
            for (int32_t d = 0; d < kb->ndecls; d++)
                h = fnv(h, kb->decls[d].sets, (size_t)kb->decls[d].nsets * sizeof(*kb->decls[d].sets));
        }
    }
    h = fnv(h, r->skips, (size_t)r->nskips * sizeof(*r->skips));
    h = fnv(h, r->faces, (size_t)r->nfaces * sizeof(*r->faces));
    for (int32_t f = 0; f < r->nfaces; f++) {
        h = fnv(h, r->faces[f].src, (size_t)r->faces[f].nsrc * sizeof(*r->faces[f].src));
        h = fnv(h, r->faces[f].ranges, (size_t)r->faces[f].nranges * sizeof(*r->faces[f].ranges));
    }
    h = fnv(h, r->anchors, (size_t)r->nanchors * sizeof(*r->anchors));
    h = fnv(h, r->subjects, (size_t)r->nsubjects * sizeof(*r->subjects));
    h = fnv(h, r->style_readers, (size_t)r->nstyle_readers * sizeof(*r->style_readers));
    h = fnv(h, r->readers, (size_t)r->nreaders * sizeof(*r->readers));
    h = fnv(h, r->reader_keys, (size_t)r->nreader_keys * sizeof(*r->reader_keys));
    h = fnv(h, &r->up_keys, sizeof(r->up_keys));
    h = fnv(h, &r->reads_any, sizeof(r->reads_any));
    h = fnv(h, &r->readers_short, sizeof(r->readers_short));
    h = fnv(h, r->inputs, (size_t)r->ninputs * sizeof(*r->inputs));
    for (int32_t i = 0; i < r->ninputs; i++)
        if (r->inputs[i].media != NULL)
            h = text_hash(h, r->inputs[i].media, os64_strlen(r->inputs[i].media));
    h = fnv(h, r->given, (size_t)r->ngiven * sizeof(*r->given));
    for (int32_t i = 0; i < r->ngiven; i++)
        h = fnv(h, r->given[i].sheets, (size_t)r->given[i].n * sizeof(*r->given[i].sheets));
    if (r->applies != NULL)
        h = fnv(h, r->applies, ((size_t)(r->ngiven > 0 ? r->ngiven : 1) * (size_t)(r->ninputs > 0 ? r->ninputs : 1) + 7) / 8);
    h = fnv(h, r->roots, (size_t)r->nroots * sizeof(*r->roots));
    h = fnv(h, r->root_slots, (size_t)r->root_slotcap * sizeof(*r->root_slots));
    h = fnv(h, &r->dependencies, sizeof(r->dependencies));
    h = fnv(h, &r->untrackable, sizeof(r->untrackable));
    return fnv(h, &r->style_bare, sizeof(r->style_bare));
}

// THE READ'S SHAPE HASH, with no address in it: its counts, rule order,
// declaration names and flags, the index's keys, the keyframes' and faces'
// names, the skipped records. Two reads of the same sheets in the same
// environment hash equal whichever cascade made them; a difference in a
// value or a selector need not show (garb_cascade_compare is the check of
// what a cascade decides) (garb_cascade_read_content).
static uint64_t read_content(const Read *r)
{
    uint64_t h = fnv(FNV_START, &r->nrules, sizeof(r->nrules));
    for (int32_t i = 0; i < r->nrules; i++) {
        const Rule *u = &r->rules[i];
        h = fnv(h, &u->order, sizeof(u->order));
        h = fnv(h, &u->sheet, sizeof(u->sheet));
        h = fnv(h, &u->nprelude, sizeof(u->nprelude));
        h = fnv(h, &u->ndecls, sizeof(u->ndecls));
        for (int32_t d = 0; d < u->ndecls; d++) {
            const Decl *x = &u->decls[d];
            h = text_hash(h, x->decl.name, x->decl.len);
            h = fnv(h, &x->decl.nvalue, sizeof(x->decl.nvalue));
            uint8_t flags = (uint8_t)(x->important | x->custom << 1 | x->pending << 2);
            h = fnv(h, &flags, 1);
            h = fnv(h, &x->nsets, sizeof(x->nsets));
        }
    }
    h = fnv(h, &r->nentries, sizeof(r->nentries));
    for (int32_t i = 0; i < r->nentries; i++) {
        h = fnv(h, &r->entries[i].rule, sizeof(r->entries[i].rule));
        h = fnv(h, &r->entries[i].sel, sizeof(r->entries[i].sel));
        h = fnv(h, &r->entries[i].next, sizeof(r->entries[i].next));
    }
    for (int t = 0; t < TARGETS; t++) {
        const Table *tables[3] = {&r->index[t].ids, &r->index[t].classes, &r->index[t].types};
        for (int k = 0; k < 3; k++)
            for (uint32_t b = 0; b < tables[k]->cap; b++)
                if (tables[k]->b[b].key != NULL) {
                    h = text_hash(h, tables[k]->b[b].key, tables[k]->b[b].len);
                    h = fnv(h, &tables[k]->b[b].head, sizeof(tables[k]->b[b].head));
                }
        h = fnv(h, &r->index[t].universal, sizeof(r->index[t].universal));
    }
    h = fnv(h, &r->nkeyframes, sizeof(r->nkeyframes));
    for (int32_t k = 0; k < r->nkeyframes; k++) {
        h = text_hash(h, r->keyframes[k].name, r->keyframes[k].len);
        h = fnv(h, &r->keyframes[k].nblocks, sizeof(r->keyframes[k].nblocks));
        h = fnv(h, &r->keyframes[k].nframes, sizeof(r->keyframes[k].nframes));
    }
    for (int32_t i = 0; i < r->nskips; i++) {
        h = fnv(h, &r->skips[i].kind, sizeof(r->skips[i].kind));
        h = text_hash(h, r->skips[i].name, r->skips[i].len);
        h = fnv(h, &r->skips[i].count, sizeof(r->skips[i].count));
    }
    h = fnv(h, &r->nfaces, sizeof(r->nfaces));
    h = fnv(h, &r->nanchors, sizeof(r->nanchors));
    h = fnv(h, &r->nsubjects, sizeof(r->nsubjects));
    h = fnv(h, &r->nbare_subjects, sizeof(r->nbare_subjects));
    h = fnv(h, &r->nbare, sizeof(r->nbare));
    h = fnv(h, &r->nstyle_readers, sizeof(r->nstyle_readers));
    h = fnv(h, &r->nreaders, sizeof(r->nreaders));
    h = fnv(h, &r->nreader_keys, sizeof(r->nreader_keys));
    h = fnv(h, &r->nplace_readers, sizeof(r->nplace_readers));
    h = fnv(h, &r->nbare_readers, sizeof(r->nbare_readers));
    h = fnv(h, &r->reads_any, sizeof(r->reads_any));
    h = fnv(h, &r->dependencies, sizeof(r->dependencies));
    h = fnv(h, &r->untrackable, sizeof(r->untrackable));
    h = fnv(h, &r->style_bare, sizeof(r->style_bare));
    h = fnv(h, &r->nroots, sizeof(r->nroots));
    return fnv(h, &r->ngiven, sizeof(r->ngiven));
}

// THE READERS' KEYS (select.h § A READER's keys), indexed for a change of
// an attribute: each key of each reader that reads keys, as one entry,
// sorted by kind, then the hash of the name folded to lower case, then the
// folded name (key_order), so the readers of one key lie together and a
// change finds them by a search (keyed_readers). Folded both ways, the
// search may find a reader whose case the selector holds apart: one asked
// too many, never one too few. A reader past its keys' room reads every
// key, under GARB_KEYS_ANY. No room for the index makes every reader
// short (readers_short), which passes every reader everywhere.
static uint32_t key_hash(const char *name, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)name[i];
        h = (h ^ (ch >= 'A' && ch <= 'Z' ? ch + 32 : ch)) * 16777619u;
    }
    return h;
}
static int key_folded_cmp(const char *a, size_t la, const char *b, size_t lb)
{
    for (size_t i = 0; i < la && i < lb; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        x = x >= 'A' && x <= 'Z' ? (unsigned char)(x + 32) : x;
        y = y >= 'A' && y <= 'Z' ? (unsigned char)(y + 32) : y;
        if (x != y) return x < y ? -1 : 1;
    }
    return la < lb ? -1 : la > lb;
}
static int key_order(const void *pa, const void *pb)
{
    const ReaderKey *a = pa, *b = pb;
    if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;
    if (a->hash != b->hash) return a->hash < b->hash ? -1 : 1;
    return key_folded_cmp(a->name, a->len, b->name, b->len);
}
static void reader_keys_build(garb_cascade_t *c)
{
    int32_t total = 0;
    for (int32_t r = 0; r < c->nreaders; r++)
        if (c->readers[r].reads & GARB_READS_KEYS)
            total += c->readers[r].keys_any ? 1 : garb_reader_keys(&c->readers[r], NULL, 0);
    if (total == 0) return;
    ReaderKey *keys = alloc(c, (size_t)total * sizeof(*keys));
    ReaderKey *tmp = os64_malloc((size_t)total * sizeof(*tmp));
    garb_key_t *one = os64_malloc(64 * sizeof(*one));
    if (!keys || !tmp || !one) {
        os64_free(tmp);
        os64_free(one);
        c->readers_short = true;
        return;
    }
    int32_t n = 0;
    for (int32_t r = 0; r < c->nreaders; r++) {
        const garb_reader_t *rd = &c->readers[r];
        if (!(rd->reads & GARB_READS_KEYS)) continue;
        int32_t m = rd->keys_any ? 65 : garb_reader_keys(rd, one, 64);
        if (m > 64) {
            // Past the room for one reader's keys: it reads every key.
            keys[n++] = (ReaderKey){0, GARB_KEYS_ANY, "", 0, r};
            continue;
        }
        for (int32_t k = 0; k < m && n < total; k++)
            keys[n++] = (ReaderKey){key_hash(one[k].name, one[k].len), one[k].kind, one[k].name, one[k].len, r};
    }
    merge_sort(keys, tmp, n, sizeof(*keys), key_order);
    os64_free(tmp);
    os64_free(one);
    c->reader_keys = keys;
    c->nreader_keys = n;
}

// THE READ made: the sheets read into a Read of this cascade's own, its
// arena the heap while it reads (alloc and grow carve from it), its fields
// then saved into it (read_save). False when there was no memory to begin.
static bool read_sheets(garb_cascade_t *c, const garb_sheet_in_t *sheets, int32_t n,
                        const garb_scope_in_t *scopes, int32_t nscopes, garb_cascade_limits_t limits)
{
    Read *r = os64_calloc(1, sizeof(*r));
    if (r == NULL)
        return false;
    r->refs = 1;
    c->read = r;
    r->heap.arena_max = limits.state_bytes;
    r->heap.arena = os64_arena_create(0, limits.state_bytes);
    if (r->heap.arena == NULL)
        return false;
    c->heap = &r->heap;
    for (int32_t t = 0; t < TARGETS; t++)
        c->index[t].universal = -1;
    // The sheets are only read: their rules' blocks, selectors and values
    // go into the read's reading of them, the whole budget each time — or,
    // with no memory for that arena, into the read's own, which is the
    // read's all the same, says so when it runs short, and is never taken
    // by another cascade.
    garb_parsed_t *into = &r->heap;
    if (n > 0) {
        r->reads.arena = os64_arena_create(0, limits.read_bytes);
        r->reads.arena_max = limits.read_bytes;
        if (r->reads.arena != NULL)
            into = &r->reads;
    }
    c->inputs = n > 0 ? alloc(c, (size_t)n * sizeof(*c->inputs)) : NULL;
    if (n > 0 && !c->inputs) return false;
    c->ninputs = n;
    for (int32_t i = 0; i < n; ++i) {
        c->inputs[i].sheet = sheets[i].sheet;
        c->inputs[i].serial = sheets[i].sheet != NULL ? sheets[i].sheet->serial : 0;
        c->inputs[i].parent = sheets[i].parent;
        c->inputs[i].imported = sheets[i].via != NULL;
        if (sheets[i].via) c->inputs[i].via = *sheets[i].via;
        if (sheets[i].media) {
            size_t len = os64_strlen(sheets[i].media) + 1;
            char *copy = alloc(c, len);
            if (!copy) return false;
            os64_memcpy(copy, sheets[i].media, len);
            c->inputs[i].media = copy;
        }
    }
    if (!scopes_take(c, sheets, n, scopes, nscopes))
        return false;
    uint32_t order = 0;
    for (int32_t s = 0; s < n; s++) {
        const garb_parsed_t *sheet = sheets[s].sheet;
        if (!sheet_applies(c, sheets, n, s, 0))
            continue;
        // A sheet whose own parse came up short is short in every cascade
        // that applies it — and only in those: one its @import's conditions
        // or its importer leave out takes nothing from the page.
        if (sheet->incomplete)
            c->incomplete = true;
        add_rules(c, into, sheet->items, sheet->nitems, &order, 0, s);
    }
    int32_t readers = 0;
    for (int32_t i = 0; i < c->nrules; ++i) readers += garb_selectors_style_readers(c->rules[i].sel, NULL, 0);
    if (readers > GARB_STYLE_READERS)
        c->style_bare = true;
    else if (readers > 0 && (c->style_readers = alloc(c, (size_t)readers * sizeof(*c->style_readers))) == NULL)
        c->style_bare = true;
    for (int32_t i = 0; i < c->nrules && c->style_readers != NULL && !c->style_bare; ++i) {
        int32_t held = garb_selectors_style_readers(c->rules[i].sel, c->style_readers + c->nstyle_readers,
                                                    readers - c->nstyle_readers);
        for (int32_t r = 0; r < held; r++)
            c->style_bare |= c->style_readers[c->nstyle_readers + r].bare;
        c->nstyle_readers += held;
    }
    int32_t anchors = 0;
    for (int32_t i = 0; i < c->nrules; ++i) anchors += garb_selectors_anchors(c->rules[i].sel, NULL, 0);
    c->anchors = anchors ? alloc(c, (size_t)anchors * sizeof(*c->anchors)) : NULL;
    int32_t subjects = 0;
    for (int32_t i = 0; i < c->nrules; ++i) subjects += garb_selectors_has_subjects(c->rules[i].sel, NULL, 0);
    c->subjects = subjects ? alloc(c, (size_t)subjects * sizeof(*c->subjects)) : NULL;
    for (int32_t i = 0; i < c->nrules && c->subjects != NULL; ++i) {
        int32_t held = garb_selectors_has_subjects(c->rules[i].sel, c->subjects + c->nsubjects,
                                                   subjects - c->nsubjects);
        for (int32_t a = 0; a < held; a++) c->nbare_subjects += c->subjects[c->nsubjects + a].bare;
        c->nsubjects += held;
    }
    int32_t readers_held = 0;
    for (int32_t i = 0; i < c->nrules; ++i) readers_held += garb_selectors_readers(c->rules[i].sel, NULL, 0);
    c->readers = readers_held ? alloc(c, (size_t)readers_held * sizeof(*c->readers)) : NULL;
    c->readers_short = readers_held > 0 && c->readers == NULL;
    for (int32_t i = 0; i < c->nrules && c->readers != NULL; ++i)
        c->nreaders += garb_selectors_readers(c->rules[i].sel, c->readers + c->nreaders, readers_held - c->nreaders);
    // The readers that read more than keys go first, and a change of
    // children asks only those (readers_at): the rest it can never ask.
    for (int32_t r = 0; r < c->nreaders; r++) {
        c->nbare_readers += c->readers[r].bare;
        c->reads_any |= c->readers[r].reads;
        if (c->readers[r].reads & (uint16_t)~GARB_READS_KEYS) {
            garb_reader_t swap = c->readers[c->nplace_readers];
            c->readers[c->nplace_readers++] = c->readers[r];
            c->readers[r] = swap;
        }
    }
    reader_keys_build(c);
    for (int32_t i = 0; i < c->nrules; ++i) {
        garb_selector_dependencies_t d = garb_selectors_dependencies(c->rules[i].sel);
        c->dependencies.following |= d.following;
        c->dependencies.front_unbounded |= d.front_unbounded;
        c->dependencies.back_unbounded |= d.back_unbounded;
        if (d.front_child > c->dependencies.front_child) c->dependencies.front_child = d.front_child;
        if (d.front_type > c->dependencies.front_type) c->dependencies.front_type = d.front_type;
        if (d.back_child > c->dependencies.back_child) c->dependencies.back_child = d.back_child;
        if (d.back_type > c->dependencies.back_type) c->dependencies.back_type = d.back_type;
        c->dependencies.parent_state |= d.parent_state;
        c->dependencies.filtered |= d.filtered;
        c->dependencies.ancestors |= d.ancestors;
        c->dependencies.ancestors_preceding |= d.ancestors_preceding;
        c->dependencies.style_attribute |= d.style_attribute;
        c->up_keys |= (uint8_t)((d.up_lang ? UP_LANG : 0) | (d.up_dir ? UP_DIR : 0) | (d.up_disabled ? UP_DISABLED : 0));
        if (d.has_filtered) c->untrackable = GARB_UPDATE_COUNTED_HAS;
        if (c->anchors == NULL) continue;
        int32_t held = garb_selectors_anchors(c->rules[i].sel, c->anchors + c->nanchors, anchors - c->nanchors);
        for (int32_t a = 0; a < held; a++)
            c->nbare += c->anchors[c->nanchors + a].bare;
        c->nanchors += held;
    }
    // Without the readers kept, what they read is what the rules hold.
    if (c->readers_short)
        c->reads_any = (uint8_t)((c->dependencies.front_unbounded ? GARB_READS_FRONT_ANY : 0) |
                                 (c->dependencies.back_unbounded ? GARB_READS_BACK_ANY : 0) |
                                 (c->dependencies.parent_state ? GARB_READS_EMPTY : 0) |
                                 (c->dependencies.following ? GARB_READS_NEIGHBOURS : 0) |
                                 GARB_READS_FRONT | GARB_READS_FRONT_TYPE | GARB_READS_BACK | GARB_READS_BACK_TYPE);
    keyframes_settle(c);
    // Each @keyframes read for no element: what an element shares.
    for (int32_t k = 0; k < c->nkeyframes; k++)
        c->keyframes[k].nframes = build_frames(c, &c->keyframes[k], NULL, false,
                                               &c->keyframes[k].frames);
    read_save(r, c);
    r->doc = c->doc;
    r->env = c->env;
    r->quirks = c->quirks;
    r->state_bytes = limits.state_bytes;
    r->read_bytes = limits.read_bytes;
    r->incomplete = c->incomplete || r->heap.incomplete || r->reads.incomplete;
    r->keep = !r->incomplete && (n == 0 || r->reads.arena != NULL);
    r->heap_bytes = os64_arena_stats(r->heap.arena).used_bytes;
    r->digest = limits.check_read ? read_digest(r) : 0;
    // The maker takes the read as a taker does, with no spare room in any
    // array it might append to, so it too copies before an append.
    read_install(c, r);
    c->heap = &c->self;
    return true;
}

typedef enum { INDEX_DONE, INDEX_STOPPED, INDEX_FAILED } IndexWalk;
static const os64_html_node_t *prev_element(const os64_html_node_t *x);
static const os64_html_node_t *next_element(const os64_html_node_t *x);
static IndexWalk index_walk(garb_cascade_t *c, const os64_html_node_t *from, bool compare, bool front);
// The first element sibling of element `x`.
static const os64_html_node_t *first_element(const os64_html_node_t *x)
{
    for (const os64_html_node_t *p = prev_element(x); p; p = prev_element(p)) x = p;
    return x;
}

static garb_cascade_t *build(const garb_sheet_in_t *sheets, int32_t n,
                             const garb_scope_in_t *scopes, int32_t nscopes,
                             const os64_html_document_t *doc, garb_env_t env,
                             garb_cascade_limits_t limits)
{
    if (!limits.state_bytes || !limits.read_bytes || !limits.attribute_bytes)
        return NULL;
    // The caller's measure (garb_cascade_build_cost): the build's parts on
    // its clock, when it gave one.
    uint64_t (*clock)(void) = limits.clock;
    uint64_t began = clock ? clock() : 0;
    garb_cascade_t *c = os64_calloc(1, sizeof(*c));
    if (c == NULL)
        return NULL;
    c->refs = 1;
    c->limits = limits;
    c->limits.read_from = NULL;         // not kept: the caller releases its hold after the build
    c->self.arena_max = limits.state_bytes;
    c->self.arena = os64_arena_create(0, limits.state_bytes);
    if (c->self.arena == NULL) {
        os64_free(c);
        return NULL;
    }
    c->heap = &c->self;
    c->doc = doc;
    c->pin = os64_html_pin(doc);
    if (c->pin == 0) {
        garb_cascade_free(c);
        return NULL;
    }
    static uint64_t generation;
    c->generation = __atomic_add_fetch(&generation, 1, __ATOMIC_RELAXED);
    c->version = os64_html_version(doc);
    c->env = env;
    c->quirks = doc->quirks;
    c->shadowed = os64_html_has_shadow_roots(doc);
    // THE READ: the offered one, with a reference of this cascade's own,
    // when the key is equal (read_key), else read again into a read of
    // this cascade's own (read_sheets).
    Read *offered = (Read *)limits.read_from;
    c->read_refusal = read_key(offered, sheets, n, scopes, nscopes, doc, env, limits);
    if (c->read_refusal == GARB_READ_KEPT) {
        offered->refs++;
        c->read = offered;
        read_install(c, offered);
        c->read_written = limits.check_read && offered->digest != 0 && read_digest(offered) != offered->digest;
    } else if (!read_sheets(c, sheets, n, scopes, nscopes, limits)) {
        garb_cascade_free(c);
        return NULL;
    }
    // A row of bits a slot while the cascade will take updates.
    if (c->untrackable == GARB_UPDATE_DONE && c->nanchors > 0)
        c->has_width = (uint32_t)(c->nanchors + 63) / 64;
    uint64_t sheets_read = clock ? clock() : 0;
    // Every element's slot, in every tree: the table is sized for them.
    size_t elements = 0;
    for (const os64_html_node_t *x = doc->document; x != NULL; x = si_next(x, doc->document, true))
        elements += is_el(x);
    c->slotcap = 64;
    while (c->slotcap < elements * 2 + 2)
        c->slotcap *= 2;
    c->slots = os64_calloc(c->slotcap, sizeof(Slot));
    if (c->has_width) {
        c->has_words = os64_calloc((size_t)c->slotcap * c->has_width, sizeof(*c->has_words));
        c->has_scratch = alloc(c, c->has_width * sizeof(*c->has_scratch));
    }
    // Its marks on the read's rules (RuleSeen), wanted by every match.
    if (c->nrules > 0)
        c->seen = os64_calloc((size_t)c->nrules, sizeof(*c->seen));
    // Without the table or the marks no element is matched: the cascade is
    // short, and holds no element's style.
    if (c->slots == NULL || (c->has_width && (c->has_words == NULL || c->has_scratch == NULL)) ||
        (c->nrules > 0 && c->seen == NULL)) {
        os64_free(c->slots);
        os64_free(c->has_words);
        os64_free(c->seen);
        c->slots = NULL;
        c->has_words = NULL;
        c->seen = NULL;
        c->has_width = 0;
        c->slotcap = 0;
        c->incomplete = true;
        return c;
    }
    Match *m = NULL;
    int32_t mcap = 0;
    AppliedScratch scratch = {0};
    uint64_t sized = clock ? clock() : 0;
    for (const os64_html_node_t *x = doc->document; x != NULL; x = si_next(x, doc->document, true))
        if (is_el(x)) {
            cascade_whole(c, x, &m, &mcap, &scratch);
            Slot *s = slot_for(c, x, false);
            if (s != NULL) anchor_bits(c, x, has_row(c, s));
            // The position indexes, one pass each way for each parent once
            // its last element child is styled, and its siblings with it.
            if (next_element(x) == NULL) {
                if (c->dependencies.back_child || c->dependencies.back_type) index_walk(c, x, false, false);
                if (c->dependencies.front_child || c->dependencies.front_type)
                    index_walk(c, first_element(x), false, true);
            }
        }
    os64_free(scratch.items);
    os64_free(scratch.produced);
    os64_free(scratch.nproduced);
    if (clock) {
        uint64_t matched = clock();
        c->cost = (garb_build_cost_t){sheets_read - began, sized - sheets_read, matched - sized, (uint32_t)c->nrules,
                                      (uint32_t)elements};
    }
    // What the cascade read ran short wherever it was read: a sheet's own
    // parse (taken in above, for the sheets the cascade applied), the rule
    // blocks, selectors and values read from them, which mark this
    // cascade's reading of them, and what is read per element, which marks
    // the cascade's own. Any of them short, or a style attribute's parse,
    // and the cascade is short.
    bool short_read = c->self.incomplete || c->read->incomplete;
    for (int32_t i = 0; i < c->nattrs; i++)
        short_read |= c->attrs[i].incomplete;
    if (short_read)
        c->incomplete = true;
    // What updates may retain is held to the initial state's size, the
    // read's heap counted in (what lay in `self` before the read had an
    // arena of its own; the declarations' arena was never counted),
    // whether this cascade made the read or took it.
    c->initial_state_bytes = os64_arena_stats(c->self.arena).used_bytes;
    size_t whole = c->initial_state_bytes + c->read->heap_bytes;
    c->retained_budget = whole > 4096 ? whole : 4096;
    // A cascade that must rebuild cannot consume invalidations. Registering
    // it would still make DOM/parser mutations allocate and retain log entries.
    if (!c->incomplete && c->untrackable == GARB_UPDATE_DONE)
        c->reader = os64_html_reader_new((os64_html_document_t *)doc, c->version);
    return c;
}

static const os64_html_node_t *subtree_next(const os64_html_node_t *x,
                                              const os64_html_node_t *root, bool children)
{
    if (children && x->first_child) return x->first_child;
    while (x != root && !x->next) x = x->parent;
    return x == root ? NULL : x->next;
}
// In what the cascade matches: the document's tree and every shadow tree
// connected to it (SHADOW_DOM.md § 6).
static bool connected(const garb_cascade_t *c, const os64_html_node_t *x)
{
    return x != NULL && os64_html_shadow_including_root(x) == c->doc->document;
}
static bool is_slot(const os64_html_node_t *x)
{
    return is_el(x) && x->ns == OS64_HTML_NS_HTML && x->tag == OS64_HTML_TAG_SLOT;
}
// Puts `x`'s slot on the dirty list. False when the list cannot grow, and
// when the dirty slots pass half of them: a broad update would recompute
// most winners and retain the old versions too, so the fresh build is
// asked for before any winners are produced.
static bool dirty_add(garb_cascade_t *c, const os64_html_node_t *x, Slot *s)
{
    if (c->ndirty == c->dirtycap) {
        size_t cap = c->dirtycap ? c->dirtycap * 2 : 32;
        void *more = cap <= SIZE_MAX / sizeof(*c->dirty_nodes)
                         ? os64_realloc(c->dirty_nodes, cap * sizeof(*c->dirty_nodes)) : NULL;
        if (!more) { c->refusal = GARB_UPDATE_NO_MEMORY; return false; }
        c->dirty_nodes = more; c->dirtycap = cap;
    }
    s->dirty = true; c->dirty_nodes[c->ndirty++] = x;
    if (c->limits.clock) c->mark_by[c->mark_now]++;
    c->marks.by[c->mark_now]++;
    if (c->ndirty > c->slotcount / 2) { c->refusal = GARB_UPDATE_BROAD; return false; }
    return true;
}
// What a slot `x` passes its style on to: the nodes assigned to it
// (`::slotted()`, inheritance).
static bool mark_subtree(garb_cascade_t *c, const os64_html_node_t *root);
static bool mark_assigned(garb_cascade_t *c, const os64_html_node_t *x)
{
    if (c->shadowed && is_slot(x))
        for (size_t i = 0, n = os64_html_slot_assigned_count(c->doc, x); i < n; i++) {
            const os64_html_node_t *a = os64_html_slot_assigned(c->doc, x, i);
            if (is_el(a) && !mark_subtree(c, a)) return false;
        }
    return true;
}
// Marks the elements under `root` whose winners may have changed: its
// shadow-including descendants, and what a slot among them passes on. An
// element already dirty with its subtree is passed over with it; one dirty
// alone (mark_element) now owes its subtree too.
static bool mark_subtree(garb_cascade_t *c, const os64_html_node_t *root)
{
    if (!connected(c, root)) return true;
    for (const os64_html_node_t *x = root; x;) {
        if (!is_el(x)) { x = si_next(x, root, true); continue; }
        Slot *s = slot_for(c, x, false);
        if (!s) { c->refusal = GARB_UPDATE_NO_MEMORY; return false; }
        if (s->dirty && !s->element_only) { x = si_next(x, root, false); continue; }
        if (s->dirty) s->element_only = false;
        else if (!dirty_add(c, x, s)) return false;
        if (!mark_assigned(c, x)) return false;
        x = si_next(x, root, true);
    }
    return true;
}
// Marks `x` alone: a :has() in its rules' subjects may have turned on it.
static bool mark_element(garb_cascade_t *c, const os64_html_node_t *x)
{
    if (!is_el(x) || !connected(c, x)) return true;
    Slot *s = slot_for(c, x, false);
    if (!s) { c->refusal = GARB_UPDATE_NO_MEMORY; return false; }
    if (s->dirty) return true;
    s->element_only = true;
    return dirty_add(c, x, s);
}
// Marks every child of the host whose shadow tree holds `node`: a change
// there may have moved which slot each is assigned to.
static bool mark_host_children(garb_cascade_t *c, const os64_html_node_t *node)
{
    const os64_html_node_t *root = node;
    while (root->parent) root = root->parent;
    const os64_html_node_t *host = os64_html_shadow_host(root);
    for (const os64_html_node_t *k = host ? host->first_child : NULL; k; k = k->next)
        if (is_el(k) && !mark_subtree(c, k)) return false;
    return true;
}
// Whether the subtree under `root` holds a slot.
static bool holds_slot(const os64_html_node_t *root)
{
    for (const os64_html_node_t *x = root; x; x = subtree_next(x, root, true))
        if (is_slot(x)) return true;
    return false;
}
static bool mark_siblings(garb_cascade_t *c, const os64_html_node_t *first)
{
    for (const os64_html_node_t *x = first; x; x = x->next) {
        if (!is_el(x)) continue;
        Slot *s = slot_for(c, x, false);
        // A previous sibling walk covered this suffix. Parser feeds can
        // append many siblings in one batch; do not walk that suffix again.
        if (s && s->dirty_suffix) break;
        if (!mark_subtree(c, x)) return false;
        s = slot_for(c, x, false);
        s->dirty_suffix = true;
    }
    return true;
}
// `node`'s subtree, marked as `mark` (for the caller's measure), and the
// siblings its change can move.
static bool mark_changed(garb_cascade_t *c, const os64_html_node_t *node, garb_mark_t mark)
{
    if (!connected(c, node)) return true;
    c->mark_now = mark;
    if (!mark_subtree(c, node)) return false;
    c->mark_now = GARB_MARK_SIBLINGS;
    if (c->dependencies.filtered && node->parent)
        return mark_siblings(c, node->parent->first_child);
    return !c->dependencies.following || mark_siblings(c, node->next);
}
// :empty, ASKED AGAIN: an element's answer turns only at a change of its
// children or of a text child's data, so those two changes ask it again
// (garb_element_empty, the matcher's own answer) and compare it with the
// one the slot kept from when the cascade last styled or asked it. Same
// answer, nothing to mark for :empty. True when it turned, or when there is
// no slot to keep it (a cascade short of memory), which marks as a turn.
// The batch's later changes at the same element find the answer already
// taken: it is read from the document as the batch left it. An element the
// batch added keeps no answer until it is styled (cascade_element), so a
// change under it may find a turn that is none. What that marks the change
// that added it has marked already: the element as added; the siblings an
// :empty reader with a `+`/`~` reach passing at it reaches, which the same
// reader reaches from the element brought (mark_neighbours), or as added
// beside it; every child, for an `of S`.
static bool empty_turned(garb_cascade_t *c, const os64_html_node_t *x)
{
    Slot *s = slot_for(c, x, false);
    if (!s) return true;
    bool now = garb_element_empty(x);
    bool turned = now != s->empty;
    s->empty = now;
    return turned;
}
// ── THE READERS, ASKED (select.h § A READER; RELAYOUT.md § Restyle) ─────
//
// A change of a parent's children moves where its children stand without
// touching them: their indexes from the start and from the end, which
// element is beside which. What can turn is the match of a reader at a
// child, and what that turns is the reader's reach from the child. So each
// such fact the change moved is asked of the readers that read it, at the
// element it moved at, and only the reach of those whose filters pass there
// is marked.

static const os64_html_node_t *prev_element(const os64_html_node_t *x)
{
    for (x = x->prev; x && !is_el(x); x = x->prev) {}
    return x;
}
static const os64_html_node_t *next_element(const os64_html_node_t *x)
{
    for (x = x->next; x && !is_el(x); x = x->next) {}
    return x;
}
// The first element at or after `x`.
static const os64_html_node_t *element_at(const os64_html_node_t *x)
{
    return x == NULL || is_el(x) ? x : next_element(x);
}

// THE BATCH, read whole before any change of it is marked (batch_take): the
// nodes its changes of children took out and put in, and the elements whose
// attributes it changed, each sorted by address, so a question about the
// batch is a search however long it is, and a change later in the batch
// answers as well as one before.
static int node_order(const void *a, const void *b)
{
    uintptr_t x = (uintptr_t)*(const os64_html_node_t *const *)a, y = (uintptr_t)*(const os64_html_node_t *const *)b;
    return x < y ? -1 : x > y;
}
static bool batch_holds(const os64_html_node_t *const *v, size_t n, const os64_html_node_t *x)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (v[mid] == x) return true;
        if ((uintptr_t)v[mid] < (uintptr_t)x) lo = mid + 1;
        else hi = mid;
    }
    return false;
}
static bool batch_push(garb_cascade_t *c, int which, const os64_html_node_t *x)
{
    const os64_html_node_t ***v = which == 0 ? &c->batch_removed : which == 1 ? &c->batch_added : &c->batch_attrs;
    size_t *n = which == 0 ? &c->nbatch_removed : which == 1 ? &c->nbatch_added : &c->nbatch_attrs;
    if (*n == c->batch_cap[which]) {
        size_t cap = c->batch_cap[which] ? c->batch_cap[which] * 2 : 32;
        void *more = cap <= INT32_MAX ? os64_realloc((void *)*v, cap * sizeof(**v)) : NULL;
        if (!more) return false;
        *v = more;
        c->batch_cap[which] = cap;
    }
    (*v)[(*n)++] = x;
    return true;
}
static bool batch_take(garb_cascade_t *c, const os64_html_change_entry_t *changes)
{
    c->nbatch_removed = c->nbatch_added = c->nbatch_attrs = 0;
    for (const os64_html_change_entry_t *e = changes; e; e = e->next) {
        if (e->kind == OS64_HTML_CHANGE_CHILDREN) {
            for (size_t i = 0; i < e->nremoved; ++i)
                if (!batch_push(c, 0, e->removed[i])) return false;
            for (size_t i = 0; i < e->nadded; ++i)
                if (!batch_push(c, 1, e->added[i])) return false;
        } else if (e->kind == OS64_HTML_CHANGE_ATTR && e->target && !batch_push(c, 2, e->target)) {
            return false;
        }
    }
    size_t most = c->nbatch_removed > c->nbatch_added ? c->nbatch_removed : c->nbatch_added;
    if (c->nbatch_attrs > most) most = c->nbatch_attrs;
    if (most >= 2) {
        void *tmp = os64_malloc(most * sizeof(*c->batch_removed));
        if (!tmp) return false;
        merge_sort((void *)c->batch_removed, tmp, (int32_t)c->nbatch_removed, sizeof(*c->batch_removed), node_order);
        merge_sort((void *)c->batch_added, tmp, (int32_t)c->nbatch_added, sizeof(*c->batch_added), node_order);
        merge_sort((void *)c->batch_attrs, tmp, (int32_t)c->nbatch_attrs, sizeof(*c->batch_attrs), node_order);
        os64_free(tmp);
    }
    // Whether a change of children happened where the batch may have had it
    // out of the document: under something it took out (wherever that
    // stands now, put back or not), or out of the document still.
    c->batch_detached = false;
    for (const os64_html_change_entry_t *e = changes; e && !c->batch_detached; e = e->next) {
        if (e->kind != OS64_HTML_CHANGE_CHILDREN) continue;
        c->batch_detached = !connected(c, e->target);
        for (const os64_html_node_t *x = e->target; x && !c->batch_detached; x = x->parent)
            c->batch_detached = batch_holds(c->batch_removed, c->nbatch_removed, x);
    }
    return true;
}

// AN ELEMENT STANDING IN FOR WHAT IT WAS. A reader is asked whether a
// change turned its match at an element the change did not bring: the
// element before a change of children, one the batch took out, one whose
// index turned. It can only be asked of the element as the batch left it,
// which stands for what it was before the batch only where the batch
// changed none of its attributes; one whose attributes it changed, earlier
// in the batch or later, passes every reader (`stale`). A taken-out subtree
// stands for what it held only where no change of children happened under
// anything the batch took out, or outside the document (`stale_removed`).
static bool stale(void *ctx, const os64_html_node_t *x)
{
    const garb_cascade_t *c = ctx;
    return batch_holds(c->batch_attrs, c->nbatch_attrs, x);
}
static bool stale_removed(void *ctx, const os64_html_node_t *x)
{
    const garb_cascade_t *c = ctx;
    return c->batch_detached || stale(ctx, x);
}
// Whether `x`, a sibling a change of `parent`'s children happened beside,
// still stands where it stood then, as the batch left the document: under
// `parent`, and neither taken out nor moved by the batch (a move takes a
// node out and puts it in again). NULL, an end, always stands.
static bool stands(const garb_cascade_t *c, const os64_html_node_t *x, const os64_html_node_t *parent)
{
    return x == NULL || (x->parent == parent && !batch_holds(c->batch_removed, c->nbatch_removed, x));
}
// Whether a change of children brought or took an element: one that moved
// only text moved no element's place.
static bool moves_elements(const os64_html_change_entry_t *e)
{
    for (size_t i = 0; i < e->nadded; ++i)
        if (is_el(e->added[i])) return true;
    for (size_t i = 0; i < e->nremoved; ++i)
        if (is_el(e->removed[i])) return true;
    return false;
}

// What readers reach, gathered: the element itself, its next element
// sibling, its following ones; `down`, each with its subtree.
typedef struct {
    bool self, next, on, down;
} Reached;
#define REACH_ANY 0xFu
#define REACH_SIBLINGS ((1u << GARB_REACH_NEXT) | (1u << GARB_REACH_FOLLOWING) | (1u << GARB_REACH_CHAIN))
// The reach of every reader that reads any of `key`, with a reach in
// `kinds` (a bit for each garb_reach_t), whose filters pass at `x`; a
// reader passes every element for a key it reads `untested`, and every
// reader passes where `x` is NULL (the element is past finding) or is
// `stand_in` and stale. Counted under `asker`. Without the readers kept,
// everything is reached.
static Reached readers_at(garb_cascade_t *c, uint8_t key, unsigned kinds, const os64_html_node_t *x,
                          bool stand_in, garb_asker_t asker)
{
    garb_reader_work_t *w = &c->reader_work;
    if (c->readers_short) {
        w->asked[asker]++, w->passed[asker]++, w->reached[asker]++;
        return (Reached){true, true, true, true};
    }
    Reached got = {0};
    bool all = x == NULL || (stand_in && stale(c, x)), any = false;
    for (int32_t i = 0; i < c->nplace_readers; i++) {
        const garb_reader_t *r = &c->readers[i];
        if (!(r->reads & key) || !(kinds >> r->reach & 1)) continue;
        w->asked[asker]++;
        if (!all && !(r->untested & r->reads & key) && !garb_reader_filters_pass(r, x)) continue;
        w->passed[asker]++;
        any = true;
        got.self |= r->reach == GARB_REACH_SELF;
        got.next |= r->reach == GARB_REACH_NEXT;
        got.on |= r->reach == GARB_REACH_FOLLOWING || r->reach == GARB_REACH_CHAIN;
        got.down |= r->down;
    }
    if (any) w->reached[asker]++;
    return got;
}
static void reached_add(Reached *into, Reached got)
{
    into->self |= got.self, into->next |= got.next, into->on |= got.on, into->down |= got.down;
}
// Marks `first`, alone or with its subtree, and with `on` each element
// sibling after it too. A walk of following siblings that comes to an
// element an earlier walk marked as far (dirty_suffix, alone_suffix) stops
// there: that walk marked the rest.
static bool mark_run(garb_cascade_t *c, const os64_html_node_t *first, bool on, bool down)
{
    if (first == NULL) return true;
    if (on && down) return mark_siblings(c, first);
    if (!on) return down ? mark_subtree(c, first) : mark_element(c, first);
    for (const os64_html_node_t *x = first; x; x = x->next) {
        if (!is_el(x)) continue;
        Slot *s = slot_for(c, x, false);
        if (s && (s->alone_suffix || s->dirty_suffix)) break;
        if (!mark_element(c, x)) return false;
        s = slot_for(c, x, false);
        if (s) s->alone_suffix = true;
    }
    return true;
}
// Marks what `got` reaches from `x`.
static bool mark_reached(garb_cascade_t *c, const os64_html_node_t *x, Reached got)
{
    if (got.self && !(got.down ? mark_subtree(c, x) : mark_element(c, x))) return false;
    return !(got.on || got.next) || mark_run(c, next_element(x), got.on, got.down);
}

// What a turned :empty at `x` can move: its own match, marked alone (what
// is under it is owed a recompute where its custom property map changed,
// restyle, as any element's is); every child of its parent for an `of S`; and the
// siblings the readers of :empty that pass at `x` reach (`p:empty + q`).
//
// WHY NOTHING UNDER IT IS MARKED FOR MATCHING (`p:not(:empty) > x`, `:empty
// x`): an element that turned from empty has under it only what the batch
// added, all marked as added; one that turned to empty has no element under
// it. A text change adds no element under it; what the batch added beside
// it is the first case. The
// exceptions are elements a selector reaches from it that the batch did not
// add: a shadow host's tree (`:host(:empty) .inner`) and a slot's assigned
// nodes, so a host or a slot keeps the whole mark (mark_changed).
static bool mark_turned(garb_cascade_t *c, const os64_html_node_t *x)
{
    if (x->shadow_root || is_slot(x)) return mark_changed(c, x, GARB_MARK_PARENT);
    c->mark_now = GARB_MARK_PARENT;
    if (!mark_element(c, x)) return false;
    c->mark_now = GARB_MARK_SIBLINGS;
    if (c->dependencies.filtered && x->parent)
        return mark_siblings(c, x->parent->first_child);
    if (!(c->reads_any & GARB_READS_EMPTY)) return true;
    Reached got = readers_at(c, GARB_READS_EMPTY, REACH_SIBLINGS, x, true, GARB_ASKED_EMPTY);
    got.self = false;
    return mark_reached(c, x, got);
}

// THE POSITION INDEXES, COUNTED AGAIN (select.h § THE POSITION FORMS): each
// styled element's slot keeps its count from the end among its element
// siblings and among those of its type, and the same from the start, each
// saturated at one past the number the rules' bounded forms need on that
// side (index_bound). Every such form's answer is a function of the
// saturated number, so where it did not change no bounded form's answer
// did. A change of a parent's children counts them again, from the last for
// the back and from the first for the front, compares, and marks each child
// whose number turned through the readers of what turned (readers_at): the
// child alone for a subject (`li:last-child`), with its subtree where a
// descendant or child combinator stands right of the form (`li:last-child
// span`), and the siblings after it that a `+`/`~` reads (`p:last-of-type ~
// span` moves the spans between the p that turned and a p appended after
// them, whose own counts stay). A child whose attributes the batch changed
// passes every reader (stale). A child never counted (added in the batch,
// or past the type table at a build) counts as turned. The walk reads the
// document as the batch left it, so removals and moves need no rule of
// their own.
//
// WHERE IT STOPS EARLY, with no -of-type form on its side: at the first
// child in the walk's order whose kept number and new number are both the
// bound. A child the batch added keeps 0, or a number an earlier walk of
// this batch counted from the document as the batch left it; so the stop is
// at a child the batch did not add, or at one that walk passed, which
// proves the same. The children the batch did not add or move keep their
// order, so each one past the stop had a larger index both before and
// after, saturated both times; what the batch added past it is marked as
// added, and its number, which is the bound for the same reason, is taken
// at the change (index_turns). With an -of-type form the walk counts every
// child. A parent with more types than GARB_INDEX_TYPES has all its children
// marked at a change, and the children past the table, at a change or a
// build, are left uncounted, so that they count as turned the next time.
#define GARB_INDEX_TYPES 32
static uint8_t index_bound(uint8_t need) { return need ? (uint8_t)(need + 1) : 0; }
// Counts the element siblings from `from`, the last when counting from the
// end and the first when counting from the start (`front`). `compare`: a
// change's walk, which marks what turned and may stop early; otherwise the
// numbers are only taken (a build, an added subtree).
static IndexWalk index_walk(garb_cascade_t *c, const os64_html_node_t *from, bool compare, bool front)
{
    uint8_t kc = index_bound(front ? c->dependencies.front_child : c->dependencies.back_child);
    uint8_t kt = index_bound(front ? c->dependencies.front_type : c->dependencies.back_type);
    uint8_t child_key = front ? GARB_READS_FRONT : GARB_READS_BACK;
    uint8_t type_key = front ? GARB_READS_FRONT_TYPE : GARB_READS_BACK_TYPE;
    struct { const os64_html_node_t *type; uint8_t n; } types[GARB_INDEX_TYPES];
    int32_t ntypes = 0;
    uint8_t n = 0;
    for (const os64_html_node_t *x = from; x; x = front ? next_element(x) : prev_element(x)) {
        if (compare && front) c->reader_work.front_walked++;
        else if (compare) c->back_work.walked++;
        uint8_t t = 0;
        if (kt) {
            int32_t i = 0;
            while (i < ntypes && !garb_element_same_type(types[i].type, x)) i++;
            if (i == GARB_INDEX_TYPES) {
                for (const os64_html_node_t *y = x; y; y = front ? next_element(y) : prev_element(y)) {
                    Slot *u = slot_for(c, y, false);
                    if (u && front) u->front = u->front_type = 0;
                    else if (u) u->back = u->back_type = 0;
                }
                if (!compare) return INDEX_DONE;
                c->mark_now = GARB_MARK_SUFFIX;
                return x->parent && mark_siblings(c, x->parent->first_child) ? INDEX_DONE : INDEX_FAILED;
            }
            if (i == ntypes) {
                types[i].type = x;
                types[i].n = 0;
                ntypes++;
            }
            if (types[i].n < kt) types[i].n++;
            t = types[i].n;
        }
        if (n < kc) n++;
        Slot *s = slot_for(c, x, false);
        if (!s) {
            if (!compare) continue;
            c->refusal = GARB_UPDATE_NO_MEMORY;
            return INDEX_FAILED;
        }
        uint8_t *kept = front ? &s->front : &s->back, *kept_type = front ? &s->front_type : &s->back_type;
        if (compare && !kt && *kept == kc && n == kc) return INDEX_STOPPED;
        uint8_t turned = (uint8_t)((*kept != n ? child_key : 0) | (*kept_type != t ? type_key : 0));
        *kept = n;
        *kept_type = t;
        if (compare && turned) {
            if (front) c->reader_work.front_turned++;
            else c->back_work.turned++;
            c->mark_now = front ? GARB_MARK_FRONT : GARB_MARK_BACK;
            Reached got = readers_at(c, turned, REACH_ANY, x, true, front ? GARB_ASKED_FRONT : GARB_ASKED_BACK);
            if (!mark_reached(c, x, got)) return INDEX_FAILED;
        }
    }
    return INDEX_DONE;
}
// A change of `e->target`'s children, counted again from the start
// (`front`) or from the end (index_walk), once an update for a parent that
// has a slot: every walk reads the document as the batch left it, so a
// second walk of the same parent finds nothing to turn. Without an -of-type
// form, a child the change added that is still uncounted lies past where
// the walk stopped, so it takes the bound as its number.
static bool index_turns(garb_cascade_t *c, const os64_html_change_entry_t *e, bool front)
{
    const os64_html_node_t *from = element_at(e->target->first_child);
    if (!front) {
        from = e->target->last_child;
        while (from && !is_el(from)) from = from->prev;
    }
    if (!from) return true;
    Slot *p = is_el(e->target) ? slot_for(c, e->target, false) : NULL;
    uint64_t *walked = p == NULL ? NULL : front ? &p->front_walked : &p->back_walked;
    if (!walked || *walked != c->updates + 1) {
        if (walked) *walked = c->updates + 1;
        if (front) c->reader_work.front_walks++;
        else c->back_work.walks++;
        if (index_walk(c, from, true, front) == INDEX_FAILED) return false;
    }
    if (front ? c->dependencies.front_type : c->dependencies.back_type) return true;
    uint8_t bound = index_bound(front ? c->dependencies.front_child : c->dependencies.back_child);
    for (size_t i = 0; i < e->nadded; ++i) {
        Slot *s = e->added[i]->parent == e->target ? slot_for(c, e->added[i], false) : NULL;
        uint8_t *kept = s == NULL ? NULL : front ? &s->front : &s->back;
        if (kept && *kept == 0) *kept = bound;
    }
    return true;
}

// THE UNBOUNDED POSITION FORMS (`:nth-child(3n)`, `:nth-last-child(2n+1)`)
// never settle, so no count kept can say whether one turned: a change of
// children walks the siblings whose index it moved, the element after the
// change and those after it for a front form (what the change brought
// before that element is marked as added), and those before the element
// after the change for a back form, and marks
// at each the reach of the readers whose filters pass there (a form with
// no filter, or inside a :not(), passes every sibling, which is every
// moved sibling marked as before). Once an update for a parent: where every
// change of the batch moved its parent's indexes is planned before any is
// marked (walks_plan), so the one walk covers them all. Where the element
// after a change no longer stands (stands), or for a back form there is
// none, every child is walked.
enum { WALK_FROM = 1, WALK_TO = 2, WALK_ALL_FRONT = 4, WALK_ALL_BACK = 8, WALK_SOME_FRONT = 16 };
static bool walk_flag(garb_cascade_t *c, const os64_html_node_t *x, uint8_t flag)
{
    Slot *s = x ? slot_for(c, x, false) : NULL;
    if (!s) return false;
    if (s->walk_update != c->updates + 1) {
        s->walk_update = c->updates + 1;
        s->walk_flags = 0;
    }
    s->walk_flags |= flag;
    return true;
}
static bool walk_flagged(const garb_cascade_t *c, const os64_html_node_t *x, uint8_t flag)
{
    const Slot *s = slot_for(c, x, false);
    return s && s->walk_update == c->updates + 1 && (s->walk_flags & flag);
}
static void walks_plan(garb_cascade_t *c, const os64_html_change_entry_t *changes)
{
    bool front = c->reads_any & GARB_READS_FRONT_ANY, back = c->reads_any & GARB_READS_BACK_ANY;
    if (!front && !back) return;
    for (const os64_html_change_entry_t *e = changes; e; e = e->next) {
        const os64_html_node_t *parent = e->target;
        if (e->kind != OS64_HTML_CHANGE_CHILDREN || !moves_elements(e) || !connected(c, parent) ||
            !is_el(parent) || !slot_for(c, parent, false))
            continue;
        const os64_html_node_t *n = e->next_sibling;
        const os64_html_node_t *at = stands(c, n, parent) ? element_at(n) : NULL;
        bool lost = !stands(c, n, parent);
        if (front && (lost || (at && !walk_flag(c, at, WALK_FROM))))
            walk_flag(c, parent, WALK_ALL_FRONT);
        else if (front && at)
            walk_flag(c, parent, WALK_SOME_FRONT);
        if (back && (!at || !walk_flag(c, at, WALK_TO)))
            walk_flag(c, parent, WALK_ALL_BACK);
    }
}
static bool position_walks(garb_cascade_t *c, const os64_html_change_entry_t *e)
{
    bool front = c->reads_any & GARB_READS_FRONT_ANY, back = c->reads_any & GARB_READS_BACK_ANY;
    if (!front && !back) return true;
    const os64_html_node_t *parent = e->target;
    Slot *p = is_el(parent) ? slot_for(c, parent, false) : NULL;
    if (p && p->unbounded_walked == c->updates + 1) return true;
    if (p) p->unbounded_walked = c->updates + 1;
    c->mark_now = GARB_MARK_POSITION;
    // An append moved no front index: nothing to walk from the front.
    if (front && (!p || walk_flagged(c, parent, WALK_ALL_FRONT | WALK_SOME_FRONT))) {
        c->reader_work.walks++;
        bool on = !p || walk_flagged(c, parent, WALK_ALL_FRONT);
        for (const os64_html_node_t *x = element_at(parent->first_child); x; x = next_element(x)) {
            on = on || walk_flagged(c, x, WALK_FROM);
            if (!on) continue;
            c->reader_work.walked++;
            Reached got = readers_at(c, GARB_READS_FRONT_ANY, REACH_ANY, x, true, GARB_ASKED_FRONT_WALK);
            if (!mark_reached(c, x, got)) return false;
        }
    }
    if (back) {
        c->reader_work.walks++;
        bool on = !p || walk_flagged(c, parent, WALK_ALL_BACK);
        const os64_html_node_t *x = parent->last_child;
        while (x && !is_el(x)) x = x->prev;
        for (; x; x = prev_element(x)) {
            if (!on) { on = walk_flagged(c, x, WALK_TO); continue; }
            c->reader_work.walked++;
            Reached got = readers_at(c, GARB_READS_BACK_ANY, REACH_ANY, x, true, GARB_ASKED_BACK_WALK);
            if (!mark_reached(c, x, got)) return false;
        }
    }
    return true;
}

// THE NEIGHBOURS OF A CHANGE OF CHILDREN: what a `+` or `~` reads. An
// element brought or taken (`X`) moves who follows it: the readers whose
// reach is `~` or a `+` chain and that pass at X reach the siblings after
// the change (the element after it, `N`, the first after the change that
// the batch did not bring, and those after N; what stands between came with
// the batch and is marked as added). A taken element is asked as it was
// (stale). And the change breaks one pair of neighbours and makes one, at
// its end: before it (P, N), or (the last taken, N); after it (the last
// brought, N), or (P, N). A reader whose first combinator to the right is
// `+` reads the pair: one that passes at the left element reaches N for `+`
// alone and N and those after it for a chain, with subtrees where it says
// so. The pair made is asked of N's previous element sibling as the batch
// left it. The pair broken is asked of the last element taken, or else of
// the element before the change, standing in for what it was; where the
// batch brought that element it may not have stood there then, and every
// reader passes. A true `~` reader that passes at P does nothing: `P ~ N`
// holds before and after.
//
// Where N no longer stands where the change happened (stands), the
// siblings after the change are past finding, and every child is marked as
// far as any reader reaches. With nothing after the change nothing moved.
static bool mark_neighbours(garb_cascade_t *c, const os64_html_change_entry_t *e)
{
    if (!(c->reads_any & GARB_READS_NEIGHBOURS)) return true;
    const os64_html_node_t *parent = e->target, *n = e->next_sibling;
    c->mark_now = GARB_MARK_NEIGHBOURS;
    if (!stands(c, n, parent)) {
        Reached all = readers_at(c, GARB_READS_NEIGHBOURS, REACH_SIBLINGS, NULL, false, GARB_ASKED_ADDED_REMOVED);
        return mark_run(c, element_at(parent->first_child), true, all.down);
    }
    // The element after the change as it stood then: the first at or after
    // N that the batch did not bring, since what it brought there since is
    // marked as added and may stand between.
    const os64_html_node_t *after = element_at(n);
    while (after && batch_holds(c->batch_added, c->nbatch_added, after)) after = next_element(after);
    if (!after) return true;
    unsigned run = (1u << GARB_REACH_FOLLOWING) | (1u << GARB_REACH_CHAIN);
    Reached moved = {0};
    for (size_t i = 0; i < e->nadded && !(moved.on && moved.down); ++i)
        if (is_el(e->added[i]))
            reached_add(&moved, readers_at(c, GARB_READS_NEIGHBOURS, run, e->added[i], false, GARB_ASKED_ADDED_REMOVED));
    for (size_t i = 0; i < e->nremoved && !(moved.on && moved.down); ++i)
        if (is_el(e->removed[i]))
            reached_add(&moved, readers_at(c, GARB_READS_NEIGHBOURS, run, e->removed[i], true, GARB_ASKED_ADDED_REMOVED));
    if (moved.on && !mark_run(c, after, true, moved.down)) return false;
    c->mark_now = GARB_MARK_ADJACENT;
    unsigned plus = (1u << GARB_REACH_NEXT) | (1u << GARB_REACH_CHAIN);
    Reached pair = {0};
    const os64_html_node_t *made = prev_element(after);
    if (made) reached_add(&pair, readers_at(c, GARB_READS_NEIGHBOURS, plus, made, false, GARB_ASKED_PAIRS));
    const os64_html_node_t *broken = NULL;
    for (size_t i = e->nremoved; i > 0 && !broken; --i)
        if (is_el(e->removed[i - 1])) broken = e->removed[i - 1];
    bool found = broken != NULL || stands(c, e->previous_sibling, parent);
    if (!broken && found) {
        for (broken = e->previous_sibling; broken && !is_el(broken); broken = broken->prev) {}
        // One the batch brought may not have stood there when this change
        // was made: then the left of the pair is past finding.
        if (broken && batch_holds(c->batch_added, c->nbatch_added, broken)) {
            broken = NULL;
            found = false;
        }
    }
    if (broken || !found)
        reached_add(&pair, readers_at(c, GARB_READS_NEIGHBOURS, plus, found ? broken : NULL, true, GARB_ASKED_PAIRS));
    return !(pair.next || pair.on) || mark_run(c, after, pair.on, pair.down);
}

// ── A CHANGE OF AN ATTRIBUTE, by its keys (select.h § A READER's keys) ──
//
// What an attribute's change can turn: its element's own match, which it
// marks alone (and counts as restyled whatever its winners: attr_touched);
// and through the readers that read one of its keys and whose other tests
// pass at the element, their reach. Its KEYS: the attribute by its local
// name; for `class`, the classes the old value and the new one do not
// share; for `id`, the old id and the new. The old values are the ones the
// element was last styled with (Slot.klass, idv), the value before the
// batch however often the batch wrote it. A key read at an ancestor too
// (`lang`, `dir`, `disabled` with :lang(), :dir(), :disabled and their
// kin) marks the element's subtree and its siblings as before, as does a
// change whose old value is past knowing or whose keys pass the room.
static const char *attr_value(const os64_html_node_t *el, const char *name)
{
    const os64_html_attr_t *a = os64_html_attr(el, name);
    return a == NULL ? NULL : a->value != NULL ? a->value : "";
}
static bool space_byte(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\f' || ch == '\r';
}
// Whether the class token `t` (`tl` bytes) is in the class value `v`.
static bool class_holds(const char *v, const char *t, size_t tl)
{
    for (size_t i = 0, n = v ? os64_strlen(v) : 0; i < n;) {
        while (i < n && space_byte(v[i])) i++;
        size_t start = i;
        while (i < n && !space_byte(v[i])) i++;
        if (i - start == tl && os64_memcmp(v + start, t, tl) == 0) return true;
    }
    return false;
}
// The tokens of class value `a` that `b` does not hold, as CLASS keys.
static int32_t class_difference(const char *a, const char *b, garb_key_t *out, int32_t n)
{
    for (size_t i = 0, len = a ? os64_strlen(a) : 0; i < len;) {
        while (i < len && space_byte(a[i])) i++;
        size_t start = i;
        while (i < len && !space_byte(a[i])) i++;
        if (i > start && !class_holds(b, a + start, i - start)) {
            if (n < CHANGE_KEYS) out[n] = (garb_key_t){GARB_KEY_CLASS, false, a + start, i - start};
            n++;
        }
    }
    return n;
}
// The keys change `e` moved at `el` into c->change_keys, or
// c->change_unknown where they cannot be named.
static void attribute_keys(garb_cascade_t *c, const os64_html_change_entry_t *e, const os64_html_node_t *el)
{
    garb_key_t *k = c->change_keys;
    int32_t n = 0;
    c->change_unknown = c->readers_short || e->attr_name == NULL;
    c->change_up = false;
    if (c->change_unknown) { c->nchange_keys = 0; return; }
    const char *name = e->attr_local != NULL ? e->attr_local : e->attr_name;
    size_t len = os64_strlen(name);
    k[n++] = (garb_key_t){GARB_KEY_ATTR, false, name, len};
    const Slot *s = slot_for(c, el, false);
    // A record is matched by its qualified name, whatever its namespace
    // (os64_html_attr): a `class` or `id` written with one is the class or
    // the id all the same.
    if (os64_streq(e->attr_name, "class")) {
        if (s == NULL || !s->placed) c->change_unknown = true;
        else {
            const char *now = attr_value(el, "class");
            n = class_difference(s->klass, now, k, n);
            n = class_difference(now, s->klass, k, n);
        }
    } else if (os64_streq(e->attr_name, "id")) {
        if (s == NULL || !s->placed) c->change_unknown = true;
        else {
            const char *now = attr_value(el, "id");
            if (s->idv != NULL) k[n++] = (garb_key_t){GARB_KEY_ID, false, s->idv, os64_strlen(s->idv)};
            if (now != NULL) k[n++] = (garb_key_t){GARB_KEY_ID, false, now, os64_strlen(now)};
        }
    }
    if (n > CHANGE_KEYS) c->change_unknown = true;
    c->change_up = (c->up_keys & UP_DISABLED && os64_streq(name, "disabled")) ||
                   (c->up_keys & UP_LANG && (os64_streq(name, "lang") || os64_streq(name, "xml:lang"))) ||
                   (c->up_keys & UP_DIR && os64_streq(name, "dir"));
    c->nchange_keys = n > CHANGE_KEYS ? CHANGE_KEYS : n;
}
// The readers of key `k`: the run of the index under it, and every reader
// under GARB_KEYS_ANY.
static void key_run(const garb_cascade_t *c, uint8_t kind, uint32_t hash, const char *name, size_t len,
                    int32_t *from, int32_t *to)
{
    ReaderKey probe = {hash, kind, name, len, 0};
    int32_t lo = 0, hi = c->nreader_keys;
    while (lo < hi) {
        int32_t mid = lo + (hi - lo) / 2;
        if (key_order(&c->reader_keys[mid], &probe) < 0) lo = mid + 1;
        else hi = mid;
    }
    *from = lo;
    while (lo < c->nreader_keys && key_order(&c->reader_keys[lo], &probe) == 0) lo++;
    *to = lo;
}
// The reach of every reader of a key in c->change_keys whose tests other
// than the changed keys' pass at `el` (every one, `all`), gathered.
static Reached keyed_readers(garb_cascade_t *c, const os64_html_node_t *el, bool all, garb_asker_t asker)
{
    garb_reader_work_t *w = &c->reader_work;
    Reached got = {0};
    bool any = false;
    for (int32_t i = 0; i <= c->nchange_keys; i++) {
        int32_t from, to;
        if (i < c->nchange_keys) {
            const garb_key_t *k = &c->change_keys[i];
            key_run(c, k->kind, key_hash(k->name, k->len), k->name, k->len, &from, &to);
        } else {
            key_run(c, GARB_KEYS_ANY, 0, "", 0, &from, &to);
        }
        for (int32_t j = from; j < to; j++) {
            const garb_reader_t *r = &c->readers[c->reader_keys[j].reader];
            w->asked[asker]++;
            if (!all && !garb_reader_filters_pass_except(r, el, c->change_keys, c->nchange_keys)) continue;
            w->passed[asker]++;
            any = true;
            got.self |= r->reach == GARB_REACH_SELF;
            got.next |= r->reach == GARB_REACH_NEXT;
            got.on |= r->reach == GARB_REACH_FOLLOWING || r->reach == GARB_REACH_CHAIN;
            got.down |= r->down;
        }
    }
    if (any) w->reached[asker]++;
    return got;
}
static bool mark_attribute(garb_cascade_t *c, const os64_html_change_entry_t *e, const os64_html_node_t *el,
                           garb_mark_t kind)
{
    if (!connected(c, el)) return true;
    if (c->change_unknown || c->change_up) return mark_changed(c, el, kind);
    c->mark_now = kind;
    if (!mark_element(c, el)) return false;
    Slot *s = slot_for(c, el, false);
    if (s) s->attr_touched = true;
    c->mark_now = GARB_MARK_SIBLINGS;
    if (c->dependencies.filtered && el->parent && !mark_siblings(c, el->parent->first_child)) return false;
    c->mark_now = kind;
    // An element the batch changed more than once: what each change's other
    // tests read as the batch left it may not be what they read before it,
    // so every change there passes every reader of its keys.
    size_t lo = 0, hi = c->nbatch_attrs;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if ((uintptr_t)c->batch_attrs[mid] < (uintptr_t)el) lo = mid + 1;
        else hi = mid;
    }
    bool twice = lo + 1 < c->nbatch_attrs && c->batch_attrs[lo + 1] == el;
    garb_asker_t asker = os64_streq(e->attr_name, "class") ? GARB_ASKED_CLASS
                       : os64_streq(e->attr_name, "id") ? GARB_ASKED_ID
                       : os64_streq(e->attr_name, "style") ? GARB_ASKED_STYLE
                       : GARB_ASKED_ATTRIBUTE;
    Reached got = keyed_readers(c, el, twice, asker);
    return mark_reached(c, el, got);
}

static bool body_hint(const os64_html_change_entry_t *e, const os64_html_document_t *doc)
{
    if (e->kind != OS64_HTML_CHANGE_ATTR || e->target != doc->body || !e->attr_name)
        return false;
    // Match the qualified-name lookup used by the HTML hint consumer.
    const char *names[] = {"link", "vlink", "alink", "text", "bgcolor"};
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i)
        if (os64_streq(e->attr_name, names[i])) return true;
    return false;
}
static bool mark_children(garb_cascade_t *c, const os64_html_change_entry_t *e)
{
    const os64_html_node_t *parent = e->target;
    // A slot leaving a tree moves its host's children, and by the time the
    // log is read the tree it left may be past finding — its parent removed
    // or moved since, in the same batch: a fresh cascade instead.
    if (c->shadowed)
        for (size_t i = 0; i < e->nremoved; ++i)
            if (holds_slot(e->removed[i])) { c->refusal = GARB_UPDATE_SLOT_LEFT; return false; }
    if (!connected(c, parent)) return true;
    // The UA first-summary/first-legend rules depend on child order, so
    // those parents keep the whole mark whatever :empty answers there; the
    // answer is still taken, for the next change to compare with.
    bool turned = c->dependencies.parent_state && is_el(parent) && empty_turned(c, parent);
    if (parent->ns == OS64_HTML_NS_HTML &&
        (parent->tag == OS64_HTML_TAG_DETAILS || parent->tag == OS64_HTML_TAG_FIELDSET)) {
        if (!mark_changed(c, parent, GARB_MARK_PARENT)) return false;
    } else if (turned && !mark_turned(c, parent)) {
        return false;
    }
    c->mark_now = GARB_MARK_ADDED;
    for (size_t i = 0; i < e->nadded; ++i)
        if (!mark_subtree(c, e->added[i])) return false;
    if (c->shadowed) {
        c->mark_now = GARB_MARK_SLOTS;
        // A shadow root attached: libhtml logs it as a change of the host's
        // children with none added or removed, and every child of the host
        // now inherits from a slot, or from nothing.
        if (parent->shadow_root && !e->nadded && !e->nremoved && !mark_subtree(c, parent))
            return false;
        // A slot arriving in a shadow tree moves assignments.
        bool slots = false;
        for (size_t i = 0; i < e->nadded && !slots; ++i) slots = holds_slot(e->added[i]);
        if (slots && !mark_host_children(c, parent)) return false;
    }
    c->mark_now = GARB_MARK_SUFFIX;
    if (c->dependencies.filtered)
        return mark_siblings(c, parent->first_child);
    if (!moves_elements(e)) return true;
    if ((c->dependencies.front_child || c->dependencies.front_type) && !index_turns(c, e, true)) return false;
    if ((c->dependencies.back_child || c->dependencies.back_type) && !index_turns(c, e, false)) return false;
    return position_walks(c, e) && mark_neighbours(c, e);
}
// THE SUBJECT :has() COMPOUNDS A CHANGE MAY TURN (select.h § A SUBJECT
// :has() AND A CHANGE OF CHILDREN): for a change of children, those whose
// arguments the elements it brought or took may match, a taken subtree
// standing in for what it held (stale_removed); for a change of an
// attribute whose keys are named (attribute_keys), those whose arguments
// read one of them; for any other change, all.
// The visits ask with every compound the batch's changes so far may turn
// (`subjects_union`), so that one visit of an element serves every change
// that climbs past it; when that set grows, the visit epoch moves on and
// the climbs that follow visit again what the earlier ones visited.
static bool subjects_live(garb_cascade_t *c, const os64_html_change_entry_t *e)
{
    if (c->subjects == NULL || c->nsubjects == 0) return true;
    uint32_t w = (uint32_t)(c->nsubjects + 63) / 64;
    if (c->subject_width != w) {
        os64_free(c->subjects_union);
        os64_free(c->subjects_now);
        c->subjects_union = os64_calloc(w, sizeof(uint64_t));
        c->subjects_now = os64_calloc(w, sizeof(uint64_t));
        c->subject_width = c->subjects_union && c->subjects_now ? w : 0;
        if (!c->subject_width) { c->refusal = GARB_UPDATE_NO_MEMORY; return false; }
    }
    bool full = true;
    for (int32_t i = 0; i < c->nsubjects && full; i++) full = c->subjects_union[i / 64] >> (i % 64) & 1;
    if (full) return true;
    for (uint32_t k = 0; k < w; k++) c->subjects_now[k] = 0;
    bool keyed = e->kind == OS64_HTML_CHANGE_ATTR && !c->change_unknown;
    for (int32_t i = 0; i < c->nsubjects; i++) {
        const garb_anchor_t *a = &c->subjects[i];
        bool may = e->kind == OS64_HTML_CHANGE_CHILDREN
                       ? garb_has_subject_children_may_turn(a, e->added, e->nadded, NULL, c) ||
                         garb_has_subject_children_may_turn(a, e->removed, e->nremoved, stale_removed, c)
                       : !keyed || garb_has_subject_reads_key(a, c->change_keys, c->nchange_keys);
        if (may) c->subjects_now[i / 64] |= (uint64_t)1 << (i % 64);
    }
    bool grew = false;
    for (uint32_t k = 0; k < w; k++) {
        grew |= (c->subjects_now[k] & ~c->subjects_union[k]) != 0;
        c->subjects_union[k] |= c->subjects_now[k];
    }
    if (grew) c->visit_epoch++;
    return true;
}
// Whether a subject :has() of the rules may have turned at `a`, which a
// change at or below it (or beside it, for `+`/`~`) visits: so `a` is marked
// alone for its own match (select.h § A SUBJECT :has()). Only where some
// subject compound the batch's changes may turn (subjects_live) has filters
// that pass `a`, or none (bare), or the entries could not be kept. A test of
// `a`'s filters as it stands now is enough: they read only `a`'s own name
// and attributes, which could have changed in this batch only by a change
// at `a`, and that change marks `a` itself. `has_subjects` counts what was
// asked, let through (marked alone, or already marked), turned away by the
// compounds' filters, and turned away by the changes.
static bool subject_may_turn(garb_cascade_t *c, const os64_html_node_t *a)
{
    c->has_subjects.visited++;
    bool may = c->subjects == NULL, held = may;
    for (int32_t i = 0; i < c->nsubjects && !may; i++) {
        bool live = c->subjects_union[i / 64] >> (i % 64) & 1;
        if ((live || !held) && garb_anchor_filters_pass(&c->subjects[i], a)) {
            held = true;
            may = live;
        }
    }
    if (may) c->has_subjects.marked++;
    else if (held) c->has_subjects.turned_away_change++;
    else c->has_subjects.turned_away++;
    return may;
}
// One element a change may have turned a :has() on: marked alone where a
// subject :has() of the rules may have turned there (subject_may_turn), and
// each anchor asked again, its subtree or its following siblings' marked
// where the answer turned.
//
// WHAT A BARE ANCHOR COSTS, and why it may cost no more: in an update an
// anchor is asked again here, at the elements mark_has visits for a change
// (where it happened and its ancestors, with `+`/`~` arguments their
// previous siblings too), and at each element a restyle recomputes. A
// filtered anchor's ask is a compare wherever its filters fail; a bare
// one's costs up to its :has() walk (has_matches): the element's subtree
// for a descendant argument; for a `>`, `+` or `~` one that goes down, the
// children or following siblings its first compound matches and what is
// under those; for one that turns sideways (`> a ~ b`), the subtree or
// each following sibling's subtree.
// So an update pays (elements visited, each at most once an update) x
// (bare anchors) such walks, plus
// (restyled elements) x (bare anchors) more, plus the restyle of the reach
// of each anchor that really turned; the cascade keeps tracking. Matching
// a restyled element against a rule whose :has() stands left of a
// combinator walks again at each of its ancestors, whether the anchor is
// bare or not: that cost is the matcher's (DEBTS, Browser layout,
// "Matching asks a `:has()` again"). bare_work counts the asks made here,
// the turns, and the elements their marks added, for the record.
static bool has_visit(garb_cascade_t *c, const os64_html_node_t *a)
{
    if (!connected(c, a)) return true;
    Slot *s = slot_for(c, a, false);
    if (!s) { c->refusal = GARB_UPDATE_NO_MEMORY; return false; }
    // Once a visit epoch (subjects_live): its marking reads the document as
    // the batch left it, so a second visit asking with the same subject
    // compounds would get the same answers, and what they turned is marked
    // already.
    if (s->visited == c->visit_epoch) return true;
    s->visited = c->visit_epoch;
    if (c->dependencies.ancestors && subject_may_turn(c, a) && !mark_element(c, a)) return false;
    if (c->has_width == 0) return true;
    c->bare_work.tested += (uint64_t)c->nbare;
    anchor_bits(c, a, c->has_scratch);
    // The answers are asked into the scratch row; the slot's row takes
    // them, and the scratch row is left holding what turned, for the marks.
    uint64_t *row = has_row(c, s);
    for (uint32_t k = 0; k < c->has_width; k++) {
        uint64_t turned = c->has_scratch[k] ^ row[k];
        row[k] = c->has_scratch[k];
        c->has_scratch[k] = turned;
    }
    for (int32_t i = 0; i < c->nanchors; i++) {
        if (!(c->has_scratch[i / 64] >> (i % 64) & 1)) continue;
        size_t before = c->ndirty;
        if (c->anchors[i].down && !mark_subtree(c, a)) return false;
        if (c->anchors[i].siblings && !mark_siblings(c, a->next)) return false;
        if (c->anchors[i].bare) {
            c->bare_work.turned++;
            c->bare_work.marked += c->ndirty - before;
        }
    }
    return true;
}
// THE ELEMENTS A CHANGE CAN TURN A :has() ON: a :has()'s argument looks
// down from the element it tests, or for `+` and `~` across to its
// following siblings and down from them, so the change at `from` reaches
// `from` and its ancestors in its own tree, and with `+`/`~` the preceding
// siblings of each. `from` is where the change happened: the parent for a
// change of children, which covers an element removed as well as one added
// (the removed one is past finding), and with `+`/`~` every child of it,
// whose order the change moved; the element itself for a change of
// attributes; the text's parent for a change of a text's data, whose
// :empty it can turn. A `from` a later change in the batch took out of the
// document turns nothing in it: nothing there is visited. One visit of
// each (has_visit) serves both kinds of :has().
static bool mark_has(garb_cascade_t *c, const os64_html_node_t *from, bool children)
{
    bool preceding = c->dependencies.ancestors_preceding;
    if (!c->dependencies.ancestors && !preceding && c->nanchors == 0) return true;
    if (children && preceding && connected(c, from))
        for (const os64_html_node_t *k = from->first_child; k; k = k->next)
            if (is_el(k) && !has_visit(c, k)) return false;
    for (const os64_html_node_t *a = from; is_el(a) && connected(c, a); a = a->parent) {
        // An element this visit epoch has visited already has had the rest
        // of this climb visited too (its previous siblings, its ancestors
        // and, with `+`/`~`, theirs), by the walk that visited it or an
        // earlier one: every walk completes its climb before the next
        // begins.
        const Slot *seen = slot_for(c, a, false);
        if (seen && seen->visited == c->visit_epoch) break;
        if (!has_visit(c, a)) return false;
        for (const os64_html_node_t *p = preceding ? a->prev : NULL; p; p = p->prev)
            if (is_el(p) && !has_visit(c, p)) return false;
    }
    return true;
}

uint64_t garb_cascade_generation(const garb_cascade_t *c) { return c ? c->generation : 0; }
uint64_t garb_cascade_updates(const garb_cascade_t *c) { return c ? c->updates : 0; }
uint64_t garb_cascade_version(const garb_cascade_t *c) { return c ? c->version : 0; }
bool garb_style_updated_since(const garb_cascade_t *c, const os64_html_node_t *node, uint64_t update)
{
    const Slot *s = c ? slot_for(c, node, false) : NULL;
    return !s || s->changed > update;
}

// Whether two declarations' values are the same tokens: kind, text, number
// as written, unit, block and arguments.
static bool values_same(const garb_value_t *a, int32_t na, const garb_value_t *b, int32_t nb)
{
    if (na != nb)
        return false;
    for (int32_t i = 0; i < na; i++) {
        const garb_value_t *x = &a[i], *y = &b[i];
        if (x->kind != y->kind || x->len != y->len || x->unit_len != y->unit_len || x->open != y->open ||
            (x->len && os64_memcmp(x->text, y->text, x->len) != 0) ||
            (x->unit_len && os64_memcmp(x->unit, y->unit, x->unit_len) != 0) ||
            !values_same(x->children, x->nchildren, y->children, y->nchildren))
            return false;
    }
    return true;
}
static bool decls_same(const garb_decl_t *a, const garb_decl_t *b)
{
    return a->len == b->len && os64_memcmp(a->name, b->name, a->len) == 0 && a->important == b->important &&
           values_same(a->value, a->nvalue, b->value, b->nvalue);
}
// A declaration's value as one lowercased keyword, when it is one
// identifier short enough for `out`; empty otherwise.
static void inline_keyword(const garb_decl_t *d, char out[16])
{
    out[0] = '\0';
    if (d == NULL || d->nvalue != 1 || d->value[0].kind != GARB_IDENT || d->value[0].len >= 16)
        return;
    for (uint32_t i = 0; i < d->value[0].len; i++) {
        char ch = d->value[0].text[i];
        out[i] = ch >= 'A' && ch <= 'Z' ? (char)(ch + 32) : ch;
    }
    out[d->value[0].len] = '\0';
}
// One declaration's winning turn over the longhands it sets: an
// `!important` one takes a longhand from the rest, a later one from an
// earlier of the same weight.
static void inline_wins(const garb_decl_t **win, garb_prop_t *props, int32_t n, const garb_decl_t *d)
{
    for (int32_t k = 0; k < n; k++)
        if ((uint32_t)props[k] < GARB_NPROPS && (win[props[k]] == NULL || d->important || !win[props[k]]->important))
            win[props[k]] = d;
}
int32_t garb_cascade_inline_changes(const garb_cascade_t *c, const os64_html_node_t *el,
                                    garb_inline_change_t *out, int32_t cap)
{
    const Slot *s = c && el ? slot_for(c, el, false) : NULL;
    if (s == NULL || !s->placed || c->incomplete)
        return -1;
    const garb_decl_t *was[GARB_NPROPS] = {0}, *now[GARB_NPROPS] = {0};
    garb_prop_t props[GARB_SETS_MAX];
    int32_t old_custom = 0;
    for (int32_t i = 0; i < s->ninl; i++) {
        const Decl *d = &s->inl[i];
        if (d->custom) { old_custom++; continue; }
        int32_t n = 0;
        if (d->pending)
            n = prop_longhands(d->decl.name, d->decl.len, c->env.modern_off, props);
        else
            for (; n < d->nsets && n < GARB_SETS_MAX; n++) props[n] = d->sets[n].prop;
        inline_wins(was, props, n, &d->decl);
    }
    const os64_html_attr_t *a = os64_html_attr(el, "style");
    garb_parsed_t p = {0};
    if (a != NULL && a->value != NULL && a->value[0] != '\0' &&
        (garb_parse_block_limited(a->value, os64_strlen(a->value), c->limits.attribute_bytes, &p) != GARB_OK ||
         p.incomplete)) {
        garb_free(&p);
        return -1;
    }
    // The custom properties, in order, as they were: any other answer is
    // one var() could carry anywhere.
    int32_t new_custom = 0, matched_custom = 0;
    bool customs_same = true;
    for (int32_t i = 0; i < p.nitems; i++) {
        if (p.items[i].kind != GARB_ITEM_DECL)
            continue;
        const garb_decl_t *d = &p.items[i].decl;
        if (garb_decl_is_custom(d)) {
            const Decl *o = NULL;
            for (int32_t j = 0, seen = 0; j < s->ninl && o == NULL; j++)
                if (s->inl[j].custom && seen++ == new_custom) o = &s->inl[j];
            customs_same = customs_same && o != NULL && decls_same(&o->decl, d);
            matched_custom += o != NULL;
            new_custom++;
            continue;
        }
        int32_t n = 0;
        if (garb_decl_has_var(d)) {
            n = prop_longhands(d->name, d->len, c->env.modern_off, props);
        } else {
            garb_set_t sets[GARB_SETS_MAX];
            int32_t got = garb_read_declaration(&p, d, reading(c), sets);
            for (; n < got && n < GARB_SETS_MAX; n++) props[n] = sets[n].prop;
        }
        inline_wins(now, props, n, d);
    }
    int32_t listed = 0;
    if (!customs_same || new_custom != old_custom || matched_custom != old_custom)
        listed = -1;
    for (uint32_t prop = 0; listed >= 0 && prop < GARB_NPROPS; prop++) {
        if (was[prop] == NULL && now[prop] == NULL)
            continue;
        if (was[prop] != NULL && now[prop] != NULL && decls_same(was[prop], now[prop]))
            continue;
        if (listed == cap) {
            listed = -1;
            break;
        }
        const garb_decl_t *d = now[prop];
        out[listed] = (garb_inline_change_t){
            .prop = (garb_prop_t)prop,
            .declared = d != NULL,
            .var = d != NULL && garb_decl_has_var(d),
        };
        inline_keyword(d, out[listed].keyword);
        listed++;
    }
    garb_free(&p);
    return listed;
}
bool garb_cascade_style_may_read(const garb_cascade_t *c, const os64_html_node_t *element)
{
    if (c == NULL || c->style_bare)
        return c == NULL || c->dependencies.style_attribute;
    for (int32_t i = 0; i < c->nstyle_readers; i++)
        if (garb_style_reader_may_match(&c->style_readers[i], element))
            return true;
    return false;
}

bool garb_cascade_style_readers_bare(const garb_cascade_t *c)
{
    return c == NULL || (c->dependencies.style_attribute && c->style_bare);
}

bool garb_cascade_reads_style_attribute(const garb_cascade_t *c)
{
    return c == NULL || c->dependencies.style_attribute;
}

bool garb_cascade_tracks_changes(const garb_cascade_t *c)
{
    return c && c->reader && !c->incomplete && !c->update_failed;
}
void garb_cascade_anchors(const garb_cascade_t *c, int32_t *anchors, int32_t *bare)
{
    if (anchors) *anchors = c ? c->nanchors : 0;
    if (bare) *bare = c ? c->nbare : 0;
}
garb_bare_work_t garb_cascade_bare_work(const garb_cascade_t *c)
{
    return c ? c->bare_work : (garb_bare_work_t){0};
}
garb_back_work_t garb_cascade_back_work(const garb_cascade_t *c)
{
    return c ? c->back_work : (garb_back_work_t){0};
}
garb_reader_work_t garb_cascade_reader_work(const garb_cascade_t *c)
{
    return c ? c->reader_work : (garb_reader_work_t){0};
}
const char *garb_asker_name(garb_asker_t asker)
{
    static const char *const kNames[GARB_ASKERS] = {
        [GARB_ASKED_FRONT] = "a front index turned", [GARB_ASKED_BACK] = "a back index turned",
        [GARB_ASKED_FRONT_WALK] = "an unbounded front walk", [GARB_ASKED_BACK_WALK] = "an unbounded back walk",
        [GARB_ASKED_EMPTY] = ":empty turned", [GARB_ASKED_ADDED_REMOVED] = "an element brought or taken",
        [GARB_ASKED_PAIRS] = "a pair broken or made", [GARB_ASKED_ATTRIBUTE] = "an attribute changed",
        [GARB_ASKED_CLASS] = "a class changed", [GARB_ASKED_ID] = "an id changed",
        [GARB_ASKED_STYLE] = "a style changed",
    };
    return asker < GARB_ASKERS ? kNames[asker] : "?";
}
garb_has_subject_work_t garb_cascade_has_subject_work(const garb_cascade_t *c)
{
    garb_has_subject_work_t w = c ? c->has_subjects : (garb_has_subject_work_t){0};
    w.entries = c ? (uint64_t)c->nsubjects : 0;
    w.bare = c ? (uint64_t)c->nbare_subjects : 0;
    return w;
}
garb_update_marks_t garb_cascade_update_marks(const garb_cascade_t *c)
{
    return c ? c->marks : (garb_update_marks_t){0};
}

size_t garb_cascade_recompute_names(const garb_cascade_t *c, char *out, size_t cap)
{
    if (out == NULL || cap == 0) return 0;
    out[0] = '\0';
    if (c == NULL || c->marks.recomputed == 0) return 0;
    size_t n = 0;
    for (int32_t i = 0; i < c->nmoved_names && n + 1 < cap; i++) {
        int w = os64_snprintf(out + n, cap - n, "%s%.*s", i ? " " : "", (int)c->moved_names[i].len,
                              c->moved_names[i].name);
        if (w < 0) break;
        n = n + (size_t)w < cap ? n + (size_t)w : cap - 1;
    }
    if (c->names_all && n + 1 < cap) {
        int w = os64_snprintf(out + n, cap - n, "%s(more)", n ? " " : "");
        if (w > 0) n = n + (size_t)w < cap ? n + (size_t)w : cap - 1;
    }
    return n;
}

garb_build_cost_t garb_cascade_build_cost(const garb_cascade_t *c)
{
    return c ? c->cost : (garb_build_cost_t){0};
}

bool garb_cascade_broad_trigger(const garb_cascade_t *c, garb_broad_trigger_t *out)
{
    if (!c || !c->broad_set) return false;
    *out = c->broad;
    return true;
}

garb_read_refusal_t garb_cascade_read_refusal(const garb_cascade_t *c)
{
    return c ? c->read_refusal : GARB_READ_NOT_OFFERED;
}

const char *garb_read_refusal_name(garb_read_refusal_t reason)
{
    static const char *const kNames[GARB_READ_REASONS] = {
        [GARB_READ_KEPT] = "kept", [GARB_READ_NOT_OFFERED] = "none offered",
        [GARB_READ_NOT_KEEPABLE] = "not keepable", [GARB_READ_DOCUMENT] = "document",
        [GARB_READ_LIMITS] = "limits", [GARB_READ_QUIRKS] = "quirks",
        [GARB_READ_ENVIRONMENT] = "environment", [GARB_READ_SHEETS] = "sheets",
        [GARB_READ_SCOPES] = "scopes",
    };
    return (unsigned)reason < GARB_READ_REASONS ? kNames[reason] : "?";
}

garb_read_t *garb_cascade_read_hold(const garb_cascade_t *c)
{
    if (c == NULL || c->read == NULL)
        return NULL;
    c->read->refs++;
    return c->read;
}

void garb_read_release(garb_read_t *r)
{
    read_release(r);
}

uint64_t garb_cascade_read_content(const garb_cascade_t *c)
{
    return c != NULL && c->read != NULL ? read_content(c->read) : 0;
}

bool garb_cascade_read_written(const garb_cascade_t *c)
{
    return c != NULL && c->read_written;
}

const char *garb_mark_name(garb_mark_t mark)
{
    static const char *const kNames[GARB_MARKS] = {
        [GARB_MARK_CHANGED] = "an attribute or a state", [GARB_MARK_ADDED] = "added subtrees",
        [GARB_MARK_SIBLINGS] = "siblings", [GARB_MARK_HAS] = ":has()",
        [GARB_MARK_PARENT] = "the parent's state", [GARB_MARK_SLOTS] = "slots",
        [GARB_MARK_INHERITED] = "inherited", [GARB_MARK_CLASS] = "a class",
        [GARB_MARK_STYLE] = "a style", [GARB_MARK_BACK] = "a back index's reach",
        [GARB_MARK_SUFFIX] = "the children's siblings", [GARB_MARK_FRONT] = "a front index's reach",
        [GARB_MARK_POSITION] = "an unbounded position's walk", [GARB_MARK_NEIGHBOURS] = "the siblings after a change",
        [GARB_MARK_ADJACENT] = "a pair of neighbours",
    };
    return (unsigned)mark < GARB_MARKS ? kNames[mark] : "?";
}

// Whether two values say the same, by content: a winner read through
// var(), or from a style attribute parsed again, is a new copy each time.
static bool val_same(const garb_val_t *a, const garb_val_t *b);
static bool calc_same(const garb_calc_t *a, const garb_calc_t *b)
{
    if (a == b) return true;
    if (a == NULL || b == NULL || a->op != b->op || a->nargs != b->nargs || !val_same(&a->leaf, &b->leaf))
        return false;
    for (int32_t i = 0; i < a->nargs; i++)
        if (!calc_same(a->args[i], b->args[i]) || (a->invert != NULL) != (b->invert != NULL) ||
            (a->invert != NULL && a->invert[i] != b->invert[i]))
            return false;
    return true;
}
static bool val_same(const garb_val_t *a, const garb_val_t *b)
{
    if (a->kind != b->kind || a->number != b->number || a->unit != b->unit || a->color.r != b->color.r ||
        a->color.g != b->color.g || a->color.b != b->color.b || a->color.a != b->color.a ||
        a->color.current != b->color.current || a->len != b->len || a->nitems != b->nitems || a->comma != b->comma)
        return false;
    if ((a->keyword == NULL) != (b->keyword == NULL) || (a->keyword && !os64_streq(a->keyword, b->keyword)))
        return false;
    if ((a->text == NULL) != (b->text == NULL) || (a->text && os64_memcmp(a->text, b->text, a->len) != 0))
        return false;
    if (!calc_same(a->calc, b->calc))
        return false;
    for (int32_t i = 0; a->items != b->items && i < a->nitems; i++)
        if (!val_same(&a->items[i], &b->items[i])) return false;
    return true;
}
static bool styles_same(const garb_style_t *a, const garb_style_t *b)
{
    if (a->n != b->n) return false;
    for (int32_t i = 0; i < a->n; i++)
        if (a->sets[i].prop != b->sets[i].prop || !val_same(&a->sets[i].value, &b->sets[i].value) ||
            a->sheet[i] != b->sheet[i] || a->important[i] != b->important[i])
            return false;
    return true;
}
// Whether two custom property maps an element was styled with hold the
// same: one map, or its own declarations the same by value over the same
// inherited map.
static bool customs_same(const CustomMap *a, const CustomMap *b)
{
    if (a == b) return true;
    if (a == NULL || b == NULL || a->parent != b->parent || a->n != b->n) return false;
    for (int32_t i = 0; i < a->n; i++) {
        const Custom *x = &a->own[i], *y = &b->own[i];
        if (x->len != y->len || os64_memcmp(x->name, y->name, x->len) != 0 || x->state != y->state ||
            (x->value == NULL) != (y->value == NULL) ||
            (x->value != NULL && !values_same(x->value, x->nvalue, y->value, y->nvalue)))
            return false;
    }
    return true;
}
// THE RECOMPUTE (L4 part 2; RELAYOUT.md § What restyle marks below). A
// descendant's selector answers do not read an ancestor's winners, so what
// an element restyled alone owes what is under it is decided by its custom
// property map, by value: the same, nothing (libflow carries ordinary
// inheritance); different, every element that inherits from it is
// recomputed in the update's walk, as it reaches it: its map made again
// over the one it inherits, substitution redone where var() made a winner,
// winners compared, `changed` only where they differ. No matching, no
// anchor asked, nothing counted toward the half.
//
// The names whose values may have moved are kept for the update
// (moved_name): the ones an origin's own map changed, and the own entries a
// recompute made again from the map it inherits. An element whose own map
// declares every one of them, none from the map above, has the map it had
// (the PRUNE): what is under it inherits nothing new, so it stops there,
// keeping its record, whose map may name an older one above with the same
// values for every name it does not declare.
static void moved_name(garb_cascade_t *c, const char *name, size_t len)
{
    for (int32_t i = 0; i < c->nmoved_names; i++)
        if (c->moved_names[i].len == len && os64_memcmp(c->moved_names[i].name, name, len) == 0) return;
    if (c->nmoved_names == RECOMPUTE_NAMES) { c->names_all = true; return; }
    c->moved_names[c->nmoved_names].name = name;
    c->moved_names[c->nmoved_names].len = len;
    c->nmoved_names++;
}
static const Custom *own_entry(const CustomMap *m, const char *name, size_t len)
{
    for (int32_t k = m ? m->n - 1 : -1; k >= 0; k--)
        if (m->own[k].len == len && os64_memcmp(m->own[k].name, name, len) == 0) return &m->own[k];
    return NULL;
}
static bool entry_same(const Custom *a, const Custom *b)
{
    const garb_decl_t *x = &a->decl->decl, *y = &b->decl->decl;
    return values_same(x->value, x->nvalue, y->value, y->nvalue);
}
// Whether an element restyled alone changed its custom property map by
// value, and the names it moved (moved_name). The map it inherits holds the
// values it held, the update's walk having made them current, while it
// inherits from the same element; a change of its `slot` can move that
// element, and then no name is known.
static bool customs_moved(garb_cascade_t *c, const Slot *was, const Slot *now)
{
    const CustomMap *o = was->own_customs ? was->customs : NULL;
    const CustomMap *n = now->own_customs ? now->customs : NULL;
    if (was->from != now->from) {
        c->names_all = true;
        return true;
    }
    bool moved = false;
    for (int32_t k = 0; n && k < n->n; k++) {
        const Custom *before = own_entry(o, n->own[k].name, n->own[k].len);
        if (before == NULL || !entry_same(before, &n->own[k])) {
            moved_name(c, n->own[k].name, n->own[k].len);
            moved = true;
        }
    }
    for (int32_t k = 0; o && k < o->n; k++)
        if (own_entry(n, o->own[k].name, o->own[k].len) == NULL) {
            moved_name(c, o->own[k].name, o->own[k].len);
            moved = true;
        }
    return moved;
}
// Whether the element `x` inherits from moved its map in this update.
static bool recompute_owed(const garb_cascade_t *c, const os64_html_node_t *x)
{
    const os64_html_node_t *from = inherit_from(c, x);
    const Slot *up = from ? slot_for((garb_cascade_t *)c, from, false) : NULL;
    return up && up->moved == c->updates + 1;
}
// Its animation-name winner, or NULL: what its keyframes (own_frames) and,
// through `inherit`, those of what inherits from it are read for.
static const garb_val_t *anim_name(const Slot *s)
{
    for (int32_t i = 0; i < s->style.n; i++)
        if (s->style.sets[i].prop == GARB_ANIMATION_NAME) return &s->style.sets[i].value;
    return NULL;
}
static bool anim_same(const Slot *a, const Slot *b)
{
    const garb_val_t *x = anim_name(a), *y = anim_name(b);
    return x == y || (x != NULL && y != NULL && val_same(x, y));
}
static bool anim_inherits(const Slot *s)
{
    const garb_val_t *v = anim_name(s);
    return v != NULL && v->kind == GARB_V_WIDE && os64_streq(v->keyword, "inherit");
}
// Whether `s`'s own map declares every moved name, none from the map above
// (the PRUNE's question): its map then holds the values it held.
static bool holds_moved(const garb_cascade_t *c, const Slot *s)
{
    if (!s->own_customs || c->names_all) return false;
    for (int32_t i = 0; i < c->nmoved_names; i++) {
        const Custom *own = own_entry(s->customs, c->moved_names[i].name, c->moved_names[i].len);
        if (own == NULL || own->reads_up) return false;
    }
    return true;
}
// `s`'s map moved: what inherits from it is owed a recompute, and its own
// entries made from the map above may have moved with it.
static void moved_slot(garb_cascade_t *c, Slot *s)
{
    s->moved = c->updates + 1;
    c->any_moved = true;
    for (int32_t k = 0; s->own_customs && k < s->customs->n; k++)
        if (s->customs->own[k].reads_up) moved_name(c, s->customs->own[k].name, s->customs->own[k].len);
}
// Recomputes dirty `x`'s winners into a fresh record in its slot `s`. One
// marked alone (mark_element) keeps its old record when the same rules
// matched it as before and its winners and its custom properties compare
// the same by value: its own declarations, and the element it inherits
// from (customs_same compares that too, which a change of its `slot` can
// move), so nothing under it reads anything new from it, and
// garb_style_updated_since says it did not change, unless its attributes
// changed (attr_touched: what it lays out may read them); what its winners
// were derived from is the fresh record's all the same. Nothing under it
// is marked. Where its custom property map moved (customs_moved; or what it
// inherits moved and its own map does not declare every moved name; or its
// animation-name changed, which `inherit` reaches), what inherits from it
// is owed a recompute (THE RECOMPUTE, above), which the update's walk
// makes as it reaches each after it. A pseudo-element's winners reach no
// descendant: when they alone differ the fresh record is all they need.
// False when the recompute failed.
static bool restyle(garb_cascade_t *c, const os64_html_node_t *x, Slot *s, Match **m, int32_t *mcap,
                    AppliedScratch *scratch)
{
    Slot old = *s;
    // The position indexes are its siblings' count, which styling it does
    // not take again: they are kept.
    *s = (Slot){.node = x, .changed = c->updates + 1, .back = old.back, .back_type = old.back_type,
                .front = old.front, .front_type = old.front_type};
    cascade_whole(c, x, m, mcap, scratch);
    if (c->incomplete || c->self.incomplete) { c->refusal = GARB_UPDATE_SHORT; return false; }
    s = slot_for(c, x, false);
    // Its anchor bits are asked again and stored, never compared. The walk
    // is sound because whatever turned a bit here was a logged change that
    // already marked all the bit's reach: a change at the element itself or
    // under it, or a sibling's for a `+`/`~` argument, visits it (mark_has)
    // and compares there; anything else its compound reads of its
    // ancestors (a fieldset's `disabled`, `lang` or `dir`) changed at an
    // ancestor whose mark covers its subtree, which holds any anchor's
    // reach from the element; its place among its siblings is read by a
    // reader whose filters and reach are the anchor's own (select.h § A
    // READER), so where it turned and the filters pass the anchor's reach is
    // marked, and where they fail the anchor cannot match there, before or
    // after (an element whose attributes the batch changed passes them:
    // stale); and where its :empty answer turned, the readers' sibling reach
    // is marked and nothing under it needs marking (mark_turned: all that is
    // under it came with the batch, or nothing is). A new kind of mark that
    // restyles an element alone must keep that true, or compare here.
    anchor_bits(c, x, has_row(c, s));
    if (!old.element_only) return true;
    // What it inherits moved in this update (recompute_owed): its map, made
    // over the new one, moved too unless it declares every moved name itself.
    bool owed = c->any_moved && recompute_owed(c, x);
    bool moved = customs_moved(c, &old, s) || (owed && !holds_moved(c, s));
    // A changed animation-name reaches what takes it by `inherit`, whatever
    // the map: every element below is recomputed, and no name prunes.
    if (!anim_same(&old, s)) {
        c->names_all = true;
        moved = true;
    }
    bool same = old.matched == s->matched && styles_same(&old.style, &s->style) &&
                customs_same(old.customs, s->customs);
    bool pseudo_same = true;
    for (int32_t t = 0; t < TARGET_AFTER; t++) pseudo_same &= styles_same(&old.pseudo[t], &s->pseudo[t]);
    if (same && pseudo_same) {
        // The old record stands for its winners; what they were derived from
        // is the fresh one's (its map, its var() winners' declarations, its
        // keyframes), since the same values can come of other declarations,
        // and a later recompute derives again from these.
        Slot fresh = *s;
        *s = old;
        s->dirty = s->dirty_suffix = s->alone_suffix = s->element_only = s->attr_touched = false;
        s->customs = fresh.customs;
        s->own_customs = fresh.own_customs;
        s->needs_substitution = fresh.needs_substitution;
        s->pseudo_vars = fresh.pseudo_vars;
        s->pend = fresh.pend;
        s->npend = fresh.npend;
        s->frames = fresh.frames;
        s->nframes = fresh.nframes;
        s->from = fresh.from;
        // The values it was styled with now; and, where its attributes
        // changed, a restyle all the same: what it lays out may read them.
        s->klass = attr_value(x, "class");
        s->idv = attr_value(x, "id");
        if (old.attr_touched) s->changed = c->updates + 1;
    }
    if (moved) moved_slot(c, s);
    return true;
}

// Substitution redone against `map` for the winners var() made (Slot.pend):
// each declaration's longhands it won take its new values, the rest stand.
// False when a declaration now sets other longhands than it did, which only
// its whole cascade can place (`*whole`), or when there was no memory.
static bool recompute_winners(garb_cascade_t *c, Slot *s, const CustomMap *map, bool *whole)
{
    garb_style_t *st = &s->style;
    garb_set_t *sets = NULL;
    for (int32_t i = 0; i < s->npend; i++) {
        const PendDecl *pd = &s->pend[i];
        garb_set_t got[GARB_SETS_MAX];
        int32_t n = pending_sets(c, pd->d, map, got);
        bool same_props = n == pd->nproduced;
        for (int32_t k = 0; same_props && k < n; k++) same_props = got[k].prop == pd->produced[k];
        if (!same_props) { *whole = true; return false; }
        for (int32_t k = 0; k < pd->nprops; k++) {
            garb_prop_t p = pd->props[k];
            int32_t g = 0, lo = 0, hi = st->n;
            while (g < n && got[g].prop != p) g++;
            while (lo < hi) {
                int32_t mid = lo + (hi - lo) / 2;
                if (st->sets[mid].prop < p) lo = mid + 1;
                else hi = mid;
            }
            if (g == n || lo == st->n || st->sets[lo].prop != p) { *whole = true; return false; }
            if (val_same(&st->sets[lo].value, &got[g].value)) continue;
            // Its keyframes are read for its animation-name: a new one is its
            // whole cascade's to build (own_frames).
            if (p == GARB_ANIMATION_NAME) { *whole = true; return false; }
            if (sets == NULL) {
                sets = alloc(c, (size_t)st->n * sizeof(*sets));
                if (sets == NULL) { c->refusal = GARB_UPDATE_NO_MEMORY; return false; }
                os64_memcpy(sets, st->sets, (size_t)st->n * sizeof(*sets));
            }
            sets[lo] = got[g];
        }
    }
    if (sets != NULL) {
        st->sets = sets;
        s->changed = c->updates + 1;
        c->marks.recompute_changed++;
    }
    return true;
}
// Recomputes `x`, under an element whose map moved, in place (THE
// RECOMPUTE): its own map made again over the one it inherits, or the
// PRUNE; its winners var() made redone. One whose pseudo-elements' winners
// or keyframes read the map takes its whole cascade instead (restyle, as
// an element marked alone), outside the half all the same.
static bool recompute(garb_cascade_t *c, const os64_html_node_t *x, Slot *s, Match **m, int32_t *mcap,
                      AppliedScratch *scratch)
{
    c->marks.recomputed++;
    if (s->pseudo_vars || s->nframes > 0 || anim_inherits(s)) {
        c->marks.recompute_whole++;
        s->element_only = true;
        return restyle(c, x, s, m, mcap, scratch);
    }
    const os64_html_node_t *from = inherit_from(c, x);
    const Slot *up = from ? slot_for(c, from, false) : NULL;
    const CustomMap *map = up ? up->customs : NULL;
    if (holds_moved(c, s)) {
        c->marks.recompute_pruned++;
        return true;
    }
    if (s->own_customs) {
        const CustomMap *o = s->customs;
        CustomMap *mine = alloc(c, sizeof(*mine));
        Custom *list = alloc(c, (size_t)o->n * sizeof(*list));
        if (mine == NULL || list == NULL) { c->refusal = GARB_UPDATE_NO_MEMORY; return false; }
        *mine = (CustomMap){map, list, o->n};
        for (int32_t k = 0; k < o->n; k++) custom_entry(c, &list[k], o->own[k].decl, map);
        map = mine;
    }
    s->customs = map;
    moved_slot(c, s);
    bool whole = false;
    if (s->npend > 0 && !recompute_winners(c, s, map, &whole)) {
        if (!whole) return false;
        c->marks.recompute_whole++;
        s->element_only = true;
        return restyle(c, x, s, m, mcap, scratch);
    }
    return true;
}

garb_update_refusal_t garb_cascade_update_refusal(const garb_cascade_t *c)
{
    return c ? c->refusal : GARB_UPDATE_DONE;
}
const char *garb_update_refusal_name(garb_update_refusal_t reason)
{
    static const char *const kNames[GARB_UPDATE_REASONS] = {
        "done", "incomplete", "failed before", "no reader", "counted has", "shadow root",
        "cap", "retained", "attributes", "history", "body hint", "slot left", "broad", "short",
        "no memory", "advance"};
    return reason >= 0 && reason < GARB_UPDATE_REASONS ? kNames[reason] : "?";
}

// THE CALLER'S MEASURE of a broad refusal (garb_cascade_broad_trigger).
// An element as a record names it: its name, its #id and its first
// .class, cut to `cap`.
static void describe(const os64_html_node_t *x, char *out, size_t cap)
{
    const char *id = "", *cls = "";
    for (const os64_html_attr_t *a = x != NULL ? x->attrs : NULL; a; a = a->next) {
        if (a->ns || !a->name || !a->value) continue;
        if (os64_streq(a->name, "id")) id = a->value;
        else if (os64_streq(a->name, "class")) cls = a->value;
    }
    size_t first = 0;
    while (cls[first] && cls[first] != ' ' && cls[first] != '\t' && cls[first] != '\n') first++;
    os64_snprintf(out, cap, "%s%s%s%s%.*s", x != NULL && x->name ? x->name : "?", *id ? "#" : "", id,
                  first ? "." : "", (int)first, cls);
}
// A change of the batch has made its marks (c->mark_by): the counts of the
// most marked so far are kept, and the batch's counts always. Only counts:
// the change is named once, at a refusal (broad_name), so an update that
// goes ahead pays for no names.
static void broad_note(garb_cascade_t *c, const os64_html_change_entry_t *e, uint32_t marked_changes,
                       uint32_t changes)
{
    garb_broad_trigger_t *b = &c->broad;
    b->changes = changes;
    b->marked_changes = marked_changes;
    b->dirty = (uint32_t)c->ndirty;
    b->slots = (uint32_t)c->slotcount;
    uint32_t total = 0;
    for (int k = 0; k < GARB_MARKS; k++) total += c->mark_by[k];
    if (total <= b->marked && b->marked_changes > 1) return;
    b->marked = total;
    os64_memcpy(b->by, c->mark_by, sizeof(b->by));
    c->broad_best = e;
}
// The kept change named, at a broad refusal: its kind, its target, the
// target's depth and the attribute. The change log's entries live until
// the reader is read again, which is after this update.
static void broad_name(garb_cascade_t *c)
{
    garb_broad_trigger_t *b = &c->broad;
    const os64_html_change_entry_t *e = c->broad_best;
    if (e == NULL) return;
    b->kind = (int32_t)e->kind;
    const os64_html_node_t *t = e->target;
    if (t && !is_el(t)) t = t->parent;
    describe(t, b->target, sizeof(b->target));
    uint32_t depth = 0;
    for (const os64_html_node_t *x = t; x && x->parent; x = x->parent) depth++;
    b->depth = depth;
    os64_snprintf(b->attr, sizeof(b->attr), "%s",
                  e->kind == OS64_HTML_CHANGE_ATTR && e->attr_name ? e->attr_name : "");
}

static bool update(garb_cascade_t *c)
{
    c->back_work = (garb_back_work_t){0};
    c->reader_work = (garb_reader_work_t){.readers = c->nreaders, .bare = c->nbare_readers};
    c->has_subjects = (garb_has_subject_work_t){0};
    c->visit_epoch++;
    for (uint32_t k = 0; k < c->subject_width; k++) c->subjects_union[k] = 0;
    c->marks = (garb_update_marks_t){0};
    c->any_moved = c->names_all = false;
    c->nmoved_names = 0;
    if (c->incomplete) { c->refusal = GARB_UPDATE_INCOMPLETE; return false; }
    if (c->update_failed) { c->refusal = GARB_UPDATE_FAILED_BEFORE; return false; }
    uint64_t version = os64_html_version(c->doc);
    if (version == c->version) return true;
    // A document's first shadow root changes how every walk goes (shadowed).
    if (!c->shadowed && os64_html_has_shadow_roots(c->doc)) { c->refusal = GARB_UPDATE_SHADOW_ROOT; return false; }
    if (!c->reader) {
        c->refusal = c->untrackable != GARB_UPDATE_DONE ? c->untrackable : GARB_UPDATE_NO_READER;
        return false;
    }
    // Old derived values stay alive for displayed trees. Renew once added
    // storage reaches one initial state's worth (at least 4 KiB). Keeping
    // many whole restyles bloats the allocator's region ledger and slows
    // even unrelated layout allocations. An update may cross this soft
    // threshold; the arena and attribute caps remain the hard bounds.
    size_t added = os64_arena_stats(c->self.arena).used_bytes - c->initial_state_bytes;
    if (c->updates >= 64) { c->refusal = GARB_UPDATE_CAP; return false; }
    if (added >= c->retained_budget) { c->refusal = GARB_UPDATE_RETAINED; return false; }
    if (c->update_attribute_bytes >= c->retained_budget - added) { c->refusal = GARB_UPDATE_ATTRIBUTES; return false; }
    const os64_html_change_entry_t *changes = NULL;
    int64_t count = os64_html_changes(c->reader, &changes);
    c->update_failed = true;
    c->bare_work = (garb_bare_work_t){0};
    if (count < 0) { c->refusal = GARB_UPDATE_HISTORY; return false; }
    // Process removals before additions: a node may have moved several
    // times since the last read, and entry arrays preserve its identity.
    for (const os64_html_change_entry_t *e = changes; e; e = e->next)
        for (size_t i = 0; i < e->nremoved; ++i)
            for (const os64_html_node_t *x = e->removed[i]; x;
                 x = si_next(x, e->removed[i], true))
                if (is_el(x)) slot_remove(c, x);
    for (const os64_html_change_entry_t *e = changes; e; e = e->next)
        for (size_t i = 0; i < e->nadded; ++i) {
            const os64_html_node_t *root = e->added[i];
            if (!connected(c, root)) continue;
            for (const os64_html_node_t *x = root; x; x = si_next(x, root, true)) {
                if (!is_el(x)) continue;
                if (!slot_for(c, x, true)) { c->refusal = GARB_UPDATE_NO_MEMORY; return false; }
                // The position indexes of what came in with the root, whose
                // parents no change names; the root's own are its parent's
                // change's to count (index_turns).
                if (x != root && next_element(x) == NULL) {
                    if (c->dependencies.back_child || c->dependencies.back_type) index_walk(c, x, false, false);
                    if (c->dependencies.front_child || c->dependencies.front_type)
                        index_walk(c, first_element(x), false, true);
                }
            }
        }
    if (!batch_take(c, changes)) { c->refusal = GARB_UPDATE_NO_MEMORY; return false; }
    walks_plan(c, changes);
    c->ndirty = 0;
    // The caller's measure: the batch's size, then each change's marks.
    uint32_t nchanges = 0, nmarked = 0;
    if (c->limits.clock) {
        for (const os64_html_change_entry_t *e = changes; e; e = e->next) nchanges++;
        c->broad = (garb_broad_trigger_t){0};
        c->broad_best = NULL;
    }
    for (const os64_html_change_entry_t *e = changes; e; e = e->next) {
        if (body_hint(e, c->doc)) { c->refusal = GARB_UPDATE_BODY_HINT; return false; }
        if (c->limits.clock) os64_memset(c->mark_by, 0, sizeof(c->mark_by));
        if (e->kind == OS64_HTML_CHANGE_ATTR) {
            const os64_html_node_t *el = e->target;
            if (el && !is_el(el)) el = el->parent;
            attribute_keys(c, e, el);
        }
        if (!subjects_live(c, e)) return false;
        bool marked;
        if (e->kind == OS64_HTML_CHANGE_CHILDREN) {
            marked = mark_children(c, e);
            c->mark_now = GARB_MARK_HAS;
            marked = marked && mark_has(c, e->target, true);
        } else if (e->kind == OS64_HTML_CHANGE_TEXT) {
            // A text's data is read by no selector but :empty (at its
            // parent) and a :has() that :empty stands in, so with :empty in
            // the rules the parent's answer is asked again and the climb
            // mark_has makes from it is taken, and without it nothing is
            // marked (`parent_state` counts an :empty inside a :has() too).
            const os64_html_node_t *parent = e->target ? e->target->parent : NULL;
            bool asked = c->dependencies.parent_state && is_el(parent) && connected(c, parent);
            marked = !asked || !empty_turned(c, parent) || mark_turned(c, parent);
            c->mark_now = GARB_MARK_HAS;
            marked = marked && (!asked || mark_has(c, parent, false));
        } else {
            const os64_html_node_t *root = e->target;
            if (root && !is_el(root)) root = root->parent;
            // By what changed, for the caller's measure (garb_mark_t).
            garb_mark_t kind = GARB_MARK_CHANGED;
            if (e->kind == OS64_HTML_CHANGE_ATTR && e->attr_name && !e->attr_ns)
                kind = os64_streq(e->attr_name, "class") ? GARB_MARK_CLASS
                     : os64_streq(e->attr_name, "style") ? GARB_MARK_STYLE : GARB_MARK_CHANGED;
            // An element the batch brought (an element it put in, or one
            // under it) is marked whole as added, and what its attributes
            // move beside it and above it is asked of it as the batch left
            // it by the change that brought it (mark_neighbours,
            // subjects_live, and that change's climb): nothing to add here.
            bool brought = false;
            for (const os64_html_node_t *x = root; x && !brought; x = x->parent)
                brought = batch_holds(c->batch_added, c->nbatch_added, x);
            marked = brought || (e->kind == OS64_HTML_CHANGE_ATTR && root ? mark_attribute(c, e, root, kind)
                                                                           : mark_changed(c, root, kind));
            c->mark_now = GARB_MARK_HAS;
            marked = marked && (brought || !root || mark_has(c, root, false));
            // A slot renamed moves which of its host's children it takes.
            c->mark_now = GARB_MARK_SLOTS;
            if (marked && c->shadowed && e->kind == OS64_HTML_CHANGE_ATTR && e->attr_name &&
                os64_streq(e->attr_name, "name") && is_slot(e->target))
                marked = mark_host_children(c, e->target);
        }
        if (c->limits.clock) broad_note(c, e, ++nmarked, nchanges);
        if (!marked) {
            if (c->limits.clock && c->refusal == GARB_UPDATE_BROAD) {
                broad_name(c);
                c->broad_set = true;
            }
            return false;
        }
    }
    c->updating = true;
    c->marks.marked = c->ndirty;
    // Nothing marks while the walk restyles (what is under a changed element
    // is recomputed, THE RECOMPUTE); a mark made then is counted inherited.
    c->mark_now = GARB_MARK_INHERITED;
    if (c->limits.clock) os64_memset(c->mark_by, 0, sizeof(c->mark_by));
    int32_t old_nattrs = c->nattrs;
    Match *m = NULL; int32_t mcap = 0;
    AppliedScratch scratch = {0};
    bool failed = false;
    if (c->shadowed) {
        // A dirty slotted element inherits from its slot, which may lie
        // outside its own dirty root, so the dirty elements are recomputed
        // in one walk in si_next's order, which puts each after the element
        // it inherits from.
        for (const os64_html_node_t *x = c->doc->document; x && !failed;
             x = si_next(x, c->doc->document, true)) {
            Slot *s = is_el(x) ? slot_for(c, x, false) : NULL;
            if (s && s->dirty) failed = !restyle(c, x, s, &m, &mcap, &scratch);
            else if (s && c->any_moved && recompute_owed(c, x)) failed = !recompute(c, x, s, &m, &mcap, &scratch);
        }
    }
    for (size_t i = 0; i < c->ndirty && !c->shadowed && !failed; ++i) {
        const os64_html_node_t *root = c->dirty_nodes[i];
        Slot *s = slot_for(c, root, false);
        const Slot *up = is_el(root->parent) ? slot_for(c, root->parent, false) : NULL;
        if (!s || !s->dirty || (up && up->dirty)) continue;
        // Minimal dirty roots traverse their dirty descendants in tree order,
        // so inherited custom properties have their new parent values. A
        // root dirty alone, an ancestor a :has() may have turned on, makes
        // the walk visit its whole subtree to find what else is dirty: a
        // pointer step and a slot lookup a node, weighed against the fresh
        // cascade it replaces, which matches every rule to every element.
        for (const os64_html_node_t *x = root; x && !failed; x = subtree_next(x, root, true)) {
            if (!is_el(x)) continue;
            s = slot_for(c, x, false);
            if (s && s->dirty) failed = !restyle(c, x, s, &m, &mcap, &scratch);
            else if (s && c->any_moved && recompute_owed(c, x)) failed = !recompute(c, x, s, &m, &mcap, &scratch);
        }
    }
    os64_free(scratch.items);
    os64_free(scratch.produced);
    os64_free(scratch.nproduced);
    c->updating = false;
    c->marks.inherited = c->ndirty - c->marks.marked;
    if (failed) return false;
    for (int32_t i = old_nattrs; i < c->nattrs; ++i) c->incomplete |= c->attrs[i].incomplete;
    if (c->incomplete || c->self.incomplete) { c->refusal = GARB_UPDATE_SHORT; return false; }
    os64_html_pin_t pin = os64_html_pin(c->doc);
    if (!pin) { c->refusal = GARB_UPDATE_NO_MEMORY; return false; }
    os64_html_unpin(c->doc, c->pin); c->pin = pin;
    c->version = version; ++c->updates; c->update_failed = false; c->ndirty = 0;
    return true;
}

bool garb_cascade_update(garb_cascade_t *c)
{
    if (!c) return false;
    c->refusal = GARB_UPDATE_DONE;
    // A trigger is the latest update's alone: one refused before it (or
    // not run at all) names none.
    c->broad_set = false;
    return update(c);
}

bool garb_cascade_advance(garb_cascade_t *c)
{
    if (!c) return false;
    c->refusal = GARB_UPDATE_DONE;
    c->broad_set = false;
    if (c->incomplete) { c->refusal = GARB_UPDATE_INCOMPLETE; return false; }
    if (c->update_failed) { c->refusal = GARB_UPDATE_FAILED_BEFORE; return false; }
    uint64_t version = os64_html_version(c->doc);
    if (version == c->version) return true;
    if (!c->reader) { c->refusal = GARB_UPDATE_NO_READER; return false; }
    const os64_html_change_entry_t *changes = NULL;
    int64_t count = os64_html_changes(c->reader, &changes);
    // The log is read whatever comes of it, so a refusal leaves the cascade
    // owing a fresh build, as a failed update does.
    c->update_failed = true;
    if (count < 0) { c->refusal = GARB_UPDATE_HISTORY; return false; }
    for (const os64_html_change_entry_t *e = changes; e; e = e->next) {
        const Slot *s = e->kind == OS64_HTML_CHANGE_ATTR && e->attr_name && !e->attr_ns &&
                        os64_streq(e->attr_name, "style") && is_el(e->target)
                        ? slot_for(c, e->target, false) : NULL;
        if (!s || !s->placed) { c->refusal = GARB_UPDATE_ADVANCE; return false; }
    }
    os64_html_pin_t pin = os64_html_pin(c->doc);
    if (!pin) { c->refusal = GARB_UPDATE_NO_MEMORY; return false; }
    os64_html_unpin(c->doc, c->pin); c->pin = pin;
    c->version = version; c->update_failed = false;
    return true;
}

bool garb_cascade_incomplete(const garb_cascade_t *c)
{
    return c == NULL || c->incomplete;
}

garb_status_t garb_parse_style_element(const os64_html_node_t *style, garb_parsed_t *out)
{
    size_t len = 0;
    const os64_html_node_t *only = NULL;
    int32_t texts = 0;
    for (const os64_html_node_t *k = style != NULL ? style->first_child : NULL; k != NULL;
         k = k->next)
        if (k->kind == OS64_HTML_TEXT) {
            len += k->text_len;
            only = k;
            texts++;
        }
    if (texts <= 1)
        return garb_parse_sheet_text(only != NULL ? only->text : "", only != NULL ? len : 0, out);
    char *joined = os64_malloc(len + 1);
    if (joined == NULL) {
        os64_memset(out, 0, sizeof(*out));
        return GARB_NO_MEMORY;
    }
    size_t at = 0;
    for (const os64_html_node_t *k = style->first_child; k != NULL; k = k->next)
        if (k->kind == OS64_HTML_TEXT) {
            os64_memcpy(joined + at, k->text, k->text_len);
            at += k->text_len;
        }
    garb_status_t st = garb_parse_sheet_text(joined, len, out);
    os64_free(joined);
    return st;
}

garb_env_t garb_cascade_env(const garb_cascade_t *c)
{
    return c != NULL ? c->env : (garb_env_t){.width = 0, .height = 0};
}

garb_style_t garb_style_for(const garb_cascade_t *c, const os64_html_node_t *element)
{
    garb_style_t none = {NULL, NULL, NULL, 0};
    if (c == NULL)
        return none;
    const Slot *s = slot_for(c, element, false);
    return s != NULL ? s->style : none;
}

garb_style_t garb_style_for_pseudo(const garb_cascade_t *c, const os64_html_node_t *element, garb_pseudo_t pseudo)
{
    garb_style_t none = {NULL, NULL, NULL, 0};
    int32_t target = pseudo == GARB_PSEUDO_BEFORE ? TARGET_BEFORE : pseudo == GARB_PSEUDO_AFTER ? TARGET_AFTER : -1;
    if (c == NULL || target < 0)
        return none;
    const Slot *s = slot_for(c, element, false);
    return s != NULL ? s->pseudo[target - 1] : none;
}

bool garb_style_needs_substitution(const garb_cascade_t *c, const os64_html_node_t *element)
{
    const Slot *s = c ? slot_for(c, element, false) : NULL;
    return s && s->needs_substitution;
}

int32_t garb_keyframes(const garb_cascade_t *c, const os64_html_node_t *element,
                       const char *name, size_t len, const garb_keyframe_t **out)
{
    const Keyframes *k = c != NULL ? keyframes_named(c, name, len) : NULL;
    if (k == NULL)
        return -1;
    const Slot *s = k->pending ? slot_for(c, element, false) : NULL;
    int32_t lo = 0, hi = s != NULL ? s->nframes : 0;
    while (lo < hi) {
        int32_t mid = lo + (hi - lo) / 2;
        if ((uintptr_t)s->frames[mid].rule < (uintptr_t)k)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (s != NULL && lo < s->nframes && s->frames[lo].rule == k) {
        *out = s->frames[lo].frames;
        return s->frames[lo].nframes;
    }
    *out = k->frames;
    return k->nframes;
}

int32_t garb_cascade_font_faces(const garb_cascade_t *c, const garb_font_face_t **out, uint32_t *lost)
{
    if (lost != NULL)
        *lost = c != NULL ? c->faces_lost : 0;
    *out = c != NULL ? c->faces : NULL;
    return c != NULL ? c->nfaces : 0;
}

int32_t garb_cascade_skips(const garb_cascade_t *c, garb_skip_t *out, int32_t cap, uint32_t *lost)
{
    if (lost != NULL)
        *lost = c != NULL ? c->skips_lost : 0;
    if (c == NULL)
        return 0;
    for (int32_t i = 0; i < c->nskips && i < cap; i++)
        out[i] = c->skips[i];
    return c->nskips;
}

// Retained users share updates; each keeps its old computed-style snapshot
// and update number. The host serializes updates with reads.
bool garb_cascade_retain(garb_cascade_t *c)
{
    if (!c || c->refs == UINT32_MAX) return false;
    ++c->refs;
    return true;
}

bool garb_cascade_matches(const garb_cascade_t *c, const garb_sheet_in_t *in,
                          int32_t n, garb_env_t env)
{
    return garb_cascade_matches_scoped(c, in, n, NULL, 0, env);
}

bool garb_cascade_matches_scoped(const garb_cascade_t *c, const garb_sheet_in_t *in, int32_t n,
                                 const garb_scope_in_t *scopes, int32_t nscopes, garb_env_t env)
{
    if (scopes == NULL || nscopes < 0)
        nscopes = 0;
    if (!c || c->ngiven != nscopes)
        return false;
    for (int32_t i = 0; i < nscopes; ++i) {
        const garb_scope_in_t *a = &c->given[i], *b = &scopes[i];
        int32_t bn = b->sheets != NULL && b->n > 0 ? b->n : 0;
        if (a->root != b->root || a->n != bn ||
            (bn > 0 && os64_memcmp(a->sheets, b->sheets, (size_t)bn * sizeof(*a->sheets)) != 0))
            return false;
    }
    if (c->ninputs != n || c->env.width != env.width || c->env.height != env.height ||
        c->env.dark != env.dark || c->env.reduced_motion != env.reduced_motion ||
        c->env.scripting != env.scripting || c->env.modern_off != env.modern_off ||
        c->env.device_width != env.device_width || c->env.device_height != env.device_height ||
        c->env.resolution != env.resolution) return false;
    for (int32_t i = 0; i < n; ++i) {
        const CascadeInput *a = &c->inputs[i];
        const garb_sheet_in_t *b = &in[i];
        if (a->sheet != b->sheet || a->parent != b->parent || a->imported != (b->via != NULL) ||
            ((a->media == NULL) != (b->media == NULL)) ||
            (a->media && !os64_streq(a->media, b->media))) return false;
        if (a->imported && (a->via.url != b->via->url || a->via.len != b->via->len ||
            a->via.media != b->via->media || a->via.nmedia != b->via->nmedia ||
            a->via.has_supports != b->via->has_supports || a->via.supports != b->via->supports ||
            a->via.nsupports != b->via->nsupports)) return false;
    }
    return true;
}

garb_cascade_parts_t garb_cascade_parts(const garb_cascade_t *c)
{
    garb_cascade_parts_t parts = {0};
    if (c == NULL) return parts;
    // The rule, entry and keyframe tables and the selector indexes come
    // from the read's arena, the attribute tables and what updates retain
    // from `self`'s (grow, alloc); the slot table, its rows of anchor bits,
    // the dirty list, the batch's arrays (batch_take), the subject
    // compounds' bits (subjects_live) and the marks on the read's rules are
    // the heap's. A read shared by two cascades is counted in each.
    parts.state = sizeof(*c) + os64_arena_stats(c->self.arena).reserved_bytes +
                  (c->read != NULL ? os64_arena_stats(c->read->heap.arena).reserved_bytes : 0) +
                  (c->seen != NULL ? (size_t)c->nrules * sizeof(*c->seen) : 0);
    parts.reads = c->read != NULL ? os64_arena_stats(c->read->reads.arena).reserved_bytes : 0;
    for (int32_t i = 0; i < c->nattrs; i++) parts.attributes += os64_arena_stats(c->attrs[i].arena).reserved_bytes;
    parts.tables = (size_t)c->slotcap * (sizeof(*c->slots) + c->has_width * sizeof(*c->has_words)) +
                   c->dirtycap * sizeof(*c->dirty_nodes) +
                   (c->batch_cap[0] + c->batch_cap[1] + c->batch_cap[2]) * sizeof(*c->batch_removed) +
                   (size_t)c->subject_width * 2 * sizeof(*c->subjects_union);
    return parts;
}

size_t garb_cascade_bytes(const garb_cascade_t *c)
{
    garb_cascade_parts_t parts = garb_cascade_parts(c);
    return parts.state + parts.reads + parts.attributes + parts.tables;
}

void garb_cascade_free(garb_cascade_t *c)
{
    if (c == NULL)
        return;
    if (--c->refs) return;
    for (int32_t i = 0; i < c->nattrs; i++)
        garb_free(&c->attrs[i]);
    read_release(c->read);
    os64_free(c->seen);
    os64_free(c->slots);
    os64_free(c->has_words);
    os64_free(c->dirty_nodes);
    os64_free((void *)c->batch_removed);
    os64_free((void *)c->batch_added);
    os64_free((void *)c->batch_attrs);
    os64_free(c->subjects_union);
    os64_free(c->subjects_now);
    garb_free(&c->self);
    os64_html_reader_free(c->reader);
    if (c->pin != 0)
        os64_html_unpin(c->doc, c->pin);
    os64_free(c);
}

// ── The dump ────────────────────────────────────────────────────────────

typedef struct {
    char *out;
    size_t cap, len;
} Out;

static void put(Out *o, const char *s, size_t n)
{
    for (size_t k = 0; k < n; k++, o->len++)
        if (o->len + 1 < o->cap)
            o->out[o->len] = s[k];
    if (o->cap > 0)
        o->out[o->len < o->cap ? o->len : o->cap - 1] = '\0';
}

static void puts_(Out *o, const char *s)
{
    put(o, s, os64_strlen(s));
}

// One line of the dump: the element (`p#x`), its pseudo-element's name
// after it when the winners are a pseudo-element's, and the winners.
static void dump_winners(Out *o, const os64_html_node_t *x, const char *pseudo, garb_style_t st)
{
    if (st.n == 0)
        return;
    puts_(o, x->name != NULL ? x->name : "?");
    const os64_html_attr_t *id = os64_html_attr(x, "id");
    if (id != NULL && id->value != NULL) {
        puts_(o, "#");
        puts_(o, id->value);
    }
    puts_(o, pseudo);
    puts_(o, ":");
    for (int32_t k = 0; k < st.n; k++) {
        char v[512];
        garb_val_dump(&st.sets[k].value, v, sizeof(v));
        puts_(o, k ? "; " : " ");
        puts_(o, garb_prop_name(st.sets[k].prop));
        puts_(o, ": ");
        puts_(o, v);
    }
    puts_(o, "\n");
}

// `x` as a path of names from its tree's top, each with its place among
// its element siblings, and its id.
static void compare_path(Out *o, const os64_html_node_t *x)
{
    const os64_html_node_t *chain[48];
    int32_t n = 0;
    for (const os64_html_node_t *up = x; up != NULL && is_el(up) && n < 48; up = up->parent) chain[n++] = up;
    while (n-- > 0) {
        int32_t place = 0;
        for (const os64_html_node_t *s = chain[n]->prev; s != NULL; s = s->prev) place += is_el(s);
        char at[16];
        os64_snprintf(at, sizeof(at), "[%d]", place);
        puts_(o, chain[n]->name != NULL ? chain[n]->name : "?");
        puts_(o, at);
        if (n > 0) puts_(o, ">");
    }
    const os64_html_attr_t *id = os64_html_attr(x, "id");
    if (id != NULL && id->value != NULL) {
        puts_(o, "#");
        puts_(o, id->value);
    }
}
// The first property at which two records differ (-1: none), and its
// index in each (-1 where absent): its value by content, its sheet and its
// importance, as styles_same compares them.
static int32_t compare_styles(garb_style_t a, garb_style_t b, int32_t *ia, int32_t *ib)
{
    for (int32_t k = 0; k < a.n || k < b.n; k++) {
        if (k < a.n && k < b.n && a.sets[k].prop == b.sets[k].prop && val_same(&a.sets[k].value, &b.sets[k].value) &&
            a.sheet[k] == b.sheet[k] && a.important[k] == b.important[k])
            continue;
        garb_prop_t prop = k < a.n ? a.sets[k].prop : b.sets[k].prop;
        *ia = *ib = -1;
        for (int32_t i = 0; i < a.n; i++) if (a.sets[i].prop == prop) *ia = i;
        for (int32_t i = 0; i < b.n; i++) if (b.sets[i].prop == prop) *ib = i;
        return (int32_t)prop;
    }
    return -1;
}
bool garb_cascade_compare(const garb_cascade_t *c, const garb_cascade_t *other, const os64_html_document_t *doc,
                          char *why, size_t cap)
{
    Out o = {why, cap, 0};
    if (cap > 0) why[0] = '\0';
    for (const os64_html_node_t *x = doc->document; x != NULL; x = si_next(x, doc->document, true)) {
        if (!is_el(x)) continue;
        for (int32_t t = 0; t < 3; t++) {
            garb_pseudo_t pseudo = t == 0 ? GARB_PSEUDO_NONE : t == 1 ? GARB_PSEUDO_BEFORE : GARB_PSEUDO_AFTER;
            garb_style_t a = t ? garb_style_for_pseudo(c, x, pseudo) : garb_style_for(c, x);
            garb_style_t b = t ? garb_style_for_pseudo(other, x, pseudo) : garb_style_for(other, x);
            int32_t ia, ib, prop = compare_styles(a, b, &ia, &ib);
            if (prop < 0) continue;
            char va[256] = "(none)", vb[256] = "(none)";
            if (ia >= 0) garb_val_dump(&a.sets[ia].value, va, sizeof(va));
            if (ib >= 0) garb_val_dump(&b.sets[ib].value, vb, sizeof(vb));
            compare_path(&o, x);
            puts_(&o, t == 1 ? "::before " : t == 2 ? "::after " : " ");
            puts_(&o, garb_prop_name((garb_prop_t)prop));
            puts_(&o, ": ");
            puts_(&o, va);
            puts_(&o, " against ");
            puts_(&o, vb);
            // The rules that match there and set it, as the other cascade
            // reads them (a value read through var() is not seen here).
            int32_t named = 0;
            for (int32_t r = 0; r < other->nrules && named < 3; r++) {
                const Rule *rule = &other->rules[r];
                bool sets = false;
                for (int32_t d = 0; d < rule->ndecls && !sets; d++)
                    for (int32_t k = 0; k < rule->decls[d].nsets && !sets; k++)
                        sets = rule->decls[d].sets[k].prop == (garb_prop_t)prop;
                bool matches = false;
                for (int32_t s = 0; sets && !matches && s < garb_selectors_count(rule->sel); s++)
                    matches = garb_selector_pseudo(rule->sel, s) == pseudo && garb_selector_matches(rule->sel, s, x);
                if (!matches) continue;
                char text[512];
                garb_dump_values(rule->prelude, rule->nprelude, text, sizeof(text));
                puts_(&o, named++ ? " | " : "; rules setting it there: ");
                puts_(&o, text);
            }
            return false;
        }
    }
    return true;
}

size_t garb_cascade_dump(const garb_cascade_t *c, const os64_html_document_t *doc, char *out,
                         size_t cap)
{
    Out o = {out, cap, 0};
    if (cap > 0)
        out[0] = '\0';
    for (const os64_html_node_t *x = doc->document; x != NULL; x = si_next(x, doc->document, true))
        if (is_el(x)) {
            dump_winners(&o, x, "", garb_style_for(c, x));
            dump_winners(&o, x, "::before", garb_style_for_pseudo(c, x, GARB_PSEUDO_BEFORE));
            dump_winners(&o, x, "::after", garb_style_for_pseudo(c, x, GARB_PSEUDO_AFTER));
        }
    return o.len;
}
