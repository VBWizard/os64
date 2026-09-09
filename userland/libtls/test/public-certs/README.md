# Public certificate metadata fixtures

These public leaf certificates were captured on 2026-09-08 from port 443
using Python `ssl.create_default_context()`, with SNI, hostname checking, and
host-system trust verification enabled. No private keys or public trust anchors
are included. The checked-in leaves make the policy tests independent of
network availability, certificate rotation, and fixture expiry.

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
