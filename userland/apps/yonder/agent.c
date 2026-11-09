// agent.c — who yonder says it is (agent.h).

#include "agent.h"
#include "os64/str.h"

// Each spelled as that browser spells it; a site that checks the number
// more closely can be handed another in the field. Chrome and Firefox as on
// Windows 10 and 11, which both report NT 10.0: their strings and their
// lines are what Chrome 156 and Firefox 158 sent a local listener in
// October 2026, byte for byte, header names in their own case (the W1
// capture files: WIRE.md § 1 as built names each). Chrome sends no
// Priority over HTTP/1.1, and asks every cache to revalidate on the
// browser's Reload; Firefox sends a Priority on every request, and its rows
// are the ones it sent for each destination asked (no font was, so a font
// goes without), and adds nothing on a Reload. Safari as on an iPhone, which many sites answer with
// a lighter page: not captured, its string as published in September 2026,
// no client hints (Safari has none), no Priority, and the language list
// MDN's Accept-Language page gives for Chrome and Safari alike. yonder's
// own asks for English as a real browser does, since a server that hears
// no language guesses one. Netscape 4, which the vintage-web sites answer
// as the old browser it is, and Lynx, which some sites answer with a
// text-only version, are costumes, and claim no lines beyond their strings.
//
// THE REST OF THE ASKING, from the same captures (WIRE.md § 2 as built).
// Each browser says `Connection: keep-alive`, and so do yonder's own and
// Safari's, since connections are kept now and the line says so
// (a costume says nothing it did not). Each browser's Accept per
// destination is what it sent, LESS every type yonder cannot decode: JPEG
// XL, AVIF, SVG as a picture and signed exchanges, which it does not read
// at all, and WebP and APNG, whose stills it decodes and whose animations
// it does not — an Accept that names a type invites it, and a server
// that would answer with a working GIF answers an `image/webp` with an
// animated WebP instead (WEBP.md § Accept). Wildcards stay: they are each
// browser's "anything else, less wanted", not a type. A font was not
// asked for (NULL: yonder's own list), and Safari's navigation list is
// MDN's for it, the rest of its own. yonder's asks for a page and anything
// else less: its other destinations' lists are its own, and already name
// only what it decodes. The line ORDER per kind of request is the
// browser's as captured, every line it names where it named it (a line it
// did not send in the captures, a page's own or a Cookie, goes after them
// in libfetch's order); Safari's is not captured, and yonder's is
// libfetch's own.
static const char *const kChromeNavigate =
    "Connection, Cache-Control, sec-ch-ua, sec-ch-ua-mobile, sec-ch-ua-platform, "
    "Upgrade-Insecure-Requests, User-Agent, Accept, Sec-Fetch-Site, Sec-Fetch-Mode, Sec-Fetch-User, "
    "Sec-Fetch-Dest, Referer, Accept-Encoding, Accept-Language";
// A page's own requests, no-cors and cors alike (capture-02..06, -08).
static const char *const kChromeRequest =
    "Connection, Content-Length, sec-ch-ua-platform, User-Agent, sec-ch-ua, sec-ch-ua-mobile, Accept, "
    "Origin, Sec-Fetch-Site, Sec-Fetch-Mode, Sec-Fetch-Dest, Referer, Accept-Encoding, Accept-Language";
static const char *const kChromePreflight =
    "Connection, Accept, Access-Control-Request-Method, Access-Control-Request-Headers, Origin, "
    "User-Agent, Sec-Fetch-Mode, Sec-Fetch-Site, Sec-Fetch-Dest, Referer, Accept-Encoding, Accept-Language";
static const char *const kFirefoxNavigate =
    "User-Agent, Accept, Accept-Language, Accept-Encoding, Connection, Referer, Upgrade-Insecure-Requests, "
    "Sec-Fetch-Dest, Sec-Fetch-Mode, Sec-Fetch-Site, Sec-Fetch-User, Priority, Pragma, Cache-Control";
static const char *const kFirefoxNoCors =
    "User-Agent, Accept, Accept-Language, Accept-Encoding, Connection, Referer, Sec-Fetch-Dest, "
    "Sec-Fetch-Mode, Sec-Fetch-Site, Priority, Pragma, Cache-Control";
static const char *const kFirefoxCors =
    "User-Agent, Accept, Accept-Language, Accept-Encoding, Referer, Origin, Connection, Sec-Fetch-Dest, "
    "Sec-Fetch-Mode, Sec-Fetch-Site, Priority, Pragma, Cache-Control, Content-Length";
static const char *const kFirefoxPreflight =
    "User-Agent, Accept, Accept-Language, Accept-Encoding, Access-Control-Request-Method, "
    "Access-Control-Request-Headers, Referer, Origin, Connection, Sec-Fetch-Dest, Sec-Fetch-Mode, "
    "Sec-Fetch-Site, Priority, Pragma, Cache-Control";

static const yonder_agent_preset_t kPresets[] = {
    {"yonder", YONDER_AGENT,
     {.language = "en-US,en;q=0.9", .keep_alive = true,
      .accept = {[WAY_DEST_DOCUMENT] = "text/html,application/xhtml+xml,text/*;q=0.9,*/*;q=0.8"}}},
    {"Chrome on Windows",
     "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
     "Chrome/156.0.0.0 Safari/537.36",
     {.hints = "sec-ch-ua: \"Not:A-Brand\";v=\"8\", \"Chromium\";v=\"156\", \"Google Chrome\";v=\"156\"\r\n"
               "sec-ch-ua-mobile: ?0\r\n"
               "sec-ch-ua-platform: \"Windows\"\r\n",
      .language = "en-US,en;q=0.9", .reload_cache_control = "max-age=0", .keep_alive = true,
      // capture-01 (a navigation), -02 (style), -03 (script), -04 (image),
      // -05 (fetch).
      .accept = {[WAY_DEST_DOCUMENT] = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
                 [WAY_DEST_STYLE] = "text/css,*/*;q=0.1", [WAY_DEST_SCRIPT] = "*/*",
                 [WAY_DEST_IMAGE] = "image/*,*/*;q=0.8", [WAY_DEST_EMPTY] = "*/*"},
      .order = {[WAY_ORDER_NAVIGATE] = kChromeNavigate, [WAY_ORDER_NO_CORS] = kChromeRequest,
                [WAY_ORDER_CORS] = kChromeRequest, [WAY_ORDER_PREFLIGHT] = kChromePreflight}}},
    {"Firefox on Windows",
     "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:158.0) Gecko/20100101 Firefox/158.0",
     {.language = "en-US,en;q=0.9", .keep_alive = true,
      .priority = {[WAY_DEST_DOCUMENT] = "u=0, i", [WAY_DEST_STYLE] = "u=2", [WAY_DEST_SCRIPT] = "u=2",
                   [WAY_DEST_IMAGE] = "u=5, i", [WAY_DEST_EMPTY] = "u=4"},
      // run1-overwritten/capture-09 (a navigation), -02 (style), -03
      // (script), -04 (image), -05 (fetch).
      .accept = {[WAY_DEST_DOCUMENT] = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
                 [WAY_DEST_STYLE] = "text/css,*/*;q=0.1", [WAY_DEST_SCRIPT] = "*/*",
                 [WAY_DEST_IMAGE] = "image/png,image/*;q=0.8,*/*;q=0.5", [WAY_DEST_EMPTY] = "*/*"},
      .order = {[WAY_ORDER_NAVIGATE] = kFirefoxNavigate, [WAY_ORDER_NO_CORS] = kFirefoxNoCors,
                [WAY_ORDER_CORS] = kFirefoxCors, [WAY_ORDER_PREFLIGHT] = kFirefoxPreflight}}},
    {"Safari on iPhone",
     "Mozilla/5.0 (iPhone; CPU iPhone OS 18_6 like Mac OS X) AppleWebKit/605.1.15 "
     "(KHTML, like Gecko) Version/26.0 Mobile/15E148 Safari/604.1",
     {.language = "en-US,en;q=0.9", .keep_alive = true,
      .accept = {[WAY_DEST_DOCUMENT] = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"}}},
    {"Netscape 4 on Windows 98", "Mozilla/4.08 [en] (Win98; U)", {0}},
    {"Lynx", "Lynx/2.9.2 libwww-FM/2.14 SSL-MM/1.4.1 OpenSSL/3.0.13", {0}},
};

// A preset's string as an earlier yonder spelled it, and the preset it is
// now: a person who saved "Chrome on Windows" saved its string, which
// would otherwise read as one typed, with none of the preset's lines.
static const struct {
    const char *agent;
    const char *now;                // the preset's name
} kRetired[] = {
    {"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
     "Chrome/154.0.0.0 Safari/537.36", "Chrome on Windows"},
    {"Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:156.0) Gecko/20100101 Firefox/156.0",
     "Firefox on Windows"},
};

size_t yonder_agent_npresets(void)
{
    return sizeof(kPresets) / sizeof(kPresets[0]);
}

const yonder_agent_preset_t *yonder_agent_preset(size_t i)
{
    return i < yonder_agent_npresets() ? &kPresets[i] : NULL;
}

bool yonder_agent_valid(const char *agent)
{
    if (agent == NULL || agent[0] == ' ' || agent[0] == '\0')
        return false;
    size_t n = 0;
    for (; agent[n] != '\0'; n++)
        if (agent[n] < 0x20 || agent[n] > 0x7e || agent[n] == '#')
            return false;
    return n < YONDER_AGENT_MAX && agent[n - 1] != ' ';
}

const char *yonder_agent_name(const char *agent)
{
    for (size_t i = 0; agent != NULL && i < yonder_agent_npresets(); i++)
        if (os64_streq(agent, kPresets[i].agent))
            return kPresets[i].name;
    return NULL;
}

const char *yonder_agent_current(const char *agent)
{
    for (size_t i = 0; agent != NULL && i < sizeof(kRetired) / sizeof(kRetired[0]); i++)
        if (os64_streq(agent, kRetired[i].agent))
            for (size_t j = 0; j < yonder_agent_npresets(); j++)
                if (os64_streq(kPresets[j].name, kRetired[i].now))
                    return kPresets[j].agent;
    return agent;
}

const way_identity_t *yonder_agent_identity(const char *agent)
{
    for (size_t i = 0; agent != NULL && i < yonder_agent_npresets(); i++)
        if (os64_streq(agent, kPresets[i].agent))
            return &kPresets[i].identity;
    return NULL;
}
