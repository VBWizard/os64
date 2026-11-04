#include "internal.h"

/* ImageData owns a traced engine array, not a native Canvas allocation. Its
 * getters expose that same array even after the page detaches its buffer;
 * pixel consumers must obtain a fresh checked view after argument conversion. */
static JSClassID image_data_slot;
typedef struct {
    JSValue array;
    uint32_t width, height;
} DImageData;

static void finalize(JSRuntime *rt, JSValue value)
{
    DImageData *image = JS_GetOpaque(value, os64_js_class_id(&image_data_slot));
    if (!image) return;
    JS_FreeValueRT(rt, image->array);
    js_free_rt(rt, image);
}
static void mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark_func)
{
    DImageData *image = JS_GetOpaque(value, os64_js_class_id(&image_data_slot));
    if (image) JS_MarkValue(rt, image->array, mark_func);
}
/* Reading these dictionaries can run page code. Keep no borrowed byte view
 * across them. WebIDL reads dictionary members in lexicographic order. */
int d_image_data_settings(JSContext *ctx, JSValueConst settings)
{
    if (JS_IsUndefined(settings) || JS_IsNull(settings)) return 0;
    if (!JS_IsObject(settings)) {
        JS_ThrowTypeError(ctx, "ImageData settings must be a dictionary");
        return -1;
    }
    bool unsupported = false;
    const char *names[] = {"colorSpace", "pixelFormat"};
    const char *supported[] = {"srgb", "rgba-unorm8"};
    const char *other[] = {"display-p3", "rgba-float16"};
    for (unsigned i = 0; i < 2; ++i) {
        JSValue value = JS_GetPropertyStr(ctx, settings, names[i]);
        if (JS_IsException(value)) return -1;
        if (!JS_IsUndefined(value)) {
            size_t length;
            const char *text = JS_ToCStringLen(ctx, &length, value);
            if (!text) { JS_FreeValue(ctx, value); return -1; }
            bool primary = length == os64_strlen(supported[i]) && !os64_memcmp(text, supported[i], length);
            bool secondary = length == os64_strlen(other[i]) && !os64_memcmp(text, other[i], length);
            JS_FreeCString(ctx, text);
            JS_FreeValue(ctx, value);
            if (!primary && !secondary) {
                JS_ThrowTypeError(ctx, "Invalid ImageData %s", names[i]);
                return -1;
            }
            unsupported |= secondary;
        } else JS_FreeValue(ctx, value);
    }
    if (unsupported) {
        d_error(ctx, "NotSupportedError", "ImageData supports sRGB RGBA8");
        return -1;
    }
    return 0;
}
static int array_bytes(os64_dom_t *dom, JSContext *ctx, JSValueConst array,
                        uint8_t **bytes, size_t *length)
{
    /* Use the engine's getter so an own `buffer` property cannot substitute
     * an unrelated allocation. Resizable/shared buffers are not admitted by
     * this WebIDL buffer-source parameter. */
    JSValue buffer = JS_Call(ctx, dom->crypto_getters[0]->value, array, 0, NULL);
    if (JS_IsException(buffer)) return -1;
    JSValue resizable = JS_Call(ctx, dom->crypto_getters[1]->value, buffer, 0, NULL);
    JS_FreeValue(ctx, buffer);
    if (JS_IsException(resizable)) return -1;
    bool changes_size = JS_ToBool(ctx, resizable);
    JS_FreeValue(ctx, resizable);
    if (changes_size) {
        JS_ThrowTypeError(ctx, "ImageData requires a fixed ArrayBuffer");
        return -1;
    }
    size_t offset = 0, whole = 0;
    buffer = JS_GetTypedArrayBuffer(ctx, array, &offset, length, NULL);
    if (JS_IsException(buffer)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        d_error(ctx, "InvalidStateError", "ImageData buffer is detached");
        return -1;
    }
    uint8_t *data = JS_GetArrayBuffer(ctx, &whole, buffer);
    JS_FreeValue(ctx, buffer);
    if (!data || offset > whole || *length > whole - offset) {
        if (!data) JS_FreeValue(ctx, JS_GetException(ctx));
        d_error(ctx, "InvalidStateError", "ImageData buffer is unavailable");
        return -1;
    }
    *bytes = data + offset;
    return 0;
}
static JSValue make(JSContext *ctx, JSValueConst prototype, JSValue array, uint32_t w, uint32_t h)
{
    JSClassID id = os64_js_class_id(&image_data_slot);
    JSValue object = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, id)
                                           : JS_NewObjectClass(ctx, id);
    if (JS_IsException(object)) { JS_FreeValue(ctx, array); return object; }
    DImageData *image = js_mallocz(ctx, sizeof(*image));
    if (!image) { JS_FreeValue(ctx, array); JS_FreeValue(ctx, object); return JS_EXCEPTION; }
    *image = (DImageData){array, w, h};
    JS_SetOpaque(object, image);
    return object;
}
JSValue d_image_data_new(JSContext *ctx, uint32_t width, uint32_t height)
{
    if (!width || !height) return d_error(ctx, "IndexSizeError", "ImageData dimensions must be nonzero");
    uint64_t length = (uint64_t)width * height;
    if (length > SIZE_MAX / 4 || length > INT32_MAX / 4)
        return JS_ThrowRangeError(ctx, "ImageData is too large");
    JSValueConst args[3] = {JS_NewInt64(ctx, (int64_t)(length * 4)), JS_UNDEFINED, JS_UNDEFINED};
    JSValue array = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_UINT8C);
    if (JS_IsException(array)) return array;
    return make(ctx, JS_UNDEFINED, array, width, height);
}
static JSValue construct(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (!dom) return JS_EXCEPTION;
    if (argc < 2) return JS_ThrowTypeError(ctx, "ImageData needs dimensions or an array and width");
    int kind = d_view_kind(dom, ctx, argv[0]);
    if (kind == -2) return JS_EXCEPTION;
    bool source = kind >= 0;
    if (source && kind != JS_TYPED_ARRAY_UINT8C && kind != JS_TYPED_ARRAY_FLOAT16)
        return JS_ThrowTypeError(ctx, "ImageData requires Uint8ClampedArray");
    uint32_t width, height = 0;
    if (JS_ToUint32(ctx, &width, argv[source ? 1 : 0]) < 0) return JS_EXCEPTION;
    bool has_height = !source || (argc > 2 && !JS_IsUndefined(argv[2]));
    if (has_height && JS_ToUint32(ctx, &height, argv[source ? 2 : 1]) < 0) return JS_EXCEPTION;
    unsigned settings_at = source ? 3 : 2;
    if (d_image_data_settings(ctx, argc > (int)settings_at ? argv[settings_at] : JS_UNDEFINED) < 0)
        return JS_EXCEPTION;
    if (source) {
        if (kind != JS_TYPED_ARRAY_UINT8C)
            return d_error(ctx, "InvalidStateError", "ImageData array does not match RGBA8");
        uint8_t *bytes; size_t length;
        if (array_bytes(dom, ctx, argv[0], &bytes, &length) < 0) return JS_EXCEPTION;
        (void)bytes;
        if (!length || length % 4)
            return d_error(ctx, "InvalidStateError", "ImageData needs complete RGBA pixels");
        if (!width || (length / 4) % width || (length / 4) / width > UINT32_MAX ||
            (has_height && height != (length / 4) / width))
            return d_error(ctx, "IndexSizeError", "ImageData dimensions do not match its array");
        height = (uint32_t)((length / 4) / width);
    } else if (!width || !height)
        return d_error(ctx, "IndexSizeError", "ImageData dimensions must be nonzero");
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return JS_EXCEPTION;
    JSValue object;
    if (source) object = make(ctx, prototype, JS_DupValue(ctx, argv[0]), width, height);
    else {
        object = d_image_data_new(ctx, width, height);
        if (!JS_IsException(object) && JS_IsObject(prototype) && JS_SetPrototype(ctx, object, prototype) < 0) {
            JS_FreeValue(ctx, object); object = JS_EXCEPTION;
        }
    }
    JS_FreeValue(ctx, prototype);
    return object;
}
static JSValue member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                       JSValue *data)
{
    (void)argc; (void)argv;
    if (!d_callback(ctx, data, OS64_JS_ABI_ID)) return JS_EXCEPTION;
    DImageData *image = JS_GetOpaque(self, os64_js_class_id(&image_data_slot));
    if (!image) return JS_ThrowTypeError(ctx, "Expected an ImageData");
    if (magic == 0) return JS_NewUint32(ctx, image->width);
    if (magic == 1) return JS_NewUint32(ctx, image->height);
    if (magic == 2) return JS_DupValue(ctx, image->array);
    return JS_NewString(ctx, magic == 3 ? "srgb" : "rgba-unorm8");
}
int d_image_data_view(os64_dom_t *dom, JSContext *ctx, JSValueConst value, canvas_rgba8_t *out)
{
    DImageData *image = JS_GetOpaque(value, os64_js_class_id(&image_data_slot));
    if (!image) { JS_ThrowTypeError(ctx, "Expected an ImageData"); return -1; }
    uint8_t *bytes; size_t length;
    if (array_bytes(dom, ctx, image->array, &bytes, &length) < 0) return -1;
    if ((uint64_t)image->width * image->height > length / 4) {
        d_error(ctx, "InvalidStateError", "ImageData storage is incomplete"); return -1;
    }
    *out = (canvas_rgba8_t){bytes, image->width, image->height, (size_t)image->width * 4, length};
    return 0;
}
int d_image_data_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    JSClassID id = os64_js_class_id(&image_data_slot);
    const JSClassDef definition = {.class_name = "ImageData", .finalizer = finalize, .gc_mark = mark};
    if (!JS_IsRegisteredClass(dom->engine, id) && JS_NewClass(dom->engine, id, &definition) < 0) return -1;
    JSValue prototype = JS_NewObject(ctx);
    if (JS_IsException(prototype) || !d_retain(dom, ctx, prototype)) return -1;
    JS_SetClassProto(ctx, id, JS_DupValue(ctx, prototype));
    JSValue constructor = d_interface_new(ctx, "ImageData", 2, construct, prototype, JS_UNDEFINED);
    if (JS_IsException(constructor)) return -1;
    if (JS_DefinePropertyValueStr(ctx, global, "ImageData", constructor, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    const char *names[] = {"width", "height", "data", "colorSpace", "pixelFormat"};
    for (unsigned i = 0; i < sizeof(names)/sizeof(*names); ++i)
        if (d_accessor(dom, ctx, prototype, names[i], member, i, false) < 0) return -1;
    return 0;
}

bool d_image_data_parts(JSValueConst value, JSValue *array, uint32_t *width, uint32_t *height)
{
    DImageData *image = JS_GetOpaque(value, os64_js_class_id(&image_data_slot));
    if (!image) return false;
    *array = image->array;
    *width = image->width;
    *height = image->height;
    return true;
}
JSValue d_image_data_from_array(JSContext *ctx, JSValueConst array, uint32_t width, uint32_t height)
{
    return make(ctx, JS_UNDEFINED, JS_DupValue(ctx, array), width, height);
}
