# sources.mk — what libmath.so is built from. manifest.json pins each
# upstream file; tools/test_math_audit.py checks the two agree.

# One file per public function, except sqrt (port/sqrt.c).
MATH_PUBLIC_NAMES := acos acosh asin asinh atan atan2 atanh cbrt ceil cos \
    cosh exp expm1 fabs floor fmax fmin fmod hypot log log10 log1p log2 \
    lrint pow round sin sinh tan tanh trunc

# The helpers those call: kernels, range reduction, the error-result
# builders, and the tables behind exp, log, log2 and pow.
MATH_SUPPORT_NAMES := __cos __expo2 __math_divzero __math_invalid \
    __math_oflow __math_uflow __math_xflow __rem_pio2 __rem_pio2_large \
    __sin __tan exp_data log2_data log_data pow_data rint scalbn

MATH_UPSTREAM_SRCS := $(patsubst %,libmath/upstream/src/math/%.c,$(MATH_PUBLIC_NAMES) $(MATH_SUPPORT_NAMES))
MATH_PORT_SRCS     := libmath/port/sqrt.c
