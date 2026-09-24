# F2.5 — Shared font sets and transactional replacement

This supplements the frozen F0 contract without changing its headers or schema.
It is the common boundary for F3, F4 and F5. Parent: accepted F2 at
`4b0a839b6f2c0c6baea3b1eb060651e7f636fe46` (implementation `32d8243`).
The implementation lives in `font_provider.c` and `font_adopt.c`; their public
headers are `os64/font_provider.h` and `os64/font_adopt.h`.

## Responsibilities

F2.5 constructs an immutable set of UI, terminal and document roles from supplied
font bytes, supplies primary metrics and ordered fallback handles, and coordinates
consumer preparation/adoption. It does not open paths, parse fonts.conf, discover
fonts, publish appearance changes or choose window layouts.

F5 owns those input and publication policies. It resolves a complete candidate,
reads its bounded assets, supplies three role specifications to
`os64_font_set_prepare`, then releases the input buffers. Ordinary production
consumers receive a set; they do not open fonts or build fallback lists themselves.
Host/guest harnesses may supply fixture bytes directly through the same function.
Before F5 integration, builtin startup and injected test sets are supported;
there is no second persistent provider or hidden file-selection protocol.

F3 owns terminal geometry and PTY adoption. F4 owns widgets, textfields, textviews
and Scribe together. F5 calls their common consumer callbacks, rather than poking
inside either consumer. Shared interface changes come back to Quinn; consumers
must not modify the frozen F0 headers or inspect text_internal.h.

## Set construction and lifetime

1. The caller owns an F2 text context and its allocator/backend. Create it through
   `os64_font_context_create`: use F2 options with allocator callbacks and leave
   backend NULL for production FreeType, or inject a backend for tests. F2's
   original `os64_text_create` remains available and keeps its mandatory-table
   rule. The provider factory calls the leaf getter inside libos64, so consumers
   need no direct FreeType linkage. No hidden global context is created. Coordinate the context's lifetime with the
   windows using it; serialize its operations on the UI/event thread.
2. `os64_font_set_prepare(context, specs, &candidate)` opens all three roles or
   returns no set. F5 can use `os64_font_set_prepare_checked` to receive the
   failing role and source slot and map it to the selected configuration line.
   Context-wide failures have no source slot; successful preparation clears the
   diagnostic. Parsing and filesystem errors remain F5-owned. NULL specs, or three zero-initialized specs, select builtin/16.
   Each role has a primary and at most two configured fallbacks. A zero size is
   constructor shorthand for 16; F5 still rejects a literal invalid size in a
   config file according to F0. Outline faces use NORMAL at the role's size.
   An explicit builtin primary requires size 16; builtin fallback stays 8x16.
3. The provider appends builtin fallback if the role's explicit list lacks it.
   It does not append a duplicate when builtin is already primary or fallback.
   Repeated explicit builtin entries are invalid. Stable asset/path deduplication
   belongs to F5, which has that information; byte buffers are not path identities.
4. Font bytes are copied by F2. A set retains its ordered font handles; views
   borrow those handles and must not release them. Set storage uses the same
   context budget as F2, including allocations and failure status distinctions.
   Old and candidate sets coexist during preparation, so a replacement may fail
   under a cap even if either set would fit alone. No cap increase is implicit.
5. `os64_font_set_retain/release` manage shared ownership. A view is valid while
   its set is retained. An F2 run can outlive the set because it retains its own
   fonts/images. Release runs and sets before destroying their text context;
   context destruction remains BUSY while their font/run handles survive.
6. A set's identity is unique within its context, assigned from its newly opened
   UI primary's F2 identity. It represents an entire immutable role set, not an
   appearance generation. New objects get a new identity even for the same path,
   file contents or size. Pair identity with the text context in caches. F5 owns
   the mapping from a publication generation to successful consumer adoption;
   a failed attempt must not advance that consumer's installed generation.

There is no active-set singleton. Consumers keep their current set and staged
plan. This also permits independent previews without publishing them. A coherent
adoption group uses a candidate from one context; isolated contexts/previews get
separate candidates. Cross-process or cross-context atomic presentation is not
promised. Input buffers and file-reader staging belong to F5's bounded I/O work,
not the text context's allocation counter.

## Metrics and role use

`os64_font_set_view` returns the text context, borrowed ordered font list,
primary face metadata, set identity and pixel row metrics. Feed the font list
unchanged to F2. Do not release/reorder its elements or construct a second cache.

- `baseline_px = ceil(primary.ascent / 64)`.
- `row_height_px = baseline_px + ceil((primary.line_height - primary.ascent) / 64)`.
- `cell_width_px` is positive for the terminal role and zero for UI/document.

This row contains the primary line box after integer baseline placement. It does
not expand when a fallback or the missing marker is taller. F4 clips each row and
uses run selection X with row Y; an empty row has the same pitch as a nonempty
one. Runs still report content metrics; consumers do not use those to vary pitch.

Terminal suitability is checked centrally using fixed-width metadata and each
printable ASCII scalar U+0020..007E from the primary alone. Each must resolve to
the primary with the same positive, whole-pixel advance. Fallback/markers cannot
make an unsuitable primary pass. Failure is UNSUPPORTED; allocation/engine
failures retain their actual status. F5 startup can report the rejected candidate
and prepare builtin defaults. Live preparation failure keeps the active set.
F3 does not silently switch to a proportional face or duplicate these checks.

Role mapping for F4: labels, buttons, list/menu captions and textfields use UI;
textviews, including Scribe's document and help view, use DOCUMENT. F3 terminal
cells use TERMINAL. Keep legacy theme `font.w/h` at 8/16; they are not selectors
or the new role metrics. Applications may opt into a different role explicitly,
with a named API owned by F4; do not infer a role from text contents.

## Consumer replacement protocol

A consumer is one coherent unit whose active state changes together, usually a
window including its application layout and widget tree. It supplies an
`os64_font_consumer_t` with prepare, optional barrier, commit and abort callbacks.
F4 owns the concrete UI/application adapter and can expose it from its UI API;
F5 depends on this generic descriptor, not on its private plan layout.

`os64_font_adopt(candidate, consumers, count, &failed_index)` performs:

1. Validate the batch (1..16 distinct consumer user pointers; required callbacks;
   at most one barrier) before calling consumers. Retain candidate for the call.
2. Prepare each consumer in order. A successful prepare returns a non-NULL owned
   plan. The plan retains candidate if its committed state will use it. Prepare
   stages the runs, caches, widget/application geometry and allocations needed
   for adoption without changing active state. On failure it cleans its own
   partial work and returns a NULL plan.
3. After all prepares succeed, call the optional barrier. This is the final
   fallible operation. F3 uses it for the existing PTY resize; failure must leave
   external geometry unchanged. No earlier prepare resizes the PTY.
4. Commit each prepared plan in order. Commit cannot allocate or fail. It swaps
   the prepared state, transfers the plan's candidate reference to active state,
   releases old ownership and consumes the plan. Prepare must reserve everything
   this requires; freeing storage is allowed. A success leaves no fallible work
   between the successful PTY resize and adoption of the new local geometry.
5. If prepare or barrier fails, abort successful plans in reverse order, consuming
   their retained sets and staged allocations. Active state stays unchanged.
   Return the error and failing consumer index. Batch validation/retention failure
   and success report SIZE_MAX. The input candidate remains caller-owned.

The caller serializes the whole call with document edits, window changes,
font replacement and context access. Callbacks do not pump events, invoke other
consumers or recursively enter adoption. Preparation is synchronous, so a plan
cannot cross a queued resize/edit without re-preparing it. The coordinator does
not allocate; callback plans may allocate during prepare. Descriptor storage is
copied before entering application callbacks.

Multiple independently fallible external barriers are deliberately rejected:
two PTY resizes cannot be rolled back atomically through the existing ABI. Each
terminal process has its own adoption group. A group can include multiple UI
consumers plus one terminal barrier; do not split a single application's coupled
widget geometry and document state into independently successful groups.

### F3's plan

Compute candidate cell/grid geometry from the view and current window. Validate
PTY dimension fences and gterm's cell-capacity bound; prepare buffers and any
render state before resize. The barrier requests the grid through
`os64_pty_resize` only when dimensions actually change. Refusal keeps the old
set, cell mapping, grid, selection and installed generation. Success commits
cell metrics and local dimensions together, clears selection/drag state that
would refer to rearranged cells, and invalidates the old snapshot. Repaint only
a snapshot matching the adopted dimensions; a transient read failure after
commit is retried without pretending the external resize was rolled back.
A separate window resize continues to use the existing old-grid clipping rule
on refusal. No new PTY syscall or terminal encoding change is authorized.

### F4's plan

Stage application layout and UI/editor state as one consumer. Include primary
row pitch, control minima/insets, document source offsets, focused widget,
selection, caret, remembered pixel X, pixel scrolling and bounded line windows.
Prepare is not the existing resize callback: that callback mutates live bounds.
Add an F4-owned application planning hook or owned layout plan so larger-font
geometry is validated before live state changes. Allocation failure must not
leave half-updated widget bounds or destroy the old editor cache. Do not resize
the actual window or mutate its minimum size during prepare. Stage a layout
that fits the current content area; if that is impossible, report a clear
prepare failure and preserve the old state. A requested window-size change is
separate, or uses the group's single checked barrier with unchanged-on-failure
semantics; do not assume multiple GUI operations form an atomic transaction.

Commit retains document bytes, legal source selections and focus, swaps the
prepared runs/geometry, clamps scroll/caret visibility and marks the window
for repaint. Painting later may fail to publish a surface, but it must not
allocate the state required to make the font adoption coherent. A paint retry
is separate from a failed font adoption. Preserve the document/help vtable
model, and ensure unadopted windows remain fully usable.

## F5 integration and completion boundary

F5 prepares all three roles from one valid resolved candidate, creates consumer
plans, and uses the common adoption helper. If the envelope also changes palette
or treatment, its F5-owned wrapper stages those values alongside the UI consumer
plan and commits them without allocation. A failed font prepare must not advance
the UI's full installed appearance generation. The wrapper carries publication
metadata; it does not put generation numbers into font identities. Publication can precede process
adoption; receiving a generation is not proof it was adopted. Cache a retryable
failure separately from the installed generation. Treat a palette-only update
as no font replacement when effective font choices are unchanged; explicit font
reload creates new objects even when paths match. Follow F0's preserving envelope
and reboot requirement; this layer does not implement those mechanisms.

F3/F4 can begin with this implemented boundary. Passing its tests does not pass
their consumer gates. Full window, editor, PTY, Apply/Save and installation
behavior remain in their assigned packages. See docs/fonts/F25-REPORT.md for
verification. The separate Opus handoff receipt identifies the exact assignment
base after this foundation is committed.
