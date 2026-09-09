// test_telnet_host.c — a hand crank for /bin/telnet's protocol engine.
//
// The engine takes bytes and gives bytes back, which is the whole reason it
// can be driven here: this program is the SOCKET's stand-in, handing over a
// stream in whatever pieces the harness asks for and printing everything the
// engine did with them. tools/test_telnet_host.sh is what decides the pieces
// and judges the answers.
//
// One invocation is a SCRIPT: each argument is a step, run in order against
// one engine, so a negotiation that crosses a keystroke can be written down
// as the sequence it actually was.
//
//   offer            the client's opening offers
//   in:<hex>         bytes arriving from the peer
//   text:<hex>       bytes a person typed
//   cmd:<n>          telnet_send_command (244 = IP, 246 = AYT)
//   size:<c>x<r>     telnet_send_size
//   echo:auto|on|off the local echo override
//   sent:<n>         report n bytes written to the peer (a partial write)
//
//   --chunk N        feed `in:` and `text:` N bytes at a time (0 = all at once)
//   --cap N          the decoded-data buffer handed to telnet_receive
//   --nodrain        never take bytes off the outbound queue, so the queue
//                    fills and the engine's back-pressure is what is measured

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "telnet_protocol.h"

#define ACC_MAX (256 * 1024)

static uint8_t g_data[ACC_MAX];
static size_t  g_data_len;
static uint8_t g_wire[ACC_MAX];
static size_t  g_wire_len;
static uint8_t g_scratch[ACC_MAX];   // the buffer telnet_receive decodes into
static uint8_t g_input[ACC_MAX];     // one step's bytes, decoded from its hex

static size_t   g_consumed;      // bytes of `in:` the engine took
static size_t   g_unfed;         // bytes of `in:` it would not take
static size_t   g_accepted;      // bytes of `text:` it took
static size_t   g_offered;       // bytes of `text:` handed to it
static uint32_t g_notes;         // every notice seen, ORed
static uint32_t g_step_notes;    // and just the ones the last step raised
static int      g_size_ok = -1;  // the last telnet_send_size verdict
static int      g_cmd_ok = -1;   // the last telnet_send_command verdict
static bool     g_offer_ok = true;

static size_t g_chunk = 0;
static size_t g_cap = ACC_MAX;
static bool   g_drain = true;

// Both accumulators at once: the run's notices, and this step's alone. The
// second is what proves a notice fires on a CHANGE and not on a repeat.
static void collect(telnet_t *t)
{
    uint32_t notices = telnet_notices(t);
    g_notes |= notices;
    g_step_notes |= notices;
}

static void die(const char *why)
{
    fprintf(stderr, "test_telnet: %s\n", why);
    exit(2);
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t unhex(const char *text, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (*text != '\0') {
        int hi = hex_digit(text[0]);
        int lo = text[1] == '\0' ? -1 : hex_digit(text[1]);
        if (hi < 0 || lo < 0) die("argument is not an even run of hex digits");
        if (n == cap) die("hex argument is longer than the buffer");
        out[n++] = (uint8_t)((hi << 4) | lo);
        text += 2;
    }
    return n;
}

static void append(uint8_t *dst, size_t *len, const uint8_t *src, size_t n)
{
    if (*len + n > ACC_MAX) die("accumulator full");
    memcpy(dst + *len, src, n);
    *len += n;
}

// Control bytes are printed as escapes so that a fixture cannot repaint the
// screen of whoever is reading the output — the same rule the gopher harness
// keeps, and for the same reason.
static void print_escaped(const char *key, const uint8_t *bytes, size_t n)
{
    printf("%s=", key);
    for (size_t i = 0; i < n; i++) {
        uint8_t c = bytes[i];
        if (c == '\\')
            printf("\\\\");
        else if (c >= 0x20 && c < 0x7F)
            putchar((int)c);
        else
            printf("\\x%02x", c);
    }
    putchar('\n');
}

static void print_hex(const char *key, const uint8_t *bytes, size_t n)
{
    printf("%s=", key);
    for (size_t i = 0; i < n; i++)
        printf("%02x", bytes[i]);
    putchar('\n');
}

static void drain(telnet_t *t)
{
    size_t n = 0;
    const uint8_t *pending = telnet_pending(t, &n);
    if (n == 0)
        return;
    append(g_wire, &g_wire_len, pending, n);
    telnet_sent(t, n);
}

// Hand the engine `len` bytes in slices of `g_chunk`, taking the short
// returns seriously: a slice the engine only partly consumed is offered again
// from where it stopped, and a slice it will not touch at all ends the feed
// with the rest UNFED. That is the contract the client's loop depends on.
static void feed_peer(telnet_t *t, const uint8_t *bytes, size_t len)
{
    size_t at = 0;

    while (at < len) {
        size_t take = g_chunk == 0 || len - at < g_chunk ? len - at : g_chunk;
        size_t done = 0;

        while (done < take) {
            size_t data_len = 0;
            size_t n = telnet_receive(t, bytes + at + done, take - done,
                                      g_scratch, g_cap, &data_len);
            append(g_data, &g_data_len, g_scratch, data_len);
            collect(t);
            if (g_drain)
                drain(t);
            done += n;
            if (n == 0 && data_len == 0)
                break;                  // no room and nothing draining: stuck
        }

        at += done;
        if (done < take)
            break;
    }

    g_consumed += at;
    g_unfed += len - at;
}

static void feed_typed(telnet_t *t, const uint8_t *bytes, size_t len)
{
    size_t at = 0;

    while (at < len) {
        size_t take = g_chunk == 0 || len - at < g_chunk ? len - at : g_chunk;
        size_t n = telnet_send_text(t, bytes + at, take);
        collect(t);
        if (g_drain)
            drain(t);
        at += n;
        if (n < take)
            break;                      // the queue is full and staying full
    }

    g_offered += len;
    g_accepted += at;
}

static void report(telnet_t *t)
{
    static const struct { const char *name; uint8_t option; } WATCHED[] = {
        { "binary", TELNET_OPT_BINARY },
        { "echo",   TELNET_OPT_ECHO   },
        { "sga",    TELNET_OPT_SGA    },
        { "ttype",  TELNET_OPT_TTYPE  },
        { "naws",   TELNET_OPT_NAWS   },
    };

    size_t pending_len = 0;
    const uint8_t *pending = telnet_pending(t, &pending_len);

    print_escaped("data", g_data, g_data_len);
    print_hex("wire", g_wire, g_wire_len);
    print_hex("pending", pending, pending_len);
    printf("consumed=%zu\n", g_consumed);
    printf("unfed=%zu\n", g_unfed);
    printf("accepted=%zu\n", g_accepted);
    printf("offered=%zu\n", g_offered);
    printf("mid=%d\n", telnet_mid_sequence(t) ? 1 : 0);
    printf("notes=%u\n", g_notes);
    printf("notes_last=%u\n", g_step_notes);
    printf("echo=%d\n", telnet_local_echo(t) ? 1 : 0);
    printf("echo_mode=%d\n", (int)telnet_echo_mode(t));
    printf("offer_ok=%d\n", g_offer_ok ? 1 : 0);
    printf("size_ok=%d\n", g_size_ok);
    printf("cmd_ok=%d\n", g_cmd_ok);

    for (size_t i = 0; i < sizeof(WATCHED) / sizeof(WATCHED[0]); i++) {
        printf("us.%s=%d\n", WATCHED[i].name,
               telnet_option_ours(t, WATCHED[i].option) ? 1 : 0);
        printf("him.%s=%d\n", WATCHED[i].name,
               telnet_option_his(t, WATCHED[i].option) ? 1 : 0);
    }

    // The limits, read back rather than spelled twice in the harness.
    printf("out_max=%d\n", TELNET_OUT_MAX);
    printf("reply_max=%d\n", TELNET_REPLY_MAX);

    // The names, so the harness can prove that every option and command it
    // knows about has one and that none of them is empty.
    printf("name.option=%s\n", telnet_option_name(TELNET_OPT_NAWS));
    printf("name.command=%s\n", telnet_command_name(TELNET_IP));
    printf("name.unknown_option=%s\n", telnet_option_name(200));
    printf("name.unknown_command=%s\n", telnet_command_name(200));
    printf("name.pair=%s %s\n", telnet_option_name(201), telnet_option_name(202));
}

int main(int argc, char **argv)
{
    telnet_t engine;
    telnet_init(&engine);

    for (int i = 1; i < argc; i++) {
        const char *step = argv[i];

        g_step_notes = 0;

        if (strcmp(step, "--chunk") == 0) {
            if (++i == argc) die("--chunk wants a number");
            g_chunk = (size_t)strtoul(argv[i], NULL, 10);
        } else if (strcmp(step, "--cap") == 0) {
            if (++i == argc) die("--cap wants a number");
            g_cap = (size_t)strtoul(argv[i], NULL, 10);
            if (g_cap == 0 || g_cap > ACC_MAX) die("--cap out of range");
        } else if (strcmp(step, "--nodrain") == 0) {
            g_drain = false;
        } else if (strcmp(step, "offer") == 0) {
            g_offer_ok = telnet_offer(&engine);
            collect(&engine);
            if (g_drain)
                drain(&engine);
        } else if (strncmp(step, "in:", 3) == 0) {
            feed_peer(&engine, g_input, unhex(step + 3, g_input, ACC_MAX));
        } else if (strncmp(step, "text:", 5) == 0) {
            feed_typed(&engine, g_input, unhex(step + 5, g_input, ACC_MAX));
        } else if (strncmp(step, "cmd:", 4) == 0) {
            unsigned long c = strtoul(step + 4, NULL, 10);
            g_cmd_ok = telnet_send_command(&engine, (uint8_t)c) ? 1 : 0;
            if (g_drain)
                drain(&engine);
        } else if (strncmp(step, "size:", 5) == 0) {
            char *end = NULL;
            unsigned long cols = strtoul(step + 5, &end, 10);
            if (end == NULL || *end != 'x') die("size wants <cols>x<rows>");
            unsigned long rows = strtoul(end + 1, NULL, 10);
            g_size_ok = telnet_send_size(&engine, (uint16_t)cols, (uint16_t)rows) ? 1 : 0;
            collect(&engine);
            if (g_drain)
                drain(&engine);
        } else if (strncmp(step, "echo:", 5) == 0) {
            const char *mode = step + 5;
            if (strcmp(mode, "auto") == 0)
                telnet_set_echo_mode(&engine, TELNET_ECHO_AUTO);
            else if (strcmp(mode, "on") == 0)
                telnet_set_echo_mode(&engine, TELNET_ECHO_ON);
            else if (strcmp(mode, "off") == 0)
                telnet_set_echo_mode(&engine, TELNET_ECHO_OFF);
            else
                die("echo wants auto, on or off");
        } else if (strncmp(step, "crlf:", 5) == 0) {
            const char *how = step + 5;
            if (strcmp(how, "on") == 0)
                telnet_set_eol_crlf(&engine, true);
            else if (strcmp(how, "off") == 0)
                telnet_set_eol_crlf(&engine, false);
            else
                die("crlf wants on or off");
        } else if (strncmp(step, "sent:", 5) == 0) {
            size_t n = (size_t)strtoul(step + 5, NULL, 10);
            size_t have = 0;
            const uint8_t *pending = telnet_pending(&engine, &have);
            if (n > have)
                n = have;
            append(g_wire, &g_wire_len, pending, n);
            telnet_sent(&engine, n);
        } else {
            die("unknown step");
        }
    }

    report(&engine);
    return 0;
}
