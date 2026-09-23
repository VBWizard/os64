// vncd — the desktop, to a VNC viewer across the room (REMOTE.md section 5).
//
// RFB (RFC 6143) came out of the Olivetti Research Laboratory in Cambridge
// in the late 1990s, to let a desktop follow its owner to any screen. It is
// a pull protocol: the viewer asks for an update, and the server answers
// with what changed since the last one. That is why it degrades gracefully
// on a slow link: changes pile up in one place and go out as fewer, larger
// updates, never as a backlog.
//
// THE SECURITY STORY IS THE LISTENER'S ADDRESS. vncd offers RFB security
// type None, and it may, because it announces on 127.0.0.1 and nowhere else:
// no card delivers a 127/8 destination (REMOTE.md section 1), so the only
// way in from another machine is sshd's direct-tcpip forward, which a key
// check guards. There is no option to listen anywhere else. A vncd that
// cannot have loopback does not start.
//
//   vncd               the listener: announce, then one child per viewer
//   vncd -session      one viewer, on handles 0 and 1 (the connection)
//
// A session keeps a SHADOW of the screen fed by /dev/glass (os64/glass.h):
// every changed rectangle the glass reports lands in the shadow and in a
// dirty list, and an update request is answered from the shadow. Two
// threads, telnetd's shape: the inbound one parses the viewer's messages and
// types on the glass; the outbound one reads the glass and is the ONLY
// writer of the connection, since a TCP write can stop short and two
// writers could interleave inside a message (SERVERS.md section 3).

#include "os64/os64.h"
#include "os64/conf.h"
#include "os64/draw.h"
#include "os64/glass.h"
#include "gzip/deflate.h"
#include "zrle.h"

#define VNCD_PORT_DEFAULT  5900
#define VNCD_SESSIONS_MAX  4
#define VNCD_DIRTY_MAX     32
#define VNCD_BANNER_H      20
#define VNCD_OUT_CAP       (64 * 1024)
#define VNCD_CUT_TEXT_MAX  (1024 * 1024)
// How much of the glass one turn takes before answering the viewer: two
// full 1080p frames of pixels, so a turn keeps up with any screen and still
// ends.
#define VNCD_DRAIN_BUDGET  (16u * 1024 * 1024)

static void log_line(const char *text)
{
	os64_debug_log(text);
	os64_write(2, text, os64_strlen(text));
	os64_write(2, "\n", 1);
}

// ── Shared session state ────────────────────────────────────────────────────
// The two threads meet here, under `s_lock`: a test-and-set that yields on
// contention. Every hold is a handful of stores.

typedef struct { int32_t x, y, w, h; } rect_t;

static volatile uint32_t s_lock;
static vnc_format_t s_format = { 32, 24, 0, 255, 255, 255, 16, 8, 0 };
static bool   s_zrle;             // the viewer prefers ZRLE to Raw
static bool   s_request;          // an update request is outstanding
static bool   s_request_full;     // ... and it asked for everything in its rectangle
static rect_t s_request_rect;
static volatile bool s_quit;      // either thread: the session is over

static void lock(void)
{
	while (__atomic_test_and_set(&s_lock, __ATOMIC_ACQUIRE))
		os64_sleep(0);
}

static void unlock(void)
{
	__atomic_clear(&s_lock, __ATOMIC_RELEASE);
}

// ── The connection ──────────────────────────────────────────────────────────

static bool read_exact(void *buf, size_t n)
{
	uint8_t *p = (uint8_t *)buf;
	while (n)
	{
		int64_t got = os64_read(0, p, n);
		if (got == OS64_INTERRUPTED)
			continue;
		if (got <= 0)
			return false;
		p += got;
		n -= (size_t)got;
	}
	return true;
}

static bool write_all(const void *buf, size_t n)
{
	const uint8_t *p = (const uint8_t *)buf;
	while (n)
	{
		int64_t wrote = os64_write(1, p, n);
		if (wrote == OS64_INTERRUPTED)
			continue;
		if (wrote <= 0)
			return false;
		p += wrote;
		n -= (size_t)wrote;
	}
	return true;
}

// The outbound thread's buffer: messages are assembled here and go out in
// large writes rather than one per pixel row.
static uint8_t s_out[VNCD_OUT_CAP];
static size_t  s_out_len;

static bool out_flush(void)
{
	bool ok = write_all(s_out, s_out_len);
	s_out_len = 0;
	return ok;
}

static bool out_bytes(const void *p, size_t n)
{
	const uint8_t *b = (const uint8_t *)p;
	while (n)
	{
		if (s_out_len == sizeof(s_out) && !out_flush())
			return false;
		size_t room = sizeof(s_out) - s_out_len;
		size_t take = n < room ? n : room;
		os64_memcpy(s_out + s_out_len, b, take);
		s_out_len += take;
		b += take;
		n -= take;
	}
	return true;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

// ── The screen ──────────────────────────────────────────────────────────────

static int32_t   s_glass = -1;
static uint32_t  s_w, s_h;
static uint32_t *s_shadow;              // the screen, XRGB, as the glass last said
static bool      s_text_vt;             // a text terminal holds the physical screen
static rect_t    s_dirty[VNCD_DIRTY_MAX];
static uint32_t  s_dirty_count;         // the outbound thread's alone
static os64_gui_surface_t s_banner;     // the notice drawn over a text-VT frame

static rect_t rect_union(rect_t a, rect_t b)
{
	int32_t x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
	int32_t x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
	int32_t y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
	return (rect_t){ x0, y0, x1 - x0, y1 - y0 };
}

static bool rect_intersect(rect_t a, rect_t b, rect_t *out)
{
	int32_t x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
	int32_t x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
	int32_t y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
	if (x1 <= x0 || y1 <= y0)
		return false;
	*out = (rect_t){ x0, y0, x1 - x0, y1 - y0 };
	return true;
}

static void dirty_add(rect_t r)
{
	if (r.w <= 0 || r.h <= 0)
		return;
	for (uint32_t i = 0; i < s_dirty_count; i++)
	{
		rect_t both;
		if (rect_intersect(s_dirty[i], r, &both))
		{
			s_dirty[i] = rect_union(s_dirty[i], r);
			return;
		}
	}
	if (s_dirty_count < VNCD_DIRTY_MAX)
	{
		s_dirty[s_dirty_count++] = r;
		return;
	}
	rect_t all = r;
	for (uint32_t i = 0; i < s_dirty_count; i++)
		all = rect_union(all, s_dirty[i]);
	s_dirty[0] = all;
	s_dirty_count = 1;
}

// One record from the glass into the shadow and the dirty list. A change of
// who holds the screen dirties everything: the frame is dimmed and bannered
// while a text terminal has it, and plain again when the desktop does.
static void apply_record(const uint8_t *rec, int64_t n)
{
	os64_glass_rect_t head;
	os64_memcpy(&head, rec, sizeof(head));
	if ((uint64_t)n != sizeof(head) + (uint64_t)head.w * head.h * 4 ||
	    (uint32_t)head.x + head.w > s_w || (uint32_t)head.y + head.h > s_h)
		return;
	const uint8_t *px = rec + sizeof(head);
	for (uint32_t y = 0; y < head.h; y++)
		os64_memcpy(&s_shadow[(size_t)(head.y + y) * s_w + head.x], px + (size_t)y * head.w * 4, (size_t)head.w * 4);
	bool text = (head.flags & OS64_GLASS_TEXT_VT) != 0;
	if (text != s_text_vt)
	{
		s_text_vt = text;
		dirty_add((rect_t){ 0, 0, (int32_t)s_w, (int32_t)s_h });
	}
	dirty_add((rect_t){ head.x, head.y, head.w, head.h });
}

// What the viewer is shown at (x, y): the screen, or while a text terminal
// holds it, the screen dimmed under a one-line notice (phase 1 shows the
// desktop only; REMOTE.md section 3).
static uint32_t presented(uint32_t x, uint32_t y)
{
	if (!s_text_vt)
		return s_shadow[(size_t)y * s_w + x];
	if (y < VNCD_BANNER_H && s_banner.pixels != NULL)
		return s_banner.pixels[(size_t)y * s_banner.pitch_px + x];
	uint32_t p = s_shadow[(size_t)y * s_w + x];
	return (p >> 2) & 0x3F3F3F;
}

static void pixel_out(uint8_t *out, uint32_t v, const vnc_format_t *pf)
{
	uint32_t bytes = pf->bpp / 8;
	for (uint32_t i = 0; i < bytes; i++)
		out[pf->big_endian ? bytes - 1 - i : i] = (uint8_t)(v >> (8 * i));
}

// One Raw rectangle, header and pixels, a row at a time: a row is built
// whole and handed to the output buffer in one call.
static uint8_t *s_row;                   // one screen row at 4 bytes a pixel

static bool send_raw(rect_t r, const vnc_format_t *pf)
{
	uint8_t head[12];
	put16(head, (uint16_t)r.x); put16(head + 2, (uint16_t)r.y);
	put16(head + 4, (uint16_t)r.w); put16(head + 6, (uint16_t)r.h);
	put32(head + 8, 0);                      // encoding 0: Raw
	if (!out_bytes(head, sizeof(head)))
		return false;
	uint32_t bytes = pf->bpp / 8;
	vnc_lut_t lut;
	vnc_lut_build(&lut, pf);
	for (int32_t y = r.y; y < r.y + r.h; y++)
	{
		for (int32_t x = 0; x < r.w; x++)
			pixel_out(s_row + (size_t)x * bytes,
			          vnc_lut_pixel(&lut, presented((uint32_t)(r.x + x), (uint32_t)y)), pf);
		if (!out_bytes(s_row, (size_t)r.w * bytes))
			return false;
	}
	return true;
}

// ── ZRLE (encoding 16): tiles from zrle.c, one zlib stream per connection ──
// RFC 6143 section 7.7.6: every rectangle's tiles go through the SAME zlib
// stream, flushed at the rectangle's end so the viewer's inflate can finish
// it, and the stream's two-byte header goes out once, in the first. A
// rectangle is sent in bands of 64 rows — the tile height, so banding changes
// nothing a viewer decodes — which bounds every buffer below by the width.

static os64_deflate_t *s_zstream;
static bool      s_zheader_sent;
static uint32_t *s_band;                // one band of presented pixels
static uint8_t  *s_tiles;               // its uncompressed tiles
static uint8_t  *s_zout;                // and their compressed bytes
static size_t    s_tiles_cap, s_zout_cap;

static bool zrle_ready(void)
{
	if (s_zstream != NULL)
		return true;
	vnc_format_t widest = { 32, 32, 0, 255, 255, 255, 16, 8, 0 };   // 4 bytes a CPIXEL: the largest
	s_tiles_cap = zrle_bound(s_w, ZRLE_TILE, &widest);
	// Fixed Huffman spends at most nine bits a byte; add block headers, the
	// stream header and a flush's marker.
	s_zout_cap = s_tiles_cap + s_tiles_cap / 8 + (s_tiles_cap / 32768 + 2) * 8 + 64;
	s_zstream = os64_deflate_create();
	s_band = os64_malloc((size_t)s_w * ZRLE_TILE * sizeof(uint32_t));
	s_tiles = os64_malloc(s_tiles_cap);
	s_zout = os64_malloc(s_zout_cap);
	if (!s_zstream || !s_band || !s_tiles || !s_zout)
	{
		log_line("vncd: out of memory for ZRLE");
		return false;
	}
	return true;
}

static bool send_zrle(rect_t r, const vnc_format_t *pf)
{
	for (int32_t y = 0; y < r.h; y++)
		for (int32_t x = 0; x < r.w; x++)
			s_band[(size_t)y * (size_t)r.w + (size_t)x] = presented((uint32_t)(r.x + x), (uint32_t)(r.y + y));
	size_t n = zrle_tiles(s_band, (size_t)r.w, (uint32_t)r.w, (uint32_t)r.h, pf, s_tiles);

	uint8_t *out = s_zout;
	size_t room = s_zout_cap;
	if (!s_zheader_sent)
	{
		out[0] = 0x78;                       // deflate, 32 KiB window
		out[1] = 0x01;                       // no dictionary; CMF*256+FLG is a multiple of 31
		out += 2;
		room -= 2;
		s_zheader_sent = true;
	}
	const uint8_t *in = s_tiles;
	size_t in_left = n;
	while (in_left != 0)
		if (os64_deflate_process(s_zstream, &in, &in_left, &out, &room, false) != OS64_DEFLATE_NEED_INPUT)
		{
			log_line("vncd: ZRLE compression outgrew its buffer");
			return false;
		}
	if (os64_deflate_flush(s_zstream, &out, &room) != OS64_DEFLATE_NEED_INPUT)
	{
		log_line("vncd: ZRLE flush outgrew its buffer");
		return false;
	}
	size_t zlen = (size_t)(out - s_zout);

	uint8_t head[16];
	put16(head, (uint16_t)r.x); put16(head + 2, (uint16_t)r.y);
	put16(head + 4, (uint16_t)r.w); put16(head + 6, (uint16_t)r.h);
	put32(head + 8, 16);                     // encoding 16: ZRLE
	put32(head + 12, (uint32_t)zlen);
	return out_bytes(head, sizeof(head)) && out_bytes(s_zout, zlen);
}

// Answer the outstanding request with whatever of the dirty list falls in
// its rectangle. Returns false when the connection is gone.
static bool answer(rect_t want, const vnc_format_t *pf, bool zrle)
{
	rect_t send[VNCD_DIRTY_MAX];
	uint32_t n = 0;
	for (uint32_t i = 0; i < s_dirty_count; i++)
		if (rect_intersect(s_dirty[i], want, &send[n]))
			n++;
	// A dirty rectangle wholly inside the request is paid; one that
	// straddles it stays for a later request that covers the rest.
	uint32_t kept = 0;
	for (uint32_t i = 0; i < s_dirty_count; i++)
	{
		rect_t both;
		bool inside = rect_intersect(s_dirty[i], want, &both) &&
		              both.w == s_dirty[i].w && both.h == s_dirty[i].h;
		if (!inside)
			s_dirty[kept++] = s_dirty[i];
	}
	s_dirty_count = kept;

	if (zrle && !zrle_ready())
		return false;
	// Under ZRLE each rectangle goes as its bands, and the count says so.
	uint32_t count = 0;
	for (uint32_t i = 0; i < n; i++)
		count += zrle ? (uint32_t)((send[i].h + ZRLE_TILE - 1) / ZRLE_TILE) : 1;
	uint8_t head[4] = { 0, 0, 0, 0 };        // FramebufferUpdate
	put16(head + 2, (uint16_t)count);
	if (!out_bytes(head, sizeof(head)))
		return false;
	for (uint32_t i = 0; i < n; i++)
	{
		if (!zrle)
		{
			if (!send_raw(send[i], pf))
				return false;
			continue;
		}
		for (int32_t y = 0; y < send[i].h; y += ZRLE_TILE)
		{
			rect_t band = { send[i].x, send[i].y + y, send[i].w,
			                send[i].h - y < ZRLE_TILE ? send[i].h - y : ZRLE_TILE };
			if (!send_zrle(band, pf))
				return false;
		}
	}
	return out_flush();
}

// The outbound thread: keep the shadow current, answer requests.
static void outbound(void)
{
	static uint8_t rec[1024 * 1024];
	while (!__atomic_load_n(&s_quit, __ATOMIC_ACQUIRE))
	{
		// A short patience, so a request that arrives while the screen is
		// still is answered within a couple of ticks; then take what else
		// the glass already holds without waiting — up to a BUDGET. While a
		// window is dragged the compositor adds a change every frame, so a
		// drain that runs "until empty" never ends, and the viewer is never
		// answered until the drag stops (the P5, 2026-09-23: seconds of
		// nothing, then everything). What the budget leaves stays in the
		// glass and is taken on the next turn; the shadow is eventually
		// exact either way.
		int64_t n = os64_read_for(s_glass, rec, sizeof(rec), 20);
		size_t taken = 0;
		while (n > 0)
		{
			apply_record(rec, n);
			taken += (size_t)n;
			if (taken >= VNCD_DRAIN_BUDGET)
				break;
			n = os64_read_for(s_glass, rec, sizeof(rec), 0);
		}
		if (n <= 0 && n != OS64_ERR_TIMEOUT && n != OS64_INTERRUPTED)
		{
			log_line("vncd: the glass closed under the session");
			break;
		}

		lock();
		bool want = s_request;
		bool full = s_request_full;
		rect_t r = s_request_rect;
		vnc_format_t pf = s_format;
		bool zrle = s_zrle;
		unlock();
		if (!want)
			continue;
		if (full)
			dirty_add(r);
		bool pending = false;
		for (uint32_t i = 0; i < s_dirty_count && !pending; i++)
		{
			rect_t both;
			pending = rect_intersect(s_dirty[i], r, &both);
		}
		if (!pending)
			continue;                        // incremental and nothing changed: wait
		lock();
		s_request = false;
		s_request_full = false;
		unlock();
		if (!answer(r, &pf, zrle))
			break;
	}
	__atomic_store_n(&s_quit, true, __ATOMIC_RELEASE);
}

// ── Input: keysyms and the pointer ──────────────────────────────────────────

// X11 keysyms (the RFB key vocabulary, RFC 6143 section 7.5.4) to HID usages,
// US layout — os64's one keyboard layout. `shifted` says the keysym is the
// shifted face of its key: '!' is Shift+1 whatever the viewer said about
// Shift, which is how a viewer whose own layout differs still types the
// character it showed.
typedef struct { uint8_t usage; bool shifted; } vnc_key_t;

static bool keysym_key(uint32_t sym, vnc_key_t *k)
{
	if (sym >= 'a' && sym <= 'z') { *k = (vnc_key_t){ (uint8_t)(0x04 + sym - 'a'), false }; return true; }
	if (sym >= 'A' && sym <= 'Z') { *k = (vnc_key_t){ (uint8_t)(0x04 + sym - 'A'), true }; return true; }
	if (sym >= '1' && sym <= '9') { *k = (vnc_key_t){ (uint8_t)(0x1E + sym - '1'), false }; return true; }
	if (sym == '0') { *k = (vnc_key_t){ 0x27, false }; return true; }
	static const struct { char c; uint8_t usage; bool shifted; } punct[] = {
		{ ' ', 0x2C, false }, { '-', 0x2D, false }, { '=', 0x2E, false }, { '[', 0x2F, false },
		{ ']', 0x30, false }, { '\\', 0x31, false }, { ';', 0x33, false }, { '\'', 0x34, false },
		{ '`', 0x35, false }, { ',', 0x36, false }, { '.', 0x37, false }, { '/', 0x38, false },
		{ '!', 0x1E, true }, { '@', 0x1F, true }, { '#', 0x20, true }, { '$', 0x21, true },
		{ '%', 0x22, true }, { '^', 0x23, true }, { '&', 0x24, true }, { '*', 0x25, true },
		{ '(', 0x26, true }, { ')', 0x27, true }, { '_', 0x2D, true }, { '+', 0x2E, true },
		{ '{', 0x2F, true }, { '}', 0x30, true }, { '|', 0x31, true }, { ':', 0x33, true },
		{ '"', 0x34, true }, { '~', 0x35, true }, { '<', 0x36, true }, { '>', 0x37, true },
		{ '?', 0x38, true },
	};
	for (unsigned i = 0; i < sizeof(punct) / sizeof(punct[0]); i++)
		if (sym == (uint32_t)(unsigned char)punct[i].c)
		{
			*k = (vnc_key_t){ punct[i].usage, punct[i].shifted };
			return true;
		}
	static const struct { uint32_t sym; uint8_t usage; } special[] = {
		{ 0xFF0D, 0x28 }, { 0xFF8D, 0x28 },                  // Return, KP_Enter
		{ 0xFF1B, 0x29 }, { 0xFF08, 0x2A }, { 0xFF09, 0x2B }, // Escape, BackSpace, Tab
		{ 0xFFE5, 0x39 },                                    // Caps_Lock
		{ 0xFF63, 0x49 }, { 0xFF50, 0x4A }, { 0xFF55, 0x4B }, // Insert, Home, Page_Up
		{ 0xFFFF, 0x4C }, { 0xFF57, 0x4D }, { 0xFF56, 0x4E }, // Delete, End, Page_Down
		{ 0xFF53, 0x4F }, { 0xFF51, 0x50 }, { 0xFF54, 0x51 }, { 0xFF52, 0x52 },  // arrows
	};
	for (unsigned i = 0; i < sizeof(special) / sizeof(special[0]); i++)
		if (sym == special[i].sym)
		{
			*k = (vnc_key_t){ special[i].usage, false };
			return true;
		}
	if (sym == 0xFE20) { *k = (vnc_key_t){ 0x2B, true }; return true; }                        // ISO_Left_Tab: Shift+Tab
	if (sym >= 0xFFBE && sym <= 0xFFC9) { *k = (vnc_key_t){ (uint8_t)(0x3A + sym - 0xFFBE), false }; return true; }  // F1-F12
	if (sym >= 0xFFB0 && sym <= 0xFFB9)                                                  // the keypad's digits, as digits
		return keysym_key(sym == 0xFFB0 ? '0' : '1' + (sym - 0xFFB1), k);
	return false;
}

// The modifier keysyms, as HID modifier bits.
static uint8_t keysym_modifier(uint32_t sym)
{
	switch (sym)
	{
		case 0xFFE3: return 0x01;   // Control_L
		case 0xFFE1: return 0x02;   // Shift_L
		case 0xFFE9: return 0x04;   // Alt_L
		case 0xFFE7: return 0x04;   // Meta_L: some viewers send Alt as Meta
		case 0xFFEB: return 0x08;   // Super_L
		case 0xFFE4: return 0x10;   // Control_R
		case 0xFFE2: return 0x20;   // Shift_R
		case 0xFFEA: return 0x40;   // Alt_R
		case 0xFFE8: return 0x40;   // Meta_R
		case 0xFFEC: return 0x80;   // Super_R
		default:     return 0;
	}
}

// The glass keyboard's state: the modifier keys held, and up to six other
// keys, each remembering whether it wants Shift. Only the inbound thread
// touches it.
static uint8_t s_mods;
static vnc_key_t   s_held[6];
static uint8_t s_held_count;

static void send_report(void)
{
	os64_glass_keyboard_t k = { .kind = OS64_GLASS_KEYBOARD };
	k.report[0] = s_mods;
	for (uint8_t i = 0; i < s_held_count; i++)
	{
		k.report[2 + i] = s_held[i].usage;
		if (s_held[i].shifted)
			k.report[0] |= 0x02;
	}
	os64_write(s_glass, &k, sizeof(k));
}

static void key_event(bool down, uint32_t sym)
{
	uint8_t mod = keysym_modifier(sym);
	if (mod)
	{
		s_mods = down ? (uint8_t)(s_mods | mod) : (uint8_t)(s_mods & ~mod);
		send_report();
		return;
	}
	vnc_key_t k;
	if (!keysym_key(sym, &k))
	{
		char note[80];
		os64_snprintf(note, sizeof(note), "vncd: keysym 0x%x has no key on os64's layout", sym);
		log_line(note);
		return;
	}
	int at = -1;
	for (uint8_t i = 0; i < s_held_count; i++)
		if (s_held[i].usage == k.usage)
			at = i;
	if (down)
	{
		// A repeat of a key already down is the viewer's autorepeat: the
		// kernel repeats a held key itself, as it does a USB keyboard's.
		if (at >= 0 || s_held_count == 6)
			return;
		s_held[s_held_count++] = k;
	}
	else
	{
		if (at < 0)
			return;
		s_held[at] = s_held[--s_held_count];
	}
	send_report();
}

// RFB buttons: 1 left, 2 middle, 4 right, 8 and 16 the wheel. The glass's:
// bit 0 left, bit 1 right, bit 2 middle, and no wheel (DEBTS).
static void pointer_event(uint8_t mask, uint16_t x, uint16_t y)
{
	os64_glass_pointer_t m = { .kind = OS64_GLASS_POINTER };
	m.buttons = (uint8_t)((mask & 1) | ((mask & 4) ? 2 : 0) | ((mask & 2) ? 4 : 0));
	m.x = x < s_w ? x : (uint16_t)(s_w - 1);
	m.y = y < s_h ? y : (uint16_t)(s_h - 1);
	os64_write(s_glass, &m, sizeof(m));
}

// ── The inbound thread: the viewer's messages ───────────────────────────────

static bool set_pixel_format(const uint8_t *pf)
{
	vnc_format_t f = {
		.bpp = pf[0], .depth = pf[1], .big_endian = pf[2] != 0,
		.rmax = get16(pf + 4), .gmax = get16(pf + 6), .bmax = get16(pf + 8),
		.rshift = pf[10], .gshift = pf[11], .bshift = pf[12],
	};
	// A colour map (true-colour 0) would need SetColourMapEntries, which is
	// not built; RFB 3.8 gives a server no way to refuse the format, so the
	// session ends and says why.
	if (!pf[3] || (f.bpp != 8 && f.bpp != 16 && f.bpp != 32) ||
	    !f.rmax || !f.gmax || !f.bmax || f.rshift > 31 || f.gshift > 31 || f.bshift > 31)
	{
		log_line("vncd: the viewer asked for a pixel format vncd does not speak (colour-mapped?)");
		return false;
	}
	lock();
	s_format = f;
	unlock();
	return true;
}

static int64_t inbound(void *unused)
{
	(void)unused;
	uint8_t type;
	while (read_exact(&type, 1))
	{
		uint8_t m[20];
		switch (type)
		{
			case 0:                          // SetPixelFormat
				if (!read_exact(m, 19) || !set_pixel_format(m + 3))
					goto done;
				break;
			case 2:                          // SetEncodings
			{
				// The viewer's list is in its order of preference: the first
				// of ZRLE and Raw it names is the one it gets. Raw is always
				// spoken, named or not.
				if (!read_exact(m, 3))
					goto done;
				uint16_t count = get16(m + 1);
				bool chosen = false, zrle = false;
				for (uint16_t i = 0; i < count; i++)
				{
					if (!read_exact(m, 4))
						goto done;
					int32_t e = (int32_t)get32(m);
					if (!chosen && (e == 16 || e == 0))
					{
						chosen = true;
						zrle = e == 16;
					}
				}
				lock();
				s_zrle = zrle;
				unlock();
				break;
			}
			case 3:                          // FramebufferUpdateRequest
			{
				if (!read_exact(m, 9))
					goto done;
				rect_t r = { get16(m + 1), get16(m + 3), get16(m + 5), get16(m + 7) };
				rect_t screen = { 0, 0, (int32_t)s_w, (int32_t)s_h }, clipped;
				if (!rect_intersect(r, screen, &clipped))
					clipped = screen;
				lock();
				s_request = true;
				s_request_full = s_request_full || m[0] == 0;
				s_request_rect = clipped;
				unlock();
				break;
			}
			case 4:                          // KeyEvent
				if (!read_exact(m, 7))
					goto done;
				key_event(m[0] != 0, get32(m + 3));
				break;
			case 5:                          // PointerEvent
				if (!read_exact(m, 5))
					goto done;
				pointer_event(m[0], get16(m + 1), get16(m + 3));
				break;
			case 6:                          // ClientCutText: not yet (DEBTS)
			{
				if (!read_exact(m, 7))
					goto done;
				uint32_t len = get32(m + 3);
				if (len > VNCD_CUT_TEXT_MAX)
					goto done;
				while (len)
				{
					uint32_t take = len < sizeof(m) ? len : sizeof(m);
					if (!read_exact(m, take))
						goto done;
					len -= take;
				}
				break;
			}
			default:
			{
				// RFB messages carry no length, so an unknown one cannot be
				// skipped: the rest of the stream is unreadable.
				char note[80];
				os64_snprintf(note, sizeof(note), "vncd: unknown message type %u; ending the session", type);
				log_line(note);
				goto done;
			}
		}
	}
done:
	__atomic_store_n(&s_quit, true, __ATOMIC_RELEASE);
	return 0;
}

// ── The handshake ───────────────────────────────────────────────────────────

// Refuse a viewer during the handshake, in its version's words.
static void refuse(int minor, const char *why)
{
	uint8_t head[4];
	uint32_t len = (uint32_t)os64_strlen(why);
	if (minor >= 7)
	{
		uint8_t none = 0;                    // zero security types, then the reason
		write_all(&none, 1);
	}
	else
	{
		put32(head, 0);                      // 3.3: security type 0 is failure
		write_all(head, 4);
	}
	put32(head, len);
	write_all(head, 4);
	write_all(why, len);
	log_line(why);
}

static int session(void)
{
	if (!write_all("RFB 003.008\n", 12))
		return 1;
	char version[13] = { 0 };
	if (!read_exact(version, 12) || os64_strlen(version) != 12 ||
	    !os64_glob_match("RFB 003.[0-9][0-9][0-9]\n", version))
	{
		log_line("vncd: not an RFB viewer");
		return 1;
	}
	int minor = (version[8] - '0') * 100 + (version[9] - '0') * 10 + (version[10] - '0');
	if (minor < 3)
	{
		log_line("vncd: an RFB version older than 3.3");
		return 1;
	}
	// 3.3 and 3.7 have their own handshakes; anything else newer than 3.3
	// is answered by 3.8's rules, which is what the RFC asks (section 7.1.1).
	if (minor != 3 && minor != 7)
		minor = 8;

	int64_t g = os64_open("/dev/glass", "u");
	if (g < 0)
	{
		refuse(minor, "vncd: no desktop on this boot (the GUI is not running)");
		return 1;
	}
	s_glass = (int32_t)g;
	if (os64_gui_screen_info(&s_w, &s_h) != 0 || !s_w || !s_h)
	{
		refuse(minor, "vncd: the desktop did not say its size");
		return 1;
	}

	// Security: None, safe because of the listener's address (see the top).
	if (minor == 3)
	{
		uint8_t type[4];
		put32(type, 1);
		if (!write_all(type, 4))
			return 1;
	}
	else
	{
		uint8_t offer[2] = { 1, 1 };
		uint8_t chosen;
		if (!write_all(offer, 2) || !read_exact(&chosen, 1))
			return 1;
		if (chosen != 1)
		{
			if (minor == 8)
			{
				const char why[] = "vncd offers security type None only";
				uint8_t result[8];
				put32(result, 1);
				put32(result + 4, sizeof(why) - 1);
				write_all(result, 8);
				write_all(why, sizeof(why) - 1);
			}
			return 1;
		}
		if (minor == 8)
		{
			uint8_t ok[4] = { 0, 0, 0, 0 };
			if (!write_all(ok, 4))
				return 1;
		}
	}

	uint8_t shared;
	if (!read_exact(&shared, 1))             // every viewer is shared: ignored
		return 1;

	s_shadow = os64_calloc((size_t)s_w * s_h, sizeof(uint32_t));
	s_row = os64_malloc((size_t)s_w * 4);
	s_banner = (os64_gui_surface_t){ os64_calloc((size_t)s_w * VNCD_BANNER_H, sizeof(uint32_t)),
	                                 s_w, VNCD_BANNER_H, s_w };
	if (s_shadow == NULL || s_row == NULL || s_banner.pixels == NULL)
		return 1;
	const char notice[] = "A text terminal has the screen. Alt+F8 brings the desktop back.";
	for (size_t i = 0; i < (size_t)s_w * VNCD_BANNER_H; i++)
		s_banner.pixels[i] = 0x00202860;
	os64_draw_text(&s_banner, 8, 2, notice, sizeof(notice) - 1, 0x00FFFFFF, 0x00202860);

	// ServerInit: size, native pixel format, name.
	const char *host = os64_getenv("HOSTNAME");
	char name[96];
	os64_snprintf(name, sizeof(name), "os64 on %s", host && *host ? host : "os64");
	uint8_t init[24] = { 0 };
	put16(init, (uint16_t)s_w);
	put16(init + 2, (uint16_t)s_h);
	init[4] = 32; init[5] = 24; init[6] = 0; init[7] = 1;   // bpp, depth, big-endian, true colour
	put16(init + 8, 255); put16(init + 10, 255); put16(init + 12, 255);
	init[14] = 16; init[15] = 8; init[16] = 0;              // shifts
	put32(init + 20, (uint32_t)os64_strlen(name));
	if (!write_all(init, sizeof(init)) || !write_all(name, os64_strlen(name)))
		return 1;

	char note[96];
	os64_snprintf(note, sizeof(note), "vncd: a viewer connected (RFB 3.%d, %ux%u)", minor, s_w, s_h);
	log_line(note);
	int64_t in = os64_thread(inbound, 0);
	if (in < 0)
		return 1;
	outbound();
	// Closing the glass lifts every key and button the viewer held
	// (os64/glass.h); the rest of the session goes with the process.
	os64_close(s_glass);
	log_line("vncd: the viewer left");
	return 0;
}

// ── The listener ────────────────────────────────────────────────────────────

static bool port_setting(const char *name, const char *value, void *user)
{
	uint32_t *port = (uint32_t *)user;
	if (!name || !os64_streq(name, "port")) { *port = 0; return false; }
	uint32_t p = 0;
	if (!*value) { *port = 0; return false; }
	for (const char *c = value; *c; c++)
	{
		if (*c < '0' || *c > '9' || p > 6553) { *port = 0; return false; }
		p = p * 10 + (uint32_t)(*c - '0');
	}
	*port = p <= 65535 ? p : 0;
	return *port != 0;
}

// sshd's rule: only a vncd.conf the ladder does not name permits the
// default; one it names but cannot read is the operator's word unread.
static uint32_t configured_port(void)
{
	uint32_t port = VNCD_PORT_DEFAULT;
	char path[OS64_CONF_PATH_MAX];
	bool found = os64_conf_find("vncd.conf", path, sizeof(path)) == 0;
	int64_t rc = found ? os64_conf_read(path, port_setting, &port) : OS64_CONF_NO_FILE;
	if (found && rc == OS64_CONF_NO_FILE) { log_line("vncd: vncd.conf found but unreadable; startup refused"); return 0; }
	if (!port || (found && rc < 0)) { log_line("vncd: invalid vncd.conf"); return 0; }
	return port;
}

int main(int argc, char **argv)
{
	if (argc == 2 && os64_streq(argv[1], "-session"))
		return session();
	if (argc != 1)
	{
		log_line("usage: vncd (port in vncd.conf; loopback only)");
		return 2;
	}
	uint32_t port = configured_port();
	if (!port)
		return 2;
	char dial[48];
	os64_snprintf(dial, sizeof(dial), "tcp!127.0.0.1!%u", port);
	int64_t listener = os64_announce(dial);
	if (listener < 0)
	{
		char why[160];
		os64_snprintf(why, sizeof(why), "vncd: cannot listen on %s: %s", dial, os64_dial_reason(listener));
		log_line(why);
		return 1;
	}
	char message[96];
	os64_snprintf(message, sizeof(message), "vncd: listening on 127.0.0.1:%u (reach it with ssh -L)", port);
	log_line(message);

	unsigned sessions = 0;
	for (;;)
	{
		while (os64_reap(0) > 0)
			if (sessions)
				sessions--;
		os64_netconn_t conn;
		int64_t n = os64_read_for((int32_t)listener, &conn, sizeof(conn), 1000);
		if (n == OS64_ERR_TIMEOUT || n == OS64_INTERRUPTED)
			continue;
		if (n != (int64_t)sizeof(conn))
			break;
		// Each viewer holds a copy of the screen: a small cap, rather than
		// as many as ask.
		if (sessions < VNCD_SESSIONS_MAX)
		{
			char *args[] = { "/bin/vncd", "-session", 0 };
			if (os64_spawn_redirected("/bin/vncd", args, conn.handle, conn.handle, -1, 0) >= 0)
				sessions++;
		}
		os64_close(conn.handle);
	}
	os64_close((int32_t)listener);
	return 1;
}
