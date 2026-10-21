// The Modern Web arc's slice 4, CSS batch A (MODERN_WEB_BRIEFS.md § Slice
// 4): each property computed from an author sheet and, where it moves
// anything, laid out — every dump worked from the specification by hand,
// as test_libflow_cascade.inc's are. The viewport is WIDTH x 600.

static void batch_a_cases(void)
{
    // cursor is inherited, the keyword after the pictures kept; user-select
    // is not: the p inside the div is auto again, and prints nothing.
    cascade_case("cursor and user-select", 800,
        "<!doctype html><style>div { cursor: pointer; user-select: none }"
        " span { cursor: url(hand.cur) 2 3, url(b.png), text }</style>"
        "<div>a<span>b</span><p>c</p></div>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block cursor=pointer user-select=none holds-block\n"
        "      span inline cursor=text\n"
        "      p block margin=16/0/16/0\n");
    // `inherit` takes the parent's, `initial` is auto.
    cascade_case("cursor and user-select: the CSS-wide keywords", 800,
        "<!doctype html><style>body { cursor: wait; user-select: all }"
        " p { user-select: inherit } em { cursor: initial }</style><p>a<em>b</em>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 cursor=wait user-select=all holds-block\n"
        "    p block margin=16/0/16/0 user-select=all\n"
        "      em inline italic cursor=auto\n");

    // With the switch off neither is a property at all.
    s_modern_off = true;
    cascade_case("cursor and user-select: unknown with the Modern Web switch off", 800,
        "<!doctype html><style>div { cursor: pointer; user-select: none }</style><div>a</div>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block\n");
    s_modern_off = false;
}
