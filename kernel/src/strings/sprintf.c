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
#include "printd.h"

extern uint32_t* kTicksSinceStart;
volatile uint32_t lock_mp_printd;

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

static char *number(char *str, long num, int base, int size, int precision,
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
			*str++ = ' ';
	if (sign)
		*str++ = sign;
	if (type & SPECIAL) {
		if (base == 8)
			*str++ = '0';
		else if (base == 16) {
			*str++ = '0';
			*str++ = ('X' | locase);
		}
	}
	if (!(type & LEFT))
		while (size-- > 0)
			*str++ = c;
	while (i < precision--)
		*str++ = '0';
	while (i-- > 0)
		*str++ = tmp[i];
	while (size-- > 0)
		*str++ = ' ';
	return str;
}

int vsprintf(char *buf, const char *fmt, va_list args)
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

	for (str = buf; *fmt; ++fmt) {
		if (*fmt != '%') {
			*str++ = *fmt;
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
					*str++ = ' ';
			*str++ = (unsigned char)va_arg(args, int);
			while (--field_width > 0)
				*str++ = ' ';
			continue;

		case 's':	//string
			s = va_arg(args, char *);
			if (s) //CLR 01/09/2019: Handling case where pointer is NULL
                            len = strlen(s);
                        else
                            len = 0;

			if (!(flags & LEFT))
				while (len < field_width--)
					*str++ = ' ';
			for (i = 0; i < len; ++i)
				*str++ = *s++;
			while (len < field_width--)
				*str++ = ' ';
			continue;

		case 'p':	//pointer
			if (field_width == -1) {
				field_width = 2 * sizeof(void *);
				flags |= ZEROPAD;
			}
			str = number(str,
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
			*str++ = '%';
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
			*str++ = '%';
			if (*fmt)
				*str++ = *fmt;
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
		str = number(str, num, base, field_width, precision, flags);
	}
	*str = '\0';
	return str - buf;
}

int sprintf(char *buf, const char *fmt, ...)
{
	va_list args;
	int i;

	va_start(args, fmt);
	i = vsprintf(buf, fmt, args);
	va_end(args);
	return i;
}


