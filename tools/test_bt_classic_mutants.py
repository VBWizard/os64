#!/usr/bin/env python3
"""One deliberate break at a time to the Classic Bluetooth link
(kernel/src/driver/system/usb/bt_classic.c: the request parser, the event
lengths and claims, each phase's transition and its condition, the latched
answers and their order, the refusals, each deadline, page scan, the
key-size bar, the sniff commands that may never end the link, the bond and
its codec), against tools/test_bt_classic_host.sh:
a mutant is CAUGHT when the suite fails, MISSED when it passes, which means a
test is missing and gets written. A build failure is neither.

The shape is test_bt_acl_mutants.py's: each mutant is applied to the tree's
copy of the file, the suite is run, and the file is put back from the bytes
read before the change, checked byte for byte, whatever happens. Run it from
a clean tree, and never beside another harness that edits the same tree; the
per-mutant logs are in --logs. Under tools/mutants_parallel.py it needs
`--with kernel`: the runner's copies do not carry the kernel's sources unless
asked.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/bt-classic-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

CLASSIC = 'kernel/src/driver/system/usb/bt_classic.c'
CASES = 'bash tools/test_bt_classic_host.sh 300'

mutants = [
    # The deadlines.
    ('command-deadline-is-longer', '#define CL_COMMAND_MS 2000', '#define CL_COMMAND_MS 3000'),
    ('page-wait-is-shorter-than-the-page', '#define CL_PAGE_MS 6000', '#define CL_PAGE_MS 5000'),
    ('page-wait-is-longer', '#define CL_PAGE_MS 6000', '#define CL_PAGE_MS 7000'),
    ('accept-wait-is-longer', '#define CL_ACCEPT_MS 6000', '#define CL_ACCEPT_MS 7000'),
    ('inbound-wait-is-longer', '#define CL_INBOUND_MS 2000', '#define CL_INBOUND_MS 3000'),
    ('authentication-wait-is-shorter', '#define CL_AUTH_MS 40000', '#define CL_AUTH_MS 30000'),
    ('encryption-wait-is-longer', '#define CL_ENCRYPT_MS 10000', '#define CL_ENCRYPT_MS 12000'),
    ('close-wait-is-longer', '#define CL_CLOSE_MS 2000', '#define CL_CLOSE_MS 3000'),
    ('answers-wait-longer', '#define CL_ANSWER_MS 25000', '#define CL_ANSWER_MS 30000'),
    ('answers-wait-less', '#define CL_ANSWER_MS 25000', '#define CL_ANSWER_MS 20000'),

    # Who the link's peer is, and what may come back.
    ('closing-is-active', '{ return c->phase>=BT_CLASSIC_PAGING && c->phase<=BT_CLASSIC_READY; }',
     '{ return c->phase>=BT_CLASSIC_PAGING && c->phase<=BT_CLASSIC_CLOSING; }'),
    ('a-dead-handle-is-ours', '{ return c->connected && cl_u16(p)==c->handle; }',
     '{ return cl_u16(p)==c->handle; }'),
    ('a-timeout-is-final', '{ return reason==0x08 || reason==0x13',
     '{ return reason==0x13'),
    ('lmp-timeout-is-final', ' || reason==0x15 || reason==0x22; }', ' || reason==0x15; }'),
    ('our-own-end-may-come-back', ' || reason==0x15 || reason==0x22; }', ' || reason==0x15 || reason==0x22 || reason==0x16; }'),

    # Names, the request parser.
    ('a-name-keeps-unit-separator', 'out[i]=name[i]>=32 && name[i]<127', 'out[i]=name[i]>=31 && name[i]<127'),
    ('a-name-keeps-delete', 'out[i]=name[i]>=32 && name[i]<127', 'out[i]=name[i]>=32 && (unsigned char)name[i]<128'),
    ('a-name-fills-its-terminator', 'for(;name && i<31 && name[i];i++)', 'for(;name && i<32 && name[i];i++)'),
    ('page-scan-is-inquiry-scan', '{ c->scan_enable_wanted=c->bond.valid ? 0x02 : 0x00; }',
     '{ c->scan_enable_wanted=c->bond.valid ? 0x03 : 0x00; }'),
    ('page-scan-before-the-disk', '{ return c->bond_loaded && c->scan_enable_wanted!=c->scan_enable_tried; }',
     '{ return c->scan_enable_wanted!=c->scan_enable_tried; }'),
    ('a-refused-write-is-repeated', '{ return c->bond_loaded && c->scan_enable_wanted!=c->scan_enable_tried; }',
     '{ return c->bond_loaded && c->scan_enable_wanted!=c->scan_enable_set; }'),
    ('a-load-trusts-the-controller', 'first turn after a load writes it.\n    c->scan_enable_set=c->scan_enable_tried=0xff;',
     'first turn after a load writes it.\n'),
    ('a-refused-write-is-confirmed', '        if(!st) c->scan_enable_set=c->command_value;',
     '        c->scan_enable_set=c->command_value;'),
    ('an-unanswered-write-says-nothing', '    if(op==0x0c1a) { c->scan_enable_status=0xff; return; }',
     '    if(op==0x0c1a) { return; }'),
    ('forget-saves-nothing', '        c->bond_dirty=true; cl_want_scan(c);\n        if(cl_active(c)) cl_close(c,0x13);',
     '        cl_want_scan(c);\n        if(cl_active(c)) cl_close(c,0x13);'),
    ('forget-leaves-the-link', '        c->bond_dirty=true; cl_want_scan(c);\n        if(cl_active(c)) cl_close(c,0x13);',
     '        c->bond_dirty=true; cl_want_scan(c);\n'),
    ('disconnect-with-nothing-is-taken', '        if(!cl_active(c)) { *refusal="not connected"; return false; }',
     '        if(0) { *refusal="not connected"; return false; }'),
    ('busy-is-not-refused', '    if(c->phase!=BT_CLASSIC_IDLE && c->phase!=BT_CLASSIC_STOPPED) { *refusal="busy',
     '    if(0 && c->phase!=BT_CLASSIC_STOPPED) { *refusal="busy'),
    ('reconnect-without-a-bond', '        if(!c->bond.valid) { *refusal="no bond', '        if(0) { *refusal="no bond'),
    ('an-le-address-is-paged', '        if(hint->le) {', '        if(0) {'),
    ('any-hint-is-this-address', '    if(hint && cl_equal(hint->address,address,6)) {', '    if(hint) {'),
    ('the-address-of-zeroes', '    return any!=0;', '    return true;'),
    ('any-separator', "(i<5 && p[3*i+2]!=':')", "(i<5 && p[3*i+2]==' ')"),
    ('bond-takes-a-tail', 'n==22 && cl_word(p,5,"bond ")', 'n>=22 && cl_word(p,5,"bond ")'),
    ('connect-takes-a-tail', 'n==25 && cl_word(p,8,"connect ")', 'n>=25 && cl_word(p,8,"connect ")'),
    ('no-upper-case-hex', "    if(ch>='A' && ch<='F') return ch-'A'+10;\n", ''),
    ('a-word-may-run-on', '    return i==n && !word[i];', '    return !word[i];'),
    ('the-address-is-backwards', '        out[5-i]=(uint8_t)(hi<<4 | lo); any|=out[5-i];',
     '        out[i]=(uint8_t)(hi<<4 | lo); any|=out[i];'),
    ('no-trailing-trim', '    while(n && cl_space(p[n-1])) n--;\n', ''),

    # Event lengths and claims.
    ('connection-complete-length', '{0x03,11,false}', '{0x03,12,false}'),
    ('connection-request-length', '{0x04,10,false}', '{0x04,11,false}'),
    ('disconnection-length', '{0x05,4,true}', '{0x05,5,true}'),
    ('disconnection-is-classic-only', '{0x05,4,true}', '{0x05,4,false}'),
    ('authentication-length', '{0x06,3,false}', '{0x06,4,false}'),
    ('key-notification-length', '{0x18,23,false}', '{0x18,22,false}'),
    ('io-response-length', '{0x32,9,false}', '{0x32,10,false}'),
    ('confirmation-length', '{0x33,10,false}', '{0x33,6,false}'),
    ('pairing-complete-length', '{0x36,7,false}', '{0x36,8,false}'),
    ('no-parameter-check', '    if(e[1]!=cl_events[i].params) { c->malformed++; return !cl_events[i].shared; }',
     '    if(0) { c->malformed++; return !cl_events[i].shared; }'),
    ('no-length-byte-check', '    if(n<2 || n!=2u+e[1]) { c->malformed++; return false; }',
     '    if(n<2) { c->malformed++; return false; }'),
    ('failed-still-hears', '    if(c->phase==BT_CLASSIC_FAILED) return !cl_events[i].shared;\n', ''),

    # Command replies.
    ('accept-completes', 'case 0x0405: case 0x0406: case 0x0409:', 'case 0x0405: case 0x0406:'),
    ('key-size-carries-an-address', '    case 0x1408: return CL_KEY_SIZE;', '    case 0x1408: return CL_ADDRESS;'),
    ('a-reply-with-nothing-in-flight', '    if(!c->pending || cl_u16(e+(cs ? 4 : 3))!=c->opcode) return false;',
     '    if(cl_u16(e+(cs ? 4 : 3))!=c->opcode) return false;'),
    ('a-second-reply-is-fine', '    if(c->command_done) { c->malformed++; return true; }', '    if(0) { c->malformed++; return true; }'),
    ('status-zero-accepts-anything', '        if(n!=6 || (kind!=CL_ASYNC && !e[2])) {', '        if(n!=6) {'),
    ('complete-finishes-an-async', '    if(kind==CL_ASYNC || n!=want) {', '    if(n!=want) {'),
    ('any-address-comes-back', '        if(kind==CL_ADDRESS && !cl_equal(e+6,c->command_address,6)) {',
     '        if(0) {'),
    ('any-handle-has-the-key-size', '            if(cl_u16(e+6)!=c->command_handle || e[8]>16) {',
     '            if(e[8]>16) {'),
    ('seventeen-bytes-of-key', '|| e[8]>16) {', '|| e[8]>17) {'),

    # Connection Complete, Connection Request.
    ('a-voice-link-completes-the-page', '        if(e[11]!=1 || !cl_equal(e+5,c->peer,6)) return true;',
     '        if(!cl_equal(e+5,c->peer,6)) return true;'),
    ('closing-takes-any-completion', '!(c->phase==BT_CLASSIC_CLOSING && c->link_may_active)) return true;',
     '!(c->phase==BT_CLASSIC_CLOSING)) return true;'),
    ('a-reserved-handle', 'cl_u16(e+3)>0x0eff) { c->malformed++; return true; }', 'cl_u16(e+3)>0x0f00) { c->malformed++; return true; }'),
    ('a-failed-page-may-come-back', 'cl_fail(c,"the connection was not made",0,status,status==0x04 || cl_far_end_gone(status),0x13);',
     'cl_fail(c,"the connection was not made",0,status,true,0x13);'),
    ('an-unanswered-page-is-paged-again', 'cl_fail(c,"the connection was not made",0,status,status==0x04 || cl_far_end_gone(status),0x13);',
     'cl_fail(c,"the connection was not made",0,status,cl_far_end_gone(status),0x13);'),
    ('an-ended-call-is-rejected', '            c->answer[BT_CLASSIC_ANSWER_ACCEPT].set=false;\n            cl_fail',
     '            cl_fail'),
    ('a-call-is-treated-as-a-page', '        if(c->session==BT_CLASSIC_SESSION_INBOUND) {\n            c->phase=BT_CLASSIC_LINKED;',
     '        if(0) {\n            c->phase=BT_CLASSIC_LINKED;'),
    ('a-voice-link-is-refused-as-a-call', 'cl_refuse(c,0x042a,a,0x0d);', 'cl_refuse(c,0x042a,a,0x0f);'),
    ('an-unknown-link-type-is-voice', '        if(e[11]>2) { c->malformed++; return true; }', '        if(e[11]>3) { c->malformed++; return true; }'),
    ('a-call-needs-no-bond', '        bool bonded=c->bond.valid && cl_equal(a,c->bond.peer,6);', '        bool bonded=cl_equal(a,c->bond.peer,6);'),
    ('a-busy-link-takes-a-call', '        if(bonded && (c->phase==BT_CLASSIC_IDLE || c->phase==BT_CLASSIC_STOPPED)) {', '        if(bonded) {'),
    ('a-stranger-rejected-for-resources', 'cl_refuse(c,0x040a,a,bonded ? 0x0d : 0x0f);', 'cl_refuse(c,0x040a,a,0x0d);'),
    ('busy-is-unacceptable', 'cl_refuse(c,0x040a,a,bonded ? 0x0d : 0x0f);', 'cl_refuse(c,0x040a,a,0x0f);'),

    # Two calls that cross.
    ('a-crossing-call-is-refused', '        if(c->phase==BT_CLASSIC_PAGING && !c->crossing && cl_equal(a,c->peer,6)) {',
     '        if(0 && c->phase==BT_CLASSIC_PAGING && !c->crossing && cl_equal(a,c->peer,6)) {'),
    ('any-call-while-paging-crosses', '        if(c->phase==BT_CLASSIC_PAGING && !c->crossing && cl_equal(a,c->peer,6)) {',
     '        if(c->phase==BT_CLASSIC_PAGING && !c->crossing) {'),
    ('calls-cross-twice', '        if(c->phase==BT_CLASSIC_PAGING && !c->crossing && cl_equal(a,c->peer,6)) {',
     '        if(c->phase==BT_CLASSIC_PAGING && cl_equal(a,c->peer,6)) {'),
    ('a-crossing-call-is-not-noted', '            c->crossing=true; cl_latch(c,BT_CLASSIC_ANSWER_ACCEPT);',
     '            cl_latch(c,BT_CLASSIC_ANSWER_ACCEPT);'),
    ('one-failure-ends-a-crossing', '        if(status && c->crossing) { c->crossing=false; return true; }', '        if(0) { c->crossing=false; return true; }'),
    ('a-crossing-failure-is-forgotten', '        if(status && c->crossing) { c->crossing=false; return true; }', '        if(status && c->crossing) { return true; }'),
    ('a-made-link-is-accepted-again', '        if(c->crossing) c->answer[BT_CLASSIC_ANSWER_ACCEPT].set=false;\n', ''),
    ('a-refused-crossing-accept-ends-it', "    if(st && (op==0x0405 || op==0x0409) && (c->crossing || c->connected)) { c->crossing=false; return; }",
     "    if(0) { c->crossing=false; return; }"),
    ('a-late-accept-refusal-ends-the-link', "    if(st && (op==0x0405 || op==0x0409) && (c->crossing || c->connected)) { c->crossing=false; return; }",
     "    if(st && (op==0x0405 || op==0x0409) && (c->crossing)) { c->crossing=false; return; }"),
    ('a-cancel-ends-the-crossing-call', '            if(c->crossing) c->crossing=false; else c->link_may_active=false;',
     '            c->crossing=false; c->link_may_active=false;'),
    ('a-refused-crossing-call-is-still-owed', '    c->crossing=c->crossing && !c->answer[BT_CLASSIC_ANSWER_ACCEPT].set;', ''),

    # Disconnection, authentication, encryption.
    ('the-reason-is-lost', 'c->encrypted=false; c->disconnect_reason=e[5];', 'c->encrypted=false;'),
    ('a-failed-disconnection-is-one', '        if(e[2]) {\n            if(c->phase==BT_CLASSIC_CLOSING) cl_give_up',
     '        if(0) {\n            if(c->phase==BT_CLASSIC_CLOSING) cl_give_up'),
    ('authenticated-anywhere', '            if(c->phase==BT_CLASSIC_AUTHENTICATING) {\n                c->phase=BT_CLASSIC_ENCRYPTING;',
     '            if(1) {\n                c->phase=BT_CLASSIC_ENCRYPTING;'),
    ('stale-without-giving-the-key', '        if(e[2]==0x06 && c->key_given)', '        if(e[2]==0x06)'),
    ('stale-is-authentication-failure', '        if(e[2]==0x06 && c->key_given)', '        if(e[2]==0x05 && c->key_given)'),
    ('stale-closes-politely', 'cl_fail(c,cl_stale,0,e[2],false,0x05);', 'cl_fail(c,cl_stale,0,e[2],false,0x13);'),
    ('stale-may-come-back', 'cl_fail(c,cl_stale,0,e[2],false,0x05);', 'cl_fail(c,cl_stale,0,e[2],true,0x05);'),
    ('aes-is-not-encryption', 'bool on=!e[2] && (e[5]==1 || e[5]==2);', 'bool on=!e[2] && (e[5]==1);'),
    ('an-error-may-say-on', 'bool on=!e[2] && (e[5]==1 || e[5]==2);', 'bool on=(e[5]==1 || e[5]==2);'),
    ('lost-encryption-closes-politely', '"encryption failed or was turned off",0,e[2],cl_far_end_gone(e[2]),0x05);',
     '"encryption failed or was turned off",0,e[2],cl_far_end_gone(e[2]),0x13);'),
    ('a-caller-must-be-asked', '        if(c->phase==BT_CLASSIC_LINKED || c->phase==BT_CLASSIC_AUTHENTICATING',
     '        if(c->phase==BT_CLASSIC_AUTHENTICATING'),
    ('the-role-is-an-address-byte', 'c->role_known=true; c->role=e[9];', 'c->role_known=true; c->role=e[8];'),
    ('the-interval-is-the-mode', 'c->mode_interval=cl_u16(e+6);', 'c->mode_interval=cl_u16(e+5);'),
    ('pin-goes-unanswered', '        if(cl_ours(c,e+2)) cl_latch(c,BT_CLASSIC_ANSWER_PIN);', '        if(0) cl_latch(c,BT_CLASSIC_ANSWER_PIN);'),
    ('a-strangers-key-ask-is-a-pin', '            if(e[0]==0x17) cl_refuse(c,0x040c,e+2,0);', '            if(e[0]==0x17) cl_refuse(c,0x040e,e+2,0);'),
    ('pairing-refused-for-another-reason', 'cl_refuse(c,0x0434,e+2,0x18);', 'cl_refuse(c,0x0434,e+2,0x16);'),
    ('a-securing-caller-is-interrupted', 'c->peer_authenticating=true; c->waiting=true; c->deadline=c->now+CL_AUTH_MS;',
     'c->peer_authenticating=true;'),
    ('every-session-keeps-a-key', '        if(cl_ours(c,e+2) && c->session==BT_CLASSIC_SESSION_BOND) {', '        if(cl_ours(c,e+2)) {'),
    ('a-strangers-key-is-kept', '        if(cl_ours(c,e+2) && c->session==BT_CLASSIC_SESSION_BOND) {',
     '        if(c->session==BT_CLASSIC_SESSION_BOND) {'),
    ('the-key-type-is-a-key-byte', 'c->new_key_type=e[24];', 'c->new_key_type=e[23];'),
    ('the-io-response-is-shifted', 'cl_copy(c->peer_io,e+8,3);', 'cl_copy(c->peer_io,e+7,3);'),
    ('a-strangers-confirmation-is-accepted', 'else cl_refuse(c,0x042d,e+2,0);', 'else cl_refuse(c,0x042c,e+2,0);'),
    ('failed-pairing-is-ignored', '        if(e[2]) cl_fail(c,"simple pairing failed"', '        if(0) cl_fail(c,"simple pairing failed"'),

    # READY, and what a finished command means.
    ('a-bond-without-a-new-key', '    if(c->session==BT_CLASSIC_SESSION_BOND && c->have_key) {', '    if(c->session==BT_CLASSIC_SESSION_BOND) {'),
    ('a-new-bond-is-not-saved', '            c->bond_dirty=true; cl_want_scan(c);\n        }', '            cl_want_scan(c);\n        }'),
    ('a-debug-key-is-kept', '        if(c->new_key_type==0x03) c->debug_key=true;', '        if(0) c->debug_key=true;'),
    ('a-debug-key-is-unremarked', '        if(c->new_key_type==0x03) c->debug_key=true;', '        if(c->new_key_type==0x03) {}'),
    ('the-wrong-key-type-is-debug', '        if(c->new_key_type==0x03) c->debug_key=true;', '        if(c->new_key_type==0x04) c->debug_key=true;'),
    ('a-debug-key-is-remembered', ' c->close_reason=0; c->debug_key=false;', ' c->close_reason=0;'),
    ('a-debug-key-is-not-said', '    if(c->debug_key) snprintf(bond,sizeof(bond),"not kept (debug link key); held: %s",held);',
     '    if(0) snprintf(bond,sizeof(bond),"not kept (debug link key); held: %s",held);'),
    ('seven-bytes-is-enough', '        if(c->key_size<16) cl_fail(c,"weak', '        if(c->key_size<7) cl_fail(c,"weak'),
    ('sixteen-is-weak', '        if(c->key_size<16) cl_fail(c,"weak', '        if(c->key_size<=16) cl_fail(c,"weak'),
    ('a-cancel-of-nothing-waits', '        if(st==0x02) {\n            if(c->crossing)', '        if(0) {\n            if(c->crossing)'),
    ('a-gone-link-is-refused', '        if(st==0x02) { c->connected=false; c->encrypted=false; }', '        if(0) { c->connected=false; c->encrypted=false; }'),
    ('a-refused-disconnect-waits', '        else if(st) cl_give_up(c,"the controller refused to disconnect",op,st);\n', ''),
    ('a-refused-page-is-still-owed', '        if(op==0x0405 || op==0x0409) c->link_may_active=false;\n', ''),
    ('an-old-reply-ends-a-new-attempt', '    if(!c->command_link || c->command_attempt!=c->attempt) return;\n    if(op==0x0408)',
     '    if(!c->command_link) return;\n    if(op==0x0408)'),
    ('an-old-timeout-ends-a-new-attempt', '    if(!c->command_link || c->command_attempt!=c->attempt) return;\n    if(op==0x0406 || op==0x0408)',
     '    if(!c->command_link) return;\n    if(op==0x0406 || op==0x0408)'),
    ('an-unanswered-disconnect-waits', '        if(c->phase==BT_CLASSIC_CLOSING) cl_give_up(c,"the controller did not answer the disconnection",op,0);\n', ''),

    # The errors.
    ('the-last-error-wins', '    if(c->error) return;\n    c->error=why;', '    c->error=why;'),
    ('the-status-is-not-kept', 'c->error_opcode=op; c->error_status=status; c->retryable=retry;', 'c->error_opcode=op; c->error_status=0*status; c->retryable=retry;'),
    ('a-new-attempt-keeps-the-old-error', '    c->error=NULL; c->error_opcode=0;', '    c->error_opcode=0;'),
    ('closing-records-failures', '    if(!cl_active(c)) return;\n    cl_error(c,why,op,status,retry);', '    cl_error(c,why,op,status,retry);'),
    ('attempts-are-not-counted', '    c->attempt++;\n', ''),

    # Closing.
    ('an-unsent-accept-is-dropped', '    if(c->answer[BT_CLASSIC_ANSWER_ACCEPT].set) cl_refuse(c,0x040a,c->peer,0x0f);\n', ''),
    ('a-call-is-cancelled-like-a-page', '        if(c->session!=BT_CLASSIC_SESSION_INBOUND) c->next_op=0x0408;', '        if(1) c->next_op=0x0408;'),
    ('closing-keeps-the-new-key', '    cl_answers_clear(c);\n    cl_key_wipe(c);\n    cl_sniff_clear(c);\n    c->next_op=0;',
     '    cl_answers_clear(c);\n    cl_sniff_clear(c);\n    c->next_op=0;'),
    ('giving-up-keeps-the-handle', '    c->connected=false; c->encrypted=false; c->link_may_active=false; c->crossing=false;\n',
     '    c->encrypted=false; c->link_may_active=false; c->crossing=false;\n'),
    ('a-late-link-is-left-up', '        if(c->connected && !c->disconnect_sent && !c->next_op) c->next_op=0x0406;\n', ''),
    ('a-stale-cancel-is-sent', '        if(c->next_op==0x0408 && !c->link_may_active) c->next_op=0;\n', ''),
    ('closing-ends-with-a-call-owed', '        if(!c->connected && !c->link_may_active) { c->next_op=0;', '        if(!c->connected) { c->next_op=0;'),

    # The tick.
    ('failed-is-not-failed', '        c->phase=BT_CLASSIC_FAILED; c->pending=false; c->next_op=0; c->waiting=false;',
     '        c->pending=false; c->next_op=0; c->waiting=false;'),
    ('failed-keeps-the-new-key', '        cl_answers_clear(c); c->refusal_count=0; cl_key_wipe(c); cl_sniff_clear(c);\n        return;',
     '        cl_answers_clear(c); c->refusal_count=0; cl_sniff_clear(c);\n        return;'),
    ('done-before-the-transfer', '        if(c->command_done && control) { c->pending=false; cl_finish(c); }',
     '        if(c->command_done) { c->pending=false; cl_finish(c); }'),
    ('the-timeout-halves-swap', 'c->timeout_usb_done=usb_done; c->timeout_reply=reply;', 'c->timeout_usb_done=reply; c->timeout_reply=usb_done;'),
    ('an-expired-call-is-rejected-late', '        cl_answers_clear(c);\n        cl_fail(c,"an answer', '        cl_fail(c,"an answer'),
    ('refusals-never-expire', '        if(now<c->refusal[i].since+CL_ANSWER_MS) c->refusal[keep++]=c->refusal[i];',
     '        if(1) c->refusal[keep++]=c->refusal[i];'),
    ('a-quiet-caller-is-waited-for', '{ c->phase=BT_CLASSIC_AUTHENTICATING; c->next_op=0x0411; }\n        else if(c->phase==BT_CLASSIC_CLOSING)',
     '{ c->phase=BT_CLASSIC_AUTHENTICATING; c->next_op=0; }\n        else if(c->phase==BT_CLASSIC_CLOSING)'),
    ('sent-without-control', '    if(c->pending || !control || gave_up || c->no_credit) return;', '    if(c->pending || gave_up || c->no_credit) return;'),
    ('a-gave-up-tick-sends', '    if(c->pending || !control || gave_up || c->no_credit) return;', '    if(c->pending || !control || (gave_up && 0) || c->no_credit) return;'),
    ('sent-without-a-credit', '    if(c->pending || !control || gave_up || c->no_credit) return;', '    if(c->pending || !control || gave_up) return;'),
    ('refusals-go-first', '    if(k<BT_CLASSIC_ANSWERS) {\n        c->answer[k].set=false;',
     '    if(k<BT_CLASSIC_ANSWERS && !c->refusal_count) {\n        c->answer[k].set=false;'),
    ('answers-in-reverse', '    while(k<BT_CLASSIC_ANSWERS && !c->answer[k].set) k++;',
     '    k=BT_CLASSIC_ANSWERS-1; while(k>0 && !c->answer[k].set) k--; if(!c->answer[k].set) k=BT_CLASSIC_ANSWERS;'),
    ('accept-takes-the-central-role', 'op=0x0409; p[len++]=0x01;', 'op=0x0409; p[len++]=0x00;'),
    ('any-bond-answers-any-peer', '            if(c->bond.valid && cl_equal(c->bond.peer,c->peer,6)) {', '            if(c->bond.valid) {'),
    ('the-key-given-is-not-noted', 'len=22; c->key_given=true;', 'len=22;'),
    ('a-session-asks-to-bond', 'c->session==BT_CLASSIC_SESSION_CONNECT ? 0x00 : 0x04;', 'c->session==BT_CLASSIC_SESSION_CONNECT ? 0x04 : 0x04;'),
    ('we-have-a-display', 'op=0x042b; p[len++]=0x03; p[len++]=0x00;', 'op=0x042b; p[len++]=0x01; p[len++]=0x00;'),
    ('pairing-refusal-has-no-reason', 'if(op==0x040a || op==0x042a || op==0x0434) p[len++]=r->reason;', 'if(op==0x040a || op==0x042a) p[len++]=r->reason;'),
    ('the-queue-does-not-move', '        for(unsigned i=0;i<c->refusal_count;i++) c->refusal[i]=c->refusal[i+1];\n', ''),
    ('refusals-are-link-commands', '        link=false;\n        c->refusal_count--;', '        c->refusal_count--;'),
    ('sco-packets-are-asked-for', 'cl_put(p+6,0xcc18);', 'cl_put(p+6,0xcc1e);'),
    ('repetition-mode-r1', 'p[8]=0x02; p[9]=0;', 'p[8]=0x01; p[9]=0;'),
    ('no-role-switch', 'cl_put(p+10,0); p[12]=1; len=13;', 'cl_put(p+10,0); p[12]=0; len=13;'),
    ('every-disconnect-is-polite', 'p[2]=c->close_reason; len=3;', 'p[2]=0x13; len=3;'),
    ('encryption-off', 'cl_put(p,c->handle); p[2]=1; len=3;', 'cl_put(p,c->handle); p[2]=0; len=3;'),
    ('refusals-are-not-urgent', '    if(c->refusal_count) return true;\n', ''),
    ('quiescent-with-a-write-owed', '!bt_classic_urgent(c) && !cl_scan_owed(c);', '!bt_classic_urgent(c);'),

    # Sniff: Write Link Policy Settings and Exit Sniff Mode, which can never
    # end the link (AUDIO_REMOTE.md § Part Z).
    ('a-refused-sniff-command-ends-the-link', '    if(cl_sniff_op(op)) { cl_sniff_done(c,op,st); return; }\n', ''),
    ('an-unanswered-sniff-command-ends-the-link', '    if(cl_sniff_op(op)) { cl_sniff_done(c,op,0xff); return; }\n', ''),
    ('the-policy-reply-is-read-as-an-address', '    case 0x080d: return CL_HANDLE;\n', ''),
    ('exit-sniff-is-not-asynchronous', '    case 0x0804:\n        return CL_ASYNC;', '        return CL_ASYNC;'),
    ('the-policy-reply-is-a-byte-long', 'kind==CL_HANDLE ? 8 : 6', 'kind==CL_HANDLE ? 9 : 6'),
    ('the-policy-reply-handle-is-unchecked',
     '        if(kind==CL_HANDLE && cl_u16(e+6)!=c->command_handle) { c->malformed++; return true; }\n', ''),
    ('allowing-drops-role-switch', 'c->sniff_wanted ? 0x0005 : 0x0001', 'c->sniff_wanted ? 0x0004 : 0x0001'),
    ('refusing-drops-role-switch', 'c->sniff_wanted ? 0x0005 : 0x0001', 'c->sniff_wanted ? 0x0005 : 0x0000'),
    ('allowing-is-hold', 'c->sniff_wanted ? 0x0005 : 0x0001', 'c->sniff_wanted ? 0x0003 : 0x0001'),
    ('exit-sniff-carries-a-reason', "op=0x0804; cl_put(p,c->handle); len=2;", "op=0x0804; cl_put(p,c->handle); p[2]=0x13; len=3;"),
    ('sniff-is-asked-before-ready', '{ return c->phase==BT_CLASSIC_READY && c->connected; }', '{ return c->connected; }'),
    ('the-policy-goes-ahead-of-a-latched-answer',
     '    while(k<BT_CLASSIC_ANSWERS && !c->answer[k].set) k++;',
     '    while(k<BT_CLASSIC_ANSWERS && !c->answer[k].set) k++;\n    if(c->policy_owed) k=BT_CLASSIC_ANSWERS;'),
    ('the-policy-goes-ahead-of-next-op', '    } else if(c->next_op) {', '    } else if(c->next_op && !c->policy_owed) {'),
    ('exit-sniff-goes-ahead-of-the-policy', '    } else if(c->policy_owed && cl_link_up(c)) {',
     '    } else if(c->policy_owed && cl_link_up(c) && !c->wake_owed) {'),
    ('page-scan-goes-ahead-of-exit-sniff', '    } else if(c->wake_owed && cl_link_up(c)) {',
     '    } else if(c->wake_owed && cl_link_up(c) && !cl_scan_owed(c)) {'),
    ('a-drop-keeps-the-sniff-state', '    cl_key_wipe(c);\n    cl_sniff_clear(c);\n', '    cl_key_wipe(c);\n'),
    ('a-failed-transport-keeps-the-sniff-state', 'c->refusal_count=0; cl_key_wipe(c); cl_sniff_clear(c);',
     'c->refusal_count=0; cl_key_wipe(c);'),
    ('a-refused-policy-write-is-confirmed', 'if(c->policy_out && same && !st) c->sniff_confirmed',
     'if(c->policy_out && same) c->sniff_confirmed'),
    ('a-late-answer-is-confirmed', 'if(c->policy_out && same && !st) c->sniff_confirmed',
     'if(c->policy_out && !st) c->sniff_confirmed'),
    ('the-policy-out-flag-stays', '    c->policy_out=false;\n    // A change asked', '    // A change asked'),
    ('a-change-while-out-is-forgotten', 'c->policy_owed=c->policy_owed && c->sniff_wanted!=c->sniff_confirmed;',
     'c->policy_owed=false;'),
    ('a-change-while-out-is-written-regardless', 'c->policy_owed=c->policy_owed && c->sniff_wanted!=c->sniff_confirmed;',
     'c->policy_owed=c->policy_owed;'),
    ('the-same-value-asks-again', '    if(!cl_link_up(c) || sniff==c->sniff_wanted) return;', '    if(!cl_link_up(c)) return;'),
    ('the-write-out-is-ignored', 'c->policy_owed=sniff!=(c->policy_out ? c->policy_out_value : c->sniff_confirmed);',
     'c->policy_owed=sniff!=c->sniff_confirmed;'),
    ('a-wake-is-owed-while-one-is-out', '    if(cl_link_up(c) && !c->wake_out) c->wake_owed=true;',
     '    if(cl_link_up(c)) c->wake_owed=true;'),
    ('the-wake-flag-stays', '        c->wake_status=st; c->wakes_failed+=st!=0; c->wake_out=false;',
     '        c->wake_status=st; c->wakes_failed+=st!=0;'),
    ('a-failed-wake-is-not-counted', 'c->wakes_failed+=st!=0;', 'c->wakes_failed+=0;'),
    ('a-wake-status-is-not-kept', '        c->wake_status=st; c->wakes_failed', '        c->wakes_failed'),
    ('a-failed-write-is-not-counted', 'c->policy_failed+=st!=0;', 'c->policy_failed+=0;'),
    ('a-write-status-is-not-kept', '    c->policy_status=st; c->policy_failed', '    c->policy_failed'),
    ('writes-are-not-counted', 'c->policy_out_value=c->sniff_wanted; c->policy_writes++;', 'c->policy_out_value=c->sniff_wanted;'),
    ('wakes-are-not-counted', 'c->wake_out=true; c->wakes++;', 'c->wake_out=true;'),
    ('a-failed-mode-change-is-dropped', '        if(e[2]) { c->mode_failed++; c->mode_fail_status=e[2]; }',
     '        if(e[2]) { }'),
    ('a-failed-mode-change-is-taken', '        if(e[2]) { c->mode_failed++; c->mode_fail_status=e[2]; }\n        else {',
     '        if(0) { }\n        else {'),
    ('a-mode-change-for-anyone', '        if(!cl_our_handle(c,e+3)) return true;\n        // One that failed', '        // One that failed'),
    ('hold-reads-as-sniff', '    case 1: return BT_CLASSIC_MODE_HOLD;', '    case 1: return BT_CLASSIC_MODE_SNIFF;'),
    ('a-dead-link-has-a-mode', '    if(!c->connected || !c->mode_known) return BT_CLASSIC_MODE_UNKNOWN;',
     '    if(!c->mode_known) return BT_CLASSIC_MODE_UNKNOWN;'),

    # The bond and its codec.
    ('a-bond-that-is-not-valid-is-kept', '    if(bond && bond->valid) {', '    if(bond) {'),
    ('crc-of-the-wrong-polynomial', '0xedb88320u', '0x04c11db7u'),
    ('encode-leaves-old-bytes', '    cl_wipe(out,BT_CLASSIC_BOND_RECORD_BYTES);\n', ''),
    ('the-key-is-a-byte-early', 'cl_copy(out+24,b->key,16);', 'cl_copy(out+23,b->key,16);'),
    ('decode-leaves-old-bytes', '    cl_wipe(b,sizeof(*b));\n    if(n!=', '    if(n!='),
    ('any-magic', '    if(n!=BT_CLASSIC_BOND_RECORD_BYTES || !cl_equal(p,(const uint8_t *)"OS64BC01",8)) return false;',
     '    if(n!=BT_CLASSIC_BOND_RECORD_BYTES) return false;'),
    ('no-check', '    for(unsigned i=0;i<4;i++) if(p[76+i]!=(uint8_t)(crc>>(8*i))) return false;\n',
     '    for(unsigned i=0;i<4;i++) if(0 && p[76+i]!=(uint8_t)(crc>>(8*i))) return false;\n'),
    ('the-check-misses-a-byte', '    uint32_t crc=cl_crc(p,76);', '    uint32_t crc=cl_crc(p,75);'),
    ('flag-two-is-valid', '    if(p[8]>1 || p[10]', '    if(p[8]>2 || p[10]'),
    ('padding-byte-eleven', '|| p[10] || p[11] ||', '|| p[10] ||'),
    ('padding-byte-seventy-five', '|| p[74] || p[75]) return false;', '|| p[74]) return false;'),
    ('an-empty-record-carries-anything', '        for(unsigned i=8;i<76;i++) if(p[i]) return false;\n', ''),
    ('a-stored-name-keeps-delete', 'if(p[40+end]<32 || p[40+end]>126) return false;', 'if(p[40+end]<32 || p[40+end]>127) return false;'),
    ('a-stored-name-keeps-unit-separator', 'if(p[40+end]<32 || p[40+end]>126) return false;', 'if(p[40+end]<31 || p[40+end]>126) return false;'),
    ('a-name-without-its-end', '    if(end==32) return false;\n', ''),
    ('bytes-after-the-name', '    for(unsigned i=end;i<32;i++) if(p[40+i]) return false;\n', ''),
]

caught, missed, unbuilt = [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / CLASSIC
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {CLASSIC}', file=sys.stderr)
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
            print(f'{name}: {CLASSIC} WAS NOT RESTORED', file=sys.stderr)
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

print(f'\nbt_classic mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
