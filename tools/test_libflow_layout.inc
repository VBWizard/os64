// Pass 3: every box, line and fragment placed, each expected dump worked
// out by hand against the test backend (tools/test_libflow_fonts.c). At
// 16px a face has ascent 12, descent 4, line height 20, so a line of
// 16px text is 20 tall with its baseline 14 down (half the 4px of leading
// above); a space and the narrow letters are 4 wide, `m w M W @` 12, the
// rest 8, and every monospace glyph 8.
//
// A dump line is `<kind> <element> x y w h` in document pixels; a `line`
// adds its baseline's offset from its top; `text` gives the content area
// (baseline minus ascent, ascent plus descent tall) and the face.

static char *layout_dump_of(const char *html, int32_t width)
{
    os64_html_document_t *doc = parse(html);
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    flow_tree_t *tree = flow_layout(doc, page, width, &kEnv);
    char *text = NULL;
    if (tree != NULL) {
        int64_t need = flow_dump(tree, NULL, 0);
        text = malloc((size_t)need + 1);
        flow_dump(tree, text, (size_t)need + 1);
    }
    flow_free(tree);
    os64_page_free(page);
    os64_html_document_free(doc);
    return text;
}

static void layout_case(const char *name, const char *html, int32_t width, const char *expected)
{
    char *got = layout_dump_of(html, width);
    bool ok = got != NULL && strcmp(got, expected) == 0;
    expect(name, ok, NULL);
    if (!ok)
        fprintf(stderr, "---- expected\n%s---- got\n%s----\n", expected, got ? got : "(null)\n");
    free(got);
}

static void layout_cases(void)
{
    // body's 8px and p's 16px collapse (§8.3.1): the paragraph is at 16.
    // html is the root and its margins collapse with nothing.
    layout_case("a paragraph: the body's margin collapses with the p's",
        "<!doctype html><p>Hi", 800,
        "page 800 52\n"
        "block html 0 0 800 52\n"
        "  block body 8 16 784 20\n"
        "    block p 8 16 784 20\n"
        "      line 8 16 784 20 base 14\n"
        "        text \"Hi\" 8 18 12 16 serif 16\n");

    layout_case("sibling margins collapse to the larger, not the sum",
        "<!doctype html><p>a</p><p>b</p>", 200,
        "page 200 88\n"
        "block html 0 0 200 88\n"
        "  block body 8 16 184 56\n"
        "    block p 8 16 184 20\n"
        "      line 8 16 184 20 base 14\n"
        "        text \"a\" 8 18 8 16 serif 16\n"
        "    block p 8 52 184 20\n"
        "      line 8 52 184 20 base 14\n"
        "        text \"b\" 8 54 8 16 serif 16\n");

    layout_case("an empty block's margins collapse through it",
        "<!doctype html><p>a</p><div></div><p>b</p>", 200,
        "page 200 88\n"
        "block html 0 0 200 88\n"
        "  block body 8 16 184 56\n"
        "    block p 8 16 184 20\n"
        "      line 8 16 184 20 base 14\n"
        "        text \"a\" 8 18 8 16 serif 16\n"
        "    block div 8 52 184 0\n"
        "    block p 8 52 184 20\n"
        "      line 8 52 184 20 base 14\n"
        "        text \"b\" 8 54 8 16 serif 16\n");

    // 84 wide: aaaa (32) + space (4) + bbbb (32) = 68 fits; + 4 + cccc
    // (32) = 104 does not. The trailing space goes with the break.
    layout_case("wrapping at the last space that fits",
        "<!doctype html><p>aaaa bbbb cccc", 100,
        "page 100 72\n"
        "block html 0 0 100 72\n"
        "  block body 8 16 84 40\n"
        "    block p 8 16 84 40\n"
        "      line 8 16 84 20 base 14\n"
        "        text \"aaaa bbbb\" 8 18 68 16 serif 16\n"
        "      line 8 36 84 20 base 14\n"
        "        text \"cccc\" 8 38 32 16 serif 16\n");

    // 54 wide: `bb<b>bb</b>cc` is ONE word of 48 across three nodes, so the
    // break is at the space before it, never between the nodes.
    layout_case("a word across element boundaries does not break inside",
        "<!doctype html><p>aaaa bb<b>bb</b>cc", 70,
        "page 70 72\n"
        "block html 0 0 70 72\n"
        "  block body 8 16 54 40\n"
        "    block p 8 16 54 40\n"
        "      line 8 16 54 20 base 14\n"
        "        text \"aaaa\" 8 18 32 16 serif 16\n"
        "      line 8 36 54 20 base 14\n"
        "        span b 24 38 16 16\n"
        "        text \"bb\" 8 38 16 16 serif 16\n"
        "        text \"bb\" 24 38 16 16 serif 16 bold\n"
        "        text \"cc\" 40 38 16 16 serif 16\n");

    // a b c d e g h are 8, f i j are 4: 68, wider than the 34 it has.
    layout_case("a word wider than its line overflows it, and the page",
        "<!doctype html><p>abcdefghij", 50,
        "page 76 52\n"
        "block html 0 0 50 52\n"
        "  block body 8 16 34 20\n"
        "    block p 8 16 34 20\n"
        "      line 8 16 34 20 base 14\n"
        "        text \"abcdefghij\" 8 18 68 16 serif 16\n");

    // `aaaa<b>bbbbbbbb</b>` is one 96px word; the <wbr> before it offers
    // the only opportunity, and breaking there would make an empty line.
    layout_case("an opportunity before any content is not a break",
        "<!doctype html><p><wbr>aaaa<b>bbbbbbbb</b>", 50,
        "page 104 52\n"
        "block html 0 0 50 52\n"
        "  block body 8 16 34 20\n"
        "    block p 8 16 34 20\n"
        "      line 8 16 34 20 base 14\n"
        "        span b 40 18 64 16\n"
        "        text \"aaaa\" 8 18 32 16 serif 16\n"
        "        text \"bbbbbbbb\" 40 18 64 16 serif 16 bold\n");

    // Past the engine's 1 MiB run cap, a text is measured in windows and
    // still laid out whole — prose and one enormous word alike.
    {
        size_t words = 300000;
        char *big = malloc(words * 5 + 64);
        strcpy(big, "<!doctype html><p>");
        char *at = big + strlen(big);
        for (size_t i = 0; i < words; i++, at += 5)
            memcpy(at, "word ", 5);
        *at = '\0';
        char *got = layout_dump_of(big, 800);
        size_t lines = 0;
        for (const char *p = got; p != NULL && (p = strstr(p, "\n      line ")) != NULL; p++)
            lines++;
        // 36px a word ("word" 32, space 4) in 784: 21 words to a line.
        expect("past the run cap: prose", got != NULL && strstr(got, "incomplete") == NULL &&
               lines == (words + 20) / 21, NULL);
        free(got);
        memset(big + 18, 'x', words * 5);
        char *one = layout_dump_of(big, 800);
        size_t frags = 0;
        for (const char *p = one; p != NULL && (p = strstr(p, "text \"x")) != NULL; p++)
            frags++;
        expect("past the run cap: one word, one line, two runs",
               one != NULL && strstr(one, "incomplete") == NULL &&
               strstr(one, "\n      line ") != NULL && frags == 2, NULL);
        free(one);
        free(big);
    }

    // The </b> after the <br> closes on the <br>'s line: no empty line.
    layout_case("an inline closing after a break closes on that line",
        "<!doctype html><p><b>a<br></b>", 200,
        "page 200 52\n"
        "block html 0 0 200 52\n"
        "  block body 8 16 184 20\n"
        "    block p 8 16 184 20\n"
        "      line 8 16 184 20 base 14\n"
        "        span b 8 18 8 16\n"
        "        text \"a\" 8 18 8 16 serif 16 bold\n");

    // 32px: ascent 24, descent 8, line 40, so 4 of leading above: the
    // baseline is 28 down. The margin is 0.67em of 32 = 1372/64 = 21.44,
    // which collapses with body's 8; the page is 21.44 + 40 + 21.44.
    layout_case("a heading: its own font's line, its own em's margins",
        "<!doctype html><h1>A</h1>", 400,
        "page 400 83\n"
        "block html 0 0 400 83\n"
        "  block body 8 21 384 40\n"
        "    block h1 8 21 384 40\n"
        "      line 8 21 384 40 base 28\n"
        "        text \"A\" 8 25 16 32 serif 32 bold\n");

    // Centred in 84: (84 - 16) / 2 = 34, (84 - 8) / 2 = 38. The line that
    // holds nothing but a <br> is a whole line tall.
    layout_case("alignment, and the blank line two <br>s make",
        "<!doctype html><p align=center>ab<br><br>c", 100,
        "page 100 92\n"
        "block html 0 0 100 92\n"
        "  block body 8 16 84 60\n"
        "    block p 8 16 84 60\n"
        "      line 8 16 84 20 base 14\n"
        "        text \"ab\" 42 18 16 16 serif 16\n"
        "      line 8 36 84 20 base 14\n"
        "      line 8 56 84 20 base 14\n"
        "        text \"c\" 46 58 8 16 serif 16\n");

    // 44 wide: aa bb (36) fits, cc does not. The first line's one gap
    // takes the 8 left over; the last line is not justified.
    layout_case("justify: the gaps grow, the last line does not",
        "<!doctype html><p align=justify>aa bb cc dd</p>", 60,
        "page 60 72\n"
        "block html 0 0 60 72\n"
        "  block body 8 16 44 40\n"
        "    block p 8 16 44 40\n"
        "      line 8 16 44 20 base 14\n"
        "        text \"aa\" 8 18 16 16 serif 16\n"
        "        text \"bb\" 36 18 16 16 serif 16\n"
        "      line 8 36 44 20 base 14\n"
        "        text \"cc dd\" 8 38 36 16 serif 16\n");

    // ul's 40px of padding: the item's content at 48; the marker, U+2022
    // (8) and a space (4), ends there, on the first line's baseline.
    layout_case("an outside marker sits in the list's padding",
        "<!doctype html><ul><li>x</ul>", 200,
        "page 200 52\n"
        "block html 0 0 200 52\n"
        "  block body 8 16 184 20\n"
        "    block ul 8 16 184 20\n"
        "      block li 48 16 144 20\n"
        "        line 48 16 144 20 base 14\n"
        "          text \"x\" 48 18 8 16 serif 16\n"
        // The marker is placed from the first line, so it comes after it.
        "        marker \"\xe2\x80\xa2 \" 36 18 12 16\n");

    // The picture's bottom edge is on the baseline: 30 above it, the text's
    // 6 below it (its descent and half its leading), 36 in all.
    layout_case("a picture on the baseline",
        "<!doctype html><p>a<img src=known.png>b", 200,
        "page 200 68\n"
        "block html 0 0 200 68\n"
        "  block body 8 16 184 36\n"
        "    block p 8 16 184 36\n"
        "      line 8 16 184 36 base 30\n"
        "        text \"a\" 8 34 8 16 serif 16\n"
        "        atomic img 16 16 40 30\n"
        "        text \"b\" 56 34 8 16 serif 16\n");

    // No quirks: every line has a strut, so each picture sits 6px above
    // the next line.
    layout_case("no quirks: the strut opens a gap under a picture",
        "<!doctype html><img src=known.png><br><img src=known.png>", 200,
        "page 200 88\n"
        "block html 0 0 200 88\n"
        "  block body 8 8 184 72\n"
        "    line 8 8 184 36 base 30\n"
        "      atomic img 8 8 40 30\n"
        "    line 8 44 184 36 base 30\n"
        "      atomic img 8 44 40 30\n");
    // Quirks: no strut, and a <br> beside a picture holds nothing open,
    // so sliced images touch (the Quirks standard's 3.3 and 3.4).
    layout_case("quirks: sliced images touch",
        "<img src=known.png><br><img src=known.png>", 200,
        "page 200 76\n"
        "block html 0 0 200 76\n"
        "  block body 8 8 184 60\n"
        "    line 8 8 184 30 base 30\n"
        "      atomic img 8 8 40 30\n"
        "    line 8 38 184 30 base 30\n"
        "      atomic img 8 38 40 30\n");
    layout_case("quirks: a line of nothing but <br> is still a line",
        "a<br><br>b", 200,
        "page 200 76\n"
        "block html 0 0 200 76\n"
        "  block body 8 8 184 60\n"
        "    line 8 8 184 20 base 14\n"
        "      text \"a\" 8 10 8 16 serif 16\n"
        "    line 8 28 184 20 base 14\n"
        "    line 8 48 184 20 base 14\n"
        "      text \"b\" 8 50 8 16 serif 16\n");

    // Monospace 16: every glyph 8, so a tab stop every 64. `\ty` begins 8
    // into its line, and its tab still lands on the line's stop at 64.
    layout_case("pre: tabs on the line's stops, whatever the fragment",
        "<!doctype html><pre>a\tb\n<b>x</b>\ty</pre>", 400,
        "page 400 72\n"
        "block html 0 0 400 72\n"
        "  block body 8 16 384 40\n"
        "    block pre 8 16 384 40\n"
        "      line 8 16 384 20 base 14\n"
        "        text \"a\\tb\" 8 18 72 16 mono 16\n"
        "      line 8 36 384 20 base 14\n"
        "        span b 8 38 8 16\n"
        "        text \"x\" 8 38 8 16 mono 16 bold\n"
        "        text \"\\ty\" 16 38 64 16 mono 16\n");

    layout_case("a link: underlined in its span and its text",
        "<!doctype html><p><a href=x>go</a>", 200,
        "page 200 52\n"
        "block html 0 0 200 52\n"
        "  block body 8 16 184 20\n"
        "    block p 8 16 184 20\n"
        "      line 8 16 184 20 base 14\n"
        "        span a 8 18 16 16 link 0\n"
        "        text \"go\" 8 18 16 16 serif 16 underline link 0\n");

    // hr: 1px inset all round, 0.5em (8) margins, width auto; its borders
    // stop its margin collapsing with the body's only through its content.
    layout_case("hr is its borders",
        "<!doctype html><hr>", 100,
        "page 100 18\n"
        "block html 0 0 100 18\n"
        "  block body 8 8 84 2\n"
        "    block hr 8 8 84 2\n");
}
