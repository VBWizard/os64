#!/usr/bin/env python3
"""Drive an os64 GUI guest over QMP: real key-down/key-up, mouse drags, shots.

HMP's `sendkey` presses and releases as one unit, so a modifier DRAG — hold
Ctrl+Alt, then move the mouse — cannot be expressed with it (and `sendkey
ctrl-alt <hold_ms>` was observed delivering only the first key of the combo).
QMP's input-send-event takes each edge separately, which is exactly what a
chord-plus-drag needs.
"""
import json
import socket
import sys
import time


class Qmp:
    def __init__(self, host="127.0.0.1", port=55557):
        self.s = socket.create_connection((host, port), timeout=30)
        self.f = self.s.makefile("rw", encoding="utf-8", newline="\n")
        self._read()                      # greeting
        self.cmd("qmp_capabilities")

    def _read(self):
        while True:
            line = self.f.readline()
            if not line:
                raise EOFError("qmp closed")
            msg = json.loads(line)
            if "event" in msg:            # async noise; keep reading
                continue
            return msg

    def cmd(self, name, **args):
        payload = {"execute": name}
        if args:
            payload["arguments"] = args
        self.f.write(json.dumps(payload) + "\n")
        self.f.flush()
        reply = self._read()
        if "error" in reply:
            raise RuntimeError(f"{name}: {reply['error']}")
        return reply.get("return")

    # ── input ──────────────────────────────────────────────────────────────
    def _events(self, events):
        self.cmd("input-send-event", events=events)
        time.sleep(0.06)

    def key(self, qcode, down):
        self._events([{"type": "key",
                       "data": {"down": down,
                                "key": {"type": "qcode", "data": qcode}}}])

    def tap(self, qcode):
        self.key(qcode, True)
        self.key(qcode, False)

    def btn(self, button, down):
        self._events([{"type": "btn", "data": {"down": down, "button": button}}])

    def move(self, dx, dy):
        # PS/2 packets carry a signed byte per axis; walk anything larger.
        while dx or dy:
            sx = max(-120, min(120, dx))
            sy = max(-120, min(120, dy))
            ev = []
            if sx:
                ev.append({"type": "rel", "data": {"axis": "x", "value": sx}})
            if sy:
                ev.append({"type": "rel", "data": {"axis": "y", "value": sy}})
            self._events(ev)
            dx -= sx
            dy -= sy

    def drag(self, dx, dy, steps=6):
        """Move in steps, so the guest sees a real gesture, not a teleport."""
        for i in range(steps):
            self.move(round(dx * (i + 1) / steps) - round(dx * i / steps),
                      round(dy * (i + 1) / steps) - round(dy * i / steps))

    def shot(self, path):
        self.cmd("screendump", filename=path)
        time.sleep(0.3)


def main():
    q = Qmp(port=int(sys.argv[1]) if len(sys.argv) > 1 else 55557)
    script = sys.argv[2] if len(sys.argv) > 2 else ""
    exec(compile(open(script).read(), script, "exec"), {"q": q, "time": time})


if __name__ == "__main__":
    main()
