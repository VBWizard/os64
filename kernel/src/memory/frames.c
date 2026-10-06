// frames.c — the frame table. See frames.h for the contract and FRAMES.md
// for the design. No kernel headers on purpose: the host harness compiles
// this file with plain cc.

#include "frames.h"

// ── The descriptor ──────────────────────────────────────────────────────
//
// 64 bits: kind (2) | position (2) | a (30) | b (30). A RUN is a span of
// frames with one owner; its head and tail both carry its length (Knuth's
// boundary tags), so a run's neighbours are one load away on either side.
//
//   position   FREE                     RUN / LEDGER
//   SINGLE     a = next, b = prev       a = 1,      b = pin count
//   HEAD       a = length, b = next     a = length, b = pin count
//   TAIL       a = length, b = prev     a = length, b = pin count
//   INTERIOR   a = 0, b = 0             a = 0,      b = pin count
//
// The free-list links live here and never in the free frames themselves: a
// free frame is HHDM-unmapped, so it can hold nothing. A RESERVED frame is
// the all-zero word, which is why a zeroed table starts fully reserved.
// RUN frames always carry pin count 0; only LEDGER frames are pinned.

enum { POS_SINGLE = 0, POS_HEAD = 1, POS_TAIL = 2, POS_INTERIOR = 3 };

#define FIELD_MASK ((uint64_t)0x3FFFFFFF)
#define KIND_SHIFT 62
#define POS_SHIFT 60
#define A_SHIFT 30

static inline frame_t make(unsigned kind, unsigned pos, uint64_t a, uint64_t b)
{
	return ((uint64_t)kind << KIND_SHIFT) | ((uint64_t)pos << POS_SHIFT) |
	       ((a & FIELD_MASK) << A_SHIFT) | (b & FIELD_MASK);
}
static inline unsigned kind_of(frame_t w) { return (unsigned)(w >> KIND_SHIFT); }
static inline unsigned pos_of(frame_t w) { return (unsigned)(w >> POS_SHIFT) & 3; }
static inline uint64_t a_of(frame_t w) { return (w >> A_SHIFT) & FIELD_MASK; }
static inline uint64_t b_of(frame_t w) { return w & FIELD_MASK; }
static inline frame_t with_a(frame_t w, uint64_t a)
{
	return (w & ~(FIELD_MASK << A_SHIFT)) | ((a & FIELD_MASK) << A_SHIFT);
}
static inline frame_t with_b(frame_t w, uint64_t b) { return (w & ~FIELD_MASK) | (b & FIELD_MASK); }

// The length of the run whose first frame is `head`.
static inline uint64_t run_length(const frames_t *f, uint64_t head)
{
	frame_t w = f->table[head];
	return pos_of(w) == POS_SINGLE ? 1 : a_of(w);
}

// ── Which list a free run belongs on ────────────────────────────────────

static inline unsigned floor_log2(uint64_t x) { return 63u - (unsigned)__builtin_clzll(x); }

static inline void tlsf_index(uint64_t len, unsigned *fl, unsigned *sl)
{
	unsigned l = floor_log2(len);
	*fl = l;
	*sl = (unsigned)(len >> (l - FRAMES_SL_LOG2)) & (FRAMES_SL_COUNT - 1);
}

static uint64_t *list_head(frames_t *f, uint64_t len)
{
	if (len <= FRAMES_EXACT_MAX)
		return &f->exact_head[len];
	unsigned fl, sl;
	tlsf_index(len, &fl, &sl);
	return &f->tlsf_head[fl][sl];
}

static void list_mark(frames_t *f, uint64_t len, bool nonempty)
{
	if (len <= FRAMES_EXACT_MAX) {
		uint64_t bit = (uint64_t)1 << (len - 1);
		f->exact_mask = nonempty ? (f->exact_mask | bit) : (f->exact_mask & ~bit);
		return;
	}
	unsigned fl, sl;
	tlsf_index(len, &fl, &sl);
	if (nonempty) {
		f->sl_mask[fl] |= (uint8_t)(1u << sl);
		f->fl_mask |= 1u << fl;
	} else {
		f->sl_mask[fl] &= (uint8_t)~(1u << sl);
		if (f->sl_mask[fl] == 0)
			f->fl_mask &= ~(1u << fl);
	}
}

// ── The doubly linked free lists, threaded through the descriptors ──────

static uint64_t free_next(const frames_t *f, uint64_t head)
{
	frame_t w = f->table[head];
	return pos_of(w) == POS_SINGLE ? a_of(w) : b_of(w);
}

static uint64_t free_prev(const frames_t *f, uint64_t head)
{
	frame_t w = f->table[head];
	if (pos_of(w) == POS_SINGLE)
		return b_of(w);
	return b_of(f->table[head + a_of(w) - 1]);
}

static void set_next(frames_t *f, uint64_t head, uint64_t next)
{
	frame_t w = f->table[head];
	f->table[head] = pos_of(w) == POS_SINGLE ? with_a(w, next) : with_b(w, next);
}

static void set_prev(frames_t *f, uint64_t head, uint64_t prev)
{
	frame_t w = f->table[head];
	if (pos_of(w) == POS_SINGLE) {
		f->table[head] = with_b(w, prev);
		return;
	}
	uint64_t tail = head + a_of(w) - 1;
	f->table[tail] = with_b(f->table[tail], prev);
}

static void list_insert(frames_t *f, uint64_t head, uint64_t len)
{
	uint64_t *lh = list_head(f, len);
	set_next(f, head, *lh);
	set_prev(f, head, FRAMES_NONE);
	if (*lh != FRAMES_NONE)
		set_prev(f, *lh, head);
	*lh = head;
	list_mark(f, len, true);
}

static void list_remove(frames_t *f, uint64_t head, uint64_t len)
{
	uint64_t *lh = list_head(f, len);
	uint64_t prev = free_prev(f, head), next = free_next(f, head);
	if (prev == FRAMES_NONE)
		*lh = next;
	else
		set_next(f, prev, next);
	if (next != FRAMES_NONE)
		set_prev(f, next, prev);
	if (*lh == FRAMES_NONE)
		list_mark(f, len, false);
}

// ── Writing runs ────────────────────────────────────────────────────────

// Every frame of [first, first+count) becomes an interior frame of `kind`.
// What makes "is this frame live?" one load costs this many writes, which
// is the size of the memory being handed over or taken back.
static void mark_interior(frames_t *f, uint64_t first, uint64_t count, unsigned kind)
{
	frame_t w = make(kind, POS_INTERIOR, 0, 0);
	for (uint64_t i = 0; i < count; i++)
		f->table[first + i] = w;
}

static void write_allocated(frames_t *f, uint64_t first, uint64_t count, unsigned kind)
{
	if (count == 1) {
		f->table[first] = make(kind, POS_SINGLE, 1, 0);
		return;
	}
	mark_interior(f, first + 1, count - 2, kind);
	f->table[first] = make(kind, POS_HEAD, count, 0);
	f->table[first + count - 1] = make(kind, POS_TAIL, count, 0);
}

// Head and tail tags for a free run whose interior is already FREE
// interior, then onto its list.
static void write_free(frames_t *f, uint64_t first, uint64_t count)
{
	if (count == 1) {
		f->table[first] = make(FRAMES_FREE, POS_SINGLE, FRAMES_NONE, FRAMES_NONE);
	} else {
		f->table[first] = make(FRAMES_FREE, POS_HEAD, count, FRAMES_NONE);
		f->table[first + count - 1] = make(FRAMES_FREE, POS_TAIL, count, FRAMES_NONE);
	}
	list_insert(f, first, count);
	f->free_runs++;
}

// [first, first+count) has just become free (every frame already FREE
// interior). Join the free run below it and the free run above it, if any,
// so no two free runs are ever adjacent, and file the result. A neighbour's
// boundary tags that end up inside the merged run go back to interior.
static void release(frames_t *f, uint64_t first, uint64_t count)
{
	uint64_t start = first, total = count;

	if (first > 0 && kind_of(f->table[first - 1]) == FRAMES_FREE) {
		frame_t below = f->table[first - 1];        // its tail, or a single
		uint64_t len = pos_of(below) == POS_SINGLE ? 1 : a_of(below);
		uint64_t head = first - len;
		list_remove(f, head, len);
		f->free_runs--;
		if (len > 1)
			f->table[first - 1] = make(FRAMES_FREE, POS_INTERIOR, 0, 0);
		start = head;
		total += len;
	}

	uint64_t end = first + count;
	if (end < f->nframes && kind_of(f->table[end]) == FRAMES_FREE) {
		uint64_t len = run_length(f, end);          // its head, or a single
		list_remove(f, end, len);
		f->free_runs--;
		if (len > 1)
			f->table[end] = make(FRAMES_FREE, POS_INTERIOR, 0, 0);
		total += len;
	}

	write_free(f, start, total);
}

// ── Describing memory ───────────────────────────────────────────────────

size_t frames_table_bytes(uint64_t nframes)
{
	return (size_t)nframes * sizeof(frame_t);
}

frames_status_t frames_init(frames_t *f, frame_t *table, uint64_t nframes)
{
	if (f == NULL || table == NULL || nframes == 0 || nframes > FRAMES_NONE)
		return FRAMES_BAD_ARGUMENT;
	f->table = table;
	f->nframes = nframes;
	for (uint64_t i = 0; i < nframes; i++)
		table[i] = 0;
	for (unsigned i = 0; i <= FRAMES_EXACT_MAX; i++)
		f->exact_head[i] = FRAMES_NONE;
	for (unsigned i = 0; i < FRAMES_FL_COUNT; i++) {
		for (unsigned j = 0; j < FRAMES_SL_COUNT; j++)
			f->tlsf_head[i][j] = FRAMES_NONE;
		f->sl_mask[i] = 0;
	}
	f->exact_mask = 0;
	f->fl_mask = 0;
	f->usable_frames = f->free_frames = f->run_frames = f->ledger_frames = 0;
	f->free_runs = 0;
	f->allocs = f->refusals = f->fallback_walks = f->fallback_examined = 0;
	return FRAMES_OK;
}

static frames_status_t check_reserved(const frames_t *f, uint64_t first, uint64_t count)
{
	if (first >= f->nframes || count > f->nframes - first)
		return FRAMES_OUT_OF_RANGE;
	for (uint64_t i = 0; i < count; i++)
		if (f->table[first + i] != 0)
			return FRAMES_NOT_RESERVED;
	return FRAMES_OK;
}

frames_status_t frames_add_region(frames_t *f, uint64_t first, uint64_t count)
{
	if (f == NULL)
		return FRAMES_BAD_ARGUMENT;
	// Page zero is never memory anybody may be handed.
	if (first == 0 && count > 0) {
		first = 1;
		count--;
	}
	if (count == 0)
		return FRAMES_OK;
	frames_status_t s = check_reserved(f, first, count);
	if (s != FRAMES_OK)
		return s;
	mark_interior(f, first, count, FRAMES_FREE);
	f->usable_frames += count;
	f->free_frames += count;
	release(f, first, count);
	return FRAMES_OK;
}

frames_status_t frames_add_allocated(frames_t *f, uint64_t first, uint64_t count,
                                     frames_kind_t kind)
{
	if (f == NULL || count == 0 || first == 0 || (kind != FRAMES_RUN && kind != FRAMES_LEDGER))
		return FRAMES_BAD_ARGUMENT;
	frames_status_t s = check_reserved(f, first, count);
	if (s != FRAMES_OK)
		return s;
	write_allocated(f, first, count, kind);
	f->usable_frames += count;
	if (kind == FRAMES_RUN)
		f->run_frames += count;
	else
		f->ledger_frames += count;
	return FRAMES_OK;
}

// ── Allocating ──────────────────────────────────────────────────────────

// The lowest non-empty TLSF list at or above (fl, sl), or FRAMES_NONE.
static uint64_t tlsf_first_at_or_above(const frames_t *f, unsigned fl, unsigned sl)
{
	if (fl >= FRAMES_FL_COUNT)
		return FRAMES_NONE;
	unsigned sl_bits = sl < FRAMES_SL_COUNT ? (unsigned)f->sl_mask[fl] & (~0u << sl) : 0;
	if (sl_bits == 0) {
		uint32_t fl_bits = fl + 1 < 32 ? f->fl_mask & (~0u << (fl + 1)) : 0;
		if (fl_bits == 0)
			return FRAMES_NONE;
		fl = (unsigned)__builtin_ctz(fl_bits);
		sl_bits = f->sl_mask[fl];
	}
	return f->tlsf_head[fl][__builtin_ctz(sl_bits)];
}

// A free run of at least `count` frames, or FRAMES_NONE. Exact first, then
// best fit by class, and never a walk except the one the TLSF round-up
// makes necessary just before refusing.
static uint64_t find(frames_t *f, uint64_t count)
{
	if (count <= FRAMES_EXACT_MAX) {
		if (f->exact_head[count] != FRAMES_NONE)
			return f->exact_head[count];
		// Lists for lengths count+1 .. 64 are mask bits count .. 63.
		uint64_t longer = count < 64 ? f->exact_mask & (~(uint64_t)0 << count) : 0;
		if (longer != 0)
			return f->exact_head[__builtin_ctzll(longer) + 1];
		// Every TLSF run is longer than FRAMES_EXACT_MAX: the smallest will do.
		return tlsf_first_at_or_above(f, 0, 0);
	}

	// Round up to the next sub-bin boundary: every run on a list at or above
	// the rounded index is at least `count` long, so its head fits.
	unsigned fl, sl;
	uint64_t rounded = count + ((uint64_t)1 << (floor_log2(count) - FRAMES_SL_LOG2)) - 1;
	tlsf_index(rounded, &fl, &sl);
	uint64_t head = tlsf_first_at_or_above(f, fl, sl);
	if (head != FRAMES_NONE)
		return head;

	// The round-up skipped `count`'s own sub-bin, which may hold a run long
	// enough. Only now, about to refuse, walk that one list.
	tlsf_index(count, &fl, &sl);
	f->fallback_walks++;
	for (uint64_t h = f->tlsf_head[fl][sl]; h != FRAMES_NONE; h = free_next(f, h)) {
		f->fallback_examined++;
		if (run_length(f, h) >= count)
			return h;
	}
	return FRAMES_NONE;
}

uint64_t frames_alloc(frames_t *f, uint64_t count, frames_kind_t kind)
{
	if (f == NULL || count == 0 || count >= FRAMES_NONE ||
	    (kind != FRAMES_RUN && kind != FRAMES_LEDGER))
		return FRAMES_NONE;
	f->allocs++;
	uint64_t head = find(f, count);
	if (head == FRAMES_NONE) {
		f->refusals++;
		return FRAMES_NONE;
	}
	uint64_t len = run_length(f, head);
	list_remove(f, head, len);
	f->free_runs--;

	// The front of the run is handed out; the rest stays free. The rest's
	// interior is already FREE interior, and its old tail is rewritten as its
	// own tail by write_free.
	write_allocated(f, head, count, kind);
	if (len > count)
		write_free(f, head + count, len - count);

	f->free_frames -= count;
	if (kind == FRAMES_RUN)
		f->run_frames += count;
	else
		f->ledger_frames += count;
	return head;
}

// ── Freeing ─────────────────────────────────────────────────────────────

frames_status_t frames_free(frames_t *f, uint64_t first, uint64_t *count_out)
{
	if (f == NULL)
		return FRAMES_BAD_ARGUMENT;
	if (first >= f->nframes)
		return FRAMES_OUT_OF_RANGE;
	frame_t w = f->table[first];
	unsigned kind = kind_of(w);
	if (kind != FRAMES_RUN && kind != FRAMES_LEDGER)
		return FRAMES_NOT_ALLOCATED;
	if (pos_of(w) != POS_SINGLE && pos_of(w) != POS_HEAD)
		return FRAMES_NOT_A_START;
	uint64_t count = run_length(f, first);
	// A ledger run goes back only when nothing it lent out is still alive.
	if (kind == FRAMES_LEDGER)
		for (uint64_t i = 0; i < count; i++)
			if (b_of(f->table[first + i]) != 0)
				return FRAMES_LEDGER_PINNED;

	mark_interior(f, first, count, FRAMES_FREE);
	if (kind == FRAMES_RUN)
		f->run_frames -= count;
	else
		f->ledger_frames -= count;
	f->free_frames += count;
	release(f, first, count);
	if (count_out != NULL)
		*count_out = count;
	return FRAMES_OK;
}

// ── The ledger's pins ───────────────────────────────────────────────────

frames_status_t frames_ledger_pin(frames_t *f, uint64_t first, uint64_t count, int delta)
{
	if (f == NULL || count == 0 || (delta != 1 && delta != -1))
		return FRAMES_BAD_ARGUMENT;
	if (first >= f->nframes || count > f->nframes - first)
		return FRAMES_OUT_OF_RANGE;
	// Check every frame before changing any, so a refusal changes nothing.
	for (uint64_t i = 0; i < count; i++) {
		frame_t w = f->table[first + i];
		if (kind_of(w) != FRAMES_LEDGER)
			return FRAMES_NOT_LEDGER;
		uint64_t pins = b_of(w);
		if ((delta < 0 && pins == 0) || (delta > 0 && pins == FIELD_MASK))
			return FRAMES_PIN_RANGE;
	}
	for (uint64_t i = 0; i < count; i++) {
		frame_t w = f->table[first + i];
		f->table[first + i] = with_b(w, delta > 0 ? b_of(w) + 1 : b_of(w) - 1);
	}
	return FRAMES_OK;
}

// ── Questions ───────────────────────────────────────────────────────────

bool frames_live(const frames_t *f, uint64_t frame)
{
	if (f == NULL || frame >= f->nframes)
		return false;
	frame_t w = f->table[frame];
	unsigned kind = kind_of(w);
	return kind == FRAMES_RUN || (kind == FRAMES_LEDGER && b_of(w) != 0);
}

frames_kind_t frames_kind(const frames_t *f, uint64_t frame)
{
	if (f == NULL || frame >= f->nframes)
		return FRAMES_RESERVED;
	return (frames_kind_t)kind_of(f->table[frame]);
}

uint64_t frames_run_start(const frames_t *f, uint64_t frame)
{
	if (f == NULL || frame >= f->nframes || f->table[frame] == 0)
		return FRAMES_NONE;
	while (pos_of(f->table[frame]) == POS_INTERIOR || pos_of(f->table[frame]) == POS_TAIL) {
		if (frame == 0)
			return FRAMES_NONE;
		frame--;
	}
	return frame;
}

uint64_t frames_largest_free(const frames_t *f)
{
	if (f == NULL)
		return 0;
	if (f->fl_mask != 0) {
		unsigned fl = floor_log2(f->fl_mask);
		unsigned sl = floor_log2(f->sl_mask[fl]);
		uint64_t best = 0;
		for (uint64_t h = f->tlsf_head[fl][sl]; h != FRAMES_NONE; h = free_next(f, h))
			if (run_length(f, h) > best)
				best = run_length(f, h);
		return best;
	}
	return f->exact_mask != 0 ? (uint64_t)floor_log2(f->exact_mask) + 1 : 0;
}

// ── The audit ───────────────────────────────────────────────────────────

#define AUDIT_FAIL(why, at) do { if (where != NULL) *where = (at); return (why); } while (0)

// Walk one list: every member is the head of a free run whose length files
// it here, the back links agree, and the walk ends. Returns the member count
// through *members.
static const char *audit_list(const frames_t *f, uint64_t head, uint64_t lo, uint64_t hi,
                              uint64_t *members, uint64_t *where)
{
	uint64_t prev = FRAMES_NONE, n = 0;
	for (uint64_t h = head; h != FRAMES_NONE; h = free_next(f, h)) {
		if (h >= f->nframes)
			AUDIT_FAIL("list link points past the table", h);
		frame_t w = f->table[h];
		if (kind_of(w) != FRAMES_FREE || (pos_of(w) != POS_SINGLE && pos_of(w) != POS_HEAD))
			AUDIT_FAIL("list member is not the head of a free run", h);
		uint64_t len = run_length(f, h);
		if (len < lo || len > hi)
			AUDIT_FAIL("free run filed on the wrong list", h);
		if (free_prev(f, h) != prev)
			AUDIT_FAIL("free list back link disagrees", h);
		if (++n > f->free_runs)
			AUDIT_FAIL("free list longer than the free runs (a cycle)", h);
		prev = h;
	}
	*members = n;
	return NULL;
}

const char *frames_audit(const frames_t *f, uint64_t *where)
{
	if (f == NULL || f->table == NULL)
		AUDIT_FAIL("no table", 0);
	if (f->nframes > 0 && f->table[0] != 0)
		AUDIT_FAIL("frame 0 is described as memory", 0);

	uint64_t counted[4] = {0, 0, 0, 0}, free_runs = 0;
	unsigned last_kind = FRAMES_RESERVED;
	for (uint64_t i = 0; i < f->nframes;) {
		frame_t w = f->table[i];
		unsigned kind = kind_of(w);
		if (kind == FRAMES_RESERVED) {
			if (w != 0)
				AUDIT_FAIL("reserved frame with bits set", i);
			last_kind = kind;
			i++;
			continue;
		}
		unsigned pos = pos_of(w);
		if (pos != POS_SINGLE && pos != POS_HEAD)
			AUDIT_FAIL("run starts at a tail or interior frame", i);
		uint64_t len = pos == POS_SINGLE ? 1 : a_of(w);
		if (pos == POS_SINGLE && kind != FRAMES_FREE && a_of(w) != 1)
			AUDIT_FAIL("allocated single frame does not say length 1", i);
		if (pos == POS_HEAD && len < 2)
			AUDIT_FAIL("run head with a length under 2", i);
		if (len > f->nframes - i)
			AUDIT_FAIL("run runs off the table", i);
		if (kind == FRAMES_FREE && last_kind == FRAMES_FREE)
			AUDIT_FAIL("two free runs side by side (not coalesced)", i);
		for (uint64_t j = 1; j + 1 < len; j++) {
			frame_t x = f->table[i + j];
			if (kind_of(x) != kind || pos_of(x) != POS_INTERIOR || a_of(x) != 0)
				AUDIT_FAIL("run interior frame is wrong", i + j);
			if (kind != FRAMES_LEDGER && b_of(x) != 0)
				AUDIT_FAIL("pin count on a frame the ledger does not hold", i + j);
		}
		if (len > 1) {
			frame_t t = f->table[i + len - 1];
			if (kind_of(t) != kind || pos_of(t) != POS_TAIL || a_of(t) != len)
				AUDIT_FAIL("run tail tag disagrees with its head", i + len - 1);
		}
		if (kind == FRAMES_RUN && b_of(w) != 0)
			AUDIT_FAIL("pin count on a frame the ledger does not hold", i);
		if (kind == FRAMES_RUN && len > 1 && b_of(f->table[i + len - 1]) != 0)
			AUDIT_FAIL("pin count on a frame the ledger does not hold", i + len - 1);
		counted[kind] += len;
		if (kind == FRAMES_FREE)
			free_runs++;
		last_kind = kind;
		i += len;
	}

	if (counted[FRAMES_FREE] != f->free_frames)
		AUDIT_FAIL("free frame counter disagrees with the table", counted[FRAMES_FREE]);
	if (counted[FRAMES_RUN] != f->run_frames)
		AUDIT_FAIL("run frame counter disagrees with the table", counted[FRAMES_RUN]);
	if (counted[FRAMES_LEDGER] != f->ledger_frames)
		AUDIT_FAIL("ledger frame counter disagrees with the table", counted[FRAMES_LEDGER]);
	if (f->free_frames + f->run_frames + f->ledger_frames != f->usable_frames)
		AUDIT_FAIL("free + run + ledger != usable", f->usable_frames);
	if (free_runs != f->free_runs)
		AUDIT_FAIL("free run counter disagrees with the table", free_runs);

	uint64_t listed = 0, members;
	const char *why;
	for (uint64_t len = 1; len <= FRAMES_EXACT_MAX; len++) {
		why = audit_list(f, f->exact_head[len], len, len, &members, where);
		if (why != NULL)
			return why;
		if ((members != 0) != ((f->exact_mask >> (len - 1)) & 1))
			AUDIT_FAIL("exact mask bit disagrees with its list", len);
		listed += members;
	}
	for (unsigned fl = 0; fl < FRAMES_FL_COUNT; fl++) {
		for (unsigned sl = 0; sl < FRAMES_SL_COUNT; sl++) {
			uint64_t head = f->tlsf_head[fl][sl], lo = 0, hi = 0;
			if (fl > FRAMES_SL_LOG2) {
				lo = ((uint64_t)1 << fl) + ((uint64_t)sl << (fl - FRAMES_SL_LOG2));
				hi = lo + ((uint64_t)1 << (fl - FRAMES_SL_LOG2)) - 1;
			}
			if (lo <= FRAMES_EXACT_MAX)
				lo = FRAMES_EXACT_MAX + 1;
			if (head != FRAMES_NONE && hi < lo)
				AUDIT_FAIL("a TLSF list that no length files onto is in use", fl);
			if (head != FRAMES_NONE) {
				why = audit_list(f, head, lo, hi, &members, where);
				if (why != NULL)
					return why;
				listed += members;
			}
			if ((head != FRAMES_NONE) != ((f->sl_mask[fl] >> sl) & 1))
				AUDIT_FAIL("TLSF sub-bin mask bit disagrees with its list", fl);
		}
		if ((f->sl_mask[fl] != 0) != ((f->fl_mask >> fl) & 1))
			AUDIT_FAIL("TLSF bin mask bit disagrees with its sub-bins", fl);
	}
	// Every free run was found by the walk; every list member is a free run;
	// with matching totals and back links, each run is on exactly one list.
	if (listed != f->free_runs)
		AUDIT_FAIL("free runs on the lists != free runs in the table", listed);
	return NULL;
}
