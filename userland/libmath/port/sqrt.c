/*
 * sqrt.c — libmath's square root is the CPU's.
 *
 * SSE2 is os64's baseline and its sqrtsd is correctly rounded by IEEE 754's
 * definition, in whatever rounding mode MXCSR holds. musl's own x86-64 sqrt
 * (src/math/x86_64/sqrt.c) is that single instruction, written as inline asm
 * in AT&T operand order that os64's -masm=intel would misread. The builtin
 * says the same thing in no syntax at all; shared.mk compiles this file with
 * -fno-math-errno, which is what lets GCC emit the bare instruction instead
 * of a call back into sqrt to set errno for a negative argument.
 *
 * Every musl routine that takes a square root (acos, asin, acosh, asinh,
 * hypot) calls this export, so musl's generic C sqrt and its table are not
 * built at all. tools/test_math_audit.py checks the disassembly.
 */
#include <math.h>

double sqrt(double x)
{
    return __builtin_sqrt(x);
}
