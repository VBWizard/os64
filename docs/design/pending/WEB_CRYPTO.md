# WEB_CRYPTO.md — `crypto.subtle`

*Opus 5.5, 2026-11-07, for Chris, then Fable. WINDOW_DOCUMENT.md's
question 1 shipped `crypto` without `subtle` (ruled 2026-10-23) and named
the alternative: "a WebCrypto design over BearSSL". This is that design.
Chris asked for the work in three tiers by demand (what we need now, what
most pages need, what fewer pages want) rather than by technology; § 2
shows the two splits come out nearly the same.*

*Fable reviewed it the same day (`~/os64-opus-kit/reviews/WEB_CRYPTO_REVIEW.md`):
"build it, in the three tiers as cut". He answered questions 4 (private
BearSSL objects, and for the harness's sake more than the ABI's) and 5
(settled promises, with three consequences now in § 3 and § 5), and his
findings on what the vendored BearSSL does and does not do are folded in
below: AES-CTR's counter, CBC's padding and the raw block door are ours
(§ 4); RSA-PSS and X25519 are in BearSSL after all (§ 2).*

## 1. Why now, and what pages ask

Two pages on the P5 die on the missing member today:

- **A social network's sign-in dialog** opens empty. Every request the
  page makes to its servers carries an `x-client-transaction-id` header
  that a script computes; the computation ends in
  `crypto.subtle.digest("SHA-256", …)`, throws, and the page sends the
  exception's text, base64'd, as the id. The dialog fetches its form
  with that header, and the form never comes. Proved by replaying the P5
  record with a JavaScript SHA-256 standing in for `digest`: the
  generator then makes real ids (94 characters, the format it always
  makes), and nothing else in it is missing.
- **A search engine's results page**: `TypeError: cannot read property
  'digest' of undefined`, in one of its own scripts, unhandled.

**The corpus** (every pull to 2026-11-07, 140 hosts whose scripts call
`crypto.subtle`, each script counted once by content). A host "names" an
algorithm when a script that calls a `subtle` method other than `digest`
spells the algorithm's name.

| What | Hosts | How they use it |
|---|---|---|
| `digest` | 137 (109 use nothing else) | SHA-256 overwhelmingly; SHA-1/384/512 rarely |
| AES-GCM | 15 | encrypt and decrypt; keys generated, imported raw (16) or JWK (6), or derived (`deriveKey` in 8 scripts) |
| AES-CTR | 14 | keys imported as JWK (11) or raw (8), encrypt |
| AES-CBC | 10 | raw keys (7), encrypt and decrypt, `wrapKey` (8) |
| ECDH | 9 | key pairs generated, public keys exported raw (11) or spki (6), `deriveKey` into AES-GCM |
| HMAC | 7 | raw (7) and JWK (6) keys, `sign` |
| RSA-OAEP | 7 | public keys imported (JWK 3, spki 2), encrypt, `wrapKey` |
| RSASSA-PKCS1-v1_5 | 4 | public keys imported as JWK, `verify` |
| ECDSA | 2 | public keys imported as JWK, `verify` |
| PBKDF2, HKDF | 2 each | the same two hosts (one company's mail and account pages) |
| AES-KW | 1 | named once |
| RSA-PSS, Ed25519, X25519 | 0 | |

Who asks beyond `digest`: one company's mail and account pages, consent
and ad scripts, bot challenges, wallpaper and game sites, a weather site,
a design tool, retailers' and ISPs' sign-in pages.

## 2. The tiers

A tier serves a host only if it covers every algorithm that host uses, so
the count that matters is hosts *fully* served:

| Tier | Adds | Hosts fully served (of 140) |
|---|---|---|
| **1. What we need now** | `SubtleCrypto` and `CryptoKey` whole (every method present), and `digest` | **109** (78%), the two pages of § 1 among them |
| **2. What most pages need** | AES-GCM, AES-CTR, AES-CBC, HMAC: keys that are bytes, raw and JWK; `generateKey`, `importKey`, `exportKey`, `encrypt`, `decrypt`, `sign`, `verify`, `wrapKey`, `unwrapKey` over them | **121** |
| **3. What fewer pages want** | ECDH, ECDSA (P-256, P-384, P-521), RSA-OAEP, RSASSA-PKCS1-v1_5, HKDF, PBKDF2, AES-KW; key pairs, spki and pkcs8; `deriveKey`, `deriveBits` | **139** |

The one host left is an ad script whose algorithm the scan could not
name. The demand split and a technical one (hashing; secret keys; key
pairs) coincide but for one algorithm: **ECDH** in tier 2 would serve 127
instead of 121, and bring key pairs, point formats and spki with it,
which are most of tier 3's machinery (question 2).

**Out of all three**, because no page asks: RSA-PSS and X25519, both
cheap, since BearSSL has them (`br_rsa_pss_sign`/`_vrfy`, and
`br_ec_c25519_*` for X25519's key agreement; question 3); Ed25519,
Ed448 and X448 (WPT's `secure-curves`), which BearSSL lacks; and the
draft's newer algorithms (WPT's `modern-algos`: ML-KEM, ML-DSA,
ChaCha20-Poly1305, SHA-3, Argon2). Each is a DEBTS row that says which
of the two it is, cheap or absent from BearSSL, and each answers
NotSupportedError, which is what a browser answers for an algorithm it
does not know.

## 3. What the spec asks (Web Cryptography Level 2, the editor's draft WPT tests)

- **`crypto.subtle`** is `[SecureContext]`: present on a secure page, as
  `randomUUID` already is (`d_secure_context`), absent on an insecure one.
  `SubtleCrypto` has twelve methods: `encrypt`, `decrypt`, `sign`,
  `verify`, `digest`, `generateKey`, `deriveKey`, `deriveBits`,
  `importKey`, `exportKey`, `wrapKey`, `unwrapKey`. **Every one rejects;
  none ever throws.** WebIDL turns an exception raised while converting
  an operation's arguments into a rejected promise when the operation
  returns one, and the spec's own steps reject for normalization
  failures, so a `data` that is no BufferSource, a `name` that is no
  string and a getter that throws inside the algorithm dictionary are all
  rejections. The binding catches at the top of each method and rejects
  with what it caught (`idlharness` and the error-order tests hold it
  there).
- **Algorithm normalization** (§ 18.4) is the spec's real machinery: the
  argument is a string or a dictionary with a `name`, matched without
  case against the table of the operation's registered algorithms, then
  converted to that algorithm's dictionary by WebIDL (inherited
  dictionaries first, each one's members in lexicographic order, every
  read able to run a page's getter). An unknown name is NotSupportedError;
  a bad member is TypeError. Tier 1 builds the whole of this, so tiers 2
  and 3 add table rows and never touch it.
- **`CryptoKey`**: `type` ("secret", "public", "private"),
  `extractable`, `algorithm` (an object, the *same* object on every read;
  WPT's `crypto_key_cached_slots` tests that), `usages` (an array, cached
  the same way). The key material lives in an internal slot no script can
  reach, and `exportKey` refuses a non-extractable key (InvalidAccessError).
- **Errors are DOMExceptions by name**, and WPT holds each case to its name
  (and to their order, `export-key-error-order`): NotSupportedError
  (algorithm or operation unknown), SyntaxError (usages empty or not
  allowed), InvalidAccessError (the key is the wrong kind or lacks the
  usage), DataError (bad key data), OperationError (a decryption whose tag
  fails, among others).
- **JWK is checked before anything is made of it**, and the error-order
  tests bite here: `kty` must be the algorithm's; `alg`, if present, must
  name the algorithm and the key's length; `use`, if present, must agree
  with the usages ("enc" or "sig"); `key_ops`, if present, must hold
  every requested usage; `ext: false` with `extractable: true` is refused;
  `k`, `n`, `e`, `d` and the rest are base64url without padding, and
  refused otherwise. All of those are DataError. Raw AES keys are 128,
  192 or 256 bits and nothing else (DataError); an HMAC key with no
  `length` is the hash's block size long, and `length` 0 is a TypeError.
- **Promises settle "in parallel"**: the spec computes off the event loop
  and queues a task to settle. See § 5.
- **Inputs** are `BufferSource`: an ArrayBuffer or any view, its bytes
  *copied* at the call (the spec's "get a copy of the bytes held by"), so
  a page that changes the buffer afterwards cannot change the result.

## 4. Where it lives

- **`userland/libdom/subtle.c`**, the binding: the `SubtleCrypto` and
  `CryptoKey` interfaces, normalization, the BufferSource copies, JWK as
  the dictionary WebIDL makes of it (a JWK is an object to `importKey`
  and from `exportKey`, never JSON text), and the promises. It joins
  `crypto.c`'s install (`d_crypto_install`), under the same secure-context
  test, and the modern switch takes it away with the rest of P1-W.
- **`userland/libdom/subtle_core.c`**, pure: given a normalized request
  and bytes, the answer or an error name. Algorithm tables, key
  formats (raw, JWK's base64url fields, and in tier 3 the DER of spki and
  pkcs8), and the calls into BearSSL. No engine and no allocation of its
  own, so a host test holds it to the spec's and the RFCs' vectors under
  ASan, as `segment.c` and `ansi.c` are held.
- **The primitives are BearSSL's**, which `libtls/upstream` already
  vendors whole: SHA-1 and SHA-2, HMAC, HKDF, the AES block cipher and
  GCM (`br_gcm_*`, the tag checked with `br_gcm_check_tag_trunc`), ECDSA
  and EC key generation on P-256/384/521, RSA OAEP, PKCS#1 v1.5 and PSS
  sign and verify and key generation, and PKCS#8 private keys decoded and
  encoded. Constant-time, tested upstream, and the code every https
  connection already trusts. The fast paths are chosen at run time the
  way libtls chooses them: `br_aes_x86ni_*_get_vtable()` and
  `br_ghash_pclmul_get()` answer NULL on a CPU without the instructions,
  and the portable `ct64` AES and `ghash_ctmul64` serve then (the host
  harness runs both arms). P-521 has only the portable `br_ec_prime_i31`,
  slow enough to note and fast enough to use.
- **Ours, over BearSSL's AES block**, because WebCrypto's modes are not
  BearSSL's as written:
  - **One block at a time** goes through `br_aes_*_cbcenc_run` and
    `_cbcdec_run` on a single block with a zero IV, which is that block
    enciphered or deciphered (the IV is XORed in, and zero changes
    nothing). That is the door AES-KW and AES-CTR use; nothing reaches
    into BearSSL's `inner.h`.
  - **AES-CTR's counter.** WebCrypto's is a 128-bit counter block whose
    low `length` bits (1 to 128) count and wrap within themselves, the
    high bits fixed; BearSSL's CTR (`br_aes_*_ctr_run`) is a 96-bit IV and
    a 32-bit counter. They agree only for `length` 32 with no wrap, and
    the corpus passes 64 and 128. So the counter is ours, the keystream
    one block at a time through the door above.
  - **AES-CBC's PKCS#7 padding.** BearSSL's CBC is unpadded; WebCrypto's
    pads on encrypt and checks on decrypt, a bad pad an OperationError.
  - **AES-KW** (RFC 3394) and **PBKDF2** (RFC 8018 over BearSSL's HMAC),
    a few dozen lines each.
  - In tier 3, the **SubjectPublicKeyInfo** wrapper of a public key
    (spki), a small DER reader and writer, since BearSSL decodes public
    keys only inside certificates. ECDSA needs no DER at all: WebCrypto's
    signature is fixed-width `r||s`, BearSSL's `_raw` sign and verify; its
    raw EC public key is `0x04||X||Y`, which is `br_ec_public_key.q`; and
    ECDH's `deriveBits` is the x-coordinate of the curve's `mul`.
- **libdom links the BearSSL objects it uses privately** (question 4,
  answered). libdom.so is PIC, so what it links is the PIC family's
  BearSSL objects, the ones libtls.so is built from, as a subset list;
  sshd's precedent ("SSH uses private BearSSL primitives without widening
  libtls.so ABI", `userland/GNUmakefile`) is a static archive for an app
  and not what a library can take. What decides it is the harness:
  `tools/test_dom_host.sh` compiles libdom's sources with the host's `cc`
  and links no libtls, so with private objects it adds the subset's `.c`
  files, portable C, and every vector runs under ASan; a door exported
  from libtls.so would need a host-built libtls kept green for one
  consumer. libtls.so's interface stays the TLS client's.
- **RSA key generation's scratch** is the one buffer measured in
  kilobytes (`BR_RSA_KBUF_PRIV_SIZE`, `BR_RSA_KBUF_PUB_SIZE`), so the
  binding allocates it and `subtle_core.c` keeps "no allocation of its
  own". `br_rsa_i62_keygen_get()` answers NULL without 64-bit multiply
  support, and `i31` serves then, as libtls already picks. The public
  exponent is a `uint32_t` there and a BigInteger in WebCrypto: one that
  does not fit is an OperationError, which the spec allows for an
  exponent an implementation does not support, and every page passes
  65537.
- **Randomness** (generated keys, and RSA-OAEP's padding seed; BearSSL's
  ECDSA nonces are deterministic, RFC 6979, and its RSA needs no
  blinding) comes through the hook `crypto.c` already uses,
  `options.entropy`, never a second source. A refused hook is an
  OperationError, as it is for `getRandomValues`.
- **Key material** is held in `d_alloc`'d memory owned by the CryptoKey,
  zeroed before it is freed, and never copied into a JavaScript value
  unless `exportKey` is asked for an extractable key.

## 5. Divergences, stated

- **Promises settle as the call returns, not from a task** (question 5,
  answered). The work is done on the call and the promise returned
  already settled, as `Blob.text()` does (`d_promise_settled`). A page
  sees the result one microtask later instead of one task later; nothing
  in the corpus can tell, and WPT's tests await. DIVERGENCES.md row.
- **Its dividend: input is read where it lies.** The spec copies a
  BufferSource's bytes because its work runs in parallel and the page
  could change them meanwhile. Here no page code runs between the read
  and the result, so `digest`, `encrypt`, `decrypt`, `sign`, `verify` and
  `deriveBits` read the page's buffer in place and copy nothing; the copy
  is owed only for bytes that outlive the call, which are key material
  (`importKey`, `unwrapKey`, generated keys) and already live in the
  key's own memory. Otherwise a 200 MiB `digest` would cost 400 MiB while
  it ran. The care this asks: a getter in the algorithm dictionary runs
  during the call and can detach or resize the buffer, so the buffer's
  pointer and length are read after normalization, never before.
- **A long input blocks the page, boundedly.** The task deadline does not
  reach inside C. An ArrayBuffer is at most the engine's memory limit
  (256 MiB), and SHA-256 or AES-GCM in BearSSL's portable code is on the
  order of a few hundred MB/s on the P5, so the worst call is under a
  second, and a page cannot make it worse than its own heap allows. Tier
  1's PR measures a 256 MiB `digest` on the P5 and writes the number
  here.
- **RSA key generation blocks the page** for as long as it takes (a 2048-bit
  key is the slow case; to be measured on the P5 in tier 3). One host in
  the corpus generates RSA keys at all. If the P5 says it is too slow,
  that operation alone moves to yonder's worker pool, then; it is the one
  operation where "in parallel" would be felt.
- **A CryptoKey cannot be cloned** (`postMessage`, structured clone):
  DataCloneError, as for any object `clone.c` cannot clone. `clone.c`
  already takes platform objects (Blob, File, DOMException, ImageData),
  so the row is small, a key's three slots and a byte copy; DEBTS, sized
  S. No corpus page posts a key.
- **Window only, for now.** `crypto` and `subtle` are
  `[Exposed=(Window,Worker)]`, and a worker-based challenge calls
  `crypto.subtle` first thing; WORKERS.md gives the rows table `[Exposed]`
  bits, and the row is marked when they exist. BearSSL is safe across
  threads as used here: every context is the caller's, and the AES-NI and
  PCLMUL getters probe the CPU on every call.

## 6. Proof

- **WPT `WebCryptoAPI`**, through the runner, with
  `tools/wpt/expected/WebCryptoAPI.txt` gaining each tier's tests as
  `[required]` (they fail with the modern switch off) and its exclusions
  named with reasons: `digest/`, `idlharness`, `crypto_key_cached_slots`,
  `normalize-algorithm-name` and `export-key-error-order` in tier 1;
  `encrypt_decrypt/`, `sign_verify/` (HMAC), `import_export/` and
  `generateKey/` (the symmetric files), `wrapKey_unwrapKey/` in tier 2;
  the rest of those directories and `derive_bits_keys/` in tier 3. The
  tier-1 run also proves the methods it does not build answer
  NotSupportedError, not TypeError, after normalization.
- **Host vectors for `subtle_core.c`** under ASan: FIPS 180-4 digests,
  RFC 4231 (HMAC), NIST SP 800-38A/D (AES-CBC/CTR/GCM), RFC 3394
  (AES-KW), RFC 5869 (HKDF), RFC 6070 (PBKDF2), and in tier 3 JWK and DER
  round trips of keys made by a browser. Two families SP 800-38A does not
  cover: AES-CTR's counter wrapping at `length` bits (WPT's
  `aes_ctr_vectors.js` has the cases), and CBC's padding (a whole block
  of padding, a bad final byte, a bad interior byte). Both of the AES and
  GHASH arms, the AES-NI one and the portable one, run every vector. Hand mutants on the glue (an IV
  ignored, a tag not checked, a usage not enforced, a byte not copied)
  must fail.
- **The pages of § 1, replayed** (tier 1): the transaction id is real
  and the dialog's request goes out with it. What the server answers is
  the P5's to show.
- **Chrome as the referee** where WPT is silent: the error a given misuse
  gets, captured as INTL.md's capture page does it, only if a case comes
  up.

## 7. Costs

- libdom.so: the binding and core, plus about 100 KiB of BearSSL objects
  in the library's image (one copy, shared by every process that maps
  libdom.so); and the DOM host harness's build compiles that subset too.
- No kernel change, no new syscall, no new library.
- Review: Fable reads this design (Chris, 2026-11-07); each tier's PR goes
  through the groove, Codex included, since a mistake here is a security
  mistake in a page's own crypto even though os64 itself trusts none of
  it.

## 8. Questions

1. **The ruling: ship `crypto.subtle`**, which WINDOW_DOCUMENT.md
   question 1 left out, in the three tiers of § 2, every method present
   from tier 1 (NotSupportedError for what is not built yet). I lean
   yes. *(Chris)*
2. **ECDH in tier 3** (six more hosts in tier 2 if it moves, at the cost
   of bringing key pairs and spki forward). I lean tier 3, so tier 2 stays
   small. *(Chris)*
3. **RSA-PSS (and X25519) left out.** No page names either; both are
   cheap once tier 3's RSA and EC exist, since BearSSL has them (Fable
   corrected the premise). I lean out, per "the stuff things might need,
   not every single thing", with DEBTS rows that say "cheap". *(Chris)*
4. **Private BearSSL objects in libdom.** *Answered (Fable): yes, the PIC
   objects, for the host harness's sake (§ 4).*
5. **Settled promises.** *Answered (Fable): yes, with the DIVERGENCES row
   and the consequences in § 3 and § 5.*
6. **Who builds it**: I do, tier by tier, each tier its own PR. *(Chris)*
