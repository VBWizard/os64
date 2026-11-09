# F2.5 consumer foundation receipt

Foundation commit: **788de9900282e941a7a93aa110ffa69154daa066**.
Coordinator branch: `codex/font-provider`.
Accepted F2 parent: `4b0a839b6f2c0c6baea3b1eb060651e7f636fe46`.
This identifier is a commit object, not a tag object.

The shared provider/adoption interfaces and FONT_PROVIDER.md at that commit
are the common baseline for F3 and F4. This is a coordinator development
freeze backed by the F25-REPORT evidence, not independent review or merge
approval. The foundation is local on the shared machine; publication follows
review/approval. Do not silently replace it with an older remote branch.

| Header | SHA256 |
|---|---|
| `userland/libos64/include/os64/font_provider.h` | `d0c9598ef2102680b74188c714b4efc10d820850c952dcaf8efc893144897463` |
| `userland/libos64/include/os64/font_adopt.h` | `6fc28b87904d0e08c0af3deff97e75ff9026cfe6adcd8885ac4c5878f6f6f7c8` |

Opus starts `opus/font-widgets` in `.worktrees/font-widgets` from this commit;
see [OPUS-F4-HANDOFF.md](OPUS-F4-HANDOFF.md) for the complete assignment and
copyable worktree commands. Quinn's F3 branch starts from the same foundation.
The assignment/receipt are a documentation follow-up, so the pinned foundation
precedes this file. Consumers should read the handoff before creating their
worktree. No runtime interface is added by this receipt.

If a shared contract deficiency appears, coordinate the proposed change and
its test before changing either consumer's interface. F3/F4 do not need the F5
settings UI or session publisher to begin their assigned implementations.
