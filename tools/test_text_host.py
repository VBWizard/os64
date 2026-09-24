#!/usr/bin/env python3
"""Build the actual F2 implementation and test it against frozen F0 vectors."""
import argparse,json,os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent

def vectors(out):
    cases=json.loads((ROOT/'tools/fonts/fixtures.json').read_text())['cases']
    lines=['static void frozen_vectors(void) {']
    for case in cases:
        data=bytes.fromhex(case['hex']);order=case.get('font_order',['P'])
        lines += ['{',f'current="{case["name"]}";',
            'const uint8_t bytes[]={'+(','.join(map(str,data)) or '0')+'};',
            'os64_text_font_t *fonts[]={'+','.join("open_font(c,'%s')"%x for x in order)+'};',
            f'os64_text_layout_t o={{.fonts=fonts,.font_count={len(order)},.tab_origin={case.get("tab_origin",0)},.tab_interval={case.get("tab_interval",512)}}};',
            'os64_text_run_t *r=NULL;',f'CHECK(os64_text_layout(c,bytes,{len(data)},&o,&r)==OS64_FONT_OK);',
            'os64_text_run_view_t v; CHECK(os64_text_run_view(r,&v)==OS64_FONT_OK);',
            f'CHECK(v.advance_x=={case["advance"]} && v.glyph_count=={len(case["origins"])} && v.caret_count=={len(case["carets"])});',
            'CHECK(rect_eq(v.ink,(os64_font_rect_t){'+','.join(map(str,case['ink']))+'}));']
        glyphs=[g for b,e,gs in case['clusters'] for g in gs if g[0]!='tab']
        for n,(x,span,g) in enumerate(zip(case['origins'],case['placement_spans'],glyphs)):
            fid='0' if g[0]=='marker' else f'fonts[{order.index(g[0])}]->identity'
            lines += [f'CHECK(v.glyphs[{n}].x=={x} && v.glyphs[{n}].byte_begin=={span[0]} && v.glyphs[{n}].byte_end=={span[1]});',f'CHECK(v.glyphs[{n}].font_identity=={fid} && v.glyphs[{n}].glyph_index=={g[1]});']
        for n,(b,x) in enumerate(case['carets']):lines += [f'CHECK(v.carets[{n}].byte_offset=={b} && v.carets[{n}].x=={x});']
        for x,b in case['hits']:lines += [f'{{os64_text_caret_t p;CHECK(os64_text_hit(r,{x},&p)==OS64_FONT_OK && p.byte_offset=={b});}}']
        for w,b in case['fits']:lines += [f'{{size_t p;CHECK(os64_text_fit(r,{w},&p)==OS64_FONT_OK && p=={b});}}']
        for b,bias,expected in case.get('snaps',[]):lines += [f'{{os64_text_caret_t p;CHECK(os64_text_caret(r,{b},OS64_TEXT_{bias.upper()},&p)==OS64_FONT_OK && p.byte_offset=={expected});}}']
        b,e,rect=case['selection'];lines += [f'{{os64_font_rect_t p;CHECK(os64_text_selection(r,{b},{e},&p)==OS64_FONT_OK);CHECK(rect_eq(p,(os64_font_rect_t){{'+','.join(map(str,rect))+'}));}',
            'os64_text_run_release(r);for(size_t i=0;i<o.font_count;i++) os64_text_font_release(fonts[i]);','}']
    lines += ['}'];out.write_text('\n'.join(lines)+'\n')

def main():
    p=argparse.ArgumentParser();p.add_argument('-O',default='2');p.add_argument('--output',type=Path);p.add_argument('--real',action='store_true');a=p.parse_args()
    out=a.output or Path(tempfile.mkdtemp(prefix='os64-text-'));out.mkdir(parents=True,exist_ok=True)
    vectors(out/'text_vectors.h')
    rows=[line.split(';') for line in (ROOT/'tools/fonts/unicode-17.0.0/UnicodeData.txt').read_text().splitlines()]
    expectations=[]
    for row in rows:
        parts=row[5].split();cp=int(row[0],16)
        if not (0xc0<=cp<=0x17f and len(parts)==2 and not parts[0].startswith('<')):continue
        base,mark=map(lambda x:int(x,16),parts)
        if not ((65<=base<=90 or 97<=base<=122) and 0x300<=mark<=0x36f):continue
        data=(chr(base)+chr(mark)).encode('utf-8')
        expectations += ['{const uint8_t bytes[]={'+','.join(map(str,data))+'};text_cluster d=text_decode(bytes,sizeof(bytes),0,0,false);'+f'CHECK(d.end==sizeof(bytes) && d.scalar=={cp} && !d.extra_marker);'+'}']
    (out/'text_composition_tests.h').write_text('\n'.join(expectations)+'\n')

    sources=['tools/test_text_host.c','tools/fonts/fake_backend.c','userland/libos64/text.c','userland/libos64/text_cache.c','userland/libos64/text_decode.c','userland/libos64/text_bitmap.c','userland/libos64/text_draw.c','userland/libos64/str.c','userland/libos64/draw.c']
    flags=['-std=c11','-O'+a.O,'-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(out),'-I'+str(ROOT/'userland/libos64/include'),'-I'+str(ROOT/'abi/include'),'-I'+str(ROOT/'userland/libos64'),'-I'+str(ROOT/'tools/fonts')]
    objects=[]
    if a.real:
        import runpy
        helpers=runpy.run_path(str(ROOT/'tools/test_freetype_host.py'))
        upstream,port=helpers['sources']()
        for n,source in enumerate(upstream+port):
            obj=out/f'ft{n}.o'
            extra=['-DFT2_BUILD_LIBRARY','-Wno-unused-variable','-Wno-unused-but-set-variable'] if source in upstream else []
            subprocess.run(['cc',*flags,'-fcf-protection=none','-fno-builtin','-fno-tree-loop-distribute-patterns','-DOS64_FREETYPE_HOSTED',
                '-I'+str(ROOT/'userland/libfreetype/port'),'-I'+str(ROOT/'userland/libfreetype/upstream/include'),
                *extra,'-c',str(source),'-o',str(obj)],check=True)
            objects.append(str(obj))
        flags += ['-DTEXT_REAL']
    subprocess.run(['cc',*flags,*[str(ROOT/s) for s in sources],*objects,'-o',str(out/'test_text')],check=True)
    result=subprocess.run([str(out/'test_text'),str(ROOT/'userland/libfreetype/fixtures')],env=os.environ)
    print('Artifacts:',out);return result.returncode
if __name__=='__main__':raise SystemExit(main())
