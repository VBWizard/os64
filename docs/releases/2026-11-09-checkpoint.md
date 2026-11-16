# Checkpoint 2026-11-09 — userland to master

*The first per-feature merge to master, and the one that resets the
baseline so the rest can be small. master last took userland on
2026-08-27 (#38). Between that day and this one, 459 pull requests merged
into userland (3,018 commits; 10 in August, 357 in September, 376 in
October, 96 in November), every one tested on the P5 before Chris merged
it. This note is the release story Quinn asked for: what changed, by area,
with the PR numbers, and how the combined state was checked. The design
record for each thing is in `docs/design/` (completed and pending); DEBTS.md
is what is consciously unfinished.*

## How this checkpoint is verified

- Every PR below passed Chris's P5 test before it merged; that is the
  acceptance evidence, and it is not repeated here.
- The combined state at the merge point, before the merge: a clean build
  from `distclean` (kernel, userland, ISO, both disk images); `make
  fsck-ext2` green; every host suite green through `tools/host_suites.py`,
  new with this checkpoint, since there was no one thing that ran them all
  (167 suites; the ones it skips, and why, are in its manifest); a QEMU
  boot of the default entry to `boot complete` with the in-OS suite green,
  and one of the FAT lifeboat entry; a last P5 install from the merge
  point.
- Mutant runs are not repeated: this merge adds no code, and each slice's
  mutants ran when it landed.
- **Results (2026-11-09, on `fable/checkpoint-2026-11` at userland HEAD
  plus the harness repairs below).** Clean build from `distclean`: kernel,
  userland, ISO and both images, under a minute on ccache. `make
  fsck-ext2`: both ext2 images clean before the boots and again after the
  default boot had written to them. Host suites: **164 passed, 0 failed,
  4 skipped** of 168 (the skips, each said by name: two need the WPT
  checkout `tools/wpt_fetch.sh` fetches, two need the Python modules
  tinycss2, cssselect2 and html5lib, which this machine does not have).
  QEMU, headless: the default ext2 entry mounted its root read-write and
  passed 37 pre-boot, 34 post-boot and 7 LATE tests; the FAT lifeboat
  entry mounted FAT and passed the same 37, 34 and 7. P5: Chris's pass
  from the merge point, before he merges.
- **What the first full run found, and fixed here.** The first pass
  failed 19 of 167. None was a defect in product code. Three were the
  runner's own manifest (suites that take an output directory) and two
  were prerequisites it had not declared. The other **fourteen were stale
  harnesses**: suites nobody had run since the code under them moved,
  some for seven weeks. By the PR that left them behind: the loopback
  slice (#120, the TCP harness's listener signature; #123, a handle kind
  the syscall-lifetime harness lacked), the inbound-servers split of write
  into a pinned half (#101, the TCP write harness), the frame table (#230,
  the memory-try harness, rewritten for it), the wake owed (#282, task
  wait), the damage merger shared by both rings (#316, gclock, dataview,
  Roam and the menu mutations), file associations (the Roam harness),
  the toolbar (#254 era, the action gallery), the canonical-ceiling check
  (#340, spawn argv), keep-alive's pool (#345, the os64get harness, four
  layers deep), ccache's compiler banner (#289, the math suite's eval),
  the icon catalog's stats (the icon theme stub), the memory census's
  three heap exports (#505, the JS port test) and five icons added on
  2026-10-29 (#415, an exact count in the os64get routing case, now a
  floor). Every fix is in tools/ or a suite's own test file; the one
  behaviour change is the Roam activation case, which now activates an
  unlisted row because activating a file really opens it since file
  associations (test_roam_launch_host covers the open). Mutants were not
  re-run: no product line changed.

## Kernel

- Memory: the FRAME TABLE allocator (#230), `/sys/memory`; a declared
  main-thread stack (#441); stack overflows named on the glass and a big
  frame that can no longer leap the guard (#422); user pointers bounded by
  the canonical ceiling (#340); page zero kept unmapped (#70, #78).
- CPUs: one number for a CPU, the APIC ID at the hardware's edge (#320);
  SPEC_CTRL written on every thread (#296); PINCORE/PINCPU (#297); kernel
  time accounted per thread by cause (#292); user and system CPU time
  (#287, #290).
- Scheduler and syscalls: a wake owed is paid by the park (#282); write's
  patience (#85); 64 handles per task (#146); `micros()` and arrival wakes
  (#145); atomic exclusive create (#58), rename policies (#56); a close
  that could not commit is heard in ring 3 (#118); the LATE test phase
  (#42); shared objects revalidated against the libraries beneath them
  (#39); the kernel's memcpy is libos64's (#309).
- Hardware: ACPI walked properly (#318); PCI functions as devices (#321);
  NVMe's register window at its HHDM alias (#68); the r8125's ring,
  watchdog and PHY (#66, #76, #307); AX210 Bluetooth with LE keyboard and
  mouse (#235, #248); entropy and the RNG (#75, #71); the TSC's fixed
  exchange rate (#105).
- Console and terminal: the escapes ANSI art asks for and the Ctrl key's
  missing half (#81); raw mode (#80); the terminal obeys and the glass
  gets a colour (#57); console fonts, slices 1 to 5 — the cell from the
  face, PSF2, reflow, `/sys/console/font`, `vtfont` and an outline face
  rendered in ring 3 (#111, #112, #114, #115, #116, #117); mouse wheel
  across VTs and GUI (#134); PTY history and fallible allocation
  contracts (#152).
- Filesystems and mounts: mount, unmount and admin (#45); bootenv.conf
  (#43).

## Networking and TLS

- TCP: a send window and congestion control (#65), round-trip
  measurement and the retransmit timer (#62), out-of-order data held in
  the ring (#61), SYN and FIN retransmission (#49), window scaling (#88),
  `/sys/net/tcp` (#46), loopback (#120); the bottom half — knet, the
  doorbell and the RTL8125's MSI, 33 to 180 Mbit/s on the P5 (#67).
- Inbound: TCP listener, stream PTY and telnetd (#101); sshd with
  authenticated remote execution and shells (#102), 4 KiB commands
  (#132), local forwarding (#121).
- TLS: the pinned BearSSL foundation and vectors (#74), the private client
  engine and certificate policy (#77, #96), the trust bundle (#79, #90),
  randomness and UTC (#82), the byte client as libtls.so (#83), the owned
  TCP transport (#87), Microsoft certificate extensions (#245); **TLS 1.3
  in-house** — key schedule and records (#241), the authenticated client
  engine and the bounded 1.2 fallback (#242), guest acceptance against
  public sites (#244).
- HTTP: chunked bodies and redirects (#60), gzip (#54, #50, #55), POST
  bodies (#140), the per-hop cookie and Referer hooks (#141), the fetch
  machinery as libfetch (#92), native HTTPS in os64get (#89); keep-alive,
  one connection for many requests, and each preset's own Accept and
  header order (#345).
- Clients: whois (#48), FTP (#86), Telnet (#84), gopher (#64), one URL
  parser (#63), os64get's archive and staging (#69); the chaos rig (#51).
- Remote access, six parts: loopback, sshd forwarding, `/dev/glass`, HID
  hands, vncd, ZRLE (#120 to #125).

## Userland libraries and tools

- libos64: arenas (#113), allocation capacity and byte search (#188), copy
  and fill by size (#285, #220), the work pool's admission and 1024-slot
  table (#428, #435), background jobs without blocking the GUI (#139),
  owner-task doorbells (#138); every library relinks when its object list
  changes (#193).
- Commands: tar (#47), find and the tree walker (#104), cut (#218), sort
  (#311), killall (#131), mkdir -p (#150), ls's columns (#119) and long
  directories (#281), gzip (#59), husk's wrapped caret (#106) and
  argument capacity (#346).
- Monitoring: shared system monitoring, htop and top (#186), sysstat
  (#308), Pulse (#514), kGuiLock's counters (#291), the compositor's
  counters (#294).

## GUI, desktop and applications

- Compositor: occlusion, each damaged pixel painted once (#306), rows
  copied with memcpy (#310), libui's short damage list (#316), per-window
  minimum sizes (#108), set_title (#341), native popups (#257).
- Toolkit: shared actions, shortcuts and status (#254), menus (#259,
  #260), Open/Save dialogs (#319, #322), the directory browser (#267),
  text fields, lists and the colour picker (#127), UTF-8 text rules
  (#126), scalable fonts and live settings (#110), web font families and
  the provider cache (#143).
- Appearance: the Workshop with composable themes (#107), the palette
  customizer (#109), Frame Studio and prepared decorations (#128, #133),
  icon sets and Roam Classic (#261), the Yonder-XP pack (#353).
- Apps: Roam (#264, #267, #325), Scribe's toolbar and navigation (#353,
  #497), gclock (#144), gterm scrollback (#151), Control Center's mouse
  settings (#249), gview's GIFs and blending (#135 to #137), window
  placement remembered (#334, #478).

## Images

- JPEG and the shared image loader (#91), PNG (#53), GIF decoding and
  playback (#135, #136, #142, #207), WebP stills (#251), pictures kept at
  their drawn size (#444), responsive images and shared raster ownership
  (#513).

## yonder — the browser

**The engine.** libhtml's streaming parser and conformance harness (#95),
libpage (#100, #103), LAYOUT.md and libflow's five slices (#153 to #157,
#147, #148), libgarb — the CSS parser, Selectors, values, the cascade,
linked sheets (#149, #165 to #171), positioned layout P1 to P4 (#175 to
#178), Flexbox (#181, #182), Grid (#183), floats (#252), transforms
(#255, #256, #258, #266), filter, backdrop-filter and blend modes (#445,
#446, #449), generated content and counters (#433, #434), the web face
and zoom (#213, #215), every background layer (#208 to #210), the box's
edge and blend (#201 to #203), web fonts (#277 to #280), CSS animations
in place (#265, #269 to #276), relayout 5a to 5d (#412, #424, #430 to
#432, #442), `:has()` cascades (#511), the layout profile (#512).

**JavaScript and the DOM.** libmath (#191), libjs on QuickJS (#189, #192,
#194 to #198, #200), libdom (#214), the DOM slices D1 to D11 — verbs on a
finished document, scripts and the parse, the stream on the window's
thread, document.write, the loop, teardown, synchronous geometry, classic
widgets (#190, #199, #211, #212, #216, #217, #223 to #229, #232, #233),
Modern Web slices 0 to 8 (#327 to #333, #336, #338, #339, #347, #352,
#354, #357, #364, #366, #367, #370, #375, #377, #383, #387 to #389, #392,
#395 to #397, #401), custom elements and MutationObserver (#403, #407,
#411, #414), Shadow DOM D2a to D4b (#483, #484, #486 to #488), Ranges and
Selection R1 to R4 (#489, #490, #494 to #496), Streams S1 to S3 (#455,
#457, #459), Canvas 2D C1 to C6 with pointer events and animation frames
(#402, #405, #406, #408, #410, #447, #450 to #456, #458), SVG (#355, #356,
#358 to #360, #363, #365, #368, #369, #372, #373, #376, #378 to #380,
#382, #384 to #386, #391, #394, #398 to #400), Intl (#471, #473 to #476,
#479), Intl.Segmenter (#499), crypto.subtle tiers 1 to 3b (#502 to #504,
#506), observers (#460, #462, #472), Performance timing (#482, #485),
TreeWalker and friends (#467 to #470), MessageChannel (#493),
XMLSerializer (#500), setAttributeNS (#501), the CSS namespace (#464),
Web Animations slice A (#515).

**Identity and the wire.** A preset is a whole browser identity (#343);
keep-alive and per-preset headers (#345); cookies kept across restarts
(#350); the request cache (#184, #425, #426); Fetch, XHR, CORS and
origins (#333); import maps (#366); ES modules (#352).

**Memory and performance.** perf-1 to perf-6 — -O2, the paint cache,
scratch kept between paints, in-place animations (#283 to #298, #313,
#317); budgets — the pool at 8 GiB, pictures at 1 GiB, a page's document
side at 1024 MiB, scripts at 512 (#429, #477, #509); the memory census in
every page record (#505, #507); bytes outside the page's heap (#463,
#465); text-run heap traffic (#439); form controls repainted only when
changed (#481).

**Diagnostics and the person.** The page file and the badge (#234, #236),
the census batches (#239, #243, #247), failures said on the console (#381,
#437), why yonder leaves (#315), downloads with a live list (#409, #413,
#415), the right-click menu (#404), dropdowns (#361), dark pages (#238),
the Settings window (#179), Ctrl+L (#351), a user's guide (#185), the
honey-do list (#393), wend — the line-mode browser yonder grew out of
(#98).

## Build and test infrastructure

- ccache in front of the cross compiler and under the host suites (#289,
  #299); leak detection in the sanitizer runners (#180); the parallel
  mutant runner and its honesty fixes (#299, #304, #305, #312, #324); the
  WPT runner (#327, #339); layout measurement through redirected console
  output (#436); the libpage refresh server and chaos rig for the wire
  (#51).

## Known unfinished at this checkpoint

Web Animations slice B is in progress on a stacked branch; the HTTP/2 arc
(HTTP2.md, its tracker) and the iframe and worker designs are written or
being revised and not yet built; DEBTS.md carries everything else, each
row with its trigger. None of it makes the state at this merge
incoherent: every feature here ships behind its own switch or stands on
its own.
