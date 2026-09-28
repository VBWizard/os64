// Tables: CSS 2.1 §17.5's automatic layout, each dump worked out by hand.
// Cells here have the sheet's 1px padding unless the table says
// otherwise, and the default 2px of border-spacing sits round and between
// every cell.

static void table_cases(void)
{
    // Cells' widths: a 8+2=10, bb 18, ccc 26, d 10. Columns 26 and 18,
    // the table 26+18 plus three 2px gaps = 50. Rows: 1+20+1 = 22.
    layout_case("tables: columns as wide as their widest cell",
        "<!doctype html><table><tr><td>a<td>bb</tr><tr><td>ccc<td>d</table>", 400,
        "page 400 66\n"
        "block html 0 0 400 66\n"
        "  block body 8 8 384 50\n"
        "    table table 8 8 50 50\n"
        "      row-group tbody 10 10 46 46\n"
        "        row tr 10 10 46 22\n"
        "          cell td 10 10 26 22\n"
        "            line 11 11 24 20 base 14\n"
        "              text \"a\" 11 13 8 16 serif 16\n"
        "          cell td 38 10 18 22\n"
        "            line 39 11 16 20 base 14\n"
        "              text \"bb\" 39 13 16 16 serif 16\n"
        "        row tr 10 34 46 22\n"
        "          cell td 10 34 26 22\n"
        "            line 11 35 24 20 base 14\n"
        "              text \"ccc\" 11 37 24 16 serif 16\n"
        "          cell td 38 34 18 22\n"
        "            line 39 35 16 20 base 14\n"
        "              text \"d\" 39 37 8 16 serif 16\n");

    // The first cell can be 34 (its widest word) and wants 70 (on one
    // line); the table's least is 58 and it has 44: it overflows at 58,
    // the cells at their least, and the short cell sits in the middle.
    layout_case("tables: narrower than its least, a table overflows",
        "<!doctype html><table><tr><td>aaaa bbbb<td>cc</table>", 60,
        "page 66 62\n"
        "block html 0 0 60 62\n"
        "  block body 8 8 44 46\n"
        "    table table 8 8 58 46\n"
        "      row-group tbody 10 10 54 42\n"
        "        row tr 10 10 54 42\n"
        "          cell td 10 10 34 42\n"
        "            line 11 11 32 20 base 14\n"
        "              text \"aaaa\" 11 13 32 16 serif 16\n"
        "            line 11 31 32 20 base 14\n"
        "              text \"bbbb\" 11 33 32 16 serif 16\n"
        "          cell td 46 10 18 42\n"
        "            line 47 21 16 20 base 14\n"
        "              text \"cc\" 47 23 16 16 serif 16\n");

    // 84 to spend: the table is 84, its grid 78; the auto columns go from
    // least (34, 18) toward most (70, 18) in proportion to the gap: the
    // first takes all 26 of it, 60.
    layout_case("tables: between least and most, in proportion",
        "<!doctype html><table><tr><td>aaaa bbbb<td>cc</table>", 100,
        "page 100 62\n"
        "block html 0 0 100 62\n"
        "  block body 8 8 84 46\n"
        "    table table 8 8 84 46\n"
        "      row-group tbody 10 10 80 42\n"
        "        row tr 10 10 80 42\n"
        "          cell td 10 10 60 42\n"
        "            line 11 11 58 20 base 14\n"
        "              text \"aaaa\" 11 13 32 16 serif 16\n"
        "            line 11 31 58 20 base 14\n"
        "              text \"bbbb\" 11 33 32 16 serif 16\n"
        "          cell td 72 10 18 42\n"
        "            line 73 21 16 20 base 14\n"
        "              text \"cc\" 73 23 16 16 serif 16\n");

    // border=1: the table's own 1px outset, each cell's 1px inset, no
    // spacing. The spanning cell needs 60+4 = 64 of two 12px columns: the
    // 40 missing split in proportion (evenly here), 32 each.
    layout_case("tables: a spanning cell spreads over its columns",
        "<!doctype html><table border=1 cellspacing=0><tr><td colspan=2>abcdefgh</td></tr>"
        "<tr><td>a<td>b</table>", 400,
        "page 400 66\n"
        "block html 0 0 400 66\n"
        "  block body 8 8 384 50\n"
        "    table table 8 8 66 50\n"
        "      row-group tbody 9 9 64 48\n"
        "        row tr 9 9 64 24\n"
        "          cell td 9 9 64 24\n"
        "            line 11 11 60 20 base 14\n"
        "              text \"abcdefgh\" 11 13 60 16 serif 16\n"
        "        row tr 9 33 64 24\n"
        "          cell td 9 33 32 24\n"
        "            line 11 35 28 20 base 14\n"
        "              text \"a\" 11 37 8 16 serif 16\n"
        "          cell td 41 33 32 24\n"
        "            line 43 35 28 20 base 14\n"
        "              text \"b\" 43 37 8 16 serif 16\n");

    // The spanning cell can be no narrower than 34 (aaaa and its padding):
    // its columns, 10 each on their own, take the 14 missing in proportion
    // (7 each), so the table's least is 34 — and at 24 to spend, 34 it is.
    layout_case("tables: a spanning cell's least widens its columns",
        "<!doctype html><table cellspacing=0><tr><td colspan=2>aaaa bbbb<tr><td>a<td>b</table>",
        40,
        "page 42 80\n"
        "block html 0 0 40 80\n"
        "  block body 8 8 24 64\n"
        "    table table 8 8 34 64\n"
        "      row-group tbody 8 8 34 64\n"
        "        row tr 8 8 34 42\n"
        "          cell td 8 8 34 42\n"
        "            line 9 9 32 20 base 14\n"
        "              text \"aaaa\" 9 11 32 16 serif 16\n"
        "            line 9 29 32 20 base 14\n"
        "              text \"bbbb\" 9 31 32 16 serif 16\n"
        "        row tr 8 50 34 22\n"
        "          cell td 8 50 17 22\n"
        "            line 9 51 15 20 base 14\n"
        "              text \"a\" 9 53 8 16 serif 16\n"
        "          cell td 25 50 17 22\n"
        "            line 26 51 15 20 base 14\n"
        "              text \"b\" 26 53 8 16 serif 16\n");

    // Quirks mode: a nowrap cell with a width in pixels wraps (pass 1) but
    // cannot be narrower than that width (the Quirks standard's 3.9).
    layout_case("tables: quirks, a nowrap cell's pixel width is its least",
        "<table cellspacing=0 cellpadding=0><tr><td nowrap width=100>aa bb</table>", 60,
        "page 108 36\n"
        "block html 0 0 60 36\n"
        "  block body 8 8 44 20\n"
        "    table table 8 8 100 20\n"
        "      row-group tbody 8 8 100 20\n"
        "        row tr 8 8 100 20\n"
        "          cell td 8 8 100 20\n"
        "            line 8 8 100 20 base 14\n"
        "              text \"aa bb\" 8 10 36 16 serif 16\n");

    // The rowspan cell needs 60; its two rows have 20 each, so each gets
    // 10 more. The one-row cells sit in the middle of their 30.
    layout_case("tables: a rowspan's height spread over its rows",
        "<!doctype html><table cellspacing=0 cellpadding=0><tr><td rowspan=2>a<br>b<br>c"
        "<td>x</tr><tr><td>y</table>", 400,
        "page 400 76\n"
        "block html 0 0 400 76\n"
        "  block body 8 8 384 60\n"
        "    table table 8 8 16 60\n"
        "      row-group tbody 8 8 16 60\n"
        "        row tr 8 8 16 30\n"
        "          cell td 8 8 8 60\n"
        "            line 8 8 8 20 base 14\n"
        "              text \"a\" 8 10 8 16 serif 16\n"
        "            line 8 28 8 20 base 14\n"
        "              text \"b\" 8 30 8 16 serif 16\n"
        "            line 8 48 8 20 base 14\n"
        "              text \"c\" 8 50 8 16 serif 16\n"
        "          cell td 16 8 8 30\n"
        "            line 16 13 8 20 base 14\n"
        "              text \"x\" 16 15 8 16 serif 16\n"
        "        row tr 8 38 16 30\n"
        "          cell td 16 38 8 30\n"
        "            line 16 43 8 20 base 14\n"
        "              text \"y\" 16 45 8 16 serif 16\n");

    // <center> aligns the 50px table inside its 184: (184-50)/2 = 67. The
    // cell inherits the centring for its text: (50-8)/2 = 21.
    layout_case("tables: <center> centres a table with a width",
        "<!doctype html><center><table width=50 cellspacing=0 cellpadding=0><tr><td>x"
        "</table></center>", 200,
        "page 200 36\n"
        "block html 0 0 200 36\n"
        "  block body 8 8 184 20\n"
        "    block center 8 8 184 20\n"
        "      table table 75 8 50 20\n"
        "        row-group tbody 75 8 50 20\n"
        "          row tr 75 8 50 20\n"
        "            cell td 75 8 50 20\n"
        "              line 75 8 50 20 base 14\n"
        "                text \"x\" 96 10 8 16 serif 16\n");

    // 200 to share: 50% is 100, the fixed column 40, and the auto column
    // takes the 60 left.
    layout_case("tables: a percentage, a fixed width and an auto column",
        "<!doctype html><table width=200 cellspacing=0 cellpadding=0><tr><td width=50%>a"
        "<td width=40>b<td>c</table>", 400,
        "page 400 36\n"
        "block html 0 0 400 36\n"
        "  block body 8 8 384 20\n"
        "    table table 8 8 200 20\n"
        "      row-group tbody 8 8 200 20\n"
        "        row tr 8 8 200 20\n"
        "          cell td 8 8 100 20\n"
        "            line 8 8 100 20 base 14\n"
        "              text \"a\" 8 10 8 16 serif 16\n"
        "          cell td 108 8 40 20\n"
        "            line 108 8 40 20 base 14\n"
        "              text \"b\" 108 10 8 16 serif 16\n"
        "          cell td 148 8 60 20\n"
        "            line 148 8 60 20 base 14\n"
        "              text \"c\" 148 10 8 16 serif 16\n");

    // A caption sits above the table's box, as wide as the table, its
    // text centred: (32-24)/2 = 4.
    layout_case("tables: a caption above the table",
        "<!doctype html><table cellspacing=0 cellpadding=0><caption>cap</caption>"
        "<tr><td>abcd</table>", 400,
        "page 400 56\n"
        "block html 0 0 400 56\n"
        "  block body 8 8 384 40\n"
        "    table table 8 28 32 20\n"
        "      caption caption 8 8 32 20\n"
        "        line 8 8 32 20 base 14\n"
        "          text \"cap\" 12 10 24 16 serif 16\n"
        "      row-group tbody 8 28 32 20\n"
        "        row tr 8 28 32 20\n"
        "          cell td 8 28 32 20\n"
        "            line 8 28 32 20 base 14\n"
        "              text \"abcd\" 8 30 32 16 serif 16\n");

    // valign=baseline: the 48px A's baseline is 42 down its line (strut
    // and text: -42 .. 18), the 16px b's 14, so b moves down 28.
    layout_case("tables: baseline cells share their row's baseline",
        "<!doctype html><table cellspacing=0 cellpadding=0><tr valign=baseline>"
        "<td><font size=7>A</font><td>b</table>", 400,
        "page 400 76\n"
        "block html 0 0 400 76\n"
        "  block body 8 8 384 60\n"
        "    table table 8 8 32 60\n"
        "      row-group tbody 8 8 32 60\n"
        "        row tr 8 8 32 60\n"
        "          cell td 8 8 24 60\n"
        "            line 8 8 24 60 base 42\n"
        "              span font 8 14 24 48\n"
        "              text \"A\" 8 14 24 48 serif 48\n"
        "          cell td 32 8 8 60\n"
        "            line 32 36 8 20 base 14\n"
        "              text \"b\" 32 38 8 16 serif 16\n");

    // Quirks mode: no wrap either side of a picture in an auto-width
    // cell (3.8), so both stay on one line and the table overflows.
    layout_case("tables: quirks keeps sliced images in a cell on one line",
        "<table cellspacing=0 cellpadding=0><tr><td><img src=known.png><img src=known.png>"
        "</table>", 60,
        "page 88 46\n"
        "block html 0 0 60 46\n"
        "  block body 8 8 44 30\n"
        "    table table 8 8 80 30\n"
        "      row-group tbody 8 8 80 30\n"
        "        row tr 8 8 80 30\n"
        "          cell td 8 8 80 30\n"
        "            line 8 8 80 30 base 30\n"
        "              atomic img 8 8 40 30\n"
        "              atomic img 48 8 40 30\n");
}

static FLayout *layout_raw(const char *html, int32_t width, os64_html_document_t **doc,
                           os64_page_t **page, FStyles **styles, FBoxes **boxes)
{
    *doc = parse(html);
    *page = os64_page_build(*doc, kPage, NULL);
    *styles = f_style_build(*doc, *page, &kEnv);
    *boxes = f_boxes_build(*doc, *page, *styles, &kEnv);
    return f_layout(*boxes, *doc, *page, &kEnv, width);
}

// LAYOUT.md § Bounds: a table nested forty deep costs each of its inline
// contexts a constant number of measurements (min/max are memoized per
// box), and a table whose every cell spans a thousand columns costs its
// cells, not the square of them.
static void table_bounds(void)
{
    size_t depth = 40;
    char *html = malloc(depth * 32 + 64);
    strcpy(html, "<!doctype html>");
    for (size_t i = 0; i < depth; i++)
        strcat(html, "<table><tr><td>");
    strcat(html, "deep");
    os64_html_document_t *doc;
    os64_page_t *page;
    FStyles *st;
    FBoxes *bx;
    FLayout *lay = layout_raw(html, 800, &doc, &page, &st, &bx);
    // One inline context, measured once for its widths and once to lay out.
    expect("bounds: forty nested tables, each context measured twice",
           lay != NULL && !lay->incomplete && lay->measures == 2, NULL);
    if (lay != NULL && lay->measures != 2)
        fprintf(stderr, "    measures %llu\n", (unsigned long long)lay->measures);
    f_layout_free(lay);
    f_boxes_free(bx);
    f_style_free(st);
    os64_page_free(page);
    os64_html_document_free(doc);
    free(html);

    size_t rows = 200;
    html = malloc(rows * 40 + 64);
    strcpy(html, "<!doctype html><table>");
    for (size_t i = 0; i < rows; i++)
        strcat(html, "<tr><td colspan=5000>x<td>y");
    lay = layout_raw(html, 800, &doc, &page, &st, &bx);
    expect("bounds: a table of spanning cells lays out, spans clamped",
           lay != NULL && !lay->incomplete && lay->measures == 2 * rows * 2, NULL);
    if (lay != NULL && lay->measures != 2 * rows * 2)
        fprintf(stderr, "    measures %llu\n", (unsigned long long)lay->measures);
    f_layout_free(lay);
    f_boxes_free(bx);
    f_style_free(st);
    os64_page_free(page);
    os64_html_document_free(doc);
    free(html);
}
