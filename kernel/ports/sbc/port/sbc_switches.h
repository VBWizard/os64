// sbc_switches.h — every configuration switch the SBC encoder's headers
// read, set here, once, for every build of it: the kernel's (sbc.mk
// force-includes this ahead of each upstream file), sbc_port.c's (which
// includes it first, because SBC_ENC_PARAMS's layout depends on
// SBC_JOINT_STE_INCLUDED), and the host proof's (tools/test_sbc_host.sh).
//
// Each value is upstream's own default (include/sbc_encoder.h), written out
// so that the configuration is a decision on this side and not whatever a
// newer header defaults to. tools/test_sbc_audit.py fails if upstream's
// headers grow a switch this file does not set. kernel/ports/sbc/README.md
// says what each one does.

#ifndef SBC_SWITCHES_H
#define SBC_SWITCHES_H

// As sbc_encoder.h spells them, so its own #ifndef finds them identical.
#define FALSE 0
#define TRUE (!FALSE)

// The integer paths. SBC_ARM_ASM_OPT and SBC_DSP_OPT would substitute ARM
// assembly and an external multiply. SBC_IPAQ_OPT unrolls the window with
// constant coefficients (accumulated in int32_t, since
// SBC_IS_64_MULT_IN_WINDOW_ACCU is FALSE) and does the DCT's 32x16 multiply
// as one int64_t product, which x86-64 does natively.
#define SBC_ARM_ASM_OPT FALSE
#define SBC_DSP_OPT FALSE
#define SBC_IPAQ_OPT TRUE
#define SBC_GHS_COMPILER FALSE
#define SBC_ARM_COMPILER TRUE      // read only when SBC_ARM_ASM_OPT is TRUE
#define SBC_USE_ARM_PRAGMA FALSE

// Arithmetic precision. The encoder's Android.bp sets none of these.
#define SBC_IS_64_MULT_IN_WINDOW_ACCU FALSE
#define SBC_IS_64_MULT_IN_IDCT FALSE
#define SBC_IS_64_MULT_IN_QUANTIZER TRUE
#define SBC_FAST_DCT TRUE

// What the encoder can produce, and its working storage.
#define SBC_JOINT_STE_INCLUDED TRUE
#define SBC_MAX_NUM_FRAME 1
#define ENC_VX_BUFFER_SIZE (MINIMUM_ENC_VX_BUFFER_SIZE + 64)

#endif
