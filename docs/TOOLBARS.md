# Toolbars

`os64_ui_toolbar_t` is a retained libos64 widget for labeled command buttons.
Scribe uses it; applications can share the component without sharing Scribe's
editing or file behavior. It uses the existing Appearance button colors,
including hover, pressed, border, text, highlight and shadow. Icons keep their
artwork colors and dim when disabled. There is no separate toolbar palette.

The public API is in [ui_toolbar.h](../userland/libos64/include/os64/ui_toolbar.h).
The component handles button construction, group spacing, wrapping, icon leases,
enabled states and hover/focus help lookup. It uses action-registry labels,
icons and help; the app supplies command IDs and a callback that chooses the
action view to invoke. Commands are supported; toggle/radio actions are refused.
The capacity is `OS64_UI_TOOLBAR_MAX` (32 buttons).

## Add and wire

Store the toolbar alongside the app's other widgets. Initialize it before
adding `toolbar.w` to the widget tree, then bind once outside dispatch:

```c
os64_ui_toolbar_init(&app.toolbar);
os64_ui_add_child(&app.root, &app.toolbar.w);

const os64_ui_toolbar_entry_t items[] = {
    {CMD_OPEN, NULL, false},
    {CMD_SAVE, NULL, false},
    {CMD_COPY, NULL, true},   // start a command group
    {CMD_HELP, "Help", true}
};
os64_ui_status_code_t rc = os64_ui_toolbar_bind(&app.toolbar, app.actions,
    items, sizeof(items) / sizeof(*items), toolbar_command, &app);
```

Check `rc` before using the binding. The registry is retained, entry descriptors
and labels are copied, and a rejected binding adds no buttons. A NULL label
uses the action's label. The callback receives the activated button, command
ID and app pointer; invoke the chosen view with `os64_ui_action_invoke`.
Scribe's callback also restores editor/field focus if the command left focus
on its button. A terminal can choose its own focus policy instead.

Once the icon theme exists, call `os64_ui_toolbar_icons` with its provider.
Do this before painting. A failed acquisition preserves that button's current
image or text fallback; other icons are still attempted. The provider must
outlive its leases.

After input dispatch, call `os64_ui_toolbar_refresh` with the app's current
action view. It checks registry identity and updates enabled states without
retaining the view. Invoke still rechecks action validity and enabled state.
Use `os64_ui_toolbar_help` to retrieve the hovered/focused command ID and
borrowed help text. Pointer help takes precedence. An app can place that text
in its status widget and suppress it while a menu owns status help, as Scribe
does. Hiding uses the normal `os64_ui_set_hidden` widget API.

## Layout and fonts

Measure first; apply only after the rest of the window also fits:

```c
os64_ui_toolbar_plan_t plan;
os64_font_status_t rc = os64_ui_toolbar_measure(&app.toolbar, &app.ui,
    left, top, available_width, &plan);
if (rc != OS64_FONT_OK) return rc;
// Reserve plan.bounds.h above the application's content.
os64_ui_toolbar_apply(&app.toolbar, &plan, false);
```

The plan contains the bar and button rectangles. Labels are measured in the
active font, or the candidate font during adoption. Buttons wrap when needed;
a group adds space within a row but no leading gap on a new row. An empty bar
has zero height. A button wider than the available width refuses the plan.
Measurement leaves live geometry and the output plan unchanged on failure.

For an atomic font change, include the toolbar plan in the app's existing
window plan and call `os64_ui_toolbar_apply(..., true)` when staging it.
The toolkit's font transaction commits or discards the staged rectangles.
A hidden toolbar can be omitted from the app's layout. Plans belong to the
bar that measured them and should be applied before changing items or fonts.

## Ownership and validation

Cancel interactions before teardown. Call `os64_ui_toolbar_destroy` before
destroying the icon provider or action registry, then release the widget tree's
fonts before freeing widget storage. Destroy releases icon leases and the
registry reference; it is idempotent and also runs during tree teardown.
The toolbar cannot be rebound after destruction. Font reface preserves its
registry and images while refreshing caption runs.

`python3 tools/test_ui_action_host.py` exercises the component independently
of Scribe: binding validation, copied labels, grouping/wrapping, refusal without
mutation, registry identity, dispatch, help and teardown. Scribe's host suite
adds editor/field targeting, icon ownership and font-failure checks:
`python3 tools/test_scribe_host.py` and
`python3 tools/test_scribe_host.py --menus`. These use ASan, UBSan and leak checks.
