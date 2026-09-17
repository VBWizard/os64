# F0 frozen contract received by F1

Contract commit: `23bf6dddfd1077bf844c661d8762a9b52e3a68f9` on `codex/font-design`.
Fable's R3 review approves backend, layout and configuration contracts.
The backend remains table revision 1; F1's header is byte-identical to the
header at that commit. B1's correction is recorded in F1-B1-REPORT.md.

The full contracts, handoffs, review and freeze receipt are in the sibling
`.worktrees/font-design` checkout. On this shared repository, the immutable
contract is available with `git show 23bf6dddfd1077bf844c661d8762a9b52e3a68f9:FONT_CONTRACTS.md`.
No scratchpad or chat transcript is needed to retrieve it.

F1's original R1 report and B1 report remain historical evidence. This receipt
updates the freeze status; it does not accept the complete implementation.
Next review includes B1, bare CFF2 refusal, the target runtime compiler-flag
regression, malformed/truncated/duplicate metadata fixtures and a fresh QEMU run.
Opus's existing guest evidence predates B1. The F1 worktree remains uncommitted.
