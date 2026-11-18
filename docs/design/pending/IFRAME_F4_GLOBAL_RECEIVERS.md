# F.4d — global property callback receivers

The embedding global-this primitive separated script receivers from storage,
but a global accessor or an inherited Proxy trap could still receive the raw
global. GroupBy also passed raw storage to its callback. These routes bypass
the identity that the future WindowProxy will protect. This milestone closes
the engine callback routes; it installs no browser proxy.

## Contract

Patch `0043-global-property-receivers.patch` substitutes a configured global's
embedding receiver immediately before invoking property getters, setters and
exotic get/set callbacks. It covers own/inherited properties, native exotic
descriptors, prototype Proxies, global identifiers and dynamic reference
operations. Optimized data-property paths need no substitution: they invoke no
author callback. `JS_GetGlobalVarRef` and the global bytecodes keep their storage
object; common property machinery handles callback receivers.

Selection follows the receiver object's embedding context, not the caller or
accessor function's realm. Non-global alternate receivers, including null and
undefined through Reflect, retain their meaning. A bound getter retains its
explicit bound this. Raw globals deliberately passed by a trusted embedding
as ordinary call arguments/receivers are not concealed by this primitive.

A configured global retains and traces its context. That context owns the
existing global-this hold; global finalization releases the context edge.
This supports exported globals without a dangling context pointer. It does
not authorize calls into retired realms or provide F15 reclamation. The
unconfigured path adds no reference. No new allocation is introduced; target
`JSObject` remains 72 bytes and `JSContext` 576 bytes. The private global member
is 24 bytes (was 16) and fits the existing object union.

`Object.groupBy` and `Map.groupBy` pass undefined, as specified by
[ECMAScript GroupBy](https://tc39.es/ecma262/multipage/keyed-collections.html#sec-groupby).
Strict/native/Proxy callbacks observe undefined. Sloppy callbacks select their
own realm's receiver. This corrects default engine behavior too, including
foreign callbacks; it does not change grouping order or iterator cleanup.

Property substitution is an embedding rule for the distinct global receiver.
For comparison, [V8's accessor implementation](https://github.com/nodejs/node/blob/main/deps/v8/src/objects/objects.cc)
substitutes a global's proxy before invoking its getter or setter. The
ECMAScript global environment still uses its binding object's storage.

## Proof boundary

The shared runtime fixture uses disjoint storage and receiver objects, with
raw globals deliberately exported only as native test oracles. Named cases:

- Identifier, dot and computed reads/writes; strict own/inherited accessors;
  increment, compound/destructuring assignment and `with` references.
- Data and lexical storage, implicit global creation, Reflect alternate
  receivers, native accessors, bound functions and Proxy apply traps.
- Prototype Proxy get/set and native exotic descriptor callbacks.
- Getter/setter self-deletion and exceptions; direct/indirect eval and Function;
  writable globalThis poisoning; compiled and cached global references.
- Foreign getters with a receiver in another realm; foreign strict/sloppy
  GroupBy callbacks; strict/native/bound/Proxy cases and iterator closure.
- Exported global after context-handle release and GC; configured receiver
  cycles, self-assignment and default embedding behavior in the existing suite.

Validation receipts are recorded below before publication. Engine preparation
identity changes, invalidating old bytecode caches as designed. Public wrapper
ABI/layouts and exported symbol names are unchanged. F4 remains in progress:
WindowProxy/Location exotics, retained Location active-document behavior,
DOM alias/event raw-global closure and integrated security/lifetime acceptance
remain. No visible frames, F15 collection or P5 acceptance is claimed.

## Validation

Prepared engine:
`a04a8148d20bd795e688ca17a18951107b7b36dcc006213c65c0625de7b95afb`.
The fixture is shared by the host and target runtime consumer.

| Check | Shipping / instrumented engine |
|---|---|
| Focused global receiver | 97 / 97 |
| Existing embedding global-this | 67 / 67 |
| Full runtime | 39,607 / 134,547 |
| Default DOM Window | 666 / 820 |
| DOM navigation authority | 1,792 / 1,792 |
| Native browsing contexts | 1,078 / 1,078 |
| Window / Location invocation authority | 304 / 304; 526 / 526 |
| Browser jobs / retirement / realm routing | 801 / 801; 9,770 / 9,770; 678 / 678 |

All positive runs have zero failures; runtime teardown reports zero live
allocations. ASan/UBSan and LeakSanitizer run outside the restricted sandbox;
the instrumented QuickJS engine uses the harness's ASan-only engine flags.
The full runtime command also exercises construction/compile allocation cuts
and fatal-invariant subprocesses. Six preparation tests, manifest verification,
contract headers, target allocation/global/visibility audit and tracker validation
pass. The stale-reference report's sole item is a moved, valid link to the
F4 brief, checked by hand. Yonder, JS CLI and both target runtime/DOM consumers
compile; exported symbol names/types and public headers are unchanged.

Ten isolated negative controls are detected: own getter, setter, prototype
get/set, exotic descriptor getter, wrong getter-realm substitution, GroupBy
receiver and the entire pre-fix engine fail behavioral assertions. Removing
the configured-global context mark or release reaches the engine's fatal
invariant path (exit 99), rather than passing a cyclic teardown unnoticed.

A final same-line comment correction leaves the preprocessed engine identical
to the negative-control baseline (SHA-256
`2b464271cc4dfe98df35f597861b61eaedb870cc7674565c54da14e2e3e91e1a`).
The final prepared-source digest and cache-identity consumer were rebuilt;
positive full/focused runtime and focused DOM checks use that final identity.
Unchanged DOM objects were reused after source/layout comparison with merged
#655, with the merged #654 runtime adapter rebuilt. Commands and matching
binaries, generated source, mutant inputs/logs and a SHA-256 manifest are
retained in the private cohort. An optional default-browser GroupBy regression
page is frozen with matching shipping binaries; it is not hardware acceptance.

Reproduction uses `tools/test_js_runtime_host.sh` (full), or
`JS_RUNTIME_FOCUSED_SELECTOR=--global-receiver` / `--global-this`, with
`ASAN_OPTIONS=detect_leaks=1`. Focused DOM selectors are `--window`,
`--navigation-authority`, `--browsing-context`, `--window-authority`,
`--binding-authority`, `--browser-jobs`, `--realm-retirement` and `--realms`.
No full DOM suite or new WPT/guest/P5 execution is claimed.
