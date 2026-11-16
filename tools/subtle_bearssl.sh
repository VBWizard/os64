# The BearSSL sources libdom's Web Cryptography calls (WEB_CRYPTO.md § 4),
# for the host harnesses that compile them with the host's cc: libdom links
# the same files' PIC objects privately (userland/libdom/library.mk's
# LIBDOM_BEARSSL), and tools/test_subtle_core_host.sh fails when the two
# lists differ.
# Sourced, not run.
SUBTLE_BEARSSL_SOURCES=(
    userland/libtls/upstream/src/hash/sha1.c
    userland/libtls/upstream/src/hash/sha2small.c
    userland/libtls/upstream/src/hash/sha2big.c
    userland/libtls/upstream/src/codec/dec32be.c
    userland/libtls/upstream/src/codec/enc32be.c
    userland/libtls/upstream/src/codec/dec64be.c
    userland/libtls/upstream/src/codec/enc64be.c
    userland/libtls/upstream/src/codec/dec32le.c
    userland/libtls/upstream/src/codec/enc32le.c
    userland/libtls/upstream/src/codec/dec64le.c
    userland/libtls/upstream/src/codec/enc64le.c
    userland/libtls/upstream/src/symcipher/aes_common.c
    userland/libtls/upstream/src/symcipher/aes_ct64.c
    userland/libtls/upstream/src/symcipher/aes_ct64_enc.c
    userland/libtls/upstream/src/symcipher/aes_ct64_dec.c
    userland/libtls/upstream/src/symcipher/aes_ct64_cbcenc.c
    userland/libtls/upstream/src/symcipher/aes_ct64_cbcdec.c
    userland/libtls/upstream/src/symcipher/aes_ct64_ctr.c
    userland/libtls/upstream/src/aead/gcm.c
    userland/libtls/upstream/src/hash/ghash_ctmul64.c
    userland/libtls/upstream/src/mac/hmac.c
    userland/libtls/upstream/src/kdf/hkdf.c
    userland/libtls/upstream/src/ec/ec_p256_m31.c
    userland/libtls/upstream/src/ec/ec_prime_i31.c
    userland/libtls/upstream/src/ec/ec_secp256r1.c
    userland/libtls/upstream/src/ec/ec_secp384r1.c
    userland/libtls/upstream/src/ec/ec_secp521r1.c
    userland/libtls/upstream/src/ec/ec_keygen.c
    userland/libtls/upstream/src/ec/ecdsa_i31_sign_raw.c
    userland/libtls/upstream/src/ec/ecdsa_i31_vrfy_raw.c
    userland/libtls/upstream/src/ec/ecdsa_i31_bits.c
    userland/libtls/upstream/src/rand/hmac_drbg.c
    userland/libtls/upstream/src/codec/ccopy.c
    userland/libtls/upstream/src/int/i31_add.c
    userland/libtls/upstream/src/int/i31_bitlen.c
    userland/libtls/upstream/src/int/i31_decmod.c
    userland/libtls/upstream/src/int/i31_decode.c
    userland/libtls/upstream/src/int/i31_encode.c
    userland/libtls/upstream/src/int/i31_fmont.c
    userland/libtls/upstream/src/int/i31_iszero.c
    userland/libtls/upstream/src/int/i31_modpow.c
    userland/libtls/upstream/src/int/i31_montmul.c
    userland/libtls/upstream/src/int/i31_muladd.c
    userland/libtls/upstream/src/int/i31_ninv31.c
    userland/libtls/upstream/src/int/i31_rshift.c
    userland/libtls/upstream/src/int/i31_sub.c
    userland/libtls/upstream/src/int/i31_tmont.c
    userland/libtls/upstream/src/int/i32_div32.c
)
SUBTLE_BEARSSL_FLAGS=(-D_DEFAULT_SOURCE -include userland/libtls/port/config.h
    -Iuserland/libtls/upstream/inc -Iuserland/libtls/upstream/src)
