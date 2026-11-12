#ifndef OS64_JS_REALM_CASES_H
#define OS64_JS_REALM_CASES_H
/* The same ownership checks run against the guest DSO and host engines. */
static os64_js_status_t realm_execute(os64_js_realm_t *realm, const char *source,
                                     os64_js_outcome_t *out)
{
    return os64_js_realm_run(realm, source, os64_strlen(source), "realm-test.js", out);
}

static JSValue realm_property(JSContext *context, const char *name)
{
    JSValue global = JS_GetGlobalObject(context);
    JSValue value = JS_GetPropertyStr(context, global, name);
    JS_FreeValue(context, global);
    return value;
}

static void realm_share(JSContext *from, JSContext *to, const char *name)
{
    JSValue global = JS_GetGlobalObject(to);
    check(JS_SetPropertyStr(to, global, name, realm_property(from, name)) == 1,
          "cross-realm fixture value installed");
    JS_FreeValue(to, global);
}

typedef struct {
    JSContext *context;
    unsigned exceptions, rejected, handled;
} RealmReports;

static void realm_exception(void *opaque, JSContext *context, JSValue error)
{
    RealmReports *reports = opaque;
    (void)error;
    check(context == reports->context, "exception callback receives owning context");
    reports->exceptions++;
}
static uint32_t realm_rejected(void *opaque, JSContext *context, JSValue promise)
{
    RealmReports *reports = opaque;
    (void)promise;
    check(context == reports->context, "rejection callback receives owning context");
    return ++reports->rejected;
}
static void realm_handled(void *opaque, JSContext *context, JSValue promise)
{
    RealmReports *reports = opaque;
    (void)promise;
    check(context == reports->context, "handled callback receives selected context");
    reports->handled++;
}
static void realm_host(os64_js_realm_t *realm, RealmReports *reports)
{
    os64_js_outcome_t out;
    const os64_js_script_host_t host = {reports, realm_exception, realm_handled, realm_rejected};
    check(os64_js_realm_set_script_host(realm, OS64_JS_ABI_ID, &host, &out) == OS64_JS_OK,
          "realm script host configured");
}
static bool realm_take(os64_js_realm_t *realm, JSContext *context, int expected)
{
    JSValue promise, reason;
    uint32_t tag;
    bool taken = os64_js_realm_take_rejection(realm, OS64_JS_ABI_ID, &promise, &reason, &tag);
    if (taken) {
        int32_t number = 0;
        check(JS_ToInt32(context, &number, reason) == 0 && number == expected && tag != 0,
              "realm takes its own rejection in order with host tag");
    }
    JS_FreeValue(context, promise);
    JS_FreeValue(context, reason);
    return taken;
}

static void realm_identity_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (!runtime) return;
    os64_js_outcome_t out;
    os64_js_realm_t *a = os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out), *b = NULL;
    check(a != NULL && os64_js_realm_create(runtime, OS64_JS_ABI_ID, &b, &out) == OS64_JS_OK,
          "additional realm created");
    if (!b) { os64_js_destroy(runtime); return; }
    JSContext *ac = os64_js_context(runtime, OS64_JS_ABI_ID, &out);
    JSContext *bc = os64_js_realm_context(b, OS64_JS_ABI_ID, &out);
    check(ac != bc && JS_GetRuntime(ac) == JS_GetRuntime(bc), "distinct contexts share one engine");
    check(os64_js_realm_from_context(runtime, ac, OS64_JS_ABI_ID, &out) == a &&
          os64_js_realm_from_context(runtime, bc, OS64_JS_ABI_ID, &out) == b,
          "native context membership resolves both realms");
    check(os64_js_realm_context(b, "wrong", &out) == NULL && out.status == OS64_JS_ABI_MISMATCH,
          "realm binding ABI checked");
    os64_js_runtime_t *other = create_fixture(&config);
    if (other) {
        JSContext *foreign = os64_js_context(other, OS64_JS_ABI_ID, &out);
        check(os64_js_realm_from_context(runtime, foreign, OS64_JS_ABI_ID, &out) == NULL &&
              out.status == OS64_JS_BAD_ARGUMENT, "foreign context rejected without trusting opaque");
        os64_js_destroy(other);
    }
    check(realm_execute(a, "globalThis.id=1;globalThis.AObject=Object;globalThis.APromise=Promise;"
                        "if(typeof SharedArrayBuffer!=='undefined')throw 1", &out) == OS64_JS_OK,
          "primary global and intrinsics");
    realm_share(ac, bc, "AObject"); realm_share(ac, bc, "APromise");
    check(os64_js_realm_install_intl(b, "en-US", &out) == OS64_JS_OK,
          "unused child installs Intl after parent executed");
    check(realm_execute(b, "if(typeof id!=='undefined'||Object===AObject||Promise===APromise||"
                        "typeof SharedArrayBuffer!=='undefined'||typeof Intl!=='object')throw 2;"
                        "globalThis.id=2", &out) == OS64_JS_OK, "child globals and intrinsics are independent");
    check(os64_js_run(runtime, "if(id!==1)throw 3", sizeof("if(id!==1)throw 3") - 1, "legacy", &out) == OS64_JS_OK,
          "legacy execution still selects primary");
    check(realm_execute(b, "if(id!==2)throw 4", &out) == OS64_JS_OK, "child execution selects child");
    os64_js_bytecode_t written = {.keep = true};
    const char *source = "globalThis.fromCache=(globalThis.id||0)+40";
    check(os64_js_realm_run_bytecode(a, source, os64_strlen(source), "cache-a", &written, &out) == OS64_JS_OK &&
          written.kept != NULL, "primary emits reusable bytecode");
    os64_js_bytecode_t read = {.blob = written.kept, .blob_length = written.kept_length};
    check(os64_js_realm_run_bytecode(b, source, os64_strlen(source), "cache-b", &read, &out) == OS64_JS_OK &&
          read.from_blob, "same bytecode executes in child realm");
    os64_free(written.kept);
    check(realm_execute(a, "if(fromCache!==41)throw 5", &out) == OS64_JS_OK &&
          realm_execute(b, "if(fromCache!==42)throw 6", &out) == OS64_JS_OK, "bytecode binds destination globals");
    os64_js_cancel(runtime);
    check(realm_execute(b, "true", &out) == OS64_JS_CANCELLED &&
          realm_execute(a, "true", &out) == OS64_JS_FAILED_RUNTIME, "cancellation fails the whole runtime");
    os64_js_destroy(runtime);
}

static void realm_rejection_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (!runtime) return;
    os64_js_outcome_t out;
    os64_js_realm_t *a = os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out), *b = NULL;
    check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &b, &out) == OS64_JS_OK, "rejection child created");
    if (!b) { os64_js_destroy(runtime); return; }
    JSContext *ac = os64_js_realm_context(a, OS64_JS_ABI_ID, &out);
    JSContext *bc = os64_js_realm_context(b, OS64_JS_ABI_ID, &out);
    RealmReports ar = {.context = ac}, br = {.context = bc};
    realm_host(a, &ar); realm_host(b, &br);
    check(realm_execute(b, "globalThis.p=Promise.reject(21)", &out) == OS64_JS_OK, "child rejection retained");
    realm_share(bc, ac, "p");
    check(realm_execute(a, "Promise.prototype.then.call(p,undefined,()=>{})", &out) == OS64_JS_OK,
          "parent intrinsic then handles child promise");
    check(!realm_take(b, bc, 0) && !realm_take(a, ac, 0) && ar.handled == 0 && br.handled == 0,
          "handled-before-take removes child record by promise identity");
    check(realm_execute(b, "globalThis.p=Promise.reject(22)", &out) == OS64_JS_OK, "await rejection retained");
    realm_share(bc, ac, "p");
    check(realm_execute(a, "(async()=>{try{await p}catch(e){}})()", &out) == OS64_JS_OK &&
          !realm_take(b, bc, 0), "parent await handles child rejection before take");
    check(realm_execute(a, "Promise.reject(11)", &out) == OS64_JS_OK &&
          realm_execute(b, "Promise.reject(31);Promise.reject(32)", &out) == OS64_JS_OK &&
          realm_execute(a, "Promise.reject(12)", &out) == OS64_JS_OK, "interleaved realm rejections queued");
    check(realm_take(b, bc, 31) && realm_take(a, ac, 11) && realm_take(b, bc, 32) &&
          realm_take(a, ac, 12) && !realm_take(b, bc, 0), "taking one realm preserves sibling order");
    check(realm_execute(b, "globalThis.p=Promise.reject(41)", &out) == OS64_JS_OK && realm_take(b, bc, 41),
          "host takes child rejection before handler");
    realm_share(bc, ac, "p");
    check(realm_execute(a, "Promise.prototype.then.call(p,undefined,()=>{})", &out) == OS64_JS_OK &&
          ar.handled == 1 && br.handled == 0, "handled-after-take uses attaching intrinsic context");
    check(realm_execute(b, "throw Error('child-error')", &out) == OS64_JS_EXCEPTION &&
          br.exceptions == 1 && ar.exceptions == 0, "child exception reaches child host");
    check(os64_js_realm_set_script_host(a, OS64_JS_ABI_ID, NULL, &out) == OS64_JS_OK &&
          realm_execute(b, "Promise.reject(51)", &out) == OS64_JS_OK &&
          realm_execute(a, "Promise.reject(52)", &out) == OS64_JS_UNHANDLED_REJECTION &&
          realm_take(b, bc, 51), "unhosted primary judges only its own rejection");
    os64_js_destroy(runtime);
}

static void realm_capacity_cases(void)
{
    os64_js_config_t config = fixture_config();
    config.limits.memory_bytes = 64 * 1024 * 1024;
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (!runtime) return;
    os64_js_outcome_t out;
    os64_js_realm_t *last = NULL;
    for (unsigned i = 1; i < OS64_JS_MAX_REALMS; i++) {
        if (os64_js_realm_create(runtime, OS64_JS_ABI_ID, &last, &out) != OS64_JS_OK) {
            check(false, "all advertised realm slots fit fixture budget");
            os64_js_destroy(runtime); return;
        }
        if (i % 2 != 0)
            check(os64_js_realm_retire(last, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
                  "retired descriptors retain their place in the cumulative slot limit");
    }
    size_t before = os64_js_heap_bytes(runtime);
    os64_js_realm_t *extra = last;
    check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &extra, &out) == OS64_JS_REALM_LIMIT &&
          extra == NULL && os64_js_heap_bytes(runtime) == before, "realm cap refuses without allocating");
    check(os64_js_realm_preflight(runtime, OS64_JS_ABI_ID, 1, &out) == OS64_JS_REALM_LIMIT &&
          os64_js_realm_create_with_headroom(runtime, OS64_JS_ABI_ID, 1, &extra, &out) == OS64_JS_REALM_LIMIT &&
          extra == NULL && os64_js_heap_bytes(runtime) == before,
          "headroom admission retains the independent slot limit");
    check(realm_execute(last, "true", &out) == OS64_JS_OK &&
          os64_js_run(runtime, "true", 4, "primary", &out) == OS64_JS_OK,
          "realm admission refusal leaves both realms usable");
    os64_js_destroy(runtime);
}

/* Admission runs inside an existing parent entry, without taking its outcome
 * storage or checkpoint. The JavaScript fixture checks FIFO delivery after it. */
static os64_js_runtime_t *admission_runtime;
static os64_js_realm_t *admission_child;
static os64_js_outcome_t *admission_outer;
static JSValue realm_admission_nested(JSContext *context, JSValueConst self,
                                      int argc, JSValueConst *argv)
{
    (void)context; (void)self; (void)argc; (void)argv;
    os64_js_outcome_t out, saved = *admission_outer;
    os64_js_realm_t *child = NULL;
    size_t bytes = os64_js_heap_bytes(admission_runtime);
    check(os64_js_realm_preflight(admission_runtime, OS64_JS_ABI_ID, 1, admission_outer) == OS64_JS_BUSY &&
          os64_memcmp(&saved, admission_outer, sizeof(saved)) == 0,
          "admission protects enclosing outcome storage");
    check(os64_js_realm_create_with_headroom(admission_runtime, OS64_JS_ABI_ID, SIZE_MAX,
              &child, admission_outer) == OS64_JS_BUSY &&
          os64_memcmp(&saved, admission_outer, sizeof(saved)) == 0,
          "guarded creation protects enclosing outcome storage");
    check(os64_js_realm_create_with_headroom(admission_runtime, OS64_JS_ABI_ID, SIZE_MAX,
              &child, &out) == OS64_JS_ADMISSION_REFUSED && child == NULL &&
          out.limit == OS64_JS_LIMIT_NONE && os64_js_heap_bytes(admission_runtime) == bytes,
          "nested headroom refusal consumes no engine memory");
    check(os64_js_realm_create_with_headroom(admission_runtime, OS64_JS_ABI_ID, 256 * 1024,
              &admission_child, &out) == OS64_JS_OK && out.jobs_pending,
          "nested admitted child leaves earlier parent jobs pending");
    check(os64_memcmp(&saved, admission_outer, sizeof(saved)) == 0,
          "refused and admitted child setup preserve parent outcome");
    return JS_UNDEFINED;
}

static void realm_admission_cases(void)
{
    check(os64_js_heap_headroom(NULL) == 0, "null runtime has no headroom");
    for (unsigned policy = 0; policy < 2; policy++) {
        os64_js_config_t config = fixture_config();
        config.limits.memory_bytes = 1024 * 1024;
        os64_js_runtime_t *runtime = NULL;
        os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config, policy ? OS64_JS_TEARDOWN_RECLAIM : OS64_JS_TEARDOWN_FATAL,
                  OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK, "admission fixture created");
        if (!runtime) continue;
        size_t headroom = os64_js_heap_headroom(runtime), bytes = os64_js_heap_bytes(runtime);
        check(headroom > 0 && headroom + bytes < config.limits.memory_bytes,
              "engine headroom accounts for wrapper storage outside engine charge");
        check(os64_js_realm_preflight(runtime, OS64_JS_ABI_ID, headroom, &out) == OS64_JS_OK &&
              os64_js_realm_preflight(runtime, OS64_JS_ABI_ID, headroom + 1, &out) == OS64_JS_ADMISSION_REFUSED &&
              os64_js_heap_bytes(runtime) == bytes && os64_js_heap_headroom(runtime) == headroom,
              "exact headroom accepted and one byte beyond refused without allocation");
        check(os64_js_realm_preflight(runtime, OS64_JS_ABI_ID, 0, &out) == OS64_JS_BAD_ARGUMENT &&
              os64_js_realm_preflight(runtime, "os64-js/3;quickjs=2026-06-04;lp64;value=16;limb=64;atomics=0", 1, &out) == OS64_JS_ABI_MISMATCH,
              "preflight requires positive reserve and current ABI");
        os64_js_realm_t *child = NULL;
        JSContext *context = os64_js_context(runtime, OS64_JS_ABI_ID, &out);
        const uint8_t byte = 1;
        JSValue held = JS_NewArrayBufferCopy(context, &byte, 1);
        check(!JS_IsException(held) && os64_js_heap_headroom(runtime) < headroom,
              "new retained allocation consumes preflight headroom");
        check(os64_js_realm_create_with_headroom(runtime, OS64_JS_ABI_ID, headroom, &child, &out) == OS64_JS_ADMISSION_REFUSED && child == NULL,
              "creation rechecks a stale admission snapshot");
        JS_FreeValue(context, held);
        check(os64_js_realm_create_with_headroom(runtime, OS64_JS_ABI_ID, 1, NULL, &out) == OS64_JS_BAD_ARGUMENT,
              "guarded creation requires handle output");
        admission_runtime = runtime; admission_child = NULL; admission_outer = &out;
        check(install_native(runtime, "admitChild", realm_admission_nested) == 1,
              "nested admission fixture installed");
        const char *source = "globalThis.order=[];Promise.resolve().then(()=>order.push(1));"
                             "admitChild();if(order.length)throw 'early checkpoint';"
                             "Promise.resolve().then(()=>order.push(2));order.push(0)";
        check(os64_js_run(runtime, source, os64_strlen(source), "admission-parent", &out) == OS64_JS_OK &&
              out.jobs_executed == 2, "parent jobs use one enclosing turn after nested admission");
        check(os64_js_run(runtime, "if(order.join()!=='0,1,2')throw order", sizeof("if(order.join()!=='0,1,2')throw order") - 1,
                  "admission-order", &out) == OS64_JS_OK,
              "parent queue order survives nested refusal and successful admission");
        if (admission_child)
            check(realm_execute(admission_child, "globalThis.sibling=42", &out) == OS64_JS_OK,
                  "admitted sibling runs after parent task");
        unsigned admitted = 0;
        os64_js_status_t result = OS64_JS_OK;
        while (admitted < OS64_JS_MAX_REALMS) {
            result = os64_js_realm_create_with_headroom(runtime, OS64_JS_ABI_ID, 256 * 1024, &child, &out);
            if (result != OS64_JS_OK) break;
            admitted++;
        }
        check(result == OS64_JS_ADMISSION_REFUSED && child == NULL && admitted > 0,
              "small heap reaches local headroom refusal before sticky OOM or slot limit");
        check(os64_js_run(runtime, "true", 4, "admission-survivor", &out) == OS64_JS_OK &&
              admission_child && realm_execute(admission_child, "if(sibling!==42)throw 1", &out) == OS64_JS_OK,
              "headroom exhaustion preserves primary and existing sibling state");
        os64_js_cancel(runtime);
        check(os64_js_realm_preflight(runtime, OS64_JS_ABI_ID, SIZE_MAX, &out) == OS64_JS_FAILED_RUNTIME &&
              os64_js_heap_headroom(runtime) > 0,
              "available headroom does not hide runtime cancellation");
        os64_js_destroy(runtime);
        admission_runtime = NULL; admission_child = NULL; admission_outer = NULL;
    }
    os64_js_config_t config = fixture_config();
    config.limits.jobs_per_turn = 2;
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (!runtime) return;
    os64_js_outcome_t out;
    admission_runtime = runtime; admission_child = NULL; admission_outer = &out;
    check(install_native(runtime, "admitChild", realm_admission_nested) == 1,
          "checkpoint admission fixture installed");
    const char *source = "Promise.resolve().then(()=>admitChild());"
                         "Promise.resolve().then(()=>1);Promise.resolve().then(()=>2)";
    check(os64_js_run(runtime, source, os64_strlen(source), "admission-job-budget", &out) == OS64_JS_LIMIT &&
          out.limit == OS64_JS_LIMIT_JOBS && out.jobs_executed == 2,
          "nested admission during a job cannot reset enclosing executed-job count");
    os64_js_destroy(runtime);
    admission_runtime = NULL; admission_child = NULL; admission_outer = NULL;
}

typedef struct { const char *source; unsigned resolves, loads; const char *prefix; } RealmLoader;
static char *realm_resolve(void *opaque, const char *base, const char *specifier)
{
    RealmLoader *loader = opaque;
    (void)base;
    loader->resolves++;
    size_t prefix = os64_strlen(loader->prefix), n = os64_strlen(specifier) + 1;
    char *name = os64_malloc(prefix + n);
    if (name) {
        os64_memcpy(name, loader->prefix, prefix);
        os64_memcpy(name + prefix, specifier, n);
    }
    return name;
}
static bool realm_load(void *opaque, const char *name, const char **bytes, size_t *length)
{
    RealmLoader *loader = opaque;
    check(fixture_contains(name, loader->prefix) == name, "module resolver used its owning provider namespace");
    loader->loads++;
    *bytes = loader->source;
    *length = os64_strlen(*bytes);
    return true;
}
static void realm_module_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (!runtime) return;
    os64_js_outcome_t out;
    os64_js_realm_t *a = os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out), *b = NULL;
    check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &b, &out) == OS64_JS_OK, "module child created");
    if (!b) { os64_js_destroy(runtime); return; }
    JSContext *ac = os64_js_realm_context(a, OS64_JS_ABI_ID, &out);
    JSContext *bc = os64_js_realm_context(b, OS64_JS_ABI_ID, &out);
    RealmReports ar = {.context = ac}, br = {.context = bc};
    realm_host(a, &ar); realm_host(b, &br);
    RealmLoader al = {"export const number=1", 0, 0, "parent/"}, bl = {"export const number=2", 0, 0, "child/"};
    os64_js_module_loader_t loader = {&al, realm_resolve, realm_load};
    check(os64_js_realm_set_module_loader(a, OS64_JS_ABI_ID, &loader, &out) == OS64_JS_OK,
          "parent loader installed");
    loader.opaque = &bl;
    check(os64_js_realm_set_module_loader(b, OS64_JS_ABI_ID, &loader, &out) == OS64_JS_OK,
          "child loader installed");
    const char *source = "import {number} from 'dependency';globalThis.number=number;globalThis.meta=import.meta.url";
    os64_js_module_t *am = NULL, *bm = NULL;
    check(os64_js_realm_module_compile(a, OS64_JS_ABI_ID, source, os64_strlen(source), "same-url", "parent-url", &am, &out) == OS64_JS_OK &&
          os64_js_realm_module_compile(b, OS64_JS_ABI_ID, source, os64_strlen(source), "same-url", "child-url", &bm, &out) == OS64_JS_OK,
          "same module name compiled independently in two realms");
    check(os64_js_realm_module_release(a, OS64_JS_ABI_ID, bm, &out) == OS64_JS_BAD_ARGUMENT,
          "wrong realm cannot release child module");
    check(os64_js_task_begin(runtime, OS64_JS_ABI_ID, "module task", &out) == OS64_JS_OK,
          "shared module task starts");
    check(os64_js_realm_module_run(a, OS64_JS_ABI_ID, bm, &out) == OS64_JS_BAD_ARGUMENT,
          "wrong realm cannot run child module");
    check(os64_js_realm_module_run(b, OS64_JS_ABI_ID, bm, &out) == OS64_JS_OK &&
          os64_js_realm_module_run(a, OS64_JS_ABI_ID, am, &out) == OS64_JS_OK &&
          os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK, "shared task runs both module graphs");
    check(al.loads == 1 && bl.loads == 1 &&
          realm_execute(a, "if(number!==1||meta!=='parent-url')throw 1", &out) == OS64_JS_OK &&
          realm_execute(b, "if(number!==2||meta!=='child-url')throw 2", &out) == OS64_JS_OK,
          "module identity, provider and metadata belong to realm");
    check(realm_execute(b, "globalThis.borrowed=()=>import('borrowed-dependency').then(m=>{globalThis.borrowedResult=m.number})", &out) == OS64_JS_OK,
          "child function with dynamic import created");
    realm_share(bc, ac, "borrowed");
    check(realm_execute(a, "borrowed()", &out) == OS64_JS_OK && bl.loads == 2 && al.loads == 1 &&
          realm_execute(b, "if(borrowedResult!==2)throw 3", &out) == OS64_JS_OK,
          "borrowed function imports through its code realm");
    check(os64_js_realm_set_module_loader(b, OS64_JS_ABI_ID, NULL, &out) == OS64_JS_OK &&
          realm_execute(a, "import('parent-still-live').then(m=>{if(m.number!==1)throw 4})", &out) == OS64_JS_OK &&
          al.loads == 2, "clearing child loader preserves parent loader");
    /* A late child failure is surfaced while the primary owns the task.
     * Poisoning the public then also proves the captured intrinsic is used. */
    check(realm_execute(b, "Promise.prototype.then=()=>{throw Error('poisoned then')}", &out) == OS64_JS_OK,
          "child public promise then poisoned after pristine capture");
    const char *late = "await new Promise(r=>{globalThis.finish=r});throw Error('child-late')";
    os64_js_module_t *pending = NULL;
    check(os64_js_realm_module_compile(b, OS64_JS_ABI_ID, late, os64_strlen(late), "child-late-module", NULL, &pending, &out) == OS64_JS_OK,
          "late child module compiled");
    check(os64_js_task_begin(runtime, OS64_JS_ABI_ID, "parent-turn", &out) == OS64_JS_OK &&
          os64_js_realm_module_run(b, OS64_JS_ABI_ID, pending, &out) == OS64_JS_OK &&
          os64_js_checkpoint(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
          "child module awaits inside parent task");
    JSValue finish = realm_property(bc, "finish");
    check(os64_js_realm_call(a, OS64_JS_ABI_ID, finish, JS_UNDEFINED, 0, NULL, NULL, &out) == OS64_JS_OK,
          "parent task calls retained child resolver");
    JS_FreeValue(bc, finish);
    os64_js_status_t late_status = os64_js_checkpoint(runtime, OS64_JS_ABI_ID, &out);
    check(late_status == OS64_JS_EXCEPTION &&
          os64_strcmp(out.source_name, "child-late-module") == 0 && br.exceptions == 1 && ar.exceptions == 0,
          "late module failure keeps child host and source under parent checkpoint");
    check(os64_js_realm_module_state(b, pending, &out) == OS64_JS_MODULE_REJECTED &&
          os64_strcmp(out.source_name, "child-late-module") == 0, "late failure is retained on child handle");
    check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
          "late module failure is owed once");
    os64_js_destroy(runtime);
}

static os64_js_runtime_t *realm_nested_runtime;
static os64_js_realm_t *realm_nested_child;
static os64_js_outcome_t *realm_outer_out;
static os64_js_outcome_t *realm_nested_setup_out;
static unsigned realm_nested_traps;
static JSValue realm_nested_probe(JSContext *context, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    os64_js_outcome_t out, before = *realm_nested_setup_out;
    check(os64_js_realm_install_output(realm_nested_child, 1, OS64_JS_OUTPUT_CONSOLE, &out) == OS64_JS_BAD_ARGUMENT,
          "setup trap cannot recursively reinstall its facility");
    check(os64_js_realm_install_output(realm_nested_child, 1, OS64_JS_OUTPUT_CONSOLE, realm_nested_setup_out) == OS64_JS_BUSY &&
          os64_memcmp(&before, realm_nested_setup_out, sizeof(before)) == 0,
          "setup trap cannot overwrite its installer's outcome");
    before = *realm_outer_out;
    check(os64_js_realm_install_intl(realm_nested_child, "en-US", realm_outer_out) == OS64_JS_BUSY &&
          os64_memcmp(&before, realm_outer_out, sizeof(before)) == 0,
          "setup trap cannot overwrite outer parent outcome");
    realm_nested_traps++;
    return JS_NewBool(context, true);
}
static JSValue realm_nested_create(JSContext *context, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    os64_js_outcome_t out, before = *realm_outer_out;
    os64_js_realm_t *aliased = NULL;
    check(os64_js_realm_create(realm_nested_runtime, OS64_JS_ABI_ID, &aliased, realm_outer_out) == OS64_JS_BUSY &&
          os64_memcmp(&before, realm_outer_out, sizeof(before)) == 0, "nested setup preserves aliased outer outcome");
    check(os64_js_realm_create(realm_nested_runtime, OS64_JS_ABI_ID, &realm_nested_child, &out) == OS64_JS_OK,
          "native callback creates child under outer guard");
    if (!realm_nested_child) return JS_ThrowInternalError(context, "child creation failed");
    check(os64_js_realm_install_intl(realm_nested_child, "en-US", &out) == OS64_JS_OK,
          "native callback installs child Intl under inherited budget");
    const os64_js_script_host_t host = {0};
    const os64_js_module_loader_t loader = {0};
    check(os64_js_realm_set_script_host(realm_nested_child, OS64_JS_ABI_ID, &host, &out) == OS64_JS_OK &&
          os64_js_realm_set_module_loader(realm_nested_child, OS64_JS_ABI_ID, &loader, &out) == OS64_JS_OK,
          "first child service configuration allowed inside parent call");
    check(os64_js_realm_set_script_host(realm_nested_child, OS64_JS_ABI_ID, NULL, &out) == OS64_JS_BUSY &&
          os64_js_realm_set_module_loader(realm_nested_child, OS64_JS_ABI_ID, &loader, &out) == OS64_JS_BUSY,
          "nested setup cannot replace or clear configured services");
    check(realm_execute(realm_nested_child, "true", &out) == OS64_JS_BUSY,
          "creation exception does not allow wrapper execution reentry");
    JSContext *child = os64_js_realm_context(realm_nested_child, OS64_JS_ABI_ID, &out);
    JSValue global = JS_GetGlobalObject(child);
    check(JS_SetPropertyStr(child, global, "probe", JS_NewCFunction(child, realm_nested_probe, "probe", 0)) == 1,
          "native binding supplies setup trap between creation and install");
    JS_FreeValue(child, global);
    const char *traps = "globalThis.console=new Proxy({}, {defineProperty(t,k,d){probe();"
        "Promise.resolve().then(()=>globalThis.setupJob=1);return Reflect.defineProperty(t,k,d)}})";
    JSValue value = JS_Eval(child, traps, os64_strlen(traps), "native-setup-traps", JS_EVAL_TYPE_GLOBAL);
    check(!JS_IsException(value), "child host setup proxy prepared");
    JS_FreeValue(child, value);
    realm_nested_setup_out = &out;
    realm_nested_traps = 0;
    check(os64_js_realm_install_output(realm_nested_child, 1, OS64_JS_OUTPUT_CONSOLE, &out) == OS64_JS_OK &&
          realm_nested_traps > 0 && out.jobs_pending, "nested output setup protects guard and leaves jobs to parent turn");
    realm_nested_setup_out = NULL;
    value = JS_Eval(child, "globalThis.childValue=42", 24, "native-child", JS_EVAL_TYPE_GLOBAL);
    check(!JS_IsException(value), "binding may evaluate child inside existing native guard");
    JS_FreeValue(child, value);
    return JS_UNDEFINED;
}
static void realm_nested_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (!runtime) return;
    realm_nested_runtime = runtime; realm_nested_child = NULL;
    os64_js_outcome_t out;
    realm_outer_out = &out;
    check(install_native(runtime, "makeChild", realm_nested_create) == 1, "child creation native installed");
    check(os64_js_run(runtime, "makeChild();globalThis.parentDone=1", sizeof("makeChild();globalThis.parentDone=1") - 1, "nested-parent", &out) == OS64_JS_OK,
          "parent continues after nested realm setup");
    if (realm_nested_child)
        check(realm_execute(realm_nested_child, "if(childValue!==42||setupJob!==1||typeof Intl!=='object')throw 1", &out) == OS64_JS_OK,
              "nested child remains available after parent turn");
    os64_js_destroy(runtime);
    realm_nested_runtime = NULL; realm_nested_child = NULL; realm_outer_out = NULL;
}

static JSValue realm_throw_job(JSContext *context, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    return JS_ThrowTypeError(context, "job-in-child");
}
static void realm_turn_cases(void)
{
    os64_js_config_t config = fixture_config();
    for (unsigned mode = 0; mode < 2; mode++) {
        config.limits.jobs_per_turn = mode ? 1 : 100;
        os64_js_runtime_t *runtime = create_fixture(&config);
        if (!runtime) return;
        os64_js_outcome_t out;
        os64_js_realm_t *a = os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out), *b = NULL;
        check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &b, &out) == OS64_JS_OK, "turn child created");
        if (!b) { os64_js_destroy(runtime); return; }
        JSContext *ac = os64_js_realm_context(a, OS64_JS_ABI_ID, &out);
        JSContext *bc = os64_js_realm_context(b, OS64_JS_ABI_ID, &out);
        RealmReports ar = {.context = ac}, br = {.context = bc};
        realm_host(a, &ar); realm_host(b, &br);
        if (mode == 0) {
            check(os64_js_task_begin(runtime, OS64_JS_ABI_ID, "parent-job-task", &out) == OS64_JS_OK &&
                  JS_EnqueueJob(bc, realm_throw_job, 0, NULL) == 0 &&
                  os64_js_checkpoint(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_EXCEPTION &&
                  ar.exceptions == 0 && br.exceptions == 1 &&
                  fixture_contains(out.message, "job-in-child") != NULL,
                  "child job exception drained by parent reaches child host");
            check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
                  "task completes after reported job exception");
        } else {
            const char *source = "globalThis.work=()=>{Promise.resolve().then(()=>{});return false}";
            check(realm_execute(a, source, &out) == OS64_JS_OK && realm_execute(b, source, &out) == OS64_JS_OK,
                  "shared job-limit callbacks prepared");
            JSValue aw = realm_property(ac, "work"), bw = realm_property(bc, "work");
            bool returned_false = false;
            check(os64_js_realm_task_begin(b, OS64_JS_ABI_ID, "child-owner-task", &out) == OS64_JS_OK &&
                  os64_js_realm_call(a, OS64_JS_ABI_ID, aw, JS_UNDEFINED, 0, NULL, &returned_false, &out) == OS64_JS_OK &&
                  returned_false && os64_js_realm_call(b, OS64_JS_ABI_ID, bw, JS_UNDEFINED, 0, NULL, NULL, &out) == OS64_JS_OK,
                  "one child-owned task calls both realms without reset");
            JS_FreeValue(ac, aw); JS_FreeValue(bc, bw);
            check(os64_js_checkpoint(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_LIMIT &&
                  out.limit == OS64_JS_LIMIT_JOBS && out.jobs_executed == 1,
                  "realm crossing shares aggregate job cap");
            check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_LIMIT &&
                  !os64_js_turn_open(runtime) && realm_execute(a, "true", &out) == OS64_JS_FAILED_RUNTIME &&
                  realm_execute(b, "true", &out) == OS64_JS_FAILED_RUNTIME,
                  "task-end closes failed shared task and both realms refuse execution");
        }
        os64_js_destroy(runtime);
    }
}

static void realm_failure_order_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (!runtime) return;
    os64_js_outcome_t out;
    os64_js_realm_t *a = os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out), *b = NULL;
    check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &b, &out) == OS64_JS_OK, "ordered failure child created");
    if (!b) { os64_js_destroy(runtime); return; }
    JSContext *ac = os64_js_realm_context(a, OS64_JS_ABI_ID, &out);
    JSContext *bc = os64_js_realm_context(b, OS64_JS_ABI_ID, &out);
    RealmReports ar = {.context = ac}, br = {.context = bc};
    realm_host(a, &ar); realm_host(b, &br);
    const char *source = "await Promise.resolve();throw Error('ordered')";
    os64_js_module_t *am = NULL, *bm = NULL;
    check(os64_js_realm_module_compile(a, OS64_JS_ABI_ID, source, os64_strlen(source), "parent-second", NULL, &am, &out) == OS64_JS_OK &&
          os64_js_realm_module_compile(b, OS64_JS_ABI_ID, source, os64_strlen(source), "child-first", NULL, &bm, &out) == OS64_JS_OK,
          "ordered failures compiled across realms");
    check(os64_js_task_begin(runtime, OS64_JS_ABI_ID, "ordered-parent-task", &out) == OS64_JS_OK &&
          os64_js_realm_module_run(b, OS64_JS_ABI_ID, bm, &out) == OS64_JS_OK &&
          os64_js_realm_module_run(a, OS64_JS_ABI_ID, am, &out) == OS64_JS_OK,
          "child failure queued before primary failure");
    check(os64_js_checkpoint(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_EXCEPTION &&
          os64_strcmp(out.source_name, "child-first") == 0 && ar.exceptions == 1 && br.exceptions == 1,
          "first surfaced module failure wins over realm table order");
    check(os64_js_checkpoint(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_EXCEPTION &&
          os64_strcmp(out.source_name, "parent-second") == 0,
          "next checkpoint receives next owed failure without new jobs");
    check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK && ar.exceptions == 1 && br.exceptions == 1,
          "each realm failure is delivered once");
    os64_js_destroy(runtime);
}


#include "retirement.h"
#include "staging.h"

static void realm_cases(void)
{
    retirement_cases();
    staging_cases();
    realm_identity_cases();
    realm_rejection_cases();
    realm_capacity_cases();
    realm_admission_cases();
    realm_module_cases();
    realm_nested_cases();
    realm_turn_cases();
    realm_failure_order_cases();
}
#endif
