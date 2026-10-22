#include "internal.h"

/* THE MODERN WEB'S TABLE (MODERN_WEB.md § How the slices run side by side).
 * Every global the arc adds to window is installed by a row here and
 * nowhere else, so os64_dom_options_t.modern removes all of it at once: with
 * the option clear the walk does not run, and a name that is not installed
 * is a name a page's feature detection finds missing, which is the point
 * (rule 1: a surface either exists whole or does not exist). What the
 * switch takes away is the table's names and what its rows put on them;
 * the prototype chain's levels are the same with it on or off.
 *
 * A row is the names it puts on window and the installer that puts them
 * there; a slice adds its file and its row. The names are the row's whole
 * promise: the DOM host holds every row to them, comparing window's own
 * names with the option set and clear (tools/test_dom_modern.inc), so a
 * name a row installs without listing, or lists without installing, fails
 * the suite. The rows run in order, once, at creation, after the classic
 * window is whole; a row that fails fails the creation, as every installer
 * does. */

typedef struct {
    const char *const *names;   /* NULL-terminated */
    int (*install)(os64_dom_t *dom, JSContext *ctx, JSValueConst global);
} DModernRow;

static const DModernRow kModern[] = {
    {(const char *const[]){"Node", "CharacterData", "DocumentFragment", "Document", "Element",
                           "HTMLElement", "HTMLFormElement", "HTMLInputElement",
                           "HTMLTextAreaElement", "HTMLSelectElement", "HTMLButtonElement",
                           "HTMLImageElement", "HTMLScriptElement", "HTMLAnchorElement",
                           "HTMLLinkElement", NULL},
     d_interfaces_install},
    {(const char *const[]){"scrollX", "pageXOffset", "scrollY", "pageYOffset", "innerWidth",
                           "innerHeight", "outerWidth", "outerHeight", "scroll", "scrollTo",
                           "scrollBy", "onscroll", "onresize", "focus", "blur", NULL},
     d_view_install},
    {(const char *const[]){"matchMedia", "MediaQueryList", "MediaQueryListEvent", NULL}, d_media_install},
    {(const char *const[]){"getComputedStyle", "CSSStyleDeclaration", NULL}, d_computed_install},
    {(const char *const[]){"onpopstate", "onhashchange", "PopStateEvent", "HashChangeEvent", NULL},
     d_history_install},
};

int d_modern_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    if (!dom->options.modern) return 0;
    /* A row that leaves an exception pending failed, whatever it answered:
     * the engine's function constructors define `length` and `name`
     * without saying when that ran out of memory. */
    for (size_t i = 0; i < sizeof(kModern) / sizeof(kModern[0]); i++)
        if (kModern[i].install(dom, ctx, global) < 0 || JS_HasException(ctx)) return -1;
    return 0;
}

const char *d_modern_name(size_t index)
{
    for (size_t i = 0; i < sizeof(kModern) / sizeof(kModern[0]); i++)
        for (const char *const *name = kModern[i].names; *name != NULL; name++)
            if (index-- == 0) return *name;
    return NULL;
}
