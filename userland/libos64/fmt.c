// fmt.c — libos64 formatted output. See os64/fmt.h for the vocabulary.
//
// One engine (os64_vsnprintf), everything else is a veneer over it. The
// engine is pure computation — no syscalls, no allocation, no shared state —
// which makes it reentrant by construction AND testable on the build host
// with a plain gcc (the build's unit test compiles this file against the
// host's snprintf and diffs the two — see tools/test_fmt_host.c).

#include "os64/fmt.h"
#include "os64/io.h"

// Emit one char into the bounded buffer; always count the full length.
// (count tracks what the output WOULD be — the snprintf contract.)
typedef struct {
	char *buf;
	size_t size;    // capacity including the NUL slot
	size_t count;   // chars the full result wants (not counting NUL)
} fmt_out_t;

static void out_char(fmt_out_t *o, char c)
{
	if (o->count + 1 < o->size)
		o->buf[o->count] = c;
	o->count++;
}

// Emit a string with width/left-align/zero-pad applied. prefix carries a
// sign or "0x" that must land BEFORE zero padding ("-007", not "00-7").
static void out_padded(fmt_out_t *o, const char *prefix, const char *body,
                       size_t body_len, int width, int zero_pad, int left_align)
{
	size_t prefix_len = 0;
	while (prefix[prefix_len])
		prefix_len++;

	size_t total = prefix_len + body_len;
	size_t pad = ((size_t)width > total) ? (size_t)width - total : 0;

	if (!left_align && !zero_pad)
		while (pad--) out_char(o, ' ');
	for (size_t i = 0; i < prefix_len; i++)
		out_char(o, prefix[i]);
	if (!left_align && zero_pad)
		while (pad--) out_char(o, '0');
	for (size_t i = 0; i < body_len; i++)
		out_char(o, body[i]);
	if (left_align)
		while (pad--) out_char(o, ' ');
}

// Unsigned to text in `base`, into the END of a scratch buffer (digits are
// generated low-to-high). Returns a pointer to the first digit.
static char *utoa_rev(uint64_t v, unsigned base, int upper, char *end)
{
	static const char lower_digits[] = "0123456789abcdef";
	static const char upper_digits[] = "0123456789ABCDEF";
	const char *digits = upper ? upper_digits : lower_digits;

	char *p = end;
	*--p = '\0';
	if (v == 0)
		*--p = '0';
	while (v)
	{
		*--p = digits[v % base];
		v /= base;
	}
	return p;
}

int32_t os64_vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
	fmt_out_t o = { buf, size, 0 };

	for (const char *p = fmt; *p; p++)
	{
		if (*p != '%')
		{
			out_char(&o, *p);
			continue;
		}
		p++;

		// %% early, so the flag machinery below never sees it.
		if (*p == '%')
		{
			out_char(&o, '%');
			continue;
		}

		// Flags, width, precision, length — the subset that earns its keep.
		int left_align = 0, zero_pad = 0, width = 0, longs = 0;
		int prec = -1;   // -1 = "no precision given" (0 is a real precision!)
		for (;; p++)
		{
			if (*p == '-')      left_align = 1;
			else if (*p == '0') zero_pad = 1;
			else break;
		}
		if (*p == '*')
		{
			// Width from the argument list — how ls does measured columns:
			// os64_printf("%-*s %10lu\n", maxlen, name, size). A negative
			// width means left-align with the absolute value (the standard
			// rule, and a handy one).
			width = va_arg(args, int);
			if (width < 0)
			{
				left_align = 1;
				width = -width;
			}
			p++;
		}
		else
			while (*p >= '0' && *p <= '9')
				width = width * 10 + (*p++ - '0');
		if (*p == '.')
		{
			p++;
			if (*p == '*')
			{
				prec = va_arg(args, int);
				if (prec < 0)
					prec = -1;   // negative %.* means "as if omitted" (std)
				p++;
			}
			else
			{
				prec = 0;
				while (*p >= '0' && *p <= '9')
					prec = prec * 10 + (*p++ - '0');
			}
		}
		while (*p == 'l')
		{
			longs++;
			p++;
		}

		char scratch[24];   // 64-bit decimal is 20 digits; 24 covers all bases
		char *body;
		const char *prefix = "";
		int64_t sv;
		uint64_t uv;

		switch (*p)
		{
			case 'c':
			{
				char c = (char)va_arg(args, int);
				out_padded(&o, "", &c, 1, width, 0, left_align);
				break;
			}
			case 's':
			{
				const char *s = va_arg(args, const char *);
				if (s == NULL)
					s = "(null)";
				// ── The poisoned-pointer tripwire (2026-08-11) ──────────
				//
				// Born from a soak mystery: twice, hours apart, a %s here
				// arrived holding a SMALL INTEGER (3, then 4) instead of a
				// string pointer — at a call site whose disassembly provably
				// loads valid pointers three instructions before the call.
				// Something outside this program is corrupting either an
				// argument register or the vararg spill slot, and a plain
				// segfault report cannot say WHICH. This can: it prints the
				// va machinery's actual state — how far the register walk
				// got and what all six spill slots hold — then substitutes a
				// visible marker and KEEPS GOING, so every occurrence yields
				// an autopsy instead of a corpse and the program survives to
				// be struck again.
				//
				// 0x10000 is a safe poison floor: no legitimate string lives
				// below 64KB in any os64 process (images load at 0x04400000,
				// stacks higher still). Remove the whole block when the
				// corruptor is caught and shot.
				if ((uintptr_t)s < 0x10000)
				{
					// x86-64 SysV va_list innards: one struct, gp_offset is
					// how many bytes of the 48-byte GPR save area (rdi, rsi,
					// rdx, rcx, r8, r9 — in that order) have been consumed.
					// Our va_arg already ran, so the poisoned slot is at
					// gp_offset - 8.
					typedef struct {
						unsigned int gp_offset, fp_offset;
						void *overflow_arg_area, *reg_save_area;
					} va_tag_t;
					const va_tag_t *tag = (const va_tag_t *)args;
					const uint64_t *save = (const uint64_t *)tag->reg_save_area;
					os64_printf("\n[fmt tripwire] %%s pointer POISONED: 0x%lx (fmt \"%s\")\n",
					            (uint64_t)(uintptr_t)s, fmt);
					os64_printf("[fmt tripwire] gp_offset=%u (poisoned slot at %u)\n",
					            tag->gp_offset, tag->gp_offset - 8);
					os64_printf("[fmt tripwire] save area: rdi=%016lx rsi=%016lx rdx=%016lx\n",
					            save[0], save[1], save[2]);
					os64_printf("[fmt tripwire]            rcx=%016lx r8 =%016lx r9 =%016lx\n",
					            save[3], save[4], save[5]);
					s = "(POISONED-PTR)";
				}
				size_t len = 0;
				while (s[len])
					len++;
				// Precision on %s = MAXIMUM chars (width is the minimum) —
				// "%-40.39s" is the bulletproof column: clip at 39, pad to
				// 40, alignment survives any filename.
				if (prec >= 0 && (size_t)prec < len)
					len = (size_t)prec;
				out_padded(&o, "", s, len, width, 0, left_align);
				break;
			}
			case 'd':
			case 'i':
				sv = longs ? va_arg(args, int64_t) : (int64_t)va_arg(args, int);
				uv = (sv < 0) ? (uint64_t)-sv : (uint64_t)sv;
				if (sv < 0)
					prefix = "-";
				body = utoa_rev(uv, 10, 0, scratch + sizeof(scratch));
				// Precision on integers = MINIMUM digits (zero-filled), and
				// the standard says it silences the '0' flag.
				if (prec >= 0)
				{
					zero_pad = 0;
					while (scratch + sizeof(scratch) - 1 - body < (long)prec && body > scratch)
						*--body = '0';
				}
				out_padded(&o, prefix, body, (size_t)(scratch + sizeof(scratch) - 1 - body),
				           width, zero_pad, left_align);
				break;
			case 'u':
			case 'x':
			case 'X':
				uv = longs ? va_arg(args, uint64_t) : (uint64_t)va_arg(args, unsigned int);
				body = utoa_rev(uv, (*p == 'u') ? 10 : 16, (*p == 'X'), scratch + sizeof(scratch));
				if (prec >= 0)
				{
					zero_pad = 0;
					while (scratch + sizeof(scratch) - 1 - body < (long)prec && body > scratch)
						*--body = '0';
				}
				out_padded(&o, "", body, (size_t)(scratch + sizeof(scratch) - 1 - body),
				           width, zero_pad, left_align);
				break;
			case 'p':
				uv = (uint64_t)(uintptr_t)va_arg(args, void *);
				body = utoa_rev(uv, 16, 0, scratch + sizeof(scratch));
				out_padded(&o, "0x", body, (size_t)(scratch + sizeof(scratch) - 1 - body),
				           width, zero_pad, left_align);
				break;
			case '\0':
				// A trailing lone '%' — emit it honestly and stop.
				out_char(&o, '%');
				goto done;
			default:
				// Unknown conversion: emit it literally rather than eat the
				// argument list from the wrong position onward.
				out_char(&o, '%');
				out_char(&o, *p);
				break;
		}
	}
done:
	if (o.size > 0)
		o.buf[(o.count < o.size - 1) ? o.count : o.size - 1] = '\0';
	return (int)o.count;
}

int32_t os64_snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int n = os64_vsnprintf(buf, size, fmt, args);
	va_end(args);
	return n;
}

int32_t os64_format_binary_size(uint64_t bytes, char *buf, size_t size)
{
	if (bytes < 1024)
		return os64_snprintf(buf, size, "%luB", bytes);

	static const char *units[] = {"K", "M", "G", "T", "P", "E"};
	uint64_t divisor = 1024;
	int32_t unit = 0;
	while (unit < 5 && bytes >= divisor * 1024)
	{
		divisor *= 1024;
		unit++;
	}

	uint64_t whole = bytes / divisor;
	uint64_t remainder = bytes % divisor;
	uint64_t tenth = 0;
	while (tenth < 9)
	{
		uint64_t candidate = tenth + 1;
		uint64_t threshold = (divisor / 10) * candidate +
			((divisor % 10) * candidate + 9) / 10;
		if (remainder < threshold)
			break;
		tenth = candidate;
	}
	return os64_snprintf(buf, size, "%lu.%lu%s", whole, tenth, units[unit]);
}

// The printf-to-a-handle worker. 1024 bytes of stack scratch per call: big
// enough for any sane line, small enough for the 16KB user stacks, and on
// the STACK so concurrent tasks (or threads, someday) never share a buffer —
// the exact mistake the kernel's printd once made and unlearned.
#define OS64_PRINTF_MAX 1024

int32_t os64_hprintf(int32_t handle, const char *fmt, ...)
{
	char buf[OS64_PRINTF_MAX];
	va_list args;

	va_start(args, fmt);
	int len = os64_vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	if (len < 0)
		return -1;
	if ((size_t)len >= sizeof(buf))
		len = sizeof(buf) - 1;   // truncated: write what was formatted
	return (int)os64_write(handle, buf, (size_t)len);
}

int32_t os64_printf(const char *fmt, ...)
{
	char buf[OS64_PRINTF_MAX];
	va_list args;

	va_start(args, fmt);
	int len = os64_vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	if (len < 0)
		return -1;
	if ((size_t)len >= sizeof(buf))
		len = sizeof(buf) - 1;
	return (int)os64_write(1, buf, (size_t)len);
}
