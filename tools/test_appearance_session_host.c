#include "os64/io.h"
#include "os64/slurp.h"
#include "os64/str.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
extern pid_t waitpid(pid_t pid, int *status, int options);
#include "os64/ui.h"
#include "os64/appearance.h"
#include "os64/conf.h"
#include "os64/font_settings.h"
#include "os64/signal.h"
#include "appearance.h"
#include "gui/event_queue.h"

pthread_mutex_t kGuiLock = PTHREAD_MUTEX_INITIALIZER;
bool fail_alloc;
static unsigned allocations, reads, writes, notifications;
static bool fail_open, fail_read, refuse_write, short_write, race_write;
static const char *startup_text;
static char next_startup[4096];
static int startup_error;
static bool fail_startup_save;
int os64_ui_theme_preserve_session(void);
int64_t os64_ui_theme_snapshot_startup(char *text, size_t cap);
static gui_event_queue_t queues[3];
static char racing[OS64_APPEARANCE_MAX + 1];
static size_t racing_length;
static struct {
    bool open, writer;
    char data[OS64_APPEARANCE_MAX];
    size_t size, pos;
} handles[16];

void *os64_malloc(size_t size) { ++allocations; return fail_alloc ? NULL : malloc(size); }
void os64_free(void *p) { free(p); }
void os64_yield(void) { sched_yield(); }
int64_t __wrap_os64_conf_find_read(const char *name, os64_conf_fn fn, void *user,
                                  char *path, size_t cap)
{
    (void)name; (void)path; (void)cap;
    if (startup_error) return startup_error;
    return startup_text ? os64_conf_parse(startup_text, strlen(startup_text), fn, user) : OS64_CONF_NO_FILE;
}
// The filesystem suite exercises the actual atomic writer. Here the seam
// changes disk configuration only after the production preservation path runs.
int64_t __wrap_os64_conf_write_checked(const char *name,
    const os64_conf_pair_t *pairs, size_t count,
    bool (*validate)(const char *, size_t, void *), void *user)
{
    assert(!strcmp(name, "theme.conf"));
    if (fail_startup_save) return OS64_CONF_IO_ERROR;
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        int n = snprintf(next_startup + used, sizeof(next_startup) - used,
                         "%s = %s\n", pairs[i].key, pairs[i].value);
        assert(n > 0 && (size_t)n < sizeof(next_startup) - used);
        used += (size_t)n;
    }
    assert(validate(next_startup, used, user));
    startup_text = next_startup;
    return 0;
}
int64_t os64_open(const char *path, const char *mode)
{
    bool font = !strcmp(path, "/test/scalable") || !strcmp(path, "/test/mono");
    assert(font || strcmp(path, OS64_APPEARANCE_PATH) == 0);
    if (fail_open) return -1;
    for (int fd = 3; fd < 16; ++fd) {
        if (handles[fd].open) continue;
        handles[fd].open = true;
        handles[fd].writer = mode && *mode == 'w';
        handles[fd].pos = 0;
        if (!handles[fd].writer) {
            if (font) {
                handles[fd].data[0] = !strcmp(path, "/test/mono") ? 'T' : 'S';
                handles[fd].size = 1;
                return fd;
            }
            ++reads;
            int n = appearance_snapshot(handles[fd].data, sizeof(handles[fd].data));
            assert(n > 0);
            handles[fd].size = (size_t)n;
        }
        return fd;
    }
    abort();
}
int64_t os64_close(int32_t fd)
{
    assert(fd >= 3 && fd < 16 && handles[fd].open);
    handles[fd].open = false;
    return 0;
}
int64_t os64_read(int32_t fd, void *buf, size_t n)
{
    assert(fd >= 3 && fd < 16 && handles[fd].open && !handles[fd].writer);
    if (fail_read) return -1;
    size_t remaining = handles[fd].size - handles[fd].pos;
    if (n > remaining) n = remaining;
    if (n > 19) n = 19; // exercise complete reads from short chunks
    memcpy(buf, handles[fd].data + handles[fd].pos, n);
    handles[fd].pos += n;
    return (int64_t)n;
}
int64_t os64_write(int32_t fd, const void *p, size_t n)
{
    if (fd == 1 || fd == 2) return (int64_t)fwrite(p, 1, n, fd == 1 ? stdout : stderr);
    assert(fd >= 3 && fd < 16 && handles[fd].open && handles[fd].writer);
    ++writes;
    if (refuse_write) return -1;
    if (short_write) return (int64_t)n - 1;
    if (race_write) {
        race_write = false;
        assert(appearance_publish(racing, racing_length) == (int)racing_length);
    }
    return appearance_publish(p, n);
}
void wm_appearance_changed(uint64_t generation)
{
    ++notifications;
    input_event_t ev = {.type = INPUT_EVENT_APPEARANCE,
        .appearance = {(uint32_t)generation, (uint32_t)(generation >> 32)}};
    for (size_t i = 0; i < 3; ++i) assert(gui_event_queue_push(&queues[i], &ev) == 1);
}

static uint64_t generation(void)
{
    char bytes[OS64_APPEARANCE_MAX];
    int n = appearance_snapshot(bytes, sizeof(bytes));
    uint64_t gen; size_t body;
    assert(n > 0 && os64_appearance_header_read(bytes, (size_t)n, &gen, &body));
    assert(appearance_length() == (size_t)n);
    return gen;
}
static size_t encode(char *out, const os64_ui_theme_t *theme, uint64_t gen)
{
    size_t head = os64_appearance_header_write(out, gen);
    int64_t n = os64_ui_theme_encode_session(theme, out + head, OS64_APPEARANCE_MAX + 1 - head);
    assert(n > 0);
    return head + (size_t)n;
}
static uint64_t publish(const os64_ui_theme_t *theme)
{
    char bytes[OS64_APPEARANCE_MAX + 1];
    uint64_t old = generation();
    size_t n = encode(bytes, theme, old);
    assert(appearance_publish(bytes, n) == (int)n);
    assert(generation() == old + 1);
    return old + 1;
}

static void store_contracts(void)
{
    assert(generation() == 0);
    char old[OS64_APPEARANCE_MAX], after[OS64_APPEARANCE_MAX];
    int initial = appearance_snapshot(old, sizeof(old));
    const char *bad[] = {"", "generation = \n", "generation = -1\n", "generation = 00\n",
        "generation = 18446744073709551616\n", "generation = 1 \n", "generation = 0"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i)
        assert(appearance_publish(bad[i], strlen(bad[i])) < 0);
    assert(notifications == 0 && generation() == 0);
    char bytes[OS64_APPEARANCE_MAX + 1];
    size_t head = os64_appearance_header_write(bytes, 0);
    memset(bytes + head, 0xa5, OS64_APPEARANCE_PAYLOAD_MAX + 1);
    assert(appearance_publish(bytes, head + OS64_APPEARANCE_PAYLOAD_MAX + 1) < 0);
    assert(appearance_publish(bytes, head + OS64_APPEARANCE_PAYLOAD_MAX) > 0);
    assert(appearance_publish(bytes, head + OS64_APPEARANCE_PAYLOAD_MAX) < 0);
    int n = appearance_snapshot(after, sizeof(after));
    assert(n == (int)(head + OS64_APPEARANCE_PAYLOAD_MAX));
    assert(memcmp(after + head, bytes + head, OS64_APPEARANCE_PAYLOAD_MAX) == 0);
    assert(initial == 15 && memcmp(old, "generation = 0\n", 15) == 0);
    uint64_t decoded; size_t body;
    head = os64_appearance_header_write(bytes, UINT64_MAX);
    assert(head == OS64_APPEARANCE_HEADER_MAX);
    assert(os64_appearance_header_read(bytes, head, &decoded, &body) && decoded == UINT64_MAX);
    assert(appearance_publish(bytes, head) < 0 && generation() == 1);
}

static void client_contracts(void)
{
    os64_ui_theme_t paper, electric;
    os64_ui_theme_defaults(&paper);
    os64_ui_theme_palette(&paper, OS64_UI_PALETTE_PAPER);
    paper.button_bevel = 3;
    electric = paper;
    os64_ui_theme_palette(&electric, OS64_UI_PALETTE_ELECTRIC);
    uint64_t gen = publish(&paper);
    os64_ui_t a, b, preview;
    os64_ui_init(&a, NULL); os64_ui_init(&b, NULL); os64_ui_init(&preview, NULL);
    preview.follow_session = false;
    a.theme.pad = 17; b.theme.pad = 3;
    os64_ui_widget_t ar = {.bounds={0,0,100,100}}, br = {.bounds={0,0,90,90}};
    a.root = &ar; b.root = &br;
    a.focus = a.grab = a.hover = &ar; ar.pressed = ar.focused = true;
    unsigned alloc_before = allocations, read_before = reads;
    gen = publish(&electric);
    os64_gui_event_t ev = {.type=OS64_GUI_EVENT_APPEARANCE,
        .appearance={(uint32_t)gen, (uint32_t)(gen >> 32)}};
    assert(os64_ui_dispatch(&a, &ev));
    unsigned after_decode = allocations;
    assert(after_decode > alloc_before);
    assert(os64_ui_dispatch(&b, &ev));
    assert(os64_ui_dispatch(&preview, &ev));
    assert(reads == read_before + 1 && allocations == after_decode);
    assert(a.appearance_generation == gen && b.appearance_generation == gen);
    assert(a.any_dirty && b.any_dirty && a.theme.panel_bg == electric.panel_bg);
    assert(a.theme.pad == 17 && b.theme.pad == 3);
    assert(a.focus == &ar && a.grab == &ar && a.hover == &ar && ar.pressed && ar.focused);
    assert(preview.theme.panel_bg == paper.panel_bg && preview.appearance_generation == gen - 1);
    // Bad payload is validated once, never installed, including in a new context.
    char bad[80]; size_t n = os64_appearance_header_write(bad, gen);
    memcpy(bad+n, "unknown = 1\n", 12); n += 12;
    assert(appearance_publish(bad, n) == (int)n);
    ev.appearance.generation_lo = (uint32_t)++gen;
    alloc_before = allocations; read_before = reads;
    os64_ui_dispatch(&a, &ev);
    after_decode = allocations;
    assert(after_decode > alloc_before);
    os64_ui_dispatch(&b, &ev);
    assert(reads == read_before + 1 && allocations == after_decode);
    assert(a.appearance_generation == gen-1 && a.theme.panel_bg == electric.panel_bg);
    os64_ui_t born; os64_ui_init(&born, NULL);
    assert(born.appearance_generation == gen-1 && born.theme.panel_bg == electric.panel_bg);
    assert(os64_ui_theme_apply(&paper, OS64_UI_COMPONENT_TREATMENT, NULL) == OS64_UI_APPLY_INVALID);
    uint64_t applied;
    assert(os64_ui_theme_apply(&paper, 3, &applied) == 0 && applied == ++gen);
    // A component publisher preserves unrelated treatment from the latest state.
    electric.button_bevel = 0;
    assert(os64_ui_theme_apply(&electric, OS64_UI_COMPONENT_PALETTE, &applied) == 0);
    gen = applied;
    uint64_t installed = 0;
    os64_ui_theme_t adopted = paper;
    assert(os64_ui_theme_session(&adopted, &installed, gen));
    assert(adopted.panel_bg == electric.panel_bg && adopted.button_bevel == 3);
    // A racing writer wins; the stale publication fails without being retried.
    racing_length = encode(racing, &paper, gen); race_write = true;
    unsigned before = writes;
    assert(os64_ui_theme_apply(&electric, 3, NULL) == OS64_UI_APPLY_CONFLICT);
    assert(writes == before + 1 && generation() == ++gen);
    installed = 0; os64_ui_theme_session(&adopted, &installed, gen);
    assert(adopted.panel_bg == paper.panel_bg);
    // I/O failures and unexpected short writes never trigger suffix writes.
    short_write = true; before = writes;
    assert(os64_ui_theme_apply(&electric, 3, NULL) == OS64_UI_APPLY_IO);
    assert(writes == before + 1 && generation() == gen); short_write = false;
    refuse_write = true;
    assert(os64_ui_theme_apply(&electric, 3, NULL) == OS64_UI_APPLY_IO); refuse_write = false;
    fail_open = true;
    assert(os64_ui_theme_apply(&electric, 3, NULL) == OS64_UI_APPLY_IO); fail_open = false;
    fail_read = true;
    assert(os64_ui_theme_apply(&electric, 3, NULL) == OS64_UI_APPLY_IO); fail_read = false;
    // Transient parse allocation failure must remain retryable at this generation.
    gen = publish(&electric); ev.appearance.generation_lo = (uint32_t)gen;
    fail_alloc = true; os64_ui_dispatch(&a, &ev);
    assert(a.appearance_generation < gen);
    fail_alloc = false; os64_ui_dispatch(&a, &ev);
    assert(a.appearance_generation == gen && a.theme.panel_bg == electric.panel_bg);
}

static pthread_barrier_t start;
static char concurrent[OS64_APPEARANCE_MAX + 1];
static size_t concurrent_length;
static void *writer(void *result)
{
    pthread_barrier_wait(&start);
    *(int *)result = appearance_publish(concurrent, concurrent_length);
    return NULL;
}
typedef struct { os64_ui_t *ui; os64_gui_event_t event; } reload_arg_t;
static void *reload(void *arg)
{
    reload_arg_t *r = arg;
    pthread_barrier_wait(&start);
    os64_ui_dispatch(r->ui, &r->event);
    return NULL;
}

static void concurrency_and_queue_contracts(void)
{
    os64_ui_theme_t t; os64_ui_theme_defaults(&t);
    uint64_t old = generation();
    concurrent_length = encode(concurrent, &t, old);
    int result[2]; pthread_t threads[2];
    pthread_barrier_init(&start, NULL, 2);
    for (int i=0; i<2; ++i) assert(!pthread_create(&threads[i], NULL, writer, &result[i]));
    for (int i=0; i<2; ++i) pthread_join(threads[i], NULL);
    pthread_barrier_destroy(&start);
    assert((result[0] > 0) != (result[1] > 0));
    assert(generation() == old + 1);
    os64_ui_t a, b;
    os64_ui_init(&a, NULL); os64_ui_init(&b, NULL);
    uint64_t fresh = publish(&t);
    reload_arg_t args[2] = {
        {.ui=&a, .event={.type=OS64_GUI_EVENT_APPEARANCE, .appearance={(uint32_t)fresh,0}}},
        {.ui=&b, .event={.type=OS64_GUI_EVENT_APPEARANCE, .appearance={(uint32_t)fresh,0}}}
    };
    unsigned before_reads = reads, before_allocs = allocations;
    pthread_barrier_init(&start, NULL, 2);
    for (int i=0; i<2; ++i) assert(!pthread_create(&threads[i], NULL, reload, &args[i]));
    for (int i=0; i<2; ++i) pthread_join(threads[i], NULL);
    pthread_barrier_destroy(&start);
    assert(reads == before_reads+1 && allocations > before_allocs);
    assert(a.appearance_generation == fresh && b.appearance_generation == fresh);
    for (size_t k=0; k<3; ++k) {
        input_event_t motion = {.type=INPUT_EVENT_MOUSE_MOVE};
        while (gui_event_queue_push(&queues[k], &motion)) {}
        input_event_t pointer = {.type=INPUT_EVENT_POINTER_STATE, .pointer={.inside=0}};
        assert(gui_event_queue_push(&queues[k], &pointer));
    }
    publish(&t); uint64_t latest = publish(&t);
    for (size_t k=0; k<3; ++k) {
        input_event_t out;
        assert(gui_event_queue_pending(&queues[k]) && gui_event_queue_pop(&queues[k], &out));
        assert(out.type == INPUT_EVENT_APPEARANCE && out.appearance.generation_lo == latest);
        for (int i=0; i<GUI_WINDOW_EVENTS_MAX-1; ++i) {
            assert(gui_event_queue_pop(&queues[k], &out));
            assert(out.type == INPUT_EVENT_MOUSE_MOVE);
        }
        assert(gui_event_queue_pop(&queues[k], &out) && out.type == INPUT_EVENT_POINTER_STATE);
        assert(!gui_event_queue_pending(&queues[k]));
    }
}
static void startup_preservation_contracts(void)
{
    enum { FRESH, RACING, REFUSED, PARTIAL, INVALID, EMPTY, FULL,
           READ_FAIL, ALLOC_FAIL, COLD_CACHE, SAVE_FAIL, CASE_COUNT };
    // Separate address spaces keep the boot-lifetime store/cache pristine.
    for (int mode = 0; mode < CASE_COUNT; ++mode) {
        pid_t pid = fork(); assert(pid >= 0);
        if (!pid) {
            os64_ui_theme_t target, paper;
            os64_ui_theme_defaults(&target);
            os64_ui_theme_palette(&target, OS64_UI_PALETTE_ELECTRIC);
            target.pad = 13;
            paper = target;
            os64_ui_theme_palette(&paper, OS64_UI_PALETTE_PAPER);
            char full[4096];
            if (mode == PARTIAL || mode == COLD_CACHE)
                startup_text = "label.fg = 123456\npad = 17\n";
            if (mode == INVALID) startup_text = "label.fg = invalid\n";
            if (mode == EMPTY) startup_text = "# No overrides\n";
            if (mode == FULL) {
                assert(os64_ui_theme_encode(&paper, full, sizeof(full)) > 0);
                startup_text = full;
            }
            os64_ui_theme_t visible[2];
            os64_ui_theme_defaults(&visible[0]);
            visible[1] = visible[0];
            os64_ui_theme_palette(&visible[1], OS64_UI_PALETTE_MIDNIGHT);
            visible[1].control_radius = 6;
            for (int i = 0; i < 2; ++i) os64_ui_theme_read_startup(&visible[i]);
            if (mode == RACING) {
                racing_length = encode(racing, &paper, 0); race_write = true;
            }
            if (mode == REFUSED) refuse_write = true;
            if (mode == READ_FAIL) startup_error = OS64_CONF_IO_ERROR;
            if (mode == ALLOC_FAIL) startup_error = OS64_CONF_NO_MEMORY;
            if (mode == SAVE_FAIL) fail_startup_save = true;
            if (mode == COLD_CACHE) {
                // Publish as another process would, without warming this cache.
                char bytes[OS64_APPEARANCE_MAX + 1];
                size_t h = os64_appearance_header_write(bytes, 0);
                int64_t n = os64_ui_theme_snapshot_startup(bytes + h, sizeof(bytes) - h);
                assert(n > 0 && appearance_publish(bytes, h + (size_t)n) > 0);
                assert(os64_ui_theme_encode(&target, full, sizeof(full)) > 0);
                startup_text = full;
            } else {
                int64_t rc = os64_ui_theme_set_startup(&target);
                bool failed = mode == REFUSED || mode == READ_FAIL ||
                              mode == ALLOC_FAIL || mode == SAVE_FAIL;
                assert(failed ? rc < 0 : rc == 0);
            }
            startup_error = 0;
            if (mode == REFUSED || mode == READ_FAIL || mode == ALLOC_FAIL) {
                assert(generation() == 0);
                _exit(0);
            }
            assert(generation() == 1);
            unsigned before = writes;
            assert(os64_ui_theme_preserve_session() == 0 && writes == before);
            for (int i = 0; i < 2; ++i) {
                os64_ui_theme_t got = visible[i];
                uint64_t installed = 0;
                assert(os64_ui_theme_session(&got, &installed, 0));
                os64_ui_theme_t expected = visible[i];
                if (mode == RACING) os64_ui_theme_merge(&expected, &paper, 3);
                assert(memcmp(&got, &expected, sizeof(got)) == 0);
                // New contexts use their own defaults for absent pinned keys,
                // with geometry still initialized from the disk configuration.
                os64_ui_theme_defaults(&got);
                if (i) {
                    os64_ui_theme_palette(&got, OS64_UI_PALETTE_MIDNIGHT);
                    got.control_radius = 6;
                }
                installed = 0;
                os64_ui_theme_current(&got, &installed);
                if (mode != SAVE_FAIL) expected.pad = target.pad;
                assert(installed == 1 && !memcmp(&got, &expected, sizeof(got)));
            }
            // Startup changes do not replace the pin; an explicit Apply does.
            fail_startup_save = false;
            assert(os64_ui_theme_set_startup(&paper) == 0 && generation() == 1);
            assert(os64_ui_theme_apply(&target, 3, NULL) == 0 && generation() == 2);
            os64_ui_t born;
            os64_ui_init(&born, NULL);
            assert(born.theme.panel_bg == target.panel_bg);
            _exit(0);
        }
        int status; assert(waitpid(pid, &status, 0) == pid);
        assert(status == 0);
    }
}

static void legacy_session_radius_contract(void)
{
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) {
        os64_ui_theme_t theme;
        os64_ui_theme_defaults(&theme);
        os64_ui_theme_palette(&theme, OS64_UI_PALETTE_ELECTRIC);
        theme.control_radius = 6;
        char bytes[OS64_APPEARANCE_MAX + 1];
        size_t n = encode(bytes, &theme, 0);
        char *line = strstr(bytes, "control.radius = 6\n");
        assert(line);
        size_t length = strlen("control.radius = 6\n");
        memmove(line, line + length, n - (size_t)(line - bytes) - length);
        n -= length;
        assert(appearance_publish(bytes, n) == (int)n);
        uint64_t installed = 0;
        assert(os64_ui_theme_session(&theme, &installed, 0));
        assert(theme.control_radius == 0);
        theme.control_radius = 6;
        os64_ui_theme_current(&theme, &installed);
        assert(theme.control_radius == 0);
        _exit(0);
    }
    int status; assert(waitpid(pid, &status, 0) == pid);
    assert(status == 0);
}

static void first_font_apply_contract(void)
{
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) {
        os64_ui_t ui; os64_ui_init(&ui, NULL);
        os64_font_config_t config; os64_font_config_defaults(&config);
        startup_text = "# custom defaults survive\nfuture.texture = silk\n";
        assert(!os64_font_settings_apply(os64_ui_font_context(&ui), &config, NULL, NULL));
        os64_ui_theme_t actual, expected;
        os64_ui_theme_defaults(&expected);
        os64_ui_theme_palette(&expected, OS64_UI_PALETTE_ELECTRIC);
        actual = expected; uint64_t installed = 0;
        os64_ui_theme_current(&actual, &installed);
        assert(installed && !memcmp(&actual, &expected, sizeof(actual)));
        char bytes[OS64_APPEARANCE_MAX + 1];
        int n = appearance_snapshot(bytes, sizeof(bytes)-1); bytes[n] = 0;
        assert(strstr(bytes, "inherit = startup\n"));
        assert(strstr(bytes, startup_text));
        os64_ui_font_release(&ui);
        _exit(0);
    }
    int status; assert(waitpid(pid, &status, 0) == pid && !status);
}

static void font_envelope_contracts(void)
{
    os64_ui_t ui;
    os64_ui_init(&ui, NULL);
    os64_text_context_t *context = os64_ui_font_context(&ui);
    assert(context);
    os64_font_config_t config, current;
    os64_font_config_defaults(&config);
    os64_ui_theme_t theme;
    os64_ui_theme_defaults(&theme);
    char bytes[OS64_APPEARANCE_MAX + 1], snapshot[OS64_APPEARANCE_MAX + 1];
    const char *extras = "# Future settings stay byte-for-byte\nfuture.texture = linen  # silk later\nfonts.ui.face = builtin\nFONTS.ui.face = builtin\n";
    size_t n = encode(bytes, &theme, generation());
    memcpy(bytes + n, extras, strlen(extras)); n += strlen(extras);
    assert(appearance_publish(bytes, n) == (int)n);
    uint64_t published, serial;
    assert(!os64_font_settings_apply(context, &config, &published, NULL));
    assert(!os64_font_settings_current(&current, &serial) && serial == published);
    int count = appearance_snapshot(snapshot, sizeof(snapshot)-1);
    assert(count > 0); snapshot[count] = 0;
    assert(strstr(snapshot, "# Future settings stay byte-for-byte\nfuture.texture = linen  # silk later\n"));
    assert(!strstr(snapshot, "FONTS.ui.face"));
    char *face = strstr(snapshot, "fonts.ui.face");
    assert(face && !strstr(face + 1, "fonts.ui.face"));
    assert(!os64_ui_theme_apply(&theme, OS64_UI_COMPONENT_PALETTE, NULL));
    assert(!os64_font_settings_current(&current, &serial) && serial == published);
    assert(!os64_font_settings_apply(context, &config, &published, NULL));
    assert(!os64_font_settings_current(&current, &serial) && serial == published);
    count = appearance_snapshot(snapshot, sizeof(snapshot)-1); snapshot[count] = 0;
    assert(strstr(snapshot, "future.texture = linen  # silk later\n"));

    // Another writer wins the exact CAS: preserve its raw bytes and retry.
    racing_length = encode(racing, &theme, generation());
    race_write = true;
    assert(os64_font_settings_apply(context, &config, NULL, NULL) == OS64_UI_APPLY_CONFLICT);
    assert(!os64_font_settings_apply(context, &config, NULL, NULL));
    uint64_t before = generation();
    refuse_write = true;
    assert(os64_font_settings_apply(context, &config, NULL, NULL) == OS64_UI_APPLY_IO);
    refuse_write = false; assert(generation() == before);
    fail_alloc = true;
    assert(os64_font_settings_apply(context, &config, NULL, NULL) != 0);
    fail_alloc = false; assert(generation() == before);

    const char *bad[] = {"unknown = x\n", "fonts.ui.size = bad\n", "fonts.ui.face = relative.ttf\n",
        "fonts.serial = 0\n", "future..bad = x\n", "= blank\n", "fonts.ui.size = 24\n"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(*bad); ++i) {
        n = encode(bytes, &theme, generation());
        memcpy(bytes + n, bad[i], strlen(bad[i])); n += strlen(bad[i]);
        assert(appearance_publish(bytes, n) == (int)n);
        before = generation();
        assert(os64_font_settings_apply(context, &config, NULL, NULL) == OS64_UI_APPLY_INVALID);
        assert(generation() == before);
        assert(!os64_ui_theme_apply(&theme, 3, NULL));
    }
    n = encode(bytes, &theme, generation());
    size_t head; uint64_t ignored;
    assert(os64_appearance_header_read(bytes, n, &ignored, &head));
    // Comments count toward the complete envelope limit too.
    bytes[n++] = '#';
    memset(bytes+n, 'x', head + OS64_APPEARANCE_PAYLOAD_MAX - n);
    n = head + OS64_APPEARANCE_PAYLOAD_MAX;
    assert(appearance_publish(bytes, n) == (int)n);
    before = generation();
    assert(os64_font_settings_apply(context, &config, NULL, NULL) == OS64_UI_APPLY_INVALID);
    assert(generation() == before);
    publish(&theme);
    os64_ui_font_release(&ui);
    puts("font envelopes: preserving component CAS, reload serials, malformed input, conflicts, allocation/I/O failure and 4096-byte refusal passed");
}

static void role_specific_fit_contract(void)
{
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) {
        os64_ui_t ui; os64_ui_init(&ui, NULL);
        os64_ui_widget_t root, button;
        os64_ui_panel(&root); os64_ui_set_root(&ui, &root);
        os64_ui_button(&button, "Test", NULL, NULL);
        button.bounds = (os64_gui_rect_t){0,0,100,18};
        os64_ui_add_child(&root, &button);
        /* Initial adoption must still validate even the builtin row. */
        assert(os64_ui_font_follow(&ui) != 0);
        button.bounds.h = 20;
        assert(!os64_ui_font_follow(&ui));
        os64_font_config_t config; os64_font_config_defaults(&config);
        strcpy(config.roles[0].face[0], "/test/scalable");
        assert(!os64_font_settings_apply(os64_ui_font_context(&ui), &config, NULL, NULL));
        assert(!os64_ui_font_follow(&ui));
        /* Restore a smaller window after acceptance. Terminal-only changes
         * must not re-refuse an already installed interface line height. */
        button.bounds.h = 18;
        strcpy(config.roles[1].face[0], "/test/mono"); config.roles[1].size = 24;
        assert(!os64_font_settings_apply(os64_ui_font_context(&ui), &config, NULL, NULL));
        assert(!os64_ui_font_follow(&ui));
        strcpy(config.roles[2].face[0], "/test/scalable"); config.roles[2].size = 28;
        assert(!os64_font_settings_apply(os64_ui_font_context(&ui), &config, NULL, NULL));
        assert(!os64_ui_font_follow(&ui));
        assert(os64_ui_font_row_height(&ui, OS64_FONT_ROLE_DOCUMENT) == 28);
        uint64_t accepted = ui.font_generation;
        /* A changed interface height still fails atomically and remains
         * retryable at the same generation after the window grows. */
        config.roles[0].size = 24;
        assert(!os64_font_settings_apply(os64_ui_font_context(&ui), &config, NULL, NULL));
        assert(os64_ui_font_follow(&ui) != 0 && ui.font_generation == accepted);
        assert(os64_ui_font_row_height(&ui, OS64_FONT_ROLE_UI) == 16);
        button.bounds.h = 28;
        assert(!os64_ui_font_follow(&ui));
        assert(os64_ui_font_row_height(&ui, OS64_FONT_ROLE_UI) == 24);
        /* Refusal is still necessary when the interface grows on a later
         * full-set publication, even if another role also changes. */
        config.roles[0].size = 30; config.roles[1].size = 26;
        assert(!os64_font_settings_apply(os64_ui_font_context(&ui), &config, NULL, NULL));
        assert(os64_ui_font_follow(&ui) != 0);
        os64_ui_font_release(&ui);
        _exit(0);
    }
    int status; assert(waitpid(pid, &status, 0) == pid && !status);
    puts("font fit: unchanged interface rows allow terminal/document changes; initial, changed-height and retry checks passed");
}

void appearance_session_contracts(void)
{
    role_specific_fit_contract();
    first_font_apply_contract();
    legacy_session_radius_contract();
    startup_preservation_contracts();
    store_contracts();
    client_contracts();
    concurrency_and_queue_contracts();
    font_envelope_contracts();
    puts("appearance: startup preservation, session store, cache, races, independent contexts, and queue pressure passed");
}

/* Two one-byte backend fixtures supply fonts for session/adoption tests.
 * The separate configuration/installation suite exercises real font files. */
int64_t __wrap_os64_conf_find(const char *name, char *out, size_t cap)
{
    if (strcmp(name,"theme.conf") || (!startup_text && !startup_error)) return OS64_CONF_NO_FILE;
    os64_strcopy(out,cap,"/test/theme.conf"); return 0;
}
os64_slurp_status_t __real_os64_slurp(const char *, size_t, uint8_t **, size_t *);
os64_slurp_status_t __wrap_os64_slurp(const char *path, size_t cap, uint8_t **out, size_t *length)
{
    if (strcmp(path,"/test/theme.conf")) return __real_os64_slurp(path,cap,out,length);
    *out = NULL; *length = 0;
    if (startup_error) return startup_error == OS64_CONF_NO_MEMORY ?
        OS64_SLURP_NO_MEMORY : OS64_SLURP_IO_ERROR;
    assert(startup_text);
    size_t n = strlen(startup_text);
    if (n > cap) return OS64_SLURP_TOO_BIG;
    *out = os64_malloc(n+1);
    if (!*out) return OS64_SLURP_NO_MEMORY;
    memcpy(*out,startup_text,n+1); *length = n; return OS64_SLURP_OK;
}
int64_t __wrap_os64_conf_target(const char *name, char *out, size_t cap)
{ int n = snprintf(out, cap, "/home/%s", name); return n < 0 || (size_t)n >= cap ? -1 : 0; }
int64_t os64_stat(const char *path, os64_dirent_t *entry)
{
    if (strcmp(path,"/test/scalable") && strcmp(path,"/test/mono")) return -1;
    *entry = (os64_dirent_t){.size = 1}; return 0;
}
int64_t os64_opendir(const char *path) { (void)path; return -1; }
int64_t os64_readdir(int32_t fd, os64_dirent_t *entry) { (void)fd; (void)entry; return 0; }
int64_t os64_mkdir(const char *path) { (void)path; return -1; }
int64_t os64_sync(int32_t fd) { (void)fd; return -1; }
int64_t os64_unlink(const char *path) { (void)path; return -1; }
int64_t os64_rename(const char *a, const char *b) { (void)a; (void)b; return -1; }
int64_t os64_rename_with_flags(const char *a, const char *b, uint64_t flags)
{ (void)a; (void)b; (void)flags; return -1; }
uint64_t os64_taskid(void) { return 1; }

int64_t __wrap_os64_conf_find_bytes(const char *name, char **out, size_t *length)
{
    assert(!strcmp(name,"theme.conf"));
    *out = NULL; *length = 0;
    if (startup_error) return startup_error;
    if (!startup_text) return OS64_CONF_NO_FILE;
    *out = os64_malloc(strlen(startup_text)+1);
    if (!*out) return OS64_CONF_NO_MEMORY;
    strcpy(*out,startup_text); *length = strlen(startup_text); return 0;
}
