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
#include "os64/signal.h"
#include "appearance.h"
#include "gui/event_queue.h"

pthread_mutex_t kGuiLock = PTHREAD_MUTEX_INITIALIZER;
bool fail_alloc;
static unsigned allocations, reads, writes, notifications;
static bool fail_open, fail_read, refuse_write, short_write, race_write;
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
    (void)name; (void)fn; (void)user; (void)path; (void)cap;
    return OS64_CONF_NO_FILE;
}
int64_t os64_open(const char *path, const char *mode)
{
    assert(strcmp(path, OS64_APPEARANCE_PATH) == 0);
    if (fail_open) return -1;
    for (int fd = 3; fd < 16; ++fd) {
        if (handles[fd].open) continue;
        handles[fd].open = true;
        handles[fd].writer = mode && *mode == 'w';
        handles[fd].pos = 0;
        if (!handles[fd].writer) {
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
    assert(os64_ui_dispatch(&b, &ev));
    assert(os64_ui_dispatch(&preview, &ev));
    assert(reads == read_before + 1 && allocations == alloc_before + 1);
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
    os64_ui_dispatch(&a, &ev); os64_ui_dispatch(&b, &ev);
    assert(reads == read_before + 1 && allocations == alloc_before + 1);
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
    assert(reads == before_reads+1 && allocations == before_allocs+1);
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
int os64_ui_theme_preserve_session(void);
void appearance_session_contracts(void)
{
    // Separate address spaces keep the production boot-lifetime store/cache
    // pristine for each initial-publication and racing-publisher scenario.
    for (int mode = 0; mode < 3; ++mode) {
        pid_t pid = fork(); assert(pid >= 0);
        if (!pid) {
            os64_ui_theme_t theme; os64_ui_theme_defaults(&theme);
            os64_ui_theme_palette(&theme, OS64_UI_PALETTE_ELECTRIC);
            if (mode == 1) {
                racing_length = encode(racing, &theme, 0); race_write = true;
            }
            if (mode == 2) refuse_write = true;
            int rc = os64_ui_theme_preserve_session();
            if (mode == 2) assert(rc == OS64_UI_APPLY_IO && generation() == 0);
            else {
                assert(rc == 0 && generation() == 1);
                unsigned before = writes;
                assert(os64_ui_theme_preserve_session() == 0 && writes == before);
                if (mode == 1) {
                    os64_ui_theme_t got; os64_ui_theme_defaults(&got);
                    uint64_t installed = 0;
                    assert(os64_ui_theme_session(&got, &installed, 0));
                    assert(got.panel_bg == theme.panel_bg);
                }
            }
            _exit(0);
        }
        int status; assert(waitpid(pid, &status, 0) == pid);
        assert(status == 0);
    }
    store_contracts();
    client_contracts();
    concurrency_and_queue_contracts();
    puts("appearance: session store, cache, races, independent contexts, and queue pressure passed");
}
