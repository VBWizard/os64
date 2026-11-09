// conftest — the ring-3 fixture for the config library (2026-08-23).
//
// os64_conf_get / os64_conf_get_bool / os64_conf_write / os64_conf_set landed
// with nothing calling them, and an API nobody has run is a rumour. This
// fixture is the consumer that proves them — and it proves the three rules
// os64_conf_write promises, not merely its happy path:
//
//   1. it writes to the TOP of the search path, never back to what it read
//   2. it MERGES — comments and unrelated settings survive a save
//   3. on ext2 it publishes atomically, leaving no .new debris behind
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
//   0x0C0F000B  get() cut an oversized value instead of refusing it
//   0x0C0F000C  write() accepted a setting the dialect cannot read back

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

// Is ANY temporary left beside the config file? (Codex #29 rd9.)
//
// This used to probe the single literal name "<path>.new", which stopped being
// what the writer creates the moment the temp gained a per-saver tag — so the
// "no debris" check silently became a check of a name nobody writes, and would
// have passed while every save leaked. The cure is to stop guessing the
// scheme: enumerate the DIRECTORY and fail on anything that looks like our
// file plus a suffix ending in ".new". That holds for "<path>.new", for
// "<path>.<taskid>.<seq>.new", and for whatever the temp is called next —
// a fixture that has to be edited whenever the code changes is a fixture that
// will one day not be.
static bool temp_debris_beside(const char *path)
{
    // Split the config path into its directory and its file name. Config
    // paths arrive absolute (the kernel builds them from the ladder), but a
    // fixture that assumes its input is a fixture with a hole in it.
    size_t at = os64_strlen(path);
    bool   has_slash = false;
    size_t cut = 0;
    for (size_t i = 0; i < at; i++)
        if (path[i] == '/') { cut = i; has_slash = true; }   // last '/' wins

    char dir[OS64_CONF_PATH_MAX];
    if (!has_slash) {
        dir[0] = '.'; dir[1] = '\0';          // a bare name: look where we are
    } else if (cut == 0) {
        dir[0] = '/'; dir[1] = '\0';          // "/name": the directory is root
    } else {
        if (cut >= sizeof(dir))
            return false;
        for (size_t i = 0; i < cut; i++)
            dir[i] = path[i];
        dir[cut] = '\0';
    }
    const char *base = has_slash ? &path[cut + 1] : path;
    size_t bl = os64_strlen(base);
    if (bl == 0)
        return false;                         // a path ending in '/': no file to guard

    int64_t d = os64_opendir(dir);
    if (d < 0)
        return false;                         // cannot look: report no debris

    bool found = false;
    os64_dirent_t e;
    while (os64_readdir((int32_t)d, &e) == 1) {
        size_t nl = os64_strlen(e.name);
        // "<base>." prefix, ".new" suffix, and longer than the base itself —
        // which is every temp shape the writer has ever used, and excludes
        // the config file itself.
        if (nl <= bl + 4)
            continue;
        size_t j = 0;
        while (j < bl && e.name[j] == base[j])
            j++;
        if (j != bl || e.name[bl] != '.')
            continue;
        if (e.name[nl - 4] == '.' && e.name[nl - 3] == 'n' &&
            e.name[nl - 2] == 'e' && e.name[nl - 1] == 'w') {
            found = true;
            break;
        }
    }
    os64_close((int32_t)d);
    return found;
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

    // RULE 3: nothing left beside it — ANY temporary, whatever it is called
    // (see temp_debris_beside; the old literal "<path>.new" probe went stale
    // the moment the temp gained a per-saver tag, and a stale probe passes).
    //
    // THE DETECTOR PROVES ITSELF FIRST. That is the actual lesson of the stale
    // probe: an assertion nobody has ever seen FAIL is not evidence, it is a
    // hope. So plant a temporary by hand, insist it is seen, remove it, and
    // insist it is gone — and only then trust the real check below.
    {
        char planted[OS64_CONF_PATH_MAX];
        if (os64_snprintf(planted, sizeof(planted), "%s.999999.0.new", gPath) > 0) {
            if (!put_file(planted, "debris\n"))
                die(10, "conftest: could not plant a temporary to test the detector");
            if (!temp_debris_beside(gPath))
                die(10, "conftest: the debris detector cannot see a temporary");
            os64_unlink(planted);
            if (temp_debris_beside(gPath))
                die(10, "conftest: the debris detector reports a temporary that is gone");
        }
    }
    if (temp_debris_beside(gPath))
        die(10, "conftest: a temporary was left behind beside the config file");

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

    // ── a value too long for the caller's buffer is REFUSED ────────────────
    // os64_strcopy answers with the length the source WANTED, and get() used
    // to discard that answer — so a value that did not fit came back cut,
    // with a success code, and nothing could tell. Half of a coordinate is a
    // valid-looking coordinate pointing somewhere else. (2026-08-24.)
    if (!put_file(gPath, "long = 0123456789abcdef\n"))
        die(11, "conftest: could not write the long-value file");
    {
        char small[8];               // 7 chars + NUL: the value needs 16
        int64_t rc = os64_conf_get(CONF_NAME, "long", small, sizeof(small));
        if (rc != OS64_CONF_TRUNCATED)
            die(11, "conftest: an oversized value was not reported as truncated");
        if (small[0] != '\0')
            die(11, "conftest: a truncated get() left a partial value behind");
    }
    // ...and the same value read into a buffer that FITS is an ordinary
    // success, so the check above is not just refusing everything.
    if (os64_conf_get(CONF_NAME, "long", value, sizeof(value)) != 0 ||
        !os64_streq(value, "0123456789abcdef"))
        die(11, "conftest: a value that fits was not read back whole");

    // ── a setting the dialect cannot represent is REFUSED, WHOLE ───────────
    // The writer used to treat caller data as file syntax: a '\n' in a value
    // wrote a SECOND setting line (data promoted to syntax — husk's ruling,
    // one subsystem over), and a '#' was eaten on the way back in. Each of
    // these must be refused before anything is opened, and must leave the
    // file exactly as it was. (2026-08-24.)
    if (!put_file(gPath, "keep = me\n"))
        die(12, "conftest: could not write the pre-injection file");
    {
        // The injection itself: a value carrying a whole extra setting.
        if (os64_conf_set(CONF_NAME, "name", "bob\nadmin = yes") != OS64_CONF_BAD_SETTING)
            die(12, "conftest: a newline in a value was not refused");
        if (file_contains(gPath, "admin = yes"))
            die(12, "conftest: a refused write forged a setting anyway");

        // A '#' would round-trip to an empty value — a silent loss.
        if (os64_conf_set(CONF_NAME, "color", "#ff0000") != OS64_CONF_BAD_SETTING)
            die(12, "conftest: a '#' in a value was not refused");

        // A key that could never match itself again on the next save.
        if (os64_conf_set(CONF_NAME, "two words", "x") != OS64_CONF_BAD_SETTING)
            die(12, "conftest: a key with whitespace was not refused");
        if (os64_conf_set(CONF_NAME, "", "x") != OS64_CONF_BAD_SETTING)
            die(12, "conftest: an empty key was not refused");

        // WHOLE-refusal: one bad pair in a batch writes NONE of them.
        {
            const os64_conf_pair_t mixed[] = {
                { "good",  "fine" },
                { "evil",  "x\nsneaked = in" },
            };
            if (os64_conf_write(CONF_NAME, mixed, 2) != OS64_CONF_BAD_SETTING)
                die(12, "conftest: a batch with one bad pair was not refused");
            if (os64_conf_get(CONF_NAME, "good", value, sizeof(value)) != OS64_CONF_NO_KEY)
                die(12, "conftest: a refused batch wrote one of its settings anyway");
        }

        // Nothing above touched the file, and an EMPTY value is still legal.
        if (!file_contains(gPath, "keep = me"))
            die(12, "conftest: a refused write disturbed the existing file");
        if (os64_conf_set(CONF_NAME, "blank", "") != 0)
            die(12, "conftest: an empty value was refused, and it is legal");
    }

    cleanup();
    os64_printf("conftest: all config-library checks passed\n");
    os64_serial_log("conftest: all config-library checks passed");
    os64_exit(STEP(0));
    return 0;   // not reached; os64_exit does not come back
}
