#!/usr/bin/env python3
"""Save public pages as explicit, dated libhtml regression inputs."""
import hashlib
import json
from pathlib import Path
import urllib.request
from datetime import datetime, timezone
ROOT=Path(__file__).resolve().parent/'html_corpus'
PAGES={
 'example':'https://example.com/',
 'wikipedia-html':'https://en.wikipedia.org/wiki/HTML',
 'hacker-news':'https://news.ycombinator.com/',
 '68k-news':'http://68k.news/',
 'textfiles-computers':'http://textfiles.com/computers/',
 'floodgap':'https://gopher.floodgap.com/gopher/gw',
}

def main():
    ROOT.mkdir(exist_ok=True)
    manifest={}
    for name,url in PAGES.items():
        request=urllib.request.Request(url,headers={'User-Agent':'os64-libhtml-fixtures/1.0','Accept-Encoding':'identity'})
        with urllib.request.urlopen(request,timeout=45) as response:
            data=response.read(8*1024*1024+1)
            if len(data)>8*1024*1024:raise RuntimeError(name+' exceeds fixture limit')
            manifest[name]={'requested_url':url,'final_url':response.url,'charset':response.headers.get_content_charset(),
                            'content_type':response.headers.get('Content-Type'),'status':response.status,
                            'fetched_utc':datetime.now(timezone.utc).isoformat(),
                            'sha256':hashlib.sha256(data).hexdigest(),'bytes':len(data)}
        (ROOT/(name+'.html')).write_bytes(data)
        print(name,len(data),flush=True)
        (ROOT/'SOURCES.json').write_text(json.dumps(manifest,indent=2,sort_keys=True)+'\n')
if __name__=='__main__':main()
