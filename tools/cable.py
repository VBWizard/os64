#!/usr/bin/env python3
# cable.py — a network cable with weather in it, for the QEMU harness.
#
#   tools/cable.py [--port 7100] [--seed N] [--up-loss 0.1 --down-delay 40 ...]
#   tools/cable.py ctl set up loss 0.3        # change the weather mid-run
#   tools/cable.py ctl link down off          # cut one direction
#   tools/cable.py ctl stats                  # what happened so far
#
# WHY THIS EXISTS. os64's TCP is LAN-calibrated: its fetches have never
# lost a segment, never seen one reordered, never measured a delay worth
# backing off for. The counters in /sys/net/tcp exist to decide WHICH of
# the booked TCP debts gets paid, with data — and slirp's NAT is a perfect
# network, so on this harness those counters read zero forever. Packet-
# level impairment normally means a tap device and tc netem, which means
# root, which the harness does not have. This is the way around it.
#
# HOW IT WORKS. QEMU's filter-redirector (the COLO building block) hands
# every frame on a netdev out over a chardev socket and takes frames back
# in from another, continuing them down the chain. Two redirectors, one
# per direction, put this process IN the cable between the guest's NIC
# and slirp: frames arrive here, some are dropped, delayed, duplicated or
# held back behind their successors, and the survivors are handed back.
# The guest keeps its virtio NIC, slirp keeps its NAT, nothing is
# installed. The wire format on each socket is a 4-byte big-endian length
# followed by the raw Ethernet frame (net/filter-mirror.c's filter_send).
#
# DIRECTION NAMES, AND THE TRAP UNDER THEM. Here `up` is guest → world
# and `down` is world → guest, always from the guest's point of view.
# QEMU's queue= names are from the NETDEV's point of view, and the netdev
# is slirp: a filter on n0 with queue=rx sees packets sent TO slirp (our
# `up`), queue=tx sees packets sent BY slirp (our `down`). The GNUmakefile
# flags pair them correctly; if you wire your own, remember that rx is up.
#
# NEVER DROP A BYTE SILENTLY. Every frame is counted — passed, lost, held
# by a dead link, blackholed, duplicated, reordered — per direction, and
# the counts print on `stats` and at exit. A frame that could not be
# handed back because QEMU's inbound socket is not connected is counted
# too (`orphaned`), never lost in silence.
#
# Ports (base 7100): base = control; +1/+2 = up out/in (QEMU→cable,
# cable→QEMU); +3/+4 = down out/in. This process is the SERVER on all
# five and QEMU connects to it, so START THE CABLE FIRST and leave it up
# for the guest's whole life: a run with no cable listening at QEMU start
# passes frames through untouched (no cable, no weather), but a cable that
# goes away mid-run wedges QEMU 8.2's redirectors — half the chardevs never
# reconnect and the sends that remain fail EPERM — so weather is changed
# live through `ctl`, never by restarting this process.

import argparse
import asyncio
import math
import random
import signal
import socket
import struct
import sys
import time


def probability(value):
    parsed = float(value)
    if not math.isfinite(parsed) or not 0.0 <= parsed <= 1.0:
        raise ValueError('must be a finite probability from 0 to 1')
    return parsed


def nonnegative_int(value):
    parsed = int(value)
    if parsed < 0:
        raise ValueError('must be nonnegative')
    return parsed


PARAMS = {
    # name: (default, parser, meaning)
    'loss':        (0.0, probability, 'probability a frame is dropped'),
    'delay':       (0.0, float, 'milliseconds added to every frame'),
    'jitter':      (0.0, float, 'milliseconds of ± noise on the delay'),
    'reorder':     (0.0, probability, 'probability a frame is held back behind its successors'),
    'reorder_ms':  (50.0, float, 'how long a held-back frame waits'),
    'dup':         (0.0, probability, 'probability a frame is delivered twice'),
    'blackhole':   (0, nonnegative_int, 'drop after this many subsequent frames (0 = never)'),
}
DIRECTIONS = ('up', 'down')


class Direction:
    def __init__(self, name, seed):
        self.name = name
        self.rng = random.Random(seed)
        self.params = {k: v[0] for k, v in PARAMS.items()}
        # Absolute `seen` cutoff; live blackhole settings translate their
        # relative frame count here when the control command arrives.
        self.blackhole_at = None
        self.link = True
        self.last_due = 0.0         # loop.time() the previous frame is due; order is kept behind it
        self.writer = None          # the QEMU side we hand survivors to
        self.connection_generation = 0  # identifies the QEMU run that supplied a scheduled frame
        self.counts = {k: 0 for k in ('seen', 'passed', 'lost', 'linkdown', 'blackholed',
                                      'duplicated', 'reordered', 'delayed', 'orphaned')}
        self.bytes_in = 0
        self.bytes_out = 0

    def begin_connection(self):
        self.connection_generation += 1
        self.last_due = 0.0
        return self.connection_generation

    def end_connection(self, generation):
        if self.connection_generation == generation:
            self.connection_generation += 1
            self.last_due = 0.0

    def set_param(self, name, value, live=False):
        self.params[name] = value
        if name == 'blackhole':
            if not value:
                self.blackhole_at = None
            elif live:
                self.blackhole_at = self.counts['seen'] + value
            else:
                self.blackhole_at = value

    def reset_counts(self):
        for key in self.counts:
            self.counts[key] = 0
        self.bytes_in = self.bytes_out = 0
        self.blackhole_at = self.params['blackhole'] or None

    def weather(self, frame, loop, generation):
        c = self.counts
        p = self.params
        c['seen'] += 1
        self.bytes_in += len(frame)
        if generation != self.connection_generation:
            c['orphaned'] += 1
            return
        if not self.link:
            c['linkdown'] += 1
            return
        if self.blackhole_at is not None and c['seen'] > self.blackhole_at:
            c['blackholed'] += 1
            return
        if p['loss'] > 0 and self.rng.random() < p['loss']:
            c['lost'] += 1
            return
        delay = p['delay']
        if p['jitter'] > 0:
            delay += self.rng.uniform(-p['jitter'], p['jitter'])
        # A pipe does not let frames overtake: jitter varies the spacing,
        # never the order. Delivery is clamped behind the previous frame's,
        # so ONLY the reorder knob reorders — otherwise a delay leg measures
        # the receiver's reassembly instead of its clock, and against a
        # receiver that drops out-of-order segments the two differ by an
        # order of magnitude.
        # Strictly later than the previous frame, never merely equal: the
        # event loop's timer heap orders ties arbitrarily, and a burst
        # clamped to one instant came back shuffled.
        due = max(loop.time() + max(0.0, delay) / 1000.0, self.last_due + 1e-6)
        self.last_due = due
        if p['reorder'] > 0 and self.rng.random() < p['reorder']:
            due += p['reorder_ms'] / 1000.0
            c['reordered'] += 1
        self.schedule(frame, due, loop, generation)
        if p['dup'] > 0 and self.rng.random() < p['dup']:
            c['duplicated'] += 1
            self.schedule(frame, due, loop, generation)

    def schedule(self, frame, due, loop, generation):
        if due > loop.time():
            self.counts['delayed'] += 1
            loop.call_at(due, self.emit, frame, generation)
        else:
            self.emit(frame, generation)

    def emit(self, frame, generation):
        # A delayed frame belongs to the QEMU connection that supplied it.
        # A replacement guest must start with an empty wire, not inherit the
        # previous guest's in-flight traffic.
        if generation != self.connection_generation:
            self.counts['orphaned'] += 1
            return
        w = self.writer
        if w is None or w.is_closing():
            self.counts['orphaned'] += 1
            return
        w.write(struct.pack('>I', len(frame)) + frame)
        self.counts['passed'] += 1
        self.bytes_out += len(frame)

    def stats_line(self):
        c = self.counts
        return (f"{self.name:4} seen {c['seen']} passed {c['passed']} lost {c['lost']} "
                f"linkdown {c['linkdown']} blackholed {c['blackholed']} dup {c['duplicated']} "
                f"reordered {c['reordered']} delayed {c['delayed']} orphaned {c['orphaned']} "
                f"bytes {self.bytes_in}/{self.bytes_out} link {'on' if self.link else 'off'} "
                + ' '.join(f"{k}={v}" for k, v in self.params.items() if v))


class Cable:
    def __init__(self, seed):
        self.dirs = {'up': Direction('up', seed), 'down': Direction('down', seed + 1)}
        self.loop = None
        self.started = time.time()

    # ── the QEMU side ──
    async def serve_out(self, direction, reader, writer):
        # QEMU → cable: frames in, weather applied, survivors scheduled.
        d = self.dirs[direction]
        generation = d.begin_connection()
        peer = writer.get_extra_info('peername')
        print(f"cable: {direction} out connected from {peer}", flush=True)
        try:
            while True:
                hdr = await reader.readexactly(4)
                (n,) = struct.unpack('>I', hdr)
                frame = await reader.readexactly(n)
                d.weather(frame, self.loop, generation)
        except (asyncio.IncompleteReadError, ConnectionResetError):
            pass
        finally:
            d.end_connection(generation)
            print(f"cable: {direction} out disconnected", flush=True)
            writer.close()

    async def serve_in(self, direction, reader, writer):
        # cable → QEMU: we only ever write here; hold the writer.
        d = self.dirs[direction]
        d.writer = writer
        print(f"cable: {direction} in connected", flush=True)
        try:
            await reader.read()          # returns at EOF: QEMU went away
        except ConnectionResetError:
            pass
        finally:
            if d.writer is writer:
                d.writer = None
            print(f"cable: {direction} in disconnected", flush=True)
            writer.close()

    # ── the control side ──
    def command(self, line):
        words = line.split()
        if not words:
            return 'err empty'
        verb = words[0]
        if verb == 'stats':
            return '\n'.join(d.stats_line() for d in self.dirs.values())
        if verb == 'reset':
            for d in self.dirs.values():
                d.reset_counts()
            return 'ok'
        if verb == 'quit':
            self.loop.call_soon(self.loop.stop)
            return 'ok'
        if verb == 'link' and len(words) == 3 and words[2] in ('on', 'off'):
            for d in self.pick(words[1]):
                d.link = (words[2] == 'on')
            return 'ok'
        if verb == 'set' and len(words) == 4 and words[2] in PARAMS:
            try:
                value = PARAMS[words[2]][1](words[3])
            except ValueError:
                return f'err bad value {words[3]!r}'
            for d in self.pick(words[1]):
                d.set_param(words[2], value, live=True)
            return 'ok'
        return ('err usage: stats | reset | quit | link <up|down|both> <on|off> | '
                'set <up|down|both> <param> <value>; params: ' + ' '.join(PARAMS))

    def pick(self, which):
        if which == 'both':
            return list(self.dirs.values())
        if which in self.dirs:
            return [self.dirs[which]]
        raise KeyError(which)

    async def serve_ctl(self, reader, writer):
        try:
            while True:
                line = await reader.readline()
                if not line:
                    break
                try:
                    reply = self.command(line.decode('utf-8', 'replace').strip())
                except KeyError as e:
                    reply = f'err no such direction {e}'
                writer.write((reply + '\n').encode())
                await writer.drain()
        except ConnectionResetError:
            pass
        finally:
            writer.close()

    def dump_stats(self):
        print(f"cable: {time.time() - self.started:.0f}s of weather", flush=True)
        for d in self.dirs.values():
            print('cable: ' + d.stats_line(), flush=True)


async def run(args):
    cable = Cable(args.seed)
    cable.loop = asyncio.get_running_loop()
    for d in DIRECTIONS:
        for k in PARAMS:
            v = getattr(args, f'{d}_{k}')
            if v is not None:
                cable.dirs[d].set_param(k, v)

    def bind(handler, port):
        return asyncio.start_server(handler, '127.0.0.1', port)

    servers = [
        await bind(cable.serve_ctl, args.port),
        await bind(lambda r, w: cable.serve_out('up', r, w), args.port + 1),
        await bind(lambda r, w: cable.serve_in('up', r, w), args.port + 2),
        await bind(lambda r, w: cable.serve_out('down', r, w), args.port + 3),
        await bind(lambda r, w: cable.serve_in('down', r, w), args.port + 4),
    ]
    print(f"cable: listening, control on {args.port}, up on +1/+2, down on +3/+4, seed {args.seed}",
          flush=True)
    for d in cable.dirs.values():
        print('cable: ' + d.stats_line(), flush=True)

    stop = asyncio.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        cable.loop.add_signal_handler(sig, stop.set)
    stopper = asyncio.ensure_future(stop.wait())
    # Either a signal or a `quit` verb (loop.stop) ends the run; both paths
    # print the ledger, because a run whose counts vanished never happened.
    try:
        await stopper
    finally:
        cable.dump_stats()
        for s in servers:
            s.close()


def ctl(args):
    with socket.create_connection(('127.0.0.1', args.port), timeout=5) as s:
        s.sendall((' '.join(args.words) + '\n').encode())
        s.shutdown(socket.SHUT_WR)
        out = b''
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            out += chunk
    sys.stdout.write(out.decode('utf-8', 'replace'))
    return 0 if not out.startswith(b'err') else 1


def main():
    ap = argparse.ArgumentParser(description='a network cable with weather in it (see the header)')
    ap.add_argument('--port', type=int, default=7100, help='control port; the four frame ports follow it')
    ap.add_argument('--seed', type=int, default=1, help='the weather is reproducible: same seed, same drops')
    for d in DIRECTIONS:
        for k, (default, parser, meaning) in PARAMS.items():
            ap.add_argument(f'--{d}-{k}', type=parser, default=None, metavar='V',
                            help=f'{d}: {meaning} (default {default})')
    sub = ap.add_subparsers(dest='cmd')
    c = sub.add_parser('ctl', help='send one control command to a running cable and print the reply')
    c.add_argument('words', nargs='+')
    args = ap.parse_args()
    if args.cmd == 'ctl':
        sys.exit(ctl(args))
    try:
        asyncio.run(run(args))
    except RuntimeError as e:
        # loop.stop() from the quit verb surfaces here on some Pythons; the
        # ledger was printed in run()'s finally, so this is a clean exit.
        if 'Event loop stopped' not in str(e):
            raise


if __name__ == '__main__':
    main()
