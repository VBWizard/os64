#ifndef OS64_TLS13_ENGINE_H
#define OS64_TLS13_ENGINE_H
#include "tls13_internal.h"

#define TLS13_MESSAGE_MAX 8192u
// These codes are separate from Bear's errors and sent/received alert ranges.
enum {
    TLS13_ERR_DOWNGRADE = 768, TLS13_ERR_SECOND_HRR, TLS13_ERR_SESSION_ID,
    TLS13_ERR_KEY_SHARE, TLS13_ERR_PKCS1, TLS13_ERR_ALPN,
    TLS13_ERR_PEER_TLS12, TLS13_ERR_RETRY, TLS13_ERR_SIGNATURE_SCHEME
};

typedef struct {
    const tls13_group *group;
    unsigned char scalar[48], point[97];
} tls13_share;
typedef struct {
    const br_x509_class **validator;
    const char *hostname;
    const char *const *alpn;
    size_t alpn_count;
    const tls13_suite *suite;
    br_multihash_context transcript;
    br_hmac_drbg_context rng;
    tls13_record read_key, write_key;
    tls13_share shares[2];
    unsigned char random[32], session[32];
    unsigned char client_hs[48], server_hs[48], master[48], client_ap[48], server_ap[48];
    unsigned char recv[TLS13_RECV_MAX], send[TLS13_SEND_MAX];
    // Accepted plaintext stays separate so a requested KeyUpdate can precede
    // its next record without losing or sending those bytes under the old key.
    unsigned char app[TLS13_CONTENT_MAX];
    unsigned char message[TLS13_MESSAGE_MAX], header[4];
    char selected[256];
    size_t recv_used, recv_need, send_used, send_at, app_used;
    size_t plain_at, plain_end, header_used, message_used, message_length;
    // Streaming Certificate / NewSessionTicket framing state. Counts refer to
    // the current message, so a record split cannot reset a length check.
    size_t stream_left, field_value;
    unsigned field_used, stream_stage, phase, message_type, share_count;
    unsigned retry_group, error;
    uint16_t version;
    bool retry, ccs_sent, certificate_requested, authenticated;
    bool read_encrypted, write_encrypted, peer_closed, local_closed, closing;
    bool update_pending, flush_pending;
#ifdef OS64_TLS13_TEST
    uint64_t test_coverage;
#endif
} tls13_engine;

// References are owned by the containing wrapper and outlive this engine.
// Init needs a zeroed engine and a fresh 32-byte entropy seed.
int tls13_init(tls13_engine *engine, const br_x509_class **validator,
               const char *hostname, const char *const *alpn, size_t alpn_count,
               const unsigned char seed[32]);
unsigned tls13_current_state(tls13_engine *engine);
int tls13_last_error(const tls13_engine *engine);
const char *tls13_selected_protocol(const tls13_engine *engine);
void tls13_flush(tls13_engine *engine);
void tls13_close(tls13_engine *engine);
unsigned char *tls13_recvrec_buf(tls13_engine *engine, size_t *length);
unsigned char *tls13_sendrec_buf(tls13_engine *engine, size_t *length);
unsigned char *tls13_recvapp_buf(tls13_engine *engine, size_t *length);
unsigned char *tls13_sendapp_buf(tls13_engine *engine, size_t *length);
void tls13_recvrec_ack(tls13_engine *engine, size_t length);
void tls13_sendrec_ack(tls13_engine *engine, size_t length);
void tls13_recvapp_ack(tls13_engine *engine, size_t length);
void tls13_sendapp_ack(tls13_engine *engine, size_t length);
#ifdef OS64_TLS13_TEST
// Host-only parser fuzz seam: complete authenticated record content, with its
// boundary intact. This symbol and the bypass do not exist in production.
void tls13_test_handshake(tls13_engine *engine, const void *data, size_t length);
#endif
#endif
