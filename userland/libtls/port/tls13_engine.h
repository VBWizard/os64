#ifndef OS64_TLS13_ENGINE_H
#define OS64_TLS13_ENGINE_H
#include "tls13_internal.h"

#define TLS13_MESSAGE_MAX 8192u
// These codes are separate from Bear's errors and sent/received alert ranges.
enum {
    TLS13_ERR_DOWNGRADE = 768, TLS13_ERR_SECOND_HRR, TLS13_ERR_SESSION_ID,
    TLS13_ERR_KEY_SHARE, TLS13_ERR_PKCS1, TLS13_ERR_ALPN,
    TLS13_ERR_PEER_TLS12, TLS13_ERR_RETRY, TLS13_ERR_SIGNATURE_SCHEME,
    TLS13_ERR_PSK
};

// ── Resumption (RFC 8446 2.2, 4.2.11, 4.6.1) ────────────────────────────
//
// A server that has authenticated itself may hand the client TICKETS. A
// ticket and the secret derived beside it are a SESSION: offered in a later
// hello, with a proof the client holds the secret (the binder), it lets the
// server skip its certificate and signature, and the two sides are
// authenticated by having been authenticated before. The key exchange is
// still done (psk_dhe_ke is the one mode offered), so a resumed connection
// has forward secrecy and nothing is ever sent before the handshake ends.
//
// The engine neither stores sessions nor reads a clock. Its owner hands it
// one to offer, and is handed each one that arrives; where they are kept,
// under what key, for how long and how often each is offered is the
// owner's (session_cache.h).
#define TLS13_TICKET_MAX 2048u
#define TLS13_TICKET_LIFETIME_MAX 604800u   // seven days, the RFC's ceiling
typedef struct {
    uint16_t suite;       // the suite of the handshake that issued it; the PSK is of its hash
    uint32_t age_add;     // the server's mask for the age the client reports
    uint32_t age_ms;      // an offer's: milliseconds since the ticket arrived
    size_t ticket_len;
    unsigned char psk[TLS13_HASH_MAX], ticket[TLS13_TICKET_MAX];
} tls13_session;
typedef struct {
    // Say so in the hello (psk_key_exchange_modes). A server learns from it
    // that a ticket would be of use, and some send none without it, so it
    // is wanted on a hello that offers nothing too.
    bool advertise;
    const tls13_session *offer;   // the session to offer, or NULL
    // Where an arriving ticket is assembled. NULL: tickets are consumed and
    // discarded, and no resumption secret is kept.
    tls13_session *arriving;
    // Called once per kept ticket, inside the call that fed its last byte,
    // with `arriving` complete (age_ms 0) and the lifetime in seconds, at
    // most TLS13_TICKET_LIFETIME_MAX. The session is wiped on return.
    void (*received)(void *context, const tls13_session *session, uint32_t lifetime);
    void *context;
} tls13_resumption;

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
    // resumption_master_secret, kept from the client's Finished for as long
    // as tickets may arrive, and only when there is somewhere to put them.
    unsigned char res_master[48];
    tls13_resumption res;
    // The NewSessionTicket being read: its fixed fields and nonce here, its
    // ticket bytes straight into res.arriving when it is to be kept.
    unsigned char ticket_nonce[255];
    size_t nonce_len, ticket_len;
    uint32_t ticket_lifetime, ticket_age_add;
    unsigned tickets_kept, tickets_dropped;
    bool ticket_keep, offered, resumed;
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

// References are owned by the containing wrapper and outlive this engine,
// the resumption's two sessions among them. Init needs a zeroed engine and a
// fresh 32-byte entropy seed; `resumption` is copied, and NULL is none of it.
int tls13_init(tls13_engine *engine, const br_x509_class **validator,
               const char *hostname, const char *const *alpn, size_t alpn_count,
               const unsigned char seed[32], const tls13_resumption *resumption);
// True once the server has accepted the offered session: this handshake is
// authenticated by that one, and carries no certificate.
bool tls13_resumed(const tls13_engine *engine);
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
