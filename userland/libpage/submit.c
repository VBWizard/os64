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

bool os64_page_submitter(const os64_page_t *page, os64_page_what_t what, int32_t *submitter)
{
    const os64_page_control_t *c = os64_page_control(page, what.index);
    os64_page_reason_t reason = OS64_PAGE_REASON_OK;
    *submitter = -1;
    return (what.how == OS64_PAGE_ACTIVATE_CONTROL || what.how == OS64_PAGE_ACTIVATE_IMPLICIT) && c != NULL &&
        p_submitter(page, c->form, what, submitter, &reason);
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

// A radio group is required when ANY member says so, and it is satisfied
// when any member is ticked — one question, one answer, however many of the
// buttons carry the attribute. The group is judged ONCE, at the first member
// validation applies to, over its own linked members: judging it at every
// member made a form of N radios cost N² to submit.
static bool radio_group_ok(const os64_page_t *page, int32_t control)
{
    // A radio with no name is in no group, and is never missing, as
    // browsers answer (WPT's form-validation-validity-valueMissing).
    const char *name = page->controls[control].name;
    if (name == NULL || name[0] == '\0')
        return true;
    int32_t head = page->group_head[control];
    if (page->group_judged_at[head] != control)
        return true;                     // judged at another member, or will be
    bool required = false, ticked = false;
    for (int32_t at = head; at >= 0; at = page->group_next[at]) {
        required = required || page->controls[at].required;
        ticked = ticked || page->controls[at].checked;
    }
    return !required || ticked;
}

// Whether the text under `n` is all white space, gathered as an option's
// text is (core.c's gather: every element's text but a script's).
static bool text_blank(const os64_html_node_t *n)
{
    for (; n != NULL; n = n->next) {
        if (n->kind == OS64_HTML_TEXT) {
            for (size_t i = 0; i < n->text_len; i++)
                if (n->text[i] != ' ' && n->text[i] != '\t' && n->text[i] != '\n' && n->text[i] != '\r' &&
                    n->text[i] != '\f')
                    return false;
        } else if (n->kind == OS64_HTML_ELEMENT && !p_is(n, OS64_HTML_TAG_SCRIPT) && !text_blank(n->first_child)) {
            return false;
        }
    }
    return true;
}

// An option's value is empty: its value attribute, else its text with the
// white space stripped and collapsed, which is empty when the text is all
// white space.
static bool option_value_empty(const os64_html_node_t *option)
{
    const char *value = p_attr(option, "value");
    if (value != NULL)
        return value[0] == '\0';
    return text_blank(option->first_child);
}

// The standard's "suffering from being missing" for a list: it is required
// and nothing is picked, or the only thing picked is the PLACEHOLDER LABEL —
// the first option of a one-line list, with an empty value, which is what
// "choose one" is written as. Read from the live state, option by option.
static bool select_missing(os64_page_state_t *state, const os64_html_node_t *select)
{
    uint64_t size = 0;
    if (!p_nonnegative(p_attr(select, "size"), &size))
        size = 0;
    bool multiple = p_has_attr(select, "multiple");
    bool one_line = !multiple && size <= 1;
    if (!multiple) {
        // One pick at most, read once: asking it of each option walks the
        // list per option, and a page may read validity per keystroke.
        int32_t index = -1;
        if (os64_page_node_selected_index(state, select, &index) < 0 || index < 0)
            return true;
        const os64_html_node_t *first = os64_page_option_next(select, NULL);
        return one_line && index == 0 && first != NULL && first->parent == select && option_value_empty(first);
    }
    for (const os64_html_node_t *at = os64_page_option_next(select, NULL); at != NULL;
         at = os64_page_option_next(select, at)) {
        bool on = false;
        if (os64_page_node_option_selected(state, at, &on) >= 0 && on)
            return false;
    }
    return true;
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

// A disabled fieldset disables what it holds, except what is in its first
// legend (core.c's first_legend says why; the model's walk carries the
// answer down, and this asks it of one node).
static bool fieldset_disabled(const os64_html_node_t *n)
{
    for (const os64_html_node_t *child = n, *up = n->parent; up != NULL; child = up, up = up->parent) {
        if (!p_is(up, OS64_HTML_TAG_FIELDSET) || !p_has_attr(up, "disabled"))
            continue;
        const os64_html_node_t *legend = up->first_child;
        while (legend != NULL && !p_is(legend, OS64_HTML_TAG_LEGEND))
            legend = legend->next;
        if (child != legend)
            return true;
    }
    return false;
}

static bool node_disabled(const os64_html_node_t *n)
{
    return p_has_attr(n, "disabled") || fieldset_disabled(n);
}

// Whether `readonly` keeps a person from changing the value, which a
// field's valueMissing asks: only where it applies, textarea and the
// text-like input types (barring is node_barred's, below, and wider).
static bool node_readonly(const os64_html_node_t *n, os64_page_element_t element, os64_page_input_t input)
{
    return p_has_attr(n, "readonly") &&
        (element == OS64_PAGE_EL_TEXTAREA || (element == OS64_PAGE_EL_INPUT && p_text_like_input(input)));
}

// Barred from constraint validation (HTML § 4.10.20.1), as the model's
// barred() says it of a control, here of a node: the types that are not
// fields, a button that does not submit, a disabled control, one in a
// datalist, a textarea with `readonly`, and an input with it whatever its
// type, though readonly applies only to the text-like types (HTML's
// readonly attribute bars "an input element" it is specified on).
static bool node_barred(const os64_html_node_t *n, os64_page_element_t element, os64_page_input_t input)
{
    if (input == OS64_PAGE_INPUT_HIDDEN || input == OS64_PAGE_INPUT_RESET || input == OS64_PAGE_INPUT_BUTTON)
        return true;
    if (element == OS64_PAGE_EL_BUTTON) {
        const char *type = p_attr(n, "type");
        if (type != NULL && (os64_streq_nocase(type, "reset") || os64_streq_nocase(type, "button")))
            return true;
    }
    bool readonly = element != OS64_PAGE_EL_SELECT && element != OS64_PAGE_EL_BUTTON && p_has_attr(n, "readonly");
    return node_disabled(n) || readonly || p_ancestor(n, OS64_HTML_TAG_DATALIST) != NULL;
}

static bool ascii_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// HTML's valid email address (§ 4.10.5.1.5): its local part from atext and
// dots, then '@', then labels of letters, digits and hyphens, each 1 to 63
// long and neither beginning nor ending with a hyphen, joined by dots.
static bool email_valid(const char *s, size_t len)
{
    size_t at = 0;
    while (at < len && s[at] != '@') {
        char c = s[at];
        // os64_strchr finds a NUL as the set's own terminator.
        if (c == '\0' || (!ascii_alnum(c) && os64_strchr(".!#$%&'*+/=?^_`{|}~-", c) == NULL))
            return false;
        at++;
    }
    if (at == 0 || at == len)
        return false;
    size_t label = 0;
    for (at++; at <= len; at++) {
        char c = at < len ? s[at] : '.';
        if (c == '.') {
            if (label == 0 || s[at - 1] == '-')
                return false;
            label = 0;
        } else if (ascii_alnum(c) || c == '-') {
            if ((label == 0 && c == '-') || ++label > 63)
                return false;
        } else {
            return false;
        }
    }
    return true;
}

static bool email_list_valid(const char *s, size_t len)
{
    for (size_t start = 0;;) {
        size_t end = start;
        while (end < len && s[end] != ',')
            end++;
        if (!email_valid(s + start, end - start))
            return false;
        if (end == len)
            return true;
        start = end + 1;
    }
}

// A valid absolute URL, as far as this answers it: a scheme (a letter, then
// letters, digits, '+', '-' or '.') and its ':', and for the schemes whose
// URLs need a host, a host after any slashes, and no space or control
// byte. The URL parser HTML names would refuse more (DEBTS.md § libpage).
static bool url_valid(const char *s, size_t len)
{
    size_t at = 0;
    if (len == 0 || !((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z')))
        return false;
    while (at < len && (ascii_alnum(s[at]) || s[at] == '+' || s[at] == '-' || s[at] == '.'))
        at++;
    if (at == len || s[at] != ':')
        return false;
    static const char *const kHosted[] = {"http", "https", "ftp", "ws", "wss"};
    bool hosted = false;
    for (size_t i = 0; i < sizeof(kHosted) / sizeof(kHosted[0]) && !hosted; i++) {
        hosted = os64_strlen(kHosted[i]) == at;
        for (size_t k = 0; hosted && k < at; k++)
            hosted = (s[k] | 0x20) == kHosted[i][k];
    }
    for (size_t i = 0; i < len; i++)
        if ((unsigned char)s[i] <= ' ')
            return false;
    if (!hosted)
        return true;
    at++;
    while (at < len && (s[at] == '/' || s[at] == '\\'))
        at++;
    return at < len && s[at] != '?' && s[at] != '#';
}

// Where `pattern` applies (§ 4.10.5.3.6).
static bool pattern_applies(os64_page_input_t input)
{
    return input == OS64_PAGE_INPUT_TEXT || input == OS64_PAGE_INPUT_SEARCH || input == OS64_PAGE_INPUT_TEL ||
        input == OS64_PAGE_INPUT_URL || input == OS64_PAGE_INPUT_EMAIL || input == OS64_PAGE_INPUT_PASSWORD;
}

static bool pattern_mismatch(const os64_html_node_t *n, os64_page_input_t input, const char *value, size_t len,
                             os64_page_pattern_t matcher, void *opaque)
{
    const char *pattern = p_attr(n, "pattern");
    if (matcher == NULL || pattern == NULL || len == 0 || !pattern_applies(input))
        return false;
    // An email list is matched address by address; anything else whole.
    bool list = input == OS64_PAGE_INPUT_EMAIL && p_has_attr(n, "multiple");
    for (size_t start = 0;;) {
        size_t end = start;
        while (list && end < len && value[end] != ',')
            end++;
        if (!list)
            end = len;
        if (matcher(opaque, pattern, value + start, end - start) == 0)
            return true;
        if (end == len)
            return false;
        start = end + 1;
    }
}

// rangeUnderflow, rangeOverflow and stepMismatch (HTML's min, max and step
// attributes), for the types they apply to, read as numbers (value.c's
// p_input_number and p_input_limits): a value below a min that converts,
// above a max that does, or off the grid its step draws from its step
// base. A time whose max is below its min wraps past midnight, and is out
// of range only between the two.
static uint32_t range_flags(const os64_html_node_t *n, os64_page_input_t input, const char *value, size_t len)
{
    double number = 0;
    if (len == 0 || !p_input_number(input, value, &number))
        return 0;
    PLimits limits;
    p_input_limits(n, input, &limits);
    // A range's max below its min is its min: the sanitizer puts the
    // value at the min, which no slider can move below, so judging it
    // above the max refused every submission. Nor is that pinned value
    // snapped to a step, so no step judges it.
    if (limits.pinned)
        return 0;
    uint32_t flags = 0;
    if (input == OS64_PAGE_INPUT_TIME && limits.has_min && limits.has_max && limits.max < limits.min) {
        if (number < limits.min && number > limits.max)
            flags |= OS64_PAGE_VALIDITY_UNDERFLOW | OS64_PAGE_VALIDITY_OVERFLOW;
    } else {
        if (limits.has_min && number < limits.min)
            flags |= OS64_PAGE_VALIDITY_UNDERFLOW;
        if (limits.has_max && number > limits.max)
            flags |= OS64_PAGE_VALIDITY_OVERFLOW;
    }
    if (!limits.any &&
        !p_step_aligned(number, limits.base, limits.step, limits.scale, input == OS64_PAGE_INPUT_RANGE))
        flags |= OS64_PAGE_VALIDITY_STEP;
    return flags;
}

// THE JUDGE: every flag a control's validity has (ValidityState, § 4.10.20.3),
// for a node of the kinds the value verbs take, given its value and its
// radio group's answer — which the model has from its chains and a node
// asked alone walks its tree for. Disabled and readonly are asked only as
// mutability, which a typed-into field's valueMissing needs (a field a
// person cannot fill is not missing); otherwise a barred control's flags
// are still its flags (customError survives disabled), and what bars it
// only stops them counting, which is willValidate's to say.
static uint32_t judge(os64_page_state_t *state, const os64_html_node_t *n, os64_page_element_t element,
                      os64_page_input_t input, const char *value, size_t len, bool radio_missing,
                      os64_page_pattern_t matcher, void *opaque)
{
    uint32_t flags = 0;
    size_t custom_len = 0;
    if (p_node_custom(state, n, &custom_len) != NULL)
        flags |= OS64_PAGE_VALIDITY_CUSTOM;
    bool required = p_has_attr(n, "required");
    bool is_mutable = !node_disabled(n) && !node_readonly(n, element, input);
    if (element == OS64_PAGE_EL_SELECT) {
        if (required && select_missing(state, n))
            flags |= OS64_PAGE_VALIDITY_MISSING;
        return flags;
    }
    if (element == OS64_PAGE_EL_BUTTON)
        return flags;
    // A checkbox, a radio and a file answer "missing" whatever their
    // mutability; a field typed into, only while it can be typed into.
    if (input == OS64_PAGE_INPUT_CHECKBOX) {
        bool on = false;
        if (required && os64_page_node_checked(state, n, &on) >= 0 && !on)
            flags |= OS64_PAGE_VALIDITY_MISSING;
    } else if (input == OS64_PAGE_INPUT_RADIO) {
        if (radio_missing)
            flags |= OS64_PAGE_VALIDITY_MISSING;
    } else if (input == OS64_PAGE_INPUT_FILE) {
        if (required && len == 0)
            flags |= OS64_PAGE_VALIDITY_MISSING;
    } else if (required && len == 0 && is_mutable &&
               (element == OS64_PAGE_EL_TEXTAREA || p_text_like_input(input))) {
        flags |= OS64_PAGE_VALIDITY_MISSING;
    }
    if (len != 0 && input == OS64_PAGE_INPUT_EMAIL &&
        !(p_has_attr(n, "multiple") ? email_list_valid(value, len) : email_valid(value, len)))
        flags |= OS64_PAGE_VALIDITY_TYPE;
    if (len != 0 && input == OS64_PAGE_INPUT_URL && !url_valid(value, len))
        flags |= OS64_PAGE_VALIDITY_TYPE;
    if (pattern_mismatch(n, input, value, len, matcher, opaque))
        flags |= OS64_PAGE_VALIDITY_PATTERN;
    // Length limits require a dirty value last changed by a person;
    // script assignment and the page's default do not trigger them.
    const PNodeState *edit = p_state_find(state, n);
    os64_page_control_t kind = {.element = element, .input = input};
    if (edit != NULL && edit->text != NULL && edit->text_user && length_limited(&kind)) {
        // HTML numeric attributes share prefix parsing with select
        // display size and textarea columns.
        size_t units = utf16_length(value, len);
        uint64_t limit = 0;
        const char *most = p_attr(n, "maxlength");
        const char *least = p_attr(n, "minlength");
        if (most != NULL && p_nonnegative(most, &limit) && units > limit)
            flags |= OS64_PAGE_VALIDITY_TOO_LONG;
        if (least != NULL && p_nonnegative(least, &limit) && len != 0 && units < limit)
            flags |= OS64_PAGE_VALIDITY_TOO_SHORT;
    }
    // The record is set only for the types whose text can fail to become a
    // value (state.c's unparsed), and a change of type clears it.
    if (p_node_bad_input(state, n))
        flags |= OS64_PAGE_VALIDITY_BAD_INPUT;
    return flags | range_flags(n, input, value, len);
}

bool p_validate(const os64_page_t *page, int32_t form, int32_t *control)
{
    for (int32_t i = 0; i < page->ncontrols; i++) {
        const os64_page_control_t *c = &page->controls[i];
        if (c->form != form || c->barred_from_validation)
            continue;
        bool radio_missing = c->input == OS64_PAGE_INPUT_RADIO && !radio_group_ok(page, i);
        // No pattern is judged here: libpage has no regular expressions. A
        // submission the page's scripts validated does not reach this
        // (os64_page_activate_validated); one they did not (scripts off,
        // the Modern Web switch off, no runtime yet, an image button) is
        // judged without its pattern (DEBTS.md § libpage), but for a form
        // that submits itself (activate.c's submit says why).
        if (judge(page->state, c->node, c->element, c->input, c->value, c->value_len, radio_missing, NULL,
                  NULL) != 0) {
            *control = i;
            return false;
        }
    }
    return true;
}

// The node verbs' kinds: the four the value verbs take, and the two whose
// only validity is a custom one.
static bool validity_kind(os64_page_state_t *state, const os64_html_node_t *node, os64_page_element_t *element,
                          os64_page_input_t *input, bool *custom_only)
{
    *custom_only = false;
    if (p_node_kind(state, node, element, input))
        return true;
    *custom_only = state != NULL && os64_html_owns_node(state->doc, node) &&
        (p_is(node, OS64_HTML_TAG_FIELDSET) || p_is(node, OS64_HTML_TAG_OUTPUT));
    return *custom_only;
}

int64_t os64_page_node_will_validate(os64_page_state_t *state, const os64_html_node_t *node, bool *will)
{
    os64_page_element_t element;
    os64_page_input_t input;
    bool custom_only;
    if (will == NULL || !validity_kind(state, node, &element, &input, &custom_only))
        return -OS64_PAGE_REASON_WRONG_KIND;
    *will = !custom_only && !node_barred(node, element, input);
    return 0;
}

int64_t os64_page_node_validity(os64_page_state_t *state, const os64_html_node_t *node, os64_page_pattern_t matcher,
                                void *opaque, uint32_t *flags)
{
    os64_page_element_t element;
    os64_page_input_t input;
    bool custom_only;
    if (flags == NULL || !validity_kind(state, node, &element, &input, &custom_only))
        return -OS64_PAGE_REASON_WRONG_KIND;
    if (custom_only) {
        size_t len = 0;
        *flags = p_node_custom(state, node, &len) != NULL ? OS64_PAGE_VALIDITY_CUSTOM : 0;
        return 0;
    }
    const char *value = "";
    size_t len = 0;
    if (element != OS64_PAGE_EL_SELECT && element != OS64_PAGE_EL_BUTTON) {
        int64_t status = os64_page_node_value(state, node, &value, &len);
        if (status < 0)
            return status;
    }
    bool radio_missing = input == OS64_PAGE_INPUT_RADIO && p_radio_group_missing(state, node);
    *flags = judge(state, node, element, input, value, len, radio_missing, matcher, opaque);
    return 0;
}

// The custom message as it was set, borrowed; "" for none.
int64_t os64_page_node_custom_validity(os64_page_state_t *state, const os64_html_node_t *node, const char **message,
                                       size_t *len)
{
    os64_page_element_t element;
    os64_page_input_t input;
    bool custom_only;
    if (message == NULL || len == NULL || !validity_kind(state, node, &element, &input, &custom_only))
        return -OS64_PAGE_REASON_WRONG_KIND;
    *message = p_node_custom(state, node, len);
    if (*message == NULL)
        *message = "";
    return 0;
}

// The sentence a person is shown for the first flag a control has, in the
// order ValidityState lists them, or its custom message; "" for none. The
// sentences are plain ASCII, as a status line prints them; a custom
// message is the page's own UTF-8.
int64_t os64_page_node_validation_message(os64_page_state_t *state, const os64_html_node_t *node, uint32_t flags,
                                          char *out, size_t cap)
{
    os64_page_element_t element;
    os64_page_input_t input;
    bool custom_only;
    if (out == NULL || cap == 0 || !validity_kind(state, node, &element, &input, &custom_only))
        return -OS64_PAGE_REASON_WRONG_KIND;
    out[0] = '\0';
    size_t custom_len = 0;
    const char *custom = p_node_custom(state, node, &custom_len);
    if ((flags & OS64_PAGE_VALIDITY_CUSTOM) && custom != NULL) {
        // Cut, when it must be, between characters: a continuation byte
        // starts none.
        size_t n = custom_len < cap - 1 ? custom_len : cap - 1;
        while (n < custom_len && n > 0 && ((unsigned char)custom[n] & 0xc0) == 0x80)
            n--;
        os64_memcpy(out, custom, n);
        out[n] = '\0';
        return 0;
    }
    uint64_t limit = 0;
    if (flags & OS64_PAGE_VALIDITY_MISSING) {
        const char *what = element == OS64_PAGE_EL_SELECT ? "Please select an item in the list." :
            input == OS64_PAGE_INPUT_CHECKBOX ? "Please check this box if you want to proceed." :
            input == OS64_PAGE_INPUT_RADIO ? "Please select one of these options." :
            input == OS64_PAGE_INPUT_FILE ? "Please select a file." : "Please fill out this field.";
        os64_strcopy(out, cap, what);
    } else if (flags & OS64_PAGE_VALIDITY_TYPE) {
        os64_strcopy(out, cap,
                     input == OS64_PAGE_INPUT_EMAIL ? "Please enter an email address." : "Please enter a URL.");
    } else if (flags & OS64_PAGE_VALIDITY_PATTERN) {
        os64_strcopy(out, cap, "Please match the requested format.");
    } else if (flags & OS64_PAGE_VALIDITY_TOO_LONG) {
        (void)p_nonnegative(p_attr(node, "maxlength"), &limit);
        os64_snprintf(out, cap, "Please shorten this text to %llu characters or less.", (unsigned long long)limit);
    } else if (flags & OS64_PAGE_VALIDITY_TOO_SHORT) {
        (void)p_nonnegative(p_attr(node, "minlength"), &limit);
        os64_snprintf(out, cap, "Please lengthen this text to %llu characters or more.", (unsigned long long)limit);
    } else if (flags & (OS64_PAGE_VALIDITY_UNDERFLOW | OS64_PAGE_VALIDITY_OVERFLOW)) {
        // A date's or a time's bound as the page wrote it, which converted
        // only as a valid value; a number's as the number it was read as,
        // which the lenient rules read from "5px" too.
        const char *min = p_attr(node, "min"), *max = p_attr(node, "max");
        bool numeric = input == OS64_PAGE_INPUT_NUMBER || input == OS64_PAGE_INPUT_RANGE;
        char min_text[32], max_text[32];
        double bound = 0;
        if (numeric) {
            p_number_spell(p_input_number(input, min, &bound) ? bound : 0, min_text);
            p_number_spell(p_input_number(input, max, &bound) ? bound : 100, max_text);
            min = min_text;
            max = max_text;
        }
        if (min == NULL) min = "";
        if (max == NULL) max = "";
        if (input == OS64_PAGE_INPUT_TIME && (flags & OS64_PAGE_VALIDITY_UNDERFLOW) &&
            (flags & OS64_PAGE_VALIDITY_OVERFLOW))
            // A time whose max is below its min: either side of midnight.
            os64_snprintf(out, cap, "Value must be %s or later, or %s or earlier.", min, max);
        else if (flags & OS64_PAGE_VALIDITY_UNDERFLOW)
            os64_snprintf(out, cap, numeric ? "Value must be greater than or equal to %s." : "Value must be %s or later.",
                          min);
        else
            os64_snprintf(out, cap, numeric ? "Value must be less than or equal to %s." : "Value must be %s or earlier.",
                          max);
    } else if (flags & OS64_PAGE_VALIDITY_BAD_INPUT) {
        os64_strcopy(out, cap,
                     input == OS64_PAGE_INPUT_NUMBER ? "Please enter a number." : "Please enter a valid value.");
    } else if (flags != 0) {
        os64_strcopy(out, cap, "Please enter a valid value.");
    }
    return 0;
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

// Only HTML elements have a dir state. Invalid values are undefined, so
// they neither override inheritance nor isolate a subtree from dir=auto.
static int dir_state(const os64_html_node_t *n)
{
    if (n->kind != OS64_HTML_ELEMENT || n->ns != OS64_HTML_NS_HTML)
        return 0;
    const char *dir = p_attr(n, "dir");
    if (dir == NULL) return 0;
    if (os64_streq_nocase(dir, "ltr")) return 1;
    if (os64_streq_nocase(dir, "rtl")) return 2;
    if (os64_streq_nocase(dir, "auto")) return 3;
    return 0;
}

// Walk text in tree order without flattening it: isolation boundaries must
// exclude their entire subtree, while an invalid dir spelling does not.
static os64_bidi_strong_t contained_direction(const os64_html_node_t *root)
{
    const os64_html_node_t *n = root->first_child;
    while (n != NULL) {
        bool excluded = n->kind == OS64_HTML_ELEMENT && n->ns == OS64_HTML_NS_HTML &&
            (n->tag == OS64_HTML_TAG_BDI || n->tag == OS64_HTML_TAG_SCRIPT ||
             n->tag == OS64_HTML_TAG_STYLE || n->tag == OS64_HTML_TAG_TEXTAREA || dir_state(n));
        if (n->kind == OS64_HTML_TEXT) {
            os64_bidi_strong_t strong = os64_bidi_first_strong(n->text, n->text_len);
            if (strong == OS64_BIDI_L || strong == OS64_BIDI_R || strong == OS64_BIDI_AL)
                return strong;
        }
        if (!excluded && n->first_child != NULL) {
            n = n->first_child;
            continue;
        }
        while (n != root && n->next == NULL)
            n = n->parent;
        if (n == root) break;
        n = n->next;
    }
    return OS64_BIDI_L;
}

static bool auto_uses_value(const os64_page_control_t *c)
{
    if (c->element == OS64_PAGE_EL_TEXTAREA) return true;
    if (c->element != OS64_PAGE_EL_INPUT) return false;
    switch (c->input) {
    case OS64_PAGE_INPUT_HIDDEN: case OS64_PAGE_INPUT_TEXT:
    case OS64_PAGE_INPUT_SEARCH: case OS64_PAGE_INPUT_TEL:
    case OS64_PAGE_INPUT_URL: case OS64_PAGE_INPUT_EMAIL:
    case OS64_PAGE_INPUT_PASSWORD: case OS64_PAGE_INPUT_SUBMIT:
    case OS64_PAGE_INPUT_RESET: case OS64_PAGE_INPUT_BUTTON:
        return true;
    default: return false;
    }
}

// Values come from the current model, including when an ancestor control
// supplies the inherited direction. Ordinary auto containers use scoped text.
static const char *directionality(const os64_page_t *page, const os64_page_control_t *c,
                                  PEntries *entries)
{
    for (const os64_html_node_t *up = c->node; up != NULL; up = up->parent) {
        int dir = dir_state(up);
        if (dir == 1) return "ltr";
        if (dir == 2) return "rtl";
        const os64_page_control_t *control = os64_page_control(page, os64_page_control_for(page, up));
        if (dir == 3 || (up->ns == OS64_HTML_NS_HTML && up->tag == OS64_HTML_TAG_BDI)) {
            os64_bidi_strong_t strong;
            if (control != NULL && auto_uses_value(control)) {
                strong = os64_bidi_first_strong(control->value, control->value_len);
            } else {
                int32_t known = p_ptrmap_get(&entries->direction, up);
                strong = known >= 0 ? (os64_bidi_strong_t)known : contained_direction(up);
                // A cache that cannot grow only costs the scan again.
                if (known < 0)
                    (void)p_ptrmap_put(&entries->direction, up, (int32_t)strong);
            }
            return strong == OS64_BIDI_R || strong == OS64_BIDI_AL ? "rtl" : "ltr";
        }
        if (control != NULL && control->input == OS64_PAGE_INPUT_TEL)
            return "ltr";
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
            const char *which = directionality(page, c, out);
            if (which == NULL || !add_normalised(out, c->dirname, which, os64_strlen(which), false))
                return false;
        }
    }
    return true;
}

void p_entries_free(PEntries *entries)
{
    p_ptrmap_free(&entries->direction);
    os64_free(entries->items);
    p_arena_free(&entries->arena);
    entries->items = NULL;
    entries->count = entries->cap = 0;
}

int64_t os64_page_form_entries(const os64_html_document_t *doc, const char *document_url, os64_page_state_t *state,
                               const os64_html_node_t *form,
                               void (*each)(void *opaque, const char *name, size_t name_len, const char *value,
                                            size_t value_len, bool is_file),
                               void *opaque)
{
    os64_page_t *page = os64_page_build(doc, document_url, NULL, state);
    if (page == NULL)
        return -1;
    int32_t index = os64_page_form_for(page, form);
    int64_t result = 0;
    if (index >= 0) {
        // A form submitting itself, as form.submit() does: no submitter.
        PEntries entries;
        os64_page_what_t what = {OS64_PAGE_ACTIVATE_FORM, index, 0, 0};
        if (!p_entry_list(page, index, -1, what, &entries))
            result = -1;
        for (int32_t i = 0; result == 0 && i < entries.count; i++)
            each(opaque, entries.items[i].name, entries.items[i].name_len, entries.items[i].value,
                 entries.items[i].value_len, entries.items[i].is_file);
        p_entries_free(&entries);
    }
    os64_page_free(page);
    return result;
}
