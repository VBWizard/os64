"""Whether a third-party notice travels with both images, for the audits
(test_av_audit.py, test_sound_audit.py, test_sbc_audit.py).

The root GNUmakefile installs every notice from one list, SYSTEM_LICENSES
(`<name>.txt:<source>` items), and the rules for the ext2 root and the FAT
lifeboat each walk that list. So a notice is installed on both images when
its item is in the list as make expands it, and both rules still walk the
list. The list is asked of make itself, not read as text, so however it is
spelled (a foreach, a variable) the answer is what the build installs.
"""
import pathlib
import subprocess


def system_licenses(root):
    """SYSTEM_LICENSES as make expands it: {installed name: source path}."""
    probe = 'audit-print-system-licenses:\n\t@echo $(SYSTEM_LICENSES)\n'
    out = subprocess.run(['make', '-s', '--no-print-directory', '-C', str(root), '-f', 'GNUmakefile', '-f', '-',
                          'audit-print-system-licenses'], input=probe, capture_output=True, text=True, check=True)
    items = {}
    for item in out.stdout.split():
        name, _, source = item.partition(':')
        items[name] = source
    return items


def both_images_walk_the_list(root):
    """Whether the ext2 rule (debugfs `write`) and the FAT rule (`mcopy`)
    each install every item of SYSTEM_LICENSES under /etc/licenses."""
    lines = (pathlib.Path(root) / 'GNUmakefile').read_text().splitlines()
    walks = [l for l in lines if '$(foreach item,$(SYSTEM_LICENSES),' in l and '/etc/licenses/' in l]
    return any("'write " in l for l in walks) and any('mcopy' in l for l in walks)


def installed_on_both_images(root, name, source):
    """(ok, why): the notice `source` is installed as /etc/licenses/`name`
    on the ext2 root and the FAT lifeboat."""
    items = system_licenses(root)
    if items.get(name) != source:
        return False, f'SYSTEM_LICENSES has no {name}:{source} (it has {name}:{items.get(name)})'
    if not both_images_walk_the_list(root):
        return False, 'the ext2 and FAT image rules no longer both walk SYSTEM_LICENSES'
    return True, ''
