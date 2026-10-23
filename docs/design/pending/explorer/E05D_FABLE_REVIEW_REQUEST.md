# E5D — Request for Fable architectural review

2026-10-23. Quinn's design revision 1:
[E05D_WINDOW_PLACEMENT.md](E05D_WINDOW_PLACEMENT.md).

Chris authorized reusable remembered position/size for Roam and Scribe, preferably
as an initialization opt-in. He explicitly asked for Fable review if this slice
needs kernel work. It does: the proposal adds a user-geometry completion event,
retained/coalesced outside the input ring, without changing event size or adding
a syscall. Persistence stays in libos64. Shared file dialogs adopt size-only mode.

Worktree: `/home/yogi/src/os64/.worktrees/window-placement`.
Branch: `codex/window-placement`, based on `12dc6b64`.
No implementation or runtime validation is claimed. No Fable response has been
received. Please place the review beside the design; the five questions at its
end identify the boundaries needing architectural review.

The proposed ownership, locking, cancellation, queue fairness, configuration
publication and regression matrix are in the design. File attributes, worker
lifetime and transfer-service work are explicitly deferred.
