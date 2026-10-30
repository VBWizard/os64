#!/usr/bin/env python3
"""Build a deterministic, instrumented article from a Yonder corpus record.

Original scripts, event handlers and asset loads are removed. Captured linked
CSS is inlined in document order; resource URLs become empty data URLs except
local fragments. Selectors, including broad dependencies, remain in place.
This is a layout workload derived from a capture, not a live-site replay.
"""
import argparse
import hashlib
from html import escape
from html.parser import HTMLParser
import json
from pathlib import Path
import re
from urllib.parse import urljoin
from yonder_records import unescape, unescape_bytes

URL = re.compile(r'''url\(\s*(?:"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|(?:\\.|[^)\\])*)\s*\)''', re.I)

def offline_css(css):
    if re.search(r'@import\b|</style', css, re.I):
        raise ValueError('imports/style terminators require an explicitly flattened capture')
    def local(m):
        value=m.group()[4:-1].strip().strip('"\'')
        return m.group() if value.startswith('#') else 'url("data:,")'
    return URL.sub(local, css)

HEADER = '''<section id="layout-stream-panel" style="position:relative;z-index:9999;display:block;margin:24px auto;padding:24px;max-width:1000px;background:white;color:#183447;border:2px solid #316d91;font:16px sans-serif">
<h1 style="font:32px sans-serif">An Article in Stages</h1><p id="layout-stream-status">Measuring the growing article…</p>
<p>Geometry reads at fixed points in a saved Wikipedia article. Keep the window and zoom fixed. After completion, press <b>d</b> to save the native report.</p>
<pre id="layout-stream-report" style="white-space:pre-wrap;font:15px monospace">Working…</pre></section>
<script>
window.layoutStreamRun={samples:[],heights:[],width:innerWidth,height:innerHeight,ratio:devicePixelRatio||1,invalid:false};
window.layoutStreamSample=function(){
 var r=window.layoutStreamRun;
 if(innerWidth!==r.width||innerHeight!==r.height||(devicePixelRatio||1)!==r.ratio)r.invalid=true;
 var before=performance.now();var height=document.body.offsetHeight;var elapsed=performance.now()-before;
 if(!isFinite(elapsed)||elapsed<0)r.invalid=true;
 r.samples.push(elapsed);r.heights.push(height);
};
</script>'''
FINISH = '''<script>
(function(){var r=window.layoutStreamRun,ordered=r.samples.slice().sort(function(a,b){return a-b;}),total=0;
for(var i=0;i<r.samples.length;i++)total+=r.samples[i];
var n=ordered.length,m=Math.floor(n/2),median=n%2?ordered[m]:(ordered[m-1]+ordered[m])/2;
var state=r.invalid||n!==EXPECTED?'invalid':'complete';
document.getElementById('layout-stream-status').textContent=state==='complete'?'Complete. Stream measurements saved on this page.':'Invalid: checkpoint count, viewport, or clock changed.';
document.getElementById('layout-stream-report').textContent=JSON.stringify({format:'yonder-stream-layout-v1',state:state,checkpoints:n,total_ms:total,median_ms:median,worst_ms:ordered[n-1],viewport:{width:r.width,height:r.height,devicePixelRatio:r.ratio},samples_ms:r.samples,heights:r.heights},null,2);
})();
</script>'''

class Article(HTMLParser):
    VOID={'area','base','br','col','embed','hr','img','input','link','meta','param','source','track','wbr'}
    BLOCKED={'svg','math','template','table'}
    def __init__(self, source, address, sheets):
        super().__init__(convert_charrefs=False)
        self.parts=[];self.address=address;self.sheets=sheets;self.used=[]
        self.skip=None;self.style=False;self.in_body=False;self.blocked=0;self.checkpoints=[];self.last=0
        self.lines=[0]
        for line in source.splitlines(keepends=True):self.lines.append(self.lines[-1]+len(line))
    def position(self):
        line,col=self.getpos();return self.lines[line-1]+col
    def handle_starttag(self,tag,attrs):
        if self.skip:return
        if tag in {'script','noscript','iframe','object','video','audio'}:self.skip=tag;return
        values=dict(attrs)
        if tag=='base' or tag=='embed':return
        if tag=='meta' and values.get('http-equiv','').lower()=='refresh':return
        if tag=='link':
            if 'stylesheet' in values.get('rel','').lower().split():
                address=urljoin(self.address,values.get('href',''))
                if address not in self.sheets:raise ValueError('missing captured stylesheet: '+address)
                css=self.sheets[address];self.used.append(address)
                media=' media="'+escape(values['media'],quote=True)+'"' if values.get('media') else ''
                self.parts.append('<style'+media+'>'+offline_css(css)+'</style>')
            return
        kept=[]
        for name,value in attrs:
            if name.startswith('on') or name in {'src','srcset','poster','background','data'}:continue
            if name in {'href','xlink:href'} and tag!='a' and value and not value.startswith('#'):continue
            if name=='style' and value:value=offline_css(value)
            kept.append(' '+name+('="'+escape(value,quote=True)+'"' if value is not None else ''))
        self.parts.append('<'+tag+''.join(kept)+'>')
        if tag=='style':self.style=True
        if tag in self.BLOCKED:self.blocked+=1
        if tag=='body':self.in_body=True;self.parts.append(HEADER);self.last=self.position()
    def handle_startendtag(self,tag,attrs):
        self.handle_starttag(tag,attrs)
        if tag not in self.VOID:self.handle_endtag(tag)
    def handle_endtag(self,tag):
        if self.skip:
            if tag==self.skip:self.skip=None
            return
        if tag=='body':self.parts.append(FINISH.replace('EXPECTED',str(len(self.checkpoints))));self.in_body=False
        self.parts.append('</'+tag+'>')
        if tag=='style':self.style=False
        if tag in self.BLOCKED:self.blocked=max(0,self.blocked-1)
        if self.in_body and not self.blocked and tag in {'p','ul','h2','section','div'} and self.position()-self.last>=4096:
            self.last=self.position();self.checkpoints.append(self.last)
            self.parts.append('<script>window.layoutStreamSample();</script>')
    def handle_data(self,data):
        if not self.skip:self.parts.append(offline_css(data) if self.style else data)
    def handle_entityref(self,name):
        if not self.skip:self.parts.append('&'+name+';')
    def handle_charref(self,name):
        if not self.skip:self.parts.append('&#'+name+';')
    def handle_decl(self,data):self.parts.append('<!'+data+'>')
    def handle_comment(self,data):
        if not self.skip:self.parts.append('<!--'+data+'-->')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('record',type=Path);parser.add_argument('output',type=Path)
    args=parser.parse_args(); record=args.record; source=record.with_suffix('.html'); resources=record.with_suffix('.d')
    address=unescape(record.read_bytes().splitlines()[0].removeprefix(b'address: '))
    sheets={};inputs=[record,source,resources/'index']
    for raw in (resources/'index').read_bytes().splitlines():
        fields=raw.split(b'\t')
        if len(fields)>=5 and fields[1]==b'sheet' and fields[3]==b'came':
            item=resources/(fields[0].decode('ascii')+'.css');inputs.append(item)
            sheets[unescape_bytes(fields[2],tokens=False).decode('utf-8')]=item.read_text(encoding='utf-8')
    text=source.read_text(encoding='utf-8');article=Article(text,address,sheets);article.feed(text);article.close()
    if len(article.checkpoints)<3:raise ValueError('capture has too few eligible checkpoints')
    out=args.output;out.mkdir(parents=True,exist_ok=True)
    (out/'layout-stream-article.html').write_text(''.join(article.parts),encoding='utf-8')
    (out/'layout-stream.html').write_text('''<!doctype html><meta charset="utf-8"><title>An Article in Stages</title>
<style>body{font:18px sans-serif;background:#edf3f7;color:#183447;margin:40px}main{max-width:900px;margin:auto;background:white;border:1px solid #a9c3d4;padding:30px}a{display:inline-block;background:#225e83;color:white;padding:14px;text-decoration:none}</style>
<main><h1>An Article in Stages</h1><p>A fixed Wikipedia capture, measured as the parser builds its document.</p>
<p>Set your window size and zoom first. This test runs automatically after the article opens. Keep Scripts and Modern Web enabled.</p>
<p><a href="layout-stream-article.html">Start streamed comparison</a></p>
<p>Wait for Complete, then click ordinary text and press <b>d</b>. Save the report screenshot too. Restart Yonder before a comparison run.</p></main>''',encoding='utf-8')
    manifest={'source_url':address,'checkpoint_offsets_characters':article.checkpoints,'stylesheet_order':article.used,
        'inputs':{str(p.relative_to(record.parent)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},
        'outputs':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(out.glob('layout-stream*.html'))},
        'normalization':'Original scripts/handlers/embedded documents and asset attributes removed; captured CSS inlined; non-fragment CSS URLs replaced by empty data URLs; fixed parser geometry checkpoints inserted.'}
    (out/'layout-stream-inputs.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(f'{len(article.checkpoints)} checkpoints, {len(article.used)} captured stylesheets, {address}')
if __name__=='__main__':main()
