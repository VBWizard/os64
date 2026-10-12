#!/usr/bin/env python3
"""Build deliberate menu hazards in scratch sources; the behavioral suite must fail."""
import argparse, os, subprocess, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
p = argparse.ArgumentParser()
p.add_argument('--output', type=Path)
a = p.parse_args()
out = a.output or Path(tempfile.mkdtemp(prefix='os64-menu-mutations-'))
out.mkdir(parents=True, exist_ok=True)
flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
         '-fno-omit-frame-pointer', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections']
flags += ['-I'+str(ROOT/s) for s in ['userland/libos64/include', 'userland/libos64', 'abi/include', 'tools/fonts']]
modules = ['ui_menu', 'ui_menu_widget', 'ui_dispatcher', 'ui_icon', 'ui_action', 'ui_status',
           'ui_font', 'ui', 'ui_controls', 'ui_list', 'ui_text', 'font_provider', 'font_adopt',
           'text', 'fmt', 'ui_theme', 'text_cache', 'text_decode', 'text_bitmap', 'text_draw', 'draw', 'str']
mutations = {
    'disabled': {'ui_action': ('if (!v->states[find(v->actions, id)].enabled)', 'if (v->states[find(v->actions, id)].enabled)')},
    'stale-target': {'ui_action': ('if (!v->context.validate(v->context.cookie, v->context.generation))',
                                 'if (false)')},
    'callback-before-close': {'ui_menu': ('if(os64_ui_menu_close(s,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK) (void)os64_ui_action_invoke(v,id);',
                                        '(void)os64_ui_action_invoke(v,id);\n    os64_ui_menu_close(s,OS64_UI_MENU_EXPLICIT);')},
    'stale-status-token': {'ui_status': ('if (!token || token != slot->token)', 'if (!token)')},
    'skip-font-close': {'ui_font': ('if (ui->menu_before_font && !ui->menu_before_font(ui)) return OS64_FONT_BUSY;', '(void)ui;')},
    'arrow-as-escape': {'ui_text': ('*seq = SEQ_ESC;\n\t\treturn K_NONE;', '*seq = SEQ_IDLE;\n\t\treturn K_ESC;')},
}
failed = False
for name, edits in mutations.items():
    folder = out/name
    folder.mkdir(exist_ok=True)
    paths = [ROOT/'tools/test_ui_menu_host.c', ROOT/'tools/fonts/fake_backend.c']
    for module in modules:
        path = ROOT/f'userland/libos64/{module}.c'
        if module in edits:
            old, new = edits[module]
            source = path.read_text()
            if old not in source:
                raise RuntimeError(f'{name}: mutation no longer matches {path}')
            path = folder/f'{module}.c'
            path.write_text(source.replace(old, new))
        paths.append(path)
    binary = folder/'test'
    subprocess.run(['cc', *flags, *map(str, paths), '-o', str(binary)], check=True)
    with (folder/'result.txt').open('w') as log:
        result = subprocess.run([str(binary), str(ROOT/'userland/libfreetype/fixtures')],
                                env=os.environ, stdout=log, stderr=subprocess.STDOUT)
    detected = result.returncode != 0 and "FAIL " in (folder/"result.txt").read_text()
    print(f'{name}: {"DETECTED" if detected else "MISSED"} (exit {result.returncode})', flush=True)
    failed |= not detected
print('Artifacts:', out)
raise SystemExit(1 if failed else 0)
