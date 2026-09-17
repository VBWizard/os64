/* runtime.h — the private freestanding runtime libfreetype.so carries.
 *
 * libfreetype.so is a LEAF: it records no DT_NEEDED, not even libos64, so
 * that libos64 can consume it later without the two importing each other.
 * A leaf has to bring its own C runtime, and this is it — the handful of
 * operations `os64_ftstdlib.h` maps FreeType's `ft_*` names onto, plus the
 * four the COMPILER emits on its own behalf (`memcpy`, `memmove`, `memset`,
 * `memcmp`: a struct assignment or a large initializer becomes a call to one
 * of them under any optimization level, whatever `-ffreestanding` suggests).
 *
 * Everything here is hidden: the version script exports `os64_freetype_backend_v1`
 * and nothing else, so these names never reach another object's symbol
 * resolution and cannot interpose on, or be interposed by, libos64's own
 * copies of the same operations.
 */

#ifndef OS64_FREETYPE_RUNTIME_H_
#define OS64_FREETYPE_RUNTIME_H_

#include <stddef.h>

void   *ftport_memchr( const void *block, int c, size_t len );
int     ftport_memcmp( const void *a, const void *b, size_t len );
void   *ftport_memcpy( void *dst, const void *src, size_t len );
void   *ftport_memmove( void *dst, const void *src, size_t len );
void   *ftport_memset( void *dst, int c, size_t len );

char   *ftport_strcat( char *dst, const char *src );
int     ftport_strcmp( const char *a, const char *b );
char   *ftport_strcpy( char *dst, const char *src );
size_t  ftport_strlen( const char *s );
int     ftport_strncmp( const char *a, const char *b, size_t len );
char   *ftport_strncpy( char *dst, const char *src, size_t len );
char   *ftport_strrchr( const char *s, int c );
char   *ftport_strstr( const char *haystack, const char *needle );

/* Heapsort, not quicksort: no recursion, no stack growth proportional to the
 * input, and an O(n log n) worst case rather than an O(n^2) one an adversarial
 * ordering could provoke. FreeType sorts small internal tables with it, so the
 * constant factor is not what matters; the bounds are. Unstable, like the
 * `qsort` it stands in for. */
void    ftport_qsort( void *base, size_t count, size_t size,
                      int ( *compare )( const void *, const void * ) );


/* ── the nonlocal return ───────────────────────────────────────────────── */
/*
 * THE ONE PLACE FREETYPE JUMPS: the SFNT character-map validator. Upstream
 * arms a jump in `tt_face_build_cmaps` (`src/sfnt/ttcmap.c`) and fires it
 * from `ft_validator_error` (`src/base/ftobjs.c`) the moment a subtable
 * fails a bounds check, so a malformed cmap abandons its parse instead of
 * walking off the end of the table. Nothing else in the compiled
 * configuration uses one — the other caller, `pngshim.c`, belongs to
 * embedded PNG bitmaps, which this port does not build.
 *
 * It is DELIBERATELY NOT a public `setjmp`. The names are private, hidden,
 * and this header is not installed: os64 does not own a conforming C
 * `setjmp` ABI and this must not be mistaken for one. GCC's
 * `__builtin_setjmp`/`__builtin_longjmp` were the alternative and were not
 * taken: their contract is "for the compiler's own exception handling, with
 * restrictions", and a restriction that goes wrong here is silent stack
 * corruption in a parser reading somebody else's font file.
 *
 * THE CONTRACT, in full, because a short assembly body does not make any of
 * it go away:
 *
 *   SAVED AND RESTORED: the System V x86-64 callee-saved integer registers —
 *   `rbx`, `rbp`, `r12`, `r13`, `r14`, `r15` — plus the stack pointer as it
 *   will be after `ftport_setjmp` returns, and the return address. Eight
 *   words, which is what `ftport_jmp_buf` is.
 *
 *   NOT SAVED: `MXCSR` and the x87 control word. The ABI calls them
 *   callee-saved, and `glibc`'s `longjmp` does not restore them either;
 *   nothing between the two points here changes them, and FreeType's
 *   rasterizer is integer arithmetic throughout.
 *
 *   RETURN VALUE: `ftport_setjmp` answers 0 when it is called and the value
 *   passed to `ftport_longjmp` when it is jumped to. A value of 0 is
 *   promoted to 1, as C requires, so "did I just arrive here" is always
 *   answerable.
 *
 *   STACK LIFETIME: the frame that called `ftport_setjmp` MUST STILL BE
 *   LIVE. Jumping into a function that has already returned restores a stack
 *   pointer into dead memory. Upstream satisfies this by construction — the
 *   jump is armed and consumed inside one function, with the validator
 *   called in between.
 *
 *   WHAT THE CALLER OWES: any local written between the `ftport_setjmp` and
 *   the jump, and read afterwards, must be `volatile`, or the compiler may
 *   have it in a register that the jump restores to a stale value. Upstream
 *   already declares both of them that way (`volatile TT_ValidatorRec valid`,
 *   `volatile FT_Error error`), which is the discipline `setjmp` has always
 *   demanded. `ftport_setjmp` carries `returns_twice` so the optimizer knows
 *   what it is looking at; without that attribute GCC is entitled to assume
 *   control passes through it once.
 *
 *   SAME THREAD, SAME ADDRESS SPACE. A buffer is a stack pointer and a code
 *   address; neither means anything on another thread. There is no signal
 *   mask to save because os64's signals do not have one.
 *
 * `test_freetype_host` drives the real path with a deliberately corrupted
 * character map, at `-O2` and under the sanitizers, because a jump that is
 * only correct at `-O0` is a bug waiting for a release build.
 */

typedef unsigned long  ftport_jmp_slot;
typedef ftport_jmp_slot  ftport_jmp_buf[8];

int  ftport_setjmp( ftport_jmp_buf buf ) __attribute__(( returns_twice ));
void ftport_longjmp( ftport_jmp_buf buf, int value ) __attribute__(( noreturn ));

#endif /* OS64_FREETYPE_RUNTIME_H_ */
