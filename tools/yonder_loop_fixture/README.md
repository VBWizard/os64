# D7 loop fixture

The guest pages for DOM_D7.md's J4 evidence, and DOM_D9.md's `write.html`.
Serve this directory on the development machine and open the pages in yonder
with **Run page scripts** on:

```sh
python3 -m http.server 8000 --bind 127.0.0.1 --directory tools/yonder_loop_fixture
```

In a QEMU guest with user networking the address is `http://10.0.2.2:8000/`.

| Page | What it shows |
|---|---|
| `order.html` | inline, blocking `src`, `defer`, `async`, a connected script, a module that never runs, DOMContentLoaded, load and `<body onload>`, each noted in the order it ran |
| `clock.html` | `setInterval` updating a heading once a second, still ticking behind another window |
| `onload.html` | `<body onload>`, window's handler, with `this` being window |
| `runaway.html` | `while(true){}` stopped at the script time limit; the page goes on without script and its link still works |
| `write.html` | `document.write` (DOM_D9.md): a counter, a date line, a written `<script src>` loading `blocking.js`, whose own write follows it, and a written inline script whose write lands before its writer's next one; the page's own last paragraph comes after them all |
| `menu.html` | a DHTML menu opening on `mouseover` and closing on `mouseout` |
| `form.html` | an `onsubmit` validator: an empty name is refused, a filled one reaches the server's log as `/signed?name=...` |

`yonder --script-audit URL` logs each task's name and cost to the kernel log.
