// settings.c — yonder's Settings window (settings.h).

#include <stddef.h>

#include "settings.h"
#include "agent.h"
#include "os64/conf.h"
#include "os64/fmt.h"
#include "os64/gui.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/proc.h"
#include "os64/str.h"
#include "os64/thread.h"
#include "os64/ui_settings.h"

#define CONF_NAME "yonder.conf"
#define CONF_AGENT "agent"

// The list's height, in rows; the dialog's rows are a line of words, the
// list, the field and a line of help.
#define LIST_ROWS 6
#define BODY_ROWS (LIST_ROWS + 3)

static struct {
    os64_ui_settings_t d;
    os64_ui_widget_t heading, help;
    os64_ui_listbox_t list;
    os64_ui_textfield_t field;
    char field_buf[YONDER_AGENT_MAX];
    void (*use)(const char *agent);
    // The relay: `thread` waits for events at the dialog's window and
    // rings `bell` on `window`, then waits on `pumped` until the loop has
    // handled them — one ring outstanding at a time, so it never spins on
    // events the loop has not taken yet.
    int64_t window, thread;
    uint32_t bell;
    bool piped;
    int32_t pumped[2];
} s;

// Every typed agent ever applied, newest first (yonder_agent_keep).
typedef struct Kept {
    struct Kept *next;
    char agent[];
} Kept;
static Kept *s_kept;

const char *yonder_agent_keep(const char *agent)
{
    for (size_t i = 0; i < yonder_agent_npresets(); i++)
        if (os64_streq(agent, yonder_agent_preset(i)->agent))
            return yonder_agent_preset(i)->agent;
    for (Kept *k = s_kept; k != NULL; k = k->next)
        if (os64_streq(agent, k->agent))
            return k->agent;
    size_t n = os64_strlen(agent) + 1;
    Kept *k = os64_malloc(sizeof(*k) + n);
    if (k == NULL)
        return NULL;
    os64_memcpy(k->agent, agent, n);
    k->next = s_kept;
    s_kept = k;
    return k->agent;
}

void yonder_agents_release(void)
{
    while (s_kept != NULL) {
        Kept *k = s_kept;
        s_kept = k->next;
        os64_free(k);
    }
}

bool yonder_settings_saved_agent(char *out, size_t cap)
{
    int64_t rc = os64_conf_get(CONF_NAME, CONF_AGENT, out, cap);
    if (rc == OS64_CONF_NO_FILE || rc == OS64_CONF_NO_KEY)
        return false;
    if (rc != 0 || !yonder_agent_valid(out)) {
        os64_debug_log("yonder: yonder.conf's agent is unreadable or not an agent; using yonder's");
        return false;
    }
    return true;
}

// ── The relay ───────────────────────────────────────────────────────────

// `arg` is the dialog's window, handed over rather than read from `s`,
// which the loop clears when it closes the dialog.
static int64_t relay(void *arg)
{
    int64_t dialog = (int64_t)(intptr_t)arg;
    for (;;) {
        // Waits without taking: the events stay queued for the pump.
        if (os64_gui_event_wait(dialog, NULL) != 1)
            return 0;
        (void)os64_gui_event_ring(s.window, s.bell);
        char byte;
        if (os64_read_for(s.pumped[0], &byte, 1, OS64_WAIT_FOREVER) != 1)
            return 0;
    }
}

// The relay's end: the dialog's window is gone, so its wait answers at
// once, and a byte ends a wait for the loop.
static void relay_stop(void)
{
    if (s.thread > 0) {
        const char byte = 1;
        (void)os64_write(s.pumped[1], &byte, 1);
        (void)os64_thread_join((int32_t)s.thread, NULL);
        os64_close((int32_t)s.thread);
    }
    if (s.piped) {
        os64_close(s.pumped[0]);
        os64_close(s.pumped[1]);
    }
    s.thread = 0;
    s.piped = false;
}

// ── The dialog ──────────────────────────────────────────────────────────

static const char *preset_label(size_t i, void *user)
{
    (void)user;
    const yonder_agent_preset_t *p = yonder_agent_preset(i);
    return p != NULL ? p->name : "";
}

static void picked(os64_ui_listbox_t *list, void *user)
{
    (void)user;
    const yonder_agent_preset_t *p = list->selected >= 0 ? yonder_agent_preset((size_t)list->selected)
                                                        : NULL;
    if (p != NULL)
        os64_ui_textfield_set(&s.d.ui, &s.field, p->agent);
}

static void place(os64_ui_widget_t *w, os64_gui_rect_t r, bool staged)
{
    if (staged)
        os64_ui_widget_stage_bounds(w, r);
    else
        w->bounds = r;
}

static bool arrange(os64_ui_settings_t *d, os64_gui_rect_t b, int32_t row, bool staged)
{
    const os64_ui_widget_t *words[] = {&s.heading, &s.help};
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        int32_t width;
        const char *text = words[i]->text;
        if (os64_ui_text_measure(&d->ui, OS64_FONT_ROLE_UI, text, os64_strlen(text), &width) ||
            width > b.w)
            return false;
    }
    place(&s.heading, (os64_gui_rect_t){b.x, b.y, b.w, row}, staged);
    place(&s.list.w, (os64_gui_rect_t){b.x, b.y + row, b.w, LIST_ROWS * row}, staged);
    place(&s.field.w, (os64_gui_rect_t){b.x, b.y + (LIST_ROWS + 1) * row, b.w, row}, staged);
    place(&s.help, (os64_gui_rect_t){b.x, b.y + (LIST_ROWS + 2) * row, b.w, row}, staged);
    return true;
}

static void apply(os64_ui_settings_t *d, bool save)
{
    const char *typed = s.field_buf;
    if (!yonder_agent_valid(typed)) {
        os64_ui_settings_report(d, "An agent is printable ASCII, without #, under 256 characters.");
        return;
    }
    const char *agent = yonder_agent_keep(typed);
    if (agent == NULL) {
        os64_ui_settings_report(d, "No memory to keep that agent; nothing changed.");
        return;
    }
    s.use(agent);
    const char *name = yonder_agent_name(agent);
    char line[160];
    if (save && os64_conf_set(CONF_NAME, CONF_AGENT, agent) != 0)
        os64_strcopy(line, sizeof(line), "Applied here, but yonder.conf could not be written.");
    else
        os64_snprintf(line, sizeof(line), "%s: sites are asked as %s from the next page on%s",
                      save ? "Saved" : "Applied", name != NULL ? name : "the agent typed",
                      save ? ", and in new windows." : "; Reload asks again.");
    os64_ui_settings_report(d, line);
}

void yonder_settings_open(int64_t parent, uint32_t bell, const char *agent,
                          void (*use)(const char *agent))
{
    // Open already: the helper brings it forward and keeps what was typed.
    if (!os64_ui_settings_open(&s.d, parent, "yonder Settings", BODY_ROWS, arrange, apply, NULL))
        return;
    s.use = use;
    s.window = parent;
    s.bell = bell;
    os64_ui_label(&s.heading, "Identify as - what every site is told this browser is:");
    os64_ui_label(&s.help, "Pick one to fill the field, or type another.");
    int selected = -1;
    for (size_t i = 0; i < yonder_agent_npresets(); i++)
        if (os64_streq(agent, yonder_agent_preset(i)->agent))
            selected = (int)i;
    os64_ui_listbox(&s.list, yonder_agent_npresets(), preset_label, picked, NULL);
    os64_ui_textfield(&s.field, s.field_buf, sizeof(s.field_buf), NULL, NULL, NULL);
    os64_ui_widget_t *kids[] = {&s.heading, &s.list.w, &s.field.w, &s.help};
    for (size_t i = 0; i < sizeof(kids) / sizeof(kids[0]); i++)
        os64_ui_add_child(&s.d.body, kids[i]);
    os64_ui_listbox_set(&s.d.ui, &s.list, yonder_agent_npresets(), selected);
    os64_ui_textfield_set(&s.d.ui, &s.field, agent);
    os64_ui_settings_ready(&s.d);
    os64_ui_set_focus(&s.d.ui, &s.list.w);
    os64_ui_paint(&s.d.ui);
    // Without the relay the dialog would answer only when yonder's own
    // window had an event: it is closed rather than left half alive.
    if (os64_pipe(s.pumped) < 0) {
        os64_ui_settings_close(&s.d);
        return;
    }
    s.piped = true;
    s.thread = os64_thread(relay, (void *)(intptr_t)s.d.window);
    if (s.thread < 0) {
        s.thread = 0;
        os64_ui_settings_close(&s.d);
        relay_stop();
    }
}

void yonder_settings_rung(void)
{
    if (s.d.window <= 0)
        return;
    os64_ui_settings_pump(&s.d);
    if (s.d.window <= 0) {
        relay_stop();
        return;
    }
    const char byte = 1;
    (void)os64_write(s.pumped[1], &byte, 1);
}

void yonder_settings_close(void)
{
    if (s.d.window > 0)
        os64_ui_settings_close(&s.d);
    relay_stop();
}
