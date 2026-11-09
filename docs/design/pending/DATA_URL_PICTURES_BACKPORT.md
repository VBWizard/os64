# Data-URL pictures: userland backport brief

Prepared by Quinn for Opus, 2026-10-31. Chris requested a separate backport so
ordinary browsing can benefit before the Canvas integration is promoted.

## Outcome and base

Target a small PR against current `userland`, in an isolated worktree. Display
embedded `data:` images through Yonder's existing picture pipeline, including
markup images and CSS backgrounds. Chris observed that Google's image results
show thumbnails on `canvas-integration` where userland shows white placeholders.
That observation motivates the backport; deterministic fixtures must establish
correctness independently of the live site's changing content.

Reference implementation: `60c1da487abf67707a8610463ae908fa00bfb439`
("Add Canvas image drawing and PNG export", C4 / PR #408), available on
`canvas-integration`. Select the picture-display changes; do not cherry-pick
that entire commit. Current userland already exports `yonder_fetch_data_url`
from `fetch_job.c` / `fetch_job.h`, including bounded input/body processing,
percent decoding, forgiving base64, MIME handling and fragment removal.
Reuse it rather than adding a decoder or reverting newer Fetch work.

## Expected implementation

The production changes should principally fit in these three files; tests and
truthful documentation are additional scope. Use current userland's surrounding
code and allocation/error conventions rather than restoring old file versions.

1. `userland/apps/yonder/yonder.c`
   - Let `picture_for` admit data URLs while preserving the existing catalog's
     duplicate lookup, entry limit and aggregate URL-byte budget.
   - In `pictures_feed`, copy a data URL into job-owned storage: the fixed
     `job->url[OS64_FETCH_URL_MAX]` cannot carry typical embedded images intact.
   - Account for copy failure through the existing picture-failure path. Free
     the owned copy if worker submission fails. Preserve waiting/in-flight
     accounting and diagnostics, including a page whose sole picture fails.
2. `userland/apps/yonder/picture.h`
   - Add the owned long-URL pointer to `yonder_picture_job_t`, with its lifetime
     documented. Existing callers must zero-initialize it.
3. `userland/apps/yonder/picture.c`
   - Include the existing decoder declaration. Select the owned URL when
     present; decode data-URL bytes with `OS64_IMAGE_CAP_DEFAULT`, then feed
     bytes and returned MIME type to the existing picture `decode` function.
   - Free temporary decoded bytes on success and failure. Keep raster signature
     dispatch, animated-GIF display behavior and SVG's existing Modern Web gate
     and secure-static processing mode.
   - Release the job-owned URL in `yonder_picture_release`, including when no
     product was produced. Respect cancellation and generation teardown.

The integration implementation uses lowercase-prefix checks. Check the current
URL-resolution contract rather than copying that assumption blindly: the shared
data decoder accepts case-insensitive schemes. Cover mixed-case input through
the actual picture entry path so catalog and worker agree.

Keep existing catalog/input/body/pixel budgets. Refused or malformed content
must settle as a failed picture, with no network attempt and no stuck pending
count. Job-owned URL memory is additional to the catalog's copy; retain bounded
worker concurrency and the established release paths.

## Keep this independent of Canvas and relayout

Do not import C4's `canvas_request`, `origin_clean`, URL-plus-request-mode keys,
`canvas_source`, image loans, image-element completion tasks, drawImage,
toDataURL, PNG encoding, or the native opaque-response Fetch entry point.
Those belong to Canvas's image acquisition and script-readback contract.
Display-only data images can use the existing URL-only catalog and decoder.

No new canvas/draw libraries, public DOM API, layout reuse, text-run retention,
or packed text allocations are needed. PR #439 remains separate and held for
Opus's review; this backport must not depend on it. Preserve current userland's
HTTP/file picture behavior and Fetch/CORS behavior.

## Verification

Use existing picture/browser fixtures as the starting point:
`tools/test_yonder_svg_images.inc`, `tools/test_yonder_scripts_host.c`, and
`tools/test_yonder_fetch_job_host.c`. Add focused coverage for:

- A valid base64 raster image and a percent-encoded SVG; verify the decoded
  result and dimensions, not merely catalog admission. Exercise a data URL
  longer than `OS64_FETCH_URL_MAX` to catch truncation.
- Markup and CSS-background entry paths; duplicate URLs reuse a catalog entry.
  Raster pictures remain available with Modern Web off; SVG follows its
  existing feature gate and must not fetch external resources.
- Malformed/base64-invalid data, bounded refusals and decoder failures: no
  network/file fetch, no leak and settled picture accounting.
- Allocation failures for the URL copy and decoder, worker-submit refusal,
  cancellation, and navigation before completion. Exercise the worker's real
  release path rather than freeing its allocations in the fixture by hand.
- Existing HTTP/file images and animated-GIF display remain covered.

Several current diagnostic fixtures deliberately use `data:` as an unsupported
scheme. Update those to an actually unsupported scheme while preserving the
original refusal/deduplication assertions. Add separate assertions for the new
supported path and malformed-image failures. Relevant test functions include
`diag_picture_unasked_case` and `diag_css_unasked_case`.

Run the affected browser/picture and Fetch suites with ASan/UBSan and leak
checks enabled, plus the target Yonder build. Run relevant existing mutation
checks according to the repository workflow. Tests that specifically prove the
new route should fail against the prior userland implementation. A small static
fixture with visible inline raster/SVG and CSS images is suitable for the P5;
Chris can also revisit the Google result page after installing the cohort.

## Documentation, delivery and reconciliation

Update the data-picture row in `DEBTS.md` to reflect the implemented scope.
Inspect comments around `picture_unasked` and picture actions: some name data
URLs as unsupported, while a save/open action may still legitimately have a
separate limitation. Correct false claims without promising new action support.
Run `tools/stale_refs.sh` and read its output before committing.

Use a small frozen delivery with tested binaries, matching unchanged libraries,
fixtures and SHA-256 manifest. Follow Chris's established Windows-server handoff.
The PR should state that it targets `userland` and is a selective display-only
backport of C4, with validation and remaining limits. Coordinate the P5 install
with Chris; its current cohort must be verified before interpreting tests.

After merge, send Chris the PR/commit and changed files for Quinn's integration
reconciliation. The picture route already exists there: merge the backport
forward deliberately, retaining Canvas's additional request-mode and ownership
fields, and bring over any improved diagnostics/tests or fixes. Do not duplicate
the decoder or replace the richer integration files with userland versions.
