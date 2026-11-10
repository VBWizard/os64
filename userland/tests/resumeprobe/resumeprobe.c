// resumeprobe — does a second TLS connection to a host resume the first's
// session, and what does that save? (TLS13.md § Resumption)
//
//     resumeprobe https://host/path [connections]
//
// Opens the address several times (three unless told), each on a connection
// of its own: every fetch is closed with its body unread, and a connection
// with a reply still on it is one libfetch does not keep, so the next open
// dials and handshakes afresh. The first is a full handshake.
// A server that issues tickets lets the ones after it resume, and each line
// says which it was and how long the open took, dial and handshake and the
// reply's head together. The difference between the first line and the rest
// is the certificate chain the resumed ones did not check.
//
// A hand probe, not a testrun fixture: it needs a network and a server.
// Exit 0 when every open answered and at least one after the first resumed;
// 2 when all answered and none resumed (the server issues no tickets, or
// declines them); 1 when an open failed.
#include "os64/os64.h"
#include "fetch/fetch.h"

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) {
        os64_printf("usage: resumeprobe https://host/path [connections]\n");
        return 1;
    }
    unsigned count = 0;
    if (argc == 3)
        for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++)
            count = count * 10 + (unsigned)(*p - '0');
    if (count < 2 || count > 50)
        count = 3;

    os64_fetch_options_t options = {0};
    unsigned resumed = 0;
    for (unsigned i = 0; i < count; i++) {
        int64_t before = os64_micros();
        os64_fetch_t *f = os64_fetch_open(argv[1], &options);
        int64_t took = os64_micros() - before;
        if (f == NULL) {
            os64_printf("resumeprobe: no memory\n");
            return 1;
        }
        if (os64_fetch_status(f) != OS64_FETCH_OK) {
            os64_printf("resumeprobe: connection %u failed: %s\n", i + 1, os64_fetch_reason(f));
            os64_fetch_close(f);
            return 1;
        }
        const os64_fetch_detail_t *detail = os64_fetch_detail(f);
        const os64_fetch_head_t *head = os64_fetch_head(f);
        os64_printf("resumeprobe: connection %u: TLS %s, %s, HTTP %d, open took %ld.%ld ms\n", i + 1,
                    detail->tls_version == 0x0304 ? "1.3" : detail->tls_version == 0x0303 ? "1.2" : "none",
                    detail->tls_resumed ? "RESUMED" : "full handshake", head->status,
                    took / 1000, took / 100 % 10);
        resumed += detail->tls_resumed;
        // Closed with the body unread, which is what makes the next open a
        // new connection. The server's tickets are not lost by it: they
        // follow its Finished, ahead of the reply, and were kept on the way
        // to the head.
        os64_fetch_close(f);
    }
    os64_printf("resumeprobe: %u of %u connections after the first resumed\n", resumed, count - 1);
    return resumed ? 0 : 2;
}
