// garbdump — a style sheet as libgarb parses it (GARB.md, G1): the rules as
// JSON in css-parsing-tests' representation, and each style rule's block
// read as declarations. The guest's probe; the host harness is
// tools/test_garb_host.sh.
//
//   garbdump FILE.css

#include "garb/garb.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/slurp.h"
#include "os64/fmt.h"

static void print_dump(size_t (*dump)(const garb_item_t *, int32_t, char *, size_t),
                       const garb_item_t *items, int32_t n)
{
    size_t need = dump(items, n, NULL, 0) + 1;
    char *text = os64_malloc(need);
    if (text == NULL) {
        os64_printf("garbdump: no memory to print that\n");
        return;
    }
    dump(items, n, text, need);
    os64_printf("%s\n", text);
    os64_free(text);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        os64_printf("usage: garbdump FILE.css\n");
        return 2;
    }
    uint8_t *bytes = NULL;
    size_t len = 0;
    if (os64_slurp(argv[1], GARB_SHEET_MAX, &bytes, &len) != OS64_SLURP_OK) {
        os64_printf("garbdump: cannot read %s\n", argv[1]);
        return 1;
    }
    garb_parsed_t sheet;
    garb_status_t st = garb_parse_sheet(bytes, len, NULL, NULL, &sheet);
    os64_free(bytes);
    if (st != GARB_OK) {
        os64_printf("garbdump: %s\n", st == GARB_TOO_BIG ? "the sheet is too big" : "no memory");
        garb_free(&sheet);
        return 1;
    }
    os64_printf("encoding %s, %d rules%s\n", sheet.encoding, (int)sheet.nitems,
                sheet.incomplete ? " (incomplete)" : "");
    print_dump(garb_dump_items, sheet.items, sheet.nitems);
    for (int32_t i = 0; i < sheet.nitems; i++) {
        const garb_rule_t *r = sheet.items[i].rule;
        if (sheet.items[i].kind != GARB_ITEM_RULE || r->at || !r->has_block)
            continue;
        garb_item_t *items;
        int32_t n;
        (void)garb_items_of(&sheet, r->block, r->nblock, &items, &n);
        os64_printf("rule %d:\n", (int)i);
        print_dump(garb_dump_items, items, n);
    }
    garb_free(&sheet);
    return 0;
}
