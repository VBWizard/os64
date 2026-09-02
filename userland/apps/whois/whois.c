// whois.c — ask a WHOIS server about a name, and print what it says.
//
// The oldest conversation on the internet that is still held the same way.
// NICNAME/WHOIS is RFC 812 (March 1982, Harrenstien and White at SRI-NIC),
// written to look people up in the ARPANET directory a year before DNS
// existed (RFCs 882/883, November 1983). RFC 954 (1985) and RFC 3912 (2004)
// restated it without changing it. The whole protocol: connect to port 43,
// send the query followed by CRLF, read until the server closes, and the
// bytes you read are the answer. No headers, no framing, no status codes.
// That is why it is the first stranger this stack talks to: the transport
// is the entire test.
//
// v1 prints the reply RAW — CRLF and all, the cat model — and follows no
// referrals. IANA answers with the registry for the TLD in a `refer:` line,
// the registry with the registrar, and chasing that chain is what a full
// whois does; it is booked in DEBTS, not built. `-h SERVER` is how you walk
// it by hand until then.
//
// Exit codes: 0 answered, 1 the network refused or the conversation broke,
// 2 usage.

#include "os64/os64.h"

#define WHOIS_PORT           43
#define WHOIS_DEFAULT_SERVER "whois.iana.org"
#define WHOIS_QUERY_MAX      256
#define WHOIS_DIAL_MAX       (OS64_RESOLVE_NAME_MAX + 16)   // "tcp!" + name + "!43"

static int write_all(int32_t handle, const void *buffer, size_t length)
{
    const uint8_t *bytes = buffer;
    size_t written = 0;
    while (written < length) {
        int64_t result = os64_write(handle, bytes + written, length - written);
        if (result <= 0)
            return -1;
        written += (size_t)result;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *server = WHOIS_DEFAULT_SERVER;
    const char *operands[2] = {0};
    os64_args_t args = {0};
    const os64_optspec_t specs[] = {
        {'h', "host", true, "ask SERVER instead of " WHOIS_DEFAULT_SERVER,
         .value_out = &server},
    };

    os64_args_init(&args, argc, argv, specs,
                   (int32_t)(sizeof(specs) / sizeof(specs[0])));
    args.about = "Ask a WHOIS server (RFC 812, port 43) about a domain, "
                 "an address block, or a handle.";
    args.details = "The reply is printed exactly as the server sent it. "
                   "Referrals are not followed: IANA names the registry for "
                   "a top-level domain in its `refer:` line — ask that server "
                   "with -h.";
    int32_t operand_count = os64_args_parse(
        &args, "whois [-h SERVER] QUERY", operands, 2);
    if (operand_count == OS64_ARG_HELP)
        return 0;
    if (operand_count != 1) {
        os64_hprintf(OS64_STDERR, operand_count < 1
                     ? "whois: what shall I ask about? (whois [-h SERVER] QUERY)\n"
                     : "whois: one query at a time\n");
        return 2;
    }
    const char *query = operands[0];
    size_t query_length = os64_strlen(query);
    if (query_length == 0 || query_length > WHOIS_QUERY_MAX) {
        os64_hprintf(OS64_STDERR, "whois: the query must be 1 to %d bytes\n",
                     WHOIS_QUERY_MAX);
        return 2;
    }

    char dialstring[WHOIS_DIAL_MAX];
    int32_t n = os64_snprintf(dialstring, sizeof(dialstring), "tcp!%s!%d",
                              server, WHOIS_PORT);
    if (n < 0 || (size_t)n >= sizeof(dialstring)) {
        os64_hprintf(OS64_STDERR, "whois: server name is too long to dial (limit %d)\n",
                     OS64_RESOLVE_NAME_MAX);
        return 2;
    }

    int64_t conn = os64_dial(dialstring);
    if (conn < 0) {
        os64_hprintf(OS64_STDERR, "whois: cannot reach %s:%d — %s\n",
                     server, WHOIS_PORT, os64_dial_reason(conn));
        return 1;
    }

    // The request is the query and a CRLF. Bare LF is not the protocol:
    // 1982 servers mean the carriage return, and some still do.
    char request[WHOIS_QUERY_MAX + 2];
    os64_memcpy(request, query, query_length);
    request[query_length] = '\r';
    request[query_length + 1] = '\n';
    if (write_all((int32_t)conn, request, query_length + 2) < 0) {
        os64_hprintf(OS64_STDERR, "whois: %s hung up before the question was asked\n",
                     server);
        os64_close((int32_t)conn);
        return 1;
    }

    // Read until the server closes — 0 is its FIN and the end of the
    // answer; a negative read is the conversation breaking (a reset, or a
    // signal). Every chunk goes straight to stdout, so `whois x | less`
    // and `whois x > file` work the way every other stream does.
    static uint8_t buffer[8192];
    int status = 0;
    for (;;) {
        int64_t got = os64_read((int32_t)conn, buffer, sizeof(buffer));
        if (got == 0)
            break;
        if (got < 0) {
            os64_hprintf(OS64_STDERR, "whois: connection to %s broke mid-answer (%ld)\n",
                         server, got);
            status = 1;
            break;
        }
        if (write_all(OS64_STDOUT, buffer, (size_t)got) < 0) {
            os64_hprintf(OS64_STDERR, "whois: cannot write the answer\n");
            status = 1;
            break;
        }
    }
    os64_close((int32_t)conn);
    return status;
}
