#include "os64/ui.h"
#include "os64/lock.h"
#include "os64/appearance.h"
#include "os64/io.h"
#include "os64/proc.h"
#include "os64/conf.h"
#include "os64/signal.h"
#include "ui_internal.h"
#include "ui_envelope.h"
#include "os64/font_settings.h"
#include "os64/str.h"
#include "os64/fmt.h"
#include "os64/mem.h"

// Process-local: shared-library writable pages are private to each task.
// Serialize reads, validation, and publication so sibling window threads do
// not race the cache. Call from ordinary event loops, not signal handlers.
// Contenders yield because the holder performs bounded file I/O and parsing.
static os64_lock_t s_lock = OS64_LOCK_INIT;
static bool s_observed;
static uint64_t s_seen, s_usable, s_fields;
static bool s_inherited;
static os64_ui_theme_t s_theme;
static char s_payload[OS64_APPEARANCE_PAYLOAD_MAX + 1];
static size_t s_length;
static bool s_has_fonts;
static uint64_t s_font_serial;
static os64_font_config_t s_fonts;

static int publish_fonts_locked(const os64_font_config_t *, uint64_t *published);

static void session_lock(void)
{
    os64_lock_acquire(&s_lock);
}
static void session_unlock(void)
{
    os64_lock_release(&s_lock);
}

static int refresh_locked(uint64_t hint)
{
    if (hint && s_observed && hint <= s_seen)
        return s_seen == s_usable ? 0 : OS64_UI_APPLY_INVALID;
    char bytes[OS64_APPEARANCE_MAX + 1];
    int64_t fd = os64_open(OS64_APPEARANCE_PATH, "r");
    if (fd < 0) return OS64_UI_APPLY_IO;
    size_t used = 0;
    bool failed = false;
    for (;;) {
        int64_t n = os64_read((int32_t)fd, bytes + used, sizeof(bytes) - used);
        if (n == OS64_INTERRUPTED) continue;
        if (n < 0 || (uint64_t)n > sizeof(bytes) - used) { failed = true; break; }
        if (!n) break;
        used += (size_t)n;
        if (used == sizeof(bytes)) { failed = true; break; }
    }
    os64_close((int32_t)fd);
    uint64_t generation;
    size_t body;
    if (failed || !os64_appearance_header_read(bytes, used, &generation, &body) ||
        used - body > OS64_APPEARANCE_PAYLOAD_MAX ||
        (generation == 0 && used != body)) return OS64_UI_APPLY_IO;
    if (s_observed && generation < s_seen) return OS64_UI_APPLY_IO;
    if (s_observed && generation == s_seen)
        return s_seen == s_usable ? 0 : OS64_UI_APPLY_INVALID;

    os64_ui_theme_t candidate;
    os64_ui_theme_defaults(&candidate);
    uint64_t fields = 0;
    bool inherited = false;
    int64_t parsed = generation ?
        os64_ui_theme_decode_session(&candidate, &fields, &inherited,
                                      bytes + body, used - body) : 0;
    // Allocation failure is transient, not evidence that these bytes are bad.
    if (parsed == OS64_CONF_NO_MEMORY) return OS64_UI_APPLY_IO;
    os64_font_config_t fonts;
    bool has_fonts = false;
    uint64_t serial = 0;
    if (parsed >= 0 && generation) {
        parsed = ui_envelope_fonts(bytes + body, used - body, &fonts, &has_fonts, &serial);
        if (parsed == OS64_CONF_NO_MEMORY) return OS64_UI_APPLY_IO;
    }
    s_seen = generation;
    s_observed = true;
    if (parsed < 0) return OS64_UI_APPLY_INVALID;
    s_theme = candidate;
    s_fields = fields;
    s_inherited = inherited;
    s_length = used - body;
    os64_memcpy(s_payload, bytes + body, s_length);
    s_payload[s_length] = 0;
    s_has_fonts = has_fonts;
    s_font_serial = serial;
    if (has_fonts) s_fonts = fonts;
    s_usable = generation;
    return 0;
}

bool os64_ui_theme_session(os64_ui_theme_t *theme, uint64_t *installed, uint64_t hint)
{
    session_lock();
    (void)refresh_locked(hint);
    bool changed = s_usable > *installed;
    if (changed) {
        os64_ui_theme_merge_fields(theme, &s_theme, s_fields);
        *installed = s_usable;
    }
    session_unlock();
    return changed;
}

// Initialize from caller defaults, then disk geometry and the current palette.
// Read the store after the file: startup writers pin the old override before
// replacing that file, so a reader of the new file can still recover the old
// palette. A preservation overlay leaves unspecified colors at caller defaults.
void os64_ui_theme_current(os64_ui_theme_t *theme, uint64_t *installed)
{
    os64_ui_theme_t fallback = *theme;
    os64_ui_theme_read_startup(theme);
    session_lock();
    (void)refresh_locked(0);
    if (s_usable) {
        if (s_inherited)
            os64_ui_theme_merge(theme, &fallback,
                OS64_UI_COMPONENT_PALETTE | OS64_UI_COMPONENT_TREATMENT);
        os64_ui_theme_merge_fields(theme, &s_theme, s_fields);
    }
    *installed = s_usable;
    session_unlock();
}

static int publish_locked(char *bytes, size_t length, uint64_t expected,
                           uint64_t *published)
{
    os64_ui_theme_t candidate;
    os64_ui_theme_defaults(&candidate);
    uint64_t fields; bool inherited;
    size_t head = os64_appearance_header_write(bytes, expected);
    int64_t parsed = os64_ui_theme_decode_session(&candidate, &fields, &inherited,
                                                   bytes + head, length - head);
    if (parsed < 0)
        return parsed == OS64_CONF_NO_MEMORY ? OS64_UI_APPLY_IO : OS64_UI_APPLY_INVALID;
    os64_font_config_t fonts;
    bool has_fonts = false;
    uint64_t serial = 0;
    parsed = ui_envelope_fonts(bytes + head, length - head, &fonts, &has_fonts, &serial);
    if (parsed < 0)
        return parsed == OS64_CONF_NO_MEMORY ? OS64_UI_APPLY_IO : OS64_UI_APPLY_INVALID;
    int result;
    int64_t fd = os64_open(OS64_APPEARANCE_PATH, "w");
    if (fd < 0) { result = OS64_UI_APPLY_IO; return result; }
    int64_t written = os64_write((int32_t)fd, bytes, length);
    os64_close((int32_t)fd);
    if (written != (int64_t)length) {
        // The file syscall exposes a generic refusal. A fresh snapshot can
        // identify a racing publisher, but a short write is never continued.
        (void)refresh_locked(0);
        result = s_seen > expected ? OS64_UI_APPLY_CONFLICT : OS64_UI_APPLY_IO;
        return result;
    }
    s_theme = candidate;
    s_fields = fields;
    s_inherited = inherited;
    s_length = length - head;
    os64_memcpy(s_payload, bytes + head, s_length);
    s_payload[s_length] = 0;
    s_has_fonts = has_fonts;
    s_font_serial = serial;
    if (has_fonts) s_fonts = fonts;
    s_seen = s_usable = expected + 1;
    s_observed = true;
    if (published) *published = s_usable;
    return 0;
}

// Pin the startup overrides, including their absence, before changing the file.
// App-specific defaults must not become a shared color choice. A racing Apply
// wins its generation; startup selection must not replace that choice.
int os64_ui_theme_preserve_session(void)
{
    session_lock();
    int result = refresh_locked(0);
    if (!result && s_seen == 0) {
        os64_font_config_t fonts;
        os64_font_config_status_t status = os64_font_config_read(&fonts, NULL);
        if (status == OS64_FONT_CONFIG_IO || status == OS64_FONT_CONFIG_NO_MEMORY)
            result = OS64_UI_APPLY_IO;
        else {
            if (status) os64_font_config_defaults(&fonts);
            result = publish_fonts_locked(&fonts, NULL);
        }
        if (result == OS64_UI_APPLY_CONFLICT && s_seen == s_usable) result = 0;
    }
    session_unlock();
    return result;
}

/* A first explicit Apply fills the known palette while retaining startup
 * comments and future components. The partial inheritance marker is replaced
 * by the complete palette so later readers do not apply defaults over it. */
static int64_t startup_payload(const os64_ui_theme_t *theme, char *out, size_t cap)
{
    char startup[OS64_APPEARANCE_PAYLOAD_MAX + 1];
    char known[OS64_APPEARANCE_PAYLOAD_MAX + 1];
    int64_t n = os64_ui_theme_snapshot_startup(startup, sizeof(startup));
    if (n < 0) return n;
    int64_t k = os64_ui_theme_encode_session(theme, known, sizeof(known));
    if (k < 0) return k;
    return ui_envelope_merge(startup, (size_t)n, known, (size_t)k,
        OS64_UI_COMPONENT_PALETTE | OS64_UI_COMPONENT_TREATMENT | UI_ENVELOPE_INHERIT,
        out, cap);
}

int os64_ui_theme_apply(const os64_ui_theme_t *draft, uint32_t components,
                        uint64_t *published)
{
    const uint32_t all = OS64_UI_COMPONENT_PALETTE | OS64_UI_COMPONENT_TREATMENT;
    if (!draft || !components || (components & ~all) || !os64_ui_theme_valid(draft))
        return OS64_UI_APPLY_INVALID;
    os64_ui_theme_t candidate;
    os64_ui_theme_defaults(&candidate);
    uint64_t installed = 0;
    os64_ui_theme_current(&candidate, &installed);
    session_lock();
    int result = refresh_locked(0);
    // A complete replacement can repair an invalid published payload. A
    // component update cannot preserve fields it could not decode.
    if (result && !(result == OS64_UI_APPLY_INVALID && components == all))
        goto done;
    uint64_t expected = s_seen;
    if (expected == UINT64_MAX) { result = OS64_UI_APPLY_EXHAUSTED; goto done; }
    if (s_usable) os64_ui_theme_merge_fields(&candidate, &s_theme, s_fields);
    os64_ui_theme_merge(&candidate, draft, components);
    char bytes[OS64_APPEARANCE_MAX + 1];
    size_t head = os64_appearance_header_write(bytes, expected);
    char replacement[OS64_APPEARANCE_PAYLOAD_MAX + 1];
    int64_t n = os64_ui_theme_encode_session(&candidate, replacement, sizeof(replacement));
    if (n >= 0) {
        if (s_seen && s_seen == s_usable)
            n = ui_envelope_merge(s_payload, s_length, replacement, (size_t)n,
                                  components, bytes + head, sizeof(bytes) - head);
        else if (!s_seen) n = startup_payload(&candidate, bytes + head, sizeof(bytes) - head);
        else os64_memcpy(bytes + head, replacement, (size_t)n);
    }
    if (n < 0 || n > OS64_APPEARANCE_PAYLOAD_MAX) result = OS64_UI_APPLY_INVALID;
    else result = publish_locked(bytes, head + (size_t)n, expected, published);
done:
    session_unlock();
    return result;
}

int os64_font_settings_current(os64_font_config_t *out, uint64_t *generation)
{
    if (!out || !generation) return OS64_UI_APPLY_INVALID;
    /* Read disk first, then the store. A concurrent Save pins the old choices
     * before replacing the file, so this ordering cannot expose its next-boot
     * choice to a reader in the current session. */
    os64_font_config_t startup;
    os64_font_config_error_t error;
    os64_font_config_status_t read = os64_font_config_read(&startup, &error);
    session_lock();
    int result = refresh_locked(0);
    if (!result && s_has_fonts) {
        *out = s_fonts;
        /* Session paths are absolute; discovery still belongs to the selected
         * startup file's directory, not the parser's synthetic source name. */
        os64_strcopy(out->path, sizeof(out->path), read ? "" : startup.path);
        *generation = s_font_serial ? s_font_serial : s_usable;
        session_unlock();
        return 0;
    }
    session_unlock();
    if (result) return result;
    if (read) {
        char line[256];
        os64_snprintf(line, sizeof(line), "fonts.conf: line %lu: %s; keeping usable fonts",
                      (unsigned long)error.line, os64_font_config_status_name(read));
        os64_debug_log(line);
        return read == OS64_FONT_CONFIG_IO || read == OS64_FONT_CONFIG_NO_MEMORY ?
               OS64_UI_APPLY_IO : OS64_UI_APPLY_INVALID;
    }
    *out = startup;
    *generation = 0;
    return 0;
}

static int64_t font_component(const os64_font_config_t *config, char *out, size_t cap)
{
    char settings[4096];
    int64_t n = os64_font_config_encode(config, settings, sizeof(settings));
    if (n < 0) return -1;
    size_t used = 0, start = 0;
    while (start < (size_t)n) {
        size_t end = start;
        while (end < (size_t)n && settings[end] != '\n') ++end;
        if (end < (size_t)n) ++end;
        if (end - start + 6 >= cap - used) return -1;
        os64_memcpy(out + used, "fonts.", 6); used += 6;
        os64_memcpy(out + used, settings + start, end - start); used += end - start;
        start = end;
    }
    out[used] = 0;
    return (int64_t)used;
}

static int publish_fonts_locked(const os64_font_config_t *config, uint64_t *published)
{
    if (s_seen == UINT64_MAX) return OS64_UI_APPLY_EXHAUSTED;
    char replacement[OS64_APPEARANCE_PAYLOAD_MAX + 1];
    int64_t replacement_len = font_component(config, replacement, sizeof(replacement));
    if (replacement_len < 0) return OS64_UI_APPLY_INVALID;
    int added = os64_snprintf(replacement + replacement_len,
        sizeof(replacement) - (size_t)replacement_len, "fonts.serial = %lu\n",
        (unsigned long)(s_seen + 1));
    if (added < 0 || (size_t)added >= sizeof(replacement) - (size_t)replacement_len)
        return OS64_UI_APPLY_INVALID;
    replacement_len += added;
    char base[OS64_APPEARANCE_PAYLOAD_MAX + 1];
    size_t length = s_length;
    if (s_seen) os64_memcpy(base, s_payload, length);
    else {
        int64_t n = os64_ui_theme_snapshot_startup(base, sizeof(base));
        if (n < 0) return OS64_UI_APPLY_IO;
        length = (size_t)n;
    }
    char bytes[OS64_APPEARANCE_MAX + 1];
    size_t head = os64_appearance_header_write(bytes, s_seen);
    int64_t n = ui_envelope_merge(base, length, replacement, (size_t)replacement_len,
                                  UI_ENVELOPE_FONTS, bytes + head, sizeof(bytes) - head);
    if (n < 0) return n == OS64_CONF_NO_MEMORY ? OS64_UI_APPLY_IO : OS64_UI_APPLY_INVALID;
    return publish_locked(bytes, head + (size_t)n, s_seen, published);
}

int os64_font_settings_apply(os64_text_context_t *context, const os64_font_config_t *config,
                             uint64_t *published, os64_font_config_error_t *error)
{
    os64_font_set_t *prepared = NULL;
    if (os64_font_config_prepare(context, config, &prepared, error)) return OS64_UI_APPLY_INVALID;
    session_lock();
    int result = refresh_locked(0);
    if (!result) result = publish_fonts_locked(config, published);
    session_unlock();
    os64_font_set_release(prepared);
    return result;
}

static void *settings_alloc(void *user, size_t bytes)
{ (void)user; return os64_malloc(bytes); }
static void settings_free(void *user, void *ptr, size_t bytes)
{ (void)user; (void)bytes; os64_free(ptr); }

static bool saved_fonts_valid(const char *text, size_t length, void *user)
{
    os64_font_config_t config;
    return os64_font_config_decode(text, length, user, &config, NULL) == OS64_FONT_CONFIG_OK;
}

int os64_font_settings_save(const os64_font_config_t *config, os64_font_config_error_t *error)
{
    char encoded[4096], target[OS64_FONT_PATH_CAP];
    int64_t n = os64_font_config_encode(config, encoded, sizeof(encoded));
    if (n < 0) return OS64_UI_APPLY_INVALID;
    if (os64_conf_target("fonts.conf", target, sizeof(target)) < 0) return OS64_UI_APPLY_IO;
    os64_font_config_t desired;
    if (os64_font_config_decode(encoded, (size_t)n, target, &desired, error))
        return OS64_UI_APPLY_INVALID;
    os64_text_options_t options = {.memory = {NULL, settings_alloc, settings_free}};
    os64_text_context_t *context = NULL;
    if (os64_font_context_create(&options, &context) != OS64_FONT_OK) return OS64_UI_APPLY_IO;
    os64_font_set_t *candidate = NULL;
    os64_font_config_status_t status = os64_font_config_prepare(context, &desired, &candidate, error);
    os64_font_set_release(candidate);
    os64_text_destroy(context);
    if (status) return OS64_UI_APPLY_INVALID;

    /* Capture startup choices before changing their file. If a font Apply
     * raced this read, the locked refresh sees it and preserves that newer
     * complete session rather than publishing these older startup choices. */
    os64_font_config_t old;
    status = os64_font_config_read(&old, NULL);
    if (status == OS64_FONT_CONFIG_IO || status == OS64_FONT_CONFIG_NO_MEMORY)
        return OS64_UI_APPLY_IO;
    if (status) os64_font_config_defaults(&old);
    session_lock();
    int result = refresh_locked(0);
    if (!result && !s_has_fonts) result = publish_fonts_locked(&old, NULL);
    session_unlock();
    if (result) return result;

    const char *const roles[] = {"ui", "terminal", "document"};
    const char *const suffix[] = {"face", "fallback.1", "fallback.2", "size"};
    os64_conf_pair_t pairs[12];
    char keys[12][32], sizes[3][8];
    for (size_t r = 0; r < 3; ++r) {
        os64_snprintf(sizes[r], sizeof(sizes[r]), "%u", desired.roles[r].size);
        for (size_t f = 0; f < 4; ++f) {
            size_t i = r * 4 + f;
            os64_snprintf(keys[i], sizeof(keys[i]), "%s.%s", roles[r], suffix[f]);
            const char *value = f == 3 ? sizes[r] : desired.roles[r].face[f];
            pairs[i] = (os64_conf_pair_t){keys[i], *value ? value : NULL};
        }
    }
    return os64_conf_update_checked("fonts.conf", pairs, 12, saved_fonts_valid, target) < 0 ?
           OS64_UI_APPLY_IO : 0;
}
