// test_bt_acl_host.c — kernel/src/driver/system/usb/bt_acl.c beside a model
// that is plainly right (a list of links and their counts, and the pool
// rules written out longhand), through random link changes, sends and
// completions in both pool layouts; then each rule by itself.
//
//   tools/test_bt_acl_host.sh [random rounds, default 2000]

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/system/usb/bt_acl.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; \
    printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { failures++; \
    printf("FAIL %s:%d: %s = %lld, want %s = %lld\n", __func__, __LINE__, #a, a_, #b, b_); } } while (0)

// ── The model ───────────────────────────────────────────────────────────────

typedef struct { int handle, out; bool le, live; } mlink_t;
typedef struct {
    bool known, shared;
    int classic_size, classic_total, le_size, le_total;
    mlink_t link[BT_ACL_LINKS];
    unsigned strays;
} model_t;

static mlink_t *m_find(model_t *m, int handle)
{
    for (int i = 0; i < BT_ACL_LINKS; i++)
        if (m->link[i].live && m->link[i].handle == handle) return &m->link[i];
    return NULL;
}

static int m_sum(const model_t *m, int kind)   // 0 LE, 1 Classic, 2 both
{
    int n = 0;
    for (int i = 0; i < BT_ACL_LINKS; i++)
        if (m->link[i].live && (kind == 2 || (kind == 0) == m->link[i].le)) n += m->link[i].out;
    return n;
}

static int m_reserve(const model_t *m)
{
    if (!m->known || !m->shared || m->classic_total == 0) return 0;
    int le = 0;
    for (int i = 0; i < BT_ACL_LINKS; i++) if (m->link[i].live && m->link[i].le) le++;
    return le > m->classic_total - 1 ? m->classic_total - 1 : le;
}

static bool m_may(model_t *m, int handle)
{
    mlink_t *l = m_find(m, handle);
    if (!m->known || !l) return false;
    if (!m->shared) return l->le ? m_sum(m, 0) < m->le_total : m_sum(m, 1) < m->classic_total;
    if (m_sum(m, 2) >= m->classic_total) return false;
    return l->le || m_sum(m, 1) + m_reserve(m) < m->classic_total;
}

static bt_acl_t A;
static model_t M;

static void compare(void)
{
    CHECK_EQ(A.known, M.known);
    if (M.known) CHECK_EQ(bt_acl_shared(&A), M.shared);
    CHECK_EQ(A.strays, M.strays);
    CHECK_EQ(bt_acl_reserved_for_le(&A), m_reserve(&M));
    if (M.known) {
        CHECK_EQ(bt_acl_packet_size(&A, false), M.classic_size);
        CHECK_EQ(bt_acl_packet_size(&A, true), M.shared ? M.classic_size : M.le_size);
        CHECK_EQ(bt_acl_in_use(&A, false), M.shared ? m_sum(&M, 2) : m_sum(&M, 1));
        CHECK_EQ(bt_acl_in_use(&A, true), M.shared ? m_sum(&M, 2) : m_sum(&M, 0));
    }
    for (int handle = 0; handle < 12; handle++) {
        mlink_t *l = m_find(&M, handle);
        CHECK_EQ(bt_acl_outstanding(&A, (uint16_t)handle), l ? l->out : 0);
        CHECK_EQ(bt_acl_may_send(&A, (uint16_t)handle), m_may(&M, handle));
    }
}

static void random_rounds(unsigned rounds)
{
    for (unsigned round = 0; round < rounds; round++) {
        srand(round + 1);
        bt_acl_init(&A);
        memset(&M, 0, sizeof(M));
        compare();
        for (int step = 0; step < 300; step++) {
            int handle = rand() % 12;
            switch (rand() % 10) {
            case 0: {
                // The sizes, once known, in one layout or the other; an LE
                // reply that is half zero is no LE pool.
                int cs = 27 + rand() % 1000, ct = rand() % 9, ls = rand() % 3 ? 27 + rand() % 200 : 0;
                int lt = rand() % 3 ? rand() % 6 : 0;
                bt_acl_sizes(&A, (uint16_t)cs, (uint16_t)ct, (uint16_t)ls, (uint16_t)lt);
                M.known = true; M.classic_size = cs; M.classic_total = ct;
                M.shared = !ls || !lt; M.le_size = M.shared ? 0 : ls; M.le_total = M.shared ? 0 : lt;
                break;
            }
            case 1: case 2: {
                bool le = rand() % 2, room = false, there = m_find(&M, handle) != NULL;
                for (int i = 0; i < BT_ACL_LINKS; i++) room |= !M.link[i].live;
                CHECK_EQ(bt_acl_link_up(&A, (uint16_t)handle, le), room && !there);
                if (room && !there)
                    for (int i = 0; i < BT_ACL_LINKS; i++) if (!M.link[i].live) {
                        M.link[i] = (mlink_t){ handle, 0, le, true }; break;
                    }
                break;
            }
            case 3: {
                mlink_t *l = m_find(&M, handle);
                bt_acl_link_down(&A, (uint16_t)handle);
                if (l) memset(l, 0, sizeof(*l));
                break;
            }
            case 4: case 5: case 6: {
                // A sender that obeys the gate, as every caller does.
                mlink_t *l = m_find(&M, handle);
                if (m_may(&M, handle)) { CHECK(bt_acl_sent(&A, (uint16_t)handle)); l->out++; }
                else if (!l) CHECK(!bt_acl_sent(&A, (uint16_t)handle));
                break;
            }
            default: {
                // A completion event of one to three entries: live links
                // and strangers, honest counts, zeros and overcounts.
                uint8_t e[3 + 4 * 3];
                int entries = 1 + rand() % 3, first_over = -1;
                e[0] = 0x13; e[1] = (uint8_t)(1 + 4 * entries); e[2] = (uint8_t)entries;
                for (int i = 0; i < entries; i++) {
                    int h = rand() % 12;
                    mlink_t *l = m_find(&M, h);
                    int count = l && l->out && rand() % 4 ? 1 + rand() % l->out : rand() % 3;
                    if (rand() % 37 == 0) count = (l ? l->out : 0) + 1 + rand() % 3;
                    // The flag bits above the handle are not part of it.
                    e[3 + 4 * i] = (uint8_t)h; e[4 + 4 * i] = (uint8_t)((h >> 8) | (rand() % 2 ? 0x20 : 0));
                    e[5 + 4 * i] = (uint8_t)count; e[6 + 4 * i] = (uint8_t)(count >> 8);
                    if (!l || !count) M.strays++;
                    else if (count > l->out) { l->out = 0; if (first_over < 0) first_over = h; }
                    else l->out -= count;
                }
                uint16_t named = 0xffff;
                bt_acl_verdict_t v = bt_acl_completed(&A, e, 3 + 4 * (size_t)entries, &named);
                CHECK_EQ(v, first_over < 0 ? BT_ACL_COUNTED : BT_ACL_OVERCOUNT);
                CHECK_EQ(named, first_over < 0 ? 0xffff : first_over);
                break;
            }
            }
            compare();
        }
    }
}

// The gate, held to its purpose: random traffic with fixed sizes never puts
// more in a pool than it holds, and in a shared pool Classic never takes
// what is kept for LE links that were up when it sent.
static void test_no_pool_is_overrun(void)
{
    for (unsigned round = 0; round < 400; round++) {
        srand(round + 7);
        bt_acl_init(&A);
        int ct = 1 + rand() % 8, lt = round % 2 ? 1 + rand() % 4 : 0;
        bt_acl_sizes(&A, 1021, (uint16_t)ct, lt ? 251 : 0, (uint16_t)lt);
        bool le[12] = { false };
        for (int step = 0; step < 400; step++) {
            uint16_t h = (uint16_t)(rand() % 12);
            int roll = rand() % 8;
            if (roll == 0) { bool kind = rand() % 2; if (bt_acl_link_up(&A, h, kind)) le[h] = kind; }
            else if (roll == 1) bt_acl_link_down(&A, h);
            else if (roll < 6) {
                if (bt_acl_may_send(&A, h)) {
                    bt_acl_sent(&A, h);
                    if (!le[h] && !lt) {
                        // Classic's own count, after this send, leaves the reserve.
                        unsigned classic = 0;
                        for (int i = 0; i < BT_ACL_LINKS; i++)
                            if (A.link[i].live && !A.link[i].le) classic += A.link[i].out;
                        CHECK(classic + bt_acl_reserved_for_le(&A) <= (unsigned)ct);
                    }
                }
            } else if (bt_acl_outstanding(&A, h)) {
                uint8_t e[7] = { 0x13, 5, 1, (uint8_t)h, 0, 1, 0 };
                uint16_t named;
                CHECK_EQ(bt_acl_completed(&A, e, 7, &named), BT_ACL_COUNTED);
            }
            if (lt) { CHECK(bt_acl_in_use(&A, false) <= ct); CHECK(bt_acl_in_use(&A, true) <= lt); }
            else CHECK(bt_acl_in_use(&A, false) <= ct);
        }
    }
}

// ── The named cases ─────────────────────────────────────────────────────────

static void test_before_the_sizes_are_known(void)
{
    bt_acl_init(&A);
    CHECK(bt_acl_link_up(&A, 3, true));
    CHECK(!bt_acl_may_send(&A, 3));
    CHECK_EQ(bt_acl_packet_size(&A, true), 0);
    CHECK_EQ(bt_acl_packet_size(&A, false), 0);
    CHECK_EQ(bt_acl_reserved_for_le(&A), 0);
}

static void test_two_pools(void)
{
    bt_acl_init(&A);
    bt_acl_sizes(&A, 1021, 2, 251, 1);
    CHECK(!bt_acl_shared(&A));
    CHECK_EQ(bt_acl_packet_size(&A, true), 251);
    CHECK_EQ(bt_acl_packet_size(&A, false), 1021);
    CHECK(bt_acl_link_up(&A, 1, true));
    CHECK(bt_acl_link_up(&A, 2, true));
    CHECK(bt_acl_link_up(&A, 9, false));
    CHECK_EQ(bt_acl_reserved_for_le(&A), 0);          // nothing is kept back from a pool LE cannot use
    CHECK(bt_acl_may_send(&A, 9)); bt_acl_sent(&A, 9);
    CHECK(bt_acl_may_send(&A, 9)); bt_acl_sent(&A, 9);
    CHECK(!bt_acl_may_send(&A, 9));                   // the Classic pool is full
    CHECK(bt_acl_may_send(&A, 1));                    // and LE's is its own
    bt_acl_sent(&A, 1);
    CHECK(!bt_acl_may_send(&A, 1));
    CHECK(!bt_acl_may_send(&A, 2));                   // one LE buffer, both LE links
    CHECK_EQ(bt_acl_in_use(&A, true), 1);
    CHECK_EQ(bt_acl_in_use(&A, false), 2);
}

static void test_one_pool_keeps_room_for_le(void)
{
    bt_acl_init(&A);
    bt_acl_sizes(&A, 1021, 4, 0, 0);
    CHECK(bt_acl_shared(&A));
    CHECK_EQ(bt_acl_packet_size(&A, true), 1021);     // LE's packets are the Classic size
    CHECK(bt_acl_link_up(&A, 9, false));
    CHECK_EQ(bt_acl_reserved_for_le(&A), 0);
    CHECK(bt_acl_link_up(&A, 1, true));
    CHECK(bt_acl_link_up(&A, 2, true));
    CHECK_EQ(bt_acl_reserved_for_le(&A), 2);
    bt_acl_sent(&A, 9); CHECK(bt_acl_may_send(&A, 9));
    bt_acl_sent(&A, 9); CHECK(!bt_acl_may_send(&A, 9));   // two of four are LE's
    CHECK(bt_acl_may_send(&A, 1)); bt_acl_sent(&A, 1);
    CHECK(bt_acl_may_send(&A, 2)); bt_acl_sent(&A, 2);
    CHECK(!bt_acl_may_send(&A, 1));                       // the pool itself is full
    CHECK_EQ(bt_acl_in_use(&A, true), 4);
    CHECK_EQ(bt_acl_in_use(&A, false), 4);                // one pool, whichever is asked
    // An LE link going gives its reserve, and its buffer, back.
    bt_acl_link_down(&A, 2);
    CHECK_EQ(bt_acl_reserved_for_le(&A), 1);
    CHECK(bt_acl_may_send(&A, 9));
    // More LE links than buffers: Classic is still left one.
    bt_acl_init(&A);
    bt_acl_sizes(&A, 1021, 2, 0, 0);
    CHECK(bt_acl_link_up(&A, 1, true)); CHECK(bt_acl_link_up(&A, 2, true)); CHECK(bt_acl_link_up(&A, 3, true));
    CHECK(bt_acl_link_up(&A, 9, false));
    CHECK_EQ(bt_acl_reserved_for_le(&A), 1);
    CHECK(bt_acl_may_send(&A, 9)); bt_acl_sent(&A, 9);
    CHECK(!bt_acl_may_send(&A, 9));
    // LE's own packets do not use up Classic's one: but the pool's end does.
    bt_acl_sent(&A, 1);
    CHECK(!bt_acl_may_send(&A, 2));
    // A pool of nothing lets nobody send, and reserves nothing.
    bt_acl_init(&A);
    bt_acl_sizes(&A, 1021, 0, 0, 0);
    CHECK(bt_acl_link_up(&A, 1, true)); CHECK(bt_acl_link_up(&A, 9, false));
    CHECK_EQ(bt_acl_reserved_for_le(&A), 0);
    CHECK(!bt_acl_may_send(&A, 1)); CHECK(!bt_acl_may_send(&A, 9));
    // Half an LE reply is a shared pool: a length with no count, a count with no length.
    bt_acl_sizes(&A, 1021, 4, 251, 0); CHECK(bt_acl_shared(&A)); CHECK_EQ(A.le_size, 0);
    bt_acl_sizes(&A, 1021, 4, 0, 3);   CHECK(bt_acl_shared(&A)); CHECK_EQ(A.le_total, 0);
}

static void test_links(void)
{
    bt_acl_init(&A);
    bt_acl_sizes(&A, 1021, 8, 0, 0);
    CHECK(bt_acl_link_up(&A, 0, true));               // handle zero is a handle
    CHECK(!bt_acl_link_up(&A, 0, false));             // already there
    CHECK(bt_acl_link_up(&A, 1, true)); CHECK(bt_acl_link_up(&A, 2, false)); CHECK(bt_acl_link_up(&A, 3, false));
    CHECK(!bt_acl_link_up(&A, 4, true));              // the table is full
    CHECK(!bt_acl_sent(&A, 4));
    CHECK(!bt_acl_may_send(&A, 4));
    bt_acl_sent(&A, 0); bt_acl_sent(&A, 0);
    CHECK_EQ(bt_acl_outstanding(&A, 0), 2);
    bt_acl_link_down(&A, 0);                          // its packets went with it
    CHECK_EQ(bt_acl_outstanding(&A, 0), 0);
    CHECK_EQ(bt_acl_in_use(&A, false), 0);
    bt_acl_link_down(&A, 0);                          // twice is nothing
    CHECK(bt_acl_link_up(&A, 4, true));               // and its place is free
    CHECK_EQ(bt_acl_outstanding(&A, 4), 0);
}

static void test_completions(void)
{
    uint16_t named = 0x1234;
    bt_acl_init(&A);
    bt_acl_sizes(&A, 1021, 8, 0, 0);
    CHECK(bt_acl_link_up(&A, 0x040, true)); CHECK(bt_acl_link_up(&A, 0x001, false));
    for (int i = 0; i < 3; i++) { bt_acl_sent(&A, 0x040); bt_acl_sent(&A, 0x001); }
    // Two entries, one event; the handle's upper four bits are not handle.
    uint8_t two[11] = { 0x13, 9, 2, 0x40, 0x20, 2, 0, 0x01, 0x00, 1, 0 };
    CHECK_EQ(bt_acl_completed(&A, two, sizeof(two), &named), BT_ACL_COUNTED);
    CHECK_EQ(named, 0x1234);                          // untouched when nothing is wrong
    CHECK_EQ(bt_acl_outstanding(&A, 0x040), 1);
    CHECK_EQ(bt_acl_outstanding(&A, 0x001), 2);
    // A stranger and a zero count: ignored, counted.
    uint8_t stray[11] = { 0x13, 9, 2, 0x99, 0x00, 1, 0, 0x01, 0x00, 0, 0 };
    CHECK_EQ(bt_acl_completed(&A, stray, sizeof(stray), &named), BT_ACL_COUNTED);
    CHECK_EQ(A.strays, 2);
    CHECK_EQ(bt_acl_outstanding(&A, 0x001), 2);
    // Exactly what is out is honest; one more is not, and names the link.
    uint8_t exact[7] = { 0x13, 5, 1, 0x01, 0x00, 2, 0 };
    CHECK_EQ(bt_acl_completed(&A, exact, 7, &named), BT_ACL_COUNTED);
    CHECK_EQ(bt_acl_outstanding(&A, 0x001), 0);
    uint8_t over[11] = { 0x13, 9, 2, 0x40, 0x00, 2, 0, 0x40, 0x00, 1, 0 };
    CHECK_EQ(bt_acl_completed(&A, over, sizeof(over), &named), BT_ACL_OVERCOUNT);
    CHECK_EQ(named, 0x040);
    CHECK_EQ(bt_acl_outstanding(&A, 0x040), 0);       // zeroed, not wrapped
    CHECK_EQ(A.strays, 2);                            // the second entry is one more than nothing: an overcount too, not a stray
    // A handle that needs its second byte, beside one that shares its first.
    CHECK(bt_acl_link_up(&A, 0x345, false)); CHECK(bt_acl_link_up(&A, 0x045, true));
    bt_acl_sent(&A, 0x345); bt_acl_sent(&A, 0x045);
    uint8_t high[7] = { 0x13, 5, 1, 0x45, 0x03, 1, 0 };
    CHECK_EQ(bt_acl_completed(&A, high, 7, &named), BT_ACL_COUNTED);
    CHECK_EQ(bt_acl_outstanding(&A, 0x345), 0);
    CHECK_EQ(bt_acl_outstanding(&A, 0x045), 1);
    bt_acl_link_down(&A, 0x345); bt_acl_link_down(&A, 0x045);
    // A 16-bit count.
    for (int i = 0; i < 300; i++) bt_acl_sent(&A, 0x001);
    uint8_t wide[7] = { 0x13, 5, 1, 0x01, 0x00, 0x2c, 0x01 };
    CHECK_EQ(bt_acl_completed(&A, wide, 7, &named), BT_ACL_COUNTED);
    CHECK_EQ(bt_acl_outstanding(&A, 0x001), 0);
    // Not this event, or not whole: nothing is counted.
    bt_acl_sent(&A, 0x001);
    uint8_t other[7] = { 0x0e, 5, 1, 0x01, 0x00, 1, 0 };
    CHECK_EQ(bt_acl_completed(&A, other, 7, &named), BT_ACL_MALFORMED);
    uint8_t lying[7] = { 0x13, 5, 2, 0x01, 0x00, 1, 0 };          // says two entries, carries one
    CHECK_EQ(bt_acl_completed(&A, lying, 7, &named), BT_ACL_MALFORMED);
    uint8_t cut[7] = { 0x13, 6, 1, 0x01, 0x00, 1, 0 };            // its own length is wrong
    CHECK_EQ(bt_acl_completed(&A, cut, 7, &named), BT_ACL_MALFORMED);
    CHECK_EQ(bt_acl_completed(&A, exact, 2, &named), BT_ACL_MALFORMED);
    uint8_t padded[11] = { 0x13, 9, 1, 0x01, 0x00, 1, 0, 0, 0, 0, 0 };   // one entry, and bytes after it
    CHECK_EQ(bt_acl_completed(&A, padded, sizeof(padded), &named), BT_ACL_MALFORMED);
    uint8_t none[3] = { 0x13, 1, 0 };                             // no entries is well formed
    CHECK_EQ(bt_acl_completed(&A, none, 3, &named), BT_ACL_COUNTED);
    CHECK_EQ(bt_acl_outstanding(&A, 0x001), 1);
}

int main(int argc, char **argv)
{
    unsigned rounds = argc > 1 ? (unsigned)atoi(argv[1]) : 2000;
    test_before_the_sizes_are_known();
    test_two_pools();
    test_one_pool_keeps_room_for_le();
    test_links();
    test_completions();
    test_no_pool_is_overrun();
    random_rounds(rounds);
    if (failures) { printf("test_bt_acl_host: %d FAILURE(S)\n", failures); return 1; }
    printf("test_bt_acl_host: all passed (%u random rounds)\n", rounds);
    return 0;
}
