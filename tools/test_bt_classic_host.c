// test_bt_classic_host.c — kernel/src/driver/system/usb/bt_classic.c
// against a MODEL HEADPHONE: a controller and the headphones behind it that
// take each command the link sends, check its bytes exactly, and answer with
// the events a real pair would, in a realistic order and after realistic
// delays. Scripted scenarios first (bonding, reconnecting, being called,
// refusing, failing at every phase), then the bond codec, every event at
// every wrong length, and random soup under ASan and UBSan.
//
//   tools/test_bt_classic_host.sh [random rounds, default 2000]

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/system/usb/bt_classic.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; \
    printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { failures++; \
    printf("FAIL %s:%d: %s = %lld (0x%llx), want %s = %lld (0x%llx)\n", __func__, __LINE__, \
           #a, a_, (unsigned long long)a_, #b, b_, (unsigned long long)b_); } } while (0)
#define CHECK_TEXT(c, want) do { char t_[2048]; bt_classic_status((c), t_, sizeof t_); \
    if (!strstr(t_, (want))) { failures++; printf("FAIL %s:%d: status lacks \"%s\":\n%s", \
        __func__, __LINE__, (want), t_); } } while (0)

static const uint8_t LOCAL[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
static const uint8_t PHONES[6] = {0xa1, 0xb2, 0xc3, 0x5d, 0x87, 0x4c};    // 4c:87:5d:c3:b2:a1
static const char PHONES_TEXT[] = "4c:87:5d:c3:b2:a1";
static const uint8_t STRANGER[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
static const uint8_t OLD_KEY[16] = {0x9a, 0x7e, 0x31, 0xc4, 0x58, 0xe2, 0x0f, 0xb6,
                                    0x73, 0xd9, 0x2c, 0x84, 0xf1, 0x46, 0xab, 0x1d};
static const uint8_t NEW_KEY[16] = {0x5e, 0xc7, 0x02, 0x9b, 0xe4, 0x38, 0x71, 0xad,
                                    0x16, 0xf3, 0x8c, 0x4f, 0xd0, 0x67, 0xba, 0x29};

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

// ── The model ───────────────────────────────────────────────────────────────

typedef struct { uint64_t at; int tag; size_t n; uint8_t b[64]; } ev_t;
typedef struct { uint64_t at; uint16_t op; uint8_t len, p[32]; } cmd_t;
enum { TAG_NONE, TAG_PAGE };

typedef struct {
    uint64_t now;
    bool control;            // what the link is told each tick
    ev_t q[64]; unsigned qn;
    cmd_t log[256]; unsigned logn;
    unsigned sends_this_tick;
    // The controller's view of the ACL link.
    bool live; uint16_t handle, next_handle;
    bool page_outstanding;   // a page whose outcome is still queued
    uint8_t scan, scan_writes, scan_reply;   // Write Scan Enable as received; the status to answer with
    // The headphones.
    uint8_t addr[6];
    bool present;            // answers a page
    uint8_t page_status;     // how an unanswered page ends (0: page timeout, 0x04)
    bool has_key; uint8_t key[16];   // the key they hold for us
    uint8_t key_size;
    bool they_secure;        // after a link they called, they authenticate and encrypt themselves
    bool they_authenticating;// the authentication on this link is theirs
    uint8_t expect_auth;     // what IO Capability Request Reply must ask for
    uint8_t expect_reason;   // what a Disconnect must give (0: not checked)
    bool silent;             // nothing answers
    bool reply_only;         // command replies, no events after them
    uint8_t key_type;        // what Link Key Notification says the new key is
    // Sniff: the policy the controller holds (written by a good Write Link
    // Policy Settings), the two commands as received, the statuses they are
    // answered with, and the headphones' mode.
    uint16_t policy; unsigned policy_writes, wakes;
    uint8_t policy_reply, wake_reply;
    uint16_t drop_op;        // answered with nothing at all
    uint16_t malform_op;     // answered with a reply of the wrong shape
    bool wrong_handle;       // Write Link Policy Settings answered good, for another handle
    uint64_t reply_delay;    // the two commands' replies come this late (0: 1 ms)
    bool sniff;
} model_t;

static model_t M;
static bool g_control;
static unsigned g_sends;

static void push(model_t *m, uint64_t delay, int tag, const uint8_t *b, size_t n)
{
    if (m->qn == 64) { failures++; printf("FAIL model queue full\n"); return; }
    ev_t *e = &m->q[m->qn++];
    e->at = m->now + delay; e->tag = tag; e->n = n; memcpy(e->b, b, n);
}
#define PUSH(m, delay, tag, ...) do { const uint8_t b_[] = {__VA_ARGS__}; push((m), (delay), (tag), b_, sizeof b_); } while (0)

static void ev_cs(model_t *m, uint64_t d, uint8_t status, uint16_t op)
{ PUSH(m, d, TAG_NONE, 0x0f, 4, status, 1, (uint8_t)op, (uint8_t)(op >> 8)); }
static void ev_cc_addr(model_t *m, uint64_t d, uint16_t op, uint8_t status, const uint8_t *a)
{ PUSH(m, d, TAG_NONE, 0x0e, 10, 1, (uint8_t)op, (uint8_t)(op >> 8), status, a[0], a[1], a[2], a[3], a[4], a[5]); }
static void ev_cc_status(model_t *m, uint64_t d, uint16_t op, uint8_t status)
{ PUSH(m, d, TAG_NONE, 0x0e, 4, 1, (uint8_t)op, (uint8_t)(op >> 8), status); }
static void ev_keysize(model_t *m, uint64_t d, uint8_t status, uint16_t h, uint8_t size)
{ PUSH(m, d, TAG_NONE, 0x0e, 7, 1, 0x08, 0x14, status, (uint8_t)h, (uint8_t)(h >> 8), size); }
static void ev_conn(model_t *m, uint64_t d, int tag, uint8_t status, uint16_t h, const uint8_t *a, uint8_t type)
{ PUSH(m, d, tag, 0x03, 11, status, (uint8_t)h, (uint8_t)(h >> 8), a[0], a[1], a[2], a[3], a[4], a[5], type, 0); }
static void ev_request(model_t *m, uint64_t d, const uint8_t *a, uint8_t type)
{ PUSH(m, d, TAG_NONE, 0x04, 10, a[0], a[1], a[2], a[3], a[4], a[5], 0x18, 0x04, 0x24, type); }
static void ev_disconn(model_t *m, uint64_t d, uint8_t status, uint16_t h, uint8_t reason)
{ PUSH(m, d, TAG_NONE, 0x05, 4, status, (uint8_t)h, (uint8_t)(h >> 8), reason); }
static void ev_auth(model_t *m, uint64_t d, uint8_t status, uint16_t h)
{ PUSH(m, d, TAG_NONE, 0x06, 3, status, (uint8_t)h, (uint8_t)(h >> 8)); }
static void ev_enc(model_t *m, uint64_t d, uint8_t status, uint16_t h, uint8_t on)
{ PUSH(m, d, TAG_NONE, 0x08, 4, status, (uint8_t)h, (uint8_t)(h >> 8), on); }
static void ev_addr(model_t *m, uint64_t d, uint8_t code, const uint8_t *a)
{ PUSH(m, d, TAG_NONE, code, 6, a[0], a[1], a[2], a[3], a[4], a[5]); }
static void ev_confirm(model_t *m, uint64_t d, const uint8_t *a)
{ PUSH(m, d, TAG_NONE, 0x33, 10, a[0], a[1], a[2], a[3], a[4], a[5], 0x40, 0xe2, 0x01, 0x00); }
static void ev_io_response(model_t *m, uint64_t d, const uint8_t *a)
{ PUSH(m, d, TAG_NONE, 0x32, 9, a[0], a[1], a[2], a[3], a[4], a[5], 0x03, 0x00, 0x04); }
static void ev_ssp(model_t *m, uint64_t d, uint8_t status, const uint8_t *a)
{ PUSH(m, d, TAG_NONE, 0x36, 7, status, a[0], a[1], a[2], a[3], a[4], a[5]); }
static void ev_role(model_t *m, uint64_t d, const uint8_t *a, uint8_t role)
{ PUSH(m, d, TAG_NONE, 0x12, 8, 0, a[0], a[1], a[2], a[3], a[4], a[5], role); }
static void ev_mode(model_t *m, uint64_t d, uint8_t status, uint16_t h, uint8_t mode, uint16_t interval)
{ PUSH(m, d, TAG_NONE, 0x14, 6, status, (uint8_t)h, (uint8_t)(h >> 8), mode, (uint8_t)interval, (uint8_t)(interval >> 8)); }
static void ev_key_notify(model_t *m, uint64_t d, const uint8_t *a, const uint8_t *key, uint8_t type)
{
    uint8_t b[25] = {0x18, 23};
    memcpy(b + 2, a, 6); memcpy(b + 8, key, 16); b[24] = type;
    push(m, d, TAG_NONE, b, sizeof b);
}

static void unqueue(model_t *m, int tag)
{
    unsigned k = 0;
    for (unsigned i = 0; i < m->qn; i++) if (m->q[i].tag != tag) m->q[k++] = m->q[i];
    m->qn = k;
}

#define WANT(cond) do { if (!(cond)) { failures++; printf("FAIL model: %04x %s\n", op, #cond); } } while (0)

// The controller and the headphones answer one command.
static void model_send(void *context, uint16_t op, const uint8_t *p, uint8_t len)
{
    model_t *m = context;
    if (!g_control) { failures++; printf("FAIL command %04x sent without control\n", op); }
    g_sends++; m->sends_this_tick++;
    if (m->logn < 256) {
        cmd_t *c = &m->log[m->logn++];
        c->at = m->now; c->op = op; c->len = len; memcpy(c->p, p, len < 32 ? len : 32);
    }
    if (m->silent) return;
    bool ours = len >= 6 && !memcmp(p, m->addr, 6);
    switch (op) {
    case 0x0405:
        WANT(len == 13); WANT(ours); WANT(u16(p + 6) == 0xcc18); WANT(p[8] == 0x02); WANT(p[9] == 0);
        WANT(u16(p + 10) == 0); WANT(p[12] == 1);
        ev_cs(m, 1, 0, op);
        if (m->reply_only) break;
        m->page_outstanding = true; m->they_authenticating = false;
        if (m->present) { m->live = true; m->handle = m->next_handle++; ev_conn(m, 60, TAG_PAGE, 0, m->handle, m->addr, 1); }
        else ev_conn(m, 5120, TAG_PAGE, m->page_status ? m->page_status : 0x04, 0, m->addr, 1);
        break;
    case 0x0408:
        WANT(len == 6); WANT(ours);
        if (m->page_outstanding && !m->live) {
            unqueue(m, TAG_PAGE); m->page_outstanding = false;
            ev_cc_addr(m, 1, op, 0, p); ev_conn(m, 20, TAG_NONE, 0x02, 0, m->addr, 1);
        } else ev_cc_addr(m, 1, op, m->live ? 0x0b : 0x02, p);
        break;
    case 0x0406:
        WANT(len == 3);
        if (m->expect_reason) WANT(p[2] == m->expect_reason);
        WANT(p[2] == 0x05 || p[2] == 0x13);
        if (m->live && u16(p) == m->handle) {
            ev_cs(m, 1, 0, op); ev_disconn(m, 20, 0, m->handle, 0x16); m->live = false;
        } else ev_cs(m, 1, 0x02, op);
        break;
    case 0x0409:
        WANT(len == 7); WANT(ours); WANT(p[6] == 0x01);
        ev_cs(m, 1, 0, op);
        if (m->reply_only) break;
        m->live = true; m->handle = m->next_handle++;
        ev_conn(m, 30, TAG_NONE, 0, m->handle, m->addr, 1);
        ev_role(m, 35, m->addr, 0x01);
        if (m->they_secure) { m->they_authenticating = true; ev_addr(m, 60, 0x17, m->addr); }
        break;
    case 0x040a:   // unacceptable address, or the bonded peer while busy (each test checks which)
        WANT(len == 7); WANT(p[6] == 0x0f || p[6] == 0x0d);
        ev_cs(m, 1, 0, op); ev_conn(m, 20, TAG_NONE, p[6], 0, p, 1);
        break;
    case 0x042a:
        WANT(len == 7); WANT(p[6] == 0x0d);
        ev_cs(m, 1, 0, op); ev_conn(m, 20, TAG_NONE, 0x0d, 0, p, 0);
        break;
    case 0x040b:
        WANT(len == 22); WANT(ours);
        ev_cc_addr(m, 1, op, 0, p);
        if (m->reply_only) break;
        if (m->has_key && !memcmp(p + 6, m->key, 16)) {
            if (m->they_authenticating) { ev_enc(m, 40, 0, m->handle, 1); }
            else ev_auth(m, 40, 0, m->handle);
        } else if (!m->they_authenticating) ev_auth(m, 40, 0x06, m->handle);
        break;
    case 0x040c:
        WANT(len == 6);
        ev_cc_addr(m, 1, op, 0, p);
        if (ours && !m->reply_only) ev_addr(m, 20, 0x31, m->addr);
        break;
    case 0x042b:
        WANT(len == 9); WANT(ours); WANT(p[6] == 0x03); WANT(p[7] == 0x00); WANT(p[8] == m->expect_auth);
        ev_cc_addr(m, 1, op, 0, p);
        if (m->reply_only) break;
        ev_io_response(m, 10, m->addr); ev_confirm(m, 25, m->addr);
        break;
    case 0x042c:
        WANT(len == 6); WANT(ours);
        ev_cc_addr(m, 1, op, 0, p);
        if (m->reply_only) break;
        ev_ssp(m, 30, 0, m->addr);
        ev_key_notify(m, 35, m->addr, NEW_KEY, m->key_type);
        m->has_key = true; memcpy(m->key, NEW_KEY, 16);
        if (m->they_authenticating) ev_enc(m, 45, 0, m->handle, 1);
        else ev_auth(m, 45, 0, m->handle);
        break;
    case 0x040e: case 0x042d:
        WANT(len == 6);
        ev_cc_addr(m, 1, op, 0, p);
        break;
    case 0x0434:
        WANT(len == 7); WANT(p[6] == 0x18);
        ev_cc_addr(m, 1, op, 0, p);
        break;
    case 0x0411:
        WANT(len == 2); WANT(m->live && u16(p) == m->handle);
        ev_cs(m, 1, 0, op);
        if (m->reply_only) break;
        m->they_authenticating = false;
        ev_addr(m, 20, 0x17, m->addr);
        break;
    case 0x0413:
        WANT(len == 3); WANT(m->live && u16(p) == m->handle); WANT(p[2] == 1);
        ev_cs(m, 1, 0, op);
        if (!m->reply_only) ev_enc(m, 20, 0, m->handle, 1);
        break;
    case 0x1408:
        WANT(len == 2); WANT(m->live && u16(p) == m->handle);
        ev_keysize(m, 1, 0, m->handle, m->key_size);
        break;
    case 0x0c1a:
        WANT(len == 1); WANT(p[0] == 0x00 || p[0] == 0x02);
        m->scan = p[0]; m->scan_writes++;
        ev_cc_status(m, 1, op, m->scan_reply);
        break;
    case 0x080d: {   // Write Link Policy Settings: handle, role switch and perhaps sniff
        uint64_t d = m->reply_delay ? m->reply_delay : 1;
        WANT(len == 4); WANT(m->live && u16(p) == m->handle); WANT(u16(p + 2) == 0x0001 || u16(p + 2) == 0x0005);
        m->policy_writes++;
        if (op == m->drop_op) break;
        if (op == m->malform_op) { ev_cc_status(m, d, op, 0); break; }   // the status alone: no handle
        if (!m->policy_reply) m->policy = u16(p + 2);
        uint16_t h = m->wrong_handle ? (uint16_t)(u16(p) + 1) : u16(p);
        PUSH(m, d, TAG_NONE, 0x0e, 6, 1, 0x0d, 0x08, m->policy_reply, (uint8_t)h, (uint8_t)(h >> 8));
        break;
    }
    case 0x0804: {   // Exit Sniff Mode: handle
        uint64_t d = m->reply_delay ? m->reply_delay : 1;
        WANT(len == 2); WANT(m->live && u16(p) == m->handle);
        m->wakes++;
        if (op == m->drop_op) break;
        if (op == m->malform_op) { ev_cc_status(m, d, op, 0); break; }   // Command Complete for an asynchronous command
        if (m->wake_reply || !m->sniff) { ev_cs(m, d, m->wake_reply ? m->wake_reply : 0x0c, op); break; }
        ev_cs(m, d, 0, op); ev_mode(m, d + 20, 0, m->handle, 0, 0); m->sniff = false;
        break;
    }
    default:
        failures++; printf("FAIL model: unexpected opcode %04x\n", op);
    }
}

static void model_reset(void)
{
    memset(&M, 0, sizeof M);
    memcpy(M.addr, PHONES, 6);
    M.present = true; M.key_size = 16; M.next_handle = 0x0b; M.control = true; M.expect_auth = 0x04; M.key_type = 0x04;
}

// One 10 ms step: what has arrived is delivered, then the link ticks.
static void step(bt_classic_t *c, model_t *m, uint64_t dt)
{
    m->now += dt;
    for (;;) {
        unsigned best = m->qn;
        for (unsigned i = 0; i < m->qn; i++)
            if (m->q[i].at <= m->now && (best == m->qn || m->q[i].at < m->q[best].at)) best = i;
        if (best == m->qn) break;
        ev_t e = m->q[best];
        memmove(&m->q[best], &m->q[best + 1], (m->qn - best - 1) * sizeof(ev_t));
        m->qn--;
        bt_classic_event(c, e.b, e.n);
    }
    m->sends_this_tick = 0;
    g_control = m->control;
    bt_classic_tick(c, m->now, m->control, false, model_send, m);
    if (m->sends_this_tick > 1) { failures++; printf("FAIL %u commands in one tick\n", m->sends_this_tick); }
}
static void run(bt_classic_t *c, model_t *m, uint64_t ms)
{
    for (uint64_t t = 0; t < ms; t += 10) step(c, m, 10);
}
static bool run_until_phase(bt_classic_t *c, model_t *m, bt_classic_phase_t phase, uint64_t ms)
{
    for (uint64_t t = 0; t < ms && c->phase != phase; t += 10) step(c, m, 10);
    return c->phase == phase;
}

static void expect_ops(const char *what, const uint16_t *ops, unsigned n, unsigned from)
{
    bool same = M.logn - from == n;
    for (unsigned i = 0; same && i < n; i++) same = M.log[from + i].op == ops[i];
    if (same) return;
    failures++;
    printf("FAIL %s: commands sent:", what);
    for (unsigned i = from; i < M.logn; i++) printf(" %04x", M.log[i].op);
    printf("\n  wanted:");
    for (unsigned i = 0; i < n; i++) printf(" %04x", ops[i]);
    printf("\n");
}
static const cmd_t *find_op(uint16_t op, unsigned from)
{
    for (unsigned i = from; i < M.logn; i++) if (M.log[i].op == op) return &M.log[i];
    return NULL;
}

// The key in any spelling: three bytes of it in hex either way round, in
// either case, with or without separators between them; or four raw bytes.
static bool text_has_key(const char *t, const uint8_t key[16])
{
    static const char digits[] = "0123456789abcdef";
    char flat[4096];
    size_t n = strlen(t), m = 0;
    for (size_t i = 0; i < n && m + 1 < sizeof flat; i++) {
        char ch = t[i];
        if (ch == ':' || ch == ' ') continue;
        flat[m++] = ch >= 'A' && ch <= 'F' ? (char)(ch - 'A' + 'a') : ch;
    }
    flat[m] = 0;
    for (int i = 0; i + 3 <= 16; i++) {
        char fwd[7], rev[7];
        for (int j = 0; j < 3; j++) {
            fwd[2 * j] = digits[key[i + j] >> 4]; fwd[2 * j + 1] = digits[key[i + j] & 15];
            rev[2 * j] = digits[key[i + 2 - j] >> 4]; rev[2 * j + 1] = digits[key[i + 2 - j] & 15];
        }
        fwd[6] = rev[6] = 0;
        if (strstr(flat, fwd) || strstr(flat, rev)) return true;
    }
    for (size_t at = 0; at + 4 <= n; at++)
        for (int i = 0; i + 4 <= 16; i++)
            if (t[at] == (char)key[i] && !memcmp(t + at, key + i, 4)) return true;
    return false;
}
static void check_no_key(const bt_classic_t *c, const uint8_t key[16])
{
    char t[2048];
    bt_classic_status(c, t, sizeof t);
    if (text_has_key(t, key)) { failures++; printf("FAIL the status text shows a key:\n%s", t); }
}

static bt_classic_bond_t bond_for(const uint8_t *peer, const uint8_t *key)
{
    bt_classic_bond_t b = {.valid = true, .key_type = 0x04};
    memcpy(b.peer, peer, 6); memcpy(b.local, LOCAL, 6); memcpy(b.key, key, 16);
    strcpy(b.name, "SoundLink II");
    return b;
}
static bt_scan_device_t classic_hint(const uint8_t *a, const char *name)
{
    bt_scan_device_t d = {.rssi = -50};
    memcpy(d.address, a, 6); snprintf(d.name, sizeof d.name, "%s", name);
    return d;
}
static bool request(bt_classic_t *c, const char *text, const bt_scan_device_t *hint, const char **why)
{
    const char *dummy;
    return bt_classic_request(c, text, strlen(text), hint, M.now, why ? why : &dummy);
}
static bool all_zero(const uint8_t *p, size_t n)
{
    while (n--) if (*p++) return false;
    return true;
}
static void check_clean(const bt_classic_t *c, const char *what)
{
    if (!bt_classic_quiescent(c) || c->pending || bt_classic_urgent(c) || c->connected ||
        (c->phase != BT_CLASSIC_STOPPED && c->phase != BT_CLASSIC_IDLE) || c->have_key || !all_zero(c->new_key, 16)) {
        failures++;
        char t[2048]; bt_classic_status(c, t, sizeof t);
        printf("FAIL %s: not clean (phase %d pending %d urgent %d connected %d next %04x):\n%s",
               what, c->phase, c->pending, bt_classic_urgent(c), c->connected, c->next_op, t);
    }
}

// A link with its bond loaded (or none) and page scan written.
static void start(bt_classic_t *c, const bt_classic_bond_t *bond)
{
    model_reset();
    bt_classic_init(c, LOCAL);
    bt_classic_set_bond(c, bond);
    run(c, &M, 50);
}

// ── 1, 9, 10: pairing ───────────────────────────────────────────────────────

static void test_bond(void)
{
    bt_classic_t c;
    bt_classic_bond_t none = {0};
    start(&c, &none);
    CHECK_EQ(M.scan_writes, 1); CHECK_EQ(M.scan, 0x00); CHECK_EQ(c.scan_enable_set, 0x00);
    unsigned from = M.logn;
    bt_scan_device_t hint = classic_hint(PHONES, "SoundLink II");
    char text[64];
    snprintf(text, sizeof text, "bond %s", PHONES_TEXT);
    CHECK(request(&c, text, &hint, NULL));
    CHECK_EQ(c.phase, BT_CLASSIC_PAGING);
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_READY, 3000));
    run(&c, &M, 100);
    static const uint16_t ops[] = {0x0405, 0x0411, 0x040c, 0x042b, 0x042c, 0x0413, 0x1408, 0x0c1a};
    expect_ops("bond", ops, 8, from);
    CHECK_EQ(c.phase, BT_CLASSIC_READY);
    CHECK(c.connected); CHECK(c.encrypted); CHECK_EQ(c.key_size, 16);
    CHECK(c.bond.valid); CHECK(c.bond_dirty);
    CHECK(!memcmp(c.bond.key, NEW_KEY, 16)); CHECK_EQ(c.bond.key_type, 0x04);
    CHECK(!memcmp(c.bond.peer, PHONES, 6)); CHECK(!memcmp(c.bond.local, LOCAL, 6));
    CHECK(!strcmp(c.bond.name, "SoundLink II"));
    CHECK(!c.have_key); CHECK(all_zero(c.new_key, 16));
    CHECK_EQ(c.scan_enable_wanted, 0x02); CHECK_EQ(c.scan_enable_set, 0x02); CHECK_EQ(M.scan, 0x02);
    CHECK_EQ(M.scan_writes, 2);
    CHECK(c.ssp_done); CHECK(c.peer_io_known); CHECK_EQ(c.peer_io[2], 0x04);
    CHECK_TEXT(&c, "state: ready\n");
    CHECK_TEXT(&c, "session: bond\n");
    CHECK_TEXT(&c, "bond: 4c:87:5d:c3:b2:a1 key type 04 name SoundLink II (to save: yes)");
    CHECK_TEXT(&c, "encrypted: yes key size: 16");
    CHECK_TEXT(&c, "peer pairing capabilities: IO 03 OOB 00 authentication 04 simple pairing: complete");
    CHECK_TEXT(&c, "page scan: wanted 02 confirmed 02");
    CHECK_TEXT(&c, "error: none");
    check_no_key(&c, NEW_KEY);
    CHECK(bt_classic_quiescent(&c) == false);
    // The host saves it and clears the mark; nothing is rewritten.
    c.bond_dirty = false;
    run(&c, &M, 3000);
    CHECK_EQ(M.scan_writes, 2);
    CHECK_EQ(c.phase, BT_CLASSIC_READY);
}

static void test_connect_session(void)
{
    bt_classic_t c;
    bt_classic_bond_t none = {0};
    start(&c, &none);
    M.expect_auth = 0x00;
    unsigned from = M.logn;
    char text[64];
    snprintf(text, sizeof text, "  connect %s\n", PHONES_TEXT);
    CHECK(request(&c, text, NULL, NULL));
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_READY, 3000));
    run(&c, &M, 500);
    static const uint16_t ops[] = {0x0405, 0x0411, 0x040c, 0x042b, 0x042c, 0x0413, 0x1408};
    expect_ops("connect", ops, 7, from);
    CHECK(!c.bond.valid); CHECK(!c.bond_dirty); CHECK(!c.have_key); CHECK(all_zero(c.new_key, 16));
    CHECK_EQ(c.scan_enable_wanted, 0x00); CHECK_EQ(M.scan_writes, 1);
    const cmd_t *io = find_op(0x042b, from);
    CHECK(io && io->p[8] == 0x00);
    CHECK_TEXT(&c, "session: session only (nothing kept)");
    CHECK_TEXT(&c, "bond: none (to save: no)");
    check_no_key(&c, NEW_KEY);
}

// A debug link key (type 0x03) gives a session that works and a bond that
// is not kept, and the status file says why.
static void test_debug_key(void)
{
    for (int had = 0; had < 2; had++) {
        bt_classic_t c;
        bt_classic_bond_t none = {0};
        bt_classic_bond_t old = bond_for(STRANGER, OLD_KEY);
        start(&c, had ? &old : &none);
        M.key_type = 0x03;
        unsigned writes = M.scan_writes;
        char text[64];
        snprintf(text, sizeof text, "bond %s", PHONES_TEXT);
        CHECK(request(&c, text, NULL, NULL));
        CHECK(run_until_phase(&c, &M, BT_CLASSIC_READY, 3000));
        run(&c, &M, 200);
        CHECK(c.connected); CHECK(c.encrypted); CHECK(c.debug_key);
        CHECK(!c.bond_dirty); CHECK(!c.have_key); CHECK(all_zero(c.new_key, 16));
        CHECK_EQ(M.scan_writes, writes);
        if (had) { CHECK(c.bond.valid); CHECK(!memcmp(c.bond.peer, STRANGER, 6)); CHECK(!memcmp(c.bond.key, OLD_KEY, 16)); }
        else CHECK(!c.bond.valid);
        CHECK_TEXT(&c, "bond: not kept (debug link key); held: ");
        CHECK_TEXT(&c, had ? "held: 06:05:04:03:02:01 key type 04" : "held: none (to save: no)");
        check_no_key(&c, NEW_KEY);
        // The next attempt says nothing of it.
        CHECK(request(&c, "disconnect", NULL, NULL));
        run(&c, &M, 200);
        M.key_type = 0x04;
        CHECK(request(&c, text, NULL, NULL));
        CHECK(!c.debug_key);
        CHECK(run_until_phase(&c, &M, BT_CLASSIC_READY, 3000));
        CHECK(c.bond.valid); CHECK(c.bond_dirty); CHECK(!memcmp(c.bond.peer, PHONES, 6));
    }
}

// ── 2: reconnecting with the bond ───────────────────────────────────────────

static void test_reconnect(void)
{
    bt_classic_t c;
    bt_classic_bond_t b = bond_for(PHONES, OLD_KEY);
    start(&c, &b);
    CHECK_EQ(M.scan_writes, 1); CHECK_EQ(M.scan, 0x02);
    M.has_key = true; memcpy(M.key, OLD_KEY, 16);
    unsigned from = M.logn;
    CHECK(request(&c, "reconnect", NULL, NULL));
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_READY, 3000));
    run(&c, &M, 500);
    static const uint16_t ops[] = {0x0405, 0x0411, 0x040b, 0x0413, 0x1408};
    expect_ops("reconnect", ops, 5, from);
    const cmd_t *reply = find_op(0x040b, from);
    CHECK(reply && !memcmp(reply->p + 6, OLD_KEY, 16) && !memcmp(reply->p, PHONES, 6));
    CHECK(c.key_given); CHECK(!c.bond_dirty); CHECK(c.bond.valid);
    CHECK(!memcmp(c.bond.key, OLD_KEY, 16));
    CHECK(!strcmp(c.peer_name, "SoundLink II"));
    CHECK_EQ(M.scan_writes, 1);
    CHECK_TEXT(&c, "session: reconnect to the bond");
    CHECK_TEXT(&c, "peer: 4c:87:5d:c3:b2:a1 name SoundLink II");
    check_no_key(&c, OLD_KEY);
}

// A Link Key Notification in a session that keeps nothing changes nothing.
static void test_reconnect_ignores_new_key(void)
{
    bt_classic_t c;
    bt_classic_bond_t b = bond_for(PHONES, OLD_KEY);
    start(&c, &b);
    M.has_key = true; memcpy(M.key, OLD_KEY, 16);
    CHECK(request(&c, "reconnect", NULL, NULL));
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_ENCRYPTING, 3000));
    ev_key_notify(&M, 0, PHONES, NEW_KEY, 0x04);
    step(&c, &M, 10);
    CHECK(!c.have_key); CHECK(all_zero(c.new_key, 16));
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_READY, 3000));
    CHECK(!c.bond_dirty); CHECK(!memcmp(c.bond.key, OLD_KEY, 16)); CHECK(!c.have_key);
}

// ── 3: they call us ─────────────────────────────────────────────────────────

static void test_inbound(bool they_secure)
{
    bt_classic_t c;
    bt_classic_bond_t b = bond_for(PHONES, OLD_KEY);
    start(&c, &b);
    M.has_key = true; memcpy(M.key, OLD_KEY, 16); M.they_secure = they_secure;
    unsigned from = M.logn;
    ev_request(&M, 0, PHONES, 1);
    step(&c, &M, 10);
    CHECK_EQ(c.session, BT_CLASSIC_SESSION_INBOUND);
    CHECK(c.phase == BT_CLASSIC_ACCEPTING);
    const cmd_t *accept = find_op(0x0409, from);
    CHECK(accept && accept->p[6] == 0x01 && !memcmp(accept->p, PHONES, 6));
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_LINKED, 1000));
    uint64_t linked_at = M.now;
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_READY, 5000));
    run(&c, &M, 500);
    if (they_secure) {
        static const uint16_t ops[] = {0x0409, 0x040b, 0x1408};
        expect_ops("they call and secure", ops, 3, from);
        CHECK(c.peer_authenticating);
    } else {
        static const uint16_t ops[] = {0x0409, 0x0411, 0x040b, 0x0413, 0x1408};
        expect_ops("they call and wait", ops, 5, from);
        const cmd_t *auth = find_op(0x0411, from);
        // An event carries the time of the tick before it, so the wait
        // starts up to one step before the phase is seen.
        CHECK(auth && auth->at >= linked_at + 1990 && auth->at <= linked_at + 2000);
    }
    CHECK(c.encrypted); CHECK(!c.bond_dirty);
    CHECK(c.role_known); CHECK_EQ(c.role, 0x01);
    CHECK_TEXT(&c, "session: called by the bonded peer");
    CHECK_TEXT(&c, "role: peripheral");
}

// A peer that begins authenticating is waited for, not interrupted at 2 s.
static void test_inbound_slow_peer(void)
{
    bt_classic_t c;
    bt_classic_bond_t b = bond_for(PHONES, OLD_KEY);
    start(&c, &b);
    M.reply_only = true;
    ev_request(&M, 0, PHONES, 1);
    step(&c, &M, 10);
    ev_conn(&M, 10, TAG_NONE, 0, 0x0c, PHONES, 1);
    M.live = true; M.handle = 0x0c;
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_LINKED, 500));
    ev_addr(&M, 500, 0x17, PHONES);
    run(&c, &M, 3000);
    CHECK_EQ(c.phase, BT_CLASSIC_LINKED);
    CHECK(!find_op(0x0411, 0));
    CHECK(find_op(0x040b, 0));
    ev_enc(&M, 0, 0, 0x0c, 1);
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_READY, 500));
}

// ── 4: refusals ─────────────────────────────────────────────────────────────

static void test_refusals(void)
{
    bt_classic_t c;
    bt_classic_bond_t b = bond_for(PHONES, OLD_KEY);
    start(&c, &b);
    unsigned from = M.logn;
    uint32_t attempt = c.attempt;
    ev_request(&M, 0, STRANGER, 1);
    step(&c, &M, 10);
    CHECK_EQ(c.phase, BT_CLASSIC_IDLE); CHECK_EQ(c.attempt, attempt);
    run(&c, &M, 100);
    static const uint16_t ops[] = {0x040a};
    expect_ops("stranger", ops, 1, from);
    CHECK(!memcmp(M.log[from].p, STRANGER, 6)); CHECK_EQ(M.log[from].p[6], 0x0f);
    CHECK_EQ(c.phase, BT_CLASSIC_IDLE); CHECK(bt_classic_quiescent(&c));
    // Pairing asks from a stranger: the negative forms.
    from = M.logn;
    ev_addr(&M, 0, 0x17, STRANGER); ev_addr(&M, 0, 0x31, STRANGER); ev_confirm(&M, 0, STRANGER);
    ev_addr(&M, 0, 0x16, STRANGER);
    run(&c, &M, 100);
    static const uint16_t neg[] = {0x040c, 0x0434, 0x042d, 0x040e};
    expect_ops("stranger pairing", neg, 4, from);
    CHECK_EQ(c.phase, BT_CLASSIC_IDLE); CHECK(bt_classic_quiescent(&c));
    // A voice link from the bonded peer while READY.
    M.has_key = true; memcpy(M.key, OLD_KEY, 16);
    CHECK(request(&c, "reconnect", NULL, NULL));
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_READY, 3000));
    from = M.logn;
    ev_request(&M, 0, PHONES, 0);
    ev_request(&M, 0, PHONES, 2);
    run(&c, &M, 200);
    static const uint16_t sco[] = {0x042a, 0x042a};
    expect_ops("voice", sco, 2, from);
    CHECK(!memcmp(M.log[from].p, PHONES, 6));
    CHECK_EQ(c.phase, BT_CLASSIC_READY); CHECK(c.connected); CHECK(c.encrypted);
    // The bonded peer calling while the link is busy is refused too, as
    // limited resources: it is welcome, just not now.
    from = M.logn;
    ev_request(&M, 0, PHONES, 1);
    run(&c, &M, 100);
    static const uint16_t busy[] = {0x040a};
    expect_ops("busy", busy, 1, from);
    CHECK_EQ(M.log[from].p[6], 0x0d);
    CHECK_EQ(c.phase, BT_CLASSIC_READY);
    // A stranger calling while the link is busy is still unacceptable.
    from = M.logn;
    ev_request(&M, 0, STRANGER, 1);
    run(&c, &M, 100);
    expect_ops("busy stranger", busy, 1, from);
    CHECK_EQ(M.log[from].p[6], 0x0f);
    CHECK_EQ(c.phase, BT_CLASSIC_READY);
    // A call from the bonded address with no bond loaded is a stranger's.
    bt_classic_bond_t none = {0};
    start(&c, &none);
    from = M.logn;
    ev_request(&M, 0, PHONES, 1);
    run(&c, &M, 100);
    expect_ops("no bond", busy, 1, from);
    CHECK_EQ(M.log[from].p[6], 0x0f);
    CHECK_EQ(c.phase, BT_CLASSIC_IDLE);
    // The bonded peer calling while we page someone else: busy.
    bt_classic_bond_t b2 = bond_for(PHONES, OLD_KEY);
    start(&c, &b2);
    M.silent = true;
    char other[64];
    snprintf(other, sizeof other, "connect 06:05:04:03:02:01");
    CHECK(request(&c, other, NULL, NULL));
    step(&c, &M, 10);
    ev_cs(&M, 0, 0, 0x0405);
    step(&c, &M, 10);
    from = M.logn;
    ev_request(&M, 0, PHONES, 1);
    ev_cs(&M, 20, 0, 0x040a);   // the silent model answers nothing; the refusal still needs its reply
    run(&c, &M, 50);
    CHECK(find_op(0x040a, from) && find_op(0x040a, from)->p[6] == 0x0d);
    CHECK_EQ(c.phase, BT_CLASSIC_PAGING); CHECK(!c.crossing);
}

// ── Two calls that cross ────────────────────────────────────────────────────

// We page the headphones as they page us. Their call is accepted, the
// controller completes one of the two and fails the other, in either
// order, and the link is whichever succeeded.
static void test_crossing(void)
{
    bt_classic_bond_t b = bond_for(PHONES, OLD_KEY);
    for (int order = 0; order < 4; order++) {
        bt_classic_t c;
        start(&c, &b);
        M.silent = true;
        CHECK(request(&c, "reconnect", NULL, NULL));
        step(&c, &M, 10);
        CHECK(c.pending && c.opcode == 0x0405);
        ev_cs(&M, 0, 0, 0x0405);
        ev_request(&M, 0, PHONES, 1);
        step(&c, &M, 10);
        CHECK(c.crossing);
        CHECK_EQ(c.phase, BT_CLASSIC_PAGING);
        CHECK(c.pending && c.opcode == 0x0409);   // accepted, not refused
        CHECK_EQ(c.refusal_count, 0);
        ev_cs(&M, 0, 0, 0x0409);
        step(&c, &M, 10);
        CHECK(c.link_may_active);
        // order 0: fail then succeed; 1: succeed then fail; 2: both fail;
        // 3: the accept refused, then the page succeeds.
        if (order == 0) {
            ev_conn(&M, 0, TAG_NONE, 0x0b, 0, PHONES, 1);
            step(&c, &M, 10);
            CHECK_EQ(c.phase, BT_CLASSIC_PAGING); CHECK(!c.error); CHECK(!c.crossing); CHECK(c.link_may_active);
            ev_conn(&M, 0, TAG_NONE, 0, 0x0040, PHONES, 1);
            step(&c, &M, 10);
            CHECK_EQ(c.phase, BT_CLASSIC_AUTHENTICATING); CHECK(c.connected); CHECK_EQ(c.handle, 0x0040);
        } else if (order == 1) {
            ev_conn(&M, 0, TAG_NONE, 0, 0x0041, PHONES, 1);
            step(&c, &M, 10);
            CHECK_EQ(c.phase, BT_CLASSIC_AUTHENTICATING); CHECK(c.connected); CHECK(!c.crossing);
            ev_cs(&M, 0, 0, 0x0411);
            ev_conn(&M, 0, TAG_NONE, 0x0b, 0, PHONES, 1);
            step(&c, &M, 10);
            CHECK_EQ(c.phase, BT_CLASSIC_AUTHENTICATING); CHECK(c.connected); CHECK(!c.error);
        } else if (order == 2) {
            ev_conn(&M, 0, TAG_NONE, 0x0b, 0, PHONES, 1);
            step(&c, &M, 10);
            CHECK_EQ(c.phase, BT_CLASSIC_PAGING); CHECK(!c.error);
            ev_conn(&M, 0, TAG_NONE, 0x04, 0, PHONES, 1);
            step(&c, &M, 10);
            CHECK_EQ(c.phase, BT_CLASSIC_STOPPED);
            CHECK(c.error && !strcmp(c.error, "the connection was not made")); CHECK_EQ(c.error_status, 0x04);
            CHECK(!c.link_may_active); CHECK(!c.crossing);
        } else {
            // The accept was answered with success above; replay the case
            // where it is refused instead.
            start(&c, &b);
            M.silent = true;
            CHECK(request(&c, "reconnect", NULL, NULL));
            step(&c, &M, 10);
            ev_cs(&M, 0, 0, 0x0405); ev_request(&M, 0, PHONES, 1);
            step(&c, &M, 10);
            CHECK(c.pending && c.opcode == 0x0409);
            ev_cs(&M, 0, 0x0c, 0x0409);
            step(&c, &M, 10);
            CHECK_EQ(c.phase, BT_CLASSIC_PAGING); CHECK(!c.error); CHECK(!c.crossing); CHECK(c.link_may_active);
            ev_conn(&M, 0, TAG_NONE, 0, 0x0042, PHONES, 1);
            step(&c, &M, 10);
            CHECK_EQ(c.phase, BT_CLASSIC_AUTHENTICATING); CHECK(c.connected);
        }
    }
    // The page succeeds while the crossing call's accept is still latched:
    // the accept is not sent for a link already made.
    {
        bt_classic_t c;
        start(&c, &b);
        M.silent = true;
        CHECK(request(&c, "reconnect", NULL, NULL));
        step(&c, &M, 10);
        M.control = false;
        ev_cs(&M, 0, 0, 0x0405); ev_request(&M, 0, PHONES, 1);
        step(&c, &M, 10);
        CHECK(c.crossing); CHECK(bt_classic_urgent(&c));
        ev_conn(&M, 0, TAG_NONE, 0, 0x0043, PHONES, 1);
        step(&c, &M, 10);
        CHECK(!c.crossing); CHECK(!bt_classic_urgent(&c));
        unsigned from = M.logn;
        M.control = true;
        step(&c, &M, 10);
        CHECK(find_op(0x0411, from)); CHECK(!find_op(0x0409, from));
    }
    // The page succeeds while the accept is in flight, and the controller
    // then refuses the accept (the call it was for is the link now): the
    // link stands.
    {
        bt_classic_t c;
        start(&c, &b);
        M.silent = true;
        CHECK(request(&c, "reconnect", NULL, NULL));
        step(&c, &M, 10);
        ev_cs(&M, 0, 0, 0x0405); ev_request(&M, 0, PHONES, 1);
        step(&c, &M, 10);
        CHECK(c.pending && c.opcode == 0x0409);
        ev_conn(&M, 0, TAG_NONE, 0, 0x0045, PHONES, 1);
        step(&c, &M, 10);
        CHECK_EQ(c.phase, BT_CLASSIC_AUTHENTICATING); CHECK(c.pending && c.opcode == 0x0409);
        ev_cs(&M, 0, 0x02, 0x0409);
        step(&c, &M, 10);
        CHECK_EQ(c.phase, BT_CLASSIC_AUTHENTICATING); CHECK(c.connected); CHECK(!c.error);
        CHECK(c.pending && c.opcode == 0x0411);
    }
    // Closed while crossing with the cancel answered "Unknown Connection":
    // the page is over, the accepted call still owes its completion.
    {
        bt_classic_t c;
        start(&c, &b);
        M.silent = true;
        CHECK(request(&c, "reconnect", NULL, NULL));
        step(&c, &M, 10);
        ev_cs(&M, 0, 0, 0x0405); ev_request(&M, 0, PHONES, 1);
        step(&c, &M, 10);
        ev_cs(&M, 0, 0, 0x0409);
        step(&c, &M, 10);
        CHECK(request(&c, "disconnect", NULL, NULL));
        step(&c, &M, 10);
        CHECK(c.pending && c.opcode == 0x0408);
        ev_cc_addr(&M, 0, 0x0408, 0x02, PHONES);
        step(&c, &M, 10);
        CHECK_EQ(c.phase, BT_CLASSIC_CLOSING); CHECK(c.link_may_active); CHECK(!c.crossing);
        ev_conn(&M, 0, TAG_NONE, 0x10, 0, PHONES, 1);
        step(&c, &M, 10);
        CHECK_EQ(c.phase, BT_CLASSIC_STOPPED); CHECK(!c.error);
    }
    // Closed while the crossing call's accept is still latched: it becomes
    // a reject, and the page's failing completion then ends the attempt at
    // once (nothing else is owed).
    {
        bt_classic_t c;
        start(&c, &b);
        M.silent = true;
        CHECK(request(&c, "reconnect", NULL, NULL));
        step(&c, &M, 10);
        M.control = false;
        ev_cs(&M, 0, 0, 0x0405); ev_request(&M, 0, PHONES, 1);
        step(&c, &M, 10);
        CHECK(request(&c, "disconnect", NULL, NULL));
        CHECK(!c.crossing); CHECK_EQ(c.refusal_count, 1);
        ev_conn(&M, 0, TAG_NONE, 0x02, 0, PHONES, 1);
        step(&c, &M, 10);
        CHECK_EQ(c.phase, BT_CLASSIC_STOPPED); CHECK(!c.error);
    }
    // A second call while one already crosses is busy, and refused.
    {
        bt_classic_t c;
        start(&c, &b);
        M.silent = true;
        CHECK(request(&c, "reconnect", NULL, NULL));
        step(&c, &M, 10);
        ev_cs(&M, 0, 0, 0x0405); ev_request(&M, 0, PHONES, 1); ev_request(&M, 0, PHONES, 1);
        step(&c, &M, 10);
        CHECK(c.crossing); CHECK_EQ(c.refusal_count, 1); CHECK_EQ(c.refusal[0].reason, 0x0d);
    }
    // Closed while crossing: the cancel and the accepted call both end it.
    {
        bt_classic_t c;
        start(&c, &b);
        M.silent = true;
        CHECK(request(&c, "reconnect", NULL, NULL));
        step(&c, &M, 10);
        ev_cs(&M, 0, 0, 0x0405); ev_request(&M, 0, PHONES, 1);
        step(&c, &M, 10);
        ev_cs(&M, 0, 0, 0x0409);
        step(&c, &M, 10);
        CHECK(request(&c, "disconnect", NULL, NULL));
        CHECK_EQ(c.phase, BT_CLASSIC_CLOSING);
        step(&c, &M, 10);
        CHECK(c.pending && c.opcode == 0x0408);
        ev_cc_addr(&M, 0, 0x0408, 0, PHONES); ev_conn(&M, 0, TAG_NONE, 0x02, 0, PHONES, 1);
        step(&c, &M, 10);
        CHECK_EQ(c.phase, BT_CLASSIC_CLOSING);   // the accepted call still owes its completion
        ev_conn(&M, 0, TAG_NONE, 0, 0x0044, PHONES, 1);
        step(&c, &M, 10);
        CHECK(c.pending && c.opcode == 0x0406);
        ev_cs(&M, 0, 0, 0x0406); ev_disconn(&M, 0, 0, 0x0044, 0x16);
        run(&c, &M, 50);
        check_clean(&c, "closed while crossing");
    }
    // A stranger's call while paging is not a crossing call.
    {
        bt_classic_t c;
        start(&c, &b);
        M.silent = true;
        CHECK(request(&c, "reconnect", NULL, NULL));
        step(&c, &M, 10);
        ev_request(&M, 0, STRANGER, 1);
        step(&c, &M, 10);
        CHECK(!c.crossing); CHECK_EQ(c.refusal_count, 1); CHECK_EQ(c.refusal[0].reason, 0x0f);
    }
}

// ── 5, 6, 7: failures ───────────────────────────────────────────────────────

static void test_stale_bond(void)
{
    bt_classic_t c;
    bt_classic_bond_t b = bond_for(PHONES, OLD_KEY);
    start(&c, &b);
    M.has_key = false; M.expect_reason = 0x05;
    unsigned from = M.logn;
    CHECK(request(&c, "reconnect", NULL, NULL));
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_STOPPED, 3000));
    run(&c, &M, 100);
    static const uint16_t ops[] = {0x0405, 0x0411, 0x040b, 0x0406};
    expect_ops("stale", ops, 4, from);
    CHECK(c.error && !strcmp(c.error, "the headphones no longer hold this bond; forget and bond again"));
    CHECK_EQ(c.error_status, 0x06); CHECK(!c.retryable);
    CHECK(c.bond.valid); CHECK(!c.bond_dirty);
    CHECK_TEXT(&c, "error: the headphones no longer hold this bond; forget and bond again (opcode 0000 status 06)");
    check_clean(&c, "stale");
}

// Authentication failing for another reason, with no key given, is not "stale".
static void test_auth_failed(void)
{
    bt_classic_t c;
    bt_classic_bond_t none = {0};
    start(&c, &none);
    M.reply_only = true; M.expect_reason = 0x05;
    char text[64];
    snprintf(text, sizeof text, "bond %s", PHONES_TEXT);
    CHECK(request(&c, text, NULL, NULL));
    step(&c, &M, 10);
    M.live = true; M.handle = 0x21;
    ev_conn(&M, 0, TAG_NONE, 0, 0x21, PHONES, 1);
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_AUTHENTICATING, 200));
    run(&c, &M, 50);
    ev_auth(&M, 0, 0x06, 0x21);
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_STOPPED, 1000));
    CHECK(c.error && !strcmp(c.error, "authentication failed"));
    CHECK(!c.key_given);
}

static void test_weak_key(void)
{
    bt_classic_t c;
    bt_classic_bond_t none = {0};
    start(&c, &none);
    M.key_size = 7; M.expect_reason = 0x05;
    unsigned from = M.logn;
    char text[64];
    snprintf(text, sizeof text, "bond %s", PHONES_TEXT);
    CHECK(request(&c, text, NULL, NULL));
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_STOPPED, 3000));
    run(&c, &M, 100);
    static const uint16_t ops[] = {0x0405, 0x0411, 0x040c, 0x042b, 0x042c, 0x0413, 0x1408, 0x0406};
    expect_ops("weak", ops, 8, from);
    CHECK(c.error && !strcmp(c.error, "weak encryption refused"));
    CHECK_EQ(c.key_size, 7); CHECK(!c.retryable);
    CHECK(!c.bond.valid); CHECK(!c.bond_dirty);
    CHECK_TEXT(&c, "error: weak encryption refused");
    check_clean(&c, "weak");
    check_no_key(&c, NEW_KEY);
    // Fifteen is weak too.
    start(&c, &none);
    M.key_size = 15;
    CHECK(request(&c, text, NULL, NULL));
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_STOPPED, 3000));
    CHECK(c.error && !strcmp(c.error, "weak encryption refused"));
}

static void test_page_timeout(void)
{
    bt_classic_t c;
    bt_classic_bond_t b = bond_for(PHONES, OLD_KEY);
    start(&c, &b);
    M.present = false;
    unsigned from = M.logn;
    CHECK(request(&c, "reconnect", NULL, NULL));
    run(&c, &M, 5000);
    CHECK_EQ(c.phase, BT_CLASSIC_PAGING);
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_STOPPED, 1000));
    run(&c, &M, 100);
    static const uint16_t ops[] = {0x0405};
    expect_ops("page timeout", ops, 1, from);
    CHECK(c.error && !strcmp(c.error, "the connection was not made"));
    CHECK_EQ(c.error_status, 0x04);
    CHECK(c.retryable);     // switched off or out of reach: they may come back
    check_clean(&c, "page timeout");
    // A page the controller ends for a reason of its own (no room for
    // another link) is not the far end gone.
    start(&c, &b);
    M.present = false; M.page_status = 0x0d;
    CHECK(request(&c, "reconnect", NULL, NULL));
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_STOPPED, 6000));
    CHECK_EQ(c.error_status, 0x0d);
    CHECK(!c.retryable);
    check_clean(&c, "page refused");
    // A controller that never reports the page's end: cancelled at 6 s.
    start(&c, &b);
    M.present = false;
    from = M.logn;
    CHECK(request(&c, "reconnect", NULL, NULL));
    step(&c, &M, 10);
    unqueue(&M, TAG_PAGE);
    run(&c, &M, 5980);
    CHECK_EQ(c.phase, BT_CLASSIC_PAGING);
    run(&c, &M, 40);
    CHECK(c.error && !strcmp(c.error, "the page was not answered"));
    run(&c, &M, 200);
    static const uint16_t cancel[] = {0x0405, 0x0408};
    expect_ops("page silence", cancel, 2, from);
    check_clean(&c, "page silence");
}

// No command credit: nothing is sent, and a command already answered is
// still finished (its reply may be what said "none yet").
static void test_no_credit(void)
{
    bt_classic_t c;
    bt_classic_bond_t b = bond_for(PHONES, OLD_KEY);
    start(&c, &b);
    M.has_key = true; memcpy(M.key, OLD_KEY, 16);
    unsigned from = M.logn;
    c.no_credit = true;
    CHECK(request(&c, "reconnect", NULL, NULL));
    run(&c, &M, 500);
    CHECK_EQ(M.logn, from); CHECK_EQ(c.phase, BT_CLASSIC_PAGING);
    c.no_credit = false;
    // The last command of the way up is answered while credit is withheld.
    for (unsigned i = 0; i < 400 && !(M.logn > from && M.log[M.logn - 1].op == 0x1408); i++) step(&c, &M, 10);
    CHECK(M.logn > from && M.log[M.logn - 1].op == 0x1408);
    c.no_credit = true;
    unsigned sent = M.logn;
    run(&c, &M, 4000);
    CHECK_EQ(c.phase, BT_CLASSIC_READY); CHECK(!c.error); CHECK(!c.timed_out); CHECK(!c.pending);
    CHECK_EQ(M.logn, sent);
    c.no_credit = false;
}

// Disconnection reasons: the far end that may come back, and the rest.
static void test_retryable(void)
{
    static const struct { uint8_t reason; bool retry; } cases[] = {
        {0x08, true}, {0x13, true}, {0x14, true}, {0x15, true}, {0x22, true},
        {0x05, false}, {0x06, false}, {0x0e, false}, {0x16, false}, {0x3d, false},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        bt_classic_t c;
        bt_classic_bond_t b = bond_for(PHONES, OLD_KEY);
        start(&c, &b);
        M.has_key = true; memcpy(M.key, OLD_KEY, 16);
        CHECK(request(&c, "reconnect", NULL, NULL));
        CHECK(run_until_phase(&c, &M, BT_CLASSIC_READY, 3000));
        ev_disconn(&M, 0, 0, M.handle, cases[i].reason); M.live = false;
        step(&c, &M, 10);
        CHECK_EQ(c.phase, BT_CLASSIC_STOPPED);
        CHECK_EQ(c.disconnect_reason, cases[i].reason);
        CHECK_EQ(c.retryable, cases[i].retry);
        CHECK(!c.connected); CHECK(!c.encrypted);
        CHECK(c.error && !strcmp(c.error, "the peer disconnected"));
        check_clean(&c, "disconnected");
    }
}

// ── 8: vanishing, silence, forget and disconnect at every phase ─────────────

enum { HOW_VANISH, HOW_SILENCE, HOW_FORGET, HOW_DISCONNECT, HOWS };
static const char *const HOW_NAMES[] = {"vanish", "silence", "forget", "disconnect"};

static void reach(bt_classic_t *c, bt_classic_phase_t phase)
{
    bt_classic_bond_t b = bond_for(PHONES, OLD_KEY);
    bt_classic_bond_t none = {0};
    bool inbound = phase == BT_CLASSIC_ACCEPTING || phase == BT_CLASSIC_LINKED;
    start(c, inbound || phase == BT_CLASSIC_CLOSING ? &b : &none);
    if (inbound) { M.has_key = true; memcpy(M.key, OLD_KEY, 16); ev_request(&M, 0, PHONES, 1); }
    else if (phase == BT_CLASSIC_CLOSING) {
        M.has_key = true; memcpy(M.key, OLD_KEY, 16);
        CHECK(request(c, "reconnect", NULL, NULL));
        CHECK(run_until_phase(c, &M, BT_CLASSIC_READY, 3000));
        CHECK(request(c, "disconnect", NULL, NULL));
        return;
    } else {
        char text[64];
        snprintf(text, sizeof text, "bond %s", PHONES_TEXT);
        CHECK(request(c, text, NULL, NULL));
    }
    if (!run_until_phase(c, &M, phase, 5000)) { failures++; printf("FAIL never reached phase %d\n", phase); }
}

static void test_every_phase(void)
{
    static const bt_classic_phase_t phases[] = {
        BT_CLASSIC_PAGING, BT_CLASSIC_ACCEPTING, BT_CLASSIC_LINKED, BT_CLASSIC_AUTHENTICATING,
        BT_CLASSIC_ENCRYPTING, BT_CLASSIC_CHECKING, BT_CLASSIC_READY, BT_CLASSIC_CLOSING,
    };
    for (unsigned i = 0; i < sizeof phases / sizeof phases[0]; i++)
        for (int how = 0; how < HOWS; how++) {
            bt_classic_t c;
            // READY has nothing to wait for, so silence there ends nothing.
            if (how == HOW_SILENCE && phases[i] == BT_CLASSIC_READY) continue;
            reach(&c, phases[i]);
            char what[64];
            snprintf(what, sizeof what, "%s at phase %d", HOW_NAMES[how], phases[i]);
            switch (how) {
            case HOW_VANISH:
                if (M.live) { M.qn = 0; ev_disconn(&M, 0, 0, M.handle, 0x08); M.live = false; }
                else { M.qn = 0; M.silent = true; }
                break;
            case HOW_SILENCE:
                M.qn = 0; M.silent = true;
                run(&c, &M, 60000);
                M.silent = false;
                break;
            case HOW_FORGET: {
                const char *why = NULL;
                CHECK(request(&c, "forget", NULL, &why));
                CHECK(!c.bond.valid); CHECK(c.bond_dirty); CHECK_EQ(c.scan_enable_wanted, 0x00);
                break;
            }
            case HOW_DISCONNECT: {
                const char *why = NULL;
                if (!request(&c, "disconnect", NULL, &why)) { failures++; printf("FAIL %s refused: %s\n", what, why); }
                break;
            }
            }
            run(&c, &M, 60000);
            M.silent = false;
            run(&c, &M, 10000);
            check_clean(&c, what);
            if (how == HOW_FORGET || how == HOW_DISCONNECT)
                CHECK(!c.retryable);
            if (how == HOW_FORGET) CHECK(!c.bond.valid);
        }
}

// A cancel that loses to the page: the link that came up is disconnected.
static void test_cancel_race(void)
{
    bt_classic_t c;
    bt_classic_bond_t b = bond_for(PHONES, OLD_KEY);
    start(&c, &b);
    M.reply_only = true;
    unsigned from = M.logn;
    CHECK(request(&c, "reconnect", NULL, NULL));
    run(&c, &M, 30);
    CHECK(c.link_may_active);
    CHECK(request(&c, "disconnect", NULL, NULL));
    CHECK_EQ(c.phase, BT_CLASSIC_CLOSING);
    step(&c, &M, 10);
    // The page completed before the cancel reached the radio.
    M.live = true; M.handle = 0x33;
    ev_conn(&M, 0, TAG_NONE, 0, 0x33, PHONES, 1);
    run(&c, &M, 200);
    static const uint16_t ops[] = {0x0405, 0x0408, 0x0406};
    expect_ops("cancel race", ops, 3, from);
    CHECK(c.error == NULL);
    check_clean(&c, "cancel race");
}

// A call accepted and then dropped by the user: its Connection Complete is
// waited for and the link it brings is disconnected.
static void test_accept_then_disconnect(void)
{
    bt_classic_t c;
    bt_classic_bond_t b = bond_for(PHONES, OLD_KEY);
    start(&c, &b);
    M.reply_only = true;
    unsigned from = M.logn;
    ev_request(&M, 0, PHONES, 1);
    run(&c, &M, 20);
    CHECK(c.link_may_active);
    CHECK(request(&c, "disconnect", NULL, NULL));
    CHECK_EQ(c.phase, BT_CLASSIC_CLOSING);
    run(&c, &M, 500);
    CHECK_EQ(c.phase, BT_CLASSIC_CLOSING);
    M.live = true; M.handle = 0x34;
    ev_conn(&M, 0, TAG_NONE, 0, 0x34, PHONES, 1);
    run(&c, &M, 200);
    static const uint16_t ops[] = {0x0409, 0x0406};
    expect_ops("accept then disconnect", ops, 2, from);
    check_clean(&c, "accept then disconnect");
    // An accept not yet sent becomes a reject.
    start(&c, &b);
    from = M.logn;
    M.control = false;
    ev_request(&M, 0, PHONES, 1);
    run(&c, &M, 50);
    CHECK(request(&c, "forget", NULL, NULL));
    CHECK_EQ(c.phase, BT_CLASSIC_STOPPED);
    M.control = true;
    run(&c, &M, 200);
    static const uint16_t rej[] = {0x040a, 0x0c1a};
    expect_ops("unsent accept", rej, 2, from);
    check_clean(&c, "unsent accept");
}

// ── 10: page scan ───────────────────────────────────────────────────────────

static void test_page_scan(void)
{
    bt_classic_t c;
    model_reset();
    bt_classic_init(&c, LOCAL);
    run(&c, &M, 1000);
    CHECK_EQ(M.scan_writes, 0);          // nothing until the disk has been read
    CHECK(bt_classic_quiescent(&c));
    bt_classic_bond_t b = bond_for(PHONES, OLD_KEY);
    bt_classic_set_bond(&c, &b);
    CHECK(!bt_classic_quiescent(&c));
    run(&c, &M, 1000);
    CHECK_EQ(M.scan_writes, 1); CHECK_EQ(M.scan, 0x02);
    CHECK(bt_classic_quiescent(&c));
    // forget turns it off, once.
    CHECK(request(&c, "forget", NULL, NULL));
    run(&c, &M, 1000);
    CHECK_EQ(M.scan_writes, 2); CHECK_EQ(M.scan, 0x00);
    // A reload rewrites it (the controller may have been reset) once.
    bt_classic_bond_t none = {0};
    bt_classic_set_bond(&c, &none);
    run(&c, &M, 1000);
    CHECK_EQ(M.scan_writes, 3); CHECK_EQ(M.scan, 0x00);
    run(&c, &M, 5000);
    CHECK_EQ(M.scan_writes, 3);
    // A refused write is not repeated until there is something new to write.
    M.scan_reply = 0x0c;
    bt_classic_set_bond(&c, &b);
    run(&c, &M, 3000);
    CHECK_EQ(M.scan_writes, 4); CHECK_EQ(c.scan_enable_set, 0xff); CHECK_EQ(c.scan_enable_status, 0x0c);
    CHECK(bt_classic_quiescent(&c));
    CHECK_TEXT(&c, "page scan: wanted 02 confirmed ff last write status 0c");
    M.scan_reply = 0;
    CHECK(request(&c, "forget", NULL, NULL));
    run(&c, &M, 1000);
    CHECK_EQ(M.scan_writes, 5); CHECK_EQ(c.scan_enable_set, 0x00);
    // An unanswered write is not repeated either.
    M.silent = true;
    bt_classic_set_bond(&c, &b);
    run(&c, &M, 5000);
    CHECK_EQ(c.scan_enable_status, 0xff); CHECK(!c.pending); CHECK(!c.error);
    unsigned writes = 0;
    for (unsigned i = 0; i < M.logn; i++) writes += M.log[i].op == 0x0c1a;
    CHECK_EQ(writes, 6);
}

// ── 11: latched answers ─────────────────────────────────────────────────────

static void test_latched(void)
{
    bt_classic_t c;
    bt_classic_bond_t none = {0};
    start(&c, &none);
    M.reply_only = true;
    char text[64];
    snprintf(text, sizeof text, "bond %s", PHONES_TEXT);
    CHECK(request(&c, text, NULL, NULL));
    step(&c, &M, 10);
    M.live = true; M.handle = 0x40;
    ev_conn(&M, 0, TAG_NONE, 0, 0x40, PHONES, 1);
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_AUTHENTICATING, 200));
    run(&c, &M, 50);
    unsigned from = M.logn;
    M.control = false;
    ev_addr(&M, 0, 0x17, STRANGER);
    ev_addr(&M, 0, 0x16, PHONES);
    ev_confirm(&M, 0, PHONES);
    ev_addr(&M, 0, 0x31, PHONES);
    ev_addr(&M, 0, 0x17, PHONES);
    run(&c, &M, 10000);
    CHECK_EQ(M.logn, from);
    CHECK(bt_classic_urgent(&c));
    CHECK_TEXT(&c, "answers waiting: 4 refusals waiting: 1");
    M.control = true;
    run(&c, &M, 200);
    static const uint16_t ops[] = {0x040c, 0x042b, 0x042c, 0x040e, 0x040c};
    expect_ops("latched order", ops, 5, from);
    CHECK(!memcmp(M.log[from].p, PHONES, 6)); CHECK(!memcmp(M.log[from + 4].p, STRANGER, 6));
    CHECK(!bt_classic_urgent(&c));
    CHECK_EQ(c.phase, BT_CLASSIC_AUTHENTICATING);
    // One that waits 25 s closes the link.
    from = M.logn;
    M.control = false; M.expect_reason = 0x13;
    ev_addr(&M, 0, 0x17, PHONES);
    run(&c, &M, 24990);
    CHECK_EQ(c.phase, BT_CLASSIC_AUTHENTICATING);
    run(&c, &M, 20);
    CHECK_EQ(c.phase, BT_CLASSIC_CLOSING);
    CHECK(c.error && !strcmp(c.error, "an answer to the peer waited too long for the command slot"));
    CHECK(!bt_classic_urgent(&c));
    M.control = true;
    M.reply_only = false;
    run(&c, &M, 200);
    static const uint16_t closing[] = {0x0406};
    expect_ops("too late", closing, 1, from);
    check_clean(&c, "too late");
}

// A refusal that waits 25 s is dropped; a full queue drops the newest.
static void test_refusal_queue(void)
{
    bt_classic_t c;
    bt_classic_bond_t none = {0};
    start(&c, &none);
    M.control = false;
    uint8_t a[6];
    memcpy(a, STRANGER, 6);
    for (int i = 0; i < 6; i++) { a[0] = (uint8_t)(0x10 + i); ev_addr(&M, 0, 0x17, a); }
    run(&c, &M, 20);
    CHECK_EQ(c.refusal_count, BT_CLASSIC_REFUSALS); CHECK_EQ(c.refusals_dropped, 2);
    CHECK(bt_classic_urgent(&c)); CHECK(!bt_classic_quiescent(&c));
    unsigned from = M.logn;
    M.control = true;
    run(&c, &M, 200);
    CHECK_EQ(M.logn - from, BT_CLASSIC_REFUSALS);
    for (unsigned i = 0; i < BT_CLASSIC_REFUSALS && from + i < M.logn; i++) CHECK_EQ(M.log[from + i].p[0], 0x10 + i);
    M.control = false;
    ev_addr(&M, 0, 0x17, STRANGER);
    run(&c, &M, 24990);
    CHECK_EQ(c.refusal_count, 1);
    run(&c, &M, 20);
    CHECK_EQ(c.refusal_count, 0);
    CHECK(bt_classic_quiescent(&c));
}

// ── 12: requests ────────────────────────────────────────────────────────────

static void test_requests(void)
{
    bt_classic_t c;
    bt_classic_bond_t none = {0};
    start(&c, &none);
    static const char *const bad[] = {
        "", "bond", "bond ", "bond 4c:87:5d:c3:b2:a", "bond 4c:87:5d:c3:b2:a1x", "bond 4c:87:5d:c3:b2:g1",
        "bond 4c-87-5d-c3-b2-a1", "bond 4c:87:5d:c3:b2:a1:", "bond  4c:87:5d:c3:b2:a1", "bond 00:00:00:00:00:00",
        "Bond 4c:87:5d:c3:b2:a1", "bondx4c:87:5d:c3:b2:a1", "connect", "connect 4c:87:5d:c3:b2", "connect 4c:87:5d:c3:b2:a1 x",
        "connectx4c:87:5d:c3:b2:a1", "reconnect now", "reconnectx", "forgetx", "forge", "disconnectx", "disconnec",
        "bond 4c:87:5d:c3:b2:a1 4c", "bond 4c:87:5d:c3:b2a1:", "bond 4c:87:5d:c3:b2:-1",
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        const char *why = NULL;
        bt_classic_t before;
        memcpy(&before, &c, sizeof c);
        if (request(&c, bad[i], NULL, &why)) { failures++; printf("FAIL \"%s\" was taken\n", bad[i]); }
        else CHECK(why != NULL);
        if (memcmp(&before, &c, sizeof c)) {
            failures++; printf("FAIL \"%s\" changed the link\n", bad[i]);
        }
    }
    const char *why = NULL;
    CHECK(!request(&c, "reconnect", NULL, &why));
    CHECK(why && !strcmp(why, "no bond to reconnect; bond first"));
    CHECK(!request(&c, "disconnect", NULL, &why));
    CHECK(why && !strcmp(why, "not connected"));
    // An LE hint for the address is refused; a hint for another is ignored.
    bt_scan_device_t le = classic_hint(PHONES, "Keyboard");
    le.le = true;
    char text[64];
    snprintf(text, sizeof text, "bond %s", PHONES_TEXT);
    CHECK(!request(&c, text, &le, &why));
    CHECK(why && !strcmp(why, "the scan heard that address as LE; give it to slot 0 or 1"));
    CHECK_EQ(c.phase, BT_CLASSIC_IDLE);
    bt_scan_device_t other = classic_hint(STRANGER, "Elsewhere");
    other.le = true;
    CHECK(request(&c, text, &other, &why));
    CHECK_EQ(c.peer_name[0], 0);
    CHECK(!memcmp(c.peer, PHONES, 6));
    // Busy.
    CHECK(!request(&c, text, NULL, &why));
    CHECK(why && !strcmp(why, "busy: the Classic link is in use"));
    CHECK(!request(&c, "reconnect", NULL, &why));
    // Upper case hex and a name with junk in it.
    start(&c, &none);
    bt_scan_device_t junk = classic_hint(PHONES, "Bose\x01Phones");
    CHECK(request(&c, "connect 4C:87:5D:C3:B2:A1", &junk, &why));
    CHECK(!memcmp(c.peer, PHONES, 6));
    CHECK(!strcmp(c.peer_name, "Bose?Phones"));
    start(&c, &none);
    bt_scan_device_t edges = classic_hint(PHONES, " \x1f~\x7f");
    CHECK(request(&c, "connect 4c:87:5d:c3:b2:a1", &edges, &why));
    CHECK(!strcmp(c.peer_name, " ?~?"));
    // FAILED refuses all but forget.
    start(&c, &none);
    g_control = true;
    bt_classic_tick(&c, M.now, true, true, model_send, &M);
    CHECK_EQ(c.phase, BT_CLASSIC_FAILED);
    CHECK(c.error && !strcmp(c.error, "USB transport failed; reboot required"));
    CHECK(!request(&c, text, NULL, &why));
    CHECK(why && !strcmp(why, "the Bluetooth controller failed; reboot required"));
    CHECK(!request(&c, "disconnect", NULL, &why));
    CHECK(request(&c, "forget", NULL, &why));
    CHECK(c.bond_dirty);
    unsigned from = M.logn;
    ev_request(&M, 0, STRANGER, 1);
    run(&c, &M, 1000);
    CHECK_EQ(M.logn, from);
    CHECK_EQ(c.phase, BT_CLASSIC_FAILED);
    CHECK(!bt_classic_urgent(&c));
    CHECK_TEXT(&c, "state: failed (reboot required)");
}

// A transport failure mid-link ends everything: FAILED, nothing sent, the
// key a pairing produced wiped.
static void test_failed_mid_link(void)
{
    bt_classic_t c;
    bt_classic_bond_t none = {0};
    start(&c, &none);
    char text[64];
    snprintf(text, sizeof text, "bond %s", PHONES_TEXT);
    CHECK(request(&c, text, NULL, NULL));
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_ENCRYPTING, 3000));
    CHECK(c.have_key);
    unsigned from = M.logn;
    g_control = true;
    bt_classic_tick(&c, M.now + 10, true, true, model_send, &M);
    CHECK_EQ(c.phase, BT_CLASSIC_FAILED);
    CHECK(!c.have_key); CHECK(all_zero(c.new_key, 16)); CHECK(!c.pending);
    run(&c, &M, 5000);
    CHECK_EQ(M.logn, from);
}

// A rejected link command ends the attempt; a rejected refusal does not.
static void test_rejections(void)
{
    bt_classic_t c;
    bt_classic_bond_t b = bond_for(PHONES, OLD_KEY);
    start(&c, &b);
    M.silent = true;
    CHECK(request(&c, "reconnect", NULL, NULL));
    step(&c, &M, 10);
    CHECK(c.pending && c.opcode == 0x0405);
    ev_cs(&M, 0, 0x0c, 0x0405);
    step(&c, &M, 10);
    CHECK_EQ(c.phase, BT_CLASSIC_STOPPED);
    CHECK(c.error && !strcmp(c.error, "HCI command rejected"));
    CHECK_EQ(c.error_opcode, 0x0405); CHECK_EQ(c.error_status, 0x0c);
    CHECK(!c.link_may_active);
    check_clean(&c, "page refused");
    // A Command Status error refuses a command that answers with Command Complete.
    start(&c, &b);
    M.silent = true;
    ev_addr(&M, 0, 0x17, STRANGER);
    step(&c, &M, 10);
    CHECK(c.pending && c.opcode == 0x040c);
    ev_cs(&M, 0, 0x12, 0x040c);
    step(&c, &M, 10);
    CHECK(!c.pending); CHECK(!c.error); CHECK_EQ(c.phase, BT_CLASSIC_IDLE);
    // A command never answered: timed out, with which half was missing.
    start(&c, &b);
    M.silent = true;
    CHECK(request(&c, "reconnect", NULL, NULL));
    run(&c, &M, 1990);
    CHECK(c.pending);
    run(&c, &M, 20);
    CHECK(c.error && !strcmp(c.error, "HCI command timed out"));
    CHECK(c.timed_out); CHECK(c.timeout_usb_done); CHECK(!c.timeout_reply);
    CHECK_TEXT(&c, "error: HCI command timed out (opcode 0405 status 00); the USB transfer completed, no reply came");
    // The reply came but the transfer never finished.
    start(&c, &b);
    M.silent = true;
    CHECK(request(&c, "reconnect", NULL, NULL));
    step(&c, &M, 10);
    ev_cs(&M, 0, 0, 0x0405);
    M.control = false;
    run(&c, &M, 2000);
    CHECK(c.timed_out); CHECK(!c.timeout_usb_done); CHECK(c.timeout_reply);
    CHECK_TEXT(&c, "; the USB transfer had not completed");
    // A late reply to a command from an attempt that has ended is ignored.
    start(&c, &b);
    M.silent = true;
    CHECK(request(&c, "reconnect", NULL, NULL));
    step(&c, &M, 10);
    CHECK(request(&c, "disconnect", NULL, NULL));     // PAGING with the page out: cancel
    CHECK_EQ(c.phase, BT_CLASSIC_CLOSING);
    ev_cs(&M, 0, 0x0c, 0x0405);
    step(&c, &M, 10);
    CHECK_EQ(c.phase, BT_CLASSIC_STOPPED);
    CHECK(!c.error);
}

// Encryption turned off on a ready link closes it; Simple Pairing failing ends the attempt.
static void test_encryption_lost(void)
{
    bt_classic_t c;
    bt_classic_bond_t b = bond_for(PHONES, OLD_KEY);
    start(&c, &b);
    M.has_key = true; memcpy(M.key, OLD_KEY, 16);
    CHECK(request(&c, "reconnect", NULL, NULL));
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_READY, 3000));
    M.expect_reason = 0x05;
    ev_enc(&M, 0, 0, M.handle, 0);
    step(&c, &M, 10);
    CHECK_EQ(c.phase, BT_CLASSIC_CLOSING); CHECK(!c.encrypted);
    CHECK(c.error && !strcmp(c.error, "encryption failed or was turned off"));
    run(&c, &M, 200);
    check_clean(&c, "encryption off");
    // Re-encryption with AES-CCM on a ready link keeps it.
    start(&c, &b);
    M.has_key = true; memcpy(M.key, OLD_KEY, 16);
    CHECK(request(&c, "reconnect", NULL, NULL));
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_READY, 3000));
    ev_enc(&M, 0, 0, M.handle, 2);
    PUSH(&M, 0, TAG_NONE, 0x30, 3, 0, (uint8_t)M.handle, (uint8_t)(M.handle >> 8));
    PUSH(&M, 0, TAG_NONE, 0x14, 6, 0, (uint8_t)M.handle, (uint8_t)(M.handle >> 8), 2, 0x20, 0x03);
    run(&c, &M, 100);
    CHECK_EQ(c.phase, BT_CLASSIC_READY); CHECK(c.encrypted);
    CHECK(c.mode_known); CHECK_EQ(c.mode, 2); CHECK_EQ(c.mode_interval, 0x0320);
    CHECK_TEXT(&c, "mode: sniff interval 800");
    // Simple Pairing Complete with a failure.
    bt_classic_bond_t none = {0};
    start(&c, &none);
    M.reply_only = true; M.expect_reason = 0x05;
    char text[64];
    snprintf(text, sizeof text, "bond %s", PHONES_TEXT);
    CHECK(request(&c, text, NULL, NULL));
    step(&c, &M, 10);
    M.live = true; M.handle = 0x41;
    ev_conn(&M, 0, TAG_NONE, 0, 0x41, PHONES, 1);
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_AUTHENTICATING, 200));
    ev_ssp(&M, 0, 0x05, PHONES);
    step(&c, &M, 10);
    CHECK_EQ(c.phase, BT_CLASSIC_CLOSING);
    CHECK(c.error && !strcmp(c.error, "simple pairing failed"));
    // SSP Complete with status 0 for a stranger does not count.
    start(&c, &none);
    ev_ssp(&M, 0, 0, STRANGER);
    step(&c, &M, 10);
    CHECK(!c.ssp_done);
}

// The waits of the phases: what each one gives up after.
static void test_phase_waits(void)
{
    bt_classic_t c;
    bt_classic_bond_t none = {0};
    char text[64];
    snprintf(text, sizeof text, "bond %s", PHONES_TEXT);
    // AUTHENTICATING: 40 s for Authentication Complete.
    start(&c, &none);
    M.reply_only = true;
    CHECK(request(&c, text, NULL, NULL));
    step(&c, &M, 10);
    M.live = true; M.handle = 0x50;
    ev_conn(&M, 0, TAG_NONE, 0, 0x50, PHONES, 1);
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_AUTHENTICATING, 200));
    run(&c, &M, 39900);
    CHECK_EQ(c.phase, BT_CLASSIC_AUTHENTICATING);
    run(&c, &M, 200);
    CHECK(c.error && !strcmp(c.error, "the peer did not answer in time"));
    run(&c, &M, 200);
    check_clean(&c, "authentication silence");
    // ENCRYPTING: 10 s for Encryption Change.
    start(&c, &none);
    M.reply_only = true;
    CHECK(request(&c, text, NULL, NULL));
    step(&c, &M, 10);
    M.live = true; M.handle = 0x51;
    ev_conn(&M, 0, TAG_NONE, 0, 0x51, PHONES, 1);
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_AUTHENTICATING, 200));
    run(&c, &M, 50);
    ev_auth(&M, 0, 0, 0x51);
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_ENCRYPTING, 200));
    run(&c, &M, 9900);
    CHECK_EQ(c.phase, BT_CLASSIC_ENCRYPTING);
    run(&c, &M, 200);
    CHECK(c.error && !strcmp(c.error, "the peer did not answer in time"));
    // ACCEPTING: 6 s for Connection Complete after the accept, then 2 s more.
    bt_classic_bond_t b = bond_for(PHONES, OLD_KEY);
    start(&c, &b);
    M.reply_only = true;
    ev_request(&M, 0, PHONES, 1);
    run(&c, &M, 5900);
    CHECK_EQ(c.phase, BT_CLASSIC_ACCEPTING);
    run(&c, &M, 200);
    CHECK_EQ(c.phase, BT_CLASSIC_CLOSING);
    run(&c, &M, 1900);
    CHECK_EQ(c.phase, BT_CLASSIC_CLOSING);
    run(&c, &M, 200);
    CHECK_EQ(c.phase, BT_CLASSIC_STOPPED);
    CHECK(c.error && !strcmp(c.error, "the peer did not answer in time"));
    // CLOSING: a Disconnect nobody confirms ends STOPPED after 2 s anyway.
    start(&c, &b);
    M.has_key = true; memcpy(M.key, OLD_KEY, 16);
    CHECK(request(&c, "reconnect", NULL, NULL));
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_READY, 3000));
    M.reply_only = true;
    CHECK(request(&c, "disconnect", NULL, NULL));
    step(&c, &M, 10);
    M.qn = 0; M.silent = true;
    ev_cs(&M, 0, 0, 0x0406);
    M.silent = false;
    step(&c, &M, 10);
    M.silent = true;
    run(&c, &M, 1900);
    CHECK_EQ(c.phase, BT_CLASSIC_CLOSING);
    run(&c, &M, 200);
    CHECK_EQ(c.phase, BT_CLASSIC_STOPPED);
    CHECK(c.error && !strcmp(c.error, "the controller did not confirm the link was closed"));
    CHECK(!c.connected);
}

// ── 13: the bond codec ──────────────────────────────────────────────────────

static uint32_t crc32_ieee(const uint8_t *p, size_t n)
{
    uint32_t table[256];
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t v = i;
        for (int k = 0; k < 8; k++) v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
        table[i] = v;
    }
    uint32_t crc = 0xffffffffu;
    while (n--) crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}
static void reseal(uint8_t *r)
{
    uint32_t crc = crc32_ieee(r, 76);
    for (int i = 0; i < 4; i++) r[76 + i] = (uint8_t)(crc >> (8 * i));
}

static void test_codec(void)
{
    CHECK_EQ(crc32_ieee((const uint8_t *)"123456789", 9), 0xcbf43926u);
    bt_classic_bond_t b = bond_for(PHONES, OLD_KEY), back;
    uint8_t r[BT_CLASSIC_BOND_RECORD_BYTES];
    bt_classic_bond_encode(r, &b);
    CHECK(!memcmp(r, "OS64BC01", 8));
    CHECK_EQ(r[8], 1); CHECK_EQ(r[9], 0x04); CHECK_EQ(r[10], 0); CHECK_EQ(r[11], 0);
    CHECK(!memcmp(r + 12, PHONES, 6)); CHECK(!memcmp(r + 18, LOCAL, 6)); CHECK(!memcmp(r + 24, OLD_KEY, 16));
    CHECK(!strcmp((const char *)r + 40, "SoundLink II"));
    CHECK(all_zero(r + 40 + 12, 32 - 12)); CHECK(all_zero(r + 72, 4));
    uint32_t crc = crc32_ieee(r, 76);
    CHECK_EQ(r[76] | r[77] << 8 | r[78] << 16 | (uint32_t)r[79] << 24, crc);
    CHECK(bt_classic_bond_decode(&back, r, sizeof r));
    CHECK(back.valid); CHECK_EQ(back.key_type, 0x04);
    CHECK(!memcmp(back.peer, PHONES, 6)); CHECK(!memcmp(back.local, LOCAL, 6));
    CHECK(!memcmp(back.key, OLD_KEY, 16)); CHECK(!strcmp(back.name, "SoundLink II"));
    for (int bit = 0; bit < 8 * BT_CLASSIC_BOND_RECORD_BYTES; bit++) {
        r[bit / 8] ^= (uint8_t)(1 << (bit % 8));
        memset(&back, 0x5a, sizeof back);
        if (bt_classic_bond_decode(&back, r, sizeof r)) { failures++; printf("FAIL bit %d flipped was taken\n", bit); }
        else if (!all_zero((const uint8_t *)&back, sizeof back)) { failures++; printf("FAIL a refused record left bytes\n"); }
        r[bit / 8] ^= (uint8_t)(1 << (bit % 8));
    }
    CHECK(!bt_classic_bond_decode(&back, r, sizeof r - 1));
    CHECK(!bt_classic_bond_decode(&back, r, 0));
    uint8_t longer[BT_CLASSIC_BOND_RECORD_BYTES + 1];
    memcpy(longer, r, sizeof r); longer[sizeof r] = 0;
    CHECK(!bt_classic_bond_decode(&back, longer, sizeof longer));
    // Each field rule, with the check made good so only the rule refuses.
    uint8_t t[BT_CLASSIC_BOND_RECORD_BYTES];
    memcpy(t, r, sizeof r); t[7] = '2'; reseal(t); CHECK(!bt_classic_bond_decode(&back, t, sizeof t));
    memcpy(t, r, sizeof r); t[0] = 'o'; reseal(t); CHECK(!bt_classic_bond_decode(&back, t, sizeof t));
    memcpy(t, r, sizeof r); t[8] = 3; reseal(t); CHECK(!bt_classic_bond_decode(&back, t, sizeof t));
    memcpy(t, r, sizeof r); t[8] = 2; reseal(t); CHECK(!bt_classic_bond_decode(&back, t, sizeof t));
    for (int at = 10; at < 12; at++) { memcpy(t, r, sizeof r); t[at] = 1; reseal(t); CHECK(!bt_classic_bond_decode(&back, t, sizeof t)); }
    for (int at = 72; at < 76; at++) { memcpy(t, r, sizeof r); t[at] = 1; reseal(t); CHECK(!bt_classic_bond_decode(&back, t, sizeof t)); }
    memcpy(t, r, sizeof r); memset(t + 40, 'A', 32); reseal(t); CHECK(!bt_classic_bond_decode(&back, t, sizeof t));
    memcpy(t, r, sizeof r); memset(t + 40, 'A', 31); t[71] = 0; reseal(t); CHECK(bt_classic_bond_decode(&back, t, sizeof t));
    CHECK_EQ(strlen(back.name), 31);
    memcpy(t, r, sizeof r); t[41] = 0x1f; reseal(t); CHECK(!bt_classic_bond_decode(&back, t, sizeof t));
    memcpy(t, r, sizeof r); t[41] = 0x7f; reseal(t); CHECK(!bt_classic_bond_decode(&back, t, sizeof t));
    memcpy(t, r, sizeof r); t[41] = 0x20; reseal(t); CHECK(bt_classic_bond_decode(&back, t, sizeof t));
    memcpy(t, r, sizeof r); t[41] = 0x7e; reseal(t); CHECK(bt_classic_bond_decode(&back, t, sizeof t));
    memcpy(t, r, sizeof r); t[40 + 13] = 'x'; reseal(t); CHECK(!bt_classic_bond_decode(&back, t, sizeof t));
    memcpy(t, r, sizeof r); t[40] = 0; reseal(t); CHECK(!bt_classic_bond_decode(&back, t, sizeof t));
    memcpy(t, r, sizeof r); memset(t + 40, 0, 32); reseal(t); CHECK(bt_classic_bond_decode(&back, t, sizeof t));
    CHECK_EQ(back.name[0], 0);
    // No bond: the record says so and nothing else.
    bt_classic_bond_t none = {0};
    memset(&none, 0x77, sizeof none); none.valid = false;
    bt_classic_bond_encode(t, &none);
    CHECK(all_zero(t + 8, 68));
    CHECK(bt_classic_bond_decode(&back, t, sizeof t)); CHECK(!back.valid);
    CHECK(all_zero((const uint8_t *)&back, sizeof back));
    for (int at = 8; at < 76; at++) {
        uint8_t u[BT_CLASSIC_BOND_RECORD_BYTES];
        memcpy(u, t, sizeof t); u[at] = 1; reseal(u);
        bool taken = bt_classic_bond_decode(&back, u, sizeof u);
        if (at == 8) CHECK(taken && back.valid);    // the valid flag alone: a bond of zeroes with no name
        else if (taken) { failures++; printf("FAIL an empty record with byte %d set was taken\n", at); }
    }
    // A name that is too long or not printable is cut and cleaned on the way in.
    bt_classic_bond_t odd = bond_for(PHONES, OLD_KEY);
    memset(odd.name, 'B', sizeof odd.name);
    odd.name[3] = '\t';
    bt_classic_bond_encode(t, &odd);
    CHECK(bt_classic_bond_decode(&back, t, sizeof t));
    CHECK_EQ(strlen(back.name), 31); CHECK_EQ(back.name[3], '?');
    // set_bond keeps a copy, and a bond that is not valid leaves none.
    bt_classic_t c;
    bt_classic_init(&c, LOCAL);
    bt_classic_set_bond(&c, &b);
    CHECK(c.bond.valid); CHECK(!memcmp(c.bond.key, OLD_KEY, 16)); CHECK(c.bond_loaded);
    CHECK_EQ(c.scan_enable_wanted, 0x02);
    bt_classic_bond_t bad = b;
    bad.valid = false;
    bt_classic_set_bond(&c, &bad);
    CHECK(!c.bond.valid); CHECK(all_zero(c.bond.key, 16)); CHECK_EQ(c.scan_enable_wanted, 0x00);
    bt_classic_set_bond(&c, &b);
    CHECK(request(&c, "forget", NULL, NULL));
    CHECK(!c.bond.valid); CHECK(all_zero((const uint8_t *)&c.bond, sizeof c.bond)); CHECK(c.bond_dirty);
}

// ── 14: every event at every wrong length ───────────────────────────────────

static const struct { uint8_t code, params; bool shared; } EVENTS[] = {
    {0x03, 11, false}, {0x04, 10, false}, {0x05, 4, true}, {0x06, 3, false}, {0x08, 4, true},
    {0x12, 8, false}, {0x14, 6, false}, {0x16, 6, false}, {0x17, 6, false}, {0x18, 23, false},
    {0x30, 3, true}, {0x31, 6, false}, {0x32, 9, false}, {0x33, 10, false}, {0x36, 7, false},
};

// The parameters an event would carry for this link: its handle, its peer.
static void fill(uint8_t *e, uint8_t code, size_t params, const bt_classic_t *c)
{
    memset(e, 0, 2 + params + 2);
    e[0] = code;
    uint8_t *p = e + 2;
    bool addr_first = code == 0x04 || code == 0x16 || code == 0x17 || code == 0x18 ||
                      code == 0x31 || code == 0x32 || code == 0x33;
    if (addr_first) { memcpy(p, c->peer, params < 6 ? params : 6); if (code == 0x04 && params > 9) p[9] = 1; }
    else if (code == 0x12 || code == 0x36) memcpy(p + 1, c->peer, params - 1 < 6 ? params - 1 : 6);
    else {
        if (params >= 3) { p[1] = (uint8_t)c->handle; p[2] = (uint8_t)(c->handle >> 8); }
        if (code == 0x03 && params >= 9) memcpy(p + 3, c->peer, 6);
        if (code == 0x03 && params >= 10) p[9] = 1;
        if (code == 0x08 && params >= 4) p[3] = 1;
    }
}

static void malformed_in(bt_classic_t *c, const char *where)
{
    for (unsigned i = 0; i < sizeof EVENTS / sizeof EVENTS[0]; i++)
        for (int delta = -1; delta <= 1; delta += 2) {
            for (int consistent = 0; consistent < 2; consistent++) {
                size_t params = (size_t)(EVENTS[i].params + delta);
                uint8_t e[40];
                fill(e, EVENTS[i].code, params, c);
                e[1] = consistent ? (uint8_t)params : EVENTS[i].params;
                bt_classic_t before;
                memcpy(&before, c, sizeof before);
                bool mine = bt_classic_event(c, e, 2 + params);
                bt_classic_t after;
                memcpy(&after, c, sizeof after);
                after.malformed = before.malformed;
                if (memcmp(&before, &after, sizeof before) || c->malformed != before.malformed + 1) {
                    failures++;
                    printf("FAIL %s: event %02x with %zu parameters (length byte %u) moved the link or was not counted\n",
                           where, EVENTS[i].code, params, e[1]);
                    memcpy(c, &before, sizeof before);
                }
                if (consistent && mine != !EVENTS[i].shared) {
                    failures++; printf("FAIL %s: event %02x malformed claimed %d\n", where, EVENTS[i].code, mine);
                }
            }
        }
    // The reply to the command in flight, one byte short and one long.
    if (c->pending) {
        unsigned full;
        bool async = c->opcode == 0x0405 || c->opcode == 0x0406 || c->opcode == 0x0409 || c->opcode == 0x040a ||
                     c->opcode == 0x0411 || c->opcode == 0x0413 || c->opcode == 0x042a;
        full = async ? 6 : c->opcode == 0x1408 ? 9 : c->opcode == 0x0c1a ? 6 : 12;
        for (int delta = -1; delta <= 1; delta += 2) {
            uint8_t e[24] = {0};
            size_t n = full + delta;
            e[0] = async ? 0x0f : 0x0e; e[1] = (uint8_t)(n - 2);
            if (async) { e[2] = 0; e[3] = 1; e[4] = (uint8_t)c->opcode; e[5] = (uint8_t)(c->opcode >> 8); }
            else {
                e[2] = 1; e[3] = (uint8_t)c->opcode; e[4] = (uint8_t)(c->opcode >> 8);
                memcpy(e + 6, c->command_address, n > 12 ? 6 : n - 6);
            }
            bt_classic_t before;
            memcpy(&before, c, sizeof before);
            bt_classic_event(c, e, n);
            bt_classic_t after;
            memcpy(&after, c, sizeof after);
            after.malformed = before.malformed;
            if (memcmp(&before, &after, sizeof before) || c->malformed != before.malformed + 1) {
                failures++;
                printf("FAIL %s: the reply to %04x at %zu bytes moved the link or was not counted\n", where, c->opcode, n);
                memcpy(c, &before, sizeof before);
            }
        }
    }
}

static void test_malformed(void)
{
    bt_classic_t c;
    bt_classic_bond_t b = bond_for(PHONES, OLD_KEY);
    start(&c, &b);
    memcpy(c.peer, PHONES, 6);
    malformed_in(&c, "idle");
    M.has_key = true; memcpy(M.key, OLD_KEY, 16);
    M.silent = true;
    CHECK(request(&c, "reconnect", NULL, NULL));
    step(&c, &M, 10);
    malformed_in(&c, "paging, 0405 out");
    M.silent = false;
    // Each reply by hand, the model silent, one step each.
    start(&c, &b);
    M.silent = true;
    CHECK(request(&c, "reconnect", NULL, NULL));
    step(&c, &M, 10);
    ev_cs(&M, 0, 0, 0x0405); ev_conn(&M, 0, TAG_NONE, 0, 0x0123, PHONES, 1);
    step(&c, &M, 10);
    CHECK(c.pending && c.opcode == 0x0411);
    malformed_in(&c, "authenticating, 0411 out");
    ev_cs(&M, 0, 0, 0x0411); ev_addr(&M, 0, 0x17, PHONES);
    step(&c, &M, 10);
    CHECK(c.pending && c.opcode == 0x040b);
    malformed_in(&c, "authenticating, 040b out");
    ev_cc_addr(&M, 0, 0x040b, 0, PHONES); ev_auth(&M, 0, 0, 0x0123);
    step(&c, &M, 10);
    CHECK(c.pending && c.opcode == 0x0413);
    malformed_in(&c, "encrypting, 0413 out");
    ev_cs(&M, 0, 0, 0x0413); ev_enc(&M, 0, 0, 0x0123, 1);
    step(&c, &M, 10);
    CHECK(c.pending && c.opcode == 0x1408);
    malformed_in(&c, "checking, 1408 out");
    ev_keysize(&M, 0, 0, 0x0123, 16);
    step(&c, &M, 10);
    CHECK_EQ(c.phase, BT_CLASSIC_READY);
    malformed_in(&c, "ready");
    // And the inbound side: a call latched, the accept out.
    start(&c, &b);
    M.silent = true;
    M.control = false;
    ev_request(&M, 0, PHONES, 1);
    step(&c, &M, 10);
    CHECK_EQ(c.phase, BT_CLASSIC_ACCEPTING);
    malformed_in(&c, "accepting, latched");
    M.control = true;
    step(&c, &M, 10);
    CHECK(c.pending && c.opcode == 0x0409);
    malformed_in(&c, "accepting, 0409 out");
    // Replies that cannot be: the wrong address back, the wrong handle, a
    // key size past 16, Command Complete for an asynchronous command,
    // Command Status without an error for one that completes, a second reply.
    start(&c, &b);
    M.silent = true;
    ev_addr(&M, 0, 0x17, STRANGER);
    step(&c, &M, 10);
    CHECK(c.pending && c.opcode == 0x040c);
    uint32_t bad = c.malformed;
    ev_cc_addr(&M, 0, 0x040c, 0, PHONES);
    ev_cs(&M, 0, 0, 0x040c);
    step(&c, &M, 10);
    CHECK_EQ(c.malformed, bad + 2); CHECK(c.pending); CHECK(!c.command_done);
    ev_cc_addr(&M, 0, 0x040c, 0, STRANGER);
    ev_cc_addr(&M, 0, 0x040c, 0, STRANGER);
    step(&c, &M, 10);
    CHECK_EQ(c.malformed, bad + 3); CHECK(!c.pending);
    // An error status needs no address match.
    ev_addr(&M, 0, 0x17, STRANGER);
    step(&c, &M, 10);
    ev_cc_addr(&M, 0, 0x040c, 0x12, PHONES);
    step(&c, &M, 10);
    CHECK(!c.pending); CHECK_EQ(c.malformed, bad + 3);
    // The page: Command Complete is not its reply.
    start(&c, &b);
    M.silent = true;
    CHECK(request(&c, "reconnect", NULL, NULL));
    step(&c, &M, 10);
    bad = c.malformed;
    ev_cc_status(&M, 0, 0x0405, 0);
    step(&c, &M, 10);
    CHECK_EQ(c.malformed, bad + 1); CHECK(c.pending);
    // Read Encryption Key Size: wrong handle, size past 16.
    start(&c, &b);
    M.silent = true;
    CHECK(request(&c, "reconnect", NULL, NULL));
    step(&c, &M, 10);
    ev_cs(&M, 0, 0, 0x0405); ev_conn(&M, 0, TAG_NONE, 0, 0x77, PHONES, 1);
    step(&c, &M, 10);
    ev_cs(&M, 0, 0, 0x0411); ev_enc(&M, 0, 0, 0x77, 1);
    step(&c, &M, 10);
    CHECK(c.pending && c.opcode == 0x1408);
    bad = c.malformed;
    ev_keysize(&M, 0, 0, 0x78, 16);
    ev_keysize(&M, 0, 0, 0x77, 17);
    step(&c, &M, 10);
    CHECK_EQ(c.malformed, bad + 2); CHECK(c.pending); CHECK_EQ(c.key_size, 0);
    // A Connection Complete with a handle past 0x0eff cannot be.
    start(&c, &b);
    M.silent = true;
    CHECK(request(&c, "reconnect", NULL, NULL));
    step(&c, &M, 10);
    bad = c.malformed;
    ev_conn(&M, 0, TAG_NONE, 0, 0x0f00, PHONES, 1);
    step(&c, &M, 10);
    CHECK_EQ(c.malformed, bad + 1); CHECK_EQ(c.phase, BT_CLASSIC_PAGING); CHECK(!c.connected);
    ev_conn(&M, 0, TAG_NONE, 0, 0x0eff, PHONES, 1);
    step(&c, &M, 10);
    CHECK(c.connected); CHECK_EQ(c.handle, 0x0eff);
    // An unknown link type in a Connection Request cannot be.
    start(&c, &b);
    bad = c.malformed;
    ev_request(&M, 0, PHONES, 3);
    step(&c, &M, 10);
    CHECK_EQ(c.malformed, bad + 1); CHECK_EQ(c.phase, BT_CLASSIC_IDLE); CHECK(!bt_classic_urgent(&c));
    // A length byte that disagrees with the bytes, and an event with none.
    uint8_t e[4] = {0x17, 6, 0, 0};
    bad = c.malformed;
    CHECK(!bt_classic_event(&c, e, 4));
    CHECK(!bt_classic_event(&c, e, 1));
    CHECK(!bt_classic_event(&c, e, 0));
    CHECK_EQ(c.malformed, bad + 3);
    // Number Of Completed Packets is never this link's.
    uint8_t done[7] = {0x13, 5, 1, (uint8_t)c.handle, (uint8_t)(c.handle >> 8), 1, 0};
    CHECK(!bt_classic_event(&c, done, sizeof done));
    CHECK_EQ(c.malformed, bad + 3);
}

// What is claimed: the shared events only for this link's live handle.
static void test_claims(void)
{
    bt_classic_t c;
    bt_classic_bond_t b = bond_for(PHONES, OLD_KEY);
    start(&c, &b);
    M.has_key = true; memcpy(M.key, OLD_KEY, 16);
    CHECK(request(&c, "reconnect", NULL, NULL));
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_READY, 3000));
    uint16_t h = c.handle, other = (uint16_t)(h + 1);
    uint8_t enc_other[6] = {0x08, 4, 0, (uint8_t)other, (uint8_t)(other >> 8), 1};
    uint8_t refresh_other[5] = {0x30, 3, 0, (uint8_t)other, (uint8_t)(other >> 8)};
    uint8_t disc_other[6] = {0x05, 4, 0, (uint8_t)other, (uint8_t)(other >> 8), 0x13};
    uint8_t refresh[5] = {0x30, 3, 0x12, (uint8_t)h, (uint8_t)(h >> 8)};
    CHECK(!bt_classic_event(&c, enc_other, sizeof enc_other));
    CHECK(!bt_classic_event(&c, refresh_other, sizeof refresh_other));
    CHECK(!bt_classic_event(&c, disc_other, sizeof disc_other));
    CHECK_EQ(c.phase, BT_CLASSIC_READY); CHECK(c.encrypted);
    CHECK(bt_classic_event(&c, refresh, sizeof refresh));
    CHECK_EQ(c.refresh_status, 0x12);
    // A reply to the last command, with nothing in flight, is not this link's.
    CHECK(!c.pending);
    uint8_t cc_other[9] = {0x0e, 7, 1, (uint8_t)c.opcode, (uint8_t)(c.opcode >> 8), 0, (uint8_t)h, (uint8_t)(h >> 8), 16};
    uint32_t bad = c.malformed;
    CHECK(!bt_classic_event(&c, cc_other, sizeof cc_other));
    CHECK_EQ(c.malformed, bad);
    uint8_t role_other[10] = {0x12, 8, 0, 1, 2, 3, 4, 5, 6, 0};
    CHECK(bt_classic_event(&c, role_other, sizeof role_other));
    CHECK_EQ(c.role_known, false);
    uint8_t unknown[3] = {0x2c, 1, 0};
    CHECK(!bt_classic_event(&c, unknown, sizeof unknown));
    // Auth Complete for another handle changes nothing.
    uint8_t auth_other[5] = {0x06, 3, 0x05, (uint8_t)other, (uint8_t)(other >> 8)};
    CHECK(bt_classic_event(&c, auth_other, sizeof auth_other));
    CHECK_EQ(c.phase, BT_CLASSIC_READY);
    // A Link Key Notification from a stranger is not kept, and neither is
    // a Mode Change for another handle.
    uint8_t mode_other[8] = {0x14, 6, 0, (uint8_t)other, (uint8_t)(other >> 8), 2, 1, 0};
    CHECK(bt_classic_event(&c, mode_other, sizeof mode_other));
    CHECK(!c.mode_known);
    // A Disconnection Complete with an error outside CLOSING is ignored.
    uint8_t disc_failed[6] = {0x05, 4, 0x0c, (uint8_t)h, (uint8_t)(h >> 8), 0x13};
    CHECK(bt_classic_event(&c, disc_failed, sizeof disc_failed));
    CHECK_EQ(c.phase, BT_CLASSIC_READY); CHECK(c.connected);
}

// A bond pairing's key from a stranger, or before READY when the session
// fails, is never kept.
static void test_key_hygiene(void)
{
    bt_classic_t c;
    bt_classic_bond_t none = {0};
    start(&c, &none);
    char text[64];
    snprintf(text, sizeof text, "bond %s", PHONES_TEXT);
    CHECK(request(&c, text, NULL, NULL));
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_AUTHENTICATING, 1000));
    ev_key_notify(&M, 0, STRANGER, OLD_KEY, 0x04);
    step(&c, &M, 10);
    CHECK(!c.have_key);
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_ENCRYPTING, 1000));
    CHECK(c.have_key); CHECK(!memcmp(c.new_key, NEW_KEY, 16));
    check_no_key(&c, NEW_KEY);
    M.qn = 0; ev_disconn(&M, 0, 0, M.handle, 0x13); M.live = false;
    step(&c, &M, 10);
    CHECK(!c.have_key); CHECK(all_zero(c.new_key, 16));
    CHECK(!c.bond.valid); CHECK(!c.bond_dirty);
}

// A link driven by hand, the model silent: the page answered and the link
// up as `handle`, Authentication Requested out.
static void silent_link(bt_classic_t *c, const bt_classic_bond_t *b, const char *text, uint16_t handle)
{
    start(c, b);
    M.silent = true;
    CHECK(request(c, text, NULL, NULL));
    step(c, &M, 10);
    ev_cs(&M, 0, 0, 0x0405); ev_conn(&M, 0, TAG_NONE, 0, handle, PHONES, 1);
    step(c, &M, 10);
    CHECK(c->pending && c->opcode == 0x0411);
}

static void test_more_rules(void)
{
    bt_classic_t c;
    bt_classic_bond_t b = bond_for(PHONES, OLD_KEY);
    bt_classic_bond_t none = {0};
    char bond_text[64];
    snprintf(bond_text, sizeof bond_text, "bond %s", PHONES_TEXT);

    // A voice link's Connection Complete while paging is not the page's.
    start(&c, &b);
    M.silent = true;
    CHECK(request(&c, "reconnect", NULL, NULL));
    step(&c, &M, 10);
    ev_conn(&M, 0, TAG_NONE, 0x0d, 0, PHONES, 0);
    ev_conn(&M, 0, TAG_NONE, 0, 0x0005, PHONES, 2);
    step(&c, &M, 10);
    CHECK_EQ(c.phase, BT_CLASSIC_PAGING); CHECK(!c.connected); CHECK(!c.error);

    // While closing a live link, a stray Connection Complete changes nothing.
    silent_link(&c, &b, "reconnect", 0x0060);
    CHECK(request(&c, "disconnect", NULL, NULL));
    ev_cs(&M, 0, 0, 0x0411);
    step(&c, &M, 10);
    CHECK(c.pending && c.opcode == 0x0406);
    ev_conn(&M, 0, TAG_NONE, 0, 0x0061, PHONES, 1);
    step(&c, &M, 10);
    CHECK_EQ(c.handle, 0x0060); CHECK(c.connected); CHECK_EQ(c.phase, BT_CLASSIC_CLOSING);

    // A latched accept the controller gave up on (accept timeout) is not
    // answered with a reject.
    start(&c, &b);
    unsigned from = M.logn;
    M.control = false;
    ev_request(&M, 0, PHONES, 1);
    step(&c, &M, 10);
    ev_conn(&M, 5060, TAG_NONE, 0x10, 0, PHONES, 1);
    run(&c, &M, 5200);
    CHECK_EQ(c.phase, BT_CLASSIC_STOPPED);
    CHECK(c.error && !strcmp(c.error, "the connection was not made"));
    M.control = true;
    run(&c, &M, 200);
    CHECK_EQ(M.logn, from);
    check_clean(&c, "accept timed out");
    // Nor one that waited 25 s with nothing from the controller at all.
    start(&c, &b);
    from = M.logn;
    M.control = false; M.silent = true;
    ev_request(&M, 0, PHONES, 1);
    run(&c, &M, 25100);
    CHECK_EQ(c.phase, BT_CLASSIC_STOPPED);
    M.control = true;
    run(&c, &M, 200);
    CHECK_EQ(M.logn, from);

    // With no bond, even a call from the address of zeroes is a stranger's.
    start(&c, &none);
    static const uint8_t zero[6] = {0};
    ev_request(&M, 0, zero, 1);
    step(&c, &M, 10);
    CHECK_EQ(c.phase, BT_CLASSIC_IDLE);

    // Authentication Complete for a ready link changes nothing.
    start(&c, &b);
    M.has_key = true; memcpy(M.key, OLD_KEY, 16);
    CHECK(request(&c, "reconnect", NULL, NULL));
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_READY, 3000));
    from = M.logn;
    ev_auth(&M, 0, 0, M.handle);
    step(&c, &M, 10);
    CHECK_EQ(c.phase, BT_CLASSIC_READY); CHECK(!c.pending); CHECK_EQ(c.next_op, 0);
    run(&c, &M, 100);
    CHECK_EQ(M.logn, from);

    // Encryption Change with an error status, even saying "on", fails it.
    silent_link(&c, &b, "reconnect", 0x0062);
    ev_cs(&M, 0, 0, 0x0411); ev_enc(&M, 0, 0x06, 0x0062, 1);
    step(&c, &M, 10);
    CHECK_EQ(c.phase, BT_CLASSIC_CLOSING); CHECK(!c.encrypted);
    CHECK_EQ(c.error_status, 0x06);

    // `bond` to the address already bonded: the stored key answers, no new
    // key comes, and nothing is marked to save.
    start(&c, &b);
    M.has_key = true; memcpy(M.key, OLD_KEY, 16);
    CHECK(request(&c, bond_text, NULL, NULL));
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_READY, 3000));
    CHECK(!c.bond_dirty); CHECK(!memcmp(c.bond.key, OLD_KEY, 16)); CHECK(c.bond.valid);

    // A bond for another address gives this one no key.
    bt_classic_bond_t elsewhere = bond_for(STRANGER, OLD_KEY);
    start(&c, &elsewhere);
    M.expect_auth = 0x00;
    from = M.logn;
    char connect_text[64];
    snprintf(connect_text, sizeof connect_text, "connect %s", PHONES_TEXT);
    CHECK(request(&c, connect_text, NULL, NULL));
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_READY, 3000));
    CHECK(find_op(0x040c, from)); CHECK(!find_op(0x040b, from)); CHECK(!c.key_given);

    // A page cancel answered "Unknown Connection": nothing was outstanding,
    // so it is over at once.
    start(&c, &b);
    M.silent = true;
    CHECK(request(&c, "reconnect", NULL, NULL));
    step(&c, &M, 10);
    ev_cs(&M, 0, 0, 0x0405);
    step(&c, &M, 10);
    CHECK(request(&c, "disconnect", NULL, NULL));
    step(&c, &M, 10);
    CHECK(c.pending && c.opcode == 0x0408);
    ev_cc_addr(&M, 0, 0x0408, 0x02, PHONES);
    step(&c, &M, 10);
    CHECK_EQ(c.phase, BT_CLASSIC_STOPPED); CHECK(!c.error);

    // Disconnect answered "Unknown Connection": the link is already gone.
    silent_link(&c, &b, "reconnect", 0x0063);
    ev_cs(&M, 0, 0, 0x0411);
    step(&c, &M, 10);
    CHECK(request(&c, "disconnect", NULL, NULL));
    step(&c, &M, 10);
    CHECK(c.pending && c.opcode == 0x0406);
    ev_cs(&M, 0, 0x02, 0x0406);
    step(&c, &M, 10);
    CHECK_EQ(c.phase, BT_CLASSIC_STOPPED); CHECK(!c.error); CHECK(!c.connected);
    // Disconnect refused for another reason: given up on, said so.
    silent_link(&c, &b, "reconnect", 0x0064);
    ev_cs(&M, 0, 0, 0x0411);
    step(&c, &M, 10);
    CHECK(request(&c, "disconnect", NULL, NULL));
    step(&c, &M, 10);
    ev_cs(&M, 0, 0x0c, 0x0406);
    step(&c, &M, 10);
    CHECK_EQ(c.phase, BT_CLASSIC_STOPPED); CHECK(!c.connected);
    CHECK(c.error && !strcmp(c.error, "the controller refused to disconnect"));
    CHECK_EQ(c.error_status, 0x0c); CHECK_EQ(c.error_opcode, 0x0406);
    // A Disconnect nobody answers at all.
    silent_link(&c, &b, "reconnect", 0x0065);
    ev_cs(&M, 0, 0, 0x0411);
    step(&c, &M, 10);
    CHECK(request(&c, "disconnect", NULL, NULL));
    run(&c, &M, 2100);
    CHECK_EQ(c.phase, BT_CLASSIC_STOPPED);
    CHECK(c.error && !strcmp(c.error, "the controller did not answer the disconnection"));

    // A reply to a command from an attempt that has ended does not end the next one.
    for (int late = 0; late < 2; late++) {
        silent_link(&c, &b, "reconnect", 0x0066);
        ev_disconn(&M, 0, 0, 0x0066, 0x13);
        step(&c, &M, 10);
        CHECK_EQ(c.phase, BT_CLASSIC_STOPPED); CHECK(c.pending && c.opcode == 0x0411);
        CHECK(request(&c, "reconnect", NULL, NULL));
        if (!late) ev_cs(&M, 0, 0x0c, 0x0411);
        run(&c, &M, late ? 2100 : 10);
        CHECK_EQ(c.phase, BT_CLASSIC_PAGING); CHECK(!c.error);
        CHECK(c.pending && c.opcode == 0x0405);
    }

    // A refusal the controller rejects does not touch the link.
    silent_link(&c, &b, "reconnect", 0x0067);
    ev_cs(&M, 0, 0, 0x0411); ev_addr(&M, 0, 0x17, STRANGER);
    step(&c, &M, 10);
    CHECK(c.pending && c.opcode == 0x040c);
    ev_cc_addr(&M, 0, 0x040c, 0x12, STRANGER);
    step(&c, &M, 10);
    CHECK_EQ(c.phase, BT_CLASSIC_AUTHENTICATING); CHECK(!c.error);

    // The first error is the one kept: a stale bond, then a controller that
    // never confirms the disconnection.
    start(&c, &b);
    M.has_key = false;
    CHECK(request(&c, "reconnect", NULL, NULL));
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_CLOSING, 3000));
    M.silent = true; M.qn = 0;
    run(&c, &M, 5000);
    CHECK_EQ(c.phase, BT_CLASSIC_STOPPED);
    CHECK(c.error && !strcmp(c.error, "the headphones no longer hold this bond; forget and bond again"));
    // And a new attempt starts with none.
    M.silent = false; M.has_key = true; memcpy(M.key, OLD_KEY, 16); M.live = false;
    CHECK(request(&c, "reconnect", NULL, NULL));
    CHECK(run_until_phase(&c, &M, BT_CLASSIC_READY, 3000));
    CHECK(!c.error); CHECK(!c.retryable); CHECK_EQ(c.error_status, 0);
    CHECK_TEXT(&c, "error: none (opcode 0000 status 00)");

    // The tick that gives up on an unanswered command sends nothing, even
    // with control: that command's credit is still the controller's.
    start(&c, &b);
    M.silent = true;
    CHECK(request(&c, "reconnect", NULL, NULL));
    step(&c, &M, 10);
    CHECK(c.pending && c.opcode == 0x0405);
    from = M.logn;
    uint64_t deadline = c.command_deadline;
    while (M.now + 10 < deadline) step(&c, &M, 10);
    CHECK(c.pending); CHECK_EQ(M.logn, from);
    step(&c, &M, 10);
    CHECK(!c.pending); CHECK(c.error && !strcmp(c.error, "HCI command timed out"));
    CHECK_EQ(c.phase, BT_CLASSIC_CLOSING); CHECK_EQ(c.next_op, 0x0408); CHECK_EQ(M.logn, from);
    step(&c, &M, 10);
    CHECK_EQ(M.logn, from + 1); CHECK(find_op(0x0408, from));

    // A cancel queued when the page completes is not sent: a Disconnect is.
    start(&c, &b);
    M.reply_only = true;
    from = M.logn;
    CHECK(request(&c, "reconnect", NULL, NULL));
    run(&c, &M, 30);
    M.control = false;
    CHECK(request(&c, "disconnect", NULL, NULL));
    M.live = true; M.handle = 0x68;
    ev_conn(&M, 0, TAG_NONE, 0, 0x68, PHONES, 1);
    step(&c, &M, 10);
    M.control = true;
    run(&c, &M, 200);
    static const uint16_t ops[] = {0x0405, 0x0406};
    expect_ops("cancel overtaken", ops, 2, from);
    check_clean(&c, "cancel overtaken");
}

// ── Random soup ─────────────────────────────────────────────────────────────

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}
static unsigned below(unsigned n) { return rnd() % n; }

typedef struct { uint16_t op; uint8_t p[32], len; } sent_t;
static sent_t S_last;
static unsigned S_sends;
static void soup_send(void *context, uint16_t op, const uint8_t *p, uint8_t len)
{
    (void)context;
    if (!g_control) { failures++; printf("FAIL soup: %04x sent without control\n", op); }
    S_sends++;
    S_last.op = op; S_last.len = len; memcpy(S_last.p, p, len < 32 ? len : 32);
}

static uint8_t soup_keys[8][16];
static unsigned soup_key_count;

static void soup_event(bt_classic_t *c, const uint8_t addrs[3][6])
{
    uint8_t e[40] = {0};
    static const uint8_t codes[] = {0x03, 0x04, 0x05, 0x06, 0x08, 0x12, 0x14, 0x16, 0x17, 0x18,
                                    0x30, 0x31, 0x32, 0x33, 0x36, 0x0e, 0x0f, 0x13, 0x2c};
    uint8_t code = codes[below(sizeof codes)];
    const uint8_t *a = below(3) ? c->peer : addrs[below(3)];
    uint16_t h = below(3) ? c->handle : (uint16_t)below(0x1000);
    uint8_t status = below(3) ? 0 : (uint8_t)rnd();
    size_t params = 0;
    e[0] = code;
    uint8_t *p = e + 2;
    switch (code) {
    case 0x03: params = 11; p[0] = status; p[1] = (uint8_t)h; p[2] = (uint8_t)(h >> 8); memcpy(p + 3, a, 6); p[9] = (uint8_t)below(4); break;
    case 0x04: params = 10; memcpy(p, a, 6); p[9] = (uint8_t)below(4); break;
    case 0x05: params = 4; p[0] = status; p[1] = (uint8_t)h; p[2] = (uint8_t)(h >> 8); p[3] = (uint8_t)rnd(); break;
    case 0x06: case 0x30: params = 3; p[0] = status; p[1] = (uint8_t)h; p[2] = (uint8_t)(h >> 8); break;
    case 0x08: params = 4; p[0] = status; p[1] = (uint8_t)h; p[2] = (uint8_t)(h >> 8); p[3] = (uint8_t)below(3); break;
    case 0x12: params = 8; p[0] = status; memcpy(p + 1, a, 6); p[7] = (uint8_t)below(2); break;
    case 0x14: params = 6; p[0] = status; p[1] = (uint8_t)h; p[2] = (uint8_t)(h >> 8); p[3] = (uint8_t)below(3); break;
    case 0x16: case 0x17: case 0x31: params = 6; memcpy(p, a, 6); break;
    case 0x18: {
        params = 23; memcpy(p, a, 6);
        uint8_t *k = soup_keys[soup_key_count++ % 8];
        for (int i = 0; i < 16; i++) k[i] = (uint8_t)rnd();
        memcpy(p + 6, k, 16); p[22] = 4;
        break;
    }
    case 0x32: params = 9; memcpy(p, a, 6); p[6] = 3; p[8] = 4; break;
    case 0x33: params = 10; memcpy(p, a, 6); break;
    case 0x36: params = 7; p[0] = status; memcpy(p + 1, a, 6); break;
    case 0x0e: {
        uint16_t op = below(4) ? c->opcode : (uint16_t)rnd();
        p[0] = 1; p[1] = (uint8_t)op; p[2] = (uint8_t)(op >> 8); p[3] = status;
        if (op == 0x1408) { params = 7; p[4] = (uint8_t)c->command_handle; p[5] = (uint8_t)(c->command_handle >> 8); p[6] = below(4) ? 16 : (uint8_t)below(20); }
        else if (op == 0x0c1a) params = 4;
        else { params = 10; memcpy(p + 4, below(4) ? c->command_address : a, 6); }
        break;
    }
    case 0x0f: {
        uint16_t op = below(4) ? c->opcode : (uint16_t)rnd();
        params = 4; p[0] = status; p[1] = 1; p[2] = (uint8_t)op; p[3] = (uint8_t)(op >> 8);
        break;
    }
    case 0x13: params = 5; p[0] = 1; p[1] = (uint8_t)h; p[2] = (uint8_t)(h >> 8); p[3] = 1; break;
    default: params = 1; break;
    }
    size_t n = 2 + params;
    e[1] = (uint8_t)params;
    unsigned shape = below(20);
    if (shape == 0 && n > 2) { n--; e[1]--; }
    else if (shape == 1) { n++; e[1]++; }
    else if (shape == 2) n = below((unsigned)n + 1);
    bt_classic_event(c, e, n);
}

static void soup_request(bt_classic_t *c, const uint8_t addrs[3][6], uint64_t now)
{
    static const char *const texts[] = {
        "bond aa:bb:cc:dd:ee:01", "connect aa:bb:cc:dd:ee:01", "bond 06:05:04:03:02:01", "reconnect",
        "forget", "disconnect", "bond zz", "", "connect 00:00:00:00:00:00",
    };
    const char *t = texts[below(sizeof texts / sizeof texts[0])];
    bt_scan_device_t hint = {0};
    memcpy(hint.address, addrs[below(3)], 6);
    hint.le = below(4) == 0;
    snprintf(hint.name, sizeof hint.name, "soup");
    const char *why = NULL;
    bool took = bt_classic_request(c, t, strlen(t), below(2) ? &hint : NULL, now, &why);
    if (took == (why != NULL)) { failures++; printf("FAIL soup: \"%s\" took=%d refusal=%s\n", t, took, why ? why : "NULL"); }
}

static void soup_invariants(const bt_classic_t *c, unsigned sends, bool control, unsigned round)
{
    int before = failures;
    CHECK(sends <= 1);
    if (!control) CHECK(sends == 0);
    if (c->pending) CHECK(c->opcode != 0);
    if (c->phase == BT_CLASSIC_READY) CHECK(c->connected && c->encrypted);
    if (c->encrypted) CHECK(c->connected);
    bool linked = c->phase >= BT_CLASSIC_LINKED && c->phase <= BT_CLASSIC_READY;
    if (linked) CHECK(c->connected);
    if (c->connected) CHECK(linked || c->phase == BT_CLASSIC_CLOSING || c->phase == BT_CLASSIC_FAILED);
    if (c->connected) CHECK(c->handle <= 0x0eff);
    if (c->phase == BT_CLASSIC_IDLE || c->phase == BT_CLASSIC_STOPPED)
        CHECK(!c->waiting && !c->next_op && !c->have_key);
    bool answers = false;
    for (int i = 0; i < BT_CLASSIC_ANSWERS; i++) answers |= c->answer[i].set;
    if (answers) CHECK(c->phase >= BT_CLASSIC_PAGING && c->phase <= BT_CLASSIC_READY);
    if (c->have_key) CHECK(c->session == BT_CLASSIC_SESSION_BOND);
    CHECK(c->refusal_count <= BT_CLASSIC_REFUSALS);
    CHECK_EQ(c->scan_enable_wanted, c->bond.valid ? 0x02 : 0x00);
    if (c->phase == BT_CLASSIC_FAILED) CHECK(!c->pending && !bt_classic_urgent(c));
    if (c->bond.valid) check_no_key(c, c->bond.key);
    if (c->have_key) check_no_key(c, c->new_key);
    if (failures != before) printf("  (soup round %u, phase %d)\n", round, c->phase);
}

// What a cooperative controller says to each command.
static void soup_answer(bt_classic_t *c)
{
    uint8_t e[16] = {0};
    uint16_t op = S_last.op;
    unsigned kind = op == 0x0405 || op == 0x0406 || op == 0x0409 || op == 0x040a || op == 0x0411 ||
                    op == 0x0413 || op == 0x042a ? 0 : op == 0x1408 ? 1 : op == 0x0c1a ? 2 : 3;
    if (kind == 0) {
        uint8_t cs[6] = {0x0f, 4, 0, 1, (uint8_t)op, (uint8_t)(op >> 8)};
        bt_classic_event(c, cs, 6);
        if (op == 0x0406) {
            uint8_t d[6] = {0x05, 4, 0, S_last.p[0], S_last.p[1], 0x16};
            bt_classic_event(c, d, 6);
        }
        return;
    }
    e[0] = 0x0e; e[2] = 1; e[3] = (uint8_t)op; e[4] = (uint8_t)(op >> 8); e[5] = 0;
    if (kind == 1) { e[1] = 7; e[6] = S_last.p[0]; e[7] = S_last.p[1]; e[8] = 16; bt_classic_event(c, e, 9); }
    else if (kind == 2) { e[1] = 4; bt_classic_event(c, e, 6); }
    else {
        e[1] = 10; memcpy(e + 6, S_last.p, 6); bt_classic_event(c, e, 12);
        if (op == 0x0408) {
            uint8_t cc[13] = {0x03, 11, 0x02, 0, 0};
            memcpy(cc + 5, S_last.p, 6); cc[11] = 1;
            bt_classic_event(c, cc, 13);
        }
    }
}

static void test_soup(unsigned rounds)
{
    const uint8_t addrs[3][6] = {
        {0x01, 0xee, 0xdd, 0xcc, 0xbb, 0xaa}, {0x01, 0x02, 0x03, 0x04, 0x05, 0x06}, {0x77, 0x66, 0x55, 0x44, 0x33, 0x22},
    };
    for (unsigned round = 0; round < rounds; round++) {
        bt_classic_t c;
        uint8_t local[6];
        for (int i = 0; i < 6; i++) local[i] = (uint8_t)rnd();
        bt_classic_init(&c, local);
        bt_classic_bond_t b = {.valid = below(2) == 0, .key_type = 4};
        memcpy(b.peer, addrs[0], 6); memcpy(b.local, local, 6);
        for (int i = 0; i < 16; i++) b.key[i] = (uint8_t)rnd();
        if (below(4)) bt_classic_set_bond(&c, &b);
        soup_key_count = 0;
        uint64_t now = 1000;
        bool dead = false;
        for (int s = 0; s < 150 && !dead; s++) {
            unsigned what = below(10);
            if (what < 5) soup_event(&c, addrs);
            else if (what == 5) soup_request(&c, addrs, now);
            now += below(8) ? 10 : below(8000);
            bool control = below(10) < 7;
            bool failed = below(1000) == 0;
            S_sends = 0; g_control = control;
            bt_classic_tick(&c, now, control, failed, soup_send, NULL);
            soup_invariants(&c, S_sends, control, round);
            if (S_sends && below(3)) soup_answer(&c);
            if (failed) { CHECK_EQ(c.phase, BT_CLASSIC_FAILED); dead = true; }
        }
        // Every key the soup's notifications carried, wherever it went.
        for (unsigned i = 0; i < soup_key_count && i < 8; i++) check_no_key(&c, soup_keys[i]);
        if (dead) continue;
        // From wherever the soup left it: forget, then a cooperative controller.
        const char *why = NULL;
        if (!bt_classic_request(&c, "forget", 6, NULL, now, &why)) { failures++; printf("FAIL soup %u: forget refused\n", round); }
        for (int s = 0; s < 1000 && !bt_classic_quiescent(&c); s++) {
            now += 50;
            S_sends = 0; g_control = true;
            bt_classic_tick(&c, now, true, false, soup_send, NULL);
            if (S_sends > 1) { failures++; printf("FAIL soup %u: %u sends in a tick\n", round, S_sends); }
            if (S_sends) soup_answer(&c);
        }
        if (!bt_classic_quiescent(&c) || c.connected || c.bond.valid || !c.bond_dirty) {
            failures++;
            char t[2048]; bt_classic_status(&c, t, sizeof t);
            printf("FAIL soup %u: forget did not reach quiescence (phase %d pending %d next %04x):\n%s",
                   round, c.phase, c.pending, c.next_op, t);
        }
    }
}

// ── Sniff: Write Link Policy Settings and Exit Sniff Mode (AUDIO_REMOTE.md § Part Z)

// A bonded link, reconnected and READY, with nothing owed.
static void ready_link(bt_classic_t *c)
{
    bt_classic_bond_t b = bond_for(PHONES, OLD_KEY);
    start(c, &b);
    M.has_key = true; memcpy(M.key, OLD_KEY, 16);
    CHECK(request(c, "reconnect", NULL, NULL));
    CHECK(run_until_phase(c, &M, BT_CLASSIC_READY, 3000));
    run(c, &M, 100);
}
static unsigned count_op(uint16_t op, unsigned from)
{
    unsigned k = 0;
    for (unsigned i = from; i < M.logn; i++) k += M.log[i].op == op;
    return k;
}
// The link is up and has recorded no error: neither command ended it.
#define STILL_UP(c) do { CHECK_EQ((c)->phase, BT_CLASSIC_READY); CHECK((c)->connected); CHECK((c)->encrypted); \
    CHECK(!(c)->error); CHECK(!(c)->waiting); } while (0)

// The two commands' bytes and answers, a write asked twice, the mode read.
static void test_sniff_commands(void)
{
    bt_classic_t c;
    ready_link(&c);
    CHECK(!bt_classic_sniff_allowed(&c)); CHECK(!c.sniff_wanted);
    CHECK_EQ(bt_classic_mode(&c), BT_CLASSIC_MODE_UNKNOWN);
    CHECK_TEXT(&c, "sniff: wanted refused confirmed refused; policy writes 0 failed 0");
    // Asking for what is confirmed asks nothing.
    unsigned from = M.logn;
    bt_classic_policy(&c, false);
    run(&c, &M, 100);
    CHECK_EQ(M.logn, from);
    // Allowing: the handle, role switch and sniff.
    bt_classic_policy(&c, true);
    CHECK(bt_classic_policy_busy(&c)); CHECK(!bt_classic_urgent(&c));
    CHECK_TEXT(&c, "confirmed refused (write owed)");
    step(&c, &M, 10);
    CHECK_EQ(M.logn, from + 1);
    CHECK_EQ(M.log[from].op, 0x080d); CHECK_EQ(M.log[from].len, 4); CHECK_EQ(u16(M.log[from].p), M.handle);
    CHECK_EQ(M.log[from].p[2], 0x05); CHECK_EQ(M.log[from].p[3], 0x00);
    CHECK(c.policy_out); CHECK(bt_classic_policy_busy(&c)); CHECK(!bt_classic_sniff_allowed(&c));
    CHECK_TEXT(&c, "(write out)");
    run(&c, &M, 50);
    CHECK(bt_classic_sniff_allowed(&c)); CHECK(!bt_classic_policy_busy(&c)); CHECK_EQ(M.policy, 0x0005);
    CHECK_EQ(c.policy_writes, 1); CHECK_EQ(c.policy_failed, 0); CHECK_EQ(c.policy_status, 0);
    CHECK_TEXT(&c, "sniff: wanted allowed confirmed allowed; policy writes 1 failed 0 last status 00;");
    // The same again sends nothing.
    bt_classic_policy(&c, true);
    run(&c, &M, 300);
    CHECK_EQ(M.policy_writes, 1);
    // The headphones go to sniff (and to hold, once): the link reads it.
    ev_mode(&M, 0, 0, M.handle, 1, 0x0010);
    run(&c, &M, 10);
    CHECK_EQ(bt_classic_mode(&c), BT_CLASSIC_MODE_HOLD);
    ev_mode(&M, 0, 0, M.handle, 2, 0x0320); M.sniff = true;
    run(&c, &M, 10);
    CHECK_EQ(bt_classic_mode(&c), BT_CLASSIC_MODE_SNIFF); CHECK_EQ(c.mode_interval, 0x320);
    CHECK_TEXT(&c, "mode: sniff interval 800");
    // Refusing: role switch alone, then Exit Sniff Mode, its Command Status
    // and the Mode Change.
    from = M.logn;
    bt_classic_policy(&c, false);
    bt_classic_wake(&c);
    CHECK(bt_classic_wake_busy(&c)); CHECK(!bt_classic_urgent(&c));
    CHECK_TEXT(&c, "exit sniff sent 0 failed 0 last status 00 (owed)");
    run(&c, &M, 100);
    static const uint16_t wake[] = {0x080d, 0x0804};
    expect_ops("refuse and wake", wake, 2, from);
    CHECK_EQ(M.log[from].p[2], 0x01); CHECK_EQ(M.log[from].p[3], 0x00); CHECK_EQ(u16(M.log[from].p), M.handle);
    CHECK_EQ(M.log[from + 1].len, 2); CHECK_EQ(u16(M.log[from + 1].p), M.handle);
    CHECK(!bt_classic_sniff_allowed(&c)); CHECK_EQ(M.policy, 0x0001); CHECK(!bt_classic_policy_busy(&c));
    CHECK_EQ(bt_classic_mode(&c), BT_CLASSIC_MODE_ACTIVE); CHECK(!bt_classic_wake_busy(&c));
    CHECK_EQ(c.wakes, 1); CHECK_EQ(c.wakes_failed, 0); CHECK_EQ(c.wake_status, 0);
    CHECK_TEXT(&c, "exit sniff sent 1 failed 0 last status 00; mode changes failed 0");
    // A wake asked while one is out is not owed twice.
    M.reply_delay = 200; M.sniff = true;
    bt_classic_wake(&c);
    step(&c, &M, 10);
    CHECK(c.wake_out);
    bt_classic_wake(&c);
    CHECK(!c.wake_owed);
    run(&c, &M, 500);
    CHECK_EQ(M.wakes, 2);
    STILL_UP(&c);
}

// The turn: LATCHED answers first, then `next_op`, then the policy write,
// then Exit Sniff Mode, then page scan.
static void test_sniff_turns(void)
{
    bt_classic_t c;
    ready_link(&c);
    unsigned from = M.logn;
    M.control = false;
    bt_classic_policy(&c, true); bt_classic_wake(&c);
    ev_addr(&M, 0, 0x16, PHONES);            // PIN Code Request: an answer latched
    run(&c, &M, 50);
    CHECK(bt_classic_urgent(&c));
    M.control = true;
    run(&c, &M, 200);
    static const uint16_t latched[] = {0x040e, 0x080d, 0x0804};
    expect_ops("behind a latched answer", latched, 3, from);
    from = M.logn;
    M.control = false;
    bt_classic_policy(&c, false); bt_classic_wake(&c);
    c.next_op = 0x1408;                      // READY leaves none of its own: one is put there
    bt_classic_bond_t b = bond_for(PHONES, OLD_KEY);
    bt_classic_set_bond(&c, &b);             // page scan owed again
    M.control = true;
    run(&c, &M, 300);
    static const uint16_t queued[] = {0x1408, 0x080d, 0x0804, 0x0c1a};
    expect_ops("behind next_op, ahead of page scan", queued, 4, from);
    STILL_UP(&c);
}

// Refused, malformed and unanswered, for each: recorded, counted, the flag
// cleared, and the link never ended. A refused write is not asked again
// until the value asked for changes.
static void test_sniff_failures(void)
{
    enum { REFUSED, REFUSED_BY_STATUS, MALFORMED, WRONG_HANDLE, UNANSWERED, KINDS };
    static const uint8_t want_status[KINDS] = {0x12, 0x0c, 0xff, 0xff, 0xff};
    for (unsigned kind = 0; kind < KINDS; kind++) {
        bt_classic_t c;
        ready_link(&c);
        uint32_t malformed = c.malformed;
        unsigned from = M.logn;
        if (kind == REFUSED) M.policy_reply = 0x12;
        if (kind == MALFORMED) M.malform_op = 0x080d;
        if (kind == WRONG_HANDLE) M.wrong_handle = true;
        if (kind == UNANSWERED || kind == REFUSED_BY_STATUS) M.drop_op = 0x080d;
        bt_classic_policy(&c, true);
        step(&c, &M, 10);
        if (kind == REFUSED_BY_STATUS) ev_cs(&M, 0, 0x0c, 0x080d);
        run(&c, &M, 3000);
        CHECK_EQ(count_op(0x080d, from), 1);
        CHECK_EQ(c.policy_failed, 1); CHECK_EQ(c.policy_status, want_status[kind]);
        CHECK(!bt_classic_sniff_allowed(&c)); CHECK(!bt_classic_policy_busy(&c)); CHECK(!c.pending);
        CHECK_EQ(c.malformed, malformed + (kind == MALFORMED || kind == WRONG_HANDLE));
        STILL_UP(&c);
        // Not asked again for the same value; nor for the confirmed one.
        M.policy_reply = 0; M.malform_op = 0; M.wrong_handle = false; M.drop_op = 0;
        bt_classic_policy(&c, true);
        run(&c, &M, 200);
        bt_classic_policy(&c, false);
        run(&c, &M, 200);
        CHECK_EQ(count_op(0x080d, from), 1);
        // A change is asked again.
        bt_classic_policy(&c, true);
        run(&c, &M, 200);
        CHECK_EQ(count_op(0x080d, from), 2);
        CHECK(bt_classic_sniff_allowed(&c));
        STILL_UP(&c);
    }
    enum { DISALLOWED, WAKE_REFUSED, WAKE_MALFORMED, WAKE_UNANSWERED, WAKE_KINDS };
    static const uint8_t wake_status[WAKE_KINDS] = {0x0c, 0x12, 0xff, 0xff};
    for (unsigned kind = 0; kind < WAKE_KINDS; kind++) {
        bt_classic_t c;
        ready_link(&c);
        unsigned from = M.logn;
        M.sniff = kind != DISALLOWED;             // out of sniff a moment before: command disallowed
        if (kind == WAKE_REFUSED) M.wake_reply = 0x12;
        if (kind == WAKE_MALFORMED) M.malform_op = 0x0804;
        if (kind == WAKE_UNANSWERED) M.drop_op = 0x0804;
        bt_classic_wake(&c);
        run(&c, &M, 3000);
        CHECK_EQ(count_op(0x0804, from), 1);
        CHECK_EQ(c.wakes, 1); CHECK_EQ(c.wakes_failed, 1); CHECK_EQ(c.wake_status, wake_status[kind]);
        CHECK(!bt_classic_wake_busy(&c)); CHECK(!c.pending);
        STILL_UP(&c);
        // A wake has no memory of the last: asked again, sent again.
        M.wake_reply = 0; M.malform_op = 0; M.drop_op = 0;
        bt_classic_wake(&c);
        run(&c, &M, 200);
        CHECK_EQ(count_op(0x0804, from), 2);
        STILL_UP(&c);
    }
    bt_classic_t c;
    ready_link(&c);
    M.wake_reply = 0;
    bt_classic_wake(&c);
    run(&c, &M, 200);
    CHECK_TEXT(&c, "exit sniff sent 1 failed 1 last status 0c;");
}

// A Mode Change that carries an error is counted and leaves the mode; one
// for another handle is not the link's.
static void test_sniff_mode_change(void)
{
    bt_classic_t c;
    ready_link(&c);
    ev_mode(&M, 0, 0, M.handle, 2, 0x0320);
    run(&c, &M, 10);
    ev_mode(&M, 0, 0x12, M.handle, 0, 0);
    ev_mode(&M, 0, 0x0c, (uint16_t)(M.handle + 1), 0, 0);
    ev_mode(&M, 0, 0, (uint16_t)(M.handle + 1), 0, 0);
    run(&c, &M, 10);
    CHECK_EQ(bt_classic_mode(&c), BT_CLASSIC_MODE_SNIFF); CHECK_EQ(c.mode_interval, 0x320);
    CHECK_EQ(c.mode_failed, 1); CHECK_EQ(c.mode_fail_status, 0x12);
    CHECK_TEXT(&c, "mode changes failed 1 last status 12\n");
    STILL_UP(&c);
    // With the connection gone the mode is nobody's.
    ev_disconn(&M, 0, 0, M.handle, 0x13); M.live = false;
    run(&c, &M, 10);
    CHECK_EQ(c.phase, BT_CLASSIC_STOPPED); CHECK_EQ(bt_classic_mode(&c), BT_CLASSIC_MODE_UNKNOWN);
}

// The link dropping with each command owed and with each out: nothing is
// sent after it, a late answer confirms nothing, the link is quiescent once
// nothing is in flight, and the next connection starts clean.
static void test_sniff_drop(void)
{
    for (unsigned local = 0; local < 2; local++) {
        bt_classic_t c;
        ready_link(&c);
        M.control = false;
        bt_classic_policy(&c, true); bt_classic_wake(&c);
        unsigned from = M.logn;
        if (local) { M.expect_reason = 0x13; CHECK(request(&c, "disconnect", NULL, NULL)); }
        else { ev_disconn(&M, 0, 0, M.handle, 0x13); M.live = false; }
        step(&c, &M, 10);
        CHECK(!bt_classic_policy_busy(&c)); CHECK(!bt_classic_wake_busy(&c));
        CHECK(!c.sniff_wanted); CHECK(!bt_classic_sniff_allowed(&c));
        M.control = true;
        run(&c, &M, 3000);
        CHECK_EQ(count_op(0x080d, from) + count_op(0x0804, from), 0);
        if (local) { static const uint16_t closing[] = {0x0406}; expect_ops("owed, then disconnect", closing, 1, from); }
        check_clean(&c, local ? "owed, then disconnect" : "owed, then dropped");
    }
    for (unsigned answered = 0; answered < 2; answered++) for (unsigned wake = 0; wake < 2; wake++) {
        bt_classic_t c;
        ready_link(&c);
        M.sniff = true;
        if (answered) M.reply_delay = 500; else M.drop_op = wake ? 0x0804 : 0x080d;
        if (wake) bt_classic_wake(&c); else bt_classic_policy(&c, true);
        step(&c, &M, 10);
        CHECK(wake ? c.wake_out : c.policy_out);
        unsigned from = M.logn;
        ev_disconn(&M, 0, 0, M.handle, 0x08); M.live = false;
        step(&c, &M, 10);
        CHECK_EQ(c.phase, BT_CLASSIC_STOPPED);
        CHECK(!bt_classic_policy_busy(&c)); CHECK(!bt_classic_wake_busy(&c));
        CHECK(!bt_classic_quiescent(&c));    // the command is still in flight
        run(&c, &M, 3000);
        CHECK_EQ(M.logn, from);
        CHECK(!bt_classic_sniff_allowed(&c)); CHECK(!c.pending);
        check_clean(&c, "out, then dropped");
        // The next connection begins at "refused, confirmed", nothing owed.
        M.reply_delay = 0; M.drop_op = 0;
        CHECK(request(&c, "reconnect", NULL, NULL));
        CHECK(run_until_phase(&c, &M, BT_CLASSIC_READY, 3000));
        run(&c, &M, 100);
        CHECK(!c.sniff_wanted); CHECK(!bt_classic_sniff_allowed(&c));
        CHECK(!bt_classic_policy_busy(&c)); CHECK(!bt_classic_wake_busy(&c));
        CHECK_EQ(bt_classic_mode(&c), BT_CLASSIC_MODE_UNKNOWN);
        from = M.logn;
        bt_classic_policy(&c, false);
        run(&c, &M, 100);
        CHECK_EQ(M.logn, from);
        bt_classic_policy(&c, true);
        run(&c, &M, 100);
        CHECK_EQ(count_op(0x080d, from), 1); CHECK_EQ(u16(M.log[from].p), M.handle);
        CHECK(bt_classic_sniff_allowed(&c));
        STILL_UP(&c);
    }
    // Before READY neither is taken; a failed transport clears what was owed.
    bt_classic_t c;
    bt_classic_bond_t b = bond_for(PHONES, OLD_KEY);
    silent_link(&c, &b, "reconnect", 0x30);
    bt_classic_policy(&c, true); bt_classic_wake(&c);
    CHECK(!c.sniff_wanted); CHECK(!bt_classic_policy_busy(&c)); CHECK(!bt_classic_wake_busy(&c));
    ready_link(&c);
    M.control = false;
    bt_classic_policy(&c, true); bt_classic_wake(&c);
    bt_classic_tick(&c, M.now + 10, false, true, model_send, &M);
    CHECK_EQ(c.phase, BT_CLASSIC_FAILED);
    CHECK(!bt_classic_policy_busy(&c)); CHECK(!bt_classic_wake_busy(&c)); CHECK(!c.sniff_wanted);
}

// The value asked for changing while a write is out: the newer is written
// after it, unless the controller ends up holding it anyway.
static void test_sniff_changed_while_out(void)
{
    for (unsigned refused = 0; refused < 2; refused++) {
        bt_classic_t c;
        ready_link(&c);
        unsigned from = M.logn;
        M.reply_delay = 300; M.policy_reply = refused ? 0x12 : 0;
        bt_classic_policy(&c, true);
        step(&c, &M, 10);
        CHECK(c.policy_out);
        bt_classic_policy(&c, false);
        CHECK(c.policy_owed);                        // owed until the answer says otherwise
        run(&c, &M, 1500);
        // Granted: refused is written after it. Refused: the controller
        // already holds "refused", so nothing more.
        CHECK_EQ(count_op(0x080d, from), refused ? 1 : 2);
        if (!refused) CHECK_EQ(M.log[M.logn - 1].p[2], 0x01);
        CHECK(!bt_classic_sniff_allowed(&c)); CHECK(!bt_classic_policy_busy(&c));
        STILL_UP(&c);
    }
    // Changed and changed back while out: the write out is the one wanted.
    bt_classic_t c;
    ready_link(&c);
    unsigned from = M.logn;
    M.reply_delay = 300;
    bt_classic_policy(&c, true);
    step(&c, &M, 10);
    bt_classic_policy(&c, false);
    bt_classic_policy(&c, true);
    CHECK(!c.policy_owed);
    run(&c, &M, 1500);
    CHECK_EQ(count_op(0x080d, from), 1);
    CHECK(bt_classic_sniff_allowed(&c));
}

// The status text in a buffer too small for it stays a string.
static void test_status_capacity(void)
{
    bt_classic_t c;
    bt_classic_bond_t b = bond_for(PHONES, OLD_KEY);
    start(&c, &b);
    char small[16];
    size_t n = bt_classic_status(&c, small, sizeof small);
    CHECK_EQ(n, sizeof small - 1); CHECK_EQ(small[15], 0);
    CHECK_EQ(bt_classic_status(&c, small, 0), 0);
    char big[2048];
    n = bt_classic_status(&c, big, sizeof big);
    CHECK_EQ(n, strlen(big));
    CHECK_TEXT(&c, "state: idle\n");
    CHECK_TEXT(&c, "bond: 4c:87:5d:c3:b2:a1 key type 04 name SoundLink II (to save: no)");
    check_no_key(&c, OLD_KEY);
}

int main(int argc, char **argv)
{
    unsigned rounds = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 2000;
    test_bond();
    test_connect_session();
    test_debug_key();
    test_reconnect();
    test_reconnect_ignores_new_key();
    test_inbound(true);
    test_inbound(false);
    test_inbound_slow_peer();
    test_refusals();
    test_crossing();
    test_stale_bond();
    test_auth_failed();
    test_weak_key();
    test_page_timeout();
    test_no_credit();
    test_retryable();
    test_every_phase();
    test_cancel_race();
    test_accept_then_disconnect();
    test_page_scan();
    test_latched();
    test_refusal_queue();
    test_requests();
    test_failed_mid_link();
    test_rejections();
    test_encryption_lost();
    test_phase_waits();
    test_codec();
    test_malformed();
    test_claims();
    test_key_hygiene();
    test_more_rules();
    test_sniff_commands();
    test_sniff_turns();
    test_sniff_failures();
    test_sniff_mode_change();
    test_sniff_drop();
    test_sniff_changed_while_out();
    test_status_capacity();
    test_soup(rounds);
    if (failures) { printf("bt_classic: %d FAILED\n", failures); return 1; }
    printf("bt_classic: all passed (%u random rounds)\n", rounds);
    return 0;
}
