// scribe_buf.c — the line-array model behind scribe's textview. See
// scribe_buf.h for the contract and SCRIBE.md for why it is deliberately
// boring. Every mutation flows through insert/erase/split/join(/erase_lines)
// — the choke points where slice two's undo journal will attach.

#include "scribe_buf.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "os64/fmt.h"

// ── line plumbing ───────────────────────────────────────────────────────────

static bool line_reserve(sbuf_line_t *ln, size_t need)
{
	if (need <= ln->cap)
		return true;
	size_t cap = ln->cap ? ln->cap : 16;
	while (cap < need)
		cap *= 2;
	char *p = os64_realloc(ln->bytes, cap);
	if (!p)
		return false;
	ln->bytes = p;
	ln->cap = cap;
	return true;
}

static bool lines_reserve(sbuf_t *b, size_t need)
{
	if (need <= b->cap)
		return true;
	size_t cap = b->cap ? b->cap : 64;
	while (cap < need)
		cap *= 2;
	sbuf_line_t *p = os64_realloc(b->lines, cap * sizeof(sbuf_line_t));
	if (!p)
		return false;
	b->lines = p;
	b->cap = cap;
	return true;
}

// Open a gap in the line table at `at` (the caller fills it).
static bool lines_open_gap(sbuf_t *b, size_t at)
{
	if (!lines_reserve(b, b->count + 1))
		return false;
	os64_memmove(&b->lines[at + 1], &b->lines[at],
	             (b->count - at) * sizeof(sbuf_line_t));
	b->count++;
	return true;
}

bool sbuf_init(sbuf_t *b)
{
	*b = (sbuf_t){0};
	if (!lines_reserve(b, 1))
		return false;
	b->lines[0] = (sbuf_line_t){0};
	b->count = 1;
	return true;
}

void sbuf_free(sbuf_t *b)
{
	for (size_t i = 0; i < b->count; i++)
		os64_free(b->lines[i].bytes);
	os64_free(b->lines);
	*b = (sbuf_t){0};
}

// ── the vtable ──────────────────────────────────────────────────────────────

static size_t vt_line_count(void *user)
{
	return ((sbuf_t *)user)->count;
}

static const char *vt_line(void *user, size_t idx, size_t *len)
{
	sbuf_t *b = user;
	if (idx >= b->count) {
		*len = 0;
		return "";
	}
	*len = b->lines[idx].len;
	// An empty line owns no bytes yet; hand back a real pointer anyway so
	// callers never branch on NULL-with-len-zero.
	return b->lines[idx].bytes ? b->lines[idx].bytes : "";
}

static bool vt_insert(void *user, size_t line, size_t col,
                      const char *s, size_t n)
{
	sbuf_t *b = user;
	if (line >= b->count)
		return false;
	sbuf_line_t *ln = &b->lines[line];
	if (col > ln->len)
		col = ln->len;
	if (!line_reserve(ln, ln->len + n))
		return false;
	os64_memmove(ln->bytes + col + n, ln->bytes + col, ln->len - col);
	os64_memcpy(ln->bytes + col, s, n);
	ln->len += n;
	b->dirty = true;
	return true;
}

static bool vt_erase(void *user, size_t line, size_t col, size_t n)
{
	sbuf_t *b = user;
	if (line >= b->count)
		return false;
	sbuf_line_t *ln = &b->lines[line];
	if (col > ln->len)
		return false;
	if (n > ln->len - col)
		n = ln->len - col;
	os64_memmove(ln->bytes + col, ln->bytes + col + n, ln->len - col - n);
	ln->len -= n;
	b->dirty = true;
	return true;
}

static bool vt_split(void *user, size_t line, size_t col)
{
	sbuf_t *b = user;
	if (line >= b->count)
		return false;
	sbuf_line_t *ln = &b->lines[line];
	if (col > ln->len)
		col = ln->len;

	sbuf_line_t tail = {0};
	size_t tail_len = ln->len - col;
	if (tail_len > 0) {
		if (!line_reserve(&tail, tail_len))
			return false;
		os64_memcpy(tail.bytes, ln->bytes + col, tail_len);
		tail.len = tail_len;
	}
	if (!lines_open_gap(b, line + 1)) {
		os64_free(tail.bytes);
		return false;
	}
	// The gap is open and nothing can fail past here — only now shorten the
	// original, so a failure above leaves the buffer exactly as it was.
	b->lines[line + 1] = tail;
	ln = &b->lines[line];      // lines[] may have moved in the realloc
	ln->len = col;
	b->dirty = true;
	return true;
}

static bool vt_join(void *user, size_t line)
{
	sbuf_t *b = user;
	if (line + 1 >= b->count)
		return false;
	sbuf_line_t *a = &b->lines[line];
	sbuf_line_t *z = &b->lines[line + 1];
	if (z->len > 0) {
		if (!line_reserve(a, a->len + z->len))
			return false;
		os64_memcpy(a->bytes + a->len, z->bytes, z->len);
		a->len += z->len;
	}
	os64_free(z->bytes);
	os64_memmove(&b->lines[line + 1], &b->lines[line + 2],
	             (b->count - line - 2) * sizeof(sbuf_line_t));
	b->count--;
	b->dirty = true;
	return true;
}

static bool vt_erase_lines(void *user, size_t first, size_t count)
{
	sbuf_t *b = user;
	if (first >= b->count || count == 0)
		return false;
	if (count > b->count - first)
		count = b->count - first;
	for (size_t i = first; i < first + count; i++)
		os64_free(b->lines[i].bytes);
	os64_memmove(&b->lines[first], &b->lines[first + count],
	             (b->count - first - count) * sizeof(sbuf_line_t));
	b->count -= count;
	// The count >= 1 invariant survives every caller today (the textview
	// only bulk-erases INTERIOR lines of a selection), but the model defends
	// itself rather than trusting that forever.
	if (b->count == 0) {
		b->lines[0] = (sbuf_line_t){0};
		b->count = 1;
	}
	b->dirty = true;
	return true;
}

const os64_ui_textbuf_t sbuf_textbuf_template = {
	.user        = NULL,   // scribe fills this with its sbuf_t*
	.line_count  = vt_line_count,
	.line        = vt_line,
	.insert      = vt_insert,
	.erase       = vt_erase,
	.split       = vt_split,
	.join        = vt_join,
	.erase_lines = vt_erase_lines,
};

// ── load / save ─────────────────────────────────────────────────────────────

int sbuf_load(sbuf_t *b, const char *path, char *err, size_t errcap)
{
	os64_dirent_t e;
	if (os64_stat(path, &e) < 0) {
		// No such file: an editor opens it EMPTY and creates it at save —
		// that has been an editor's contract since ed.
		sbuf_free(b);
		if (!sbuf_init(b))
			goto oom;
		return 1;
	}
	uint64_t size = e.size;

	// The memory-aware ceiling (SCRIBE.md — the 4MB magic number died at
	// second reading): the file, its line copies, and a line-table estimate
	// must fit comfortably in what the machine says is AVAILABLE. Refuse
	// with both numbers, never thrash.
	os64_memory_t m;
	if (os64_memory(&m) == 0) {
		uint64_t need = size * 2 + size / 8 + (1u << 20);
		if (need > m.available / 2) {
			os64_snprintf(err, errcap,
			              "too big: %lu MB file needs ~%lu MB, %lu MB available",
			              size >> 20, need >> 20, m.available >> 20);
			return -1;
		}
	}

	char *blob = os64_malloc(size + 1);
	if (!blob)
		goto oom;
	int64_t fd = os64_open(path, "r");
	if (fd < 0) {
		os64_free(blob);
		os64_snprintf(err, errcap, "cannot open %s", path);
		return -1;
	}
	uint64_t got = 0;
	int64_t n;
	while (got < size && (n = os64_read((int32_t)fd, blob + got, size - got)) > 0)
		got += (uint64_t)n;
	os64_close((int32_t)fd);

	// Count lines first so the table is ONE allocation, not a doubling walk.
	size_t nlines = 1;
	for (uint64_t i = 0; i < got; i++)
		if (blob[i] == '\n')
			nlines++;
	// A file ENDING in \n has no phantom empty last line ("a\n" is one line
	// — vi's answer, kept).
	if (got > 0 && blob[got - 1] == '\n')
		nlines--;
	if (nlines == 0)
		nlines = 1;

	sbuf_free(b);
	*b = (sbuf_t){0};
	if (!lines_reserve(b, nlines)) {
		os64_free(blob);
		goto oom;
	}

	uint64_t at = 0;
	for (size_t li = 0; li < nlines; li++) {
		uint64_t end = at;
		while (end < got && blob[end] != '\n')
			end++;
		sbuf_line_t ln = {0};
		if (end > at) {
			if (!line_reserve(&ln, end - at)) {
				os64_free(blob);
				goto oom;
			}
			os64_memcpy(ln.bytes, blob + at, end - at);
			ln.len = end - at;
		}
		b->lines[li] = ln;
		b->count++;
		at = end + 1;   // step over the newline
	}
	os64_free(blob);
	if (b->count == 0) {
		b->lines[0] = (sbuf_line_t){0};
		b->count = 1;
	}
	b->dirty = false;
	return 0;

oom:
	if (b->count == 0)
		sbuf_init(b);
	os64_snprintf(err, errcap, "out of memory loading %s", path);
	return -1;
}

// Chunked writer: 1.5 million two-syscall lines would be the slow way to
// save a big log; 64KB batches are the block cache's own stride.
typedef struct
{
	int32_t fd;
	char    chunk[65536];
	size_t  fill;
	bool    failed;
} sbuf_writer_t;

static void writer_flush(sbuf_writer_t *wr)
{
	if (wr->fill == 0 || wr->failed)
		return;
	if (os64_write(wr->fd, wr->chunk, wr->fill) != (int64_t)wr->fill)
		wr->failed = true;
	wr->fill = 0;
}

static void writer_put(sbuf_writer_t *wr, const char *s, size_t n)
{
	while (n > 0 && !wr->failed) {
		size_t room = sizeof(wr->chunk) - wr->fill;
		size_t take = n < room ? n : room;
		os64_memcpy(wr->chunk + wr->fill, s, take);
		wr->fill += take;
		s += take;
		n -= take;
		if (wr->fill == sizeof(wr->chunk))
			writer_flush(wr);
	}
}

int sbuf_save(sbuf_t *b, const char *path, char *err, size_t errcap)
{
	int64_t fd = os64_open(path, "w");
	if (fd < 0) {
		os64_snprintf(err, errcap, "cannot write %s", path);
		return -1;
	}
	// Field-by-field, not a struct initializer: the `= { ... }` spelling
	// makes GCC memset the whole 64KB chunk (the call this line originally
	// couldn't even LINK — str.c carries the compiler's four now), and
	// zeroing 65536 bytes to initialize three is waste either way. Only
	// these fields are live before first use.
	sbuf_writer_t wr;
	wr.fd = (int32_t)fd;
	wr.fill = 0;
	wr.failed = false;
	for (size_t i = 0; i < b->count; i++) {
		writer_put(&wr, b->lines[i].bytes ? b->lines[i].bytes : "",
		           b->lines[i].len);
		writer_put(&wr, "\n", 1);
	}
	writer_flush(&wr);
	os64_close((int32_t)fd);
	if (wr.failed) {
		os64_snprintf(err, errcap, "write failed on %s", path);
		return -1;
	}
	b->dirty = false;
	return 0;
}
