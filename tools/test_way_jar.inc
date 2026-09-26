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
    expect("room: whole cookies only, and a count of the rest",
           n == 13 && strcmp(out, "d=4; b=2; a=1") == 0 && left == 1, out);

    char lines[256];
    int32_t off = way_hop_headers(jar, "http://o.test/page", &u, false, 5, lines, sizeof(lines));
    expect("hop: a Cookie line, then a Referer", off == 0 && strcmp(lines,
           "Cookie: d=4; b=2; a=1; c=3\r\nReferer: http://o.test/page\r\n") == 0, lines);
    off = way_hop_headers(jar, "http://o.test/page", &u, false, 5, lines, 40);
    expect("hop: the cookies first, the Referer left off when it will not fit",
           off == 0 && strcmp(lines, "Cookie: d=4; b=2; a=1; c=3\r\n") == 0, lines);
    off = way_hop_headers(jar, NULL, &u, false, 5, lines, 20);
    expect("hop: cookies that will not fit are left off whole",
           off == 2 && strcmp(lines, "Cookie: d=4; b=2\r\n") == 0, lines);
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

static void jar_cases(void)
{
    cookie_dates();
    cookie_scope();
    cookie_security();
    cookie_lifetimes();
    cookie_text();
    cookie_order_and_room();
    referrers();
}
