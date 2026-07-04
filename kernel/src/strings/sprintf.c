/* -*- linux-c -*- ------------------------------------------------------- *
 *
 *   Copyright (C) 1991, 1992 Linus Torvalds
 *   Copyright 2007 rPath, Inc. - All Rights Reserved
 *
 *   This file is part of the Linux kernel, and is made available under
 *   the terms of the GNU General Public License version 2.
 *
 * ----------------------------------------------------------------------- */

#include <stddef.h>
#include <stdint.h>
#include "sprintf.h"
#include "io.h"
#include "strings.h"
#include "memset.h"
#include "strlen.h"

extern volatile uint64_t kTicksSinceStart;

static int skip_atoi(const char **s)
{
	int i = 0;

	while (ISDIGIT(**s))
		i = i * 10 + *((*s)++) - '0';
	return i;
}

#define ZEROPAD	1		/* pad with zero */
#define SIGN	2		/* unsigned/signed long */
#define PLUS	4		/* show plus */
#define SPACE	8		/* space if plus */
#define LEFT	16		/* left justified */
#define SMALL	32		/* Must be 32 == 0x20 */
#define SPECIAL	64		/* 0x */

#define __do_div(n, base) ({ \
int __res; \
__res = ((unsigned long) n) % (unsigned) base; \
n = ((unsigned long) n) / (unsigned) base; \
__res; })

// Bounded store used by the formatter below. It stores the byte only while the
// cursor is still inside the caller's buffer (str < end), but ALWAYS advances
// the cursor, so the return value is the would-be length (C99 snprintf
// semantics) and overlong output is truncated instead of overflowing the
// buffer. The argument is evaluated into a temporary FIRST so side effects
// (va_arg, *s++) always happen even when we are past `end`; otherwise a
// truncated %c/%s would desync the variadic arguments. Relies on `str`/`end`
// being in scope (number() and format_core()); #undef'd at end of file.
#define EMIT(ch) do { char __emit_c = (char)(ch); if (str < end) *str = __emit_c; ++str; } while (0)

static char *number(char *str, char *end, long num, int base, int size, int precision,
		    int type)
{
	/* we are called with base 8, 10 or 16, only, thus don't need "G..."  */
	static const char digits[16] = "0123456789ABCDEF"; /* "GHIJKLMNOPQRSTUVWXYZ"; */

	char tmp[66];
	char c, sign, locase;
	int i;

	/* locase = 0 or 0x20. ORing digits or letters with 'locase'
	 * produces same digits or (maybe lowercased) letters */
	locase = (type & SMALL);
	if (type & LEFT)
		type &= ~ZEROPAD;
	if (base < 2 || base > 16)
		return NULL;
	c = (type & ZEROPAD) ? '0' : ' ';
	sign = 0;
	if (type & SIGN) {
		if (num < 0) {
			sign = '-';
			num = -num;
			size--;
		} else if (type & PLUS) {
			sign = '+';
			size--;
		} else if (type & SPACE) {
			sign = ' ';
			size--;
		}
	}
	if (type & SPECIAL) {
		if (base == 16)
			size -= 2;
		else if (base == 8)
			size--;
	}
	i = 0;
	if (num == 0)
		tmp[i++] = '0';
	else
		while (num != 0)
			tmp[i++] = (digits[__do_div(num, base)] | locase);
	if (i > precision)
		precision = i;
	size -= precision;
	if (!(type & (ZEROPAD + LEFT)))
		while (size-- > 0)
			EMIT(' ');
	if (sign)
		EMIT(sign);
	if (type & SPECIAL) {
		if (base == 8)
			EMIT('0');
		else if (base == 16) {
			EMIT('0');
			EMIT('X' | locase);
		}
	}
	if (!(type & LEFT))
		while (size-- > 0)
			EMIT(c);
	while (i < precision--)
		EMIT('0');
	while (i-- > 0)
		EMIT(tmp[i]);
	while (size-- > 0)
		EMIT(' ');
	return str;
}

static int format_core(char *buf, size_t size, const char *fmt, va_list args)
{
	int len;
	unsigned long num;
	int i, base;
	char *str;
	const char *s;

	int flags;		/* flags to number() */

	int field_width;	/* width of output field */
	int precision;		/* min. # of digits for integers; max
				   number of chars for from string */
	int qualifier;		/* 'h', 'l', or 'L' for integer fields */

	// Last writable content position: buf[0..size-2] hold content, buf[size-1]
	// holds the terminating NUL, so stores are allowed while (str < end). Callers
	// that want no size limit (vsprintf) pass SIZE_MAX; clamp `end` to the top of
	// the address space so buf+size can't overflow and the guard is simply never
	// hit — making the unbounded path byte-identical to the old vsprintf.
	char *end;
	if (size == 0)
		end = buf;
	else if (size > (size_t)(UINTPTR_MAX - (uintptr_t)buf))
		end = (char *)UINTPTR_MAX;
	else
		end = buf + (size - 1);

	for (str = buf; *fmt; ++fmt) {
		if (*fmt != '%') {
			EMIT(*fmt);
			continue;
		}

		/* process flags */
		flags = 0;
	      repeat:
		++fmt;		/* this also skips first '%' */
		switch (*fmt) {
		case '-':
			flags |= LEFT;
			goto repeat;
		case '+':
			flags |= PLUS;
			goto repeat;
		case ' ':
			flags |= SPACE;
			goto repeat;
		case '#':
			flags |= SPECIAL;
			goto repeat;
		case '0':
			flags |= ZEROPAD;
			goto repeat;
		}

		/* get field width */
		field_width = -1;
		if (ISDIGIT(*fmt))
			field_width = skip_atoi(&fmt);
		else if (*fmt == '*') {
			++fmt;
			/* it's the next argument */
			field_width = va_arg(args, int);
			if (field_width < 0) {
				field_width = -field_width;
				flags |= LEFT;
			}
		}

		/* get the precision */
		precision = -1;
		if (*fmt == '.') {
			++fmt;
			if (ISDIGIT(*fmt))
				precision = skip_atoi(&fmt);
			else if (*fmt == '*') {
				++fmt;
				/* it's the next argument */
				precision = va_arg(args, int);
			}
			if (precision < 0)
				precision = 0;
		}

		/* get the conversion qualifier */
		qualifier = -1;
		if (*fmt == 'h' || *fmt == 'l' || *fmt == 'L') {
			qualifier = *fmt;
			++fmt;
		}

		/* default base */
		base = 10;

		switch (*fmt) {
		case 'c':	//character
			if (!(flags & LEFT))
				while (--field_width > 0)
					EMIT(' ');
			EMIT((unsigned char)va_arg(args, int));
			while (--field_width > 0)
				EMIT(' ');
			continue;

		case 's':	//string
			s = va_arg(args, char *);
			if (s) //CLR 01/09/2019: Handling case where pointer is NULL
                            len = strlen(s);
                        else
                            len = 0;

			if (!(flags & LEFT))
				while (len < field_width--)
					EMIT(' ');
			for (i = 0; i < len; ++i)
				EMIT(*s++);
			while (len < field_width--)
				EMIT(' ');
			continue;

		case 'p':	//pointer
			if (field_width == -1) {
				field_width = 2 * sizeof(void *);
				flags |= ZEROPAD;
			}
			str = number(str, end,
				     (unsigned long)va_arg(args, void *), 16,
				     field_width, precision, flags);
			continue;

		case 'n':	//Number of characters printed
			if (qualifier == 'l') {
				long *ip = va_arg(args, long *);
				*ip = (str - buf);
			} else {
				int *ip = va_arg(args, int *);
				*ip = (str - buf);
			}
			continue;

		case '%':	//Literal % character
			EMIT('%');
			continue;

			/* integer number formats - set up the flags and "break" */
		case 'o':	//Octal
			base = 8;
			break;

		case 'x':	//lower case hex
			flags |= SMALL;
			[[fallthrough]]; // Tells the compiler this fallthrough is intentional
		case 'X':	//upper case hex
			base = 16;
			break;

		case 'd':	//signed integer
		case 'i':
			flags |= SIGN;
		case 'u':	//unsigned integer
			break;

		default:
			EMIT('%');
			if (*fmt)
				EMIT(*fmt);
			else
				--fmt;
			continue;
		}
		if (qualifier == 'L')
			num = va_arg(args, unsigned long long);
		else if (qualifier == 'l')
			num = va_arg(args, unsigned long);
		else if (qualifier == 'h') {
			num = (unsigned short)va_arg(args, int);
			if (flags & SIGN)
				num = (short)num;
		} else if (flags & SIGN)
			num = va_arg(args, int);
		else
			num = va_arg(args, unsigned int);
		str = number(str, end, num, base, field_width, precision, flags);
	}
	// NUL-terminate within the buffer (unless size==0, where nothing is writable).
	if (size != 0)
		*((str < end) ? str : end) = '\0';
	return str - buf;
}

// vsprintf keeps its unbounded contract (callers guarantee the buffer is big
// enough) by driving the bounded core with SIZE_MAX, so its output is
// byte-identical to before this change.
int vsprintf(char *buf, const char *fmt, va_list args)
{
	return format_core(buf, SIZE_MAX, fmt, args);
}

// Bounded formatting straight into the caller's buffer — no unbounded temp to
// overflow. Writes at most size-1 chars plus a NUL, and returns the would-be
// length (C99 semantics). Fixes the stack overflow where a long %s (e.g. a
// malformed ELF DT_NEEDED name) blew a fixed 512-byte temp before truncation.
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args) {
    return format_core(buf, size, fmt, args);
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list args;
    int i;

    va_start(args, fmt);
    i = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return i;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list args;
    int i;

    va_start(args, fmt);
    i = vsprintf(buf, fmt, args);
    va_end(args);
    return i;
}
#undef EMIT
