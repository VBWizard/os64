#!/usr/bin/env python3
"""Generate certificate policy regressions with cryptography/OpenSSL and run the C gate."""
import argparse
import datetime as dt
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from check_bearssl_import import ROOT, BASE
from test_bearssl_host import check
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, rsa, padding
from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID as EKU


def tlv(tag, data):
    n = len(data)
    length = bytes([n]) if n < 128 else bytes([0x80 + (n.bit_length() + 7) // 8]) + n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([tag]) + length + data


def split(data):
    values = []
    while data:
        tag, n, at = data[0], data[1], 2
        if n & 128:
            size = n & 127
            n, at = int.from_bytes(data[2:2 + size], "big"), 2 + size
        values.append((tag, data[at:at + n]))
        data = data[at + n:]
    return values


def join(values):
    return b"".join(tlv(t, v) for t, v in values)


def mutate(cert, signer, change, digest=None):
    """Re-sign edited TBS bytes so policy negatives are not just bad signatures."""
    fields = split(split(cert)[0][1])
    tbs = split(fields[0][1])
    change(tbs)
    fields[0] = (0x30, join(tbs))
    signed = tlv(*fields[0])
    digest = digest or hashes.SHA256()
    sig = signer.sign(signed, ec.ECDSA(digest)) if isinstance(signer, ec.EllipticCurvePrivateKey) else signer.sign(signed, padding.PKCS1v15(), digest)
    fields[2] = (3, b"\0" + sig)
    return tlv(0x30, join(fields))


def extension(oid_tail, value, critical=False):
    return tlv(0x30, tlv(6, b"\x55\x1d" + bytes([oid_tail])) + (b"\x01\x01\xff" if critical else b"") + tlv(4, value))


def edit_extensions(tbs, edit):
    i = next(i for i, (tag, _) in enumerate(tbs) if tag == 0xa3)
    ext = split(split(tbs[i][1])[0][1])
    tbs[i] = (0xa3, tlv(0x30, edit(ext)))


def replace_extension(cert, signer, tail, value, critical=False):
    def edit(ext):
        kept = [e for e in ext if split(e[1])[0][1] != b"\x55\x1d" + bytes([tail])]
        return join(kept) + extension(tail, value, critical)
    return mutate(cert, signer, lambda t: edit_extensions(t, edit))


def certificate(key, name, issuer, signer, ca=False, san="example.test", eku=True, path=None, dates=None):
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, name)])
    before, after = dates or (dt.datetime(2025, 1, 1), dt.datetime(2030, 1, 1))
    builder = (x509.CertificateBuilder().subject_name(subject).issuer_name(issuer or subject)
        .public_key(key.public_key()).serial_number(x509.random_serial_number())
        .not_valid_before(before).not_valid_after(after)
        .add_extension(x509.BasicConstraints(ca, path), True)
        .add_extension(x509.KeyUsage(not ca, False, False, False, False, ca, ca, None, None), True))
    if san is not None:
        builder = builder.add_extension(x509.SubjectAlternativeName([x509.DNSName(san)]), False)
    if eku:
        builder = builder.add_extension(x509.ExtendedKeyUsage([EKU.SERVER_AUTH]), False)
    return builder.sign(signer, hashes.SHA256()).public_bytes(serialization.Encoding.DER)


def corpus(work):
    root_key = ec.generate_private_key(ec.SECP256R1())
    int_key = ec.generate_private_key(ec.SECP384R1())
    leaf_key = ec.generate_private_key(ec.SECP256R1())
    root = certificate(root_key, "policy root", None, root_key, ca=True, san=None, eku=False)
    root_name = x509.load_der_x509_certificate(root).subject
    intermediate = certificate(int_key, "policy intermediate", root_name, root_key, ca=True, san=None, path=2)
    int_name = x509.load_der_x509_certificate(intermediate).subject
    leaf = certificate(leaf_key, "example.test", int_name, int_key)
    blobs = {}
    cases = []
    anchors = []

    def blob(data):
        if data not in blobs:
            blobs[data] = f"blob_{len(blobs)}"
        return blobs[data]

    def case(name, cert=leaf, chain=None, reason="OK", success=False, host="example.test", trust=root, upstream=-1, days=740232):
        # Explicit fixture time; BearSSL's epoch offset is 719528 days.
        cases.append((name, blob(trust), [blob(c) for c in (chain if chain is not None else [cert, intermediate])], reason, success, host, upstream, days))

    def anchor(name, data, reason):
        anchors.append((name, blob(data), reason))

    case("valid", success=True, upstream=1)
    def attribute(tail, text):
        return tlv(0x30, tlv(6, b"\x55\x04" + bytes([tail])) + tlv(12, text))
    def with_name(cert, signer, index, attributes):
        return mutate(cert, signer, lambda t: t.__setitem__(index, (0x30, tlv(0x31, b"".join(attributes)))))
    # Compare complete encodings, including their lengths, not attribute values.
    rdn_pairs = [
        ("value", [attribute(3, b"a"), attribute(3, b"z")]),
        ("length", [attribute(3, b"z"), attribute(3, b"aa")]),
        ("long-length", [attribute(3, b"z" * 120), attribute(3, b"a" * 130)]),
        ("oid", [attribute(3, b"z"), attribute(10, b"a")]),
    ]
    for label, attributes in rdn_pairs:
        ordered = sorted(attributes)
        for backwards in (False, True):
            members = ordered[::-1] if backwards else ordered
            changed = with_name(leaf, int_key, 5, members)
            case(f"rdn-subject-{label}-descending-{backwards}", changed,
                 success=not backwards, reason="DER" if backwards else "OK", upstream=1)
    duplicate = [attribute(3, b"same")] * 2
    case("rdn-equal-members", with_name(leaf, int_key, 5, duplicate), success=True, upstream=1)
    ordered = sorted([attribute(3, b"root"), attribute(10, b"org")])
    for backwards in (False, True):
        members = ordered[::-1] if backwards else ordered
        changed_int = with_name(intermediate, root_key, 5, members)
        changed_leaf = with_name(leaf, int_key, 3, members)
        case(f"rdn-matching-issuer-descending-{backwards}", chain=[changed_leaf, changed_int],
             success=not backwards, reason="DER" if backwards else "OK", upstream=1)
        changed_root = with_name(with_name(root, root_key, 3, members), root_key, 5, members)
        anchor(f"anchor-rdn-descending-{backwards}", changed_root, "DER" if backwards else "OK")
        case(f"rdn-trailing-after-trust-descending-{backwards}", chain=[leaf, intermediate, changed_root],
             success=not backwards, reason="DER" if backwards else "OK", upstream=1)
    # Separate RDNs are a SEQUENCE, so their relative order must not be sorted.
    reverse_rdns = b"".join(tlv(0x31, value) for value in ordered[::-1])
    sequence_name = mutate(leaf, int_key, lambda t: t.__setitem__(5, (0x30, reverse_rdns)))
    case("rdn-sequence-order-preserved", sequence_name, success=True, upstream=1)
    def signature_value(cert, change):
        fields = split(split(cert)[0][1])
        fields[2] = (3, b"\0" + change(fields[2][1][1:]))
        return tlv(0x30, join(fields))
    def padded_signature(value, index):
        ints = split(split(value)[0][1])
        ints[index] = (2, b"\0" + ints[index][1])
        return tlv(0x30, join(ints))
    # Exercise necessary sign padding regardless of the generated signature.
    for attempt in range(128):
        canonical = mutate(leaf, int_key, lambda t: None)
        signature = split(split(canonical)[0][1])[2][1][1:]
        if all(value[0] == 0 for _, value in split(split(signature)[0][1])):
            break
    else:
        raise AssertionError("could not generate an ECDSA signature with both sign pads")
    case("ecdsa-required-sign-padding", canonical, success=True, upstream=1)
    for index in (0, 1):
        changed = signature_value(leaf, lambda v: padded_signature(v, index))
        case(f"ecdsa-redundant-zero-{index}", changed, reason="DER", upstream=1)
        changed_root = signature_value(root, lambda v: padded_signature(v, index))
        anchor(f"anchor-ecdsa-redundant-zero-{index}", changed_root, "DER")
        case(f"ecdsa-trailing-root-padding-{index}", chain=[leaf, intermediate, changed_root], reason="DER", upstream=1)
    case("ecdsa-long-form-short-length", signature_value(leaf, lambda v: b"\x30\x81" + v[1:]), reason="DER", upstream=1)
    for label, value in [("empty-sequence", b"\x30\0"), ("zero-r", b"\x30\x06\x02\x01\0\x02\x01\x01"),
                         ("zero-s", b"\x30\x06\x02\x01\x01\x02\x01\0"),
                         ("negative-r", b"\x30\x06\x02\x01\xff\x02\x01\x01"),
                         ("negative-s", b"\x30\x06\x02\x01\x01\x02\x01\xff"),
                         ("empty-r", b"\x30\x05\x02\0\x02\x01\x01"),
                         ("missing-s", b"\x30\x03\x02\x01\x01"),
                         ("integer-long-form-short-length", b"\x30\x07\x02\x81\x01\x01\x02\x01\x01"),
                         ("truncated-integer", b"\x30\x06\x02\x01\x01\x02\x02\x01"),
                         ("third-integer", b"\x30\x09\x02\x01\x01\x02\x01\x01\x02\x01\x01"),
                         ("trailing", b"\x30\x06\x02\x01\x01\x02\x01\x01\x05\0")]:
        case("ecdsa-" + label, signature_value(leaf, lambda v: value), reason="DER")
    def string_name(cert, signer, index, tag, value):
        encoded = tlv(0x31, tlv(0x30, tlv(6, b"\x55\x04\x0a") + tlv(tag, value)))
        return mutate(cert, signer, lambda t: t.__setitem__(index, (0x30, encoded)))
    strings = [
        ("utf8", 12, "Aé€🐻".encode(), True), ("utf8-upper", 12, b"\xf4\x8f\xbf\xbf", True),
        ("utf8-ff", 12, b"\xff", False), ("utf8-overlong", 12, b"\xc0\x80", False),
        ("utf8-overlong3", 12, b"\xe0\x80\xaf", False), ("utf8-overlong4", 12, b"\xf0\x80\x80\xaf", False),
        ("utf8-truncated", 12, b"\xe2\x82", False), ("utf8-continuation", 12, b"\x80", False),
        ("utf8-bad-continuation", 12, b"\xc2A", False), ("utf8-surrogate", 12, b"\xed\xa0\x80", False),
        ("utf8-too-large", 12, b"\xf4\x90\x80\x80", False), ("utf8-nul", 12, b"A\0B", False),
        ("numeric", 18, b"01 29", True), ("numeric-letter", 18, b"A", False),
        ("printable", 19, b"AZaz09 '()+,-./:=?", True), ("printable-at", 19, b"@", False),
        ("printable-high", 19, b"\x80", False), ("teletex-refused", 20, b"plain", False),
        ("ia5", 22, b"mail@example.test", True), ("ia5-high", 22, b"\x80", False), ("ia5-nul", 22, b"A\0B", False),
        ("visible", 26, b" !~", True), ("visible-control", 26, b"\x1f", False), ("visible-del", 26, b"\x7f", False),
        ("universal", 28, "A🐻".encode("utf-32-be"), True), ("universal-width", 28, b"\0\0A", False),
        ("universal-surrogate", 28, b"\0\0\xd8\0", False), ("universal-too-large", 28, b"\0\x11\0\0", False),
        ("bmp", 30, "Aé€".encode("utf-16-be"), True), ("bmp-width", 30, b"\0", False),
        ("bmp-surrogate", 30, b"\xd8\0", False), ("bmp-surrogate-pair", 30, "🐻".encode("utf-16-be"), False),
        ("bmp-nul", 30, b"\0\0", False), ("universal-nul", 28, b"\0\0\0\0", False),
    ]
    for label, tag, value, valid in strings:
        case("dn-string-" + label, string_name(leaf, int_key, 5, tag, value),
             success=valid, reason="OK" if valid else "DER", upstream=1 if label == "utf8-ff" else -1)
    bad_name = (12, b"\xff")
    bad_int = string_name(intermediate, root_key, 5, *bad_name)
    bad_leaf = string_name(leaf, int_key, 3, *bad_name)
    case("dn-bad-matching-issuer", chain=[bad_leaf, bad_int], reason="DER", upstream=1)
    bad_root = string_name(string_name(root, root_key, 3, *bad_name), root_key, 5, *bad_name)
    anchor("anchor-bad-string", bad_root, "DER")
    case("dn-bad-trailing-root", chain=[leaf, intermediate, bad_root], reason="DER", upstream=1)
    case("included-root", chain=[leaf, intermediate, root], success=True)
    case("mixed-case-san", certificate(leaf_key, "ignored", int_name, int_key, san="EXAMPLE.TEST"), success=True)
    case("wildcard", certificate(leaf_key, "ignored", int_name, int_key, san="*.example.test"), host="www.example.test", success=True)
    wildcard = certificate(leaf_key, "ignored", int_name, int_key, san="*.example.test")
    case("wildcard-multi-label", wildcard, host="a.b.example.test", reason="SAN")
    case("wildcard-bare-suffix", wildcard, reason="SAN")
    case("partial-wildcard", certificate(leaf_key, "example.test", int_name, int_key, san="e*.test"), reason="SAN")
    case("cn-only", certificate(leaf_key, "example.test", int_name, int_key, san=None), reason="SAN", upstream=1)
    case("cn-cannot-rescue-san", certificate(leaf_key, "example.test", int_name, int_key, san="wrong.test"), reason="SAN")
    case("absent-eku", certificate(leaf_key, "example.test", int_name, int_key, eku=False), success=True)
    for label, oid_value in [("client-auth", b"\x2b\x06\x01\x05\x05\x07\x03\x02"), ("any-eku", b"\x55\x1d\x25\x00")]:
        value = tlv(0x30, tlv(6, oid_value))
        case(label, replace_extension(leaf, int_key, 37, value), reason="EKU", upstream=1)
        bad_int = replace_extension(intermediate, root_key, 37, value)
        case(label + "-intermediate", chain=[leaf, bad_int], reason="EKU", upstream=1)
    server_oid = tlv(6, b"\x2b\x06\x01\x05\x05\x07\x03\x01")
    case("critical-eku", replace_extension(leaf, int_key, 37, tlv(0x30, server_oid), True), reason="OK")
    case("eku-server-and-any", replace_extension(leaf, int_key, 37, tlv(0x30, server_oid + tlv(6, b"\x55\x1d\x25\x00"))), success=True)
    case("eku-32", replace_extension(leaf, int_key, 37, tlv(0x30, server_oid + b"".join(tlv(6, b"\x2a\x03" + bytes([i])) for i in range(31)))), success=True)
    case("eku-empty", replace_extension(leaf, int_key, 37, b"\x30\0"), reason="DER")
    case("eku-duplicate", replace_extension(leaf, int_key, 37, tlv(0x30, server_oid * 2)), reason="DUPLICATE")
    case("eku-limit", replace_extension(leaf, int_key, 37, tlv(0x30, b"".join(tlv(6, b"\x2a\x03" + bytes([i])) for i in range(33)))), reason="LIMIT")
    for tail, value in [(30, tlv(0x30, tlv(0xa0, tlv(0x30, tlv(0x82, b"other.test"))))),
                         (36, b"\x30\x03\x80\x01\0"), (33, b"\x30\0"), (54, b"\x02\x01\0"), (99, b"\x05\0")]:
        for critical in (False, True):
            case(f"restriction-{tail}-{critical}", replace_extension(leaf, int_key, tail, value, critical), reason="EXTENSION")
        anchor(f"anchor-restriction-{tail}", replace_extension(root, root_key, tail, value), "EXTENSION")
    case("critical-metadata", replace_extension(leaf, int_key, 14, tlv(4, b"identifier"), True), reason="CRITICAL", upstream=1)
    case("metadata", replace_extension(leaf, int_key, 14, tlv(4, b"identifier")), success=True)
    sct_oid = bytes.fromhex("2b06010401d679020402")
    # This is an opaque SCT-shaped test value, not a verified CT log receipt.
    sct_signature = int_key.sign(b"fixture SCT", ec.ECDSA(hashes.SHA256()))
    sct = b"\0" + bytes(range(32)) + bytes(8) + b"\0\0\x04\x03" + len(sct_signature).to_bytes(2, "big") + sct_signature
    entries = len(sct).to_bytes(2, "big") + sct
    sct_value = tlv(4, len(entries).to_bytes(2, "big") + entries)
    def with_sct(value, critical=False, duplicate=False):
        encoded = tlv(0x30, tlv(6, sct_oid) + (b"\x01\x01\xff" if critical else b"") + tlv(4, value))
        return mutate(leaf, int_key, lambda t: edit_extensions(t, lambda e: join(e) + encoded * (2 if duplicate else 1)))
    case("sct-metadata", with_sct(sct_value), success=True, upstream=1)
    case("sct-critical", with_sct(sct_value, critical=True), reason="CRITICAL")
    case("sct-wrong-envelope", with_sct(b"\x05\0"), reason="DER")
    case("sct-trailing-envelope", with_sct(sct_value + b"\x04\0"), reason="DER")
    case("sct-duplicate", with_sct(sct_value, duplicate=True), reason="DUPLICATE")
    def with_delegation(value, critical=False, duplicate=False):
        encoded = tlv(0x30, tlv(6, bytes.fromhex("2b0601040182da4b2c")) +
                      (b"\x01\x01\xff" if critical else b"") + tlv(4, value))
        return mutate(leaf, int_key, lambda t: edit_extensions(t, lambda e: join(e) + encoded * (2 if duplicate else 1)))
    case("delegation-metadata", with_delegation(b"\x05\0"), success=True, upstream=1)
    case("delegation-critical", with_delegation(b"\x05\0", critical=True), reason="CRITICAL")
    case("delegation-wrong-envelope", with_delegation(b"\x04\0"), reason="DER")
    case("delegation-nonempty-null", with_delegation(b"\x05\x01\0"), reason="DER")
    case("delegation-trailing-envelope", with_delegation(b"\x05\0\x05\0"), reason="DER")
    case("delegation-duplicate", with_delegation(b"\x05\0", duplicate=True), reason="DUPLICATE")
    for name, host in [("example-com", "example.com"), ("letsencrypt-isrgrootx1", "valid-isrgrootx1.letsencrypt.org")]:
        public = x509.load_pem_x509_certificate((BASE / "test/public-certs" / (name + ".pem")).read_bytes())
        # Policy inspection can pass, but the fixture root must not trust this
        # public certificate. No public trust anchor enters the generated store.
        case("public-leaf-" + name, chain=[public.public_bytes(serialization.Encoding.DER)], host=host)
    case("must-staple", mutate(leaf, int_key, lambda t: edit_extensions(t, lambda e: join(e) + tlv(0x30,
        tlv(6, bytes.fromhex("2b06010505070118")) + tlv(4, b"\x30\x03\x02\x01\x05")))), reason="EXTENSION", upstream=1)
    case("duplicate-san", mutate(leaf, int_key, lambda t: edit_extensions(t, lambda e: join(e) + next(tlv(*v) for v in e if split(v[1])[0][1] == b"\x55\x1d\x11"))), reason="DUPLICATE")
    for label, name in [("nul", b"example.test\0evil"), ("underscore", b"example_test"), ("empty-label", b"example..test"),
                        ("trailing-dot", b"example.test."), ("high-byte", b"\xff.test"), ("hyphen", b"-example.test"),
                        ("long-label", b"a" * 64 + b".test")]:
        case("san-" + label, replace_extension(leaf, int_key, 17, tlv(0x30, tlv(0x82, name))), reason="SAN")
    case("ip-san", replace_extension(leaf, int_key, 17, tlv(0x30, tlv(0x87, bytes([127, 0, 0, 1])))), reason="SAN")
    case("non-dns-plus-dns", replace_extension(leaf, int_key, 17, tlv(0x30, tlv(0x86, b"https://example.test") + tlv(0x82, b"example.test"))), success=True)
    dns_name = tlv(0x82, b"example.test")
    constructed_names = [
        ("otherName", 0xa0, tlv(6, b"\x2a\x03") + tlv(0xa0, tlv(12, b"fixture"))),
        ("x400Address", 0xa3, tlv(0x30, b"")),
        ("directoryName", 0xa4, tlv(0x30, tlv(0x31, tlv(0x30, tlv(6, b"\x55\x04\x03") + tlv(12, b"fixture"))))),
        ("ediPartyName", 0xa5, tlv(0xa1, tlv(12, b"fixture"))),
    ]
    for label, tag, encoded in constructed_names:
        for shape, value in [("empty", b""), ("wrong-schema", b"\x05\0")]:
            for first in (False, True):
                alternative = tlv(tag, value)
                names = alternative + dns_name if first else dns_name + alternative
                case(f"san-{label}-{shape}-first-{first}", replace_extension(leaf, int_key, 17, tlv(0x30, names)),
                     reason="SAN", upstream=int(tag != 0xa0))
        # Even well-formed constructed alternatives are outside this DNS profile.
        case("san-unsupported-" + label, replace_extension(leaf, int_key, 17,
             tlv(0x30, dns_name + tlv(tag, encoded))), reason="SAN", upstream=1)
        restricted_root = replace_extension(root, root_key, 17, tlv(0x30, tlv(tag, b"")))
        case("san-trailing-after-trust-" + label, chain=[leaf, intermediate, restricted_root], reason="SAN", upstream=1)
        anchor("anchor-san-" + label, restricted_root, "SAN")
    for label, alternative in [("email", tlv(0x81, b"user@example.test")),
                               ("ip", tlv(0x87, bytes([127, 0, 0, 1]))),
                               ("registered-id", tlv(0x88, b"\x2a\x03"))]:
        case("san-primitive-plus-dns-" + label, replace_extension(leaf, int_key, 17,
             tlv(0x30, alternative + dns_name)), success=True, upstream=1)
    case("leaf-ca", certificate(leaf_key, "example.test", int_name, int_key, ca=True), reason="CA")
    case("leaf-ku", replace_extension(leaf, int_key, 15, b"\x03\x02\x05\x20", True), reason="KEY_USAGE")
    case("intermediate-ku", chain=[leaf, replace_extension(intermediate, root_key, 15, b"\x03\x02\x07\x80", True)], reason="KEY_USAGE")
    case("intermediate-nonca", chain=[leaf, certificate(int_key, "policy intermediate", root_name, root_key, ca=False, san=None)], reason="CA")
    case("expired", certificate(leaf_key, "example.test", int_name, int_key, dates=(dt.datetime(2020, 1, 1), dt.datetime(2021, 1, 1))))
    case("future", certificate(leaf_key, "example.test", int_name, int_key, dates=(dt.datetime(2031, 1, 1), dt.datetime(2032, 1, 1))))
    case("untrusted", chain=[leaf])
    corrupt = leaf[:-1] + bytes([leaf[-1] ^ 1])
    case("bad-signature", corrupt)
    case("trailing-restriction-after-trust", chain=[leaf, intermediate, replace_extension(root, root_key, 30, b"\x30\0")], reason="EXTENSION", upstream=1)
    case("trailing-malformed-after-trust", chain=[leaf, intermediate, root + b"\0"], reason="DER", upstream=1)
    case("chain-limit", chain=[leaf, intermediate] + [root] * 7, reason="LIMIT")
    case("certificate-limit", cert=b"\0" * 32769, chain=[b"\0" * 32769], reason="LIMIT")
    for label, data in [("truncated", leaf[:-1]), ("trailing", leaf + b"\0"), ("indefinite", b"\x30\x80" + leaf[4:] + b"\0\0"),
                        ("nonminimal-length", b"\x30\x83\0" + leaf[2:])]:
        case(label, data, reason="DER")
    case("bad-extension-inner", replace_extension(leaf, int_key, 14, b"\x04\x81\x01a"), reason="DER")
    case("bad-critical-bool", mutate(leaf, int_key, lambda t: edit_extensions(t, lambda e: join(e).replace(b"\x01\x01\xff", b"\x01\x01\x01", 1))), reason="DER")
    case("explicit-default-bool", mutate(leaf, int_key, lambda t: edit_extensions(t, lambda e: join(e).replace(b"\x01\x01\xff", b"\x01\x01\0", 1))), reason="DER")
    case("negative-pathlen", replace_extension(leaf, int_key, 19, b"\x30\x03\x02\x01\xff"), reason="DER")
    nested = b"\x05\0"
    for _ in range(18):
        nested = tlv(0x30, nested)
    case("der-depth", replace_extension(leaf, int_key, 14, nested), reason="DER")
    case("der-nodes", replace_extension(leaf, int_key, 14, tlv(0x30, b"\x05\0" * 4096)), reason="DER")
    case("repeated-extension-list", mutate(leaf, int_key, lambda t: edit_extensions(t, lambda e: join(e) * 9)), reason="DUPLICATE")
    case("malformed-date", mutate(leaf, int_key, lambda t: t.__setitem__(4, (0x30, tlv(23, b"250230000000Z") + tlv(23, b"300101000000Z")))), reason="DER")
    case("mismatched-signature-id", mutate(leaf, int_key, lambda t: t.__setitem__(2, (0x30, tlv(6, bytes.fromhex("2a8648ce3d040303"))))), reason="SIGNATURE")

    def invalid_point(tbs):
        spki = split(tbs[6][1])
        spki[1] = (3, b"\0\x04" + bytes(len(spki[1][1]) - 2))
        tbs[6] = (0x30, join(spki))
    case("invalid-ec-point", mutate(leaf, int_key, invalid_point), reason="KEY")
    anchor("anchor-invalid-ec-point", mutate(root, root_key, invalid_point), "KEY")
    anchor("anchor-good", root, "OK")
    def v1(tbs):
        tbs[:] = [(tag, value) for tag, value in tbs if tag not in (0xa0, 0xa3)]
    anchor("anchor-v1", mutate(root, root_key, v1), "DER")
    anchor("anchor-intermediate", intermediate, "ANCHOR")
    anchor("anchor-end-entity", certificate(root_key, "policy root", None, root_key, ca=False, san=None, eku=False), "CA")
    anchor("anchor-pathlen", certificate(root_key, "policy root", None, root_key, ca=True, san=None, eku=False, path=0), "ANCHOR")
    anchor("anchor-eku", replace_extension(root, root_key, 37, tlv(0x30, server_oid)), "EKU")
    anchor("anchor-keyusage", replace_extension(root, root_key, 15, b"\x03\x02\x07\x80", True), "KEY_USAGE")
    lower_key = ec.generate_private_key(ec.SECP256R1())
    lower = certificate(lower_key, "lower intermediate", int_name, int_key, ca=True, san=None)
    lower_leaf = certificate(leaf_key, "example.test", x509.load_der_x509_certificate(lower).subject, lower_key)
    no_path = certificate(int_key, "policy intermediate", root_name, root_key, ca=True, san=None, path=0)
    case("pathlen-allowed", chain=[lower_leaf, lower, intermediate], success=True)
    case("pathlen-exceeded", chain=[lower_leaf, lower, no_path])
    for curve in (ec.SECP384R1(), ec.SECP521R1()):
        key = ec.generate_private_key(curve)
        case("ec-" + curve.name, certificate(key, "example.test", int_name, int_key), success=True)
    rsa_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    rsa_leaf = certificate(rsa_key, "example.test", int_name, int_key)
    case("rsa-2048", rsa_leaf, success=True)
    rsa_root = certificate(rsa_key, "rsa root", None, rsa_key, ca=True, san=None, eku=False)
    rsa_issued = certificate(leaf_key, "example.test", x509.load_der_x509_certificate(rsa_root).subject, rsa_key)
    case("rsa-signature-and-anchor", chain=[rsa_issued], trust=rsa_root, success=True)
    max_cert = replace_extension(rsa_issued, rsa_key, 14, tlv(4, bytes(32000)))
    max_cert = replace_extension(rsa_issued, rsa_key, 14, tlv(4, bytes(32000 + 32768 - len(max_cert))))
    assert len(max_cert) == 32768
    case("certificate-exact-limit", chain=[max_cert], trust=rsa_root, success=True)

    def weak_modulus(tbs):
        spki = split(tbs[6][1])
        key = split(split(spki[1][1][1:])[0][1])
        n = bytearray(key[0][1][1:])
        n[0] = 0x7f  # Still 256 bytes, now 2047 actual bits.
        key[0] = (2, bytes(n))
        spki[1] = (3, b"\0" + tlv(0x30, join(key)))
        tbs[6] = (0x30, join(spki))
    case("rsa-2047", mutate(rsa_leaf, int_key, weak_modulus), reason="KEY", upstream=1)
    anchor("anchor-rsa-2047", mutate(rsa_root, rsa_key, weak_modulus), "KEY")
    weak_key = rsa.generate_private_key(public_exponent=65537, key_size=1024)
    case("rsa-1024", certificate(weak_key, "example.test", int_name, int_key), reason="KEY")
    large_key = rsa.generate_private_key(public_exponent=65537, key_size=4096)
    case("rsa-4096", certificate(large_key, "example.test", int_name, int_key), success=True)
    anchor("anchor-rsa-4096", certificate(large_key, "large root", None, large_key, ca=True, san=None, eku=False), "OK")
    def oversized_modulus(tbs):
        spki = split(tbs[6][1])
        key = split(split(spki[1][1][1:])[0][1])
        key[0] = (2, b"\x01" + bytes(511) + b"\x01")
        spki[1] = (3, b"\0" + tlv(0x30, join(key)))
        tbs[6] = (0x30, join(spki))
    case("rsa-4097", mutate(rsa_leaf, int_key, oversized_modulus), reason="KEY")
    anchor("anchor-rsa-4097", mutate(rsa_root, rsa_key, oversized_modulus), "KEY")

    # Change both identifiers and sign with SHA-1; no builder SHA-1 opt-in needed.
    outer = split(split(leaf)[0][1])
    sha1_alg = tlv(6, bytes.fromhex("2a8648ce3d0401"))
    sha1 = mutate(leaf, int_key, lambda t: t.__setitem__(2, (0x30, sha1_alg)), hashes.SHA1())
    outer = split(split(sha1)[0][1]); outer[1] = (0x30, sha1_alg)
    case("sha1-signature", tlv(0x30, join(outer)), reason="SIGNATURE")

    # A large encoded DN reaches the aggregate anchor-byte limit before count.
    def large_name(tbs):
        name = b"".join(tlv(0x31, tlv(0x30, tlv(6, b"\x55\x04\x0b") + tlv(12, (f"{i:04d}" + "a" * 60).encode()))) for i in range(110))
        tbs[3] = tbs[5] = (0x30, name)
    large_root = blob(mutate(root, root_key, large_name))
    private_scalar = leaf_key.private_numbers().private_value.to_bytes(32, "big")

    # Supply an independent OpenSSL validation check for the positive chain.
    for name, cert in [("root", root), ("intermediate", intermediate), ("leaf", leaf)]:
        (work / (name + ".pem")).write_bytes(x509.load_der_x509_certificate(cert).public_bytes(serialization.Encoding.PEM))
    subprocess.run(["openssl", "verify", "-attime", "1788868800", "-purpose", "sslserver", "-verify_hostname", "example.test",
        "-CAfile", str(work / "root.pem"), "-untrusted", str(work / "intermediate.pem"), str(work / "leaf.pem")], check=True)

    with (work / "policy_corpus.h").open("w") as out:
        for data, name in blobs.items():
            out.write(f"static const unsigned char {name}_bytes[] = {{" + ",".join(str(b) for b in data) + "};\n")
            out.write(f"static const blob {name} = {{{name}_bytes, sizeof {name}_bytes}};\n")
        out.write("static const policy_case cases[] = {\n")
        for name, root_id, chain, reason, success, host, upstream, days in cases:
            out.write(f'{{"{name}", &{root_id}, {{' + ",".join("&" + c for c in chain) + f'}}, {len(chain)}, TLS_POLICY_{reason}, {int(success)}, "{host}", {upstream}, {days}' + "},\n")
        out.write("};\nstatic const anchor_case anchor_cases[] = {\n")
        for name, data, reason in anchors:
            out.write(f'{{"{name}", &{data}, TLS_POLICY_{reason}}},\n')
        out.write("};\n")
        out.write(f"static const blob *large_root = &{large_root};\n")
        out.write("static const unsigned char leaf_scalar[] = {" + ",".join(str(b) for b in private_scalar) + "};\n")


def policy(archive, work):
    corpus(work)
    executable = work / "policy"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-g", "-pthread",
        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-ffreestanding", "-fno-builtin", "-include", str(BASE / "port/config.h"),
        "-I" + str(BASE / "upstream/inc"), "-I" + str(ROOT / "userland/libos64/include"),
        "-I" + str(ROOT / "abi/include"), "-I" + str(work),
        str(BASE / "port/certificate_der.c"), str(BASE / "port/certificate_policy.c"),
        str(BASE / "port/client_engine.c"), str(BASE / "port/client_profile.c"),
        str(ROOT / "tools/test_tls_policy_host.c"), str(archive), "-o", str(executable)], check=True)
    subprocess.run([executable], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--foundation", type=Path, help="reuse a matching adapted/core.a")
    parser.add_argument("--output", type=Path, help="retain corpus and executable in a new directory")
    args = parser.parse_args()
    def run(work):
        if not args.foundation:
            check(work)
        policy(args.foundation.resolve() if args.foundation else work / "adapted/core.a", work)
    if args.output:
        args.output.mkdir()
        run(args.output.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix="tls-policy-") as directory:
            run(Path(directory))
