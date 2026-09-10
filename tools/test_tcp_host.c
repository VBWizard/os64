// Protocol event tests against tcp.c. Host stubs exercise state and bytes;
// they do not establish scheduler, IRQ or DMA correctness.
static net_device_t dev = {1500};
static tcp_conn_t* init(void)
{
	tcp_conn_t* c = calloc(1, sizeof(*c));
	c->state = TCP_ESTABLISHED; c->dev = &dev; c->peer_ip = 0x0a000202;
	c->local_port = 50000; c->peer_port = 7200; c->rcv_nxt = 5000;
	c->snd_una = c->snd_nxt = c->snd_max = c->recover = 1000;
	c->snd_wl1 = 5000; c->snd_wl2 = 1000;
	c->snd_wnd = 65535; c->snd_mss = 1000; c->cwnd = 10000; c->ssthresh = 65536;
	c->rto = c->rto_ticks = 20;
	c->rcv_buf = calloc(1, TCP_RCV_BUF); c->snd_buf = calloc(1, TCP_SND_BUF);
	kTcpConnList = c; kTicksSinceStart = 100; npackets = 0;
	disposition = IPV4_TX_SENT; dev.mtu = 1500;
	cls.currentThread = &test_thread; interrupted = false; sleeps = 0; sleep_hook = NULL;
	s_tomb_head = s_tomb_tail = NULL; s_tomb_count = 0;
	memset(&kTcpStats, 0, sizeof(kTcpStats));
	return c;
}

static void cleanup(tcp_conn_t* c)
{
	free(c->snd_buf); free(c->rcv_buf); free(c); kTcpConnList = NULL;
}

static void incoming(tcp_conn_t* c, uint32_t seq, uint32_t ack, uint16_t win, uint8_t flags)
{
	uint8_t b[20] = {0}, pseudo[12] = {0};
	net_write16(b, c->peer_port); net_write16(b + 2, c->local_port);
	net_write32(b + 4, seq); net_write32(b + 8, ack);
	b[12] = 0x50; b[13] = flags; net_write16(b + 14, win);
	net_write32(pseudo, c->peer_ip); net_write32(pseudo + 4, kNetIPv4Address);
	pseudo[9] = 6; net_write16(pseudo + 10, 20);
	net_write16(b + 16, net_checksum_fold(net_checksum_add(net_checksum_add(0, pseudo, 12), b, 20)));
	tcp_input(&dev, c->peer_ip, kNetIPv4Address, b, 20);
}

static void ack(tcp_conn_t* c, uint32_t n, uint16_t win)
{
	incoming(c, c->rcv_nxt, n, win, TCP_ACK);
}

static void deadline(tcp_conn_t* c)
{
	assert(c->send_timer != TCP_TIMER_IDLE);
	kTicksSinceStart = c->send_deadline;
	tcp_poll();
}

static void test_submissions(void)
{
	for (unsigned fate = IPV4_TX_SENT; fate <= IPV4_TX_DROPPED; fate++)
		for (unsigned kind = 0; kind < 3; kind++)
		{
			tcp_conn_t* c = init(); disposition = fate;
			if (kind == 0) { c->state = TCP_SYN_SENT; tcp_send_syn(c); }
			if (kind == 1) { c->snd_count = 1000; tcp_output(c, 0); }
			if (kind == 2) tcp_conn_close(c);
			unsigned span = kind == 1 ? 1000 : 1;
			assert(c->snd_max == 1000 + span && c->snd_nxt == c->snd_max);
			assert(c->send_timer == TCP_TIMER_RETRANSMIT && npackets == 1);
			kTicksSinceStart++; tcp_poll(); assert(npackets == 1);
			deadline(c);
			assert(npackets == 2 && c->retries == 1 && tcp_unacked(c));
			assert(packets[1].seq == 1000 && packets[1].len == (kind == 1 ? 1000 : 0));
			assert(c->send_timer == TCP_TIMER_RETRANSMIT);
			cleanup(c);
		}
	// Construction failures stop immediately; no impossible-packet loop.
	for (unsigned kind = 0; kind < 3; kind++)
	{
		tcp_conn_t* c = init(); dev.mtu = 39;
		if (kind == 0) { c->state = TCP_SYN_SENT; tcp_send_syn(c); }
		if (kind == 1) { c->snd_count = 1000; tcp_output(c, 0); }
		if (kind == 2) tcp_conn_close(c);
		assert(c->state == TCP_CLOSED && c->send_timer == TCP_TIMER_IDLE);
		assert(c->snd_max == c->snd_una); cleanup(c);
	}
}

static void test_handshake_reset(void)
{
    tcp_conn_t* c = init(); c->state = TCP_SYN_SENT;
    tcp_send_syn(c); deadline(c);
    incoming(c, 9000, c->snd_max, 4096, TCP_SYN | TCP_ACK);
    assert(c->state == TCP_ESTABLISHED && !c->recover_valid);
    assert(c->send_timer == TCP_TIMER_IDLE && c->snd_mss == 536);
    assert(c->snd_wnd == 4096 && c->retries == 0 && !c->rtt_timing);
    c->snd_count = 1000; tcp_output(c, 0);
    incoming(c, c->rcv_nxt, c->snd_una, 4096, TCP_RST | TCP_ACK);
    assert(c->state == TCP_CLOSED && c->send_timer == TCP_TIMER_IDLE && !c->rtt_timing);
    cleanup(c);
}

static void test_lifetime(void)
{
	for (unsigned zero = 0; zero < 2; zero++)
		for (unsigned data = 0; data < 2; data++)
		{
			tcp_conn_t* c = init(); c->snd_count = data ? 1000 : 0;
			c->snd_wnd = zero ? 0 : 65535; disposition = IPV4_TX_DROPPED;
			tcp_conn_close(c); uint64_t end = c->detached_deadline;
			for (unsigned i = 0; i < 7; i++) { kTicksSinceStart++; tcp_poll(); }
			assert(c->state != TCP_CLOSED && !c->stripped);
			while (c->state != TCP_CLOSED) { kTicksSinceStart++; tcp_poll(); }
			assert(kTicksSinceStart <= end && c->stripped);
			cleanup(c);
		}
	// An owned zero-window connection can wait beyond the orphan bound.
	tcp_conn_t* c = init(); c->snd_count = 1000; c->snd_wnd = 0; tcp_output(c, 0);
	for (unsigned i = 0; i < 12; i++) deadline(c);
	assert(kTicksSinceStart > 100 + TCP_DETACHED_IDLE_TICKS && !c->reset && c->retries == 0);
	// Reopening and ACK progress before detached expiry retain queued data.
	tcp_conn_close(c); uint64_t old_end = c->detached_deadline;
	kTicksSinceStart = old_end - 1; ack(c, c->snd_una, 65535);
	ack(c, c->snd_una + 1000, 65535);
	assert(c->detached_deadline > old_end && c->snd_count == 0);
	ack(c, c->snd_max, 65535); assert(c->state == TCP_FIN_WAIT_2);
	kTicksSinceStart = c->detached_deadline; tcp_poll();
	assert(c->state == TCP_CLOSED && c->stripped); cleanup(c);
}

static void test_rto(void)
{
	tcp_conn_t* c = init(); c->snd_count = 10000; tcp_output(c, 0);
	disposition = IPV4_TX_DROPPED; deadline(c);
	assert(c->snd_max == 11000 && c->snd_nxt == 2000 && tcp_bytes_in_flight(c) == 10000);
	assert(c->send_timer == TCP_TIMER_RETRANSMIT && c->ssthresh == 5000);
	deadline(c); assert(c->ssthresh == 5000 && c->retries == 2);
	// Queue becomes usable: ACK of the resend clocks out the next range.
	disposition = IPV4_TX_SENT; ack(c, 2000, 65535);
	assert(c->retries == 0 && c->snd_nxt > 2000 && c->snd_max == 11000);
	ack(c, 11000, 65535);
	assert(c->snd_count == 0 && c->send_timer == TCP_TIMER_IDLE && !c->recover_valid);
	cleanup(c);
}

static void test_congestion(void)
{
	tcp_conn_t* c = init(); c->snd_count = 20000; tcp_output(c, 0);
	ack(c, 1000, 65535); assert(c->limited_bytes == 1000);
	ack(c, 1000, 65535); assert(c->limited_bytes == 2000 && c->snd_max == 13000);
	ack(c, 1000, 65535);
	assert(c->fast_recovery && c->ssthresh == 5000 && c->cwnd == 8000);
	assert(kTcpStats.fast_retransmits == 1);
	ack(c, 1001, 65535); assert(c->cwnd == 7999 && c->fast_recovery);
	ack(c, 2001, 65535); assert(c->cwnd == 7999 && c->fast_recovery);
	for (unsigned i = 0; i < 300; i++) ack(c, 2001, 65535);
	assert(c->dup_acks == 255 && c->fast_recovery && c->cwnd <= TCP_SND_BUF);
	uint32_t recover = c->recover; ack(c, recover, 65535);
	assert(!c->fast_recovery && !c->recover_valid && c->cwnd == 5000);
	cleanup(c);
	// A fully retired recovery cannot suppress a new loss after 2 GiB.
	c = init(); c->snd_una = c->snd_nxt = c->snd_max = 1000u + 0x80000010u;
	c->snd_count = 10000; tcp_output(c, 0);
	for (unsigned i = 0; i < 3; i++) ack(c, c->snd_una, 65535);
	assert(c->fast_recovery && kTcpStats.fast_retransmits == 1); cleanup(c);
	// Restart after actual sends and complete acknowledgment, then idle.
	c = init(); c->snd_count = 1000; tcp_output(c, 0); ack(c, 2000, 65535);
	c->cwnd = 65536; kTicksSinceStart += 6000; c->snd_count = 65535; tcp_output(c, 0);
	assert(c->snd_max - c->snd_una == 10000 && c->cwnd == 10000); cleanup(c);
}

static void test_persist(void)
{
	for (unsigned fate = IPV4_TX_SENT; fate <= IPV4_TX_DROPPED; fate++)
		for (unsigned reply = 0; reply < 2; reply++)
			for (unsigned fin = 0; fin < 2; fin++)
			{
				tcp_conn_t* c = init(); c->snd_wnd = 0; disposition = fate;
				if (fin) tcp_conn_close(c); else { c->snd_count = 1000; tcp_output(c, 0); }
				assert(c->send_timer == TCP_TIMER_PERSIST && !tcp_unacked(c));
				uint64_t times[] = {200, 400, 800, 1600};
				for (unsigned i = 0; i < 4; i++)
				{
					deadline(c); assert(packets[npackets - 1].tick == times[i]);
					assert(c->probe_pending && c->snd_max == 1000 && c->snd_nxt == 1000);
					assert(c->cwnd == 10000 && c->retries == 0);
					uint64_t next = c->send_deadline;
					if (reply) ack(c, 1000, 0);
					assert(c->send_deadline == next);
				}
				ack(c, 1002, 65535); assert(c->snd_una == 1000); // never offered
				disposition = IPV4_TX_SENT; ack(c, 1001, 65535);
				assert(c->snd_una == 1001 && !c->probe_pending);
				if (fin) assert(c->state == TCP_FIN_WAIT_2 && c->send_timer == TCP_TIMER_IDLE);
				else { ack(c, c->snd_max, 65535); assert(!c->snd_count); }
				cleanup(c);
			}
	// Shrinking a window with a flight outstanding enters persist directly.
	tcp_conn_t* c = init(); c->snd_count = 10000; tcp_output(c, 0);
	ack(c, 1000, 0); assert(c->send_timer == TCP_TIMER_PERSIST && tcp_unacked(c));
	deadline(c); assert(c->retries == 0 && c->snd_max == 11000);
	ack(c, 11000, 65535); assert(!c->snd_count && c->send_timer == TCP_TIMER_IDLE);
	cleanup(c);
}

static void test_wrap_fin_window(void)
{
	// The ring wraps under the three segments: the head sits 536 bytes
	// before the end, sized from TCP_SND_BUF so the wrap survives the ring
	// growing.
	tcp_conn_t* c = init(); c->snd_una = c->snd_nxt = c->snd_max = 0xfffffff0u;
	c->snd_head = TCP_SND_BUF - 536; c->snd_count = 3000;
	for (unsigned i = 0; i < c->snd_count; i++) c->snd_buf[(c->snd_head + i) % TCP_SND_BUF] = (uint8_t)i;
	tcp_conn_close(c); assert(c->snd_fin_sent && npackets == 3);
	for (unsigned i = 0; i < 3; i++)
		for (unsigned j = 0; j < packets[i].len; j++) assert(packets[i].payload[j] == (uint8_t)(i * 1000 + j));
	uint32_t end = c->snd_max; deadline(c);
	assert(seq_lt(c->snd_nxt, end) && c->snd_fin_sent);
	ack(c, end, 65535);
	assert(!c->snd_count && c->state == TCP_FIN_WAIT_2 && c->snd_head == (TCP_SND_BUF - 536 + 3000) % TCP_SND_BUF);
	incoming(c, c->rcv_nxt, end, 65535, TCP_FIN | TCP_ACK); tcp_poll();
	assert(c->state == TCP_TIME_WAIT && c->stripped); cleanup(c);
	// FIN waits for its own receive-window sequence unit.
	c = init(); c->snd_wnd = 1000; c->snd_count = 1000; tcp_conn_close(c);
	assert(!c->snd_fin_sent && c->snd_max == 2000);
	ack(c, 2000, 1); assert(c->snd_fin_sent && c->snd_max == 2001); cleanup(c);
	// A stale sequence must not overwrite a newer window with the same ACK.
	c = init(); incoming(c, 5001, 1000, 0, TCP_ACK); incoming(c, 5000, 1000, 4000, TCP_ACK);
	assert(c->snd_wnd == 0); cleanup(c);
}

// Drive multiple ring turns through a peer that retains out-of-order bytes.
// Independent data/ACK losses force both duplicate-ACK and timeout recovery.
static void test_stream(void)
{
    // More than one whole ring, sized from TCP_SND_BUF: the point of the
    // test is the wrapped copies, acks and resends, and a fixed count
    // stopped wrapping the day the ring grew.
    const unsigned total = TCP_SND_BUF + 150000;
    unsigned msses[] = {48, 536, 1460};
    unsigned windows[] = {100, 4000, 65535};
    for (unsigned m = 0; m < 3; m++)
        for (unsigned w = 0; w < 3; w++)
        {
            tcp_conn_t* c = init();
            uint32_t base = 0xffff0000u;
            c->snd_una = c->snd_nxt = c->snd_max = base;
            c->snd_mss = msses[m]; c->cwnd = 10 * msses[m];
            c->snd_wnd = windows[w];
            uint8_t* seen = calloc(total + 1, 1);
            unsigned produced = 0, received = 0, examined = 0, replies = 0;
            for (unsigned round = 0; round < 400000 && received <= total; round++)
            {
                // The capture array is finite and a ring's worth of
                // stream is thousands of segments: keep only what the
                // examination below has not reached yet.
                memmove(packets, packets + examined, (npackets - examined) * sizeof packets[0]);
                npackets -= examined; examined = 0;
                unsigned n = total - produced;
                if (n > TCP_SND_BUF - c->snd_count) n = TCP_SND_BUF - c->snd_count;
                for (unsigned j = 0; j < n; j++)
                    c->snd_buf[(c->snd_head + c->snd_count + j) % TCP_SND_BUF] = (uint8_t)((produced + j) * 37u + 11u);
                c->snd_count += n; produced += n;
                if (produced == total && !c->detached) tcp_conn_close(c);
                else tcp_output(c, 0);
                // Snapshot the pass: ACK-generated packets belong to the next round.
                unsigned end = npackets;
                while (examined < end)
                {
                    struct packet* p = &packets[examined++];
                    if (examined % 7 == 0) continue;
                    unsigned offset = p->seq - base;
                    assert(offset + p->len <= total);
                    for (unsigned j = 0; j < p->len; j++)
                    {
                        assert(p->payload[j] == (uint8_t)((offset + j) * 37u + 11u));
                        seen[offset + j] = 1;
                    }
                    if (p->flags & TCP_FIN) { assert(offset + p->len == total); seen[total] = 1; }
                    while (received <= total && seen[received]) received++;
                    if (++replies % 13) ack(c, base + received, (uint16_t)windows[w]);
                }
                kTicksSinceStart++;
                tcp_poll();
                assert(!c->reset);
            }
            assert(received == total + 1);
            ack(c, base + received, (uint16_t)windows[w]);
            assert(c->state == TCP_FIN_WAIT_2 && c->snd_count == 0);
            free(seen); cleanup(c);
        }
}

static void test_arp_hold(void)
{
	// A parked segment holds output until it is acknowledged: passes under
	// the hold submit nothing however long ARP takes (a second frame would
	// replace the first in the waiting room), and the parked unit's ACK
	// resumes the cursor where it stopped — nothing sits with no flight and
	// no timer, because the parked unit keeps the RTO armed.
	tcp_conn_t* c = init(); disposition = IPV4_TX_PARKED;
	c->snd_count = 1000; tcp_output(c, 0);
	assert(npackets == 1 && c->snd_max == 2000 && c->send_timer == TCP_TIMER_RETRANSMIT && c->arp_hold);
	disposition = IPV4_TX_SENT;
	c->snd_count += 1000; tcp_output(c, 0); assert(npackets == 1 && c->snd_nxt == 2000);
	kTicksSinceStart += 15; tcp_output(c, 0); assert(npackets == 1 && c->arp_hold);
	ack(c, 2000, 65535);
	assert(npackets == 2 && packets[1].seq == 2000 && packets[1].len == 1000 && c->snd_nxt == 3000);
	assert(!c->arp_hold && c->send_timer == TCP_TIMER_RETRANSMIT);
	cleanup(c);

	// An ACK for an EARLIER unit does not lift it: the parked one is still out.
	c = init(); c->snd_count = 1000; tcp_output(c, 0);
	disposition = IPV4_TX_PARKED; c->snd_count += 1000; tcp_output(c, 0);
	assert(npackets == 2 && packets[1].seq == 2000);
	disposition = IPV4_TX_SENT; c->snd_count += 1000; ack(c, 2000, 65535);
	assert(npackets == 2 && c->snd_nxt == 3000 && c->snd_una == 2000 && c->arp_hold);
	ack(c, 3000, 65535); assert(npackets == 3 && packets[2].seq == 3000 && !c->arp_hold);
	cleanup(c);

	// Recovery outranks the hold: the RTO resends from snd_una at once.
	c = init(); c->snd_count = 1000; tcp_output(c, 0);
	disposition = IPV4_TX_PARKED; c->snd_count += 1000; tcp_output(c, 0);
	assert(npackets == 2 && c->arp_hold);
	disposition = IPV4_TX_SENT; deadline(c);
	assert(npackets == 3 && packets[2].seq == 1000 && c->retries == 1 && !c->arp_hold);
	cleanup(c);

	// A stale hold does not survive a SENT submission: a parked SYN, then
	// the handshake, then a burst — the burst goes, and the pass after it
	// is not held for that burst's ACK.
	c = init(); c->state = TCP_SYN_SENT; disposition = IPV4_TX_PARKED; tcp_send_syn(c);
	deadline(c); assert(c->arp_hold);
	incoming(c, 9000, c->snd_max, 65535, TCP_SYN | TCP_ACK);
	assert(c->state == TCP_ESTABLISHED && npackets == 3);   // the handshake's ACK is the third
	disposition = IPV4_TX_SENT; c->snd_count = 536; tcp_output(c, 0);
	assert(npackets == 4 && packets[3].len == 536 && !c->arp_hold);
	c->snd_count += 536; tcp_output(c, 0); assert(npackets == 5);
	cleanup(c);

	// A bare ACK is withheld under the hold — it would replace the parked
	// data segment, whose own ACK field predates their FIN — and is OWED:
	// it goes with the very segment that makes the hold moot, not when
	// the peer gives up and sends the FIN again.
	c = init(); disposition = IPV4_TX_PARKED; c->snd_count = 1000; tcp_output(c, 0);
	disposition = IPV4_TX_SENT;
	incoming(c, c->rcv_nxt, c->snd_una, 65535, TCP_ACK | TCP_FIN);   // their FIN: an ACK is owed
	assert(c->rcv_fin && npackets == 1 && c->state == TCP_CLOSE_WAIT && c->ack_owed);
	ack(c, 2000, 65535); assert(!tcp_unacked(c) && !c->ack_owed);      // the hold is moot: paid
	assert(npackets == 2 && packets[1].len == 0 && packets[1].ack == c->rcv_nxt);
	incoming(c, c->rcv_nxt - 1, c->snd_una, 65535, TCP_ACK | TCP_FIN); // their FIN again
	assert(npackets == 3 && packets[2].len == 0);                       // re-acknowledged
	cleanup(c);

	// And a segment of ours that goes first pays the debt on the way.
	c = init(); disposition = IPV4_TX_PARKED; c->snd_count = 1000; tcp_output(c, 0);
	incoming(c, c->rcv_nxt, c->snd_una, 65535, TCP_ACK | TCP_FIN); assert(c->ack_owed);
	disposition = IPV4_TX_SENT; deadline(c);                            // RTO: resend carries the ACK
	assert(!c->ack_owed && npackets == 2 && packets[1].len == 1000 && packets[1].ack == c->rcv_nxt);
	cleanup(c);

	// A bare ACK the driver drops has no timer of its own: the debt stays
	// owed, and the next segment from the peer — even one that earns no
	// ACK by itself — pays it.
	c = init(); disposition = IPV4_TX_PARKED; c->snd_count = 1000; tcp_output(c, 0);
	incoming(c, c->rcv_nxt, c->snd_una, 65535, TCP_ACK | TCP_FIN); assert(c->ack_owed);
	disposition = IPV4_TX_DROPPED; ack(c, 2000, 65535);                // the hold is moot; the ACK is dropped
	assert(npackets == 2 && packets[1].how == IPV4_TX_DROPPED && c->ack_owed);
	disposition = IPV4_TX_SENT; ack(c, 2000, 60000);                   // a window update, nothing to ack
	assert(npackets == 3 && packets[2].len == 0 && packets[2].ack == c->rcv_nxt && !c->ack_owed);
	cleanup(c);

	// The same debt without any hold: a bare ACK the driver drops is owed,
	// and their next segment pays it.
	c = init(); disposition = IPV4_TX_DROPPED;
	incoming(c, c->rcv_nxt, c->snd_una, 65535, TCP_ACK | TCP_FIN);
	assert(npackets == 1 && packets[0].how == IPV4_TX_DROPPED && c->ack_owed);
	disposition = IPV4_TX_SENT; ack(c, c->snd_una, 60000);
	assert(npackets == 2 && packets[1].ack == c->rcv_nxt && !c->ack_owed);
	cleanup(c);

	// A debt dies with the connection: a reset pays nothing, and neither
	// does a conversation the segment itself ended.
	c = init(); disposition = IPV4_TX_DROPPED;
	incoming(c, c->rcv_nxt, c->snd_una, 65535, TCP_ACK | TCP_FIN); assert(c->ack_owed);
	disposition = IPV4_TX_SENT;
	incoming(c, c->rcv_nxt, c->snd_una, 65535, TCP_RST | TCP_ACK);
	assert(c->state == TCP_CLOSED && !c->ack_owed && npackets == 1);
	cleanup(c);
	c = init(); c->state = TCP_LAST_ACK; c->snd_fin = true; c->snd_fin_sent = true;
	c->snd_fin_seq = 1000; c->snd_nxt = c->snd_max = 1001; c->rcv_fin = true;
	c->ack_owed = true; disposition = IPV4_TX_SENT;
	ack(c, 1001, 65535);                                               // their ACK of our FIN: CLOSED
	assert(c->state == TCP_CLOSED && npackets == 0);
	cleanup(c);

	// A SENT probe clears a stale hold: the neighbour answered it, so the
	// window's reopening is not held for the timer.
	c = init(); disposition = IPV4_TX_PARKED; c->snd_count = 1000; tcp_output(c, 0);
	ack(c, 1000, 0); assert(c->send_timer == TCP_TIMER_PERSIST && c->arp_hold);
	disposition = IPV4_TX_SENT; deadline(c); assert(c->probe_pending && !c->arp_hold);
	ack(c, 1000, 65535);
	assert(npackets == 3 && packets[2].seq == 1000 && packets[2].len == 1000);
	cleanup(c);

	// A parked fast retransmission holds too: the window inflating on
	// further duplicates must not replace the resent head; the partial ACK
	// that proves it landed resumes.
	c = init(); c->snd_count = 5000; tcp_output(c, 0); assert(npackets == 5);
	disposition = IPV4_TX_PARKED;
	for (unsigned i = 0; i < 3; i++) ack(c, 1000, 65535);
	assert(npackets == 6 && packets[5].seq == 1000 && c->fast_recovery && c->arp_hold);
	disposition = IPV4_TX_SENT; c->snd_count += 1000;
	ack(c, 1000, 65535); assert(npackets == 6);
	ack(c, 2000, 65535);
	assert(packets[6].seq == 2000 && !c->arp_hold);            // NewReno resend of the next hole, SENT
	assert(npackets == 8 && packets[7].seq == 6000);          // and the pass behind it moves again
	cleanup(c);
}

static void test_writer_wake(void)
{
	// A writer parked on a full ring is woken by the ACK that makes room,
	// on tcp_input's own exit — not when the whole flight has drained, and
	// not at the next sweep. The reader's condition is the same door.
	thread_t writer = {THREAD_STATE_ISLEEP}, reader = {THREAD_STATE_ISLEEP};
	tcp_conn_t* c = init(); c->snd_count = TCP_SND_BUF; tcp_output(c, 0);
	assert(tcp_unacked(c) && c->snd_max - c->snd_una == 10000);
	c->writer = &writer; c->reader = &reader;
	ack(c, 1000, 65535); assert(c->writer == &writer);        // no room made
	ack(c, 2000, 65535); assert(c->writer == NULL);           // room, flight still out
	assert(c->reader == &reader);                              // nothing to read yet
	c->writer = &writer; writer.threadState = 0;
	ack(c, 3000, 65535); assert(c->writer == &writer);        // not parked yet: stays registered
	incoming(c, c->rcv_nxt, c->snd_una, 65535, TCP_ACK | TCP_FIN);
	assert(c->reader == NULL);                                 // EOF wakes the reader
	cleanup(c);
}

static void fill_ring(tcp_conn_t *c)
{
	c->snd_count = TCP_SND_BUF;
	c->snd_nxt = c->snd_max = c->snd_una + TCP_SND_BUF;
}
static void wake_room(uint64_t wake, thread_t *self)
{
	(void)wake;
	assert(kTcpConnList->writer == self && !kTcpConnList->lock);
	kTicksSinceStart++;
	ack(kTcpConnList, kTcpConnList->snd_una + 7, 65535);
}
static void wake_interrupt(uint64_t wake, thread_t *self)
{
	assert(kTcpConnList->writer == self && !kTcpConnList->lock);
	kTicksSinceStart = wake; interrupted = true;
}
static void wake_reset(uint64_t wake, thread_t *self)
{
	assert(kTcpConnList->writer == self && !kTcpConnList->lock);
	kTicksSinceStart = wake; kTcpConnList->reset = true;
}
static void test_write_deadline(void)
{
	unsigned char bytes[40]; for (unsigned i = 0; i < sizeof bytes; i++) bytes[i] = (uint8_t)(i + 1);
	tcp_conn_t *c = init(); fill_ring(c);
	assert(tcp_conn_write(c, bytes, sizeof bytes, 100) == TCP_ERR_TIMEOUT);
	assert(!sleeps && !c->writer && !c->reset && !c->detached && c->snd_count == TCP_SND_BUF);
	assert(tcp_conn_write(c, bytes, sizeof bytes, 105) == TCP_ERR_TIMEOUT);
	assert(sleeps == 1 && last_wake == 105 && !c->writer);
	sleeps = 0;
	assert(tcp_conn_write(c, bytes, sizeof bytes, 350) == TCP_ERR_TIMEOUT);
	assert(sleeps == 3 && kTicksSinceStart == 350 && !c->writer);
	cleanup(c);

	c = init(); c->snd_wnd = 0; c->snd_head = TCP_SND_BUF - 3;
	assert(tcp_conn_write(c, bytes, sizeof bytes, 99) == sizeof bytes);
	assert(!memcmp(c->snd_buf + TCP_SND_BUF - 3, bytes, 3));
	assert(!memcmp(c->snd_buf, bytes + 3, sizeof bytes - 3) && !sleeps);
	cleanup(c);

	c = init(); fill_ring(c); sleep_hook = wake_room;
	assert(tcp_conn_write(c, bytes, sizeof bytes, 103) == 21);
	assert(sleeps == 3 && !c->writer && !memcmp(c->snd_buf, bytes, 21));
	cleanup(c);
	c = init(); fill_ring(c); sleep_hook = wake_room;
	assert(tcp_conn_write(c, bytes, sizeof bytes, 0) == sizeof bytes && sleeps == 6);
	assert(!c->writer); cleanup(c);

	c = init(); fill_ring(c); sleep_hook = wake_interrupt;
	assert(tcp_conn_write(c, bytes, sizeof bytes, 150) == TCP_ERR_INTERRUPTED);
	assert(!c->writer && !c->reset); cleanup(c);
	c = init(); fill_ring(c); sleep_hook = wake_reset;
	assert(tcp_conn_write(c, bytes, sizeof bytes, 150) == TCP_ERR_RESET);
	assert(!c->writer); cleanup(c);

	c = init(); fill_ring(c); sleep_hook = wake_room;
	assert(tcp_conn_write(c, bytes, 14, 0) == 14 && sleeps == 2 && !c->writer);
	assert(!memcmp(c->snd_buf, bytes, 14)); cleanup(c);
	c = init(); c->snd_count = TCP_SND_BUF - 7; c->snd_wnd = 0;
	sleep_hook = wake_interrupt;
	assert(tcp_conn_write(c, bytes, 14, 0) == 7 && sleeps == 1 && !c->writer);
	cleanup(c);
	puts("TCP write waits: poll, finite expiry, partial/wrapped progress, wake, signal/reset and blocking writes PASS");
}

int main(void)
{
	test_submissions(); test_handshake_reset(); test_lifetime(); test_rto(); test_congestion();
	test_persist(); test_wrap_fin_window(); test_stream(); test_arp_hold(); test_writer_wake(); test_write_deadline();
	puts("TCP host: submission, lifetime, RTO, congestion, persist, ring/sequence/FIN, ARP-hold, writer-wake tests PASS");
	return 0;
}
