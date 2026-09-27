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

    // Codex #147. Justification grows only real gaps: "a" and "b" are one
    // word across an element boundary, so "b" stays against "a". "ab c d"
    // is 40 of 44; two gaps share the 4, so "c" moves 2 and "d" 4 and the
    // line ends flush at 52. The <b> box's edges are before the first gap.
    layout_case("justify grows gaps, not element boundaries",
        "<!doctype html><p align=justify>a<b>b</b> c d e f g</p>", 60,
        "page 60 72\n"
        "block html 0 0 60 72\n"
        "  block body 8 16 44 40\n"
        "    block p 8 16 44 40\n"
        "      line 8 16 44 20 base 14\n"
        "        span b 16 18 8 16\n"
        "        text \"a\" 8 18 8 16 serif 16\n"
        "        text \"b\" 16 18 8 16 serif 16 bold\n"
        "        text \"c\" 30 18 8 16 serif 16\n"
        "        text \"d\" 44 18 8 16 serif 16\n"
        "      line 8 36 44 20 base 14\n"
        "        text \"e f g\" 8 38 28 16 serif 16\n");

    // A line holding only an empty inline is zero-height (CSS 2.1 § 9.4.2),
    // so the first p is too, its margins collapsing through it: the second
    // p is at 16. The empty line sits where a line would, at 16.
    layout_case("an empty inline makes no line",
        "<!doctype html><p><span></span></p><p>x</p>", 200,
        "page 200 52\n"
        "block html 0 0 200 52\n"
        "  block body 8 16 184 20\n"
        "    block p 8 16 184 0\n"
        "      line 8 16 184 0 base 14\n"
        "        span span 8 18 0 16\n"
        "    block p 8 16 184 20\n"
        "      line 8 16 184 20 base 14\n"
        "        text \"x\" 8 18 8 16 serif 16\n");

    // An empty item still shows its bullet, on a line of its own font:
    // 20 tall, the marker's baseline 14 down, and the next item below it.
    layout_case("an empty list item holds its marker's line",
        "<!doctype html><ul><li><li>x</ul>", 200,
        "page 200 72\n"
        "block html 0 0 200 72\n"
        "  block body 8 16 184 40\n"
        "    block ul 8 16 184 40\n"
        "      block li 48 16 144 20\n"
        "        marker \"\xe2\x80\xa2 \" 36 18 12 16\n"
        "      block li 48 36 144 20\n"
        "        line 48 36 144 20 base 14\n"
        "          text \"x\" 48 38 8 16 serif 16\n"
        "        marker \"\xe2\x80\xa2 \" 36 38 12 16\n");

    // A frame the page sized on one side keeps 300 by 150 on the other:
    // 600 x 150 and 300 x 50, each in its 2px inset border.
    layout_case("a frame keeps its default on the side left unset",
        "<!doctype html><iframe width=600></iframe><iframe height=50></iframe>", 1000,
        "page 1000 176\n"
        "block html 0 0 1000 176\n"
        "  block body 8 8 984 160\n"
        "    line 8 8 984 160 base 154\n"
        "      atomic iframe 8 8 604 154\n"
        "      atomic iframe 612 108 304 54\n");

    // A missing picture's alt text collapses its spaces as text does:
    // "a b" is 8 + 4 + 8.
    layout_case("alt text collapses its white space",
        "<!doctype html><p>q<img src=missing.png alt='a   b'>z", 200,
        "page 200 52\n"
        "block html 0 0 200 52\n"
        "  block body 8 16 184 20\n"
        "    block p 8 16 184 20\n"
        "      line 8 16 184 20 base 14\n"
        "        text \"q\" 8 18 8 16 serif 16\n"
        "        text \"a b\" 16 18 20 16 serif 16\n"
        "        text \"z\" 36 18 8 16 serif 16\n");

    // An outside marker is in the link its item sits in.
    layout_case("a marker keeps its item's link",
        "<!doctype html><a href=x><ul><li>foo</li></ul></a>", 200,
        "page 200 52\n"
        "block html 0 0 200 52\n"
        "  block body 8 16 184 20\n"
        "    block ul 8 16 184 20 link 0\n"
        "      block li 48 16 144 20 link 0\n"
        "        line 48 16 144 20 base 14\n"
        "          text \"foo\" 48 18 20 16 serif 16 underline link 0\n"
        "        marker \"\xe2\x80\xa2 \" 36 18 12 16 link 0\n");

    // Codex #147 round 2. Alt text collapses against the text beside it:
    // "a " already ends in a space, so " b" loses its own, and " c" keeps
    // one. 12, 8 and 12 wide.
    layout_case("alt text collapses against its neighbours",
        "<!doctype html><p>a <img src=missing.png alt=\" b\"> c", 200,
        "page 200 52\n"
        "block html 0 0 200 52\n"
        "  block body 8 16 184 20\n"
        "    block p 8 16 184 20\n"
        "      line 8 16 184 20 base 14\n"
        "        text \"a \" 8 18 12 16 serif 16\n"
        "        text \"b\" 20 18 8 16 serif 16\n"
        "        text \" c\" 28 18 12 16 serif 16\n");

    // A link has no edge an inline-block's content could fall outside: the
    // text in a linked marquee is the link, though the `a`'s underline
    // stops at the atom (decorations do not reach an atom's content). The
    // marquee's overflow is hidden, so its baseline is its bottom edge, 20;
    // the 16px strut hangs 6 below it, and the `a`'s own box is the strut's
    // font box on that baseline: 8 + 20 - 12 = 16.
    layout_case("an inline-block's content is in the link its atom is",
        "<!doctype html><a href=x><marquee>hi</marquee></a>", 200,
        "page 200 42\n"
        "block html 0 0 200 42\n"
        "  block body 8 8 184 26\n"
        "    line 8 8 184 26 base 20\n"
        "      span a 8 16 184 16 link 0\n"
        "      atomic marquee 8 8 184 20 link 0\n"
        "        block marquee 8 8 184 20 link 0\n"
        "          line 8 8 184 20 base 14\n"
        "            text \"hi\" 8 10 12 16 serif 16 link 0\n");

    // Codex #147 round 3. A right-aligned rule wider than its containing
    // block: the auto left margin is zero (§10.3.3), so the rule starts at
    // the content edge and overflows to the right, 8 + 1000 + 2 of border.
    layout_case("an over-wide box's auto margin is zero",
        "<!doctype html><hr align=right width=1000>", 800,
        "page 1010 18\n"
        "block html 0 0 800 18\n"
        "  block body 8 8 784 2\n"
        "    block hr 8 8 1002 2\n");

    // Codex #147 round 3. An inside marker ends in its suffix space, and the
    // break after it is the item's: in 15px the marker (12) fits the first
    // line and the x (8) goes to the next.
    layout_case("a line may break after an inside marker",
        "<!doctype html><details><summary>x</summary></details>", 31,
        "page 31 56\n"
        "block html 0 0 31 56\n"
        "  block body 8 8 15 40\n"
        "    block details 8 8 15 40\n"
        "      block summary 8 8 15 40\n"
        "        line 8 8 15 20 base 14\n"
        "          marker \"\xe2\x96\xb8 \" 8 10 12 16\n"
        "        line 8 28 15 20 base 14\n"
        "          text \"x\" 8 30 8 16 serif 16\n");

    // Codex #147 round 3. Pre-wrap spaces ending a line hang: drawn (x and
    // a hundred 8px spaces, 808 wide) but counted for nothing, so the page
    // stays as wide as the viewport.
    layout_case("hanging pre-wrap spaces do not widen the page",
        "<!doctype html><pre wrap>x                                                                                                    </pre>", 100,
        "page 100 52\n"
        "block html 0 0 100 52\n"
        "  block body 8 16 84 20\n"
        "    block pre 8 16 84 20\n"
        "      line 8 16 84 20 base 14\n"
        "        text \"x                                                                                                    \" 8 18 808 16 mono 16\n");

    // Codex #147 round 3. A tab is as wide as the way to the line's next
    // stop, measured where the line puts it: after the 8px x the tab runs to
    // 64 and the y ends at 72, exactly the content width, so it fits.
    layout_case("a tab is measured from where the line puts it",
        "<!doctype html><pre wrap><b>x</b>\ty</pre>", 88,
        "page 88 52\n"
        "block html 0 0 88 52\n"
        "  block body 8 16 72 20\n"
        "    block pre 8 16 72 20\n"
        "      line 8 16 72 20 base 14\n"
        "        span b 8 18 8 16\n"
        "        text \"x\" 8 18 8 16 mono 16 bold\n"
        "        text \"\\ty\" 16 18 64 16 mono 16\n");

    // Codex #147 round 3. Sliced images in an auto-width cell, full quirks:
    // the newline between them is no opportunity either, so the row is one
    // word and the cell is as wide as it: 100 + a 4px space + 100 = 204,
    // with the cell's 1px padding and the table's 2px spacing round it.
    layout_case("quirks: whitespace between sliced images does not break them",
        "<table><tr><td><img src=known.png width=100 height=10>\n"
        "<img src=known.png width=100 height=10></table>", 150,
        "page 218 42\n"
        "block html 0 0 150 42\n"
        "  block body 8 8 134 26\n"
        "    table table 8 8 210 26\n"
        "      row-group tbody 10 10 206 22\n"
        "        row tr 10 10 206 22\n"
        "          cell td 10 10 206 22\n"
        "            line 11 11 204 20 base 14\n"
        "              atomic img 11 15 100 10\n"
        "              text \" \" 111 13 4 16 serif 16\n"
        "              atomic img 115 15 100 10\n");

    // Codex #147 round 4. Alt text collapses against its neighbours across
    // inline edges: " b " inside a <b> between "a " and " c" is just the b.
    layout_case("alt text collapses across inline edges",
        "<!doctype html><p>a <b><img src=missing.png alt=\" b \"></b> c", 200,
        "page 200 52\n"
        "block html 0 0 200 52\n"
        "  block body 8 16 184 20\n"
        "    block p 8 16 184 20\n"
        "      line 8 16 184 20 base 14\n"
        "        span b 20 18 8 16\n"
        "        text \"a \" 8 18 12 16 serif 16\n"
        "        text \"b\" 20 18 8 16 serif 16 bold\n"
        "        text \" c\" 28 18 12 16 serif 16\n");

    // An inside marker is in its item's link, as its text is.
    layout_case("an inside marker keeps its item's link",
        "<!doctype html><a href=x><details><summary>x</summary></details></a>", 200,
        "page 200 36\n"
        "block html 0 0 200 36\n"
        "  block body 8 8 184 20\n"
        "    block details 8 8 184 20 link 0\n"
        "      block summary 8 8 184 20 link 0\n"
        "        line 8 8 184 20 base 14\n"
        "          marker \"\xe2\x96\xb8 \" 8 10 12 16 link 0\n"
        "          text \"x\" 20 10 8 16 serif 16 underline link 0\n");

    // A table's height is a least height (CSS 2.1 § 17.5.3): the row is
    // 1 + 20 + 1, so the table is that and its 2px of spacing a side, 26
    // tall, not 1. The cell is "x" and 1px of padding a side, 10 wide; the
    // table 14.
    layout_case("a table's set height is its least",
        "<!doctype html><table height=1><tr><td>x</table>", 200,
        "page 200 42\n"
        "block html 0 0 200 42\n"
        "  block body 8 8 184 26\n"
        "    table table 8 8 14 26\n"
        "      row-group tbody 10 10 10 22\n"
        "        row tr 10 10 10 22\n"
        "          cell td 10 10 10 22\n"
        "            line 11 11 8 20 base 14\n"
        "              text \"x\" 11 13 8 16 serif 16\n");

    // A missing picture whose alt is empty is decoration: no line at all,
    // so its p is empty and the next p's margin collapses through it.
    layout_case("a missing picture with an empty alt takes no space",
        "<!doctype html><p><img src=missing.png alt=\"\"></p><p>x</p>", 200,
        "page 200 52\n"
        "block html 0 0 200 52\n"
        "  block body 8 16 184 20\n"
        "    block p 8 16 184 0\n"
        "    block p 8 16 184 20\n"
        "      line 8 16 184 20 base 14\n"
        "        text \"x\" 8 18 8 16 serif 16\n");
}
