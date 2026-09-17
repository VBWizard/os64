/* backend.c — os64's font backend, implemented on the pinned FreeType.
 *
 * This is the whole of what leaves the library: `os64_freetype_backend_v1()`
 * hands back a table of function pointers and nothing else is exported, so no
 * FreeType type, constant or version-specific struct reaches a consumer. The
 * contract those functions implement is F0's — `os64/font_backend.h` for the
 * declarations, FONT_CONTRACTS.md for the semantics — and this file's job is
 * to make FreeType answer to it, including where the two disagree about who
 * owns what.
 *
 * THE THREE PLACES FREETYPE AND THE CONTRACT DISAGREE, because each shapes
 * code here that would otherwise look like needless work:
 *
 *   FREE WITH A SIZE. The contract's `free` callback is handed the size that
 *   was requested; FreeType's is handed a pointer alone. So every block
 *   carries a header with its own size (`block_header`), and the size the
 *   caller sees on the way out is the one it saw on the way in.
 *
 *   OWNED MASKS. FreeType renders into ONE glyph slot per face and overwrites
 *   it on the next load; the contract promises a glyph that outlives its face
 *   and is released on its own schedule. So a render COPIES the coverage into
 *   a block of its own. That is the whole difference between a slot and a
 *   cache entry, and it is why the contract can say what it says.
 *
 *   UTF-8 NAMES. FreeType flattens a face's name-table strings to ASCII,
 *   turning every character it cannot spell into a literal `?`. The contract
 *   asks for UTF-8 with U+FFFD for what cannot be decoded, which a settings
 *   application can show. So the names are read from the name table directly
 *   (`face_name`) and FreeType's flattened copy is only the fallback.
 *
 * No path here opens a file, touches a surface, or makes a system call.
 */

#include <os64/font_backend.h>

#include <ft2build.h>
#include <freetype/freetype.h>
#include <freetype/ftsystem.h>
#include <freetype/ftmodapi.h>
#include <freetype/ftsnames.h>
#include <freetype/tttables.h>
#include <freetype/tttags.h>
#include <freetype/ttnameid.h>

/* 26.6: the unit the contract measures in. Spelled here so the conversions
 * below read as arithmetic rather than as a magic 64. */
#define FIXED_ONE  ( (os64_font_pos_t)OS64_FONT_UNIT )


/* ── the engine ────────────────────────────────────────────────────────── */

struct os64_font_engine
{
    os64_font_memory_t  memory;      /* the caller's, copied at create */
    size_t              cap;
    size_t              live_bytes;
    size_t              peak_bytes;
    uint32_t            live_faces;
    uint32_t            live_glyphs;
    os64_font_status_t  allocation_failure; /* reset before allocating operations */

    /* Lives HERE, not on a stack or in FreeType: the library holds this by
     * pointer for its whole life. Upstream gives the struct no typedef of
     * its own — only `FT_Memory`, which is a pointer to it. */
    struct FT_MemoryRec_  ft_memory;
    FT_Library          library;
};

struct os64_font_face
{
    os64_font_engine_t     *engine;
    FT_Face                 ft;
    FT_Int32                load_flags;
    FT_Kerning_Mode         kern_mode;
    os64_font_face_info_t   info;    /* answered from here; open computed it */
};

struct os64_font_glyph
{
    os64_font_engine_t  *engine;
    uint8_t             *coverage;   /* NULL for a zero-area glyph */
    size_t               coverage_bytes;
    uint32_t             width, height, stride;
    int32_t              left, top;
    os64_font_pos_t      advance_x;
    os64_font_rect_t     ink;
};


/* ── allocation, accounting and the block header ───────────────────────── */
/*
 * Every byte FreeType or this adapter asks for passes through here, which is
 * what makes `engine_stats` a fact rather than an estimate, and what makes the
 * memory cap enforceable at all. The header costs 16 bytes per block: enough
 * to record the size the caller must be given back at free, and exactly the
 * alignment `max_align_t` wants on x86-64, so a caller that returns aligned
 * storage still hands aligned storage to FreeType.
 */

typedef struct
{
    size_t    size;      /* the total this block cost, header included */
    uint64_t  padding;   /* keeps the payload 16-byte aligned */
} block_header;

#define BLOCK_OVERHEAD  ( sizeof( block_header ) )

_Static_assert( BLOCK_OVERHEAD == 16,
                "the block header must keep the payload max_align_t-aligned" );


static void *engine_alloc( os64_font_engine_t *engine, size_t bytes )
{
    size_t        total;
    block_header *header;

    if ( bytes == 0 || bytes > SIZE_MAX - BLOCK_OVERHEAD )
    {
        engine->allocation_failure = OS64_FONT_LIMIT;
        return NULL;
    }

    total = bytes + BLOCK_OVERHEAD;

    /* Record cap refusals separately: FreeType collapses both failure sources
     * into Out_Of_Memory, but callers need LIMIT to recover cache space. */
    if ( total > engine->cap || engine->live_bytes > engine->cap - total )
    {
        engine->allocation_failure = OS64_FONT_LIMIT;
        return NULL;
    }

    header = engine->memory.alloc( engine->memory.context, total );
    if ( header == NULL )
    {
        engine->allocation_failure = OS64_FONT_NO_MEMORY;
        return NULL;
    }

    header->size = total;
    engine->live_bytes += total;
    if ( engine->live_bytes > engine->peak_bytes )
        engine->peak_bytes = engine->live_bytes;

    return (uint8_t *)header + BLOCK_OVERHEAD;
}


static void engine_free( os64_font_engine_t *engine, void *payload )
{
    block_header *header;
    size_t        total;

    if ( payload == NULL )
        return;

    header = (block_header *)( (uint8_t *)payload - BLOCK_OVERHEAD );
    total  = header->size;

    engine->live_bytes -= total;
    engine->memory.free( engine->memory.context, header, total );
}


static size_t engine_block_size( const void *payload )
{
    const block_header *header =
        (const block_header *)( (const uint8_t *)payload - BLOCK_OVERHEAD );

    return header->size - BLOCK_OVERHEAD;
}


/* FreeType's three memory callbacks. `memory->user` is the engine, which is
 * how per-engine accounting survives a library that has no other way to carry
 * context. */

static void *ft_alloc( FT_Memory memory, long size )
{
    if ( size <= 0 )
        return NULL;
    return engine_alloc( (os64_font_engine_t *)memory->user, (size_t)size );
}


static void ft_free( FT_Memory memory, void *block )
{
    engine_free( (os64_font_engine_t *)memory->user, block );
}


static void *ft_realloc( FT_Memory memory, long cur_size, long new_size,
                         void *block )
{
    os64_font_engine_t *engine = (os64_font_engine_t *)memory->user;
    void               *fresh;
    size_t              keep;

    (void)cur_size;   /* the block's own header is the authority on its size */

    if ( new_size <= 0 )
    {
        engine_free( engine, block );
        return NULL;
    }
    if ( block == NULL )
        return engine_alloc( engine, (size_t)new_size );

    fresh = engine_alloc( engine, (size_t)new_size );
    if ( fresh == NULL )
        return NULL;      /* THE OLD BLOCK IS STILL THE CALLER'S, untouched */

    keep = engine_block_size( block );
    if ( keep > (size_t)new_size )
        keep = (size_t)new_size;

    __builtin_memcpy( fresh, block, keep );
    engine_free( engine, block );
    return fresh;
}


/* ── translating FreeType's failures ───────────────────────────────────── */
/*
 * The distinction the contract cares about is WHOSE fault it is, and
 * FreeType's error space answers that if it is read carefully. A format no
 * driver claims is something os64 chose not to support; a format a driver
 * claimed and then could not parse is a broken file.
 */

static os64_font_status_t status_for( const os64_font_engine_t *engine,
                                       FT_Error error )
{
    switch ( error )
    {
    case FT_Err_Ok:
        return OS64_FONT_OK;

    case FT_Err_Out_Of_Memory:
        return engine->allocation_failure == OS64_FONT_LIMIT
                   ? OS64_FONT_LIMIT : OS64_FONT_NO_MEMORY;

    case FT_Err_Unknown_File_Format:
    case FT_Err_Unimplemented_Feature:
    case FT_Err_Invalid_Pixel_Size:
        return OS64_FONT_UNSUPPORTED;

    case FT_Err_Invalid_Argument:
    case FT_Err_Invalid_Face_Handle:
    case FT_Err_Invalid_Size_Handle:
    case FT_Err_Invalid_Slot_Handle:
    case FT_Err_Invalid_Library_Handle:
        return OS64_FONT_BAD_ARGUMENT;

    case FT_Err_Invalid_Glyph_Index:
    case FT_Err_Invalid_Character_Code:
        return OS64_FONT_MISSING;

    case FT_Err_Cannot_Open_Resource:
    case FT_Err_Invalid_File_Format:
    case FT_Err_Invalid_Version:
    case FT_Err_Invalid_Table:
    case FT_Err_Invalid_Offset:
    case FT_Err_Table_Missing:
    case FT_Err_Invalid_Stream_Operation:
    case FT_Err_Invalid_Stream_Seek:
    case FT_Err_Invalid_Stream_Skip:
    case FT_Err_Invalid_Stream_Read:
    case FT_Err_Invalid_Frame_Operation:
    case FT_Err_Invalid_Frame_Read:
    case FT_Err_Invalid_Composite:
    case FT_Err_Invalid_Outline:
    case FT_Err_Too_Many_Hints:
        return OS64_FONT_MALFORMED;

    default:
        /* Deliberately not a catch-all for the cases above: a status of
         * ENGINE_ERROR means "upstream refused and os64 has no better word
         * for it", which is a lead rather than a shrug. */
        return OS64_FONT_ENGINE_ERROR;
    }
}


/* ── names ─────────────────────────────────────────────────────────────── */
/*
 * Read the face's own name table rather than FreeType's ASCII flattening of
 * it, so a family called "Kožená" reaches a settings list as its own name
 * instead of as "Ko?en?". Only Unicode-encoded entries are decoded; a face
 * whose names exist solely in a legacy platform encoding falls back to
 * FreeType's copy, which is ASCII and honest about it.
 */

#define REPLACEMENT  0xFFFDu     /* U+FFFD, what undecodable input becomes */

static int name_is_unicode( const FT_SfntName *name )
{
    if ( name->platform_id == TT_PLATFORM_APPLE_UNICODE )
        return 1;
    if ( name->platform_id == TT_PLATFORM_MICROSOFT )
        return name->encoding_id == TT_MS_ID_UNICODE_CS ||
               name->encoding_id == TT_MS_ID_UCS_4;
    return 0;
}


/* Append one scalar as UTF-8. Returns 0 when it did not fit, which is how
 * truncation stays on a scalar boundary: a partial sequence is never
 * written, so the result is always decodable. */
static int utf8_append( char *out, size_t cap, size_t *used, uint32_t scalar )
{
    size_t need = scalar < 0x80u     ? 1
                : scalar < 0x800u    ? 2
                : scalar < 0x10000u  ? 3
                :                      4;

    if ( *used + need + 1 > cap )      /* +1: the NUL always has its place */
        return 0;

    switch ( need )
    {
    case 1:
        out[( *used )++] = (char)scalar;
        break;
    case 2:
        out[( *used )++] = (char)( 0xC0u | ( scalar >> 6 ) );
        out[( *used )++] = (char)( 0x80u | ( scalar & 0x3Fu ) );
        break;
    case 3:
        out[( *used )++] = (char)( 0xE0u | ( scalar >> 12 ) );
        out[( *used )++] = (char)( 0x80u | ( ( scalar >> 6 ) & 0x3Fu ) );
        out[( *used )++] = (char)( 0x80u | ( scalar & 0x3Fu ) );
        break;
    default:
        out[( *used )++] = (char)( 0xF0u | ( scalar >> 18 ) );
        out[( *used )++] = (char)( 0x80u | ( ( scalar >> 12 ) & 0x3Fu ) );
        out[( *used )++] = (char)( 0x80u | ( ( scalar >> 6 ) & 0x3Fu ) );
        out[( *used )++] = (char)( 0x80u | ( scalar & 0x3Fu ) );
        break;
    }
    return 1;
}


/* UTF-16BE (the name table's encoding) to UTF-8. An unpaired surrogate is
 * data somebody wrote wrong, not a reason to refuse the font, so it becomes
 * U+FFFD and the walk continues. */
static int utf16be_to_utf8( const FT_Byte *in, FT_UInt in_len,
                            char *out, size_t cap )
{
    size_t  used      = 0;
    int     truncated = 0;
    FT_UInt i         = 0;

    while ( i + 1 < in_len )
    {
        uint32_t unit = (uint32_t)( in[i] << 8 | in[i + 1] );
        uint32_t scalar;


        i += 2;

        if ( unit >= 0xD800u && unit <= 0xDBFFu )
        {
            uint32_t low = ( i + 1 < in_len )
                         ? (uint32_t)( in[i] << 8 | in[i + 1] )
                         : 0;


            if ( low >= 0xDC00u && low <= 0xDFFFu )
            {
                scalar = 0x10000u + ( ( unit - 0xD800u ) << 10 )
                                  + ( low - 0xDC00u );
                i += 2;
            }
            else
                scalar = REPLACEMENT;
        }
        else if ( unit >= 0xDC00u && unit <= 0xDFFFu )
            scalar = REPLACEMENT;
        else
            scalar = unit;

        if ( scalar == 0 )             /* an embedded NUL ends the string */
        {
            out[used] = '\0';
            return 0;
        }

        if ( !utf8_append( out, cap, &used, scalar ) )
        {
            truncated = 1;
            break;
        }
    }

    /* An odd trailing byte is a malformed entry; say so rather than dropping
     * it silently. */
    if ( !truncated && i + 1 == in_len && in_len % 2 == 1 )
    {
        if ( !utf8_append( out, cap, &used, REPLACEMENT ) )
            truncated = 1;
    }

    out[used] = '\0';
    return truncated;
}


/* ASCII already IS UTF-8, so FreeType's flattened name needs copying and
 * nothing else — except for the bytes above 0x7F it should never produce.
 * Those become U+FFFD rather than an invalid UTF-8 sequence. */
static int ascii_to_utf8( const char *in, char *out, size_t cap )
{
    size_t used = 0;

    for ( ; in != NULL && *in != '\0'; in++ )
    {
        unsigned char c = (unsigned char)*in;


        if ( !utf8_append( out, cap, &used,
                           c < 0x80u ? (uint32_t)c : REPLACEMENT ) )
        {
            out[used] = '\0';
            return 1;
        }
    }
    out[used] = '\0';
    return 0;
}


/* Score a candidate name entry. Higher wins; zero means "not usable". The
 * shape of the preference is upstream's own: a typographic family name (16)
 * describes the family a person means, while the legacy family name (1) was
 * bent for four-style menus in 1990s applications. Microsoft en-US and
 * Unicode-platform language zero are deterministic preferences, not an
 * inference about a font designer's language or the encoding of its label. */
static unsigned name_score( const FT_SfntName *name,
                            FT_UShort preferred, FT_UShort legacy )
{
    unsigned score;

    if ( !name_is_unicode( name ) || name->string == NULL ||
         name->string_len == 0 )
        return 0;

    if ( name->name_id == preferred )
        score = 200;
    else if ( name->name_id == legacy )
        score = 100;
    else
        return 0;

    if ( name->platform_id == TT_PLATFORM_MICROSOFT )
        score += 20;

    if ( ( name->platform_id == TT_PLATFORM_MICROSOFT &&
           name->language_id == TT_MS_LANGID_ENGLISH_UNITED_STATES ) ||
         ( name->platform_id == TT_PLATFORM_APPLE_UNICODE &&
           name->language_id == TT_MAC_LANGID_ENGLISH ) )
        score += 10;

    return score;
}


static int face_name( FT_Face ft, FT_UShort preferred, FT_UShort legacy,
                      const char *fallback, char *out, size_t cap )
{
    FT_SfntName best       = { 0, 0, 0, 0, NULL, 0 };
    unsigned    best_score = 0;
    FT_UInt     count      = FT_Get_Sfnt_Name_Count( ft );

    for ( FT_UInt i = 0; i < count; i++ )
    {
        FT_SfntName name;
        unsigned    score;


        if ( FT_Get_Sfnt_Name( ft, i, &name ) != FT_Err_Ok )
            continue;

        score = name_score( &name, preferred, legacy );
        if ( score > best_score )
        {
            best_score = score;
            best       = name;
        }
    }

    if ( best_score > 0 )
        return utf16be_to_utf8( best.string, best.string_len, out, cap );

    /* No Unicode name entry. FreeType's flattened copy is all there is, and
     * an absent name stays empty so a settings list can show the file name
     * instead of inventing one. */
    return ascii_to_utf8( fallback, out, cap );
}


/* ── charmap selection ─────────────────────────────────────────────────── */
/* Full-repertoire formats outrank BMP maps regardless of their platform.
 * Unicode-platform IDs also describe BMP and variation-selector maps, so
 * platform alone cannot establish coverage or selectability. */

static int select_unicode_charmap( FT_Face ft )
{
    FT_CharMap best     = NULL;
    unsigned   best_rank = 0;

    for ( FT_Int i = 0; i < ft->num_charmaps; i++ )
    {
        FT_CharMap map = ft->charmaps[i];
        unsigned   rank;


        if ( map->encoding != FT_ENCODING_UNICODE )
            continue;
        FT_Long format = FT_Get_CMap_Format( map );
        if ( format == 14 )             /* variation selectors, not a base map */
            continue;

        if ( map->platform_id == TT_PLATFORM_MICROSOFT &&
             map->encoding_id == TT_MS_ID_UCS_4 )
            rank = 40;
        else if ( map->platform_id == TT_PLATFORM_APPLE_UNICODE )
            rank = 30;
        else if ( map->platform_id == TT_PLATFORM_MICROSOFT &&
                  map->encoding_id == TT_MS_ID_UNICODE_CS )
            rank = 20;
        else
            rank = 10;

        if ( format == 8 || format == 10 || format == 12 || format == 13 )
            rank += 100;

        if ( rank > best_rank )
        {
            best_rank = rank;
            best      = map;
        }
    }

    if ( best == NULL )
        return 0;

    return FT_Set_Charmap( ft, best ) == FT_Err_Ok;
}


/* ── engine operations ─────────────────────────────────────────────────── */

static os64_font_status_t engine_create( const os64_font_engine_options_t *options,
                                         os64_font_engine_t **out )
{
    /* Zeroed whole: this is copied into the real engine below, so a field
     * left unset here would be read before it was ever written. */
    os64_font_engine_t  bootstrap = { 0 };
    os64_font_engine_t *engine;
    size_t              cap;
    FT_Error            error;

    if ( out == NULL )
        return OS64_FONT_BAD_ARGUMENT;
    *out = NULL;

    if ( options == NULL || options->memory.alloc == NULL ||
         options->memory.free == NULL )
        return OS64_FONT_BAD_ARGUMENT;

    cap = options->memory_cap;
    if ( cap == 0 )
        cap = OS64_FONT_MEMORY_DEFAULT;
    else if ( cap > OS64_FONT_MEMORY_MAX )
        return OS64_FONT_BAD_ARGUMENT;

    /* The engine's own block has to be accounted for by an engine that does
     * not exist yet, so accounting starts on the stack and moves in. */
    bootstrap.memory = options->memory;
    bootstrap.cap    = cap;

    engine = engine_alloc( &bootstrap, sizeof( *engine ) );
    if ( engine == NULL )
        return bootstrap.allocation_failure;

    *engine = bootstrap;

    engine->ft_memory.user    = engine;
    engine->ft_memory.alloc   = ft_alloc;
    engine->ft_memory.free    = ft_free;
    engine->ft_memory.realloc = ft_realloc;

    /* FT_New_Library rather than FT_Init_FreeType: the latter builds an
     * FT_Memory from a process-wide heap, which is the one thing this
     * backend must not have. The modules come from os64_ftmodule.h. */
    error = FT_New_Library( &engine->ft_memory, &engine->library );
    if ( error != FT_Err_Ok )
    {
        os64_font_status_t status = status_for( engine, error );
        engine_free( &bootstrap, engine );
        return status;
    }

    /* The void module installer continues after allocation failures. Reject
     * that incomplete engine instead of silently losing a driver or renderer. */
    FT_Add_Default_Modules( engine->library );
    if ( engine->allocation_failure != OS64_FONT_OK )
    {
        os64_font_status_t status = engine->allocation_failure;
        FT_Done_Library( engine->library );
        engine_free( &bootstrap, engine );
        return status;
    }

    *out = engine;
    return OS64_FONT_OK;
}


static os64_font_status_t engine_destroy( os64_font_engine_t *engine )
{
    os64_font_engine_t bootstrap;

    if ( engine == NULL )
        return OS64_FONT_OK;

    /* A live child holds a pointer into this engine's accounting, so tearing
     * the engine down under it would leave that child valid-looking and
     * lethal. Refusing changes nothing, which is what BUSY promises. */
    if ( engine->live_faces != 0 || engine->live_glyphs != 0 )
        return OS64_FONT_BUSY;

    FT_Done_Library( engine->library );

    /* The engine's own block is the last thing accounted for, so free it
     * through a copy: `engine_free` writes to the engine it is given. */
    bootstrap = *engine;
    engine_free( &bootstrap, engine );
    return OS64_FONT_OK;
}


static os64_font_status_t engine_stats( os64_font_engine_t *engine,
                                        os64_font_engine_stats_t *out )
{
    if ( out != NULL )
    {
        out->live_bytes  = 0;
        out->peak_bytes  = 0;
        out->live_faces  = 0;
        out->live_glyphs = 0;
    }
    if ( engine == NULL || out == NULL )
        return OS64_FONT_BAD_ARGUMENT;

    out->live_bytes  = engine->live_bytes;
    out->peak_bytes  = engine->peak_bytes;
    out->live_faces  = engine->live_faces;
    out->live_glyphs = engine->live_glyphs;
    return OS64_FONT_OK;
}


/* ── face operations ───────────────────────────────────────────────────── */

static int has_table( FT_Face ft, FT_ULong tag )
{
    FT_ULong length = 0;

    return FT_Load_Sfnt_Table( ft, tag, 0, NULL, &length ) == FT_Err_Ok;
}


static os64_font_status_t face_open( os64_font_engine_t *engine,
                                     const uint8_t *bytes, size_t length,
                                     const os64_font_face_options_t *options,
                                     os64_font_face_t **out )
{
    os64_font_face_t   *face;
    FT_Face             ft = NULL;
    FT_Error            error;
    os64_font_status_t  status;
    os64_font_pos_t     ascent, descent, line_height;
    int                 cut;

    if ( out == NULL )
        return OS64_FONT_BAD_ARGUMENT;
    *out = NULL;

    if ( engine == NULL || bytes == NULL || options == NULL )
        return OS64_FONT_BAD_ARGUMENT;
    if ( options->pixel_height == 0 ||
         options->pixel_height > OS64_FONT_PIXEL_MAX )
        return OS64_FONT_BAD_ARGUMENT;
    if ( options->hint != OS64_FONT_HINT_NORMAL &&
         options->hint != OS64_FONT_HINT_NONE )
        return OS64_FONT_BAD_ARGUMENT;
    if ( length == 0 )
        return OS64_FONT_BAD_ARGUMENT;

    /* Cheap refusals before any parse: a 40 MiB "font" should cost a
     * comparison, not a walk through somebody's tables. */
    if ( length > OS64_FONT_FILE_MAX )
        return OS64_FONT_LIMIT;
    if ( engine->live_faces >= OS64_FONT_FACE_MAX )
        return OS64_FONT_LIMIT;

    /* A one-face TTC is still a collection; face count is not its format. */
    if ( length >= 4 && __builtin_memcmp( bytes, "ttcf", 4 ) == 0 )
        return OS64_FONT_UNSUPPORTED;

    engine->allocation_failure = OS64_FONT_OK;
    face = engine_alloc( engine, sizeof( *face ) );
    if ( face == NULL )
        return engine->allocation_failure;

    error = FT_New_Memory_Face( engine->library, bytes, (FT_Long)length, 0, &ft );
    if ( error != FT_Err_Ok )
    {
        status = status_for( engine, error );
        goto fail;
    }

    /* The contract accepts static SFNT outlines. Bare CFF can synthesize a
     * Unicode map but is outside that container profile. Variation state is
     * not represented in face identity; bitmap-only faces have no outline
     * for the selected renderer. */
    if ( !FT_IS_SFNT( ft ) || ft->num_faces > 1 || has_table( ft, TTAG_fvar ) ||
         has_table( ft, TTAG_CFF2 ) || !FT_IS_SCALABLE( ft ) )
    {
        status = OS64_FONT_UNSUPPORTED;
        goto fail;
    }

    /* Without a Unicode map there is no way to answer the only lookup this
     * contract has. Falling back to a symbol map would answer a DIFFERENT
     * question with the same glyph indices. */
    if ( !select_unicode_charmap( ft ) )
    {
        status = OS64_FONT_UNSUPPORTED;
        goto fail;
    }

    error = FT_Set_Pixel_Sizes( ft, 0, options->pixel_height );
    if ( error != FT_Err_Ok )
    {
        status = status_for( engine, error );
        goto fail;
    }

    face->engine = engine;
    face->ft     = ft;

    /* FT_LOAD_NO_BITMAP even though embedded bitmap support is compiled out:
     * the flag says what this backend wants of a font, and it keeps saying it
     * if that option is ever turned back on. */
    if ( options->hint == OS64_FONT_HINT_NONE )
        face->load_flags = FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING |
                           FT_LOAD_NO_AUTOHINT | FT_LOAD_TARGET_NORMAL;
    else
        face->load_flags = FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_NORMAL;

    /* KERNING IS REPORTED UNROUNDED, in both hinting modes.
     *
     * `FT_KERNING_DEFAULT` would grid-fit it to whole pixels and, below
     * 25ppem, scale it down by a heuristic from the days when FreeType's
     * callers drew at integer positions. Both throw information away that
     * the caller cannot get back: at 32 pixels Source Sans 3 tucks "AV" by
     * about seven hundredths of an em, which rounds to nothing at all — and
     * a line of text whose every small kern rounded to zero is a line that
     * silently lost its spacing.
     *
     * 26.6 exists precisely so the fraction survives the trip. Where to round
     * is a decision about a SURFACE, and this backend does not have one. */
    face->kern_mode = FT_KERNING_UNFITTED;

    ascent  = (os64_font_pos_t)ft->size->metrics.ascender;
    descent = (os64_font_pos_t)-ft->size->metrics.descender;
    if ( ascent < 0 )
        ascent = 0;
    if ( descent < 0 )
        descent = 0;

    /* A face whose declared line height is less than its own ascent plus
     * descent would make consecutive lines overlap. The contract's floor is
     * applied here so no consumer has to know that some fonts do this. */
    line_height = (os64_font_pos_t)ft->size->metrics.height;
    if ( line_height < ascent + descent )
        line_height = ascent + descent;

    face->info.glyph_count = (uint32_t)( ft->num_glyphs < 0 ? 0 : ft->num_glyphs );
    face->info.flags       = 0;
    face->info.ascent      = ascent;
    face->info.descent     = descent;
    face->info.line_height = line_height;

    if ( FT_IS_FIXED_WIDTH( ft ) )
        face->info.flags |= OS64_FONT_FACE_FIXED_WIDTH;

    /* Both names are read before either verdict is used: a truncated style
     * must set the flag even when the family fitted. */
    cut  = face_name( ft, TT_NAME_ID_TYPOGRAPHIC_FAMILY, TT_NAME_ID_FONT_FAMILY,
                      ft->family_name, face->info.family,
                      sizeof( face->info.family ) );
    cut |= face_name( ft, TT_NAME_ID_TYPOGRAPHIC_SUBFAMILY,
                      TT_NAME_ID_FONT_SUBFAMILY, ft->style_name,
                      face->info.style, sizeof( face->info.style ) );
    if ( cut )
        face->info.flags |= OS64_FONT_FACE_NAME_TRUNCATED;

    /* Name strings are loaded lazily. FreeType can suppress a failed read
     * allocation and return an empty record, which is not a usable fallback
     * decision: callers must be able to retry after recovering memory. */
    if ( engine->allocation_failure != OS64_FONT_OK )
    {
        status = engine->allocation_failure;
        goto fail;
    }

    engine->live_faces++;
    *out = face;
    return OS64_FONT_OK;

fail:
    if ( ft != NULL )
        FT_Done_Face( ft );
    engine_free( engine, face );
    return status;
}


static void face_close( os64_font_face_t *face )
{
    os64_font_engine_t *engine;

    if ( face == NULL )
        return;

    engine = face->engine;
    FT_Done_Face( face->ft );
    engine->live_faces--;
    engine_free( engine, face );
}


static os64_font_status_t face_info( os64_font_face_t *face,
                                     os64_font_face_info_t *out )
{
    if ( out != NULL )
        __builtin_memset( out, 0, sizeof( *out ) );
    if ( face == NULL || out == NULL )
        return OS64_FONT_BAD_ARGUMENT;

    *out = face->info;
    return OS64_FONT_OK;
}


static os64_font_status_t lookup( os64_font_face_t *face, uint32_t scalar,
                                  uint32_t *glyph_index )
{
    FT_UInt index;

    if ( glyph_index != NULL )
        *glyph_index = 0;
    if ( face == NULL || glyph_index == NULL )
        return OS64_FONT_BAD_ARGUMENT;

    /* A surrogate is half of a UTF-16 encoding and never a scalar value;
     * anything above U+10FFFF is not Unicode at all. Both are the caller
     * having decoded badly, which is a different problem from a font that
     * simply lacks the character. */
    if ( scalar > 0x10FFFFu || ( scalar >= 0xD800u && scalar <= 0xDFFFu ) )
        return OS64_FONT_BAD_ARGUMENT;

    index = FT_Get_Char_Index( face->ft, scalar );
    if ( index == 0 )
        return OS64_FONT_MISSING;

    *glyph_index = (uint32_t)index;
    return OS64_FONT_OK;
}


static int glyph_index_ok( const os64_font_face_t *face, uint32_t index )
{
    return index != 0 && index < face->info.glyph_count;
}


static os64_font_status_t pair_adjust( os64_font_face_t *face, uint32_t left,
                                       uint32_t right, os64_font_pos_t *delta_x )
{
    FT_Vector kerning;
    FT_Error  error;
    os64_font_engine_t *engine;

    if ( delta_x != NULL )
        *delta_x = 0;
    if ( face == NULL || delta_x == NULL )
        return OS64_FONT_BAD_ARGUMENT;
    if ( !glyph_index_ok( face, left ) || !glyph_index_ok( face, right ) )
        return OS64_FONT_BAD_ARGUMENT;

    /* Upstream picks between the legacy `kern` table and GPOS pair values and
     * never sums them, which is the precedence the contract asks for. It is
     * pair kerning and nothing more: no contextual positioning is implied by
     * a non-zero answer here. */
    engine = face->engine;
    engine->allocation_failure = OS64_FONT_OK;
    error = FT_Get_Kerning( face->ft, left, right, face->kern_mode, &kerning );
    if ( error != FT_Err_Ok )
        return status_for( engine, error );

    if ( kerning.x < INT32_MIN || kerning.x > INT32_MAX )
        return OS64_FONT_LIMIT;

    *delta_x = (os64_font_pos_t)kerning.x;
    return OS64_FONT_OK;
}


/* ── rendering ─────────────────────────────────────────────────────────── */

static os64_font_status_t render( os64_font_face_t *face, uint32_t glyph_index,
                                  os64_font_glyph_t **out )
{
    os64_font_engine_t *engine;
    os64_font_glyph_t  *glyph;
    FT_GlyphSlot        slot;
    FT_Error            error;
    uint32_t            width, height;
    size_t              bytes;
    int64_t             left, top;

    if ( out == NULL )
        return OS64_FONT_BAD_ARGUMENT;
    *out = NULL;

    if ( face == NULL || !glyph_index_ok( face, glyph_index ) )
        return OS64_FONT_BAD_ARGUMENT;

    engine = face->engine;
    engine->allocation_failure = OS64_FONT_OK;

    error = FT_Load_Glyph( face->ft, glyph_index, face->load_flags );
    if ( error != FT_Err_Ok )
        return status_for( engine, error );

    slot = face->ft->glyph;

    /* A glyph that arrived as an image rather than an outline is a colour or
     * bitmap glyph this configuration cannot render. Reporting it as an empty
     * glyph would draw a blank where a character exists. */
    if ( slot->format != FT_GLYPH_FORMAT_OUTLINE )
        return OS64_FONT_UNSUPPORTED;

    if ( slot->advance.x < 0 || slot->advance.x > INT32_MAX )
        return OS64_FONT_LIMIT;

    error = FT_Render_Glyph( slot, FT_RENDER_MODE_NORMAL );
    if ( error != FT_Err_Ok )
        return status_for( engine, error );

    if ( slot->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY ||
         slot->bitmap.num_grays != 256 )
        return OS64_FONT_UNSUPPORTED;

    width  = slot->bitmap.width;
    height = slot->bitmap.rows;

    if ( width > OS64_FONT_MASK_DIM_MAX || height > OS64_FONT_MASK_DIM_MAX )
        return OS64_FONT_LIMIT;

    /* The bearings are FreeType's, turned the way os64 surfaces face: its
     * `bitmap_top` counts UP from the baseline to the mask's first row, and
     * everything downstream of here counts DOWN. Widened first because the
     * sum below can leave the range that either end is stored in. */
    left = (int64_t)slot->bitmap_left;
    top  = -(int64_t)slot->bitmap_top;

    if ( left < INT32_MIN || left > INT32_MAX ||
         top < INT32_MIN || top > INT32_MAX )
        return OS64_FONT_LIMIT;

    /* The ink rectangle is those bearings and dimensions in 26.6, so its
     * corners have to survive the multiply as well as the add. */
    if ( ( left + width ) * FIXED_ONE > INT32_MAX ||
         ( left * FIXED_ONE ) < INT32_MIN ||
         ( top + height ) * FIXED_ONE > INT32_MAX ||
         ( top * FIXED_ONE ) < INT32_MIN )
        return OS64_FONT_LIMIT;

    glyph = engine_alloc( engine, sizeof( *glyph ) );
    if ( glyph == NULL )
        return engine->allocation_failure;

    glyph->engine         = engine;
    glyph->coverage       = NULL;
    glyph->coverage_bytes = 0;
    glyph->advance_x      = (os64_font_pos_t)slot->advance.x;

    if ( width == 0 || height == 0 )
    {
        /* A space has an advance and no ink. Every geometric field is zero,
         * including the bearings, so a caller cannot accidentally place a
         * mask that is not there. */
        glyph->width  = 0;
        glyph->height = 0;
        glyph->stride = 0;
        glyph->left   = 0;
        glyph->top    = 0;
        glyph->ink.x0 = glyph->ink.y0 = glyph->ink.x1 = glyph->ink.y1 = 0;

        engine->live_glyphs++;
        *out = glyph;
        return OS64_FONT_OK;
    }

    bytes = (size_t)width * (size_t)height;

    glyph->coverage = engine_alloc( engine, bytes );
    if ( glyph->coverage == NULL )
    {
        engine_free( engine, glyph );
        return engine->allocation_failure;
    }
    glyph->coverage_bytes = bytes;

    /* Copy row by row rather than wholesale: upstream's pitch is a signed
     * stride that can run the rows BACKWARD through memory, and it is not
     * required to equal the width. FreeType's buffer starts at the lowest
     * address, so a negative pitch places the top row at the far end. The
     * contract's output is tightly packed and top row first. */
    for ( uint32_t row = 0; row < height; row++ )
    {
        const uint8_t *src = slot->bitmap.pitch >= 0
            ? slot->bitmap.buffer + (size_t)row * (size_t)slot->bitmap.pitch
            : slot->bitmap.buffer
                  + (size_t)( height - 1 - row )
                      * (size_t)( -(int64_t)slot->bitmap.pitch );


        __builtin_memcpy( glyph->coverage + (size_t)row * (size_t)width,
                          src, width );
    }

    glyph->width  = width;
    glyph->height = height;
    glyph->stride = width;
    glyph->left   = (int32_t)left;
    glyph->top    = (int32_t)top;
    glyph->ink.x0 = (os64_font_pos_t)( left * FIXED_ONE );
    glyph->ink.y0 = (os64_font_pos_t)( top * FIXED_ONE );
    glyph->ink.x1 = (os64_font_pos_t)( ( left + width ) * FIXED_ONE );
    glyph->ink.y1 = (os64_font_pos_t)( ( top + height ) * FIXED_ONE );

    engine->live_glyphs++;
    *out = glyph;
    return OS64_FONT_OK;
}


static os64_font_status_t glyph_view( os64_font_glyph_t *glyph,
                                      os64_font_glyph_view_t *out )
{
    if ( out != NULL )
        __builtin_memset( out, 0, sizeof( *out ) );
    if ( glyph == NULL || out == NULL )
        return OS64_FONT_BAD_ARGUMENT;

    out->advance_x = glyph->advance_x;
    out->ink       = glyph->ink;
    out->width     = glyph->width;
    out->height    = glyph->height;
    out->stride    = glyph->stride;
    out->left      = glyph->left;
    out->top       = glyph->top;
    out->coverage  = glyph->coverage;
    return OS64_FONT_OK;
}


static void glyph_release( os64_font_glyph_t *glyph )
{
    os64_font_engine_t *engine;

    if ( glyph == NULL )
        return;

    engine = glyph->engine;
    engine_free( engine, glyph->coverage );
    engine->live_glyphs--;
    engine_free( engine, glyph );
}


/* ── the table, and the only symbol that leaves ────────────────────────── */

static const os64_font_backend_t backend = {
    .revision      = OS64_FONT_BACKEND_REVISION,
    .struct_size   = sizeof( os64_font_backend_t ),
    .engine_create = engine_create,
    .engine_destroy = engine_destroy,
    .engine_stats  = engine_stats,
    .face_open     = face_open,
    .face_close    = face_close,
    .face_info     = face_info,
    .lookup        = lookup,
    .pair_adjust   = pair_adjust,
    .render        = render,
    .glyph_view    = glyph_view,
    .glyph_release = glyph_release,
};


__attribute__(( visibility( "default" ) ))
const os64_font_backend_t *os64_freetype_backend_v1( void )
{
    return &backend;
}
