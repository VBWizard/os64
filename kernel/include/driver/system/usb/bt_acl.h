#ifndef OS64_BT_ACL_H
#define OS64_BT_ACL_H

// bt_acl.h — the controller's ACL data buffers, counted in one place
// (docs/design/pending/AUDIO_CLASSIC.md § T1).
//
// A controller says how many ACL packets it can hold (Read Buffer Size, LE
// Read Buffer Size) and gives each buffer back with Number Of Completed
// Packets when the packet has gone over the air. A host that sends more
// than it was told loses packets. With one link sending one packet at a
// time nothing needs counting; with a Classic link streaming beside two LE
// peers, something does, and it has to be the controller's count, not any
// one link's: LE and Classic may draw on ONE pool.
//
// PURE: no kernel headers, no I/O, no clock. Owned by whoever holds USB's
// serializing flag (xhci.c).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BT_ACL_LINKS 4

typedef struct {
    uint16_t handle;
    uint16_t out;      // packets sent on this link and not yet completed
    bool le, live;
} bt_acl_link_t;

typedef struct {
    // What the controller said; all zero, and `known` clear, until it
    // has. le_total of zero: LE has no pool of its own and draws on the
    // Classic one.
    bool known;
    uint16_t classic_size, classic_total;
    uint16_t le_size, le_total;
    bt_acl_link_t link[BT_ACL_LINKS];
    // Completions that named no link here, or carried a count of zero:
    // ignored, and counted for the status file.
    uint32_t strays;
} bt_acl_t;

void bt_acl_init(bt_acl_t *a);
// The two replies: Read Buffer Size's ACL packet length and count, LE Read
// Buffer Size's. An LE length or count of zero means one shared pool.
void bt_acl_sizes(bt_acl_t *a, uint16_t classic_size, uint16_t classic_total,
                  uint16_t le_size, uint16_t le_total);
bool bt_acl_shared(const bt_acl_t *a);

// A link came up, or went. False: no room in the table, or the handle is
// already there. A link that goes takes its outstanding packets with it:
// the controller drops them and their buffers are free (Vol 4 Part E
// § 4.3).
bool bt_acl_link_up(bt_acl_t *a, uint16_t handle, bool le);
void bt_acl_link_down(bt_acl_t *a, uint16_t handle);

// The largest ACL payload a link of this kind may send, 0 before the sizes
// are known.
uint16_t bt_acl_packet_size(const bt_acl_t *a, bool le);
// Is there a buffer this link may use now. In a shared pool a Classic link
// may not take the last buffers, one for each live LE link (and is always
// left one): a keystroke's acknowledgement does not wait behind audio.
bool bt_acl_may_send(const bt_acl_t *a, uint16_t handle);
// One packet went to the controller. False, and nothing counted, if the
// link is not in the table.
bool bt_acl_sent(bt_acl_t *a, uint16_t handle);

// Number Of Completed Packets (event 0x13), the whole event.
typedef enum {
    BT_ACL_COUNTED,      // every entry was a link's, within what it had out
    BT_ACL_MALFORMED,    // not a well-formed event: nothing was counted
    BT_ACL_OVERCOUNT     // a link was given back more than it had out: *link is its handle,
                         // its count is zeroed, and its session cannot be trusted
} bt_acl_verdict_t;
bt_acl_verdict_t bt_acl_completed(bt_acl_t *a, const uint8_t *event, size_t bytes, uint16_t *link);

// For the status file.
uint16_t bt_acl_outstanding(const bt_acl_t *a, uint16_t handle);
uint16_t bt_acl_in_use(const bt_acl_t *a, bool le_pool);   // le_pool is ignored when the pool is shared
uint16_t bt_acl_reserved_for_le(const bt_acl_t *a);

#endif
