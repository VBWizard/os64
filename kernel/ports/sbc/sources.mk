# sources.mk — what the kernel's SBC encoder is built from. manifest.json
# pins each upstream file; tools/test_sbc_audit.py checks the two agree, and
# tools/test_sbc_host.sh builds the same lists for the host. Paths are
# relative to kernel/.

SBC_UPSTREAM_SRCS := \
    ports/sbc/upstream/srce/sbc_analysis.c \
    ports/sbc/upstream/srce/sbc_dct.c \
    ports/sbc/upstream/srce/sbc_dct_coeffs.c \
    ports/sbc/upstream/srce/sbc_enc_bit_alloc_mono.c \
    ports/sbc/upstream/srce/sbc_enc_bit_alloc_ste.c \
    ports/sbc/upstream/srce/sbc_enc_coeffs.c \
    ports/sbc/upstream/srce/sbc_encoder.c \
    ports/sbc/upstream/srce/sbc_packing.c
SBC_PORT_SRCS := ports/sbc/port/sbc_port.c
