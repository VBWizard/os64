/* ftsystem.c — the system interface os64 supplies instead of upstream's.
 *
 * Upstream's `src/base/ftsystem.c` is FreeType's default platform layer: a
 * `malloc`-backed allocator and a `stdio` file reader. os64 wants neither —
 * `FT_CONFIG_OPTION_DISABLE_STREAM_SUPPORT` removes the reader, and the
 * allocator is the caller's, installed per engine — so that file is not
 * compiled at all and this one stands in its place. Replacing it is
 * upstream's documented arrangement (`upstream/docs/CUSTOMIZE`, section III).
 *
 * What remains is the pair `FT_Init_FreeType` needs. THEY REFUSE, on purpose:
 * `FT_Init_FreeType` exists to conjure a library out of a process-wide heap,
 * and this backend has no process-wide heap to conjure it from. It builds
 * its library with `FT_New_Library` and an `FT_Memory` belonging to one
 * engine, which is what lets two engines in one process account for their
 * memory separately and fail independently.
 *
 * So a call to `FT_Init_FreeType` here returns "out of memory" rather than a
 * library with an allocator nobody chose. The two functions exist so the
 * library still links — deleting them would make the failure an undefined
 * symbol, which says nothing about why.
 */

#include <ft2build.h>
#include <freetype/freetype.h>
#include <freetype/internal/ftobjs.h>


FT_BASE_DEF( FT_Memory )
FT_New_Memory( void )
{
    return NULL;
}


FT_BASE_DEF( void )
FT_Done_Memory( FT_Memory  memory )
{
    /* Only ever reached with the NULL above, since nothing else in this port
     * produces an FT_Memory this function owns: an engine's lives inside the
     * engine and is freed with it. */
    FT_UNUSED( memory );
}
