/* os64_ftstdlib.h — what FreeType is allowed to call.
 *
 * Upstream's `include/freetype/config/ftstdlib.h` is one `#include` of the
 * hosted C library after another: <string.h>, <stdio.h>, <stdlib.h>,
 * <setjmp.h>. os64's userland is freestanding and libfreetype.so is a LEAF —
 * no DT_NEEDED at all, not even libos64 — so every one of those names has to
 * come from inside this port. This file is the complete list of what the
 * engine may reach for, which makes it the place to look when the question
 * is "what does the font engine depend on?".
 *
 * The three headers below are the freestanding ones: a conforming compiler
 * supplies <stddef.h>, <limits.h> and <stdarg.h> with no library behind
 * them. They are the only headers this file includes, and that is the
 * property worth protecting — the moment a fourth appears, libfreetype has
 * a dependency it did not have before.
 *
 * NAMES THAT ARE NOT PROVIDED ARE POISONED RATHER THAN OMITTED. A macro
 * pointing at a function that is declared and never defined turns an
 * accidental use into a LINK error that names the operation
 * (`ftport_poison_fopen`), instead of an undefined symbol that reads as a
 * missing libc. `--no-undefined` on the library link is what makes the trap
 * spring at build time.
 */

#ifndef FTSTDLIB_H_          /* upstream's guard: defining it is how a */
#define FTSTDLIB_H_          /* replacement announces itself           */

#include <stddef.h>
#include <limits.h>
#include <stdarg.h>

#include "runtime.h"

#define ft_ptrdiff_t  ptrdiff_t


/* ── integer limits ────────────────────────────────────────────────────── */
/* FreeType computes the width of `int` and `long` from these at compile
 * time; `ftconfig.h` refuses to build if they disagree with the target. */

#define FT_CHAR_BIT    CHAR_BIT
#define FT_USHORT_MAX  USHRT_MAX
#define FT_INT_MAX     INT_MAX
#define FT_INT_MIN     INT_MIN
#define FT_UINT_MAX    UINT_MAX
#define FT_LONG_MIN    LONG_MIN
#define FT_LONG_MAX    LONG_MAX
#define FT_ULONG_MAX   ULONG_MAX
#define FT_LLONG_MAX   LLONG_MAX
#define FT_LLONG_MIN   LLONG_MIN
#define FT_ULLONG_MAX  ULLONG_MAX


/* ── character and string processing ───────────────────────────────────── */
/* Ordinary C semantics, implemented in runtime.c. They are the port's own
 * because the library must not import them: os64's spellings live in
 * libos64 and several of them differ in contract (`os64_streq` is a
 * boolean, not `strcmp`'s three-way answer), so borrowing by name would
 * have been a silent behaviour change. */

#define ft_memchr   ftport_memchr
#define ft_memcmp   ftport_memcmp
#define ft_memcpy   ftport_memcpy
#define ft_memmove  ftport_memmove
#define ft_memset   ftport_memset
#define ft_strcat   ftport_strcat
#define ft_strcmp   ftport_strcmp
#define ft_strcpy   ftport_strcpy
#define ft_strlen   ftport_strlen
#define ft_strncmp  ftport_strncmp
#define ft_strncpy  ftport_strncpy
#define ft_strrchr  ftport_strrchr
#define ft_strstr   ftport_strstr


/* ── file handling: there is none ──────────────────────────────────────── */
/* `FT_CONFIG_OPTION_DISABLE_STREAM_SUPPORT` compiles the file paths out, so
 * these names survive only in declarations nothing calls. They are poisoned
 * so that turning stream support back on fails loudly here rather than
 * quietly acquiring a libc. */

#define FT_FILE      void
#define ft_fclose    ftport_poison_fclose
#define ft_fopen     ftport_poison_fopen
#define ft_fread     ftport_poison_fread
#define ft_fseek     ftport_poison_fseek
#define ft_ftell     ftport_poison_ftell
#define ft_snprintf  ftport_poison_snprintf


/* ── sorting ───────────────────────────────────────────────────────────── */

#define ft_qsort  ftport_qsort


/* ── memory allocation ─────────────────────────────────────────────────── */
/* THE ENGINE HAS NO GLOBAL ALLOCATOR. Every allocation goes through the
 * `FT_Memory` callbacks the caller installs, so these four exist only for
 * upstream's `src/base/ftsystem.c` — which this port does not compile (see
 * port/ftsystem.c for what replaces it). Poisoned, so a future source list
 * that pulls ftsystem.c back in says so at link time instead of silently
 * introducing a process-wide heap the accounting cannot see. */

#define ft_scalloc   ftport_poison_calloc
#define ft_sfree     ftport_poison_free
#define ft_smalloc   ftport_poison_malloc
#define ft_srealloc  ftport_poison_realloc


/* ── miscellaneous ─────────────────────────────────────────────────────── */
/* Both are reachable only from `FT_Set_Default_Properties`, which
 * `FT_CONFIG_OPTION_ENVIRONMENT_PROPERTIES` compiles away. */

#define ft_strtol  ftport_poison_strtol
#define ft_getenv  ftport_poison_getenv


/* ── execution control ─────────────────────────────────────────────────── */
/* The engine's one nonlocal return: the character-map validator aborts a
 * malformed subtable by jumping out of the parse. runtime.h documents what
 * the implementation saves and what the caller must do to stay correct. */

#define ft_jmp_buf     ftport_jmp_buf
/* The cast drops `volatile`: upstream declares the validator holding this
 * buffer as volatile (the discipline a nonlocal return demands of its
 * caller), and a qualified pointer will not pass as an unqualified one. The
 * qualifier is the CALLER's business, not the jump's. */
#define ft_setjmp( b ) ftport_setjmp( ( ftport_jmp_slot * )( b ) )
#define ft_longjmp     ftport_longjmp

#endif /* FTSTDLIB_H_ */
