# Font package handoff template

Copy this into an assignment and fill the fields. Include the linked repository
documents or provide a checkout containing them; no prior chat is required.

```text
Task: implement/audit package F__ from docs/fonts/____.md.
Repository/remote:
Working branch and isolated worktree:
Base commit:
Frozen contract commit (or explicitly "source audit only; no API frozen"):
Prerequisite package commits:
Deliverable for this assignment:
Owned files and permitted shared-file changes:
Coordinator/contact for interface questions:
Publication requested (local result / commit / push / PR):

Read AGENTS.md, FONTS.md, FONTS_WORK_PLAN.md and your packet before editing.
Distinguish settled requirements from draft proposals. Use the frozen interfaces;
do not independently change the ABI, schema, encoding scope or kernel interfaces.
Return contract questions with a concrete proposal and supporting code evidence.
Complete independent work while a dependent decision is pending.

Report:
- Behavior delivered and exact commit(s), or changed files if uncommitted.
- Contract revision and any deviations requiring integration.
- Test commands/results, separated into host, cross-build, QEMU and hardware.
- Unrun checks, known limitations, and remaining work.
- Status of requested publication; do not imply a PR was merged when it was not.
```

Accepted product scope to preserve: installable outline TTF/OTF fonts;
independent interface/terminal/document choices; monospace terminal and
proportional-or-monospace Scribe; UTF-8 Western text and symbols first;
byte-preserving document storage; complex-script shaping as a later slice.

For the first font-envelope deployment, refresh the compatible library and
reboot before publishing font keys. Include this in F5's install instructions
and evidence; old resident libos64 instances do not gain compatibility from
a file refresh. See FONT_CONTRACTS and APPEARANCE for the full envelope rule.
