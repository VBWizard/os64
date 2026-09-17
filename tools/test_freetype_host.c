/* test_freetype_host.c — the font backend, on the host, under the sanitizers.
 *
 * It drives the SHIPPED sources: the same pinned FreeType translation units,
 * the same port/backend.c, the same port/runtime.c and port/nonlocal.S that
 * libfreetype.so is built from — recompiled by the host cc with ASan and
 * UBSan instead of cross-compiled. That is what lets a green run here say
 * anything about the guest. `OS64_FREETYPE_HOSTED` changes two things and no
 * more: it leaves out the port's own `memcpy`/`memset`/`memmove`/`memcmp`, so
 * the sanitizers' interceptors can see those calls, and it exposes a counter
 * on the nonlocal return so this harness can prove the jump ran rather than
 * infer it from a rejected font.
 *
 * WHAT THIS HARNESS IS FOR, beyond "does it work": every path that a font
 * file can steer. A font is somebody else's bytes, so the interesting cases
 * are the broken ones — truncated, corrupted, oversized, and the character-map
 * validator's nonlocal return firing out of a parse at -O2. Allocation failure
 * is walked exhaustively rather than sampled: the harness fails the Nth
 * allocation for every reachable N and requires the engine to survive each.
 *
 * Run it through tools/test_freetype_host.py, which knows the flags.
 */

#define _POSIX_C_SOURCE 200809L

#include <os64/font_backend.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const os64_font_backend_t *os64_freetype_backend_v1( void );

static const os64_font_backend_t *ft;
static int reverse_bitmap_rows;
FT_Error __real_FT_Render_Glyph( FT_GlyphSlot slot, FT_Render_Mode mode );

/* Keep the rendered image identical while changing its storage flow. The
 * shipped smooth renderer uses positive pitch; this exercises the adapter's
 * negative-pitch branch without replacing the rasterizer or mask contents. */
FT_Error __wrap_FT_Render_Glyph( FT_GlyphSlot slot, FT_Render_Mode mode )
{
    FT_Error error = __real_FT_Render_Glyph( slot, mode );
    FT_Bitmap *bitmap = &slot->bitmap;
    if ( error == 0 && reverse_bitmap_rows && bitmap->pitch > 0 )
    {
        for ( unsigned row = 0; row < bitmap->rows / 2; row++ )
            for ( int col = 0; col < bitmap->pitch; col++ )
            {
                size_t a = (size_t)row * bitmap->pitch + col;
                size_t b = (size_t)(bitmap->rows - 1 - row) * bitmap->pitch + col;
                unsigned char temp = bitmap->buffer[a];
                bitmap->buffer[a] = bitmap->buffer[b]; bitmap->buffer[b] = temp;
            }
        bitmap->pitch = -bitmap->pitch;
    }
    return error;
}

static int  checks;
static int  failures;
static char current_case[160];


/* ── the harness's own scaffolding ─────────────────────────────────────── */

static void begin( const char *name )
{
    snprintf( current_case, sizeof( current_case ), "%s", name );
}


static void check( int condition, const char *fmt, ... )
{
    va_list ap;

    checks++;
    if ( condition )
        return;

    failures++;
    fprintf( stderr, "FAIL [%s] ", current_case );
    va_start( ap, fmt );
    vfprintf( stderr, fmt, ap );
    va_end( ap );
    fputc( '\n', stderr );
}


static const char *status_name( os64_font_status_t s )
{
    switch ( s )
    {
    case OS64_FONT_OK:            return "OK";
    case OS64_FONT_BAD_ARGUMENT:  return "BAD_ARGUMENT";
    case OS64_FONT_UNSUPPORTED:   return "UNSUPPORTED";
    case OS64_FONT_MALFORMED:     return "MALFORMED";
    case OS64_FONT_LIMIT:         return "LIMIT";
    case OS64_FONT_NO_MEMORY:     return "NO_MEMORY";
    case OS64_FONT_MISSING:       return "MISSING";
    case OS64_FONT_BUSY:          return "BUSY";
    case OS64_FONT_ENGINE_ERROR:  return "ENGINE_ERROR";
    }
    return "?";
}


/* ── the allocator under test ──────────────────────────────────────────── */
/*
 * Two jobs: obey the contract (max_align_t storage, free told its own size),
 * and lie on demand. `fail_at` counts allocations down and refuses the one
 * that reaches zero, which is how every reachable allocation site gets its
 * turn at failing. The size handed to `free` is CHECKED against the size that
 * was requested — the whole reason the port keeps a header — so a mismatch is
 * a test failure rather than a silent corruption.
 */

typedef struct
{
    long   fail_at;        /* <0 never fails */
    long   allocations;
    size_t live;
    size_t peak;
    int    size_mismatches;
} test_alloc;

/* A header of our own, so `free` can be told what it was given and check it.
 * Two size_t keeps the payload 16-byte aligned, which is what the contract
 * promises the engine. */
typedef struct
{
    size_t  size;
    size_t  pad;
} host_header;


static void *test_alloc_fn( void *context, size_t bytes )
{
    test_alloc  *a = context;
    host_header *h;

    if ( bytes == 0 )
        return NULL;

    a->allocations++;
    if ( a->fail_at >= 0 && a->allocations > a->fail_at )
        return NULL;

    h = malloc( sizeof( *h ) + bytes );
    if ( h == NULL )
        return NULL;

    h->size = bytes;
    a->live += bytes;
    if ( a->live > a->peak )
        a->peak = a->live;
    return (unsigned char *)h + sizeof( *h );
}


static void test_free_fn( void *context, void *allocation, size_t bytes )
{
    test_alloc  *a = context;
    host_header *h;

    if ( allocation == NULL )
        return;

    h = (host_header *)( (unsigned char *)allocation - sizeof( *h ) );
    if ( h->size != bytes )
        a->size_mismatches++;

    a->live -= h->size;
    free( h );
}


static void alloc_init( test_alloc *a )
{
    memset( a, 0, sizeof( *a ) );
    a->fail_at = -1;
}


static os64_font_engine_options_t options_for( test_alloc *a, size_t cap )
{
    os64_font_engine_options_t options;

    memset( &options, 0, sizeof( options ) );
    options.memory.context = a;
    options.memory.alloc   = test_alloc_fn;
    options.memory.free    = test_free_fn;
    options.memory_cap     = cap;
    return options;
}


/* ── the pinned fixtures ───────────────────────────────────────────────── */

typedef struct
{
    const char *path;
    const char *label;
    uint8_t    *bytes;
    size_t      length;
    int         fixed_width;
} fixture;

static fixture fixtures[4];
#define SANS      ( &fixtures[0] )   /* DejaVu Sans        — TTF, proportional */
#define SANS_MONO ( &fixtures[1] )   /* DejaVu Sans Mono   — TTF, monospace    */
#define CFF_PROP  ( &fixtures[2] )   /* Source Sans 3      — CFF, proportional */
#define CFF_MONO  ( &fixtures[3] )   /* Source Code Pro    — CFF, monospace    */


static int load_fixture( fixture *f, const char *dir, const char *name,
                         const char *label, int fixed_width )
{
    char   path[1024];
    FILE  *fp;
    long   size;

    snprintf( path, sizeof( path ), "%s/%s", dir, name );
    fp = fopen( path, "rb" );
    if ( fp == NULL )
    {
        fprintf( stderr, "cannot open fixture %s\n", path );
        return 0;
    }
    fseek( fp, 0, SEEK_END );
    size = ftell( fp );
    fseek( fp, 0, SEEK_SET );

    f->bytes = malloc( (size_t)size );
    if ( f->bytes == NULL || fread( f->bytes, 1, (size_t)size, fp ) != (size_t)size )
    {
        fprintf( stderr, "cannot read fixture %s\n", path );
        fclose( fp );
        return 0;
    }
    fclose( fp );

    f->path        = strdup( path );
    f->label       = label;
    f->length      = (size_t)size;
    f->fixed_width = fixed_width;
    return 1;
}


/* ── engine lifecycle and bounds ───────────────────────────────────────── */

static void test_engine_bounds( void )
{
    test_alloc                 a;
    os64_font_engine_options_t options;
    os64_font_engine_t        *engine = NULL;
    os64_font_engine_stats_t   stats;
    os64_font_status_t         status;

    begin( "engine/bounds" );

    alloc_init( &a );

    check( ft->revision == OS64_FONT_BACKEND_REVISION,
           "revision %u, expected %u", ft->revision, OS64_FONT_BACKEND_REVISION );
    check( ft->struct_size == sizeof( os64_font_backend_t ),
           "struct_size %zu, expected %zu", ft->struct_size,
           sizeof( os64_font_backend_t ) );

    /* NULL out is refused before anything else is looked at. */
    options = options_for( &a, 0 );
    check( ft->engine_create( &options, NULL ) == OS64_FONT_BAD_ARGUMENT,
           "NULL out accepted" );
    check( ft->engine_create( NULL, &engine ) == OS64_FONT_BAD_ARGUMENT,
           "NULL options accepted" );
    check( engine == NULL, "failed create left output set" );

    /* A descriptor missing a callback is not a usable allocator. */
    options = options_for( &a, 0 );
    options.memory.alloc = NULL;
    check( ft->engine_create( &options, &engine ) == OS64_FONT_BAD_ARGUMENT,
           "NULL alloc accepted" );
    options = options_for( &a, 0 );
    options.memory.free = NULL;
    check( ft->engine_create( &options, &engine ) == OS64_FONT_BAD_ARGUMENT,
           "NULL free accepted" );

    /* Over the ceiling is a bad argument; under the floor is a limit. The
     * difference matters: one is a caller mistake, the other is a machine
     * that cannot do what was asked. */
    options = options_for( &a, (size_t)OS64_FONT_MEMORY_MAX + 1 );
    check( ft->engine_create( &options, &engine ) == OS64_FONT_BAD_ARGUMENT,
           "cap above OS64_FONT_MEMORY_MAX accepted" );

    options = options_for( &a, 8 );
    status  = ft->engine_create( &options, &engine );
    check( status == OS64_FONT_LIMIT, "tiny cap gave %s, expected LIMIT",
           status_name( status ) );
    check( a.allocations == 0,
           "a cap too small to fit the engine still called the allocator" );

    /* Zero means the default, and the default is big enough to work in. */
    options = options_for( &a, 0 );
    status  = ft->engine_create( &options, &engine );
    check( status == OS64_FONT_OK, "default cap create gave %s",
           status_name( status ) );
    check( engine != NULL, "create succeeded with NULL handle" );

    check( ft->engine_stats( engine, NULL ) == OS64_FONT_BAD_ARGUMENT,
           "NULL stats accepted" );
    check( ft->engine_stats( NULL, &stats ) == OS64_FONT_BAD_ARGUMENT,
           "NULL engine accepted by stats" );
    check( stats.live_bytes == 0 && stats.live_faces == 0,
           "failed stats left output set" );

    check( ft->engine_stats( engine, &stats ) == OS64_FONT_OK, "stats failed" );
    check( stats.live_faces == 0 && stats.live_glyphs == 0,
           "fresh engine reports %u faces, %u glyphs",
           stats.live_faces, stats.live_glyphs );
    check( stats.live_bytes > 0, "fresh engine reports zero live bytes" );
    check( stats.peak_bytes >= stats.live_bytes, "peak below live" );

    check( ft->engine_destroy( NULL ) == OS64_FONT_OK, "destroy(NULL) refused" );
    check( ft->engine_destroy( engine ) == OS64_FONT_OK, "destroy failed" );

    check( a.live == 0, "engine leaked %zu bytes", a.live );
    check( a.size_mismatches == 0, "%d frees were given the wrong size",
           a.size_mismatches );
}


/* ── opening faces, and refusing the ones R1 does not serve ────────────── */

static void test_face_open_bounds( void )
{
    test_alloc                 a;
    os64_font_engine_options_t options;
    os64_font_engine_t        *engine = NULL;
    os64_font_face_t          *face   = NULL;
    os64_font_face_options_t   fo     = { 16, OS64_FONT_HINT_NORMAL };
    os64_font_status_t         status;
    uint8_t                    junk[64];

    begin( "face/bounds" );

    alloc_init( &a );
    options = options_for( &a, 0 );
    if ( ft->engine_create( &options, &engine ) != OS64_FONT_OK )
    {
        check( 0, "engine create failed" );
        return;
    }

    check( ft->face_open( engine, SANS->bytes, SANS->length, &fo, NULL )
               == OS64_FONT_BAD_ARGUMENT, "NULL out accepted" );
    check( ft->face_open( NULL, SANS->bytes, SANS->length, &fo, &face )
               == OS64_FONT_BAD_ARGUMENT, "NULL engine accepted" );
    check( ft->face_open( engine, NULL, SANS->length, &fo, &face )
               == OS64_FONT_BAD_ARGUMENT, "NULL bytes accepted" );
    check( ft->face_open( engine, SANS->bytes, SANS->length, NULL, &face )
               == OS64_FONT_BAD_ARGUMENT, "NULL options accepted" );
    check( ft->face_open( engine, SANS->bytes, 0, &fo, &face )
               == OS64_FONT_BAD_ARGUMENT, "zero length accepted" );
    check( face == NULL, "a refused open left the handle set" );

    fo.pixel_height = 0;
    check( ft->face_open( engine, SANS->bytes, SANS->length, &fo, &face )
               == OS64_FONT_BAD_ARGUMENT, "pixel_height 0 accepted" );
    fo.pixel_height = OS64_FONT_PIXEL_MAX + 1;
    check( ft->face_open( engine, SANS->bytes, SANS->length, &fo, &face )
               == OS64_FONT_BAD_ARGUMENT, "pixel_height above the cap accepted" );
    fo.pixel_height = 16;
    fo.hint         = (os64_font_hint_t)7;
    check( ft->face_open( engine, SANS->bytes, SANS->length, &fo, &face )
               == OS64_FONT_BAD_ARGUMENT, "unknown hint mode accepted" );
    fo.hint = OS64_FONT_HINT_NORMAL;

    /* A length past the file ceiling is refused on the ARGUMENT, before the
     * bytes are read — the pointer here is deliberately short. */
    check( ft->face_open( engine, junk, (size_t)OS64_FONT_FILE_MAX + 1, &fo, &face )
               == OS64_FONT_LIMIT, "oversized length accepted" );

    /* Not a font at all. */
    memset( junk, 0, sizeof( junk ) );
    status = ft->face_open( engine, junk, sizeof( junk ), &fo, &face );
    check( status == OS64_FONT_UNSUPPORTED || status == OS64_FONT_MALFORMED,
           "all-zero input gave %s", status_name( status ) );
    check( face == NULL, "a refused open left the handle set" );

    memcpy( junk, "not a font at all, just some prose", 33 );
    status = ft->face_open( engine, junk, sizeof( junk ), &fo, &face );
    check( status == OS64_FONT_UNSUPPORTED || status == OS64_FONT_MALFORMED,
           "prose gave %s", status_name( status ) );

    check( ft->engine_destroy( engine ) == OS64_FONT_OK, "destroy failed" );
    check( a.live == 0, "leaked %zu bytes", a.live );
}


/* A font truncated at every one of a spread of lengths. None may crash, none
 * may succeed by accident at a length that cannot hold the tables. */
static void test_truncation( void )
{
    test_alloc                 a;
    os64_font_engine_options_t options;
    os64_font_engine_t        *engine = NULL;
    os64_font_face_options_t   fo     = { 16, OS64_FONT_HINT_NORMAL };
    static const double        cuts[] = { 0.001, 0.01, 0.05, 0.1, 0.25, 0.5,
                                          0.75, 0.9, 0.99, 0.999 };

    begin( "face/truncation" );

    alloc_init( &a );
    options = options_for( &a, 0 );
    if ( ft->engine_create( &options, &engine ) != OS64_FONT_OK )
    {
        check( 0, "engine create failed" );
        return;
    }

    for ( size_t f = 0; f < sizeof( fixtures ) / sizeof( fixtures[0] ); f++ )
    {
        for ( size_t c = 0; c < sizeof( cuts ) / sizeof( cuts[0] ); c++ )
        {
            size_t              len  = (size_t)( (double)fixtures[f].length * cuts[c] );
            os64_font_face_t   *face = NULL;
            os64_font_status_t  status;


            if ( len == 0 )
                len = 1;

            status = ft->face_open( engine, fixtures[f].bytes, len, &fo, &face );

            /* A truncated font may still open — the tables it needs might all
             * live in the prefix — but it must never come back "OK" with a
             * NULL handle, and it must never leave a handle behind on
             * failure. What matters is that the parse stays inside the bytes
             * it was given, which is what the sanitizer is here to watch. */
            if ( status == OS64_FONT_OK )
            {
                check( face != NULL, "%s@%zu: OK with no handle",
                       fixtures[f].label, len );
                if ( face != NULL )
                {
                    /* Exercise it: a half-font that opened must still answer
                     * safely. */
                    uint32_t index = 0;


                    ft->lookup( face, 'A', &index );
                    if ( index != 0 )
                    {
                        os64_font_glyph_t *glyph = NULL;


                        if ( ft->render( face, index, &glyph ) == OS64_FONT_OK )
                            ft->glyph_release( glyph );
                    }
                    ft->face_close( face );
                }
            }
            else
                check( face == NULL, "%s@%zu: %s left a handle",
                       fixtures[f].label, len, status_name( status ) );
        }
    }

    check( ft->engine_destroy( engine ) == OS64_FONT_OK,
           "destroy after truncation sweep failed" );
    check( a.live == 0, "truncation sweep leaked %zu bytes", a.live );
}


/* ── metadata, metrics, lookup, kerning and masks ──────────────────────── */

static os64_font_face_t *open_fixture( os64_font_engine_t *engine, fixture *f,
                                       uint32_t px, os64_font_hint_t hint )
{
    os64_font_face_options_t  fo = { px, hint };
    os64_font_face_t         *face = NULL;
    os64_font_status_t        status;

    status = ft->face_open( engine, f->bytes, f->length, &fo, &face );
    check( status == OS64_FONT_OK, "%s at %upx: open gave %s",
           f->label, px, status_name( status ) );
    return face;
}


static os64_font_pos_t advance_of( os64_font_face_t *face, uint32_t scalar )
{
    uint32_t               index = 0;
    os64_font_glyph_t     *glyph = NULL;
    os64_font_glyph_view_t view;
    os64_font_pos_t        advance;

    if ( ft->lookup( face, scalar, &index ) != OS64_FONT_OK )
        return -1;
    if ( ft->render( face, index, &glyph ) != OS64_FONT_OK )
        return -1;
    if ( ft->glyph_view( glyph, &view ) != OS64_FONT_OK )
    {
        ft->glyph_release( glyph );
        return -1;
    }
    advance = view.advance_x;
    ft->glyph_release( glyph );
    return advance;
}


static void test_face_shape( fixture *f, uint32_t px )
{
    test_alloc                 a;
    os64_font_engine_options_t options;
    os64_font_engine_t        *engine = NULL;
    os64_font_face_t          *face;
    os64_font_face_info_t      info;
    char                       label[160];
    os64_font_pos_t            narrow, wide;

    snprintf( label, sizeof( label ), "face/%s@%u", f->label, px );
    begin( label );

    alloc_init( &a );
    options = options_for( &a, 0 );
    if ( ft->engine_create( &options, &engine ) != OS64_FONT_OK )
    {
        check( 0, "engine create failed" );
        return;
    }

    face = open_fixture( engine, f, px, OS64_FONT_HINT_NORMAL );
    if ( face == NULL )
    {
        ft->engine_destroy( engine );
        return;
    }

    check( ft->face_info( face, NULL ) == OS64_FONT_BAD_ARGUMENT,
           "NULL info accepted" );
    check( ft->face_info( face, &info ) == OS64_FONT_OK, "face_info failed" );

    check( info.glyph_count > 100, "glyph_count %u looks wrong", info.glyph_count );
    check( info.family[0] != '\0', "family name is empty" );
    check( memchr( info.family, '\0', sizeof( info.family ) ) != NULL,
           "family name is not terminated" );
    check( memchr( info.style, '\0', sizeof( info.style ) ) != NULL,
           "style name is not terminated" );

    check( info.ascent >= 0 && info.descent >= 0,
           "ascent %d / descent %d must be non-negative magnitudes",
           info.ascent, info.descent );
    check( info.line_height >= info.ascent + info.descent,
           "line_height %d < ascent %d + descent %d",
           info.line_height, info.ascent, info.descent );

    /* The metrics have to be in the right ballpark for the size asked for:
     * an em of `px` pixels is `px * 64` in 26.6, and a face's ascent plus
     * descent lands within a small factor of it. This is the check that would
     * catch a unit mix-up — pixels reported as 26.6 or the other way round. */
    check( info.ascent + info.descent > (os64_font_pos_t)px * 32 &&
           info.ascent + info.descent < (os64_font_pos_t)px * 64 * 3,
           "ascent+descent %d is not plausible for %u pixels in 26.6",
           info.ascent + info.descent, px );

    check( ( ( info.flags & OS64_FONT_FACE_FIXED_WIDTH ) != 0 ) == f->fixed_width,
           "fixed-width flag %s, expected %s",
           ( info.flags & OS64_FONT_FACE_FIXED_WIDTH ) ? "set" : "clear",
           f->fixed_width ? "set" : "clear" );

    /* THE INDEPENDENT ASSERTION: 'i' and 'W' are the narrowest and widest
     * ordinary Latin letters in any text face. A monospace face must give
     * them the same advance and a proportional one must not — a property of
     * the FONT, checked against the FACE's own claim about itself, so two
     * calls to one wrapper agreeing proves nothing here. */
    narrow = advance_of( face, 'i' );
    wide   = advance_of( face, 'W' );
    check( narrow > 0 && wide > 0, "i/W advances %d/%d", narrow, wide );
    if ( f->fixed_width )
        check( narrow == wide, "monospace face gives i=%d W=%d", narrow, wide );
    else
        check( narrow < wide, "proportional face gives i=%d W=%d (expected i<W)",
               narrow, wide );

    ft->face_close( face );
    check( ft->engine_destroy( engine ) == OS64_FONT_OK, "destroy failed" );
    check( a.live == 0, "leaked %zu bytes", a.live );
    check( a.size_mismatches == 0, "%d frees given the wrong size",
           a.size_mismatches );
}


static void test_lookup( void )
{
    test_alloc                 a;
    os64_font_engine_options_t options;
    os64_font_engine_t        *engine = NULL;
    os64_font_face_t          *face;
    uint32_t                   index;
    os64_font_status_t         status;

    begin( "lookup" );

    alloc_init( &a );
    options = options_for( &a, 0 );
    if ( ft->engine_create( &options, &engine ) != OS64_FONT_OK )
    {
        check( 0, "engine create failed" );
        return;
    }

    face = open_fixture( engine, SANS, 16, OS64_FONT_HINT_NORMAL );
    if ( face == NULL )
    {
        ft->engine_destroy( engine );
        return;
    }

    check( ft->lookup( face, 'A', NULL ) == OS64_FONT_BAD_ARGUMENT,
           "NULL out accepted" );
    check( ft->lookup( NULL, 'A', &index ) == OS64_FONT_BAD_ARGUMENT,
           "NULL face accepted" );

    index = 0xDEAD;
    check( ft->lookup( face, 'A', &index ) == OS64_FONT_OK, "'A' not found" );
    check( index != 0, "'A' resolved to glyph 0" );

    /* Western text, the declared first scope: an ASCII letter, a PRECOMPOSED
     * accented letter, the COMBINING acute that would compose it, and a
     * couple of symbols a document actually contains. */
    check( ft->lookup( face, 0x00E9, &index ) == OS64_FONT_OK && index != 0,
           "precomposed e-acute (U+00E9) not found" );
    check( ft->lookup( face, 0x0301, &index ) == OS64_FONT_OK && index != 0,
           "combining acute (U+0301) not found" );
    check( ft->lookup( face, 0x2014, &index ) == OS64_FONT_OK && index != 0,
           "em dash (U+2014) not found" );
    check( ft->lookup( face, 0x20AC, &index ) == OS64_FONT_OK && index != 0,
           "euro sign (U+20AC) not found" );

    /* A scalar the face does not carry is MISSING, and that is not an error
     * in the font, the caller or the engine. */
    index  = 0xDEAD;
    status = ft->lookup( face, 0x10FFFD, &index );
    check( status == OS64_FONT_MISSING, "private-use plane 16 gave %s",
           status_name( status ) );
    check( index == 0, "a MISSING lookup left the index set" );

    /* These are not characters at all, and saying MISSING would hide a caller
     * decoding UTF-8 or UTF-16 wrongly. */
    check( ft->lookup( face, 0xD800, &index ) == OS64_FONT_BAD_ARGUMENT,
           "leading surrogate accepted as a scalar" );
    check( ft->lookup( face, 0xDFFF, &index ) == OS64_FONT_BAD_ARGUMENT,
           "trailing surrogate accepted as a scalar" );
    check( ft->lookup( face, 0x110000, &index ) == OS64_FONT_BAD_ARGUMENT,
           "a value above U+10FFFF accepted as a scalar" );
    check( ft->lookup( face, 0xFFFFFFFF, &index ) == OS64_FONT_BAD_ARGUMENT,
           "0xFFFFFFFF accepted as a scalar" );

    ft->face_close( face );
    check( ft->engine_destroy( engine ) == OS64_FONT_OK, "destroy failed" );
    check( a.live == 0, "leaked %zu bytes", a.live );
}


/* THE PINNED KERNING EXPECTATIONS.
 *
 * Every value here was decoded from the font file by SEPARATE CODE — a
 * Python reader of the `kern` and `GPOS` tables, written for the purpose and
 * recorded in userland/libfreetype/fixtures/FIXTURES.md — and then converted
 * by hand: units * ppem * 64 / unitsPerEm, which is 26.6 pixels. So a wrong
 * answer from the backend cannot agree with itself into a pass; it has to
 * agree with a different program's reading of the same bytes.
 *
 * The two fonts reach the same answer down DIFFERENT ROADS, which is why
 * both are here. DejaVu Sans carries a legacy `kern` table, and upstream
 * prefers it. Source Sans 3 carries none, only `GPOS` pair positioning, so
 * it is the only fixture that proves TT_CONFIG_OPTION_GPOS_KERNING is doing
 * anything at all. Its "AV" is also the case for reporting kerning
 * unrounded: -14 units on a 1000-unit em is under half a pixel at 32px, and
 * a grid-fitted answer would be a flat zero.
 */
typedef struct
{
    fixture        **font;
    uint32_t         left, right;
    uint32_t         ppem;
    os64_font_pos_t  expected;   /* 26.6 pixels, derived independently */
    const char      *source;
} kern_expectation;

static fixture *kern_sans;
static fixture *kern_cff;

static const kern_expectation kern_table[] = {
    /* DejaVu Sans: 2048 units/em, from its `kern` table. */
    { &kern_sans, 'A', 'V', 32, -131, "kern -131/2048" },
    { &kern_sans, 'A', 'T', 32, -159, "kern -159/2048" },
    { &kern_sans, 'T', 'o', 32, -348, "kern -348/2048" },
    { &kern_sans, 'A', 'V', 16,  -65, "kern -131/2048" },
    { &kern_sans, 'T', 'o', 16, -174, "kern -348/2048" },
    /* Source Sans 3: 1000 units/em, from GPOS pair positioning only. */
    { &kern_cff,  'A', 'V', 32,  -28, "GPOS -14/1000" },
    { &kern_cff,  'L', 'T', 32, -245, "GPOS -120/1000" },
    { &kern_cff,  'T', 'o', 32, -135, "GPOS -66/1000" },
    { &kern_cff,  'L', 'T', 16, -122, "GPOS -120/1000" },
};


static void test_kerning( void )
{
    test_alloc                 a;
    os64_font_engine_options_t options;
    os64_font_engine_t        *engine = NULL;

    begin( "kerning" );

    kern_sans = SANS;
    kern_cff  = CFF_PROP;

    alloc_init( &a );
    options = options_for( &a, 0 );
    if ( ft->engine_create( &options, &engine ) != OS64_FONT_OK )
    {
        check( 0, "engine create failed" );
        return;
    }

    for ( size_t i = 0; i < sizeof( kern_table ) / sizeof( kern_table[0] ); i++ )
    {
        const kern_expectation *e = &kern_table[i];
        fixture                *f = *e->font;
        os64_font_face_t       *face;
        uint32_t                l = 0, r = 0;
        os64_font_pos_t         delta = 0xDEAD;


        face = open_fixture( engine, f, e->ppem, OS64_FONT_HINT_NORMAL );
        if ( face == NULL )
            continue;

        check( ft->lookup( face, e->left, &l ) == OS64_FONT_OK &&
               ft->lookup( face, e->right, &r ) == OS64_FONT_OK,
               "%s: no glyph for '%c%c'", f->label, (char)e->left, (char)e->right );

        if ( l != 0 && r != 0 )
        {
            check( ft->pair_adjust( face, l, r, &delta ) == OS64_FONT_OK,
                   "%s: pair_adjust('%c','%c') failed", f->label,
                   (char)e->left, (char)e->right );

            /* A tolerance of two 64ths absorbs the fixed-point scale's own
             * rounding; it is far tighter than the difference between a
             * right answer and a wrong table. */
            check( delta >= e->expected - 2 && delta <= e->expected + 2,
                   "%s @%upx '%c%c': kerning %d, expected %d (%s)",
                   f->label, e->ppem, (char)e->left, (char)e->right,
                   delta, e->expected, e->source );
        }

        ft->face_close( face );
    }

    /* Arguments, and the pairs that legitimately have no adjustment. */
    {
        os64_font_face_t *face = open_fixture( engine, SANS, 32,
                                               OS64_FONT_HINT_NORMAL );
        uint32_t          A = 0, V = 0, I = 0;
        os64_font_pos_t   delta = 0xDEAD;


        if ( face != NULL )
        {
            ft->lookup( face, 'A', &A );
            ft->lookup( face, 'V', &V );
            ft->lookup( face, 'I', &I );

            check( ft->pair_adjust( face, A, V, NULL ) == OS64_FONT_BAD_ARGUMENT,
                   "NULL delta accepted" );
            check( ft->pair_adjust( face, 0, V, &delta ) == OS64_FONT_BAD_ARGUMENT,
                   "glyph 0 accepted as a kerning operand" );
            check( ft->pair_adjust( face, A, 0xFFFFFF, &delta )
                       == OS64_FONT_BAD_ARGUMENT,
                   "out-of-range glyph accepted" );
            check( ft->pair_adjust( NULL, A, V, &delta ) == OS64_FONT_BAD_ARGUMENT,
                   "NULL face accepted" );
            check( delta == 0, "a refused pair_adjust left the delta set" );

            /* Two upright stems have nothing to tuck. An ABSENT pair is a
             * success returning zero, not a MISSING — the difference matters
             * to a layout engine, which asks about every adjacent pair. */
            check( ft->pair_adjust( face, I, I, &delta ) == OS64_FONT_OK,
                   "I/I pair_adjust failed" );
            check( delta == 0, "I/I kerning is %d, expected 0", delta );

            ft->face_close( face );
        }
    }

    /* A monospace face keeps its grid: kerning it would break the cell. */
    {
        os64_font_face_t *face = open_fixture( engine, SANS_MONO, 32,
                                               OS64_FONT_HINT_NORMAL );
        uint32_t          A = 0, V = 0;
        os64_font_pos_t   delta = 1;


        if ( face != NULL )
        {
            ft->lookup( face, 'A', &A );
            ft->lookup( face, 'V', &V );
            check( ft->pair_adjust( face, A, V, &delta ) == OS64_FONT_OK,
                   "mono A/V pair_adjust failed" );
            check( delta == 0, "monospace face kerns A/V by %d", delta );
            ft->face_close( face );
        }
    }

    check( ft->engine_destroy( engine ) == OS64_FONT_OK, "destroy failed" );
    check( a.live == 0, "leaked %zu bytes", a.live );
}


static void test_render( fixture *f, uint32_t px )
{
    test_alloc                 a;
    os64_font_engine_options_t options;
    os64_font_engine_t        *engine = NULL;
    os64_font_face_t          *face;
    os64_font_face_info_t      info;
    os64_font_glyph_t         *glyph = NULL;
    os64_font_glyph_view_t     view;
    uint32_t                   index = 0;
    char                       label[160];

    snprintf( label, sizeof( label ), "render/%s@%u", f->label, px );
    begin( label );

    alloc_init( &a );
    options = options_for( &a, 0 );
    if ( ft->engine_create( &options, &engine ) != OS64_FONT_OK )
    {
        check( 0, "engine create failed" );
        return;
    }

    face = open_fixture( engine, f, px, OS64_FONT_HINT_NORMAL );
    if ( face == NULL )
    {
        ft->engine_destroy( engine );
        return;
    }
    ft->face_info( face, &info );

    check( ft->render( face, 0, &glyph ) == OS64_FONT_BAD_ARGUMENT,
           "glyph 0 accepted by render" );
    check( ft->render( face, info.glyph_count, &glyph ) == OS64_FONT_BAD_ARGUMENT,
           "glyph index at the count accepted by render" );
    check( ft->render( face, 1, NULL ) == OS64_FONT_BAD_ARGUMENT,
           "NULL out accepted by render" );
    check( glyph == NULL, "a refused render left the handle set" );

    /* AN INKED GLYPH. Every geometric field is cross-checked against another:
     * ink must be exactly the bearings and dimensions in 26.6, the stride
     * must be the width (the contract's mask is tightly packed), and the
     * coverage must actually contain ink. */
    check( ft->lookup( face, 'W', &index ) == OS64_FONT_OK, "no 'W'" );
    check( ft->render( face, index, &glyph ) == OS64_FONT_OK, "'W' render failed" );
    if ( glyph != NULL )
    {
        int any_ink   = 0;
        int any_edge  = 0;


        check( ft->glyph_view( glyph, NULL ) == OS64_FONT_BAD_ARGUMENT,
               "NULL view accepted" );
        check( ft->glyph_view( glyph, &view ) == OS64_FONT_OK, "view failed" );

        check( view.width > 0 && view.height > 0, "'W' has no raster" );
        check( view.stride == view.width, "stride %u != width %u",
               view.stride, view.width );
        check( view.width <= OS64_FONT_MASK_DIM_MAX &&
               view.height <= OS64_FONT_MASK_DIM_MAX, "mask past the dimension cap" );
        check( view.coverage != NULL, "an inked glyph has no coverage" );
        check( view.advance_x > 0, "'W' advance is %d", view.advance_x );

        check( view.ink.x0 == view.left * OS64_FONT_UNIT, "ink.x0 != left*64" );
        check( view.ink.y0 == view.top * OS64_FONT_UNIT, "ink.y0 != top*64" );
        check( view.ink.x1 == ( view.left + (int32_t)view.width ) * OS64_FONT_UNIT,
               "ink.x1 != (left+width)*64" );
        check( view.ink.y1 == ( view.top + (int32_t)view.height ) * OS64_FONT_UNIT,
               "ink.y1 != (top+height)*64" );

        /* Y GROWS DOWN and the origin is the baseline, so a capital letter's
         * top edge is ABOVE it — a negative `top` — and it does not descend
         * past the baseline. This is the check that catches the sign of the
         * bearing being carried over from FreeType's upward convention. */
        check( view.top < 0, "'W' top bearing is %d; expected negative (Y down)",
               view.top );
        check( view.top + (int32_t)view.height <= 1,
               "'W' descends %d pixels below the baseline",
               view.top + (int32_t)view.height );
        check( -view.top * OS64_FONT_UNIT <= info.ascent + OS64_FONT_UNIT,
               "'W' rises %d past the face ascent %d",
               -view.top * OS64_FONT_UNIT, info.ascent );

        if ( view.coverage != NULL )
        {
            for ( size_t i = 0; i < (size_t)view.width * view.height; i++ )
            {
                if ( view.coverage[i] != 0 )
                    any_ink = 1;
                /* A partially covered pixel is the proof that this is a
                 * GRAYSCALE mask and not a 1-bit one widened to bytes —
                 * which is what a monochrome renderer would hand back. */
                if ( view.coverage[i] != 0 && view.coverage[i] != 255 )
                    any_edge = 1;
            }
        }
        check( any_ink, "'W' rendered to an all-zero mask" );
        check( any_edge, "'W' has no partial coverage — is this antialiased?" );

        /* THE OWNED-MASK PROMISE: the glyph outlives the face it came from.
         * Reading the coverage after the close is the test — under ASan, a
         * borrowed slot would report here instead of passing. */
        ft->face_close( face );
        face = NULL;

        check( ft->glyph_view( glyph, &view ) == OS64_FONT_OK,
               "view failed after the face closed" );
        if ( view.coverage != NULL )
        {
            /* Touch the FIRST and LAST bytes: under ASan a mask that was
             * really the face's slot reports here, which is the whole point
             * of reading it at this moment rather than earlier. */
            size_t last = (size_t)view.width * view.height - 1;
            int    sum  = view.coverage[0] + view.coverage[last];


            check( sum >= 0, "coverage unreadable after the face closed" );
        }
        else
            check( 0, "an inked glyph lost its coverage when the face closed" );

        check( ft->engine_destroy( engine ) == OS64_FONT_BUSY,
               "engine destroyed while a glyph was still live" );

        ft->glyph_release( glyph );
        glyph = NULL;
    }

    if ( face != NULL )
        ft->face_close( face );

    ft->glyph_release( NULL );          /* documented no-op */

    check( ft->engine_destroy( engine ) == OS64_FONT_OK,
           "destroy after releasing everything failed" );
    check( a.live == 0, "leaked %zu bytes", a.live );
    check( a.size_mismatches == 0, "%d frees given the wrong size",
           a.size_mismatches );
}


/* The two shapes that are easy to get wrong and invisible when you do: a
 * glyph with no ink at all, and one whose ink hangs to the LEFT of the pen. */
static void test_glyph_edges( void )
{
    test_alloc                 a;
    os64_font_engine_options_t options;
    os64_font_engine_t        *engine = NULL;
    os64_font_face_t          *face;
    os64_font_glyph_t         *glyph = NULL;
    os64_font_glyph_view_t     view;
    uint32_t                   index = 0;
    int                        found_negative_bearing = 0;

    begin( "glyph/edges" );

    alloc_init( &a );
    options = options_for( &a, 0 );
    if ( ft->engine_create( &options, &engine ) != OS64_FONT_OK )
    {
        check( 0, "engine create failed" );
        return;
    }

    face = open_fixture( engine, SANS, 24, OS64_FONT_HINT_NORMAL );
    if ( face == NULL )
    {
        ft->engine_destroy( engine );
        return;
    }

    /* A SPACE: a real glyph with a real advance and no raster. The contract
     * calls this a success, not an empty failure, and every geometric field
     * is zero so a caller cannot place a mask that is not there. */
    check( ft->lookup( face, ' ', &index ) == OS64_FONT_OK, "no space glyph" );
    check( ft->render( face, index, &glyph ) == OS64_FONT_OK, "space render failed" );
    if ( glyph != NULL )
    {
        check( ft->glyph_view( glyph, &view ) == OS64_FONT_OK, "space view failed" );
        check( view.width == 0 && view.height == 0, "space has a %ux%u raster",
               view.width, view.height );
        check( view.stride == 0, "space stride is %u", view.stride );
        check( view.coverage == NULL, "space has a coverage pointer" );
        check( view.left == 0 && view.top == 0, "space has bearings %d,%d",
               view.left, view.top );
        check( view.ink.x0 == 0 && view.ink.y0 == 0 &&
               view.ink.x1 == 0 && view.ink.y1 == 0, "space has a non-empty ink" );
        check( view.advance_x > 0, "space advance is %d", view.advance_x );
        ft->glyph_release( glyph );
        glyph = NULL;
    }

    /* A NEGATIVE LEFT BEARING: ink that starts before the pen position.
     * Italic and script faces are full of them; in an upright text face the
     * usual suspects are 'j' and 'J'. Whichever one this face has, the point
     * is that the contract can carry it — `left` is signed and the ink
     * rectangle follows it. */
    for ( uint32_t scalar = 32; scalar < 127 && !found_negative_bearing; scalar++ )
    {
        if ( ft->lookup( face, scalar, &index ) != OS64_FONT_OK )
            continue;
        if ( ft->render( face, index, &glyph ) != OS64_FONT_OK )
            continue;
        if ( ft->glyph_view( glyph, &view ) == OS64_FONT_OK && view.left < 0 )
        {
            found_negative_bearing = 1;
            check( view.ink.x0 == view.left * OS64_FONT_UNIT,
                   "negative-bearing glyph: ink.x0 %d != left*64 %d",
                   view.ink.x0, view.left * OS64_FONT_UNIT );
            check( view.ink.x0 < 0, "a negative left bearing gave ink.x0 %d",
                   view.ink.x0 );
        }
        ft->glyph_release( glyph );
        glyph = NULL;
    }
    check( found_negative_bearing,
           "no ASCII glyph in %s has a negative left bearing at 24px "
           "— the overhang case went untested", SANS->label );

    ft->face_close( face );
    check( ft->engine_destroy( engine ) == OS64_FONT_OK, "destroy failed" );
    check( a.live == 0, "leaked %zu bytes", a.live );
}


/* ── independence, lifetimes and the face ceiling ──────────────────────── */

static void test_independent_contexts( void )
{
    test_alloc                 a1, a2;
    os64_font_engine_options_t o1, o2;
    os64_font_engine_t        *e1 = NULL, *e2 = NULL;
    os64_font_face_t          *f1, *f2;
    os64_font_engine_stats_t   s1, s2;
    os64_font_glyph_t         *g1 = NULL, *g2 = NULL;
    uint32_t                   i1 = 0, i2 = 0;

    begin( "contexts/independent" );

    alloc_init( &a1 );
    alloc_init( &a2 );
    o1 = options_for( &a1, 0 );
    o2 = options_for( &a2, 0 );

    check( ft->engine_create( &o1, &e1 ) == OS64_FONT_OK, "engine 1 failed" );
    check( ft->engine_create( &o2, &e2 ) == OS64_FONT_OK, "engine 2 failed" );
    if ( e1 == NULL || e2 == NULL )
        return;

    /* Two engines over the SAME bytes at different sizes, interleaved. Each
     * accounts through its own allocator, so a shared anything shows up as
     * one context's bytes appearing in the other's ledger. */
    f1 = open_fixture( e1, SANS, 12, OS64_FONT_HINT_NORMAL );
    f2 = open_fixture( e2, SANS, 48, OS64_FONT_HINT_NONE );

    if ( f1 != NULL && f2 != NULL )
    {
        os64_font_glyph_view_t v1, v2;


        ft->lookup( f1, 'M', &i1 );
        ft->lookup( f2, 'M', &i2 );
        check( i1 == i2, "the same scalar resolved to %u and %u in one font",
               i1, i2 );

        check( ft->render( f1, i1, &g1 ) == OS64_FONT_OK, "engine 1 render failed" );
        check( ft->render( f2, i2, &g2 ) == OS64_FONT_OK, "engine 2 render failed" );

        if ( g1 != NULL && g2 != NULL )
        {
            ft->glyph_view( g1, &v1 );
            ft->glyph_view( g2, &v2 );
            check( v2.advance_x > v1.advance_x,
                   "48px 'M' advance %d is not greater than 12px's %d",
                   v2.advance_x, v1.advance_x );
            check( v2.height > v1.height,
                   "48px 'M' is %u rows, 12px is %u", v2.height, v1.height );
        }

        ft->engine_stats( e1, &s1 );
        ft->engine_stats( e2, &s2 );
        check( s1.live_faces == 1 && s2.live_faces == 1,
               "face counts %u / %u", s1.live_faces, s2.live_faces );
        check( s1.live_glyphs == 1 && s2.live_glyphs == 1,
               "glyph counts %u / %u", s1.live_glyphs, s2.live_glyphs );
        check( a1.live > 0 && a2.live > 0, "one context allocated nothing" );
    }

    /* Tear them down in the WRONG order on purpose: engine 1's face first,
     * engine 2's glyph last. Neither may notice the other. */
    if ( f1 != NULL )
        ft->face_close( f1 );
    ft->glyph_release( g1 );
    check( ft->engine_destroy( e1 ) == OS64_FONT_OK, "engine 1 destroy failed" );
    check( a1.live == 0, "engine 1 leaked %zu bytes", a1.live );
    check( a2.live > 0, "engine 1's destruction freed engine 2's memory" );

    if ( f2 != NULL )
        ft->face_close( f2 );
    ft->glyph_release( g2 );
    check( ft->engine_destroy( e2 ) == OS64_FONT_OK, "engine 2 destroy failed" );
    check( a2.live == 0, "engine 2 leaked %zu bytes", a2.live );
}


static void test_face_ceiling( void )
{
    test_alloc                 a;
    os64_font_engine_options_t options;
    os64_font_engine_t        *engine = NULL;
    os64_font_face_t          *faces[OS64_FONT_FACE_MAX + 1];
    os64_font_face_options_t   fo = { 16, OS64_FONT_HINT_NORMAL };
    os64_font_engine_stats_t   stats;
    unsigned                   opened = 0;

    begin( "faces/ceiling" );

    alloc_init( &a );
    options = options_for( &a, 0 );
    if ( ft->engine_create( &options, &engine ) != OS64_FONT_OK )
    {
        check( 0, "engine create failed" );
        return;
    }

    memset( faces, 0, sizeof( faces ) );
    for ( unsigned i = 0; i < OS64_FONT_FACE_MAX; i++ )
    {
        if ( ft->face_open( engine, CFF_MONO->bytes, CFF_MONO->length, &fo,
                            &faces[i] ) == OS64_FONT_OK )
            opened++;
        else
            break;
    }
    check( opened == OS64_FONT_FACE_MAX, "only %u of %u faces opened",
           opened, OS64_FONT_FACE_MAX );

    check( ft->face_open( engine, CFF_MONO->bytes, CFF_MONO->length, &fo,
                          &faces[OS64_FONT_FACE_MAX] ) == OS64_FONT_LIMIT,
           "the face past the ceiling was accepted" );
    check( faces[OS64_FONT_FACE_MAX] == NULL, "a refused open left a handle" );

    ft->engine_stats( engine, &stats );
    check( stats.live_faces == opened, "stats say %u faces, %u were opened",
           stats.live_faces, opened );

    check( ft->engine_destroy( engine ) == OS64_FONT_BUSY,
           "engine destroyed with %u live faces", opened );

    for ( unsigned i = 0; i < opened; i++ )
        ft->face_close( faces[i] );
    ft->face_close( NULL );             /* documented no-op */

    check( ft->engine_destroy( engine ) == OS64_FONT_OK, "destroy failed" );
    check( a.live == 0, "leaked %zu bytes", a.live );
}


/* Repeated open/close of the same bytes: the accounting has to come back to
 * the same number every time, or something is retained that should not be. */
static void test_repeated_open( void )
{
    test_alloc                 a;
    os64_font_engine_options_t options;
    os64_font_engine_t        *engine = NULL;
    size_t                     baseline = 0;

    begin( "faces/repeated" );

    alloc_init( &a );
    options = options_for( &a, 0 );
    if ( ft->engine_create( &options, &engine ) != OS64_FONT_OK )
    {
        check( 0, "engine create failed" );
        return;
    }
    baseline = a.live;

    for ( int round = 0; round < 8; round++ )
    {
        for ( size_t f = 0; f < sizeof( fixtures ) / sizeof( fixtures[0] ); f++ )
        {
            os64_font_face_t  *face;
            os64_font_glyph_t *glyph = NULL;
            uint32_t           index = 0;


            face = open_fixture( engine, &fixtures[f], 16 + (uint32_t)round,
                                 ( round & 1 ) ? OS64_FONT_HINT_NONE
                                               : OS64_FONT_HINT_NORMAL );
            if ( face == NULL )
                continue;
            if ( ft->lookup( face, 'g', &index ) == OS64_FONT_OK &&
                 ft->render( face, index, &glyph ) == OS64_FONT_OK )
                ft->glyph_release( glyph );
            ft->face_close( face );
        }
        check( a.live == baseline,
               "round %d left %zu bytes live, baseline is %zu",
               round, a.live, baseline );
    }

    check( ft->engine_destroy( engine ) == OS64_FONT_OK, "destroy failed" );
    check( a.live == 0, "leaked %zu bytes", a.live );
    check( a.size_mismatches == 0, "%d frees given the wrong size",
           a.size_mismatches );
}


/* ── allocation denial, at every reachable allocation ──────────────────── */
/*
 * Not a sample: the harness first counts how many allocations a full
 * open-render-close cycle makes, then replays the cycle failing the Nth for
 * every N up to that count. What is being tested is not that failure is
 * reported — it is that the engine SURVIVES each one with a usable state and
 * no leak, which is the part a spot check always misses.
 */

static void alloc_denial_cycle( os64_font_engine_t *engine, fixture *f )
{
    os64_font_face_options_t  fo    = { 20, OS64_FONT_HINT_NORMAL };
    os64_font_face_t         *face  = NULL;
    os64_font_glyph_t        *glyph = NULL;
    os64_font_face_info_t     info;
    uint32_t                  index = 0;

    if ( ft->face_open( engine, f->bytes, f->length, &fo, &face ) != OS64_FONT_OK )
        return;
    if ( ft->face_info( face, &info ) == OS64_FONT_OK &&
         ft->lookup( face, 'R', &index ) == OS64_FONT_OK &&
         ft->render( face, index, &glyph ) == OS64_FONT_OK )
    {
        os64_font_glyph_view_t view;


        ft->glyph_view( glyph, &view );
        ft->glyph_release( glyph );
    }
    ft->face_close( face );
}


static void test_allocation_denial( void )
{
    test_alloc                 a;
    os64_font_engine_options_t options;
    os64_font_engine_t        *engine = NULL;
    long                       total;
    long                       create_cost;

    begin( "allocation/denial" );

    /* First, the census: how many allocations does a whole cycle make? */
    alloc_init( &a );
    options = options_for( &a, 0 );
    if ( ft->engine_create( &options, &engine ) != OS64_FONT_OK )
    {
        check( 0, "engine create failed" );
        return;
    }
    create_cost = a.allocations;
    alloc_denial_cycle( engine, SANS );
    total = a.allocations;
    ft->engine_destroy( engine );
    check( a.live == 0, "census cycle leaked %zu bytes", a.live );
    check( total > create_cost, "the cycle made no allocations of its own" );

    /* Now fail each one in turn. Two invariants per round, and they are the
     * whole point: nothing is leaked, and the engine can still be destroyed. */
    for ( long n = 0; n < total; n++ )
    {
        alloc_init( &a );
        a.fail_at = n;
        options   = options_for( &a, 0 );
        engine    = NULL;

        os64_font_status_t status = ft->engine_create( &options, &engine );
        if ( n < create_cost )
            check( status == OS64_FONT_NO_MEMORY && engine == NULL,
                   "creation callback refusal %ld returned %d", n, status );
        if ( status != OS64_FONT_OK )
        {
            check( a.live == 0, "failing allocation %ld leaked %zu bytes during "
                   "engine create", n, a.live );
            continue;
        }

        alloc_denial_cycle( engine, SANS );

        check( ft->engine_destroy( engine ) == OS64_FONT_OK,
               "failing allocation %ld left the engine undestroyable", n );
        check( a.live == 0, "failing allocation %ld leaked %zu bytes",
               n, a.live );
        check( a.size_mismatches == 0,
               "failing allocation %ld produced %d wrong-size frees",
               n, a.size_mismatches );
    }


}


/* Budget refusal and callback refusal drive different cache recovery paths.
 * Measure a warmed space glyph through the public table, then fill a cap
 * exactly with owned glyphs: the next render must not reach the allocator. */
static void test_budget_status( void )
{
    test_alloc a;
    os64_font_engine_options_t options;
    os64_font_face_options_t fo = { 16, OS64_FONT_HINT_NORMAL };
    os64_font_engine_t *engine = NULL;
    os64_font_face_t *face = NULL;
    os64_font_glyph_t *glyph = NULL;
    os64_font_glyph_t *held[512] = { 0 };
    os64_font_engine_stats_t before, after;
    size_t creation_peak, baseline, glyph_cost, cap;
    uint32_t index = 0;
    os64_font_status_t status;
    long calls;
    size_t count = 0;

    begin( "allocation/budget-status" );
    alloc_init( &a );
    options = options_for( &a, 0 );
    if ( ft->engine_create( &options, &engine ) != OS64_FONT_OK )
    {
        check( 0, "budget census create failed" );
        return;
    }
    ft->engine_stats( engine, &before );
    creation_peak = before.peak_bytes;
    if ( ft->face_open( engine, SANS->bytes, SANS->length, &fo, &face ) != OS64_FONT_OK ||
         ft->lookup( face, ' ', &index ) != OS64_FONT_OK ||
         ft->render( face, index, &glyph ) != OS64_FONT_OK )
    {
        check( 0, "budget census warm-up failed" );
        goto cleanup;
    }
    ft->glyph_release( glyph );
    glyph = NULL;
    ft->engine_stats( engine, &before );
    baseline = before.live_bytes;
    if ( ft->render( face, index, &glyph ) != OS64_FONT_OK )
    {
        check( 0, "budget census render failed" );
        goto cleanup;
    }
    ft->engine_stats( engine, &after );
    glyph_cost = after.live_bytes - baseline;
    cap = baseline + glyph_cost * 512;
    check( glyph_cost > 0 && cap >= after.peak_bytes,
           "census must leave enough room for initialization" );
    ft->glyph_release( glyph );
    glyph = NULL;
    ft->face_close( face );
    face = NULL;
    ft->engine_destroy( engine );
    engine = NULL;

    options = options_for( &a, creation_peak - 1 );
    status = ft->engine_create( &options, &engine );
    check( status == OS64_FONT_LIMIT && engine == NULL,
           "module initialization cap refusal returned %d", status );
    if ( engine != NULL )
        ft->engine_destroy( engine );
    engine = NULL;
    check( a.live == 0, "failed module initialization leaked %zu bytes", a.live );

    options = options_for( &a, creation_peak );
    if ( ft->engine_create( &options, &engine ) == OS64_FONT_OK )
    {
        status = ft->face_open( engine, SANS->bytes, SANS->length, &fo, &face );
        check( status == OS64_FONT_LIMIT && face == NULL,
               "face-open cap refusal returned %d", status );
        if ( face != NULL )
            ft->face_close( face );
        face = NULL;
        ft->engine_destroy( engine );
        engine = NULL;
    }
    else
        check( 0, "measured creation cap must permit initialization" );

    options = options_for( &a, cap );
    if ( ft->engine_create( &options, &engine ) != OS64_FONT_OK ||
         ft->face_open( engine, SANS->bytes, SANS->length, &fo, &face ) != OS64_FONT_OK ||
         ft->lookup( face, ' ', &index ) != OS64_FONT_OK ||
         ft->render( face, index, &glyph ) != OS64_FONT_OK )
    {
        check( 0, "capped engine warm-up failed" );
        goto cleanup;
    }
    ft->glyph_release( glyph );
    glyph = NULL;
    for ( ; count < 512; count++ )
    {
        if ( ft->render( face, index, &held[count] ) != OS64_FONT_OK )
            break;
    }
    check( count == 512, "only %zu glyphs fit the measured cap", count );
    calls = a.allocations;
    status = ft->render( face, index, &glyph );
    check( status == OS64_FONT_LIMIT && glyph == NULL,
           "full-cap render returned %d", status );
    check( a.allocations == calls, "cap refusal called the allocator" );
    if ( count != 0 )
        ft->glyph_release( held[--count] );
    a.fail_at = a.allocations;
    status = ft->render( face, index, &glyph );
    check( status == OS64_FONT_NO_MEMORY && glyph == NULL,
           "callback refusal after cap refusal returned %d", status );
    check( a.allocations > calls, "callback refusal never reached allocator" );
    a.fail_at = -1;
    status = ft->render( face, index, &glyph );
    check( status == OS64_FONT_OK && glyph != NULL,
           "render did not recover after eviction and allocator recovery" );
cleanup:
    ft->glyph_release( glyph );
    while ( count != 0 )
        ft->glyph_release( held[--count] );
    ft->face_close( face );
    check( ft->engine_destroy( engine ) == OS64_FONT_OK, "budget engine destroy" );
    check( a.live == 0 && a.size_mismatches == 0, "budget cleanup/accounting" );
}


/* ── the character-map validator's nonlocal return ─────────────────────── */
/*
 * The one jump in the compiled configuration, and the reason port/nonlocal.S
 * exists. Upstream validates every `cmap` subtable as it builds the charmap
 * list, and `ft_validator_error` jumps out of that parse the moment a bound
 * fails. This test CORRUPTS a real font's cmap so the jump actually fires,
 * then keeps using the engine — because a jump that unwinds correctly and
 * leaves the library unusable afterwards has not worked.
 *
 * It must run at the optimization level that ships. -O0 would spill every
 * local to the stack and hide exactly the register-liveness mistake the jump
 * can make.
 */

static uint32_t read_be32( const uint8_t *p )
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8  | (uint32_t)p[3];
}


static uint16_t read_be16( const uint8_t *p )
{
    return (uint16_t)( p[0] << 8 | p[1] );
}


/* Find a table in an SFNT directory. Returns 0 when it is not there. */
static uint32_t find_table( const uint8_t *bytes, size_t length,
                            const char *tag, uint32_t *out_length )
{
    uint16_t count;

    if ( length < 12 )
        return 0;
    count = read_be16( bytes + 4 );
    if ( (size_t)12 + (size_t)count * 16 > length )
        return 0;

    for ( uint16_t i = 0; i < count; i++ )
    {
        const uint8_t *entry = bytes + 12 + (size_t)i * 16;


        if ( memcmp( entry, tag, 4 ) == 0 )
        {
            *out_length = read_be32( entry + 12 );
            return read_be32( entry + 8 );
        }
    }
    return 0;
}


/* The port's host-only jump counter (port/runtime.c). Reading it is how this
 * test distinguishes "the corrupted font was rejected" — which any bounds
 * check could do — from "the validator's nonlocal return actually ran". */
extern unsigned long ftport_longjmp_count;


static void test_charmap_validator( void )
{
    unsigned long jumps_before;

    test_alloc                 a;
    os64_font_engine_options_t options;
    os64_font_engine_t        *engine = NULL;
    os64_font_face_options_t   fo     = { 16, OS64_FONT_HINT_NORMAL };
    uint8_t                   *copy;
    uint32_t                   cmap_offset, cmap_length;
    int                        changed_any = 0;

    begin( "cmap/validator" );
    jumps_before = ftport_longjmp_count;

    cmap_offset = find_table( SANS->bytes, SANS->length, "cmap", &cmap_length );
    check( cmap_offset != 0, "no cmap table found in %s", SANS->label );
    if ( cmap_offset == 0 || cmap_offset + cmap_length > SANS->length )
        return;

    copy = malloc( SANS->length );
    if ( copy == NULL )
    {
        check( 0, "out of host memory" );
        return;
    }

    alloc_init( &a );
    options = options_for( &a, 0 );
    if ( ft->engine_create( &options, &engine ) != OS64_FONT_OK )
    {
        check( 0, "engine create failed" );
        free( copy );
        return;
    }

    /* Walk a corruption through the cmap table a byte at a time, at a stride
     * that covers subtable headers, segment counts and range arrays. Some
     * corruptions are harmless, some make the face unopenable, and some trip
     * the validator — which is the one being hunted. Every outcome must be
     * survivable and leave the engine usable, so the loop asserts that after
     * EACH one rather than at the end. */
    for ( uint32_t at = 0; at < cmap_length; at += 7 )
    {
        os64_font_face_t *face = NULL;
        uint32_t          index = 0;


        memcpy( copy, SANS->bytes, SANS->length );
        copy[cmap_offset + at] ^= 0xFF;

        if ( ft->face_open( engine, copy, SANS->length, &fo, &face )
                 == OS64_FONT_OK )
        {
            /* It opened — which a corrupted cmap often does, because a
             * subtable that fails validation is DROPPED and the face keeps
             * the others. It must still answer lookups without reading past
             * the table, and a lookup that now misses is itself evidence
             * that a subtable was thrown away. */
            if ( ft->lookup( face, 'A', &index ) != OS64_FONT_OK )
                changed_any = 1;
            ft->face_close( face );
        }
        else
        {
            changed_any = 1;
            check( face == NULL, "a refused open at cmap+%u left a handle", at );
        }

        /* The engine is still the engine: a jump that landed badly would show
         * up as a failure to open a font that was fine a moment ago. */
        {
            os64_font_face_t *good = NULL;


            if ( ft->face_open( engine, SANS->bytes, SANS->length, &fo, &good )
                     != OS64_FONT_OK )
            {
                check( 0, "the engine stopped working after corrupting cmap+%u",
                       at );
                break;
            }
            ft->face_close( good );
        }
    }

    check( changed_any,
           "no cmap corruption changed the outcome — the sweep may not have "
           "reached anything the validator looks at" );

    /* THE ACCEPTANCE CRITERION ITSELF: the jump ran, out of a real parse of a
     * real font, in a build compiled at the level that ships. */
    check( ftport_longjmp_count > jumps_before,
           "the character-map validator never took its nonlocal return "
           "(%lu jumps before, %lu after)", jumps_before, ftport_longjmp_count );

    check( ft->engine_destroy( engine ) == OS64_FONT_OK,
           "destroy after the cmap sweep failed" );
    check( a.live == 0, "cmap sweep leaked %zu bytes", a.live );
    free( copy );
}


/* A direct exercise of the jump itself, independent of any font: arm it,
 * fire it from a callee, and check both the value and that the locals the
 * caller declared volatile survived. */

static unsigned long jump_target[8];

/* Recurses a few frames before jumping, so the restore has real frames to
 * skip rather than just its caller's. It never returns by any path — which
 * is what the attribute says, and what stops the recursion warning from
 * reading it as a runaway. */
static void __attribute__(( noinline, noreturn )) jump_from_here( int depth )
{
    if ( depth > 0 )
        jump_from_here( depth - 1 );

    ftport_longjmp( jump_target, 42 );
}


static void test_nonlocal_return( void )
{
    volatile int arrived = 0;
    volatile int marker  = 0x5A5A;
    int          answer;

    begin( "nonlocal/direct" );

    answer = ftport_setjmp( jump_target );
    if ( answer == 0 )
    {
        arrived = 1;
        jump_from_here( 5 );
        check( 0, "control returned from a jump that should not return" );
    }
    else
    {
        check( answer == 42, "the jump delivered %d, expected 42", answer );
        check( arrived == 1, "a volatile local set before the jump read back %d",
               arrived );
        check( marker == 0x5A5A, "a volatile local was clobbered by the jump" );
    }

    /* A value of zero must arrive as one, or "did I just get here" is not
     * answerable. */
    answer = ftport_setjmp( jump_target );
    if ( answer == 0 )
        ftport_longjmp( jump_target, 0 );
    else
        check( answer == 1, "longjmp(0) delivered %d, expected 1", answer );
}


/* ── the private runtime, on its own terms ─────────────────────────────── */

size_t  ftport_strlen( const char *s );
int     ftport_strcmp( const char *a, const char *b );
int     ftport_strncmp( const char *a, const char *b, size_t n );
char   *ftport_strcpy( char *d, const char *s );
char   *ftport_strncpy( char *d, const char *s, size_t n );
char   *ftport_strcat( char *d, const char *s );
char   *ftport_strrchr( const char *s, int c );
char   *ftport_strstr( const char *h, const char *n );
void   *ftport_memchr( const void *b, int c, size_t n );
int     ftport_memcmp( const void *a, const void *b, size_t n );
void   *ftport_memmove( void *d, const void *s, size_t n );
void    ftport_qsort( void *base, size_t count, size_t size,
                      int ( *cmp )( const void *, const void * ) );


static int compare_int( const void *a, const void *b )
{
    int x = *(const int *)a, y = *(const int *)b;

    return x < y ? -1 : x > y ? 1 : 0;
}


static void test_runtime( void )
{
    char  buf[16];
    int   values[64];
    int   sorted = 1;

    begin( "runtime" );

    /* Against the host's own libc, because these stand in for it. Sign is the
     * classic trap: `strcmp` compares as UNSIGNED characters, so a byte above
     * 0x7F must sort ABOVE 'a', not below it. */
    check( ftport_strlen( "" ) == 0 && ftport_strlen( "abc" ) == 3, "strlen" );
    check( ( ftport_strcmp( "abc", "abd" ) < 0 ) == ( strcmp( "abc", "abd" ) < 0 ),
           "strcmp ordering" );
    check( ftport_strcmp( "abc", "abc" ) == 0, "strcmp equality" );
    check( ftport_strcmp( "\xFF", "a" ) > 0,
           "strcmp compared a high byte as signed" );
    check( ftport_strncmp( "abcd", "abce", 3 ) == 0, "strncmp prefix" );
    check( ftport_strncmp( "abc", "abd", 8 ) < 0, "strncmp past the NUL" );

    memset( buf, 'Z', sizeof( buf ) );
    ftport_strcpy( buf, "hi" );
    check( memcmp( buf, "hi\0ZZZ", 6 ) == 0, "strcpy wrote past the NUL" );

    /* strncpy's two warts, both of which FreeType relies on: pad the tail
     * with NULs, and write NO terminator when the source fills the buffer. */
    memset( buf, 'Z', sizeof( buf ) );
    ftport_strncpy( buf, "hi", 6 );
    check( memcmp( buf, "hi\0\0\0\0ZZ", 8 ) == 0, "strncpy did not pad" );
    memset( buf, 'Z', sizeof( buf ) );
    ftport_strncpy( buf, "abcdef", 3 );
    check( memcmp( buf, "abcZ", 4 ) == 0, "strncpy over-wrote or terminated" );

    strcpy( buf, "ab" );
    ftport_strcat( buf, "cd" );
    check( strcmp( buf, "abcd" ) == 0, "strcat" );

    {
        static const char  path[]   = "a/b/c";
        static const char  haystack[] = "hello world";


        check( ftport_strrchr( path, '/' ) == strrchr( path, '/' ),
               "strrchr picked the wrong slash" );
        check( ftport_strrchr( path, '\0' ) == strrchr( path, '\0' ),
               "strrchr does not find the terminator" );
        check( ftport_strrchr( path, 'z' ) == NULL, "strrchr found a missing byte" );

        check( ftport_strstr( haystack, "o w" ) == strstr( haystack, "o w" ),
               "strstr found the wrong occurrence" );
        check( ftport_strstr( haystack, "" ) == haystack,
               "an empty needle must match at the start" );
        check( ftport_strstr( haystack, "xyz" ) == NULL, "strstr false positive" );
    }

    check( ftport_memchr( "abcd", 'c', 4 ) != NULL, "memchr miss" );
    check( ftport_memchr( "abcd", 'c', 2 ) == NULL, "memchr ran past its length" );
    check( ftport_memcmp( "\xFF", "\x01", 1 ) > 0,
           "memcmp compared a high byte as signed" );

    /* memmove's whole reason to exist: overlapping, both directions. */
    {
        char m[] = "0123456789";


        ftport_memmove( m + 2, m, 5 );
        check( memcmp( m, "0101234789", 10 ) == 0, "memmove forward overlap" );
        strcpy( m, "0123456789" );
        ftport_memmove( m, m + 2, 5 );
        check( memcmp( m, "2345656789", 10 ) == 0, "memmove backward overlap" );
    }

    /* The sort: an adversarial input (already reversed) is the case a naive
     * quicksort degrades on, and heapsort does not care. Correctness is what
     * is checked; the bound is why heapsort was chosen. */
    for ( int i = 0; i < 64; i++ )
        values[i] = 63 - i;
    ftport_qsort( values, 64, sizeof( values[0] ), compare_int );
    for ( int i = 0; i < 64; i++ )
        if ( values[i] != i )
            sorted = 0;
    check( sorted, "qsort did not sort a reversed sequence" );

    ftport_qsort( values, 0, sizeof( values[0] ), compare_int );
    ftport_qsort( values, 1, sizeof( values[0] ), compare_int );
    check( values[0] == 0, "qsort disturbed a trivial input" );
}


/* ── what it costs ─────────────────────────────────────────────────────── */
/*
 * Not a pass/fail: a table, so F2 can size a glyph cache against measured
 * numbers instead of a guess. Each row is ONE face open at one size, every
 * printable ASCII glyph rendered and released, then the face closed — which
 * is the shape of a first paint. `peak` is what the engine held at its worst
 * moment, INCLUDING the adapter's own bookkeeping and the 16-byte header on
 * every block; it EXCLUDES the font file, which the caller owns.
 */

static void report_costs( void )
{
    static const uint32_t sizes[] = { 16, 24, 32 };

    printf( "\n%-34s %5s %10s %10s %8s\n",
            "cost: face @ size", "px", "peak bytes", "allocations", "held" );

    for ( size_t f = 0; f < sizeof( fixtures ) / sizeof( fixtures[0] ); f++ )
    {
        for ( size_t s = 0; s < sizeof( sizes ) / sizeof( sizes[0] ); s++ )
        {
            test_alloc                 a;
            os64_font_engine_options_t options;
            os64_font_engine_t        *engine = NULL;
            os64_font_face_t          *face;
            size_t                     held;


            alloc_init( &a );
            options = options_for( &a, 0 );
            if ( ft->engine_create( &options, &engine ) != OS64_FONT_OK )
                continue;

            face = open_fixture( engine, &fixtures[f], sizes[s],
                                 OS64_FONT_HINT_NORMAL );
            if ( face == NULL )
            {
                ft->engine_destroy( engine );
                continue;
            }

            /* Every printable ASCII glyph, rendered and released one at a
             * time — a cache that keeps them all would hold the sum of the
             * masks on top of this. */
            for ( uint32_t scalar = 32; scalar < 127; scalar++ )
            {
                uint32_t           index = 0;
                os64_font_glyph_t *glyph = NULL;


                if ( ft->lookup( face, scalar, &index ) != OS64_FONT_OK )
                    continue;
                if ( ft->render( face, index, &glyph ) == OS64_FONT_OK )
                    ft->glyph_release( glyph );
            }

            held = a.live;      /* what an open face costs while it is open */
            ft->face_close( face );
            ft->engine_destroy( engine );

            printf( "%-34s %5u %10zu %10ld %8zu\n",
                    fixtures[f].label, sizes[s], a.peak, a.allocations, held );
        }
    }
    printf( "\n" );
}


/* ── main ──────────────────────────────────────────────────────────────── */


/* Review fixtures replace one table in a pinned face; they do not use the
 * production decoder to calculate expected names or character-map choices. */
static void put_be16( uint8_t *p, uint16_t v )
{
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

static void put_be32( uint8_t *p, uint32_t v )
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static uint8_t *replace_table( fixture *source, const char *tag,
                               const uint8_t *table, size_t size, size_t *length )
{
    size_t offset = (source->length + 3) & ~(size_t)3;
    uint8_t *copy = calloc( 1, offset + size );
    if ( copy == NULL ) return NULL;
    memcpy( copy, source->bytes, source->length );
    for ( uint16_t i = 0; i < read_be16(copy + 4); i++ )
    {
        uint8_t *entry = copy + 12 + (size_t)i * 16;
        if ( memcmp(entry, tag, 4) != 0 ) continue;
        put_be32(entry + 4, 0);
        put_be32(entry + 8, (uint32_t)offset);
        put_be32(entry + 12, (uint32_t)size);
        memcpy(copy + offset, table, size);
        *length = offset + size;
        return copy;
    }
    free(copy);
    return NULL;
}

typedef struct {
    uint16_t platform, encoding, language, id;
    const uint8_t *bytes;
    uint16_t length;
} name_record;

static uint8_t *named_font( const name_record *records, size_t count, size_t *length )
{
    size_t size = 6 + 12 * count, at = size;
    for (size_t i = 0; i < count; i++) size += records[i].length;
    uint8_t *table = calloc(1, size);
    if (!table) return NULL;
    put_be16(table + 2, (uint16_t)count);
    put_be16(table + 4, (uint16_t)at);
    for (size_t i = 0; i < count; i++) {
        uint8_t *entry = table + 6 + i * 12;
        put_be16(entry, records[i].platform);
        put_be16(entry + 2, records[i].encoding);
        put_be16(entry + 4, records[i].language);
        put_be16(entry + 6, records[i].id);
        put_be16(entry + 8, records[i].length);
        put_be16(entry + 10, (uint16_t)(at - (6 + 12 * count)));
        memcpy(table + at, records[i].bytes, records[i].length);
        at += records[i].length;
    }
    uint8_t *copy = replace_table(SANS, "name", table, size, length);
    free(table);
    return copy;
}

static void check_names( const name_record *records, size_t count,
                          const char *family, const char *style, int truncated )
{
    size_t length;
    uint8_t *copy = named_font(records, count, &length);
    test_alloc a;
    alloc_init(&a);
    os64_font_engine_options_t options = options_for(&a, 0);
    os64_font_face_options_t fo = {16, OS64_FONT_HINT_NORMAL};
    os64_font_engine_t *engine = NULL;
    os64_font_face_t *face = NULL;
    os64_font_face_info_t info;
    if (!copy || ft->engine_create(&options, &engine) != OS64_FONT_OK ||
        ft->face_open(engine, copy, length, &fo, &face) != OS64_FONT_OK) {
        check(0, "synthetic name font must open");
    } else {
        check(ft->face_info(face, &info) == OS64_FONT_OK, "name metadata read");
        check(strcmp(info.family, family) == 0, "family '%s', expected '%s'", info.family, family);
        check(strcmp(info.style, style) == 0, "style '%s', expected '%s'", info.style, style);
        check(!!(info.flags & OS64_FONT_FACE_NAME_TRUNCATED) == truncated, "name truncation flag");
    }
    ft->face_close(face);
    check(ft->engine_destroy(engine) == OS64_FONT_OK && a.live == 0 && !a.size_mismatches,
          "name fixture cleanup");
    free(copy);
}

static void test_name_review( void )
{
    const uint8_t pair[] = {0xd8,0x3d,0xde,0x00,0x00,0xe9};
    const uint8_t unpaired[] = {0xd8,0x00,0,0x41,0xdc,0x00};
    const uint8_t odd[] = {0,0x41,0xff};
    const uint8_t nul_odd[] = {0,0x41,0,0,0xff};
    const uint8_t label_a[] = {0,'A'}, label_b[] = {0,'B'}, label_c[] = {0,'C'};
    name_record names[] = {{3,1,0x409,16,pair,sizeof(pair)}, {3,1,0x409,17,label_b,sizeof(label_b)}};
    begin("names/Unicode-boundaries");
    check_names(names, 2, "\xf0\x9f\x98\x80\xc3\xa9", "B", 0);
    names[0].bytes=unpaired; names[0].length=sizeof(unpaired);
    check_names(names, 2, "\xef\xbf\xbd" "A" "\xef\xbf\xbd", "B", 0);
    names[0].bytes=odd; names[0].length=sizeof(odd);
    check_names(names, 2, "A\xef\xbf\xbd", "B", 0);
    names[0].bytes=nul_odd; names[0].length=sizeof(nul_odd);
    check_names(names, 2, "A", "B", 0);
    uint8_t long_name[260];
    char expected[128];
    for (size_t i=0;i<126;i++) {long_name[2*i]=0;long_name[2*i+1]='a';expected[i]='a';}
    expected[126]=0; long_name[252]=0;long_name[253]=0xe9;
    names[0].bytes=long_name;names[0].length=254;
    check_names(names, 2, expected, "B", 1);
    for (size_t i=0;i<123;i++) expected[i]='a';
    memcpy(expected+123,"\xf0\x9f\x98\x80",5);
    memcpy(long_name+246,pair,4);names[0].length=250;
    check_names(names, 2, expected, "B", 0);
    /* Equal scores choose first; preferred IDs outrank platform preferences;
     * family and style choose independently. */
    begin("names/preference-and-duplicates");
    name_record order[] = {{3,1,0x409,1,label_c,2}, {0,3,0,16,label_a,2},
        {0,3,0,16,label_b,2}, {3,1,0x409,2,label_b,2}};
    check_names(order,4,"A","B",0);
    order[2].platform=3;order[2].encoding=1;order[2].language=0x409;
    check_names(order,4,"B","B",0);
    order[1].platform=3;order[1].encoding=1;order[1].language=0x409;
    check_names(order,4,"A","B",0);
    order[1].language=0x407;
    check_names(order,4,"B","B",0);

    /* Name strings can be loaded lazily after FreeType opened the face. A
     * callback refusal must not become a successful face with missing labels. */
    begin("names/allocation-failure");
    size_t length;
    uint8_t *copy=named_font(order,4,&length);
    check(copy!=NULL,"name denial fixture allocation");
    if (!copy) return;
    long cost=0;
    for (long failure=-1; failure<cost; failure++) {
        test_alloc a; alloc_init(&a);
        os64_font_engine_options_t options=options_for(&a,0);
        os64_font_engine_t *engine=NULL;os64_font_face_t *face=NULL;
        os64_font_face_options_t fo={16,OS64_FONT_HINT_NORMAL};
        if (ft->engine_create(&options,&engine)!=OS64_FONT_OK) {check(0,"name denial engine");break;}
        long start=a.allocations;
        if (failure>=0) a.fail_at=start+failure;
        os64_font_status_t status=ft->face_open(engine,copy,length,&fo,&face);
        if (failure<0) {cost=a.allocations-start;check(status==OS64_FONT_OK,"name census");}
        else if (a.allocations>a.fail_at)
            check(status==OS64_FONT_NO_MEMORY && !face,"name allocation %ld returned %s",failure,status_name(status));
        ft->face_close(face);
        check(ft->engine_destroy(engine)==OS64_FONT_OK && a.live==0 && !a.size_mismatches,"name denial cleanup %ld",failure);
    }
    free(copy);
}

static void test_unicode_map_review( void )
{
    /* BMP first, then each supported full-repertoire format. The synthetic
     * maps point U+10000 at glyph 1, independently of its artwork. */
    const unsigned formats[]={8,10,12,13};
    for (size_t f=0;f<sizeof(formats)/sizeof(formats[0]);f++)
    {
        uint8_t cmap[8260]={0};
        unsigned format=formats[f];
        size_t map_size=format==8 ? 8220 : format==10 ? 22 : 28;
        put_be16(cmap+2,2);
        put_be16(cmap+4,0);put_be16(cmap+6,3);put_be32(cmap+8,20);
        put_be16(cmap+12,0);put_be16(cmap+14,4);put_be32(cmap+16,40);
        put_be16(cmap+20,6);put_be16(cmap+22,20);put_be16(cmap+26,0x41);
        put_be16(cmap+28,1);put_be16(cmap+30,1);
        uint8_t *map=cmap+40;
        put_be16(map,format);put_be32(map+4,map_size);
        if (format==10)
        {
            put_be32(map+12,0x10000);put_be32(map+16,1);put_be16(map+20,1);
        }
        else
        {
            size_t at=format==8 ? 8204 : 12;
            if (format==8) map[12]=0xC0; /* high and low halves marked 32-bit */
            put_be32(map+at,1);put_be32(map+at+4,0x10000);
            put_be32(map+at+8,0x10000);put_be32(map+at+12,1);
        }
        size_t length;uint8_t *copy=replace_table(SANS,"cmap",cmap,40+map_size,&length);
        begin("cmap/full-repertoire-before-platform");
        test_alloc a;alloc_init(&a);os64_font_engine_options_t options=options_for(&a,0);
        os64_font_engine_t *engine=NULL;os64_font_face_t *face=NULL;
        os64_font_face_options_t fo={16,OS64_FONT_HINT_NORMAL};uint32_t index=0;
        if (!copy || ft->engine_create(&options,&engine)!=OS64_FONT_OK ||
            ft->face_open(engine,copy,length,&fo,&face)!=OS64_FONT_OK) check(0,"two-map font open");
        else check(ft->lookup(face,0x10000,&index)==OS64_FONT_OK && index==1,
                   "full-repertoire format %u lost U+10000",format);
        ft->face_close(face);check(ft->engine_destroy(engine)==OS64_FONT_OK && !a.live,"map cleanup");free(copy);
    }
}

static void test_bare_cff2_review( void )
{
    /* CFF2 header and minimal index scaffold, without an SFNT directory.
     * The raw-CFF loader rejects major version 2 before parsing the body. */
    const uint8_t cff2[]={2,0,5,0,2,150,17,0,0,0,0,0,0,0,1,1,1,1};
    begin("formats/bare-CFF2");
    test_alloc a;alloc_init(&a);os64_font_engine_options_t options=options_for(&a,0);
    os64_font_engine_t *engine=NULL;os64_font_face_t *face=NULL;
    os64_font_face_options_t fo={16,OS64_FONT_HINT_NORMAL};
    if (ft->engine_create(&options,&engine)!=OS64_FONT_OK) {check(0,"CFF2 engine");return;}
    os64_font_status_t status=ft->face_open(engine,cff2,sizeof(cff2),&fo,&face);
    check(status==OS64_FONT_UNSUPPORTED && !face,"bare CFF2 returned %s",status_name(status));
    ft->face_close(face);check(ft->engine_destroy(engine)==OS64_FONT_OK && !a.live,"CFF2 cleanup");
}

static void test_bitmap_flow_review( void )
{
    begin("render/negative-pitch");
    test_alloc a; alloc_init(&a);
    os64_font_engine_options_t options=options_for(&a,0);
    os64_font_engine_t *engine=NULL; os64_font_face_t *face=NULL;
    os64_font_glyph_t *forward=NULL, *reverse=NULL;
    os64_font_glyph_view_t fv,rv;
    os64_font_face_options_t fo={24,OS64_FONT_HINT_NORMAL};
    uint32_t index=0;
    if (ft->engine_create(&options,&engine)!=OS64_FONT_OK) {check(0,"flow engine");return;}
    if (ft->face_open(engine,fixtures[0].bytes,fixtures[0].length,&fo,&face)!=OS64_FONT_OK)
        check(0,"flow face");
    else
    {
        check(ft->lookup(face,'R',&index)==OS64_FONT_OK,"flow lookup");
        check(ft->render(face,index,&forward)==OS64_FONT_OK,"forward render");
        reverse_bitmap_rows=1;
        check(ft->render(face,index,&reverse)==OS64_FONT_OK,"reverse render");
        reverse_bitmap_rows=0;
        if (ft->glyph_view(forward,&fv)==OS64_FONT_OK && ft->glyph_view(reverse,&rv)==OS64_FONT_OK)
            check(fv.width==rv.width && fv.height==rv.height &&
                  memcmp(fv.coverage,rv.coverage,(size_t)fv.width*fv.height)==0,
                  "negative pitch changed top-down mask contents");
        else check(0,"flow views");
    }
    ft->glyph_release(forward); ft->glyph_release(reverse); ft->face_close(face);
    check(ft->engine_destroy(engine)==OS64_FONT_OK && !a.live,"flow cleanup");
}

static void test_container_review( void )
{
    begin("formats/container-boundary");
    test_alloc a;alloc_init(&a);os64_font_engine_options_t options=options_for(&a,0);
    os64_font_engine_t *engine=NULL;os64_font_face_t *face=NULL;
    os64_font_face_options_t fo={16,OS64_FONT_HINT_NORMAL};
    if (ft->engine_create(&options,&engine)!=OS64_FONT_OK) {check(0,"container engine");return;}
    size_t length=SANS->length+16;uint8_t *ttc=calloc(1,length);
    if (!ttc) {check(0,"TTC allocation");ft->engine_destroy(engine);return;}
    memcpy(ttc,"ttcf",4);put_be32(ttc+4,0x10000);put_be32(ttc+8,1);put_be32(ttc+12,16);
    memcpy(ttc+16,SANS->bytes,SANS->length);
    for (uint16_t i=0;i<read_be16(ttc+20);i++) {
        uint8_t *entry=ttc+28+(size_t)i*16;
        put_be32(entry+8,read_be32(entry+8)+16);
    }
    os64_font_status_t status=ft->face_open(engine,ttc,length,&fo,&face);
    check(status==OS64_FONT_UNSUPPORTED && !face,"one-face collection returned %s",status_name(status));
    ft->face_close(face);face=NULL;free(ttc);
    uint32_t cff_length=0;
    uint32_t offset=find_table(CFF_PROP->bytes,CFF_PROP->length,"CFF ",&cff_length);
    check(offset && (size_t)offset+cff_length<=CFF_PROP->length,"CFF extraction bounds");
    if (offset && (size_t)offset+cff_length<=CFF_PROP->length) {
        status=ft->face_open(engine,CFF_PROP->bytes+offset,cff_length,&fo,&face);
        check(status==OS64_FONT_UNSUPPORTED && !face,"bare CFF1 outside SFNT profile returned %s",status_name(status));
        ft->face_close(face);
    }
    check(ft->engine_destroy(engine)==OS64_FONT_OK && !a.live,"container cleanup");
}

int main( int argc, char **argv )
{
    const char *dir = argc > 1 ? argv[1] : "userland/libfreetype/fixtures";

    ft = os64_freetype_backend_v1();
    if ( ft == NULL )
    {
        fprintf( stderr, "os64_freetype_backend_v1() returned NULL\n" );
        return 1;
    }

    if ( !load_fixture( SANS, dir, "DejaVuSans.ttf", "DejaVu Sans (TTF)", 0 ) ||
         !load_fixture( SANS_MONO, dir, "DejaVuSansMono.ttf",
                        "DejaVu Sans Mono (TTF)", 1 ) ||
         !load_fixture( CFF_PROP, dir, "SourceSans3-Regular.otf",
                        "Source Sans 3 (CFF)", 0 ) ||
         !load_fixture( CFF_MONO, dir, "SourceCodePro-Regular.otf",
                        "Source Code Pro (CFF)", 1 ) )
        return 1;

    test_runtime();
    test_nonlocal_return();

    test_engine_bounds();
    test_face_open_bounds();

    for ( size_t f = 0; f < sizeof( fixtures ) / sizeof( fixtures[0] ); f++ )
    {
        test_face_shape( &fixtures[f], 16 );
        test_face_shape( &fixtures[f], 24 );
        test_face_shape( &fixtures[f], 32 );
        test_render( &fixtures[f], 16 );
        test_render( &fixtures[f], 32 );
    }

    test_lookup();
    test_kerning();
    test_glyph_edges();
    test_independent_contexts();
    test_face_ceiling();
    test_repeated_open();
    test_truncation();
    test_charmap_validator();
    test_allocation_denial();
    test_budget_status();
    test_name_review();
    test_unicode_map_review();
    test_bare_cff2_review();
    test_container_review();
    test_bitmap_flow_review();

    report_costs();

    for ( size_t f = 0; f < sizeof( fixtures ) / sizeof( fixtures[0] ); f++ )
    {
        free( fixtures[f].bytes );
        free( (void *)fixtures[f].path );
    }

    printf( "%d checks, %d failures\n", checks, failures );
    return failures == 0 ? 0 : 1;
}
