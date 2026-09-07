#ifndef OS64_BEARSSL_CONFIG_H
#define OS64_BEARSSL_CONFIG_H

// Entropy and validation time enter through the caller, including on hosts
// where upstream could otherwise discover libc or CPU facilities.
#define BR_USE_GETENTROPY 0
#define BR_USE_URANDOM 0
#define BR_USE_WIN32_RAND 0
#define BR_USE_UNIX_TIME 0
#define BR_USE_WIN32_TIME 0
#define BR_RDRAND 0

// Scalar x86-64 baseline. Disable optional intrinsics and 128-bit integer
// backends so backend selection does not depend on the build host.
#define BR_AES_X86NI 0
#define BR_SSE2 0
#define BR_POWER8 0
#define BR_INT128 0
#define BR_UMUL128 0
#define BR_64 1
#define BR_LOMUL 0
#define BR_SLOW_MUL 0
#define BR_SLOW_MUL15 0
// Use byte loads instead of the x86 union-pointer shortcut, which performs
// misaligned C accesses even though the hardware accepts the instructions.
#define BR_LE_UNALIGNED 0
#define BR_BE_UNALIGNED 0

#endif
