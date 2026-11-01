#include "internal.h"
#include <math.h>

/* Points are engine-owned values, not document registry entries: sampling a
 * scratch ticket can create thousands of temporary points in a single turn. */
static JSClassID point_slot, readonly_slot;
typedef struct { double v[4]; } DPoint;
static const char *const names[]={"x","y","z","w"};
static DPoint *point(JSValueConst value)
{
    DPoint *p=JS_GetOpaque(value,os64_js_class_id(&point_slot));
    return p?p:JS_GetOpaque(value,os64_js_class_id(&readonly_slot));
}
static void finalize(JSRuntime *rt,JSValue value) {js_free_rt(rt,point(value));}
int d_point_dictionary(JSContext *ctx,JSValueConst value,double out[4])
{
    *out=0;out[1]=out[2]=0;out[3]=1;
    if(JS_IsUndefined(value)||JS_IsNull(value))return 0;
    if(!JS_IsObject(value)){JS_ThrowTypeError(ctx,"Expected a point dictionary");return -1;}
    /* WebIDL dictionaries read inherited members in lexicographic order. */
    const unsigned order[]={3,0,1,2};
    for(unsigned j=0;j<4;j++) {
        unsigned i=order[j];JSValue v=JS_GetPropertyStr(ctx,value,names[i]);
        if(JS_IsException(v))return -1;
        int result=JS_IsUndefined(v)?0:JS_ToFloat64(ctx,&out[i],v);
        JS_FreeValue(ctx,v);if(result<0)return -1;
    }
    return 0;
}
static JSValue make(JSContext *ctx,bool readonly,JSValueConst proto,const double v[4])
{
    JSClassID id=os64_js_class_id(readonly?&readonly_slot:&point_slot);
    JSValue object=JS_IsObject(proto)?JS_NewObjectProtoClass(ctx,proto,id):JS_NewObjectClass(ctx,id);
    if(JS_IsException(object))return object;
    DPoint *p=js_malloc(ctx,sizeof(*p));
    if(!p){JS_FreeValue(ctx,object);return JS_EXCEPTION;}
    for(unsigned i=0;i<4;i++)p->v[i]=v[i];
    JS_SetOpaque(object,p);return object;
}
static JSValue construct_point(JSContext *ctx,JSValueConst target,int argc,JSValueConst *argv,bool readonly)
{
    if(!d_binding(ctx))return JS_EXCEPTION;
    double v[]={0,0,0,1};
    for(int i=0;i<argc && i<4;i++)
        if(!JS_IsUndefined(argv[i]) && JS_ToFloat64(ctx,&v[i],argv[i])<0)return JS_EXCEPTION;
    JSValue proto=JS_GetPropertyStr(ctx,target,"prototype");
    if(JS_IsException(proto))return proto;
    JSValue object=make(ctx,readonly,proto,v);JS_FreeValue(ctx,proto);return object;
}
static JSValue construct(JSContext *c,JSValueConst t,int n,JSValueConst *v) {return construct_point(c,t,n,v,false);}
static JSValue construct_readonly(JSContext *c,JSValueConst t,int n,JSValueConst *v) {return construct_point(c,t,n,v,true);}
static JSValue from_point(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,int magic,JSValue *data)
{
    (void)self;if(!d_callback(ctx,data,OS64_JS_ABI_ID))return JS_EXCEPTION;
    double v[4];if(d_point_dictionary(ctx,argc?argv[0]:JS_UNDEFINED,v)<0)return JS_EXCEPTION;
    return make(ctx,magic!=0,JS_UNDEFINED,v);
}
static JSValue member(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,int magic,JSValue *data)
{
    if(!d_callback(ctx,data,OS64_JS_ABI_ID))return JS_EXCEPTION;
    DPoint *p=point(self);if(!p)return JS_ThrowTypeError(ctx,"Expected a DOMPoint");
    bool set=(magic&D_SET)!=0;magic&=~D_SET;
    if(set) {
        if(!JS_GetOpaque(self,os64_js_class_id(&point_slot)))return JS_ThrowTypeError(ctx,"Point is read only");
        double v;if(JS_ToFloat64(ctx,&v,argc?argv[0]:JS_UNDEFINED)<0)return JS_EXCEPTION;
        p->v[magic]=v;return JS_UNDEFINED;
    }
    if(magic<4)return JS_NewFloat64(ctx,p->v[magic]);
    JSValue out=JS_NewObject(ctx);if(JS_IsException(out))return out;
    for(unsigned i=0;i<4;i++)
        if(JS_DefinePropertyValueStr(ctx,out,names[i],JS_NewFloat64(ctx,p->v[i]),JS_PROP_C_W_E)<0) {
            JS_FreeValue(ctx,out);return JS_EXCEPTION;
        }
    return out;
}
static bool same_number(double a,double b) {return a==b||(isnan(a)&&isnan(b));}
static JSValue matrix_transform(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,int magic,JSValue *data)
{
    (void)magic;
    if(!d_callback(ctx,data,OS64_JS_ABI_ID))return JS_EXCEPTION;
    DPoint *p=point(self);if(!p)return JS_ThrowTypeError(ctx,"Expected a DOMPoint");
    int dictionary=d_dictionary(ctx,argc,argv,0);if(dictionary<0)return JS_EXCEPTION;
    double matrix[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    double aliases[6]={0};bool has_alias[6]={0},has_matrix[16]={0};
    bool explicit_2d=false,is2d=true;
    if(dictionary) {
        const char *alias_names[]={"a","b","c","d","e","f"};
        for(unsigned i=0;i<6;i++) {
            JSValue v=JS_GetPropertyStr(ctx,argv[0],alias_names[i]);if(JS_IsException(v))return v;
            has_alias[i]=!JS_IsUndefined(v);
            int r=has_alias[i]?JS_ToFloat64(ctx,&aliases[i],v):0;JS_FreeValue(ctx,v);if(r<0)return JS_EXCEPTION;
        }
        JSValue v=JS_GetPropertyStr(ctx,argv[0],"is2D");if(JS_IsException(v))return v;
        explicit_2d=!JS_IsUndefined(v);if(explicit_2d)is2d=JS_ToBool(ctx,v);JS_FreeValue(ctx,v);
        for(unsigned i=0;i<16;i++) {
            char key[]={'m',(char)('1'+i/4),(char)('1'+i%4),0};
            v=JS_GetPropertyStr(ctx,argv[0],key);if(JS_IsException(v))return v;
            has_matrix[i]=!JS_IsUndefined(v);
            int r=has_matrix[i]?JS_ToFloat64(ctx,&matrix[i],v):0;JS_FreeValue(ctx,v);if(r<0)return JS_EXCEPTION;
        }
    }
    const unsigned alias_at[]={0,1,4,5,12,13};
    for(unsigned i=0;i<6;i++)if(has_alias[i]) {
        unsigned at=alias_at[i];
        if(has_matrix[at]&&!same_number(aliases[i],matrix[at]))return JS_ThrowTypeError(ctx,"Conflicting matrix aliases");
        matrix[at]=aliases[i];
    }
    const unsigned zero_at[]={2,3,6,7,8,9,11,14};bool actual2d=matrix[10]==1&&matrix[15]==1;
    for(unsigned i=0;i<sizeof(zero_at)/sizeof(*zero_at);i++)actual2d=actual2d&&matrix[zero_at[i]]==0;
    if(explicit_2d&&is2d&&!actual2d)return JS_ThrowTypeError(ctx,"A 2D matrix has 3D members");
    double out[4]={0};
    for(unsigned row=0;row<4;row++)for(unsigned col=0;col<4;col++)out[row]+=matrix[col*4+row]*p->v[col];
    return make(ctx,false,JS_UNDEFINED,out);
}
int d_point_install(os64_dom_t *dom,JSContext *ctx,JSValueConst global)
{
    JSValue parent=JS_UNDEFINED,parent_proto=JS_UNDEFINED;
    for(unsigned i=0;i<2;i++) {
        bool readonly=i==0;const char *name=readonly?"DOMPointReadOnly":"DOMPoint";
        JSClassID id=os64_js_class_id(readonly?&readonly_slot:&point_slot);
        JSClassDef definition={.class_name=name,.finalizer=finalize};
        if(!JS_IsRegisteredClass(dom->engine,id)&&JS_NewClass(dom->engine,id,&definition)<0)return -1;
        JSValue proto=readonly?JS_NewObject(ctx):JS_NewObjectProto(ctx,parent_proto);
        if(!d_retain(dom,ctx,proto))return -1;
        JS_SetClassProto(ctx,id,JS_DupValue(ctx,proto));
        JSValue constructor=d_interface_new(ctx,name,0,readonly?construct_readonly:construct,proto,parent);
        if(JS_IsException(constructor))return -1;
        if(!d_retain(dom,ctx,constructor))return -1;
        if(JS_DefinePropertyValueStr(ctx,global,name,JS_DupValue(ctx,constructor),JS_PROP_WRITABLE|JS_PROP_CONFIGURABLE)<0)return -1;
        for(unsigned j=0;j<4;j++)if(d_accessor(dom,ctx,proto,names[j],member,j,!readonly)<0)return -1;
        if(d_method(dom,ctx,constructor,"fromPoint",from_point,0,readonly)<0)return -1;
        if(readonly && (d_method(dom,ctx,proto,"toJSON",member,0,4)<0 ||
            d_method(dom,ctx,proto,"matrixTransform",matrix_transform,0,0)<0))return -1;
        parent=constructor;parent_proto=proto;
    }
    return 0;
}
