# SSHD.md — the secure door: a Secure Shell server on the telnetd seams

*Design record, Fable, 2026-09-13, written BEFORE code per the known-debt
rule. Chris marks this up; Quinn builds from the marked-up version, one
reviewed slice at a time. telnetd (SERVERS.md) proved the seams — the
listener, the STREAM pty, spawn-with-connection, and the pin that makes a
multi-threaded server's teardown sound; sshd is what makes the door safe
to leave open, and what lets a model run a command on the P5 from its own
harness and read the answer back. Quinn's point, which is the whole
argument: `ssh p5 'cat /sys/net/tcp'` returns bytes into a conversation
with nobody in the loop. Telnet cannot: no exec, no exit status, and
driving it means expect-style scripting that breaks on a slow prompt.*

## Rulings (Chris, 2026-09-13)

- **Ed25519 waits.** BearSSL has no Ed25519, so the host key is ECDSA
  P-256 and the client's key must be too — `ssh-keygen -t ecdsa -b 256`
  on the WSL2 side — until TweetNaCl's verify (public domain, ~800 lines)
  is borrowed. Booked, with the one-liner that turns it on.
- **It must work with the client Chris has:** OpenSSH_9.6p1 Ubuntu
  3ubuntu13.19 on WSL2, whose defaults are the algorithm set below. Not a
  general-purpose sshd; one that this client, out of the box, connects to.
- **Exec before shell.** The slices land the command channel first: it
  needs no pty, and it is the payoff. The interactive shell rides the
  telnetd shape and comes after.
- **Nothing speculative.** No port forwarding, no agent forwarding, no
  SFTP, no compression, no keyboard-interactive, no passwords, no users.
  Each refusal is spelled on the wire the way the RFCs say to refuse, so a
  client hears "no" instead of a hang.

## 1. What OpenSSH 9.6 offers, and what we answer

One algorithm per slot. The client sends its preference list; the server's
list has one entry per slot, and the RFC's rule — the first of the client's
that the server also names — selects it. Every entry is in the 9.6 client's
DEFAULT list, so no `-o` on the client side.

| Slot | Chosen | Why this one | BearSSL primitive |
|---|---|---|---|
| kex | `curve25519-sha256` (RFC 8731) | First modern KEX in the client's list after the post-quantum hybrid it will not get; X25519 is 32 bytes each way | `br_ec_c25519_i31` (`mulgen` for our public point, `mul` for the shared secret), SHA-256 |
| host key | `ecdsa-sha2-nistp256` (RFC 5656) | The only signature scheme BearSSL and the client both hold that is not RSA | `br_ec_p256_m31`, `br_ecdsa_i31_sign_raw` (deterministic nonces, RFC 6979 — no RNG at signing time), `br_ec_keygen`, `br_ec_compute_pub` |
| cipher, both directions | `aes128-ctr` (RFC 4344) | In the client's default list; a CTR stream needs no padding tricks; sidesteps the Terrapin (2023) family, which bit chacha20-poly1305 and the `-etm` MACs | `br_aes_ct64_ctrcbc_init` + `br_aes_ct64_ctrcbc_ctr` — the ONE BearSSL CTR entry point with a full 128-bit big-endian counter, which SSH's counter is (`br_aes_ct64_ctr_run` carries a 32-bit counter and would wrap wrong) |
| MAC, both directions | `hmac-sha2-256` (RFC 6668) | In the default list; plain encrypt-and-MAC, the RFC 4253 layout, no `-etm` | `br_hmac_key_init` / `br_hmac_init` / `br_hmac_out` with `br_sha256_vtable` |
| compression | `none` | | |
| user auth | `publickey` only, key type `ecdsa-sha2-nistp256` | RFC 4252 §7; os64 has no users and no passwords | `br_ecdsa_i31_vrfy_raw` |
| languages | empty | | |

`kex-strict-s-v00@openssh.com` is advertised in our kex list (the 9.6
client advertises its `-c` twin): it costs a reset of the sequence numbers
at NEWKEYS and forbids stray packets before it, and it is what makes the
2023 prefix-truncation attack not apply even though our cipher choice
already avoids it. `ext-info-s` is NOT advertised; the client's `ext-info-c`
is ignored, which the RFC (8308) permits.

**What is refused, and how:** any other host-key type or KEX the client
insists on → `SSH_MSG_DISCONNECT` with `KEY_EXCHANGE_FAILED`; `password`
and `keyboard-interactive` → `USERAUTH_FAILURE` naming `publickey` as the
only method, never a prompt; `direct-tcpip`, `forwarded-tcpip`, `x11`,
`auth-agent-req@openssh.com` → `CHANNEL_OPEN_FAILURE` /
`CHANNEL_FAILURE` with `ADMINISTRATIVELY_PROHIBITED`; a `subsystem`
request (sftp) → `CHANNEL_FAILURE`; a second session channel while one is
open → refused (one session per connection is the whole consumer).

## 2. The wire, in the order it happens

Every number here is a reference to RFC 4253 unless named otherwise.
Quinn will write the packet layer as a PURE ENGINE (bytes in, bytes and
verdicts out, no syscalls) exactly as the telnet engine is written, so the
host harness drives it under ASan at every chunk size — the discipline that
caught four telnet bugs before the kernel ever ran the code.

1. **Identification** (§4.2): we send `SSH-2.0-os64sshd_1.0\r\n` at once
   (the server speaks first here too); read the client's line, refuse
   anything not `SSH-2.0-`. Both lines are inputs to the exchange hash.
2. **KEXINIT** (§7.1): sixteen random cookie bytes (from the DRBG below),
   the one-entry lists above, `first_kex_packet_follows` false. The
   client's whole KEXINIT payload and ours are inputs to the hash.
3. **ECDH** (RFC 8731 / RFC 5656 §4): `KEX_ECDH_INIT` carries the client's
   32-byte X25519 point Q_C. We generate a fresh scalar (DRBG), compute
   Q_S = `mulgen`, K = `mul(Q_C, scalar)` — refuse an all-zero K, the
   low-order-point check RFC 8731 §3 requires — then the exchange hash
   H = SHA-256(V_C, V_S, I_C, I_S, K_S, Q_C, Q_S, K) with every field
   string-encoded and K as an mpint (leading 0x00 when the top bit is set,
   no leading zeros otherwise — the mpint rule, RFC 4251 §5). K_S is our
   host public key blob (`ecdsa-sha2-nistp256`, `nistp256`, the
   uncompressed 65-byte point). `KEX_ECDH_REPLY` = K_S, Q_S, and the
   signature of H: `br_ecdsa_i31_sign_raw` over SHA-256(H) gives r||s,
   which becomes the blob `string "ecdsa-sha2-nistp256", string(mpint r,
   mpint s)` — the one place BearSSL's raw format and SSH's disagree.
4. **NEWKEYS** (§7.3): keys and IVs from K and H by the §7.2 letters —
   A/B the IVs, C/D the cipher keys, E/F the MAC keys, each
   SHA-256(K, H, letter, session_id) extended by the §7.2 chaining rule to
   the needed length (AES-128 wants 16, HMAC-SHA-256 wants 32). session_id
   = the FIRST H, forever. Sequence numbers reset to zero here under
   strict KEX.
5. **Service request** (RFC 4252): `ssh-userauth` accepted, anything else
   disconnected.
6. **Userauth publickey** (RFC 4252 §7): the client first asks "would
   this key do?" (no signature) → `USERAUTH_PK_OK` if the key is in
   `authorized_keys`; then sends the signed request, and the signature is
   verified over `string session_id || the request as sent`. The username
   is READ AND RECORDED in the log line and otherwise IGNORED — os64 has
   no users, so the question a login asks is "does the key-holder ask",
   never "who". Five failures or two minutes without success → disconnect.
7. **Channels** (RFC 4254): one `session` channel; window and packet size
   honoured both ways (ours: window 2 MB, max packet 32 KB; theirs as
   advertised, never exceeded); `WINDOW_ADJUST` sent as we consume.
   Requests, in the order a client sends them:
   - `pty-req`: record cols/rows (and the TERM name, for nothing yet);
     the pty is created at `shell` time in STREAM mode with those
     dimensions. `window-change` later → `os64_pty_resize`.
   - `env`: refused silently (want_reply false is the norm).
   - `exec` — THE PAYOFF: spawn `/bin/husk -c "<command>"` with 0/1/2 on
     three fresh pipes (no pty), pump stdout to `CHANNEL_DATA` and stderr
     to `CHANNEL_EXTENDED_DATA` type 1, stdin from the channel to the
     pipe, and when husk exits send `exit-status` with its code, then
     `CHANNEL_EOF`/`CHANNEL_CLOSE`. husk's `-c` already runs one line and
     exits with its status, pipes and all, so a compound command is free.
   - `shell`: telnetd's shape exactly — `os64_pty_create_stream(cols,
     rows)`, `os64_spawn_seated("/bin/husk", …, master)`, one thread per
     direction between the master and the channel, `SIGWINCH` through
     `pty_resize`, session ends when the master reads EOF (the seats
     emptied). No NVT translation this time: the bytes are the bytes.
   - `signal`, `break`, `xon-xoff`: refused.
8. **Rekey**: the 9.6 client rekeys after 1 GB or ONE HOUR, by sending a
   fresh KEXINIT mid-session. Not optional for a shell that stays up: the
   transport engine treats KEXINIT in the established state as a return to
   step 2 with the same session_id, and new keys take effect at the next
   NEWKEYS in each direction. A server that disconnected on rekey would
   drop every session at sixty minutes, which is exactly the length of a
   debugging session.
9. **Disconnect**: `SSH_MSG_DISCONNECT` with a reason and a one-line
   description on every refusal path, so the client prints WHY. The
   kernel's last-handle-close sends the FIN (SERVERS.md § 1).

**Bounds, spelled once:** packet length ≤ 35000 (the RFC's floor of
required support, and our ceiling), padding 4..255, a KEXINIT list ≤ 4 KB,
a name-list entry ≤ 64, a userauth request ≤ 8 KB, a channel request ≤ 8
KB, a command line ≤ 4 KB (`husk -c` refuses over-long lines itself — the
request is refused before spawn so the client hears it). Every length is
checked against the packet's remaining bytes before it is read; a length
that runs past the packet disconnects with `PROTOCOL_ERROR`. This is the
DER-reader rule from the TLS slices, applied to a second binary format.

## 3. Randomness, keys and files

- **Entropy is `/dev/random`, once per connection** (RANDOM.md, the
  BearSSL rule): 32 bytes seed a `br_hmac_drbg` per session, and the DRBG
  serves the KEXINIT cookie, the X25519 scalar, and padding bytes. An
  unseeded pool (`/dev/random` answering -1) refuses the connection with a
  DISCONNECT that says so, the production-inputs rule — never a weak key.
  ECDSA signing needs no randomness (RFC 6979 in BearSSL).
- **The host key lives in /home** — the USER's disk under the persistence
  doctrine (CLAUDE.md), never rebuilt: `/home/sshd_host_key`, a small
  typed text file: one line `ecdsa-p256 <64 hex digits>` (the private
  scalar) and one comment line with the generation date. Generated on
  first start if absent, with `br_ec_keygen` from a DRBG seeded as above,
  and the SHA256 fingerprint printed to the console in OpenSSH's own
  spelling (`SHA256:<base64>`) so the person at the client can compare it
  against the `accept-new` prompt. The public half is recomputed at every
  start (`br_ec_compute_pub`); nothing derived is stored. A key file that
  does not parse is refused loudly — a server that silently minted a new
  identity would strand every client's known_hosts.
- **`authorized_keys` is on the conf ladder** (CLAUDE.md § The config
  search path), MERGING like `hosts` and `crontab`: `/home/authorized_keys`
  layers over `/etc/authorized_keys`, and a merged file can only add.
  OpenSSH's own line format — `ecdsa-sha2-nistp256 <base64 blob>
  [comment]` — so `cat ~/.ssh/id_ecdsa.pub >> /home/authorized_keys` on
  the P5 (via telnet, once) is the whole enrolment. The blob is decoded
  and its inner type name and curve name checked against the outer word;
  a line of any other key type is skipped with one log line naming it, not
  an error, so a file shared with a Linux box still works. No options
  field (`from=`, `command=`) — refused by name if present, so a line that
  meant to restrict does not silently permit.
- **Config**: `sshd.conf` on the ladder — `port = 22` and nothing else in
  v1. Launch is the CRON/TELNETD shape: an `SSHD` cmdline token, deliberately
  absent from the lifeboat entry, and it DOES NOT SHIP TO THE P5 by itself
  (the boot menu does not travel; say so out loud every time). `sshd &` at
  a prompt is the other door, and `sshd -session` is the child the
  listener spawns with the connection as 0/1, telnetd's single-binary shape.

## 4. The seams it stands on, and what it asks of them

Nothing new in the kernel is expected. Everything below exists and was
proven by telnetd on 2026-09-12/13:

| Need | Seam |
|---|---|
| Listen, accept | `os64_net_announce`, accept-is-a-read with `os64_read_for` patience so the reaper runs while idle (SERVERS.md § 1) |
| Session process with the socket as its stdio | `os64_spawn_redirected(path, argv, conn, conn, -1, 0)` — HANDLE_NET_TCP crosses the spawn boundary with a reference; the last close sends the FIN |
| The shell | `os64_pty_create_stream`, `os64_spawn_seated`, `os64_pty_resize`, EOF on the master when the seats empty (SERVERS.md § 2) |
| exec's three streams | `os64_pipe` ×3 + `os64_spawn_redirected` with all three, `os64_wait` for the exit code |
| Two threads sharing the connection and the master | `os64_thread`; the pin (handle.c § The pin) is what makes a session's teardown sound with a thread parked in a read |
| Crypto | the BearSSL foundation archive, linked STATICALLY the way `bearssltest` links it (`APP_EXTRA_LIBS` in userland/GNUmakefile). `libtls.so` exports the TLS byte API only; raw primitives do not become public ABI for this |
| Randomness | `/dev/random` |
| Where files are | `conf_find` through `SYSCALL_CONF_RESOLVE` for `authorized_keys` and `sshd.conf`; `/home/sshd_host_key` by name (it is state, not config — the ladder is for what a person edits) |

One thing to KNOW rather than build: a session child holds the connection
as 0 and 1 and reads it from one thread while writing it from another. The
rd1 lesson stands — `tcp_conn_write` drops its lock under backpressure, so
ONE THREAD WRITES THE SOCKET, and the other hands it bytes through a
mailbox (telnetd.c's SPSC ring is the pattern). In sshd the outbound thread
also owns the CIPHER STATE for its direction, so the two directions never
share a key schedule or a sequence counter: the inbound thread decrypts and
verifies, the outbound thread encrypts and MACs, and nothing crosses.

## 5. Verification — the harness comes with the slice

- **Host, pure engine, under ASan at every chunk size**
  (`tools/test_sshd_host.sh`, the telnet harness's shape): packet framing
  and padding, the mpint rule (leading zero, no leading zeros, zero
  itself), name-list selection, the KEX hash against a captured OpenSSH
  transcript (record one with `ssh -vvv` and the server's DRBG seeded
  from a fixed vector — the engine takes its randomness as an input for
  exactly this reason), X25519 against RFC 7748 §6.1's vectors, ECDSA
  sign/verify round trip, HMAC-SHA-256 against RFC 4231, AES-128-CTR's
  128-bit counter carry across a block boundary whose low word is
  0xFFFFFFFF (the `ctr_run` trap, made a test), a truncated packet at
  every length field, and the rekey path.
- **Differential, the real client**: `ssh -p 2222 -o
  StrictHostKeyChecking=accept-new -i tools/sshd_test_key
  os64@127.0.0.1 'ls /'` against QEMU with `hostfwd=tcp::2222-:22`, and
  the exit status carried through (`ssh … 'false'; echo $?` → 1). Then a
  pty session with `ssh -t` driven through a pty on the host, resizing it
  and reading `/proc/self/tty` over the wire, the SIGWINCH proof telnetd
  already passed. `tools/sshd_probe.py` scripts these the way
  `telnetd_probe.py` does for telnet — including the DROP mid-session,
  which is the pin's shape.
- **Fixtures in /tests**: an engine fixture the way `/tests/bearssltest`
  carries BearSSL vectors, so the guest proves the arithmetic it links.
- **The known-hosts moment is a test too**: the fingerprint the server
  prints at generation must equal what the client shows on first connect.

## 6. Slices, each reviewed and merged on its own

1. **Transport** — identification, KEXINIT, curve25519-sha256, the host
   key (generation, storage, fingerprint), NEWKEYS, aes128-ctr +
   hmac-sha2-256 both ways, rekey, DISCONNECT. Acceptance: `ssh -vvv`
   completes the key exchange, accepts `ssh-userauth`, and is refused at
   the first userauth request with the method list — which proves the
   whole encrypted transport under the real client with no auth code yet.
2. **Auth** — `authorized_keys` on the ladder, the two-step publickey
   dance, the failure budget. Acceptance: the same command succeeds with
   an enrolled ECDSA key and is refused with an unenrolled one, and with
   an Ed25519 one, BY NAME.
3. **Channels, exec first** — the session channel, windows, `exec` through
   `husk -c` with the exit status, then `pty-req` / `shell` /
   `window-change` on the telnetd shape. Acceptance: `tools/sshd_probe.py`
   end to end, and a model running a command on the P5 from its harness.

Estimate for the three together: 2,500–3,500 lines of C, the telnet slice's
size again, with the engine's host harness a third of it. Codex earns its
rounds on slices 1 and 2 (crypto and a ring boundary); slice 3 is reviewed
here first, per the reviewer-matches-the-risk rule.

## 7. Booked at birth (DEBTS.md rows follow the code)

- **Ed25519** host and client keys (borrow TweetNaCl's `crypto_sign_open`
  and keypair; the client's default key type). Reversing condition: Chris
  wanting to use his existing `id_ed25519`.
- **SFTP / scp** on the same channel layer — retires the os64serve and
  FTP transfer fallbacks. Reversing condition: the first time a model
  needs to move a file to the P5 and reaches for FTP.
- **Port forwarding**, agent forwarding, `keyboard-interactive`: no
  consumer.
- **Per-user anything**: os64 has no users. The DIVERGENCES row: "a login
  asks whether the key-holder asks, never who" — the day os64 grows users
  the `username` field is already read and logged.
- **A second session channel per connection** (ControlMaster multiplexing
  on the client): one is the consumer; refused by the spec so the client
  opens a new connection instead of hanging.

## 8. Questions for review (Chris marks these up)

1. `/home/sshd_host_key` as a typed text line, or OpenSSH's own PEM so
   `ssh-keygen -y` can read it? The text line is os64's shape and eight
   lines of code; PEM is interop nobody asked for.
2. `authorized_keys` MERGING across the ladder (chosen above, hosts'
   rule) versus first-hit-wins. Merge means `/etc/authorized_keys` in a
   shipped image would enrol a key everywhere it boots — which is why the
   shipped image carries NONE, and the rule stays honest only while that
   holds.
3. Port 22 by default on the P5, or something else while it is the only
   thing behind ICS? The router is Chris's; the spec has no opinion.
4. Slice 1's acceptance stops at the userauth refusal on purpose — is that
   a merge, or does Chris want 1 and 2 as one PR?

## Protocol references

RFC 4251 (architecture, data types, the mpint rule), 4252 (userauth),
4253 (transport), 4254 (connection and channels), 4344 (aes-ctr), 5656
(ECDSA and ECDH in SSH), 6668 (hmac-sha2), 8308 (ext-info), 8731
(curve25519-sha256), 7748 (X25519, and its test vectors), 6979
(deterministic ECDSA), OpenSSH's `PROTOCOL` file for
`kex-strict-*-v00@openssh.com`, and the Terrapin paper (Bäumer, Brinkmann,
Schwenk, 2023) for why the cipher and MAC were chosen the way they were.
