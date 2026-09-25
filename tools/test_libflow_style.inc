// Pass 1: the Rendering chapter and the presentational attributes, each
// expected dump worked out from the rule by hand. A line names the element,
// its display, then what it CHANGED: an inherited property where it differs
// from its parent, any other where it differs from its initial value.
//
// The arithmetic, once, for the numbers that recur: sizes are 26.6, so
// 0.67em of 16px is 1024 * 670 / 1000 = 686 units = 10.72px, and a heading's
// margin is em of its OWN size — h1's 0.67em of 32px is 1372 units, 21.44px.

#define PAGE_OPEN "html block holds-block\n  head none\n"

static void style_cases(void)
{
    style_case("the page: body margins, a paragraph's",
        "<!doctype html><p>Hi",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    p block margin=16/0/16/0\n");

    style_case("headings: size in the parent's em, margins in their own",
        "<!doctype html><h1>a</h1><h6>b</h6>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    h1 block weight=700 size=32 margin=21.44/0/21.44/0\n"
        // 686 units; 686 * 2330 / 1000 = 1598 units = 24.97px
        "    h6 block weight=700 size=10.72 margin=24.97/0/24.97/0\n");

    style_case("body attributes: margins and colours",
        "<!doctype html><body marginwidth=0 topmargin=3 bgcolor=white text=#00f>x",
        PAGE_OPEN
        "  body block color=#0000ff bg=#ffffff margin=3/0/3/0\n");

    style_case("phrasing: bolder twice, italic, monospace, sizes",
        "<!doctype html><b><b>x</b></b><i>y</i><tt>z</tt><big>b</big><small>s</small>"
        "<sub>d</sub>",
        PAGE_OPEN
        "  body block margin=8/8/8/8\n"
        "    b inline weight=700\n"
        "      b inline weight=900\n"
        "    i inline italic\n"
        "    tt inline font=mono\n"
        // 1024 * 6 / 5 = 1228.8 -> 1229 units; 1024 * 5 / 6 -> 853 units
        "    big inline size=19.20\n"
        "    small inline size=13.33\n"
        "    sub inline size=13.33 valign=sub\n");

    style_case("decorations: u, s, ins, del, abbr only with a title",
        "<!doctype html><u>a</u><s>b</s><ins>c</ins><del>d</del><abbr>e</abbr>"
        "<abbr title=t>f</abbr><mark>g</mark>",
        PAGE_OPEN
        "  body block margin=8/8/8/8\n"
        "    u inline underline\n"
        "    s inline line-through\n"
        "    ins inline underline\n"
        "    del inline line-through\n"
        "    abbr inline\n"
        "    abbr inline underline\n"
        "    mark inline color=#000000 bg=#ffff00\n");

    style_case("font: face list, legacy size, legacy colour",
        "<!doctype html><font face=\"Verdana, Arial, sans-serif\" size=+2 color=chucknorris>x"
        "</font><font face=\"Courier New\" size=1>y</font><font face=Times size=-9>z</font>"
        "<font size=9 face=\" \">w</font><font size=\"\">v</font>",
        PAGE_OPEN
        "  body block margin=8/8/8/8\n"
        // +2 -> 5 -> x-large, 3/2 of 16
        "    font inline font=\"Verdana\",\"Arial\",sans size=24 color=#c00000\n"
        // 1 -> x-small, 3/4 of 16
        "    font inline font=\"Courier New\",mono size=12\n"
        // 3 - 9 clamps to 1
        "    font inline font=\"Times\",serif size=12\n"
        // 9 clamps to 7, xxx-large; a face of nothing names no family, and
        // an empty font-family is no declaration at all
        "    font inline size=48\n"
        "    font inline\n");

    style_case("links: libpage's links only, body link= recolours them",
        "<!doctype html><a href=x>l</a><a name=n>m</a><a href=y><font color=green>g</font></a>",
        PAGE_OPEN
        "  body block margin=8/8/8/8\n"
        "    a inline color=link underline\n"
        "    a inline\n"
        "    a inline color=link underline\n"
        "      font inline color=#008000\n");
    style_case("links: body link=",
        "<!doctype html><body link=red><a href=x>l</a>",
        PAGE_OPEN
        "  body block margin=8/8/8/8\n"
        "    a inline color=#ff0000 underline\n");

    style_case("lists: the nesting sheet",
        "<!doctype html><ul><li>a<ul><li>b<ol><li>c<ul><li>d</ul></ol></ul></ul>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    ul block margin=16/0/16/0 padding=0/0/0/40 holds-block\n"
        "      li list-item holds-block\n"
        "        ul block padding=0/0/0/40 list=circle holds-block\n"
        "          li list-item holds-block\n"
        "            ol block padding=0/0/0/40 list=decimal holds-block\n"
        "              li list-item holds-block\n"
        "                ul block padding=0/0/0/40 list=square holds-block\n"
        "                  li list-item\n");

    style_case("lists: type is case-sensitive for letters, not for names",
        "<!doctype html><ol type=A><li type=i>a<li type=DISC>b</ol><ul type=Square><li>c</ul>"
        "<dl><dt>t<dd>d</dl>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    ol block margin=16/0/16/0 padding=0/0/0/40 list=upper-alpha holds-block\n"
        "      li list-item list=lower-roman\n"
        // `disc` on an li is a name, matched without case
        "      li list-item list=disc\n"
        "    ul block margin=16/0/16/0 padding=0/0/0/40 list=square holds-block\n"
        "      li list-item\n"
        "    dl block margin=16/0/16/0 holds-block\n"
        "      dt block\n"
        "      dd block margin=0/0/0/40\n");

    style_case("tables: border, cellpadding, cellspacing, width",
        "<!doctype html><table border=2 cellpadding=5 cellspacing=0 width=50%>"
        "<tr><td nowrap>x</td></tr></table>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    table table border=2:outset:ink width=50% holds-block\n"
        "      tbody table-row-group valign=middle holds-block\n"
        "        tr table-row valign=middle holds-block\n"
        "          td table-cell padding=5/5/5/5 border=1:inset:ink valign=middle ws=nowrap\n");

    style_case("tables: the defaults, th, caption, valign, bgcolor",
        "<!doctype html><table><caption align=bottom>c</caption>"
        "<tr valign=top bgcolor=red><th>h<td valign=bottom width=0>d</table>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    table table spacing=2/2 holds-block\n"
        "      caption table-caption align=center caption-bottom\n"
        "      tbody table-row-group valign=middle holds-block\n"
        "        tr table-row bg=#ff0000 valign=top holds-block\n"
        // th centres because the row's alignment is the initial one
        "          th table-cell weight=700 padding=1/1/1/1 align=center valign=top\n"
        // `width=0` on a cell is ignored: a nonzero dimension
        "          td table-cell padding=1/1/1/1 valign=bottom\n");

    style_case("tables: border=0 draws nothing, a bad border is 1",
        "<!doctype html><table border=0><tr><td>a</table><table border=x><tr><td>b</table>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    table table spacing=2/2 holds-block\n"
        "      tbody table-row-group valign=middle holds-block\n"
        "        tr table-row valign=middle holds-block\n"
        "          td table-cell padding=1/1/1/1 valign=middle\n"
        "    table table border=1:outset:ink spacing=2/2 holds-block\n"
        "      tbody table-row-group valign=middle holds-block\n"
        "        tr table-row valign=middle holds-block\n"
        "          td table-cell padding=1/1/1/1 border=1:inset:ink valign=middle\n");

    style_case("alignment: center and div align reach the blocks inside",
        "<!doctype html><center><div align=right><table align=center><tr><th>x"
        "<td align=left>y</table></div></center>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    center block align=html-center holds-block\n"
        "      div block align=html-right holds-block\n"
        "        table table margin=0/auto/0/auto spacing=2/2 holds-block\n"
        "          tbody table-row-group valign=middle holds-block\n"
        "            tr table-row valign=middle holds-block\n"
        // a th under an aligned row keeps the row's alignment
        "              th table-cell weight=700 padding=1/1/1/1 valign=middle\n"
        "              td table-cell padding=1/1/1/1 align=html-left valign=middle\n");

    style_case("hr: the sheet, align, size, noshade, colour",
        "<!doctype html><hr><hr align=left width=50% size=1><hr noshade size=4 color=red>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    hr block color=#808080 margin=8/auto/8/auto border=1:inset:#808080\n"
        "    hr block color=#808080 margin=8/auto/8/0 border=1:inset:#808080/1:inset:#808080/"
        "0:inset:#808080/1:inset:#808080 width=50%\n"
        "    hr block color=#ff0000 margin=8/auto/8/auto border=2:solid:#ff0000\n");

    style_case("pictures: align, hspace, border, size",
        "<!doctype html><img align=left hspace=5 border=2 width=100 height=50%>"
        "<img align=middle vspace=1><img align=absmiddle><input type=image align=texttop>",
        PAGE_OPEN
        "  body block margin=8/8/8/8\n"
        "    img inline margin=0/5/0/5 border=2:solid:ink width=100 height=50% float=left\n"
        "    img inline margin=1/0/1/0 valign=html-middle\n"
        "    img inline valign=middle\n"
        "    input inline-block valign=text-top\n");

    style_case("pre, listing, pre wrap, nobr, br clear",
        "<!doctype html><pre wrap>a</pre><listing>b</listing><nobr>c</nobr><br clear=all>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    pre block font=mono margin=16/0/16/0 ws=pre-wrap\n"
        "    listing block font=mono margin=16/0/16/0 ws=pre\n"
        "    nobr inline ws=nowrap\n"
        "    br inline clear=both\n");

    style_case("hidden things: display none, and nothing inside them",
        "<!doctype html><div hidden><p>x</p></div><input type=HIDDEN><dialog>d</dialog>"
        "<dialog open>o</dialog><embed hidden>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    div none\n"
        "    input none\n"
        "    dialog none\n"
        // 1em of padding; `border: solid` is medium, 3px; colours from the face
        "    dialog block bg=paper padding=16/16/16/16 border=3:solid:ink\n"
        "    embed inline width=0 height=0\n");

    style_case("details: the first summary is the disclosure",
        "<!doctype html><details><summary>S</summary><p>hidden</p></details>"
        "<details open><summary>T</summary><p>shown</p></details>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    details block holds-block\n"
        "      summary list-item list=disclosure-closed list-inside\n"
        "      p none\n"
        "    details block holds-block\n"
        "      summary list-item list=disclosure-open list-inside\n"
        "      p block margin=16/0/16/0\n");

    style_case("a block inside an inline: the bit the mixing scan reads",
        "<!doctype html><font face=arial><p>x</p></font><span><b>y</b></span>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    font inline font=\"arial\",sans holds-block\n"
        "      p block margin=16/0/16/0\n"
        "    span inline\n"
        "      b inline weight=700\n");

    style_case("foreign content: SVG makes no box, the HTML inside it does",
        "<!doctype html><p>a<svg><foreignObject><div>b</div></foreignObject></svg></p>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    p block margin=16/0/16/0 holds-block\n"
        "      svg contents holds-block\n"
        "        foreignObject contents holds-block\n"
        "          div block\n");

    style_case("forms: controls are inline-block, a form in a table is empty",
        "<!doctype html><table><form><tr><td><input><select></select><textarea></textarea>"
        "<button>b</button></table>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    table table spacing=2/2 holds-block\n"
        "      form none\n"
        "      tbody table-row-group valign=middle holds-block\n"
        "        tr table-row valign=middle holds-block\n"
        "          td table-cell padding=1/1/1/1 valign=middle\n"
        "            input inline-block\n"
        "            select inline-block\n"
        "            textarea inline-block\n"
        "            button inline-block\n");

    // ── Quirks mode ─────────────────────────────────────────────────────

    style_case("quirks: first and last paragraph margins in cells and body",
        "<p>first</p><table><tr><td><p>x</p></td></tr></table><form></form>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    p block margin=0/0/16/0\n"
        "    table table spacing=2/2 holds-block\n"
        "      tbody table-row-group valign=middle holds-block\n"
        "        tr table-row valign=middle holds-block\n"
        "          td table-cell padding=1/1/1/1 valign=middle holds-block\n"
        "            p block\n"
        "    form block margin=0/0/16/0\n");

    style_case("quirks: a table does not inherit the font, and takes the body's colour",
        "<body text=blue><font size=7 color=red><b><table><tr><td>x</table></b></font>",
        PAGE_OPEN
        "  body block color=#0000ff margin=8/8/8/8 holds-block\n"
        "    font inline size=48 color=#ff0000 holds-block\n"
        "      b inline weight=700 holds-block\n"
        "        table table weight=400 size=16 color=#0000ff spacing=2/2 holds-block\n"
        "          tbody table-row-group valign=middle holds-block\n"
        "            tr table-row valign=middle holds-block\n"
        "              td table-cell padding=1/1/1/1 valign=middle\n");
    style_case("no quirks: a table does inherit the font and the colour",
        "<!doctype html><font size=7 color=red><table><tr><td>x</table></font>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    font inline size=48 color=#ff0000 holds-block\n"
        "      table table spacing=2/2 holds-block\n"
        "        tbody table-row-group valign=middle holds-block\n"
        "          tr table-row valign=middle holds-block\n"
        "            td table-cell padding=1/1/1/1 valign=middle\n");

    style_case("quirks: an li outside any list puts its marker inside",
        "<li>x</li><ul><li>y</ul>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    li list-item list-inside\n"
        "    ul block margin=16/0/16/0 padding=0/0/0/40 holds-block\n"
        "      li list-item\n");

    style_case("quirks: an image floated by align keeps 3px",
        "<img align=left><img align=right hspace=0>",
        PAGE_OPEN
        "  body block margin=8/8/8/8\n"
        "    img inline margin=0/3/0/0 float=left\n"
        "    img inline float=right\n");

    style_case("quirks: nowrap with a pixel width wraps",
        "<table><tr><td nowrap width=100>a<td nowrap width=50%>b</table>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    table table spacing=2/2 holds-block\n"
        "      tbody table-row-group valign=middle holds-block\n"
        "        tr table-row valign=middle holds-block\n"
        "          td table-cell padding=1/1/1/1 width=100 valign=middle\n"
        "          td table-cell padding=1/1/1/1 width=50% valign=middle ws=nowrap\n");
}
