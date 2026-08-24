// conftest — the ring-3 fixture for the config library (2026-08-23).
//
// os64_conf_get / os64_conf_get_bool / os64_conf_write / os64_conf_set landed
// with nothing calling them, and an API nobody has run is a rumour. This
// fixture is the consumer that proves them — and it proves the three rules
// os64_conf_write promises, not merely its happy path:
//
//   1. it writes to the TOP of the search path, never back to what it read
//   2. it MERGES — comments and unrelated settings survive a save
//   3. it publishes atomically, leaving no .new debris behind
//
// It works under a name nothing else uses and removes the file on the way
// out, so a passing run leaves the machine exactly as it found it.
//
// Exit codes 0x0C0F00xx ("C0nF"; the step is the low byte):
//   0x0C0F0000  success
//   0x0C0F0001  set() failed
//   0x0C0F0002  get() did not read back what set() wrote
//   0x0C0F0003  the file did not land at the TOP of the ladder
//   0x0C0F0004  a hand-written comment did not survive the merge
//   0x0C0F0005  an unrelated setting did not survive the merge
//   0x0C0F0006  a multi-key write() lost a key
//   0x0C0F0007  case-insensitive key lookup failed
//   0x0C0F0008  get_bool mis-parsed a value it should understand
//   0x0C0F0009  get_bool ACCEPTED a value that is not a boolean
//   0x0C0F000A  a .new temporary was left behind

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "os64/os64.h"
#include "os64/conf.h"
#include "os64/str.h"

#define CONF_NAME "conftest.conf"
#define STEP(n)   (0x0C0F0000u | (uint32_t)(n))

static char gPath[OS64_CONF_PATH_MAX];

static void cleanup(void)
{
    if (gPath[0])
        os64_unlink(gPath);
}

static void die(uint32_t step, const char *why)
{
    os64_printf("conftest: %s\n", why);
    os64_serial_log(why);
    cleanup();
    os64_exit(STEP(step));
}

// Write a file by hand, so the merge has something with COMMENTS in it to
// preserve. (The whole point of rule 2 is that a machine-written config is
// still a file a human can edit, and the only way to test that is to edit it
// like a human first.)
static bool put_file(const char *path, const char *text)
{
    int64_t fd = os64_open(path, "w");
    if (fd < 0)
        return false;
    size_t len = os64_strlen(text), put = 0;
    while (put < len) {
        int64_t n = os64_write((int32_t)fd, text + put, len - put);
        if (n <= 0)
            break;
        put += (size_t)n;
    }
    os64_close((int32_t)fd);
    return put == len;
}

static bool file_contains(const char *path, const char *needle)
{
    char buf[2048];
    int64_t fd = os64_open(path, "r");
    if (fd < 0)
        return false;
    size_t got = 0;
    for (;;) {
        int64_t n = os64_read((int32_t)fd, buf + got, sizeof(buf) - 1 - got);
        if (n <= 0)
            break;
        got += (size_t)n;
        if (got >= sizeof(buf) - 1)
            break;
    }
    os64_close((int32_t)fd);
    buf[got] = '\0';

    size_t nl = os64_strlen(needle);
    if (nl == 0 || nl > got)
        return false;
    for (size_t i = 0; i + nl <= got; i++) {
        size_t j = 0;
        while (j < nl && buf[i + j] == needle[j])
            j++;
        if (j == nl)
            return true;
    }
    return false;
}

int main(void)
{
    char value[OS64_CONF_PATH_MAX];

    // ── get_bool, before anything touches the disk ──────────────────────────
    bool b = false;
    if (!os64_conf_get_bool("ON", &b) || !b)
        die(8, "conftest: get_bool did not accept ON as true");
    if (!os64_conf_get_bool("No", &b) || b)
        die(8, "conftest: get_bool did not accept No as false");
    if (!os64_conf_get_bool("TRUE", &b) || !b)
        die(8, "conftest: get_bool did not accept TRUE");
    // The THIRD answer, and the reason this returns a bool separate from the
    // value: a word that is not a boolean must be REFUSED, not silently
    // rounded down to false.
    b = true;
    if (os64_conf_get_bool("purple", &b))
        die(9, "conftest: get_bool accepted 'purple' as a boolean");
    if (!b)
        die(9, "conftest: get_bool clobbered *out on a value it rejected");

    // ── set(), and where it lands ──────────────────────────────────────────
    if (os64_conf_set(CONF_NAME, "alpha", "one") != 0)
        die(1, "conftest: os64_conf_set failed");

    // RULE 1: the file must be at the TOP of the ladder, which is also where
    // a subsequent find() picks it up.
    if (os64_conf_find(CONF_NAME, gPath, sizeof(gPath)) != 0)
        die(3, "conftest: the file it just wrote cannot be found");
    {
        char slot0[OS64_CONF_PATH_MAX];
        if (os64_conf_find_from(CONF_NAME, 0, slot0, sizeof(slot0)) < 1 ||
            !os64_streq(slot0, gPath))
            die(3, "conftest: the saved file is not at the top of the search path");
    }

    if (os64_conf_get(CONF_NAME, "alpha", value, sizeof(value)) != 0 ||
        !os64_streq(value, "one"))
        die(2, "conftest: get() did not read back what set() wrote");

    // ── the merge ──────────────────────────────────────────────────────────
    // Replace the file with something a HUMAN would have written: a comment,
    // a setting this test is about to change, and a setting it must not touch.
    if (!put_file(gPath,
                  "# conftest: this comment must survive a save\n"
                  "alpha = one\n"
                  "bystander = untouched\n"))
        die(4, "conftest: could not write the hand-made file");

    if (os64_conf_set(CONF_NAME, "alpha", "two") != 0)
        die(1, "conftest: the merging set() failed");

    if (!file_contains(gPath, "# conftest: this comment must survive a save"))
        die(4, "conftest: the merge ate a comment");
    if (!file_contains(gPath, "bystander = untouched"))
        die(5, "conftest: the merge ate an unrelated setting");
    if (os64_conf_get(CONF_NAME, "alpha", value, sizeof(value)) != 0 ||
        !os64_streq(value, "two"))
        die(2, "conftest: the merge did not update the key it was given");

    // RULE 3: nothing left beside it.
    {
        char temp[OS64_CONF_PATH_MAX];
        os64_strcopy(temp, sizeof(temp), gPath);
        size_t at = os64_strlen(temp);
        if (at + 5 < sizeof(temp)) {
            temp[at + 0] = '.'; temp[at + 1] = 'n'; temp[at + 2] = 'e';
            temp[at + 3] = 'w'; temp[at + 4] = '\0';
            int64_t leftover = os64_open(temp, "r");
            if (leftover >= 0) {
                os64_close((int32_t)leftover);
                die(10, "conftest: a .new temporary was left behind");
            }
        }
    }

    // ── a multi-key write, and case-insensitive lookup ──────────────────────
    {
        const os64_conf_pair_t pairs[] = {
            { "position", "280,10" },
            { "titlebar", "off" },
            { "pinned",   "true" },
        };
        if (os64_conf_write(CONF_NAME, pairs, 3) != 0)
            die(6, "conftest: the multi-key write failed");
    }
    if (os64_conf_get(CONF_NAME, "position", value, sizeof(value)) != 0 ||
        !os64_streq(value, "280,10"))
        die(6, "conftest: write() lost 'position'");
    if (os64_conf_get(CONF_NAME, "pinned", value, sizeof(value)) != 0 ||
        !os64_streq(value, "true"))
        die(6, "conftest: write() lost 'pinned'");
    if (!file_contains(gPath, "bystander = untouched"))
        die(5, "conftest: the multi-key write ate an unrelated setting");

    // THE gclock BUG, as a regression test. A file written `Titlebar` must be
    // readable as `titlebar`: a key reached through get() is a SETTING name,
    // and case is noise. (2026-08-23 — gclock.conf shipped capitalized keys
    // against lowercase compares and every setting in it was ignored.)
    if (!put_file(gPath, "# capitalized, on purpose\nTitlebar = off\n"))
        die(7, "conftest: could not write the capitalized file");
    if (os64_conf_get(CONF_NAME, "titlebar", value, sizeof(value)) != 0)
        die(7, "conftest: a capitalized key was not found by its lowercase name");
    if (!os64_conf_get_bool(value, &b) || b)
        die(8, "conftest: get_bool did not read the capitalized file's value");

    cleanup();
    os64_printf("conftest: all config-library checks passed\n");
    os64_serial_log("conftest: all config-library checks passed");
    os64_exit(STEP(0));
    return 0;   // not reached; os64_exit does not come back
}
