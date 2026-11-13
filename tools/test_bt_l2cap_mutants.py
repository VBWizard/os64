#!/usr/bin/env python3
"""One deliberate break at a time to Classic L2CAP
(kernel/src/driver/system/usb/bt_l2cap.c: reassembly, the signalling
commands and their result codes, both directions of configuration, the
timers, the outgoing queue, the byte order of each field), against
tools/test_bt_l2cap_host.sh: a mutant is CAUGHT when the suite fails, MISSED
when it passes, which means a test is missing and gets written. A build
failure is neither.

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
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/bt-l2cap-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

L2CAP = 'kernel/src/driver/system/usb/bt_l2cap.c'
CASES = 'bash tools/test_bt_l2cap_host.sh 300'

mutants = [
    # Constants and byte order.
    ('min-mtu-is-47', '#define L2_MIN_MTU     48', '#define L2_MIN_MTU     47'),
    ('default-mtu-is-670', '#define L2_DEFAULT_MTU 672', '#define L2_DEFAULT_MTU 670'),
    ('rtx-is-12s', '#define L2_RTX_MS      10000', '#define L2_RTX_MS      12000'),
    ('security-waits-20s', '#define L2_SECURITY_MS 30000', '#define L2_SECURITY_MS 20000'),
    ('configuration-has-20s', '#define L2_CONFIG_MS   30000', '#define L2_CONFIG_MS   20000'),
    ('ertx-is-30s', '#define L2_ERTX_MS     60000', '#define L2_ERTX_MS     30000'),
    ('five-rounds', '#define L2_ROUNDS      4', '#define L2_ROUNDS      5'),
    ('three-rounds', '#define L2_ROUNDS      4', '#define L2_ROUNDS      3'),
    ('an-oversized-pdu-keeps-five', '#define L2_KEPT        6', '#define L2_KEPT        5'),
    ('fields-are-read-big-endian', 'static uint16_t l2_u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1]<<8); }',
     'static uint16_t l2_u16(const uint8_t *p) { return (uint16_t)(p[1] | p[0]<<8); }'),
    ('fields-are-written-big-endian', 'static void l2_put(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }',
     'static void l2_put(uint8_t *p, uint16_t v) { p[1]=(uint8_t)v; p[0]=(uint8_t)(v>>8); }'),
    ('the-psm-is-read-big-endian', '    uint16_t psm=l2_u16(d), scid=l2_u16(d+2), result, status=0;',
     '    uint16_t psm=(uint16_t)(d[1] | d[0]<<8), scid=l2_u16(d+2), result, status=0;'),
    ('the-frame-length-is-big-endian', '    l2_put(s->pdu,(uint16_t)(4+n)); l2_put(s->pdu+2,L2_CID_SIGNAL);',
     '    s->pdu[1]=(uint8_t)(4+n); s->pdu[0]=0; l2_put(s->pdu+2,L2_CID_SIGNAL);'),
    ('the-command-length-counts-its-header', '    s->pdu[4]=code; s->pdu[5]=ident; l2_put(s->pdu+6,(uint16_t)n);',
     '    s->pdu[4]=code; s->pdu[5]=ident; l2_put(s->pdu+6,(uint16_t)(n+4));'),
    ('the-continuation-flag-is-the-high-byte', '    bool more=!result && (flags & 0x0001);',
     '    bool more=!result && (flags & 0x0100);'),
    # PSMs and identifiers.
    ('a-psm-high-byte-is-not-checked', '{ return (psm & 0x0101)==0x0001; }', '{ return (psm & 0x0001)==0x0001; }'),
    ('any-psm-is-valid', '{ return (psm & 0x0101)==0x0001; }', '{ return psm!=0; }'),
    ('admit-takes-an-invalid-psm', '    if(!l2_psm_valid(psm)) return;\n    for', '    for'),
    ('identifiers-wrap-to-zero', '    l->next_ident=id==255 ? 1 : (uint8_t)(id+1);', '    l->next_ident=(uint8_t)(id+1);'),
    ('cids-wrap-to-zero', '        l->next_cid=cid==0xffff ? L2_CID_DYNAMIC : (uint16_t)(cid+1);',
     '        l->next_cid=(uint16_t)(cid+1);'),
    ('a-cid-in-use-is-taken-again', '    } while(l2_cid_used(l,cid));', '    } while(0 && l2_cid_used(l,cid));'),
    ('a-response-matches-any-state', '        if(ident && l->ch[i].ident==ident && l->ch[i].state==state) return (int)i;',
     '        if(ident && l->ch[i].ident==ident && l->ch[i].state!=(state^state)) return (int)i;'),
    # The queue.
    ('the-purge-keeps-requests', '        if(s->request && s->channel==ch) continue;', '        if(0 && s->request && s->channel==ch) continue;'),
    ('the-purge-takes-answers', '        if(s->request && s->channel==ch) continue;', '        if(s->channel==ch || s->channel==0xff) continue;'),
    ('a-dropped-request-has-no-timer', '        if(request && ch>=0) { l->ch[ch].timer=true;',
     '        if(0 && request && ch>=0) { l->ch[ch].timer=true;'),
    ('a-full-queue-is-not-counted', '        l->queue_full++;\n', ''),
    ('the-queue-is-one-short', '    if(l->sig_count==BT_L2CAP_SIGNALS) {', '    if(l->sig_count==BT_L2CAP_SIGNALS-1) {'),
    ('a-freed-channel-keeps-its-requests', '    l2_sig_purge(l,i);\n    l2_wipe(&l->ch[i],sizeof(l->ch[i]));',
     '    l2_wipe(&l->ch[i],sizeof(l->ch[i]));'),
    ('a-new-request-keeps-older-ones', '    l2_sig_purge(l,i);\n    c->ident=l2_ident(l);', '    c->ident=l2_ident(l);'),
    ('a-new-request-keeps-the-old-timer', '    c->ident=l2_ident(l); c->timer=false;', '    c->ident=l2_ident(l);'),
    ('the-timer-runs-from-the-queueing', '    c->ident=l2_ident(l); c->timer=false;',
     '    c->ident=l2_ident(l); c->timer=true; c->deadline=l->now+L2_RTX_MS;'),
    ('the-timer-never-starts', '            if(c->state!=BT_L2CAP_FREE && c->ident==s.ident && !c->timer) { c->timer=true; c->deadline=now+L2_RTX_MS; }',
     '            if(c->state!=BT_L2CAP_FREE && c->ident==s.ident && !c->timer && 0) { c->timer=true; c->deadline=now+L2_RTX_MS; }'),
    ('not-understood-is-mtu-exceeded', '{ l->malformed++; l2_reject(l,ident,0x0000,0,0,0); }', '{ l->malformed++; l2_reject(l,ident,0x0001,0,0,0); }'),
    ('not-understood-is-not-counted', '{ l->malformed++; l2_reject(l,ident,0x0000,0,0,0); }', '{ l2_reject(l,ident,0x0000,0,0,0); }'),
    # Our requests.
    ('our-request-states-the-fixed-mtu', '    d[4]=0x01; d[5]=2; l2_put(d+6,c->our_mtu);', '    d[4]=0x01; d[5]=2; l2_put(d+6,BT_L2CAP_MTU);'),
    ('our-request-carries-the-flag', '    l2_put(d,c->dcid); l2_put(d+2,0);', '    l2_put(d,c->dcid); l2_put(d+2,1);'),
    ('our-request-names-our-cid', '    l2_put(d,c->dcid); l2_put(d+2,0);', '    l2_put(d,c->cid); l2_put(d+2,0);'),
    ('our-rounds-are-not-counted', '    c->our_rounds++;\n', ''),
    ('a-close-frees-a-connected-channel', '    if(c->state!=BT_L2CAP_CONFIG) { l2_free(l,i); return; }',
     '    if(1) { l2_free(l,i); return; }'),
    ('the-disconnection-cids-are-swapped', '    l2_put(d,c->dcid); l2_put(d+2,c->cid);\n    c->state=BT_L2CAP_DISCONNECTING;',
     '    l2_put(d,c->cid); l2_put(d+2,c->dcid);\n    c->state=BT_L2CAP_DISCONNECTING;'),
    ('a-close-keeps-the-data', '    c->state=BT_L2CAP_DISCONNECTING; c->queued=false;', '    c->state=BT_L2CAP_DISCONNECTING;'),
    # The peer's Connection Request.
    ('a-long-connection-request-is-read', '    if(n!=4) { l2_not_understood(l,ident); return; }\n    uint16_t psm',
     '    if(n<4) { l2_not_understood(l,ident); return; }\n    uint16_t psm'),
    ('a-taken-source-cid-is-not-noticed', '        if(l->ch[k].state!=BT_L2CAP_FREE && l->ch[k].dcid==scid) scid_taken=true;',
     '        if(l->ch[k].state!=BT_L2CAP_FREE && l->ch[k].dcid==scid) scid_taken=false;'),
    ('any-valid-psm-is-admitted', '    if(!l2_psm_valid(psm) || !l2_admitted(l,psm)) result=0x0002;',
     '    if(!l2_psm_valid(psm) || (0 && !l2_admitted(l,psm))) result=0x0002;'),
    ('sdp-waits-for-security', '    else if(psm!=BT_L2CAP_PSM_SDP && !l->encrypted) { result=0x0001; status=0x0001; }',
     '    else if(!l->encrypted) { result=0x0001; status=0x0001; }'),
    ('encryption-is-not-asked', '    else if(psm!=BT_L2CAP_PSM_SDP && !l->encrypted) { result=0x0001; status=0x0001; }',
     '    else if(psm!=BT_L2CAP_PSM_SDP && 0) { result=0x0001; status=0x0001; }'),
    ('security-is-blocked-at-once', '    else if(psm!=BT_L2CAP_PSM_SDP && !l->encrypted) { result=0x0001; status=0x0001; }',
     '    else if(psm!=BT_L2CAP_PSM_SDP && !l->encrypted) { result=0x0003; status=0x0000; }'),
    ('pending-gives-no-reason', '    else if(psm!=BT_L2CAP_PSM_SDP && !l->encrypted) { result=0x0001; status=0x0001; }',
     '    else if(psm!=BT_L2CAP_PSM_SDP && !l->encrypted) { result=0x0001; status=0x0000; }'),
    ('security-comes-before-the-source-cid', '    else if(scid<L2_CID_DYNAMIC) result=0x0006;',
     '    else if(psm!=BT_L2CAP_PSM_SDP && !l->encrypted && (i=l2_free_channel(l))>=0) { result=0x0001; status=0x0001; }\n    else if(scid<L2_CID_DYNAMIC) result=0x0006;'),
    ('the-request-identifier-is-forgotten', '            c->state=BT_L2CAP_AWAITING_SECURITY; c->peer_ident=ident;',
     '            c->state=BT_L2CAP_AWAITING_SECURITY; c->peer_ident=1;'),
    ('security-is-awaited-no-time', '            c->security_deadline=l->now+L2_SECURITY_MS;', '            c->security_deadline=l->now;'),
    ('pending-names-no-cid', '    l2_put(r,cid); l2_put(r+2,scid); l2_put(r+4,result); l2_put(r+6,status);',
     '    l2_put(r,result ? 0 : cid); l2_put(r+2,scid); l2_put(r+4,result); l2_put(r+6,status);'),
    ('a-pending-request-holds-no-channel', '    if(result<=0x0001) {\n        bt_l2cap_channel_t *c=&l->ch[i];',
     '    if(result==0x0000) {\n        bt_l2cap_channel_t *c=&l->ch[i];'),
    ('encryption-sends-no-answer', '        l2_answer_waiting(l,i,0x0000);\n        l2_enter_config(l,i);', '        l2_enter_config(l,i);'),
    ('encryption-sends-no-request', '        l2_answer_waiting(l,i,0x0000);\n        l2_enter_config(l,i);',
     '        l2_answer_waiting(l,i,0x0000);\n        l->ch[i].state=BT_L2CAP_CONFIG;'),
    ('losing-encryption-answers-the-waiting', '    l->encrypted=on;\n    if(!on) return;\n', '    l->encrypted=on;\n'),
    ('the-deferred-success-names-no-cid', '    l2_put(r,result ? 0 : c->cid); l2_put(r+2,c->dcid);', '    l2_put(r,0); l2_put(r+2,c->dcid);'),
    ('the-deferred-refusal-names-our-cid', '    l2_put(r,result ? 0 : c->cid); l2_put(r+2,c->dcid);', '    l2_put(r,c->cid); l2_put(r+2,c->dcid);'),
    ('the-deferred-answer-takes-a-new-identifier', '    l2_signal(l,-1,L2_CONN_RSP,c->peer_ident,r,sizeof(r));',
     '    l2_signal(l,-1,L2_CONN_RSP,l2_ident(l),r,sizeof(r));'),
    ('security-is-awaited-a-tick-longer', '            if(now>=c->security_deadline) l2_close(l,i);', '            if(now>c->security_deadline) l2_close(l,i);'),
    ('a-waiting-channel-is-freed-silently', '    if(c->state==BT_L2CAP_AWAITING_SECURITY) l2_answer_waiting(l,i,0x0003);\n', ''),
    ('a-waiting-channel-has-no-resources', '    if(c->state==BT_L2CAP_AWAITING_SECURITY) l2_answer_waiting(l,i,0x0003);',
     '    if(c->state==BT_L2CAP_AWAITING_SECURITY) l2_answer_waiting(l,i,0x0004);'),
    ('a-waiting-channel-cannot-be-disconnected', '        if(c->state>=BT_L2CAP_CONFIG && c->cid==dcid && c->dcid==scid) {',
     '        if((c->state==BT_L2CAP_CONFIG || c->state==BT_L2CAP_DISCONNECTING) && c->cid==dcid && c->dcid==scid) {'),
    ('source-cid-0x3f-is-dynamic', '    else if(scid<L2_CID_DYNAMIC) result=0x0006;', '    else if(scid<0x003f) result=0x0006;'),
    ('no-resources-is-not-supported', '    else if((i=l2_free_channel(l))<0) result=0x0004;', '    else if((i=l2_free_channel(l))<0) result=0x0002;'),
    ('the-connection-response-cids-are-swapped', '    l2_put(r,cid); l2_put(r+2,scid); l2_put(r+4,result); l2_put(r+6,status);',
     '    l2_put(r,scid); l2_put(r+2,cid); l2_put(r+4,result); l2_put(r+6,status);'),
    ('the-status-carries-the-result', '    l2_put(r,cid); l2_put(r+2,scid); l2_put(r+4,result); l2_put(r+6,status);',
     '    l2_put(r,cid); l2_put(r+2,scid); l2_put(r+4,result); l2_put(r+6,result|status);'),
    ('an-accepted-channel-sends-no-request', '    if(!result) l2_enter_config(l,(unsigned)i);\n}', '}'),
    ('an-accepted-channel-states-less', '        c->our_mtu=BT_L2CAP_MTU;\n        if(result) {',
     '        c->our_mtu=600;\n        if(result) {'),
    # The peer's Connection Response.
    ('a-short-connection-response-is-read', '    if(n!=8) { l2_not_understood(l,ident); return; }', '    if(n<6) { l2_not_understood(l,ident); return; }'),
    ('pending-waits-rtx', '    if(result==0x0001) { c->timer=true; c->deadline=l->now+L2_ERTX_MS; return; }',
     '    if(result==0x0001) { c->timer=true; c->deadline=l->now+L2_RTX_MS; return; }'),
    ('pending-waits-forever', '    if(result==0x0001) { c->timer=true; c->deadline=l->now+L2_ERTX_MS; return; }',
     '    if(result==0x0001) { c->timer=false; return; }'),
    ('pending-is-a-refusal', '    if(result==0x0001) { c->timer=true;', '    if(result==0x0101) { c->timer=true;'),
    ('a-fixed-dcid-is-a-channel', '    if(result==0x0000 && dcid>=L2_CID_DYNAMIC) {', '    if(result==0x0000) {'),
    ('a-response-for-another-cid-is-taken', '    if(i<0 || l->ch[i].cid!=scid) {\n        l->strays++;\n        // A success',
     '    if(i<0) {\n        l->strays++;\n        // A success'),
    ('a-late-success-is-left-open', '        if(result==0x0000 && dcid>=L2_CID_DYNAMIC && !l2_cid_used(l,scid)) {',
     '        if(0 && result==0x0000 && dcid>=L2_CID_DYNAMIC && !l2_cid_used(l,scid)) {'),
    ('a-late-pending-is-disconnected', '        if(result==0x0000 && dcid>=L2_CID_DYNAMIC && !l2_cid_used(l,scid)) {',
     '        if(result<=0x0001 && dcid>=L2_CID_DYNAMIC && !l2_cid_used(l,scid)) {'),
    ('a-late-fixed-cid-is-disconnected', '        if(result==0x0000 && dcid>=L2_CID_DYNAMIC && !l2_cid_used(l,scid)) {',
     '        if(result==0x0000 && !l2_cid_used(l,scid)) {'),
    ('a-duplicate-success-disconnects-a-live-channel', '        if(result==0x0000 && dcid>=L2_CID_DYNAMIC && !l2_cid_used(l,scid)) {',
     '        if(result==0x0000 && dcid>=L2_CID_DYNAMIC) {'),
    ('the-late-disconnection-cids-are-swapped', '            l2_put(q,dcid); l2_put(q+2,scid);', '            l2_put(q,scid); l2_put(q+2,dcid);'),
    ('a-success-without-a-cid-reads-as-none', '{ l->malformed++; result=0xfffd; }', '{ l->malformed++; }'),
    ('the-refusal-is-not-kept', '    l->refused=result; l->refused_psm=c->psm;\n    l2_free', '    l->refused_psm=c->psm;\n    l2_free'),
    # The peer's Configuration Request.
    ('a-short-configuration-request-is-read', '    if(n<4) { l2_not_understood(l,ident); return; }\n    uint16_t dcid=l2_u16(d), flags',
     '    if(n<2) { l2_not_understood(l,ident); return; }\n    uint16_t dcid=l2_u16(d), flags'),
    ('configuration-for-a-channel-in-any-state', '        if(l->ch[k].state==BT_L2CAP_CONFIG && l->ch[k].cid==dcid) i=(int)k;',
     '        if(l->ch[k].state && l->ch[k].cid==dcid) i=(int)k;'),
    ('the-invalid-cid-reject-is-short', '    if(i<0) { l2_reject(l,ident,0x0002,dcid,0,2); return; }',
     '    if(i<0) { l2_reject(l,ident,0x0002,dcid,0,1); return; }'),
    ('an-option-may-run-past', '        if(n-at<2 || d[at+1]>n-at-2) { l2_not_understood(l,ident); return; }',
     '        if(n-at<2 || d[at+1]>n-at) { l2_not_understood(l,ident); return; }'),
    ('mtu-47-is-taken', '            if(l2_u16(v)>=L2_MIN_MTU) { mtu=l2_u16(v); break; }', '            if(l2_u16(v)>=47) { mtu=l2_u16(v); break; }'),
    ('mtu-48-is-refused', '            if(l2_u16(v)>=L2_MIN_MTU) { mtu=l2_u16(v); break; }', '            if(l2_u16(v)>L2_MIN_MTU) { mtu=l2_u16(v); break; }'),
    ('the-refusal-carries-the-asked-mtu', 'l2_put(fix+nf+2,L2_MIN_MTU); nf+=4;', 'l2_put(fix+nf+2,l2_u16(v)); nf+=4;'),
    ('a-long-mtu-option-is-read', '        case 0x01:      // MTU\n            if(len!=2)', '        case 0x01:      // MTU\n            if(len<2)'),
    ('a-short-flush-timeout-is-read', "        case 0x02:      // Flush Timeout: a hint about the peer's own sending\n            if(len!=2)",
     "        case 0x02:      // Flush Timeout: a hint about the peer's own sending\n            if(len>2)"),
    ('a-short-qos-is-read', '            if(len!=22)', '            if(len<21)'),
    ('guaranteed-qos-is-taken', '            if(v[1]==0x01) break;', '            if(v[1]<=0x02) break;'),
    ('the-qos-refusal-keeps-the-service-type', 'fix[nf+3]=0x01; nf+=24;', 'nf+=24;'),
    ('a-short-mode-option-is-read', '            if(len!=9)', '            if(len<8)'),
    ('ertm-is-taken', '            if(v[0]==0x00) break;', '            if(v[0]<=0x03) break;'),
    ('the-mode-refusal-keeps-the-mode', 'l2_wipe(fix+nf+2,9); nf+=11;', 'l2_copy(fix+nf+2,v,9); nf+=11;'),
    ('a-long-fcs-option-is-read', '            if(len!=1)', '            if(len<1)'),
    ('a-hinted-option-is-unknown', '            if(type & 0x80) break;\n', ''),
    ('the-hint-bit-hides-a-known-option', '        switch(type & 0x7f) {', '        switch(type) {'),
    ('only-one-unknown-is-listed', '            if(nu<room) unknown[nu++]=type;', '            if(nu<1) unknown[nu++]=type;'),
    ('unacceptable-beats-unknown', '    uint16_t result=any_unknown ? 0x0003 : unacceptable ? 0x0001 : 0x0000;',
     '    uint16_t result=unacceptable ? 0x0001 : any_unknown ? 0x0003 : 0x0000;'),
    ('unacceptable-is-success', '    uint16_t result=any_unknown ? 0x0003 : unacceptable ? 0x0001 : 0x0000;',
     '    uint16_t result=any_unknown ? 0x0003 : 0x0000;'),
    ('a-fifth-request-is-answered', '    if(c->peer_rounds>=L2_ROUNDS) { l2_close(l,(unsigned)i); return; }',
     '    if(c->peer_rounds>L2_ROUNDS) { l2_close(l,(unsigned)i); return; }'),
    ('peer-rounds-are-not-counted', '    c->peer_rounds++;\n', ''),
    ('agreement-keeps-the-count', ' c->peer_done=true; c->peer_rounds=0; }', ' c->peer_done=true; }'),
    ('a-new-configuration-keeps-the-old-mtu', '    if(!c->peer_partial) c->peer_mtu_pending=L2_DEFAULT_MTU;\n', ''),
    ('a-continuation-forgets-the-mtu', '    if(!c->peer_partial) c->peer_mtu_pending=L2_DEFAULT_MTU;', '    c->peer_mtu_pending=L2_DEFAULT_MTU;'),
    ('a-reconfiguration-stays-open', '    c->peer_done=false;\n    uint8_t r[', '    uint8_t r['),
    ('the-response-names-our-cid', '    l2_put(r,c->dcid);\n    uint16_t result', '    l2_put(r,c->cid);\n    uint16_t result'),
    ('the-continuation-flag-is-dropped', '    bool more=!result && (flags & 0x0001);', '    bool more=!result && (flags & 0x0000);'),
    ('a-failure-carries-the-flag', '    bool more=!result && (flags & 0x0001);', '    bool more=(flags & 0x0001);'),
    ('the-mtu-is-not-taken', '        if(mtu) c->peer_mtu_pending=mtu;', '        if(mtu && 0) c->peer_mtu_pending=mtu;'),
    ('a-failure-keeps-gathering', '    } else c->peer_partial=false;', '    }'),
    # The peer's Configuration Response.
    ('a-short-configuration-response-is-read', '    if(n<6) { l2_not_understood(l,ident); return; }\n    uint16_t scid=l2_u16(d), result',
     '    if(n<4) { l2_not_understood(l,ident); return; }\n    uint16_t scid=l2_u16(d), result'),
    ('a-configuration-response-for-another-cid-is-taken', '    if(i<0 || l->ch[i].cid!=scid) { l->strays++; return; }\n    bt_l2cap_channel_t *c=&l->ch[i];\n    c->ident=0;',
     '    if(i<0 || (0 && scid)) { l->strays++; return; }\n    bt_l2cap_channel_t *c=&l->ch[i];\n    c->ident=0;'),
    ('an-answered-request-stays-outstanding', '    c->ident=0; c->timer=false;\n    if(result==0x0000) { c->ours_done=true; return; }',
     '    c->timer=false;\n    if(result==0x0000) { c->ours_done=true; return; }'),
    ('an-answered-request-keeps-its-timer', '    c->ident=0; c->timer=false;\n    if(result==0x0000) { c->ours_done=true; return; }',
     '    c->ident=0;\n    if(result==0x0000) { c->ours_done=true; return; }'),
    ('success-is-not-recorded', '    if(result==0x0000) { c->ours_done=true; return; }', '    if(result==0x0000) { return; }'),
    ('a-proposed-mtu-of-47-is-asked-for', '    if(result==0x0001 && mtu>=L2_MIN_MTU && mtu<=BT_L2CAP_MTU && c->our_rounds<L2_ROUNDS) {',
     '    if(result==0x0001 && mtu>=47 && mtu<=BT_L2CAP_MTU && c->our_rounds<L2_ROUNDS) {'),
    ('a-proposed-mtu-over-ours-is-asked-for', '    if(result==0x0001 && mtu>=L2_MIN_MTU && mtu<=BT_L2CAP_MTU && c->our_rounds<L2_ROUNDS) {',
     '    if(result==0x0001 && mtu>=L2_MIN_MTU && mtu<=BT_L2CAP_MTU+1 && c->our_rounds<L2_ROUNDS) {'),
    ('any-failure-is-asked-again', '    if(result==0x0001 && mtu>=L2_MIN_MTU && mtu<=BT_L2CAP_MTU && c->our_rounds<L2_ROUNDS) {',
     '    if(mtu>=L2_MIN_MTU && mtu<=BT_L2CAP_MTU && c->our_rounds<L2_ROUNDS) {'),
    ('our-requests-never-run-out', '    if(result==0x0001 && mtu>=L2_MIN_MTU && mtu<=BT_L2CAP_MTU && c->our_rounds<L2_ROUNDS) {',
     '    if(result==0x0001 && mtu>=L2_MIN_MTU && mtu<=BT_L2CAP_MTU) {'),
    ('a-response-option-may-run-past', '        if(n-at<2 || d[at+1]>n-at-2) { l->malformed++; mtu=0; break; }',
     '        if(n-at<2) { l->malformed++; mtu=0; break; }'),
    ('the-hint-bit-hides-the-proposed-mtu', '        if((d[at] & 0x7f)==0x01 && d[at+1]==2) mtu=l2_u16(d+at+2);',
     '        if(d[at]==0x01 && d[at+1]==2) mtu=l2_u16(d+at+2);'),
    # Disconnection.
    ('a-short-disconnection-request-is-read', '    if(n!=4) { l2_not_understood(l,ident); return; }\n    uint16_t dcid=l2_u16(d), scid=l2_u16(d+2);',
     '    if(n<2) { l2_not_understood(l,ident); return; }\n    uint16_t dcid=l2_u16(d), scid=l2_u16(d+2);'),
    ('a-disconnection-ignores-the-peer-cid', ' && c->cid==dcid && c->dcid==scid) {', ' && c->cid==dcid) {'),
    ('a-disconnection-ignores-our-cid', ' && c->cid==dcid && c->dcid==scid) {', ' && c->dcid==scid) {'),
    ('a-disconnection-is-not-answered', '            l2_signal(l,-1,L2_DISC_RSP,ident,d,4);\n', ''),
    ('the-invalid-cid-order-is-swapped', '    l2_reject(l,ident,0x0002,dcid,scid,2);', '    l2_reject(l,ident,0x0002,scid,dcid,2);'),
    ('a-long-disconnection-response-is-read', '    if(n!=4) { l2_not_understood(l,ident); return; }\n    int i=l2_by_ident(l,ident,BT_L2CAP_DISCONNECTING);',
     '    if(n<4) { l2_not_understood(l,ident); return; }\n    int i=l2_by_ident(l,ident,BT_L2CAP_DISCONNECTING);'),
    ('a-disconnection-response-for-other-cids-is-taken', '    if(i<0 || l->ch[i].dcid!=l2_u16(d) || l->ch[i].cid!=l2_u16(d+2)) { l->strays++; return; }',
     '    if(i<0 || !d) { l->strays++; return; }'),
    # Information and echo.
    ('a-long-information-request-is-read', '    if(n!=2) { l2_not_understood(l,ident); return; }', '    if(n<2) { l2_not_understood(l,ident); return; }'),
    ('the-features-are-eight-bytes', '    if(type==0x0002) len=8;', '    if(type==0x0002) len=12;'),
    ('the-fixed-channels-include-connectionless', '{ r[4]=0x02; len=12; }', '{ r[4]=0x06; len=12; }'),
    ('the-fixed-channels-are-four-bytes', '{ r[4]=0x02; len=12; }', '{ r[4]=0x02; len=8; }'),
    ('an-unknown-type-is-a-success', '    else l2_put(r+2,0x0001);', '    else l2_put(r+2,0x0000);'),
    ('the-echo-is-unbounded', 'len<BT_L2CAP_ECHO_BYTES ? len : BT_L2CAP_ECHO_BYTES);', 'len);'),
    ('the-echo-is-one-short', 'len<BT_L2CAP_ECHO_BYTES ? len : BT_L2CAP_ECHO_BYTES);', 'len<BT_L2CAP_ECHO_BYTES ? len : BT_L2CAP_ECHO_BYTES-1);'),
    ('a-stray-response-is-answered', '        case L2_ECHO_RSP: case L2_INFO_RSP: l->strays++; break;',
     '        case L2_ECHO_RSP: case L2_INFO_RSP: l2_not_understood(l,ident); break;'),
    # Command Reject.
    ('a-one-byte-reject-is-read', '    if(n<2) { l->malformed++; return; }\n    for', '    if(n<1) { l->malformed++; return; }\n    for'),
    ('a-rejected-configuration-frees', '        if(c->state==BT_L2CAP_CONFIG) l2_close(l,i);\n        else l2_free(l,i);\n        return;',
     '        l2_free(l,i);\n        return;'),
    ('a-rejected-connection-is-not-said', '        if(c->state==BT_L2CAP_CONNECTING) { l->refused=0xfffe;', '        if(0) { l->refused=0xfffe;'),
    ('a-reject-of-identifier-0-matches', '        if(!ident || c->state==BT_L2CAP_FREE || c->ident!=ident) continue;',
     '        if(c->state==BT_L2CAP_FREE || c->ident!=ident) continue;'),
    ('a-stray-reject-is-not-counted', '        return;\n    }\n    l->strays++;\n}', '        return;\n    }\n}'),
    # The C-frame.
    ('trailing-bytes-are-not-counted', '        if(n<4) { l->malformed++; return; }\n        uint8_t code', '        if(n<4) { return; }\n        uint8_t code'),
    ('a-command-may-run-past', '        if(len>n-4) {', '        if(len>n) {'),
    ('a-reject-running-past-is-answered', '            if(code==L2_REJECT) l->malformed++;\n            else l2_not_understood(l,ident);',
     '            l2_not_understood(l,ident);'),
    ('identifier-0-is-taken', '        if(!ident && code!=L2_REJECT) l2_not_understood(l,ident);\n        else switch(code) {',
     '        switch(code) {'),
    # Receive.
    ('an-oversized-signal-is-not-refused', '        if(cid==L2_CID_SIGNAL && l->rx[5]) l2_reject(l,l->rx[5],0x0001,BT_L2CAP_MTU,0,1);\n', ''),
    ('an-oversized-signal-is-not-understood', '        if(cid==L2_CID_SIGNAL && l->rx[5]) l2_reject(l,l->rx[5],0x0001,BT_L2CAP_MTU,0,1);',
     '        if(cid==L2_CID_SIGNAL && l->rx[5]) l2_reject(l,l->rx[5],0x0000,BT_L2CAP_MTU,0,1);'),
    ('an-oversized-signal-is-refused-under-its-code', '        if(cid==L2_CID_SIGNAL && l->rx[5]) l2_reject(l,l->rx[5],0x0001,BT_L2CAP_MTU,0,1);',
     '        if(cid==L2_CID_SIGNAL && l->rx[5]) l2_reject(l,l->rx[4],0x0001,BT_L2CAP_MTU,0,1);'),
    ('an-oversized-pdu-is-not-counted', '        l->oversized++;\n', ''),
    ('connectionless-goes-to-the-channels', '    if(cid<L2_CID_DYNAMIC) { l->fixed_cid++; return; }', '    if(cid<2) { l->fixed_cid++; return; }'),
    ('a-closed-channel-is-delivered', '        if(c->cid!=cid || !l2_open(c)) continue;', '        if(c->cid!=cid) continue;'),
    ('over-our-mtu-is-delivered', '        if(len>c->our_mtu) break;', '        if(len>BT_L2CAP_MTU) break;'),
    ('open-without-ours', '{ return c->state==BT_L2CAP_CONFIG && c->ours_done && c->peer_done; }', '{ return c->state==BT_L2CAP_CONFIG && c->peer_done; }'),
    ('open-without-the-peers', '{ return c->state==BT_L2CAP_CONFIG && c->ours_done && c->peer_done; }', '{ return c->state==BT_L2CAP_CONFIG && c->ours_done; }'),
    ('open-while-disconnecting', '{ return c->state==BT_L2CAP_CONFIG && c->ours_done && c->peer_done; }', '{ return c->ours_done && c->peer_done; }'),
    ('the-acl-length-is-not-checked', '    if(bytes<4 || l2_u16(acl+2)!=bytes-4) { l->malformed++; return; }', '    if(bytes<4) { l->malformed++; return; }'),
    ('broadcast-is-taken', '    if((flags & 0xc) || boundary==3) { l->malformed++; return; }', '    if(boundary==3) { l->malformed++; return; }'),
    ('0b11-is-taken', '    if((flags & 0xc) || boundary==3) { l->malformed++; return; }', '    if((flags & 0xc)) { l->malformed++; return; }'),
    ('a-continuation-is-a-start', '    if(boundary==1) {', '    if(boundary==3) {'),
    ('an-orphan-is-taken', '        if(!l->rx_active) { l->orphans++; return; }', '        if(!l->rx_active) { l->orphans++; }'),
    ('an-unfinished-pdu-is-not-counted', '        if(l->rx_active) l->unfinished++;\n', ''),
    ('a-start-continues-the-old', '        l2_rx_clear(l);\n        l->rx_active=true;', '        l->rx_active=true;'),
    ('the-length-counts-the-header', '                l->rx_total=4u+l2_u16(l->rx);', '                l->rx_total=l2_u16(l->rx);'),
    ('a-whole-buffer-is-oversized', '                l->rx_oversize=l->rx_total>sizeof(l->rx);', '                l->rx_oversize=l->rx_total>=sizeof(l->rx);'),
    ('a-byte-over-the-buffer-is-taken', '                l->rx_oversize=l->rx_total>sizeof(l->rx);', '                l->rx_oversize=l->rx_total>sizeof(l->rx)+1;'),
    ('an-oversized-pdu-is-kept-whole', '                if(!l->rx_oversize || l->rx_have+k<L2_KEPT) l->rx[l->rx_have+k]=p[k];',
     '                if(l->rx_have+k<sizeof(l->rx)+8) l->rx[l->rx_have+k]=p[k];'),
    ('an-overlong-packet-is-delivered', '            if(n) { l->malformed++; l2_rx_clear(l); return; }', '            if(n) { l->malformed++; }'),
    ('a-link-that-is-down-is-read', '    l->now=now;\n    if(!l->up) return;', '    l->now=now;'),
    ('receive-keeps-the-old-clock', '    l->now=now;\n    if(!l->up) return;', '    (void)now;\n    if(!l->up) return;'),
    # The API.
    ('a-reset-keeps-the-channels', '    for(unsigned i=0;i<BT_L2CAP_CHANNELS;i++) l2_wipe(&l->ch[i],sizeof(l->ch[i]));\n    l->sig_head',
     '    l->sig_head'),
    ('a-reset-keeps-the-queue', '    l->sig_head=l->sig_count=0;\n    l2_rx_clear(l);', '    l2_rx_clear(l);'),
    ('a-reset-stays-up', '    l->up=false; l->encrypted=false;', '    l->encrypted=false;'),
    ('a-reset-stays-encrypted', '    l->up=false; l->encrypted=false;', '    l->up=false;'),
    ('a-link-that-is-down-connects', '    if(!l->up || i<0 || !l2_psm_valid(psm)) return -1;', '    if(i<0 || !l2_psm_valid(psm)) return -1;'),
    ('an-invalid-psm-connects', '    if(!l->up || i<0 || !l2_psm_valid(psm)) return -1;', '    if(!l->up || i<0) return -1;'),
    ('a-second-close-asks-again', '       l->ch[channel].state==BT_L2CAP_DISCONNECTING) return;', '       0) return;'),
    ('a-send-may-pass-the-peer-mtu', '    if(c->queued || bytes>c->peer_mtu || bytes>BT_L2CAP_MTU) return false;',
     '    if(c->queued || bytes>BT_L2CAP_MTU) return false;'),
    ('a-send-overwrites-the-slot', '    if(c->queued || bytes>c->peer_mtu || bytes>BT_L2CAP_MTU) return false;',
     '    if(bytes>c->peer_mtu || bytes>BT_L2CAP_MTU) return false;'),
    ('data-goes-to-our-own-cid', '    l2_put(c->out,(uint16_t)bytes); l2_put(c->out+2,c->dcid);', '    l2_put(c->out,(uint16_t)bytes); l2_put(c->out+2,c->cid);'),
    ('a-timer-ends-a-tick-early', '        bool unanswered=c->timer && now>=c->deadline;', '        bool unanswered=c->timer && now+1>=c->deadline;'),
    ('a-timer-ends-a-tick-late', '        bool unanswered=c->timer && now>=c->deadline;', '        bool unanswered=c->timer && now>c->deadline;'),
    ('configuration-has-no-deadline', '        bool unfinished=c->state==BT_L2CAP_CONFIG && !l2_open(c) && now>=c->config_deadline;',
     '        bool unfinished=0 && c->state==BT_L2CAP_CONFIG && !l2_open(c) && now>=c->config_deadline;'),
    ('an-open-channel-has-a-deadline', '        bool unfinished=c->state==BT_L2CAP_CONFIG && !l2_open(c) && now>=c->config_deadline;',
     '        bool unfinished=c->state==BT_L2CAP_CONFIG && now>=c->config_deadline;'),
    ('the-deadline-ends-a-tick-late', '        bool unfinished=c->state==BT_L2CAP_CONFIG && !l2_open(c) && now>=c->config_deadline;',
     '        bool unfinished=c->state==BT_L2CAP_CONFIG && !l2_open(c) && now>c->config_deadline;'),
    ('both-timers-close-twice', '        if(c->state==BT_L2CAP_CONFIG) l2_close(l,i);\n        else l2_free(l,i);\n    }',
     '        if(c->state==BT_L2CAP_CONFIG) { l2_close(l,i); if(unanswered && unfinished) l2_close(l,i); }\n        else l2_free(l,i);\n    }'),
    ('a-reconfiguration-keeps-the-old-deadline', '    if(l2_open(c)) c->config_deadline=l->now+L2_CONFIG_MS;', '    if(0 && l2_open(c)) c->config_deadline=l->now+L2_CONFIG_MS;'),
    ('configuration-is-timed-from-nothing', '    c->config_deadline=l->now+L2_CONFIG_MS;\n    l2_send_config(l,i);', '    c->config_deadline=L2_CONFIG_MS;\n    l2_send_config(l,i);'),
    ('no-answer-is-not-said', '        if(c->state==BT_L2CAP_CONNECTING) { l->refused=0xffff;', '        if(0) { l->refused=0xffff;'),
    ('an-unanswered-configuration-frees', '        if(c->state==BT_L2CAP_CONFIG) l2_close(l,i);\n        else l2_free(l,i);\n    }',
     '        l2_free(l,i);\n    }'),
    ('sending-without-may-send', '    if(!may_send || !send) return;', '    if((0 && !may_send) || !send) return;'),
    ('data-before-signalling', '    if(l->sig_count) {\n        bt_l2cap_signal_t s',
     '    if(l->sig_count && !l->ch[0].queued && !l->ch[1].queued) {\n        bt_l2cap_signal_t s'),
    ('signalling-newest-first', '        bt_l2cap_signal_t s=*l2_sig_at(l,0);', '        bt_l2cap_signal_t s=*l2_sig_at(l,l->sig_count-1);'),
    ('data-newest-first', '(int32_t)(l->ch[i].queued_seq-l->ch[oldest].queued_seq)<0', '(int32_t)(l->ch[i].queued_seq-l->ch[oldest].queued_seq)>0'),
    ('sent-data-stays-queued', '    c->queued=false;\n    send(context,c->out', '    send(context,c->out'),
    ('wants-send-ignores-data', '    for(unsigned i=0;i<BT_L2CAP_CHANNELS;i++) if(l2_sendable(&l->ch[i])) return true;\n', ''),
    ('wants-send-counts-a-held-payload', '    for(unsigned i=0;i<BT_L2CAP_CHANNELS;i++) if(l2_sendable(&l->ch[i])) return true;',
     '    for(unsigned i=0;i<BT_L2CAP_CHANNELS;i++) if(l->ch[i].queued) return true;'),
    # A payload queued before a reconfiguration.
    ('a-held-payload-is-sent', '{ return c->queued && l2_open(c) && c->queued_bytes-4u<=c->peer_mtu; }',
     '{ return c->queued && c->queued_bytes-4u<=c->peer_mtu; }'),
    ('an-outgrown-payload-is-kept', '        if(c->queued && l2_open(c) && c->queued_bytes-4u>c->peer_mtu) { c->queued=false; l->outgrown++; }',
     '        if(0 && c->queued && l2_open(c) && c->queued_bytes-4u>c->peer_mtu) { c->queued=false; l->outgrown++; }'),
    ('a-payload-of-the-new-mtu-is-outgrown', '        if(c->queued && l2_open(c) && c->queued_bytes-4u>c->peer_mtu) {',
     '        if(c->queued && l2_open(c) && c->queued_bytes-4u>=c->peer_mtu) {'),
    ('outgrown-is-not-counted', '{ c->queued=false; l->outgrown++; }', '{ c->queued=false; }'),
    ('outgrown-is-not-shown', '                   (unsigned)l->outgrown);', '                   0u);'),
    ('the-oldest-goes-held-or-not', '        if(l2_sendable(&l->ch[i]) && (oldest<0 ||',
     '        if(l->ch[i].queued && (oldest<0 ||'),
    ('a-held-payload-blocks-the-rest',
     '        if(l2_sendable(&l->ch[i]) && (oldest<0 || (int32_t)(l->ch[i].queued_seq-l->ch[oldest].queued_seq)<0)) oldest=(int)i;\n    if(oldest<0) return;',
     '        if(l->ch[i].queued && (oldest<0 || (int32_t)(l->ch[i].queued_seq-l->ch[oldest].queued_seq)<0)) oldest=(int)i;\n    if(oldest<0 || !l2_sendable(&l->ch[oldest])) return;'),
]

caught, missed, unbuilt = [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / L2CAP
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {L2CAP}', file=sys.stderr)
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
            print(f'{name}: {L2CAP} WAS NOT RESTORED', file=sys.stderr)
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

print(f'\nbt_l2cap mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
