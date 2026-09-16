#include "os64/ui.h"
#include "os64/appearance.h"
#include "os64/io.h"
#include "os64/proc.h"
#include "os64/conf.h"
#include "os64/signal.h"
#include "ui_internal.h"

// Process-local: shared-library writable pages are private to each task.
// Serialize reads, validation, and publication so sibling window threads do
// not race the cache. Call from ordinary event loops, not signal handlers.
// Contenders yield because the holder performs bounded file I/O and parsing.
static unsigned s_lock;
static bool s_observed;
static uint64_t s_seen, s_usable;
static os64_ui_theme_t s_theme;

static void session_lock(void)
{
    while (__atomic_exchange_n(&s_lock, 1u, __ATOMIC_ACQUIRE)) os64_yield();
}
static void session_unlock(void)
{
    __atomic_store_n(&s_lock, 0u, __ATOMIC_RELEASE);
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
    int64_t parsed = generation ?
        os64_ui_theme_parse_status(&candidate, bytes + body, used - body, true) : 0;
    // Allocation failure is transient, not evidence that these bytes are bad.
    if (parsed == OS64_CONF_NO_MEMORY) return OS64_UI_APPLY_IO;
    s_seen = generation;
    s_observed = true;
    if (parsed < 0) return OS64_UI_APPLY_INVALID;
    s_theme = candidate;
    s_usable = generation;
    return 0;
}

bool os64_ui_theme_session(os64_ui_theme_t *theme, uint64_t *installed, uint64_t hint)
{
    session_lock();
    (void)refresh_locked(hint);
    bool changed = s_usable > *installed;
    if (changed) {
        os64_ui_theme_merge(theme, &s_theme,
            OS64_UI_COMPONENT_PALETTE | OS64_UI_COMPONENT_TREATMENT);
        *installed = s_usable;
    }
    session_unlock();
    return changed;
}

static int publish_locked(const os64_ui_theme_t *theme, uint64_t expected, uint64_t *published)
{
    os64_ui_theme_t candidate = *theme;
    int result;
    char bytes[OS64_APPEARANCE_MAX + 1];
    size_t head = os64_appearance_header_write(bytes, expected);
    int64_t n = os64_ui_theme_encode_session(&candidate, bytes + head,
                                            sizeof(bytes) - head);
    if (n < 0 || n > OS64_APPEARANCE_PAYLOAD_MAX) {
        result = OS64_UI_APPLY_INVALID;
        return result;
    }
    int64_t fd = os64_open(OS64_APPEARANCE_PATH, "w");
    if (fd < 0) { result = OS64_UI_APPLY_IO; return result; }
    size_t length = head + (size_t)n;
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
    s_seen = s_usable = expected + 1;
    s_observed = true;
    if (published) *published = s_usable;
    return 0;
}

// Pin the current startup colors before changing theme.conf so future
// applications join the same session. A racing Apply wins its generation;
// startup selection must not replace that publisher's choice.
int os64_ui_theme_preserve_session(void)
{
    os64_ui_theme_t startup;
    os64_ui_theme_startup(&startup);
    session_lock();
    int result = refresh_locked(0);
    if (!result && s_seen == 0) {
        result = publish_locked(&startup, 0, NULL);
        if (result == OS64_UI_APPLY_CONFLICT && s_seen == s_usable) result = 0;
    }
    session_unlock();
    return result;
}

int os64_ui_theme_apply(const os64_ui_theme_t *draft, uint32_t components,
                        uint64_t *published)
{
    const uint32_t all = OS64_UI_COMPONENT_PALETTE | OS64_UI_COMPONENT_TREATMENT;
    if (!draft || !components || (components & ~all) || !os64_ui_theme_valid(draft))
        return OS64_UI_APPLY_INVALID;
    os64_ui_theme_t candidate;
    os64_ui_theme_startup(&candidate);
    session_lock();
    int result = refresh_locked(0);
    // A complete replacement can repair an invalid published payload. A
    // component update cannot preserve fields it could not decode.
    if (result && !(result == OS64_UI_APPLY_INVALID && components == all))
        goto done;
    uint64_t expected = s_seen;
    if (expected == UINT64_MAX) { result = OS64_UI_APPLY_EXHAUSTED; goto done; }
    if (s_usable) os64_ui_theme_merge(&candidate, &s_theme, all);
    os64_ui_theme_merge(&candidate, draft, components);
    result = publish_locked(&candidate, expected, published);
done:
    session_unlock();
    return result;
}
