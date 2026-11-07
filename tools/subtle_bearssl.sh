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
)
SUBTLE_BEARSSL_FLAGS=(-D_DEFAULT_SOURCE -include userland/libtls/port/config.h
    -Iuserland/libtls/upstream/inc -Iuserland/libtls/upstream/src)
