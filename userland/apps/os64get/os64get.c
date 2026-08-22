// os64get.c — fetch a file over the wire, put it where it belongs, keep a copy,
// and refuse to install a bad one.
//
// THE MISSION, in one line: `os64 refresh` without a thumb drive. Until
// 2026-08-16 the only way a fresh build reached the P5 was a human carrying a
// USB stick across a room; since 2026-08-21 the P5 boots from its own disk
// and this program is the whole supply line — kernel, menu, bootloader, apps.
//
// The protocol is RTL8125.md's, deliberately 1971-shaped — one TCP
// connection per file, ASCII where a human might read it, binary only where
// a machine must:
//
//     client -> server:   GET <name>\n
//     server -> client:   OK <length-decimal> <crc32-hex8>\n  then <length> bytes
//                    or:  NO <reason>\n
//
// You can drive it by hand from any telnet client, which is not nostalgia:
// a protocol you can type is a protocol you can debug at 1am on a machine
// with no tooling.
//
// ── WHERE A FILE GOES (2026-08-22) ──────────────────────────────────────
//
// The valet serves NAMES, one path component each; where a name lands on
// this machine is THIS machine's business, and it is written in
// /etc/os64get.conf (or /home/os64get.conf, which wins — the same ladder as
// logd.conf and husk.rc): exact names, `*.suffix` patterns, and a `*`
// default, each mapped to a directory. The kernel goes to /fat/boot where
// Limine reads it, programs go to /bin, and nobody has to remember which is
// which at the prompt. An explicit DEST on the command line beats the file;
// no file and no DEST means the current directory, as it always did, so the
// refresh that delivers a new os64get works before the one that delivers
// its conf. Lineage: rdist(1), 4.3BSD 1985 — the distfile names the
// destinations and the tool obeys.
//
// ── THE ARCHIVE (same day) ──────────────────────────────────────────────
//
// Before a file is installed it is KEPT, at
//
//     <archive>/YYYY-MM-DD/HHMMSS/<name>          (UTC, the clock's tongue)
//
// and the install is made FROM that copy. Chris was doing this by hand —
// every refresh, a copy to a dated folder, then a move — which is the
// surest sign a program should. The archive is what lets a bad build be
// walked back from a prompt, and every os64get run in the same second
// lands in one folder, so a scripted refresh reads as one install. The
// copy is removed if the install fails: the archive records what LANDED.
// (Plan 9's dump kept every day's tree under /n/dump/YYYY/MMDD; the
// instinct is the same, and so is the reason it lives on /home — the root
// partition is rewritten by every refresh and would torch the history.)
//
// ── THE PART THAT MATTERS: PUBLISHING ───────────────────────────────────
//
// Every file this program creates is written as `<target>.part` BESIDE its
// target and renamed into place only after its length and CRC32 check out.
// That ordering is the entire safety property, and it is why a rename
// syscall got built the morning the driver did:
//
//   - a transfer that dies halfway leaves a .part file and the PREVIOUS
//     version of the real name completely untouched;
//   - a transfer that completes but arrives damaged is DISCARDED, not
//     installed, because "complete" and "correct" are different claims and
//     only the checksum can tell them apart;
//   - the swap itself is atomic — os64's rename replaces the destination
//     with no instant at which the name fails to resolve, which is what
//     4.2BSD invented rename(2) for in 1983.
//
// BESIDE is load-bearing: rename works within ONE filesystem (the kernel
// refuses a cross-mount rename, correctly), and /home, /fat and / are three.
// So the archive copy gets its own .part next to it, the install gets its
// own .part next to IT, and the filesystem boundary is crossed by a plain
// copy in between — never by a rename. The install copy is checksummed as
// it is written, too: "refuse to install a bad one" covers the disk, not
// just the wire.
//
// That last property is what makes this safe to point at /bin. A refresh
// that replaces a program someone is running works: the running copy keeps
// demand-paging the inode it already holds (the ext2 orphan chain), and the
// new binary is there at the next launch.
//
// Exit codes name the step that failed, per the house fixture convention —
// a shell script driving a dozen of these should not have to parse English.

#include "os64/os64.h"
#include "os64/crc32.h"
#include "os64/conf.h"
#include "os64/date.h"

#define GET_OK             0
#define GET_USAGE          2
#define GET_DIAL_FAILED    3
#define GET_REQUEST_FAILED 4
#define GET_REFUSED        5   // the server said NO
#define GET_BAD_HEADER     6   // the server said something we don't speak
#define GET_SHORT          7   // connection died mid-file
#define GET_CORRUPT        8   // arrived complete and WRONG — the CRC caught it
#define GET_WRITE_FAILED   9
#define GET_PUBLISH_FAILED 10  // downloaded fine, could not put it in place
#define GET_ARCHIVE_FAILED 11  // could not make or keep the archive copy

#define GET_PORT      6464
#define GET_CHUNK     4096
#define GET_PATH_MAX  256

// ── The config ──────────────────────────────────────────────────────────
// A small fixed table, because the whole install map is six lines and a
// utility that mallocs to read its own config has its priorities wrong.
// Precedence is by SPECIFICITY (exact beats suffix beats `*`), and within a
// class the LAST match in the file wins, so a /home copy can override by
// repeating a line.
#define CONF_RULES_MAX 16
#define CONF_DIR_MAX   128

typedef struct {
    char name[OS64_DIRENT_NAME_MAX + 1];   // "os64_kernel", or ".so" for a suffix rule
    char dir[CONF_DIR_MAX];
} conf_rule_t;

typedef struct {
    conf_rule_t exact[CONF_RULES_MAX];  size_t nexact;
    conf_rule_t suffix[CONF_RULES_MAX]; size_t nsuffix;
    char star[CONF_DIR_MAX];            // the `*` rule; empty = none
    char archive[CONF_DIR_MAX];         // the `archive` key; empty = don't
    const char *path;                   // which file answered (for complaints)
    bool anyRule;                       // did the file route anything at all?
} conf_t;

static const char *const kConfPaths[] = { "/home/os64get.conf", "/etc/os64get.conf" };

// Trim a trailing '/' off a directory value so "/bin/" and "/bin" mean the
// same thing — the one courtesy the reader extends beyond the dialect.
static void conf_take_dir(char *dst, const char *value)
{
    size_t n = os64_strcopy(dst, CONF_DIR_MAX, value);
    while (n > 1 && dst[n - 1] == '/')
        dst[--n] = '\0';
}

static bool conf_line(const char *key, const char *value, void *user)
{
    conf_t *c = (conf_t *)user;

    if (key == NULL) {
        // Not `key = value`. Say so and carry on: one bad line must not
        // silence the five good ones, and silence is how a setting "does
        // nothing" for an afternoon.
        os64_hprintf(OS64_STDERR, "os64get: %s: expected 'key = value' - ignored: %s\n",
                     c->path, value);
        return true;
    }

    if (os64_streq(key, "archive")) {
        conf_take_dir(c->archive, value);
        return true;
    }
    if (os64_streq(key, "*")) {
        conf_take_dir(c->star, value);
        c->anyRule = true;
        return true;
    }
    if (key[0] == '*' && key[1] == '.') {
        // A suffix rule: stored as ".so", matched against the name's tail.
        // Later lines append; the matcher walks the table backwards so the
        // last one wins.
        if (c->nsuffix < CONF_RULES_MAX) {
            os64_strcopy(c->suffix[c->nsuffix].name, sizeof(c->suffix[0].name), key + 1);
            conf_take_dir(c->suffix[c->nsuffix].dir, value);
            c->nsuffix++;
            c->anyRule = true;
        } else {
            os64_hprintf(OS64_STDERR, "os64get: %s: too many '*.' rules (limit %d) - ignored: %s\n",
                         c->path, CONF_RULES_MAX, key);
        }
        return true;
    }
    if (key[0] == '*' || os64_strlen(key) > OS64_DIRENT_NAME_MAX) {
        os64_hprintf(OS64_STDERR, "os64get: %s: not a name, '*.suffix' or '*' - ignored: %s\n",
                     c->path, key);
        return true;
    }

    if (c->nexact < CONF_RULES_MAX) {
        os64_strcopy(c->exact[c->nexact].name, sizeof(c->exact[0].name), key);
        conf_take_dir(c->exact[c->nexact].dir, value);
        c->nexact++;
        c->anyRule = true;
    } else {
        os64_hprintf(OS64_STDERR, "os64get: %s: too many name rules (limit %d) - ignored: %s\n",
                     c->path, CONF_RULES_MAX, key);
    }
    return true;
}

static void conf_load(conf_t *c)
{
    size_t which = 0;
    // The callback needs c->path for its complaints before we know which
    // file answered; try each in order ourselves so the name is right.
    for (size_t i = 0; i < sizeof(kConfPaths) / sizeof(kConfPaths[0]); i++) {
        c->path = kConfPaths[i];
        int64_t rc = os64_conf_read(kConfPaths[i], conf_line, c);
        if (rc == OS64_CONF_NO_FILE)
            continue;
        if (rc == OS64_CONF_TRUNCATED)
            os64_hprintf(OS64_STDERR, "os64get: %s is larger than %d bytes - the tail was not read\n",
                         kConfPaths[i], OS64_CONF_MAX);
        which = i;
        (void)which;
        return;
    }
    c->path = NULL;   // no file: cwd semantics, no archive
}

// The directory a name installs into, or NULL for "no rule" (= cwd).
static const char *conf_route(const conf_t *c, const char *name)
{
    for (size_t i = c->nexact; i > 0; i--)
        if (os64_streq(c->exact[i - 1].name, name))
            return c->exact[i - 1].dir;

    size_t nlen = os64_strlen(name);
    for (size_t i = c->nsuffix; i > 0; i--) {
        const char *suf = c->suffix[i - 1].name;
        size_t slen = os64_strlen(suf);
        if (nlen > slen && os64_streq(name + (nlen - slen), suf))
            return c->suffix[i - 1].dir;
    }

    return c->star[0] ? c->star : NULL;
}

// ── Small file verbs ────────────────────────────────────────────────────

// mkdir -p, the honest way: stat each prefix, make what is missing.
// os64_mkdir refuses an existing directory, which is right for mkdir and
// wrong for "make sure this path exists", so the stat comes first.
static bool ensure_dir(const char *path)
{
    char walk[GET_PATH_MAX];
    size_t n = os64_strcopy(walk, sizeof(walk), path);
    if (n == 0 || n >= sizeof(walk) - 1)
        return false;

    for (size_t i = 1; i <= n; i++) {
        if (walk[i] != '/' && walk[i] != '\0')
            continue;
        char saved = walk[i];
        walk[i] = '\0';
        os64_dirent_t e;
        if (os64_stat(walk, &e) < 0) {
            if (os64_mkdir(walk) < 0)
                return false;
        } else if (!(e.flags & OS64_DE_DIR)) {
            return false;   // a FILE where a directory is needed
        }
        walk[i] = saved;
    }
    return true;
}

// Build "<dir>/<name>" (or just name when dir is NULL). False if it won't fit.
static bool join_path(char *out, size_t cap, const char *dir, const char *name)
{
    int32_t n = dir ? os64_snprintf(out, cap, "%s/%s", dir, name)
                    : os64_snprintf(out, cap, "%s", name);
    return n > 0 && (size_t)n < cap;
}

// Copy `src` to `dst`, checksumming as it goes; `expect` bytes, `crc` must
// match at the end. Returns 0 or the GET_ code that names the failure. The
// destination is opened fresh ("w"), written, and synced — it is a .part
// file, so a failure here leaves the real name untouched.
static int copy_verified(const char *src, const char *dst, uint64_t expect, uint32_t want)
{
    int64_t in = os64_open(src, "r");
    if (in < 0)
        return GET_ARCHIVE_FAILED;
    int64_t out = os64_open(dst, "w");
    if (out < 0) {
        os64_close((int32_t)in);
        return GET_WRITE_FAILED;
    }

    static uint8_t buf[GET_CHUNK];
    uint64_t total = 0;
    uint32_t crc = os64_crc32_begin();
    int rc = GET_OK;
    for (;;) {
        int64_t n = os64_read((int32_t)in, buf, sizeof(buf));
        if (n < 0) { rc = GET_ARCHIVE_FAILED; break; }
        if (n == 0) break;
        if (os64_write((int32_t)out, buf, (size_t)n) != n) { rc = GET_WRITE_FAILED; break; }
        crc = os64_crc32_update(crc, buf, (size_t)n);
        total += (uint64_t)n;
    }
    if (rc == GET_OK && os64_sync((int32_t)out) < 0)
        rc = GET_WRITE_FAILED;
    os64_close((int32_t)in);
    os64_close((int32_t)out);

    if (rc == GET_OK && (total != expect || os64_crc32_end(crc) != want))
        rc = GET_CORRUPT;   // the disk lied between the archive and here
    return rc;
}

// ── Parsing the header ──────────────────────────────────────────────────

// Parse a decimal from [s, end). Returns false on empty, on a non-digit, or
// on a value too large for 64 bits — refusal rather than a guess, because a
// malformed length is exactly the case where guessing writes a file of the
// wrong size. The overflow arm is not pedantry: "18446744073709551616" wraps
// to 0, and a zero length paired with the empty-file CRC would let a hostile
// or broken server publish an EMPTY file over a good one and call it verified.
static bool parse_u64(const char *s, const char *end, uint64_t *out)
{
    if (s >= end)
        return false;
    uint64_t v = 0;
    while (s < end)
    {
        if (*s < '0' || *s > '9')
            return false;
        uint64_t digit = (uint64_t)(*s - '0');
        // Ask BEFORE multiplying — once it has wrapped there is nothing left
        // to detect. (UINT64_MAX - digit) / 10 is the largest value that can
        // still absorb one more digit.
        if (v > (UINT64_MAX - digit) / 10)
            return false;
        v = v * 10 + digit;
        s++;
    }
    *out = v;
    return true;
}

// Parse exactly 8 hex digits (the CRC). Same strictness, same reason.
static bool parse_hex32(const char *s, const char *end, uint32_t *out)
{
    if (end - s != 8)
        return false;
    uint32_t v = 0;
    while (s < end)
    {
        char c = *s++;
        uint32_t d;
        if (c >= '0' && c <= '9')      d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else return false;
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

int main(int argc, char **argv)
{
    os64_args_t args = {0};
    const char *operands[4] = {0};
    bool quiet = false;
    bool noArchive = false;
    const os64_optspec_t specs[] = {
        {'q', "quiet", false, "no progress, just the exit code", .flag = &quiet},
        {'n', "no-archive", false, "install only; keep no archive copy", .flag = &noArchive},
    };

    os64_args_init(&args, argc, argv, specs, 2);
    args.about = "Fetch a file over the network, archive it, and install it only if it is intact.";
    args.details = "DEST defaults to the directory /etc/os64get.conf names for NAME (or the cwd). "
                   "Keeps <archive>/DATE/TIME/NAME first, then installs from that copy via DEST.part + rename.";

    int32_t count = os64_args_parse(&args, "os64get [-q] [-n] HOST NAME [DEST]", operands, 4);
    if (count == OS64_ARG_HELP)
        return GET_OK;
    if (count < 2)
    {
        if (count != OS64_ARG_ERROR)
            os64_hprintf(OS64_STDERR, "os64get: need a HOST and a NAME\n");
        return GET_USAGE;
    }

    const char *host = operands[0];
    const char *name = operands[1];

    // ── Where it goes ───────────────────────────────────────────────────
    static conf_t conf;   // static: two 16-entry tables are too fat for the stack
    conf_load(&conf);

    char dest[GET_PATH_MAX];
    if (count >= 3)
    {
        // The command line's word is final.
        if (!join_path(dest, sizeof(dest), NULL, operands[2]))
        {
            os64_hprintf(OS64_STDERR, "os64get: destination path too long\n");
            return GET_USAGE;
        }
    }
    else
    {
        const char *dir = conf_route(&conf, name);
        if (!join_path(dest, sizeof(dest), dir, name))
        {
            os64_hprintf(OS64_STDERR, "os64get: destination path too long\n");
            return GET_USAGE;
        }
        if (dir == NULL && conf.path != NULL && conf.anyRule)
            // A conf exists and routes things, just not THIS thing — say
            // so, because "it went to the cwd" is a surprise worth a line.
            os64_hprintf(OS64_STDERR, "os64get: %s has no rule for '%s'; installing in the current directory\n",
                         conf.path, name);
    }

    // ── Where it is kept ────────────────────────────────────────────────
    // The archive path is decided NOW, before the wire, so one run's files
    // share a second — and so a missing /home fails loudly before a single
    // byte is fetched rather than after the last one.
    char archiveFile[GET_PATH_MAX];
    bool archiving = !noArchive && conf.archive[0] != '\0';
    if (archiving)
    {
        os64_time_t now;
        os64_date_t d;
        if (os64_time(&now) < 0)
        {
            os64_hprintf(OS64_STDERR, "os64get: cannot read the clock for the archive path\n");
            return GET_ARCHIVE_FAILED;
        }
        os64_date_from_epoch(now.epoch, &d);   // UTC: the system clock's tongue (the clock ruling)

        char archiveDir[GET_PATH_MAX];
        int32_t n = os64_snprintf(archiveDir, sizeof(archiveDir), "%s/%04d-%02d-%02d/%02d%02d%02d",
                                  conf.archive, d.year, d.month, d.day, d.hour, d.minute, d.second);
        if (n <= 0 || (size_t)n >= sizeof(archiveDir) ||
            !join_path(archiveFile, sizeof(archiveFile), archiveDir, name))
        {
            os64_hprintf(OS64_STDERR, "os64get: archive path too long\n");
            return GET_ARCHIVE_FAILED;
        }
        if (!ensure_dir(archiveDir))
        {
            os64_hprintf(OS64_STDERR, "os64get: cannot create archive directory %s "
                         "(is %s mounted? pass -n to install without archiving)\n",
                         archiveDir, conf.archive);
            return GET_ARCHIVE_FAILED;
        }
    }

    // The file the wire is written into. With an archive, that is the
    // archive copy; without one, the destination itself. Either way it is
    // a .part beside its final name.
    const char *wireTarget = archiving ? archiveFile : dest;
    char partPath[GET_PATH_MAX];
    int32_t pathlen = os64_snprintf(partPath, sizeof(partPath), "%s.part", wireTarget);
    if (pathlen < 0 || (size_t)pathlen >= sizeof(partPath))
    {
        os64_hprintf(OS64_STDERR, "os64get: path is too long to append '.part'\n");
        return GET_WRITE_FAILED;
    }

    // ── Dial ────────────────────────────────────────────────────────────
    // Plan 9's bang path, which libos64 parses into an os64_netdest_t below
    // the syscall boundary (nothing textual crosses it).
    char dialstring[GET_PATH_MAX];
    os64_snprintf(dialstring, sizeof(dialstring), "tcp!%s!%d", host, GET_PORT);

    int64_t conn = os64_dial(dialstring);
    if (conn < 0)
    {
        // The dial error codes are specific on purpose (os64/net.h) — a
        // refusal and a timeout mean very different things to whoever is
        // standing at the other machine, and printing "failed" for both
        // wastes their next ten minutes.
        const char *why =
            (conn == OS64_NET_ERR_REFUSED)      ? "connection refused — is the server running?" :
            (conn == OS64_NET_ERR_TIMEOUT)      ? "timed out — is the host reachable?" :
            (conn == OS64_NET_ERR_NO_NIC)       ? "no network interface on this boot" :
            (conn == OS64_NET_ERR_BAD_ADDRESS)  ? "that host is not a dotted quad" :
            (conn == OS64_NET_ERR_NO_RESOURCES) ? "out of handles or ports" :
                                                  "refused";
        os64_hprintf(OS64_STDERR, "os64get: cannot reach %s:%d — %s\n", host, GET_PORT, why);
        return GET_DIAL_FAILED;
    }

    // ── Ask ─────────────────────────────────────────────────────────────
    char request[GET_PATH_MAX];
    int32_t reqlen = os64_snprintf(request, sizeof(request), "GET %s\n", name);
    if (reqlen < 0 || (size_t)reqlen >= sizeof(request))
    {
        os64_hprintf(OS64_STDERR,
                     "os64get: could not create request, file name too long\n");
        os64_close((int32_t)conn);
        return GET_REQUEST_FAILED;
    }

    if (os64_write((int32_t)conn, request, (size_t)reqlen) != reqlen)
    {
        os64_hprintf(OS64_STDERR, "os64get: could not send the request\n");
        os64_close((int32_t)conn);
        return GET_REQUEST_FAILED;
    }

    // ── The header ──────────────────────────────────────────────────────
    // Read one byte at a time to the newline. Wasteful in principle and
    // exactly right here: the header is short, and reading in chunks would
    // swallow the front of the FILE into a header buffer we would then have
    // to hand back. A stream has no message boundaries — that is the whole
    // difference between TCP and UDP — so the only safe way to stop exactly
    // at the newline is to not read past it.
    char header[160];
    size_t hlen = 0;
    for (;;)
    {
        char c;
        int64_t n = os64_read((int32_t)conn, &c, 1);
        if (n != 1)
        {
            os64_hprintf(OS64_STDERR, "os64get: server hung up before answering\n");
            os64_close((int32_t)conn);
            return GET_BAD_HEADER;
        }
        if (c == '\n')
            break;
        if (c == '\r')
            continue;                     // a server written on Windows is still a server
        if (hlen + 1 >= sizeof(header))
        {
            os64_hprintf(OS64_STDERR, "os64get: header too long — is that an os64get server?\n");
            os64_close((int32_t)conn);
            return GET_BAD_HEADER;
        }
        header[hlen++] = c;
    }
    header[hlen] = '\0';

    if (header[0] == 'N' && header[1] == 'O')
    {
        // The server's own words, verbatim. It knows why and we do not.
        os64_hprintf(OS64_STDERR, "os64get: server refused '%s':%s\n",
                     name, header[2] ? header + 2 : " (no reason given)");
        os64_close((int32_t)conn);
        return GET_REFUSED;
    }
    if (!(header[0] == 'O' && header[1] == 'K' && header[2] == ' '))
    {
        os64_hprintf(OS64_STDERR, "os64get: unexpected reply '%s'\n", header);
        os64_close((int32_t)conn);
        return GET_BAD_HEADER;
    }

    // "OK <length> <crc>" — split on the single space between the fields.
    const char *lenStart = header + 3;
    const char *p = lenStart;
    while (*p && *p != ' ')
        p++;
    uint64_t expectLen = 0;
    uint32_t expectCrc = 0;
    if (*p != ' ' ||
        !parse_u64(lenStart, p, &expectLen) ||
        !parse_hex32(p + 1, p + 1 + os64_strlen(p + 1), &expectCrc))
    {
        os64_hprintf(OS64_STDERR, "os64get: malformed OK line '%s'\n", header);
        os64_close((int32_t)conn);
        return GET_BAD_HEADER;
    }

    // ── Receive into <target>.part ──────────────────────────────────────
    int64_t out = os64_open(partPath, "w");
    if (out < 0)
    {
        os64_hprintf(OS64_STDERR, "os64get: cannot create %s\n", partPath);
        os64_close((int32_t)conn);
        return archiving ? GET_ARCHIVE_FAILED : GET_WRITE_FAILED;
    }

    uint8_t buf[GET_CHUNK];
    uint64_t got = 0;
    uint32_t crc = os64_crc32_begin();
    int64_t status = GET_OK;

    while (got < expectLen)
    {
        uint64_t want = expectLen - got;
        if (want > sizeof(buf))
            want = sizeof(buf);

        int64_t n = os64_read((int32_t)conn, buf, (size_t)want);
        if (n <= 0)
        {
            // A short read is normal on a stream and handled by the loop;
            // zero or negative means the conversation ended early, and the
            // file we have is a fragment. Say how far it got — "it failed"
            // and "it failed at 4MB of 5" are different amounts of help.
            os64_hprintf(OS64_STDERR,
                         "os64get: connection ended after %lu of %lu bytes\n",
                         (unsigned long)got, (unsigned long)expectLen);
            status = GET_SHORT;
            break;
        }

        if (os64_write((int32_t)out, buf, (size_t)n) != n)
        {
            os64_hprintf(OS64_STDERR, "os64get: write to %s failed (disk full?)\n", partPath);
            status = GET_WRITE_FAILED;
            break;
        }
        // Checksum as it flies past. The file is never held whole in memory,
        // which is why the streaming interface exists.
        crc = os64_crc32_update(crc, buf, (size_t)n);
        got += (uint64_t)n;

        // Progress every 4KB, not every 64KB. The first version updated so
        // rarely that Chris watched a 146KB transfer sit on one line for
        // fifty seconds and concluded it had frozen — which, at the speeds
        // this stack currently manages, was an entirely reasonable reading.
        // A progress meter exists to distinguish "slow" from "dead", and one
        // that updates less often than a human's patience runs out is doing
        // the opposite of its job.
        if (!quiet && (got % 4096 < (uint64_t)n || got == expectLen))
            os64_printf("\r%s: %lu/%lu bytes", name,
                        (unsigned long)got, (unsigned long)expectLen);
    }
    if (!quiet)
        os64_printf("\n");

    os64_close((int32_t)conn);

    if (status == GET_OK && os64_sync((int32_t)out) < 0)
    {
        os64_hprintf(OS64_STDERR,
                     "os64get: could not commit %s; %s NOT installed\n",
                     partPath, dest);
        status = GET_WRITE_FAILED;
    }

    os64_close((int32_t)out);

    if (status != GET_OK)
    {
        // Leave the .part behind ON PURPOSE. The real name still holds the
        // previous version, which is the point, and the fragment is evidence
        // — a human debugging a failed refresh wants to look at it, and
        // deleting it to be tidy would throw that away.
        return (int)status;
    }

    // ── Verify, THEN publish the wire copy ──────────────────────────────
    uint32_t actual = os64_crc32_end(crc);
    if (actual != expectCrc)
    {
        os64_hprintf(OS64_STDERR,
                     "os64get: CHECKSUM MISMATCH for %s — got %08x, expected %08x. "
                     "%s left in place; %s NOT installed.\n",
                     name, actual, expectCrc, partPath, dest);
        return GET_CORRUPT;
    }

    // One motion. Before this call the old file is whole; after it, the new
    // one is. There is no instant in between, which is what makes pointing
    // this at /bin a reasonable thing to do rather than a brave one.
    if (os64_rename(partPath, wireTarget) < 0)
    {
        os64_hprintf(OS64_STDERR,
                     "os64get: %s verified but could not be renamed to %s "
                     "(read-only filesystem, or a directory in the way?)\n",
                     partPath, wireTarget);
        return archiving ? GET_ARCHIVE_FAILED : GET_PUBLISH_FAILED;
    }

    if (!archiving)
    {
        if (!quiet)
            os64_printf("%s: %lu bytes, crc %08x, verified and installed\n",
                        dest, (unsigned long)expectLen, actual);
        return GET_OK;
    }

    // ── Install FROM the archive ────────────────────────────────────────
    // The archive copy is sealed. Now the same dance beside the destination:
    // copy (checksummed — the disk gets no more trust than the wire), sync,
    // rename. A failure here takes the archive copy with it: the archive
    // records what landed, and this did not.
    char destPart[GET_PATH_MAX];
    int rc = GET_OK;
    if (os64_snprintf(destPart, sizeof(destPart), "%s.part", dest) >= (int32_t)sizeof(destPart))
        rc = GET_WRITE_FAILED;
    else
        rc = copy_verified(archiveFile, destPart, expectLen, expectCrc);

    if (rc == GET_OK && os64_rename(destPart, dest) < 0)
    {
        os64_hprintf(OS64_STDERR,
                     "os64get: %s verified but could not be renamed to %s "
                     "(read-only filesystem, or a directory in the way?)\n",
                     destPart, dest);
        rc = GET_PUBLISH_FAILED;
    }

    if (rc != GET_OK)
    {
        if (rc == GET_CORRUPT)
            os64_hprintf(OS64_STDERR, "os64get: the copy of %s to %s did not verify — %s NOT installed\n",
                         archiveFile, destPart, dest);
        else if (rc == GET_WRITE_FAILED)
            os64_hprintf(OS64_STDERR, "os64get: cannot write %s (disk full, or the directory missing?) — %s NOT installed\n",
                         destPart, dest);
        else if (rc == GET_ARCHIVE_FAILED)
            os64_hprintf(OS64_STDERR, "os64get: cannot read back %s — %s NOT installed\n",
                         archiveFile, dest);
        // The rule: only a successful install earns an archive entry. The
        // dated directories stay (they may hold a sibling that did land).
        if (os64_unlink(archiveFile) < 0)
            os64_hprintf(OS64_STDERR, "os64get: (and %s could not be removed — remove it by hand)\n",
                         archiveFile);
        return rc;
    }

    if (!quiet)
        os64_printf("%s: %lu bytes, crc %08x, verified and installed (kept at %s)\n",
                    dest, (unsigned long)expectLen, actual, archiveFile);
    return GET_OK;
}
