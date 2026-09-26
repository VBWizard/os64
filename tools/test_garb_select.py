#!/usr/bin/env python3
"""libgarb's Selectors against cssselect2 (GARB.md, G2a): the same page parsed
by both sides' HTML parsers (libhtml there, html5lib here), and every
selector's validity, specificity, pseudo-element and the elements it matches
compared.

    python3 tools/test_garb_select.py DRIVER [PAGES_GLOB] [GENERATED]

PAGES_GLOB defaults to tools/html_corpus/*.html. cssselect2 and html5lib are
not system packages here; make a scratch venv, as test_garb_differential.py
says, and `pip install cssselect2 html5lib`.

Where the reference and the Selectors standard part company, the standard
is followed and the difference is named in ADJUDICATED below, never
silently skipped.
"""
import glob
import re
import json
import random
import subprocess
import sys
from pathlib import Path

try:
    import cssselect2
    import html5lib
except ImportError:
    print(__doc__)
    sys.exit(2)

PSEUDO = {None: 0, 'before': 1, 'after': 2, 'first-line': 3, 'first-letter': 4, 'marker': 5,
          'placeholder': 6, 'selection': 7, 'backdrop': 8}

# Every kind of simple selector, combinator and pseudo-class the engine
# knows, and a few it must refuse.
CATALOGUE = r'''
*
html
BODY
p
a
div p
div > p
h1 + p
h1 ~ p
div *
body > * > *
p a, div a
#main
.x
div.x.y
[href]
[href="/"]
[href^=http]
[href$=".html"]
[href*=wiki]
[class~=x]
[lang|=en]
[type=text i]
[type="TEXT"]
[type="TEXT" s]
a[href][title]
:root
html:root
:first-child
:last-child
:only-child
p:first-of-type
p:last-of-type
:only-of-type
:nth-child(2)
:nth-child(odd)
:nth-child(even)
:nth-child(2n+1)
:nth-child(-n+3)
:nth-child(3n - 1)
:nth-last-child(2)
:nth-of-type(2n)
:nth-last-of-type(1)
:nth-child(2 of .x)
:nth-child(odd of p, a)
:empty
:link
:any-link
:visited
:hover
a:hover
:focus
:active
:focus-within
:focus-visible
:target
:enabled
:disabled
:checked
:required
:optional
:read-only
:read-write
:not(p)
:not(p, a)
:not(.x)
p:not(:first-child)
:is(h1, h2, h3)
:where(h1, h2) a
:is(p, :nonsense) a
:where(.x, #main)
div:has(> p)
div:has(p)
body:has(a[href])
h1:has(+ p)
h2:has(~ p)
:has(img)
:lang(en)
:lang(en, fr)
p::before
p::after
::first-line
p:before
a::first-letter
::marker
*|p
|p
*|*
p::before:hover
div:hover > p
p:nonsense
p::nonsense
p::before span
ns|p
#1a
.
[
p,
, p
p > > a
:nth-child(n+)
:not()
:has()
a:not(:hover)
:is()
:where()
ul li
ul > li:nth-child(2n)
table tr td
tr:nth-child(odd) td
td + td
li ~ li
form input[type=submit]
input:not([type=hidden])
option:checked
fieldset:disabled input
select:required
[data-x]
div#main.x
a[href^="https://"]:not([href*="example"])
body div div div div div a
:root > body > *
*:not(html):not(body):first-child
'''.strip().split('\n')

# Where cssselect2 is not the standard. Keys are selectors or (selector,
# page) pairs; each value says why the standard's answer stands. Every entry
# was checked by hand against a small page as well (the engine's answer
# there is in the commit that added it).
LINKS = ('HTML § Pseudo-classes: :link and :any-link match a and area elements '
         'with an href; cssselect2 still counts link elements, as HTML once did')
ADJUDICATED = {
    '[type="TEXT"]': 'HTML § Case-sensitivity of selectors: type is on the legacy list of '
                     'attributes whose values match without regard to ASCII case on an HTML '
                     'element; cssselect2 compares them exactly',
    ':nth-child(2 of .x)': 'Selectors 4 § 17: nth-child(An+B of S) weighs a pseudo-class plus '
                           'the heaviest selector in S, (0,2,0) here; cssselect2 counts only '
                           'the pseudo-class',
    ':nth-child(odd of p, a)': 'the same specificity rule, and cssselect2 matches nothing when S '
                               'is a list of more than one selector (it finds the right element '
                               'with one)',
    ':link': LINKS,
    ':any-link': LINKS,
    ':enabled': 'HTML § Pseudo-classes: :enabled is a button, input, select, textarea, '
                'optgroup, option or fieldset that is not disabled; cssselect2 still counts '
                'a, area and link elements, as HTML did while it had commands',
    ':is(p, :nonsense) a': 'Selectors 4 § 4.2: :is() takes a FORGIVING list, so the selector '
                           'it cannot read is dropped and :is(p) stands; cssselect2 refuses '
                           'the whole list',
    'p::before:hover': 'Selectors 4 § 3.6.3: a user-action pseudo-class may follow a '
                       'pseudo-element; cssselect2 refuses it (it matches nothing here either)',
}

# Pseudo-classes the reference answers differently wherever they appear in a
# selector, for the reasons above.
PSEUDO_CLASSES = {
    ':link': LINKS,
    ':any-link': LINKS,
    ':enabled': ADJUDICATED[':enabled'],
    ':required': 'Selectors 4 § 13.3 and HTML define it; cssselect2 does not know it',
    ':optional': 'Selectors 4 § 13.3 and HTML define it; cssselect2 does not know it',
    ':read-only': 'Selectors 4 § 13.2 and HTML define it; cssselect2 does not know it',
    ':read-write': 'Selectors 4 § 13.2 and HTML define it; cssselect2 does not know it',
}


def adjudicated(text, page):
    why = ADJUDICATED.get(text) or ADJUDICATED.get((text, page))
    if why:
        return why
    for name, reason in PSEUDO_CLASSES.items():
        if name in text:
            return reason
    return None


def reference(tree, text, elements, index):
    try:
        compiled = cssselect2.compile_selector_list(text)
    except cssselect2.SelectorError:
        return None
    except Exception:
        return None
    out = []
    for sel in compiled:
        spec = sel.specificity
        pseudo = PSEUDO.get(sel.pseudo_element, -1)
        hits = [index[id(e.etree_element)] for e in elements if sel.test(e)]
        out.append([spec[0], spec[1], spec[2], pseudo, hits])
    return out


def generated(page_elements, rng, count):
    """Random selectors built from what the page actually holds."""
    tags = sorted({e.local_name for e in page_elements})
    classes = sorted({c for e in page_elements for c in e.classes})
    ids = sorted({e.id for e in page_elements if e.id})
    attrs = sorted({a for e in page_elements for a in e.etree_element.attrib})
    pseudo = [':first-child', ':last-child', ':only-child', ':empty', ':nth-child(2n+1)',
              ':nth-last-child(2)', ':first-of-type', ':not(:first-child)', ':link']
    out = []
    for _ in range(count):
        parts = []
        for k in range(rng.randint(1, 4)):
            compound = ''
            r = rng.random()
            if r < 0.5 and tags:
                compound += rng.choice(tags)
            elif r < 0.6:
                compound += '*'
            if classes and rng.random() < 0.4:
                compound += '.' + rng.choice(classes)
            if ids and rng.random() < 0.1:
                compound += '#' + rng.choice(ids)
            if attrs and rng.random() < 0.2:
                compound += '[' + rng.choice(attrs) + ']'
            if rng.random() < 0.25:
                compound += rng.choice(pseudo)
            if not compound:
                compound = rng.choice(tags) if tags else '*'
            if k:
                parts.append(rng.choice([' ', ' > ', ' + ', ' ~ ']))
            parts.append(compound)
        out.append(''.join(parts))
    return out


def valid_ident(s):
    return s and all(c.isalnum() or c in '-_' for c in s) and not s[0].isdigit() and \
        not (s[0] == '-' and len(s) > 1 and s[1].isdigit())


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    driver = sys.argv[1]
    pages = sorted(glob.glob(sys.argv[2] if len(sys.argv) > 2 else 'tools/html_corpus/*.html'))
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 400
    total = bad = excused = 0
    for path in pages:
        # html5lib makes a template's contents its children and libhtml keeps
        # them apart, as the standard's tree does; the element order the two
        # sides count would part there, so templates are taken out first.
        raw = re.sub(rb'(?is)<template\b.*?</template\s*>', b'', Path(path).read_bytes())
        tree = html5lib.parse(raw, treebuilder='etree')
        root = cssselect2.ElementWrapper.from_html_root(tree)
        elements = list(root.iter_subtree())
        index = {id(e.etree_element): i for i, e in enumerate(elements)}
        rng = random.Random(path)
        gen = [g for g in generated(elements, rng, count)
               if all(valid_ident(c) for e in [g] for c in [])]
        selectors = CATALOGUE + gen
        feed = f'{len(raw)}\n'.encode() + raw
        lists = '\n'.join(selectors).encode()
        feed += f'{len(lists)}\n'.encode() + lists + b'-1\n'
        run = subprocess.run([driver, 'select'], input=feed, capture_output=True)
        if run.returncode != 0:
            print(f'FAIL {path}: the driver exited {run.returncode}\n'
                  + run.stderr.decode(errors='replace')[:3000])
            sys.exit(1)
        answers = run.stdout.decode().splitlines()
        if len(answers) != len(selectors):
            print(f'FAIL {path}: {len(answers)} answers for {len(selectors)} selectors')
            sys.exit(1)
        page_bad = 0
        for text, line in zip(selectors, answers):
            total += 1
            got = json.loads(line)
            want = reference(tree, text, elements, index)
            if got == want:
                continue
            if adjudicated(text, Path(path).name):
                excused += 1
                continue
            bad += 1
            page_bad += 1
            if page_bad <= 6:
                print(f'FAIL {Path(path).name}: {text!r}\n  want {json.dumps(want)[:300]}'
                      f'\n  got  {json.dumps(got)[:300]}')
        print(f'{Path(path).name}: {len(selectors)} selectors over {len(elements)} elements, '
              f'{page_bad} differ')
    print(f'selectors: {total} compared, {bad} differ, {excused} adjudicated to the standard')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
