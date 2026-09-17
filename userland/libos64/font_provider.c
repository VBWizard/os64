#include "os64/font_provider.h"
#include "text_internal.h"

os64_font_status_t os64_font_context_create(const os64_text_options_t *options,
    os64_text_context_t **out)
{
    if (!options) {
        if (out) *out=NULL;
        return OS64_FONT_BAD_ARGUMENT;
    }
    os64_text_options_t resolved=*options;
    if (!resolved.backend) resolved.backend=os64_freetype_backend_v1();
    return os64_text_create(&resolved,out);
}

struct os64_font_set {
    os64_text_context_t *text;
    size_t refs;
    os64_text_font_t *fonts[OS64_FONT_ROLE_COUNT][OS64_FONT_ROLE_FONTS_MAX];
    os64_font_role_view_t roles[OS64_FONT_ROLE_COUNT];
};

static bool valid_source(const os64_font_source_t *s)
{
    if (s->kind==OS64_FONT_SOURCE_BUILTIN) return !s->bytes && !s->length;
    return s->kind==OS64_FONT_SOURCE_OUTLINE && s->bytes && s->length;
}
static os64_font_status_t open_source(os64_text_context_t *text,
    const os64_font_source_t *s, uint32_t size, os64_text_font_t **out)
{
    if (s->kind==OS64_FONT_SOURCE_BUILTIN) return os64_text_font_bitmap(text,out);
    os64_font_face_options_t options={size,OS64_FONT_HINT_NORMAL};
    return os64_text_font_open(text,s->bytes,s->length,&options,out);
}
static os64_font_status_t terminal_width(os64_text_context_t *text,
    os64_text_font_t *font, int32_t *out)
{
    if (!(font->info.flags&OS64_FONT_FACE_FIXED_WIDTH)) return OS64_FONT_UNSUPPORTED;
    os64_font_pos_t width=0;
    os64_text_layout_t layout={.fonts=&font,.font_count=1,
        .encoding=OS64_TEXT_LATIN1,.tab_interval=512};
    for (uint8_t c=0x20;c<=0x7e;c++) {
        os64_text_run_t *run=NULL;
        os64_font_status_t status=os64_text_layout(text,&c,1,&layout,&run);
        if (status!=OS64_FONT_OK) return status;
        os64_text_run_view_t v;
        status=os64_text_run_view(run,&v);
        if (status==OS64_FONT_OK && (v.glyph_count!=1 ||
            v.glyphs[0].font_identity!=font->identity || v.advance_x<=0 ||
            v.advance_x%64 || (width && width!=v.advance_x))) status=OS64_FONT_UNSUPPORTED;
        if (status==OS64_FONT_OK) width=v.advance_x;
        os64_text_run_release(run);
        if (status!=OS64_FONT_OK) return status;
    }
    *out=width/64;
    return OS64_FONT_OK;
}
os64_font_status_t os64_font_set_prepare(os64_text_context_t *text,
    const os64_font_role_spec_t specs[OS64_FONT_ROLE_COUNT], os64_font_set_t **out)
{
    return os64_font_set_prepare_checked(text,specs,out,NULL);
}
os64_font_status_t os64_font_set_prepare_checked(os64_text_context_t *text,
    const os64_font_role_spec_t specs[OS64_FONT_ROLE_COUNT], os64_font_set_t **out,
    os64_font_problem_t *problem)
{
    if (out) *out=NULL;
    if (problem) *problem=(os64_font_problem_t){OS64_FONT_ROLE_COUNT,SIZE_MAX};
    if (!text || !out) return OS64_FONT_BAD_ARGUMENT;
    const os64_font_role_spec_t defaults[OS64_FONT_ROLE_COUNT]={0};
    if (!specs) specs=defaults;
    for (size_t r=0;r<OS64_FONT_ROLE_COUNT;r++) {
        const os64_font_role_spec_t *s=&specs[r];
        uint32_t size=s->pixel_height?s->pixel_height:16;
        if (problem) *problem=(os64_font_problem_t){(os64_font_role_t)r,0};
        if (size<8 || size>96 || s->fallback_count>OS64_FONT_CONFIG_FALLBACK_MAX ||
            !valid_source(&s->primary) ||
            (s->primary.kind==OS64_FONT_SOURCE_BUILTIN && size!=16)) return OS64_FONT_BAD_ARGUMENT;
        bool builtin=s->primary.kind==OS64_FONT_SOURCE_BUILTIN;
        for (size_t n=0;n<s->fallback_count;n++) {
            if (problem) problem->source_index=n+1;
            if (!valid_source(&s->fallbacks[n])) return OS64_FONT_BAD_ARGUMENT;
            if (s->fallbacks[n].kind==OS64_FONT_SOURCE_BUILTIN) {
                if (builtin) return OS64_FONT_BAD_ARGUMENT;
                builtin=true;
            }
        }
    }
    if (problem) *problem=(os64_font_problem_t){OS64_FONT_ROLE_COUNT,SIZE_MAX};
    os64_font_set_t *set=text_alloc(text,sizeof(*set));
    if (!set) return text->refusal;
    *set=(os64_font_set_t){.text=text,.refs=1};
    os64_font_status_t status=OS64_FONT_OK;
    for (size_t r=0;r<OS64_FONT_ROLE_COUNT;r++) {
        const os64_font_role_spec_t *s=&specs[r];
        uint32_t size=s->pixel_height?s->pixel_height:16;
        os64_font_role_view_t *view=&set->roles[r];
        view->text=text;view->fonts=set->fonts[r];
        bool builtin=false;
        for (size_t n=0;n<=s->fallback_count;n++) {
            const os64_font_source_t *source=n?&s->fallbacks[n-1]:&s->primary;
            if (problem) *problem=(os64_font_problem_t){(os64_font_role_t)r,n};
            status=open_source(text,source,size,&set->fonts[r][view->font_count]);
            if (status!=OS64_FONT_OK) goto fail;
            view->font_count++;
            builtin|=source->kind==OS64_FONT_SOURCE_BUILTIN;
        }
        if (!builtin) {
            if (problem) *problem=(os64_font_problem_t){(os64_font_role_t)r,3};
            status=os64_text_font_bitmap(text,&set->fonts[r][view->font_count]);
            if (status!=OS64_FONT_OK) goto fail;
            view->font_count++;
        }
        if (problem) *problem=(os64_font_problem_t){(os64_font_role_t)r,0};
        view->primary=set->fonts[r][0]->info;
        /* The row contains the primary's ascent and remaining line box after
         * integer baseline placement. Fallback metrics do not change it. */
        view->baseline_px=(int32_t)(((int64_t)view->primary.ascent+63)/64);
        view->row_height_px=view->baseline_px+(int32_t)
            (((int64_t)view->primary.line_height-view->primary.ascent+63)/64);
        if (view->row_height_px<=0) {status=OS64_FONT_UNSUPPORTED;goto fail;}
        if (r==OS64_FONT_ROLE_TERMINAL) {
            status=terminal_width(text,set->fonts[r][0],&view->cell_width_px);
            if (status!=OS64_FONT_OK) goto fail;
        }
    }
    for (size_t r=0;r<OS64_FONT_ROLE_COUNT;r++)
        set->roles[r].identity=set->fonts[OS64_FONT_ROLE_UI][0]->identity;
    *out=set;
    if (problem) *problem=(os64_font_problem_t){OS64_FONT_ROLE_COUNT,SIZE_MAX};
    return OS64_FONT_OK;
fail:
    os64_font_set_release(set);
    return status;
}
os64_font_status_t os64_font_set_retain(os64_font_set_t *set)
{
    if (!set) return OS64_FONT_BAD_ARGUMENT;
    if (set->refs==SIZE_MAX) return OS64_FONT_LIMIT;
    set->refs++;
    return OS64_FONT_OK;
}
void os64_font_set_release(os64_font_set_t *set)
{
    if (!set || --set->refs) return;
    for (size_t r=0;r<OS64_FONT_ROLE_COUNT;r++)
        for (size_t n=0;n<set->roles[r].font_count;n++)
            os64_text_font_release(set->fonts[r][n]);
    text_free(set->text,set);
}
os64_font_status_t os64_font_set_view(const os64_font_set_t *set,
    os64_font_role_t role, os64_font_role_view_t *out)
{
    if (out) *out=(os64_font_role_view_t){0};
    if (!set || !out || role<OS64_FONT_ROLE_UI || role>=OS64_FONT_ROLE_COUNT)
        return OS64_FONT_BAD_ARGUMENT;
    *out=set->roles[role];
    return OS64_FONT_OK;
}
