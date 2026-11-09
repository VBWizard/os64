# Public roots for os64 HTTPS

This pinned public trust snapshot supplies the ext2 image's default
`/etc/certs/roots.pem`. The image build installs it alongside the other system
configuration under `/etc`. A selected `tls.conf`, including `/home/tls.conf`,
can replace that store through the configuration ladder; selection never unions
stores or silently falls back. The FAT lifeboat is not provisioned with this bundle.

## Source and compatibility

The source is curl's Mozilla CA extract dated **2026-08-13**, downloaded from
<https://curl.se/ca/cacert.pem> over host-verified HTTPS on 2026-09-10. The
published checksum at <https://curl.se/ca/cacert.pem.sha256> matched:

```text
f66dff1bdf8f96060b8177976f8b7d9254bc89bc4db933d769f7384d28480bc9
```

The source has 121 certificates. os64's current anchor loader accepts 114 and
refuses 7. The bundle contains exactly the accepted certificates, with
unchanged DER bytes and original ordering. Certificate labels in the source
include non-ASCII text, which the bounded ASCII PEM loader rejects; the
bundle replaces explanatory text with an ASCII provenance header.

The exclusions are recorded individually by subject, certificate SHA-256,
and policy reason in [manifest.json](trust/mozilla/2026-08-13/manifest.json):
4 unsupported extensions, 2 anchor-policy refusals, and 1 SAN-policy refusal.
These are compatibility exclusions, not declarations that those CAs are malicious or invalid under
other clients' policies.

Anchor import distinguishes the installed name/key from a peer certificate:
zero/negative serials, a self-signature outside the peer SHA-2 allowlist, and
GeneralizedTime before 2050 do not prevent import. This admits the Go Daddy G2,
Starfield G2 and Services G2, SECOM RootCA2, both HARICA 2015 roots, Certum
Trusted Network CA 2, Certum Trusted Network CA, TWCA Root and ACCVRAIZ1.
Key strength, CA/Key Usage and restrictive extension checks remain enforced.
See [TLS_CERTIFICATE_POLICY.md](TLS_CERTIFICATE_POLICY.md) for the import
contract and the boundary between the authenticated path and redundant tails.

Default [roots.pem](trust/mozilla/2026-08-13/install/roots.pem): **114 roots**, 170633
bytes, SHA-256:

```text
84731395ad8a9bbcd9cc09c68fc03c91be32be6b328760f5c0da657dafa87101
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
MPL-2.0; the bundle's changed packaging is described above.

## Validation

`python3 tools/check_tls_public_roots.py` verifies the source and output
hashes, compares accepted certificate bytes to the original, reproduces each
accept/refuse decision with the production policy, and loads the combined
bundle with the real PEM loader. The policy/PEM probe uses ASan/UBSan;
LeakSanitizer is disabled for the host tracing environment.

The host policy corpus exercises successful chains using zero/negative-serial,
SHA-1-self-signed and early-GeneralizedTime anchors, plus matching refusals in
certificates on the authenticated path. Redundant tails after authentication
do not veto that path. Weak keys and restrictive anchor extensions remain covered.

A fresh ext2 image was built and its default bundle matched the pinned file
byte for byte. In an isolated QEMU guest, the revised library and companion
os64get passed `testrun tlstrusttest` and downloaded example.com and RFC 9112
using that extracted bundle at the default path, with no `tls.conf` or proxy.
Both downloads matched host copies, and the guest filesystem check passed.
The expanded 114-root bundle and revised library have not been rechecked on P5;
Chris's earlier production checkout used the 104-root subset.

## P5 installation for checkout

New ext2 images include the bundle automatically. To update an existing P5,
install the revised `libtls.so` with the bundle: the expanded subset requires
the anchor-import policy in this slice. Use the same trusted administrative
transfer used for executables to provision this trust file. The valet's CRC
checks transfer integrity; its connection does not authenticate the source.

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

The shipped default requires manual root maintenance. Rebuilding the ext2
image restores the pinned bundle under `/etc`; personal store selection belongs
in `/home/tls.conf`, and custom bundles should live on a persistent filesystem.
Check the upstream extract at
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
