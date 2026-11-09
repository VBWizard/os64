/* runtime.c — the C runtime libfreetype.so carries instead of importing one.
 *
 * See runtime.h for why a leaf library needs this at all. Plain, obvious
 * implementations: none of this is hot (FreeType's own arithmetic is), and
 * an obvious byte loop is worth more here than a clever one.
 *
 * shared.mk disables loop-to-libcall transformations as a build policy for
 * these private memory aliases. fonttest and the freestanding target runner
 * exercise the same runtime objects, without substituting a host libc.
 */

#include "runtime.h"

#define HIDDEN  __attribute__(( visibility( "hidden" ) ))


/* ── memory ────────────────────────────────────────────────────────────── */

HIDDEN void *ftport_memchr( const void *block, int c, size_t len )
{
    const unsigned char *p    = block;
    unsigned char        want = (unsigned char)c;

    for ( size_t i = 0; i < len; i++ )
    {
        if ( p[i] == want )
            return (void *)( p + i );
    }
    return NULL;
}


HIDDEN int ftport_memcmp( const void *a, const void *b, size_t len )
{
    const unsigned char *pa = a, *pb = b;

    for ( size_t i = 0; i < len; i++ )
    {
        if ( pa[i] != pb[i] )
            return pa[i] < pb[i] ? -1 : 1;
    }
    return 0;
}


HIDDEN void *ftport_memcpy( void *dst, const void *src, size_t len )
{
    unsigned char       *d = dst;
    const unsigned char *s = src;

    for ( size_t i = 0; i < len; i++ )
        d[i] = s[i];
    return dst;
}


HIDDEN void *ftport_memmove( void *dst, const void *src, size_t len )
{
    unsigned char       *d = dst;
    const unsigned char *s = src;

    /* Overlap decides the direction: copy forward when the destination is
     * below the source, backward when it is above, so a byte is never
     * overwritten before it has been read. */
    if ( d < s )
    {
        for ( size_t i = 0; i < len; i++ )
            d[i] = s[i];
    }
    else if ( d > s )
    {
        for ( size_t i = len; i > 0; i-- )
            d[i - 1] = s[i - 1];
    }
    return dst;
}


HIDDEN void *ftport_memset( void *dst, int c, size_t len )
{
    unsigned char *d = dst;

    for ( size_t i = 0; i < len; i++ )
        d[i] = (unsigned char)c;
    return dst;
}


/* ── strings ───────────────────────────────────────────────────────────── */

HIDDEN size_t ftport_strlen( const char *s )
{
    size_t n = 0;

    while ( s[n] != '\0' )
        n++;
    return n;
}


HIDDEN char *ftport_strcat( char *dst, const char *src )
{
    char *end = dst + ftport_strlen( dst );

    while ( ( *end++ = *src++ ) != '\0' )
        ;
    return dst;
}


HIDDEN int ftport_strcmp( const char *a, const char *b )
{
    /* C's three-way answer, compared as UNSIGNED characters. os64's own
     * `os64_streq` is a boolean and would have been the wrong thing to
     * borrow — FreeType sorts with this result, not just tests it. */
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;

    while ( *pa != '\0' && *pa == *pb )
    {
        pa++;
        pb++;
    }
    if ( *pa == *pb )
        return 0;
    return *pa < *pb ? -1 : 1;
}


HIDDEN int ftport_strncmp( const char *a, const char *b, size_t len )
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;

    for ( size_t i = 0; i < len; i++ )
    {
        if ( pa[i] != pb[i] )
            return pa[i] < pb[i] ? -1 : 1;
        if ( pa[i] == '\0' )
            break;
    }
    return 0;
}


HIDDEN char *ftport_strcpy( char *dst, const char *src )
{
    char *out = dst;

    while ( ( *out++ = *src++ ) != '\0' )
        ;
    return dst;
}


HIDDEN char *ftport_strncpy( char *dst, const char *src, size_t len )
{
    size_t i = 0;

    while ( i < len && src[i] != '\0' )
    {
        dst[i] = src[i];
        i++;
    }
    /* C's `strncpy`, warts and all: pad the remainder with NULs, and write no
     * terminator at all when the source filled the buffer. FreeType is the
     * caller and expects exactly this. */
    while ( i < len )
        dst[i++] = '\0';
    return dst;
}


HIDDEN char *ftport_strrchr( const char *s, int c )
{
    char        want  = (char)c;
    const char *found = NULL;

    for ( ;; )
    {
        if ( *s == want )
            found = s;
        if ( *s == '\0' )                /* the terminator is searchable too */
            break;
        s++;
    }
    return (char *)found;
}


HIDDEN char *ftport_strstr( const char *haystack, const char *needle )
{
    size_t want = ftport_strlen( needle );

    if ( want == 0 )
        return (char *)haystack;

    for ( ; *haystack != '\0'; haystack++ )
    {
        if ( ftport_strncmp( haystack, needle, want ) == 0 )
            return (char *)haystack;
    }
    return NULL;
}


/* ── sorting ───────────────────────────────────────────────────────────── */

static void swap_bytes( unsigned char *a, unsigned char *b, size_t size )
{
    for ( size_t i = 0; i < size; i++ )
    {
        unsigned char t = a[i];


        a[i] = b[i];
        b[i] = t;
    }
}


/* Sift `root` down through a heap of `count` elements. Iterative on purpose:
 * this runs on an os64 thread stack, and a sort should not be able to decide
 * how deep it goes. */
static void sift_down( unsigned char *base, size_t size, size_t count,
                       size_t root,
                       int ( *compare )( const void *, const void * ) )
{
    for ( ;; )
    {
        size_t child = 2 * root + 1;


        if ( child >= count )
            break;

        if ( child + 1 < count &&
             compare( base + child * size, base + ( child + 1 ) * size ) < 0 )
            child++;

        if ( compare( base + root * size, base + child * size ) >= 0 )
            break;

        swap_bytes( base + root * size, base + child * size, size );
        root = child;
    }
}


HIDDEN void ftport_qsort( void *base, size_t count, size_t size,
                          int ( *compare )( const void *, const void * ) )
{
    unsigned char *array = base;

    if ( count < 2 || size == 0 )
        return;

    for ( size_t i = count / 2; i > 0; i-- )
        sift_down( array, size, count, i - 1, compare );

    for ( size_t end = count - 1; end > 0; end-- )
    {
        swap_bytes( array, array + end * size, size );
        sift_down( array, size, end, 0, compare );
    }
}


/* ── the nonlocal return's sanitizer-aware half ────────────────────────── */

void ftport_longjmp_raw( ftport_jmp_buf buf, int value )
    __attribute__(( noreturn ));

/* Written as two nested tests rather than one `&&`: a preprocessor that does
 * not know `__has_feature` evaluates both halves of a single condition and
 * chokes on the call syntax. GCC answers to the first spelling, clang to the
 * second. */
#if defined( __SANITIZE_ADDRESS__ )
#  define FTPORT_UNDER_ASAN  1
#elif defined( __has_feature )
#  if __has_feature( address_sanitizer )
#    define FTPORT_UNDER_ASAN  1
#  endif
#endif

#ifdef OS64_FREETYPE_HOSTED
/* THE HOST HARNESS'S ONE WINDOW INTO THIS FILE. "A malformed character map
 * must enter the jump path" is an acceptance criterion, and a rejected font
 * does not prove it — a dozen ordinary bounds checks reject fonts too. This
 * counter is how the harness tells a jump from a refusal. It exists only in
 * the host build; the library os64 loads has no such symbol. */
unsigned long  ftport_longjmp_count;
#endif

#ifdef FTPORT_UNDER_ASAN
/* AddressSanitizer poisons the red zones around every stack local and
 * expects a function's own epilogue to unpoison them. A jump abandons whole
 * frames without running any epilogue, so the poison outlives the frames and
 * the NEXT call to reuse that stack reports a phantom overflow. `longjmp` in
 * a real libc is intercepted for exactly this reason; this one is not, so it
 * says so itself. */
void __asan_handle_no_return( void );
#endif

HIDDEN void ftport_longjmp( ftport_jmp_buf buf, int value )
{
#ifdef OS64_FREETYPE_HOSTED
    ftport_longjmp_count++;
#endif
#ifdef FTPORT_UNDER_ASAN
    __asan_handle_no_return();
#endif
    ftport_longjmp_raw( buf, value );
}


/* ── the four the compiler emits ───────────────────────────────────────── */
/* Even under `-ffreestanding` GCC may turn a struct assignment or a large
 * initializer into a call to one of these. They are NOT API — the port says
 * `ftport_memcpy` — and they are hidden, so they satisfy libfreetype's own
 * code generation without ever reaching another object's resolution.
 *
 * The host harness compiles this same file against a real C library, which
 * already has all four — and whose sanitizers intercept them to find the
 * overruns the harness exists to look for. There they would be a duplicate
 * definition at best and a blind spot at worst, so `OS64_FREETYPE_HOSTED`
 * leaves them out. Everything above this line is identical in both builds,
 * which is what lets a host result say anything about the guest. */

#ifndef OS64_FREETYPE_HOSTED

HIDDEN void *memcpy( void *dst, const void *src, size_t len )
{
    return ftport_memcpy( dst, src, len );
}


HIDDEN void *memmove( void *dst, const void *src, size_t len )
{
    return ftport_memmove( dst, src, len );
}


HIDDEN void *memset( void *dst, int c, size_t len )
{
    return ftport_memset( dst, c, len );
}


HIDDEN int memcmp( const void *a, const void *b, size_t len )
{
    return ftport_memcmp( a, b, len );
}

#endif /* !OS64_FREETYPE_HOSTED */
