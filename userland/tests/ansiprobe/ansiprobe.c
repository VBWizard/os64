// ansiprobe — paint every rendition the terminal claims to know, so that a
// regression is something you SEE rather than something you argue about.
//
// It is an acceptance probe, not a badge-code fixture: there is no exit code
// that can tell you a red is red. What it gives you is one screen holding
// every combination, in a fixed layout, which a screendump can be compared
// against — and which a person can look at and say "the bright yellow is
// missing" in less time than it takes to read a log.
//
//   ansiprobe          the palette, the attributes, and the cursor moves
//   ansiprobe paper    the same, on a terminal whose own background changed
//
// The `paper` argument is separate because it changes the WHOLE terminal,
// including the parts this program never wrote to — and a probe that did
// that by default would leave every later command sitting on it.

#include "os64/os64.h"
#include "os64/fmt.h"

static void esc(const char *seq)
{
    os64_printf("\033%s", seq);
}

int main(int argc, char **argv)
{
    bool paper = (argc > 1 && argv[1] != NULL && argv[1][0] == 'p');

    // `raw`: the same sequence through os64_write rather than printf, which
    // is the shape husk's prompt uses. Kept because the two paths differing
    // would be worth knowing about immediately.
    if (argc > 1 && argv[1] != NULL && argv[1][0] == 'r')
    {
        static const char raw[] = "[write] \033[31;44mZ> \033[0mafter\n";
        os64_write(1, raw, sizeof(raw) - 1);
        os64_printf("[printf] \033[31;44mZ> \033[0mafter\n");
        return 0;
    }

    esc("[2J");
    esc("[1;1H");

    if (paper)
    {
        // OSC 11: the terminal's own background. Everything the program has
        // NOT written sits on this, which is the whole point of it being a
        // different mechanism from SGR 40-47.
        os64_printf("\033]11;#101828\007");
        esc("[2J");
        esc("[1;1H");
    }

    // ASCII only, deliberately: this screen is READ as the verdict, and the
    // console draws bytes as Latin-1 glyphs, so a UTF-8 dash in a probe
    // would put mojibake in the evidence.
    os64_printf("ansiprobe - every rendition this terminal knows\n\n");

    // The sixteen foregrounds, named by the code that selects them.
    os64_printf("foreground  ");
    for (int i = 30; i <= 37; i++)
        os64_printf("\033[%dm %d \033[0m", i, i);
    os64_printf("\n            ");
    for (int i = 90; i <= 97; i++)
        os64_printf("\033[%dm %d \033[0m", i, i);
    os64_printf("\n\n");

    // The sixteen backgrounds. Black text on them, so a background that
    // silently did nothing shows up as an unreadable row rather than a
    // pretty one.
    os64_printf("background  ");
    for (int i = 40; i <= 47; i++)
        os64_printf("\033[30;%dm %d \033[0m", i, i);
    os64_printf("\n            ");
    for (int i = 100; i <= 107; i++)
        os64_printf("\033[30;%dm %d \033[0m", i, i);
    os64_printf("\n\n");

    // Attributes. Bold brightens (this font has one weight); reverse swaps.
    os64_printf("attributes  ");
    os64_printf("\033[31mplain\033[0m  ");
    os64_printf("\033[1;31mbold\033[0m  ");
    os64_printf("\033[7;31mreverse\033[0m  ");
    os64_printf("\033[1;7;31mboth\033[0m  ");
    os64_printf("\033[31;44mon blue\033[0m\n\n");

    // The moves. Written out of order on purpose: if CUP is wrong, the words
    // land in the wrong places and the sentence stops making sense, which is
    // easier to spot than a cursor one column off.
    uint32_t row = 14;
    esc("[14;13H");
    os64_printf("cursor      ");
    os64_printf("\033[%u;30Hthird\033[%u;13Hfirst\033[%u;21Hsecond",
                row + 1, row + 1, row + 1);

    // Erase to end of line, from a known column: everything past column 44
    // must vanish and nothing before it may. The line is written long enough
    // to reach well past the cut, or the test proves only that erasing
    // nothing erases nothing.
    os64_printf("\033[%u;13Hkeep this half |<- cut here ->| THIS MUST NOT SURVIVE",
                row + 3);
    os64_printf("\033[%u;44H\033[K", row + 3);

    esc("[19;1H");
    os64_printf("\033[32mdone.\033[0m %s\n",
                paper ? "(paper changed; 'clear' leaves it changed)" : "");
    return 0;
}
