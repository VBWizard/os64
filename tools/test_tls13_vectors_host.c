#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../userland/libtls/port/tls13_internal.h"

typedef struct { const char *name, *hex; } rfc_field;
typedef struct { unsigned section; const char *actor, *action; const rfc_field *fields; size_t count; } rfc_step;
#include "../userland/libtls/test/tls13_vectors.h"
typedef struct {
    unsigned suite; const char *early, *derived, *shared, *handshake, *client_hs, *server_hs;
    const char *master, *client_ap, *server_ap, *key, *iv, *finished, *updated, *retry_hash;
    const char *updated_key, *updated_iv, *updated_wire;
} diff_schedule;
typedef struct { unsigned group; const char *scalar, *peer, *secret; } diff_ecdh;
typedef struct {
    unsigned suite; uint64_t sequence; const char *key, *iv, *inner, *wire;
    int error; bool application;
} diff_record;
#include "tls13_independent.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static const char *operation;
static unsigned comparisons;
static unsigned nibble(char c)
{
    if (c >= '0' && c <= '9') return (unsigned)(c - '0');
    CHECK(c >= 'a' && c <= 'f');
    return (unsigned)(c - 'a' + 10);
}
static size_t unhex(const char *hex, void *out, size_t capacity)
{
    CHECK(hex != NULL);
    size_t n = strlen(hex);
    CHECK(!(n & 1) && n / 2 <= capacity);
    unsigned char *p = out;
    for (size_t i = 0; i < n / 2; i++) p[i] = (unsigned char)(16 * nibble(hex[2*i]) + nibble(hex[2*i+1]));
    return n / 2;
}
static void equal_hex(const void *value, size_t length, const char *expected)
{
    unsigned char bytes[TLS13_RECV_MAX];
    size_t n = unhex(expected, bytes, sizeof bytes);
    if (n != length || memcmp(value, bytes, n)) {
        fprintf(stderr, "Mismatch: %s (%zu versus %zu bytes)\n", operation, length, n);
        exit(1);
    }
    comparisons++;
}
static bool zero(const void *data, size_t length)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < length; i++) if (p[i]) return false;
    return true;
}
static const char *field(const rfc_step *s, const char *name)
{
    for (size_t i = 0; i < s->count; i++) if (!strcmp(s->fields[i].name, name)) return s->fields[i].hex;
    return NULL;
}
static size_t read_field(const rfc_step *s, const char *name, void *out, size_t capacity)
{
    return unhex(field(s, name), out, capacity);
}
static void expand_vector(const rfc_step *step, const char *info_name, const char *value_name)
{
    unsigned char prk[48], info[514], out[48];
    CHECK(read_field(step, "PRK", prk, sizeof prk) == 32);
    size_t n = read_field(step, info_name, info, sizeof info);
    CHECK(n >= 10 && info[0] == 0 && info[2] >= 7);
    CHECK(!memcmp(info + 3, "tls13 ", 6));
    size_t label_len = info[2] - 6, context_at = 3 + info[2];
    char label[250];
    memcpy(label, info + 9, label_len); label[label_len] = 0;
    CHECK(context_at < n && context_at + 1 + info[context_at] == n);
    CHECK(!tls13_expand_label(tls13_suite_find(0x1301), prk, label,
        info + context_at + 1, info[context_at], out, info[1]));
    equal_hex(out, info[1], field(step, value_name));
}
static void rfc_vectors(void)
{
    unsigned section = 0, extracts = 0, expands = 0, records = 0, finished = 0, exchanges = 0;
    unsigned char secret[48], out[48], private_key[2][48], public_key[2][97];
    size_t private_len[2] = {0}, public_len[2] = {0};
    tls13_record directions[2] = {0};
    br_multihash_context transcript, empty;
    tls13_transcript_init(&empty);
    const tls13_suite *suite = tls13_suite_find(0x1301);
    for (size_t i = 0; i < sizeof rfc_steps / sizeof *rfc_steps; i++) {
        const rfc_step *step = &rfc_steps[i];
        operation = step->action;
        if (section != step->section) {
            section = step->section;
            tls13_transcript_init(&transcript);
            memset(directions, 0, sizeof directions);
            memset(private_len, 0, sizeof private_len);
            memset(public_len, 0, sizeof public_len);
        }
        unsigned actor = !strcmp(step->actor, "server");
        if (field(step, "private key")) {
            private_len[actor] = read_field(step, "private key", private_key[actor], sizeof private_key[actor]);
            public_len[actor] = read_field(step, "public key", public_key[actor], sizeof public_key[actor]);
            // RFC X25519 scalars are little endian; Bear's scalar API is big endian.
            if (public_len[actor] == 32) {
                for (size_t j = 0; j < 16; j++) {
                    unsigned char b = private_key[actor][j];
                    private_key[actor][j] = private_key[actor][31-j]; private_key[actor][31-j] = b;
                }
            }
        }
        if (field(step, "secret")) {
            unsigned char salt[48] = {0}, ikm[48];
            if (field(step, "salt")) CHECK(read_field(step, "salt", salt, sizeof salt) == 32);
            size_t n = read_field(step, "IKM", ikm, sizeof ikm);
            if (strstr(step->action, "\"handshake\"")) {
                const tls13_group *g = tls13_group_find(public_len[1] == 32 ? 29 : 23);
                CHECK(!tls13_shared_secret(g, private_key[0], private_len[0], public_key[1], public_len[1], out));
                CHECK(n == g->secret_len && !memcmp(out, ikm, n));
                CHECK(!tls13_shared_secret(g, private_key[1], private_len[1], public_key[0], public_len[0], out));
                CHECK(!memcmp(out, ikm, n)); exchanges++;
            }
            CHECK(!tls13_extract(suite, salt, ikm, n, out));
            equal_hex(out, 32, field(step, "secret")); extracts++;
        }
        if (field(step, "info")) { expand_vector(step, "info", "expanded"); expands++; }
        if (field(step, "key info")) {
            expand_vector(step, "key info", "key expanded"); expands++;
            expand_vector(step, "iv info", "iv expanded"); expands++;
            CHECK(read_field(step, "PRK", secret, sizeof secret) == 32);
            unsigned char key[32], iv[12];
            CHECK(!tls13_traffic_keys(suite, secret, key, iv));
            equal_hex(key, 16, field(step, "key expanded")); equal_hex(iv, 12, field(step, "iv expanded"));
            // RFC 8448 omits duplicate calculations: a peer's read keys also
            // establish this direction when its write-key step says "same".
            unsigned direction = strstr(step->action, "write traffic") ? actor : 1 - actor;
            CHECK(!tls13_record_init(&directions[direction], suite, key, iv));
        }
        if (field(step, "finished")) {
            CHECK(read_field(step, "PRK", secret, sizeof secret) == 32);
            CHECK(!tls13_finished(suite, secret, &transcript, out));
            equal_hex(out, 32, field(step, "finished")); finished++;
        }
        // Compare the running transcript at each printed Derive-Secret checkpoint.
        if (field(step, "info") && strstr(step->action, "derive secret")) {
            unsigned char info[128], hash[48];
            read_field(step, "info", info, sizeof info);
            size_t label_len = info[2] - 6;
            char label[64]; CHECK(label_len < sizeof label);
            memcpy(label, info + 9, label_len); label[label_len] = 0;
            const br_multihash_context *t = !strcmp(label, "derived") ? &empty : &transcript;
            CHECK(br_multihash_out(t, br_sha256_ID, hash) == 32);
            equal_hex(hash, 32, field(step, "hash"));
            read_field(step, "PRK", secret, sizeof secret);
            CHECK(!tls13_derive_secret(suite, secret, label, t, out));
            equal_hex(out, 32, field(step, "expanded"));
        }
        if (!strncmp(step->action, "construct ", 10)) {
            unsigned char msg[2048];
            size_t n = unhex(step->fields[0].hex, msg, sizeof msg);
            if (msg[0] == 2 && msg[6] == 0xcf && msg[7] == 0x21) CHECK(!tls13_transcript_retry(&transcript, suite));
            if (msg[0] != 4) br_multihash_update(&transcript, msg, n);
        }
        if (field(step, "complete record")) {
            unsigned char wire[TLS13_RECV_MAX], expected[TLS13_RECV_MAX], encoded[TLS13_SEND_MAX];
            size_t n = read_field(step, "complete record", wire, sizeof wire);
            size_t count, payload_len = read_field(step, "payload", expected, sizeof expected);
            unsigned type;
            if (wire[0] == TLS13_APPLICATION) {
                tls13_record receiver = directions[actor];
                int error = tls13_record_open(&receiver, wire, n, true, &type, &count);
                if (error) fprintf(stderr, "RFC %u %s %s: record error %d\n", step->section, step->actor, step->action, error);
                CHECK(!error);
                CHECK(count == payload_len && !memcmp(wire + 5, expected, count));
                unsigned expected_type = strstr(step->action, "application_data") ? TLS13_APPLICATION :
                    strstr(step->action, "alert") ? TLS13_ALERT : TLS13_HANDSHAKE;
                CHECK(type == expected_type);
                memcpy(encoded + 5, expected, payload_len);
                CHECK(!tls13_record_seal(&directions[actor], type, encoded, payload_len, sizeof encoded, &count));
                equal_hex(encoded, count, field(step, "complete record"));
            } else {
                CHECK(!tls13_plaintext_open(wire, n, true, &type, &count));
                CHECK(count == payload_len && !memcmp(wire + 5, expected, count));
            }
            records++;
        }
    }
    CHECK(extracts == 6 && exchanges == 2 && finished == 4);
    printf("RFC 8448 sections 3/5: %u extracts, %u expansions, %u records, %u Finished, %u ECDH exchanges PASS\n",
           extracts, expands, records, finished, exchanges);
}

static void differential_schedule(void)
{
    unsigned char seed[48], secret[48], derived[48], out[48], shared[48], zeroes[48] = {0};
    unsigned char messages[256], key[32], iv[12];
    for (size_t i = 0; i < sizeof messages; i++) messages[i] = (unsigned char)i;
    for (size_t i = 0; i < sizeof diff_schedules / sizeof *diff_schedules; i++) {
        const diff_schedule *v = &diff_schedules[i];
        const tls13_suite *s = tls13_suite_find((uint16_t)v->suite);
        operation = "independent schedule";
        br_multihash_context t, empty;
        tls13_transcript_init(&t); tls13_transcript_init(&empty);
        for (size_t j = 0; j < sizeof messages; j++) br_multihash_update(&t, messages + j, 1);
        CHECK(!tls13_extract(s, zeroes, zeroes, s->hash_len, seed)); equal_hex(seed,s->hash_len,v->early);
        CHECK(!tls13_derive_secret(s,seed,"derived",&empty,derived)); equal_hex(derived,s->hash_len,v->derived);
        size_t n = unhex(v->shared,shared,sizeof shared);
        CHECK(!tls13_extract(s,derived,shared,n,secret)); equal_hex(secret,s->hash_len,v->handshake);
        CHECK(!tls13_derive_secret(s,secret,"c hs traffic",&t,out)); equal_hex(out,s->hash_len,v->client_hs);
        CHECK(!tls13_finished(s,out,&t,seed)); equal_hex(seed,s->hash_len,v->finished);
        CHECK(!tls13_derive_secret(s,secret,"s hs traffic",&t,out)); equal_hex(out,s->hash_len,v->server_hs);
        CHECK(!tls13_derive_secret(s,secret,"derived",&empty,derived));
        CHECK(!tls13_extract(s,derived,zeroes,s->hash_len,secret)); equal_hex(secret,s->hash_len,v->master);
        br_multihash_update(&t,messages,17);
        CHECK(!tls13_derive_secret(s,secret,"c ap traffic",&t,out)); equal_hex(out,s->hash_len,v->client_ap);
        CHECK(!tls13_derive_secret(s,secret,"s ap traffic",&t,out)); equal_hex(out,s->hash_len,v->server_ap);
        CHECK(!tls13_traffic_keys(s,out,key,iv)); equal_hex(key,s->key_len,v->key); equal_hex(iv,sizeof iv,v->iv);
        tls13_record sender = {0}, old_receiver = {0};
        CHECK(!tls13_record_init(&sender,s,key,iv));
        CHECK(!tls13_record_init(&old_receiver,s,key,iv));
        sender.sequence = TLS13_UPDATE_RECORDS;
        CHECK(!tls13_update_secret(s,out)); equal_hex(out,s->hash_len,v->updated);
        CHECK(!tls13_traffic_keys(s,out,key,iv));
        equal_hex(key,s->key_len,v->updated_key); equal_hex(iv,sizeof iv,v->updated_iv);
        CHECK(!tls13_record_init(&sender,s,key,iv));
        CHECK(!sender.sequence && !tls13_record_needs_update(&sender));
        unsigned char wire[26]; memcpy(wire+5,"next",4);
        size_t wire_len; unsigned type;
        CHECK(!tls13_record_seal(&sender,TLS13_APPLICATION,wire,4,sizeof wire,&wire_len));
        equal_hex(wire,wire_len,v->updated_wire);
        CHECK(tls13_record_open(&old_receiver,wire,wire_len,true,&type,&n) == BR_ERR_BAD_MAC);
        CHECK(!type && !n && zero(wire,wire_len));
        CHECK(!tls13_transcript_retry(&t,s));
        CHECK(br_multihash_out(&t,s->hash_len == 32 ? br_sha256_ID : br_sha384_ID,out) == s->hash_len);
        equal_hex(out,s->hash_len,v->retry_hash);
    }
    for (size_t i = 0; i < sizeof diff_exchanges / sizeof *diff_exchanges; i++) {
        const diff_ecdh *v = &diff_exchanges[i];
        const tls13_group *g = tls13_group_find((uint16_t)v->group);
        unsigned char scalar[48], peer[97];
        size_t sl = unhex(v->scalar,scalar,sizeof scalar), pl = unhex(v->peer,peer,sizeof peer);
        CHECK(!tls13_shared_secret(g,scalar,sl,peer,pl,out)); equal_hex(out,g->secret_len,v->secret);
        memset(peer,0,pl); memset(out,0xa5,sizeof out);
        if (g->id != 29) peer[0] = 4; // Uncompressed framing, but (0,0) is off-curve.
        CHECK(tls13_shared_secret(g,scalar,sl,peer,pl,out) != 0);
        CHECK(zero(out,g->secret_len));
    }
    puts("Independent SHA-256/SHA-384 schedules, HRR, KeyUpdate and all named groups PASS");
}
static void differential_records(void)
{
    unsigned char buffer[TLS13_RECV_MAX], original[TLS13_RECV_MAX], inner[TLS13_RECV_MAX];
    unsigned char key[32], iv[12];
    unsigned accepted = 0, refused = 0;
    for (size_t i = 0; i < sizeof diff_records / sizeof *diff_records; i++) {
        const diff_record *v = &diff_records[i];
        operation = "independent AEAD record";
        const tls13_suite *s = tls13_suite_find((uint16_t)v->suite);
        CHECK(unhex(v->key,key,sizeof key) == s->key_len);
        CHECK(unhex(v->iv,iv,sizeof iv) == sizeof iv);
        size_t n = unhex(v->wire,original,sizeof original), ilen = unhex(v->inner,inner,sizeof inner);
        tls13_record r = {0};
        CHECK(!tls13_record_init(&r,s,key,iv)); r.sequence = v->sequence;
        memcpy(buffer,original,n);
        unsigned type = 99; size_t length = 99;
        CHECK(tls13_record_open(&r,buffer,n,v->application,&type,&length) == v->error);
        if (v->error) {
            CHECK(!type && !length && r.error == v->error);
            CHECK(zero(buffer,n) && zero(r.key,sizeof r.key) && zero(r.iv,sizeof r.iv) && zero(&r.aes,sizeof r.aes));
            CHECK(tls13_record_open(&r,buffer,n,true,&type,&length) == v->error);
            refused++; continue;
        }
        size_t at = ilen;
        while (at && !inner[at-1]) at--;
        CHECK(at && type == inner[at-1] && length == at-1 && !memcmp(buffer+5,inner,length));
        CHECK(r.exhausted == (v->sequence == UINT64_MAX));
        if (!r.exhausted) CHECK(r.sequence == v->sequence+1);
        if (at == ilen) {
            tls13_record sender = {0};
            CHECK(!tls13_record_init(&sender,s,key,iv)); sender.sequence = v->sequence;
            memcpy(buffer+5,inner,ilen-1);
            size_t wn;
            CHECK(!tls13_record_seal(&sender,type,buffer,ilen-1,sizeof buffer,&wn));
            CHECK(wn == n && !memcmp(buffer,original,n));
            if (sender.exhausted) {
                CHECK(tls13_record_seal(&sender,type,buffer,ilen-1,sizeof buffer,&wn) == BR_ERR_LIMIT_EXCEEDED);
                CHECK(!wn && zero(sender.key,sizeof sender.key));
                memcpy(buffer,original,n);
                CHECK(tls13_record_open(&r,buffer,n,true,&type,&length) == BR_ERR_LIMIT_EXCEEDED);
                CHECK(!length && !type && zero(buffer,n));
            }
        }
        const size_t flips[] = {1,5,n-1};
        for (size_t j = 0; j < sizeof flips / sizeof *flips; j++) {
            CHECK(!tls13_record_init(&r,s,key,iv)); r.sequence = v->sequence;
            memcpy(buffer,original,n); buffer[flips[j]] ^= 1;
            CHECK(tls13_record_open(&r,buffer,n,true,&type,&length) == BR_ERR_BAD_MAC);
            CHECK(!length && !type && zero(buffer,n));
        }
        CHECK(!tls13_record_init(&r,s,key,iv)); r.sequence = v->sequence ^ 1;
        memcpy(buffer,original,n);
        CHECK(tls13_record_open(&r,buffer,n,true,&type,&length) == BR_ERR_BAD_MAC);
        // A successfully received record cannot be replayed in the next slot.
        if (v->sequence != UINT64_MAX) {
            CHECK(!tls13_record_init(&r,s,key,iv)); r.sequence = v->sequence;
            memcpy(buffer,original,n); CHECK(!tls13_record_open(&r,buffer,n,true,&type,&length));
            memcpy(buffer,original,n); CHECK(tls13_record_open(&r,buffer,n,true,&type,&length) == BR_ERR_BAD_MAC);
        }
        accepted++;
    }
    printf("Independent AEAD: %u accepted, %u valid-tag protocol refusals; tamper/replay/order/nonce exhaustion PASS\n",accepted,refused);
}
static void boundaries(void)
{
    const tls13_suite *s = tls13_suite_find(0x1301);
    CHECK(!tls13_suite_find(0x1304) && !tls13_group_find(25));
    unsigned char secret[48] = {0}, out[64], context[256] = {0};
    char label[251]; memset(label,'a',sizeof label); label[250] = 0;
    memset(out,0xa5,sizeof out);
    CHECK(tls13_expand_label(s,secret,"key",NULL,0,out,33) == BR_ERR_BAD_PARAM);
    CHECK(tls13_expand_label(s,secret,label,NULL,0,out,16) == BR_ERR_BAD_PARAM);
    CHECK(tls13_expand_label(s,secret,"key",context,256,out,16) == BR_ERR_BAD_PARAM);
    CHECK(out[0] == 0xa5);
    label[249] = 0;
    CHECK(!tls13_expand_label(s,secret,label,context,255,out,32));
    br_multihash_context t; tls13_transcript_init(&t);
    CHECK(!tls13_transcript_retry(&t,s));
    CHECK(tls13_derive_secret(tls13_suite_find(0x1302),secret,"derived",&t,out) == BR_ERR_BAD_STATE);
    tls13_record r = {0};
    unsigned char key[32] = {0}, iv[12] = {0}, buffer[TLS13_RECV_MAX];
    CHECK(!tls13_record_init(&r,s,key,iv)); r.sequence = TLS13_UPDATE_RECORDS-1;
    CHECK(!tls13_record_needs_update(&r));
    size_t n; unsigned type;
    CHECK(tls13_record_seal(&r,TLS13_APPLICATION,buffer,0,21,&n) == BR_ERR_BAD_PARAM);
    CHECK(r.sequence == TLS13_UPDATE_RECORDS-1 && !n);
    CHECK(!tls13_record_seal(&r,TLS13_APPLICATION,buffer,0,sizeof buffer,&n));
    CHECK(tls13_record_needs_update(&r));
    key[0] = 1; CHECK(!tls13_record_init(&r,s,key,iv));
    CHECK(!r.sequence && !r.error && !r.exhausted && !tls13_record_needs_update(&r));
    CHECK(tls13_record_seal(&r,TLS13_APPLICATION,buffer,TLS13_CONTENT_MAX+1,sizeof buffer,&n) == BR_ERR_TOO_LARGE);
    CHECK(tls13_record_seal(&r,TLS13_HANDSHAKE,buffer,0,sizeof buffer,&n) == BR_ERR_UNEXPECTED);
    CHECK(tls13_record_seal(&r,TLS13_ALERT,buffer,3,sizeof buffer,&n) == BR_ERR_BAD_ALERT);
    unsigned char ccs[] = {20,3,3,0,1,1};
    CHECK(!tls13_plaintext_open(ccs,sizeof ccs,true,&type,&n) && type == 20 && n == 1);
    CHECK(tls13_plaintext_open(ccs,sizeof ccs,false,&type,&n) == BR_ERR_BAD_CCS && !n && !type);
    ccs[5] = 2; CHECK(tls13_plaintext_open(ccs,sizeof ccs,true,&type,&n) == BR_ERR_BAD_CCS);
    unsigned char header[] = {23,3,3,0x41,0x01};
    CHECK(tls13_record_length(header,true,&n) == BR_ERR_TOO_LARGE);
    header[4] = 0; CHECK(!tls13_record_length(header,true,&n) && n == TLS13_CIPHER_MAX);
    header[0] = 22; CHECK(tls13_record_length(header,false,&n) == BR_ERR_TOO_LARGE);
    header[3] = 0x40; CHECK(!tls13_record_length(header,false,&n) && n == TLS13_CONTENT_MAX);
    header[3] = 0; CHECK(tls13_plaintext_open(header,5,true,&type,&n) == BR_ERR_UNEXPECTED);
    for (size_t prefix = 0; prefix < 22; prefix++) {
        CHECK(!tls13_record_init(&r,s,key,iv));
        CHECK(!tls13_record_seal(&r,TLS13_APPLICATION,buffer,0,sizeof buffer,&n));
        CHECK(!tls13_record_init(&r,s,key,iv));
        CHECK(tls13_record_open(&r,buffer,prefix,true,&type,&n) != 0 && !n && !type);
    }
    tls13_wipe(&r,sizeof r); CHECK(zero(&r,sizeof r));
    puts("Bounds, plaintext/CCS framing, update threshold, rekey reset, truncated records and wiping PASS");
}
int main(void)
{
    rfc_vectors(); differential_schedule(); differential_records(); boundaries();
    printf("TLS 1.3 S1: %u byte comparisons PASS; record context %zu bytes\n",comparisons,sizeof(tls13_record));
    return 0;
}
