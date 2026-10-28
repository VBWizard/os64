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
    {(const char *const[]){"Node", "CharacterData", "Text", "Comment", "DocumentFragment", "Document",
                           "DocumentType", "Attr", "NamedNodeMap", "NodeList", "HTMLCollection",
                           "DOMTokenList", "DOMStringMap", "Element", "SVGElement",
                           "HTMLElement", "HTMLFormElement", "HTMLInputElement",
                           "HTMLTextAreaElement", "HTMLSelectElement", "HTMLOptionElement",
                           "HTMLOptGroupElement", "HTMLTemplateElement", "HTMLButtonElement",
                           "HTMLImageElement", "HTMLScriptElement", "HTMLAnchorElement",
                           "HTMLLinkElement", "HTMLLabelElement", "HTMLFieldSetElement",
                           "HTMLLegendElement", "HTMLOutputElement", "HTMLStyleElement",
                           "HTMLMetaElement", "HTMLCanvasElement", "HTMLUnknownElement", "HTMLTimeElement",
                           "HTMLDivElement", "HTMLBodyElement", "HTMLOListElement", "HTMLHeadElement",
                           "HTMLDataElement", "HTMLLIElement", "HTMLAreaElement", "HTMLBaseElement",
                           "HTMLMapElement", "HTMLModElement", "HTMLQuoteElement", "HTMLTableCellElement",
                           "HTMLTableColElement", "HTMLTableElement", "HTMLTableRowElement",
                           "HTMLTableSectionElement", "HTMLUListElement", "HTMLBRElement", "HTMLDListElement",
                           "HTMLDirectoryElement", "HTMLFontElement", "HTMLHRElement", "HTMLHeadingElement",
                           "HTMLHtmlElement", "HTMLMenuElement", "HTMLParagraphElement", "HTMLPreElement",
                           "HTMLSpanElement", "HTMLTableCaptionElement", "HTMLTitleElement", NULL},
     d_interfaces_install},
    /* After the interfaces row, whose HTMLElement its interface inherits. */
    {(const char *const[]){"HTMLIFrameElement", NULL}, d_frames_install},
    {(const char *const[]){"scrollX", "pageXOffset", "scrollY", "pageYOffset", "innerWidth",
                           "innerHeight", "outerWidth", "outerHeight", "scroll", "scrollTo",
                           "scrollBy", "onscroll", "onresize", "focus", "blur", NULL},
     d_view_install},
    {(const char *const[]){"matchMedia", "MediaQueryList", "MediaQueryListEvent", NULL}, d_media_install},
    {(const char *const[]){"getComputedStyle", "CSSStyleDeclaration", NULL}, d_computed_install},
    {(const char *const[]){"onpopstate", "onhashchange", "PopStateEvent", "HashChangeEvent", NULL},
     d_history_install},
    {(const char *const[]){"localStorage", "sessionStorage", "Storage", NULL}, d_storage_install},
    {(const char *const[]){"performance", "Performance", "PerformanceEntry", "PerformanceMark",
                           "PerformanceMeasure", "PerformanceTiming", "PerformanceNavigation", NULL},
     d_performance_install},
    {(const char *const[]){"URL", "URLSearchParams", NULL}, d_url_install},
    {(const char *const[]){"TextEncoder", "TextDecoder", NULL}, d_encoding_install},
    {(const char *const[]){"screen", "devicePixelRatio", "Screen", "ScreenOrientation", NULL}, d_screen_install},
    {(const char *const[]){"btoa", "atob", NULL}, d_base64_install},
    {(const char *const[]){"DOMImplementation", "DOMParser", NULL}, d_implementation_install},
    {(const char *const[]){"DOMException", NULL}, d_exception_install},
    {(const char *const[]){"AbortController", "AbortSignal", NULL}, d_abort_install},
    {(const char *const[]){"Blob", "File", "FormData", NULL}, d_blob_install},
    {(const char *const[]){"fetch", "Headers", "Request", "Response", NULL}, d_fetch_install},
    {(const char *const[]){"XMLHttpRequest", "XMLHttpRequestEventTarget", "XMLHttpRequestUpload", "ProgressEvent",
                           NULL},
     d_xhr_install},
    {(const char *const[]){"opener", "closed", "clientInformation", "isSecureContext", "frameElement", "origin",
                           "crossOriginIsolated", "originAgentCluster", "close", "moveTo", "moveBy", "resizeTo",
                           "resizeBy", "captureEvents", "releaseEvents", "queueMicrotask", "ononline",
                           "onoffline", "onlanguagechange", NULL},
     d_browsing_install},
    {(const char *const[]){"locationbar", "menubar", "personalbar", "scrollbars", "statusbar", "toolbar", "BarProp",
                           NULL},
     d_bars_install},
    {(const char *const[]){"external", "External", NULL}, d_external_install},
    /* After the encoding row, whose DataView it tells a view by. */
    {(const char *const[]){"crypto", "Crypto", NULL}, d_crypto_install},
    {(const char *const[]){"onerror", "onunhandledrejection", "onrejectionhandled", "reportError", "ErrorEvent",
                           "PromiseRejectionEvent", NULL},
     d_report_install},
    /* P1-Da: the document's state reads, members of Document's prototype
     * and none of window's, so the row lists no name; the DOM host's chain
     * shape holds them instead (tools/test_dom_modern.inc). */
    {(const char *const[]){NULL}, d_document_install},
    {(const char *const[]){"CustomEvent", NULL}, d_custom_event_install},
    /* P2b. The clone is read and written through the encoding row's
     * DataView, the blob row's File and the exception row's DOMException,
     * all installed above. */
    {(const char *const[]){"structuredClone", NULL}, d_clone_install},
    {(const char *const[]){"postMessage", "onmessage", "onmessageerror", "MessageEvent", NULL}, d_message_install},
    {(const char *const[]){"requestIdleCallback", "cancelIdleCallback", "IdleDeadline", NULL}, d_idle_install},
    /* 0f's node row: members of Node's, Attr's, Text's and a template's
     * prototypes, none of window's (as P1-Da's row). */
    {(const char *const[]){NULL}, d_nodes_install},
    /* After the interfaces row, whose HTMLCollection its
     * HTMLOptionsCollection inherits. */
    {(const char *const[]){"Option", "HTMLOptionsCollection", NULL}, d_options_install},
    /* 0f's P2 core: the text controls' members, and the Selection. */
    {(const char *const[]){"getSelection", "Selection", NULL}, d_selection_install},
    /* 0f's P3a: the controls' reflections and state, members of the
     * interfaces row's control prototypes and none of window's. */
    {(const char *const[]){NULL}, d_controls_install},
    /* 0f's P6a: the element kinds whose members are reflections, members
     * of the interfaces row's prototypes and none of window's. */
    {(const char *const[]){NULL}, d_elements_install},
    /* 0f's P3b: a form's controls collection and RadioNodeList, over the
     * interfaces row's HTMLCollection and NodeList; and the form's,
     * label's, fieldset's, legend's and output's own members. */
    {(const char *const[]){"HTMLFormControlsCollection", "RadioNodeList", NULL}, d_form_collections_install},
    {(const char *const[]){NULL}, d_forms_install},
    /* 0f's P4: constraint validation, members of the interfaces row's
     * control and form prototypes, and ValidityState and oninvalid. */
    {(const char *const[]){"ValidityState", "oninvalid", NULL}, d_validity_install},
    /* The page's style sheets: StyleSheetList, StyleSheet and
     * CSSStyleSheet, document.styleSheets, and a link's and a style's
     * `sheet` (sheets.c), after the interfaces row's HTMLStyleElement. */
    {(const char *const[]){"StyleSheetList", "StyleSheet", "CSSStyleSheet", NULL}, d_sheets_install},
    /* Import maps: HTMLScriptElement.supports, a member of the interfaces
     * row's HTMLScriptElement and none of window's (as P1-Da's row). */
    {(const char *const[]){NULL}, d_import_map_install},
    /* 0f's P1b: last, since every interface whose prototype inherits
     * EventTarget.prototype is made to inherit EventTarget. */
    {(const char *const[]){"EventTarget", "Window", NULL}, d_targets_install},
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
