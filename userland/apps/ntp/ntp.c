// ntp — set os64's wall clock from one SNTP exchange.
//
// This is deliberately a one-shot command, not a clock discipline daemon.
// The kernel's monotonic tick clock continues untouched; only the UTC wall
// clock moves.  NTP timestamps and packet integers are network byte order,
// while os64's network ABI is host order only for the dial destination.

#include "os64/os64.h"

#define NTP_PACKET_SIZE       48u
#define NTP_PORT              123
#define NTP_UNIX_DELTA        2208988800u
#define NTP_FRACTION_SCALE    0x100000000ULL
#define DEFAULT_TIMEOUT_MS    5000u
#define DEFAULT_BIG_CHANGE_S  300u

#define NTP_OFF_ORIGINATE     24u
#define NTP_OFF_RECEIVE       32u
#define NTP_OFF_TRANSMIT      40u

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static uint64_t get_timestamp(const uint8_t *p)
{
    return ((uint64_t)get_be32(p) << 32) | get_be32(p + 4);
}

static void put_timestamp(uint8_t *p, uint64_t value)
{
    put_be32(p, (uint32_t)(value >> 32));
    put_be32(p + 4, (uint32_t)value);
}

// NTP's seconds word is modulo 2^32.  The era split below covers Unix time
// from 1968 through 2104 and, importantly, crosses NTP's 2036 rollover.
static int64_t ntp_seconds_to_unix(uint32_t seconds)
{
    if (seconds >= NTP_UNIX_DELTA)
        return (int64_t)seconds - NTP_UNIX_DELTA;
    return (int64_t)seconds + (int64_t)NTP_FRACTION_SCALE - NTP_UNIX_DELTA;
}

static int time_snapshot(os64_time_t *time, uint64_t *stamp)
{
    if (os64_time(time) < 0 || time->ticks_per_second == 0 ||
        time->ticks_into_second >= time->ticks_per_second)
        return -1;

    uint64_t fraction = ((uint64_t)time->ticks_into_second << 32) /
                        time->ticks_per_second;
    uint32_t seconds = (uint32_t)((uint64_t)time->epoch + NTP_UNIX_DELTA);
    *stamp = ((uint64_t)seconds << 32) | fraction;
    return 0;
}

static int parse_u32(const char *text, uint32_t min, uint32_t max,
                     uint32_t *out)
{
    if (text == NULL || *text == '\0')
        return -1;

    uint64_t value = 0;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9')
            return -1;
        value = value * 10u + (uint32_t)(*p - '0');
        if (value > max)
            return -1;
    }
    if (value < min)
        return -1;
    *out = (uint32_t)value;
    return 0;
}

static int64_t average_signed(int64_t a, int64_t b)
{
    // a+b can overflow even though its average is representable.
    return a / 2 + b / 2 + (a % 2 + b % 2) / 2;
}

static void print_offset(const char *prefix, int64_t offset)
{
    bool negative = offset < 0;
    uint64_t magnitude = negative ? 0u - (uint64_t)offset : (uint64_t)offset;
    uint64_t seconds = magnitude >> 32;
    uint64_t millis = ((magnitude & 0xffffffffu) * 1000u) >> 32;
    os64_hprintf(OS64_STDERR, "%s%c%llu.%03llu seconds\n", prefix,
                 negative ? '-' : '+', (unsigned long long)seconds,
                 (unsigned long long)millis);
}

static const char *dial_error(int64_t result)
{
    if (result == OS64_NET_ERR_BAD_ADDRESS)
        return "server must be a dotted IPv4 address or a name";
    if (result == OS64_NET_ERR_NO_SUCH_HOST)
        return "no such host (not in the hosts files or DNS)";
    if (result == OS64_NET_ERR_NO_RESOLVER)
        return "a name, but no name server to ask (see /etc/net.conf)";
    if (result == OS64_NET_ERR_NO_NIC)
        return "no network interface is available";
    if (result == OS64_NET_ERR_NO_RESOURCES)
        return "the network stack is out of resources";
    return "the UDP endpoint could not be opened";
}

int main(int argc, char **argv)
{
    const char *timeout_text = NULL;
    const char *warning_text = NULL;
    bool verbose = false;
    const os64_optspec_t specs[] = {
        { 't', "timeout", true, "reply timeout in milliseconds (default: 5000)",
          .value_out = &timeout_text },
        { 'w', "warn", true, "warn when the correction is at least this many seconds (default: 300)",
          .value_out = &warning_text },
        { 'v', "verbose", false, "print successful ordinary corrections",
          .flag = &verbose },
    };
    os64_args_t args;
    os64_args_init(&args, argc, argv, specs, 3);
    args.about = "Set the UTC wall clock from one SNTP/NTPv4 server exchange.";
    args.details = "SERVER is a dotted IPv4 address. Normal corrections are silent; large ones warn.";

    const char *server = NULL;
    int32_t count = os64_args_parse(&args,
                                    "ntp [-v] [-t milliseconds] [-w seconds] SERVER",
                                    &server, 1);
    if (count == OS64_ARG_HELP)
        return 0;
    if (count == OS64_ARG_ERROR)
        return 2;
    if (count != 1) {
        os64_hprintf(OS64_STDERR, "ntp: missing NTP server address\n");
        return 2;
    }

    uint32_t timeout_ms = DEFAULT_TIMEOUT_MS;
    uint32_t warning_seconds = DEFAULT_BIG_CHANGE_S;
    if (timeout_text != NULL &&
        parse_u32(timeout_text, 1, 60000, &timeout_ms) < 0) {
        os64_hprintf(OS64_STDERR,
                     "ntp: timeout must be between 1 and 60000 milliseconds\n");
        return 2;
    }
    if (warning_text != NULL &&
        parse_u32(warning_text, 0, 0x7fffffffu, &warning_seconds) < 0) {
        os64_hprintf(OS64_STDERR,
                     "ntp: warning threshold must be a non-negative number of seconds\n");
        return 2;
    }

    // Sized from the NAME LIMIT now that the second segment may be a name:
    // "udp!" + up to OS64_RESOLVE_NAME_MAX (253, DNS's own ceiling) + "!123"
    // + NUL. The old 80 bytes were chosen when only a dotted quad could stand
    // here, and they refused every hostname over 71 characters — with the
    // resolver sitting right there, willing and able. A limit inherited from
    // the previous version of a feature is the quietest kind of wrong.
    // (Codex review, 2026-08-23.)
    char dial_string[OS64_RESOLVE_NAME_MAX + 16];
    int32_t dial_length = os64_snprintf(dial_string, sizeof(dial_string),
                                        "udp!%s!%d", server, NTP_PORT);
    if (dial_length < 0 || (size_t)dial_length >= sizeof(dial_string)) {
        os64_hprintf(OS64_STDERR, "ntp: server name is too long (limit %d)\n",
                     OS64_RESOLVE_NAME_MAX);
        return 2;
    }

    int64_t handle = os64_dial(dial_string);
    if (handle < 0) {
        os64_hprintf(OS64_STDERR, "ntp: cannot contact %s: %s\n",
                     server, dial_error(handle));
        return 1;
    }

    uint8_t packet[NTP_PACKET_SIZE] = {0};
    packet[0] = (uint8_t)((4u << 3) | 3u); // leap=0, version=4, mode=client

    os64_time_t sent_time;
    uint64_t t1;
    if (time_snapshot(&sent_time, &t1) < 0) {
        os64_hprintf(OS64_STDERR, "ntp: cannot read the local clock\n");
        os64_close((int32_t)handle);
        return 1;
    }
    put_timestamp(packet + NTP_OFF_TRANSMIT, t1);

    if (os64_write((int32_t)handle, packet, sizeof(packet)) !=
        (int64_t)sizeof(packet)) {
        os64_hprintf(OS64_STDERR, "ntp: request send failed\n");
        os64_close((int32_t)handle);
        return 1;
    }

    int64_t received = os64_read_for((int32_t)handle, packet, sizeof(packet),
                                     timeout_ms);
    os64_time_t received_time;
    uint64_t t4;
    int clock_ok = time_snapshot(&received_time, &t4);
    os64_close((int32_t)handle);

    if (received == OS64_ERR_TIMEOUT) {
        os64_hprintf(OS64_STDERR, "ntp: %s did not answer within %u ms\n",
                     server, timeout_ms);
        return 1;
    }
    if (received != (int64_t)sizeof(packet)) {
        os64_hprintf(OS64_STDERR, "ntp: malformed reply (%lld bytes)\n",
                     (long long)received);
        return 1;
    }
    if (clock_ok < 0) {
        os64_hprintf(OS64_STDERR, "ntp: cannot read the local clock\n");
        return 1;
    }

    uint8_t leap = packet[0] >> 6;
    uint8_t version = (packet[0] >> 3) & 7u;
    uint8_t mode = packet[0] & 7u;
    uint8_t stratum = packet[1];
    uint64_t origin = get_timestamp(packet + NTP_OFF_ORIGINATE);
    uint64_t t2 = get_timestamp(packet + NTP_OFF_RECEIVE);
    uint64_t t3 = get_timestamp(packet + NTP_OFF_TRANSMIT);

    if (leap == 3u) {
        os64_hprintf(OS64_STDERR, "ntp: %s reports an unsynchronized clock\n",
                     server);
        return 1;
    }
    if ((version != 3u && version != 4u) || mode != 4u) {
        os64_hprintf(OS64_STDERR, "ntp: reply has invalid version or mode\n");
        return 1;
    }
    if (stratum == 0u || stratum > 15u) {
        os64_hprintf(OS64_STDERR, "ntp: server refused synchronization (stratum %u)\n",
                     stratum);
        return 1;
    }
    if (origin != t1) {
        os64_hprintf(OS64_STDERR, "ntp: reply does not match this request\n");
        return 1;
    }
    if (t2 == 0 || t3 == 0) {
        os64_hprintf(OS64_STDERR, "ntp: reply is missing server timestamps\n");
        return 1;
    }

    // RFC 5905/SNTP clock offset: ((T2-T1) + (T3-T4)) / 2.  Subtraction
    // is deliberately modulo 2^64; converting the small resulting delta to
    // signed form is how NTP timestamp arithmetic spans the era rollover.
    int64_t offset = average_signed((int64_t)(t2 - t1),
                                    (int64_t)(t3 - t4));
    uint64_t corrected = t4 + (uint64_t)offset;

    int64_t target_epoch = ntp_seconds_to_unix((uint32_t)(corrected >> 32));
    uint32_t remote_fraction = (uint32_t)corrected;
    uint64_t local_fraction = ((uint64_t)received_time.ticks_into_second << 32) /
                              received_time.ticks_per_second;

    // set_time accepts whole seconds and preserves the kernel's current tick
    // phase. Choose the nearest whole-second counter value for that phase.
    int64_t fraction_delta = (int64_t)remote_fraction - (int64_t)local_fraction;
    if (fraction_delta >= (int64_t)(NTP_FRACTION_SCALE / 2u))
        target_epoch++;
    else if (fraction_delta <= -(int64_t)(NTP_FRACTION_SCALE / 2u))
        target_epoch--;

    uint64_t magnitude = offset < 0 ? 0u - (uint64_t)offset : (uint64_t)offset;
    bool big_change = magnitude >= ((uint64_t)warning_seconds << 32);
    if (os64_set_time(target_epoch) < 0) {
        os64_hprintf(OS64_STDERR, "ntp: the kernel refused to set the clock\n");
        return 1;
    }

    if (big_change)
        print_offset("ntp: warning: large wall-clock correction of ", offset);
    else if (verbose)
        print_offset("ntp: synchronized; correction ", offset);
    return 0;
}
