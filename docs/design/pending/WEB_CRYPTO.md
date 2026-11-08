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

*Chris took every lean on questions 1, 2, 3 and 6 the same day ("unless
Fable disagreed with any of them, your leans are my leans"): `subtle`
ships, in these tiers; ECDH stays in tier 3; RSA-PSS and X25519 stay out,
booked as cheap; I build it, a PR per tier.*

*Tier 2 revisited question 5 on evidence: promises returned already
settled made a page's chain of calls one long run of microtasks, and
WPT's tier-2 pages, thousands of calls each, ran into the script
deadline. The work is still done on the call; the promise now
settles from a task (§ 5). The input-in-place dividend is unchanged.*

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
  `length` is the hash's block size long (generated) or the data's
  (imported), and `length` 0 is refused: an OperationError from
  `generateKey`, a DataError from `importKey`, which is also what a
  `length` past the data, or short of its last byte, gets.
- **Where the spec leaves the answer open**, tier 2 chose, as a web
  semantics call (decided, and stated here so a reviewer can disagree
  with one): an AES-CTR input longer than the counter's `length` bits can
  count without repeating a block is an OperationError, since repeated
  keystream gives away the plaintext; an empty AES-GCM `iv` is an
  OperationError (SP 800-38D asks at least one bit; the spec names no
  bound below); an HMAC `length` that is no multiple of 8 is taken, the
  key's last byte holding that many bits and the rest zero; base64url is
  read strictly, unpadded and in the URL alphabet only, with no stray
  bits past the last byte (DataError otherwise); and a JWK `key_ops` that
  names a usage twice is a DataError (RFC 7517 § 4.3 says it must not);
  an HMAC key `generateKey` is asked to make longer than 65,536 bytes
  (what `getRandomValues` fills at most) is an OperationError, the
  spec's "the key generation step fails"; and an unwrapped JWK is read
  as the spec's new global object would read it (own members only,
  arrays by index, nothing a page put on `Object.prototype`), except that
  a member that should be a string and is an object or array is a
  DataError rather than that object's string.
- **Promises settle "in parallel"**: the spec computes off the event loop
  and queues a task to settle. See § 5.
- **Inputs** are `BufferSource`: an ArrayBuffer or any view, its bytes
  copied once the algorithm is normalized (the spec's "get a copy of the
  bytes held by", which WPT's `digest/` holds after normalization: a name
  getter that refills or detaches the buffer is seen), so a page that
  changes the buffer after the call cannot change the result.

## 4. Where it lives

- **`userland/libdom/subtle.c`**, the binding: the `SubtleCrypto` and
  `CryptoKey` interfaces, normalization, the BufferSource reads, JWK as
  the dictionary WebIDL makes of it (a JWK is an object to `importKey`
  and from `exportKey`, never JSON text), and the promises. As built in
  tier 1: each method's arguments are a table (`kMethods`: name, required
  count, the IDL type of each, the operation its algorithm normalizes
  for), so tiers 2 and 3 add steps, not conversions; `importKey`'s
  `keyData`, when it is no BufferSource, is converted as the JsonWebKey
  dictionary on the spot, every member in order, as WebIDL converts a
  dictionary argument, so its getters run before the algorithm is
  normalized, and a usage sequence is converted item by item as it is
  iterated. KeyFormat and KeyUsage are the editor's draft's enumerations
  whole, its newer formats (`raw-public`, `raw-private`, `raw-seed`,
  `raw-secret`) and usages (`encapsulateKey` and the three like it)
  included, though no algorithm built here takes them: the enumeration
  decides only whether a value converts, and WPT holds the draft's IDL,
  so `encapsulateKey` on an AES key is a SyntaxError (a usage the
  algorithm refuses), not a TypeError (no such usage). The interfaces' members, and
  Crypto's, are shaped as WebIDL makes them (operations enumerable,
  accessors named "get x") by `d_idl_members`, which `window.c` already
  did for Window's own members and now shares. It joins
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
  connection already trusts. **os64 builds BearSSL as a scalar baseline**
  (`libtls/port/config.h`: `BR_AES_X86NI 0`, `BR_SSE2 0`, `BR_INT128 0`),
  so the AES-NI and PCLMUL getters (`br_aes_x86ni_*_get_vtable()`,
  `br_ghash_pclmul_get()`) answer NULL and the portable `ct64` AES and
  `ghash_ctmul64` are what run, here as in libtls; the code asks the
  getters anyway, so a build that turns them on takes them. P-521 has only
  the portable `br_ec_prime_i31`, slow enough to note and fast enough to
  use. (The review assumed the fast paths were live; the configuration
  says otherwise.)
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
  support, which os64's configuration leaves off (`BR_INT128`,
  `BR_UMUL128`), so `i31` serves, as libtls already picks. The public
  exponent is a `uint32_t` there and a BigInteger in WebCrypto: one that
  does not fit is an OperationError, which the spec allows for an
  exponent an implementation does not support, and every page passes
  65537.
- **Randomness** (generated keys, and RSA-OAEP's padding seed; BearSSL's
  ECDSA nonces are deterministic, RFC 6979, and its RSA needs no
  blinding) comes through the hook `crypto.c` already uses,
  `options.entropy`, never a second source. A refused hook is an
  OperationError, as it is for `getRandomValues`.
- **Key material** is held in engine-heap memory (`js_malloc`, so a page's
  keys count against its script heap) owned by the CryptoKey, zeroed
  before it is freed (the key's finalizer), and never copied into a
  JavaScript value unless `exportKey` or `wrapKey` is asked for an
  extractable key. Every scratch copy a step makes (a generated key's
  random bytes, a decoded JWK `k`, a key serialized for wrapping) is
  zeroed before it is freed too.

## 5. Divergences, stated

- **The work is done on the call; the promise settles from a task.** The
  spec works in parallel and queues a task to settle. Here the answer is
  computed before the method returns and waits in a queue, in call
  order, for the task that settles it: one binding timer of no delay
  settles every answer waiting when it runs (subtle.c's THE SETTLE
  QUEUE), so a page's reaction runs in a task of its own, as it does in
  other browsers. Question 5's first answer returned the promise already
  settled, as `Blob.text()` does; tier 2 measured what that costs: a
  page that chains thousands of calls (WPT's tier-2 pages, and any page
  that encrypts in a loop) became one run of microtasks that the script
  deadline ended. One timer for the whole queue, not one per call,
  because the page's timers have a quota (4096 pending) and a page with
  thousands of calls in flight must still be able to `setTimeout`, and
  because the promises must settle in the order they were made. Only a
  page whose own timers hold the whole quota finds no task to be had;
  its promise is settled on the call, which is the old behaviour and
  still the right answer, only sooner. DIVERGENCES.md row.
- **The dividend of doing the work on the call: input is read where it
  lies.** The spec copies a
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
  1's PR measures it on the P5 with the timing section of tier 1's
  validation page (`/tests/pages/webcrypto-tier1.html`, § 6), which times
  a whole `digest` call at sizes up to 192 MiB (the script heap is 256
  MiB), and reports the numbers.
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
- **Window only.** `crypto` and `subtle` are
  `[Exposed=(Window,Worker)]`, and a worker-based challenge calls
  `crypto.subtle` first thing; WORKERS.md gives the rows table `[Exposed]`
  bits, and the row is marked when they exist. BearSSL is safe across
  threads as used here: every context is the caller's, and its fast-path
  getters keep no state (in a build that has those paths, they probe the
  CPU on each call).

## 6. Proof

- **WPT `WebCryptoAPI`**, through the runner, with
  `tools/wpt/expected/WebCryptoAPI.txt` gaining each tier's tests as
  `[required]` (they fail with the modern switch off) and its exclusions
  named with reasons: `digest/`, `idlharness` and
  `normalize-algorithm-name` in tier 1; `crypto_key_cached_slots`,
  `export-key-error-order` (both need a key), `encrypt_decrypt/`,
  `sign_verify/` (HMAC), `import_export/` and `generateKey/` (the
  symmetric files) in tier 2; `wrapKey_unwrapKey/` in tier 3, whose
  files wrap with AES-KW and RSA-OAEP;
  the rest of those directories and `derive_bits_keys/` in tier 3. The
  tier-1 run also proves the methods it does not build answer
  NotSupportedError, not TypeError, after normalization.
- **Host vectors for `subtle_core.c`** under ASan: FIPS 180-4 digests,
  RFC 4231 (HMAC), NIST SP 800-38A/D (AES-CBC/CTR/GCM), RFC 3394
  (AES-KW), RFC 5869 (HKDF), RFC 6070 (PBKDF2), and in tier 3 JWK and DER
  round trips of keys made by a browser. Two families SP 800-38A does not
  cover: AES-CTR's counter wrapping at `length` bits (WPT's
  `aes_ctr_vectors.js` has the cases), and CBC's padding (a whole block
  of padding, a bad final byte, a bad interior byte). The portable AES
  and GHASH are the arms this build has (§ 4); a build that turned on the
  AES-NI and PCLMUL paths would run every vector through both. Hand mutants on the glue (an IV
  ignored, a tag not checked, a usage not enforced, a byte not copied)
  must fail.
- **The pages of § 1, replayed** (tier 1): the transaction id is real
  and the dialog's request goes out with it. What the server answers is
  the P5's to show.

**Tier 1, as proved (2026-11-07):**
- `tools/test_subtle_core_host.sh`: 66 checks against FIPS 180-2's
  vectors for all four hashes (the empty message, "abc", the 448-bit
  message, a million "a") and the registry's matching; it also fails when
  `library.mk`'s BearSSL list and the harnesses' copy differ.
- WPT WebCryptoAPI: 597 of 610 pass on the pages tier 1 runs (`digest/`,
  `idlharness`, `normalize-algorithm-name`, `generateKey/failures_bad_algorithm`,
  `importKey-unsettled-promise`, `historical`, the secure-context page,
  and `getRandomValues`/`randomUUID` as before); 536 of them are
  `[required]` and none passes with the modern switch off. The 13 left:
  getRandomValues' nine "Large length" (QuotaExceededError's interface,
  booked before this), the two names that need tier 3's `importKey`, the
  worker case, and `window.crypto`'s getter taking any receiver (booked).
  Every other page names its tier, or why it is out, in the exclusions.
- The DOM host's window and modern subsets, with tier 1's cases
  (`tools/test_dom_window.inc`: the interfaces, digest over every buffer
  source, every failure a rejection, the name read twice, the buffer read
  after it, absence on an insecure page).
- The social network's sign-in page, replayed from its P5 record with
  this build: every request carries a real transaction id, the same bytes
  the JavaScript SHA-256 stand-in produced. On the P5 the dialog's
  sign-in step now loads (its fetch answers 200, 6.5 KB), and the next
  wall is `window.open`, the popup the identity provider's button opens
  (IFRAME.md keeps popups out; a design about windows).
- `tools/pages/webcrypto-tier1.html`: 29 checks, every one PASS in yonder
  and in a Chrome-equivalent engine (node's WebCrypto).
**Tier 2, as proved (2026-11-07):**
- `tools/test_subtle_core_host.sh`: 111 checks. NIST SP 800-38A's CBC
  (F.2.1, with its block of padding) and CTR (F.5.1); the GCM paper's
  test case 4, with a 96-bit tag and with an 8-byte IV; AES-CTR counters
  of 8, 4 and 16 bits wrapping within themselves and a 1-bit counter
  refusing its third block (values node's WebCrypto computed; a mutant
  that wrapped at the byte proved the 4- and 16-bit cases necessary);
  CBC's bad pad and ragged input; RFC 4231's and RFC 2202's HMAC case 1
  for all four hashes; RFC 4648's base64url; the registry's rows.
- WPT WebCryptoAPI: the tier-2 pages (`encrypt_decrypt/` for AES,
  `sign_verify/hmac`, the symmetric `import_export/` and `generateKey/`
  files, `crypto_key_cached_slots`, `export-key-error-order`) pass but
  for tests that need tier 3 (AES-KW, HKDF, PBKDF2, an ECDSA key); 5381
  of them join `[required]`, and with the modern switch off 5950
  required tests fail, as they should.
- The DOM host's window subset, with tier 2's cases: a round trip in
  each AES mode, RFC 4231's HMAC, `verify`'s three answers, a key's
  properties, JWK export and back, `wrapKey`/`unwrapKey` as raw and as
  JWK, the refusals, a result arriving after a microtask queued with
  the call, wrapping under each mode, HMAC lengths within a byte and
  past what is generated, an unwrapped JWK read past a polluted
  `Object.prototype`, and 5000 calls in flight that settle in call order
  while the page's own `setTimeout` still works. A cold reader's six
  findings (a 32-bit length sum that wrapped, the timer quota, the
  unwrapped JWK's realm, an error name, keystream and a MAC left on the
  stack, the tests for each) are fixed in the same change.
- `tools/pages/webcrypto-tier2.html`: 50 checks, every one PASS in
  yonder's window (run through the WPT runner as a testharness page) and
  in node's WebCrypto, which spells InvalidAccessError
  `InvalidAccessException` in two of them; the spec's name is the page's.
- **A validation page per tier, and a live page** (Chris, 2026-11-07:
  each tier shows it works on a page, ours or one on the Internet). Ours
  are `tools/pages/webcrypto-tier<n>.html`, which ship to `/tests/pages`:
  each check a published vector or a behaviour the spec requires, PASS or
  FAIL on the page, every expectation confirmed in a Chrome-equivalent
  engine before it ships, and written so a later tier does not turn it
  red (its NotSupportedError checks name algorithms no browser knows). A
  page from disk is a secure context, as Chrome and Firefox count one
  (`d_secure_context`; tier 1 brought that, or `crypto.subtle` would be
  absent there). The live page is a public WebCrypto examples site whose
  table marks every algorithm and operation the visiting browser supports
  with ✓ or ✗: Chrome beside yonder, tier by tier, the ✓s should grow to
  match (its legacy algorithms, AES-CFB, AES-CMAC, DH, CONCAT and
  HKDF-CTR, are ✗ in every current browser).
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
   and the consequences in § 3 and § 5. Revisited in tier 2 on evidence
   (§ 5): the promise settles from a task; the work stays on the call.*
6. **Who builds it**: I do, tier by tier, each tier its own PR. *(Chris)*
