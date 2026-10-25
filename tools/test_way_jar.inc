// The cookie jar and the Referer policy (way/jar.h, YONDER.md § Y3b):
// RFC 6265's dates, attributes, matching and order, the 6265bis rules the
// browsers ship, the limits, and what a hop is sent. Each expected epoch
// was worked out with a calendar, and the famous one is RFC 6265's own
// example date.

static int64_t date_of(const char *text)
{
    int64_t when = -1;
    return way_cookie_date(text, strlen(text), &when) ? when : -1;
}

static void cookie_dates(void)
{
    // 1994-11-06 08:49:37 UTC, in the three spellings HTTP has had.
    expect("date: RFC 1123", date_of("Sun, 06 Nov 1994 08:49:37 GMT") == 784111777, NULL);
    expect("date: RFC 850, a two-digit year",
           date_of("Sunday, 06-Nov-94 08:49:37 GMT") == 784111777, NULL);
    expect("date: asctime", date_of("Sun Nov  6 08:49:37 1994") == 784111777, NULL);
    expect("date: no weekday, lowercase month", date_of("06 nov 1994 08:49:37") == 784111777, NULL);
    expect("date: the epoch", date_of("Thu, 01 Jan 1970 00:00:00 GMT") == 0, NULL);
    expect("date: 2038 is not the end", date_of("Fri, 01 Jan 2038 00:00:00 GMT") == 2145916800,
           NULL);
    expect("date: a leap day", date_of("29 Feb 2024 12:00:00") == 1709208000, NULL);
    expect("date: 69 is 2069", date_of("01 Jan 69 00:00:00") == 3124224000LL, NULL);
    expect("date: no such day", date_of("30 Feb 2020 10:00:00") < 0, NULL);
    expect("date: not a leap year", date_of("29 Feb 2023 10:00:00") < 0, NULL);
    expect("date: before 1601", date_of("31 Dec 1600 00:00:00") < 0, NULL);
    expect("date: no time", date_of("Sun, 06 Nov 1994") < 0, NULL);
    expect("date: an hour past 23", date_of("06 Nov 1994 24:00:00") < 0, NULL);
    expect("date: nonsense", date_of("tomorrow") < 0, NULL);
    expect("date: empty", date_of("") < 0, NULL);
}

static os64_url_t url_of(const char *text)
{
    os64_url_t u;
    memset(&u, 0, sizeof(u));
    if (os64_url_parse(text, &u) != OS64_URL_OK)
        expect("test address parses", false, text);
    return u;
}

static void hear(way_jar_t *jar, const char *from, bool encrypted, const char *line, int64_t now)
{
    os64_url_t u = url_of(from);
    way_jar_hear(jar, &u, encrypted, line, strlen(line), now);
}

// What a request to `to` sends, in a buffer that stays valid until the
// next call.
static const char *sent(way_jar_t *jar, const char *to, bool encrypted, int64_t now)
{
    static char out[4096];
    os64_url_t u = url_of(to);
    way_jar_cookies(jar, &u, encrypted, now, out, sizeof(out), NULL);
    return out;
}

static void jar_expect(const char *name, way_jar_t *jar, const char *to, bool encrypted,
                       int64_t now, const char *want)
{
    const char *got = sent(jar, to, encrypted, now);
    expect(name, strcmp(got, want) == 0, got);
}

static void cookie_scope(void)
{
    way_jar_t *jar = way_jar_new();
    hear(jar, "http://h.test/x/y", false, "a=1", 100);
    jar_expect("scope: the directory it was set from", jar, "http://h.test/x/z", false, 100, "a=1");
    jar_expect("scope: not above it", jar, "http://h.test/", false, 100, "");
    jar_expect("scope: not a sibling spelled alike", jar, "http://h.test/xy", false, 100, "");
    jar_expect("scope: the directory itself", jar, "http://h.test/x", false, 100, "a=1");
    jar_expect("scope: a query is not a path", jar, "http://h.test/x?p=/", false, 100, "a=1");
    jar_expect("scope: not another host", jar, "http://other.test/x/z", false, 100, "");
    jar_expect("scope: host-only, not a subdomain", jar, "http://a.h.test/x/z", false, 100, "");
    hear(jar, "http://h.test/deep/er/page?next=/a/b", false, "q=1", 100);
    jar_expect("scope: the default path ignores the query", jar, "http://h.test/deep/er/x", false,
               100, "q=1");
    way_jar_free(jar);

    jar = way_jar_new();
    hear(jar, "http://www.example.com/", false, "d=1; Domain=.Example.COM; Path=/", 100);
    jar_expect("domain: the domain itself", jar, "http://example.com/", false, 100, "d=1");
    jar_expect("domain: a subdomain", jar, "http://a.b.example.com/p", false, 100, "d=1");
    jar_expect("domain: not a name that merely ends alike", jar, "http://badexample.com/", false,
               100, "");
    hear(jar, "http://www.example.com/", false, "o=1; Domain=other.com; Path=/", 100);
    hear(jar, "http://www.example.com/", false, "t=1; Domain=com; Path=/", 100);
    hear(jar, "http://www.example.com/", false, "s=1; Domain=sub.www.example.com; Path=/", 100);
    expect("domain: another domain, a bare suffix, a narrower name are refused",
           way_jar_count(jar) == 1, NULL);
    hear(jar, "http://localhost/", false, "l=1; Domain=localhost; Path=/", 100);
    jar_expect("domain: a single label that is the host itself", jar, "http://localhost/", false,
               100, "l=1");
    hear(jar, "http://10.0.2.2/", false, "i=1; Domain=0.2.2; Path=/", 100);
    hear(jar, "http://10.0.2.2/", false, "j=1; Domain=10.0.2.2; Path=/", 100);
    jar_expect("domain: an address takes only itself", jar, "http://10.0.2.2/", false, 100, "j=1");
    way_jar_free(jar);
}

static void cookie_security(void)
{
    way_jar_t *jar = way_jar_new();
    hear(jar, "http://s.test/", false, "s=1; Secure; Path=/", 100);
    expect("secure: not set over the clear", way_jar_count(jar) == 0, NULL);
    hear(jar, "https://s.test/", true, "s=1; Secure; Path=/", 100);
    jar_expect("secure: sent encrypted", jar, "https://s.test/", true, 100, "s=1");
    jar_expect("secure: never in the clear", jar, "http://s.test/", false, 100, "");
    hear(jar, "http://s.test/", false, "s=2; Path=/", 100);
    jar_expect("secure: the clear cannot shadow it", jar, "https://s.test/", true, 100, "s=1");
    hear(jar, "https://s.test/", true, "s=3; Path=/", 100);
    jar_expect("secure: an encrypted reply replaces it", jar, "http://s.test/", false, 100, "s=3");
    hear(jar, "https://s.test/", true, "__Secure-a=1; Path=/", 100);
    hear(jar, "https://s.test/", true, "__secure-b=1; Secure; Path=/", 100);
    hear(jar, "https://s.test/", true, "__Host-c=1; Secure; Path=/; Domain=s.test", 100);
    hear(jar, "https://s.test/", true, "__Host-d=1; Secure; Path=/x", 100);
    hear(jar, "https://s.test/", true, "__Host-e=1; Secure; Path=/", 100);
    jar_expect("prefixes: held to their promises, whatever their case", jar, "https://s.test/",
               true, 100, "s=3; __secure-b=1; __Host-e=1");
    way_jar_free(jar);
}

static void cookie_lifetimes(void)
{
    way_jar_t *jar = way_jar_new();
    hear(jar, "http://e.test/", false, "m=1; Max-Age=10", 1000);
    jar_expect("max-age: alive before", jar, "http://e.test/", false, 1009, "m=1");
    jar_expect("max-age: gone after", jar, "http://e.test/", false, 1010, "");
    expect("max-age: gone from the jar", way_jar_count(jar) == 0, NULL);

    hear(jar, "http://e.test/", false, "x=1; Expires=Thu, 01 Jan 1970 00:00:10 GMT; Max-Age=100",
         1000);
    jar_expect("max-age: outranks expires", jar, "http://e.test/", false, 1050, "x=1");
    hear(jar, "http://e.test/", false, "x=gone; Max-Age=0", 1060);
    jar_expect("max-age: zero deletes its namesake", jar, "http://e.test/", false, 1060, "");
    hear(jar, "http://e.test/", false, "y=1", 1000);
    hear(jar, "http://e.test/", false, "y=2; Expires=Thu, 01 Jan 1970 00:00:10 GMT", 1000);
    jar_expect("expires: a date passed deletes its namesake", jar, "http://e.test/", false, 1001,
               "");
    hear(jar, "http://e.test/", false, "z=1; Expires=someday", 1000);
    jar_expect("expires: an unreadable date is a session cookie", jar, "http://e.test/", false,
               INT64_MAX - 1, "z=1");
    hear(jar, "http://e.test/", false, "n=1; Max-Age=soon", 1000);
    hear(jar, "http://e.test/", false, "k=1; Max-Age=-5", 1000);
    jar_expect("max-age: not a number is ignored, a negative is gone", jar, "http://e.test/", false,
               1001, "z=1; n=1");
    way_jar_free(jar);

    jar = way_jar_new();
    hear(jar, "http://r.test/", false, "r=1", 10);
    hear(jar, "http://r.test/", false, "r=2", 20);
    jar_expect("replace: one cookie, the newer value", jar, "http://r.test/", false, 30, "r=2");
    expect("replace: counted once", way_jar_count(jar) == 1, NULL);
    hear(jar, "http://r.test/", false, "r=3; Path=/", 25);
    hear(jar, "http://r.test/a/", false, "r=4; Path=/a", 26);
    jar_expect("replace: the same path spelled is the same cookie; another is another, first",
               jar, "http://r.test/a/b", false, 30, "r=4; r=3");
    way_jar_free(jar);
}

static void cookie_text(void)
{
    way_jar_t *jar = way_jar_new();
    hear(jar, "http://t.test/", false, "  spaced  =  out  ; Path = / ", 1);
    hear(jar, "http://t.test/", false, "justavalue", 2);
    hear(jar, "http://t.test/", false, "=", 3);
    hear(jar, "http://t.test/", false, "", 3);
    hear(jar, "http://t.test/", false, "bad=a\x01z", 3);
    hear(jar, "http://t.test/", false, "tab=a\tb", 4);
    hear(jar, "http://t.test/", false, "UPPER=1; PATH=/; SECURE; HttpOnly; SameSite=Strict", 5);
    jar_expect("text: trimmed, nameless, controls refused, tab kept, attributes any case", jar,
               "http://t.test/", false, 10, "spaced=out; justavalue; tab=a\tb");
    jar_expect("text: SECURE in capitals is still secure", jar, "https://t.test/", true, 10,
               "spaced=out; justavalue; tab=a\tb");
    way_jar_free(jar);

    jar = way_jar_new();
    char big[WAY_COOKIE_BYTES_MAX + 8];
    memset(big, 'v', sizeof(big) - 1);
    big[0] = 'b';
    big[1] = '=';
    big[sizeof(big) - 1] = '\0';
    hear(jar, "http://t.test/", false, big, 1);
    big[WAY_COOKIE_BYTES_MAX + 1] = '\0';     // "b=" and 4095 v's: 4096 of name and value
    hear(jar, "http://t.test/", false, big, 1);
    expect("text: past 4096 bytes refused, at it kept", way_jar_count(jar) == 1, NULL);
    way_jar_free(jar);
}

static void cookie_order_and_room(void)
{
    way_jar_t *jar = way_jar_new();
    hear(jar, "http://o.test/", false, "a=1; Path=/", 1);
    hear(jar, "http://o.test/", false, "b=2; Path=/x", 2);
    hear(jar, "http://o.test/", false, "c=3; Path=/", 3);
    hear(jar, "http://o.test/", false, "d=4; Path=/x/y", 4);
    jar_expect("order: longest path first, then oldest", jar, "http://o.test/x/y/z", false, 5,
               "d=4; b=2; a=1; c=3");
    char out[14];
    int32_t left = -1;
    os64_url_t u = url_of("http://o.test/x/y/z");
    size_t n = way_jar_cookies(jar, &u, false, 5, out, sizeof(out), &left);
    // Not all fit: the newest that do (d, c, b), whole, in the RFC's order;
    // the oldest, a, is the one left out.
    expect("room: whole cookies only, the newest, and a count of the rest",
           n == 13 && strcmp(out, "d=4; b=2; c=3") == 0 && left == 1, out);

    char lines[256];
    int32_t off = way_hop_headers(jar, "http://o.test/page", &u, false, 5, lines, sizeof(lines));
    expect("hop: a Cookie line, then a Referer", off == 0 && strcmp(lines,
           "Cookie: d=4; b=2; a=1; c=3\r\nReferer: http://o.test/page\r\n") == 0, lines);
    off = way_hop_headers(jar, "http://o.test/page", &u, false, 5, lines, 40);
    expect("hop: the cookies first, the Referer left off when it will not fit",
           off == 0 && strcmp(lines, "Cookie: d=4; b=2; a=1; c=3\r\n") == 0, lines);
    off = way_hop_headers(jar, NULL, &u, false, 5, lines, 20);
    expect("hop: cookies that will not fit are left off whole, the oldest first",
           off == 2 && strcmp(lines, "Cookie: d=4; c=3\r\n") == 0, lines);
    u = url_of("http://nothing.test/");
    off = way_hop_headers(jar, "", &u, false, 5, lines, sizeof(lines));
    expect("hop: nothing to send is nothing", off == 0 && lines[0] == '\0', lines);
    way_jar_free(jar);

    jar = way_jar_new();
    char line[32];
    for (int i = 0; i < WAY_COOKIES_PER_DOMAIN + 1; i++) {
        snprintf(line, sizeof(line), "c%d=1", i);
        hear(jar, "http://many.test/", false, line, i);
    }
    const char *all = sent(jar, "http://many.test/", false, 1000);
    expect("limits: past 180 in a domain the oldest goes",
           way_jar_count(jar) == WAY_COOKIES_PER_DOMAIN && strncmp(all, "c1=1; ", 6) == 0, all);
    way_jar_free(jar);

    // The cookie in use is the last to go (§5.3 step 12, §5.4 step 3): the
    // oldest is sent once, alone at its own path, so when the 181st
    // arrives the second-oldest goes instead.
    jar = way_jar_new();
    hear(jar, "http://used.test/", false, "login=1; Path=/in", 0);
    for (int i = 1; i < WAY_COOKIES_PER_DOMAIN; i++) {
        snprintf(line, sizeof(line), "c%d=1; Path=/out", i);
        hear(jar, "http://used.test/", false, line, i);
    }
    jar_expect("limits: the oldest cookie, sent", jar, "http://used.test/in", false, 500,
               "login=1");
    hear(jar, "http://used.test/", false, "late=1; Path=/out", 600);
    const char *rest = sent(jar, "http://used.test/out", false, 700);
    expect("limits: past 180 the cookie used longest ago goes, not the one in use",
           way_jar_count(jar) == WAY_COOKIES_PER_DOMAIN && strncmp(rest, "c2=1; ", 6) == 0 &&
               strcmp(sent(jar, "http://used.test/in", false, 700), "login=1") == 0,
           rest);
    way_jar_free(jar);

    // The expired go before any limit is counted: 179 live and then one
    // that expires — the newest, so no other rule would pick it — then a
    // new one, and every live cookie stays.
    jar = way_jar_new();
    for (int i = 1; i < WAY_COOKIES_PER_DOMAIN; i++) {
        snprintf(line, sizeof(line), "c%d=1", i);
        hear(jar, "http://aged.test/", false, line, i);
    }
    hear(jar, "http://aged.test/", false, "gone=1; Max-Age=5", 300);
    hear(jar, "http://aged.test/", false, "late=1", 600);
    const char *kept = sent(jar, "http://aged.test/", false, 700);
    expect("limits: an expired cookie goes before a live one",
           way_jar_count(jar) == WAY_COOKIES_PER_DOMAIN && strncmp(kept, "c1=1; ", 6) == 0 &&
               strstr(kept, "gone=") == NULL,
           kept);
    way_jar_free(jar);

    jar = way_jar_new();
    for (int i = 0; i < WAY_COOKIES_MAX + 1; i++) {
        snprintf(line, sizeof(line), "http://d%d.test/", i / 100);
        char name[16];
        snprintf(name, sizeof(name), "n%d=1", i);
        hear(jar, line, false, name, i);
    }
    expect("limits: past 3000 in all the oldest goes",
           way_jar_count(jar) == WAY_COOKIES_MAX &&
               strstr(sent(jar, "http://d0.test/", false, 5000), "n0=1") == NULL &&
               strstr(sent(jar, "http://d0.test/", false, 5000), "n1=1") != NULL,
           NULL);
    way_jar_free(jar);

    // Every allocation failed in turn: nothing leaks and nothing crashes,
    // and the jar says only what it kept.
    for (size_t k = 1; k < 8; k++) {
        size_t before = live;
        allocations = 0;
        fail_at = k;
        way_jar_t *j = way_jar_new();
        if (j != NULL) {
            hear(j, "http://f.test/", false, "a=1", 1);
            hear(j, "http://f.test/", false, "b=2", 2);
            (void)sent(j, "http://f.test/", false, 3);
        }
        fail_at = 0;
        way_jar_free(j);
        expect("sweep: a failed allocation leaks nothing", live == before, NULL);
    }
}

// THE BUDGET a hop's Cookie line gets (WAY_COOKIE_LINE_MAX), through
// way_hop_headers at the room a page's own hop has (libfetch's per-hop
// room less who is asking), and which cookies go when they do not all fit.
static void cookie_budget(void)
{
    const size_t hop_room = OS64_FETCH_HOP_FIELDS_MAX - WAY_IDENTITY_MAX;
    static char lines[OS64_FETCH_HOP_FIELDS_MAX];
    char line[400];
    os64_url_t u = url_of("https://login.test/home");

    // A SITE'S 3 KB AND A LOGIN AFTER THEM: twelve cookies of about 250
    // bytes from the visits before, then the session the sign-in made,
    // newest of all. Every one goes, the session among them.
    way_jar_t *jar = way_jar_new();
    char value[241];
    memset(value, 'v', sizeof(value) - 1);
    value[sizeof(value) - 1] = '\0';
    for (int i = 0; i < 12; i++) {
        snprintf(line, sizeof(line), "pre%02d=%s; Path=/; Max-Age=100000", i, value);
        hear(jar, "https://login.test/", true, line, 10 + i);
    }
    hear(jar, "https://login.test/session", true,
         "user_session=SESSION; Path=/; Secure; HttpOnly; Max-Age=100000", 500);
    int32_t off = way_hop_headers(jar, "https://login.test/away", &u, true, 600, lines, hop_room);
    expect("budget: 3 KB of a site's cookies go whole, the login with them",
           off == 0 && strstr(lines, "user_session=SESSION") != NULL && strstr(lines, "pre00=") != NULL &&
               strstr(lines, "pre11=") != NULL && strstr(lines, "\r\nReferer: https://login.test/away\r\n") != NULL,
           NULL);
    way_jar_free(jar);

    // PAST THE BUDGET: forty of them (10 KB) and the login. The line stays
    // within WAY_COOKIE_LINE_MAX, the login goes, the oldest are the ones
    // left out, and the Referer still has its room after the cookies.
    jar = way_jar_new();
    for (int i = 0; i < 40; i++) {
        snprintf(line, sizeof(line), "pre%02d=%s; Path=/; Max-Age=100000", i, value);
        hear(jar, "https://login.test/", true, line, 10 + i);
    }
    hear(jar, "https://login.test/session", true,
         "user_session=SESSION; Path=/; Secure; HttpOnly; Max-Age=100000", 500);
    off = way_hop_headers(jar, "https://login.test/away", &u, true, 600, lines, hop_room);
    const char *end = strstr(lines, "\r\n");
    size_t cookie_line = end != NULL ? (size_t)(end - lines) + 2 : 0;
    expect("budget: past it, the newest go and the oldest are counted out",
           off > 0 && off < 40 && strstr(lines, "user_session=SESSION") != NULL &&
               strstr(lines, "pre39=") != NULL && strstr(lines, "pre00=") == NULL &&
               cookie_line + 1 <= WAY_COOKIE_LINE_MAX && cookie_line + 1 > WAY_COOKIE_LINE_MAX - 260 &&
               strstr(lines, "\r\nReferer: https://login.test/away\r\n") != NULL,
           lines + (cookie_line > 40 ? cookie_line - 40 : 0));
    // ...and the order on the line is still the RFC's: the login, newest
    // of all and at the same path as the rest, is written last.
    const char *session = strstr(lines, "user_session=");
    expect("budget: the chosen go in the RFC's order",
           session != NULL && strstr(session, "pre") == NULL && strncmp(lines, "Cookie: pre", 11) == 0, NULL);

    // A LOGIN, THEN A SCRIPT'S PILE: the session set HttpOnly at sign-in,
    // larger than any of what follows (so it is not merely the small one
    // that fits after the rest), and after it forty cookies a page set,
    // none HttpOnly. The session is kept first however much newer they
    // are; of them, the newest that fit.
    way_jar_free(jar);
    jar = way_jar_new();
    char login_cookie[400];
    snprintf(login_cookie, sizeof(login_cookie), "user_session=SESSION%0300d; Path=/; Secure; HttpOnly; Max-Age=100000",
             0);
    hear(jar, "https://login.test/session", true, login_cookie, 5);
    for (int i = 0; i < 40; i++) {
        snprintf(line, sizeof(line), "pile%02d=%s; Path=/; Max-Age=100000", i, value);
        hear(jar, "https://login.test/", true, line, 10 + i);
    }
    off = way_hop_headers(jar, NULL, &u, true, 600, lines, hop_room);
    expect("budget: an HttpOnly session outlasts a newer pile of a script's cookies",
           off > 0 && strstr(lines, "user_session=SESSION") != NULL && strstr(lines, "pile39=") != NULL &&
               strstr(lines, "pile00=") == NULL,
           NULL);

    // A smaller older cookie still fits where a large newer one did not.
    way_jar_free(jar);
    jar = way_jar_new();
    hear(jar, "http://fit.test/", false, "a=1", 1);
    hear(jar, "http://fit.test/", false, "big=0123456789", 2);
    hear(jar, "http://fit.test/", false, "c=3", 3);
    char out[12];
    int32_t left = -1;
    os64_url_t f = url_of("http://fit.test/");
    way_jar_cookies(jar, &f, false, 4, out, sizeof(out), &left);
    expect("budget: a cookie that does not fit is passed over, not the end of the choosing",
           strcmp(out, "a=1; c=3") == 0 && left == 1, out);
    way_jar_free(jar);

    // What a load notes of its hops: the most one left out, and its host.
    way_cookies_short_t s;
    memset(&s, 0, sizeof(s));
    os64_url_t a = url_of("https://a.test/"), b = url_of("https://b.test/");
    way_cookies_short_note(&s, &a, 0);
    expect("short: a hop that left nothing out is not noted", s.left == 0 && s.host[0] == '\0', NULL);
    way_cookies_short_note(&s, &a, 3);
    way_cookies_short_note(&s, &b, 2);
    expect("short: the most, and the host it went to", s.left == 3 && strcmp(s.host, "a.test") == 0, s.host);
    way_cookies_short_note(&s, &b, 5);
    expect("short: a worse one replaces it", s.left == 5 && strcmp(s.host, "b.test") == 0, s.host);
}

static void referrers(void)
{
    char out[512];
    os64_url_t to = url_of("https://a.test/x");
    expect("referer: same origin, the whole address, no fragment",
           way_referrer("https://a.test/p?q=1#frag", &to, out, sizeof(out)) &&
               strcmp(out, "https://a.test/p?q=1") == 0,
           out);
    to = url_of("https://b.test/");
    expect("referer: another origin, just the origin",
           way_referrer("https://a.test/p?q=1", &to, out, sizeof(out)) &&
               strcmp(out, "https://a.test/") == 0,
           out);
    to = url_of("http://a.test/");
    expect("referer: https to http, nothing", !way_referrer("https://a.test/p", &to, out, sizeof(out)),
           NULL);
    to = url_of("https://a.test/");
    expect("referer: http to https, the origin",
           way_referrer("http://a.test/p", &to, out, sizeof(out)) && strcmp(out, "http://a.test/") == 0,
           out);
    to = url_of("http://a.test:8080/z");
    expect("referer: a port is part of the origin",
           way_referrer("http://a.test:8080/p", &to, out, sizeof(out)) &&
               strcmp(out, "http://a.test:8080/p") == 0,
           out);
    to = url_of("http://a.test/z");
    expect("referer: another port is another origin",
           way_referrer("http://a.test:8080/p", &to, out, sizeof(out)) &&
               strcmp(out, "http://a.test:8080/") == 0,
           out);
    to = url_of("https://a.test:443/z");
    expect("referer: the default port spelled is the same origin",
           way_referrer("https://a.test/p", &to, out, sizeof(out)) &&
               strcmp(out, "https://a.test/p") == 0,
           out);
    expect("referer: a page from disk names nothing",
           !way_referrer("file:///home/page.html", &to, out, sizeof(out)), NULL);
    expect("referer: no page names nothing", !way_referrer(NULL, &to, out, sizeof(out)), NULL);
}

// A page's script (document.cookie): it reads what a request would carry
// less the HttpOnly cookies, may not set an HttpOnly one, and may not
// replace one (RFC 6265 § 5.3 step 11.2, § 5.4 step 1).
static const char *script_read(way_jar_t *jar, const char *page, bool encrypted, int64_t now)
{
    static char out[4096];
    os64_url_t u = url_of(page);
    way_jar_script_cookies(jar, &u, encrypted, now, out, sizeof(out), NULL);
    return out;
}

static void script_hear(way_jar_t *jar, const char *page, bool encrypted, const char *line, int64_t now)
{
    os64_url_t u = url_of(page);
    way_jar_script_hear(jar, &u, encrypted, line, strlen(line), now);
}

static void cookie_scripts(void)
{
    way_jar_t *jar = way_jar_new();
    hear(jar, "http://s.test/", false, "session=secret; HttpOnly", 100);
    hear(jar, "http://s.test/", false, "theme=dark", 100);
    expect("script: an HttpOnly cookie is not read", strcmp(script_read(jar, "http://s.test/", false, 101), "theme=dark") == 0,
           script_read(jar, "http://s.test/", false, 101));
    jar_expect("script: a request still carries it", jar, "http://s.test/", false, 101, "session=secret; theme=dark");
    script_hear(jar, "http://s.test/", false, "late=1; HttpOnly", 102);
    script_hear(jar, "http://s.test/", false, "session=forged", 102);
    script_hear(jar, "http://s.test/", false, "lang=en", 102);
    jar_expect("script: may not set an HttpOnly cookie nor replace one", jar, "http://s.test/", false, 103,
               "session=secret; theme=dark; lang=en");
    script_hear(jar, "http://s.test/", false, "session=forged; Domain=s.test", 103);
    hear(jar, "http://s.test/", false, "sid=secret; Domain=s.test; HttpOnly", 103);
    script_hear(jar, "http://s.test/", false, "sid=forged", 103);
    jar_expect("script: nor set one beside it that differs only in being host-only", jar,
               "http://s.test/", false, 103, "session=secret; theme=dark; lang=en; sid=secret");
    {
        os64_url_t u = url_of("http://s.test/");
        int32_t left = 0;
        expect("script: a read with no room says what did not fit",
               way_jar_script_cookies(jar, &u, false, 103, NULL, 0, &left) == 0 && left == 2, NULL);
    }
    script_hear(jar, "http://s.test/", false, "theme=light", 104);
    expect("script: replaces a cookie it may see", strcmp(script_read(jar, "http://s.test/", false, 105), "theme=light; lang=en") == 0,
           script_read(jar, "http://s.test/", false, 105));
    way_jar_free(jar);
}

// ── Kept across restarts (cookies.txt) ──────────────────────────────────

// The jar's file text, in a buffer that stays valid until the next call.
static const char *file_of(way_jar_t *jar, int64_t now)
{
    static char text[1 << 16];
    size_t len = 0;
    char *t = way_jar_text(jar, now, &len);
    snprintf(text, sizeof(text), "%s", t != NULL ? t : "(no memory)");
    expect("file: the length is the text's", t == NULL || len == strlen(t), NULL);
    os64_free(t);
    return text;
}

static way_jar_read_t read_into(way_jar_t *jar, const char *text, int64_t now)
{
    way_jar_read_t r;
    way_jar_read(jar, text, strlen(text), now, &r);
    return r;
}

static void cookie_file_round_trip(void)
{
    way_jar_t *jar = way_jar_new();
    hear(jar, "https://h.test/a/page", true, "host=1; Max-Age=1000", 100);
    hear(jar, "https://www.d.test/", true, "dom=2; Domain=d.test; Path=/; Max-Age=1000", 101);
    hear(jar, "https://h.test/", true, "sid=3; Path=/; Secure; HttpOnly; Max-Age=1000", 102);
    hear(jar, "https://h.test/", true, "__Host-tok=4; Path=/; Secure; Max-Age=1000", 103);
    hear(jar, "https://h.test/", true, "__Secure-s=5; Path=/x; Secure; Max-Age=1000", 104);
    hear(jar, "https://h.test/", true, "session=6; Path=/", 105);
    hear(jar, "https://h.test/", true, "brief=7; Path=/; Max-Age=50", 106);
    hear(jar, "https://h.test/", true, "empty=; Path=/; Max-Age=1000", 107);
    const char *text = file_of(jar, 200);
    expect("file: Netscape's first line, then what the file is", strncmp(text, "# Netscape HTTP Cookie File\n", 28) == 0 &&
           strstr(text, "sign in as you") != NULL, text);
    expect("file: a host-only cookie, its directory's path",
           strstr(text, "\nh.test\tFALSE\t/a\tFALSE\t1100\thost\t1\n") != NULL, text);
    expect("file: a domain cookie, spelled with its dot",
           strstr(text, "\n.d.test\tTRUE\t/\tFALSE\t1101\tdom\t2\n") != NULL, text);
    expect("file: HttpOnly as curl writes it",
           strstr(text, "\n#HttpOnly_h.test\tFALSE\t/\tTRUE\t1102\tsid\t3\n") != NULL, text);
    expect("file: the prefixed cookies", strstr(text, "\th.test\tFALSE\t/\tTRUE\t1103\t__Host-tok\t4\n") == NULL &&
           strstr(text, "\nh.test\tFALSE\t/\tTRUE\t1103\t__Host-tok\t4\n") != NULL &&
           strstr(text, "\nh.test\tFALSE\t/x\tTRUE\t1104\t__Secure-s\t5\n") != NULL, text);
    expect("file: an empty value is an empty field",
           strstr(text, "\nh.test\tFALSE\t/\tFALSE\t1107\tempty\t\n") != NULL, text);
    expect("file: a session cookie is never written, an expired one neither",
           strstr(text, "session") == NULL && strstr(text, "brief") == NULL, text);
    expect("file: oldest first", strstr(text, "\thost\t") < strstr(text, "\tdom\t") &&
           strstr(text, "\tdom\t") < strstr(text, "\tsid\t") && strstr(text, "\tsid\t") < strstr(text, "\tempty\t"), text);

    way_jar_t *back = way_jar_new();
    way_jar_read_t r = read_into(back, text, 300);
    expect("file: read back whole", r.read == 6 && r.refused == 0 && !r.short_of_memory && way_jar_count(back) == 6, NULL);
    jar_expect("file: what the host is sent is what it was", back, "https://h.test/a/b", true, 300,
               sent(jar, "https://h.test/a/b", true, 300));
    jar_expect("file: the domain cookie reaches a name under it", back, "https://x.d.test/", true, 300, "dom=2");
    jar_expect("file: a secure cookie stays secure", back, "http://h.test/x/y", false, 300, "empty=");
    expect("file: an HttpOnly cookie stays out of a script's reach",
           strstr(script_read(back, "https://h.test/", true, 300), "sid=") == NULL, NULL);
    expect("file: what was read is no change a file has still to hear", way_jar_changes(back) == 0, NULL);
    expect("file: and writes back the same", strcmp(file_of(back, 300), file_of(jar, 300)) == 0 ||
           strcmp(strchr(file_of(back, 300), '\n'), strchr(file_of(jar, 300), '\n')) == 0, NULL);
    expect("file: an expired line is not read", read_into(back, text, 1150).read == 0, NULL);
    way_jar_free(back);
    way_jar_free(jar);

    // Oldest first by AGE, not by where the jar keeps them: a cookie set
    // again keeps its age (§5.3 step 11) and moves in the jar.
    jar = way_jar_new();
    hear(jar, "http://age.test/", false, "a=1; Max-Age=1000", 10);
    hear(jar, "http://age.test/", false, "b=2; Max-Age=1000", 11);
    hear(jar, "http://age.test/", false, "c=3; Max-Age=1000", 12);
    hear(jar, "http://age.test/", false, "a=9; Max-Age=1000", 13);
    text = file_of(jar, 20);
    expect("file: oldest first, a cookie set again keeping its place", strstr(text, "\ta\t9\n") != NULL &&
           strstr(text, "\ta\t9\n") < strstr(text, "\tb\t2\n") && strstr(text, "\tb\t2\n") < strstr(text, "\tc\t3\n"),
           text);
    way_jar_free(jar);

    // A Max-Age past any calendar is written as the most a line holds, and
    // read back.
    jar = way_jar_new();
    hear(jar, "https://h.test/", true, "forever=1; Path=/; Max-Age=99999999999999999999999", 100);
    text = file_of(jar, 200);
    back = way_jar_new();
    expect("file: a cookie past any calendar is written as a line can hold it, and read back",
           strstr(text, "\t999999999999999999\tforever\t1\n") != NULL && read_into(back, text, 300).read == 1, text);
    way_jar_free(back);
    way_jar_free(jar);
}

static void cookie_file_hostile(void)
{
    static char text[WAY_JAR_LINE_MAX * 2 + 4096];
    size_t n = (size_t)snprintf(text, sizeof(text),
        "# a comment\n"
        "\n"
        "ok.test\tFALSE\t/\tFALSE\t5000\tgood\t1\r\n"           // CR LF is a line end
        "ok.test\tFALSE\t/\tFALSE\t5000\tsix\n"                  // six fields
        "ok.test\tFALSE\t/\tFALSE\t5000\ta\tb\tc\n"              // eight
        "ok.test\tMAYBE\t/\tFALSE\t5000\tflag\t1\n"
        "ok.test\tFALSE\t/\tYES\t5000\tflag2\t1\n"
        "ok.test\tFALSE\t/\tFALSE\tsoon\tnum\t1\n"
        "ok.test\tFALSE\t/\tFALSE\t-5\tneg\t1\n"
        "ok.test\tFALSE\t/\tFALSE\t1234567890123456789\thuge\t1\n"
        "ok.test\tFALSE\trel\tFALSE\t5000\tpath\t1\n"
        ".com\tTRUE\t/\tFALSE\t5000\tsuffix\t1\n"                // a single label's every name
        "\tFALSE\t/\tFALSE\t5000\tnohost\t1\n"
        "ok..test\tFALSE\t/\tFALSE\t5000\tdots\t1\n"
        ".ok.test\tTRUE\t/\tTRUE\t5000\t__Host-x\t1\n"            // a __Host- cookie with a Domain
        "ok.test\tFALSE\t/sub\tTRUE\t5000\t__Host-y\t1\n"         // ...or a path other than /
        "ok.test\tFALSE\t/\tFALSE\t5000\t__Secure-z\t1\n"        // a __Secure- one not Secure
        "ok.test\tFALSE\t/\tFALSE\t5000\tctl\ta\x01z\n"
        "ok.test\tFALSE\t/\tFALSE\t5000\ta=b\tc\n"                // a name no Set-Cookie makes
        "ok.test\tFALSE\t/\tFALSE\t5000\tsemi\tx;y\n"              // ...nor a value
        "ok.test\tFALSE\t/\tFALSE\t5000\t\t\n"                   // no name, no value
        "ok.test\tFALSE\t/\tFALSE\t0\tsession\t1\n"              // curl's session line: not refused, not read
        "ok.test\tFALSE\t/\tFALSE\t10\told\t1\n"                 // expired: the same
        "#HttpOnly_ok.test\tFALSE\t/\tTRUE\t5000\thidden\t1\n"
        ".sub.ok.test\tTRUE\t/\tFALSE\t5000\twide\t1\n");
    // A line past WAY_JAR_LINE_MAX, and a value past WAY_COOKIE_BYTES_MAX.
    n += (size_t)snprintf(text + n, sizeof(text) - n, "ok.test\tFALSE\t/\tFALSE\t5000\tlong\t");
    memset(text + n, 'v', WAY_JAR_LINE_MAX);
    n += WAY_JAR_LINE_MAX;
    n += (size_t)snprintf(text + n, sizeof(text) - n, "\nok.test\tFALSE\t/\tFALSE\t5000\tbig\t");
    memset(text + n, 'v', WAY_COOKIE_BYTES_MAX);
    n += WAY_COOKIE_BYTES_MAX;
    n += (size_t)snprintf(text + n, sizeof(text) - n, "\nok.test\tFALSE\t/\tFALSE\t5000\tlast\t1");   // no newline at the end
    way_jar_t *jar = way_jar_new();
    way_jar_read_t r;
    way_jar_read(jar, text, n, 100, &r);
    expect("hostile: every malformed or refused line is skipped and counted, the rest read",
           r.read == 4 && r.refused == 20 && !r.short_of_memory, NULL);
    jar_expect("hostile: what was read", jar, "https://ok.test/", true, 100, "good=1; hidden=1; last=1");
    jar_expect("hostile: a domain cookie with two labels", jar, "https://a.sub.ok.test/", true, 100, "wide=1");
    // A NUL inside a line is no line at all; the reader is handed bytes.
    static const char nul[] = "ok.test\tFALSE\t/\tFALSE\t5000\tz\t1\0tail\nok.test\tFALSE\t/\tFALSE\t5000\tafter\t1\n";
    way_jar_read(jar, nul, sizeof(nul) - 1, 100, &r);
    expect("hostile: a NUL in a line refuses it and reading goes on", r.read == 1 && r.refused == 1, NULL);
    // Every lone fragment of a line, and nothing at all, is safe.
    const char *good = "ok.test\tFALSE\t/\tTRUE\t5000\tsecure\tv\n";
    for (size_t k = 0; k <= strlen(good); k++) {
        char *piece = malloc(k > 0 ? k : 1);
        memcpy(piece, good, k);
        way_jar_read(jar, piece, k, 100, &r);
        free(piece);
    }
    way_jar_read(jar, NULL, 0, 100, &r);
    expect("hostile: fragments and nothing are read safely", r.read == 0 && r.refused == 0, NULL);
    way_jar_free(jar);

    // The caps: 200 lines for one domain keep 180; past 3000 in all, 3000.
    static char many[(WAY_COOKIES_MAX + 10) * 48];
    size_t m = 0;
    for (int i = 0; i < 200; i++)
        m += (size_t)snprintf(many + m, sizeof(many) - m, "one.test\tFALSE\t/\tFALSE\t5000\tc%d\t1\n", i);
    jar = way_jar_new();
    way_jar_read(jar, many, m, 100, &r);
    expect("caps: a domain keeps WAY_COOKIES_PER_DOMAIN", r.read == 200 && way_jar_count(jar) == WAY_COOKIES_PER_DOMAIN,
           NULL);
    way_jar_free(jar);
    // A domain full of cookies in use, and a file naming one more: the one
    // read is the least used and is the one let go (under ASan: its
    // domain's string is not read after it goes).
    jar = way_jar_new();
    for (int i = 0; i < WAY_COOKIES_PER_DOMAIN; i++) {
        char line[32];
        snprintf(line, sizeof(line), "u%d=1; Max-Age=5000", i);
        hear(jar, "http://one.test/", false, line, 100);
    }
    (void)sent(jar, "http://one.test/", false, 100);
    static const char kLate[] = "one.test\tFALSE\t/\tFALSE\t5000\tlate\t1\n";
    way_jar_read(jar, kLate, sizeof(kLate) - 1, 100, &r);
    expect("caps: a cookie read into a full domain of cookies in use is the one let go",
           r.read == 1 && way_jar_count(jar) == WAY_COOKIES_PER_DOMAIN &&
               strstr(sent(jar, "http://one.test/", false, 100), "late=") == NULL,
           NULL);
    way_jar_free(jar);
    m = 0;
    for (int i = 0; i < WAY_COOKIES_MAX + 10; i++)
        m += (size_t)snprintf(many + m, sizeof(many) - m, "d%d.test\tFALSE\t/\tFALSE\t5000\tc\t1\n", i);
    jar = way_jar_new();
    way_jar_read(jar, many, m, 100, &r);
    expect("caps: the jar keeps WAY_COOKIES_MAX", way_jar_count(jar) == WAY_COOKIES_MAX, NULL);
    way_jar_free(jar);
}

// One window's save, as a face does it: the file read into a jar of its
// own, this jar's changes over it, the text written, the changes marked
// heard, and the file followed so this window learns what the other set
// and let go.
static void save_window(way_jar_t *jar, char *file, size_t cap, int64_t now)
{
    uint64_t through = way_jar_changes(jar);
    way_jar_t *merged = way_jar_new();
    way_jar_read(merged, file, strlen(file), now, NULL);
    way_jar_overlay(merged, jar, through, now);
    size_t len;
    char *text = way_jar_text(merged, now, &len);
    snprintf(file, cap, "%s", text);
    os64_free(text);
    way_jar_free(merged);
    way_jar_saved(jar, through);
    way_jar_follow(jar, file, strlen(file), now, NULL);
}

static void cookie_file_two_windows(void)
{
    static char file[1 << 14];
    snprintf(file, sizeof(file), "w.test\tFALSE\t/\tFALSE\t9000\tx\t0\nw.test\tFALSE\t/\tFALSE\t9000\tz\t0\n");
    way_jar_t *a = way_jar_new(), *b = way_jar_new();
    way_jar_read(a, file, strlen(file), 100, NULL);
    way_jar_read(b, file, strlen(file), 100, NULL);
    hear(a, "http://w.test/", false, "x=A; Max-Age=5000", 110);
    expect("windows: a kept cookie set is a change", way_jar_changes(a) != 0, NULL);
    hear(b, "http://w.test/", false, "y=B; Max-Age=5000", 111);
    hear(b, "http://w.test/", false, "z=gone; Max-Age=0", 112);
    hear(b, "http://w.test/", false, "s=session", 113);
    save_window(a, file, sizeof(file), 120);
    save_window(b, file, sizeof(file), 121);
    expect("windows: each one's change kept, the other's not undone, the one let go gone",
           strstr(file, "\tx\tA\n") != NULL && strstr(file, "\ty\tB\n") != NULL && strstr(file, "\tz\t") == NULL &&
               strstr(file, "\ts\t") == NULL, file);
    jar_expect("windows: the second learned the first's at its save", b, "http://w.test/", false, 122, "x=A; y=B; s=session");
    way_jar_t *probe = way_jar_new();
    way_jar_overlay(probe, b, way_jar_changes(b), 122);
    expect("windows: a save leaves nothing to hear", way_jar_count(probe) == 0, NULL);
    way_jar_free(probe);
    // The first saves again for a change of its own: what the second wrote
    // stands, and the first, reading the file back, learns it.
    hear(a, "http://w.test/", false, "w=1; Max-Age=5000", 130);
    save_window(a, file, sizeof(file), 131);
    expect("windows: a later save of the first keeps the second's", strstr(file, "\ty\tB\n") != NULL &&
           strstr(file, "\tw\t1\n") != NULL && strstr(file, "\tx\tA\n") != NULL, file);
    // (y is new to the first when it follows the file at 131, so it is the
    // youngest there; z, which the second let go, the first lets go too.)
    jar_expect("windows: and the first sends what the second set, not what it let go", a, "http://w.test/", false,
               132, "x=A; w=1; y=B");
    // The same cookie changed in both: the later save is the newer change.
    hear(a, "http://w.test/", false, "x=A2; Max-Age=5000", 140);
    hear(b, "http://w.test/", false, "x=B2; Max-Age=5000", 141);
    save_window(a, file, sizeof(file), 142);
    save_window(b, file, sizeof(file), 143);
    expect("windows: one cookie, the later change", strstr(file, "\tx\tB2\n") != NULL && strstr(file, "\tx\tA2\n") == NULL,
           file);
    // A change not yet saved outranks the file: reading it in leaves the
    // window's own.
    hear(a, "http://w.test/", false, "x=mine; Max-Age=5000", 150);
    way_jar_read(a, file, strlen(file), 151, NULL);
    jar_expect("windows: a change of this window's stands against the file", a, "http://w.test/", false, 151,
               "x=mine; w=1; y=B");
    way_jar_follow(a, file, strlen(file), 151, NULL);
    jar_expect("windows: following the file too", a, "http://w.test/", false, 151, "x=mine; w=1; y=B");
    // A session cookie in a kept one's place takes it out of the file.
    hear(a, "http://w.test/", false, "w=now-a-session", 160);
    save_window(a, file, sizeof(file), 161);
    expect("windows: a kept cookie made a session cookie leaves the file", strstr(file, "\tw\t") == NULL &&
           strstr(file, "\tx\tmine\n") != NULL, file);
    // A page's script setting a kept cookie is a change like any.
    uint64_t before = way_jar_changes(a);
    script_hear(a, "http://w.test/", false, "js=1; Max-Age=5000", 170);
    expect("windows: a script's kept cookie is a change", way_jar_changes(a) > before, NULL);
    hear(a, "http://w.test/", false, "plain=session", 171);
    expect("windows: a session cookie beside nothing kept is not", way_jar_changes(a) == before + 1, NULL);
    // Clear cookies in the second empties the file; the first, following
    // it, lets go every kept cookie but the one it has not saved yet, and
    // keeps its session cookies.
    way_jar_clear(b);
    size_t len;
    char *empty = way_jar_text(b, 180, &len);
    snprintf(file, sizeof(file), "%s", empty);
    os64_free(empty);
    way_jar_follow(a, file, strlen(file), 181, NULL);
    jar_expect("windows: a file cleared elsewhere is followed", a, "http://w.test/", false, 182,
               "w=now-a-session; js=1; plain=session");
    way_jar_free(a);
    way_jar_free(b);
}

static void cookie_switch(void)
{
    way_jar_t *jar = way_jar_new();
    hear(jar, "http://o.test/", false, "a=1; Max-Age=100", 10);
    way_jar_refuse(jar, true);
    hear(jar, "http://o.test/", false, "b=2; Max-Age=100", 11);
    jar_expect("off: nothing sent", jar, "http://o.test/", false, 12, "");
    way_jar_read(jar, "o.test\tFALSE\t/\tFALSE\t500\tc\t3\n", 29, 12, NULL);
    way_jar_refuse(jar, false);
    jar_expect("off: nothing heard nor read while it was", jar, "http://o.test/", false, 13, "a=1");
    hear(jar, "http://o.test/", false, "b=2; Max-Age=100", 14);
    hear(jar, "http://o.test/", false, "a=gone; Max-Age=0", 15);
    way_jar_clear(jar);
    expect("clear: every cookie gone", way_jar_count(jar) == 0 && strstr(file_of(jar, 16), "\t") == NULL, NULL);
    way_jar_t *merged = way_jar_new();
    way_jar_read(merged, "o.test\tFALSE\t/\tFALSE\t500\tb\t2\n", 29, 16, NULL);
    way_jar_overlay(merged, jar, way_jar_changes(jar), 16);
    expect("clear: and no change left to lay over a file", way_jar_count(merged) == 1, NULL);
    way_jar_free(merged);
    way_jar_free(jar);

    // A change made after the number a save took stays owed after it.
    jar = way_jar_new();
    hear(jar, "http://o.test/", false, "a=1; Max-Age=100", 20);
    uint64_t through = way_jar_changes(jar);
    hear(jar, "http://o.test/", false, "b=2; Max-Age=100", 21);
    way_jar_saved(jar, through);
    merged = way_jar_new();
    way_jar_overlay(merged, jar, way_jar_changes(jar), 22);
    expect("saved: a change after the save's number stays owed", way_jar_count(merged) == 1 &&
           strcmp(sent(merged, "http://o.test/", false, 22), "b=2") == 0, NULL);
    way_jar_free(merged);
    way_jar_free(jar);

    // A follow that ran short of memory loses nothing: the cookie it could
    // not replace stays as it was, and the one it did not reach stays.
    static const char kTwo[] = "f.test\tFALSE\t/\tFALSE\t500\ta\t1\nf.test\tFALSE\t/\tFALSE\t500\tb\t2\n";
    jar = way_jar_new();
    way_jar_follow(jar, kTwo, sizeof(kTwo) - 1, 10, NULL);
    allocations = 0;
    fail_at = 2;                        // the line's buffer, then the first cookie's block
    way_jar_read_t short_read;
    way_jar_follow(jar, kTwo, sizeof(kTwo) - 1, 10, &short_read);
    fail_at = 0;
    expect("follow: short of memory, nothing is lost", short_read.short_of_memory &&
           strcmp(sent(jar, "http://f.test/", false, 11), "a=1; b=2") == 0, sent(jar, "http://f.test/", false, 11));
    way_jar_free(jar);

    // Every allocation of following a file, changing, laying over and
    // writing failed in turn, and from there on: nothing leaks or crashes,
    // and a follow short of memory lets nothing go.
    static const char kFile[] = "f.test\tFALSE\t/\tFALSE\t500\ta\t1\nf.test\tFALSE\t/\tFALSE\t500\tb\t2\n";
    for (size_t k = 1; k < 32; k++) {
        size_t before = live;
        allocations = 0;
        fail_at = k;
        way_jar_t *j = way_jar_new(), *m = way_jar_new();
        if (j != NULL && m != NULL) {
            way_jar_follow(j, kFile, sizeof(kFile) - 1, 10, NULL);
            hear(j, "http://f.test/", false, "c=3; Max-Age=100", 11);
            hear(j, "http://f.test/", false, "a=gone; Max-Age=0", 12);
            way_jar_follow(j, "", 0, 12, NULL);
            way_jar_overlay(m, j, way_jar_changes(j), 13);
            size_t len;
            char *t = way_jar_text(m, 13, &len);
            os64_free(t);
        }
        fail_at = 0;
        way_jar_free(j);
        way_jar_free(m);
        expect("sweep: a failed allocation in the file's work leaks nothing", live == before, NULL);
    }
}

static void jar_cases(void)
{
    cookie_file_round_trip();
    cookie_file_hostile();
    cookie_file_two_windows();
    cookie_switch();
    cookie_scripts();
    cookie_dates();
    cookie_scope();
    cookie_security();
    cookie_lifetimes();
    cookie_text();
    cookie_order_and_room();
    cookie_budget();
    referrers();
}
