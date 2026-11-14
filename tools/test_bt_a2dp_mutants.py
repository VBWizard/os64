#!/usr/bin/env python3
"""One deliberate break at a time to the A2DP pump
(kernel/src/driver/system/usb/bt_a2dp.c, and the constants in its header:
the RTP header and the payload header, frames a packet from the MTU, the
sample clock, its slips and its anchors, made and sent, the pulls and what
each may send, quiet and the Suspend, holding a Start back, the session,
attachment and the delay, the failed sink and its reasons), against
tools/test_bt_a2dp_host.sh: a mutant is CAUGHT when the suite fails, MISSED
when it passes, which means a test is missing and gets written. A build
failure is neither.

The shape is test_bt_avdtp_mutants.py's: each mutant is applied to the
tree's copy of its file, the suite is run, and the file is put back from
the bytes read before the change, checked byte for byte, whatever happens.
A mutant of three fields breaks the source; one of four names the file it
breaks. Run it from a clean tree, and never beside another harness that
edits the same tree; the per-mutant logs are in --logs. Under
tools/mutants_parallel.py it needs `--with kernel`: the runner's copies do
not carry the kernel's sources unless asked.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/bt-a2dp-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

PUMP = 'kernel/src/driver/system/usb/bt_a2dp.c'
HEADER = 'kernel/include/driver/system/usb/bt_a2dp.h'
CASES = 'bash tools/test_bt_a2dp_host.sh'

mutants = [
    # The packet.
    ('rtp-version-byte-is-0x90', '    out[0]=0x80;', '    out[0]=0x90;'),
    ('the-marker-is-set', '    out[1]=BT_A2DP_PAYLOAD_TYPE;', '    out[1]=BT_A2DP_PAYLOAD_TYPE|0x80;'),
    ('the-sequence-is-little-endian', 'out[2]=(uint8_t)(p->sequence>>8); out[3]=(uint8_t)p->sequence;',
     'out[3]=(uint8_t)(p->sequence>>8); out[2]=(uint8_t)p->sequence;'),
    ('the-sequence-never-moves', '    p->sequence++;\n', ''),
    ('the-timestamp-counts-sbc-frames', '    p->timestamp+=samples;', '    p->timestamp+=n;'),
    ('a-word-is-little-endian', '{ at[0]=(uint8_t)(v>>24); at[1]=(uint8_t)(v>>16);', '{ at[1]=(uint8_t)(v>>24); at[0]=(uint8_t)(v>>16);'),
    ('the-frame-count-is-the-high-nibble', '    out[12]=(uint8_t)(n&0x0f);', '    out[12]=(uint8_t)(n<<4);'),
    ('the-payload-type-is-97', '#define BT_A2DP_PAYLOAD_TYPE   96', '#define BT_A2DP_PAYLOAD_TYPE   97', HEADER),
    ('the-ssrc-is-2', '#define BT_A2DP_SSRC           1u', '#define BT_A2DP_SSRC           2u', HEADER),
    ('the-header-is-14-bytes', '#define BT_A2DP_HEADER_BYTES   13', '#define BT_A2DP_HEADER_BYTES   14', HEADER),
    ('at-most-14-frames', '#define BT_A2DP_FRAMES_MAX     15', '#define BT_A2DP_FRAMES_MAX     14', HEADER),
    # Frames a packet.
    ('the-mtu-forgets-the-header', '    uint32_t n=(mtu-BT_A2DP_HEADER_BYTES)/fb;', '    uint32_t n=mtu/fb;'),
    ('frames-are-not-capped', '    if(n>BT_A2DP_FRAMES_MAX) n=BT_A2DP_FRAMES_MAX;\n', ''),
    ('the-buffer-is-not-a-limit', '    if(n>room) n=room;', '    if(n>room+1) n=room;'),
    ('a-small-buffer-is-not-refused', '    if(cap<BT_A2DP_HEADER_BYTES+fb) return 0;\n', ''),
    ('a-small-mtu-is-not-a-failure', '{ a2_fail(p,"the media channel\'s MTU is smaller than a frame",ops,ctx); return 0; }', '{ return 0; }'),
    # The clock.
    ('due-forgets-the-anchor', '    uint64_t due=p->anchor+p->frames_since_anchor*1000000u/p->settings.rate;',
     '    uint64_t due=p->frames_since_anchor*1000000u/p->settings.rate;'),
    ('always-due', '    if(now<due) return 0;\n', ''),
    ('a-waiting-packet-does-not-hold-the-next', '    if(!p->streaming || p->pending) return 0;', '    if(!p->streaming) return 0;'),
    ('sent-frames-do-not-count', '    if(p->pending_anchor==p->anchors) p->frames_since_anchor+=p->pending_frames;\n', ''),
    ('sent-frames-count-under-any-anchor', '    if(p->pending_anchor==p->anchors) p->frames_since_anchor+=p->pending_frames;',
     '    p->frames_since_anchor+=p->pending_frames;'),
    ('sent-without-a-packet-counts', '    if(!p->pending) return;\n    p->pending=false;', '    p->pending=false;'),
    ('a-slip-is-not-anchored', '{ p->slips++; p->anchor=now; p->frames_since_anchor=0; p->anchors++; }', '{ p->slips++; }'),
    ('a-slip-is-not-counted', '{ p->slips++; p->anchor=now;', '{ p->anchor=now;'),
    ('a-slip-is-200ms', '#define BT_A2DP_SLIP_US        100000u', '#define BT_A2DP_SLIP_US        200000u', HEADER),
    ('a-slip-is-80ms', '#define BT_A2DP_SLIP_US        100000u', '#define BT_A2DP_SLIP_US        80000u', HEADER),
    ('a-stop-is-not-anchored', '    if(p->stopped) { p->stopped=false; p->anchor=now; p->frames_since_anchor=0; p->anchors++; }',
     '    if(p->stopped) { p->stopped=false; }'),
    ('streaming-is-not-anchored', '            p->anchor=now; p->frames_since_anchor=0; p->anchors++;\n            p->stopped=false;',
     '            p->frames_since_anchor=0; p->anchors++;\n            p->stopped=false;'),
    ('their-start-is-not-anchored', '        if(!p->streaming || their_start) {', '        if(!p->streaming || (their_start && 0)) {'),
    # The pulls.
    ('a-paused-pull-sends', '    if(got.paused) { a2_quiet(p,now); return 0; }', '    if(got.paused) { a2_quiet(p,now); }'),
    ('a-pull-not-ours-sends', '    if(!got.yours) { p->attached=false; p->displaced=true; p->streaming=false; return 0; }', ''),
    ('a-pull-not-ours-takes-the-seam-back', 'p->attached=false; p->displaced=true; p->streaming=false; return 0; }',
     'p->attached=false; p->streaming=false; return 0; }'),
    ('silence-goes-after-a-refused-suspend', '        if(p->suspend_refused) { a2_quiet(p,now); return 0; }\n', ''),
    ('silence-is-not-quiet', '        if(!p->quiet_since) p->quiet_since=now;\n    } else {', '    } else {'),
    ('sound-does-not-end-the-quiet', '        p->quiet_since=0;\n        p->suspend_refused=false;\n    }\n    // Sending',
     '        p->suspend_refused=false;\n    }\n    // Sending'),
    ('sound-keeps-the-refusal', '        p->quiet_since=0;\n        p->suspend_refused=false;\n    }\n    // Sending',
     '        p->quiet_since=0;\n    }\n    // Sending'),
    ('dry-packets-are-not-counted', '    if(dry) p->dry_packets++;\n', ''),
    ('a-flush-is-not-seen', '{ p->generation=got.generation; p->generations++; }', '{ p->generation=got.generation; }'),
    # The encode.
    ('the-encode-is-not-timed', '    if(t1>t0 && t1-t0>BT_A2DP_SLOW_US) {', '    if(t1<t0 && t1-t0>BT_A2DP_SLOW_US) {'),
    ('a-quick-encode-keeps-the-run', '    } else p->slow_run=0;', '    }'),
    ('slow-is-7ms', '#define BT_A2DP_SLOW_US        5000u', '#define BT_A2DP_SLOW_US        7000u', HEADER),
    ('nine-slow-fail', '#define BT_A2DP_SLOW_RUN       8', '#define BT_A2DP_SLOW_RUN       9', HEADER),
    ('a-refused-frame-is-not-a-failure', '            a2_fail(p,"the encoder refused a frame",ops,ctx);\n', ''),
    # Quiet and the Suspend.
    ('quiet-is-2s', '#define BT_A2DP_QUIET_US       3000000u', '#define BT_A2DP_QUIET_US       2000000u', HEADER),
    ('quiet-is-4s', '#define BT_A2DP_QUIET_US       3000000u', '#define BT_A2DP_QUIET_US       4000000u', HEADER),
    ('nothing-sendable-is-not-quiet', '        if(!p->streaming && !p->quiet_since) p->quiet_since=now;\n', ''),
    ('a-refused-suspend-is-asked-again', '>=BT_A2DP_QUIET_US && !p->suspend_refused)', '>=BT_A2DP_QUIET_US)'),
    ('a-refusal-is-not-noted', '{ p->suspend_rejects_seen+=suspend_rejects; p->suspend_refused=true; }',
     '{ p->suspend_rejects_seen+=suspend_rejects; }'),
    ('leaving-streaming-keeps-the-quiet', '    } else {\n        p->quiet_since=0;\n        p->suspend_refused=false;\n    }\n}',
     '    } else {\n        p->suspend_refused=false;\n    }\n}'),
    ('their-start-keeps-the-quiet', '    if(their_start) p->quiet_since=0;\n', ''),
    # Holding a Start back.
    ('their-suspend-holds-nothing', '        p->hold_until=now+BT_A2DP_THEIRS_US;\n', ''),
    ('their-suspend-holds-1s', '#define BT_A2DP_THEIRS_US      2000000u', '#define BT_A2DP_THEIRS_US      1000000u', HEADER),
    ('the-hold-does-not-double', '            uint64_t until=now+((uint64_t)BT_A2DP_HOLD_US<<(p->start_rejects-1));',
     '            uint64_t until=now+(uint64_t)BT_A2DP_HOLD_US;'),
    ('the-first-hold-is-2s', '#define BT_A2DP_HOLD_US        1000000u', '#define BT_A2DP_HOLD_US        2000000u', HEADER),
    ('two-holds', '#define BT_A2DP_HOLDS          3', '#define BT_A2DP_HOLDS          2', HEADER),
    ('never-gives-up', '        if(p->start_rejects>BT_A2DP_HOLDS) p->gave_up=true;\n        else {', '        {'),
    ('a-start-ignores-the-hold', ' && now>=p->hold_until && ops->has_sound(ctx)', ' && ops->has_sound(ctx)'),
    ('a-start-without-sound', ' && now>=p->hold_until && ops->has_sound(ctx)', ' && now>=p->hold_until'),
    ('a-start-while-failed', '    bool healthy=p->attached && !p->failed;', '    bool healthy=p->attached;'),
    ('no-walk-is-begun', '    if(st==BT_AVDTP_CONNECTED && bt_avdtp_may_begin(a,ms)) bt_avdtp_begin(a,ms);\n', ''),
    # The session.
    ('the-edges-are-not-read-afresh', '        a2_wipe(&p->seen,sizeof(p->seen));\n', '        (void)0;\n'),
    ('a-new-session-is-not-begun', '    else if(new_session) a2_session(p,s,delay,ops,ctx);', '    else if(new_session && 0) a2_session(p,s,delay,ops,ctx);'),
    ('a-session-keeps-the-sequence', '    p->pending=false; p->sequence=0; p->timestamp=0; p->slow_run=0;',
     '    p->pending=false; p->timestamp=0; p->slow_run=0;'),
    ('a-session-keeps-the-timestamp', '    p->pending=false; p->sequence=0; p->timestamp=0; p->slow_run=0;',
     '    p->pending=false; p->sequence=0; p->slow_run=0;'),
    ('a-session-keeps-its-holds', '    p->hold_until=0; p->start_rejects=0; p->gave_up=false; p->displaced=false;',
     '    p->gave_up=false; p->displaced=false;'),
    ('a-session-keeps-gave-up', '    p->hold_until=0; p->start_rejects=0; p->gave_up=false; p->displaced=false;',
     '    p->hold_until=0; p->start_rejects=0; p->displaced=false;'),
    ('settings-of-no-frame-are-begun', '    p->encoder_ok=p->frame_bytes && ops->encoder_begin(ctx,&p->settings);',
     '    p->encoder_ok=ops->encoder_begin(ctx,&p->settings);'),
    ('a-failed-begin-is-not-attached', '    if(!p->attached) a2_attach(p,delay_us,ops,ctx);\n    a2_fail(', '    if(0) a2_attach(p,delay_us,ops,ctx);\n    a2_fail('),
    ('a-good-begin-does-not-clear-the-seam', '            if(p->attached) a2_detach(p,ops,ctx);\n', ''),
    ('a-good-begin-keeps-the-failure', '            p->failed=false; p->why=0;\n', '            p->why=0;\n'),
    # Attachment and the delay.
    ('a-failed-sink-detaches', '    bool want=p->failed || (a2_sink_states(st)', '    bool want=(a2_sink_states(st)'),
    ('giving-up-stays-attached', ' && p->encoder_ok && !p->gave_up && !p->displaced);', ' && p->encoder_ok && !p->displaced);'),
    ('displaced-takes-the-seam-back', ' && p->encoder_ok && !p->gave_up && !p->displaced);', ' && p->encoder_ok && !p->gave_up);'),
    ('displaced-forever', '    if(!a2_sink_states(st)) p->displaced=false;\n', ''),
    ('attached-from-starting-only', 'return s>=BT_AVDTP_OPEN && s<=BT_AVDTP_SUSPENDING; }', 'return s>=BT_AVDTP_STARTING && s<=BT_AVDTP_SUSPENDING; }'),
    ('the-attach-tells-no-delay', '    p->attached=true;\n    ops->delay(ctx,delay_us);\n', '    p->attached=true;\n'),
    ('a-new-report-is-not-told', '    if(p->attached && delay!=p->delay_told_us) { ops->delay(ctx,delay); p->delay_told_us=delay; }\n', ''),
    ('a-report-is-in-ms', '    p->delay_us=p->delay_reported ? d*100u : BT_A2DP_DEFAULT_DELAY_US;',
     '    p->delay_us=p->delay_reported ? d*1000u : BT_A2DP_DEFAULT_DELAY_US;'),
    ('the-default-is-100ms', '#define BT_A2DP_DEFAULT_DELAY_US 150000u', '#define BT_A2DP_DEFAULT_DELAY_US 100000u', HEADER),
    ('a-failed-sink-is-not-closed', '    if(p->failed && (st==BT_AVDTP_OPEN || st==BT_AVDTP_STREAMING) && bt_avdtp_close(a,ms)) {',
     '    if(0 && bt_avdtp_close(a,ms)) {'),
    ('the-link-drop-does-not-detach', '    if(p->attached) ops->detach(ctx);\n    a2_wipe(p,sizeof(*p));', '    if(0) ops->detach(ctx);\n    a2_wipe(p,sizeof(*p));'),
]

caught, missed, unbuilt = [], [], []
for mutant in mutants:
    name, old, new = mutant[:3]
    target = mutant[3] if len(mutant) > 3 else PUMP
    if args.only and args.only not in name:
        continue
    path = root / target
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {target}', file=sys.stderr)
        unbuilt.append(name)
        continue
    log = args.logs / f'{name}.log'
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            try:
                result = subprocess.run(['bash', '-c', CASES], cwd=root, stdout=out, stderr=out, timeout=300)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
    finally:
        path.write_bytes(original)
        if path.read_bytes() != original:
            print(f'{name}: {target} WAS NOT RESTORED', file=sys.stderr)
            sys.exit(2)
    body = log.read_text(errors='replace')
    # A compiler's error, not a sanitizer's "runtime error:", which is a catch.
    built = re.search(r'\.(c|h):\d+:\d+: error:', body) is None and 'undefined reference' not in body
    if not built:
        unbuilt.append(name)
        verdict = 'did not build'
    elif code != 0:
        caught.append(name)
        verdict = 'caught'
    else:
        missed.append(name)
        verdict = 'MISSED'
    print(f'{name}: {verdict}', flush=True)

print(f'\nbt_a2dp mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
