// /bin/scribe. The editor itself is scribe_main, which /tests/scribefonttest
// runs too with hooks of its own; production passes none, so nothing a test
// needs is reachable from here.

#include "scribe.h"

int main(int argc, char **argv)
{
    return scribe_main(argc, argv, (const scribe_hooks_t *)0);
}
