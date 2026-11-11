// h2_conn.c — see h2_conn.h.
//
// ONE LOCK guards a connection: the TLS engine, the out queue, the stream
// table and every stream's state, the windows, what the peer set, the
// encoder, and the flags that say how it is ending. It is held for engine
// calls and bookkeeping and never across a call that can block; a bell is
// rung under it, so that a fetch can take its bell away under it.
//
// THE READER OWNS, WITH NO LOCK: the plaintext it is cutting frames from,
// the reassembler, the HPACK decoder and the head it is decoding into. Only
// the reader decodes, so a block is decoded outside the lock and the lock is
// taken to hand the result to a stream.
//
// LOCK ORDER: a caller's own lock (the pool's), then this one. The hooks are
// called with this one let go for that reason.
#include "h2_conn.h"

#include "os64/io.h"
#include "os64/lock.h"
#include "os64/mem.h"
#include "os64/proc.h"
#include "os64/signal.h"
#include "os64/str.h"
#include "os64/thread.h"

#define OUT_BLOCK      16384u
// What we cut our own frames to: the size every peer takes.
#define OUR_FRAME      H2_FRAME_SIZE_LEAST
#define FIELDS_MOST    512u
// The most a stream's receive buffer keeps once it has been read empty.
#define BUF_KEEP       (64u * 1024u)
// The largest frame, and the largest header table, a greeting may promise to
// take: each is memory this connection sets aside the moment it is adopted.
#define GREETING_MOST  (1024u * 1024u)
#define WINDOW_MOST    ((int64_t)H2_STREAM_MOST)

typedef struct out_block {
    struct out_block *next;
    uint32_t at, len;
    uint8_t bytes[OUT_BLOCK];
} out_block_t;

struct h2_stream {
    h2_conn_t *conn;
    uint32_t id;
    os64_bell_t *bell;              // the fetch's; rung under the connection's lock, taken away under it
    bool sent_end, got_end;         // each side's END_STREAM
    bool bodiless;                  // the request was a HEAD: its response has no content
    bool no_content;                // the final head's verdict: bodiless, or a 204 or 304
    bool peer_reset;                // the peer reset it, before or after its END_STREAM
    h2_ending_t ending;
    // The final head: its bytes, and the fields that point into them.
    uint8_t *head;
    h2_field_t *fields;
    uint32_t field_count;
    bool head_ready;
    bool has_length;
    uint64_t length, body_seen;
    // The body that has arrived and not been read: [at, len) of `buf`.
    uint8_t *buf;
    uint32_t room, at, len;
    int64_t recv_window, send_window;
    uint32_t credit;                // consumed since the stream's last WINDOW_UPDATE
    uint32_t weight;                // its priority weight, 1 to 256, for the streams opened after it
};

struct h2_conn {
    os64_lock_t lock;
    uint32_t holds;                 // counted atomically: the adopter's, the reader's, each stream's
    os64_tls_client *tls;
    int32_t socket;
    h2_conn_hooks_t hooks;

    // The out queue: ciphertext in the order the engine made it. One thread
    // at a time drains it (`draining`), writing with the lock let go.
    out_block_t *out_head, *out_tail;
    size_t out_bytes;
    bool draining;
    bool refused;                   // the socket would not take the queue's head
    uint64_t refused_at;            // and since when, in milliseconds
    h2_wait_t *door;                // callers parked until the queue is under its mark

    h2_stream_t *streams[H2_STREAMS_MOST];
    uint32_t stream_count, next_id;
    uint32_t first_id;              // the id its first stream took: no stream of ours is below it

    uint32_t peer_max_streams, peer_max_frame;
    int64_t peer_initial_window;
    bool peer_settings_seen;
    uint32_t our_max_frame, our_table;
    int64_t our_initial_window, our_window;
    // What a stream's receive window is once its opening WINDOW_UPDATE has
    // gone: our_initial_window and the greeting's stream increment.
    int64_t our_stream_window;
    uint32_t stream_increment;
    h2_priority_mode_t priority;
    bool our_settings_acked;
    int64_t send_window, recv_window;
    uint64_t recv_credit;           // consumed or discarded since the connection's last WINDOW_UPDATE

    h2_hpack_encoder_t encoder;
    void *encoder_store;
    uint8_t scratch[H2_FRAME_HEADER_LEN + OUR_FRAME];   // one small frame or one DATA frame, under the lock

    bool dying, dead, goaway, quiet;
    uint32_t goaway_last;
    uint32_t die_code;              // the error code that ended it, when one did
    uint64_t frames_in;

    // The reader's alone.
    h2_hpack_decoder_t decoder;
    void *decoder_store;
    uint8_t *decoder_scratch;
    uint8_t *plain;
    uint32_t plain_room, plain_len;
    uint8_t *block_room;
    h2_block_t block;
    uint8_t *head_buf;
    uint32_t head_len;
    h2_field_t *head_fields;
    uint32_t head_count;
    bool head_refused;
    uint8_t unread[4096];
    size_t unread_len;
};

static bool fatal(os64_tls_status_t status)
{
    return status != OS64_TLS_OK && status != OS64_TLS_NEED_PROGRESS && status != OS64_TLS_CLEAN_EOF;
}

static bool now_ms(uint64_t *ms)
{
    os64_ticks_t t;
    if (os64_ticks(&t) < 0 || t.per_second == 0)
        return false;
    *ms = t.ticks / t.per_second * 1000u + t.ticks % t.per_second * 1000u / t.per_second;
    return true;
}

static void hold(h2_conn_t *c) { __atomic_add_fetch(&c->holds, 1, __ATOMIC_ACQ_REL); }

static void let_go(h2_conn_t *c)
{
    if (__atomic_sub_fetch(&c->holds, 1, __ATOMIC_ACQ_REL) != 0)
        return;
    for (out_block_t *b = c->out_head; b != NULL;) {
        out_block_t *next = b->next;
        os64_free(b);
        b = next;
    }
    os64_tls_free(c->tls);
    os64_free(c->encoder_store);
    os64_free(c->decoder_store);
    os64_free(c->decoder_scratch);
    os64_free(c->plain);
    os64_free(c->block_room);
    os64_free(c->head_buf);
    os64_free(c->head_fields);
    os64_free(c);
}

// ── Under the lock ──────────────────────────────────────────────────────

static void ring(h2_stream_t *s)
{
    if (s->bell != NULL)
        os64_bell_ring(s->bell);
}

static void ring_all(h2_conn_t *c)
{
    for (uint32_t i = 0; i < H2_STREAMS_MOST; i++)
        if (c->streams[i] != NULL)
            ring(c->streams[i]);
}

static void ring_door(h2_conn_t *c)
{
    for (h2_wait_t *w = c->door; w != NULL; w = w->next)
        os64_bell_ring(w->bell);
}

static void door_add(h2_conn_t *c, h2_wait_t *w)
{
    w->next = c->door;
    c->door = w;
}

static void door_remove(h2_conn_t *c, h2_wait_t *w)
{
    for (h2_wait_t **link = &c->door; *link != NULL; link = &(*link)->next)
        if (*link == w) {
            *link = w->next;
            break;
        }
    w->next = NULL;
}

// The connection is to end. Senders send nothing more; the reader closes it.
static void die(h2_conn_t *c, uint32_t code)
{
    if (c->dying)
        return;
    c->dying = true;
    c->die_code = code;
}

// Move what the engine has ready to the queue's tail.
static void pump(h2_conn_t *c)
{
    for (;;) {
        os64_tls_state_t state = os64_tls_state(c->tls);
        if (fatal(state.status)) {
            die(c, H2_ERR_INTERNAL);
            return;
        }
        if (!(state.flags & OS64_TLS_SEND_CIPHER))
            return;
        out_block_t *b = c->out_tail;
        if (b == NULL || b->len == OUT_BLOCK) {
            b = os64_malloc(sizeof(*b));
            if (b == NULL) {
                die(c, H2_ERR_INTERNAL);
                return;
            }
            b->next = NULL;
            b->at = b->len = 0;
            if (c->out_tail != NULL)
                c->out_tail->next = b;
            else
                c->out_head = b;
            c->out_tail = b;
        }
        os64_tls_transfer_t took = os64_tls_take_ciphertext(c->tls, b->bytes + b->len, OUT_BLOCK - b->len);
        if (fatal(took.status) || took.transferred == 0) {
            die(c, H2_ERR_INTERNAL);
            return;
        }
        b->len += (uint32_t)took.transferred;
        c->out_bytes += took.transferred;
    }
}

// Plaintext frames into the engine and their ciphertext onto the queue.
// False, with the connection dying, when the engine would not take them.
static bool emit(h2_conn_t *c, const uint8_t *frames, size_t len)
{
    if (c->dying)
        return false;
    for (size_t done = 0; done < len;) {
        os64_tls_transfer_t wrote = os64_tls_write_plaintext(c->tls, frames + done, len - done);
        done += wrote.transferred;
        os64_tls_flush(c->tls);
        pump(c);
        if (c->dying)
            return false;
        if (fatal(wrote.status) || wrote.transferred == 0) {
            die(c, H2_ERR_INTERNAL);
            return false;
        }
    }
    return true;
}

// A frame a PEER made us owe: an acknowledgement, a reset of its stream, a
// window given back for bytes it sent. A peer that makes us owe them faster
// than its socket takes them is ended at the frame that finds the queue
// past its ceiling. False when the frame did not go.
static bool emit_owed(h2_conn_t *c, const uint8_t *frame, size_t len)
{
    if (c->out_bytes > (size_t)H2_OUT_MARK + H2_OUT_CONTROL_MOST) {
        die(c, H2_ERR_ENHANCE_YOUR_CALM);
        return false;
    }
    return emit(c, frame, len);
}

// A connection error of ours: GOAWAY with the code, and the end.
static void conn_error(h2_conn_t *c, uint32_t code)
{
    if (c->dying)
        return;
    size_t n = h2_write_goaway(c->scratch, sizeof(c->scratch), 0, code, NULL, 0);
    emit(c, c->scratch, n);
    die(c, code);
}

static h2_stream_t *find(h2_conn_t *c, uint32_t id)
{
    for (uint32_t i = 0; i < H2_STREAMS_MOST; i++)
        if (c->streams[i] != NULL && c->streams[i]->id == id)
            return c->streams[i];
    return NULL;
}

// A stream we opened at some time: odd, and below the next id to give out.
static bool was_ours(const h2_conn_t *c, uint32_t id)
{
    return (id & 1u) != 0 && id >= c->first_id && id < c->next_id;
}

static bool over(const h2_stream_t *s) { return s->ending.how != H2_END_NOT_YET; }

// A stream error of ours: RST_STREAM with the code, and that stream's end.
static void reset_stream(h2_conn_t *c, h2_stream_t *s, uint32_t code, h2_end_t how)
{
    if (over(s))
        return;
    size_t n = h2_write_rst_stream(c->scratch, sizeof(c->scratch), s->id, code);
    emit_owed(c, c->scratch, n);
    s->ending.how = how;
    s->ending.code = code;
    ring(s);
}

// Bytes of the connection's window that nobody will read, or that somebody
// has: said back to the peer once they are half the window.
static void credit_conn(h2_conn_t *c, uint64_t bytes)
{
    c->recv_credit += bytes;
    if (c->recv_credit == 0 || (int64_t)c->recv_credit < c->our_window / 2)
        return;
    uint32_t increment = c->recv_credit > H2_STREAM_MOST ? H2_STREAM_MOST : (uint32_t)c->recv_credit;
    size_t n = h2_write_window_update(c->scratch, sizeof(c->scratch), 0, increment);
    if (emit_owed(c, c->scratch, n)) {
        c->recv_window += increment;
        c->recv_credit -= increment;
    }
}

static void credit_stream(h2_conn_t *c, h2_stream_t *s, uint32_t bytes)
{
    s->credit += bytes;
    // A stream the peer has finished, or that is over, wants no more window.
    if (s->got_end || over(s) || s->credit == 0 || (int64_t)s->credit < c->our_stream_window / 2)
        return;
    size_t n = h2_write_window_update(c->scratch, sizeof(c->scratch), s->id, s->credit);
    if (emit_owed(c, c->scratch, n)) {
        s->recv_window += s->credit;
        s->credit = 0;
    }
}

// The peer's END_STREAM: the body is whole, if it is the length it promised.
static void peer_ended(h2_conn_t *c, h2_stream_t *s)
{
    if (s->has_length && s->body_seen != s->length) {
        reset_stream(c, s, H2_ERR_PROTOCOL, H2_END_BAD_LENGTH);
        return;
    }
    s->got_end = true;
    s->ending.how = H2_END_CLEAN;
}

static bool append(h2_stream_t *s, const uint8_t *bytes, uint32_t len)
{
    if (s->room - s->len < len) {
        uint32_t live = s->len - s->at;
        if (s->room - live >= len) {
            os64_memmove(s->buf, s->buf + s->at, live);
        } else {
            uint32_t room = s->room != 0 ? s->room : 16384u;
            while (room - live < len)
                room *= 2;
            uint8_t *grown = os64_malloc(room);
            if (grown == NULL)
                return false;
            if (live != 0)
                os64_memcpy(grown, s->buf + s->at, live);
            os64_free(s->buf);
            s->buf = grown;
            s->room = room;
        }
        s->at = 0;
        s->len = live;
    }
    os64_memcpy(s->buf + s->len, bytes, len);
    s->len += len;
    return true;
}

static bool room_locked(const h2_conn_t *c)
{
    uint32_t most = c->peer_max_streams < H2_STREAMS_MOST ? c->peer_max_streams : H2_STREAMS_MOST;
    return !c->dying && !c->dead && !c->goaway && !c->quiet && c->stream_count < most && c->next_id <= H2_STREAM_MOST;
}

// ── The out queue's drainer (§ 2.3) ─────────────────────────────────────

// Become the drainer if nobody is, and write the queue's head to the socket
// with the lock let go, until the queue is empty, the socket will take no
// more, or the connection is dying. `patient` waits on the socket for what
// is left of the write patience and ends the connection when it runs out; a
// thread with no patience to give writes what the socket takes at once.
// `last` is the reader's, once, before it closes a dying connection's
// socket: what the socket will take at once of what is still queued, which
// is how the GOAWAY of a connection error of ours is sent.
static void drain_as(h2_conn_t *c, bool patient, bool last)
{
    os64_lock_acquire(&c->lock);
    if (c->draining || c->dead) {
        os64_lock_release(&c->lock);
        return;
    }
    c->draining = true;
    while (last || !c->dying) {
        out_block_t *b = c->out_head;
        if (b == NULL || b->at == b->len)
            break;
        uint64_t now = 0, wait = 0;
        if (!now_ms(&now)) {
            die(c, H2_ERR_INTERNAL);
            break;
        }
        if (patient) {
            uint64_t spent = c->refused ? now - c->refused_at : 0;
            if (spent >= H2_WRITE_PATIENCE_MS) {
                die(c, H2_ERR_INTERNAL);
                break;
            }
            wait = H2_WRITE_PATIENCE_MS - spent;
        }
        const uint8_t *from = b->bytes + b->at;
        uint32_t len = b->len - b->at;
        os64_lock_release(&c->lock);
        int64_t wrote = os64_write_for(c->socket, from, len, wait);
        os64_lock_acquire(&c->lock);
        if (wrote > 0 && (uint64_t)wrote <= len) {
            b->at += (uint32_t)wrote;
            c->out_bytes -= (size_t)wrote;
            c->refused = false;
            if (b->at == b->len) {
                if (b->next != NULL) {
                    c->out_head = b->next;
                    os64_free(b);
                } else
                    b->at = b->len = 0;
            }
            if (c->out_bytes <= H2_OUT_MARK)
                ring_door(c);
            continue;
        }
        if (wrote == OS64_INTERRUPTED && patient) {
            // A signal is the fetch's news; the bytes are everybody's. The
            // patience is counted from the first attempt that moved nothing,
            // so a signal does not start it again.
            if (!c->refused) {
                c->refused = true;
                c->refused_at = now;
            }
            continue;
        }
        if (wrote == OS64_ERR_TIMEOUT || wrote == OS64_INTERRUPTED) {
            if (!c->refused) {
                c->refused = true;
                c->refused_at = now;
            }
            if (patient)
                die(c, H2_ERR_INTERNAL);
            break;
        }
        die(c, H2_ERR_INTERNAL);
        break;
    }
    c->draining = false;
    os64_lock_release(&c->lock);
}

static void drain(h2_conn_t *c, bool patient) { drain_as(c, patient, false); }

// ── Delivery: what each frame does (HTTP2.md § 4 H2.4's table) ──────────

static void on_data(h2_conn_t *c, const h2_frame_t *f)
{
    if (f->flow_len > c->recv_window) {
        conn_error(c, H2_ERR_FLOW_CONTROL);
        return;
    }
    c->recv_window -= f->flow_len;
    h2_stream_t *s = find(c, f->stream);
    if (s == NULL || over(s) || s->got_end) {
        if (s == NULL && !was_ours(c, f->stream)) {
            conn_error(c, H2_ERR_PROTOCOL);
            return;
        }
        credit_conn(c, f->flow_len);    // sent before our reset was seen: nobody will read it
        return;
    }
    if (f->flow_len > s->recv_window) {
        reset_stream(c, s, H2_ERR_FLOW_CONTROL, H2_END_FLOW);
        credit_conn(c, f->flow_len);
        return;
    }
    s->recv_window -= f->flow_len;
    if (!s->head_ready) {
        reset_stream(c, s, H2_ERR_PROTOCOL, H2_END_BAD_HEAD);
        credit_conn(c, f->flow_len);
        return;
    }
    s->body_seen += f->data_len;
    if ((s->no_content && f->data_len != 0) || (s->has_length && s->body_seen > s->length)) {
        reset_stream(c, s, H2_ERR_PROTOCOL, H2_END_BAD_LENGTH);
        credit_conn(c, f->flow_len);
        return;
    }
    uint32_t padding = f->flow_len - f->data_len;
    if (padding != 0) {
        credit_conn(c, padding);
        credit_stream(c, s, padding);
    }
    if (f->data_len != 0 && !append(s, f->data, f->data_len)) {
        conn_error(c, H2_ERR_INTERNAL);
        return;
    }
    if (f->flags & H2_FLAG_END_STREAM)
        peer_ended(c, s);
    ring(s);
}

static void on_settings(h2_conn_t *c, const h2_frame_t *f, bool *more_room)
{
    if (f->flags & H2_FLAG_ACK) {
        if (!c->our_settings_acked) {
            c->our_settings_acked = true;
            h2_hpack_decoder_settings_acked(&c->decoder);
        }
        return;
    }
    // A frame may state a setting more than once, and the last stands: what
    // the pool is told is judged on the limit the whole frame leaves.
    uint32_t streams_before = c->peer_max_streams;
    for (uint32_t i = 0; i < h2_settings_count(f); i++) {
        h2_setting_t setting = h2_settings_at(f, i);
        h2_fault_t fault;
        if (!h2_setting_check(setting, &fault)) {
            conn_error(c, fault.error);
            return;
        }
        switch (setting.identifier) {
        case H2_SETTING_HEADER_TABLE_SIZE:
            h2_hpack_encoder_peer_table_size(&c->encoder, setting.value);
            break;
        case H2_SETTING_MAX_CONCURRENT_STREAMS:
            c->peer_max_streams = setting.value;
            break;
        case H2_SETTING_INITIAL_WINDOW_SIZE: {
            // § 6.9.2: the difference goes to every stream's send window,
            // which may go below nothing and may not pass 2^31 - 1.
            int64_t change = (int64_t)setting.value - c->peer_initial_window;
            c->peer_initial_window = setting.value;
            for (uint32_t k = 0; k < H2_STREAMS_MOST; k++) {
                h2_stream_t *s = c->streams[k];
                // A stream that will send no more keeps its slot until its
                // fetch leaves, and has no window to judge.
                if (s == NULL || s->sent_end || s->peer_reset ||
                    (s->ending.how != H2_END_NOT_YET && s->ending.how != H2_END_CLEAN))
                    continue;
                s->send_window += change;
                if (s->send_window > WINDOW_MOST) {
                    conn_error(c, H2_ERR_FLOW_CONTROL);
                    return;
                }
            }
            if (change > 0)
                ring_all(c);
            break;
        }
        case H2_SETTING_MAX_FRAME_SIZE:
            c->peer_max_frame = setting.value;
            break;
        case H2_SETTING_ENABLE_PUSH:
            // § 6.5.2: a server may say 0 and may not say 1.
            if (setting.value != 0) {
                conn_error(c, H2_ERR_PROTOCOL);
                return;
            }
            break;
        default:
            break;              // a setting we do not know (§ 6.5.2: ignored)
        }
    }
    *more_room = c->peer_max_streams > streams_before;
    c->peer_settings_seen = true;
    size_t n = h2_write_settings_ack(c->scratch, sizeof(c->scratch));
    emit_owed(c, c->scratch, n);
}

// One frame that is not part of a field block. `*more_room` when the pool
// should be told the connection may take more streams.
static void deliver(h2_conn_t *c, const h2_frame_t *f, bool *more_room)
{
    h2_stream_t *s;
    switch (f->kind) {
    case H2_FRAME_DATA:
        on_data(c, f);
        break;
    case H2_FRAME_RST_STREAM:
        s = find(c, f->stream);
        if (s == NULL) {
            if (!was_ours(c, f->stream))
                conn_error(c, H2_ERR_PROTOCOL);
            break;
        }
        s->peer_reset = true;
        if (s->got_end) {
            // The response was whole before this came (RFC 9113 § 8.1): it
            // stands, and the reset stops only what we were still sending.
            s->ending.reset_after_end = true;
            s->ending.reset_code = f->value;
        } else if (!over(s)) {
            s->ending.how = H2_END_PEER_RESET;
            s->ending.code = f->value;
        }
        ring(s);
        break;
    case H2_FRAME_SETTINGS:
        on_settings(c, f, more_room);
        break;
    case H2_FRAME_PING:
        if (!(f->flags & H2_FLAG_ACK)) {
            size_t n = h2_write_ping(c->scratch, sizeof(c->scratch), true, f->data);
            emit_owed(c, c->scratch, n);
        }
        break;
    case H2_FRAME_GOAWAY:
        c->goaway = true;
        c->goaway_last = f->last_stream;
        for (uint32_t i = 0; i < H2_STREAMS_MOST; i++) {
            s = c->streams[i];
            if (s != NULL && s->id > f->last_stream && !over(s)) {
                s->ending.how = H2_END_UNPROCESSED;
                s->ending.code = f->value;
                ring(s);
            }
        }
        if (f->value != H2_ERR_NO_ERROR || c->stream_count == 0)
            die(c, f->value);
        break;
    case H2_FRAME_WINDOW_UPDATE:
        if (f->stream == 0) {
            c->send_window += f->value;
            if (c->send_window > WINDOW_MOST)
                conn_error(c, H2_ERR_FLOW_CONTROL);
            else
                ring_all(c);
            break;
        }
        s = find(c, f->stream);
        if (s == NULL) {
            if (!was_ours(c, f->stream))
                conn_error(c, H2_ERR_PROTOCOL);
            break;
        }
        s->send_window += f->value;
        // § 6.9.1: the stream's error or the connection's. A stream whose
        // response is whole and whose request is still going up has an
        // ending a reset would not change, so there it is the connection's.
        if (s->send_window > WINDOW_MOST && s->ending.how == H2_END_CLEAN && !s->sent_end && !s->peer_reset)
            conn_error(c, H2_ERR_FLOW_CONTROL);
        else if (s->send_window > WINDOW_MOST)
            reset_stream(c, s, H2_ERR_FLOW_CONTROL, H2_END_FLOW);
        ring(s);
        break;
    default:
        break;                  // PRIORITY, and types we do not know: dropped
    }
}

// The decoder's emit: one field into the reader's head buffer. A head past
// the buffer or the field table is refused, and the decoder reads on.
static bool head_field(void *ctx, const h2_field_t *field)
{
    h2_conn_t *c = ctx;
    uint64_t need = (uint64_t)field->name_len + field->value_len;
    if (c->head_count == FIELDS_MOST || need > H2_HEAD_MOST - c->head_len) {
        c->head_refused = true;
        return false;
    }
    uint8_t *at = c->head_buf + c->head_len;
    if (field->name_len != 0)
        os64_memcpy(at, field->name, field->name_len);
    if (field->value_len != 0)
        os64_memcpy(at + field->name_len, field->value, field->value_len);
    c->head_fields[c->head_count++] = (h2_field_t){.name = at, .value = at + field->name_len,
                                                   .name_len = field->name_len, .value_len = field->value_len,
                                                   .sensitive = field->sensitive};
    c->head_len += (uint32_t)need;
    return true;
}

static bool is_named(const h2_field_t *f, const char *name)
{
    size_t len = os64_strlen(name);
    return f->name_len == len && os64_memcmp(f->name, name, len) == 0;
}

// A whole field block has arrived for the stream that `c->block.first`
// names. Decoded with no lock, since the table must see it whoever it was
// for; then judged and handed to its stream under the lock.
static void on_block(h2_conn_t *c)
{
    const h2_frame_t *first = &c->block.first;
    c->head_len = c->head_count = 0;
    c->head_refused = false;
    h2_hpack_result_t decoded = h2_hpack_decode(&c->decoder, c->block.buffer, c->block.len, head_field, c);
    // A copy for the stream to keep, made before the lock is taken.
    uint8_t *bytes = NULL;
    h2_field_t *fields = NULL;
    if (decoded == H2_HPACK_OK && !c->head_refused) {
        bytes = os64_malloc(c->head_len != 0 ? c->head_len : 1);
        fields = os64_malloc(sizeof(*fields) * (c->head_count != 0 ? c->head_count : 1));
    }

    os64_lock_acquire(&c->lock);
    h2_stream_t *s = find(c, first->stream);
    bool ends = (first->flags & H2_FLAG_END_STREAM) != 0;
    if (decoded == H2_HPACK_BAD)
        conn_error(c, H2_ERR_COMPRESSION);
    else if (s == NULL && !was_ours(c, first->stream))
        conn_error(c, H2_ERR_PROTOCOL);
    else if (s == NULL || over(s) || s->got_end)
        ;                       // a stream that has gone: decoded for the table's sake, and dropped
    else if (decoded == H2_HPACK_REFUSED || c->head_refused)
        reset_stream(c, s, H2_ERR_ENHANCE_YOUR_CALM, H2_END_HEAD_TOO_LARGE);
    else if (bytes == NULL || fields == NULL)
        conn_error(c, H2_ERR_INTERNAL);
    else if (s->head_ready) {
        // Trailers: no pseudo-header, and they are the stream's last word.
        if (h2_head_check(c->head_fields, c->head_count, H2_HEAD_TRAILERS, NULL) != H2_HEAD_OK || !ends)
            reset_stream(c, s, H2_ERR_PROTOCOL, H2_END_BAD_HEAD);
        else
            peer_ended(c, s);
        ring(s);
    } else if (h2_head_check(c->head_fields, c->head_count, H2_HEAD_RESPONSE, NULL) != H2_HEAD_OK)
        reset_stream(c, s, H2_ERR_PROTOCOL, H2_END_BAD_HEAD);
    else {
        // h2_head_check has made `:status` the first field and three digits.
        const uint8_t *digits = c->head_fields[0].value;
        unsigned status = (unsigned)(digits[0] - '0') * 100 + (unsigned)(digits[1] - '0') * 10 +
                          (unsigned)(digits[2] - '0');
        s->ending.any_seen = true;
        // Syntax and range are h2_head_check's: each content-length is a
        // number. Two that differ are no length at all (RFC 9110 § 8.6).
        bool has_length = false, lengths_differ = false;
        uint64_t length = 0;
        for (uint32_t i = 0; i < c->head_count; i++) {
            const h2_field_t *field = &c->head_fields[i];
            if (!is_named(field, "content-length"))
                continue;
            uint64_t n = 0;
            for (uint32_t k = 0; k < field->value_len; k++)
                n = n * 10 + (uint64_t)(field->value[k] - '0');
            lengths_differ = lengths_differ || (has_length && n != length);
            has_length = true;
            length = n;
        }
        if (lengths_differ)
            reset_stream(c, s, H2_ERR_PROTOCOL, H2_END_BAD_HEAD);
        else if (status < 200) {
            // An interim head: dropped, and the wait goes on. It may not end
            // the stream, and 101 has no meaning in HTTP/2 (§ 8.6).
            if (ends || status == 101)
                reset_stream(c, s, H2_ERR_PROTOCOL, H2_END_BAD_HEAD);
        } else {
            os64_memcpy(bytes, c->head_buf, c->head_len);
            for (uint32_t i = 0; i < c->head_count; i++) {
                fields[i] = c->head_fields[i];
                fields[i].name = bytes + (c->head_fields[i].name - c->head_buf);
                fields[i].value = bytes + (c->head_fields[i].value - c->head_buf);
            }
            // A HEAD's answer, a 204 and a 304 carry no content, whatever
            // length they state (RFC 9110 § 6.4.1).
            s->no_content = s->bodiless || status == 204 || status == 304;
            if (has_length && !s->no_content) {
                s->has_length = true;
                s->length = length;
            }
            s->head = bytes;
            s->fields = fields;
            s->field_count = c->head_count;
            s->head_ready = true;
            s->ending.head_seen = true;
            bytes = NULL;
            fields = NULL;
            if (ends)
                peer_ended(c, s);
            ring(s);
        }
    }
    os64_lock_release(&c->lock);
    os64_free(bytes);
    os64_free(fields);
}

// ── The reader ──────────────────────────────────────────────────────────

// One whole frame from the reader's plaintext.
static void on_frame(h2_conn_t *c, const h2_frame_header_t *header, const uint8_t *payload)
{
    h2_frame_t frame;
    h2_fault_t fault;
    bool more_room = false;
    bool parsed = h2_frame_parse(header, payload, c->our_max_frame, &frame, &fault);

    os64_lock_acquire(&c->lock);
    c->frames_in++;
    // RFC 9113 § 3.4: the server's connection preface is a SETTINGS frame.
    bool first_is_wrong = !c->peer_settings_seen &&
                          (header->type != H2_FRAME_SETTINGS || (header->flags & H2_FLAG_ACK) != 0);
    if (first_is_wrong)
        conn_error(c, H2_ERR_PROTOCOL);
    else if (!parsed && !fault.stream)
        conn_error(c, fault.error);
    else if (!parsed && c->block.open)
        conn_error(c, H2_ERR_PROTOCOL);     // § 6.10: only a CONTINUATION may follow an open block
    else if (!parsed) {
        h2_stream_t *s = find(c, header->stream);
        if (s != NULL)
            reset_stream(c, s, fault.error, H2_END_BAD_FRAME);
        else if (!was_ours(c, header->stream))
            conn_error(c, H2_ERR_PROTOCOL);
    } else if (frame.kind == H2_FRAME_PUSH_PROMISE)
        conn_error(c, H2_ERR_PROTOCOL);     // § 2.10: we asked for none and take none
    else if (frame.kind == H2_FRAME_HEADERS || frame.kind == H2_FRAME_DATA) {
        // A frame of a response has arrived, whatever comes of it: a head
        // whose CONTINUATION never comes is still a server that answered.
        h2_stream_t *s = find(c, header->stream);
        if (s != NULL)
            s->ending.any_seen = true;
    }
    bool stop = c->dying || !parsed;
    os64_lock_release(&c->lock);
    if (stop)
        return;

    switch (h2_block_feed(&c->block, &frame, &fault)) {
    case H2_BLOCK_MORE:
        return;
    case H2_BLOCK_DONE:
        on_block(c);
        return;
    case H2_BLOCK_FAULT:
        os64_lock_acquire(&c->lock);
        conn_error(c, fault.error);
        os64_lock_release(&c->lock);
        return;
    case H2_BLOCK_NOT_MINE:
        break;
    }
    os64_lock_acquire(&c->lock);
    deliver(c, &frame, &more_room);
    os64_lock_release(&c->lock);
    if (more_room && c->hooks.room != NULL)
        c->hooks.room(c->hooks.ctx, c);
}

// Cut every whole frame from the reader's plaintext and keep the rest.
static void cut_frames(h2_conn_t *c)
{
    uint32_t at = 0;
    while (c->plain_len - at >= H2_FRAME_HEADER_LEN) {
        h2_frame_header_t header;
        h2_fault_t fault;
        h2_frame_header_read(c->plain + at, &header);
        if (!h2_frame_header_check(&header, c->our_max_frame, &fault)) {
            os64_lock_acquire(&c->lock);
            conn_error(c, fault.error);
            os64_lock_release(&c->lock);
            c->plain_len = 0;
            return;
        }
        if (c->plain_len - at < H2_FRAME_HEADER_LEN + header.length)
            break;
        on_frame(c, &header, c->plain + at + H2_FRAME_HEADER_LEN);
        at += H2_FRAME_HEADER_LEN + header.length;
    }
    if (at != 0) {
        os64_memmove(c->plain, c->plain + at, c->plain_len - at);
        c->plain_len -= at;
    }
}

// Ciphertext from the socket: fed to the engine a piece at a time, its
// plaintext cut into frames between pieces so the engine always has room.
static void take_in(h2_conn_t *c, const uint8_t *cipher, size_t len)
{
    size_t fed = 0;
    for (;;) {
        os64_lock_acquire(&c->lock);
        if (c->dying) {
            os64_lock_release(&c->lock);
            return;
        }
        bool moved = false;
        if (fed < len) {
            os64_tls_transfer_t in = os64_tls_feed_ciphertext(c->tls, cipher + fed, len - fed);
            fed += in.transferred;
            moved = in.transferred != 0;
        }
        os64_tls_transfer_t out = os64_tls_read_plaintext(c->tls, c->plain + c->plain_len, c->plain_room - c->plain_len);
        c->plain_len += (uint32_t)out.transferred;
        moved = moved || out.transferred != 0;
        os64_tls_state_t state = os64_tls_state(c->tls);
        // The peer's close_notify ends the connection once the plaintext
        // that came before it has been read and cut: a response whose last
        // frame shares a read with the close is whole.
        bool eof = state.status == OS64_TLS_CLEAN_EOF;
        if (fatal(state.status))
            die(c, H2_ERR_NO_ERROR);
        else
            pump(c);            // what the engine owes for what it was fed
        // What the engine owes of its own accord (a KeyUpdate's answer) is
        // the peer's doing too, and has the same ceiling as a frame it
        // makes us owe.
        if (c->out_bytes > (size_t)H2_OUT_MARK + H2_OUT_CONTROL_MOST)
            die(c, H2_ERR_ENHANCE_YOUR_CALM);
        // Fed nothing and gave nothing with input still in hand: the engine
        // is waiting on a frame larger than the room one may have.
        if (!moved && fed < len && !eof)
            conn_error(c, H2_ERR_INTERNAL);
        os64_lock_release(&c->lock);
        cut_frames(c);
        if (!moved) {
            if (eof) {
                os64_lock_acquire(&c->lock);
                die(c, H2_ERR_NO_ERROR);
                os64_lock_release(&c->lock);
            }
            return;
        }
    }
}

static int64_t reader(void *arg)
{
    h2_conn_t *c = arg;
    uint8_t cipher[4096];
    // What came with the handshake: ciphertext not yet fed, and plaintext the
    // engine already holds, which no later read of the socket would bring.
    take_in(c, c->unread, c->unread_len);
    for (;;) {
        os64_lock_acquire(&c->lock);
        bool leave = c->dying && !c->draining;
        os64_lock_release(&c->lock);
        if (leave)
            break;
        int64_t got = os64_read_for(c->socket, cipher, sizeof(cipher), H2_READER_SLICE_MS);
        if (got > 0)
            take_in(c, cipher, (size_t)got);
        else if (got != OS64_ERR_TIMEOUT && got != OS64_INTERRUPTED) {
            // The peer's FIN, or the transport's failure.
            os64_lock_acquire(&c->lock);
            die(c, H2_ERR_NO_ERROR);
            os64_lock_release(&c->lock);
        }
        drain(c, false);

        // The look around: bytes the socket has refused for too long, and
        // an idle connection the pool is done keeping.
        uint64_t now;
        os64_lock_acquire(&c->lock);
        bool idle = !c->dying && c->stream_count == 0;
        if (!now_ms(&now) || (c->refused && now - c->refused_at >= H2_WRITE_PATIENCE_MS))
            die(c, H2_ERR_INTERNAL);
        os64_lock_release(&c->lock);
        if (idle && c->hooks.idle_out != NULL && c->hooks.idle_out(c->hooks.ctx, c)) {
            os64_lock_acquire(&c->lock);
            // A stream opened since the question was asked keeps it: the
            // pool's answer was for a connection with none.
            if (c->stream_count == 0)
                die(c, H2_ERR_NO_ERROR);
            os64_lock_release(&c->lock);
        }
    }

    // Nobody is writing and nobody will: the socket is the reader's, for one
    // last word and then to close.
    drain_as(c, false, true);
    os64_close(c->socket);
    os64_lock_acquire(&c->lock);
    c->dead = true;
    for (uint32_t i = 0; i < H2_STREAMS_MOST; i++) {
        h2_stream_t *s = c->streams[i];
        if (s != NULL && !over(s)) {
            s->ending.how = H2_END_CONN_DEAD;
            s->ending.code = c->die_code;
        }
    }
    ring_all(c);
    ring_door(c);
    os64_lock_release(&c->lock);
    if (c->hooks.dead != NULL)
        c->hooks.dead(c->hooks.ctx, c);
    let_go(c);
    return 0;
}

// ── The connection, from outside ────────────────────────────────────────

h2_conn_t *h2_conn_adopt(os64_tls_client *engine, int32_t socket, const void *unread, size_t unread_len,
                         const h2_greeting_t *greeting, const h2_conn_hooks_t *hooks)
{
    h2_conn_t *c = os64_malloc(sizeof(*c));
    if (c == NULL)
        return NULL;
    os64_memset(c, 0, sizeof(*c));
    c->our_max_frame = H2_FRAME_SIZE_LEAST;
    c->our_table = H2_HPACK_TABLE_DEFAULT;
    c->our_initial_window = 65535;
    bool sound = unread_len <= sizeof(c->unread) && greeting->count <= 64;
    for (uint32_t i = 0; sound && i < greeting->count; i++) {
        h2_fault_t fault;
        h2_setting_t setting = greeting->settings[i];
        sound = h2_setting_check(setting, &fault);
        if (setting.identifier == H2_SETTING_MAX_FRAME_SIZE)
            c->our_max_frame = setting.value;
        else if (setting.identifier == H2_SETTING_INITIAL_WINDOW_SIZE)
            c->our_initial_window = setting.value;
        else if (setting.identifier == H2_SETTING_HEADER_TABLE_SIZE)
            c->our_table = setting.value;
    }
    uint32_t first = greeting->first_stream != 0 ? greeting->first_stream : 1;
    sound = sound && c->our_max_frame <= GREETING_MOST && c->our_table <= GREETING_MOST &&
            (uint64_t)65535 + greeting->window_increment <= H2_STREAM_MOST &&
            (uint64_t)c->our_initial_window + greeting->stream_increment <= H2_STREAM_MOST &&
            (first & 1u) != 0 && first <= H2_STREAM_MOST;
    uint32_t table_room = c->our_table > H2_HPACK_TABLE_DEFAULT ? c->our_table : H2_HPACK_TABLE_DEFAULT;
    // One literal field at a time passes through the scratch, and a field
    // may be as large as a head, whatever the table holds.
    uint32_t scratch_room = table_room > H2_HEAD_MOST ? table_room : H2_HEAD_MOST;
    if (sound) {
        c->plain_room = H2_FRAME_HEADER_LEN + c->our_max_frame;
        c->encoder_store = os64_malloc(H2_HPACK_STORAGE(H2_HPACK_TABLE_DEFAULT));
        c->decoder_store = os64_malloc(H2_HPACK_STORAGE(table_room));
        c->decoder_scratch = os64_malloc(scratch_room);
        c->plain = os64_malloc(c->plain_room);
        c->block_room = os64_malloc(H2_BLOCK_MOST);
        c->head_buf = os64_malloc(H2_HEAD_MOST);
        c->head_fields = os64_malloc(sizeof(h2_field_t) * FIELDS_MOST);
        sound = c->encoder_store != NULL && c->decoder_store != NULL && c->decoder_scratch != NULL &&
                c->plain != NULL && c->block_room != NULL && c->head_buf != NULL && c->head_fields != NULL &&
                h2_hpack_encoder_init(&c->encoder, c->encoder_store, H2_HPACK_STORAGE(H2_HPACK_TABLE_DEFAULT),
                                      H2_HPACK_TABLE_DEFAULT) &&
                h2_hpack_decoder_init(&c->decoder, c->decoder_store, H2_HPACK_STORAGE(table_room),
                                      c->decoder_scratch, scratch_room, c->our_table);
    }
    uint8_t *hello = sound ? os64_malloc(H2_PREFACE_LEN + H2_FRAME_HEADER_LEN * 2 + greeting->count * 6 + 4) : NULL;
    if (hello != NULL) {
        h2_block_init(&c->block, c->block_room, H2_BLOCK_MOST);
        c->tls = engine;
        c->socket = socket;
        c->hooks = *hooks;
        c->next_id = first;
        c->first_id = first;
        c->stream_increment = greeting->stream_increment;
        c->our_stream_window = c->our_initial_window + greeting->stream_increment;
        c->priority = greeting->priority;
        c->peer_max_streams = UINT32_MAX;       // no limit until its SETTINGS give one (§ 6.5.2)
        c->peer_max_frame = H2_FRAME_SIZE_LEAST;
        c->peer_initial_window = 65535;
        c->send_window = 65535;
        c->our_window = (int64_t)65535 + greeting->window_increment;
        c->recv_window = c->our_window;
        if (unread_len != 0)
            os64_memcpy(c->unread, unread, unread_len);
        c->unread_len = unread_len;

        size_t n = H2_PREFACE_LEN;
        os64_memcpy(hello, h2_preface, H2_PREFACE_LEN);
        n += h2_write_settings(hello + n, H2_FRAME_HEADER_LEN + greeting->count * 6, greeting->settings, greeting->count);
        if (greeting->window_increment != 0)
            n += h2_write_window_update(hello + n, H2_FRAME_HEADER_LEN + 4, 0, greeting->window_increment);
        c->holds = 2;                           // the adopter's and the reader's
        os64_lock_acquire(&c->lock);
        bool said = emit(c, hello, n);
        os64_lock_release(&c->lock);
        int64_t thread = said ? os64_thread(reader, c) : -1;
        os64_free(hello);
        if (thread >= 0) {
            os64_close((int32_t)thread);        // detached: it runs on, and nobody collects it
            drain(c, true);
            return c;
        }
    }
    // Not made. The engine and the socket stay the caller's: only what this
    // call allocated goes.
    for (out_block_t *b = c->out_head; b != NULL;) {
        out_block_t *next = b->next;
        os64_free(b);
        b = next;
    }
    os64_free(c->encoder_store);
    os64_free(c->decoder_store);
    os64_free(c->decoder_scratch);
    os64_free(c->plain);
    os64_free(c->block_room);
    os64_free(c->head_buf);
    os64_free(c->head_fields);
    os64_free(c);
    return NULL;
}

bool h2_conn_room(h2_conn_t *c)
{
    os64_lock_acquire(&c->lock);
    bool room = room_locked(c);
    os64_lock_release(&c->lock);
    return room;
}

uint32_t h2_conn_streams(h2_conn_t *c)
{
    os64_lock_acquire(&c->lock);
    uint32_t count = c->stream_count;
    os64_lock_release(&c->lock);
    return count;
}

size_t h2_conn_buffered(h2_conn_t *c)
{
    os64_lock_acquire(&c->lock);
    size_t held = 0;
    for (uint32_t i = 0; i < H2_STREAMS_MOST; i++)
        if (c->streams[i] != NULL)
            held += c->streams[i]->room;
    os64_lock_release(&c->lock);
    return held;
}

size_t h2_conn_queued(h2_conn_t *c)
{
    os64_lock_acquire(&c->lock);
    size_t queued = c->out_bytes;
    os64_lock_release(&c->lock);
    return queued;
}

void h2_conn_condemn(h2_conn_t *c)
{
    os64_lock_acquire(&c->lock);
    die(c, H2_ERR_NO_ERROR);
    os64_lock_release(&c->lock);
}

void h2_conn_release(h2_conn_t *c) { let_go(c); }

// ── Waiting ─────────────────────────────────────────────────────────────

typedef struct {
    uint64_t began, frames;
    bool begun;
} waiting_t;

static bool bell_ready(h2_wait_t *w)
{
    return w->bell != NULL && (w->bell->handle >= 0 || os64_bell_make(w->bell) >= 0);
}

// The patience ran out. A connection that received no frame of any kind
// while this call waited takes no new stream (§ 2.5).
static h2_result_t silent(h2_conn_t *c, const waiting_t *st)
{
    os64_lock_acquire(&c->lock);
    if (c->frames_in == st->frames)
        c->quiet = true;
    os64_lock_release(&c->lock);
    return H2_SILENT;
}

// Park on the bell, with no lock held. H2_OK is "ask the question again":
// rung, interrupted by a signal, or looking up to ask about cancellation.
static h2_result_t park(h2_conn_t *c, h2_wait_t *w, waiting_t *st)
{
    uint64_t now;
    if (w->cancelled != NULL && w->cancelled(w->cancel_ctx))
        return H2_INTERRUPTED;
    if (!now_ms(&now))
        return H2_FAILED;
    if (!st->begun) {
        st->begun = true;
        st->began = now;
        os64_lock_acquire(&c->lock);
        st->frames = c->frames_in;
        os64_lock_release(&c->lock);
    }
    uint64_t waited = now - st->began;
    if (waited >= w->patience_ms)
        return silent(c, st);
    uint64_t left = w->patience_ms - waited;
    bool look = left > H2_LOOK_MS;
    int64_t rung = os64_bell_wait(w->bell, look ? H2_LOOK_MS : left);
    if (w->cancelled != NULL && w->cancelled(w->cancel_ctx))
        return H2_INTERRUPTED;
    if (rung == 1 || rung == OS64_INTERRUPTED || (rung == OS64_ERR_TIMEOUT && look))
        return H2_OK;
    return rung == OS64_ERR_TIMEOUT ? silent(c, st) : H2_FAILED;
}

// ── Streams, from the fetch's side ──────────────────────────────────────

// The stream a new one of `weight` depends on under H2_PRIORITY_CHAINED.
static uint32_t chained_parent(const h2_conn_t *c, uint32_t weight)
{
    const h2_stream_t *parent = NULL;
    for (uint32_t i = 0; i < H2_STREAMS_MOST; i++) {
        const h2_stream_t *s = c->streams[i];
        if (s == NULL || s->weight < weight)
            continue;
        if (parent == NULL || s->weight < parent->weight || (s->weight == parent->weight && s->id > parent->id))
            parent = s;
    }
    return parent != NULL ? parent->id : 0;
}

h2_result_t h2_stream_open(h2_conn_t *c, const h2_field_t *fields, size_t count, bool ends, bool bodiless,
                           uint32_t weight, h2_wait_t *wait, h2_stream_t **stream)
{
    *stream = NULL;
    if (weight < 1 || weight > 256)
        weight = 16;                    // RFC 9113 § 5.3.5's default, for a caller that states none
    size_t bound = h2_hpack_encode_bound(fields, count);
    if (bound > H2_HEAD_MOST)
        return H2_TOO_LARGE;
    size_t frames_room = h2_headers_size(bound, OUR_FRAME, true) + H2_FRAME_HEADER_LEN + 4;
    uint8_t *block = os64_malloc(bound);
    uint8_t *frames = os64_malloc(frames_room);
    h2_stream_t *s = os64_malloc(sizeof(*s));
    h2_result_t result = block != NULL && frames != NULL && s != NULL && bell_ready(wait) ? H2_OK : H2_FAILED;
    waiting_t st = {0};
    while (result == H2_OK) {
        os64_lock_acquire(&c->lock);
        if (!room_locked(c)) {
            os64_lock_release(&c->lock);
            result = H2_NO_ROOM;
            break;
        }
        if (c->out_bytes <= H2_OUT_MARK) {
            uint32_t slot = 0;
            while (c->streams[slot] != NULL)
                slot++;
            size_t len;
            h2_hpack_encode(&c->encoder, fields, count, block, bound, &len);
            os64_memset(s, 0, sizeof(*s));
            s->conn = c;
            s->id = c->next_id;
            c->next_id += 2;
            s->bell = wait->bell;
            s->bodiless = bodiless;
            s->sent_end = ends;
            s->recv_window = c->our_initial_window;
            s->send_window = c->peer_initial_window;
            s->weight = weight;
            h2_priority_t priority = {.weight = (uint8_t)(weight - 1)};
            if (c->priority == H2_PRIORITY_CHAINED) {
                priority.exclusive = true;
                priority.depends_on = chained_parent(c, weight);    // before this stream is among them
            }
            c->streams[slot] = s;
            c->stream_count++;
            hold(c);
            size_t n = h2_write_headers(frames, frames_room, s->id, block, len, ends,
                                        c->priority != H2_PRIORITY_NONE ? &priority : NULL, OUR_FRAME);
            if (c->stream_increment != 0) {
                // In the same write as the HEADERS, so nothing of ours comes
                // between them.
                n += h2_write_window_update(frames + n, frames_room - n, s->id, c->stream_increment);
                s->recv_window += c->stream_increment;
            }
            emit(c, frames, n);         // a connection that would not take it is dying, and ends this stream
            os64_lock_release(&c->lock);
            drain(c, true);
            *stream = s;
            s = NULL;
            break;
        }
        // The queue is past its mark: a new stream waits at the door.
        door_add(c, wait);
        os64_lock_release(&c->lock);
        result = park(c, wait, &st);
        os64_lock_acquire(&c->lock);
        door_remove(c, wait);
        os64_lock_release(&c->lock);
    }
    os64_free(block);
    os64_free(frames);
    os64_free(s);
    return result;
}

h2_result_t h2_stream_send(h2_stream_t *s, const void *bytes, size_t len, bool ends, h2_wait_t *wait, size_t *sent)
{
    h2_conn_t *c = s->conn;
    waiting_t st = {0};
    *sent = 0;
    if (!bell_ready(wait))
        return H2_FAILED;
    for (;;) {
        os64_lock_acquire(&c->lock);
        h2_result_t stop = H2_OK;
        if (s->ending.how != H2_END_NOT_YET && s->ending.how != H2_END_CLEAN)
            stop = H2_ENDED;
        else if (s->peer_reset || s->ending.how == H2_END_CLEAN)
            // The response is whole (or the peer reset the stream after it
            // was): the server has answered without the rest of the request,
            // and may never give the window the rest would wait for. The
            // upload ends here, as HTTP/1.1's does when the final head
            // arrives under it.
            stop = H2_STOP;
        size_t left = len - *sent;
        bool done = left == 0 && (!ends || s->sent_end);
        if (stop != H2_OK || done) {
            os64_lock_release(&c->lock);
            return stop;
        }
        int64_t window = s->send_window < c->send_window ? s->send_window : c->send_window;
        uint32_t most = c->peer_max_frame < OUR_FRAME ? c->peer_max_frame : OUR_FRAME;
        if (window < (int64_t)most)
            most = window > 0 ? (uint32_t)window : 0;
        bool at_door = c->out_bytes > H2_OUT_MARK;
        if (!c->dying && !at_door && (most != 0 || left == 0)) {
            size_t taken;
            size_t n = h2_write_data(c->scratch, sizeof(c->scratch), s->id, (const uint8_t *)bytes + *sent, left, most,
                                     ends, &taken);
            if (emit(c, c->scratch, n)) {
                s->send_window -= (int64_t)taken;
                c->send_window -= (int64_t)taken;
                *sent += taken;
                if (ends && taken == left)
                    s->sent_end = true;
            }
            os64_lock_release(&c->lock);
            drain(c, true);
            continue;
        }
        // No window, a queue past its mark, or a connection on its way out,
        // whose reader ends this stream and rings.
        if (at_door)
            door_add(c, wait);
        os64_lock_release(&c->lock);
        h2_result_t parked = park(c, wait, &st);
        os64_lock_acquire(&c->lock);
        door_remove(c, wait);
        os64_lock_release(&c->lock);
        if (parked != H2_OK)
            return parked;
    }
}

h2_result_t h2_stream_head(h2_stream_t *s, h2_wait_t *wait, h2_hpack_emit_t emit_field, void *ctx)
{
    h2_conn_t *c = s->conn;
    waiting_t st = {0};
    if (!bell_ready(wait))
        return H2_FAILED;
    for (;;) {
        os64_lock_acquire(&c->lock);
        bool ready = s->head_ready;
        bool ended = s->ending.how != H2_END_NOT_YET && s->ending.how != H2_END_CLEAN;
        os64_lock_release(&c->lock);
        if (ready) {
            // The head is the stream's from here to its close, and the
            // stream is this caller's: read with no lock.
            for (uint32_t i = 0; i < s->field_count; i++)
                if (!emit_field(ctx, &s->fields[i]))
                    break;
            return H2_OK;
        }
        if (ended)
            return H2_ENDED;
        h2_result_t parked = park(c, wait, &st);
        if (parked != H2_OK)
            return parked;
    }
}

int64_t h2_stream_read(h2_stream_t *s, void *buffer, size_t room, h2_wait_t *wait)
{
    h2_conn_t *c = s->conn;
    waiting_t st = {0};
    if (!bell_ready(wait))
        return -(int64_t)H2_FAILED;
    for (;;) {
        os64_lock_acquire(&c->lock);
        uint32_t have = s->len - s->at;
        if (have != 0 && room != 0) {
            uint32_t n = have < room ? have : (uint32_t)room;
            os64_memcpy(buffer, s->buf + s->at, n);
            s->at += n;
            // A buffer read to its end is given back when it grew large:
            // the window its bytes held is the peer's again, and the memory
            // would otherwise be held twice over, here and by whichever
            // stream the peer fills next.
            uint8_t *spent = NULL;
            if (s->at == s->len) {
                s->at = s->len = 0;
                if (s->room > BUF_KEEP) {
                    spent = s->buf;
                    s->buf = NULL;
                    s->room = 0;
                }
            }
            credit_stream(c, s, n);
            credit_conn(c, n);
            os64_lock_release(&c->lock);
            os64_free(spent);
            drain(c, false);
            return n;
        }
        h2_end_t how = s->ending.how;
        os64_lock_release(&c->lock);
        if (how == H2_END_CLEAN)
            return 0;
        if (how != H2_END_NOT_YET)
            return -(int64_t)H2_ENDED;
        if (room == 0)
            return 0;
        h2_result_t parked = park(c, wait, &st);
        if (parked != H2_OK)
            return -(int64_t)parked;
    }
}

uint32_t h2_stream_id(const h2_stream_t *s) { return s->id; }

h2_ending_t h2_stream_ended(h2_stream_t *s)
{
    os64_lock_acquire(&s->conn->lock);
    h2_ending_t ending = s->ending;
    os64_lock_release(&s->conn->lock);
    return ending;
}

void h2_stream_close(h2_stream_t *s)
{
    h2_conn_t *c = s->conn;
    os64_lock_acquire(&c->lock);
    s->bell = NULL;
    // A reset of ours, unless both sides finished, or the peer reset it, or
    // we already did (an ending that is neither "not yet" nor "clean").
    bool finished = s->sent_end && s->got_end;
    bool ours_to_reset = s->ending.how == H2_END_NOT_YET || s->ending.how == H2_END_CLEAN;
    if (!finished && !s->peer_reset && ours_to_reset && !c->dead) {
        size_t n = h2_write_rst_stream(c->scratch, sizeof(c->scratch), s->id, H2_ERR_CANCEL);
        emit(c, c->scratch, n);
    }
    credit_conn(c, s->len - s->at);     // what it leaves unread, nobody will read
    for (uint32_t i = 0; i < H2_STREAMS_MOST; i++)
        if (c->streams[i] == s)
            c->streams[i] = NULL;
    c->stream_count--;
    if ((c->goaway || c->quiet) && c->stream_count == 0)
        die(c, H2_ERR_NO_ERROR);        // it takes no new stream, and its last has left
    os64_lock_release(&c->lock);
    drain(c, false);
    if (c->hooks.room != NULL)
        c->hooks.room(c->hooks.ctx, c);
    os64_free(s->head);
    os64_free(s->fields);
    os64_free(s->buf);
    os64_free(s);
    let_go(c);
}
