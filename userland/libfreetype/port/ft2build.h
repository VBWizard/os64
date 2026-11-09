/* ft2build.h — os64's entry point into the pinned FreeType sources.
 *
 * Upstream's `include/freetype/config/ftconfig.h` includes <ft2build.h>, and
 * whichever copy the include path finds FIRST decides which configuration
 * headers the whole library is compiled against. libfreetype/shared.mk puts
 * this directory ahead of `upstream/include`, so this file wins and names
 * os64's three replacements. That is upstream's own documented mechanism —
 * `upstream/docs/CUSTOMIZE`, section IV.3 — not a local trick.
 *
 * Nothing else in the port is allowed to edit an upstream file to change a
 * build option: an option changed HERE survives a version bump as a diff
 * anybody can read, while an edit inside upstream/ becomes a patch to
 * rebase forever.
 */

#ifndef OS64_FT2_BUILD_H_
#define OS64_FT2_BUILD_H_

#define FT_CONFIG_OPTIONS_H           <os64_ftoption.h>
#define FT_CONFIG_MODULES_H           <os64_ftmodule.h>
#define FT_CONFIG_STANDARD_LIBRARY_H  <os64_ftstdlib.h>

#include <freetype/config/ftheader.h>

#endif /* OS64_FT2_BUILD_H_ */
