#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

// virtio_net.h — the paravirtual NIC (NETWORK.md Phase 1, first driver).
//
// Why virtio first: it is QEMU's native NIC *and* VirtualBox emulates it,
// so one driver covers both hypervisors; and its virtqueue ring discipline
// is the template every modern paravirt device shares — learning it here
// pays again for every future virtio device (block, console, entropy...).
// The e1000 comes second to prove the net_device seam against real-hardware
// register style (and because the Intel 8254x manual is the classic teaching
// datasheet of hobby OS development).

// Scan PCI for a virtio-net device; if found, bring it up through feature
// negotiation, build the virtqueues, read its MAC, and register it with
// the net_device seam. Quietly does nothing if no device is present (a
// boot with no NIC attached must stay a normal boot, not an error).
void init_virtio_net(void);

// Drain the used rings: TX completions free their slots, RX arrivals are
// delivered through net_device_rx and their buffers recycled. Called from
// processSignals every scheduler pass (the xhci_poll precedent) — cheap
// when idle (one guarded compare against each ring's index), safe from
// any context (irqsave lock inside). Interrupt-driven delivery is a
// future slice; this is the liveness path until then.
void virtio_net_poll(void);

#endif // VIRTIO_NET_H
