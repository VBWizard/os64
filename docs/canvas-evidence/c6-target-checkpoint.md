# C6 captured-page checkpoint

2026-11-01, integration baseline `684f8665` (after C6d).

Danlegt remains a useful diagnostic, but is not the current release acceptance
target. The next real-page checks use
[Curran's HTML5 Canvas examples](https://curran.github.io/HTML5Examples/).

## Captured Danlegt behavior

A private capture keeps the public page, stylesheets, scripts, images and
signature GET responses with URL, length and SHA-256 provenance. The replay
uses the production parser, cascade, layout, DOM, Canvas and SVG code; transport
is local and sends nothing to the site. It loads the original external scripts
through the browser's script queue. Public signature images and the captured
page are not redistributed in this repository.

- The wall reports all 131 captured signatures loaded and renders their
  composed bitmap, including transformed images and shadows.
- The selected cursor effect is the vendor `rainbowCursor`, initialized by
  `home/qrcode-rainbow-cursor.js` inside the QR-code panel. Its own 830 × 470
  canvas changes from empty to a colored trail after native mouse movement and
  scheduled frames. The page also contains a separate hidden dither canvas;
  that is not the cursor effect's drawing surface.
- Normal game startup does not complete. `main.js` invokes chat before Scratch
  2000 in one `DOMContentLoaded` callback. Chat constructs an absent
  `IntersectionObserver`, so that callback never reaches the game initializer.
- Calling the unchanged game initializer separately exposes another blocker:
  sound setup constructs `Audio` before wallet and ticket setup. That global
  is absent too. These are startup dependencies, not failed Canvas draws.
- A further diagnostic directly initializes the unchanged wallet and ticket
  modules. The floppy purchase deducts five coins, paints its coating, accepts
  native pointer strokes, reveals the prize, and credits the local wallet.
  This bypass is diagnostic only; it does not establish live-site acceptance.
- The full-style modal replay is not accepted: the continue button's native
  hit target differs from the intended control, and CD-ROM sectors measure
  zero CSS area in this harness. The earlier stripped CD-ROM fixture's four
  successful reveals remain narrower evidence. CRT completion is unverified.

The wall/cursor run completes 608 harness checks with ASan, UBSan and
LeakSanitizer enabled and no tracked allocations remaining. Check counts are
not a claim that the complete site works. The harness uses its layout font
fixture, supports PNG/SVG image decoding, and refuses unrelated uncaptured
requests; other raster formats and real web-font layout need native validation.

## Captured script identities

| Script | SHA-256 |
|---|---|
| `signature_wall.js` | `2b6247abd711b1fd00bdf9eb813fa093bcc32aed6b98c1bf312da08b8e97a921` |
| `scratch2000/ticket-modal.js` | `3db20d6f4891968a642f1998c33fe35d967c5968c0040c1026cad7f3174b0def` |
| `scratch2000/wallet.js` | `7ad6306fb5433498b9b2772739c37d8c0a50426c3fbfda3963c37862eba07403` |
| `vendor/cursor-effects.min.js` | `19afd3c18b591e805118ab4d5e9e4d3bfe81a6aa3b91c804ccd2361b3def999c` |
| `home/qrcode-rainbow-cursor.js` | `87cc946db1448e626393a3468b073867d6172b7fdd354039676a129b8369f460` |

Observer and media support require their own contracts and validation. No
placeholder globals or site-specific exceptions were added to bypass them.
