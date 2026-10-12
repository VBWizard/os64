# E1B controller invariants and transition audit

2026-10-12. Governing behavior remains [E01_MENUS](E01_MENUS.md) and
[E1K lifecycle](E01K_HANDOFF.md). These rules describe the controller's observable
commitments; host tests check them between events and dispatcher turns.

| State | Owned/live state | Exit obligation |
| --- | --- | --- |
| Closed | No session view, panels or pending model; bar view remains independently retained | New request prepares its saved target before activation |
| Bar mode | Active saved view and eligible displayed bar selection; no panel/pending model | Open selected panel, or close/recover original focus |
| Live cascade | Active saved view, contiguous registered panels with parent/model/selection links | Destroy children before changing their parent anchor or releasing owner state |
| Pending opening | Active saved view and retained model at the next path slot; creation awaits surviving queue drains | Revalidate parent/model link, cancel on path change, or publish after quarantine |
| Refused teardown | Surviving panels, view, hooks and font context remain owned; pending intent and pressed activation are cancelled | Retry destruction; commit selection/focus/visibility release after successful closure |

| Invariant | Transitions audited | Evidence obligation |
| --- | --- | --- |
| I1: live path is contiguous and child model matches the selected parent row | Root/child opening, hover, keyboard selection, collapse, refresh, partial destruction | Check native ownership, registrations and parent row/model after each step |
| I2: failed native removal does not release or detach surviving borrowed state | Close, refresh, selection, scroll, replacement, font adoption, disposal | Inject failure at each depth; selection/anchor stays intact until removal succeeds |
| I3: pending intent belongs to the current surviving parent and saved target | Replacement, collapse, selection, scrolling, loss, boundary creation | Vary queue budget/order and interrupt before the drain boundary; no orphan birth |
| I4: input pairing belongs to the current family delivery route | Matched release, owner dismissal, external focus, coverage, INPUT_LOST | Family transitions retain necessary guards; interrupted delivery clears them; unrelated later input passes |
| I5: visible bar state tracks its model/view/selection and invocation kind; bar-only selection stays eligible after publication, and requests use fresh snapshots | View switch, state refresh, bar navigation, context opening, close, font/theme adoption | Use production owner rendering, dirty damage and pixels, including partial repaint |
| I6: an explicit menu opening/closing request has an observable outcome | Alt/F10, mnemonic, pointer open, deferred opening, native/resource failure | Return a result or notify the error hook; repeat suppression does not suppress a fresh gesture |
| I7: invocation owns its saved target independently and validates it after close | Keyboard/pointer activation, shortcuts, stale/disabled target, callback teardown | No callback with live menus; no mutation against a stale target; no widget access after callback |
| I8: font and handle lifetimes follow native ownership and the attached owner UI | Font change/release, child-first close, disposal, tombstone draining | Close before font adoption; failed close refuses release; no handle reuse before surviving queues drain |
| I9: menu gestures act on visible selectable state with the governing input precedence | Wheel, navigation, page distance, activation, menu mnemonics versus action shortcuts | Active owner wheel is consumed; hidden rows cannot invoke; page distance counts model rows; menu gestures precede unmodified shortcuts |
| I10: viewport effects follow actual movement and tail geometry | Wheel, affordances, selection reveal, extreme counts, child/pending path | No-op preserves path/help/intent; actual movement closes descendants before changing top; final viewport packs a fitting suffix using row heights |
| I11: published model/view pairs share their action registry | Model graph creation, popup creation, bar model bind, bar view bind, target preparation | Reject mismatched registries before retention/preparation/publication; colliding IDs cannot cross registry identity |

The review findings map to obligations rather than individual source lines:

| Finding | Invariant | Shared paths checked |
| --- | --- | --- |
| R1 refresh leaves invalid descendants/help | I1/I2 | Refresh, selection and help recomputation |
| R1 disabled remembered bar entry | I5/I9 | Alt/F10, overflow and all-disabled temporary recovery |
| R1 native destruction failure releases borrowed state | I2/I8 | Close, replacement, disposal, font release/adoption, invocation |
| R1 bar belongs to another UI | I8 | Session attachment, owner registration and bar ownership |
| R1 active owner wheel falls through | I9 | Owner/popup wheel routing and normal post-close delivery |
| R2 refresh hides destruction error | I2/I6 | Stale refresh plus failed close, retry before owner teardown |
| R2 wheel retains hidden selection | I9 | Wheel/affordance, selected help, Enter and next navigation |
| R2 keyboard leaves pending child | I1/I3 | Arrows, edges, pages, duplicate mnemonic, pointer and scroll |
| R2 hidden mnemonic selects an unpainted bar index | I5 | Direct target, overflow hover and Left/Right |
| R2 standalone external focus classification | I4 | Owner zero is outside the family; reported nondismissal was not reproduced |
| R3 shortcuts shadow menu gestures | I9 | Navigation, release activation, popup mnemonics, unmatched shortcuts |
| R3 failed hidden-bar refresh leaves temporary reveal | I5/I6/I8 | Retained-view preparation, collapse and error routing |
| R3 Escape/Left leaves pending descendants | I1/I3 | Shared close depth, boundary creation and missing-parent refusal |
| R3 Page counts enabled-entry moves | I9 | One viewport's model-row target, clamp and enabled landing |
| R4 failed refresh detaches surviving child | I1/I2 | Selection transaction, partial failure and bar replacement |
| R4 bar state changes without damage | I5 | Successful/invalidated snapshots, view swap, same-view update and context painting |
| R4 external loss strands input guards | I4 | Open/closed session loss, family release, mouse suppression, INPUT_LOST and coverage |
| R4 bare Alt/F10 refresh failure is silent | I6/I8 | Shared retained-view preparation, visible/hidden mode and repeat versus fresh gesture |
| R5 deferred opening hides failed teardown | I2/I6/I8 | Deferred/immediate failure cleanup, model/view preparation and error hook observation |
| R5 refreshed bar-only selection is disabled | I5/I9 | Entry, explicit refresh, same-view/target publication, all-disabled collapse and invalid pending roots |
| R5 no-op wheel collapses children | I3/I10 | Non-scroll parent, zero delta, edge clamps, affordances, pending intent and failed actual movement |
| R5 final viewport contains one row | I9/I10 | Actual separator/command heights, extreme counts, repeated movement and selection reveal |
| R6 foreign-registry bar model rebind | I11 | Both binding directions; inactive refusal before allocation and active path preservation |
| R6 opening uses cached eligibility | I1/I2/I5/I7/I9 | Fresh candidate for mnemonic/pointer/overflow/navigation; old target remains until close; declined same-view request reconciles invalid descendants |

Earlier-round tests remain in the complete suite; the fourth-round observer
adds path checks between events and owner damage/pixels. The matrix is a bounded
transition audit, not a claim that every possible event trace has been explored.

Focus transfer within the family keeps held-key guards whose releases still
arrive there. Owner-content dismissal retains the dismissing down/up pair.
External focus loss and INPUT_LOST invalidate the delivery route and reset held
keys, F10, decoder, Alt arming, pressed activation and mouse suppression.
Coverage cancels input pairing while preserving menus, as ordinary libui and
the action router already do; it does not imply external focus or menu closure.

Bar state publication and bar repaint are separate commitments. Successful and
invalidated refresh snapshots schedule changed bar state. A
target swap schedules damage even if its enabled mask happens to match. Reusing
the same view detects changed enabled state without scheduling an idle repaint
on each gallery paint. Context menus do not represent a selected top-level bar
heading. Changed panel selection commits after descendant destruction succeeds.

Failure reporting follows cleanup: when cleanup cannot remove a registration,
the result preserves that teardown obligation before the application sees the
error hook. Bar-only selection normalization accompanies snapshot publication;
if none is enabled, mode closes. A pending root whose saved model loses its
enabled commands is cancelled before normalization, preserving anchor identity.

Scrolling computes its requested viewport before changing the path. A clamped
or zero movement preserves descendants, pending intent and help. Actual movement
cancels activation and closes children before publishing the new viewport.
The final viewport packs the fitting suffix by command/separator heights;
selection reveal uses the same viewport bound.

Model and view binding preserve registry identity in both directions before
publishing a pair. Opening prepares a retained fresh candidate, uses that
snapshot for eligibility and navigation, and transfers its ownership into the
session after native teardown succeeds. A declined request leaves the saved
target intact. When candidate and saved view are the same, updated state follows
explicit refresh's selection reconciliation, including refused teardown.

The test harness's private session layout is shared through the internal header
to observe these properties without duplicating opaque layouts or adding public
test APIs. Sequence tests exercise complete and interrupted gestures, queued
work, failure injection, view changes and production owner rendering. Legacy
font/allocation/action checks remain separate evidence from transition coverage.
