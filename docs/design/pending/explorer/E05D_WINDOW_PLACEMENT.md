# E5D — Remember application window placement

## Status and ownership

Planned, 2026-10-21. Chris requested this as a separate quality-of-life slice
while testing E5C and assigning the new PR #322 findings. Quinn maintains this
record; implementation has not started. No branch or review exists for E5D.

## Accepted outcome

Roam remembers its position and size when they change and restores both on the
next launch, including across reboot. Chris identified other windows that
would benefit; their identities are not yet recorded. Build reusable support
and use Roam as its first consumer, with separately named preferences for
other consumers rather than one application overwriting another's placement.

## Engineering work to settle

- A shared helper loads and atomically publishes one complete geometry record
  through the configuration search path. Application/window keys select it.
- Save completed user moves/resizes, with bounded writes during dragging.
  Closing an older instance must not overwrite a newer instance's preference.
- Restore safely after resolution or decoration changes. Clamp the live frame
  to the available screen without destroying the larger-screen preference.
- Preserve the user's normal placement through maximize, minimize and restore;
  a manual resize from maximized state is a new user preference.
- Separate automatic layout/font adjustments from explicit user geometry.
- Configuration write failures must leave a usable window and the old record.

The existing ABI exposes current window state and resize events. `wm_move()`
does not emit an application move notification. The design must settle a
completed-geometry notification or equivalent lifecycle contract before coding;
closing-only persistence does not meet Chris's request. Assess any kernel/API
addition under D012 and seek architectural review if its scope warrants it.
No general window-manager persistence policy has been approved here.

## Acceptance outline

Move/resize/relaunch/reboot on P5; resolution down/up round trip; maximize and
manual resize; concurrent instances; read-only configuration; interrupted
write; a second consumer demonstrating shared support. Name the additional
apps with Chris when selecting consumers. No test or implementation result is
claimed. The PR #322 fixes and E5C review take precedence.
