# Worker-callable library audit

Scope: the `codex/work-pool` implementation, over doorbell commit `08fab5fc`.
This is a source and linked-object audit, supplemented by the pool's HTTP
guest fixture. It is not a claim that the new fixture exercises HTTPS or
parallel image decoding.

The searches below identified candidates; each candidate's initialization,
mutation, and cleanup path was then inspected. In addition, `x86_64-elf-nm -S`
was run on the strict-build shared libraries, checking `b/B/d/D/s/S/g/G`
symbols. Pointer tables can appear in data sections despite being const;
section placement alone neither establishes nor disproves thread safety.

```sh
rg -n 'static|trust_owned|calloc' userland/libfetch
rg -n 'static|references|__atomic|sealed' userland/libtls/port
rg -n 'HASH_OID|static.*pp' userland/libtls/upstream/src
rg -n 'static|__os64_env' userland/libos64/{resolve,net,url,str,env,conf}.c
rg -n 'static|alloc|free' userland/libimage userland/libpng userland/libgzip
rg -n 'static|client_data|setjmp|destroy' userland/libjpeg/port
x86_64-elf-nm -S userland/bin/libfetch.so
# Repeated for libtls, libimage, libpng, libjpeg, libgzip, and libos64.
```

| Path | Inspected state and ownership | Worker contract |
|---|---|---|
| `libfetch/fetch.c`, `http.c`, `proxy.c`, `transport.c` | Request, response, proxy decision, transport deadlines, gzip stream and staging buffers belong to each fetch object. The linked library has no data/BSS symbols. `trust_owned` distinguishes its loaded store from the caller's borrowed store. | One fetch object per job. Caller callbacks and borrowed trust stores outlive its close. |
| `libtls/port/client_engine.c`, `certificate_policy.c`, `platform_inputs.c`, `libtls/transport.c` | TLS engine, validator, entropy handle, time snapshot and transport are per connection. Sealed anchor contents are immutable. The store's reference count is deliberately mutable and already uses atomic retain/release with overflow refusal. | Construct and seal a store before publishing it to workers. Keep the caller's store reference until the pool is destroyed. Do not add anchors concurrently. |
| BearSSL selected build | Linked data symbols are algorithm vtables, curve parameters, and `HASH_OID`. The latter is an array of pointers to const bytes whose pointer slots lack a const qualifier upstream; inspection of its two occurrences in the selected client source finds initialization and lookup, no writes. Server source is outside the selected client build. Curve `pp` and the vtables are const. | Separate client contexts; no runtime global initialization or table mutation on this path. The non-const pointer-array declaration is not a shared write and needs no lock. |
| `libos64/net.c`, `resolve.c` | Dial text, hosts parsing, DNS request/reply buffers and random-id handle are local to each call. There is no resolver cache or shared answer buffer. Each DNS call opens its own UDP endpoint. | DNS remains synchronous and has its own finite retries; pool cancellation cannot interrupt a resolver in the middle of a call. |
| `libos64/url.c`, `str.c`, `crc32.c`, read-side `conf.c` | Caller-owned parse/output buffers and local iteration state. No mutable lookup caches. The config writer's separate temp-file sequence is atomic and is not on the fetch read path. | Do not concurrently mutate caller input/output storage. |
| `libos64/env.c`, `init.c` | The environment pointer is published during runtime initialization before main; its mapped environment block is read-only. There is no worker-side lazy initialization. Kernel setenv/unsetenv can change this same block despite its read-only user mapping. | Do not mutate the environment or call runtime/environment publication while workers may read it. |
| `libos64/heap.c` | Regions, free list and report are shared; allocator entry points protect them with the heap lock. Heap initialization happens before main. The private spin/yield implementation is now `os64_lock_t` with the same acquire/release ordering and 64-spin policy. | Ordinary allocation/free is shared safely. Do not reinitialize the heap from a worker. |
| `libimage/image.c`, `gif.c` | Dispatch and static-image decode use per-call state. A GIF sequence owns its mutable canvas, restore buffer, palette/frame state and cursor. No linked data/BSS symbols. | Independent decodes may run in parallel. Serialize access to an individual sequence; transfer it only after its worker finishes. |
| `libpng/png.c`, `libgzip/{inflate,gzip,deflate}.c` | PNG rows/pixels/parser, gzip framing and inflate/deflate windows belong to their call/stream. Numeric tables are const; no linked data/BSS symbols. | One stream per active worker operation. Include scratch/window allocation in reservations. |
| `libjpeg/port/decode.c` and selected scalar IJG sources | `decoder` owns the decompressor, error manager, escape buffer, allocation accounting, scanline and pixels. Callbacks reach it through `client_data`; cleanup destroys that decompressor. Linked data contains only the const status-name pointer table. | Independent decoder objects; the nonlocal error escape remains within its calling thread. |

`ui_session` also adopts the shared lock. Its appearance/font cache remains
protected through bounded configuration I/O. UI widgets, font sessions and
application state are not made generally thread-safe by this change; pool
callbacks must not manipulate them.

No new worker-side shared-state race was found in these paths. The initial
packet's shorthand that any non-const static requires a lock was too broad:
the upstream OID pointer table has no mutator. Conversely, TLS reference
counts are shared mutable state even though they are fields rather than
statics; their atomic lifetime protocol is part of the safety argument.

The audit also distinguishes cancellation bounds: DNS retries, network idle
deadlines, bounded TLS close, and codec work limits all contribute. The pool
cannot promise that destruction takes just one libfetch idle interval.
