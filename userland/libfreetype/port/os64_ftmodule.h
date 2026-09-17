/* os64_ftmodule.h — the modules that are registered, in registration order.
 *
 * FreeType tries each DRIVER in this order when it opens a face, so the
 * order is behaviour, not taste; it follows upstream's own
 * `include/freetype/config/ftmodule.h` with the entries os64 does not build
 * removed. `upstream/modules.cfg` is where upstream documents which module
 * needs which.
 *
 * The three PostScript helpers look optional and are not: the CFF driver
 * reads charstrings through `psaux`, hints them through `pshinter`, and
 * resolves charset names through `psnames`. Without them a `.otf` file
 * opens and every glyph comes back empty.
 *
 * `pshinter` is also the answer to "what hints an OpenType/CFF face here?".
 * The autofitter hints TrueType — which has no other hinter in this
 * configuration — while CFF keeps its own, native one. Two hinters, split by
 * format; port/os64_ftoption.h says why, under the bytecode interpreter.
 *
 * Every module here has its sources in `upstream/src/<name>/`, and
 * `sources.mk` compiles exactly those. A module added to one list and not
 * the other is a link error, which is the intended way to find out.
 */

FT_USE_MODULE( FT_Module_Class,    autofit_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Module_Class,    psaux_module_class )
FT_USE_MODULE( FT_Module_Class,    psnames_module_class )
FT_USE_MODULE( FT_Module_Class,    pshinter_module_class )
FT_USE_MODULE( FT_Module_Class,    sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class,  ft_smooth_renderer_class )

/* Deliberately absent: every standalone Type 1/CID/PFR/Windows FON/BDF/PCF
 * driver, the monochrome (`raster1`) and signed-distance-field renderers,
 * and the SVG renderer. The monochrome one is the only near miss — os64
 * renders grayscale coverage and has no consumer for a 1-bit mask. */
