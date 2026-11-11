// h2_peer.h — the far end of an HTTP/2 connection, for host harnesses
// (tools/h2_peer.c): an in-memory socket, a stand-in for the TLS engine,
// and a scripted server a case drives one frame at a time.
//
// THE ENGINE STAND-IN NUMBERS ITS RECORDS. Each plaintext write becomes a
// record with a sequence number, and each side refuses a record out of
// order, repeated or cut. That is what makes the out queue's one rule (the
// engine's ciphertext reaches the wire whole and in the engine's order,
// HTTP2.md § 2.3) something a test can fail. It also refuses two threads
// inside it at once, which is the rule that it is driven under one lock.
//
// THE CLOCK is the host's, plus whatever a case adds with peer_clock_skip:
// a write patience of thirty seconds is skipped, not waited.
//
// A CASE SPEAKS FOR ONE CONNECTION AT A TIME, the one it has in hand.
// peer_begin makes the first (its socket and engine are what h2_conn_adopt
// is handed) and takes it in hand; peer_end checks it was let go. A harness
// with several connections opens each with peer_open and says which it
// speaks for with peer_use, which is the calling thread's own choice.
#ifndef H2_PEER_H
#define H2_PEER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "h2_frame.h"
#include "h2_hpack.h"
#include "tls/tls.h"

// A new socket and engine, and a server whose tables are empty.
void peer_begin(os64_tls_client **engine, int32_t *socket);
// Another connection, as new: its number, for peer_use, peer_engine and
// peer_socket. peer_forget_all lets every number be given out again.
int peer_open(void);
void peer_forget_all(void);
void peer_use(int index);
os64_tls_client *peer_engine(int index);
int32_t peer_socket(int index);
// The socket was closed once, the engine freed, no record was left cut, and
// no record arrived out of order. False names nothing; the case's CHECK does.
bool peer_end(void);
unsigned peer_closes(void);         // how many times the socket has been closed
bool peer_engine_freed(void);
bool peer_records_sound(void);      // every record so far was whole and in order, both ways

// The socket's moods.
void peer_stall(bool stalled);      // the socket takes no bytes from the client
void peer_write_cap(size_t most);   // and takes at most this many a write when it does (0 = all)
void peer_hang_up(void);            // the server closes: the client reads end of input
// The next `times` waiting writes to a stalled socket each end twenty
// seconds later (by the clock) with OS64_INTERRUPTED and nothing written.
void peer_interrupt_writes(unsigned times);
void peer_clock_skip(uint64_t ms);  // the clock jumps forward

// What the client sent, as the server reads it. Each waits up to
// `timeout_ms` of real time.
bool peer_preface(int timeout_ms);
// The next frame: its header, and its payload in a buffer good until the
// next call.
bool peer_frame(h2_frame_header_t *header, const uint8_t **payload, int timeout_ms);
// The next frame of this type, the ones before it dropped.
bool peer_frame_of(uint8_t type, h2_frame_header_t *header, const uint8_t **payload, int timeout_ms);
// Decode a request's field block with the server's decoder. The fields are
// good until the next call.
bool peer_decode(const uint8_t *block, size_t len, const h2_field_t **fields, size_t *count);
unsigned peer_owed_records(void);   // records the client's engine sent because it owed them

// What the server sends.
void peer_send_frame(uint8_t type, uint8_t flags, uint32_t stream, const void *payload, size_t len);
void peer_send_settings(const h2_setting_t *settings, uint32_t count);
void peer_send_headers(uint32_t stream, const h2_field_t *fields, size_t count, bool end_stream);
// The same block cut across HEADERS and CONTINUATION frames of `piece` bytes.
void peer_send_headers_split(uint32_t stream, const h2_field_t *fields, size_t count, bool end_stream, size_t piece);
void peer_send_data(uint32_t stream, const void *bytes, size_t len, bool end_stream);
void peer_send_window(uint32_t stream, uint32_t increment);
void peer_send_rst(uint32_t stream, uint32_t code);
void peer_send_goaway(uint32_t last_stream, uint32_t code);
void peer_send_ping(void);
// The stream's last DATA (END_STREAM, at most 256 bytes) and the engine's
// close_notify, handed to the client together: one read brings both.
void peer_send_data_and_close(uint32_t stream, const void *bytes, size_t len);
// A record that makes the client's engine owe one of its own (a KeyUpdate).
void peer_send_engine_owes(void);
// The server's SETTINGS as plaintext the engine already holds: decrypted
// during the handshake, before the connection is adopted.
void peer_engine_holds_settings(const h2_setting_t *settings, uint32_t count);
// The server's bytes as records, not sent: for h2_conn_adopt's `unread`.
size_t peer_records_of_settings(const h2_setting_t *settings, uint32_t count, uint8_t *out, size_t room);

#endif
