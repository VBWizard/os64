# F0 contract baseline

Fable's [R3 verdict](FABLE-REVIEW-R3.md) approves the backend, layout and
configuration contracts. The freeze commit identity is recorded after creating
the approved snapshot; no identity has been recorded in this version of this file.

The baseline comprises FONT_CONTRACTS.md, the three os64 font/text headers,
and tools/fonts fixtures: backend table revision 1 and fixture schema revision 2.
R3 to this snapshot changes header status comments, not declarations or semantics.
F2 also receives the requested note about serializing complete engine operations.

F1's B1 correction is in the separate opus/freetype-backend worktree; its
F1-B1-REPORT.md and the selected source/evidence hashes in f0-review-inputs.json
record the exact implementation inputs. The original R2 and R3 manifests and
Fable verdicts are preserved. Design approval does not accept the F1 implementation.

Next review: complete F1 implementation, including B1, bare CFF2 refusal, the
compiler-loop-pattern runtime tripwire, malformed/truncated/duplicate metadata
names, and a fresh QEMU run. F2 owns spawn-latency measurement before integrating
the production libos64-to-libfreetype dependency. F5 owns the appearance protocol
and reboot-rollout tests. No kernel change or publication is part of this freeze.
