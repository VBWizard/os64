#include "ssh_internal.h"
#include "os64/os64.h"
#include "os64/pty.h"
#include "os64/conf.h"

/* The event loop owns the socket and engine. The input worker can block in
 * pipe_write without preventing SSH window updates or command output. */
static ssh_engine engine;
static uint8_t input_ring[SSH_WINDOW];
static uint32_t input_head, input_tail;
static int input_done, input_broken;
static int32_t child_input = -1, child_output = -1, child_error = -1, master = -1;
static int64_t child = -1;
static uint32_t credited;
#define CHILD_RING_CAP 65536u
typedef struct {
    uint8_t bytes[CHILD_RING_CAP];
    uint32_t head, tail;
    int eof, error;
    int32_t fd;
} child_stream;
static child_stream streams[2];
/* One local connection per forward slot (engine.forwards[i], REMOTE.md
 * section 2). to_local holds client bytes the connection has not taken yet,
 * bounded by the window the forward advertised; from_local holds what the
 * channel could not take yet. Both are allocated when the forward opens, not
 * reserved for all seven up front: a program's image must fit its link slot
 * (app_bases.py), and most connections forward nothing. The event loop alone
 * touches them, as it alone touches the engine. */
typedef struct {
    int32_t handle;                         // the dialed connection, or -1
    uint8_t *to_local;                      // SSH_FORWARD_WINDOW bytes while open
    uint32_t to_len;
    uint8_t *from_local;                    // SSH_DATA_MAX bytes while open
    uint32_t from_len;
    int local_ended;                        // read EOF or a failed write: EOF + CLOSE owed
    int client_eof;                         // the client half-closed (FORWARD_EOF)
    int peer_closed;                        // the channel closed: deliver to_local, then release
} forward_link;
static forward_link links[SSH_FORWARDS];
static const char key_path[] = "/home/sshd_host_key";
/* The forwarding policy this session obeys, from the listener's argv. The
 * engine is zeroed at init, so it is applied after (session()). */
static enum ssh_forward_policy engine_policy = SSH_FORWARD_NONE;

static uint64_t now_ms(void)
{
    os64_ticks_t t;
    if (os64_ticks(&t) < 0 || !t.per_second) return 0;
    return t.ticks / t.per_second * 1000 + t.ticks % t.per_second * 1000 / t.per_second;
}
static void log_line(const char *text)
{
    os64_debug_log(text);
    os64_write(2, text, strlen(text)); os64_write(2, "\n", 1);
}
static int write_all(int32_t fd, const void *data, size_t n)
{
    const uint8_t *p = data;
    while (n) {
        int64_t wrote = os64_write(fd, p, n);
        if (wrote <= 0) return 0;
        p += wrote; n -= (size_t)wrote;
    }
    return 1;
}
static int entropy(uint8_t seed[32])
{
    int64_t fd = os64_open("/dev/random", 0);
    if (fd < 0) return 0;
    size_t n = 0;
    while (n < 32) {
        int64_t got = os64_read((int32_t)fd, seed + n, 32 - n);
        if (got <= 0) break;
        n += (size_t)got;
    }
    os64_close((int32_t)fd);
    if (n == 32) return 1;
    ssh_wipe(seed, 32); return 0;
}
static int host_key(uint8_t key[32], int generate)
{
    int64_t fd = os64_open(key_path, 0);
    if (fd >= 0) {
        char text[256]; size_t n = 0; int64_t got;
        while (n < sizeof(text) && (got = os64_read((int32_t)fd, text+n, sizeof(text)-n)) > 0) n += (size_t)got;
        int good = n < sizeof(text) && got == 0 && ssh_private_parse(text, n, key);
        os64_close((int32_t)fd); ssh_wipe(text, sizeof(text));
        if (!good) log_line("sshd: malformed or unreadable /home/sshd_host_key; identity preserved, startup refused");
        return good;
    }
    if (!generate) { log_line("sshd: cannot read persistent host key"); return 0; }
    uint8_t seed[32];
    if (!entropy(seed)) { log_line("sshd: host-key generation requires seeded /dev/random"); return 0; }
    br_hmac_drbg_context rng; br_hmac_drbg_init(&rng, &br_sha256_vtable, seed, 32);
    br_ec_private_key sk;
    size_t size = br_ec_keygen(&rng.vtable, &br_ec_p256_m31, &sk, key, BR_EC_secp256r1);
    ssh_wipe(seed, 32); ssh_wipe(&rng, sizeof(rng));
    if (size != 32) return 0;
    /* Exclusive creation distinguishes absence from an unreadable existing
     * identity. A failed/partial write leaves a refused identity, not a rekey. */
    fd = os64_open(key_path, "x");
    if (fd < 0) { log_line("sshd: cannot exclusively create persistent host key"); ssh_wipe(key, 32); return 0; }
    char text[160];
    const char hex[] = "0123456789abcdef";
    memcpy(text, "ecdsa-p256 ", 11);
    for (size_t i = 0; i < 32; i++) { text[11+i*2] = hex[key[i] >> 4]; text[12+i*2] = hex[key[i] & 15]; }
    text[75] = '\n';
    os64_time_t stamp = {0}; os64_time(&stamp);
    int more = os64_snprintf(text+76, sizeof(text)-76, "# generated epoch %ld\n", (long)stamp.epoch);
    int good = more > 0 && (size_t)more < sizeof(text)-76 && write_all((int32_t)fd, text, 76+(size_t)more);
    if (os64_close((int32_t)fd) < 0) good = 0;
    ssh_wipe(text, sizeof(text));
    if (!good) { log_line("sshd: host-key persistence failed; startup refused"); ssh_wipe(key, 32); }
    return good;
}
static void fingerprint(const uint8_t key[32])
{
    uint8_t point[65], blob[SSH_KEY_BLOB_MAX], digest[32]; char encoded[48], line[96];
    if (!ssh_host_public(key, point)) return;
    size_t n = ssh_public_blob(blob, sizeof(blob), point);
    br_sha256_context h; br_sha256_init(&h); br_sha256_update(&h, blob, n); br_sha256_out(&h, digest);
    size_t len = ssh_base64_encode(digest, 32, encoded, sizeof(encoded));
    while (len && encoded[len-1] == '=') encoded[--len] = 0;
    os64_snprintf(line, sizeof(line), "sshd: host key SHA256:%s", encoded); log_line(line);
}
static int authorized_keys(void)
{
    char path[OS64_CONF_PATH_MAX]; size_t cursor = 0;
    int64_t next;
    while ((next = os64_conf_find_from("authorized_keys", cursor, path, sizeof(path))) >= 1) {
        cursor = (size_t)next;
        int64_t fd = os64_open(path, 0);
        if (fd < 0) { log_line("sshd: authorized_keys open failed"); return 0; }
        char line[2048]; size_t used = 0, total = 0; int good = 1, overflow = 0;
        for (;;) {
            char c; int64_t got = os64_read((int32_t)fd, &c, 1);
            if (got < 0) { good = 0; break; }
            if (got == 1 && ++total > 131072) { good = 0; break; }
            if (!got || c == '\n') {
                int result = overflow ? -2 : ssh_authorized_line(&engine, line, used);
                if (result == -1) {
                    char note[160]; size_t k = 0, start = 0;
                    while (start < used && (line[start] == ' ' || line[start] == '\t')) start++;
                    while (start+k < used && k < 64 && line[start+k] > ' ' && line[start+k] <= '~') k++;
                    line[start+k] = 0;
                    os64_snprintf(note, sizeof(note), "sshd: skipping unsupported authorized key type %s", line+start); log_line(note);
                } else if (result < 0) {
                    log_line("sshd: refused authorized_keys line (options, malformed key, length or capacity)");
                    good = 0; break;
                }
                used = 0; overflow = 0;
                if (!got) break;
            } else if (used < sizeof(line)-1) line[used++] = c;
            else overflow = 1;
        }
        os64_close((int32_t)fd);
        if (!good) return 0;
    }
    return engine.key_count > 0;
}
static void ignore_pipe(int signo) { (void)signo; }
static int64_t input_worker(void *unused)
{
    (void)unused;
    uint8_t chunk[4096];
    for (;;) {
        uint32_t head = input_head, tail = __atomic_load_n(&input_tail, __ATOMIC_ACQUIRE);
        size_t n = (uint32_t)(tail-head); if (n > sizeof(chunk)) n = sizeof(chunk);
        if (!n) {
            if (__atomic_load_n(&input_done, __ATOMIC_ACQUIRE)) {
                /* SSH EOF closes exec stdin. A STREAM PTY has no separate
                 * write half; Ctrl-D remains the interactive EOF operation. */
                if (master < 0) os64_close(child_input);
                return 0;
            }
            os64_sleep(10); continue;
        }
        for (size_t i = 0; i < n; i++) chunk[i] = input_ring[(head+i) % SSH_WINDOW];
        int64_t wrote = os64_write(child_input, chunk, n);
        if (wrote < 0) { __atomic_store_n(&input_broken, 1, __ATOMIC_RELEASE); return 0; }
        if (!wrote) { os64_sleep(10); continue; }
        __atomic_store_n(&input_head, head + (uint32_t)wrote, __ATOMIC_RELEASE);
    }
}
static int64_t output_worker(void *arg)
{
    child_stream *stream = arg;
    uint8_t data[4096], terminal[8192];
    int previous_cr = 0;
    for (;;) {
        int64_t got = os64_read(stream->fd, data, sizeof(data));
        if (got == OS64_INTERRUPTED) continue;
        if (got <= 0) {
            if (got < 0) __atomic_store_n(&stream->error, 1, __ATOMIC_RELEASE);
            __atomic_store_n(&stream->eof, 1, __ATOMIC_RELEASE); return 0;
        }
        const uint8_t *bytes = data;
        if (master >= 0) {
            /* os64 renders LF as a new row at column zero. An SSH client's
             * raw terminal needs CR LF for that motion; preserve existing
             * CR LF pairs, including pairs split between reads. Exec pipes
             * bypass this terminal adaptation and remain byte-exact. */
            size_t count = 0;
            for (size_t i = 0; i < (size_t)got; i++) {
                uint8_t c = data[i];
                if (c == '\n' && !previous_cr) terminal[count++] = '\r';
                terminal[count++] = c; previous_cr = c == '\r';
            }
            bytes = terminal; got = (int64_t)count;
        }
        size_t off = 0;
        while (off < (size_t)got) {
            uint32_t tail = stream->tail;
            uint32_t head = __atomic_load_n(&stream->head, __ATOMIC_ACQUIRE);
            size_t n = CHILD_RING_CAP - (uint32_t)(tail-head);
            if (n > (size_t)got-off) n = (size_t)got-off;
            if (!n) { os64_sleep(10); continue; }
            for (size_t i = 0; i < n; i++) stream->bytes[(tail+i) % CHILD_RING_CAP] = bytes[off+i];
            __atomic_store_n(&stream->tail, tail+(uint32_t)n, __ATOMIC_RELEASE); off += n;
        }
    }
}
static int spawn_command(int interactive)
{
    if (interactive) {
        master = (int32_t)os64_pty_create_stream((uint16_t)engine.cols, (uint16_t)engine.rows);
        if (master < 0) return 0;
        /* The environment copies downward at spawn, so TERM set here reaches
         * the seated shell and everything it runs. */
        const char *term = ssh_term_env(&engine);
        if (term) os64_setenv("TERM", term);
        char *args[] = {"/bin/husk", 0};
        child = os64_spawn_seated("/bin/husk", args, master);
        child_input = child_output = master;
        if (child < 0) { os64_close(master); master = -1; return 0; }
    } else {
        int32_t in[2] = {-1,-1}, out[2] = {-1,-1}, err[2] = {-1,-1};
        if (os64_pipe(in) < 0 || os64_pipe(out) < 0 || os64_pipe(err) < 0) goto failed;
        char *args[] = {"/bin/husk", "-c", engine.command, 0};
        child = os64_spawn_redirected("/bin/husk", args, in[0], out[1], err[1], 0);
        if (child < 0) goto failed;
        os64_close(in[0]); os64_close(out[1]); os64_close(err[1]);
        child_input = in[1]; child_output = out[0]; child_error = err[0];
        goto spawned;
failed:
        for (int i = 0; i < 2; i++) {
            if (in[i] >= 0) os64_close(in[i]);
            if (out[i] >= 0) os64_close(out[i]);
            if (err[i] >= 0) os64_close(err[i]);
        }
        return 0;
    }
spawned:
    streams[0].fd = child_output; streams[1].fd = child_error;
    for (int i = 0; i < 2; i++) {
        if (streams[i].fd < 0) { streams[i].eof = 1; continue; }
        if (os64_thread(output_worker, &streams[i]) < 0) {
            ssh_disconnect(&engine, 11, "cannot start command output worker"); return 0;
        }
    }
    if (os64_thread(input_worker, 0) < 0) {
        /* Ending the session tears down its descriptors and worker together. */
        ssh_disconnect(&engine, 11, "cannot start command input worker"); return 0;
    }
    return 1;
}
static void link_close(uint32_t i)
{
    forward_link *l = &links[i];
    if (l->handle >= 0) os64_close(l->handle);
    if (l->to_local) os64_free(l->to_local);
    if (l->from_local) os64_free(l->from_local);
    l->handle = -1; l->to_local = l->from_local = 0;
    l->to_len = l->from_len = 0; l->local_ended = l->client_eof = l->peer_closed = 0;
}
/* Judge and dial a direct-tcpip destination. The check is on the ADDRESS the
 * name resolves to, never its spelling: `localhost` passes because /etc/hosts
 * says 127.0.0.1, and a name that resolves elsewhere is refused whatever it
 * looks like. */
static void forward_open(uint32_t i)
{
    ssh_forward *f = &engine.forwards[i];
    char note[400];
    uint32_t ip = 0;
    int64_t rc = os64_resolve(f->host, &ip);
    if (rc < 0) { ssh_forward_result(&engine, i, 0, 2, os64_dial_reason(rc)); return; }
    if ((ip >> 24) != 127) {
        os64_snprintf(note, sizeof(note), "sshd: refused forward to %s:%u (not loopback)", f->host, (unsigned)f->port);
        log_line(note);
        ssh_forward_result(&engine, i, 0, 1, "forwarding reaches this machine's loopback only"); return;
    }
    link_close(i);
    links[i].to_local = os64_malloc(SSH_FORWARD_WINDOW);
    links[i].from_local = os64_malloc(SSH_DATA_MAX);
    if (!links[i].to_local || !links[i].from_local) {
        link_close(i); ssh_forward_result(&engine, i, 0, 4, "out of memory for a forward"); return;
    }
    os64_netdest_t dest = {.ip = ip, .port = (uint16_t)f->port, .protocol = OS64_NET_TCP};
    int64_t h = os64_net_dial(&dest);
    if (h < 0) { link_close(i); ssh_forward_result(&engine, i, 0, 2, os64_dial_reason(h)); return; }
    links[i].handle = (int32_t)h;
    os64_snprintf(note, sizeof(note), "sshd: forwarding to %s:%u", f->host, (unsigned)f->port); log_line(note);
    ssh_forward_result(&engine, i, 1, 0, "");
}
/* One turn of every live forward, both directions, never blocking. Returns 0
 * when nothing moved; bit 0 when something did, which lets the loop skip its
 * nap, and bit 1 when a forward sent bytes, which is what the session loop's
 * turn-taking counts.
 *
 * Forwards share one output queue, so the order they are served in is who
 * gets it when it is short. Round robin: the next pass starts at the forward
 * where the queue ran out, the first one left holding bytes because no room
 * was left for them (ssh_output_full). A forward held back by its own limits,
 * the client's window for it or its packet size, is not that point, or it
 * would pin the rotation and hand every refill to the forward after it. That
 * keeps every busy forward within one staging buffer of the others however
 * long they run. Starting after the first forward that SENT (the rule the
 * two session streams use) is exact for two and drifts without bound for
 * three or more. */
static uint32_t next_forward;
static int forwards_pass(void)
{
    int moved = 0, cut = 0, sent_any = 0;
    uint32_t first = next_forward;
    for (uint32_t k = 0; k < SSH_FORWARDS; k++) {
        uint32_t i = (first + k) % SSH_FORWARDS;
        forward_link *l = &links[i];
        if (l->handle < 0) continue;
        ssh_forward_consumed(&engine, i, 0);   // credit the output queue had no room for
        if (l->to_len && !l->local_ended) {
            int64_t wrote = os64_write_for(l->handle, l->to_local, l->to_len, 0);
            if (wrote > 0) {
                l->to_len -= (uint32_t)wrote; memmove(l->to_local, l->to_local + wrote, l->to_len);
                ssh_forward_consumed(&engine, i, (uint32_t)wrote); moved = 1;
            } else if (wrote != OS64_ERR_TIMEOUT && wrote != OS64_INTERRUPTED) {
                /* The local end is gone: what the client sent can no longer
                 * be delivered, and the channel ends as a read EOF would. */
                l->local_ended = 1; l->to_len = 0;
            }
        }
        /* A closed channel takes nothing more from the local end; what the
         * client sent before its CLOSE is delivered first, and then the
         * slot is the engine's to reuse. */
        if (l->peer_closed) {
            if (!l->to_len || l->local_ended) { link_close(i); ssh_forward_release(&engine, i); moved = 1; }
            continue;
        }
        /* The client's EOF, once its bytes are delivered, ends the local
         * connection whole: there is no ring-3 half-close to pass on, so a
         * reply the local end would have sent after it is lost (DEBTS). A
         * client that closes fully, which is what a viewer does, loses
         * nothing. */
        if (l->client_eof && !l->to_len) l->local_ended = 1;
        if (!l->local_ended && !l->from_len) {
            int64_t got = os64_read_for(l->handle, l->from_local, SSH_DATA_MAX, 0);
            if (got > 0) { l->from_len = (uint32_t)got; moved = 1; }
            else if (got != OS64_ERR_TIMEOUT && got != OS64_INTERRUPTED) l->local_ended = 1;
        }
        if (l->from_len) {
            size_t sent = ssh_forward_send(&engine, i, l->from_local, l->from_len);
            if (sent) { l->from_len -= (uint32_t)sent; memmove(l->from_local, l->from_local + sent, l->from_len); moved = 1; sent_any = 2; }
            if (l->from_len && !cut && ssh_output_full(&engine)) { next_forward = i; cut = 1; }
        }
        /* Everything read has been sent: the channel follows the connection
         * into EOF and CLOSE. The slot stays the engine's until the client's
         * CLOSE crosses ours (SSH_EVENT_FORWARD_CLOSE). */
        if (l->local_ended && !l->from_len && ssh_forward_finish(&engine, i)) {
            link_close(i); moved = 1;
        }
    }
    return moved | sent_any;
}
static void resize_terminal(void)
{
    /* Before shell startup the accepted proposal supplies its initial size.
     * A live terminal must apply it before the engine acknowledges success. */
    int good = master < 0 ? !engine.started :
        os64_pty_resize((int32_t)master, (uint16_t)engine.resize_cols,
                        (uint16_t)engine.resize_rows) >= 0;
    ssh_resize_result(&engine, good);
}
static void event(void)
{
    switch (engine.event) {
    case SSH_EVENT_AUTH: {
        char note[320]; os64_snprintf(note, sizeof(note), "sshd: publickey accepted, requested username=%s", engine.username); log_line(note); break;
    }
    case SSH_EVENT_EXEC: ssh_start_result(&engine, spawn_command(0)); break;
    case SSH_EVENT_SHELL: ssh_start_result(&engine, spawn_command(1)); break;
    case SSH_EVENT_RESIZE:
        resize_terminal();
        break;
    case SSH_EVENT_INPUT: {
        if (__atomic_load_n(&input_broken, __ATOMIC_ACQUIRE)) {
            ssh_input_consumed(&engine, (uint32_t)engine.event_len); break;
        }
        uint32_t tail = input_tail, head = __atomic_load_n(&input_head, __ATOMIC_ACQUIRE);
        if (engine.event_len > SSH_WINDOW - (uint32_t)(tail-head)) {
            ssh_disconnect(&engine, 2, "stdin queue exceeds advertised window"); break;
        }
        for (size_t i = 0; i < engine.event_len; i++) input_ring[(tail+i) % SSH_WINDOW] = engine.event_data[i];
        __atomic_store_n(&input_tail, tail + (uint32_t)engine.event_len, __ATOMIC_RELEASE); break;
    }
    case SSH_EVENT_EOF: __atomic_store_n(&input_done, 1, __ATOMIC_RELEASE); break;
    case SSH_EVENT_FORWARD_OPEN: forward_open(engine.event_forward); break;
    case SSH_EVENT_FORWARD_DATA: {
        forward_link *l = &links[engine.event_forward];
        /* A dead local end discards, and the window is credited back so the
         * client is not left waiting on bytes nobody will take. */
        if (l->handle < 0 || l->local_ended) { ssh_forward_consumed(&engine, engine.event_forward, (uint32_t)engine.event_len); break; }
        if (engine.event_len > SSH_FORWARD_WINDOW - l->to_len) {
            ssh_disconnect(&engine, 2, "forward queue exceeds advertised window"); break;
        }
        memcpy(l->to_local + l->to_len, engine.event_data, engine.event_len);
        l->to_len += (uint32_t)engine.event_len; break;
    }
    /* A half-close ends the local connection once the queue drains
     * (forwards_pass). */
    case SSH_EVENT_FORWARD_EOF: links[engine.event_forward].client_eof = 1; break;
    /* A link already closed (our CLOSE went first, after the local end
     * ended) owes nothing; an open one delivers its queue before closing
     * (forwards_pass). Bytes read for the client can no longer be sent. */
    case SSH_EVENT_FORWARD_CLOSE: {
        forward_link *l = &links[engine.event_forward];
        if (l->handle < 0) ssh_forward_release(&engine, engine.event_forward);
        else { l->peer_closed = 1; l->from_len = 0; }
        break;
    }
    default: break;
    }
}
static int flush(void)
{
    const uint8_t *p; size_t n = ssh_output(&engine, &p);
    if (!n) return 1;
    int64_t wrote = os64_write_for(1, p, n, 0);
    /* Caught signals leave the queued bytes intact for the next turn. */
    if (wrote == OS64_ERR_TIMEOUT || wrote == OS64_INTERRUPTED) return 1;
    if (wrote <= 0) return 0;
    ssh_output_consume(&engine, (size_t)wrote); return 1;
}
static int session(void)
{
    uint8_t seed[32], key[32];
    if (!entropy(seed) || !host_key(key, 0)) {
        /* Identification and plaintext DISCONNECT need no random packet
         * padding before keys exist (RFC 4253 recommends random padding).
         * Do not initialize a cryptographic session with substitute entropy. */
        const char ident[] = SSH_IDENT "\r\n";
        uint8_t packet[128]; ssh_writer w = {packet, 5, sizeof(packet), 0};
        ssh_put_byte(&w, 1); ssh_put_u32(&w, 11);
        ssh_put_text(&w, "host key or seeded entropy unavailable"); ssh_put_text(&w, "");
        size_t padding = 8 - w.n % 8; if (padding < 4) padding += 8;
        memset(packet+w.n, 0, padding); w.n += padding; packet[4] = (uint8_t)padding;
        ssh_writer length = {packet, 0, 4, 0}; ssh_put_u32(&length, (uint32_t)w.n - 4);
        os64_write_for(1, ident, sizeof(ident)-1, 1000); os64_write_for(1, packet, w.n, 1000);
        ssh_wipe(seed, 32); ssh_wipe(key, 32); return 1;
    }
    int good = ssh_init(&engine, seed, key);
    ssh_wipe(seed, 32); ssh_wipe(key, 32);
    if (!good) return 1;
    engine.forward_policy = engine_policy;
    if (!authorized_keys()) ssh_disconnect(&engine, 14, "no usable authorized keys or key file refused");
    os64_signal_set_handler(OS64_SIGPIPE, ignore_pipe);
    for (uint32_t i = 0; i < SSH_FORWARDS; i++) links[i].handle = -1;
    uint8_t net[8192], out[2][4096]; size_t net_len = 0, net_off = 0, out_len[2] = {0,0};
    int eof[2] = {0,0}, exited = 0, close_received = 0; int32_t status = 0;
    uint64_t begin = now_ms(), last_progress = begin, key_time = begin, kex_begin = begin, close_time = 0;
    int had_kex = engine.kex;
    unsigned next_stream = 0;
    /* The session's streams and the forwards take turns going first for the
     * output queue's room, by the streams' own rule: whichever group went
     * first and spent room hands the lead to the other. Going first every
     * time, either would take all of a trickle of room and starve the other.
     * The lead passes because every channel sends what fits the room
     * (ssh_connection.c fit_room): a leader with anything to send spends
     * some, however little is free. */
    int forwards_lead = 0;
    for (;;) {
        size_t queued = engine.out_len;
        if (!flush()) break;
        uint64_t now = now_ms();
        int moved = queued && engine.out_len < queued;
        if (!queued || engine.out_len < queued) last_progress = now;
        /* An in-flight channel close can owe a deferred reply. Keep reading
         * the active KEX until NEWKEYS releases it, or its deadline expires.
         * A closed session is not the end while forwards are live: the
         * client ends those itself, then the connection. */
        if (engine.closed || (close_received && !engine.kex && !ssh_forwards_live(&engine))) {
            if (!engine.out_len || now - last_progress > 5000) break;
            os64_sleep(10); continue;
        }
        if (!engine.authenticated && now - begin >= 120000) ssh_disconnect(&engine, 14, "authentication timeout");
        if (engine.out_len && now - last_progress > 30000) break;
        if (!had_kex && engine.kex) kex_begin = now;
        if (had_kex && !engine.kex) key_time = now;
        had_kex = engine.kex;
        if (engine.kex && now - kex_begin > 120000) ssh_disconnect(&engine, 3, "key exchange timeout");
        /* Key-use limits hold for the connection, not the session channel:
         * forwards outlive the session. Only a connection already ending
         * skips the rekey. */
        int ending = (close_received || engine.sent_close) && !ssh_forwards_live(&engine);
        if (!ending && !engine.kex && (now-key_time >= 3600000 || engine.tx.bytes >= 536870912 || engine.rx.bytes >= 536870912)) {
            ssh_rekey(&engine); kex_begin = now; had_kex = engine.kex;
        }
        if (net_off == net_len) {
            int64_t n = os64_read_for(0, net, sizeof(net), 0);
            if (n == 0 || (n < 0 && n != OS64_ERR_TIMEOUT && n != OS64_INTERRUPTED)) break;
            net_off = net_len = 0; if (n > 0) { net_len = (size_t)n; moved = 1; }
        }
        if (net_off < net_len) {
            size_t took = ssh_receive(&engine, net+net_off, net_len-net_off);
            net_off += took; if (took) moved = 1;
            event();
            if (engine.event == SSH_EVENT_CLOSE) close_received = 1;
        }
        int forwards_went_first = forwards_lead;
        if (forwards_went_first) {
            int result = forwards_pass();
            if (result) moved = 1;
            if (result & 2) forwards_lead = 0;
        }
        if (engine.started) {
            /* ssh_input_consumed keeps the credit owed until it can go out
             * (not during a key exchange, not without output room), so every
             * pass reports what left and a zero retries. */
            uint32_t head = __atomic_load_n(&input_head, __ATOMIC_ACQUIRE);
            ssh_input_consumed(&engine, head-credited); credited = head;
            unsigned first_stream = next_stream; int rotated = 0;
            for (unsigned pass = 0; pass < 2; pass++) {
                unsigned i = (first_stream + pass) % 2;
                child_stream *stream = &streams[i];
                if (!out_len[i]) {
                    uint32_t head = stream->head;
                    int ended = __atomic_load_n(&stream->eof, __ATOMIC_ACQUIRE);
                    uint32_t tail = __atomic_load_n(&stream->tail, __ATOMIC_ACQUIRE);
                    size_t n = (uint32_t)(tail-head); if (n > sizeof(out[i])) n = sizeof(out[i]);
                    for (size_t k = 0; k < n; k++) out[i][k] = stream->bytes[(head+k) % CHILD_RING_CAP];
                    __atomic_store_n(&stream->head, head+(uint32_t)n, __ATOMIC_RELEASE);
                    out_len[i] = n; eof[i] = ended && !n;
                    if (__atomic_load_n(&stream->error, __ATOMIC_ACQUIRE)) ssh_disconnect(&engine, 11, "command output read failed");
                }
                if (out_len[i]) {
                    size_t n = ssh_send_data(&engine, out[i], out_len[i], i);
                    /* Rotate once per pass, on credit spent: the first stream
                     * to send hands priority to the other. Idle turns must
                     * not rotate, or sparse credit favors one stream; a
                     * second send in the same pass must not rotate either,
                     * or credit just over one staging buffer splits 4096:1
                     * the same way every time. */
                    if (n && !rotated) { next_stream = i ^ 1u; rotated = 1; }
                    if (n && !forwards_went_first) forwards_lead = 1;
                    if (n) moved = 1;
                    out_len[i] -= n; memmove(out[i], out[i]+n, out_len[i]);
                }
            }
            if (!exited) {
                int64_t done = os64_reap(&status);
                if (done == child) exited = 1;
            }
            if (exited && eof[0] && eof[1] && !out_len[0] && !out_len[1]) ssh_send_exit(&engine, (uint32_t)status);
            if (engine.sent_close && !engine.kex && !ssh_forwards_live(&engine)) {
                if (!close_time) close_time = now;
                if (!engine.out_len && now-close_time > 5000) break;
            }
        }
        if (!forwards_went_first && forwards_pass()) moved = 1;
        /* Nap only on a pass that moved nothing. A forward carries a screen's
         * worth of pixels, and a nap per pass capped it at one read per tick.
         * Taking bytes the engine buffered counts as moving: ssh_receive
         * takes one packet per call, so a read holding several would
         * otherwise cost a tick for each packet after the first. */
        os64_sleep(moved ? 0 : 1);
    }
    if (engine.error[0]) log_line(engine.error);
    /* Returning tears down the task, including a worker parked in a pipe
     * write. The kernel's handle pin keeps that operation's object alive. */
    ssh_wipe(&engine, sizeof(engine));
    return 0;
}
typedef struct { uint32_t port; enum ssh_forward_policy forward; int bad; } sshd_settings;
/* Every key sshd.conf may hold; an unknown key or value refuses the file
 * rather than being read as less than the operator wrote. */
static bool setting(const char *name, const char *value, void *user)
{
    sshd_settings *c = user;
    if (name && os64_streq(name, "port")) {
        uint32_t p = 0;
        if (!*value) { c->bad = 1; return false; }
        for (const char *d = value; *d; d++) {
            if (*d < '0' || *d > '9' || p > 6553) { c->bad = 1; return false; }
            p = p*10 + (unsigned)(*d-'0');
        }
        if (!p || p > 65535) { c->bad = 1; return false; }
        c->port = p; return true;
    }
    if (name && os64_streq(name, "forward")) {
        if (os64_streq(value, "loopback")) c->forward = SSH_FORWARD_LOOPBACK;
        else if (os64_streq(value, "none")) c->forward = SSH_FORWARD_NONE;
        else { c->bad = 1; return false; }
        return true;
    }
    c->bad = 1; return false;
}
/* What sshd.conf configures, into `out`; returns 0 with the reason logged
 * when it cannot be used. Resolve, then read: only a file the ladder does not
 * name permits the defaults (port 22, forwarding to loopback). One it names
 * but cannot read is the operator's word unread, and listening anyway would
 * be inventing a configuration. */
static int configured(sshd_settings *out)
{
    sshd_settings c = {22, SSH_FORWARD_LOOPBACK, 0}; char conf_path[OS64_CONF_PATH_MAX];
    int found = os64_conf_find("sshd.conf", conf_path, sizeof(conf_path)) == 0;
    int64_t config = found ? os64_conf_read(conf_path, setting, &c) : OS64_CONF_NO_FILE;
    if (found && config == OS64_CONF_NO_FILE) { log_line("sshd: sshd.conf found but unreadable; startup refused"); return 0; }
    if (c.bad || (found && config < 0)) { log_line("sshd: invalid sshd.conf"); return 0; }
    *out = c; return 1;
}
int main(int argc, char **argv)
{
    /* A session is told the forwarding policy by the listener that read
     * sshd.conf, so every connection it accepted obeys the same file. */
    if (argc == 3 && os64_streq(argv[1], "-session")) {
        if (os64_streq(argv[2], "loopback")) engine_policy = SSH_FORWARD_LOOPBACK;
        else if (!os64_streq(argv[2], "none")) return 2;
        return session();
    }
    if (argc != 1) { log_line("usage: sshd (port and forward in sshd.conf)"); return 2; }
    sshd_settings conf;
    if (!configured(&conf)) return 2;
    uint32_t port = conf.port;
    os64_netdest_t local = {.ip=0, .port=(uint16_t)port, .protocol=OS64_NET_TCP};
    int64_t listener = os64_net_announce(&local);
    if (listener < 0) { log_line("sshd: cannot announce configured port"); return 1; }
    uint8_t key[32];
    if (!host_key(key, 1)) { os64_close((int32_t)listener); return 1; }
    fingerprint(key); ssh_wipe(key, 32);
    char message[96];
    os64_snprintf(message, sizeof(message), "sshd: listening on port %u, %s", (unsigned)port,
                  conf.forward == SSH_FORWARD_LOOPBACK ? "forwarding to loopback" : "forwarding disabled");
    log_line(message);
    unsigned sessions = 0;
    for (;;) {
        while (os64_reap(0) > 0) if (sessions) sessions--;
        os64_netconn_t conn;
        int64_t n = os64_read_for((int32_t)listener, &conn, sizeof(conn), 1000);
        if (n == OS64_ERR_TIMEOUT || n == OS64_INTERRUPTED) continue;
        if (n != sizeof(conn)) break;
        /* Each unauthenticated connection owns bounded but substantial crypto
         * and input storage. Cap children before allocating another session. */
        if (sessions < 16) {
            char *args[] = {"/bin/sshd", "-session", conf.forward == SSH_FORWARD_LOOPBACK ? "loopback" : "none", 0};
            if (os64_spawn_redirected("/bin/sshd", args, conn.handle, conn.handle, -1, 0) >= 0) sessions++;
        }
        os64_close(conn.handle);
    }
    os64_close((int32_t)listener); return 1;
}
