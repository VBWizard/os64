// desktop.c — the desktop background, from a config file (2026-08-23).
//
// The bottom layer of the scene was a test pattern from the day the surface
// core came up: four clipping rectangles, a crosshair and a banner, painted
// once by the compositor at start. It proved the clipper; it was never meant
// to be looked at all day. This file gives the desktop to the USER, the same
// way the log format and the shell's startup went to the user before it:
//
//   desktop.conf, found wherever the CONFIG SEARCH PATH says (conf.h) — by
//   default /home then /etc, first hit wins, the persistence gradient every
//   config file in os64 climbs (/home survives a rebuild; /etc is the
//   system's and does not). This file used to hardcode that pair; it was the
//   sixth reader to do so, and the one that made Chris rule the search path
//   into existence the same afternoon. /sys/conf and the DEBUG_BOOT log both
//   say which copy actually answered.
//
//   color = 0x2a5566        the fill, XRGB in hex (the # is the comment
//                           character in every os64 config, so no CSS spelling)
//   image = /home/wall.ppm  optional; a binary PPM (P6, 8-bit) drawn
//                           CENTERED over the color, never scaled — the
//                           kernel owns no scaler and a wallpaper is not the
//                           reason to write one. Smaller than the screen gets
//                           a colored frame; larger gets cropped around its
//                           middle.
//
// PPM because os64 already speaks it: every QEMU `screendump` the harness
// takes is a P6, so a screenshot of the desktop can become the desktop with
// no tool in between — and the format is three tokens of header followed by
// the bytes, which is the right amount of image format for a kernel.
//
// No config file at all means the test pattern stays (paint_desktop in
// compositor.c) — a fresh install looks exactly as it did, and the pattern
// keeps proving the clipper on every boot that nobody has personalized.
//
// Runs in the compositor's own task at startup — kernel context, VFS
// available, before the first frame — and never again: the desktop is
// painted ONCE into kDesktop and composited from there. A change to the
// file takes effect at the next boot. (Re-reading on a signal is a fine
// slice for another day; nothing here prevents it.)

#include "gui/desktop.h"
#include "gui/surface.h"

#include "CONFIG.h"
#include "driver/filesystem/vfs/vfs.h"
#include "kmalloc.h"
#include "memory/vma.h"   // call_in_kernel_context — disk I/O from a task's address space
#include "conf.h"          // conf_find — the search path, no longer this file's business
#include "printd.h"
#include "strings/strcmp.h"
#include "strings/strlen.h"
#include "strings/strcpy.h"

// ── a tiny file reader ──────────────────────────────────────────────────────
// Whole file into a kmalloc'd buffer, NUL-terminated for the text case.
// Returns NULL if the file is absent or larger than `cap`. Mount-routed
// like every other kernel-side open (the ELF loader's idiom).
//
// IN KERNEL CONTEXT, via call_in_kernel_context — learned on the first boot
// (2026-08-23): the compositor is a TASK with its own address space (ONE
// TASK, ONE ADDRESS SPACE), and the NVMe doorbell is MMIO mapped only in
// kKernelPML4, so the first read page-faulted in nvme_ring_doorbell from
// guicomp. The shared-object page reader takes the same trampoline for the
// same reason. Params live in a kmalloc'd block (HHDM — visible under any
// CR3), never on this stack, per the trampoline's contract.
typedef struct {
	char        path[256];   // a COPY: the caller's string may be on the task stack, which kKernelPML4 does not map
	size_t      cap;
	uint8_t    *buf;     // out: kmalloc'd, cap+1 bytes, NUL-terminated
	size_t      len;     // out
} read_whole_file_params_t;

static void read_whole_file_kernel(void *arg)
{
	read_whole_file_params_t *p = (read_whole_file_params_t *)arg;
	p->buf = NULL;
	p->len = 0;

	const char *tail = NULL;
	vfs_filesystem_t *fs = vfs_resolve_mount(p->path, &tail);
	if (fs == NULL || fs->fops == NULL || fs->fops->open == NULL || fs->fops->read == NULL)
		return;
	vfs_file_t *file = NULL;
	if (fs->fops->open(&file, tail, "r", fs) != 0)
		return;

	uint8_t *buf = kmalloc(p->cap + 1);
	size_t len = 0;
	if (buf != NULL) {
		// Read until short: the fs tells us the true length by handing back
		// less than we asked for. A file exactly at `cap` bytes reads full
		// and is refused on the next pass, which is the honest outcome.
		for (;;) {
			if (len >= p->cap) { kfree(buf); buf = NULL; break; }
			int n = fs->fops->read(file, buf + len, p->cap - len);
			if (n <= 0) break;
			len += (size_t)n;
		}
	}
	if (fs->fops->close != NULL)
		fs->fops->close(file);
	if (buf == NULL)
		return;
	buf[len] = 0;
	p->buf = buf;
	p->len = len;
}

static uint8_t *read_whole_file(const char *path, size_t cap, size_t *out_len)
{
	read_whole_file_params_t *p = kmalloc(sizeof(*p));
	if (p == NULL)
		return NULL;
	strncpy(p->path, path, sizeof(p->path));
	p->path[sizeof(p->path) - 1] = 0;
	p->cap = cap;
	call_in_kernel_context(read_whole_file_kernel, p);
	uint8_t *buf = p->buf;
	*out_len = p->len;
	kfree(p);
	return buf;
}

// ── config parsing ──────────────────────────────────────────────────────────
// `key = value`, `#` comments, whitespace tolerant — logd.conf's grammar.
static bool parse_hex_color(const char *s, uint32_t *out)
{
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	uint32_t v = 0;
	int digits = 0;
	for (; *s && *s != ' ' && *s != '\t' && *s != '\r'; s++, digits++) {
		char c = *s;
		uint32_t d;
		if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
		else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
		else return false;
		v = (v << 4) | d;
	}
	if (digits != 6)
		return false;
	*out = 0xff000000u | v;
	return true;
}

static void parse_config(char *text, desktop_config_t *cfg)
{
	char *line = text;
	while (*line) {
		char *next = line;
		while (*next && *next != '\n') next++;
		if (*next) *next++ = 0;

		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || *p == 0) { line = next; continue; }
		char *key = p;
		while (*p && *p != ' ' && *p != '\t' && *p != '=') p++;
		char *key_end = p;
		while (*p == ' ' || *p == '\t') p++;
		if (*p != '=') {
			printd(DEBUG_GUI, "desktop: ignoring a line with no '=' (\"%s\")\n", line);
			line = next; continue;
		}
		p++;
		while (*p == ' ' || *p == '\t') p++;
		char *val = p;
		// trim trailing whitespace/CR
		char *e = val + strlen(val);
		while (e > val && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
		*key_end = 0;

		if (strcmp(key, "color") == 0) {
			if (!parse_hex_color(val, &cfg->color))
				printd(DEBUG_GUI, "desktop: color wants six hex digits, got \"%s\" — keeping the default\n", val);
		} else if (strcmp(key, "image") == 0) {
			size_t n = strlen(val);
			if (n == 0 || n >= sizeof(cfg->image))
				printd(DEBUG_GUI, "desktop: image path empty or too long — ignored\n");
			else
				strcpy(cfg->image, val);
		} else {
			printd(DEBUG_GUI, "desktop: unknown key \"%s\" — ignored\n", key);
		}
		line = next;
	}
}

// ── PPM (P6) decoding ───────────────────────────────────────────────────────
// Header: "P6" <ws> width <ws> height <ws> maxval <one ws> then width*height
// RGB triples. Comments (# to end of line) are legal anywhere in the header
// and GIMP writes one, so they are skipped. Only maxval 255 is accepted —
// the 16-bit flavour is rare and not worth a second code path.
static bool ppm_skip_ws(const uint8_t *d, size_t len, size_t *i)
{
	for (;;) {
		while (*i < len && (d[*i] == ' ' || d[*i] == '\t' || d[*i] == '\r' || d[*i] == '\n')) (*i)++;
		if (*i < len && d[*i] == '#') {
			while (*i < len && d[*i] != '\n') (*i)++;
			continue;
		}
		return *i < len;
	}
}

static bool ppm_read_uint(const uint8_t *d, size_t len, size_t *i, uint32_t *out)
{
	if (!ppm_skip_ws(d, len, i)) return false;
	uint32_t v = 0; int digits = 0;
	while (*i < len && d[*i] >= '0' && d[*i] <= '9') { v = v * 10 + (uint32_t)(d[*i] - '0'); (*i)++; digits++; }
	if (digits == 0 || v > 8192) return false;
	*out = v;
	return true;
}

// Draw the image centered on `desk`. Returns false (desk untouched) on any
// malformed header, and says why — a wallpaper that silently fails to appear
// is a config bug with no handle on it.
static bool ppm_draw_centered(surface_t *desk, const uint8_t *d, size_t len)
{
	size_t i = 0;
	if (len < 2 || d[0] != 'P' || d[1] != '6') {
		printd(DEBUG_GUI, "desktop: image is not a binary PPM (P6)\n");
		return false;
	}
	i = 2;
	uint32_t w, h, maxval;
	if (!ppm_read_uint(d, len, &i, &w) || !ppm_read_uint(d, len, &i, &h) ||
	    !ppm_read_uint(d, len, &i, &maxval) || maxval != 255 || w == 0 || h == 0) {
		printd(DEBUG_GUI, "desktop: PPM header unreadable, or maxval is not 255\n");
		return false;
	}
	i++;   // exactly one whitespace byte after maxval, by the spec
	if (len - i < (size_t)w * h * 3) {
		printd(DEBUG_GUI, "desktop: PPM is truncated (%lux%lu wants %lu bytes, has %lu)\n",
		       (uint64_t)w, (uint64_t)h, (uint64_t)w * h * 3, (uint64_t)(len - i));
		return false;
	}

	// Center, cropping a larger image around its middle.
	int32_t dx = ((int32_t)desk->width  - (int32_t)w) / 2;
	int32_t dy = ((int32_t)desk->height - (int32_t)h) / 2;
	for (uint32_t y = 0; y < h; y++) {
		int32_t sy = dy + (int32_t)y;
		if (sy < 0 || sy >= (int32_t)desk->height) continue;
		const uint8_t *row = d + i + (size_t)y * w * 3;
		uint32_t *out = desk->pixels + (size_t)sy * desk->pitch_px;
		for (uint32_t x = 0; x < w; x++) {
			int32_t sx = dx + (int32_t)x;
			if (sx < 0 || sx >= (int32_t)desk->width) continue;
			const uint8_t *px = row + (size_t)x * 3;
			out[sx] = 0xff000000u | ((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2];
		}
	}
	printd(DEBUG_GUI, "desktop: image %lux%lu drawn at (%d,%d)\n", (uint64_t)w, (uint64_t)h, dx, dy);
	return true;
}

// ── the entry point ─────────────────────────────────────────────────────────
bool desktop_paint_from_config(surface_t *desk)
{
	desktop_config_t cfg = { .color = DESKTOP_DEFAULT_COLOR, .image = "" };

	// THE LADDER IS NOT OURS ANY MORE (2026-08-23). This used to hold its own
	// { "/home/desktop.conf", "/etc/desktop.conf" } — the sixth private copy of
	// the same sequence in the system, and the one whose arrival made Chris
	// rule the search path into existence. conf_find walks whatever
	// /etc/os64.conf says and announces which file won at DEBUG_BOOT, so the
	// "which config am I actually getting" question is answered in the log and
	// in /sys/conf rather than by reading this function.
	char found[CONF_PATH_MAX];
	if (!conf_find("desktop.conf", found, sizeof(found)))
		return false;   // no config anywhere: the caller keeps its test pattern

	size_t len = 0;
	uint8_t *text = read_whole_file(found, DESKTOP_CONF_MAX, &len);
	if (text == NULL)
		return false;   // it opened for the walker and not for us: over the cap, or gone since

	parse_config((char *)text, &cfg);
	kfree(text);
	printd(DEBUG_GUI, "desktop: %s — color 0x%06x%s%s\n", found, cfg.color & 0xffffff,
	       cfg.image[0] ? ", image " : "", cfg.image);

	surface_fill_rect(desk, (rect_t){0, 0, (int32_t)desk->width, (int32_t)desk->height}, cfg.color);

	if (cfg.image[0]) {
		size_t ilen = 0;
		uint8_t *img = read_whole_file(cfg.image, DESKTOP_IMAGE_MAX, &ilen);
		if (img == NULL)
			printd(DEBUG_GUI, "desktop: cannot read image %s (missing, or over %lu bytes)\n",
			       cfg.image, (uint64_t)DESKTOP_IMAGE_MAX);
		else {
			ppm_draw_centered(desk, img, ilen);
			kfree(img);
		}
	}
	return true;
}
