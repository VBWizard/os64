#include "os64/ui.h"
#include "ui_internal.h"
#include "os64/conf.h"
#include "os64/str.h"
#include "os64/fmt.h"

// ── the theme ───────────────────────────────────────────────────────────────

void os64_ui_theme_defaults(os64_ui_theme_t *t)
{
	// Compiled defaults are the fallback when no usable startup theme exists.
	t->panel_bg            = OS64_GUI_COLOR_LIGHT_GRAY;
	t->panel_border        = OS64_GUI_COLOR_DARK_GRAY;
	t->label_fg            = OS64_GUI_COLOR_BLACK;
	t->button_face         = 0xff2a62b8u;   // the titlebar-focused blue
	t->button_face_pressed = 0xff1c4380u;   // same hue, pressed down a stop
	t->button_border       = OS64_GUI_COLOR_DARK_GRAY;
	t->button_fg           = OS64_GUI_COLOR_WHITE;
	t->button_highlight    = 0xff7199d4u;
	t->button_shadow       = 0xff163968u;
	t->button_face_hover   = 0xff3975cdu;
	t->hover_border        = 0xff7199d4u;
	t->focus_ring          = 0xffe6b765u;
	t->disabled_bg         = 0xffb8b8b8u;
	t->disabled_fg         = 0xff666666u;

	// The text family: gkeys' warm paper, ink on it, and the house blue
	// inverted for selection — the look every editor since Bravo settled on.
	t->text_bg             = 0xfff4f2eau;   // warm paper white
	t->text_fg             = OS64_GUI_COLOR_BLACK;
	t->text_sel_bg         = 0xff2a62b8u;   // the house blue
	t->text_sel_fg         = OS64_GUI_COLOR_WHITE;
	t->text_caret          = OS64_GUI_COLOR_BLACK;
	t->field_bg            = OS64_GUI_COLOR_WHITE;
	t->field_fg            = OS64_GUI_COLOR_BLACK;
	t->field_border        = OS64_GUI_COLOR_DARK_GRAY;
	t->field_border_focus  = 0xff2a62b8u;   // "your keys land here"
	t->scroll_track        = 0xffd8d6ceu;
	t->scroll_thumb        = 0xff8a8880u;

	// The menu family: the panel's gray, the house blue for the row under
	// the pointer — a menu is a column of buttons that have not been drawn
	// as buttons.
	t->menu_bg             = OS64_GUI_COLOR_LIGHT_GRAY;
	t->menu_fg             = OS64_GUI_COLOR_BLACK;
	t->menu_hi_bg          = 0xff2a62b8u;
	t->menu_hi_fg          = OS64_GUI_COLOR_WHITE;
	t->menu_sep            = OS64_GUI_COLOR_DARK_GRAY;

	t->pad      = 8;
	t->gap      = 8;
	t->button_h = 24;
	t->scroll_w = 14;
	t->button_bevel = 0;
	t->checkbox_size = 18;
	t->slider_track_h = 4;

	t->font_w = 8;    // the embedded PSF1 face (font_psf1.h)
	t->font_h = 16;
}

// The key table: theme.conf names → theme fields. Adding a themable value =
// one struct field + one row here; scattering a constant anywhere else is
// the review offense the ui.h header warns about.
typedef enum { THEME_COLOR, THEME_METRIC } theme_kind_t;
typedef struct
{
	const char  *key;
	theme_kind_t kind;
	size_t       offset;
	int32_t min, max;
} theme_key_t;

#define THEME_ROW(name, kind, field) \
	{ name, kind, __builtin_offsetof(os64_ui_theme_t, field), 0, 0 }

#define METRIC(name, field, lo, hi) \
    { name, THEME_METRIC, __builtin_offsetof(os64_ui_theme_t, field), lo, hi }

static const theme_key_t kThemeKeys[] = {
	THEME_ROW("panel.bg",            THEME_COLOR,  panel_bg),
	THEME_ROW("panel.border",        THEME_COLOR,  panel_border),
	THEME_ROW("label.fg",            THEME_COLOR,  label_fg),
	THEME_ROW("button.face",         THEME_COLOR,  button_face),
	THEME_ROW("button.face.pressed", THEME_COLOR,  button_face_pressed),
	THEME_ROW("button.border",       THEME_COLOR,  button_border),
	THEME_ROW("button.fg",           THEME_COLOR,  button_fg),
	THEME_ROW("button.highlight",    THEME_COLOR,  button_highlight),
	THEME_ROW("button.shadow",       THEME_COLOR,  button_shadow),
	THEME_ROW("button.face.hover",   THEME_COLOR,  button_face_hover),
	THEME_ROW("hover.border",        THEME_COLOR,  hover_border),
	THEME_ROW("focus.ring",          THEME_COLOR,  focus_ring),
	THEME_ROW("disabled.bg",         THEME_COLOR,  disabled_bg),
	THEME_ROW("disabled.fg",         THEME_COLOR,  disabled_fg),
	THEME_ROW("text.bg",             THEME_COLOR,  text_bg),
	THEME_ROW("text.fg",             THEME_COLOR,  text_fg),
	THEME_ROW("text.sel.bg",         THEME_COLOR,  text_sel_bg),
	THEME_ROW("text.sel.fg",         THEME_COLOR,  text_sel_fg),
	THEME_ROW("text.caret",          THEME_COLOR,  text_caret),
	THEME_ROW("field.bg",            THEME_COLOR,  field_bg),
	THEME_ROW("field.fg",            THEME_COLOR,  field_fg),
	THEME_ROW("field.border",        THEME_COLOR,  field_border),
	THEME_ROW("field.border.focus",  THEME_COLOR,  field_border_focus),
	THEME_ROW("scroll.track",        THEME_COLOR,  scroll_track),
	THEME_ROW("scroll.thumb",        THEME_COLOR,  scroll_thumb),
	THEME_ROW("menu.bg",             THEME_COLOR,  menu_bg),
	THEME_ROW("menu.fg",             THEME_COLOR,  menu_fg),
	THEME_ROW("menu.hi.bg",          THEME_COLOR,  menu_hi_bg),
	THEME_ROW("menu.hi.fg",          THEME_COLOR,  menu_hi_fg),
	THEME_ROW("menu.sep",            THEME_COLOR,  menu_sep),
	METRIC("pad", pad, 0, 32),
	METRIC("gap", gap, 0, 32),
	METRIC("button.h", button_h, 16, 80),
	METRIC("scroll.w", scroll_w, 4, 40),
	METRIC("button.bevel", button_bevel, 0, 4),
	METRIC("checkbox.size", checkbox_size, 8, 40),
	METRIC("slider.track.h", slider_track_h, 1, 16),
	METRIC("font.w", font_w, 8, 8),
	METRIC("font.h", font_h, 16, 16),
};
#define THEME_KEY_COUNT (sizeof(kThemeKeys) / sizeof(kThemeKeys[0]))

// Parse a 6-digit RGB hex span ("2a62b8") into opaque XRGB. Returns false on
// anything else — a color the user wrote wrong should be refused loudly, not
// half-parsed into a surprise.
static bool parse_color(const char *s, size_t len, uint32_t *out)
{
	if (len != 6)
		return false;
	uint32_t v = 0;
	for (size_t i = 0; i < 6; i++) {
		char c = s[i];
		uint32_t d;
		if (c >= '0' && c <= '9')      d = (uint32_t)(c - '0');
		else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
		else return false;
		v = (v << 4) | d;
	}
	*out = 0xff000000u | v;   // the X byte stays opaque until alpha exists
	return true;
}

static bool parse_metric(const char *s, size_t len, int32_t *out)
{
	if (len == 0 || len > 5)
		return false;
	int32_t v = 0;
	for (size_t i = 0; i < len; i++) {
		if (s[i] < '0' || s[i] > '9')
			return false;
		v = v * 10 + (s[i] - '0');
	}
	*out = v;
	return true;
}


static bool session_key(const theme_key_t *key)
{
    return key->kind == THEME_COLOR ||
           key->offset == __builtin_offsetof(os64_ui_theme_t, button_bevel);
}

bool os64_ui_theme_valid(const os64_ui_theme_t *t)
{
    for (size_t i = 0; i < THEME_KEY_COUNT; ++i) {
        const theme_key_t *key = &kThemeKeys[i];
        const char *value = (const char *)t + key->offset;
        if (key->kind == THEME_COLOR) {
            if ((*(const uint32_t *)value >> 24) != 255) return false;
        } else {
            int32_t metric = *(const int32_t *)value;
            if (metric < key->min || metric > key->max) return false;
        }
    }
    return true;
}

typedef struct {
    os64_ui_theme_t candidate;
    uint64_t seen;
    bool bad, session;
} theme_parse_t;
_Static_assert(THEME_KEY_COUNT <= 64, "theme schema exceeds presence bitmap");

static bool theme_setting(const char *name, const char *value, void *user)
{
    theme_parse_t *p = user;
    if (!name) { p->bad = true; return false; }
    for (size_t i = 0; i < THEME_KEY_COUNT; ++i) {
        const theme_key_t *key = &kThemeKeys[i];
        if (!os64_streq(name, key->key)) continue;
        if (p->session && !session_key(key)) { p->bad = true; return false; }
        void *field = (char *)&p->candidate + key->offset;
        bool ok = key->kind == THEME_COLOR ?
            parse_color(value, os64_strlen(value), field) :
            parse_metric(value, os64_strlen(value), field);
        if (!ok) { p->bad = true; return false; }
        p->seen |= (uint64_t)1 << i;
        return true;
    }
    p->bad = true;
    return false;
}

// Presence is carried by key names, not table indices, in the shared payload.
// An explicit startup marker permits missing live keys to retain app defaults.
#define STARTUP_OVERLAY "inherit = startup\n"
int64_t os64_ui_theme_decode_session(os64_ui_theme_t *t, uint64_t *fields,
                                     bool *inherited, const char *text, size_t length)
{
    if (!text) return OS64_CONF_BAD_SETTING;
    const size_t prefix = sizeof(STARTUP_OVERLAY) - 1;
    bool overlay = length >= prefix;
    for (size_t i = 0; overlay && i < prefix; ++i)
        if (text[i] != STARTUP_OVERLAY[i]) overlay = false;
    if (overlay) { text += prefix; length -= prefix; }
    theme_parse_t p = {.candidate = *t, .session = true};
    int64_t result = os64_conf_parse(text, length, theme_setting, &p);
    if (result < 0) return result;
    if (p.bad || !os64_ui_theme_valid(&p.candidate)) return OS64_CONF_BAD_SETTING;
    if (!overlay)
        for (size_t i = 0; i < THEME_KEY_COUNT; ++i)
            if (session_key(&kThemeKeys[i]) && !(p.seen & ((uint64_t)1 << i)))
                return OS64_CONF_BAD_SETTING;
    *t = p.candidate;
    *fields = p.seen;
    *inherited = overlay;
    return 0;
}

int64_t os64_ui_theme_parse_status(os64_ui_theme_t *t, const char *text, size_t length,
                                  bool session)
{
    if (session) {
        uint64_t fields; bool inherited;
        return os64_ui_theme_decode_session(t, &fields, &inherited, text, length);
    }
    theme_parse_t p = {.candidate = *t};
    int64_t result = os64_conf_parse(text, length, theme_setting, &p);
    if (result < 0) return result;
    if (p.bad || !os64_ui_theme_valid(&p.candidate)) return OS64_CONF_BAD_SETTING;
    *t = p.candidate;
    return 0;
}

bool os64_ui_theme_parse(os64_ui_theme_t *t, const char *text, size_t length,
                        bool session)
{
    return os64_ui_theme_parse_status(t, text, length, session) == 0;
}

int64_t os64_ui_theme_parse_saved_status(os64_ui_theme_t *t, const char *text, size_t length)
{
    theme_parse_t p = {.candidate = *t};
    int64_t result = os64_conf_parse(text, length, theme_setting, &p);
    if (result < 0) return result;
    if (p.bad || !os64_ui_theme_valid(&p.candidate)) return OS64_CONF_BAD_SETTING;
    for (size_t i = 0; i < THEME_KEY_COUNT; ++i)
        if (!(p.seen & ((uint64_t)1 << i))) return OS64_CONF_BAD_SETTING;
    *t = p.candidate;
    return 0;
}

bool os64_ui_theme_parse_saved(os64_ui_theme_t *t, const char *text, size_t length)
{
    return os64_ui_theme_parse_saved_status(t, text, length) == 0;
}

static int64_t theme_encode(const os64_ui_theme_t *t, char *text, size_t cap, bool session, uint64_t fields)
{
    if (!text || !os64_ui_theme_valid(t)) return -1;
    size_t used = 0;
    for (size_t i = 0; i < THEME_KEY_COUNT; ++i) {
        const theme_key_t *key = &kThemeKeys[i];
        if (!(fields & ((uint64_t)1 << i)) || (session && !session_key(key))) continue;
        if (used >= cap) return -1;
        const void *field = (const char *)t + key->offset;
        int n = key->kind == THEME_COLOR ?
            os64_snprintf(text + used, cap - used, "%s = %06x\n", key->key,
                          *(const uint32_t *)field & 0xffffffu) :
            os64_snprintf(text + used, cap - used, "%s = %d\n", key->key,
                          *(const int32_t *)field);
        if (n < 0 || (size_t)n >= cap - used) return -1;
        used += (size_t)n;
    }
    return (int64_t)used;
}

int64_t os64_ui_theme_encode_session(const os64_ui_theme_t *t, char *text, size_t cap)
{
    return theme_encode(t, text, cap, true, UINT64_MAX);
}

int64_t os64_ui_theme_encode(const os64_ui_theme_t *t, char *text, size_t cap)
{
    return theme_encode(t, text, cap, false, UINT64_MAX);
}

int64_t os64_ui_theme_snapshot_startup(char *text, size_t cap)
{
    theme_parse_t p = {0};
    os64_ui_theme_defaults(&p.candidate);
    int64_t result = os64_conf_find_read("theme.conf", theme_setting, &p, NULL, 0);
    // Missing or malformed configuration has no overrides. Transient failures
    // must not freeze an empty overlay in place of a valid startup file.
    if (result == OS64_CONF_NO_MEMORY || result == OS64_CONF_IO_ERROR) return result;
    if (result < 0 || p.bad || !os64_ui_theme_valid(&p.candidate)) {
        p.seen = 0;
        os64_ui_theme_defaults(&p.candidate);
    }
    const size_t prefix = sizeof(STARTUP_OVERLAY) - 1;
    if (cap <= prefix) return OS64_CONF_TRUNCATED;
    os64_memcpy(text, STARTUP_OVERLAY, prefix);
    int64_t n = theme_encode(&p.candidate, text + prefix, cap - prefix, true, p.seen);
    return n < 0 ? n : (int64_t)prefix + n;
}

void os64_ui_theme_merge_fields(os64_ui_theme_t *dst, const os64_ui_theme_t *src,
                                uint64_t fields)
{
    for (size_t i = 0; i < THEME_KEY_COUNT; ++i)
        if (fields & ((uint64_t)1 << i))
            os64_memcpy((char *)dst + kThemeKeys[i].offset,
                        (const char *)src + kThemeKeys[i].offset, 4);
}

static bool startup_text_valid(const char *text, size_t length, void *user)
{
    (void)user;
    os64_ui_theme_t candidate;
    os64_ui_theme_defaults(&candidate);
    return os64_ui_theme_parse_saved(&candidate, text, length);
}

int64_t os64_ui_theme_set_startup(const os64_ui_theme_t *t)
{
    if (!os64_ui_theme_valid(t)) return OS64_CONF_BAD_SETTING;
    _Static_assert(THEME_KEY_COUNT <= OS64_CONF_WRITE_MAX, "theme must fit one save");
    os64_conf_pair_t pairs[THEME_KEY_COUNT];
    char values[THEME_KEY_COUNT][16];
    for (size_t i = 0; i < THEME_KEY_COUNT; ++i) {
        const theme_key_t *key = &kThemeKeys[i];
        const void *field = (const char *)t + key->offset;
        if (key->kind == THEME_COLOR)
            os64_snprintf(values[i], sizeof(values[i]), "%06x", *(const uint32_t *)field & 0xffffffu);
        else
            os64_snprintf(values[i], sizeof(values[i]), "%d", *(const int32_t *)field);
        pairs[i] = (os64_conf_pair_t){key->key, values[i]};
    }
    if (os64_ui_theme_preserve_session() != 0) return OS64_CONF_IO_ERROR;
    return os64_conf_write_checked("theme.conf", pairs, THEME_KEY_COUNT, startup_text_valid, NULL);
}

void os64_ui_theme_merge(os64_ui_theme_t *dst, const os64_ui_theme_t *src,
                         uint32_t components)
{
    for (size_t i = 0; i < THEME_KEY_COUNT; ++i) {
        const theme_key_t *key = &kThemeKeys[i];
        bool copy = key->kind == THEME_COLOR ? components & OS64_UI_COMPONENT_PALETTE :
            session_key(key) && (components & OS64_UI_COMPONENT_TREATMENT);
        if (copy)
            os64_memcpy((char *)dst + key->offset, (const char *)src + key->offset, 4);
    }
}

bool os64_ui_theme_read_startup(os64_ui_theme_t *t)
{
    theme_parse_t p = {.candidate = *t};
    char path[OS64_CONF_PATH_MAX] = {0};
    int64_t result = os64_conf_find_read("theme.conf", theme_setting, &p, path, sizeof(path));
    if (result == OS64_CONF_NO_FILE) return false;
    if (result < 0 || p.bad || !os64_ui_theme_valid(&p.candidate)) {
        os64_printf("libui: unusable startup theme %s; keeping existing theme\n", path);
        return false;
    }
    *t = p.candidate;
    return true;
}

void os64_ui_theme_startup(os64_ui_theme_t *t)
{
    os64_ui_theme_defaults(t);
    os64_ui_theme_read_startup(t);
}

void os64_ui_theme_init(os64_ui_theme_t *t)
{
    os64_ui_theme_defaults(t);
    uint64_t installed = 0;
    os64_ui_theme_current(t, &installed);
}
