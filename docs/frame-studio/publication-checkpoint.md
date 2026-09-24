# Frame Studio: review stack and publication validation

The completed feature is arranged as three dependent review units:

1. `codex/shared-text-profile` against `userland`: strict UTF-8 scalar decoding,
   bounded Western clustering and shared generated Unicode tables. No kernel
   implementation changes. Commit `81820a4`.
2. `codex/ui-editing` against `codex/shared-text-profile`: textfield selection and
   clipboard behavior, keyboard list visibility, and stable HSV editing. No
   kernel changes. Commit `7c81028`.
3. `codex/frame-studio` against `codex/ui-editing`: prepared decoration ABI,
   shared painter, kernel publication and window-manager integration, userland
   preparation/startup, Frame Studio and persistent compositions. Its build,
   test fixtures, design and integrated evidence travel with the feature.

Merge in order, retargeting the dependent PRs to `userland` as their prerequisites
land. Review and merge are separate from the validation recorded here. The
composition collection requested after Studio completion is not included.

## Validation at the review boundaries

- The exact first commit was checked in an isolated worktree: strict userland
  build, [3,336,108 sanitized UTF-8 checks](publication/utf8.txt),
  [7,161 frozen text-layout checks](publication/text.txt), and
  [pinned Unicode regeneration](publication/generated.txt).
- The exact second commit was checked in another isolated worktree: strict
  userland build, [sanitized appearance/list/picker checks](publication/appearance.txt),
  and [1,430 real-font UI-text checks](publication/ui-text.txt). Its Scribe and
  field changes also passed [2,470 sanitized Scribe checks](publication/scribe.txt)
  in the integrated checkout.
- The integrated feature passed the strict kernel/userland/ISO build and
  [sanitized decoration/storage/startup suite](publication/decoration.txt).
  The tested implementation is the same as the final per-slice guest evidence;
  publication preparation adjusted only documentation and a header comment.
- QEMU coverage and screenshots are in the typography, controls, editor,
  two-color, saving, button-fill, navigation/picker, startup, deletion, active
  lookup and symbol checkpoints. The final cold-boot and V4/V5/V6 publication
  evidence is in [symbol-checkpoint.md](symbol-checkpoint.md).
- Chris reported successful P5 use of deletion, startup selection, automatic
  active-composition selection, and independent button-symbol colors. These
  reports supplement the host/QEMU evidence; they are not an independent code
  review. Older checkpoints describe their then-current hardware status.

The combined implementation includes the vncd/ZRLE base at `be2d755`. The
separate main `userland` checkout and its unrelated working changes were not
used for edits. Private QEMU data and normal boot configuration were restored
before publication preparation.
