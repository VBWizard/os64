// submit.c — families C, D and I: WHO submits, WHAT goes out, and whether
// the page said it may.
//
// Six places in the first draft of this project computed "which submitter
// applies and what does it impose", and nine of the review's worst findings
// came through those six doors. There is one door per question here, and
// every other family asks through it.

#include "internal.h"
#include "os64/fmt.h"

// ── C. The submitter ────────────────────────────────────────────────────

// §4.10.21.3's implicit submission. The form's DEFAULT BUTTON is the first
// control in tree order that submits and whose owner is this form, WHEREVER
// IT STANDS: inside a subtree the page marked `hidden`, bound to the form by
// `form=` from the far end of the document, out of reach of any cursor. It is
// still the button whose method and action an implicit submission takes, and
// a browser that cannot press it must not send the form as though it had.
static int32_t default_button(const os64_page_t *page, int32_t form)
{
    for (int32_t i = 0; i < page->ncontrols; i++)
        if (page->controls[i].form == form && page->controls[i].submits)
            return i;
    return -1;
}

static int32_t blocking_fields(const os64_page_t *page, int32_t form)
{
    int32_t n = 0;
    for (int32_t i = 0; i < page->ncontrols; i++)
        if (page->controls[i].form == form && page->controls[i].blocks_implicit)
            n++;
    return n;
}

bool p_submitter(const os64_page_t *page, int32_t form, os64_page_what_t what,
                 int32_t *submitter, os64_page_reason_t *reason)
{
    const os64_page_control_t *c = os64_page_control(page, what.index);
    if (c == NULL) {
        *reason = OS64_PAGE_REASON_NO_CONTROL;
        return false;
    }
    if (c->disabled) {
        // The page took this control away; a browser does nothing at all.
        *reason = OS64_PAGE_REASON_DISABLED;
        return false;
    }
    if (what.how == OS64_PAGE_ACTIVATE_CONTROL) {
        if (c->resets) {
            *reason = OS64_PAGE_REASON_RESET;
            return false;
        }
        if (!c->submits) {
            *reason = OS64_PAGE_REASON_NO_SUBMISSION;
            return false;
        }
        if (form < 0) {
            *reason = OS64_PAGE_REASON_NO_FORM;
            return false;
        }
        *submitter = what.index;
        return true;
    }
    if (form < 0) {
        *reason = OS64_PAGE_REASON_NO_FORM;
        return false;
    }
    int32_t button = default_button(page, form);
    if (button >= 0) {
        if (page->controls[button].disabled) {
            *reason = OS64_PAGE_REASON_DEFAULT_BUTTON_DISABLED;
            return false;
        }
        *submitter = button;
        return true;
    }
    // With no submit control at all, one text-like field submits on Enter
    // and two do not: the standard will not guess which of them a person
    // meant to finish. `readonly` exempts nothing here, because the rule is
    // about what KIND of field it is.
    if (blocking_fields(page, form) > 1) {
        *reason = OS64_PAGE_REASON_IMPLICIT_BLOCKED;
        return false;
    }
    // The form submits itself, and carries no overrides because no button
    // was pressed to carry any.
    *submitter = -1;
    return true;
}

void p_effective(const os64_page_t *page, int32_t form, int32_t submitter,
                 const os64_page_ref_t **action, os64_page_method_t *method,
                 os64_page_enctype_t *enctype, bool *novalidate)
{
    const os64_page_form_t *f = os64_page_form(page, form);
    *action = &f->action;
    *method = f->method;
    *enctype = f->enctype;
    *novalidate = f->novalidate;
    const os64_page_control_t *c = os64_page_control(page, submitter);
    if (c == NULL)
        return;
    // A button's own attributes overrule its form's. `formaction=""` is the
    // page's own address, so PRESENCE decides and not the string — reading
    // an empty action as "none given" sends the form to the form's
    // destination instead of to the page it is on.
    if (c->overrides.action.spelled)
        *action = &c->overrides.action;
    if (c->overrides.has_method)
        *method = c->overrides.method;
    if (c->overrides.has_enctype)
        *enctype = c->overrides.enctype;
    if (c->overrides.novalidate)
        *novalidate = true;
}

// ── I. Constraint validation ────────────────────────────────────────────

static bool text_like(const os64_page_control_t *c)
{
    if (c->element == OS64_PAGE_EL_TEXTAREA)
        return true;
    if (c->element != OS64_PAGE_EL_INPUT)
        return false;
    switch (c->input) {
    case OS64_PAGE_INPUT_TEXT:
    case OS64_PAGE_INPUT_SEARCH:
    case OS64_PAGE_INPUT_TEL:
    case OS64_PAGE_INPUT_URL:
    case OS64_PAGE_INPUT_EMAIL:
    case OS64_PAGE_INPUT_PASSWORD:
    case OS64_PAGE_INPUT_DATE:
    case OS64_PAGE_INPUT_MONTH:
    case OS64_PAGE_INPUT_WEEK:
    case OS64_PAGE_INPUT_TIME:
    case OS64_PAGE_INPUT_DATETIME_LOCAL:
    case OS64_PAGE_INPUT_NUMBER:
    case OS64_PAGE_INPUT_FILE:
        return true;
    default:
        return false;
    }
}

// A radio group is required when ANY member says so, and it is satisfied
// when any member is ticked — one question, one answer, however many of the
// buttons carry the attribute.
static bool radio_group_ok(const os64_page_t *page, int32_t control)
{
    const os64_page_control_t *c = &page->controls[control];
    if (c->name == NULL || c->name[0] == '\0')
        return !c->required || c->checked;
    const void *head = p_strmap_get(&page->radio_map, c->name);
    bool required = false, ticked = false;
    for (int32_t at = head != NULL ? p_ptrmap_get(&page->control_map, head) : -1; at >= 0;
         at = page->radio_next[at]) {
        if (page->controls[at].form != c->form)
            continue;
        required = required || page->controls[at].required;
        ticked = ticked || page->controls[at].checked;
    }
    return !required || ticked;
}

// The standard's "suffering from being missing" for a list: it is required
// and nothing is picked, or the only thing picked is the PLACEHOLDER LABEL —
// the first option of a one-line list, with an empty value, which is what
// "choose one" is written as.
static bool select_ok(const os64_page_control_t *c)
{
    if (!c->required)
        return true;
    bool one_line = p_select_one_line(c);
    for (int32_t i = 0; i < c->noptions; i++) {
        if (!c->options[i].selected)
            continue;
        bool placeholder = one_line && i == 0 && c->options[i].value[0] == '\0' &&
                           c->options[i].node->parent == c->node;
        if (!placeholder)
            return true;
    }
    return false;
}

static size_t utf16_length(const char *s, size_t len)
{
    size_t units = 0;
    for (size_t at = 0; at < len;) {
        uint32_t cp = 0;
        size_t used = os64_utf8_decode(s+at, len-at, &cp);
        at += used != 0 ? used : 1;
        units += cp > 0xffff ? 2 : 1;
    }
    return units;
}

static bool length_limited(const os64_page_control_t *c)
{
    return c->element == OS64_PAGE_EL_TEXTAREA ||
        (c->element == OS64_PAGE_EL_INPUT &&
         (c->input == OS64_PAGE_INPUT_TEXT || c->input == OS64_PAGE_INPUT_SEARCH ||
          c->input == OS64_PAGE_INPUT_TEL || c->input == OS64_PAGE_INPUT_EMAIL ||
          c->input == OS64_PAGE_INPUT_URL || c->input == OS64_PAGE_INPUT_PASSWORD));
}

bool p_validate(const os64_page_t *page, int32_t form, int32_t *control)
{
    for (int32_t i = 0; i < page->ncontrols; i++) {
        const os64_page_control_t *c = &page->controls[i];
        if (c->form != form || c->barred_from_validation)
            continue;
        bool ok = true;
        if (c->element == OS64_PAGE_EL_SELECT)
            ok = select_ok(c);
        else if (c->input == OS64_PAGE_INPUT_RADIO)
            ok = radio_group_ok(page, i);
        else if (c->input == OS64_PAGE_INPUT_CHECKBOX)
            ok = !c->required || c->checked;
        else if (c->required && text_like(c))
            ok = c->value_len != 0;
        // THE LENGTH LIMITS APPLY TO WHAT A PERSON TYPED and to nothing
        // else: the standard hangs them on the dirty value, so a page that
        // ships a value longer than its own maxlength still submits it.
        const PEdit *edit = p_edit_find(page, i);
        if (ok && edit != NULL && edit->text != NULL && length_limited(c)) {
            // HTML numeric attributes share prefix parsing with select
            // display size and textarea columns.
            size_t units = utf16_length(c->value, c->value_len);
            uint64_t limit = 0;
            const char *most = p_attr(c->node, "maxlength");
            const char *least = p_attr(c->node, "minlength");
            if (most != NULL && p_nonnegative(most, &limit) && units > limit)
                ok = false;
            if (ok && least != NULL && p_nonnegative(least, &limit) && c->value_len != 0 &&
                units < limit)
                ok = false;
        }
        if (!ok) {
            *control = i;
            return false;
        }
    }
    return true;
}

// ── D. Constructing the entry list ──────────────────────────────────────

// The standard's "button" for submission purposes: the input types that ARE
// buttons, plus every `button` element whatever its type says. None of them
// is sent unless it is the one that was pressed.
static bool is_button(const os64_page_control_t *c)
{
    return c->element == OS64_PAGE_EL_BUTTON || c->input == OS64_PAGE_INPUT_SUBMIT ||
           c->input == OS64_PAGE_INPUT_IMAGE || c->input == OS64_PAGE_INPUT_RESET ||
           c->input == OS64_PAGE_INPUT_BUTTON;
}

static bool entry_add(PEntries *entries, const char *name, size_t name_len, const char *value,
                      size_t value_len, bool is_file)
{
    if (!p_grow((void **)&entries->items, &entries->cap, entries->count, sizeof(*entries->items)))
        return false;
    PEntry *entry = &entries->items[entries->count];
    entry->name = name;
    entry->name_len = name_len;
    entry->value = value;
    entry->value_len = value_len;
    entry->is_file = is_file;
    entries->count++;
    return true;
}

// A lone CR and a lone LF both become CRLF. The wire is what makes this
// matter: a textarea holding one newline would otherwise send `%0A` where
// every other browser sends `%0D%0A`, and a server splitting on CRLF would
// see one line where a person typed two.
static const char *to_crlf(PArena *arena, const char *s, size_t len, size_t *out_len)
{
    size_t breaks = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\r' && (i + 1 == len || s[i + 1] != '\n'))
            breaks++;
        else if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r'))
            breaks++;
    }
    if (breaks == 0) {
        *out_len = len;
        return s;
    }
    char *out = p_arena_alloc(arena, len + breaks + 1);
    if (out == NULL)
        return NULL;
    size_t at = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\r' && (i + 1 == len || s[i + 1] != '\n')) {
            out[at++] = '\r';
            out[at++] = '\n';
        } else if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) {
            out[at++] = '\r';
            out[at++] = '\n';
        } else {
            out[at++] = s[i];
        }
    }
    out[at] = '\0';
    *out_len = at;
    return out;
}

static bool add_normalised(PEntries *entries, const char *name, const char *value,
                           size_t value_len, bool is_file)
{
    size_t name_len = 0, out_len = 0;
    const char *n = to_crlf(&entries->arena, name, os64_strlen(name), &name_len);
    const char *v = to_crlf(&entries->arena, value, value_len, &out_len);
    if (n == NULL || v == NULL)
        return false;
    return entry_add(entries, n, name_len, v, out_len, is_file);
}

// The `dirname` companion: which way the field's text runs, as the standard
// defines an element's directionality. `dir=auto` is why the Unicode table
// is in libos64 — it means "read the text and see".
static const char *directionality(PArena *arena, const os64_page_control_t *c)
{
    for (const os64_html_node_t *up = c->node; up != NULL; up = up->parent) {
        const char *dir = p_attr(up, "dir");
        if (dir == NULL)
            continue;
        if (os64_streq_nocase(dir, "ltr"))
            return "ltr";
        if (os64_streq_nocase(dir, "rtl"))
            return "rtl";
        if (!os64_streq_nocase(dir, "auto"))
            continue;
        // A form control under `dir=auto` is read from its VALUE; anything
        // else from the text inside it. Either way the first STRONG
        // character decides and nothing else gets a vote, so a phone number
        // in front of a Hebrew name does not make the field left to right.
        os64_bidi_strong_t strong;
        if (up == c->node) {
            strong = os64_bidi_first_strong(c->value, c->value_len);
        } else {
            size_t len = 0;
            const char *text = p_subtree_text_into(arena, up, false, &len);
            if (text == NULL)
                return NULL;
            strong = os64_bidi_first_strong(text, len);
        }
        return strong == OS64_BIDI_R || strong == OS64_BIDI_AL ? "rtl" : "ltr";
    }
    return "ltr";
}

// Deterministic hard wrapping: count Unicode scalar values, retain existing
// LF breaks, and insert LF before the next scalar once cols is reached.
static const char *submission_value(PArena *arena, const os64_page_control_t *c, size_t *len)
{
    *len = c->value_len;
    const char *wrap = p_attr(c->node, "wrap");
    if (c->element != OS64_PAGE_EL_TEXTAREA || wrap == NULL || !os64_streq_nocase(wrap, "hard"))
        return c->value;
    uint64_t cols = 0;
    const char *attr = p_attr(c->node, "cols");
    if (attr == NULL || !p_nonnegative(attr, &cols) || cols == 0)
        cols = 20;
    size_t extra = c->value_len / cols;
    if (extra >= SIZE_MAX - c->value_len)
        return NULL;
    char *out = p_arena_alloc(arena, c->value_len+extra+1);
    if (out == NULL)
        return NULL;
    size_t at = 0, column = 0;
    for (size_t read = 0; read < c->value_len;) {
        uint32_t cp = 0;
        size_t used = os64_utf8_decode(c->value+read, c->value_len-read, &cp);
        if (used == 0)
            used = 1;
        if (cp == '\n')
            column = 0;
        else {
            if (column == cols) {
                out[at++] = '\n';
                column = 0;
            }
            column++;
        }
        os64_memcpy(out+at,c->value+read,used);
        at += used;
        read += used;
    }
    out[at] = '\0';
    *len = at;
    return out;
}

static bool sends_dirname(const os64_page_control_t *c)
{
    // The submission step's own list: a textarea, or a text or search input. The
    // attribute is spelled on more types than the submission step reads.
    return c->dirname != NULL && c->dirname[0] != '\0' &&
           (c->element == OS64_PAGE_EL_TEXTAREA || c->input == OS64_PAGE_INPUT_TEXT ||
            c->input == OS64_PAGE_INPUT_SEARCH);
}

bool p_entry_list(const os64_page_t *page, int32_t form, int32_t submitter,
                  os64_page_what_t what, PEntries *out)
{
    os64_memset(out, 0, sizeof(*out));
    const char *encoding = p_encoding_for(page, form);
    for (int32_t i = 0; i < page->ncontrols; i++) {
        const os64_page_control_t *c = &page->controls[i];
        if (c->form != form)
            continue;
        // A `datalist` holds suggestions and not answers; a disabled control
        // was taken away; and a button that is not the one pressed is not
        // part of what the person did.
        if (c->has_datalist_ancestor || c->disabled)
            continue;
        if (is_button(c) && i != submitter)
            continue;
        if ((c->input == OS64_PAGE_INPUT_CHECKBOX || c->input == OS64_PAGE_INPUT_RADIO) &&
            !c->checked)
            continue;
        // AN IMAGE BUTTON SENDS WHERE IT WAS CLICKED and never its value,
        // under its own name when it has one and bare `x`/`y` when it does
        // not — so it is settled before the "no name, nothing sent" rule.
        if (c->input == OS64_PAGE_INPUT_IMAGE) {
            char number[24];
            size_t prefix = c->name != NULL ? os64_strlen(c->name) : 0;
            if (prefix > SIZE_MAX - 3)
                return false;
            for (int32_t axis = 0; axis < 2; axis++) {
                size_t name_len = prefix != 0 ? prefix+2 : 1;
                char *name = p_arena_alloc(&out->arena, name_len+1);
                if (name == NULL)
                    return false;
                if (prefix != 0) {
                    os64_memcpy(name,c->name,prefix);
                    name[prefix] = '.';
                }
                name[name_len-1] = axis == 0 ? 'x' : 'y';
                name[name_len] = '\0';
                os64_snprintf(number, sizeof(number), "%d", axis == 0 ? what.x : what.y);
                char *held = p_arena_copy(&out->arena, number, os64_strlen(number));
                if (held == NULL || !add_normalised(out, name, held, os64_strlen(held), false))
                    return false;
            }
            continue;
        }
        if (c->name == NULL || c->name[0] == '\0')
            continue;
        if (c->element == OS64_PAGE_EL_SELECT) {
            // EVERY picked option, `multiple` or not: collapsing them to one
            // would send a server one choice where the page made three.
            for (int32_t o = 0; o < c->noptions; o++) {
                if (!c->options[o].selected || c->options[o].disabled)
                    continue;
                if (!add_normalised(out, c->name, c->options[o].value,
                                    os64_strlen(c->options[o].value), false))
                    return false;
            }
            continue;
        }
        if (c->input == OS64_PAGE_INPUT_CHECKBOX || c->input == OS64_PAGE_INPUT_RADIO) {
            // A tick with no `value` sends `on`, the standard's default. One
            // that says `value=""` asked for the empty string, and sending
            // `on` for it picks a different branch on the far side.
            if (!add_normalised(out, c->name, c->value, c->value_len, false))
                return false;
            continue;
        }
        if (c->input == OS64_PAGE_INPUT_FILE) {
            // No face can pick a file yet, so what goes out is the empty
            // name the standard sends for a field with none.
            if (!add_normalised(out, c->name, c->value, c->value_len, true))
                return false;
            continue;
        }
        if (c->input == OS64_PAGE_INPUT_HIDDEN && os64_streq_nocase(c->name, "_charset_")) {
            // THE FORM'S OWN ANSWER, NOT THE PAGE'S: this field is filled in
            // with the encoding the values are going out in, whatever the
            // page left in its `value`, so a server can decode the rest.
            if (!add_normalised(out, c->name, encoding, os64_strlen(encoding), false))
                return false;
            continue;
        }
        size_t value_len = 0;
        const char *value = submission_value(&out->arena, c, &value_len);
        if (value == NULL || !add_normalised(out, c->name, value, value_len, false))
            return false;
        if (sends_dirname(c)) {
            const char *which = directionality(&out->arena, c);
            if (which == NULL || !add_normalised(out, c->dirname, which, os64_strlen(which), false))
                return false;
        }
    }
    return true;
}

void p_entries_free(PEntries *entries)
{
    os64_free(entries->items);
    p_arena_free(&entries->arena);
    entries->items = NULL;
    entries->count = entries->cap = 0;
}
