# E3 transition map

Production entry points and public behavioral tests; names refer to the final
implementation, including failure and lifecycle paths.

| Invariant | Production paths | Shared enforcement | Public evidence |
| --- | --- | --- | --- |
| I1: selected/focus/anchor IDs are projection members | Detached bind/command; attached bind/sort/command/context; pointer, keyboard and typeahead; RESET/domain replacement; disposal | `os64_ui_selection_bind`, `os64_ui_selection_command`, `os64_ui_data_projection_position`; `command_impl` stages a detached copy | `core_selection`, `widget_input`, `bounds_and_independence`, `gallery_behavior` check complete tuples and independent projections |
| I2: failed candidates do not partially publish | Snapshot/projection/sort construction; selection bind/copy; attach; bind/sort/command/reveal/scroll/columns/layout; font prepare/discard; companion scrollbar callback | `prepare`, `publish`, `frame_free`, `ui_selection_publish`, `bind_impl`, `command_impl`, `result`; `sync_bars` restores committed positions | `core_refusals`, `core_sorts`, `allocation_sweep`, `allocation_capacity` compare tuples, revision, order, viewport and production pixels after failures and retry |
| I3: intent belongs to its original delivery/revision | Row/header/grip press/release; successful bind/sort/reset/layout/font commit; wheel/scroll; context; focus/cover/INPUT_LOST; toolkit hide/disable/root detachment; explicit disposal | `view_cancel`, `end_gesture`, `pressed_revision`, `clear_click`, held-key preservation across publication, shared key decoder and shortcut burst validation; separate held Enter/Space identities | `interruptions`, `widget_input` exercise PS/2 and HID, stale release, lost key-up, orphan burst bytes, double/triple clicks and fresh input after cancellation |
| I4: drawing and hits use committed geometry/resources | `layout`, bind/sort/columns/scroll/reveal; class font prepare/commit/discard/reface; native bounds mismatch in paint/event; blocked retry | `view_frame` (bounds, UI context and face role), `prepare`, `publish`, `usable`, `block`, `row_at`, `header_at`, `sync_bars`; `view_paint` draws retained runs and leases | `allocation_sweep`, `bounds_and_independence` check blocked resize, failed retry, font failure, clipping, tiny/zero bounds, hide/move columns and allocation-free production rendering |
| I5: ownership survives failure and ends once | Partial snapshot/projection/selection/frame construction; acquired row icons and text; replacement; callbacks; reface; generic tree font destruction; explicit disposal | Reference ownership in data/projection/selection; `frame_free`; `view_destroy` delegates to idempotent `dataview_dispose`; reface releases resources while preserving frame presentation | Allocation sweeps count successful leases/releases between attempts; `view_delete`, `repeated_lifetime` and LeakSanitizer cover teardown and repeat replacement |
| I6: observable results agree | Programmatic setters; input commands, context, sorting, resize/retry; font hooks; errors and callbacks; scrollbar restoration | `selection_flags`, `publish`, `notify`, `result`, `sync_bars`; getters borrow complete committed state; callback `busy` and app-owned result fields guard initial failures, publication/disposal and nested status overwrite | `fixture_changed` and `fixture_error` attempt reentrant mutations and read revision/status; failure sweeps compare arrow descriptor and pixels; programmatic operations emit no user callback |
| I7: work/resources are bounded | Snapshot validation and copying; projection lookup/sort/filter; selection enumeration; visible preparation/paint; keyboard lookup; replacement and maximum fixtures | Checked model limits; actual allocation-capacity charges; ID/source and source/projection indexes; merge sort; viewport run-slot cap; `metadata`/`record_peak`; fixed prefix buffer | `core_limits`, `performance`, `bounds_and_independence`, `repeated_lifetime`; paint allocation/acquisition counters, viewport row counts, maximum fixture and 100,001 refusal |

The snapshot and projection own their copied data and indexes. A selection
retains its projection; the widget borrows that app-owned handle. View leases
borrow the app's provider. The gallery tears down views before its provider. Font-error notice text is
queued until the dispatcher boundary, after the candidate face has committed
or been discarded.

No public asynchronous view candidate exists: operations prepare and publish
synchronously on the lifecycle thread. The font coordinator owns its staged
candidate, and mutation is BUSY while that stage exists. Enumeration returns
STALE when the selection revision changes. Font commit validates its recorded
revision and allocates nothing.
