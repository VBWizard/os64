#ifndef OS64_SSH_ENGINE_H
#define OS64_SSH_ENGINE_H

#include <stddef.h>
#include <stdint.h>
#include "../../libtls/upstream/inc/bearssl.h"

#define SSH_PACKET_MAX 35000u
#define SSH_OUTPUT_CAP 131072u
#define SSH_WINDOW 2097152u
#define SSH_DATA_MAX 32768u
#define SSH_COMMAND_MAX 255u
#define SSH_KEYS_MAX 64u
#define SSH_KEY_BLOB_MAX 104u
#define SSH_IDENT "SSH-2.0-os64sshd_1.0"
#define SSH_KEY_TYPE "ecdsa-sha2-nistp256"
/* direct-tcpip channels (RFC 4254 section 7.2, REMOTE.md section 2). The
 * session is local channel 0; forward i is local channel i + 1. Each forward
 * advertises its own, smaller window, sized for a client that mostly asks
 * (a viewer's keystrokes and update requests) rather than uploads: it bounds
 * the client-to-local direction only, and an upload through a forward runs
 * at this window per round trip. */
#define SSH_FORWARDS 7u
#define SSH_FORWARD_WINDOW 262144u
#define SSH_FORWARD_HOST_MAX 255u
enum ssh_forward_policy { SSH_FORWARD_NONE, SSH_FORWARD_LOOPBACK };
/* CLOSED: both CLOSEs have crossed, so the channel number is dead, but the
 * caller may still owe the local connection bytes the client sent before its
 * CLOSE. The slot stays taken until ssh_forward_release. */
enum ssh_forward_state { SSH_FORWARD_FREE, SSH_FORWARD_OPENING, SSH_FORWARD_OPEN, SSH_FORWARD_CLOSED };

/* The caller owns time, I/O and process lifetime. One thread owns this engine.
 * receive stops at an event; its data remains valid until the next receive.
 * Drain output before retrying a receive that consumed zero bytes. */
enum ssh_event { SSH_EVENT_NONE, SSH_EVENT_EXEC, SSH_EVENT_SHELL,
    SSH_EVENT_INPUT, SSH_EVENT_EOF, SSH_EVENT_RESIZE, SSH_EVENT_CLOSE,
    SSH_EVENT_AUTH, SSH_EVENT_FORWARD_OPEN, SSH_EVENT_FORWARD_DATA,
    SSH_EVENT_FORWARD_EOF, SSH_EVENT_FORWARD_CLOSE };
typedef struct { const uint8_t *p; size_t n; int bad; } ssh_reader;
typedef struct { uint8_t *p; size_t n, cap; int bad; } ssh_writer;
typedef struct {
    br_aes_ct64_ctrcbc_keys aes;
    br_hmac_key_context mac;
    uint8_t iv[16];
    uint32_t seq;
    uint64_t bytes;
    int active;
} ssh_direction;
typedef struct {
    uint8_t blob[SSH_KEY_BLOB_MAX];
    size_t len;
} ssh_public_key;
typedef struct {
    enum ssh_forward_state state;
    uint32_t peer_channel, peer_window, peer_packet, receive_window;
    uint32_t credit_owed;                  // consumed, but no room yet to say so
    int input_eof, sent_close;
    char host[SSH_FORWARD_HOST_MAX + 1];   // requested destination, printable
    uint32_t port;
} ssh_forward;
typedef struct {
    br_hmac_drbg_context rng;
    uint8_t host_private[32], host_public[65];
    ssh_public_key keys[SSH_KEYS_MAX];
    size_t key_count;
    ssh_direction rx, tx;
    uint8_t pending_keys[6][32], session_id[32];
    uint8_t client_kex[8192], server_kex[1024];
    size_t client_kex_len, server_kex_len;
    char client_ident[256];
    size_t ident_len;
    int identified, established, strict, kex, skip_guess;
    int service, authenticated, failures, closed;
    uint8_t packet[SSH_PACKET_MAX + 40];
    size_t packet_have, packet_need;
    int header_decrypted;
    uint8_t output[SSH_OUTPUT_CAP];
    size_t out_head, out_len;
    uint8_t scratch[SSH_PACKET_MAX + 40];
    uint8_t deferred[8192];
    size_t deferred_len, deferred_wire; // stored replies, and their framed cost
    char error[128], username[256], command[SSH_COMMAND_MAX + 1];
    int channel, started, pty, request_reply, input_eof, sent_close;
    int peer_close;                         // the session channel's CLOSE arrived
    uint32_t credit_owed;                   // session input consumed, not yet adjusted
    uint32_t peer_channel, peer_window, peer_packet, receive_window;
    uint32_t cols, rows;
    uint32_t resize_cols, resize_rows; // proposal exposed by SSH_EVENT_RESIZE
    char term[128];
    ssh_forward forwards[SSH_FORWARDS];
    enum ssh_forward_policy forward_policy;
    uint32_t event_forward;                 // the forward a FORWARD_* event names
    enum ssh_event event;
    const uint8_t *event_data;
    size_t event_len;
} ssh_engine;

uint32_t ssh_u32(ssh_reader *r);
uint8_t ssh_byte(ssh_reader *r);
ssh_reader ssh_string(ssh_reader *r);
int ssh_equal(ssh_reader r, const char *s);
void ssh_put_u32(ssh_writer *w, uint32_t n);
void ssh_put_byte(ssh_writer *w, uint8_t n);
void ssh_put_bytes(ssh_writer *w, const void *p, size_t n);
void ssh_put_string(ssh_writer *w, const void *p, size_t n);
void ssh_put_text(ssh_writer *w, const char *s);
void ssh_put_mpint(ssh_writer *w, const uint8_t *p, size_t n);
void ssh_wipe(void *p, size_t n);
int ssh_host_public(const uint8_t private_key[32], uint8_t public_key[65]);
size_t ssh_public_blob(uint8_t *out, size_t cap, const uint8_t public_key[65]);
int ssh_parse_public(const uint8_t *blob, size_t len, uint8_t point[65]);
int ssh_base64_decode(const char *s, size_t n, uint8_t *out, size_t cap);
size_t ssh_base64_encode(const uint8_t *p, size_t n, char *out, size_t cap);
int ssh_authorized_line(ssh_engine *s, const char *line, size_t n);
int ssh_private_parse(const char *text, size_t n, uint8_t private_key[32]);
int ssh_init(ssh_engine *s, const uint8_t seed[32], const uint8_t host_key[32]);
size_t ssh_receive(ssh_engine *s, const uint8_t *data, size_t len);
size_t ssh_output(ssh_engine *s, const uint8_t **data);
void ssh_output_consume(ssh_engine *s, size_t n);
void ssh_disconnect(ssh_engine *s, uint32_t reason, const char *description);
void ssh_start_result(ssh_engine *s, int success);
/* The accepted pty-req's terminal type as a TERM value, or NULL when there
 * is no PTY or the name is not a plain printable one. */
const char *ssh_term_env(const ssh_engine *s);
/* Complete SSH_EVENT_RESIZE before receiving another packet. Success commits
 * the proposed geometry; either result sends the requested channel reply. */
void ssh_resize_result(ssh_engine *s, int success);
size_t ssh_send_data(ssh_engine *s, const uint8_t *p, size_t n, int stderr_stream);
/* Credit the client's window for n bytes of session input that have left.
 * The credit is owed until it can go out as one WINDOW_ADJUST: never during
 * a key exchange (every packet then waits in the deferred-reply budget, one
 * per call) and never without output room. Call with n = 0 to retry. */
void ssh_input_consumed(ssh_engine *s, uint32_t n);
void ssh_send_exit(ssh_engine *s, uint32_t status);
void ssh_rekey(ssh_engine *s);
/* Forwards. SSH_EVENT_FORWARD_OPEN names a forward whose host and port the
 * caller must judge and dial; complete it with ssh_forward_result before
 * receiving another packet (success confirms the channel, failure sends
 * OPEN_FAILURE with `reason`, RFC 4254 section 5.1, and frees the slot).
 * FORWARD_DATA carries client bytes for the local connection, to be credited
 * back with ssh_forward_consumed as they leave, by ssh_input_consumed's
 * rule (owed through a key exchange or a full output queue; n = 0 retries).
 * ssh_forward_send sends what fits the output queue's free room. FORWARD_EOF is the client's half-close. FORWARD_CLOSE means
 * the channel is gone (SSH_FORWARD_CLOSED): nothing more goes to the client,
 * the caller delivers what the client already sent, closes its connection,
 * and frees the slot with ssh_forward_release. */
void ssh_forward_result(ssh_engine *s, uint32_t forward, int success,
                        uint32_t reason, const char *text);
size_t ssh_forward_send(ssh_engine *s, uint32_t forward, const uint8_t *p, size_t n);
void ssh_forward_consumed(ssh_engine *s, uint32_t forward, uint32_t n);
/* The local connection ended: send EOF and CLOSE. Returns 0 when that must
 * wait (a key exchange, or no output room); call again next turn. */
int ssh_forward_finish(ssh_engine *s, uint32_t forward);
void ssh_forward_release(ssh_engine *s, uint32_t forward);
/* The output queue has no room for one more byte of channel data: a send cut
 * short now was cut by the queue, not by that channel's own window. */
int ssh_output_full(const ssh_engine *s);
/* Forwards not yet free, whatever their state. */
uint32_t ssh_forwards_live(const ssh_engine *s);
#endif
