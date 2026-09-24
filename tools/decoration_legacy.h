#ifndef TEST_DECORATION_LEGACY_H
#define TEST_DECORATION_LEGACY_H
/* Build an actual old wire layout, not a new header with a changed version. */
static void *legacy_bundle(const void *source,size_t length,unsigned version,size_t *size)
{
    size_t delta=sizeof(os64_decor_header_t)-OS64_DECOR_LEGACY_HEADER_BYTES;
    *size=length-delta;
    uint8_t *out=os64_malloc(*size);assert(out);
    memcpy(out,source,OS64_DECOR_LEGACY_HEADER_BYTES);
    memcpy(out+OS64_DECOR_LEGACY_HEADER_BYTES,(const uint8_t *)source+sizeof(os64_decor_header_t),length-sizeof(os64_decor_header_t));
    os64_decor_header_t *h=(void *)out;
    h->version=version;h->bytes=(uint32_t)*size;
    h->glyph_offset-=(uint32_t)delta;h->pair_offset-=(uint32_t)delta;
    h->mask_offset-=(uint32_t)delta;h->tile_offset-=(uint32_t)delta;
    return out;
}
#endif
