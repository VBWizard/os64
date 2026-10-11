#include "os64/ui_action.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "ui_message.h"

#define MODS (OS64_GUI_MOD_SHIFT | OS64_GUI_MOD_CTRL | OS64_GUI_MOD_ALT)
#define SPEC_BUDGET (128u * 1024u)
#define STATE_BUDGET (64u * 1024u)

typedef struct action_binding {
    os64_ui_widget_t *button;
    os64_ui_action_view_t *view;
    uint32_t id;
    struct action_binding *next;
} action_binding;
struct os64_ui_actions {
    os64_ui_action_callbacks_t callbacks;
    void *app;
    size_t count, references, views, state_bytes;
    os64_ui_action_spec_t *specs;
};
struct os64_ui_action_view {
    os64_ui_actions_t *actions;
    os64_ui_action_context_t context;
    size_t references, bytes;
    os64_ui_status_code_t validity;
    bool refreshing;
    action_binding *bindings;
    os64_ui_action_state_t *states, *candidate;
};


static uint16_t lower(uint16_t k) { return k >= 'A' && k <= 'Z' ? k + 'a' - 'A' : k; }
static os64_ui_status_code_t normalize(os64_ui_shortcut_t *s)
{
    s->key = lower(s->key);
    if (s->modifiers & ~MODS) return OS64_UI_INVALID;
    if (!s->key) return s->modifiers ? OS64_UI_INVALID : OS64_UI_OK;
    if (!((s->key >= 32 && s->key < 127) ||
          (s->key >= OS64_UI_KEY_ESCAPE && s->key <= OS64_UI_KEY_F12)))
        return OS64_UI_INVALID;
    if ((s->modifiers & (OS64_GUI_MOD_CTRL | OS64_GUI_MOD_ALT)) ==
        (OS64_GUI_MOD_CTRL | OS64_GUI_MOD_ALT)) return OS64_UI_INVALID;
    if ((s->modifiers & OS64_GUI_MOD_ALT) &&
        (s->key == OS64_UI_KEY_TAB || s->key == OS64_UI_KEY_F4 ||
         s->key == OS64_UI_KEY_LEFT || s->key == OS64_UI_KEY_RIGHT)) return OS64_UI_INVALID;
    return OS64_UI_OK;
}
static size_t find(const os64_ui_actions_t *a, uint32_t id)
{
    for (size_t i = 0; i < a->count; ++i) if (a->specs[i].id == id) return i;
    return a->count;
}

os64_ui_status_code_t os64_ui_actions_create(const os64_ui_action_spec_t *specs,
    size_t count, const os64_ui_action_callbacks_t *cb, void *app, os64_ui_actions_t **out)
{
    if (!out) return OS64_UI_INVALID;
    *out = NULL;
    if (!specs || !count || !cb || !cb->query || !cb->invoke) return OS64_UI_INVALID;
    if (count > OS64_UI_ACTIONS_MAX) return OS64_UI_LIMIT;
    size_t lengths[OS64_UI_ACTIONS_MAX][3];
    os64_ui_shortcut_t shortcuts[OS64_UI_ACTIONS_MAX];
    size_t bytes = sizeof(os64_ui_actions_t) + count * sizeof(*specs);
    for (size_t i = 0; i < count; ++i) {
        const os64_ui_action_spec_t *s = &specs[i];
        if (!s->id || s->kind > OS64_UI_ACTION_RADIO || s->kind < OS64_UI_ACTION_COMMAND ||
            (s->kind == OS64_UI_ACTION_RADIO ? !s->radio_group : s->radio_group != 0) ||
            (s->mnemonic && (s->mnemonic < 32 || s->mnemonic >= 127))) return OS64_UI_INVALID;
        const char *strings[] = {s->label, s->help ? s->help : "", s->icon ? s->icon : ""};
        const size_t caps[] = {128, 256, 48};
        for (size_t j = 0; j < 3; ++j) {
            os64_ui_status_code_t rc = ui_message_length(strings[j], caps[j], &lengths[i][j]);
            if (rc != OS64_UI_OK) return rc;
            bytes += lengths[i][j] + 1;
        }
        if (!lengths[i][0]) return OS64_UI_INVALID;
        shortcuts[i] = s->shortcut;
        if (normalize(&shortcuts[i]) != OS64_UI_OK) return OS64_UI_INVALID;
        for (size_t j = 0; j < i; ++j)
            if (s->id == specs[j].id || (shortcuts[i].key &&
                shortcuts[i].key == shortcuts[j].key &&
                shortcuts[i].modifiers == shortcuts[j].modifiers)) return OS64_UI_DUPLICATE;
    }
    if (bytes > SPEC_BUDGET) return OS64_UI_LIMIT;
    os64_ui_actions_t *a = os64_malloc(bytes);
    if (!a) return OS64_UI_NO_MEMORY;
    *a = (os64_ui_actions_t){.callbacks=*cb, .app=app, .count=count};
    a->specs = (void *)(a + 1);
    char *text = (char *)(a->specs + count);
    for (size_t i = 0; i < count; ++i) {
        a->specs[i] = specs[i];
        a->specs[i].shortcut = shortcuts[i];
        a->specs[i].mnemonic = (char)lower((uint8_t)specs[i].mnemonic);
        const char *strings[] = {specs[i].label, specs[i].help ? specs[i].help : "",
                                specs[i].icon ? specs[i].icon : ""};
        const char **dest[] = {&a->specs[i].label, &a->specs[i].help, &a->specs[i].icon};
        for (size_t j = 0; j < 3; ++j) {
            *dest[j] = text;
            os64_memcpy(text, strings[j], lengths[i][j] + 1);
            text += lengths[i][j] + 1;
        }
        if (!specs[i].icon) a->specs[i].icon = NULL;
    }
    *out = a;
    return OS64_UI_OK;
}
os64_ui_status_code_t os64_ui_actions_retain(os64_ui_actions_t *a)
{
    if (!a) return OS64_UI_INVALID;
    if (a->references == SIZE_MAX) return OS64_UI_LIMIT;
    ++a->references;
    return OS64_UI_OK;
}
void os64_ui_actions_release(os64_ui_actions_t *a) { if (a && a->references) --a->references; }
os64_ui_status_code_t os64_ui_actions_destroy(os64_ui_actions_t *a)
{
    if (!a) return OS64_UI_INVALID;
    if (a->references) return OS64_UI_BUSY;
    os64_free(a);
    return OS64_UI_OK;
}
os64_ui_status_code_t os64_ui_action_spec(const os64_ui_actions_t *a, uint32_t id,
                                         os64_ui_action_spec_t *out)
{
    if (!a || !out) return OS64_UI_INVALID;
    size_t i = find(a, id);
    if (i == a->count) return OS64_UI_INVALID;
    *out = a->specs[i];
    return OS64_UI_OK;
}
os64_ui_status_code_t os64_ui_action_view_retain(os64_ui_action_view_t *v)
{
    if (!v) return OS64_UI_INVALID;
    if (v->references == SIZE_MAX) return OS64_UI_LIMIT;
    ++v->references;
    return OS64_UI_OK;
}
void os64_ui_action_view_release(os64_ui_action_view_t *v) { if (v && v->references) --v->references; }

static void sync_buttons(os64_ui_action_view_t *v)
{
    for (action_binding *b = v->bindings; b; b = b->next) {
        bool enabled = v->validity == OS64_UI_OK && v->states[find(v->actions, b->id)].enabled;
        if (b->button->ui) os64_ui_set_enabled(b->button->ui, b->button, enabled);
        else b->button->disabled = !enabled;
    }
}
static os64_ui_status_code_t refresh(os64_ui_action_view_t *v, bool sync)
{
    if (!v) return OS64_UI_INVALID;
    if (v->refreshing) return OS64_UI_BUSY;
    v->refreshing = true;
    os64_ui_status_code_t rc = OS64_UI_OK;
    if (!v->context.validate(v->context.cookie, v->context.generation)) rc = OS64_UI_STALE;
    if (rc == OS64_UI_OK) {
        for (size_t i = 0; i < v->actions->count; ++i) {
            v->candidate[i] = (os64_ui_action_state_t){0};
            v->actions->callbacks.query(v->actions->app, &v->context,
                v->actions->specs[i].id, &v->candidate[i]);
            const os64_ui_action_spec_t *s = &v->actions->specs[i];
            if (s->kind == OS64_UI_ACTION_COMMAND && v->candidate[i].checked) rc = OS64_UI_INVALID;
            if (s->kind == OS64_UI_ACTION_RADIO && v->candidate[i].checked)
                for (size_t j = 0; j < i; ++j)
                    if (v->actions->specs[j].radio_group == s->radio_group &&
                        v->candidate[j].checked) rc = OS64_UI_INVALID;
        }
    }
    v->validity = rc;
    if (rc == OS64_UI_OK) os64_memcpy(v->states, v->candidate,
                                     v->actions->count * sizeof(*v->states));
    else os64_memset(v->states, 0, v->actions->count * sizeof(*v->states));
    v->refreshing = false;
    if (sync) sync_buttons(v);
    return rc;
}
os64_ui_status_code_t os64_ui_actions_refresh(os64_ui_action_view_t *v) { return refresh(v, true); }
os64_ui_status_code_t os64_ui_action_view_create(os64_ui_actions_t *a,
    const os64_ui_action_context_t *context, os64_ui_action_view_t **out)
{
    if (!out) return OS64_UI_INVALID;
    *out = NULL;
    if (!a || !context || !context->retain || !context->release || !context->validate)
        return OS64_UI_INVALID;
    size_t bytes = sizeof(os64_ui_action_view_t) + 2 * a->count * sizeof(os64_ui_action_state_t);
    if (a->views == OS64_UI_ACTION_VIEWS_MAX || bytes > STATE_BUDGET - a->state_bytes)
        return OS64_UI_LIMIT;
    if (!context->retain(context->cookie, context->generation)) return OS64_UI_REFUSED;
    os64_ui_action_view_t *v = os64_malloc(bytes);
    if (!v) { context->release(context->cookie, context->generation); return OS64_UI_NO_MEMORY; }
    *v = (os64_ui_action_view_t){.actions=a, .context=*context, .bytes=bytes};
    v->states = (void *)(v + 1);
    v->candidate = v->states + a->count;
    os64_ui_status_code_t rc = os64_ui_actions_retain(a);
    if (rc != OS64_UI_OK) { os64_free(v); context->release(context->cookie, context->generation); return rc; }
    ++a->views; a->state_bytes += bytes;
    rc = refresh(v, false);
    if (rc != OS64_UI_OK) { os64_ui_action_view_destroy(v); return rc; }
    *out = v;
    return OS64_UI_OK;
}
os64_ui_status_code_t os64_ui_action_view_destroy(os64_ui_action_view_t *v)
{
    if (!v) return OS64_UI_INVALID;
    if (v->references || v->refreshing) return OS64_UI_BUSY;
    os64_ui_actions_t *a = v->actions;
    v->context.release(v->context.cookie, v->context.generation);
    --a->views; a->state_bytes -= v->bytes;
    os64_ui_actions_release(a);
    os64_free(v);
    return OS64_UI_OK;
}
os64_ui_status_code_t os64_ui_action_snapshot(const os64_ui_action_view_t *v,
    uint32_t id, os64_ui_action_state_t *out)
{
    if (!v || !out) return OS64_UI_INVALID;
    *out = (os64_ui_action_state_t){0};
    size_t i = find(v->actions, id);
    if (i == v->actions->count) return OS64_UI_INVALID;
    *out = v->states[i];
    return v->validity;
}
os64_ui_action_result_t os64_ui_action_invoke(os64_ui_action_view_t *v, uint32_t id)
{
    os64_ui_action_result_t result = {OS64_UI_INVALID, 0};
    if (!v || find(v->actions, id) == v->actions->count) return result;
    result.status = os64_ui_action_view_retain(v);
    if (result.status != OS64_UI_OK) return result;
    result.status = refresh(v, true);
    if (result.status == OS64_UI_OK) {
        if (!v->states[find(v->actions, id)].enabled) result.status = OS64_UI_REFUSED;
        else {
            result = v->actions->callbacks.invoke(v->actions->app, &v->context, id);
            /* Only application semantic state is read after invoke. Presentation
             * refresh is the app's end-of-dispatch responsibility. */
            (void)refresh(v, false);
        }
    }
    os64_ui_action_view_release(v);
    return result;
}

static void bound_click(os64_ui_widget_t *button, void *user)
{
    (void)button;
    action_binding *b = user;
    os64_ui_action_view_t *v = b->view;
    uint32_t id = b->id;
    (void)os64_ui_action_invoke(v, id);
}
os64_ui_status_code_t os64_ui_action_bind(os64_ui_widget_t *w,
    os64_ui_action_view_t *v, uint32_t id)
{
    if (!w || !v || w->cls != &os64_ui_button_class) return OS64_UI_INVALID;
    if (w->on_click == bound_click) return OS64_UI_DUPLICATE;
    size_t i = find(v->actions, id);
    if (i == v->actions->count) return OS64_UI_INVALID;
    action_binding *b = os64_malloc(sizeof(*b));
    if (!b) return OS64_UI_NO_MEMORY;
    os64_ui_widget_t candidate = *w;
    candidate.text = v->actions->specs[i].label;
    candidate.run_staged = NULL;
    os64_font_status_t fs = os64_ui_stage_caption(&candidate, w->ui);
    if (fs != OS64_FONT_OK) { os64_free(b); return ui_message_font_result(fs); }
    os64_ui_status_code_t rc = os64_ui_action_view_retain(v);
    if (rc != OS64_UI_OK) { os64_ui_discard_caption(&candidate); os64_free(b); return rc; }
    *b = (action_binding){w, v, id, v->bindings};
    v->bindings = b;
    w->pressed = false; w->activation_key = 0;
    if (w->ui && w->ui->grab == w) w->ui->grab = NULL;
    os64_ui_discard_caption(w);
    os64_ui_run_release(w->run);
    w->run = candidate.run_staged;
    w->text = candidate.text; w->on_click = bound_click; w->user = b;
    sync_buttons(v);
    if (w->ui) os64_ui_mark_dirty(w->ui, w);
    return OS64_UI_OK;
}
os64_ui_status_code_t os64_ui_action_unbind(os64_ui_widget_t *w)
{
    if (!w || w->on_click != bound_click) return OS64_UI_INVALID;
    action_binding *b = w->user;
    action_binding **link = &b->view->bindings;
    while (*link != b) link = &(*link)->next;
    *link = b->next;
    w->pressed = false; w->activation_key = 0;
    if (w->ui && w->ui->grab == w) w->ui->grab = NULL;
    os64_ui_run_release(w->run); w->run = NULL;
    os64_ui_discard_caption(w);
    w->text = NULL; w->on_click = NULL; w->user = NULL;
    if (w->ui) os64_ui_mark_dirty(w->ui, w);
    os64_ui_action_view_release(b->view);
    os64_free(b);
    return OS64_UI_OK;
}

static uint16_t physical_key(const os64_gui_event_t *ev)
{
    uint8_t sc = ev->key.scancode;
    if (ev->key.modifiers & OS64_GUI_MOD_HID) {
        if (sc >= 0x3a && sc <= 0x45) return OS64_UI_KEY_F1 + sc - 0x3a;
        switch (sc) {
        case 0x29: return OS64_UI_KEY_ESCAPE; case 0x28: return OS64_UI_KEY_ENTER;
        case 0x2b: return OS64_UI_KEY_TAB; case 0x2a: return OS64_UI_KEY_BACKSPACE;
        case 0x4c: return OS64_UI_KEY_DELETE; case 0x49: return OS64_UI_KEY_INSERT;
        case 0x52: return OS64_UI_KEY_UP; case 0x51: return OS64_UI_KEY_DOWN;
        case 0x50: return OS64_UI_KEY_LEFT; case 0x4f: return OS64_UI_KEY_RIGHT;
        case 0x4a: return OS64_UI_KEY_HOME; case 0x4d: return OS64_UI_KEY_END;
        case 0x4b: return OS64_UI_KEY_PAGE_UP; case 0x4e: return OS64_UI_KEY_PAGE_DOWN;
        }
    } else {
        if (sc >= 0x3b && sc <= 0x44) return OS64_UI_KEY_F1 + sc - 0x3b;
        if (sc == 0x57 || sc == 0x58) return OS64_UI_KEY_F11 + sc - 0x57;
        switch (sc) {
        case 0x01: return OS64_UI_KEY_ESCAPE; case 0x1c: return OS64_UI_KEY_ENTER;
        case 0x0f: return OS64_UI_KEY_TAB; case 0x0e: return OS64_UI_KEY_BACKSPACE;
        case 0x53: return OS64_UI_KEY_DELETE; case 0x52: return OS64_UI_KEY_INSERT;
        case 0x48: return OS64_UI_KEY_UP; case 0x50: return OS64_UI_KEY_DOWN;
        case 0x4b: return OS64_UI_KEY_LEFT; case 0x4d: return OS64_UI_KEY_RIGHT;
        case 0x47: return OS64_UI_KEY_HOME; case 0x4f: return OS64_UI_KEY_END;
        case 0x49: return OS64_UI_KEY_PAGE_UP; case 0x51: return OS64_UI_KEY_PAGE_DOWN;
        }
    }
    return OS64_UI_KEY_NONE;
}
static bool navigation(uint16_t k)
{
    return k >= OS64_UI_KEY_DELETE && k <= OS64_UI_KEY_PAGE_DOWN;
}
static bool burst_byte(uint16_t key, uint8_t ascii)
{
    if (!navigation(key)) return false;
    if (ascii == 0x1b || ascii == '[') return true;
    switch (key) {
    case OS64_UI_KEY_UP: return ascii == 'A';
    case OS64_UI_KEY_DOWN: return ascii == 'B';
    case OS64_UI_KEY_LEFT: return ascii == 'D';
    case OS64_UI_KEY_RIGHT: return ascii == 'C';
    case OS64_UI_KEY_HOME: return ascii == 'H';
    case OS64_UI_KEY_END: return ascii == 'F';
    case OS64_UI_KEY_DELETE: return ascii == '3' || ascii == '~';
    case OS64_UI_KEY_INSERT: return ascii == '2' || ascii == '~';
    case OS64_UI_KEY_PAGE_UP: return ascii == '5' || ascii == '~';
    case OS64_UI_KEY_PAGE_DOWN: return ascii == '6' || ascii == '~';
    default: return false;
    }
}
bool os64_ui_shortcut_match(os64_ui_shortcut_t s, const os64_gui_event_t *ev)
{
    if (!ev || ev->type != OS64_GUI_EVENT_KEY_DOWN || normalize(&s) != OS64_UI_OK || !s.key)
        return false;
    if ((ev->key.modifiers & MODS) != s.modifiers) return false;
    uint16_t p = physical_key(ev), k = 0;
    uint8_t a = (uint8_t)ev->key.ascii;
    /* Bursts share the navigation key's identity across all bytes. Reject
     * their bytes; they belong to the widget decoder, including repeats.
     * PS/2 keypad digits share make codes with navigation, so ordinary
     * printable keypad input still matches by ASCII. */
    if (burst_byte(p, a)) return false;
    if ((p == OS64_UI_KEY_TAB && a == '\t') ||
        (p == OS64_UI_KEY_ENTER && (a == '\n' || a == '\r')) ||
        (p == OS64_UI_KEY_BACKSPACE && a == '\b') ||
        (p == OS64_UI_KEY_ESCAPE && a == 0x1b)) k = p;
    else if ((ev->key.modifiers & OS64_GUI_MOD_CTRL) && a >= 1 && a <= 26)
        k = 'a' + a - 1;
    else if (a >= 32 && a < 127) k = lower(a);
    else k = p;
    return s.key == k;
}
void os64_ui_shortcut_reset(os64_ui_shortcut_router_t *r)
{
    if (r) os64_memset(r, 0, sizeof(*r));
}
bool os64_ui_shortcut_route(os64_ui_shortcut_router_t *r, os64_ui_action_view_t *v,
    const os64_gui_event_t *ev, os64_ui_action_result_t *out)
{
    if (out) *out = (os64_ui_action_result_t){OS64_UI_OK, 0};
    if (!r || !ev) return false;
    if ((ev->type == OS64_GUI_EVENT_WINDOW_FOCUS && !ev->focus.gained) ||
        ev->type == OS64_GUI_EVENT_WINDOW_COVERED) { os64_ui_shortcut_reset(r); return false; }
    if (ev->type != OS64_GUI_EVENT_KEY_DOWN && ev->type != OS64_GUI_EVENT_KEY_UP) return false;
    unsigned identity = ev->key.scancode + ((ev->key.modifiers & OS64_GUI_MOD_HID) ? 256u : 0);
    unsigned byte = identity / 8, mask = 1u << (identity % 8);
    if (r->held[byte] & mask) {
        if (ev->type == OS64_GUI_EVENT_KEY_UP) r->held[byte] &= (uint8_t)~mask;
        return true;
    }
    if (!v || ev->type != OS64_GUI_EVENT_KEY_DOWN) return false;
    for (size_t i = 0; i < v->actions->count; ++i) {
        if (!os64_ui_shortcut_match(v->actions->specs[i].shortcut, ev)) continue;
        r->held[byte] |= (uint8_t)mask;
        os64_ui_action_result_t result = os64_ui_action_invoke(v, v->actions->specs[i].id);
        if (out) *out = result;
        return true;
    }
    return false;
}
