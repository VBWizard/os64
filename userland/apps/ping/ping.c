// ping.c — a tiny os64 userland ICMP echo client.
//
// The kernel owns the wire and the ICMP conversation machinery; this app just
// speaks the handle model already wired into libos64: dial an ICMP handle,
// write an echo request, read the echoed payload back, and report the RTT in
// scheduler ticks. The current stack is still polled, so the resolution is a
// single tick (10ms) — exactly the shape the note promised.

#include "os64/os64.h"
typedef struct {
    uint32_t seq;
    uint32_t sent_tick;
} ping_payload_t;

static char ipString[16] = {0}; // Max = 15 chars xxx.yyy.zzz.aaa
static bool ctrlCExit = false;
static const char *net_dial_error_message(int64_t err)
{
    switch (err)
    {
        case OS64_NET_ERR_BAD_STRING:
            return "bad dial string or unsupported protocol";
        case OS64_NET_ERR_BAD_ADDRESS:
            return "bad address; expected a dotted quad or a name";
        case OS64_NET_ERR_NO_SUCH_HOST:
            return "no such host (not in /home/hosts, /etc/hosts, or DNS)";
        case OS64_NET_ERR_NO_RESOLVER:
            return "a name, but no name server to ask (DHCP gave none; see /etc/net.conf)";
        case OS64_NET_ERR_BAD_SERVICE:
            return "bad service/port; ICMP has no port";
        case OS64_NET_ERR_BAD_DEST:
            return "bad destination data";
        case OS64_NET_ERR_NO_NIC:
            return "no network interface available";
        case OS64_NET_ERR_NO_RESOURCES:
            return "out of network resources";
        case OS64_NET_ERR_REFUSED:
            return "destination refused the connection";
        case OS64_NET_ERR_TIMEOUT:
            return "dial timed out waiting for the peer";
        case OS64_NET_ERR_INVALID:
            return "invalid network dial request";
        case OS64_NET_ERR_BAD_POINTER:
            return "bad destination pointer";
        default:
            return "unknown dial error";
    }
}

static int build_dial_string(char *buf, size_t cap, const char *arg)
{
    size_t arg_len = os64_strlen(arg);
    if (arg_len == 0)
        return -1;

    if (arg_len >= 5 &&
        arg[0] == 'i' && arg[1] == 'c' && arg[2] == 'm' &&
        arg[3] == 'p' && arg[4] == '!')
        return os64_strcopy(buf, cap, arg) < cap ? 0 : -1;

    int32_t length = os64_snprintf(buf, cap, "icmp!%s", arg);
    return length >= 0 && (size_t)length < cap ? 0 : -1;
}

static const char *count_arg = NULL;
static const char *wait_arg = NULL;
static const char *interval_arg = NULL;

static uint64_t ms_to_ticks_ceil(uint64_t ms, uint32_t ticks_per_second)
{
    return (ms * ticks_per_second + 999u) / 1000u;
}

static uint64_t ticks_to_ms_ceil(uint64_t ticks, uint32_t ticks_per_second)
{
    return (ticks * 1000u + ticks_per_second - 1u) / ticks_per_second;
}

static void print_summary(const char *target, uint32_t sent, uint32_t received,
                          uint64_t rtt_min, uint64_t rtt_total,
                          uint64_t rtt_max)
{
    uint32_t lost = sent - received;
    uint32_t loss_percent = sent == 0 ? 0 : (lost * 100u) / sent;

    os64_printf("--- %s ping statistics ---\n", target);
    os64_printf("%u packet%s transmitted, %u received, %u%% packet loss\n",
                sent, sent == 1 ? "" : "s", received, loss_percent);
    if (received != 0)
    {
        uint64_t average_tenths = (rtt_total * 10u + received / 2u) / received;
        os64_printf("round-trip min/avg/max = %llu/%llu.%llu/%llu ticks\n",
                    (unsigned long long)rtt_min,
                    (unsigned long long)(average_tenths / 10u),
                    (unsigned long long)(average_tenths % 10u),
                    (unsigned long long)rtt_max);
    }
}

int main(int argc, char **argv)
{
    const char *target = NULL;
    uint32_t paramPingCount = 0;
    uint64_t paramTimeout = 5000;
    uint64_t paramInterval = 1000;
    uint32_t rawIP = 0;
    static const os64_optspec_t specs[] = {
        {'n', "count", 1, "stop after count echo requests", .value_out = &count_arg},
        {'w', "wait", 1, "wait this many ms for each reply (default: 5000)", .value_out = &wait_arg},
        {'i', "interval", 1, "start pings this many ms apart (default: 1000)", .value_out = &interval_arg}};
    char dial_string[64];
    os64_args_t args;
    os64_args_init(&args, argc, argv, specs, 3);
    args.about = "Send ICMP echo requests to a target address.";
    args.details = "By default this program pings indefinitely until interrupted.";

    const char *positionals[1] = { NULL };
    int32_t positional_count = os64_args_parse(&args,
                                              "ping [-n count] [-w ms] [-i ms] <ip-or-icmp-dial-or-hostname>",
                                              positionals, 1);
    if (positional_count < 0)
        return positional_count == OS64_ARG_HELP ? 0 : 1;
    if (positional_count < 1)
    {
        os64_printf("ping: missing target\n");
        return 1;
    }

    if (count_arg != NULL)
    {
        int64_t parsed_count = os64_atoi(count_arg);
        if (parsed_count <= 0 || parsed_count > UINT32_MAX)
        {
            os64_printf("ping: bad count '%s'\n", count_arg);
            return 1;
        }
        paramPingCount = (uint32_t)parsed_count;
    }

    if (wait_arg != NULL)
    {
        int64_t parsed_timeout = os64_atoi(wait_arg);
        if (parsed_timeout <= 0)
        {
            os64_printf("ping: bad wait time '%s'\n", wait_arg);
            return 1;
        }
        paramTimeout = (uint64_t)parsed_timeout;
    }

    if (interval_arg != NULL)
    {
        int64_t parsed_interval = os64_atoi(interval_arg);
        if (parsed_interval <= 0)
        {
            os64_printf("ping: bad interval '%s'\n", interval_arg);
            return 1;
        }
        paramInterval = (uint64_t)parsed_interval;
    }

    bool forever = count_arg == NULL;

    target = positionals[0];

    // Resolve ONCE, here, and dial the address that came back — not the name
    // again. os64_dial resolves whatever name it is handed, so passing the
    // name through would ask twice: two DNS timeouts on a bad name, and on a
    // round-robin name two different answers, with the header line printing
    // an address the packets never went to (0.0.0.0 in the case where the
    // first lookup failed and the second one didn't). One lookup, one
    // address, and it is the address we say it is. (Codex review, 2026-08-22.)
    //
    // A full dial string ("icmp!host") is still allowed as the argument; the
    // name to resolve is what follows the bang.
    const char *host = target;
    if (os64_strlen(target) >= 5 && target[0] == 'i' && target[1] == 'c' &&
        target[2] == 'm' && target[3] == 'p' && target[4] == '!')
        host = target + 5;

    int64_t resolved = os64_resolve(host, &rawIP);
    if (resolved < 0)
    {
        os64_printf("ping: cannot resolve %s: %s\n", host,
                    net_dial_error_message(resolved));
        return 1;
    }
    os64_format_ipv4(rawIP, ipString, sizeof(ipString));

    if (build_dial_string(dial_string, sizeof(dial_string), ipString) < 0)
    {
        os64_printf("ping: dial string too long\n");
        return 1;
    }

    int64_t handle = os64_dial(dial_string);
    if (handle < 0)
    {
        os64_printf("ping: dial failed for %s: %s\n",
                    target, net_dial_error_message(handle));
        return 1;
    }

    os64_printf("PING %s (icmp echo)%s (%s)\n",
                target,
                forever ? " forever" : "",
                ipString);

    uint32_t sent = 0;
    uint32_t received = 0;
    uint32_t seq = 1;
    uint64_t rtt_min = 0;
    uint64_t rtt_total = 0;
    uint64_t rtt_max = 0;

    for (;;)
    {
        if ((!forever && seq > paramPingCount) || ctrlCExit)
            break;

        sent++;
        os64_ticks_t start = {0, 0};
        if (os64_ticks(&start) < 0)
        {
            os64_printf("ping: clock read failed\n");
            os64_close(handle);
            return 1;
        }

        ping_payload_t payload;
        payload.seq = seq;
        payload.sent_tick = (uint32_t)start.ticks;

        int64_t written = os64_write((int32_t)handle, &payload, sizeof(payload));
        os64_ticks_t finish = {0, 0};
        if (written == OS64_NET_ERR_TIMEOUT)
        {
            os64_printf("Request timeout sending seq %u\n", seq);
            if (os64_ticks(&finish) < 0)
            {
                os64_printf("ping: clock read failed\n");
                os64_close(handle);
                return 1;
            }
        }
        else if (written != (int64_t)sizeof(payload))
        {
            os64_printf("ping: write failed for seq %u (%lld)\n",
                        seq, (long long)written);
            os64_close(handle);
            return 1;
        }
        else
        {
            os64_ticks_t wait_start = {0, 0};
            if (os64_ticks(&wait_start) < 0)
            {
                os64_printf("ping: clock read failed\n");
                os64_close(handle);
                return 1;
            }
            uint64_t deadline = wait_start.ticks +
                                ms_to_ticks_ceil(paramTimeout, wait_start.per_second);

            for (;;)
            {
                uint64_t remaining_ms;
                if (wait_start.ticks >= deadline)
                {
                    os64_printf("Request timeout for seq %u\n", seq);
                    finish = wait_start;
                    break;
                }
                remaining_ms = ticks_to_ms_ceil(deadline - wait_start.ticks,
                                                wait_start.per_second);

                ping_payload_t reply;
                int64_t n = os64_read_for((int32_t)handle, &reply,
                                          sizeof(reply), remaining_ms);
                if (os64_ticks(&finish) < 0)
                {
                    os64_printf("ping: clock read failed\n");
                    os64_close(handle);
                    return 1;
                }

                if (n == OS64_NET_ERR_TIMEOUT)
                {
                    os64_printf("Request timeout for seq %u\n", seq);
                    break;
                }
                if (n < 0)
                {
                    os64_printf("ping: seq %u failed (%lld)\n",
                                seq, (long long)n);
                    break;
                }
                if ((size_t)n != sizeof(reply) || reply.seq != payload.seq)
                {
                    wait_start = finish;
                    continue;
                }

                uint64_t elapsed = finish.ticks - (uint64_t)reply.sent_tick;
                if (received == 0 || elapsed < rtt_min)
                    rtt_min = elapsed;
                if (received == 0 || elapsed > rtt_max)
                    rtt_max = elapsed;
                rtt_total += elapsed;
                received++;
                os64_printf("reply %u from %s: %llu tick%s\n",
                            seq,
                            ipString,
                            (unsigned long long)elapsed,
                            elapsed == 1 ? "" : "s");
                break;
            }
        }
        seq++;
        if (!forever && seq > paramPingCount)
            break;

        uint64_t elapsed_ms = (finish.ticks - start.ticks) * 1000u /
                              start.per_second;
        if (elapsed_ms < paramInterval)
            os64_sleep(paramInterval - elapsed_ms);
    }

    if (!forever)
        print_summary(target, sent, received, rtt_min, rtt_total, rtt_max);

    os64_close(handle);
    return 0;
}
