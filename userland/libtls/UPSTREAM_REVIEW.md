# Upstream pin review

Reviewed 2026-09-07 for the os64 foundation import. The official repository
HEAD is `7bea48e5e850ab4cafbe68d3765cdaba13a86d6f` (2026-04-06),
39 commits after v0.6. This is a source-selection review, not an independent
cryptographic audit or a production TLS approval.

## Reasons to use the reviewed commit

- `7bea48e` rejects CBC record lengths that are not block multiples before
  decryption. The fixture exercises both IV modes with 3DES, AES-small,
  AES-big, and AES-ct64. CBC remains outside the proposed public TLS profile.
- `3479195` makes certificate/private-key decoder errors terminal across
  subsequent chunks. The fixture checks that a failed decoder stays unchanged.
- `b715b43` corrects the private-key decoder buffer bound. The T0 and generated
  C changes agree. Private-key import is outside the client-facing slice.
- `87a796d` corrects RSA modulus scratch sizing at maximum key size.
- `52a69fe` corrects Curve25519 scalar endianness; `dda1f8a` normalizes invalid
  point-length rejection. The selected scalar EC code includes these changes.
- `252dba9` and `b2ec203` correct carries in the added P-256 m62/m64 code.
  Both fixes are retained, but the os64 configuration disables these backends.
- `6a691e6` corrects RSA-PSS checking with unequal hash/salt lengths. PSS is
  retained in the reference core; the proposed TLS 1.2 profile does not add
  RSA-PSS signature negotiation merely by importing it.
- `946f5ba` discards unread application data on explicit TLS close. The public
  close/drain contract must account for this behavior when the engine is built.
- `d40d23b` adds the certificate validity-range callback; `46f7ddd` makes its
  availability detectable. The callback provides dates, not an EKU policy
  hook: the certificate-policy work described in TLS.md remains necessary.
- `e4edfb8` adds getentropy and an AMD RDRAND workaround. Both mechanisms are
  explicitly disabled in the os64 build, along with the older OS providers.

The review also covered the scalar integer arithmetic changes, selected EC
length checks, UTF-8 acceptance correction, default-backend changes, and the
build/generated-code dependencies. New SHAKE, PSS, and 64-bit EC code is
retained for reference reproducibility; its presence does not enable new
public algorithms. No claim is made that this list exhausts unknown upstream
vulnerabilities. Upstream tests provide regression evidence, not a proof.

## Complete release-to-pin commit inventory

- `966078b` Added SHAKE implementation.
- `420f50c` Added stand-alone RSA/PSS implementation.
- `c6ffcd2` Fixed warning on GCC 4.6 to 4.9 (macro redefinition).
- `431629d` Changed speed benchmark for i31 to a 521-bit modulus.
- `fd98320` Cosmetic fix (value did not conform to its announced bit length, but this did not have bad consequences since br_i31_decode_mod() is lenient on that).
- `52a69fe` Fixed endianness in Curve25519 implementation (no consequence on security). Also added new Curve25519 code for 64-bit platforms.
- `b2a08e9` Made ec_c25519_m62 implementation the default on supported architectures.
- `f0ddbc3` Added new 64-bit implementations of Curve25519 and P-256.
- `d5acc4f` Made m64 implementations of elliptic curves the default (when available).
- `08eb078` Fixed fd leak in test code.
- `001d094` Some small performance improvements on 32-bit architectures.
- `6433cc2` Added detection for MIPS64 with n32 ABI.
- `87a796d` Fixed computing of intermediate buffer size for maximum-size RSA keys.
- `c1bb535` Small workaround for CompCert compatibility.
- `ecdf897` Normalize use of BR_DOXYGEN_IGNORE.
- `9721b3e` Fixed efficiency pre-test on RSA prime generation (no security issue, but RSA key generation with pubexp 5, 7 or 11 may be slightly more efficient).
- `924921d` Fixed mishandling of UTF-8 codepoints in the FDF0..FEDF range (these were unduly rejected when extracting names from certificates, thereby preventing use of the extra presentation forms of Arabic).
- `e4edfb8` Added support for getrandom()/getentropy(), and a fix for the RDRAND bug on AMD CPU (family 22).
- `2893441` Fixed a spurious warning on some compilers.
- `b715b43` Fixed buffer overflow in private key decoding (wrong buffer length used in size check).
- `4b60464` Fixed small display bug in debug tool.
- `fb4296c` Fixed some errors in comments.
- `69807a3` Fixed typo in comment.
- `15b3af7` Typo fix in comment.
- `252dba9` Fixed carry propagation bug in P-256 'm62' implementation (found by Auke Zeilstra; consequences unclear, possibly some invalid curve attacks in static ECDH contexts).
- `946f5ba` Added discard of unread appdata on explicit close.
- `acc70b1` Typo fix in comment.
- `dda1f8a` Harmonized behaviour when point length is invalid.
- `b2ec203` Fixed carry propagation bug in m64 impl for P-256.
- `79b1a99` Fixed comment.
- `d40d23b` Added generic API for date range validation (with callbacks).
- `6a691e6` Fixed RSA PSS verificatiobn bug (when hash_len != salt_len).
- `46f7ddd` Added macro that indicates presence of the time callback feature. Also added C++ compatibility.
- `79c060e` Fixed spurious warning about old-style prototype.
- `3c04036` Fix: make static ECDH selectable with the br_ssl_client_set_single_ec() helper function.
- `3d9be2f` Added dependencies to better support parallel or shuffled compilation.
- `3479195` Fixed chunked decoding in case of errors (if decoding failed at some point, subsequent chunks should be ignored, trying to reenter the decoder after a failure is a recipe for Bad Thing). Impacted functions were not used over malicious on-the-wire data for "normal" SSL/TLS usage.
- `8f795e5` Fixed Makefile dependencies.
- `7bea48e` Fixed bug in handling incoming records with invalid length (impacted CBC encryption with 3DES or with the aes_small or aes_big AES implementations; only 3DES was selectable by default).
