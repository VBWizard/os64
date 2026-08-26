// menu.c — the menu grammar (os64/menu.h has the language; this is the reader).
//
// Line-oriented, because every os64 config is: a '#' outside quotes ends
// the line, blank lines are nothing, and one statement is one line. The
// braces are the one nod to structure — `menu "Label" {` opens a cascade
// and `}` on its own line closes it — and they are what makes the same
// file describe a root menu, a dock and whatever comes after both.
//
// Built once, in the library, so every launcher parses the file the same
// way: a label that quotes correctly in the root menu and not in the dock
// would be the kind of drift the shared grammar exists to prevent.

#include "os64/menu.h"
#include "os64/slurp.h"
#include "os64/str.h"
#include "os64/fmt.h"
#include "os64/mem.h"

#define MENU_DEPTH_MAX 16
#define WORD_MAX       OS64_MENU_COMMAND_MAX

// ── the tokenizer ───────────────────────────────────────────────────────────

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// Next word from *p (never crossing the end of the line): whitespace
// separates, double quotes group (and are stripped), and when `braces` is
// set a lone '{' or '}' is a word of its own. Returns false at end of
// line. `quoted` reports whether the word wore quotes — a bare `{` and a
// quoted "{" are different things.
static bool next_word(const char **p, const char *end, char *out, size_t cap,
                      bool braces, bool *quoted)
{
    const char *s = *p;
    while (s < end && is_space(*s))
        s++;
    if (s >= end) {
        *p = s;
        return false;
    }
    size_t n = 0;
    *quoted = false;
    if (braces && (*s == '{' || *s == '}')) {
        if (cap > 1) { out[0] = *s; out[1] = 0; }
        *p = s + 1;
        return true;
    }
    if (*s == '"') {
        *quoted = true;
        s++;
        while (s < end && *s != '"') {
            if (n + 1 < cap)
                out[n++] = *s;
            s++;
        }
        if (s < end)
            s++;   // the closing quote
    } else {
        while (s < end && !is_space(*s) && !(braces && (*s == '{' || *s == '}'))) {
            if (n + 1 < cap)
                out[n++] = *s;
            s++;
        }
    }
    out[n < cap ? n : cap - 1] = 0;
    *p = s;
    return true;
}

// Where the line's content ends: at the newline, or at a '#' that is not
// inside quotes. A '#' inside "..." is part of a label or a command.
static const char *line_end(const char *s, const char *stop)
{
    bool in_quote = false;
    for (; s < stop && *s != '\n'; s++) {
        if (*s == '"')
            in_quote = !in_quote;
        else if (*s == '#' && !in_quote)
            return s;
    }
    return s;
}

// ── the parser ──────────────────────────────────────────────────────────────

typedef struct
{
    int16_t *first;   // where this scope's first child is recorded
    int16_t  last;    // the last child appended, -1 = none yet
    int      line;    // where it was opened, for "unclosed" complaints
} scope_t;

static void complain(char *err, size_t cap, const char *path, int line, const char *what)
{
    if (err != NULL && cap != 0)
        os64_snprintf(err, cap, "%s:%d: %s", path, line, what);
}

static int16_t new_node(os64_menu_t *m, uint8_t kind, const char *label)
{
    if (m->count >= OS64_MENU_NODES_MAX)
        return -1;
    os64_menu_node_t *n = &m->nodes[m->count];
    os64_memset(n, 0, sizeof(*n));
    n->kind = kind;
    n->first_child = -1;
    n->next = -1;
    if (label != NULL)
        os64_strcopy(n->label, sizeof(n->label), label);
    return (int16_t)m->count++;
}

static void append(os64_menu_t *m, scope_t *sc, int16_t node)
{
    if (sc->last < 0)
        *sc->first = node;
    else
        m->nodes[sc->last].next = node;
    sc->last = node;
}

static os64_menu_status_t parse(os64_menu_t *m, const char *text, size_t len,
                                char *err, size_t err_cap)
{
    scope_t stack[MENU_DEPTH_MAX];
    int depth = 0;
    int line = 0;
    const char *s = text, *stop = text + len;
    char w1[WORD_MAX], w2[WORD_MAX], w3[WORD_MAX];
    bool q1, q2, q3;

    while (s < stop) {
        line++;
        const char *end = line_end(s, stop);
        const char *nl  = end;
        while (nl < stop && *nl != '\n')
            nl++;
        const char *p = s;
        s = (nl < stop) ? nl + 1 : nl;

        if (!next_word(&p, end, w1, sizeof(w1), true, &q1))
            continue;   // blank, or only a comment

        if (!q1 && os64_streq(w1, "}")) {
            if (depth == 0) {
                complain(err, err_cap, m->path, line, "'}' with no menu open");
                return OS64_MENU_SYNTAX;
            }
            depth--;
            if (next_word(&p, end, w2, sizeof(w2), true, &q2)) {
                complain(err, err_cap, m->path, line, "unexpected text after '}'");
                return OS64_MENU_SYNTAX;
            }
            continue;
        }

        // Keywords fold case; everything that follows them is data.
        if (!q1 && os64_streq_nocase(w1, "menu")) {
            if (!next_word(&p, end, w2, sizeof(w2), true, &q2) || w2[0] == 0 ||
                (!q2 && (os64_streq(w2, "{") || os64_streq(w2, "}")))) {
                complain(err, err_cap, m->path, line, "menu needs a name or a label");
                return OS64_MENU_SYNTAX;
            }
            bool have3 = next_word(&p, end, w3, sizeof(w3), true, &q3);
            bool opens = have3 && !q3 && os64_streq(w3, "{");

            if (depth == 0) {
                // A NAMED menu. It always opens a block: a top-level
                // reference would be a menu that is just another menu.
                if (!opens) {
                    complain(err, err_cap, m->path, line, "a named menu is `menu <name> {`");
                    return OS64_MENU_SYNTAX;
                }
                if (m->named_count >= OS64_MENU_NAMED_MAX) {
                    complain(err, err_cap, m->path, line, "too many named menus");
                    return OS64_MENU_TOO_MANY;
                }
                if (os64_menu_named_exists(m, w2)) {
                    complain(err, err_cap, m->path, line, "a menu of that name already exists");
                    return OS64_MENU_SYNTAX;
                }
                os64_menu_named_t *nm = &m->named[m->named_count++];
                os64_strcopy(nm->name, sizeof(nm->name), w2);
                nm->first_child = -1;
                stack[depth++] = (scope_t){ .first = &nm->first_child, .last = -1, .line = line };
            } else {
                // A cascade: inline (opens a block) or by reference (names
                // a menu defined elsewhere in the file, twm's f.menu).
                if (!have3) {
                    complain(err, err_cap, m->path, line, "a cascade is `menu \"Label\" {` or `menu \"Label\" <name>`");
                    return OS64_MENU_SYNTAX;
                }
                int16_t node = new_node(m, OS64_MENU_SUBMENU, w2);
                if (node < 0) {
                    complain(err, err_cap, m->path, line, "too many menu entries");
                    return OS64_MENU_TOO_MANY;
                }
                append(m, &stack[depth - 1], node);
                if (opens) {
                    if (depth >= MENU_DEPTH_MAX) {
                        complain(err, err_cap, m->path, line, "menus nested too deeply");
                        return OS64_MENU_TOO_MANY;
                    }
                    stack[depth++] = (scope_t){ .first = &m->nodes[node].first_child,
                                                .last = -1, .line = line };
                } else {
                    os64_strcopy(m->nodes[node].ref, sizeof(m->nodes[node].ref), w3);
                }
            }
            if (opens && next_word(&p, end, w3, sizeof(w3), true, &q3)) {
                complain(err, err_cap, m->path, line, "unexpected text after '{'");
                return OS64_MENU_SYNTAX;
            }
            continue;
        }

        if (depth == 0) {
            complain(err, err_cap, m->path, line, "an entry outside any menu (open one with `menu <name> {`)");
            return OS64_MENU_SYNTAX;
        }

        if (!q1 && os64_streq_nocase(w1, "item")) {
            if (!next_word(&p, end, w2, sizeof(w2), false, &q2) || w2[0] == 0) {
                complain(err, err_cap, m->path, line, "item needs a label");
                return OS64_MENU_SYNTAX;
            }
            // The command is the REST OF THE LINE, verbatim, trimmed.
            while (p < end && is_space(*p))
                p++;
            const char *ce = end;
            while (ce > p && is_space(ce[-1]))
                ce--;
            if (ce == p) {
                complain(err, err_cap, m->path, line, "item needs a command after its label");
                return OS64_MENU_SYNTAX;
            }
            if ((size_t)(ce - p) >= OS64_MENU_COMMAND_MAX) {
                complain(err, err_cap, m->path, line, "command too long");
                return OS64_MENU_SYNTAX;
            }
            int16_t node = new_node(m, OS64_MENU_ITEM, w2);
            if (node < 0) {
                complain(err, err_cap, m->path, line, "too many menu entries");
                return OS64_MENU_TOO_MANY;
            }
            os64_memcpy(m->nodes[node].command, p, (size_t)(ce - p));
            m->nodes[node].command[ce - p] = 0;
            append(m, &stack[depth - 1], node);
            continue;
        }

        if (!q1 && (os64_streq_nocase(w1, "separator") || os64_streq(w1, "---"))) {
            if (next_word(&p, end, w2, sizeof(w2), true, &q2)) {
                complain(err, err_cap, m->path, line, "separator takes nothing after it");
                return OS64_MENU_SYNTAX;
            }
            int16_t node = new_node(m, OS64_MENU_SEPARATOR, NULL);
            if (node < 0) {
                complain(err, err_cap, m->path, line, "too many menu entries");
                return OS64_MENU_TOO_MANY;
            }
            append(m, &stack[depth - 1], node);
            continue;
        }

        complain(err, err_cap, m->path, line, "expected item, separator, menu or '}'");
        return OS64_MENU_SYNTAX;
    }

    if (depth > 0) {
        char what[96];
        os64_snprintf(what, sizeof(what), "menu opened here was never closed with '}'");
        complain(err, err_cap, m->path, stack[depth - 1].line, what);
        return OS64_MENU_SYNTAX;
    }

    // Resolve the cascades-by-reference now that every name is known. A
    // reference to a menu defined LATER in the file is fine — that is the
    // whole reason this is a second pass.
    for (uint16_t i = 0; i < m->count; i++) {
        os64_menu_node_t *n = &m->nodes[i];
        if (n->kind != OS64_MENU_SUBMENU || n->ref[0] == 0)
            continue;
        int16_t first = os64_menu_find(m, n->ref);
        if (first < 0 && !os64_menu_named_exists(m, n->ref)) {
            char what[96];
            os64_snprintf(what, sizeof(what), "cascade \"%s\" references a menu named \"%s\" that is not defined",
                          n->label, n->ref);
            complain(err, err_cap, m->path, 0, what);
            return OS64_MENU_UNRESOLVED;
        }
        n->first_child = first;
    }
    return OS64_MENU_OK;
}

// ── the API ─────────────────────────────────────────────────────────────────

bool os64_menu_named_exists(const os64_menu_t *menu, const char *name)
{
    for (uint16_t i = 0; i < menu->named_count; i++)
        if (os64_streq(menu->named[i].name, name))
            return true;
    return false;
}

int16_t os64_menu_find(const os64_menu_t *menu, const char *name)
{
    for (uint16_t i = 0; i < menu->named_count; i++)
        if (os64_streq(menu->named[i].name, name))
            return menu->named[i].first_child;
    return -1;
}

os64_menu_status_t os64_menu_load(os64_menu_t *menu, const char *name,
                                  char *err, size_t err_cap)
{
    os64_memset(menu, 0, sizeof(*menu));
    if (err != NULL && err_cap != 0)
        err[0] = 0;

    if (os64_conf_find(name, menu->path, sizeof(menu->path)) != 0) {
        os64_strcopy(menu->path, sizeof(menu->path), name);
        return OS64_MENU_NO_FILE;
    }

    uint8_t *text = NULL;
    size_t len = 0;
    os64_slurp_status_t st = os64_slurp(menu->path, OS64_MENU_FILE_MAX, &text, &len);
    switch (st) {
    case OS64_SLURP_OK:      break;
    case OS64_SLURP_NO_FILE: return OS64_MENU_NO_FILE;
    case OS64_SLURP_TOO_BIG: return OS64_MENU_TOO_BIG;
    default:                 return OS64_MENU_IO_ERROR;
    }

    os64_menu_status_t rc = parse(menu, (const char *)text, len, err, err_cap);
    os64_free(text);
    return rc;
}

const char *os64_menu_status_name(os64_menu_status_t status)
{
    switch (status) {
    case OS64_MENU_OK:         return "ok";
    case OS64_MENU_NO_FILE:    return "no menu file found";
    case OS64_MENU_TOO_BIG:    return "menu file too large";
    case OS64_MENU_IO_ERROR:   return "read error";
    case OS64_MENU_NO_MEMORY:  return "out of memory";
    case OS64_MENU_SYNTAX:     return "syntax error";
    case OS64_MENU_TOO_MANY:   return "too many entries";
    case OS64_MENU_UNRESOLVED: return "unresolved cascade";
    }
    return "unknown";
}

size_t os64_menu_argv(const char *command, char *buf, size_t buf_cap,
                      char *argv[], size_t argv_max)
{
    const char *p = command;
    const char *end = command + os64_strlen(command);
    size_t used = 0, n = 0;
    char word[OS64_MENU_COMMAND_MAX];
    bool quoted;

    while (n + 1 < argv_max && next_word(&p, end, word, sizeof(word), false, &quoted)) {
        size_t wl = os64_strlen(word) + 1;
        if (used + wl > buf_cap)
            return 0;
        os64_memcpy(buf + used, word, wl);
        argv[n++] = buf + used;
        used += wl;
    }
    argv[n] = NULL;
    return n;
}
