// The attribute grammars, against values worked out from HTML's
// microsyntax algorithms by hand.

static void color_case(const char *in, bool ok, uint32_t rgb)
{
    uint32_t got = 0xDEADBE;
    bool parsed = f_parse_legacy_color(in, &got);
    char name[160];
    snprintf(name, sizeof(name), "legacy colour \"%s\"", in);
    char detail[64];
    snprintf(detail, sizeof(detail), "got %s #%06x", parsed ? "ok" : "failure", got);
    expect(name, parsed == ok && (!ok || got == rgb), detail);
}

static void dimension_case(const char *in, bool nonzero, f_dim_kind_t kind, int32_t value)
{
    int32_t got = -1;
    f_dim_kind_t k = f_parse_dimension(in, nonzero, &got);
    char name[160];
    snprintf(name, sizeof(name), "dimension \"%s\"%s", in, nonzero ? " (nonzero)" : "");
    char detail[64];
    snprintf(detail, sizeof(detail), "got kind %d value %d", (int)k, (int)got);
    expect(name, k == kind && (kind == F_DIM_NONE || got == value), detail);
}

static void attrs_cases(void)
{
    // Named first, then #rgb, then the long algorithm.
    color_case("red", true, 0xff0000);
    color_case("  Green ", true, 0x008000);
    color_case("REBECCAPURPLE", true, 0x663399);
    color_case("#abc", true, 0xaabbcc);
    color_case("#fff000", true, 0xfff000);
    color_case("  #FF0000\t", true, 0xff0000);
    color_case("", false, 0);
    color_case("   ", true, 0x000000);   // not empty before stripping: pads to 000
    color_case("transparent", false, 0);
    color_case("TransParent", false, 0);
    // The famous ones, each worked through steps 7-16.
    color_case("chucknorris", true, 0xc00000);   // c00c 0000 0000 -> c0 00 00
    color_case("sick", true, 0x00c000);          // 00c0 -> 00c000
    color_case("crap", true, 0xc0a000);          // c0a0 -> c0a000
    color_case("ninjaturtle", true, 0x00a000);   // 0000 a000 00e0 -> 00 a0 00
    color_case("abc", true, 0x0a0b0c);           // three one-digit components
    color_case("#12345", true, 0x123450);        // padded to 123450
    // Nine-digit components keep their last eight, then two digits each.
    color_case("#000000000111111111222222222", true, 0x001122);
    // Leading zeros go while ALL THREE components have one, down to two.
    color_case("#000100020003", true, 0x010203);
    color_case("#001002003", true, 0x010203);
    // A non-ASCII character in the BMP is ONE zero; one outside it is two.
    color_case("\xc3\xa9" "ff", true, 0x000f0f);             // 0ff: one digit each
    color_case("\xf0\x9f\x98\x80" "f", true, 0x00000f);      // 00f: one digit each

    dimension_case("50%", false, F_DIM_PERCENT, 50 * 64);
    dimension_case(" 12.5", false, F_DIM_PX, 800);
    dimension_case("12.5px", false, F_DIM_PX, 800);
    dimension_case("3.14159", false, F_DIM_PX, 201);        // .14159 * 64 = 9.06
    dimension_case("0", false, F_DIM_PX, 0);
    dimension_case("0", true, F_DIM_NONE, 0);
    dimension_case("0%", true, F_DIM_NONE, 0);
    dimension_case(".5", false, F_DIM_NONE, 0);
    dimension_case("-5", false, F_DIM_NONE, 0);
    dimension_case("7.", false, F_DIM_PX, 7 * 64);
    dimension_case("7.%", false, F_DIM_PX, 7 * 64);           // the fraction needs a digit
    dimension_case("", false, F_DIM_NONE, 0);

    int32_t v = 0;
    expect("integer: sign and junk", f_parse_integer(" -12abc", &v) && v == -12, NULL);
    expect("integer: plus", f_parse_integer("+7", &v) && v == 7, NULL);
    expect("integer: nothing", !f_parse_integer("x1", &v), NULL);
    expect("non-negative: refuses a negative", !f_parse_nonnegative("-1", &v), NULL);
    expect("non-negative: clamps the enormous",
           f_parse_nonnegative("99999999999999999999", &v) && v == F_INT_MAX, NULL);
    expect("nocase", f_eq_nocase("CeNtEr", "center") && !f_eq_nocase("cent", "center") &&
                         !f_eq_nocase(NULL, "x"), NULL);
}
