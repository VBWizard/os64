#!/usr/bin/env python3
"""Generate libjs's en-US locale data (userland/libjs/intl/intl_data.js).

Usage: tools/intl_cldr.py <cldr dir> [zoneinfo dir] > userland/libjs/intl/intl_data.js
  <cldr dir> holds the cldr-json packages of one version, unpacked:
  cldr-core/, cldr-dates-full/, cldr-numbers-full/, cldr-bcp47/,
  cldr-localenames-full/, cldr-units-full/ and cldr-misc-full/ (npm's
  tarballs; the last three only their main/en), and cldr-uca/ holding
  CLDR's root collation, common/uca/allkeys_CLDR.txt from the same
  release's source.

INTL.md § 4.2: every table the formatters read is CLDR's, taken whole for
`en` and copied, never typed, its version written into the file's head.
What is taken:
  gregorian      months, days, eras, day periods (format and stand-alone),
                 the date, time and date-time patterns (and the time
                 patterns' -alt-ascii forms), availableFormats,
                 appendItems, intervalFormats
  zones          the GMT and region formats, the metazones' en names, the
                 zone-specific names and exemplar cities, each zone's
                 current metazone and its country (tzdata's zone.tab,
                 where ICU's regions come from, matched to CLDR's names
                 by their aliases), the primary zones of countries with
                 several, and the countries' en names: what the generic
                 and location names are made of
  dayPeriods     en's rules for the `dayPeriod` option
  hourCycles     the US's preferred and allowed hour cycles (timeData)
  likely         likely subtags for the English tags and every `und` row
                 (INTL.md § 7: other tags maximize to themselves)
  likelyRegions  the likely region of each language or language-script
                 that picks a retired region's replacement other than the
                 first
  numbers        deferred (a function returning the five below, which
                 the first NumberFormat, PluralRules or RelativeTimeFormat
                 calls):
    numbers      the latn symbols, the decimal, percent, scientific and
                 currency patterns (standard, accounting, the
                 alphaNextToNumber forms, compact short and long, currency
                 compact), minimumGroupingDigits, the misc patterns (range,
                 approximately), every currency's en names and symbols,
                 and each currency's digits where not the default 2
    units        the short, long and narrow patterns of the simple units
                 ECMA-402 sanctions, CLDR's own patterns for the
                 sanctioned per-units it has (km/h), and the compound
                 "per" patterns
    plurals      en's cardinal and ordinal rules and its plural ranges
    currentCurrencies  the codes some region uses as tender today
    relative     the relative-time fields of RelativeTimeFormat's eight
                 units in each width: the words for -1, 0 and 1 ("yesterday")
                 and the future and past patterns by plural category
  lists          the list patterns of the three types in each width
                 (conjunction is CLDR's "standard", disjunction its "or")
  collation      deferred (built by the first Collator): CLDR root's
                 collation elements for Latin (U+0000-024F), the combining
                 marks (U+0300-036F) and general punctuation (U+2000-206F),
                 the contractions among them ("l·"), and the UCA version
  aliases        language, script, territory and variant aliases (the
                 canonical form of every locale argument, UTS 35)
  uAliases       each -u- key's value aliases, from the bcp47 data: a
                 deprecated value's `_preferred`, and a value's `_alias`
                 names (not the time zones', whose `_alias` is IANA's)
The file is one expression, `({...})`, evaluated by the data door
(libjs/intl/intl.c) and handed to intl.js, so nothing of it is a global.
Every object literal in it has no prototype (`"__proto__":null` first), so
no lookup of a key the data lacks reaches what a page put on
Object.prototype; a deferred part's function is called, never looked in.
"""
import json
import os
import re
import sys


def load(*parts):
    with open(os.path.join(*parts), encoding='utf-8') as f:
        return json.load(f)


class Deferred:
    """A part of the data a realm builds only when asked: written as a
    function returning the literal, so evaluating the data builds none of
    its objects (INTL.md § 6)."""
    def __init__(self, value):
        self.value = value


def literal(value):
    """`value` as a JavaScript literal whose objects have no prototype."""
    if isinstance(value, Deferred):
        return '() => (' + literal(value.value) + ')'
    if isinstance(value, dict):
        items = ['"__proto__":null'] + [json.dumps(k, ensure_ascii=False) + ':' + literal(value[k])
                                        for k in sorted(value)]
        return '{' + ','.join(items) + '}'
    if isinstance(value, list):
        return '[' + ','.join(literal(v) for v in value) + ']'
    return json.dumps(value, ensure_ascii=False)

# ── Collation (INTL.md § 4.1): CLDR root's collation elements, from
# allkeys_CLDR.txt, for the characters a page's names and words are made
# of; everything else collates by code point after them.
COLLATION_RANGES = ((0x0000, 0x024F), (0x0300, 0x036F), (0x2000, 0x206F))


def in_collation_ranges(cp):
    return any(lo <= cp <= hi for lo, hi in COLLATION_RANGES)


def collation(root):
    """{cp: [element, ...]} and {first cp: {second cp: elements}}: each
    collation element its primary, secondary and tertiary weights and
    whether it is variable (ignorable under ignorePunctuation: CLDR's
    maxVariable is punct, so its spaces, punctuation and the whitespace
    controls are, symbols are not; in these ranges the file marks exactly
    those with `*`), packed into one number, p * 0x10000 + s * 0x40 + t * 2 + v, because a
    realm holds them as numbers far more cheaply than as arrays."""
    path = os.path.join(root, 'cldr-uca', 'allkeys_CLDR.txt')
    version = None
    singles, pairs = {}, {}
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.split('#', 1)[0].strip()
            if line.startswith('@version'):
                version = line.split()[1]
                continue
            if not line or ';' not in line:
                continue
            chars, ces = line.split(';', 1)
            cps = [int(c, 16) for c in chars.split()]
            if not all(in_collation_ranges(c) for c in cps) or len(cps) > 2:
                continue
            elements = []
            for m in re.finditer(r'\[([.*])([0-9A-F]{4})\.([0-9A-F]{4})\.([0-9A-F]{4})\]', ces):
                variable = m.group(1) == '*'
                p, s, t = int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16)
                if s >= 0x400 or t >= 0x20:
                    sys.exit('intl_cldr: collation weights of %04X do not pack' % cps[0])
                elements.append(p * 0x10000 + s * 0x40 + t * 2 + (1 if variable else 0))
            if len(cps) == 1:
                singles[cps[0]] = elements
            else:
                pairs.setdefault(cps[0], {})[cps[1]] = elements
    return version, singles, pairs


def likely_regions(likely, later_choices):
    """The likely region of each language, language-script or und-script
    that can pick a retired region's replacement other than the first of
    its territoryAlias choices ("hy-SU" is "hy-AM", "und-Armn-SU"
    "und-Armn-AM"), and of each language-script whose language can, since
    it is looked up before the language is."""
    def region(tag):
        return tag.split('-')[-1]
    out = {}
    for k, v in likely.items():
        parts = k.split('-')
        scripted = len(parts) == 2 and len(parts[1]) == 4 and parts[1][0].isalpha()
        if len(parts) != 1 and not scripted:
            continue
        if region(v) in later_choices or (scripted and parts[0] != 'und' and parts[0] in likely
                                          and region(likely[parts[0]]) in later_choices):
            out[k] = region(v)
    return out


def main():
    root = sys.argv[1]
    version = load(root, 'cldr-core', 'package.json')['version']
    greg = load(root, 'cldr-dates-full', 'main', 'en', 'ca-gregorian.json')['main']['en']['dates']['calendars']['gregorian']
    tzn = load(root, 'cldr-dates-full', 'main', 'en', 'timeZoneNames.json')['main']['en']['dates']['timeZoneNames']
    meta = load(root, 'cldr-core', 'supplemental', 'metaZones.json')['supplemental']['metaZones']
    periods = load(root, 'cldr-core', 'supplemental', 'dayPeriods.json')['supplemental']['dayPeriodRuleSet']['en']
    time_data = load(root, 'cldr-core', 'supplemental', 'timeData.json')['supplemental']['timeData']
    likely = load(root, 'cldr-core', 'supplemental', 'likelySubtags.json')['supplemental']['likelySubtags']
    alias = load(root, 'cldr-core', 'supplemental', 'aliases.json')['supplemental']['metadata']['alias']
    later_choices = set()
    for entry in alias['territoryAlias'].values():
        later_choices.update(entry['_replacement'].split()[1:])

    # Each zone's current metazone: the usesMetazone entry with no `_to`.
    current = {}

    def walk(node, path):
        if isinstance(node, list):
            for use in node:
                u = use.get('usesMetazone', {})
                if '_to' not in u and '_mzone' in u:
                    current['/'.join(path)] = u['_mzone']
            return
        for k, v in node.items():
            walk(v, path + [k])
    walk(meta['metazoneInfo']['timezone'], [])
    golden = {}
    for m in meta['metazones']:
        z = m['mapZone']
        golden.setdefault(z['_other'], {})[z['_territory']] = z['_type']

    def zone_names(node, path, out):
        for k, v in node.items():
            if isinstance(v, dict) and any(x in v for x in ('long', 'short', 'exemplarCity')):
                out['/'.join(path + [k])] = {x: v[x] for x in ('long', 'short', 'exemplarCity') if x in v}
            elif isinstance(v, dict):
                zone_names(v, path + [k], out)
    zone_specific = {}
    zone_names(tzn['zone'], [], zone_specific)

    # Each canonical zone's country, from tzdata's zone.tab (ICU's regions
    # come from it): a zone there under any of its CLDR names is in its
    # country; one not there (`Etc/*`, `EST5EDT`) names none.
    zoneinfo = sys.argv[2] if len(sys.argv) > 2 else '/usr/share/zoneinfo'
    tab = {}
    with open(os.path.join(zoneinfo, 'zone.tab'), encoding='utf-8') as f:
        for line in f:
            if line.startswith('#') or not line.strip():
                continue
            fields = line.rstrip('\n').split('\t')
            tab[fields[2]] = fields[0]
    bcp = load(root, 'cldr-bcp47', 'bcp47', 'timezone.json')['keyword']['u']['tz']
    country = {}
    for key, value in bcp.items():
        if not isinstance(value, dict) or '_alias' not in value or value.get('_deprecated'):
            continue
        names = value['_alias'].split() + ([value['_iana']] if value.get('_iana') else [])
        found = next((tab[n] for n in names if n in tab), None)
        if found is not None:
            country[names[0]] = found
    primary = load(root, 'cldr-core', 'supplemental', 'primaryZones.json')['supplemental']['primaryZones']
    territories = load(root, 'cldr-localenames-full', 'main', 'en', 'territories.json')['main']['en'][
        'localeDisplayNames']['territories']
    countries = {code: territories[code] for code in set(country.values()) if code in territories}

    u_aliases = {}
    bcp47_dir = os.path.join(root, 'cldr-bcp47', 'bcp47')
    for name in sorted(os.listdir(bcp47_dir)):
        keys = load(bcp47_dir, name).get('keyword', {}).get('u', {})
        for key, entries in keys.items():
            if not isinstance(entries, dict):
                continue
            table = {}
            for value, entry in entries.items():
                if value.startswith('_') or not isinstance(entry, dict):
                    continue
                if '_preferred' in entry:
                    table[value] = entry['_preferred']
                elif '_alias' in entry and key != 'tz':
                    for legacy in entry['_alias'].split():
                        table[legacy] = value
            if table:
                u_aliases[key] = table

    numbers = load(root, 'cldr-numbers-full', 'main', 'en', 'numbers.json')['main']['en']['numbers']
    currency_names = load(root, 'cldr-numbers-full', 'main', 'en', 'currencies.json')['main']['en']['numbers']['currencies']
    fractions = load(root, 'cldr-core', 'supplemental', 'currencyData.json')['supplemental']['currencyData']['fractions']
    units_en = load(root, 'cldr-units-full', 'main', 'en', 'units.json')['main']['en']['units']
    fields_all = load(root, 'cldr-dates-full', 'main', 'en', 'dateFields.json')['main']['en']['dates']['fields']
    plural_rules = load(root, 'cldr-core', 'supplemental', 'plurals.json')['supplemental']['plurals-type-cardinal']['en']
    ordinal_rules = load(root, 'cldr-core', 'supplemental', 'ordinals.json')['supplemental']['plurals-type-ordinal']['en']
    plural_ranges = load(root, 'cldr-core', 'supplemental', 'pluralRanges.json')['supplemental']['plurals']['en']

    # ECMA-402's sanctioned simple units (§ 6.6.2, Table 2), each found
    # under its CLDR category ("length-kilometer").
    sanctioned = ('acre bit byte celsius centimeter day degree fahrenheit fluid-ounce foot gallon gigabit '
                  'gigabyte gram hectare hour inch kilobit kilobyte kilogram kilometer liter megabit megabyte '
                  'meter microsecond mile mile-scandinavian milliliter millimeter millisecond minute month '
                  'nanosecond ounce percent petabyte pound second stone terabit terabyte week yard year').split()

    def unit_key(width, unit):
        hits = [k for k in units_en[width] if k.split('-', 1)[-1] == unit and '-' in k]
        hits = [k for k in hits if not k.startswith('duration-') or unit in
                ('day', 'hour', 'microsecond', 'millisecond', 'minute', 'month', 'nanosecond', 'second', 'week', 'year')]
        if unit == 'degree':
            hits = [k for k in hits if k.startswith('angle-')]
        if len(hits) != 1:
            sys.exit('intl_cldr: unit %s is %r in %s' % (unit, hits, width))
        return hits[0]

    units = {}
    for width in ('short', 'long', 'narrow'):
        table = {'per': units_en[width]['per']['compoundUnitPattern']}
        for unit in sanctioned:
            entry = units_en[width][unit_key(width, unit)]
            table[unit] = {k: v for k, v in entry.items() if k.startswith('unitPattern-count-') or k == 'perUnitPattern'}
        # CLDR's own patterns for a sanctioned unit per a sanctioned unit.
        for key, entry in units_en[width].items():
            if '-per-' in key:
                pair = key.split('-', 1)[1]
                num, _, den = pair.partition('-per-')
                if num in sanctioned and den in sanctioned:
                    table[pair] = {k: v for k, v in entry.items() if k.startswith('unitPattern-count-')}
        units[width] = table

    # The currencies some region uses as tender today (an entry with no
    # `_to`, not marked `_tender: false`): supportedValuesOf's answer.
    current_currencies = sorted({code for entries in load(root, 'cldr-core', 'supplemental', 'currencyData.json')
                                 ['supplemental']['currencyData']['region'].values()
                                 for entry in entries for code, info in entry.items()
                                 if '_to' not in info and info.get('_tender', 'true') != 'false'})
    latn = lambda name: numbers[name + '-numberSystem-latn']
    number_data = {
        'symbols': latn('symbols'),
        'decimal': latn('decimalFormats'),
        'percent': latn('percentFormats'),
        'scientific': latn('scientificFormats'),
        'currency': latn('currencyFormats'),
        'misc': latn('miscPatterns'),
        'minimumGroupingDigits': int(numbers['minimumGroupingDigits']),
        'currencies': {code: {k: v for k, v in entry.items()
                              if k in ('displayName', 'displayName-count-one', 'displayName-count-other',
                                       'symbol', 'symbol-alt-narrow')}
                       for code, entry in currency_names.items()},
        'currencyDigits': {code: int(f['_digits']) for code, f in fractions.items()
                           if code != 'DEFAULT' and f['_digits'] != fractions['DEFAULT']['_digits']},
    }

    # RelativeTimeFormat's units, each in its three widths.
    relative = {}
    for unit in ('second', 'minute', 'hour', 'day', 'week', 'month', 'quarter', 'year'):
        for width in ('', '-short', '-narrow'):
            entry = fields_all[unit + width]
            relative[unit + width] = {k: v for k, v in entry.items()
                                      if k.startswith('relative-type-') or k.startswith('relativeTime-type-')}
    lists = load(root, 'cldr-misc-full', 'main', 'en', 'listPatterns.json')['main']['en']['listPatterns']
    uca_version, coll_singles, coll_pairs = collation(root)

    dtf = greg['dateTimeFormats']
    # The fields' names, which an append item's {2} writes (ICU's
    # getAppendName: the wide display name, quoted).
    fields = load(root, 'cldr-dates-full', 'main', 'en', 'dateFields.json')['main']['en']['dates']['fields']
    field_names = {k: fields[k]['displayName'] for k in
                   ('era', 'year', 'quarter', 'month', 'week', 'weekday', 'day', 'dayperiod', 'hour', 'minute',
                    'second', 'zone')}
    data = {
        'cldr': version,
        'gregorian': {
            'months': greg['months'], 'days': greg['days'], 'eras': greg['eras'],
            'dayPeriods': greg['dayPeriods'],
            'dateFormats': greg['dateFormats'], 'timeFormats': greg['timeFormats'],
            'dateTimeFormats': {k: dtf[k] for k in ('full', 'long', 'medium', 'short')},
            'dateTimeFormatsAtTime': greg['dateTimeFormats-atTime']['standard'],
            'availableFormats': dtf['availableFormats'],
            'appendItems': dtf['appendItems'],
            'intervalFormats': dtf['intervalFormats'],
            'fieldNames': field_names,
        },
        'zones': {
            'formats': {k: v for k, v in tzn.items() if k not in ('zone', 'metazone')},
            'metazones': tzn['metazone'],
            'zones': zone_specific,
            'current': current,
            'golden': golden,
            'country': country,
            'primary': primary,
            'countries': countries,
        },
        'dayPeriods': periods,
        # NumberFormat's, PluralRules' and RelativeTimeFormat's tables, built
        # by the first of them a realm constructs, so a realm that formats
        # only dates never does.
        'numbers': Deferred({
            'numbers': number_data,
            'units': units,
            'plurals': {'cardinal': plural_rules, 'ordinal': ordinal_rules, 'ranges': plural_ranges},
            'relative': relative,
            'currentCurrencies': current_currencies,
        }),
        'lists': lists,
        # Collator's elements, built by the first Collator a realm constructs.
        'collation': Deferred({'singles': {str(cp): ces for cp, ces in coll_singles.items()},
                               'pairs': {str(cp): {str(c2): ces for c2, ces in follow.items()}
                                         for cp, follow in coll_pairs.items()},
                               'uca': uca_version}),
        'hourCycles': {'US': time_data['US'], '001': time_data['001']},
        'likely': {k: v for k, v in likely.items()
                   if k in ('und', 'en') or k.startswith('en-') or k.startswith('und-')},
        'likelyRegions': likely_regions(likely, later_choices),
        'uAliases': u_aliases,
        'aliases': {k: {name: entry.get('_replacement') for name, entry in alias[k].items()}
                    for k in ('languageAlias', 'scriptAlias', 'territoryAlias', 'variantAlias')},
    }
    text = literal(data)
    sys.stdout.write('/* GENERATED by tools/intl_cldr.py: do not edit. CLDR %s, locale en '
                     '(INTL.md § 4.2). */\n' % version)
    sys.stdout.write('(' + text + ')\n')
    sys.stderr.write('intl_cldr: CLDR %s, %d bytes\n' % (version, len(text.encode('utf-8'))))


if __name__ == '__main__':
    main()
