// os64get.c — fetch a file over the wire, and refuse to install a bad one.
//
// THE MISSION, in one line: `os64 refresh` without a thumb drive. Until
// today the only way a fresh build reached the P5 was a human carrying a
// USB stick across a room.
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
// with no tooling, and this OS's network stack met its first real wire
// about four hours ago.
//
// ── THE PART THAT MATTERS: PUBLISHING ───────────────────────────────────
//
// The bytes are written to `<dest>.part` and only become `<dest>` after
// BOTH the length and the CRC32 check out. That ordering is the entire
// safety property, and it is the reason a rename syscall got built this
// morning before a line of the driver existed:
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
// That last property is what makes this safe to point at /bin. A refresh
// that replaces a program someone is running works: the running copy keeps
// demand-paging the inode it already holds (the ext2 orphan chain, also
// built this morning), and the new binary is there at the next boot.
//
// Exit codes name the step that failed, per the house fixture convention —
// a shell script driving a dozen of these should not have to parse English.

#include "os64/os64.h"
#include "os64/crc32.h"

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

#define GET_PORT      6464
#define GET_CHUNK     4096
#define GET_PATH_MAX  256

// Parse a decimal from [s, end). Returns false on empty or non-digit —
// refusal rather than a guess, because a malformed length is exactly the
// case where guessing writes a file of the wrong size.
static bool parse_u64(const char *s, const char *end, uint64_t *out)
{
    if (s >= end)
        return false;
    uint64_t v = 0;
    while (s < end)
    {
        if (*s < '0' || *s > '9')
            return false;
        v = v * 10 + (uint64_t)(*s - '0');
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
    const os64_optspec_t specs[] = {
        {'q', "quiet", false, "no progress, just the exit code", .flag = &quiet},
    };

    os64_args_init(&args, argc, argv, specs, 1);
    args.about = "Fetch a file over the network and install it only if it is intact.";
    args.details = "Writes DEST.part, verifies length and CRC32, then renames into place.";

    int32_t count = os64_args_parse(&args, "os64get [-q] HOST NAME [DEST]", operands, 4);
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
    const char *dest = (count >= 3) ? operands[2] : name;

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
    if (reqlen <= 0 || os64_write((int32_t)conn, request, (size_t)reqlen) != reqlen)
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

    // ── Receive into <dest>.part ────────────────────────────────────────
    char partPath[GET_PATH_MAX];
    if (os64_snprintf(partPath, sizeof(partPath), "%s.part", dest) < 0)
    {
        os64_hprintf(OS64_STDERR, "os64get: destination name too long\n");
        os64_close((int32_t)conn);
        return GET_WRITE_FAILED;
    }

    int64_t out = os64_open(partPath, "w");
    if (out < 0)
    {
        os64_hprintf(OS64_STDERR, "os64get: cannot create %s\n", partPath);
        os64_close((int32_t)conn);
        return GET_WRITE_FAILED;
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
            os64_printf("\r%s: %lu/%lu bytes", dest,
                        (unsigned long)got, (unsigned long)expectLen);
    }
    if (!quiet)
        os64_printf("\n");

    os64_close((int32_t)conn);
    os64_close((int32_t)out);

    if (status != GET_OK)
    {
        // Leave the .part behind ON PURPOSE. The real name still holds the
        // previous version, which is the point, and the fragment is evidence
        // — a human debugging a failed refresh wants to look at it, and
        // deleting it to be tidy would throw that away.
        return (int)status;
    }

    // ── Verify, THEN publish ────────────────────────────────────────────
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
    if (os64_rename(partPath, dest) < 0)
    {
        os64_hprintf(OS64_STDERR,
                     "os64get: %s verified but could not be renamed to %s "
                     "(read-only filesystem, or a directory in the way?)\n",
                     partPath, dest);
        return GET_PUBLISH_FAILED;
    }

    if (!quiet)
        os64_printf("%s: %lu bytes, crc %08x, verified and installed\n",
                    dest, (unsigned long)expectLen, actual);
    return GET_OK;
}
