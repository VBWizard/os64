# SSHD — remote commands and interactive shells

Design: Fable, 2026-09-13. Implementation: Quinn, stacked on
`fable/servers` at `8b5e974` in `.worktrees/sshd`, branch `codex/sshd`.
This record incorporates the server-branch API audit into Fable's design.

The primary consumer is a command runner:

```sh
ssh -i ~/.ssh/id_ecdsa os64@p5 'cat /sys/net/tcp'
ssh -i ~/.ssh/id_ecdsa os64@p5 'false'
echo $?  # 1
```

`ssh -t -i ~/.ssh/id_ecdsa os64@p5` opens an interactive husk on a STREAM
PTY. There is one session channel per connection. The daemon is userland;
the kernel addition is the `SSHD` token's existing-style launch hook. No new
syscall, pipe operation, PTY operation, or cryptographic ABI is introduced.

## Identity, enrollment, and starting the service

The client key must be ECDSA P-256:

```sh
ssh-keygen -t ecdsa -b 256 -f ~/.ssh/id_ecdsa
```

Enroll the **public** line from `id_ecdsa.pub` in the target's
`/home/authorized_keys` through an existing trusted access path. Do not copy
the client private key to the target. The files named `authorized_keys` on
the configuration ladder are merged, with duplicates harmless. The image
ships no authorized keys. Supported lines are:

```text
ecdsa-sha2-nistp256 <base64 public-key blob> [comment]
```

Other recognizable key types are skipped with their type logged. Options
such as `from=`, `command=`, and `restrict` refuse the key file; they are not
stripped into unrestricted grants. Invalid supported keys, truncated lines,
and excess key/file capacity also refuse the session. There are at most 64
distinct enrolled keys; each file is at most 128 KiB and each line fits 2047
bytes. The outer type, inner type, curve name, point length, and P-256 point
are validated before enrollment.

`/home/sshd_host_key` is persistent machine state, outside the config ladder:

```text
ecdsa-p256 <64 hexadecimal digits>
# generated epoch <UTC seconds>
```

The listener reads this key or generates it on first start using seeded
`/dev/random` and BearSSL's EC key generator. Exclusive creation prevents
replacement of an unreadable or concurrently created identity. A malformed,
partial, oversized, or unreadable existing file is refused. A persistence
failure does not silently generate a replacement on the next boot. The
public point is recomputed, and its OpenSSH `SHA256:` fingerprint is printed
at startup. Compare that fingerprint with the client's first-connect result.
The key is not an OpenSSH private-key file.

`sshd.conf` uses the ordinary configuration ladder:

```text
port = 22
```

Start `sshd &` at a prompt, or add `SSHD` to the intended boot entry's kernel
command line. Standard boot entries do not automatically enable the service;
the lifeboat remains without it. **Building this branch does not deploy it
to P5. The P5 boot menu does not travel with an ordinary refresh.**

A public key grants machine access. os64 has no user accounts: the requested
username is recorded for an accepted login but is not an authorization role.
Password and keyboard-interactive authentication are refused without prompts.

## Protocol contract

The supported client is OpenSSH 9.6p1, including the Ubuntu
`3ubuntu13.19` installation used for validation. No algorithm override is
needed for an ECDSA P-256 client key.

| Slot | Algorithm |
| --- | --- |
| Exchange | `curve25519-sha256` |
| Host and authentication key | `ecdsa-sha2-nistp256` |
| Cipher in each direction | `aes128-ctr` |
| MAC in each direction | `hmac-sha2-256` |
| Compression | `none` |

The server identifies as `SSH-2.0-os64sshd_1.0`. It advertises
`kex-strict-s-v00@openssh.com` in the initial exchange, recognizes the client
counterpart there, and resets each direction's packet sequence at NEWKEYS
when strict KEX is negotiated. Extension-info is not advertised. Cipher and
MAC state belong to separate receive/transmit contexts.

BearSSL is linked through the private foundation archive. AES uses
`br_aes_ct64_ctrcbc_ctr`, whose counter carries across the full 128 bits;
the 32-bit-counter CTR entry point is unsuitable. The MAC authenticates the
sequence and plaintext SSH packet before encrypted input reaches auth or
channel handling. X25519 rejects wrong-length and all-zero shared secrets.
The X25519 output bytes are interpreted in network order for SSH's mpint,
without reversing that byte string. BearSSL's scalar API is big endian;
RFC 7748's scalar test vector is converted at the test boundary.

The exchange hash covers the identification strings, complete KEXINIT
payloads, host public blob, both ephemeral points, and the mpint secret.
ECDSA signs SHA-256 of that exchange hash. Raw r/s values are encoded as
canonical positive SSH mpints, including the maximum case where both need a
leading zero. Key derivation uses RFC 4253's A through F labels. The first
exchange hash remains the session ID through rekeys.

Each connection obtains a fresh 32-byte `/dev/random` seed for its HMAC
DRBG. Unavailable entropy prevents session initialization; there is no weak
seed fallback. The DRBG supplies cookies, ephemeral scalars, and padding.
Private scalar copies, pending derived keys, and the engine are wiped when
those lifetimes end normally.

Authentication validates the signature over `string session_id` plus the
original userauth request prefix. An enrolled-key query is not an
authentication success. Five failed requests or 120 seconds without success
end the connection. The listener caps concurrent session children at 16.

Client-initiated rekeys are supported while a command or shell is running.
The server also initiates after 512 MiB in either direction or one hour.
A KEX has a separate 120-second completion deadline. Connection replies
received in flight before the client's KEXINIT are deferred until NEWKEYS;
application output pauses during KEX. The deferred reply budget is 8 KiB.

## Commands, channels, and backpressure

The server advertises a 2 MiB receive window and 32768-byte data packets.
Peer window and maximum-packet limits constrain outgoing data. Window
addition overflow, data exceeding the advertised window, invalid channel
numbers, malformed lengths, and invalid protocol transitions are refused.
Transport packet length is capped at 35000 bytes. KEX name-lists are capped
at 4096 bytes and names at 64; userauth and channel requests are capped at
8192 bytes.

`exec` spawns `/bin/husk -c <command>` with three independent pipes.
Channel data feeds stdin; stdout uses CHANNEL_DATA; stderr uses
CHANNEL_EXTENDED_DATA type 1. SSH EOF closes stdin after queued input drains.
After the child exits and both output streams drain, the server sends
`exit-status`, EOF, and CLOSE, in that order. A close handshake has a bounded
wait. Backpressured network output also has a finite no-progress deadline.

**Audit corrections to the original design:**

- This branch limits both a spawn argument and husk's command line to 255
  bytes. SSH refuses longer commands before spawning; it does not advertise
  the draft's 4 KiB allowance. Shell syntax is husk's: `exit N` ignores N and
  `>&2` is not POSIX descriptor duplication on this baseline. The SSH
  fixture's `-streams` and `-stderr` modes test status 7 and raw stderr
  without relying on those shell features.
- Ordinary pipe reads reject finite patience on the server branch. stdout
  and stderr therefore have dedicated blocking readers, each feeding a
  bounded 64 KiB SPSC queue. A separate stdin writer owns a bounded 2 MiB
  queue. The main event loop alone reads/writes the socket and owns the
  engine, eliminating cross-thread cipher/rekey coordination. Worker queue
  publication uses release/acquire ordering. Kernel handle pins protect
  outstanding operations during task teardown.
- os64 renders a newline as a new row at column zero. The interactive
  adapter emits CR LF for a lone LF so OpenSSH's raw terminal renders that
  same motion, preserving existing CR LF pairs across read boundaries.
  This is terminal output adaptation; exec stdout/stderr stay byte-exact.
- An exec request with an allocated PTY is refused; the interactive path is
  `pty-req` followed by `shell`. Shell requests without a PTY are refused.
  `pty-req` records TERM and dimensions; `window-change` resizes the STREAM
  PTY. Terminal-mode opcode framing is checked, while os64 owns the line
  discipline. Interactive EOF uses Ctrl-D; a STREAM master lacks an
  independent write-half close.
- Disconnect closes command pipes. A non-PTY command that does not interact
  with those pipes can outlive the connection; this is not a job-cancellation
  interface. Interactive teardown closes the PTY master and hangs up the
  seated shell.

Forwarding channel types and second session channels receive OPEN_FAILURE.
Unsupported global requests receive REQUEST_FAILURE when requested.
Unsupported channel requests, including env, subsystem/SFTP, agent
forwarding, signal, break, and xon-xoff, receive CHANNEL_FAILURE when
requested. No passwords, per-user access control, Ed25519, SFTP/scp,
compression, forwarding, or multiplexed sessions are implemented. Reversing
conditions are recorded in DEBTS.md.

## Validation and review boundaries

```sh
tools/test_sshd_host.sh --build-dir /tmp/sshd-host
python3 tools/sshd_probe.py 127.0.0.1 --port 2222 \
  --key /tmp/sshd-host/client --known-hosts /tmp/sshd-qemu/known_hosts
```

The host build runs the same pure engine and BearSSL primitive sources
under ASan/UBSan. A loopback-only POSIX adapter uses fixed **test** host-key
and DRBG inputs; it is not a second deployable daemon. OpenSSH tests cover
authentication, separate stdout/stderr, exit status, refusal paths, binary
streams larger than the window, repeated forced rekeys, and independent
host-fingerprint serialization. The host driver runs local test commands;
its keys and known-hosts file are generated in the temporary test directory.

`/tests/sshtest` runs the codec/crypto/engine fixture inside os64. Additional
fixture modes supply explicit stdout/stderr markers with status 7 and copy
binary stdin to stderr. `sshd_probe.py` checks these, large binary transfers,
interactive shell/resize, and a fresh command after an abruptly dropped
interactive client. `--fixture` selects an explicitly installed test copy.

An initial fixture named `sshdtest` exposed a separate task-address collision:
its assigned link base equalled `USER_TASK_MEMORY_BASE` (`0x10000000`). Its
first PLT fetch encountered zeroed NX memory, and cleanup panicked in the
allocator. Renaming it to `sshtest` places this fixture at `0x22000000` on the
stacked build. That avoids the observed collision; it does not fix the
underlying app-base/task-allocation overlap. DEBTS.md records the separate
kernel-scope follow-up.

External crypto/implementation review is still required before a production
merge. Host/QEMU checks and user-reported P5 results are recorded separately
below. Implementation review is deferred until the parent server branch is
ready.

## Recorded validation — 2026-09-13

Final parent: `fable/servers` at `8b5e974`. Full strict `make -j8` passed.
The complete staged diff passes `git diff --cached --check`. GCC's analyzer
reported no diagnostics for the transport and connection engines.

- Host ASan/UBSan fixture: **683 checks, 0 failures**. OpenSSH 9.6p1 passed
  separate stdout/stderr and status 7, `false` status 1, unenrolled/Ed25519
  refusal, overlong exec refusal, 3 MiB binary echo with repeated client
  rekeys, server-initiated rekeys with data in flight, oversized-window
  stderr output, unsupported PTY exec refusal, and independent fingerprint
  comparison. Evidence: `/tmp/sshd-host/final.log`.
- Private q35 QEMU, 4 vCPUs, 2 GiB, separate NVMe root and copied home
  images, loopback port 2222: **683 guest checks, 0 failures**; binary
  stdin/stdout and stderr each passed 3 MiB; explicit stream markers returned
  status 7; forced rekeys passed. Interactive Enter, CRLF rendering,
  88x30 to 103x41 live resize, and a fresh command after an abruptly dropped
  interactive client passed. Strict host-key checking succeeded after
  reboot with the retained home disk. Evidence:
  `/tmp/sshd-qemu/final-probe.log`, `/tmp/sshd-qemu/final-probe/pty.log`,
  `/tmp/sshd-qemu/serial-final.log`.
- `tools/stale_refs.sh` reported documentary references to SSH message
  names, the example `id_ecdsa` path, and the newly added probe script
  names. Each remains intentional and current; there were no new
  superlative claims. Evidence: `/tmp/sshd-stale-refs.log`.
- Chris deployed the branch binaries to the P5 and rebooted, enrolled an
  ECDSA P-256 public key, and confirmed SSH login and remote `ls /` output.
  A remote `top` command streamed updates every second. After about half an
  hour of exploratory use, he reported no SSH failures. These are
  user-reported hardware results, not an instrumented repeat of the host
  and QEMU suites.

The earlier fixture-address collision is recorded above and in DEBTS.md;
the final guest serial log contains no panic or segmentation-fault report.
External review and a production merge remain pending.

## References

- [RFC 4251](https://www.rfc-editor.org/rfc/rfc4251.html): SSH types and mpints.
- [RFC 4252](https://www.rfc-editor.org/rfc/rfc4252.html): public-key authentication.
- [RFC 4253](https://www.rfc-editor.org/rfc/rfc4253.html): transport and key derivation.
- [RFC 4254](https://www.rfc-editor.org/rfc/rfc4254.html): channels and exit status.
- [RFC 5656](https://www.rfc-editor.org/rfc/rfc5656.html): ECDSA encoding.
- [RFC 8731](https://www.rfc-editor.org/rfc/rfc8731.html): SSH X25519 encoding.
- [OpenSSH 9.6 PROTOCOL](https://github.com/openssh/openssh-portable/blob/V_9_6_P1/PROTOCOL): strict KEX.
