// Host check for fpu_demo's homemade log2 and pow2 (there is no libm on
// os64, so the demo carries its own, built from the double's exponent bits).
// Build and run: gcc -O2 -o /tmp/mm tools/test_mandel_math_host.c -lm && /tmp/mm
// Keep the two functions IDENTICAL to userland/apps/fpu_demo/fpu_demo.c —
// this file exists so a retuned polynomial is measured before it ships.
#include <stdio.h>
#include <stdint.h>
#include <math.h>

static double log2_approx(double x)
{
	union { double d; uint64_t u; } bits = { .d = x };
	int e = (int)((bits.u >> 52) & 0x7FF) - 1023;
	bits.u = (bits.u & 0x000FFFFFFFFFFFFFULL) | 0x3FF0000000000000ULL;
	double t = bits.d - 1.0;
	double p = t * (1.4425 + t * (-0.7071 + t * (0.3597 - t * 0.0952)));
	return (double)e + p;
}

static double pow2(double p)
{
	int ip = (int)p;
	if (p < 0.0 && (double)ip != p) ip--;
	double fr = p - (double)ip;
	double f = 1.0 + fr * (0.6931472 + fr * (0.2402265 + fr * (0.0555041 + fr * 0.0096181)));
	union { double d; uint64_t u; } bits = { .u = (uint64_t)(ip + 1023) << 52 };
	return bits.d * f;
}

int main(void)
{
	double worst_log2 = 0, worst_pow2 = 0;
	for (double x = 1e-3; x < 1e6; x *= 1.0137) {
		double err = fabs(log2_approx(x) - log2(x));
		if (err > worst_log2) worst_log2 = err;
	}
	for (double p = -5; p < 40; p += 0.0173) {
		double err = fabs(pow2(p) / exp2(p) - 1);
		if (err > worst_pow2) worst_pow2 = err;
	}
	printf("log2 max abs err %.2e (budget 2e-3)   pow2 max rel err %.2e (budget 1e-3)\n", worst_log2, worst_pow2);
	return (worst_log2 <= 2e-3 && worst_pow2 <= 1e-3) ? 0 : 1;
}
