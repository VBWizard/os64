# Public certificate fixtures

These public leaf certificates were captured on 2026-09-08 from port 443
using Python `ssl.create_default_context()`, with SNI, hostname checking, and
host-system trust verification enabled. No private keys are included. The
checked-in leaves make their policy tests independent of network availability,
certificate rotation, and fixture expiry.

| File | Host | Issuer CN | SHA-256 of DER |
|---|---|---|---|
| `example-com.pem` | `example.com` | Cloudflare TLS Issuing ECC CA 3 | `6153a96fd1a6ab7f4d438fc34932484299d0729d9140b3a126bb2f9c07b02200` |
| `letsencrypt-isrgrootx1.pem` | `valid-isrgrootx1.letsencrypt.org` | YR1 (Let's Encrypt) | `56d0f2a5ffee83bb3d90f1d946354f5122249adaa1d40bd547afadf930bdf433` |

Both leaves carry the noncritical RFC 6962 SCT-list extension. The example.com
leaf also carries noncritical RFC 9345 DelegationUsage. They exercise the
extension allowlist and typed DER envelopes in the production policy parser.
The harness expects policy inspection to pass and the overall certificate gate
to fail under its unrelated generated root. This is not evidence of os64
validating either public chain or checking CT receipts. Separate generated,
signed fixtures exercise successful validation with these metadata extensions
and refusal of their critical, malformed, and duplicate forms.

TLS Feature / must-staple remains refused because OCSP stapling is not
implemented. Legacy v1 roots also remain refused by the explicit v3 policy.
These samples do not establish compatibility with a complete public root store.

## Princeton chain regression (#93)

`princeton-chain.pem` contains the four certificates returned on 2026-09-11 by:

```sh
openssl s_client -connect mirror.math.princeton.edu:443 \
    -servername mirror.math.princeton.edu -tls1_2 -showcerts </dev/null
```

The host verified the chain through USERTrust RSA Certification Authority.
The policy corpus uses that CA from the pinned shipped root bundle and fixes
validation time to 2026-09-11 UTC. It tests the two-certificate authenticated
prefix, the three-certificate cross-sign form, the full four-certificate form,
and refusal under an unrelated fixture root. The last two certificates are
peer-supplied test data, not additions to the configured trust store.

| Position | Subject CN | Signature | SHA-256 of DER |
|---|---|---|---|
| 1 | mirror.math.princeton.edu | sha256 | `9d3b3ea32091c319928cefc974cf0cbf2ef24a9195dd3f547c803f676d4e21bd` |
| 2 | InCommon RSA Server CA 2 | sha384 | `87e01cc4dd0c9d92a3dbd49092ff13f9cd387445cdc57e5b984e1b7721b5b029` |
| 3 | USERTrust RSA Certification Authority | sha384 | `68b9c761219a5b1f0131784474665db61bbdb109e00f05ca9f74244ee5f5f52b` |
| 4 | AAA Certificate Services | sha1 | `d7a7a0fb5d7e2731d771e9484ebcdef71d5f0c3e0a2948782bc83ee0ea699ef4` |

## Microsoft chain regression (2026-10-09)

`microsoft-chain.pem` contains the four public certificates captured with
`openssl s_client -connect microsoft.com:443 -servername microsoft.com
-showcerts -verify_return_error` on 2026-10-09. OpenSSL validated the chain
through DigiCert Global Root G2. No private keys or new trust anchors are
included. Tests select DigiCert Global Root G2 from the existing shipped
bundle and fix validation time to the capture date.

The leaf carries noncritical Microsoft Application Policies (serverAuth)
and Certificate Template extensions. Its issuer carries noncritical CA
Version and Certificate Template Name (`SubCA`). The unmodified policy
rejected the leaf and issuer with reason EXTENSION (4), ending with engine
error 34; the typed extension support validates both. Three- and
four-certificate forms must succeed; an unrelated trust store and a wrong
hostname must still fail. Generated, independently signed fixtures cover
purpose conflicts, CA restrictions, anchor preservation, critical values,
duplicates, malformed encodings and bounds without relying on this live
certificate's issuance details.

| Position | Subject CN | SHA-256 of DER |
|---|---|---|
| 1 | microsoft.com | `1e16fa37bf143e7ad494aa2ee922846a6d02434f1fdca413b78097a31c7b77f5` |
| 2 | Microsoft TLS G2 RSA CA OCSP 04 | `ac8ea9f2874fd368a3e778b1a0b165ee898db9b9687c17edcdc76908ab58c82c` |
| 3 | Microsoft TLS RSA Root G2 | `ddcd1e8a20638d4aaff7201bb1d56452acd2c759f1686bdc38f73dd15732bdc2` |
| 4 | DigiCert Global Root G2 | `cb3ccbb76031e5e0138f8dd39a23f9de47ffc35e43c1144cea27d46a5ab1cb5f` |
