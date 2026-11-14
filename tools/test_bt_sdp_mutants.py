#!/usr/bin/env python3
"""One deliberate break at a time to SDP (kernel/src/driver/system/usb/bt_sdp.c:
the data-element grammar, the UUID base comparison, the bounded walker and
its depth, the client's continuation and its three bounds, the responder's
three request kinds, its error codes and the continuation state it issues,
the byte order of each field), against tools/test_bt_sdp_host.sh: a mutant
is CAUGHT when the suite fails, MISSED when it passes, which means a test is
missing and gets written. A build failure is neither.

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
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/bt-sdp-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

SDP = 'kernel/src/driver/system/usb/bt_sdp.c'
CASES = 'bash tools/test_bt_sdp_host.sh 300'

mutants = [
    # Byte order and constants.
    ('fields-are-read-little-endian', 'static uint16_t sdp_u16(const uint8_t *p) { return (uint16_t)(p[0]<<8 | p[1]); }',
     'static uint16_t sdp_u16(const uint8_t *p) { return (uint16_t)(p[1]<<8 | p[0]); }'),
    ('fields-are-written-little-endian', 'static void sdp_put16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }',
     'static void sdp_put16(uint8_t *p, uint16_t v) { p[1]=(uint8_t)(v>>8); p[0]=(uint8_t)v; }'),
    ('a-u32-is-read-little-endian', 'return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }',
     'return (uint32_t)p[3]<<24 | (uint32_t)p[2]<<16 | (uint32_t)p[1]<<8 | p[0]; }'),
    ('element-lengths-are-little-endian', '        for(size_t i=0;i<bytes;i++) size=size<<8 | p[at+1+i];',
     '        for(size_t i=0;i<bytes;i++) size=size | (size_t)p[at+1+i]<<(8*i);'),
    ('the-least-maximum-is-6', '#define SDP_MIN_ATTR_BYTES 7', '#define SDP_MIN_ATTR_BYTES 6'),
    ('a-pattern-holds-13', '#define SDP_MAX_PATTERN    12', '#define SDP_MAX_PATTERN    13'),
    # The grammar.
    ('nil-and-bool-take-a-size', '    case T_NIL: case T_BOOL: ok=index==0; break;', '    case T_NIL: case T_BOOL: ok=index<=1; break;'),
    ('an-integer-takes-a-length', '    case T_UINT: case T_INT: ok=index<=4; break;', '    case T_UINT: case T_INT: ok=index<=5; break;'),
    ('a-uuid-of-8-bytes', '    case T_UUID: ok=index==1 || index==2 || index==4; break;', '    case T_UUID: ok=index>=1 && index<=4; break;'),
    ('a-string-of-fixed-size', '    case T_TEXT: case T_SEQ: case T_ALT: case T_URL: ok=index>=5; break;',
     '    case T_TEXT: case T_SEQ: case T_ALT: case T_URL: ok=index>=4; break;'),
    ('reserved-types-are-read', '    default: ok=false; break;', '    default: ok=true; break;'),
    ('nil-has-a-byte', '    if(index<5) size=type==T_NIL ? 0 : (size_t)1<<index;', '    if(index<5) size=(size_t)1<<index;'),
    ('a-length-may-run-past', '        if(lim-at-1<bytes) return EL_SHORT;', '        if(lim-at<bytes) return EL_SHORT;'),
    ('a-whole-element-may-run-past', '{ return sdp_header(p,at,lim,e)==EL_OK && e->size<=lim-e->data; }', '{ return sdp_header(p,at,lim,e)==EL_OK; }'),
    # UUIDs.
    ('a-32-bit-uuid-ignores-its-top', '    if(e->size==4) return v[0] || v[1] ? SDP_NOT_16 : sdp_u16(v+2);', '    if(e->size==4) return sdp_u16(v+2);'),
    ('a-128-bit-uuid-ignores-its-top', '    if(v[0] || v[1]) return SDP_NOT_16;\n    for', '    for'),
    ('the-base-is-compared-short', '    for(unsigned i=0;i<12;i++) if(v[4+i]!=base[i]) return SDP_NOT_16;',
     '    for(unsigned i=0;i<11;i++) if(v[4+i]!=base[i]) return SDP_NOT_16;'),
    ('the-base-is-wrong', '0x5f,0x9b,0x34,0xfb};', '0x5f,0x9b,0x34,0xfa};'),
    ('a-128-bit-uuid-reads-its-first-half', '    return sdp_u16(v+2);\n}', '    return sdp_u16(v);\n}'),
    # The walker.
    ('containers-never-close', '        while(depth && at==end[depth-1]) depth--;', '        while(depth && at>end[depth-1]) depth--;'),
    ('open-containers-at-the-end-are-whole', '            if(at>bytes || depth || !outer_seen) verdict=BT_SDP_TRUNCATED;',
     '            if(at>bytes || !outer_seen) verdict=BT_SDP_TRUNCATED;'),
    ('nothing-is-a-list', '            if(at>bytes || depth || !outer_seen) verdict=BT_SDP_TRUNCATED;',
     '            if(at>bytes || depth) verdict=BT_SDP_TRUNCATED;'),
    ('bytes-after-the-list-are-read', '        if(!depth && outer_seen) { verdict=BT_SDP_MALFORMED; break; }   // bytes after the list\n', ''),
    ('a-whole-answer-may-claim-more', '        size_t lim=depth ? end[depth-1] : complete ? bytes : (size_t)-1;',
     '        size_t lim=depth ? end[depth-1] : (size_t)-1;'),
    ('a-header-across-its-container-is-a-cut', '        if(r==EL_BAD || (r==EL_SHORT && avail==lim)) { verdict=BT_SDP_MALFORMED; break; }',
     '        if(r==EL_BAD) { verdict=BT_SDP_MALFORMED; break; }'),
    ('an-element-may-pass-its-container', '        if(e.size>lim-e.data) { verdict=BT_SDP_MALFORMED; break; }     // past its container\n', ''),
    ('a-cut-element-is-read', '        if(!container && e.size>bytes-e.data) { verdict=BT_SDP_TRUNCATED; break; }\n', ''),
    ('the-list-may-be-anything', '        if(depth==0 && e.type!=T_SEQ) { verdict=BT_SDP_MALFORMED; break; }', '        if(0 && depth==0 && e.type!=T_SEQ) { verdict=BT_SDP_MALFORMED; break; }'),
    ('a-record-may-be-anything', '            if(e.type!=T_SEQ) { verdict=BT_SDP_MALFORMED; break; }\n            sdp_commit',
     '            sdp_commit'),
    ('a-record-is-not-committed', '            sdp_commit(out,&rec);\n            rec=(sdp_record_t){0};', '            rec=(sdp_record_t){0};'),
    ('records-run-together', '            sdp_commit(out,&rec);\n            rec=(sdp_record_t){0};', '            sdp_commit(out,&rec);'),
    ('values-are-ids', '        if(depth==2 && !(index&1)) {', '        if(depth==2 && (index&1)) {'),
    ('an-id-may-be-anything', '            if(!sdp_is_u16(&e)) { verdict=BT_SDP_MALFORMED; break; }\n', ''),
    ('features-are-another-attribute', '        } else if(depth==2 && attribute==0x0311 && sdp_is_u16(&e)) {',
     '        } else if(depth==2 && attribute==0x0312 && sdp_is_u16(&e)) {'),
    ('the-class-is-any-attribute', '            if(attribute==0x0001 && depth==3 && u==wanted) rec.match=true;',
     '            if(depth==3 && u==wanted) rec.match=true;'),
    ('the-class-is-always-the-sink', '            if(attribute==0x0001 && depth==3 && u==wanted) rec.match=true;',
     '            if(attribute==0x0001 && depth==3 && u==(BT_SDP_UUID_AUDIO_SINK|(wanted&0))) rec.match=true;'),
    ('the-psm-is-avdtp-s-entry', '            if(attribute==0x0004 && first[depth-1]==0x0100 && !rec.psm) rec.psm=v;',
     '            if(attribute==0x0004 && first[depth-1]==0x0019 && !rec.psm) rec.psm=v;'),
    ('the-avdtp-version-is-l2cap-s-entry', '            if(attribute==0x0004 && first[depth-1]==0x0019 && !rec.avdtp) rec.avdtp=v;',
     '            if(attribute==0x0004 && first[depth-1]==0x0100 && !rec.avdtp) rec.avdtp=v;'),
    ('the-profile-is-in-the-protocols', '            if(attribute==0x0009 && first[depth-1]==0x110D && !rec.a2dp) rec.a2dp=v;',
     '            if(attribute==0x0004 && first[depth-1]==0x110D && !rec.a2dp) rec.a2dp=v;'),
    ('a-container-keeps-the-count', '            end[depth]=e.data+e.size; count[depth]=0; first[depth]=SDP_NOT_16;',
     '            end[depth]=e.data+e.size; first[depth]=SDP_NOT_16;'),
    ('the-depth-is-five', '        if(container && depth<BT_SDP_DEPTH) {', '        if(container && depth<BT_SDP_DEPTH-1) {'),
    ('the-depth-is-seven', '        if(container && depth<BT_SDP_DEPTH) {', '        if(container && depth<=BT_SDP_DEPTH) {'),
    ('the-list-is-never-seen', '            if(depth==1) outer_seen=true;\n', ''),
    ('the-last-record-is-not-committed', '    sdp_commit(out,&rec);\n    if(verdict', '    if(verdict'),
    ('a-malformed-prefix-keeps-its-findings', '    if(verdict==BT_SDP_MALFORMED || (complete && verdict!=BT_SDP_PARSED)) sdp_wipe(out,sizeof(*out));',
     '    if(complete && verdict!=BT_SDP_PARSED) sdp_wipe(out,sizeof(*out));'),
    # The client's request.
    ('the-maximum-forgets-the-state', '    c->max_bytes=mtu>24+SDP_MIN_ATTR_BYTES ? (uint16_t)(mtu-24) : SDP_MIN_ATTR_BYTES;',
     '    c->max_bytes=mtu>24+SDP_MIN_ATTR_BYTES ? (uint16_t)(mtu-7) : SDP_MIN_ATTR_BYTES;'),
    ('the-maximum-has-no-floor', '    c->max_bytes=mtu>24+SDP_MIN_ATTR_BYTES ? (uint16_t)(mtu-24) : SDP_MIN_ATTR_BYTES;',
     '    c->max_bytes=mtu>24 ? (uint16_t)(mtu-24) : SDP_MIN_ATTR_BYTES;'),
    ('the-clock-starts-at-zero', '    c->state=BT_SDP_ASKING; c->started=c->now=now; c->owed=true;', '    c->state=BT_SDP_ASKING; c->now=now; c->owed=true;'),
    ('the-request-overruns-its-room', '    if(c->state!=BT_SDP_ASKING || !c->owed || cap<n) return 0;', '    if(c->state!=BT_SDP_ASKING || !c->owed || cap+1<n) return 0;'),
    ('requests-are-not-owed', '    if(c->state!=BT_SDP_ASKING || !c->owed || cap<n) return 0;', '    if(c->state!=BT_SDP_ASKING || cap<n) return 0;'),
    ('the-transaction-never-moves', '    c->tid++;\n', ''),
    ('the-request-length-is-the-pdu', '    out[0]=SDP_SEARCH_ATTR_REQ; sdp_put16(out+1,c->tid); sdp_put16(out+3,(uint16_t)(n-5));',
     '    out[0]=SDP_SEARCH_ATTR_REQ; sdp_put16(out+1,c->tid); sdp_put16(out+3,(uint16_t)n);'),
    ('the-range-stops-short', '    static const uint8_t range[7]={0x35,0x05,0x0a,0x00,0x00,0xff,0xff};',
     '    static const uint8_t range[7]={0x35,0x05,0x0a,0x00,0x00,0x0f,0xff};'),
    ('the-state-is-not-handed-back', '    out[19]=c->cont_len;', '    out[19]=0;'),
    ('a-written-request-is-still-owed', '    c->owed=false; c->awaiting=true;', '    c->awaiting=true;'),
    # The client's answer.
    ('a-finished-query-reads-on', '    if(c->state!=BT_SDP_ASKING) return;\n    if(!c->awaiting)', '    if(!c->awaiting)'),
    ('an-unasked-response-is-taken', '    if(!c->awaiting) { sdp_fail(c,"a response nobody asked for",false); return; }\n', ''),
    ('the-pdu-length-is-not-checked', '    if(n<5 || sdp_u16(p+3)!=n-5) { sdp_fail(c,"a PDU whose length is wrong",true); return; }',
     '    if(n<5) { sdp_fail(c,"a PDU whose length is wrong",true); return; }'),
    ('any-transaction-is-ours', '    if(sdp_u16(p+1)!=c->tid) {', '    if(0 && sdp_u16(p+1)!=c->tid) {'),
    ('an-error-is-another-kind', '    if(p[0]==SDP_ERROR_RSP) {', '    if(p[0]==0x00) {'),
    ('the-error-code-is-lost', '        c->error=n>=7 ? sdp_u16(p+5) : 0;', '        c->error=0;'),
    ('any-response-is-an-answer', '    if(p[0]!=SDP_SEARCH_ATTR_RSP) {', '    if(p[0]==0x08) {'),
    ('a-response-state-of-17', '    if(cont>BT_SDP_CONTINUATION || plen!=3u+count+cont)', '    if(cont>BT_SDP_CONTINUATION+1 || plen!=3u+count+cont)'),
    ('trailing-bytes-are-taken', '    if(cont>BT_SDP_CONTINUATION || plen!=3u+count+cont)', '    if(cont>BT_SDP_CONTINUATION || plen<3u+count+cont)'),
    ('responses-are-not-counted', '    c->rounds++;\n', ''),
    ('the-buffer-overflows', '    size_t room=BT_SDP_BUFFER-c->held, take=count<room ? count : room;', '    size_t room=BT_SDP_BUFFER-c->held, take=count+0*room;'),
    ('an-overflow-is-complete', '    if(take<count) { sdp_finish(c,false,', '    if(take<count) { sdp_finish(c,true,'),
    ('the-last-part-is-not-the-end', '    if(!cont) { sdp_finish(c,true,"answered"); return; }', '    if(!cont && 0) { sdp_finish(c,true,"answered"); return; }'),
    ('a-seventeenth-response', '    if(c->rounds>=BT_SDP_ROUNDS) {', '    if(c->rounds>BT_SDP_ROUNDS) {'),
    ('the-round-bound-beats-the-end', '    if(!cont) { sdp_finish(c,true,"answered"); return; }\n    if(c->rounds>=BT_SDP_ROUNDS)',
     '    if(c->rounds>=BT_SDP_ROUNDS) { sdp_finish(c,false,"x"); return; }\n    if(!cont) { sdp_finish(c,true,"answered"); return; }\n    if(c->rounds>=BT_SDP_ROUNDS)'),
    ('the-state-is-not-kept', '    c->cont_len=cont;\n', ''),
    ('the-state-is-copied-a-byte-early', '    sdp_copy(c->cont,q+3+count,cont);', '    sdp_copy(c->cont,q+2+count,cont);'),
    ('five-seconds-is-not-enough', '    if(c->state==BT_SDP_ASKING && now-c->started>=BT_SDP_LIMIT_MS)', '    if(c->state==BT_SDP_ASKING && now-c->started>BT_SDP_LIMIT_MS)'),
    ('a-cut-answer-is-complete', '    c->result.complete=complete;', '    c->result.complete=true;'),
    ('a-whole-cut-answer-is-taken', '    if(complete && r!=BT_SDP_PARSED) { sdp_fail(', '    if(complete && r==BT_SDP_MALFORMED) { sdp_fail('),
    # The record.
    ('the-record-says-avdtp-1-2', '0x35,0x06,0x19,0x00,0x19,0x09,0x01,0x03}},', '0x35,0x06,0x19,0x00,0x19,0x09,0x01,0x02}},'),
    ('the-record-says-sink', '    {0x0001,5,{0x35,0x03,0x19,0x11,0x0a}},', '    {0x0001,5,{0x35,0x03,0x19,0x11,0x0b}},'),
    ('the-record-says-features-3', '    {0x0311,3,{0x09,0x00,0x01}},', '    {0x0311,3,{0x09,0x00,0x03}},'),
    ('the-record-is-searched-flat', '            i=e.type==T_SEQ || e.type==T_ALT ? e.data : e.data+e.size;', '            i=e.data+e.size;'),
    # The responder.
    ('two-bytes-are-answered', '    if(n<3) return 0;', '    if(n<2) return 0;'),
    ('the-request-length-is-not-checked', '    if(n<5 || sdp_u16(req+3)!=n-5) return sdp_error(req,out,cap,SDP_BAD_SIZE);',
     '    if(n<5) return sdp_error(req,out,cap,SDP_BAD_SIZE);'),
    ('a-bad-size-is-0x0006', '#define SDP_BAD_SIZE       0x0004', '#define SDP_BAD_SIZE       0x0006'),
    ('an-unknown-pdu-is-a-bad-size', 'pdu!=SDP_SEARCH_ATTR_REQ) return sdp_error(req,out,cap,SDP_BAD_SYNTAX);',
     'pdu!=SDP_SEARCH_ATTR_REQ) return sdp_error(req,out,cap,SDP_BAD_SIZE);'),
    ('pdu-0x08-is-a-request', '    if(pdu!=SDP_SEARCH_REQ && pdu!=SDP_ATTR_REQ && pdu!=SDP_SEARCH_ATTR_REQ)',
     '    if(pdu!=SDP_SEARCH_REQ && pdu!=SDP_ATTR_REQ && pdu!=SDP_SEARCH_ATTR_REQ && pdu!=0x08)'),
    ('a-missing-state-is-bad-syntax', '    if(n-at<1) return sdp_error(req,out,cap,SDP_BAD_SIZE);', '    if(n-at<1) return sdp_error(req,out,cap,SDP_BAD_SYNTAX);'),
    ('a-state-of-17-is-read', '    if(cont>BT_SDP_CONTINUATION) return sdp_error(req,out,cap,SDP_BAD_SYNTAX);',
     '    if(cont>BT_SDP_CONTINUATION+1) return sdp_error(req,out,cap,SDP_BAD_SYNTAX);'),
    ('trailing-request-bytes-are-taken', '    if(n-at-1!=cont) return sdp_error(req,out,cap,SDP_BAD_SIZE);', '    if(n-at-1<cont) return sdp_error(req,out,cap,SDP_BAD_SIZE);'),
    ('any-handle-is-ours', '    if(pdu==SDP_ATTR_REQ && sdp_u32(req+5)!=BT_SDP_RECORD_HANDLE)', '    if(0 && pdu==SDP_ATTR_REQ && sdp_u32(req+5)!=BT_SDP_RECORD_HANDLE)'),
    ('a-bad-handle-is-bad-syntax', 'return sdp_error(req,out,cap,SDP_BAD_HANDLE);', 'return sdp_error(req,out,cap,SDP_BAD_SYNTAX);'),
    ('a-search-for-none-is-asked', '    if(pdu==SDP_SEARCH_REQ ? !max : max<SDP_MIN_ATTR_BYTES)', '    if(pdu==SDP_SEARCH_REQ ? 0 : max<SDP_MIN_ATTR_BYTES)'),
    ('a-maximum-of-6-is-asked', '    if(pdu==SDP_SEARCH_REQ ? !max : max<SDP_MIN_ATTR_BYTES)', '    if(pdu==SDP_SEARCH_REQ ? !max : max<SDP_MIN_ATTR_BYTES-1)'),
    ('a-search-takes-a-state', '        if(cont) return sdp_error(req,out,cap,SDP_BAD_CONTINUATION);\n', ''),
    ('a-search-always-matches', '        size_t m=match ? 1 : 0, len=5+4+4*m+1;', '        size_t m=1, len=5+4+4*m+1;'),
    ('the-current-count-is-zero', '        sdp_put16(out+5,(uint16_t)m); sdp_put16(out+7,(uint16_t)m);', '        sdp_put16(out+5,(uint16_t)m); sdp_put16(out+7,0);'),
    ('a-search-overruns-its-room', '        if(cap<len) return 0;', '        if(cap+1<len) return 0;'),
    ('a-search-carries-a-state', '        out[len-1]=0;', '        out[len-1]=1;'),
    ('a-pattern-matches-on-one', '        if(u==SDP_NOT_16 || !sdp_record_has(u)) *match=false;', '        if(u==SDP_NOT_16 || (0 && !sdp_record_has(u))) *match=false;'),
    ('a-pattern-may-hold-anything', '        if(!sdp_element(p,i,s.data+s.size,&e) || e.type!=T_UUID || ++n>SDP_MAX_PATTERN) return false;',
     '        if(!sdp_element(p,i,s.data+s.size,&e) || ++n>SDP_MAX_PATTERN) return false;'),
    ('an-empty-pattern-matches', '    return n>0;', '    return true;'),
    ('a-pattern-may-be-an-alternative', '    if(!sdp_element(p,at,lim,&s) || s.type!=T_SEQ) return false;', '    if(!sdp_element(p,at,lim,&s)) return false;'),
    ('an-empty-id-list-is-taken', '    if(!sdp_element(p,at,lim,&s) || s.type!=T_SEQ || !s.size) return false;',
     '    if(!sdp_element(p,at,lim,&s) || s.type!=T_SEQ) return false;'),
    ('a-signed-id-is-taken', '        if(!sdp_element(p,i,s.data+s.size,&e) || e.type!=T_UINT || (e.index!=1 && e.index!=2)) return false;',
     '        if(!sdp_element(p,i,s.data+s.size,&e) || (e.index!=1 && e.index!=2)) return false;'),
    ('a-one-byte-id-is-taken', '        if(!sdp_element(p,i,s.data+s.size,&e) || e.type!=T_UINT || (e.index!=1 && e.index!=2)) return false;',
     '        if(!sdp_element(p,i,s.data+s.size,&e) || e.type!=T_UINT || e.index>2) return false;'),
    ('a-range-is-read-backwards', '        else { lo=sdp_u16(p+e.data); hi=sdp_u16(p+e.data+2); }', '        else { hi=sdp_u16(p+e.data); lo=sdp_u16(p+e.data+2); }'),
    ('a-backward-range-is-taken', '        if(lo>hi) return false;\n', ''),
    ('a-range-leaves-out-its-end', 'if(sdp_record[a].id>=lo && sdp_record[a].id<=hi) wanted[a]=true;', 'if(sdp_record[a].id>=lo && sdp_record[a].id<hi) wanted[a]=true;'),
    ('a-range-leaves-out-its-start', 'if(sdp_record[a].id>=lo && sdp_record[a].id<=hi) wanted[a]=true;', 'if(sdp_record[a].id>lo && sdp_record[a].id<=hi) wanted[a]=true;'),
    ('no-match-answers-the-record', '    if(pdu==SDP_SEARCH_ATTR_REQ && !match) w=sdp_seq(whole,0);', '    if(0 && pdu==SDP_SEARCH_ATTR_REQ && !match) w=sdp_seq(whole,0);'),
    ('the-list-of-lists-is-not-wrapped', '        if(pdu==SDP_SEARCH_ATTR_REQ) o=sdp_seq(whole,inner+2);', '        if(0) o=sdp_seq(whole,inner+2);'),
    ('an-id-is-a-uint32', '            whole[o]=0x09; sdp_put16(whole+o+1,sdp_record[a].id);', '            whole[o]=0x0a; sdp_put16(whole+o+1,sdp_record[a].id);'),
    ('the-check-is-not-checked', '        if(!offset || offset>=w || sdp_u32(s+2)!=sdp_check(key,key_bytes,list,list_bytes,pdu,offset))',
     '        if(!offset || offset>=w)'),
    ('the-check-ignores-the-id-list', '    for(size_t i=0;i<bn;i++) h=(h^b[i])*16777619u;', '    for(size_t i=0;i<bn;i++) h=(h^b[i]^b[i])*16777619u;'),
    ('the-check-ignores-the-offset', '    h=(h^pdu)*16777619u; h=(h^(offset>>8))*16777619u; h=(h^(offset&0xff))*16777619u;',
     '    h=(h^pdu)*16777619u; h=(h^(offset&0))*16777619u;'),
    ('the-room-is-one-short', '    if(cap<5+2+1+SDP_STATE_BYTES+1) return 0;', '    if(cap<5+2+1+SDP_STATE_BYTES) return 0;'),
    ('a-part-passes-the-maximum', '    if(part>max) part=max;\n', ''),
    ('a-part-passes-the-room', '    if(part>room) part=room;', '    if(part>room+65536) part=room;'),
    ('there-is-always-more', '    bool more=offset+part<w;', '    bool more=offset+part<=w;'),
    ('the-attribute-response-is-the-search-s', '    out[0]=pdu==SDP_SEARCH_ATTR_REQ ? SDP_SEARCH_ATTR_RSP : SDP_ATTR_RSP;', '    out[0]=SDP_SEARCH_ATTR_RSP;'),
    ('the-count-is-the-whole', '    sdp_put16(out+5,(uint16_t)part);', '    sdp_put16(out+5,(uint16_t)w);'),
    ('the-state-names-this-part', '        sdp_put16(out+8+part,following);', '        sdp_put16(out+8+part,offset);'),
    ('an-error-swaps-the-transaction', '    out[0]=SDP_ERROR_RSP; out[1]=req[1]; out[2]=req[2]; sdp_put16(out+3,2); sdp_put16(out+5,code);',
     '    out[0]=SDP_ERROR_RSP; out[1]=req[2]; out[2]=req[1]; sdp_put16(out+3,2); sdp_put16(out+5,code);'),
    ('an-error-is-three-bytes', '    out[0]=SDP_ERROR_RSP; out[1]=req[1]; out[2]=req[2]; sdp_put16(out+3,2); sdp_put16(out+5,code);',
     '    out[0]=SDP_ERROR_RSP; out[1]=req[1]; out[2]=req[2]; sdp_put16(out+3,3); sdp_put16(out+5,code);'),
    ('an-error-overruns-its-room', '    if(cap<7) return 0;', '    if(cap<6) return 0;'),
]

caught, missed, unbuilt = [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / SDP
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {SDP}', file=sys.stderr)
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
            print(f'{name}: {SDP} WAS NOT RESTORED', file=sys.stderr)
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

print(f'\nbt_sdp mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
