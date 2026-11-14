#!/usr/bin/env python3
"""One deliberate break at a time to AVDTP's signalling
(kernel/src/driver/system/usb/bt_avdtp.c, and the constants in its header:
the header byte, SEIDs in the top six bits, rejects laid out by signal, the
SBC bits, the walk through their endpoints, the judgment of their
configuration, the state table, the timers, the labels, the one answer and
one command, the fragments and their bounds), against
tools/test_bt_avdtp_host.sh: a mutant is CAUGHT when the suite fails,
MISSED when it passes, which means a test is missing and gets written. A
build failure is neither.

The shape is test_bt_sdp_mutants.py's: each mutant is applied to the tree's
copy of its file, the suite is run, and the file is put back from the bytes
read before the change, checked byte for byte, whatever happens. A mutant of
three fields breaks the source; one of four names the file it breaks. Run it
from a clean tree, and never beside another harness that edits the same
tree; the per-mutant logs are in --logs. Under tools/mutants_parallel.py it
needs `--with kernel`: the runner's copies do not carry the kernel's sources
unless asked.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/bt-avdtp-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

AVDTP = 'kernel/src/driver/system/usb/bt_avdtp.c'
HEADER = 'kernel/include/driver/system/usb/bt_avdtp.h'
CASES = 'bash tools/test_bt_avdtp_host.sh 300'

mutants = [
    # The header byte, the signal, SEIDs, the elements we write.
    ('the-label-is-the-low-nibble', 'av_put(m,(uint8_t)((label&15)<<4 | PKT_SINGLE<<2 | type));',
     'av_put(m,(uint8_t)((label&15) | PKT_SINGLE<<2 | type<<4));'),
    ('the-signal-keeps-its-reserved-bits', '    uint8_t signal=p[0]&0x3f;', '    uint8_t signal=p[0];'),
    ('a-command-seid-is-not-shifted', '    av_put(m,(uint8_t)(a->remote_seid<<2));\n}', '    av_put(m,(uint8_t)(a->remote_seid));\n}'),
    ('the-setconf-acp-seid-is-not-shifted', '    av_put(m,(uint8_t)(a->remote_seid<<2));\n    av_put(m,BT_AVDTP_SEID<<2);',
     '    av_put(m,(uint8_t)(a->remote_seid));\n    av_put(m,BT_AVDTP_SEID<<2);'),
    ('the-setconf-int-seid-is-not-shifted', '    av_put(m,BT_AVDTP_SEID<<2);', '    av_put(m,BT_AVDTP_SEID);'),
    ('the-codec-element-is-7-bytes', 'av_put(m,CAT_CODEC); av_put(m,6);', 'av_put(m,CAT_CODEC); av_put(m,7);'),
    ('the-transport-has-a-length', 'av_put(m,CAT_TRANSPORT); av_put(m,0);', 'av_put(m,CAT_TRANSPORT); av_put(m,1);'),
    ('delay-reporting-is-always-written', '    if(delay) { av_put(m,CAT_DELAY); av_put(m,0); }', '    (void)delay; { av_put(m,CAT_DELAY); av_put(m,0); }'),
    ('the-codec-is-not-audio', '    av_put(m,0x00);              // media type audio, in the top four bits',
     '    av_put(m,0x10);              // media type audio, in the top four bits'),
    ('the-codec-is-not-sbc', '    av_put(m,0x00);              // codec type SBC', '    av_put(m,0x01);              // codec type SBC'),
    ('48k-is-44k', '#define SBC_48000      0x10', '#define SBC_48000      0x20'),
    ('joint-is-bit-1','#define SBC_JOINT      0x01', '#define SBC_JOINT      0x02'),
    ('stereo-is-dual', '#define SBC_STEREO     0x02', '#define SBC_STEREO     0x04'),
    ('16-blocks-is-12', '#define SBC_BLOCKS_16  0x10', '#define SBC_BLOCKS_16  0x20'),
    ('8-subbands-is-4', '#define SBC_SUBBANDS_8 0x04', '#define SBC_SUBBANDS_8 0x08'),
    ('loudness-is-snr', '#define SBC_LOUDNESS   0x01', '#define SBC_LOUDNESS   0x02'),
    ('the-least-bitpool-is-3', '#define SBC_LEAST_BITPOOL 2', '#define SBC_LEAST_BITPOOL 3'),
    ('seids-run-to-63', '#define SEID_LAST 0x3e', '#define SEID_LAST 0x3f'),
    ('in-use-is-bit-0', '(in_use ? 0x02 : 0)', '(in_use ? 0x01 : 0)'),
    ('our-endpoint-is-a-sink', '        av_put(m,0x00);          // media type audio (bits 7-4), endpoint type source (bit 3)',
     '        av_put(m,0x08);          // media type audio (bits 7-4), endpoint type source (bit 3)'),
    # Rejects, by signal.
    ('reconfigure-carries-no-category', '    if(signal==SIG_SET_CONFIGURATION || signal==SIG_RECONFIGURE) av_put(m,param);',
     '    if(signal==SIG_SET_CONFIGURATION) av_put(m,param);'),
    ('close-carries-an-endpoint', '    else if(signal==SIG_START || signal==SIG_SUSPEND) av_put(m,(uint8_t)(param<<2));',
     '    else if(signal==SIG_START || signal==SIG_SUSPEND || signal==SIG_CLOSE) av_put(m,(uint8_t)(param<<2));'),
    ('suspend-carries-no-endpoint', '    else if(signal==SIG_START || signal==SIG_SUSPEND) av_put(m,(uint8_t)(param<<2));',
     '    else if(signal==SIG_START) av_put(m,(uint8_t)(param<<2));'),
    ('the-reject-endpoint-is-not-shifted', 'av_put(m,(uint8_t)(param<<2));', 'av_put(m,param);'),
    # Our command and its label.
    ('the-label-never-moves', '    a->next_label=(uint8_t)((a->next_label+1)&15);\n', ''),
    ('labels-run-to-31', '    a->next_label=(uint8_t)((a->next_label+1)&15);', '    a->next_label=(uint8_t)((a->next_label+1)&31);'),
    ('the-timer-starts-at-zero', '    a->command_deadline=a->now+wait_ms;', '    a->command_deadline=wait_ms;'),
    ('a-command-is-born-sent', '    a->command_live=true; a->command_sent=false;', '    a->command_live=true; a->command_sent=true;'),
    # Leaving a session.
    ('a-dropped-stream-stays-configured', '    a->configured=false;\n}', '    (void)a;\n}'),
    ('connected-keeps-our-command', '    av_drop_stream(a);\n    av_abandon_command(a);\n    a->state=BT_AVDTP_CONNECTED;',
     '    av_drop_stream(a);\n    a->state=BT_AVDTP_CONNECTED;'),
    ('an-abort-is-abandoned', '    if(a->command_signal!=SIG_ABORT) a->command_live=false;', '    a->command_live=false;'),
    ('nothing-is-abandoned', '    if(a->command_signal!=SIG_ABORT) a->command_live=false;', '    if(0) a->command_live=false;'),
    ('abort-to-connected-sends-no-abort', '    av_command_seid(a,SIG_ABORT,BT_AVDTP_ABORT_MS);\n    a->state=BT_AVDTP_CONNECTED;',
     '    a->state=BT_AVDTP_CONNECTED;'),
    ('abort-waits-6s', '    av_command_seid(a,SIG_ABORT,BT_AVDTP_ABORT_MS);\n    a->state=BT_AVDTP_CONNECTED;',
     '    av_command_seid(a,SIG_ABORT,BT_AVDTP_REPLY_MS);\n    a->state=BT_AVDTP_CONNECTED;'),
    ('gone-wrong-never-aborts', '    if(stream) av_command_seid(a,SIG_ABORT,BT_AVDTP_ABORT_MS);\n', '    (void)stream;\n'),
    ('gone-wrong-always-aborts', '    if(stream) av_command_seid(a,SIG_ABORT,BT_AVDTP_ABORT_MS);', '    (void)stream; av_command_seid(a,SIG_ABORT,BT_AVDTP_ABORT_MS);'),
    ('configuring-has-no-stream', 'bool stream=a->state>=BT_AVDTP_CONFIGURING &&', 'bool stream=a->state>=BT_AVDTP_CONFIGURED &&'),
    ('one-wrong-is-failed', '    a->state=a->wrongs>=2 ? BT_AVDTP_FAILED : BT_AVDTP_ENDING;', '    a->state=a->wrongs>=1 ? BT_AVDTP_FAILED : BT_AVDTP_ENDING;'),
    ('three-wrongs-to-fail', '    a->state=a->wrongs>=2 ? BT_AVDTP_FAILED : BT_AVDTP_ENDING;', '    a->state=a->wrongs>=3 ? BT_AVDTP_FAILED : BT_AVDTP_ENDING;'),
    ('gone-wrong-closes-nothing', '    a->close_wanted=true;\n', ''),
    ('gone-wrong-keeps-no-reason', '    a->why=why; a->wrongs++;', '    (void)why; a->wrongs++;'),
    ('a-walk-end-keeps-no-reason', '    a->walk_why=why;\n    a->walk=walk;', '    (void)why;\n    a->walk=walk;'),
    ('a-walk-end-holds-nothing', '    a->walk_why=why;\n    a->walk=walk;', '    a->walk_why=why;\n    (void)walk;'),
    ('a-walk-end-waits-from-zero', '    a->begin_after=a->now+BT_AVDTP_BUSY_MS;', '    a->begin_after=BT_AVDTP_BUSY_MS;'),
    ('a-walk-end-is-failed', '    a->begin_after=a->now+BT_AVDTP_BUSY_MS;\n    a->state=BT_AVDTP_CONNECTED;',
     '    a->begin_after=a->now+BT_AVDTP_BUSY_MS;\n    a->state=BT_AVDTP_FAILED;'),
    # The walk.
    ('sources-are-asked', '{ return e->media==0 && e->sink && e->seid && e->seid<=SEID_LAST; }', '{ return e->media==0 && e->seid && e->seid<=SEID_LAST; }'),
    ('video-is-asked', '{ return e->media==0 && e->sink && e->seid && e->seid<=SEID_LAST; }', '{ return e->sink && e->seid && e->seid<=SEID_LAST; }'),
    ('in-use-is-asked', '        if(!av_audio_sink(e) || e->in_use) continue;', '        if(!av_audio_sink(e)) continue;'),
    ('seid-0-is-asked', '{ return e->media==0 && e->sink && e->seid && e->seid<=SEID_LAST; }', '{ return e->media==0 && e->sink && e->seid<=SEID_LAST; }'),
    ('seid-63-is-asked', '{ return e->media==0 && e->sink && e->seid && e->seid<=SEID_LAST; }', '{ return e->media==0 && e->sink && e->seid; }'),
    ('a-failed-walk-is-for-a-while', '    if(a->walk_why) { av_walk_ends(a,a->walk_why,BT_AVDTP_WALK_NEVER); return; }',
     '    if(a->walk_why) { av_walk_ends(a,a->walk_why,BT_AVDTP_WALK_LATER); return; }'),
    ('a-failed-walk-is-no-sink', '    if(a->walk_why) { av_walk_ends(a,a->walk_why,BT_AVDTP_WALK_NEVER); return; }\n', ''),
    ('held-sinks-are-for-good', 'av_walk_ends(a,"no sink endpoint free",BT_AVDTP_WALK_LATER); return; }', 'av_walk_ends(a,"no sink endpoint free",BT_AVDTP_WALK_NEVER); return; }'),
    ('held-sinks-are-no-sinks', '        if(av_audio_sink(&a->endpoints[i])) { av_walk_ends', '        if(0 && av_audio_sink(&a->endpoints[i])) { av_walk_ends'),
    ('no-sink-is-for-a-while', '    av_walk_ends(a,"no sink endpoint",BT_AVDTP_WALK_NEVER);', '    av_walk_ends(a,"no sink endpoint",BT_AVDTP_WALK_LATER);'),
    ('get-all-of-1-2', 'bool all=a->peer_version>=0x0103 && !a->no_get_all;', 'bool all=a->peer_version>=0x0102 && !a->no_get_all;'),
    ('get-all-of-1-3-only', 'bool all=a->peer_version>=0x0103 && !a->no_get_all;', 'bool all=a->peer_version==0x0103 && !a->no_get_all;'),
    ('get-all-after-its-refusal', 'bool all=a->peer_version>=0x0103 && !a->no_get_all;', 'bool all=a->peer_version>=0x0103;'),
    ('no-sink-hides-the-reason', '    if(a->walk_why) { av_walk_ends(a,a->walk_why,BT_AVDTP_WALK_NEVER); return; }',
     '    if(a->walk_why) { av_walk_ends(a,"no sink endpoint",BT_AVDTP_WALK_NEVER); return; }'),
    ('begin-keeps-an-old-reason', '    a->endpoint_count=0; a->endpoint_at=0; a->no_get_all=false; a->walk_why=0;',
     '    a->endpoint_count=0; a->endpoint_at=0; a->no_get_all=false;'),
    ('begin-keeps-a-refusal', '    a->endpoint_count=0; a->endpoint_at=0; a->no_get_all=false; a->walk_why=0;',
     '    a->endpoint_count=0; a->endpoint_at=0; a->walk_why=0;'),
    ('begin-erases-the-wrong', '    a->endpoint_count=0; a->endpoint_at=0; a->no_get_all=false; a->walk_why=0;',
     '    a->endpoint_count=0; a->endpoint_at=0; a->no_get_all=false; a->walk_why=0; a->why=0;'),
    ('begin-ignores-may-begin', '    if(!bt_avdtp_may_begin(a,now)) return false;\n    a->tried=true;', '    a->tried=true;'),
    ('begin-is-not-a-try', '    if(!bt_avdtp_may_begin(a,now)) return false;\n    a->tried=true;', '    if(!bt_avdtp_may_begin(a,now)) return false;'),
    ('begin-keeps-the-hold', '    a->tried=true;\n    a->walk=BT_AVDTP_WALK_OPEN;', '    a->tried=true;'),
    ('may-begin-anywhere', '    if(a->state!=BT_AVDTP_CONNECTED || a->command_live) return false;\n    switch(a->walk) {',
     '    if(a->command_live) return false;\n    switch(a->walk) {'),
    ('may-begin-over-an-abort', '    if(a->state!=BT_AVDTP_CONNECTED || a->command_live) return false;\n    switch(a->walk) {',
     '    if(a->state!=BT_AVDTP_CONNECTED) return false;\n    switch(a->walk) {'),
    ('may-begin-a-tick-early', '    case BT_AVDTP_WALK_LATER: return now>=a->begin_after;', '    case BT_AVDTP_WALK_LATER: return now+1>=a->begin_after;'),
    ('may-begin-a-tick-late', '    case BT_AVDTP_WALK_LATER: return now>=a->begin_after;', '    case BT_AVDTP_WALK_LATER: return now>a->begin_after;'),
    ('may-begin-later-after-all', '    case BT_AVDTP_WALK_NEVER: return false;', '    case BT_AVDTP_WALK_NEVER: return now>=a->begin_after;'),
    ('may-begin-again-and-again', '    default: return !a->tried;', '    default: return true;'),
    ('a-new-channel-may-not-try', '    a->tried=false;              // a new channel may try once', '    (void)0;              // a new channel may try once'),
    ('the-busy-wait-is-31s', '#define BT_AVDTP_BUSY_MS       30000', '#define BT_AVDTP_BUSY_MS       31000', HEADER),
    # Their capabilities.
    ('caps-an-overrun-is-read', '        if(n-at<2 || p[at+1]>n-at-2) { a->malformed++;', '        if(n-at<2) { a->malformed++;'),
    ('caps-half-a-header-is-read', '        if(n-at<2 || p[at+1]>n-at-2) { a->malformed++;', '        if(p[at+1]>n-at-2) { a->malformed++;'),
    ('caps-a-transport-length-is-fine', '        if(cat==CAT_TRANSPORT && len!=0) {', '        if(0) {'),
    ('caps-a-one-byte-codec-is-read', '            if(len<2) { a->malformed++;', '            if(len<1) { a->malformed++;'),
    ('caps-sbc-of-any-length', '                if(len!=6) { a->malformed++;', '                if(len<6) { a->malformed++;'),
    ('caps-the-last-sbc-is-used', '                if(!sbc) sbc=v+2;', '                sbc=v+2;'),
    ('caps-video-sbc-is-sbc', '            if(v[0]>>4==0 && v[1]==0) {', '            if(v[1]==0) {'),
    ('caps-any-audio-codec-is-sbc', '            if(v[0]>>4==0 && v[1]==0) {', '            if(v[0]>>4==0) {'),
    ('caps-delay-is-never-offered', '        if(cat==CAT_DELAY) delay=true;\n', ''),
    ('caps-the-walk-is-a-byte-short', '        at+=2u+len;\n    }\n    if(!sbc) { *why', '        at+=1u+len;\n    }\n    if(!sbc) { *why'),
    ('caps-no-sbc-is-fine', '    if(!sbc) { *why="no settings in common: no SBC"; return false; }\n', ''),
    ('caps-any-rate', '    if(!(sbc[0]&SBC_48000)) {', '    if(0) {'),
    ('caps-joint-only', '    if(!(sbc[0]&(SBC_JOINT|SBC_STEREO))) {', '    if(!(sbc[0]&SBC_JOINT)) {'),
    ('caps-any-mode', '    if(!(sbc[0]&(SBC_JOINT|SBC_STEREO))) {', '    if(0) {'),
    ('caps-any-blocks', '    if(!(sbc[1]&SBC_BLOCKS_16)) {', '    if(0) {'),
    ('caps-any-subbands', '    if(!(sbc[1]&SBC_SUBBANDS_8)) {', '    if(0) {'),
    ('caps-any-allocation', '    if(!(sbc[1]&SBC_LOUDNESS)) {', '    if(0) {'),
    ('caps-their-least-is-taken-under-2', '    uint8_t lo=sbc[2]>SBC_LEAST_BITPOOL ? sbc[2] : SBC_LEAST_BITPOOL;', '    uint8_t lo=sbc[2];'),
    ('caps-the-bitpool-is-always-51', '    uint8_t chosen=sbc[3]<BT_AVDTP_BITPOOL ? sbc[3] : BT_AVDTP_BITPOOL;', '    uint8_t chosen=BT_AVDTP_BITPOOL;'),
    ('caps-the-bitpool-is-their-most', '    uint8_t chosen=sbc[3]<BT_AVDTP_BITPOOL ? sbc[3] : BT_AVDTP_BITPOOL;', '    uint8_t chosen=sbc[3];'),
    ('caps-any-range-meets-ours', '    if(chosen<lo) { *why="no settings in common: bitpool"; return false; }\n', ''),
    ('caps-joint-is-stereo', '.joint=(sbc[0]&SBC_JOINT)!=0,', '.joint=(sbc[0]&SBC_STEREO)==0,'),
    ('caps-the-range-tops-at-theirs', '    a->range_lo=lo; a->range_hi=chosen;', '    a->range_lo=lo; a->range_hi=sbc[3];'),
    ('caps-delay-reporting-is-dropped', '    a->range_lo=lo; a->range_hi=chosen;\n    a->delay_reporting=delay;', '    a->range_lo=lo; a->range_hi=chosen;\n    (void)delay;'),
    # Their configuration.
    ('judge-an-overrun-is-read', '        if(n-at<2 || p[at+1]>n-at-2) { *code=ERR_BAD_LENGTH; return false; }',
     '        if(n-at<2) { *code=ERR_BAD_LENGTH; return false; }'),
    ('judge-an-overrun-is-unsupported', '        if(n-at<2 || p[at+1]>n-at-2) { *code=ERR_BAD_LENGTH; return false; }',
     '        if(n-at<2 || p[at+1]>n-at-2) { return false; }'),
    ('judge-the-fault-is-category-0', '        *cat=p[at];\n', '        *cat=0;\n'),
    ('judge-two-transports', '            if(len!=0 || transport) {', '            if(len!=0 || (0 && transport)) {'),
    ('judge-a-transport-length-is-unsupported', '{ *code=ERR_BAD_MEDIA_TRANSPORT; return false; }', '{ return false; }'),
    ('judge-two-codecs', '            if(sbc || len!=6 ||', '            if(len!=6 ||'),
    ('judge-sbc-of-any-length', '            if(sbc || len!=6 ||', '            if(sbc || len<2 ||'),
    ('judge-video-sbc', ' || v[0]>>4!=0 || v[1]!=0) return false;', ' || v[1]!=0) return false;'),
    ('judge-any-codec', ' || v[0]>>4!=0 || v[1]!=0) return false;', ' || v[0]>>4!=0) return false;'),
    ('judge-delay-with-bytes', '            if(len!=0 || delay) return false;', '            if(delay) return false;'),
    ('judge-two-delays', '            if(len!=0 || delay) return false;', '            if(len!=0) return false;'),
    ('judge-categories-we-lack-are-taken', '        case 2: case 3: case 4: case 5: case 6:    // reporting, recovery, content protection, header compression, multiplexing\n            return false;',
     '        case 2: case 3: case 4: case 5: case 6:    // reporting, recovery, content protection, header compression, multiplexing\n            break;'),
    ('judge-an-unknown-category-is-unsupported', '            *code=ERR_BAD_SERV_CATEGORY;\n', ''),
    ('judge-no-codec-blames-nothing', '    *cat=CAT_CODEC;\n    if(!sbc) return false;', '    *cat=0;\n    if(!sbc) return false;'),
    ('judge-two-rates', '    if((sbc[0]&0xf0)!=SBC_48000 ||', '    if(!(sbc[0]&SBC_48000) ||'),
    ('judge-two-modes', '(mode!=SBC_JOINT && mode!=SBC_STEREO)) return false;', '!(mode&(SBC_JOINT|SBC_STEREO))) return false;'),
    ('judge-two-blocks', '    if(sbc[1]!=(SBC_BLOCKS_16|SBC_SUBBANDS_8|SBC_LOUDNESS)) return false;', '    if(!(sbc[1]&SBC_BLOCKS_16)) return false;'),
    ('judge-snr-and-loudness', '    if(sbc[1]!=(SBC_BLOCKS_16|SBC_SUBBANDS_8|SBC_LOUDNESS)) return false;',
     '    if((sbc[1]&~0x02)!=(SBC_BLOCKS_16|SBC_SUBBANDS_8|SBC_LOUDNESS)) return false;'),
    ('judge-a-bitpool-under-2', '    if(lo<SBC_LEAST_BITPOOL || chosen<lo) return false;', '    if(chosen<lo) return false;'),
    ('judge-a-range-that-misses-ours', '    if(lo<SBC_LEAST_BITPOOL || chosen<lo) return false;', '    if(lo<SBC_LEAST_BITPOOL) return false;'),
    ('judge-the-bitpool-is-their-most', '.bitpool=chosen };\n    a->range_lo=lo; a->range_hi=hi;', '.bitpool=hi };\n    a->range_lo=lo; a->range_hi=hi;'),
    ('judge-the-range-is-cut-to-ours', '    a->range_lo=lo; a->range_hi=hi;', '    a->range_lo=lo; a->range_hi=chosen;'),
    ('judge-always-joint', '.joint=mode==SBC_JOINT,', '.joint=true,'),
    ('judge-delay-reporting-is-dropped', '    a->range_lo=lo; a->range_hi=hi;\n    a->delay_reporting=delay;', '    a->range_lo=lo; a->range_hi=hi;'),
    # Their Set Configuration.
    ('setconf-a-short-one-is-read', '    if(n<2) { av_reject(m,label,SIG_SET_CONFIGURATION,0,ERR_BAD_LENGTH); return; }',
     '    if(n<1) { av_reject(m,label,SIG_SET_CONFIGURATION,0,ERR_BAD_LENGTH); return; }'),
    ('setconf-any-acp-seid', '    if(acp!=BT_AVDTP_SEID || !intr || intr>SEID_LAST)', '    if((0 && acp) || !intr || intr>SEID_LAST)'),
    ('setconf-int-seid-0', '    if(acp!=BT_AVDTP_SEID || !intr || intr>SEID_LAST)', '    if(acp!=BT_AVDTP_SEID || intr>SEID_LAST)'),
    ('setconf-int-seid-63', '    if(acp!=BT_AVDTP_SEID || !intr || intr>SEID_LAST)', '    if(acp!=BT_AVDTP_SEID || !intr)'),
    ('setconf-a-queued-one-is-out', 'bool ours_out=a->state==BT_AVDTP_CONFIGURING && a->command_live && a->command_sent;',
     'bool ours_out=a->state==BT_AVDTP_CONFIGURING && a->command_live;'),
    ('setconf-a-waiting-one-is-out', 'bool ours_out=a->state==BT_AVDTP_CONFIGURING && a->command_live && a->command_sent;',
     'bool ours_out=a->state==BT_AVDTP_CONFIGURING && a->command_sent;'),
    ('setconf-refused-in-connected', 'bool open=a->state==BT_AVDTP_CONNECTED || a->state==BT_AVDTP_DISCOVERING',
     'bool open=a->state==BT_AVDTP_DISCOVERING'),
    ('setconf-refused-while-discovering', '|| a->state==BT_AVDTP_DISCOVERING || a->state==BT_AVDTP_ASKING', '|| a->state==BT_AVDTP_ASKING'),
    ('setconf-refused-while-asking', '|| a->state==BT_AVDTP_DISCOVERING || a->state==BT_AVDTP_ASKING', '|| a->state==BT_AVDTP_DISCOVERING'),
    ('setconf-refused-while-configuring', '           || (a->state==BT_AVDTP_CONFIGURING && !ours_out);', '           || (0 && !ours_out);'),
    ('setconf-a-rejection-is-not-noted', '        if(a->state==BT_AVDTP_CONFIGURING) a->rejected_theirs=true;\n        uint8_t code', '        uint8_t code'),
    ('setconf-in-use-is-bad-state', 'uint8_t code=a->state>=BT_AVDTP_CONFIGURING && a->state<=BT_AVDTP_CLOSING ? ERR_SEP_IN_USE : ERR_BAD_STATE;',
     'uint8_t code=ERR_BAD_STATE;'),
    ('setconf-bad-state-is-in-use', 'uint8_t code=a->state>=BT_AVDTP_CONFIGURING && a->state<=BT_AVDTP_CLOSING ? ERR_SEP_IN_USE : ERR_BAD_STATE;',
     'uint8_t code=ERR_SEP_IN_USE;'),
    ('setconf-ours-goes-on', '    // after this answer).\n    av_abandon_command(a);\n', '    // after this answer).\n'),
    ('setconf-retires-our-abort', '    // after this answer).\n    av_abandon_command(a);\n', '    // after this answer).\n    a->command_live=false;\n'),
    ('setconf-is-no-session', '    a->configured=true;\n    a->edges.sessions++;\n    a->state=BT_AVDTP_CONFIGURED;', '    a->configured=true;\n    a->state=BT_AVDTP_CONFIGURED;'),
    ('setconf-their-seid-is-not-kept', '    a->remote_seid=intr;\n', ''),
    ('setconf-not-configured', '    a->remote_seid=intr;\n    a->configured=true;', '    a->remote_seid=intr;'),
    ('setconf-configured-has-no-timer', '    a->state_deadline=a->now+BT_AVDTP_HOLD_MS;\n    av_start(m,label,MSG_ACCEPT,SIG_SET_CONFIGURATION);',
     '    a->state_deadline=BT_AVDTP_HOLD_MS;\n    av_start(m,label,MSG_ACCEPT,SIG_SET_CONFIGURATION);'),
    # Their other commands.
    ('discover-never-in-use', '(in_use ? 0x02 : 0)', '(in_use ? 0 : 0)'),
    ('in-use-from-configured', 'bool in_use=a->state>=BT_AVDTP_CONFIGURING &&', 'bool in_use=a->state>=BT_AVDTP_CONFIGURED &&'),
    ('getcap-an-empty-one-is-a-bad-seid', '        if(!n) { av_reject(m,label,signal,0,ERR_BAD_LENGTH); return; }\n        if(seid!=BT_AVDTP_SEID) { av_reject(m,label,signal,0,ERR_BAD_ACP_SEID); return; }\n        av_start(m,label,MSG_ACCEPT,signal);\n        av_put_ours',
     '        if(seid!=BT_AVDTP_SEID) { av_reject(m,label,signal,0,ERR_BAD_ACP_SEID); return; }\n        av_start(m,label,MSG_ACCEPT,signal);\n        av_put_ours'),
    ('getcap-any-seid', '        if(seid!=BT_AVDTP_SEID) { av_reject(m,label,signal,0,ERR_BAD_ACP_SEID); return; }\n        av_start(m,label,MSG_ACCEPT,signal);\n        av_put_ours',
     '        av_start(m,label,MSG_ACCEPT,signal);\n        av_put_ours'),
    ('get-all-without-delay', 'av_put_ours(m,signal==SIG_GET_ALL_CAPABILITIES);', 'av_put_ours(m,false);'),
    ('getconf-when-nothing-is', '        if(!a->configured) { av_reject(m,label,signal,0,ERR_BAD_STATE); return; }\n', ''),
    ('reconfigure-is-bad-state', '        av_reject(m,label,signal,0,ERR_NOT_SUPPORTED_COMMAND);', '        av_reject(m,label,signal,0,ERR_BAD_STATE);'),
    ('open-anywhere', '        if(a->state!=BT_AVDTP_CONFIGURED) { av_reject(m,label,signal,0,ERR_BAD_STATE); return; }\n', ''),
    ('their-open-is-ours-to-carry', '        a->media_ours=false;\n', '        a->media_ours=true;\n'),
    ('their-open-has-no-timer', '        a->state_deadline=a->now+BT_AVDTP_HOLD_MS;\n        av_start(m,label,MSG_ACCEPT,signal);\n        return;\n    case SIG_START:',
     '        a->state_deadline=a->now;\n        av_start(m,label,MSG_ACCEPT,signal);\n        return;\n    case SIG_START:'),
    ('start-refused-while-starting', '        if(a->state!=BT_AVDTP_OPEN && a->state!=BT_AVDTP_STARTING)', '        if(a->state!=BT_AVDTP_OPEN)'),
    ('their-start-is-not-counted', '        a->edges.their_starts++;\n', ''),
    ('their-start-blames-no-endpoint','        if(seid!=BT_AVDTP_SEID) { av_reject(m,label,signal,seid,ERR_BAD_ACP_SEID); return; }\n        if(a->state!=BT_AVDTP_OPEN',
     '        if(seid!=BT_AVDTP_SEID) { av_reject(m,label,signal,0,ERR_BAD_ACP_SEID); return; }\n        if(a->state!=BT_AVDTP_OPEN'),
    ('suspend-refused-while-suspending', '        if(a->state!=BT_AVDTP_STREAMING && a->state!=BT_AVDTP_SUSPENDING)', '        if(a->state!=BT_AVDTP_STREAMING)'),
    ('their-suspend-is-not-counted', '        a->edges.their_suspends++;\n', ''),
    ('their-suspend-streams-on', '        a->state=BT_AVDTP_OPEN;\n        a->edges.their_suspends++;', '        a->edges.their_suspends++;'),
    ('their-close-anywhere', '        if(!av_streamed(a->state)) { av_reject(m,label,signal,0,ERR_BAD_STATE); return; }\n        av_to_connected(a);',
     '        av_to_connected(a);'),
    ('their-close-ends-nothing', '        av_to_connected(a);\n        av_start(m,label,MSG_ACCEPT,signal);\n        return;\n    case SIG_ABORT:',
     '        av_start(m,label,MSG_ACCEPT,signal);\n        return;\n    case SIG_ABORT:'),
    ('abort-any-endpoint', '        if(seid!=BT_AVDTP_SEID) { a->malformed++; return; }\n', ''),
    ('abort-leaves-the-stream', '        if(av_streamed(a->state)) av_to_connected(a);\n', ''),
    ('abort-ends-anything', '        if(av_streamed(a->state)) av_to_connected(a);', '        av_to_connected(a);'),
    ('delay-a-short-one-is-read', '        if(n<3) { av_reject(m,label,signal,0,ERR_BAD_LENGTH); return; }', '        if(n<2) { av_reject(m,label,signal,0,ERR_BAD_LENGTH); return; }'),
    ('delay-any-endpoint', '        if(seid!=BT_AVDTP_SEID && !(a->configured && seid==a->remote_seid)) {',
     '        if(0 && seid!=BT_AVDTP_SEID && !(a->configured && seid==a->remote_seid)) {'),
    ('delay-ours-only', '        if(seid!=BT_AVDTP_SEID && !(a->configured && seid==a->remote_seid)) {',
     '        if(seid!=BT_AVDTP_SEID) {'),
    ('delay-theirs-unconfigured', '        if(seid!=BT_AVDTP_SEID && !(a->configured && seid==a->remote_seid)) {',
     '        if(seid!=BT_AVDTP_SEID && seid!=a->remote_seid) {'),
    ('delay-anywhere', '        if(!av_streamed(a->state)) { av_reject(m,label,signal,0,ERR_BAD_STATE); return; }\n        a->delay=', '        a->delay='),
    ('delay-is-little-endian', '        a->delay=(uint16_t)(p[1]<<8 | p[2]);', '        a->delay=(uint16_t)(p[2]<<8 | p[1]);'),
    ('delay-is-never-known', '        a->delay_known=true;\n', ''),
    ('an-unknown-signal-is-accepted', '        av_start(m,label,MSG_GENERAL_REJECT,signal);\n        return;\n    }\n}', '        av_start(m,label,MSG_ACCEPT,signal);\n        return;\n    }\n}'),
    ('the-streamed-states-stop-short', 'static bool av_streamed(bt_avdtp_state_t s) { return s>=BT_AVDTP_CONFIGURED && s<=BT_AVDTP_CLOSING; }',
     'static bool av_streamed(bt_avdtp_state_t s) { return s>=BT_AVDTP_CONFIGURED && s<BT_AVDTP_CLOSING; }'),
    ('the-streamed-states-start-early', 'static bool av_streamed(bt_avdtp_state_t s) { return s>=BT_AVDTP_CONFIGURED && s<=BT_AVDTP_CLOSING; }',
     'static bool av_streamed(bt_avdtp_state_t s) { return s>=BT_AVDTP_CONFIGURING && s<=BT_AVDTP_CLOSING; }'),
    # One answer and one command.
    ('a-second-abort-is-rejected', '    if(signal==SIG_ABORT || signal==SIG_SECURITY_CONTROL || !signal || signal>SIG_DELAY_REPORT) {',
     '    if(signal==SIG_SECURITY_CONTROL || !signal || signal>SIG_DELAY_REPORT) {'),
    ('a-second-unknown-is-rejected', '    if(signal==SIG_ABORT || signal==SIG_SECURITY_CONTROL || !signal || signal>SIG_DELAY_REPORT) {',
     '    if(signal==SIG_ABORT) {'),
    ('a-second-setconf-is-not-noted', '    if(signal==SIG_SET_CONFIGURATION && a->state==BT_AVDTP_CONFIGURING) a->rejected_theirs=true;\n', ''),
    ('a-second-start-blames-no-endpoint', '    bool names_seid=(signal==SIG_START || signal==SIG_SUSPEND) && n;', '    bool names_seid=false;'),
    ('a-second-is-answered-in-full', 'else if(!a->answer[1].bytes) av_one_too_many(a,&a->answer[1],label,signal,p,n);',
     'else if(!a->answer[1].bytes) { (void)av_one_too_many; av_answer(a,&a->answer[1],label,signal,p,n); }'),
    ('a-third-overwrites-the-second', '    }\n    else a->overrun++;', '    }\n    else av_one_too_many(a,&a->answer[1],label,signal,p,n);'),
    ('a-third-is-not-counted', '    }\n    else a->overrun++;', '    }\n    else {}'),
    ('a-third-abort-is-dropped', '    else if(signal==SIG_ABORT) {', '    else if(0 && signal==SIG_ABORT) {'),
    ('a-third-abort-displaces-uncounted', '        if(m.bytes) { a->answer[1]=m; a->overrun++; }', '        if(m.bytes) { a->answer[1]=m; }'),
    ('a-third-abort-displaces-always', '        if(m.bytes) { a->answer[1]=m; a->overrun++; }', '        { a->answer[1]=m; a->overrun++; }'),
    ('a-third-abort-takes-the-oldest', '        if(m.bytes) { a->answer[1]=m; a->overrun++; }', '        if(m.bytes) { a->answer[0]=m; a->overrun++; }'),
    # Answers to our commands.
    ('a-reject-code-is-read-first', 'size_t at=signal==SIG_SET_CONFIGURATION || signal==SIG_RECONFIGURE || signal==SIG_START || signal==SIG_SUSPEND ? 1 : 0;',
     'size_t at=signal==SIG_SET_CONFIGURATION ? 1 : 0;'),
    ('a-short-reject-is-read', '    if(n<at+1) { a->malformed++; return 0; }', '    if(n<at) { a->malformed++; return 0; }'),
    ('a-general-reject-carries-a-code', '    if(type==MSG_GENERAL_REJECT || !n) return 0;', '    if(0 && type==MSG_GENERAL_REJECT) return 0;'),
    ('a-signal-only-reject-is-malformed', '    if(type==MSG_GENERAL_REJECT || !n) return 0;', '    if(type==MSG_GENERAL_REJECT) return 0;'),
    ('any-label-answers', '|| label!=a->command_label\n', '|| (0 && label!=a->command_label)\n'),
    ('any-signal-answers', '       || (!any_signal && signal!=a->command_signal)) { a->strays++; return; }',
     '       || (0 && !any_signal && signal!=a->command_signal)) { a->strays++; return; }'),
    ('a-queued-command-is-answered', '    if(!a->command_live || !a->command_sent || label!=a->command_label', '    if(!a->command_live || label!=a->command_label'),
    ('a-command-is-answered-twice', '    signal=a->command_signal;\n    a->command_live=false;', '    signal=a->command_signal;'),
    ('strays-are-not-counted', '{ a->strays++; return; }', '{ return; }'),
    ('rejects-are-not-recorded', '        a->last_reject_signal=signal; a->last_reject_code=code;\n', ''),
    ('a-discover-reject-is-taken', '        if(!accepted) { av_gone_wrong(a,"Discover rejected"); return; }\n', ''),
    ('an-odd-byte-is-not-counted', '        if(n&1) a->malformed++;\n', ''),
    ('nine-endpoints', 'a->endpoint_count<BT_AVDTP_ENDPOINTS;i+=2)', 'a->endpoint_count<=BT_AVDTP_ENDPOINTS;i+=2)'),
    ('an-odd-byte-is-read', 'for(size_t i=0;i+1<n &&', 'for(size_t i=0;i<n &&'),
    ('in-use-is-read-from-bit-0', 'e->in_use=(p[i]&0x02)!=0;', 'e->in_use=(p[i]&0x01)!=0;'),
    ('sink-is-read-from-bit-2', 'e->sink=(p[i+1]&0x08)!=0;', 'e->sink=(p[i+1]&0x04)!=0;'),
    ('media-is-read-from-the-low-nibble', 'e->media=p[i+1]>>4;', 'e->media=p[i+1]&15;'),
    ('a-seid-is-read-shifted-once', 'e->seid=p[i]>>2;', 'e->seid=p[i]>>1;'),
    ('a-get-all-refusal-moves-on', '                a->no_get_all=true;\n                av_ask(a,a->endpoint_at);', '                a->no_get_all=true;\n                av_ask(a,a->endpoint_at+1u);'),
    ('a-get-all-refusal-is-forgotten', '                a->no_get_all=true;\n', ''),
    ('a-get-caps-refusal-asks-again', '            a->walk_why="no settings in common: capabilities refused";\n            av_ask(a,a->endpoint_at+1u);',
     '            a->walk_why="no settings in common: capabilities refused";\n            av_ask(a,a->endpoint_at);'),
    ('a-get-caps-refusal-is-no-reason', '            a->walk_why="no settings in common: capabilities refused";\n', ''),
    ('an-unsuited-sink-is-asked-again', 'if(!av_suits(a,p,n,&why)) { a->walk_why=why; av_ask(a,a->endpoint_at+1u); return; }',
     'if(!av_suits(a,p,n,&why)) { a->walk_why=why; av_ask(a,a->endpoint_at); return; }'),
    ('an-unsuited-sink-gives-no-reason', 'if(!av_suits(a,p,n,&why)) { a->walk_why=why; av_ask(a,a->endpoint_at+1u); return; }',
     'if(!av_suits(a,p,n,&why)) { (void)why; av_ask(a,a->endpoint_at+1u); return; }'),
    ('the-retry-carries-over', '        a->retried=false; a->rejected_theirs=false; a->retry_waiting=false;\n        a->state=BT_AVDTP_CONFIGURING;',
     '        a->state=BT_AVDTP_CONFIGURING;'),
    ('setconf-accepted-not-configured', '            a->configured=true;\n            a->edges.sessions++;\n            a->state=BT_AVDTP_OPENING;',
     '            a->edges.sessions++;\n            a->state=BT_AVDTP_OPENING;'),
    ('setconf-accepted-is-no-session', '            a->configured=true;\n            a->edges.sessions++;\n            a->state=BT_AVDTP_OPENING;',
     '            a->configured=true;\n            a->state=BT_AVDTP_OPENING;'),
    ('setconf-accepted-opens-nothing', '            av_command_seid(a,SIG_OPEN,BT_AVDTP_REPLY_MS);\n            return;\n        }', '            return;\n        }'),
    ('retry-without-a-collision', '&& a->rejected_theirs && !a->retried)', '&& !a->retried)'),
    ('retry-forever', '&& a->rejected_theirs && !a->retried)', '&& a->rejected_theirs)'),
    ('retry-on-any-code', 'if((code==ERR_SEP_IN_USE || code==ERR_BAD_STATE) && a->rejected_theirs', 'if(a->rejected_theirs'),
    ('retry-on-in-use-only', 'if((code==ERR_SEP_IN_USE || code==ERR_BAD_STATE) && a->rejected_theirs', 'if((code==ERR_SEP_IN_USE) && a->rejected_theirs'),
    ('retry-at-once', '            a->state_deadline=a->now+BT_AVDTP_RETRY_MS;', '            a->state_deadline=a->now;'),
    ('an-open-reject-is-taken', '        if(!accepted) { av_gone_wrong(a,"Open rejected"); return; }\n', ''),
    ('our-open-leaves-media-to-them', '        a->media_ours=true;\n', '        a->media_ours=false;\n'),
    ('our-open-has-no-timer', '        a->state_deadline=a->now+BT_AVDTP_HOLD_MS;\n        return;\n    case SIG_START:',
     '        a->state_deadline=a->now;\n        return;\n    case SIG_START:'),
    ('a-start-answer-acts-anywhere', '        if(a->state!=BT_AVDTP_STARTING) return;\n', ''),
    ('a-start-accept-is-ignored', '        if(accepted) { a->state=BT_AVDTP_STREAMING; return; }', '        if(accepted) { return; }'),
    ('a-start-reject-is-not-counted', '        a->edges.start_rejects++;\n', ''),
    ('a-start-reject-keeps-starting', '        a->edges.start_rejects++;\n        a->state=BT_AVDTP_OPEN;', '        a->edges.start_rejects++;'),
    ('a-start-reject-counts-after-theirs', '        if(a->state!=BT_AVDTP_STARTING) return;\n        if(accepted)',
     '        if(a->state!=BT_AVDTP_STARTING && accepted) return;\n        if(a->state!=BT_AVDTP_STARTING) { a->edges.start_rejects++; return; }\n        if(accepted)'),
    ('a-suspend-answer-acts-anywhere', '        if(a->state!=BT_AVDTP_SUSPENDING) return;\n', ''),
    ('a-suspend-accept-is-ignored', '        if(accepted) { a->state=BT_AVDTP_OPEN; return; }', '        if(accepted) { return; }'),
    ('a-suspend-reject-is-not-counted', '        a->edges.suspend_rejects++;\n', ''),
    ('a-suspend-reject-keeps-suspending', '        a->edges.suspend_rejects++;\n        a->state=BT_AVDTP_STREAMING;', '        a->edges.suspend_rejects++;'),
    ('a-suspend-reject-counts-after-theirs', '        if(a->state!=BT_AVDTP_SUSPENDING) return;\n        if(accepted)',
     '        if(a->state!=BT_AVDTP_SUSPENDING && accepted) return;\n        if(a->state!=BT_AVDTP_SUSPENDING) { a->edges.suspend_rejects++; return; }\n        if(accepted)'),
    ('a-close-answer-is-ignored', '        if(a->state==BT_AVDTP_CLOSING) av_to_connected(a);\n        return;\n    default:', '        return;\n    default:'),
    ('the-1-0-general-reject-is-malformed', '        if(type==MSG_REJECT) av_response(a,label,0,MSG_GENERAL_REJECT,p,0,true);', '        if(0) av_response(a,label,0,MSG_GENERAL_REJECT,p,0,true);'),
    ('responses-are-commands', '    if(type==MSG_COMMAND) av_command_in(a,label,signal,p+1,n-1);', '    if(type!=MSG_ACCEPT) av_command_in(a,label,signal,p+1,n-1);'),
    # The host's calls.
    ('sig-opens-over-failed', '    if(a->state!=BT_AVDTP_IDLE) return;\n    a->state=BT_AVDTP_CONNECTED;', '    a->state=BT_AVDTP_CONNECTED;'),
    ('the-version-is-ignored', '    a->peer_version=peer_version;\n', '    (void)peer_version;\n'),
    ('sig-close-keeps-answers', '    av_wipe(a->answer,sizeof(a->answer));\n', ''),
    ('sig-close-keeps-the-reassembly', '    a->rx_active=false;\n    a->close_wanted=false;', '    a->close_wanted=false;'),
    ('sig-close-keeps-close-wanted', '    a->rx_active=false;\n    a->close_wanted=false;', '    a->rx_active=false;'),
    ('sig-close-keeps-our-command', '    av_drop_stream(a);\n    a->command_live=false;\n    av_wipe', '    av_drop_stream(a);\n    av_wipe'),
    ('failed-recovers-on-sig-close', '    if(was==BT_AVDTP_FAILED) return;\n', ''),
    ('sig-closing-is-not-wrong', '    if(was==BT_AVDTP_ENDING || !session) return;', '    if(1) return;'),
    ('ending-counts-twice', '    if(was==BT_AVDTP_ENDING || !session) return;', '    if(!session) return;'),
    ('any-sig-close-is-wrong', '    if(was==BT_AVDTP_ENDING || !session) return;', '    if(was==BT_AVDTP_ENDING) return;'),
    ('a-stream-is-no-session', 'bool session=(was>=BT_AVDTP_CONFIGURING && was<=BT_AVDTP_CLOSING) || (a->command_live && a->command_sent);',
     'bool session=(a->command_live && a->command_sent);'),
    ('configuring-is-no-session', 'bool session=(was>=BT_AVDTP_CONFIGURING && was<=BT_AVDTP_CLOSING) ||', 'bool session=(was>=BT_AVDTP_CONFIGURED && was<=BT_AVDTP_CLOSING) ||'),
    ('a-command-out-is-no-session', 'bool session=(was>=BT_AVDTP_CONFIGURING && was<=BT_AVDTP_CLOSING) || (a->command_live && a->command_sent);',
     'bool session=(was>=BT_AVDTP_CONFIGURING && was<=BT_AVDTP_CLOSING);'),
    ('a-queued-command-is-a-session', '|| (a->command_live && a->command_sent);', '|| a->command_live;'),
    ('sig-close-fails-at-three', '    if(a->wrongs>=2) a->state=BT_AVDTP_FAILED;', '    if(a->wrongs>=3) a->state=BT_AVDTP_FAILED;'),
    ('sig-close-keeps-no-reason', '    a->why="the signalling channel closed";\n', ''),
    ('media-opens-anywhere', '    if(a->state!=BT_AVDTP_AWAITING_MEDIA) return;\n    a->state=BT_AVDTP_OPEN;', '    a->state=BT_AVDTP_OPEN;'),
    ('a-refusal-is-not-counted', '    if(refused) a->media_refused++;\n', ''),
    ('media-closing-while-awaited-is-fine', '    if(a->state==BT_AVDTP_AWAITING_MEDIA)\n        av_gone_wrong', '    if(0)\n        av_gone_wrong'),
    ('media-closing-while-closing-aborts', '    else if(a->state>=BT_AVDTP_OPEN && a->state<=BT_AVDTP_SUSPENDING)', '    else if(a->state>=BT_AVDTP_OPEN && a->state<=BT_AVDTP_CLOSING)'),
    ('media-closing-while-configured-aborts', '    else if(a->state>=BT_AVDTP_OPEN && a->state<=BT_AVDTP_SUSPENDING)', '    else if(a->state>=BT_AVDTP_CONFIGURED && a->state<=BT_AVDTP_SUSPENDING)'),
    # Receiving, and fragments.
    ('ending-takes-messages', '    if(a->state==BT_AVDTP_IDLE || a->state==BT_AVDTP_ENDING) { a->ignored++; return; }', '    if(a->state==BT_AVDTP_IDLE) { a->ignored++; return; }'),
    ('idle-takes-messages', '    if(a->state==BT_AVDTP_IDLE || a->state==BT_AVDTP_ENDING) { a->ignored++; return; }', '    if(a->state==BT_AVDTP_ENDING) { a->ignored++; return; }'),
    ('an-empty-payload-is-not-counted', '    if(!n) { a->malformed++; return; }\n    uint8_t type', '    if(!n) return;\n    uint8_t type'),
    ('an-unfinished-message-is-not-counted', '        if(a->rx_active) { a->fragments_dropped++; a->rx_active=false; }', '        if(a->rx_active) { a->rx_active=false; }'),
    ('a-start-of-one-packet', '        if(n<3 || p[1]<2 || p[1]>BT_AVDTP_FRAGMENTS', '        if(n<3 || p[1]>BT_AVDTP_FRAGMENTS'),
    ('a-start-of-five-packets', '|| p[1]>BT_AVDTP_FRAGMENTS ||', '|| p[1]>BT_AVDTP_FRAGMENTS+1 ||'),
    ('a-start-too-long-is-copied', '|| n-2>sizeof(a->rx)) { a->fragments_dropped++; return; }', '|| n-2>sizeof(a->rx)+1) { a->fragments_dropped++; return; }'),
    ('a-start-without-its-signal', '        if(n<3 || p[1]<2', '        if(n<2 || p[1]<2'),
    ('the-start-is-counted-twice', '        a->rx_header=p[0]; a->rx_left=(uint8_t)(p[1]-1);', '        a->rx_header=p[0]; a->rx_left=p[1];'),
    ('an-orphan-is-not-counted', '    if(!a->rx_active) { a->fragments_dropped++; return; }', '    if(!a->rx_active) { return; }'),
    ('any-label-continues', '    if((p[0]&0xf3)!=(a->rx_header&0xf3) ||', '    if((p[0]&0x03)!=(a->rx_header&0x03) ||'),
    ('any-message-type-continues', '    if((p[0]&0xf3)!=(a->rx_header&0xf3) ||', '    if((p[0]&0xf0)!=(a->rx_header&0xf0) ||'),
    ('an-end-may-come-early', '(end ? a->rx_left!=1 : a->rx_left<=1)', '(end ? 0 : a->rx_left<=1)'),
    ('a-continue-may-come-last', '(end ? a->rx_left!=1 : a->rx_left<=1)', '(end ? a->rx_left!=1 : 0)'),
    ('a-continue-may-overflow', '       || n-1>sizeof(a->rx)-a->rx_have) {', '       || n-1>sizeof(a->rx)-a->rx_have+1) {'),
    ('an-abandoned-message-stays-open', '        a->fragments_dropped++; a->rx_active=false;\n        return;', '        a->fragments_dropped++;\n        return;'),
    ('the-count-never-falls', '    a->rx_left--;\n', ''),
    ('an-end-is-not-delivered', '    if(end) { a->rx_active=false; av_message(a,a->rx_header,a->rx,a->rx_have); }', '    if(end) { a->rx_active=false; }'),
    ('an-end-leaves-it-open', '    if(end) { a->rx_active=false; av_message(a,a->rx_header,a->rx,a->rx_have); }', '    if(end) { av_message(a,a->rx_header,a->rx,a->rx_have); }'),
    # The timers.
    ('an-abort-timeout-goes-wrong', '        if(signal==SIG_ABORT) {}', '        if(0) {}'),
    ('a-close-timeout-goes-wrong', '        else if(signal==SIG_CLOSE) { if(a->state==BT_AVDTP_CLOSING) av_to_connected(a); }', '        else if(0) {}'),
    ('a-close-timeout-stays-closing', '        else if(signal==SIG_CLOSE) { if(a->state==BT_AVDTP_CLOSING) av_to_connected(a); }', '        else if(signal==SIG_CLOSE) {}'),
    ('a-command-times-out-early', '    if(a->command_live && now>=a->command_deadline) {', '    if(a->command_live && now+1>=a->command_deadline) {'),
    ('a-command-times-out-late', '    if(a->command_live && now>=a->command_deadline) {', '    if(a->command_live && now>a->command_deadline) {'),
    ('a-timeout-keeps-the-command', '        uint8_t signal=a->command_signal;\n        a->command_live=false;', '        uint8_t signal=a->command_signal;'),
    ('the-retry-goes-early', '    if(a->state==BT_AVDTP_CONFIGURING && a->retry_waiting && now>=a->state_deadline) {',
     '    if(a->state==BT_AVDTP_CONFIGURING && a->retry_waiting && now+1>=a->state_deadline) {'),
    ('the-retry-is-not-spent', '        a->retry_waiting=false; a->retried=true;', '        a->retry_waiting=false;'),
    ('the-retry-repeats', '        a->retry_waiting=false; a->retried=true;', '        a->retried=true;'),
    ('configured-waits-forever', '    } else if(a->state==BT_AVDTP_CONFIGURED && now>=a->state_deadline) {', '    } else if(0) {'),
    ('configured-waits-late', '    } else if(a->state==BT_AVDTP_CONFIGURED && now>=a->state_deadline) {', '    } else if(a->state==BT_AVDTP_CONFIGURED && now>a->state_deadline) {'),
    ('configured-timeout-goes-wrong', '        av_abort_to_connected(a);\n    } else if(a->state==BT_AVDTP_AWAITING_MEDIA', '        av_gone_wrong(a,"x");\n    } else if(a->state==BT_AVDTP_AWAITING_MEDIA'),
    ('awaiting-waits-forever', '    } else if(a->state==BT_AVDTP_AWAITING_MEDIA && now>=a->state_deadline) {', '    } else if(0) {'),
    ('awaiting-waits-late', '    } else if(a->state==BT_AVDTP_AWAITING_MEDIA && now>=a->state_deadline) {', '    } else if(a->state==BT_AVDTP_AWAITING_MEDIA && now>a->state_deadline) {'),
    # What we hand out.
    ('the-mtu-is-ignored', '        bool fits=n<=mtu;', '        bool fits=true || n<=mtu;'),
    ('the-mtu-must-be-passed', '        bool fits=n<=mtu;', '        bool fits=n<mtu;'),
    ('a-small-buffer-is-overrun', '        if(fits && cap<n) return 0;', '        if(0 && cap<n) return 0;'),
    ('an-unsendable-message-is-not-counted', '        else a->unsendable++;', '        else {}'),
    ('our-command-goes-first', '        if(a->answer[0].bytes) m=&a->answer[0];\n        else if(a->command_live && !a->command_sent) { m=&a->command; command=true; }',
     '        if(a->command_live && !a->command_sent) { m=&a->command; command=true; }\n        else if(a->answer[0].bytes) m=&a->answer[0];'),
    ('the-second-answer-is-lost', '        else { a->answer[0]=a->answer[1]; a->answer[1].bytes=0; }', '        else { a->answer[0].bytes=0; }'),
    ('the-second-answer-repeats', '        else { a->answer[0]=a->answer[1]; a->answer[1].bytes=0; }', '        else { a->answer[0]=a->answer[1]; }'),
    ('an-unsendable-command-is-tried-again', '        if(command) a->command_sent=true;', '        if(command && fits) a->command_sent=true;'),
    # What the pump asks for.
    ('start-anywhere', '    if(a->state!=BT_AVDTP_OPEN || a->command_live) return false;', '    if(a->command_live) return false;'),
    ('suspend-anywhere', '    if(a->state!=BT_AVDTP_STREAMING || a->command_live) return false;', '    if(a->command_live) return false;'),
    ('close-from-open-only', '    if((a->state!=BT_AVDTP_OPEN && a->state!=BT_AVDTP_STREAMING) || a->command_live) return false;',
     '    if(a->state!=BT_AVDTP_OPEN || a->command_live) return false;'),
    ('close-anywhere-we-like', '    if((a->state!=BT_AVDTP_OPEN && a->state!=BT_AVDTP_STREAMING) || a->command_live) return false;',
     '    if(a->command_live) return false;'),
    ('suspend-waits-6s', '    av_command_seid(a,SIG_SUSPEND,BT_AVDTP_SUSPEND_MS);', '    av_command_seid(a,SIG_SUSPEND,BT_AVDTP_REPLY_MS);'),
    ('close-waits-10s', '    av_command_seid(a,SIG_CLOSE,BT_AVDTP_REPLY_MS);', '    av_command_seid(a,SIG_CLOSE,BT_AVDTP_SUSPEND_MS);'),
    # What the host reads.
    ('media-is-ours-to-open-either-way', 'return a->state==BT_AVDTP_AWAITING_MEDIA && a->media_ours; }', 'return a->state==BT_AVDTP_AWAITING_MEDIA; }'),
    ('media-is-dropped-while-closing', 'return a->state>=BT_AVDTP_AWAITING_MEDIA && a->state<=BT_AVDTP_CLOSING; }', 'return a->state>=BT_AVDTP_AWAITING_MEDIA && a->state<BT_AVDTP_CLOSING; }'),
    ('closed-before-the-abort-leaves', '&& !(a->command_live && !a->command_sent); }', '; }'),
    ('closed-before-the-answers-leave', '{ return a->close_wanted && !a->answer[0].bytes &&', '{ return a->close_wanted &&'),
    ('settings-before-agreement', 'return a->configured ? &a->settings : 0; }', 'return &a->settings; }'),
    ('a-delay-before-a-report', '    if(!a->delay_known) return false;\n', ''),
    ('the-status-overruns', '    return n<cap ? n : cap-1;', '    return n;'),
    ('the-status-delay-has-no-tenths', '(unsigned)(a->delay/10),(unsigned)(a->delay%10)', '(unsigned)(a->delay/10),0u'),
    # The header's figures.
    ('our-endpoint-is-2', '#define BT_AVDTP_SEID          1 ', '#define BT_AVDTP_SEID          2 ', HEADER),
    ('the-bitpool-is-53', '#define BT_AVDTP_BITPOOL       51', '#define BT_AVDTP_BITPOOL       53', HEADER),
    ('a-message-of-257', '#define BT_AVDTP_MESSAGE       256', '#define BT_AVDTP_MESSAGE       257', HEADER),
    ('five-fragments', '#define BT_AVDTP_FRAGMENTS     4', '#define BT_AVDTP_FRAGMENTS     5', HEADER),
    ('three-fragments', '#define BT_AVDTP_FRAGMENTS     4', '#define BT_AVDTP_FRAGMENTS     3', HEADER),
    ('seven-endpoints', '#define BT_AVDTP_ENDPOINTS     8', '#define BT_AVDTP_ENDPOINTS     7', HEADER),
    ('the-reply-waits-5s', '#define BT_AVDTP_REPLY_MS      6000', '#define BT_AVDTP_REPLY_MS      5000', HEADER),
    ('suspend-waits-9s', '#define BT_AVDTP_SUSPEND_MS    10000', '#define BT_AVDTP_SUSPEND_MS    9000', HEADER),
    ('abort-waits-3s', '#define BT_AVDTP_ABORT_MS      2000', '#define BT_AVDTP_ABORT_MS      3000', HEADER),
    ('the-hold-is-11s', '#define BT_AVDTP_HOLD_MS       10000', '#define BT_AVDTP_HOLD_MS       11000', HEADER),
    ('the-retry-waits-2s', '#define BT_AVDTP_RETRY_MS      1000', '#define BT_AVDTP_RETRY_MS      2000', HEADER),
    ('the-reassembly-holds-one-more', '    uint8_t rx[BT_AVDTP_MESSAGE - 1];', '    uint8_t rx[BT_AVDTP_MESSAGE];', HEADER),
]

caught, missed, unbuilt = [], [], []
for mutant in mutants:
    name, old, new = mutant[:3]
    target = mutant[3] if len(mutant) > 3 else AVDTP
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
                result = subprocess.run(['bash', '-c', CASES], cwd=root, stdout=out, stderr=out, timeout=120)
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

print(f'\nbt_avdtp mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
