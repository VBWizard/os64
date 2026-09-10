# Public roots for os64 HTTPS

This is a proposed public trust snapshot, separate from the native HTTPS
application slice. It supplies a candidate for public-site checkout without
changing certificate-policy code or automatically installing trust.

## Source and compatibility

The source is curl's Mozilla CA extract dated **2026-08-13**, downloaded from
<https://curl.se/ca/cacert.pem> over host-verified HTTPS on 2026-09-10. The
published checksum at <https://curl.se/ca/cacert.pem.sha256> matched:

```text
f66dff1bdf8f96060b8177976f8b7d9254bc89bc4db933d769f7384d28480bc9
```

The source has 121 certificates. os64's current anchor loader accepts 104 and
refuses 17. The candidate contains exactly the accepted certificates, with
unchanged DER bytes and original ordering. Certificate labels in the source
include non-ASCII text, which the bounded ASCII PEM loader rejects; the
candidate replaces explanatory text with an ASCII provenance header.

The exclusions are recorded individually by subject, certificate SHA-256,
and policy reason in [manifest.json](trust/mozilla/2026-08-13/manifest.json):
7 DER-policy refusals, 4 unsupported extensions, 3 signature-policy refusals,
2 anchor-policy refusals, and 1 SAN-policy refusal. These are compatibility
exclusions, not declarations that those CAs are malicious or invalid under
other clients' policies. For example, a zero certificate serial is outside
os64's positive-serial policy even on a trusted root.

Candidate [roots.pem](trust/mozilla/2026-08-13/install/roots.pem): **104 roots**, 154679
bytes, SHA-256:

```text
6216af976e9de71b21b46a4d721c6b6f349f5218d1da593beb04b677676c96a5
```

This is an **os64 compatibility subset**, not the complete Mozilla browser
trust policy. curl's extraction drops browser-side domain and other root
constraints that are not encoded in the certificates. os64 enforces its
certificate extension policy but does not implement those external browser
restrictions, revocation checks, CT verification or alternate path building.
See [curl's extraction notes](https://curl.se/docs/caextract.html) and
[TLS.md](TLS.md). Websites depending on excluded roots or unsupported chains
may fail. Do not add arbitrary peer-supplied certificates to make them pass.

The original bundle, checksum, derived bundle, exclusion manifest, and
[MPL-2.0 license](trust/mozilla/2026-08-13/LICENSE-MPL-2.0.txt) are retained
under `trust/mozilla/2026-08-13/`. The certificate data is Mozilla-derived and
MPL-2.0; the candidate's changed packaging is described above.

## Validation

`python3 tools/check_tls_public_roots.py` verifies the source and output
hashes, compares accepted certificate bytes to the original, reproduces each
accept/refuse decision with the production policy, and loads the combined
candidate with the real PEM loader. The policy/PEM probe uses ASan/UBSan;
LeakSanitizer is disabled for the host tracing environment.

The os64get integration worktree was tested in an isolated copied-root QEMU
guest with this candidate: direct HTTPS to `https://example.com/` and
`https://www.rfc-editor.org/rfc/rfc9112.txt` exited 0, and both saved files
matched host downloads byte for byte (559 and 109913 bytes respectively).
The guest filesystem check passed. That is public-site integration evidence,
not evidence of unrestricted website compatibility. Chris also confirmed a
successful direct `https://example.com/` download on the production P5 with
`https_proxy` unset. The controlled failure matrix was exercised in QEMU and
host tests, not repeated on the P5.

## P5 installation for checkout

No application rebuild is needed beyond the os64get HTTPS slice already
installed on the P5. Use the same trusted administrative transfer used for
executables to provision this trust file. The valet's CRC checks transfer
integrity; its connection does not authenticate the source.

Add this directory to the running Windows os64serve command's served roots
(the server searches directories without recursion):

```text
\\wsl$\Ubuntu-big2\home\yogi\src\os64\.worktrees\tls-public-roots\trust\mozilla\2026-08-13\install
```

Use an explicit destination for the one file, replacing BUILDHOST with the
normal build-host name or Windows LAN address:

```sh
mkdir /etc/certs
os64get BUILDHOST roots.pem /etc/certs/roots.pem
os64get https://example.com/ /home/example.html
os64get https://www.rfc-editor.org/rfc/rfc9112.txt /home/rfc9112.txt
```

The default path needs no tls.conf. If a selected tls.conf already names a
fixture or another bundle, change that setting deliberately to
`trust_store = /etc/certs/roots.pem`; there is no fallback from a bad selected
file. Leave `https_proxy` unset for native HTTPS checkout. No fixture host
mapping is needed for these public DNS names. Use `cat /home/example.html`
and `head /home/rfc9112.txt` to inspect the results.

The served `install` directory contains just the deployment bundle; source
and audit files stay in its parent directory. Use the explicit destination
above rather than filename routing.

## Update and removal policy

Adoption includes manual root maintenance. Check the upstream extract at
least monthly and promptly when a relevant CA distrust or compromise is
announced. There is no automatic online root fetching during a handshake,
and no claim of OCSP/CRL coverage.

For an update, fetch the new dated source and checksum over trusted host
HTTPS, retain both, regenerate the compatible subset against the current
production loader, and review every addition, removal and exclusion. Repeat
bundle-load and public-site acceptance. Update the pinned snapshot, manifest,
validation tool and this record together in a separate reviewable change.
Deploy the complete replacement through the existing staged administrative
install; do not union old and new roots. A new os64get invocation reloads the
file, while an existing invocation retains its immutable snapshot. Review any
rollback for trust that the newer snapshot deliberately removed.
