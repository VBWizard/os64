# Intel AX210 Wi-Fi on the P5

*Revised draft, 2026-11-28. [Slice tracker](WIFI_TRACKER.csv) · [W.1 bring-up and evidence](WIFI_W1.md)*

The first usable target is a P5 that boots cold, joins one WPA2-Personal
network at 2.4 GHz, gets a DHCP lease and loads a page in Yonder with the
Ethernet cable disconnected. Keep the P5's existing static Ethernet boot
available. Os64 still has one active IPv4 uplink per boot.

## 1. Hardware and code we can build on

The running P5 enumerates an Intel AX210 at PCI `03:00.0`, vendor/device
`8086:2725`, revision `1a`, subsystem `8086:0024`. BAR0 is 64-bit MMIO.
The function advertises one MSI vector and sixteen MSI-X vectors. Linux
maps this PCI ID and subsystem to the Typhoon Peak `ty-a0-gf-a0` firmware
family. The driver will match device identity rather than the slot number.
The module's Bluetooth function is USB `8087:0032`, with its own HCI
transport and firmware. [Linux PCI table](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/intel/iwlwifi/pcie/drv.c#L631) · [Bluetooth firmware provenance](../../../firmware/intel/SOURCE.txt)

The RTL8125 and xHCI work gives us PCI enumeration, MMIO mapping, DMA
allocation, interrupt registration and the doorbell pattern for deferred
work. The AX210 needs its own register sequence, descriptor formats, queue
ownership and firmware commands. Above those, a station layer can translate
802.11 data to and from the existing Ethernet-shaped `net_device` interface.
DHCP, ARP, IPv4, DNS, TCP and Yonder then use the normal network path.
[RTL8125](../completed/RTL8125.md) · [Doorbells](../completed/DOORBELL.md) · [NIC interface](../../../kernel/include/driver/net/net_device.h)

Use OpenBSD's native `iwx` as the main reference for the minimum station
path, and Intel's Linux `iwlwifi` for hardware and firmware details. Both
rely on a host 802.11 layer. The device can scan under firmware control
and offload unicast CCMP, while the host still coordinates association,
key negotiation and frame delivery. Review the license of each source file
used for adaptation and retain its notices. Pin the reference revisions,
firmware API and command layouts together in W.1.
[OpenBSD iwx driver](https://github.com/openbsd/src/blob/master/sys/dev/pci/if_iwx.c) · [OpenBSD iwx manual](https://man.openbsd.org/iwx.4) · [Intel AX210 firmware configuration](https://github.com/torvalds/linux/blob/master/drivers/net/wireless/intel/iwlwifi/cfg/ax210.c)

## 2. Boot selection and IP configuration

The P5 currently boots with `IP=`, `GW=` and `MASK=` for the wired
RTL8125. Os64 builds one global IPv4 configuration from those tokens and
starts DHCP at boot on the first registered NIC when `IP=` is absent.
The DHCP client also holds one transaction and one lease.
[Boot configuration](../../../kernel/src/kernel_commandline.c) · [Network startup](../../../kernel/src/kernel.c) · [DHCP client](../../../kernel/src/driver/net/dhcp.c)

The proposal gives the boot menu three choices.

| Boot choice | Active IPv4 uplink | Address configuration |
| --- | --- | --- |
| Existing command line, with no `WIFI=` | Legacy wired NIC selection | Existing `IP=`, `GW=`, `MASK=` behavior, including boot-time DHCP when `IP=` is absent |
| `WIFI=probe` or `WIFI=probe-msix` plus the existing command line | Legacy wired NIC selection | Existing address configuration. Initialize the AX210 for diagnostics while wired connectivity remains available. |
| `WIFI=dhcp` plus the existing command line | AX210 | Start DHCP after the protected Wi-Fi link is ready. Legacy static values belong to the wired profile. |

`NONET` disables networking in every mode. The probe modes perform no
automatic scan, association or Wi-Fi IP traffic. `WIFI=probe-msix` selects
the single-vector interrupt path for W.1 hardware verification. Later slices may run
explicit test operations through the boot-started station service in that
mode. With `WIFI=dhcp`, a missing adapter, missing profile, rejected join
or failed lease leaves
external IPv4 offline with a visible reason. Recovery to the static wired
configuration is a boot choice. This keeps the established Ethernet entry
usable on the P5 and makes Wi-Fi acceptance independent of the cable.
Edit the P5 boot entries by hand with the case-sensitive tokens above.
The normal update leaves the machine's boot menu in place.
[Boot-menu convention](../../../CLAUDE.md)

The supplicant reads one explicitly selected profile under `/home`.
Programs with local filesystem access can read the credential under
Os64's present file model. Local credential confidentiality therefore
depends on running trusted software. Keep secrets out of the boot command
line, process arguments, diagnostics, `/sys` and test logs. Simultaneous
Wi-Fi and wired addresses, automatic failover and runtime uplink
switching can get their own design when there is a use for them.
[Bluetooth bond storage precedent](BLUETOOTH_BONDING.md)

## 3. Implementation slices

### W.1 Cold boot and firmware command round trip

The [W.1 brief](WIFI_W1.md) pins the firmware and defines the transport
and hardware checkpoints.

Choose a known matching firmware image, API and PNVM format from the
pinned reference driver. A Linux boot on the P5 can supply a useful
comparison of hardware identity, firmware and PNVM, when convenient for
the operator. W.1 can proceed from the pinned reference. W.1.1 chooses
kernel embedding or a root-filesystem input and documents how the image and
parameters stay paired on install and rollback. Favor embedding for a
kernel-only bring-up if the terms allow it. Record the source revision,
hashes and distribution terms beside the selected files, following the
Bluetooth precedent. Add the shipped component and upstream to
`THIRD_PARTY.md` when introducing it.
[Intel firmware files](https://gitlab.com/kernel-firmware/linux-firmware/-/tree/main/intel/iwlwifi) · [Linux ALIVE and PNVM sequence](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/intel/iwlwifi/mvm/fw.c#L391)
[Bluetooth provenance](../../../firmware/intel/SOURCE.txt) · [Third-party inventory](../../../THIRD_PARTY.md)

Discover and map the function during boot. Have `knet` advance a bounded
firmware state machine from reset through initialization, with deadlines
and bounded work per wake so existing NICs and timers get their turn.
Each boot resets the function before handing it new DMA addresses. Give
`knet` a way to service firmware events before ordinary NIC publication.
The current registration contract expects a real MAC and a transmit
operation, and boot picks a wired uplink by registration order. Keep
discovery and command service independent of that selection. Where
firmware bytes need filesystem I/O, stage them outside the network
drain path. The boot thread proceeds while the firmware responds.
[Network drainer](../../../kernel/src/knet.c) · [NIC registration](../../../kernel/src/driver/net/net_device.c)

Use one verified interrupt vector with `pci_msi_enable` or
`pci_msix_enable_one` and the matching device cause mapping. The RTL8125
uses `0x46` and xHCI uses `0x47`. The IRQ acknowledges or masks causes
and wakes deferred work. The transport brief must show buffer ownership,
descriptor reuse and how failed initialization, timeout and reset quiesce
DMA before releasing storage. On orderly reboot and shutdown, stop new
work, mask interrupts and confirm DMA has stopped before handoff. Make
this a bounded step in shutdown, while the network worker and logs are
available. Define the containment path for a wedged device in the brief.
A timeout alone cannot authorize freeing DMA storage or a warm handoff.
[PCI interrupts](../../../kernel/src/driver/system/pci.c) · [Shutdown sequence](../../../kernel/src/shutdown.c) · [Bluetooth warm-reboot evidence](../../../DEBTS.md)

Send the pinned firmware's Bluetooth coexistence command (`BT_CONFIG`)
during initialization, before scan or association. Record the mode, command
result and any later channel update the reference requires. OpenBSD and
Linux send different coexistence settings. Use the configuration for the
chosen API and verify behavior on the P5.
[OpenBSD coexistence configuration](https://github.com/openbsd/src/blob/master/sys/dev/pci/if_iwx.c#L9344) · [Linux coexistence configuration](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/intel/iwlwifi/mvm/coex.c#L129)

Accept a power-removed P5 boot into Os64 with `ALIVE`, PNVM and required
initialization complete, plus a successful ordinary command reply, such
as an NVM query. Record the card's MAC and channel capabilities, firmware
version and interrupt mode. Repeat the cold boot, then accept ten
consecutive normal warm reboots and one orderly shutdown with the active
AX210. Record quiesce and startup in the logs. Exercise a bounded failure
and show that wired networking and Bluetooth still work in the diagnostic
boot.

### W.2 Scan and associate on 2.4 GHz

Provide firmware scan requests, bounded scan results and the station
management needed to authenticate and associate with a selected BSSID.
Record the channel, signal and advertised security. Keep the first station
path to one BSSID at 20 MHz with legacy 802.11g-class rates. Advertise
the matching Supported Rates and Extended Supported Rates, honoring the
AP's required basic rates. Leave HT, VHT and HE capabilities unadvertised,
disable TX aggregation and decline block-ack setup in either direction.
Legacy OFDM tops out at a 54 Mb/s PHY rate. Measure actual throughput
after connectivity works. Before transmitting, intersect
device/firmware channel permissions with applicable regulatory restrictions.
Honor radio disable, and proceed with passive discovery or stop when the
available channel information does not authorize transmission.
[Linux wireless regulatory rules](https://wireless.docs.kernel.org/en/latest/en/developers/regulatory/processing_rules.html) · [Channel flags](https://docs.kernel.org/driver-api/80211/cfg80211.html#enum-ieee80211-channel-flags)

Accept a P5 scan that finds the controlled AP and an association confirmed
by both ends. Report association separately from authorization. Keep the
Wi-Fi data port closed and Wi-Fi DHCP idle through this slice. Exercise a
failed join and a retry, with deadlines and stale events tied to the
association attempt that produced them. Run the Bluetooth coexistence
check in § 4.

### W.3 Authorize a WPA2-Personal link

Support one WPA2-PSK network using CCMP. Run the four-way handshake in a
userland supplicant started by the kernel when the enabled Wi-Fi mode
needs station operations. The kernel gives that task exclusive station
control. Bind access to the task created at boot rather than whichever
program opens a public file first. Keep diagnostics read-only with respect
to station and key state. The W.3 brief must define the ownership checks,
handoff and revocation on task exit. Use a station service with the same
ownership for W.2 test operations.
[Kernel-launched services](../../../kernel/src/kernel.c)

Choose between a small Os64 supplicant built on existing vetted
primitives and adapting the reusable `wpa_supplicant` protocol code.
Compare the porting work and the evidence for state-machine correctness.
Define a bounded kernel interface for connection events, EAPOL traffic,
and key install and removal. Use the established Os64 entropy interface.
Scope events and key requests to the current association and keep ordinary
traffic gated until the supplicant has completed the exchange and the
required pairwise and group keys are ready. Assign CCMP TX/RX, packet-number
and replay ownership explicitly for both unicast and group traffic. Use
hardware offload where confirmed for the pinned firmware and a vetted
implementation for any required host-side crypto.
[wpa_supplicant porting interface](https://w1.fi/wpa_supplicant/devel/porting.html) · [Os64 randomness](../completed/RANDOM.md)

Accept authorization against the controlled AP, with protected unicast and
group delivery exercised before general IPv4 enablement. A wrong password
leaves the port closed. Retries and duplicate handshake messages must
preserve installed-key state and replay protection. Disconnect, firmware
reset or loss of the supplicant closes the port and discards that
association's keys. Rejoin has fresh state. Exercise the Bluetooth
coexistence check in § 4 with the protected link.

### W.4 Run the existing IPv4 stack over Wi-Fi

Add the station data path in both directions at the `net_device` seam
under the controlled port. Boot explicitly selects the active external
device independently of enumeration order. Use that selection consistently
for dial, ingress and address ownership in `/sys/net`. Loopback remains
independent. In a Wi-Fi boot with cable attached, the wired device can be
visible as hardware without owning the active address or sending ordinary
network traffic. Report unavailable external connectivity at dial while
Wi-Fi is disconnected or waiting for a lease.

Start and reset the Wi-Fi DHCP transaction in the same serialized network
context that handles replies and timers. Bind it to the selected device
and current connection, reject stale or mismatched replies and invalidate
the lease, DNS and neighbor state on link loss or lease expiry. Retire
connections using the old link so they cannot resume under a new lease.
Reacquire on reconnect or expiry. Renewal and rebinding are follow-on
work for uninterrupted leases. The Wi-Fi path starts without a wired
address, mask or gateway. The slice brief should spell out what happens
when DHCP options are missing and how a configured `net.conf` resolver
override interacts with the DHCP-provided name server.
The legacy wired DHCP fallback stays on the legacy path.
[Os64 resolver](../../../userland/libos64/resolve.c) · [DHCP lease rules](https://www.rfc-editor.org/rfc/rfc2131.html#section-4.4.5)

Accept the existing P5 static Ethernet boot, a QEMU legacy DHCP boot and a
P5 Wi-Fi boot with the old static tokens still present. With Ethernet
physically disconnected, prove lease acquisition, gateway reachability,
DNS and a Yonder page load. Exercise DHCP timeout, a wrong Wi-Fi credential,
and reconnect with delayed traffic from the old association or lease.
Repeat with the Ethernet cable attached and verify the selected uplink
and source address. Record the active device and who supplied the lease
in the evidence. Run the Bluetooth coexistence check in § 4 while
loading a page on the Wi-Fi link.

Exercise parsing and frame conversion with host fixtures and use
`WIFI=probe` for hardware scan, association, key and link-frame checks
that can run while wired IP remains active. For the Wi-Fi-only boots,
set `LOGD=/home/wifi-boot.log` and collect the log after a wired reboot
if the Wi-Fi lease never comes up. Check the leased address locally
before attempting SSH. If the Wi-Fi segment reaches the PC update server,
use its address from that segment for `os64get`, with a local
`/home/hosts` entry if helpful. Stage the test build over Ethernet
before the Wi-Fi boot so evidence collection works without that route.
[Log daemon](../../../userland/apps/logd/logd.c)

### W.5 Prove 5 GHz on a non-DFS channel

Reuse the association, WPA2 and IP path at 20 MHz with legacy 802.11a
rates. Resolve the channel permissions for the applicable regulatory
domain before transmitting. Add and verify the band-specific channel
entries, scan mode and rates on an allowed non-DFS channel. Leave DFS
channels unavailable in v1. An AP channel move outside the supported
set should drop the link without
sending on the unsupported channel. 5 GHz adds a bounded RF and
interoperability test once 2.4 GHz works. HT, VHT and HE rates, aggregation,
block-ack and reorder buffers belong to the throughput follow-on. DFS,
wide channels, 6 GHz, WPA3 and roaming can be planned separately.
[Linux wireless channel flags](https://docs.kernel.org/driver-api/80211/cfg80211.html#enum-ieee80211-channel-flags) · [DFS background](https://wireless.docs.kernel.org/en/latest/en/developers/dfs.html)

Accept scan, protected join, DHCP and a Yonder fetch on a controlled 5 GHz
AP, with the actual channel recorded and 2.4 GHz rechecked. Include a
channel-filter test proving the driver refuses transmissions on a
restricted channel.

## 4. Evidence and tracker upkeep

Keep parsing cores independently host-testable and fuzz firmware
notification and RX descriptor decoding, beacon and probe-response
headers and information elements, and EAPOL parsing. Validate device
lengths against the actual DMA buffer and frame boundaries before
dispatch. Model station, key and DHCP transitions in host tests with a
fake AP or DHCP peer, and use mutants to prove the tests reject broken
transitions. Use QEMU for wired and loopback regressions and the P5 for
AX210 firmware and RF evidence.

Expose a read-only `/sys/net/wifi` report with the firmware version,
interrupt mode, current phase, channel and latest failure. Show
firmware boot, scan, association, authorization and IP readiness as
distinct milestones. Keep secrets outside the report and logs.

Show the AX210 initialization result on the system console. Use the
system boot log for detailed initialization milestones and failures.
For `WIFI=dhcp`, show when Wi-Fi obtains a lease or when DHCP fails. Keep
the assigned address, gateway and netmask in the live DHCP status under
`/sys/net/dhcp` without printing those values in the routine Wi-Fi boot log
or on the console. The wired boot entry retains its current logging.

For the Bluetooth coexistence check in W.2 through W.4, compare against a
Bluetooth-only baseline on the same P5. Use the Bluetooth keyboard and
mouse while playing a track on the headphones through a scan and while
associated. Listen for stutter and record changes in
`/sys/audio`'s `underruns_since_read` and the Classic slot's `a2dp:`
slip counter. W.4 also checks with active Wi-Fi data traffic.

Use the normal P5 update path through the Windows server and
`os64get -a`. Serve the small frozen set of tested files with a SHA-256
manifest so the same builds are installed across hardware runs. Record
the boot token with the tested set and apply it by hand on the P5.

Start each slice with a concrete task list and acceptance proof. Keep
known work under its `W.n` parent as `W.n.m` rows. Append newly discovered
work with `ADDED YYYY-MM-DD` and the reason. Preserve task statuses and
implementing PRs. Close the parent after the slice's acceptance, including
hardware checks where specified. The [shared tracker format](../../../tools/TRACKERS.md)
defines the CSV columns.

Once the Wi-Fi arc passes acceptance and its implementation slices are
merged, move this design to `docs/design/completed/WIFI.md` and update
the links that point to or from it.

```sh
python3 -I tools/tracker.py --csv docs/design/pending/WIFI_TRACKER.csv --prefix W --quiet
~/tracker.sh wifi
```

## 5. Decisions for review

- Use explicit boot selection with `WIFI=probe` and `WIFI=dhcp`, keeping
  the P5's existing static Ethernet entry as the wired boot.
- Use the native OpenBSD driver as the main implementation reference.
  Choose the exact firmware/API and the interrupt mode in W.1.
- Put WPA2 negotiation in the kernel-launched userland service. Settle
  code reuse, ownership mechanics and the security tests in the W.3 brief.
- Make 2.4 GHz the first end-to-end gate and track non-DFS 5 GHz as the
  next hardware proof.
- An isolated open AP could provide an early frame and DHCP diagnostic
  if the operator wants it. The default plan uses host fixtures for that
  separation and keeps the first P5 IP acceptance on WPA2-Personal.
