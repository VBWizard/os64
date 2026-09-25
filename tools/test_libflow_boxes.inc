// Pass 2: the box tree and each block container's item sequence, every
// expected dump worked out by hand from CSS 2.1 §9.2 (anonymous boxes),
// §16.6.1 (white space) and §17.2.1 (anonymous table objects).
//
// A box line is `<kind> <element|anon>`; its items follow one level
// deeper, then its child boxes. White space is processed HERE, so a text
// item shows what the line breaker will see: a run of spaces is one space,
// a space after a space anywhere in the same context is gone, and a
// context's leading space is gone. A line's TRAILING space stays for pass 3,
// which removes it where a line ends.

static char *boxes_dump_of(const char *html)
{
    os64_html_document_t *doc = parse(html);
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    FStyles *styles = f_style_build(doc, page, &kEnv);
    FBoxes *boxes = f_boxes_build(doc, page, styles, &kEnv);
    char *text = NULL;
    if (boxes != NULL) {
        int64_t need = f_boxes_dump(boxes, NULL, 0);
        text = malloc((size_t)need + 1);
        f_boxes_dump(boxes, text, (size_t)need + 1);
    }
    f_boxes_free(boxes);
    f_style_free(styles);
    os64_page_free(page);
    os64_html_document_free(doc);
    return text;
}

static void boxes_case(const char *name, const char *html, const char *expected)
{
    char *got = boxes_dump_of(html);
    bool ok = got != NULL && strcmp(got, expected) == 0;
    expect(name, ok, NULL);
    if (!ok)
        fprintf(stderr, "---- expected\n%s---- got\n%s----\n", expected, got ? got : "(null)\n");
    free(got);
}

static void boxes_cases(void)
{
    boxes_case("a paragraph: head makes no box, text is an item",
        "<!doctype html><p>Hi",
        "block html\n"
        "  block body\n"
        "    block p\n"
        "      text \"Hi\"\n");

    boxes_case("white space: collapsed across element boundaries",
        "<!doctype html><p>  a  <b> b </b>  c\n\td </p>",
        "block html\n"
        "  block body\n"
        "    block p\n"
        "      text \"a \"\n"
        "      open b\n"
        "      text \"b \"\n"
        "      close b\n"
        "      text \"c d \"\n");

    boxes_case("white space: a lone space inside an element survives once",
        "<!doctype html><p>a<b> </b>c<i> </i> <u> </u>d</p>",
        "block html\n"
        "  block body\n"
        "    block p\n"
        "      text \"a\"\n"
        "      open b\n"
        "      text \" \"\n"
        "      close b\n"
        "      text \"c\"\n"
        "      open i\n"
        "      text \" \"\n"
        "      close i\n"
        "      open u\n"
        "      close u\n"
        "      text \"d\"\n");

    boxes_case("white space between blocks makes nothing",
        "<!doctype html>\n<p>a</p>\n  <p>b</p>\n",
        "block html\n"
        "  block body\n"
        "    block p\n"
        "      text \"a\"\n"
        "    block p\n"
        "      text \"b\"\n");

    boxes_case("pre: every byte kept, a newline is a break, offsets into the node",
        "<!doctype html><pre>a\tb\n  c\n</pre>",
        "block html\n"
        "  block body\n"
        "    block pre\n"
        "      text \"a\\tb\"\n"
        "      newline\n"
        "      text \"  c\" @4\n"
        "      newline\n");

    boxes_case("§9.2.1.1: inline content beside a block is wrapped in anonymous blocks",
        "<!doctype html><div>one<p>two</p>three</div>",
        "block html\n"
        "  block body\n"
        "    block div\n"
        "      block anon\n"
        "        text \"one\"\n"
        "      block p\n"
        "        text \"two\"\n"
        "      block anon\n"
        "        text \"three\"\n");

    boxes_case("§9.2.1.1: an inline holding a block is split round it",
        "<!doctype html><div>x<font face=arial>one<p>two</p>three</font>y</div>",
        "block html\n"
        "  block body\n"
        "    block div\n"
        "      block anon\n"
        "        text \"x\"\n"
        "        open font\n"
        "        text \"one\"\n"
        "        close font split\n"
        "      block p\n"
        "        text \"two\"\n"
        "      block anon\n"
        "        open font continued\n"
        "        text \"three\"\n"
        "        close font\n"
        "        text \"y\"\n");

    boxes_case("§9.2.1.1: <font> round every paragraph makes no empty boxes",
        "<!doctype html><font face=arial><p>a</p> <p>b</p></font>",
        "block html\n"
        "  block body\n"
        "    block p\n"
        "      text \"a\"\n"
        "    block p\n"
        "      text \"b\"\n");

    boxes_case("§9.2.1.1: nested inlines split and reopen outermost first",
        "<!doctype html><div><a href=x><b>1<p>2</p>3</b></a></div>",
        "block html\n"
        "  block body\n"
        "    block div\n"
        "      block anon\n"
        "        open a link 0\n"
        "        open b\n"
        "        text \"1\"\n"
        "        close b split\n"
        "        close a split\n"
        "      block p link 0\n"
        "        text \"2\"\n"
        "      block anon\n"
        "        open a link 0 continued\n"
        "        open b continued\n"
        "        text \"3\"\n"
        "        close b\n"
        "        close a\n");

    boxes_case("a block that splits a link carries the link",
        "<!doctype html><center><a href=vote><div></div></a></center>"
        "<a href=x><table><tr><td>t</table></a>",
        "block html\n"
        "  block body\n"
        "    block center\n"
        "      block div link 0\n"
        "    table table link 1\n"
        "      row-group tbody\n"
        "        row tr\n"
        "          cell td\n"
        "            text \"t\"\n");

    boxes_case("an empty inline beside a block opens no run",
        "<!doctype html><div><span></span><p>x</p></div>",
        "block html\n"
        "  block body\n"
        "    block div\n"
        "      block p\n"
        "        text \"x\"\n");

    boxes_case("markers: start, value, reversed, letters, roman numerals, bullets",
        "<!doctype html><ol start=3><li>a<li value=10>b<li>c</ol><ul><li>d</ul>"
        "<ol reversed><li>x<li>y</ol><ol type=a start=27><li>z</ol>"
        "<ol type=I start=1994><li>w</ol><ol start=-2><li>n</ol>",
        "block html\n"
        "  block body\n"
        "    block ol\n"
        "      block li marker \"3. \"\n"
        "        text \"a\"\n"
        "      block li marker \"10. \"\n"
        "        text \"b\"\n"
        "      block li marker \"11. \"\n"
        "        text \"c\"\n"
        "    block ul\n"
        "      block li marker \"\xe2\x80\xa2 \"\n"
        "        text \"d\"\n"
        "    block ol\n"
        "      block li marker \"2. \"\n"
        "        text \"x\"\n"
        "      block li marker \"1. \"\n"
        "        text \"y\"\n"
        "    block ol\n"
        "      block li marker \"aa. \"\n"
        "        text \"z\"\n"
        "    block ol\n"
        "      block li marker \"MCMXCIV. \"\n"
        "        text \"w\"\n"
        // letters and numerals cannot spell zero or less: decimal
        "    block ol\n"
        "      block li marker \"-2. \"\n"
        "        text \"n\"\n");

    boxes_case("markers: a nested list keeps the outer count going after it",
        "<!doctype html><ol><li>a<ol><li>b<li>c</ol><li>d</ol>",
        "block html\n"
        "  block body\n"
        "    block ol\n"
        "      block li marker \"1. \"\n"
        "        block anon\n"
        "          text \"a\"\n"
        "        block ol\n"
        "          block li marker \"1. \"\n"
        "            text \"b\"\n"
        "          block li marker \"2. \"\n"
        "            text \"c\"\n"
        "      block li marker \"2. \"\n"
        "        text \"d\"\n");

    boxes_case("markers: an inside marker is the item's first inline content",
        "<li>x</li><li><p>y</p></li>",
        "block html\n"
        "  block body\n"
        "    block li\n"
        "      marker \"\xe2\x80\xa2 \"\n"
        "      text \"x\"\n"
        // the marker is inline content, so a block beside it mixes the item
        "    block li\n"
        "      block anon\n"
        "        marker \"\xe2\x80\xa2 \"\n"
        "      block p\n"
        "        text \"y\"\n");

    boxes_case("details: the disclosure and nothing a closed one hides",
        "<!doctype html><details><summary>S</summary>hidden<p>h</p></details>"
        "<details open><summary>T</summary>shown</details>",
        "block html\n"
        "  block body\n"
        "    block details\n"
        "      block summary\n"
        "        marker \"\xe2\x96\xb8 \"\n"
        "        text \"S\"\n"
        "    block details\n"
        "      block summary\n"
        "        marker \"\xe2\x96\xbe \"\n"
        "        text \"T\"\n"
        "      block anon\n"
        "        text \"shown\"\n");

    boxes_case("generated content: q's quotes by level, parentheses round rt",
        "<!doctype html><p><q>a <q>b</q></q><ruby>x<rt>y</rt></ruby>"
        "<ruby>z<rp>(</rp><rt>w</rt><rp>)</rp></ruby>",
        "block html\n"
        "  block body\n"
        "    block p\n"
        "      open q\n"
        "      text \"\xe2\x80\x9c\" generated-by q\n"
        "      text \"a \"\n"
        "      open q\n"
        "      text \"\xe2\x80\x98\" generated-by q\n"
        "      text \"b\"\n"
        "      text \"\xe2\x80\x99\" generated-by q\n"
        "      close q\n"
        "      text \"\xe2\x80\x9d\" generated-by q\n"
        "      close q\n"
        "      open ruby\n"
        "      text \"x\"\n"
        "      open rt\n"
        "      text \"(\" generated-by rt\n"
        "      text \"y\"\n"
        "      text \")\" generated-by rt\n"
        "      close rt\n"
        "      close ruby\n"
        // a ruby that spells its own parentheses gets none generated;
        // rp is display none, and the page's own are all there is
        "      open ruby\n"
        "      text \"z\"\n"
        "      open rt\n"
        "      text \"w\"\n"
        "      close rt\n"
        "      close ruby\n");

    boxes_case("atomic inlines: controls and pictures, br and wbr",
        "<!doctype html><form><p>Name <input name=n><br>go<wbr>on <img src=x.png alt=pic> "
        "<select name=s><option>o</select></form>",
        "block html\n"
        "  block body\n"
        "    block form\n"
        "      block p\n"
        "        text \"Name \"\n"
        "        atomic input control 0\n"
        "        br\n"
        "        text \"go\"\n"
        "        wbr\n"
        "        text \"on \"\n"
        "        atomic img\n"
        "        text \" \"\n"
        "        atomic select control 1\n");

    boxes_case("a button's content is its widget's, not the page's",
        "<!doctype html><p><button><b>B</b></button><textarea>t</textarea>",
        "block html\n"
        "  block body\n"
        "    block p\n"
        "      atomic button control 0\n"
        "      atomic textarea control 1\n");

    boxes_case("links: libpage's index on the opening edge",
        "<!doctype html><p><a href=x>one <b>two</b></a> <a name=n>three</a>",
        "block html\n"
        "  block body\n"
        "    block p\n"
        "      open a link 0\n"
        "      text \"one \"\n"
        "      open b\n"
        "      text \"two\"\n"
        "      close b\n"
        "      close a\n"
        "      text \" \"\n"
        "      open a\n"
        "      text \"three\"\n"
        "      close a\n");

    boxes_case("frames: block-level replaced boxes, a link each that names one",
        "<frameset><frame src=a.html><frame></frameset>",
        "block html\n"
        "  block frameset\n"
        "    replaced frame link 0\n"
        "    replaced frame\n");

    boxes_case("an iframe is an atomic inline, drawn as its link",
        "<!doctype html><p>x<iframe src=f.html></iframe>",
        "block html\n"
        "  block body\n"
        "    block p\n"
        "      text \"x\"\n"
        "      atomic iframe link 0\n");

    boxes_case("a marquee is an atom with a block of its own",
        "<!doctype html><marquee>news <b>flash</b></marquee>",
        "block html\n"
        "  block body\n"
        "    atomic marquee\n"
        "      block marquee\n"
        "        text \"news \"\n"
        "        open b\n"
        "        text \"flash\"\n"
        "        close b\n");

    boxes_case("foreign content: SVG's text is not prose, HTML inside it is",
        "<!doctype html><p>a<svg><text>no</text><foreignObject><b>yes</b></foreignObject></svg>b",
        "block html\n"
        "  block body\n"
        "    block p\n"
        "      text \"a\"\n"
        "      open b\n"
        "      text \"yes\"\n"
        "      close b\n"
        "      text \"b\"\n");

    boxes_case("hr is an empty block its borders draw",
        "<!doctype html><hr><p>x",
        "block html\n"
        "  block body\n"
        "    block hr\n"
        "    block p\n"
        "      text \"x\"\n");

    boxes_case("embed and audio make nothing, object shows its fallback",
        "<!doctype html><p>a<embed src=x><audio src=y>no</audio><object>fb</object>b",
        "block html\n"
        "  block body\n"
        "    block p\n"
        "      text \"a\"\n"
        "      open object\n"
        "      text \"fb\"\n"
        "      close object\n"
        "      text \"b\"\n");

    boxes_case("tables: caption, columns, the row group the parser made",
        "<!doctype html><table><caption>c</caption><colgroup><col><col></colgroup>"
        "<tr><td>1<td>2</tr></table>",
        "block html\n"
        "  block body\n"
        "    table table\n"
        "      caption caption\n"
        "        text \"c\"\n"
        "      column-group colgroup\n"
        "        column col\n"
        "        column col\n"
        "      row-group tbody\n"
        "        row tr\n"
        "          cell td\n"
        "            text \"1\"\n"
        "          cell td\n"
        "            text \"2\"\n");

    boxes_case("tables: the white space between parts belongs to none of them",
        "<!doctype html><table>\n <tr>\n  <td> a </td>\n </tr>\n</table>",
        "block html\n"
        "  block body\n"
        "    table table\n"
        "      row-group tbody\n"
        "        row tr\n"
        "          cell td\n"
        "            text \"a \"\n");

    boxes_case("tables: a form the parser left in a table makes no box",
        "<!doctype html><table><form><tr><td><input name=q></table>",
        "block html\n"
        "  block body\n"
        "    table table\n"
        "      row-group tbody\n"
        "        row tr\n"
        "          cell td\n"
        "            atomic input control 0\n");
}

// Every allocation of pass 2 fails in turn, on a page with lists, tables,
// split inlines and controls: each partial tree is marked incomplete and
// is a PREFIX of the whole one — the same boxes and items in the same
// order, stopping where memory did (LAYOUT.md § Proof, rule (a)).
static void boxes_sweep(void)
{
    const char *html =
        "<!doctype html><div>x<font face=arial>one<p>two</p>three</font>y</div>"
        "<ol start=3><li>a<li><ol><li>b</ol></ol><table><tr><td>1<td><b>2</b></table>"
        "<form><p>n <input name=n> <q>q</q><br><img src=i.png></form>"
        "<li>quirk-free inside?<pre>p\nq</pre>";
    os64_html_document_t *doc = parse(html);
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    FStyles *styles = f_style_build(doc, page, &kEnv);
    FBoxes *whole = f_boxes_build(doc, page, styles, &kEnv);
    int64_t whole_len = f_boxes_dump(whole, NULL, 0);
    char *whole_text = malloc((size_t)whole_len + 1);
    f_boxes_dump(whole, whole_text, (size_t)whole_len + 1);
    f_boxes_free(whole);

    size_t base_live = live;
    allocations = 0;
    FBoxes *probe = f_boxes_build(doc, page, styles, &kEnv);
    size_t count = allocations;
    f_boxes_free(probe);
    size_t tried = 0;
    for (size_t at = 1; at <= count; at++) {
        allocations = 0;
        fail_at = at;
        FBoxes *part = f_boxes_build(doc, page, styles, &kEnv);
        fail_at = 0;
        tried++;
        if (part != NULL) {
            int64_t n = f_boxes_dump(part, NULL, 0);
            char *text = malloc((size_t)n + 1);
            f_boxes_dump(part, text, (size_t)n + 1);
            const char *tail = "incomplete\n";
            size_t body = strlen(text);
            if (part->incomplete && body >= strlen(tail))
                body -= strlen(tail);
            char name[64];
            snprintf(name, sizeof(name), "boxes sweep: failure %zu is a prefix", at);
            expect(name, strncmp(text, whole_text, body) == 0 &&
                         (part->incomplete || strcmp(text, whole_text) == 0), NULL);
            free(text);
        }
        f_boxes_free(part);
        if (live != base_live) {
            expect("boxes sweep: nothing leaked", false, NULL);
            break;
        }
    }
    printf("libflow boxes sweep: each of %zu allocations failed in turn, every tree a prefix\n",
           tried);
    free(whole_text);
    f_style_free(styles);
    os64_page_free(page);
    os64_html_document_free(doc);
}
