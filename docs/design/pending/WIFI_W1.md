# AX210 firmware bring-up on the P5

[Design](WIFI.md) · [Tracker](WIFI_TRACKER.csv)

W.1 covers the P5's discrete Typhoon Peak device `8086:2725`, subsystem
`8086:0024`. The firmware is Intel `77.f92b5fed.0` and the paired PNVM
is `ce1a5094`, both extracted verbatim from OpenBSD's signed 7.7 firmware
distribution. The kernel embeds both files and the redistribution notice
so the boot image and rollback carry a matching pair. Use OpenBSD
`f133b38f0e87720f82ec740eb05264298ecab82a` for the Gen3 context and queue
sequence. The transport must check the firmware/API it actually receives.
[Firmware provenance](../../../firmware/intel/ax210-wifi/SOURCE.txt) · [Pinned iwx reference](https://github.com/openbsd/src/tree/f133b38f0e87720f82ec740eb05264298ecab82a/sys/dev/pci)

## First hardware checkpoint

On the inventory build, add `WIFI=probe` to a copy of the P5's working
wired boot entry. Read `/sys/net/wifi` and capture the serial or `logd`
line starting `ax210:`. The successful inventory reports the PCI identity,
whether BAR0's read-only identification registers were accessible, embedded
firmware version, and the loader and section counts. Expected counts for
the pinned package are LMAC 15, UMAC 16, paging 23 and IML 13944 bytes.
Existing Ethernet stays active. The boot menu entry is edited on the P5.

The P5 reported `phase: inventory` for `03:00.0`, revision `1a`,
with the expected firmware and section counts. PCI memory decoding was
disabled on entry. The transport must enable MMIO decoding with bus
mastering clear before reading the chip IDs.

The inventory phase leaves PCI decode, interrupts, and bus mastering as
they were at boot. It uses the absence of hardware to exercise the
bounded QEMU path. Actual firmware startup, radio commands and warm-reboot
acceptance require the matching P5 function.

The inventory build booted in QEMU with the default settings, with
`WIFI=probe`, and with `WIFI=probe NONET`. Wired DHCP passed on the
networked boots. The probe boot listed `wifi` under `/sys/net` and
returned `phase: absent` from the report. The embedded ucode and PNVM
passed the host fixture and malformed-record tests with ASan, UBSan and
LeakSanitizer enabled.
[Host checks](../../../tools/test_ax210_fw_host.sh)

## Reset checkpoint

`WIFI=probe` on the reset build clears PCI bus mastering and disables PCI
interrupt delivery before handing the controller to `knet`. The worker
brings the function to D0 if needed, verifies BAR0 before enabling MMIO,
checks that outstanding PCI transactions have drained, acquires device
ownership, and requests a software reset and MAC clock. It advances on
ordinary `knet` wakes and bounds each wait. The `reset-ready` report means
the MAC clock is ready and PCI bus mastering still reads disabled.
The build submits no DMA descriptors or firmware commands.

The host model drives the production probe through D0 and D3 startup,
and exercises stuck PCI transactions and a MAC clock timeout. On the P5,
read `/sys/net/wifi` for the terminal phase and the raw MAC/RF registers
or the failing stage.

The P5 reached `reset-ready` with the MAC clock ready and PCI bus
mastering disabled. The raw registers were `hw_rev=0x00000420` and
`rf_id=0x0010d000`, matching the Typhoon Peak MAC and GF radio expected
by the pinned firmware. This verifies the reset checkpoint. Firmware
startup and repeated boot acceptance belong to the following proof.

## ALIVE checkpoint

The ALIVE build publishes the pinned image, Gen3 context and queue memory
before enabling PCI bus mastering. It polls the RFH-ready cause through
the legacy cause registers with PCI interrupt delivery disabled. This
keeps interrupt setup separate from the first hardware proof. It offers
RX buffers after RFH is ready and validates the ALIVE v6 notification
from the DMA completion ring. The RFH-ready cause alone is insufficient.

`phase: alive` means the firmware returned status `0xcafe` and the
driver then stopped DMA, cleared bus mastering and reset the device.
`pnvm_match` identifies a matching packaged record for the returned SKU.
Firmware initialization, including PNVM upload and commands, continues
in the next checkpoint. The report captures PCI command and PM state
on entry, CSR and boot-stage values, and the firmware's ALIVE payload.

The shutdown path closes the startup gate. If a boot is in flight it
waits for the network worker to stop it. The worker retains the DMA
allocations if it cannot prove the stop. If the descent cannot confirm
DMA safety in time, it takes the poweroff path.

The P5 acceptance compares power-removed boots and ordinary
Os64-to-Os64 reboots. The Os64 loader resets and repopulates the queues
on each run. A warm handoff from Linux with the AX210 connected is a useful
diagnostic when the machine can reach Limine. Warm-boot repetition and
firmware initialization remain part of full W.1 acceptance.

The first P5 run was a warm boot. PCI command was `0x0000` in D0 on
entry. The probe consumed one RX completion, received ALIVE status
`0xcafe` from firmware `77.f92b5fed`, and matched SKU
`000510d0/00000000/00000000` to PNVM `ce1a5094`. The terminal report was
`phase: alive` with PCI bus mastering disabled after the probe stopped.

The power-removed P5 boot produced the same complete Wi-Fi report,
including the PCI entry state, one RX completion, ALIVE status, PNVM
match and disabled bus mastering after stop.

For the Linux comparison, Chris rebooted from that Os64 cold boot,
selected Ubuntu in firmware setup, confirmed Wi-Fi was connected in
Ubuntu, then ran Linux's `reboot`. The P5 was still on a black screen
after five minutes without reaching the Limine menu. He has also seen
long waits after Linux reboots before this test. The handoff yielded no
Os64 AX210 evidence. The cause of the pre-Limine delay is undetermined.

## PNVM and command checkpoint

After ALIVE, the probe selects the matching PNVM for the reported SKU and
uses the contiguous layout advertised by the pinned API 77 image. It
publishes the PNVM address and byte count in Gen3 scratch, rings the
PNVM doorbell and waits for a valid `PNVM_INIT_COMPLETE`. The P5 returned
`0x00020001` for the 13,704-byte `ce1a5094` image. The initial build
mistook that word for a rejection. OpenBSD advances on the completion
notification and Linux records the raw word before advancing. Os64 checks
the notification's format, records its status and keeps MAC access
through the bounded command sequence.
[OpenBSD PNVM completion](https://github.com/openbsd/src/blob/f133b38f0e87720f82ec740eb05264298ecab82a/sys/dev/pci/if_iwx.c#L10527)
[Linux PNVM completion](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/intel/iwlwifi/fw/pnvm.c#L20-L31)

Send `INIT_EXTENDED_CFG`, then `NVM_ACCESS_COMPLETE`. Match each response
to the command queue, sequence, group and opcode. Consume the independent
`INIT_COMPLETE` notification before asking for `NVM_GET_INFO`. Decode its
v4 response and read the MAC from the strap registers or OTP. Retain
device-owned TFDs, command payloads and the PNVM image through the
verified stop. Keep RX service polled for this checkpoint.
[Init and NVM query order](https://github.com/openbsd/src/blob/f133b38f0e87720f82ec740eb05264298ecab82a/sys/dev/pci/if_iwx.c#L4413)

After reading NVM, send `BT_CONFIG` with the reference driver's Wi-Fi
coexistence mode and its optional module flags clear. Submit the legacy
opcode with the long-group header and accept a matching reply in either
the legacy or the long group. Acknowledge this checkpoint only after a
successful reply on the expected command slot.
[OpenBSD coexistence setup](https://github.com/openbsd/src/blob/f133b38f0e87720f82ec740eb05264298ecab82a/sys/dev/pci/if_iwx.c#L9249)

Expect `phase: nvm-ready`, a raw PNVM completion status, `replies=4`,
`bt_config=1`, and a MAC and NVM summary in `/sys/net/wifi`. Channel counts are
NVM capabilities. Regulatory selection for active scanning belongs to
W.2. After the coexistence reply the probe verifies DMA stop and resets
the controller.
Host tests replay the P5 PNVM completion through the command sequence and
exercise a truncated PNVM notification, command rejection, an early init
notification, a stale reply, `BT_CONFIG` reply matching and timeout, and
shutdown with DMA active.
[Host checks](../../../tools/test_ax210_fw_host.sh)

Before adding coexistence configuration, the P5 reported `nvm-ready` on
the corrected PNVM build with status `0x00020001`, three command replies
and `init_complete=1`. The
last reply was `NVM_GET_INFO` on queue 0 at index 2. The NVM version was
`0x1291` with 38 valid entries out of 110, and the driver read a valid
MAC address and confirmed PCI bus mastering was disabled at stop.
The system boot log contained the probe, ALIVE, PNVM completion,
`NVM_GET_INFO` reply and `nvm-ready` lines for this run.

The console reports the probe result through the same `printf` path as
the wired NIC. A successful probe prints the PCI location, firmware and
MAC address after DMA stop is verified. A failure prints the phase and
reason. Detailed milestones stay in the system boot log and the probe
state stays readable in `/sys/net/wifi`.

On the polled coexistence build the P5 reported `nvm-ready`, `replies=4` and
`bt_config=1`. The final reply was group `0x01`, opcode `0x9b`, queue 0
and index 3. The probe stopped with PCI bus mastering disabled, and the
report was retrieved over the wired link. Chris confirmed that the
Bluetooth keyboard and mouse both worked on this boot. Later slices
cover Bluetooth coexistence during active Wi-Fi scanning and traffic.

## MSI-X checkpoint

`WIFI=probe-msix` selects the interrupt-driven probe for P5 verification.
Keep `WIFI=probe` as the polled baseline. Program table entry zero for
BSP vector `0x48` after reset and before starting the loader. The handler
acknowledges the device's FH and hardware causes, clears the vector's
automask and rings knet. Knet owns RX descriptors and commands, checks
the ring on its existing timer wakes, and requires the interrupt's ALIVE
cause before offering the first RX buffers. On stop, knet closes the
handler, masks the causes and disables PCI MSI-X before resetting the
controller and freeing DMA.
[OpenBSD single-vector routing](https://github.com/openbsd/src/blob/f133b38f0e87720f82ec740eb05264298ecab82a/sys/dev/pci/if_iwx.c#L2505)

The P5 proof is `interrupt_mode: msix`, at least one RX interrupt
(`rx_irqs`), and the same four command replies and safe stop as the
polled probe. The host model drives the handler with ALIVE and RX
causes, checks the command sequence and verifies that a late IRQ after
shutdown cannot rearm the vector. A separate host case injects a DMA
engine fault over MSI-X while the loader owns the buffers. It checks
that the worker masks the causes, disables PCI MSI-X, and waits for DMA
stop and reset before freeing the buffers.

The first P5 run with `WIFI=probe-msix` programmed entry zero at BAR0
offset `0x2000` with vector `0x48`. The device delivered eight MSI-X
interrupts, seven with RX causes. The probe reached `nvm-ready` with
`replies=4`, `bt_config=1` and PCI bus mastering disabled. The report
was retrieved over the wired link.

Chris removed AC power for about ten seconds before the cold MSI-X run.
The P5 again delivered eight interrupts with seven RX causes, completed
all four commands including `BT_CONFIG`, and stopped with PCI bus
mastering disabled. The wired link was available to retrieve the report.

The next two ordinary warm reboots both reached `nvm-ready` with seven
RX interrupts and four command replies. The third boot faulted before
the SSH watch could collect a report. After recovery, the persisted log
showed that this boot had already reached `nvm-ready` over MSI-X,
acknowledged `BT_CONFIG`, disabled PCI bus mastering and passed all 38
post-boot tests, including both wired gateway echo tests. Userland
started and SSH logged that it was listening. The fault screen identified AP1,
`kworker` task 46, thread `0x2e`, vector 14 and error code `0x10`.
RIP and the fault address were zero, with RSP `0x70013250`. The warm
reboot count stopped at two. The cause remains to be established.
The fault photo shows IF clear, RBP zero, and no readable stack in the
panic report. R12 `0xffffffff800f963a` resolves in the frozen kernel's
symbols to the instruction after `call exception_dispatch` in
`exc_common`. The captured RIP is zero, so R12 alone cannot identify
the instruction that transferred control there. Compare with
`WIFI=probe` on the same kernel before changing the driver or scheduler.
The power-cycle recovery and the following ordinary warm reboot both
reached `nvm-ready` in polled mode on the same kernel. Each returned all
four command replies. The recovery boot passed both wired gateway echo
tests. On the polled warm reboot, gateway ARP and UDP passed, but
`net_ping` and `net_icmp_conn` both timed out waiting for an echo reply
from the wired gateway. A subsequent live ping from the same boot
received three replies from that gateway, and Chris could ping an
external address. The gateway echo failure was transient. The kernel
fault on the MSI-X boot remains unexplained.

Two more normal warm reboots with `WIFI=probe` brought the same-kernel
polled comparison to three consecutive warm boots. Both new boots
reached `nvm-ready`, completed all four firmware commands, and passed
all 38 post-boot tests. This comparison makes the MSI-X path the next
place to investigate. The sample is too small to identify the cause.

The diagnostic kernel leaves the AX210 paths as tested. Its fatal
kernel-mode exception report includes the scheduler's last return
target on the faulting core and can dump a task-local kernel stack.
It reads the recorded stack allocation through its mapped physical
alias after matching the task to the fault-time CR3. In a QEMU
snapshot, the debugger stopped at `kworker_thread` and redirected
execution to page zero. The report retained the scheduler target
and the seeded `task_exit_with_retval` word even with RBP zero.
All four normal QEMU boot modes also passed.

On the diagnostic kernel, Chris completed ten consecutive warm
reboots with `WIFI=probe-msix`. The persistent log shows ten MSI-X
table setups, ALIVE and PNVM completions, NVM replies, `BT_CONFIG`
acknowledgments and safe stops. Each boot passed all 38 post-boot
tests. The final live report showed eight interrupts, seven RX
interrupts, `replies=4` and `bt_config=1`. This completes the warm
reboot check on the diagnostic kernel. The cause of the earlier
fault remains open.

The same diagnostic kernel then passed an orderly shutdown and a cold
boot after AC power was removed. The persistent log records shutdown
and the Bluetooth controller reset. On the subsequent boot, MSI-X
delivered eight interrupts with seven RX causes. All four firmware
commands completed, PCI bus mastering was disabled, and all 38
post-boot tests passed. Wired SSH retrieved the report.
Chris confirmed that the Bluetooth keyboard and mouse both worked on
this bundle. The same build now has the required P5 boot, shutdown,
wired link and Bluetooth input evidence. The injected MSI-X DMA fault
was exercised in the host model. The earlier P5 kernel fault remains
under investigation.

## Transport and proof

1. Check the live PCI memory and bus-master controls. Before publishing
   any new DMA addresses, force the discrete device to a reset baseline
   with old DMA excluded. Use the reference's generation-specific CSR
   and peripheral register sequence, including MAC access, SRAM/clock
   readiness and the proper reset delay. Bound each wait independently.
   Keep this work in `knet`, with a deadline wake or an existing scheduler
   timing source when hardware cannot yet interrupt. Advance at most a
   bounded amount per pass so DHCP and Ethernet still run.
2. Allocate distinct, page-aligned DMA storage for the image loader,
   LMAC and UMAC sections, paging sections, Gen3 context, scratch and
   peripheral information, RX free/used descriptors and status, and
   the command ring. The host owns each allocation until its device
   pointer is published. Firmware DMA uses physical addresses. Publish
   initialized data before enabling bus mastering or starting the loader.
   Prove the ABI offsets and ring sizes against the pinned register header.
3. Start with one interrupt at the BSP. Use MSI-X entry zero and the
   single-vector cause mapping from the pinned OpenBSD path, after checking
   that the P5's table programming took. The top half acknowledges causes
   and rings `kNetDoorbell`. The worker validates the used-ring cursor and
   buffer token before indexing and validates message lengths before decoding.
   Treat firmware errors
   and timeouts as distinct phases in `/sys/net/wifi`.
4. Accept `ALIVE` from the expected firmware, validate its status and
   extract the SKU. Select PNVM by SKU plus MAC and RF types. Publish
   the matching DMA sections, trigger PNVM load, and wait for its notification.
   Run the required init commands including Bluetooth coexistence
   (`BT_CONFIG`). Confirm a solicited NVM response on the expected
   command sequence, then report the MAC and supported channels. A
   command buffer is reusable after its completion or confirmed transport
   stop. General frame delivery starts in a later slice.
5. Use one worker-owned stop path for startup failure, reboot, and shutdown.
   Close command submission, mask causes and drain or reset before returning
   any device-owned memory. Clear bus mastering and confirm transactions
   have stopped. Retain DMA allocations if the driver cannot prove stop.
   On orderly shutdown, do this while the worker and logger are alive.
   A failed quiesce must leave a visible reason and require a power cycle
   if the platform cannot prove DMA has been contained for a warm reset.

The transport acceptance is a cold boot and repeat following power removal,
ten consecutive warm reboots, and one orderly shutdown. Collect the
`ALIVE`, PNVM, coexistence and NVM-response evidence together with the
stop result. Exercise a fault path and verify the wired link and Bluetooth
on the same build.
