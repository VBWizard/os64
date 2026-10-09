#ifndef OS64_TLS13_INTERNAL_H
#define OS64_TLS13_INTERNAL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bearssl.h"

#define TLS13_HASH_MAX 48u
#define TLS13_KEY_MAX 32u
#define TLS13_IV_SIZE 12u
#define TLS13_TAG_SIZE 16u
#define TLS13_CONTENT_MAX 16384u
#define TLS13_INNER_MAX (TLS13_CONTENT_MAX + 1u)
#define TLS13_CIPHER_MAX (TLS13_CONTENT_MAX + 256u)
#define TLS13_SEND_MAX (5u + TLS13_INNER_MAX + TLS13_TAG_SIZE)
#define TLS13_RECV_MAX (5u + TLS13_CIPHER_MAX)
#define TLS13_UPDATE_RECORDS (UINT64_C(1) << 24)

enum { TLS13_CCS = 20, TLS13_ALERT = 21, TLS13_HANDSHAKE = 22, TLS13_APPLICATION = 23 };
typedef struct {
    uint16_t id;
    const br_hash_class *hash;
    size_t hash_len, key_len;
    bool chacha;
} tls13_suite;
// Returned descriptors are immutable. Helpers accept descriptors from this lookup.
const tls13_suite *tls13_suite_find(uint16_t id);
void tls13_wipe(void *data, size_t length);
bool tls13_equal(const void *a, const void *b, size_t length);

// Outputs need hash_len bytes, except Expand-Label (length bytes) and keys.
// Extract takes an explicit hash-sized salt; absent PSK/IKM is hash_len zeros.
int tls13_extract(const tls13_suite *suite, const void *salt,
                  const void *ikm, size_t ikm_len, void *out);
int tls13_expand_label(const tls13_suite *suite, const void *secret,
                       const char *label, const void *context, size_t context_len,
                       void *out, size_t length);
int tls13_derive_secret(const tls13_suite *suite, const void *secret,
                        const char *label, const br_multihash_context *transcript, void *out);
int tls13_traffic_keys(const tls13_suite *suite, const void *secret, void *key, void *iv);
int tls13_finished(const tls13_suite *suite, const void *traffic_secret,
                   const br_multihash_context *transcript, void *out);
int tls13_update_secret(const tls13_suite *suite, void *traffic_secret);
void tls13_transcript_init(br_multihash_context *transcript);
// Hash complete handshake framing, not record headers or compatibility CCS.
// The engine validates message ordering and calls retry at most once.
int tls13_transcript_retry(br_multihash_context *transcript, const tls13_suite *suite);

typedef struct { uint16_t id; int curve; size_t point_len, secret_len; } tls13_group;
const tls13_group *tls13_group_find(uint16_t id);
// Scalar is a local private key from br_ec_keygen, in BearSSL's big endian
// encoding (including X25519). Output needs group->secret_len bytes. The
// peer point is borrowed; invalid peer points wipe the shared-secret output.
int tls13_shared_secret(const tls13_group *group, const void *scalar, size_t scalar_len,
                         const void *peer, size_t peer_len, void *out);

typedef struct {
    const tls13_suite *suite;
    br_aes_ct64_ctr_keys aes;
    unsigned char key[TLS13_KEY_MAX], iv[TLS13_IV_SIZE];
    uint64_t sequence;
    int error;
    bool exhausted;
} tls13_record;

// One context per direction and key epoch. Init replaces/wipes the old key and
// resets the sequence. Zero-initialize before use; destroy via tls13_wipe.
int tls13_record_init(tls13_record *record, const tls13_suite *suite,
                      const void *key, const void *iv);
bool tls13_record_needs_update(const tls13_record *record);
// The caller places content at buffer+5. Seal appends type/tag with no padding.
// On a local argument/capacity error, no bytes or sequence numbers are consumed.
int tls13_record_seal(tls13_record *record, unsigned type, unsigned char *buffer,
                      size_t content_len, size_t capacity, size_t *wire_len);
// Open consumes one complete wire record in place. Success exposes content at
// buffer+5 and excludes type/padding/tag. Authentication or protocol failure
// wipes the supplied record and keys, makes the error sticky, and returns no
// content. The handshake engine owns application readiness and record ordering.
int tls13_record_open(tls13_record *record, unsigned char *buffer, size_t wire_len,
                      bool application_allowed, unsigned *type, size_t *content_len);
// Header validation supports incremental input without peer-sized allocation.
// encrypted=false covers unprotected handshake/alert/CCS; CCS timing is checked
// by plaintext_open. The legacy version bytes do not select a protocol version.
int tls13_record_length(const unsigned char header[5], bool encrypted, size_t *payload_len);
int tls13_plaintext_open(const unsigned char *buffer, size_t wire_len,
                         bool ccs_allowed, unsigned *type, size_t *content_len);
#endif
