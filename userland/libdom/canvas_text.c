#include "internal.h"
#include "garb/values.h"
#include "os64/fmt.h"
#include <math.h>

#define TEXT_BYTES 16384u
#define FONT_NAMES 64u
static JSClassID metrics_slot;
static const char *const aligns[]={"start","end","left","right","center"};
static const char *const baselines[]={"alphabetic","top","hanging","middle","ideographic","bottom"};
static const char *const directions[]={"inherit","ltr","rtl"};
static const char *const metric_names[]={"width","actualBoundingBoxLeft","actualBoundingBoxRight",
    "fontBoundingBoxAscent","fontBoundingBoxDescent","actualBoundingBoxAscent","actualBoundingBoxDescent",
    "emHeightAscent","emHeightDescent","hangingBaseline","alphabeticBaseline","ideographicBaseline"};
void os64_dom_set_text(os64_dom_t *dom,const os64_dom_text_provider_t *provider)
{ if(dom&&!dom->closed)dom->canvas_text=provider?*provider:(os64_dom_text_provider_t){0}; }
static bool word(const garb_val_t *v,const char *s)
{ return v->kind==GARB_V_KEYWORD&&!os64_strcmp(v->keyword,s); }
typedef struct {
    garb_parsed_t parsed;
    garb_set_t sets[GARB_SETS_MAX];
    const garb_val_t *family,*size,*weight,*style,*variant;
} Font;
static int parse_font(JSContext *ctx,const char *text,size_t n,Font *f)
{
    *f=(Font){0};
    garb_status_t status=n<=65536?garb_parse_values_limited(text,n,2u*1024*1024,&f->parsed):GARB_TOO_BIG;
    if(status==GARB_NO_MEMORY||status==GARB_TOO_BIG||f->parsed.incomplete) {
        d_error(ctx,"QuotaExceededError","Canvas font parse refused");return -1;
    }
    if(status!=GARB_OK)return 0;
    garb_decl_t decl={.name="font",.len=4,.value=f->parsed.values,.nvalue=f->parsed.nvalues};
    int count=garb_read_declaration(&f->parsed,&decl,(garb_reading_t){0},f->sets);
    if(f->parsed.incomplete){d_error(ctx,"QuotaExceededError","Canvas font parse refused");return -1;}
    for(int i=0;i<count;++i) {
        const garb_val_t *v=&f->sets[i].value;
        switch(f->sets[i].prop) {
        case GARB_FONT_FAMILY:f->family=v;break;
        case GARB_FONT_SIZE:f->size=v;break;
        case GARB_FONT_WEIGHT:f->weight=v;break;
        case GARB_FONT_STYLE:f->style=v;break;
        case GARB_FONT_VARIANT:f->variant=v;break;
        default:break;
        }
    }
    return f->family&&f->family->nitems>0&&f->size&&f->weight&&f->style&&f->variant;
}
static double font_size(const garb_val_t *v,double base,double root,unsigned depth)
{
    if(depth>32)return NAN;
    if(v->kind==GARB_V_NUMBER)return v->number;
    if(v->kind==GARB_V_PERCENTAGE)return v->number*base/100;
    if(v->kind==GARB_V_LENGTH) {
        double factor;
        switch(v->unit) {
        case GARB_U_PX:factor=1;break;case GARB_U_EM:factor=base;break;case GARB_U_REM:factor=root;break;
        case GARB_U_EX:case GARB_U_CH:factor=base*.5;break;
        case GARB_U_PT:factor=96.0/72;break;case GARB_U_PC:factor=16;break;
        case GARB_U_IN:factor=96;break;case GARB_U_CM:factor=96.0/2.54;break;
        case GARB_U_MM:factor=96.0/25.4;break;case GARB_U_Q:factor=96.0/101.6;break;
        default:return NAN;
        }
        return v->number*factor;
    }
    if(v->kind==GARB_V_CALC) {
        const garb_calc_t *c=v->calc;if(!c)return NAN;
        if(c->op==GARB_CALC_LEAF)return font_size(&c->leaf,base,root,depth+1);
        double result=c->op==GARB_CALC_PRODUCT?1:c->op==GARB_CALC_MIN?INFINITY:c->op==GARB_CALC_MAX?-INFINITY:0;
        double values[3]={0};
        for(int i=0;i<c->nargs;++i) {
            garb_val_t child={.kind=GARB_V_CALC,.calc=c->args[i]};double n=font_size(&child,base,root,depth+1);
            if(!isfinite(n))return NAN;
            bool invert=c->invert&&c->invert[i];
            switch(c->op) {
            case GARB_CALC_SUM:result+=invert?-n:n;break;
            case GARB_CALC_PRODUCT:result*=invert?1/n:n;break;
            case GARB_CALC_MIN:result=fmin(result,n);break;
            case GARB_CALC_MAX:result=fmax(result,n);break;
            case GARB_CALC_CLAMP:if(i<3)values[i]=n;break;
            default:return NAN;
            }
        }
        return c->op==GARB_CALC_CLAMP?fmax(values[0],fmin(values[1],values[2])):result;
    }
    if(word(v,"larger"))return base*1.2;
    if(word(v,"smaller"))return base/1.2;
    const char *names[]={"xx-small","x-small","small","medium","large","x-large","xx-large","xxx-large"};
    const double sizes[]={9,10,13,16,18,24,32,48};
    for(unsigned i=0;i<8;++i)if(word(v,names[i]))return sizes[i];
    return NAN;
}
static unsigned font_relative(const garb_val_t *v)
{
    if(v->kind==GARB_V_PERCENTAGE||word(v,"larger")||word(v,"smaller"))return 1;
    if(v->kind==GARB_V_LENGTH)return v->unit==GARB_U_REM?2:
        v->unit==GARB_U_EM||v->unit==GARB_U_EX||v->unit==GARB_U_CH?1:0;
    if(v->kind!=GARB_V_CALC||!v->calc)return 0;
    if(v->calc->op==GARB_CALC_LEAF)return font_relative(&v->calc->leaf);
    unsigned used=0;
    for(int i=0;i<v->calc->nargs;++i){garb_val_t child={.kind=GARB_V_CALC,.calc=v->calc->args[i]};used|=font_relative(&child);}
    return used;
}
static int computed_size(DValue *e,JSContext *ctx,const os64_html_node_t *node,double *out)
{
    JSValue value=d_computed_property(e->dom,ctx,node,GARB_FONT_SIZE,"10px");
    if(JS_IsException(value))return -1;
    const char *s=JS_ToCString(ctx,value);JS_FreeValue(ctx,value);if(!s)return -1;
    garb_parsed_t parsed={0};garb_set_t sets[GARB_SETS_MAX];
    garb_status_t status=garb_parse_values_limited(s,os64_strlen(s),2u*1024*1024,&parsed);JS_FreeCString(ctx,s);
    garb_decl_t decl={.name="font-size",.len=9,.value=parsed.values,.nvalue=parsed.nvalues};
    int n=status==GARB_OK?garb_read_declaration(&parsed,&decl,(garb_reading_t){0},sets):0;
    *out=n==1?font_size(&sets[0].value,10,16,0):10;
    if(!isfinite(*out))*out=10;
    bool refused=status==GARB_NO_MEMORY||status==GARB_TOO_BIG||parsed.incomplete;garb_free(&parsed);
    if(refused){d_error(ctx,"QuotaExceededError","Canvas font size refused");return -1;}
    return 0;
}
/* Font-family serialization must round-trip names containing CSS escapes.
 * Simple non-keyword names can use an identifier; others use a quoted string. */
static size_t family_dump(const garb_val_t *family,char *out,size_t cap)
{
    size_t at=0;
#define PUT(ch) do {if(cap&&at<cap-1)out[at]=(char)(ch);if(at!=SIZE_MAX)++at;}while(0)
    for(int i=0;i<family->nitems;++i) {
        const garb_val_t *v=&family->items[i];if(i){PUT(',');PUT(' ');}
        const char *text=v->kind==GARB_V_KEYWORD?v->keyword:v->text;
        size_t length=v->kind==GARB_V_KEYWORD?os64_strlen(text):v->len;
        bool quote=v->kind!=GARB_V_KEYWORD;
        if(quote&&length&&((text[0]>='A'&&text[0]<='Z')||(text[0]>='a'&&text[0]<='z')||text[0]=='_')) {
            quote=false;
            for(size_t k=1;k<length;++k)if(!((text[k]>='A'&&text[k]<='Z')||(text[k]>='a'&&text[k]<='z')||
                (text[k]>='0'&&text[k]<='9')||text[k]=='_'||text[k]=='-'))quote=true;
            const char *reserved[]={"serif","sans-serif","monospace","cursive","fantasy","system-ui","ui-serif","ui-sans-serif",
                "ui-monospace","ui-rounded","math","fangsong","emoji","inherit","initial","unset","revert","revert-layer","default"};
            for(unsigned k=0;k<sizeof(reserved)/sizeof(*reserved);++k)
                if(length==os64_strlen(reserved[k])) {
                    bool match=true;for(size_t j=0;j<length;++j){unsigned ch=(unsigned char)text[j];if(ch>='A'&&ch<='Z')ch+='a'-'A';if(ch!=(unsigned char)reserved[k][j])match=false;}
                    if(match)quote=true;
                }
        }
        if(quote)PUT('"');
        for(size_t k=0;k<length;++k) {
            unsigned ch=(unsigned char)text[k];
            if(ch=='"'||ch=='\\'){PUT('\\');PUT(ch);}
            else if(ch<32||ch==127){const char *hex="0123456789abcdef";PUT('\\');PUT(hex[ch>>4]);PUT(hex[ch&15]);PUT(' ');}
            else PUT(ch);
        }
        if(quote)PUT('"');
    }
    if(cap)out[at<cap?at:cap-1]=0;
#undef PUT
    return at;
}
static int canonical_font(DValue *e,JSContext *ctx,const char *s,size_t n,canvas_text_style_t *style)
{
    Font f;int valid=parse_font(ctx,s,n,&f);
    if(valid<=0){garb_free(&f.parsed);return valid;}
    double base=16,root=16;
    unsigned relative=font_relative(f.size);
    if((relative&1)&&computed_size(e,ctx,e->node,&base)<0){garb_free(&f.parsed);return -1;}
    /* Root-relative units use the document element, independently of the canvas. */
    const os64_html_node_t *element=e->node;
    while(element&&element->parent&&element->parent->kind==OS64_HTML_ELEMENT)element=element->parent;
    if((relative&2)&&computed_size(e,ctx,element,&root)<0){garb_free(&f.parsed);return -1;}
    double px=font_size(f.size,base,root,0);
    if(!isfinite(px)||px<0){garb_free(&f.parsed);return 0;}
    if(px>OS64_FONT_PIXEL_MAX){garb_free(&f.parsed);d_error(ctx,"QuotaExceededError","Canvas font exceeds size budget");return -1;}
    char family[768],weight[32],variant[32],slant[32],size[64];
    size_t family_length=family_dump(f.family,family,sizeof(family));
    garb_val_t resolved={.kind=GARB_V_LENGTH,.number=px,.unit=GARB_U_PX};garb_val_dump(&resolved,size,sizeof(size));
    weight[0]=variant[0]=slant[0]=0;
    if(!word(f.weight,"normal")&&!(f.weight->kind==GARB_V_NUMBER&&f.weight->number==400))garb_val_dump(f.weight,weight,sizeof(weight));
    if(!word(f.variant,"normal"))garb_val_dump(f.variant,variant,sizeof(variant));
    if(!word(f.style,"normal"))garb_val_dump(f.style,slant,sizeof(slant));
    int length=os64_snprintf(style->font,sizeof(style->font),"%s%s%s%s%s%s%s %s",slant,slant[0]?" ":"",
        variant,variant[0]?" ":"",weight,weight[0]?" ":"",size,family);
    garb_free(&f.parsed);
    if(family_length>=sizeof(family)||length<0||(size_t)length>=sizeof(style->font)) {
        d_error(ctx,"QuotaExceededError","Canvas font exceeds text budget");return -1;
    }
    style->size=px;return 1;
}
static canvas_text_style_t text_style(DValue *e)
{
    canvas_state_t state={.text={.font="10px sans-serif",.size=10}};
    d_canvas_sync(e);if(e->canvas)canvas_state(e->canvas,&state);
    return state.text;
}
static JSValue style_member(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,int magic,JSValue *data)
{
    os64_dom_t *dom=d_callback(ctx,data,OS64_JS_ABI_ID);if(!dom)return JS_EXCEPTION;
    DValue *e=JS_GetOpaque(self,dom->canvas_class);
    if(!e||e->dom!=dom)return JS_ThrowTypeError(ctx,"Expected a CanvasRenderingContext2D");
    bool set=magic&D_SET;magic&=~D_SET;
    const char *s=NULL;size_t n=0;
    if(set){s=JS_ToCStringLen(ctx,&n,argc?argv[0]:JS_UNDEFINED);if(!s)return JS_EXCEPTION;}
    canvas_text_style_t style=text_style(e);
    const char *const *names=magic==1?aligns:magic==2?baselines:directions;
    unsigned count=magic==1?5:magic==2?6:3;
    unsigned *slot=magic==1?&style.align:magic==2?&style.baseline:&style.direction;
    if(!set)return JS_NewString(ctx,magic?names[*slot]:style.font);
    int valid=0;
    if(!magic)valid=canonical_font(e,ctx,s,n,&style);
    else for(unsigned i=0;i<count;++i)if(n==os64_strlen(names[i])&&!os64_memcmp(s,names[i],n)){*slot=i;valid=1;break;}
    JS_FreeCString(ctx,s);
    if(valid<0)return JS_EXCEPTION;
    return valid&&e->canvas?d_canvas_result(ctx,canvas_set_text_style(e->canvas,&style)):JS_UNDEFINED;
}
static JSValue metric_member(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,int magic,JSValue *data)
{
    (void)argc;(void)argv;
    os64_dom_t *dom=d_callback(ctx,data,OS64_JS_ABI_ID);if(!dom)return JS_EXCEPTION;
    double *values=JS_GetOpaque(self,dom->metrics_class);
    if(!values)return JS_ThrowTypeError(ctx,"Expected TextMetrics");
    return JS_NewFloat64(ctx,values[magic]);
}
static JSValue metrics_new(os64_dom_t *dom,JSContext *ctx,const double *values)
{
    JSValue object=JS_NewObjectClass(ctx,dom->metrics_class);if(JS_IsException(object))return object;
    double *copy=js_malloc(ctx,12*sizeof(double));
    if(!copy){JS_FreeValue(ctx,object);return JS_EXCEPTION;}
    os64_memcpy(copy,values,12*sizeof(double));JS_SetOpaque(object,copy);
    return object;
}
static void metrics_finalize(JSRuntime *runtime,JSValue value)
{ js_free_rt(runtime,JS_GetOpaque(value,JS_GetClassID(value))); }

typedef struct { os64_geometry_command_t *commands;size_t count; } Commands;
static os64_font_status_t collect(void *opaque,const os64_geometry_command_t *command)
{
    Commands *c=opaque;if(c->count==OS64_GEOMETRY_COMMAND_MAX)return OS64_FONT_LIMIT;
    c->commands[c->count++]=*command;return OS64_FONT_OK;
}
static JSValue font_error(JSContext *ctx,os64_font_status_t status)
{
    return d_error(ctx,status==OS64_FONT_LIMIT||status==OS64_FONT_NO_MEMORY?"QuotaExceededError":
        status==OS64_FONT_UNSUPPORTED?"NotSupportedError":"InvalidStateError","Canvas text preparation refused");
}
static JSValue text_method(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,int magic,JSValue *data)
{
    os64_dom_t *dom=d_callback(ctx,data,OS64_JS_ABI_ID);if(!dom)return JS_EXCEPTION;
    DValue *e=JS_GetOpaque(self,dom->canvas_class);
    if(!e||e->dom!=dom)return JS_ThrowTypeError(ctx,"Expected a CanvasRenderingContext2D");
    if(argc<(magic==0?1:3))return JS_ThrowTypeError(ctx,"Canvas text arguments are missing");
    size_t length;const char *string=JS_ToCStringLen(ctx,&length,argv[0]);if(!string)return JS_EXCEPTION;
    double x=0,y=0,max_width=0;bool limited=magic&&argc>3;
    if(magic&&(JS_ToFloat64(ctx,&x,argv[1])<0||JS_ToFloat64(ctx,&y,argv[2])<0||
       (limited&&JS_ToFloat64(ctx,&max_width,argv[3])<0))){JS_FreeCString(ctx,string);return JS_EXCEPTION;}
    if(magic&&(!isfinite(x)||!isfinite(y)||(limited&&(isnan(max_width)||max_width<=0)))) {
        JS_FreeCString(ctx,string);return JS_UNDEFINED;
    }
    if(length>TEXT_BYTES){JS_FreeCString(ctx,string);return d_error(ctx,"QuotaExceededError","Canvas text exceeds byte budget");}
    uint8_t *text=NULL;os64_geometry_status_t allocated=d_canvas_allocate(dom,length+1,(void **)&text);
    if(allocated!=OS64_GEOMETRY_OK){JS_FreeCString(ctx,string);return d_canvas_result(ctx,CANVAS_NO_MEMORY);}
    for(size_t i=0;i<length;++i)text[i]=string[i]=='\t'||string[i]=='\n'||string[i]=='\r'||string[i]=='\f'? ' ':(uint8_t)string[i];
    text[length]=0;JS_FreeCString(ctx,string);
    canvas_text_style_t style=text_style(e);
    bool rtl=style.direction==2;
    if(!style.direction) {
        /* HTML direction inheritance supplies alignment while the shared text
         * engine retains its Western shaping profile. */
        for(const os64_html_node_t *node=e->node;node;node=node->parent) {
            const os64_html_attr_t *dir=os64_html_attr(node,"dir");
            if(dir&&(os64_streq_nocase(dir->value,"rtl")||os64_streq_nocase(dir->value,"ltr"))) {
                rtl=os64_streq_nocase(dir->value,"rtl");break;
            }
        }
    }
    Font f;int valid=parse_font(ctx,style.font,os64_strlen(style.font),&f);
    if(valid<=0){garb_free(&f.parsed);d_canvas_release(dom,text,length+1);return valid<0?JS_EXCEPTION:font_error(ctx,OS64_FONT_UNSUPPORTED);}
    os64_font_family_name_t names[FONT_NAMES];unsigned count=0;
    os64_font_family_t generic=OS64_FONT_FAMILY_SANS;bool have_generic=false;
    for(int i=0;i<f.family->nitems;++i) {
        const garb_val_t *v=&f.family->items[i];
        if(v->kind==GARB_V_STRING) {
            if(count==FONT_NAMES){garb_free(&f.parsed);d_canvas_release(dom,text,length+1);return font_error(ctx,OS64_FONT_LIMIT);}
            names[count++]=(os64_font_family_name_t){v->text,v->len};
        } else if(!have_generic) {
            generic=word(v,"serif")?OS64_FONT_FAMILY_SERIF:word(v,"monospace")?OS64_FONT_FAMILY_MONO:OS64_FONT_FAMILY_SANS;
            have_generic=true;
        }
    }
    bool bold=word(f.weight,"bold")||word(f.weight,"bolder")||(f.weight->kind==GARB_V_NUMBER&&f.weight->number>=600);
    bool italic=!word(f.style,"normal");
    os64_font_family_list_t families={names,count,generic};os64_text_run_t *run=NULL;
    uint32_t pixels=style.size>0?(uint32_t)ceil(style.size):1;
    os64_font_status_t status=dom->canvas_text.layout?dom->canvas_text.layout(dom->canvas_text.opaque,&families,bold,italic,pixels,text,length,&run):OS64_FONT_UNSUPPORTED;
    garb_free(&f.parsed);d_canvas_release(dom,text,length+1);
    if(status!=OS64_FONT_OK){os64_text_run_release(run);return font_error(ctx,status);}
    os64_text_run_view_t view;status=os64_text_run_view(run,&view);
    Commands commands={0};size_t capacity=OS64_GEOMETRY_COMMAND_MAX*sizeof(*commands.commands);
    if(magic&&status==OS64_FONT_OK&&d_canvas_allocate(dom,capacity,(void **)&commands.commands)!=OS64_GEOMETRY_OK)status=OS64_FONT_LIMIT;
    os64_geometry_bounds_t bounds={.empty=true};
    if(status==OS64_FONT_OK)status=os64_text_run_outline(run,OS64_GEOMETRY_COMMAND_MAX,magic?collect:NULL,&commands,&bounds);
    if(status!=OS64_FONT_OK){os64_text_run_release(run);d_canvas_release(dom,commands.commands,capacity);return font_error(ctx,status);}
    double scale=style.size/pixels,width=view.advance_x/64.0*scale;
    double ascent=view.ascent/64.0*scale,descent=view.descent/64.0*scale;
    /* Fonts without explicit baseline tables use the same metric-based
     * synthesis for measurement and placement. Full script shaping is owned
     * by the shared text engine's supported encoding profile. */
    double em_ascent=ascent+descent?style.size*ascent/(ascent+descent):0;
    double em_descent=style.size-em_ascent;
    double baseline=style.baseline==1?em_ascent:style.baseline==2?ascent*.8:
        style.baseline==3?(em_ascent-em_descent)/2:style.baseline==4?-descent:style.baseline==5?-em_descent:0;
    bool right=style.align==3||(style.align==0&&rtl)||(style.align==1&&!rtl);
    double anchor=style.align==4?width/2:right?width:0;
    JSValue result;
    if(!magic) {
        double values[]={width,bounds.empty?0:anchor-bounds.x_min*scale,bounds.empty?0:bounds.x_max*scale-anchor,
            ascent-baseline,descent+baseline,bounds.empty?0:-bounds.y_min*scale-baseline,bounds.empty?0:bounds.y_max*scale+baseline,
            em_ascent-baseline,em_descent+baseline,ascent*.8-baseline,-baseline,-descent-baseline};
        result=metrics_new(dom,ctx,values);
    } else {
        double squeeze=limited&&width>max_width?max_width/width:1;
        os64_geometry_map_t placement={scale*squeeze,0,0,scale,x-anchor*squeeze,y+baseline};
        result=e->canvas?d_canvas_result(ctx,canvas_draw_contours(e->canvas,commands.commands,commands.count,&placement,magic==2)):JS_UNDEFINED;
    }
    os64_text_run_release(run);d_canvas_release(dom,commands.commands,capacity);return result;
}
int d_canvas_text_install(os64_dom_t *dom,JSContext *ctx,JSValueConst global,JSValueConst prototype)
{
    dom->metrics_class=os64_js_class_id(&metrics_slot);
    const JSClassDef definition={.class_name="TextMetrics",.finalizer=metrics_finalize};if(JS_NewClass(dom->engine,dom->metrics_class,&definition)<0)return -1;
    JSValue metrics=JS_NewObject(ctx);if(!d_retain(dom,ctx,metrics))return -1;
    JS_SetClassProto(ctx,dom->metrics_class,JS_DupValue(ctx,metrics));
    JSValue constructor=d_interface_new(ctx,"TextMetrics",0,NULL,metrics,JS_UNDEFINED);
    if(JS_IsException(constructor)||JS_DefinePropertyValueStr(ctx,global,"TextMetrics",constructor,JS_PROP_WRITABLE|JS_PROP_CONFIGURABLE)<0)return -1;
    for(unsigned i=0;i<12;++i)if(d_accessor(dom,ctx,metrics,metric_names[i],metric_member,i,false)<0)return -1;
    const char *styles[]={"font","textAlign","textBaseline","direction"};
    for(unsigned i=0;i<4;++i)if(d_accessor(dom,ctx,prototype,styles[i],style_member,i,true)<0)return -1;
    const char *methods[]={"measureText","fillText","strokeText"};
    for(unsigned i=0;i<3;++i)if(d_method_named(dom,ctx,prototype,methods[i],text_method,i?3:1,i)<0)return -1;
    return 0;
}
