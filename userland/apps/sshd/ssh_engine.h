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

/* The caller owns time, I/O and process lifetime. One thread owns this engine.
 * receive stops at an event; its data remains valid until the next receive.
 * Drain output before retrying a receive that consumed zero bytes. */
enum ssh_event { SSH_EVENT_NONE, SSH_EVENT_EXEC, SSH_EVENT_SHELL,
    SSH_EVENT_INPUT, SSH_EVENT_EOF, SSH_EVENT_RESIZE, SSH_EVENT_CLOSE,
    SSH_EVENT_AUTH };
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
    uint32_t peer_channel, peer_window, peer_packet, receive_window;
    uint32_t cols, rows;
    uint32_t resize_cols, resize_rows; // proposal exposed by SSH_EVENT_RESIZE
    char term[128];
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
void ssh_input_consumed(ssh_engine *s, uint32_t n);
void ssh_send_exit(ssh_engine *s, uint32_t status);
void ssh_rekey(ssh_engine *s);
#endif
