/* Guest measurements use the public runtime/binding APIs and proc reports. */
#include "os64/os64.h"
#include "os64/js_engine.h"
#include "quickjs.h"

/* flags: bit 0 sleeps instead of yielding; bit 1 perturbs the saved state
 * comparison as a negative control. */
extern unsigned js_measure_fp_pause(unsigned worker, unsigned flags);

static unsigned checks, failures, start_workers;
typedef struct {
    uintptr_t bottom, top, lowest;
    unsigned depth, padded, heap_sample;
    int64_t engine_bytes;
    unsigned pauses, pause_failures, peer_moves;
    os64_js_status_t status;
} measurement_t;
/* Each worker owns its slot; peer pause counts use atomic accesses. */
static measurement_t measurements[2];

static void check(bool passed, const char *name)
{
    checks++;
    if (!passed) { failures++; os64_printf("FAIL J2: %s\n", name); }
}

static uintptr_t stack_pointer(void)
{
    uintptr_t value;
    __asm__ volatile("mov %0, rsp" : "=r"(value));
    return value;
}

static bool hex_address(const char **text, uintptr_t *value)
{
    const char *p = *text;
    *value = 0;
    if (p[0] == '0' && p[1] == 'x') p += 2;
    unsigned digits = 0;
    while (*p) {
        unsigned c = (unsigned char)*p, n;
        if (c >= '0' && c <= '9') n = c - '0';
        else if (c >= 'a' && c <= 'f') n = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') n = c - 'A' + 10;
        else break;
        if (++digits > 16) return false;
        *value = *value * 16 + n;
        p++;
    }
    *text = p;
    return digits != 0;
}

static bool stack_bounds(measurement_t *m)
{
    int64_t handle = os64_open("/proc/self/maps", "r");
    if (handle < 0) return false;
    char line[512];
    uintptr_t current = stack_pointer();
    bool found = false;
    while (os64_readline((int32_t)handle, line, sizeof(line)) == 1) {
        const char *p = line;
        uintptr_t low, high;
        if (!hex_address(&p, &low) || *p++ != '-' || !hex_address(&p, &high)) continue;
        const char *label = os64_strchr(p, '[');
        if (current >= low && current < high && label && os64_strlen(label) >= 7 &&
            os64_memcmp(label, "[stack:", 7) == 0) {
            m->bottom = low; m->top = high; m->lowest = high;
            found = true;
            break;
        }
    }
    os64_close((int32_t)handle);
    return found;
}

static os64_js_config_t settings(size_t stack)
{
    os64_js_config_t config = {.limits = os64_js_default_limits()};
    config.limits.stack_bytes = stack;
    return config;
}

static void sample(measurement_t *m, unsigned depth)
{
    uintptr_t current = stack_pointer();
    if (current < m->lowest) m->lowest = current;
    if (depth > m->depth) m->depth = depth;
}

__attribute__((noinline)) static void padded_sample(measurement_t *m, unsigned depth)
{
    volatile unsigned char native_frame[64 * 1024];
    native_frame[0] = 1;
    native_frame[sizeof(native_frame) - 1] = 2;
    sample(m, depth);
    /* Retain this representative native frame across the sample. */
    __asm__ volatile("" : : "m"(native_frame) : "memory");
}

static JSValue probe(JSContext *context, JSValueConst self, int argc,
                     JSValueConst *args, int magic)
{
    (void)self;
    measurement_t *m = &measurements[magic];
    uint32_t depth = 0;
    if (argc && JS_ToUint32(context, &depth, args[0]) < 0) return JS_EXCEPTION;
    if (m->padded) padded_sample(m, depth); else sample(m, depth);
    if (m->heap_sample) {
        JSMemoryUsage usage;
        JS_ComputeMemoryUsage(JS_GetRuntime(context), &usage);
        if (usage.malloc_size > m->engine_bytes) m->engine_bytes = usage.malloc_size;
    }
    return JS_UNDEFINED;
}

static JSValue pause_runtime(JSContext *context, JSValueConst self, int argc,
                             JSValueConst *args, int magic)
{
    (void)self;
    uint32_t sleeping = 0;
    if (argc && JS_ToUint32(context, &sleeping, args[0]) < 0) return JS_EXCEPTION;
    measurement_t *m = &measurements[magic];
    unsigned before = __atomic_load_n(&measurements[1 - magic].pauses, __ATOMIC_ACQUIRE);
    unsigned error = js_measure_fp_pause((unsigned)magic, sleeping != 0);
    unsigned after = __atomic_load_n(&measurements[1 - magic].pauses, __ATOMIC_ACQUIRE);
    if (after > before) m->peer_moves++;
    __atomic_add_fetch(&m->pauses, 1, __ATOMIC_RELEASE);
    if (error) {
        m->pause_failures++;
        return JS_ThrowInternalError(context, "floating-point pause mismatch %u", error);
    }
    return JS_UNDEFINED;
}

static bool bind(JSContext *context, const char *name, JSCFunctionMagic *fn, int slot)
{
    JSValue global = JS_GetGlobalObject(context);
    JSValue value = JS_NewCFunctionMagic(context, fn, name, 1, JS_CFUNC_generic_magic, slot);
    bool ok = !JS_IsException(global) && !JS_IsException(value);
    if (ok) ok = JS_SetPropertyStr(context, global, name, value) >= 0;
    else JS_FreeValue(context, value);
    JS_FreeValue(context, global);
    return ok;
}

static void recursion(const char *name, const char *source, size_t budget, bool padded)
{
    measurement_t *m = &measurements[0];
    os64_memset(m, 0, sizeof(*m));
    m->padded = padded;
    check(stack_bounds(m), "recursion probe reads its mapped native stack");
    if (!m->top) return;
    os64_js_runtime_t *runtime = NULL;
    os64_js_outcome_t result;
    os64_js_config_t config = settings(budget);
    check(os64_js_create(&config, OS64_JS_ABI_ID, &runtime, &result) == OS64_JS_OK,
          "recursion runtime creation");
    if (!runtime) return;
    JSContext *context = os64_js_context(runtime, OS64_JS_ABI_ID, &result);
    check(context && bind(context, "probe", probe, 0), "recursion native sampler binding");
    os64_js_status_t status = os64_js_run(runtime, source, os64_strlen(source), name, &result);
    check(status == OS64_JS_EXCEPTION && result.limit == OS64_JS_LIMIT_NONE,
          "recursion ends as a reusable engine exception");
    check(m->depth > 10 && m->lowest >= m->bottom + 128 * 1024,
          "sampled native stack retains at least 128 KiB headroom");
    os64_printf("stack,%s,%lu,%u,%lu,%lu,%u\n", name, (unsigned long)budget, m->depth,
                (unsigned long)(m->top - m->lowest), (unsigned long)(m->lowest - m->bottom), status);
    const char *reuse = "if(Math.sqrt(2)!==1.4142135623730951)throw 'reuse';";
    check(os64_js_run(runtime, reuse, os64_strlen(reuse), "reuse.js", &result) == OS64_JS_OK,
          "runtime remains usable after stack exception");
    os64_js_destroy(runtime);
}

static void workload(const char *name, const char *source)
{
    measurement_t *m = &measurements[0];
    os64_memset(m, 0, sizeof(*m));
    m->heap_sample = 1;
    check(stack_bounds(m), "workload probe reads its mapped native stack");
    os64_js_runtime_t *runtime = NULL;
    os64_js_outcome_t result;
    os64_js_config_t config = settings(256 * 1024);
    check(os64_js_create(&config, OS64_JS_ABI_ID, &runtime, &result) == OS64_JS_OK,
          "workload runtime creation");
    if (!runtime) return;
    JSContext *context = os64_js_context(runtime, OS64_JS_ABI_ID, &result);
    check(context && bind(context, "probe", probe, 0), "workload native sampler binding");
    int64_t before = os64_micros();
    os64_js_status_t status = os64_js_run(runtime, source, os64_strlen(source), name, &result);
    int64_t after = os64_micros();
    check(before >= 0 && after >= before && status == OS64_JS_OK, "representative workload succeeds");
    os64_printf("workload,%s,%ld,%ld,%lu,%lu\n", name, (long)(after - before),
                (long)m->engine_bytes, (unsigned long)result.jobs_executed,
                (unsigned long)(m->top - m->lowest));
    os64_js_destroy(runtime);
}

static int64_t floating_worker(void *arg)
{
    int slot = (int)(uintptr_t)arg;
    while (!__atomic_load_n(&start_workers, __ATOMIC_ACQUIRE)) os64_yield();
    if (__atomic_load_n(&start_workers, __ATOMIC_ACQUIRE) != 1) return 1;
    os64_js_runtime_t *runtime = NULL;
    os64_js_outcome_t result;
    os64_js_config_t config = settings(256 * 1024);
    if (os64_js_create(&config, OS64_JS_ABI_ID, &runtime, &result) != OS64_JS_OK) return 2;
    JSContext *context = os64_js_context(runtime, OS64_JS_ABI_ID, &result);
    if (!context || !bind(context, "pause", pause_runtime, slot)) {
        os64_js_destroy(runtime);
        return 3;
    }
    const char *source =
        "let sum=0;for(let i=0;i<128;i++){let fraction=0.1+0.2;pause(i&1);"
        "if(fraction!==0.30000000000000004||1/(-0)!==-Infinity||!Number.isNaN(0/0))throw 'edges';"
        "if(Math.sqrt(2)!==1.4142135623730951||Math.sin(Math.PI/2)!==1)throw 'math';sum+=0.25;}"
        "if(sum!==32)throw 'sum';Promise.resolve().then(()=>{pause(1);"
        "if((1.25).toFixed(1)!=='1.3'||JSON.parse(JSON.stringify(0.1))!==0.1)throw 'format';});";
    measurements[slot].status = os64_js_run(runtime, source, os64_strlen(source), "floating.js", &result);
    os64_js_destroy(runtime);
    return measurements[slot].status == OS64_JS_OK ? 0 : 4;
}

static void floating(void)
{
    os64_memset(measurements, 0, sizeof(measurements));
    os64_proc_info_t before, after;
    bool read_before = os64_proc_read(os64_taskid(), &before) == 0;
    int64_t handles[2];
    for (uintptr_t i = 0; i < 2; i++) handles[i] = os64_thread(floating_worker, (void *)i);
    bool started = handles[0] >= 0 && handles[1] >= 0;
    __atomic_store_n(&start_workers, started ? 1 : 2, __ATOMIC_RELEASE);
    check(started, "two independent floating-point runtimes start");
    for (unsigned i = 0; i < 2; i++) {
        if (handles[i] < 0) continue;
        int64_t value = -1;
        check(os64_thread_join((int32_t)handles[i], &value) == 0 && value == 0,
              "floating-point worker completes and joins");
        os64_close((int32_t)handles[i]);
        check(measurements[i].pauses == 129 && !measurements[i].pause_failures,
              "XMM/x87/control state survives yield and sleep");
        os64_printf("floating,%u,%u,%u,%u,%u\n", i, measurements[i].pauses,
                    measurements[i].pause_failures, measurements[i].peer_moves, measurements[i].status);
    }
    check(measurements[0].peer_moves + measurements[1].peer_moves > 0,
          "workers observe actual peer progress while paused");
    bool read_after = os64_proc_read(os64_taskid(), &after) == 0;
    check(read_before && read_after && after.switches > before.switches,
          "proc report confirms intervening context switches");
    if (read_before && read_after)
        os64_printf("switches,%lu\n", (unsigned long)(after.switches - before.switches));
}

int main(void)
{
    check(js_measure_fp_pause(0, 2) == 7,
          "negative control detects XMM, x87 and control-state changes");
    const char *simple = "function r(n){probe(n);return r(n+1)+n;}r(1);";
    const char *wide = "function r(n){let a=n,b=n+1,c=n+2,d=n+3,e=n+4,f=n+5,g=n+6,h=n+7;"
                       "probe(n);return r(n+1)+a+b+c+d+e+f+g+h;}r(1);";
    const char *getter = "let depth=0;const object={get value(){probe(++depth);return object.value+1;}};object.value;";
    os64_printf("stack,shape,budget_bytes,depth,sampled_used_bytes,sampled_headroom_bytes,status\n");
    const size_t budgets[] = {128 * 1024, 256 * 1024, 768 * 1024};
    for (unsigned i = 0; i < sizeof(budgets) / sizeof(budgets[0]); i++) {
        recursion("simple", simple, budgets[i], false);
        recursion("wide", wide, budgets[i], false);
        recursion("getter", getter, budgets[i], false);
        recursion("native64k", simple, budgets[i], true);
    }
    os64_printf("workload,name,elapsed_us,sampled_engine_bytes,jobs,sampled_stack_bytes\n");
    workload("primes", "let count=0;for(let n=2;n<=10000;n++){let prime=true;for(let d=2;d*d<=n;d++)"
             "if(n%d===0){prime=false;break;}if(prime)count++;}if(count!==1229)throw 'primes';probe(0);");
    workload("json", "let rows=Array.from({length:10000},(_,i)=>({id:i,text:'record-'+i,value:i/8}));"
             "let text=JSON.stringify(rows);let copy=JSON.parse(text);probe(0);"
             "if(copy.length!==10000||copy[9999].value!==1249.875)throw 'json';");
    workload("typed8m", "let data=new Float64Array(1048576);for(let i=0;i<data.length;i++)data[i]=i/8;"
             "probe(0);if(data[1048575]!==131071.875)throw 'typed';");
    workload("bigint", "let a=0n,b=1n;for(let i=0;i<10000;i++){let next=a+b;a=b;b=next;}"
             "probe(0);if(a.toString().length!==2090)throw 'bigint';");
    workload("jobs100k", "let count=0;function job(){if(++count<100000)Promise.resolve().then(job);"
             "else probe(0);}Promise.resolve().then(job);");
    size_t length = os64_js_default_limits().source_bytes;
    char *source = os64_malloc(length + 1);
    check(source != NULL, "source-ceiling workload input allocation");
    if (source) {
        os64_memset(source, 'x', length);
        source[0] = '/'; source[1] = '*';
        os64_memcpy(source + length - 11, "*/probe(0);", 12);
        workload("source4m", source);
        os64_free(source);
    }
    floating();
    os64_printf("J2: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
