#!/usr/bin/env python3
"""One deliberate break at a time to AVCTP and AVRCP
(kernel/src/driver/system/usb/bt_avrcp.c, and the figures in its header:
AVCTP's header byte and profile, the AV/C types and the panel, the vendor
header, the PDUs, events and statuses, the target's answers and rejects,
the presses and their ring, the controller's states, labels, timers and
Sets, take-then-commit, the fragments and their bounds), against
tools/test_bt_avrcp_host.sh: a mutant is CAUGHT when the suite fails,
MISSED when it passes, which means a test is missing and gets written. A
build failure is neither.

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
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/bt-avrcp-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

AVRCP = 'kernel/src/driver/system/usb/bt_avrcp.c'
HEADER = 'kernel/include/driver/system/usb/bt_avrcp.h'
CASES = 'bash tools/test_bt_avrcp_host.sh 300'

mutants = [
    # AVCTP's header, the AV/C header, the vendor header.
    ('labels-run-to-7', 'ar_put(m,(uint8_t)((label&15)<<4 | PKT_SINGLE<<2 | (response ? HDR_RESPONSE : 0)));',
     'ar_put(m,(uint8_t)((label&7)<<4 | PKT_SINGLE<<2 | (response ? HDR_RESPONSE : 0)));'),
    ('a-response-is-a-command', 'ar_put(m,(uint8_t)((label&15)<<4 | PKT_SINGLE<<2 | (response ? HDR_RESPONSE : 0)));',
     'ar_put(m,(uint8_t)((label&15)<<4 | PKT_SINGLE<<2 | (response ? 0 : 0)));'),
    ('the-profile-is-the-target-s', '#define BT_AVRCP_PROFILE        0x110E', '#define BT_AVRCP_PROFILE        0x110C', HEADER),
    ('sixteen-bits-are-written-little-endian', 'static void ar_put16(bt_avrcp_msg_t *m, uint16_t v) { ar_put(m,(uint8_t)(v>>8)); ar_put(m,(uint8_t)v); }',
     'static void ar_put16(bt_avrcp_msg_t *m, uint16_t v) { ar_put(m,(uint8_t)v); ar_put(m,(uint8_t)(v>>8)); }'),
    ('sixteen-bits-are-read-little-endian', 'static uint16_t ar_u16(const uint8_t *p) { return (uint16_t)(p[0]<<8 | p[1]); }',
     'static uint16_t ar_u16(const uint8_t *p) { return (uint16_t)(p[1]<<8 | p[0]); }'),
    ('the-company-is-wrong', 'static void ar_company(bt_avrcp_msg_t *m) { ar_put(m,0x00); ar_put(m,0x19); ar_put(m,0x58); }',
     'static void ar_company(bt_avrcp_msg_t *m) { ar_put(m,0x00); ar_put(m,0x19); ar_put(m,0x59); }'),
    ('the-vendor-packet-is-a-start', '    ar_put(m,pdu); ar_put(m,PKT_SINGLE); ar_put16(m,params);', '    ar_put(m,pdu); ar_put(m,PKT_START); ar_put16(m,params);'),
    ('the-panel-is-0x49', '#define SUBUNIT_PANEL      0x48', '#define SUBUNIT_PANEL      0x49'),
    ('stable-is-accepted', '#define CT_STABLE          0xc', '#define CT_STABLE          0x9'),
    ('interim-is-changed', '#define CT_INTERIM         0xf', '#define CT_INTERIM         0xd'),
    ('not-implemented-is-rejected', '#define CT_NOT_IMPLEMENTED 0x8', '#define CT_NOT_IMPLEMENTED 0xa'),
    ('notify-is-status', '#define CT_NOTIFY          0x3', '#define CT_NOTIFY          0x1'),
    ('control-is-2', '#define CT_CONTROL         0x0', '#define CT_CONTROL         0x2'),
    ('get-capabilities-is-0x11', '#define PDU_GET_CAPABILITIES       0x10', '#define PDU_GET_CAPABILITIES       0x11'),
    ('register-is-0x32', '#define PDU_REGISTER_NOTIFICATION  0x31', '#define PDU_REGISTER_NOTIFICATION  0x32'),
    ('set-volume-is-0x51', '#define PDU_SET_ABSOLUTE_VOLUME    0x50', '#define PDU_SET_ABSOLUTE_VOLUME    0x51'),
    ('get-play-status-is-0x32', '#define PDU_GET_PLAY_STATUS        0x30', '#define PDU_GET_PLAY_STATUS        0x32'),
    ('get-elements-is-0x21', '#define PDU_GET_ELEMENT_ATTRIBUTES 0x20', '#define PDU_GET_ELEMENT_ATTRIBUTES 0x21'),
    ('the-events-capability-is-4', '#define CAP_EVENTS         0x03', '#define CAP_EVENTS         0x04'),
    ('the-volume-is-event-0x0c', '#define EVENT_VOLUME       0x0d', '#define EVENT_VOLUME       0x0c'),
    ('the-playback-is-event-3', '#define EVENT_PLAYBACK     0x01', '#define EVENT_PLAYBACK     0x03'),
    ('the-track-is-event-3', '#define EVENT_TRACK        0x02', '#define EVENT_TRACK        0x03'),
    ('invalid-parameter-is-0x02', '#define STATUS_INVALID_PARAMETER 0x01', '#define STATUS_INVALID_PARAMETER 0x02'),
    ('invalid-command-is-0x03', '#define STATUS_INVALID_COMMAND   0x00', '#define STATUS_INVALID_COMMAND   0x03'),
    ('pass-through-is-0x7d', '#define OP_PASS_THROUGH    0x7c', '#define OP_PASS_THROUGH    0x7d'),
    ('the-unit-opcode-is-0x32', '#define OP_UNIT_INFO       0x30', '#define OP_UNIT_INFO       0x32'),
    ('the-subunit-opcode-is-0x32','#define OP_SUBUNIT_INFO    0x31', '#define OP_SUBUNIT_INFO    0x32'),
    # Pass through and the presses.
    ('released-is-bit-6', '#define PASS_RELEASED      0x80', '#define PASS_RELEASED      0x40'),
    ('the-operation-keeps-its-release-bit', '    uint8_t op=f[3]&0x7f;', '    uint8_t op=f[3];'),
    ('pass-through-to-any-subunit', '    if((f[0]&0x0f)!=CT_CONTROL || f[1]!=SUBUNIT_PANEL || n<5) {', '    if((f[0]&0x0f)!=CT_CONTROL || n<5) {'),
    ('pass-through-of-any-type', '    if((f[0]&0x0f)!=CT_CONTROL || f[1]!=SUBUNIT_PANEL || n<5) {', '    if(f[1]!=SUBUNIT_PANEL || n<5) {'),
    ('pass-through-without-its-length', '    if((f[0]&0x0f)!=CT_CONTROL || f[1]!=SUBUNIT_PANEL || n<5) {', '    if((f[0]&0x0f)!=CT_CONTROL || f[1]!=SUBUNIT_PANEL || n<4) {'),
    ('volume-down-is-not-taken', 'BT_AVRCP_OP_VOLUME_UP,BT_AVRCP_OP_VOLUME_DOWN,BT_AVRCP_OP_PLAY', 'BT_AVRCP_OP_VOLUME_UP,0x43,BT_AVRCP_OP_PLAY'),
    ('mute-is-taken-for-backward', 'BT_AVRCP_OP_PAUSE,BT_AVRCP_OP_FORWARD,BT_AVRCP_OP_BACKWARD};', 'BT_AVRCP_OP_PAUSE,BT_AVRCP_OP_FORWARD,0x43};'),
    ('backward-is-0x4d', '#define BT_AVRCP_OP_BACKWARD    0x4c', '#define BT_AVRCP_OP_BACKWARD    0x4d', HEADER),
    ('a-repeat-is-a-new-press', '    if(a->held==op) { a->held_until=a->now+BT_AVRCP_HOLD_MS; return; }\n', ''),
    ('a-repeat-keeps-the-old-deadline', '    if(a->held==op) { a->held_until=a->now+BT_AVRCP_HOLD_MS; return; }', '    if(a->held==op) { return; }'),
    ('the-hold-is-3s', '#define BT_AVRCP_HOLD_MS        2000', '#define BT_AVRCP_HOLD_MS        3000', HEADER),
    ('the-release-has-no-room', '    a->held_reported=a->ring_count+2u<=BT_AVRCP_PRESSES && ar_ring_push(a,op,true);',
     '    a->held_reported=a->ring_count+1u<=BT_AVRCP_PRESSES && ar_ring_push(a,op,true);'),
    ('a-lost-press-is-not-counted', '    if(!a->held_reported) a->presses_lost++;\n', ''),
    ('a-lost-press-is-released', '    if(a->held_reported) ar_ring_push(a,a->held,false);', '    ar_ring_push(a,a->held,false);'),
    ('a-press-is-not-counted', '    a->presses[which]++;\n', '    (void)which;\n'),
    ('a-release-of-another-ends-the-hold', '    if(a->held!=op) { a->unmatched_releases++; return; }', '    if(!a->held && op) { a->unmatched_releases++; return; }'),
    ('auto-release-is-late', '    if(a->held && now>=a->held_until)', '    if(a->held && now>a->held_until)'),
    ('auto-release-is-not-counted', '{ a->auto_released++; ar_let_go(a); }', '{ ar_let_go(a); }'),
    ('the-ring-forgets-its-head', '    a->ring[(a->ring_head+a->ring_count)%BT_AVRCP_PRESSES]=', '    a->ring[a->ring_count%BT_AVRCP_PRESSES]='),
    ('a-read-does-not-move-the-head', '    a->ring_head=(uint8_t)((a->ring_head+1)%BT_AVRCP_PRESSES);\n', ''),
    ('reset-forgets-the-presses', '    a->ring_head=head; a->ring_count=count;\n', '    (void)head; (void)count;\n'),
    ('reset-does-not-release', '    ar_let_go(a);\n    bt_avrcp_press_t ring[BT_AVRCP_PRESSES];', '    bt_avrcp_press_t ring[BT_AVRCP_PRESSES];'),
    ('closing-does-not-release', '    ar_let_go(a);\n    ar_wipe(a->answer,sizeof(a->answer));', '    ar_wipe(a->answer,sizeof(a->answer));'),
    ('the-presses-are-15', '#define BT_AVRCP_PRESSES        16', '#define BT_AVRCP_PRESSES        15', HEADER),
    # The target's answers.
    ('unit-info-is-not-the-panel', '        if(f[2]==OP_UNIT_INFO) { ar_put(&m,0x07); ar_put(&m,SUBUNIT_PANEL); ar_company(&m); }',
     '        if(f[2]==OP_UNIT_INFO) { ar_put(&m,0x08); ar_put(&m,SUBUNIT_PANEL); ar_company(&m); }'),
    ('subunit-info-forgets-the-page', '        else { ar_put(&m,n>3 ? f[3] : 0x07);', '        else { ar_put(&m,0x07);'),
    ('subunit-info-pads-with-zeros', 'ar_put(&m,SUBUNIT_PANEL); ar_put(&m,0xff); ar_put(&m,0xff); ar_put(&m,0xff); }',
     'ar_put(&m,SUBUNIT_PANEL); ar_put(&m,0x00); ar_put(&m,0x00); ar_put(&m,0x00); }'),
    ('info-of-any-type', '        if((f[0]&0x0f)!=CT_STATUS) { ar_echo(&m,label,CT_REJECTED,f,n); a->rejected++; break; }',
     '        if(0) { ar_echo(&m,label,CT_REJECTED,f,n); a->rejected++; break; }'),
    ('an-unknown-opcode-is-rejected', '    default:\n        ar_echo(&m,label,CT_NOT_IMPLEMENTED,f,n);', '    default:\n        ar_echo(&m,label,CT_REJECTED,f,n);'),
    ('the-echo-is-one-short', '    size_t k=n-3<BT_AVRCP_ECHO ? n-3 : BT_AVRCP_ECHO;', '    size_t k=n-3<BT_AVRCP_ECHO ? n-3 : BT_AVRCP_ECHO-1;'),
    ('the-echo-keeps-their-type', '    ar_begin(m,label,true,ctype,f[1],f[2]);', '    ar_begin(m,label,true,(uint8_t)(f[0]|(ctype&0)),f[1],f[2]);'),
    ('an-echo-is-a-command', '    ar_begin(m,label,true,ctype,f[1],f[2]);', '    ar_begin(m,label,false,ctype,f[1],f[2]);'),
    ('any-company-is-the-sig', '    if(on<3 || o[0]!=0x00 || o[1]!=0x19 || o[2]!=0x58) { ar_echo(', '    if(on<3) { ar_echo('),
    ('a-short-header-is-read', '    if(on<7 || (o[4]&3)!=PKT_SINGLE) {', '    if(on>4 && (o[4]&3)!=PKT_SINGLE) {'),
    ('avrcp-fragments-are-taken', '    if(on<7 || (o[4]&3)!=PKT_SINGLE) {', '    if(on<7) {'),
    ('the-length-is-not-checked', '    if(pn>on-7) { ar_reject(', '    if(0) { ar_reject('),
    ('the-length-must-be-the-bytes', '    if(pn>on-7) { ar_reject(', '    if(pn!=on-7) { ar_reject('),
    ('get-capabilities-of-any-type', '        if(ctype!=CT_STATUS) { ar_reject(a,m,label,subunit,pdu,STATUS_INVALID_COMMAND); return; }\n        if(pn<1',
     '        if(pn<1'),
    ('get-capabilities-of-any-length', '        if(pn<1 || (p[0]!=CAP_COMPANY && p[0]!=CAP_EVENTS))', '        if((p[0]!=CAP_COMPANY && p[0]!=CAP_EVENTS))'),
    ('get-capabilities-must-be-exact', '        if(pn<1 || (p[0]!=CAP_COMPANY && p[0]!=CAP_EVENTS))', '        if(pn!=1 || (p[0]!=CAP_COMPANY && p[0]!=CAP_EVENTS))'),
    ('the-company-count-is-2', '            ar_put(m,CAP_COMPANY); ar_put(m,1); ar_company(m);', '            ar_put(m,CAP_COMPANY); ar_put(m,2); ar_company(m);'),
    ('track-is-not-offered', '            ar_put(m,CAP_EVENTS); ar_put(m,2); ar_put(m,EVENT_PLAYBACK); ar_put(m,EVENT_TRACK);',
     '            ar_put(m,CAP_EVENTS); ar_put(m,2); ar_put(m,EVENT_PLAYBACK); ar_put(m,EVENT_PLAYBACK);'),
    ('register-of-any-type', '        if(ctype!=CT_NOTIFY) { ar_reject(a,m,label,subunit,pdu,STATUS_INVALID_COMMAND); return; }\n', ''),
    ('register-of-any-length', '        if(pn<5 || (p[0]!=EVENT_PLAYBACK && p[0]!=EVENT_TRACK))', '        if(pn<1 || (p[0]!=EVENT_PLAYBACK && p[0]!=EVENT_TRACK))'),
    ('register-must-be-exact', '        if(pn<5 || (p[0]!=EVENT_PLAYBACK && p[0]!=EVENT_TRACK))', '        if(pn!=5 || (p[0]!=EVENT_PLAYBACK && p[0]!=EVENT_TRACK))'),
    ('volume-is-offered', '        if(pn<5 || (p[0]!=EVENT_PLAYBACK && p[0]!=EVENT_TRACK))',
     '        if(pn<5 || (p[0]!=EVENT_PLAYBACK && p[0]!=EVENT_TRACK && p[0]!=EVENT_VOLUME))'),
    ('the-changed-slot-is-not-kept', '            if(!a->play_registered && a->answers+2u>BT_AVRCP_ANSWERS) { m->bytes=0; a->overrun++; return; }\n', ''),
    ('the-kept-slot-is-theirs', '    unsigned keep=a->play_registered ? 1 : 0;', '    unsigned keep=0;'),
    ('a-registration-is-not-remembered', '            a->play_registered=true; a->play_label=label;', '            a->play_label=label;'),
    ('the-changed-is-under-the-panel', '    ar_begin(m,a->play_label,true,CT_CHANGED,a->play_subunit,OP_VENDOR);', '    ar_begin(m,a->play_label,true,CT_CHANGED,SUBUNIT_PANEL,OP_VENDOR);'),
    ('the-interim-says-playing', '            ar_put(m,EVENT_PLAYBACK); ar_put(m,a->playback);', '            ar_put(m,EVENT_PLAYBACK); ar_put(m,BT_AVRCP_PLAYING);'),
    ('a-track-plays-while-stopped', '            uint8_t track=a->playback==BT_AVRCP_PLAYING ? 0x00 : 0xff;', '            uint8_t track=a->playback==BT_AVRCP_PLAYING ? 0x00 : 0x00;'),
    ('no-track-while-playing', '            uint8_t track=a->playback==BT_AVRCP_PLAYING ? 0x00 : 0xff;', '            uint8_t track=a->playback==BT_AVRCP_PLAYING ? 0xff : 0xff;'),
    ('a-paused-track-plays', '            uint8_t track=a->playback==BT_AVRCP_PLAYING ? 0x00 : 0xff;', '            uint8_t track=a->playback!=BT_AVRCP_STOPPED ? 0x00 : 0xff;'),
    # FAILED begun again by playing.
    ('failed-is-for-ever', '    if(status==BT_AVRCP_PLAYING && a->state==BT_AVRCP_FAILED && a->restarts<BT_AVRCP_RESTARTS) {', '    if(0) {'),
    ('restarts-are-unbounded', '    if(status==BT_AVRCP_PLAYING && a->state==BT_AVRCP_FAILED && a->restarts<BT_AVRCP_RESTARTS) {',
     '    if(status==BT_AVRCP_PLAYING && a->state==BT_AVRCP_FAILED) {'),
    ('any-change-restarts', '    if(status==BT_AVRCP_PLAYING && a->state==BT_AVRCP_FAILED && a->restarts<BT_AVRCP_RESTARTS) {',
     '    if(a->state==BT_AVRCP_FAILED && a->restarts<BT_AVRCP_RESTARTS) {'),
    ('plain-restarts', '    if(status==BT_AVRCP_PLAYING && a->state==BT_AVRCP_FAILED && a->restarts<BT_AVRCP_RESTARTS) {',
     '    if(status==BT_AVRCP_PLAYING && (a->state==BT_AVRCP_FAILED || a->state==BT_AVRCP_PLAIN) && a->restarts<BT_AVRCP_RESTARTS) {'),
    ('ready-restarts', '    if(status==BT_AVRCP_PLAYING && a->state==BT_AVRCP_FAILED && a->restarts<BT_AVRCP_RESTARTS) {',
     '    if(status==BT_AVRCP_PLAYING && a->state==BT_AVRCP_READY && a->restarts<BT_AVRCP_RESTARTS) {'),
    ('restarts-are-not-counted', '        a->restarts++;\n', ''),
    ('closing-keeps-the-restarts', '    a->restarts=0;\n', ''),
    ('four-restarts', '#define BT_AVRCP_RESTARTS       3', '#define BT_AVRCP_RESTARTS       4', HEADER),
    ('the-status-hides-the-restarts', '(unsigned)a->restarts,(unsigned)BT_AVRCP_RESTARTS);', '0u,(unsigned)BT_AVRCP_RESTARTS);'),
    ('the-song-length-is-zero', '        for(int i=0;i<8;i++) ar_put(m,0xff);\n        ar_put(m,a->playback);', '        for(int i=0;i<8;i++) ar_put(m,0x00);\n        ar_put(m,a->playback);'),
    ('play-status-must-be-empty', '        if(ctype!=CT_STATUS) { ar_reject(a,m,label,subunit,pdu,STATUS_INVALID_COMMAND); return; }\n        // It needs no parameters',
     '        if(ctype!=CT_STATUS) { ar_reject(a,m,label,subunit,pdu,STATUS_INVALID_COMMAND); return; }\n        if(pn) { ar_reject(a,m,label,subunit,pdu,STATUS_INVALID_PARAMETER); return; }\n        // It needs no parameters'),
    ('play-status-of-any-type', '        if(ctype!=CT_STATUS) { ar_reject(a,m,label,subunit,pdu,STATUS_INVALID_COMMAND); return; }\n        // It needs no parameters',
     '        // It needs no parameters'),
    ('any-element-is-playing', '        for(size_t i=0;playing && i<8;i++) if(p[i]) playing=false;\n', ''),
    ('the-element-count-is-not-checked', '        if(!playing || pn<9u+4u*p[8])', '        if(!playing)'),
    ('the-elements-must-be-exact', '        if(!playing || pn<9u+4u*p[8])', '        if(!playing || pn!=9u+4u*p[8])'),
    ('the-elements-answer-one', '        ar_vendor(m,pdu,1);\n        ar_put(m,0);', '        ar_vendor(m,pdu,1);\n        ar_put(m,1);'),
    ('an-unknown-pdu-is-an-invalid-parameter', '    default:\n        ar_reject(a,m,label,subunit,pdu,STATUS_INVALID_COMMAND);',
     '    default:\n        ar_reject(a,m,label,subunit,pdu,STATUS_INVALID_PARAMETER);'),
    ('a-reject-has-no-status', '    ar_vendor(m,pdu,1);\n    ar_put(m,status);', '    ar_vendor(m,pdu,1);\n    ar_put(m,(uint8_t)(status&0));'),
    ('a-reject-has-no-pdu', '    ar_begin(m,label,true,CT_REJECTED,subunit,OP_VENDOR);\n    ar_vendor(m,pdu,1);',
     '    ar_begin(m,label,true,CT_REJECTED,subunit,OP_VENDOR);\n    ar_vendor(m,(uint8_t)(pdu&0),1);'),
    # Profiles.
    ('any-profile-is-ours', '    if(profile!=BT_AVRCP_PROFILE) {', '    if(0) {'),
    ('the-ipid-answer-has-no-ipid', 'PKT_SINGLE<<2 | HDR_RESPONSE | HDR_IPID));', 'PKT_SINGLE<<2 | HDR_RESPONSE));'),
    ('the-ipid-answer-names-our-profile', '        ar_put16(&m,profile);\n', '        ar_put16(&m,BT_AVRCP_PROFILE);\n'),
    ('a-command-with-ipid-is-answered', '    if(header&HDR_IPID) { a->malformed++; return; }\n', ''),
    ('another-profile-s-response-is-ours', '    if(profile!=BT_AVRCP_PROFILE && !(header&HDR_IPID)) { a->strays++; return; }\n', ''),
    # The controller.
    ('we-ask-for-the-company', '    ar_put(m,CAP_EVENTS);\n}', '    ar_put(m,CAP_COMPANY);\n}'),
    ('get-capabilities-is-a-control', 'ar_command(a,&a->reg,CT_STATUS,PDU_GET_CAPABILITIES,1);', 'ar_command(a,&a->reg,CT_CONTROL,PDU_GET_CAPABILITIES,1);'),
    ('register-is-a-status', 'ar_command(a,&a->reg,CT_NOTIFY,PDU_REGISTER_NOTIFICATION,5);', 'ar_command(a,&a->reg,CT_STATUS,PDU_REGISTER_NOTIFICATION,5);'),
    ('the-interval-is-one', '    ar_put(m,0); ar_put(m,0); ar_put(m,0); ar_put(m,0);', '    ar_put(m,0); ar_put(m,0); ar_put(m,0); ar_put(m,1);'),
    ('set-is-a-status', 'ar_command(a,&a->set,CT_CONTROL,PDU_SET_ABSOLUTE_VOLUME,1);', 'ar_command(a,&a->set,CT_STATUS,PDU_SET_ABSOLUTE_VOLUME,1);'),
    ('the-set-keeps-the-top-bit', '    ar_put(m,level&0x7f);', '    ar_put(m,level);'),
    ('a-level-keeps-the-top-bit', '    a->volume=v&0x7f;', '    a->volume=v;'),
    ('a-level-is-not-counted', '    a->volume_serial++;\n', ''),
    ('every-list-has-the-volume', '        if(p[2+i]==EVENT_VOLUME) {', '        if(p[2+i]==EVENT_VOLUME || 1) {'),
    ('the-event-count-is-not-checked', '    if(ctype!=CT_STABLE || n<2 || p[0]!=CAP_EVENTS || n<2u+p[1]) {', '    if(ctype!=CT_STABLE || n<2 || p[0]!=CAP_EVENTS) {'),
    ('the-events-must-be-exact', '    if(ctype!=CT_STABLE || n<2 || p[0]!=CAP_EVENTS || n<2u+p[1]) {', '    if(ctype!=CT_STABLE || n<2 || p[0]!=CAP_EVENTS || n!=2u+p[1]) {'),
    ('an-events-answer-of-any-type', '    if(ctype!=CT_STABLE || n<2 || p[0]!=CAP_EVENTS || n<2u+p[1]) {', '    if(n<2 || p[0]!=CAP_EVENTS || n<2u+p[1]+0u*ctype) {'),
    ('the-company-is-the-events', '    if(ctype!=CT_STABLE || n<2 || p[0]!=CAP_EVENTS || n<2u+p[1]) {', '    if(ctype!=CT_STABLE || n<2 || n<2u+p[1]) {'),
    ('a-refused-set-ends-the-volume', '    if(c==&a->set) { a->sets_rejected++; ar_set_done(a); return; }', '    if(c==&a->set) { a->sets_rejected++; ar_set_done(a); }'),
    ('nothing-waits-for-a-set', '    if(a->set_pending) { a->set_pending=false; ar_send_set(a,a->set_wanted); }\n', ''),
    ('the-first-waiting-level-wins', '    if(a->set.live && a->set.sent) { a->set_pending=true; a->set_wanted=raw; return true; }',
     '    if(a->set.live && a->set.sent) { if(!a->set_pending) a->set_wanted=raw; a->set_pending=true; return true; }'),
    ('a-sent-set-is-replaced', '    if(a->set.live && a->set.sent) { a->set_pending=true;', '    if(a->set.live && a->set.sent && 0) { a->set_pending=true;'),
    ('a-set-outside-ready', '    if(a->state!=BT_AVRCP_READY) return false;\n    if(a->set.live', '    if(a->state==BT_AVRCP_IDLE) return false;\n    if(a->set.live'),
    ('an-accepted-set-is-not-the-level', '    ar_level(a,p[0]);\n    ar_set_done(a);', '    (void)p;\n    ar_set_done(a);'),
    ('a-set-interim-is-the-end', '{ a->sets_interim++; a->set.deadline=a->now+BT_AVRCP_REPLY_MS; return; }', '{ a->sets_interim++; ar_set_done(a); return; }'),
    ('a-set-interim-keeps-the-deadline', '{ a->sets_interim++; a->set.deadline=a->now+BT_AVRCP_REPLY_MS; return; }', '{ a->sets_interim++; return; }'),
    ('an-accepted-set-of-any-length', '    if(ctype!=CT_ACCEPTED || n<1) { a->malformed++; return; }', '    if(ctype!=CT_ACCEPTED+(int)(0*n)) { a->malformed++; return; }'),
    ('an-accepted-set-must-be-exact', '    if(ctype!=CT_ACCEPTED || n<1) { a->malformed++; return; }', '    if(ctype!=CT_ACCEPTED || n!=1) { a->malformed++; return; }'),
    ('a-registration-of-any-event', '    if(n<2 || p[0]!=EVENT_VOLUME || (ctype!=CT_INTERIM && ctype!=CT_CHANGED))', '    if(n<2 || (ctype!=CT_INTERIM && ctype!=CT_CHANGED))'),
    ('a-registration-answered-accepted', '    if(n<2 || p[0]!=EVENT_VOLUME || (ctype!=CT_INTERIM && ctype!=CT_CHANGED))', '    if(n<2 || p[0]!=EVENT_VOLUME)'),
    ('a-registration-of-any-length', '    if(n<2 || p[0]!=EVENT_VOLUME || (ctype!=CT_INTERIM && ctype!=CT_CHANGED))', '    if(p[0]!=EVENT_VOLUME+(int)(0*n) || (ctype!=CT_INTERIM && ctype!=CT_CHANGED))'),
    ('a-registration-must-be-exact', '    if(n<2 || p[0]!=EVENT_VOLUME || (ctype!=CT_INTERIM && ctype!=CT_CHANGED))', '    if(n!=2 || p[0]!=EVENT_VOLUME || (ctype!=CT_INTERIM && ctype!=CT_CHANGED))'),
    ('a-changed-is-not-registered-again', '    a->state=BT_AVRCP_READY;\n    ar_register(a);\n}', '    a->state=BT_AVRCP_READY;\n}'),
    ('an-interim-keeps-the-timer', '        a->reg.sent=true; a->reg.waiting=false;\n        ar_level(a,p[1]);', '        a->reg.sent=true;\n        ar_level(a,p[1]);'),
    ('an-interim-leaves-the-copy-queued', '        a->reg.sent=true; a->reg.waiting=false;\n        ar_level(a,p[1]);', '        a->reg.waiting=false;\n        ar_level(a,p[1]);'),
    ('an-interim-is-not-ready', '        ar_level(a,p[1]);\n        a->state=BT_AVRCP_READY;\n        return;', '        ar_level(a,p[1]);\n        return;'),
    ('a-label-may-be-the-other-s', '    if(other->live && other->label==l) l=(uint8_t)((l+1)&15);\n', '    (void)other;\n'),
    ('labels-do-not-move', '    a->next_label=(uint8_t)((l+1)&15);', '    a->next_label=l;'),
    ('an-unsent-command-is-answered', '    return c->live && (c->sent || c->tries) && c->label==label;', '    return c->live && c->label==label;'),
    ('a-dead-command-is-answered', '    return c->live && (c->sent || c->tries) && c->label==label;', '    return (c->sent || c->tries) && c->label==label;'),
    ('the-first-copy-s-answer-is-a-stray', '    return c->live && (c->sent || c->tries) && c->label==label;', '    return c->live && c->sent && c->label==label;'),
    ('any-opcode-answers', '    if(f[2]!=OP_VENDOR) { a->strays++; return; }\n', ''),
    ('a-not-implemented-of-another-pdu-is-ours', '        if(sig && on>=4 && o[3]!=c->pdu_id) { a->strays++; return; }\n', ''),
    ('a-response-of-another-pdu-is-ours', '    if(o[3]!=c->pdu_id) { a->strays++; return; }\n', ''),
    ('a-response-s-length-is-not-checked', '    if((o[4]&3)!=PKT_SINGLE || pn>on-7) { a->malformed++; return; }', '    if((o[4]&3)!=PKT_SINGLE) { a->malformed++; return; }'),
    ('a-response-fragment-is-taken', '    if((o[4]&3)!=PKT_SINGLE || pn>on-7) { a->malformed++; return; }', '    if(pn>on-7) { a->malformed++; return; }'),
    ('a-response-must-be-exact', '    if((o[4]&3)!=PKT_SINGLE || pn>on-7) { a->malformed++; return; }', '    if((o[4]&3)!=PKT_SINGLE || pn!=on-7) { a->malformed++; return; }'),
    ('a-short-response-is-read', '    if(!sig || on<7) { a->malformed++; return; }', '    if(!sig || on<4) { a->malformed++; return; }'),
    ('an-ipid-answer-is-a-stray', '    if(header&HDR_IPID) { a->invalid_profile++; ar_refused(a,c); return; }', '    if(header&HDR_IPID) { a->invalid_profile++; return; }'),
    ('a-rejection-is-not-a-refusal', '    if(ctype==CT_REJECTED) { ar_refused(a,c); return; }\n', ''),
    # Timers.
    ('the-reply-waits-3s', '#define BT_AVRCP_REPLY_MS       2000', '#define BT_AVRCP_REPLY_MS       3000', HEADER),
    ('one-silence-is-failure', '    if(c->tries) { ar_controller_ends(a,BT_AVRCP_FAILED); return; }', '    if(1) { ar_controller_ends(a,BT_AVRCP_FAILED); return; }'),
    ('the-set-is-sent-twice', '        c->tries=1;\n        c->deadline=a->now+BT_AVRCP_REPLY_MS;\n        return;',
     '        c->tries=1;\n        c->sent=false; c->waiting=false; c->msg.id=ar_id(a);\n        return;'),
    ('the-set-has-no-second-period', '        c->tries=1;\n        c->deadline=a->now+BT_AVRCP_REPLY_MS;\n        return;', '        c->tries=1;\n        return;'),
    ('a-silent-set-fails-the-controller', '        if(c->tries) { a->sets_silent++; ar_set_done(a); return; }', '        if(c->tries) { ar_controller_ends(a,BT_AVRCP_FAILED); return; }'),
    ('a-silent-set-fails-at-once', '        if(c->tries) { a->sets_silent++; ar_set_done(a); return; }', '        if(1) { a->sets_silent++; ar_set_done(a); return; }'),
    ('a-silent-set-is-not-counted', '        if(c->tries) { a->sets_silent++; ar_set_done(a); return; }', '        if(c->tries) { ar_set_done(a); return; }'),
    ('a-silent-set-keeps-waiting', '        if(c->tries) { a->sets_silent++; ar_set_done(a); return; }', '        if(c->tries) { a->sets_silent++; return; }'),
    ('a-silent-set-drops-the-waiting-level', '        if(c->tries) { a->sets_silent++; ar_set_done(a); return; }',
     '        if(c->tries) { a->sets_silent++; a->set.live=false; a->set_pending=false; return; }'),
    ('the-status-hides-the-silent-sets', '                   (unsigned)a->sets_silent,', '                   0u,'),
    ('a-retry-is-not-sent', '    c->sent=false; c->waiting=false;\n    c->msg.id=ar_id(a);', '    c->waiting=false;\n    c->msg.id=ar_id(a);'),
    ('a-retry-keeps-its-serial', '    c->sent=false; c->waiting=false;\n    c->msg.id=ar_id(a);', '    c->sent=false; c->waiting=false;'),
    ('the-timer-is-not-started', '        c->sent=true; c->waiting=true; c->deadline=a->now+BT_AVRCP_REPLY_MS;', '        c->sent=true; c->waiting=true;'),
    ('a-silence-is-not-counted', '    a->silences++;\n', ''),
    # Take then commit.
    ('an-unsendable-message-stops-the-queue', '        if(n>mtu) { a->unsendable++; ar_commit(a,c,0); continue; }', '        if(n>mtu) { a->unsendable++; return 0; }'),
    ('answers-leave-out-of-order', '    for(unsigned i=at+1;i<a->answers;i++) a->answer[i-1]=a->answer[i];\n', '    (void)at;\n'),
    ('a-set-goes-before-the-answers', '    if(a->answers) return &a->answer[0];', '    if(a->answers && !(a->set.live && !a->set.sent)) return &a->answer[0];'),
    ('sent-takes-any-answer', '    for(unsigned i=0;i<a->answers;i++) if(a->answer[i].id==serial) { *at=i; return &a->answer[i]; }',
     '    for(unsigned i=0;i<a->answers;i++) if(a->answer[i].id==serial || 1) { *at=i; return &a->answer[i]; }'),
    ('sent-takes-no-answer', '    for(unsigned i=0;i<a->answers;i++) if(a->answer[i].id==serial) { *at=i; return &a->answer[i]; }',
     '    for(unsigned i=0;i<a->answers && 0;i++) if(a->answer[i].id==serial) { *at=i; return &a->answer[i]; }'),
    ('a-parked-registration-is-refused', '    if(a->reg.live && !a->reg.sent && a->reg.msg.id==serial) { *c=&a->reg;',
     '    if(!a->answers && a->reg.live && !a->reg.sent && a->reg.msg.id==serial) { *c=&a->reg;'),
    ('a-parked-set-is-refused', '    if(a->set.live && !a->set.sent && a->set.msg.id==serial) { *c=&a->set;',
     '    if(!a->answers && a->set.live && !a->set.sent && a->set.msg.id==serial) { *c=&a->set;'),
    ('a-sent-command-is-sent-again', '    if(a->reg.live && !a->reg.sent && a->reg.msg.id==serial) { *c=&a->reg;',
     '    if(a->reg.live && a->reg.msg.id==serial) { *c=&a->reg;'),
    ('a-replaced-set-is-sent', '    if(a->set.live && !a->set.sent && a->set.msg.id==serial) { *c=&a->set;',
     '    if(a->set.live && !a->set.sent) { *c=&a->set;'),
    ('the-capacity-is-not-checked', '        if(cap<n) return 0;', '        if(cap+1<n) return 0;'),
    ('a-set-is-not-counted', '        if(c==&a->set) a->sets++;\n', ''),
    ('a-replaced-set-keeps-its-serial', '    c->msg.id=ar_id(a);\n    return &c->msg;', '    if(!c->msg.id) c->msg.id=ar_id(a);\n    return &c->msg;'),
    # Fragments.
    ('a-single-keeps-the-gathering', '        if(a->rx_active) { a->fragments_dropped++; a->rx_active=false; }\n        if(type==PKT_SINGLE) {',
     '        if(type==PKT_SINGLE) {'),
    ('a-start-of-one-packet-is-taken', '        if(n<4 || p[1]<2) { a->fragments_dropped++; return; }', '        if(n<4 || p[1]<1) { a->fragments_dropped++; return; }'),
    ('a-start-may-run-past', '        if(n-4>sizeof(a->rx)) { a->too_long++; return; }', '        if(n-4>sizeof(a->rx)+1) { a->too_long++; return; }'),
    ('a-continue-may-run-past', '    if(n-1>sizeof(a->rx)-a->rx_have) { a->too_long++;', '    if(n-1>sizeof(a->rx)-a->rx_have+1) { a->too_long++;'),
    ('the-header-is-not-matched', '    if((p[0]&0xf3)!=(a->rx_header&0xf3) || (end ? a->rx_left!=1 : a->rx_left<=1)) {', '    if((end ? a->rx_left!=1 : a->rx_left<=1)) {'),
    ('the-ipid-is-not-matched', '    if((p[0]&0xf3)!=(a->rx_header&0xf3) ||', '    if((p[0]&0xf2)!=(a->rx_header&0xf2) ||'),
    ('an-end-may-come-early', '(end ? a->rx_left!=1 : a->rx_left<=1)) {', '(end ? 0 : a->rx_left<=1)) {'),
    ('a-continue-may-be-last', '(end ? a->rx_left!=1 : a->rx_left<=1)) {', '(end ? a->rx_left!=1 : 0)) {'),
    ('the-fragment-s-profile-is-ours', 'a->rx_profile=ar_u16(p+2);', 'a->rx_profile=BT_AVRCP_PROFILE;'),
    ('a-message-of-511', '#define BT_AVRCP_MESSAGE        512', '#define BT_AVRCP_MESSAGE        511', HEADER),
    ('the-reassembly-holds-one-more', '    uint8_t rx[BT_AVRCP_MESSAGE];', '    uint8_t rx[BT_AVRCP_MESSAGE + 1];', HEADER),
    # The states and the channel.
    ('idle-takes-messages', '    if(a->state==BT_AVRCP_IDLE) { a->ignored++; return; }\n', ''),
    ('init-plays', 'a->state=BT_AVRCP_IDLE; a->playback=BT_AVRCP_STOPPED; }', 'a->state=BT_AVRCP_IDLE; a->playback=BT_AVRCP_PLAYING; }'),
    ('open-twice-asks-twice', '    if(a->state!=BT_AVRCP_IDLE) return;\n    ar_ask(a);', '    ar_ask(a);'),
    ('closing-keeps-registrations', '    a->play_registered=false; a->track_registered=false;', '    a->track_registered=false;'),
    ('closing-keeps-the-answers', '    a->answers=0;\n    ar_controller_ends(a,BT_AVRCP_IDLE);', '    ar_controller_ends(a,BT_AVRCP_IDLE);'),
    ('closing-keeps-the-gathering', '    a->rx_active=false;\n}\n\nvoid bt_avrcp_receive', '}\n\nvoid bt_avrcp_receive'),
    ('closing-keeps-the-commands', '    a->reg.live=false; a->set.live=false; a->set_pending=false;', '    a->set_pending=false;'),
    ('the-volume-is-known-outside-ready', '    if(a->state!=BT_AVRCP_READY) return false;\n    *raw=a->volume;', '    *raw=a->volume;'),
    ('a-status-of-7-is-taken', '    if(status>BT_AVRCP_PAUSED || status==a->playback) return;', '    if(status>BT_AVRCP_PAUSED+5 || status==a->playback) return;'),
    ('the-same-status-is-a-change', '    if(status>BT_AVRCP_PAUSED || status==a->playback) return;', '    if(status>BT_AVRCP_PAUSED) return;'),
    ('a-change-does-not-spend', '    a->play_registered=false;\n    if(a->answers>=BT_AVRCP_ANSWERS)', '    if(a->answers>=BT_AVRCP_ANSWERS)'),
    ('the-changed-says-the-old-status', '    ar_put(m,EVENT_PLAYBACK); ar_put(m,status);', '    ar_put(m,EVENT_PLAYBACK); ar_put(m,a->playback^3);'),
    # The status text.
    ('the-status-overruns', '    return n<cap ? n : cap-1;', '    return n;'),
    ('the-status-shows-no-level', '"avrcp: %s, their volume %u/127 (told %u times)\\n"', '"avrcp: %s, their volume %u/128 (told %u times)\\n"'),
]

caught, missed, unbuilt = [], [], []
for mutant in mutants:
    name, old, new = mutant[:3]
    target = mutant[3] if len(mutant) > 3 else AVRCP
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

print(f'\nbt_avrcp mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
