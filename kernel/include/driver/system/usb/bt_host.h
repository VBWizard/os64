#ifndef OS64_BT_HOST_H
#define OS64_BT_HOST_H
#include "driver/system/usb/bt_manager.h"
#include "driver/system/usb/bt_acl.h"
#include "driver/system/usb/bt_classic.h"
#include "driver/system/usb/bt_l2cap.h"
#include "driver/system/usb/bt_sdp.h"

#define BT_HOST_PEERS BT_BOND_SLOTS
// The transport's ACL send slots (xhci.c): each holds one packet until its
// USB transfer completes, and they complete in the order they were filled.
// The Classic link always leaves one free for the LE peers.
#define BT_HOST_TX_SLOTS 4
// The Classic link is slot 2 to a person (docs/design/pending/AUDIO_CLASSIC.md
// § L1): the slot after the LE peers'. BT_HOST_PEERS also sizes the LE bond
// files, keyboards and pointers, so the Classic slot is not one of them.
#define BT_HOST_CLASSIC_SLOT BT_HOST_PEERS
// Who holds the one command slot (`owner`): nobody, the scan, LE peer i
// (BT_HOST_OWNER_PEER0 + i), or the Classic link. Every reader of `owner`
// is bounded for each kind.
enum {
    BT_HOST_OWNER_NONE = 0,
    BT_HOST_OWNER_SCAN = 1,
    BT_HOST_OWNER_PEER0 = 2,
    BT_HOST_OWNER_CLASSIC = 2 + BT_HOST_PEERS
};
// Controller commands and ACL credits are shared; pairing, encryption, ATT,
// bond policy and input state belong to a peer. USB's serializing flag
// (xhci.c) orders access.
typedef struct {
    bt_le_t le;
    bt_manager_t manager;
} bt_host_peer_t;
typedef struct {
    bt_scan_t scan;
    bt_host_peer_t peers[BT_HOST_PEERS];
    bool initialized, failed, command_done, tx_used;
    uint8_t owner, credits, next_peer, poll_peer;
    // The Classic link (bt_classic.h). Initialised when the start-up round
    // reaches DONE and not before, so no reset can follow and clear a page
    // scan the link believes is on; ticked and offered events only while
    // `classic_available`: the controller has BR/EDR, the round ran its
    // Classic setup, and the controller refused none of it. Otherwise
    // `classic_unavailable` says why, as a string literal. The first
    // refused setup command is copied here from the round, as the buffer
    // sizes are: the next scan clears the round's copy.
    bt_classic_t classic;
    bool classic_available;
    const char *classic_unavailable;
    bool classic_setup_refused;
    uint16_t classic_refused_opcode;
    uint8_t classic_refused_status;
    // The handle the Classic link is counted under in `acl`, PLUS ONE, as
    // acl_link[].
    uint16_t acl_classic;
    // The Classic link's services (AUDIO_CLASSIC.md part S). `l2cap` is
    // brought up when the link connects, admitting SDP alone, and reset
    // when it goes; every ACL packet on the link's handle is its.
    // `l2cap_encrypted` is what it was last told of the link's encryption.
    bt_l2cap_t l2cap;
    bool l2cap_encrypted;
    // PDUs L2CAP handed over that one of the controller's Classic ACL
    // packets cannot carry: dropped, never cut (bt_host.c host_l2cap_send).
    uint32_t l2cap_unsendable;
    // One SDP query a connection, made by us once the link is READY: the
    // channel we opened to the headphones' SDP PLUS ONE (0: none being
    // followed, so a host made by zeroing is right) and its CID, so a
    // channel index reused by the peer is not taken for it; the client;
    // the request it wrote, held until L2CAP takes it; and, when the query
    // could not be made or its channel went first, why (a string literal)
    // and the L2CAP result that refused it (0: none).
    bool sdp_asked;
    uint8_t sdp_channel;
    uint16_t sdp_cid;
    bt_sdp_client_t sdp;
    uint8_t sdp_request[40];
    uint8_t sdp_request_bytes;
    const char *sdp_not_made;
    uint16_t sdp_refused;
    // The headphones asking US (a channel they opened to PSM 1): an answer
    // L2CAP's one payload slot could not take yet, held here and offered
    // again every tick, with the channel and CID it is for. A request that
    // arrives while one is held is dropped and counted. Counted since
    // start-up, not per connection.
    uint8_t sdp_answer[BT_L2CAP_MTU];
    uint16_t sdp_answer_bytes;
    uint8_t sdp_answer_channel;
    uint16_t sdp_answer_cid;
    uint32_t sdp_answered, sdp_dropped;
    // What the last query found, kept across a disconnection for A.3 and
    // the status file, and whether it is from the connection that is up;
    // with its status line. `sdp_line` is the line the last query ENDED
    // with, whatever the end, and `sdp_ended` counts the ends, so the
    // transport can log each once. `l2cap_refusals` counts the channels we
    // asked for that the peer refused or never answered.
    bt_sdp_sink_t sdp_result;
    bool sdp_result_valid, sdp_result_current;
    char sdp_result_line[128], sdp_line[128];
    uint32_t sdp_ended, l2cap_refusals;
    // Reconnection (AUDIO_CLASSIC.md § L6): with a bond and `classic_auto`,
    // page the headphones once when the bond is loaded, and again after a
    // drop they may come back from or a page they did not answer, at
    // classic_page_at (0: no page owed), the wait doubling from 5 s to
    // 60 s and starting again at READY. Any other end waits for them to
    // call (page scan is on) or for the user.
    bool classic_auto;
    uint64_t classic_page_at;
    uint32_t classic_backoff_ms;
    // Why the Classic bond file could not be read or written (a string
    // literal, from whoever keeps the file); NULL when it could.
    const char *classic_storage;
    // Why the last command was refused, when the refusal has words (a
    // string literal); NULL when it was taken or has none.
    const char *refused_why;
    // The controller's ACL buffers (bt_acl.h): sized from the start-up
    // round, a link for each connected LE peer and for the Classic link,
    // every packet sent and completed counted. The LE peers still send one
    // packet at a time between them by their own rule (tx_used); the
    // Classic link's L2CAP sends only while the count gives it a buffer.
    // acl_link[i] is the handle peer i is counted under PLUS ONE, zero when
    // it has none, so a host made by zeroing is right.
    bt_acl_t acl;
    uint16_t acl_link[BT_HOST_PEERS];
    // Number Of Completed Packets events the count could not take: not
    // well formed, or giving a link back more than it had out.
    uint32_t acl_bad_completions;
    // Who sent each packet still in the transport's send slots, oldest
    // first: LE peer i (i), or the Classic link (BT_HOST_PEERS). The
    // transport reports only how many are in flight; since they complete in
    // order, the oldest are the ones that finished. Slots in flight that
    // this host did not fill (`tx_unknown`) are counted against every LE
    // peer, as the whole endpoint's business was before there were two
    // kinds of sender.
    uint8_t tx_from[BT_HOST_TX_SLOTS];
    uint8_t tx_head, tx_count, tx_unknown;
    uint8_t duplicate_slot; // Last refused peer's owning slot plus one; zero means none.
    uint64_t init_retry_at;
    // Start-up rounds that ended FAILED: a first error, and then a cleanup
    // that did not complete either (on the P5, the controller's reset
    // unanswered and the cleanup's reset unanswered too). The round is run
    // again, after the same pause a cleanly stopped one gets, until
    // BT_HOST_INIT_ROUNDS have failed: one bad start is not the boot.
    uint8_t init_failed_rounds;
    // What the last such round died of. The next round rebuilds the scan,
    // so it is kept here for the status file.
    const char *round_error, *round_timeout;
    uint16_t round_opcode;
    uint8_t round_status;
    // One ACL packet being assembled, for any link: large enough for a
    // Classic packet the size of the L2CAP PDU we state the MTU for, plus
    // its header (bt_host.c asserts it), or a full-size Classic packet
    // would read as "not a header" and resynchronise the assembler.
    uint8_t wire[BT_LE_ACL_MAX+4];
    size_t wire_used, wire_need;
    // ACL data being discarded until its transfer ends, and how often the
    // assembler was found out of step and emptied (bt_host_receive).
    bool wire_skip;
    uint32_t wire_resyncs;
} bt_host_t;
#define BT_HOST_INIT_ROUNDS 3
typedef void (*bt_host_report_t)(void *, unsigned, const bt_le_input_t *);
void bt_host_init(bt_host_t *, uint64_t);
void bt_host_event(bt_host_t *, const uint8_t *, size_t);
// `max_packet`: the bulk endpoint's maximum packet size when the bytes are
// ONE USB packet, 0 for an arbitrary slice of the stream. As bt_hci_feed:
// a packet shorter than the maximum ends a transfer and an ACL packet does
// not continue past the end of one, so the packet form can tell when its
// assembler is out of step, and empties it. A header whose length cannot
// be real is then a transfer to skip, where the stream form can only call
// the host failed.
void bt_host_receive(bt_host_t *, const uint8_t *, size_t, size_t max_packet,
                     bt_host_report_t, void *);
// One step. `usb_done`: no command's control transfer is in flight.
// `tx_in_flight`: how many of the transport's BT_HOST_TX_SLOTS send slots
// hold a packet whose USB transfer has not completed. `failed`: the
// transport is gone. ACL packets leave through the `bt_le_acl_send_t`
// callback, whole, never more than one slot's worth, and only into a free
// slot: the LE peers first, then the Classic link's L2CAP, which waits
// whenever its packet could make an LE peer wait.
void bt_host_tick(bt_host_t *, uint64_t now, bool usb_done, unsigned tx_in_flight, bool failed,
                  bt_scan_send_t, bt_le_acl_send_t, bt_host_report_t, void *);
void bt_host_policy(bt_host_t *, uint64_t);
bool bt_host_scan(bt_host_t *, uint64_t);
// `scan classic`: a Classic inquiry, then the LE scan. Refused as
// bt_host_scan is, and while the Classic link is neither quiescent nor
// READY (an inquiry beside a page or a pairing would starve it).
bool bt_host_scan_classic(bt_host_t *, uint64_t);
// The Classic bond file has been read (bond->valid false: there is none).
// A bond made as another adapter is taken as none. Given to the link, and
// a valid one is paged once. False, and nothing done, unless the host is
// initialised with its Classic slot available.
bool bt_host_classic_bond(bt_host_t *, const bt_classic_bond_t *, uint64_t);
// "slot N COMMAND" (slot 0 when no slot is named). Slot 2 is the Classic
// link: COMMAND goes to bt_classic_request.
bool bt_host_command(bt_host_t *, const char *, size_t, uint64_t);
size_t bt_host_status(const bt_host_t *, char *, size_t);
#endif
