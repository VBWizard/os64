/* ECMA-402, SELF-HOSTED (INTL.md). libjs's install door (intl.c) runs this
 * file in a realm before any other script has run there, and calls it with
 * `I`, the host's half. It returns the Intl object, which the door defines
 * on the global.
 *
 *   I.locale         the default locale (OS64_JS_LOCALE)
 *   I.data           () → the locale data (intl_data.js, CLDR's, generated),
 *                    evaluated in this realm; asked once, by loadData
 *   I.zoneCanonical  (name) → the zone's canonical name, or undefined for
 *                    one the zone table does not hold (zones.inc)
 *   I.zoneOffset     (canonical name, epoch ms) → minutes east of UTC
 *   I.zoneHost       () → the machine's zone: a canonical name, or an
 *                    offset zone ("+05:30")
 *
 * Because it runs before any page script, every built-in it touches is
 * the realm's own: the ones it calls while formatting are captured here,
 * at load, so a page that replaces Array.prototype.push or
 * Date.prototype.getUTCHours later changes nothing Intl does. Nor can a
 * page's properties on Object.prototype or Array.prototype reach in: every
 * record the file makes, and the data's every object, has no prototype,
 * and arrays and results get their elements by defineProperty (put, push),
 * never by an assignment a setter could answer.
 *
 * The data is en-US's (INTL.md § 3): every locale resolves to "en" or
 * "en-US", which ECMA-402 permits (an implementation's available locales
 * are its own), and the Gregorian calendar and Latin digits are the only
 * ones. What it formats it formats as Chrome does for en-US, held by
 * captured strings (INTL.md § 8). */
(function (I) {
'use strict';

// ── The captures ────────────────────────────────────────────────────────
const Object_ = Object, Array_ = Array, String_ = String, Number_ = Number, Math_ = Math;
const TypeError_ = TypeError, RangeError_ = RangeError, Symbol_ = Symbol, Date_ = Date;
const apply = Reflect.apply;
const defineProperty = Object_.defineProperty;
const getOwnPropertyDescriptor = Object_.getOwnPropertyDescriptor;
const objectCreate = Object_.create;
const toStringTag = Symbol_.toStringTag;
// OrdinaryHasInstance, which the legacy constructor behaviour asks: never
// `instanceof`, which a page's Symbol.hasInstance would answer.
const ordinaryHasInstance = Function.prototype[Symbol_.hasInstance];
const hasInstance = (C, o) => apply(ordinaryHasInstance, C, [o]);
const floor = Math_.floor, abs = Math_.abs, trunc = Math_.trunc;
const isNaN_ = Number_.isNaN, isFinite_ = Number_.isFinite;
const fromCharCode = String_.fromCharCode;
const StringProto = String_.prototype;
const s_slice = StringProto.slice, s_indexOf = StringProto.indexOf, s_lastIndexOf = StringProto.lastIndexOf;
const s_toLowerCase = StringProto.toLowerCase, s_toUpperCase = StringProto.toUpperCase;
const s_charCodeAt = StringProto.charCodeAt;
const s_padStart = StringProto.padStart;
const a_sort = Array_.prototype.sort, a_join = Array_.prototype.join;
const DateNow = Date_.now;
const getPrototypeOf = Object_.getPrototypeOf, setPrototypeOf = Object_.setPrototypeOf;
const ObjectPrototype = Object_.prototype;
const RegExpProto = RegExp.prototype, r_exec = RegExpProto.exec;
const zoneCanonical = I.zoneCanonical, zoneOffset = I.zoneOffset, zoneHost = I.zoneHost;

// THE DATA, loaded by the first construction in a realm (INTL.md § 6, the
// § 9.3 ruling): the constructors and prototypes are installed whole and
// eagerly, and a realm that never constructs one never evaluates CLDR's
// tables. Every door into them calls loadData first: the constructors and
// supportedLocalesOf; a method has an instance, so its realm has loaded.
let ALIASES, U_VALUE_ALIASES, LIKELY, G, Z, IV, IV_FALLBACK, DAY_PERIOD_RULES;
let dataLoaded = false;
function loadData() {
    if (dataLoaded) return;
    const D = I.data();
    ALIASES = D.aliases; U_VALUE_ALIASES = D.uAliases; LIKELY = D.likely; G = D.gregorian; Z = D.zones;
    IV = G.intervalFormats; IV_FALLBACK = own(IV, 'intervalFormatFallback'); DAY_PERIOD_RULES = D.dayPeriods;
    dataLoaded = true;
}
// The default locale is the host's, and ECMA-402 requires it to be one the
// data holds: a host that names another is refused (the install fails),
// rather than every format reporting a locale it was not written in.
const DEFAULT_LOCALE = I.locale;

const slice = (s, a, b) => apply(s_slice, s, [a, b]);
const indexOf = (s, t, from) => apply(s_indexOf, s, [t, from]);
const lastIndexOf = (s, t) => apply(s_lastIndexOf, s, [t]);
const lower = s => apply(s_toLowerCase, s, []);
const upper = s => apply(s_toUpperCase, s, []);
const codeAt = (s, i) => apply(s_charCodeAt, s, [i]);
const pad = (s, n) => apply(s_padStart, s, [n, '0']);
const join = (a, sep) => apply(a_join, a, [sep]);
const sortStrings = a => apply(a_sort, a, [(x, y) => x < y ? -1 : x > y ? 1 : 0]);
const exec = (re, s) => apply(r_exec, re, [s]);
// split and a character swap written out: String.prototype.split and
// replace ask the separator or pattern for Symbol.split or Symbol.replace,
// which a page can put on String.prototype or RegExp.prototype.
function split(s, sep) {
    const out = [];
    for (let from = 0;;) {
        const at = indexOf(s, sep, from);
        if (at < 0) { push(out, slice(s, from, s.length)); return out; }
        push(out, slice(s, from, at));
        from = at + sep.length;
    }
}
// `s` with each character the table names replaced by its entry.
function swapChars(s, table) {
    let out = '';
    for (let i = 0; i < s.length; i++) {
        const c = fromCharCode(codeAt(s, i));
        const r = own(table, c);
        out += r !== undefined ? r : c;
    }
    return out;
}
// CreateDataProperty: a new own data property, never a [[Set]] that a
// setter a page put on Object.prototype or Array.prototype could answer.
function put(o, k, v) { defineProperty(o, k, {__proto__: null, value: v, writable: true, enumerable: true, configurable: true}); }
function push(a, v) { put(a, a.length, v); }
function contains(a, v) { for (let i = 0; i < a.length; i++) if (a[i] === v) return true; return false; }
const hasOwn = (o, k) => getOwnPropertyDescriptor(o, k) !== undefined;

// A data record's member, or undefined, never a member of Object.prototype.
function own(o, k) { const d = o !== undefined && o !== null ? getOwnPropertyDescriptor(o, k) : undefined; return d === undefined ? undefined : d.value; }

// A function defined as a built-in method is: non-enumerable, with its
// name and length; a class's members are defined the same way below.
function method(target, name, fn, length) {
    defineProperty(fn, 'name', {__proto__: null, value: name, configurable: true});
    defineProperty(fn, 'length', {__proto__: null, value: length, configurable: true});
    defineProperty(target, name, {__proto__: null, value: fn, writable: true, enumerable: false, configurable: true});
}
function getter(target, name, fn) {
    const get = getOwnPropertyDescriptor({__proto__: null, get [name]() { return apply(fn, this, []); }}, name).get;
    defineProperty(target, name, {__proto__: null, get: get, enumerable: false, configurable: true});
}
// The object `new` made for a constructor (the engine's
// OrdinaryCreateFromConstructor, which read newTarget.prototype once, as
// ECMA-402 reads it). Where that was no object, the engine fell back to
// Object.prototype and ECMA-402 to the constructor's own prototype, which
// this restores; a newTarget whose prototype IS Object.prototype gets the
// constructor's too, the one case the two cannot be told apart.
function constructed(made, intrinsic) {
    if (getPrototypeOf(made) === ObjectPrototype) setPrototypeOf(made, intrinsic);
    return made;
}
// A method that is not a constructor (a built-in method has no
// [[Construct]]): an arrow, or a concise method, never a function
// expression.
function methodOf(f) { return {m(...rest) { return apply(f, this, rest); }}.m; }

// ── ECMAScript abstract operations, as the engine does them ─────────────
function ToString(v) {
    if (typeof v === 'symbol') throw new TypeError_('Cannot convert a Symbol to a string');
    return String_(v);
}
function ToNumber(v) { return +v; }
function ToObject(v) {
    if (v === undefined || v === null) throw new TypeError_('Cannot convert undefined or null to an object');
    return Object_(v);
}
function ToLength(v) {
    let n = ToNumber(v);
    if (isNaN_(n) || n <= 0) return 0;
    n = trunc(n);
    return n > 9007199254740991 ? 9007199254740991 : n;
}
function isObject(v) { return (typeof v === 'object' && v !== null) || typeof v === 'function'; }

// ── Options (ECMA-402 § 9.2) ────────────────────────────────────────────
// GetOption: the option read once, converted to `type`, refused when not
// one of `values`; `fallback` when absent. REQUIRED throws instead.
const REQUIRED = {__proto__: null};
function GetOption(options, property, type, values, fallback) {
    let value = options[property];
    if (value === undefined) {
        if (fallback === REQUIRED) throw new RangeError_(property + ' is required');
        return fallback;
    }
    if (type === 'boolean') value = !!value;
    else if (type === 'number') {
        value = ToNumber(value);
        if (isNaN_(value)) throw new RangeError_(property + ' is not a number');
    } else value = ToString(value);
    if (values !== undefined && !contains(values, value))
        throw new RangeError_('Value ' + value + ' out of range for ' + property);
    return value;
}
function DefaultNumberOption(value, minimum, maximum, fallback) {
    if (value === undefined) return fallback;
    value = ToNumber(value);
    if (isNaN_(value) || value < minimum || value > maximum)
        throw new RangeError_('Value out of range');
    return floor(value);
}
function GetNumberOption(options, property, minimum, maximum, fallback) {
    return DefaultNumberOption(options[property], minimum, maximum, fallback);
}
function CoerceOptionsToObject(options) {
    return options === undefined ? objectCreate(null) : ToObject(options);
}

// ── Language tags (UTS 35 § 3; ECMA-402 § 6.2) ──────────────────────────
// A tag parsed into its parts, or null when it is not a structurally valid
// unicode_locale_id. Every subtag is lowercased here; canonical case is
// put back when the tag is written.
const isAlpha = c => (c >= 65 && c <= 90) || (c >= 97 && c <= 122);
const isDigit = c => c >= 48 && c <= 57;
function allOf(s, test) {
    for (let i = 0; i < s.length; i++) if (!test(codeAt(s, i))) return false;
    return true;
}
const alpha = s => allOf(s, isAlpha);
const digits = s => allOf(s, isDigit);
const alnum = s => allOf(s, c => isAlpha(c) || isDigit(c));
function isLanguage(s) { return alpha(s) && (s.length === 2 || s.length === 3 || (s.length >= 5 && s.length <= 8)); }
function isScript(s) { return s.length === 4 && alpha(s); }
function isRegion(s) { return (s.length === 2 && alpha(s)) || (s.length === 3 && digits(s)); }
function isVariant(s) {
    return (s.length >= 5 && s.length <= 8 && alnum(s)) || (s.length === 4 && isDigit(codeAt(s, 0)) && alnum(s));
}
function isTypeSequence(s) {
    // (3*8alphanum) *("-" (3*8alphanum))
    const parts = split(s, '-');
    for (let i = 0; i < parts.length; i++)
        if (parts[i].length < 3 || parts[i].length > 8 || !alnum(parts[i])) return false;
    return parts.length > 0;
}

function parseTag(tag) {
    if (typeof tag !== 'string' || tag.length === 0) return null;
    const parts = split(lower(tag), '-');
    let i = 0;
    const t = {__proto__: null, language: '', script: '', region: '', variants: [], extensions: [], privateuse: ''};
    // unicode_language_id: a language is required, as ECMA-402 requires
    // (no bare script, no "root").
    if (!isLanguage(parts[0])) return null;
    t.language = parts[i++];
    if (i < parts.length && isScript(parts[i])) t.script = parts[i++];
    if (i < parts.length && isRegion(parts[i])) t.region = parts[i++];
    while (i < parts.length && isVariant(parts[i])) {
        if (contains(t.variants, parts[i])) return null;
        push(t.variants, parts[i++]);
    }
    const singletons = [];
    while (i < parts.length) {
        const s = parts[i];
        if (s.length !== 1 || !alnum(s)) return null;
        if (s === 'x') {
            i++;
            const pu = [];
            while (i < parts.length && parts[i].length >= 1 && parts[i].length <= 8 && alnum(parts[i])) push(pu, parts[i++]);
            if (pu.length === 0 || i !== parts.length) return null;
            t.privateuse = join(pu, '-');
            break;
        }
        if (contains(singletons, s)) return null;
        push(singletons, s);
        i++;
        const start = i;
        if (s === 'u') {
            while (i < parts.length && parts[i].length >= 3 && parts[i].length <= 8 && alnum(parts[i])) i++;
            while (i < parts.length && parts[i].length === 2 && alnum(parts[i]) && isAlpha(codeAt(parts[i], 1))) {
                i++;
                while (i < parts.length && parts[i].length >= 3 && parts[i].length <= 8 && alnum(parts[i])) i++;
            }
        } else if (s === 't') {
            // tlang (a unicode_language_id without extensions), then tfields
            if (i < parts.length && isLanguage(parts[i])) {
                i++;
                if (i < parts.length && isScript(parts[i])) i++;
                if (i < parts.length && isRegion(parts[i])) i++;
                const seen = [];
                while (i < parts.length && isVariant(parts[i])) {
                    if (contains(seen, parts[i])) return null;
                    push(seen, parts[i++]);
                }
            }
            while (i < parts.length && parts[i].length === 2 && isAlpha(codeAt(parts[i], 0)) && isDigit(codeAt(parts[i], 1))) {
                i++;
                const vstart = i;
                while (i < parts.length && parts[i].length >= 3 && parts[i].length <= 8 && alnum(parts[i])) i++;
                if (i === vstart) return null;
            }
        } else {
            while (i < parts.length && parts[i].length >= 2 && parts[i].length <= 8 && alnum(parts[i])) i++;
        }
        if (i === start) return null;
        push(t.extensions, {__proto__: null, singleton: s, subtags: slicePart(parts, start, i)});
    }
    return t;
}
function slicePart(parts, from, to) { const out = []; for (let i = from; i < to; i++) push(out, parts[i]); return out; }

function IsStructurallyValidLanguageTag(tag) { return parseTag(tag) !== null; }

// The case UTS 35 writes each subtag in.
function titleCase(s) { return upper(slice(s, 0, 1)) + slice(s, 1); }

// Alias replacement (UTS 35 Annex C, ECMA-402 CanonicalizeUnicodeLocaleId),
// over CLDR's language, script, territory and variant aliases.
function replaceAliases(t) {
    // Language aliases: the longest source that matches (language plus its
    // variants or region, or the language alone), its replacement's own
    // script, region and variants added where the tag has none.
    const langAliases = ALIASES.languageAlias;
    let changed = true, guard = 0;
    while (changed && guard++ < 8) {
        changed = false;
        let match = null, matchedVariant = null;
        for (let v = 0; v < t.variants.length && match === null; v++) {
            const key = t.language + '-' + t.variants[v];
            if (own(langAliases, key) !== undefined) { match = own(langAliases, key); matchedVariant = t.variants[v]; }
        }
        if (match === null && t.region !== '' && own(langAliases, t.language + '-' + t.region) !== undefined)
            match = own(langAliases, t.language + '-' + t.region), matchedVariant = '';
        if (match === null && own(langAliases, t.language) !== undefined) match = own(langAliases, t.language);
        if (match !== null) {
            const r = parseTag(swapChars(match, {__proto__: null, _: '-'}));
            if (r === null) break;
            if (matchedVariant) {
                const kept = [];
                for (let v = 0; v < t.variants.length; v++) if (t.variants[v] !== matchedVariant) push(kept, t.variants[v]);
                t.variants = kept;
            } else if (matchedVariant === '') t.region = '';
            t.language = r.language;
            if (t.script === '' && r.script !== '') t.script = r.script;
            if (t.region === '' && r.region !== '') t.region = r.region;
            for (let v = 0; v < r.variants.length; v++) if (!contains(t.variants, r.variants[v])) push(t.variants, r.variants[v]);
            changed = own(langAliases, t.language) !== undefined && own(langAliases, t.language) !== match;
        }
    }
    if (t.script !== '') {
        const s = own(ALIASES.scriptAlias, titleCase(t.script)) || own(ALIASES.scriptAlias, t.script);
        if (s !== undefined) t.script = lower(s);
    }
    if (t.region !== '') {
        const r = own(ALIASES.territoryAlias, upper(t.region)) || own(ALIASES.territoryAlias, t.region);
        if (r !== undefined) {
            const choices = split(r, ' ');
            let pick = choices[0];
            if (choices.length > 1) {
                // The region the language is likeliest in, when it is among
                // the choices (UTS 35 Annex C.10), else the first.
                const likely = own(LIKELY, t.language);
                if (likely !== undefined) {
                    const lr = parseTag(likely);
                    for (let c = 0; c < choices.length; c++) if (lower(choices[c]) === lr.region) pick = choices[c];
                }
            }
            t.region = lower(pick);
        }
    }
    const variants = [];
    for (let v = 0; v < t.variants.length; v++) {
        const a = own(ALIASES.variantAlias, t.variants[v]);
        const name = a !== undefined && a !== null ? lower(a) : t.variants[v];
        if (!contains(variants, name)) push(variants, name);
    }
    t.variants = variants;
}

// The canonical value of a -u- keyword (UTS 35 § 3.6.4): CLDR's bcp47
// aliases (a deprecated value's preferred one, a legacy name's value),
// generated into the data; a value not listed is its own canonical form.
function canonicalUValue(key, value) {
    const table = own(U_VALUE_ALIASES, key);
    const a = table !== undefined ? own(table, value) : undefined;
    return a !== undefined ? a : value;
}

// The -u- extension's attributes and keywords (UTS 35 § 3.6.1): first
// keyword of a key wins, "true" written as nothing.
function uComponents(subtags) {
    const attributes = [], keywords = [];
    let i = 0;
    while (i < subtags.length && subtags[i].length !== 2) {
        if (!contains(attributes, subtags[i])) push(attributes, subtags[i]);
        i++;
    }
    while (i < subtags.length) {
        const key = subtags[i++];
        const value = [];
        while (i < subtags.length && subtags[i].length !== 2) push(value, subtags[i++]);
        let found = false;
        for (let k = 0; k < keywords.length; k++) if (keywords[k].key === key) found = true;
        if (!found) push(keywords, {__proto__: null, key: key, value: join(value, '-')});
    }
    return {__proto__: null, attributes: attributes, keywords: keywords};
}

function writeLanguageId(t) {
    let s = t.language;
    if (t.script !== '') s += '-' + titleCase(t.script);
    if (t.region !== '') s += '-' + upper(t.region);
    for (let v = 0; v < t.variants.length; v++) s += '-' + t.variants[v];
    return s;
}

// The canonical form of a parsed tag (ECMA-402 CanonicalizeUnicodeLocaleId).
function canonicalize(t) {
    replaceAliases(t);
    sortStrings(t.variants);
    let s = writeLanguageId(t);
    const exts = [];
    for (let e = 0; e < t.extensions.length; e++) push(exts, t.extensions[e]);
    apply(a_sort, exts, [(a, b) => a.singleton < b.singleton ? -1 : 1]);
    for (let e = 0; e < exts.length; e++) {
        const ext = exts[e];
        if (ext.singleton === 'u') {
            const c = uComponents(ext.subtags);
            apply(a_sort, c.keywords, [(a, b) => a.key < b.key ? -1 : a.key > b.key ? 1 : 0]);
            let u = '-u';
            sortStrings(c.attributes);
            for (let a = 0; a < c.attributes.length; a++) u += '-' + c.attributes[a];
            for (let k = 0; k < c.keywords.length; k++) {
                let value = canonicalUValue(c.keywords[k].key, c.keywords[k].value);
                u += '-' + c.keywords[k].key;
                if (value !== '' && value !== 'true') u += '-' + value;
            }
            s += u;
        } else if (ext.singleton === 't') {
            s += '-t' + canonicalT(ext.subtags);
        } else {
            s += '-' + ext.singleton + '-' + join(ext.subtags, '-');
        }
    }
    if (t.privateuse !== '') s += '-x-' + t.privateuse;
    return s;
}
// The -t- extension: its tlang canonicalized (lowercase), its fields sorted.
function canonicalT(subtags) {
    let i = 0;
    const lang = [];
    while (i < subtags.length && !(subtags[i].length === 2 && isDigit(codeAt(subtags[i], 1)))) push(lang, subtags[i++]);
    let out = '';
    if (lang.length > 0) {
        const t = parseTag(join(lang, '-'));
        replaceAliases(t);
        sortStrings(t.variants);
        out += '-' + lower(writeLanguageId(t));
    }
    const fields = [];
    while (i < subtags.length) {
        const key = subtags[i++];
        const value = [];
        while (i < subtags.length && !(subtags[i].length === 2 && isDigit(codeAt(subtags[i], 1)))) push(value, subtags[i++]);
        push(fields, {__proto__: null, key: key, value: join(value, '-')});
    }
    apply(a_sort, fields, [(a, b) => a.key < b.key ? -1 : a.key > b.key ? 1 : 0]);
    for (let f = 0; f < fields.length; f++) {
        let value = fields[f].value;
        if (fields[f].key === 'm0' && value === 'names') value = 'prprname';
        out += '-' + fields[f].key + '-' + value;
    }
    return out;
}

function CanonicalizeUnicodeLocaleId(tag) { return canonicalize(parseTag(tag)); }

// ── Locale lists and resolution (ECMA-402 § 9.2) ────────────────────────
// %Intl.Locale%'s instances carry their tag in this map's entries.
const LOCALES = new WeakMap();
const wm_get = WeakMap.prototype.get, wm_set = WeakMap.prototype.set, wm_has = WeakMap.prototype.has;
const slots = (map, o) => isObject(o) ? apply(wm_get, map, [o]) : undefined;

function CanonicalizeLocaleList(locales) {
    const seen = [];
    if (locales === undefined) return seen;
    let O;
    if (typeof locales === 'string' || slots(LOCALES, locales) !== undefined) O = [locales];
    else O = ToObject(locales);
    const len = ToLength(O.length);
    for (let k = 0; k < len; k++) {
        const Pk = ToString(k);
        if (!(Pk in O)) continue;
        const kValue = O[Pk];
        if (typeof kValue !== 'string' && !isObject(kValue))
            throw new TypeError_('Language ID should be string or object.');
        const locale = slots(LOCALES, kValue);
        const tag = locale !== undefined ? locale.locale : ToString(kValue);
        if (!IsStructurallyValidLanguageTag(tag)) throw new RangeError_('Incorrect locale information provided');
        const canonical = CanonicalizeUnicodeLocaleId(tag);
        if (!contains(seen, canonical)) push(seen, canonical);
    }
    return seen;
}

// The locales the data holds (INTL.md § 3).
const AVAILABLE = ['en', 'en-US'];
if (!contains(AVAILABLE, DEFAULT_LOCALE))
    throw new RangeError_('Intl: the default locale ' + DEFAULT_LOCALE + ' is not one the data holds');

function removeUnicodeExtensions(locale) {
    const t = parseTag(locale);
    let out = writeLanguageId(t);
    for (let e = 0; e < t.extensions.length; e++)
        if (t.extensions[e].singleton !== 'u') out += '-' + t.extensions[e].singleton + '-' + join(t.extensions[e].subtags, '-');
    if (t.privateuse !== '') out += '-x-' + t.privateuse;
    return out;
}
function unicodeExtension(locale) {
    const t = parseTag(locale);
    for (let e = 0; e < t.extensions.length; e++)
        if (t.extensions[e].singleton === 'u') return t.extensions[e].subtags;
    return null;
}
function BestAvailableLocale(available, locale) {
    let candidate = locale;
    for (;;) {
        if (contains(available, candidate)) return candidate;
        let pos = lastIndexOf(candidate, '-');
        if (pos < 0) return undefined;
        if (pos >= 2 && codeAt(candidate, pos - 2) === 45) pos -= 2;
        candidate = slice(candidate, 0, pos);
    }
}
function LookupMatcher(available, requested) {
    for (let i = 0; i < requested.length; i++) {
        const noExt = removeUnicodeExtensions(requested[i]);
        const found = BestAvailableLocale(available, noExt);
        if (found !== undefined) return {__proto__: null, locale: found, extension: unicodeExtension(requested[i])};
    }
    return {__proto__: null, locale: DEFAULT_LOCALE, extension: null};
}

// ResolveLocale: the found locale, and each relevant key's value from the
// locale's data, the -u- keyword or the option, in the spec's order.
function ResolveLocale(available, requested, options, relevantKeys, localeData) {
    const r = LookupMatcher(available, requested);
    const found = r.locale;
    const keywords = r.extension !== null ? uComponents(r.extension).keywords : [];
    const result = {__proto__: null};
    const supported = [];
    for (let k = 0; k < relevantKeys.length; k++) {
        const key = relevantKeys[k];
        const keyData = localeData(found, key);
        let value = keyData[0];
        let supportedKeyword = null;
        for (let w = 0; w < keywords.length; w++) {
            if (keywords[w].key !== key) continue;
            const requestedValue = canonicalUValue(key, keywords[w].value);
            if (requestedValue !== '') {
                if (contains(keyData, requestedValue)) { value = requestedValue; supportedKeyword = {__proto__: null, key: key, value: value}; }
            } else if (contains(keyData, 'true')) {
                value = 'true'; supportedKeyword = {__proto__: null, key: key, value: ''};
            }
        }
        let optionsValue = own(options, key);
        if (typeof optionsValue === 'string') {
            optionsValue = canonicalUValue(key, lower(optionsValue));
            if (optionsValue === '') optionsValue = 'true';
        }
        if (optionsValue !== undefined && optionsValue !== value && contains(keyData, optionsValue)) {
            value = optionsValue; supportedKeyword = null;
        }
        if (supportedKeyword !== null) push(supported, supportedKeyword);
        result[key] = value;
    }
    let locale = found;
    if (supported.length > 0) {
        let u = '-u';
        apply(a_sort, supported, [(a, b) => a.key < b.key ? -1 : 1]);
        for (let s = 0; s < supported.length; s++) {
            u += '-' + supported[s].key;
            if (supported[s].value !== '' && supported[s].value !== 'true') u += '-' + supported[s].value;
        }
        locale = CanonicalizeUnicodeLocaleId(locale + u);
    }
    result.locale = locale;
    result.dataLocale = found;
    return result;
}

function SupportedLocales(available, requested, options) {
    options = CoerceOptionsToObject(options);
    GetOption(options, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit');
    const subset = [];
    for (let i = 0; i < requested.length; i++)
        if (BestAvailableLocale(available, removeUnicodeExtensions(requested[i])) !== undefined) push(subset, requested[i]);
    return subset;
}
function supportedLocalesOf(locales, options) {
    loadData();
    const list = SupportedLocales(AVAILABLE, CanonicalizeLocaleList(locales), options);
    const out = [];
    for (let i = 0; i < list.length; i++) push(out, list[i]);
    return out;
}

// ── Intl.Locale (ECMA-402 § 14) ─────────────────────────────────────────

const LOCALE_KEYS = ['ca', 'co', 'hc', 'kf', 'kn', 'nu'];

function updateLanguageId(tag, options) {
    const t = parseTag(tag);
    if (t === null) throw new RangeError_('Incorrect locale information provided');
    const language = GetOption(options, 'language', 'string', undefined, undefined);
    if (language !== undefined && !isLanguage(lower(language))) throw new RangeError_('Incorrect locale information provided');
    const script = GetOption(options, 'script', 'string', undefined, undefined);
    if (script !== undefined && !isScript(lower(script))) throw new RangeError_('Incorrect locale information provided');
    const region = GetOption(options, 'region', 'string', undefined, undefined);
    if (region !== undefined && !isRegion(lower(region))) throw new RangeError_('Incorrect locale information provided');
    const variants = GetOption(options, 'variants', 'string', undefined, undefined);
    let variantList = null;
    if (variants !== undefined) {
        if (variants === '') throw new RangeError_('Incorrect locale information provided');
        variantList = split(lower(variants), '-');
        for (let v = 0; v < variantList.length; v++)
            if (!isVariant(variantList[v]) || contains(slicePart(variantList, 0, v), variantList[v]))
                throw new RangeError_('Incorrect locale information provided');
    }
    if (language !== undefined) t.language = lower(language);
    if (script !== undefined) t.script = lower(script);
    if (region !== undefined) t.region = lower(region);
    if (variantList !== null) t.variants = variantList;
    let out = writeLanguageId(t);
    for (let e = 0; e < t.extensions.length; e++) out += '-' + t.extensions[e].singleton + '-' + join(t.extensions[e].subtags, '-');
    if (t.privateuse !== '') out += '-x-' + t.privateuse;
    if (!IsStructurallyValidLanguageTag(out)) throw new RangeError_('Incorrect locale information provided');
    return out;
}

// MakeLocaleRecord: the tag with each relevant key's option written into
// its -u- extension, canonicalized.
function makeLocaleRecord(tag, opt) {
    const t = parseTag(tag);
    let attributes = [], keywords = [];
    const others = [];
    for (let e = 0; e < t.extensions.length; e++) {
        if (t.extensions[e].singleton === 'u') {
            const c = uComponents(t.extensions[e].subtags);
            attributes = c.attributes; keywords = c.keywords;
        } else push(others, t.extensions[e]);
    }
    const result = {__proto__: null};
    for (let k = 0; k < LOCALE_KEYS.length; k++) {
        const key = LOCALE_KEYS[k];
        let value;
        let entry = null;
        for (let w = 0; w < keywords.length; w++) if (keywords[w].key === key) entry = keywords[w];
        // A canonical tag writes a "true" value as no value (UTS 35): the
        // record holds what the canonical tag says.
        if (entry !== null) value = entry.value === 'true' ? '' : entry.value;
        const override = own(opt, key);
        if (override !== undefined) {
            value = override;
            if (entry !== null) entry.value = value;
            else push(keywords, {__proto__: null, key: key, value: value});
        }
        result[key] = value === undefined ? undefined : canonicalUValue(key, value);
    }
    t.extensions = others;
    if (attributes.length > 0 || keywords.length > 0) {
        const sub = [];
        for (let a = 0; a < attributes.length; a++) push(sub, attributes[a]);
        for (let w = 0; w < keywords.length; w++) {
            push(sub, keywords[w].key);
            if (keywords[w].value !== '') {
                const vs = split(keywords[w].value, '-');
                for (let v = 0; v < vs.length; v++) push(sub, vs[v]);
            }
        }
        push(t.extensions, {__proto__: null, singleton: 'u', subtags: sub});
    }
    result.locale = canonicalize(t);
    return result;
}

function thisLocale(o, name) {
    const s = slots(LOCALES, o);
    if (s === undefined) throw new TypeError_('Method Intl.Locale.prototype.' + name + ' called on incompatible receiver');
    return s;
}

// Likely subtags (UTS 35 § 4.4), over the rows the data keeps, English's
// and every `und` row (INTL.md § 7): a tag no row matches is its own
// maximum.
function addLikely(t) {
    const tries = [];
    const l = t.language, s = t.script, r = t.region;
    // The data holds English's rows and every `und` row. A language it
    // does not hold would have stopped at its own row, never reaching
    // `und`'s, so it is its own maximum.
    if (l !== 'und' && own(LIKELY, l) === undefined) return null;
    push(tries, l + (s ? '-' + s : '') + (r ? '-' + r : ''));
    if (s) push(tries, l + '-' + s);
    if (r) push(tries, l + '-' + r);
    push(tries, l);
    if (s) push(tries, 'und-' + s);
    for (let i = 0; i < tries.length; i++) {
        let key = tries[i];
        // The data's keys are written in canonical case.
        const k = parseTag(key);
        if (k === null) continue;
        key = k.language + (k.script ? '-' + titleCase(k.script) : '') + (k.region ? '-' + upper(k.region) : '');
        const hit = own(LIKELY, key);
        if (hit !== undefined) {
            const m = parseTag(hit);
            return {__proto__: null, language: l === 'und' ? m.language : l, script: s || m.script, region: r || m.region};
        }
    }
    return null;
}

function Locale(tag, options) {
    if (new.target === undefined) throw new TypeError_("Constructor Intl.Locale requires 'new'");
    const locale = constructed(this, LocalePrototype);
    loadData();
    if (typeof tag !== 'string' && !isObject(tag)) throw new TypeError_('First argument to Intl.Locale constructor can\'t be empty or missing');
    const existing = slots(LOCALES, tag);
    tag = existing !== undefined ? existing.locale : ToString(tag);
    options = CoerceOptionsToObject(options);
    // Canonicalized before the options apply and again after (aliases
    // first: "cel-gaulish" is "xtg" before a `language` option replaces it).
    if (!IsStructurallyValidLanguageTag(tag)) throw new RangeError_('Incorrect locale information provided');
    tag = updateLanguageId(CanonicalizeUnicodeLocaleId(tag), options);
    const opt = {__proto__: null};
    const calendar = GetOption(options, 'calendar', 'string', undefined, undefined);
    if (calendar !== undefined && !isTypeSequence(lower(calendar))) throw new RangeError_('Incorrect calendar');
    opt.ca = calendar === undefined ? undefined : lower(calendar);
    const collation = GetOption(options, 'collation', 'string', undefined, undefined);
    if (collation !== undefined && !isTypeSequence(lower(collation))) throw new RangeError_('Incorrect collation');
    opt.co = collation === undefined ? undefined : lower(collation);
    opt.hc = GetOption(options, 'hourCycle', 'string', ['h11', 'h12', 'h23', 'h24'], undefined);
    opt.kf = GetOption(options, 'caseFirst', 'string', ['upper', 'lower', 'false'], undefined);
    const kn = GetOption(options, 'numeric', 'boolean', undefined, undefined);
    opt.kn = kn === undefined ? undefined : ToString(kn);
    const nu = GetOption(options, 'numberingSystem', 'string', undefined, undefined);
    if (nu !== undefined && !isTypeSequence(lower(nu))) throw new RangeError_('Incorrect numberingSystem');
    opt.nu = nu === undefined ? undefined : lower(nu);
    const r = makeLocaleRecord(tag, opt);
    apply(wm_set, LOCALES, [locale, r]);
    return locale;
}
const LocalePrototype = Locale.prototype;
defineProperty(Locale, 'prototype', {__proto__: null, writable: false, enumerable: false, configurable: false});
defineProperty(Locale, 'length', {__proto__: null, value: 1, configurable: true});

function localeParts(o, name) { return parseTag(thisLocale(o, name).locale); }
method(LocalePrototype, 'toString', methodOf(function () { return thisLocale(this, 'toString').locale; }), 0);
getter(LocalePrototype, 'baseName', function () { return writeLanguageId(localeParts(this, 'baseName')); });
getter(LocalePrototype, 'calendar', function () { return thisLocale(this, 'calendar').ca; });
getter(LocalePrototype, 'caseFirst', function () { return thisLocale(this, 'caseFirst').kf; });
getter(LocalePrototype, 'collation', function () { return thisLocale(this, 'collation').co; });
getter(LocalePrototype, 'hourCycle', function () { return thisLocale(this, 'hourCycle').hc; });
getter(LocalePrototype, 'numeric', function () { const kn = thisLocale(this, 'numeric').kn; return kn === '' || kn === 'true'; });
getter(LocalePrototype, 'numberingSystem', function () { return thisLocale(this, 'numberingSystem').nu; });
getter(LocalePrototype, 'language', function () { return localeParts(this, 'language').language; });
getter(LocalePrototype, 'script', function () { const s = localeParts(this, 'script').script; return s === '' ? undefined : titleCase(s); });
getter(LocalePrototype, 'region', function () { const r = localeParts(this, 'region').region; return r === '' ? undefined : upper(r); });
getter(LocalePrototype, 'variants', function () { const v = localeParts(this, 'variants').variants; return v.length === 0 ? undefined : join(v, '-'); });
method(LocalePrototype, 'maximize', methodOf(function () {
    const r = thisLocale(this, 'maximize');
    const t = parseTag(r.locale);
    const likely = addLikely(t);
    if (likely !== null) { t.language = likely.language; t.script = likely.script; t.region = likely.region; }
    return new Locale(canonicalize(t));
}), 0);
method(LocalePrototype, 'minimize', methodOf(function () {
    const r = thisLocale(this, 'minimize');
    const t = parseTag(r.locale);
    const max = addLikely(t);
    if (max !== null) {
        const trials = [{__proto__: null, language: max.language, script: '', region: ''},
                        {__proto__: null, language: max.language, script: '', region: max.region},
                        {__proto__: null, language: max.language, script: max.script, region: ''}];
        for (let i = 0; i < trials.length; i++) {
            const m = addLikely(trials[i]);
            if (m !== null && m.language === max.language && m.script === max.script && m.region === max.region) {
                t.language = trials[i].language; t.script = trials[i].script; t.region = trials[i].region;
                return new Locale(canonicalize(t));
            }
        }
        t.language = max.language; t.script = max.script; t.region = max.region;
    }
    return new Locale(canonicalize(t));
}), 0);
defineProperty(LocalePrototype, toStringTag, {__proto__: null, value: 'Intl.Locale', writable: false, enumerable: false, configurable: true});

// ── Patterns: ICU's DateTimePatternGenerator over CLDR's en data ─────────
// Other browsers turn a DateTimeFormat's options into a pattern with ICU's
// generator: a SKELETON (the fields asked for, each a letter repeated) is
// matched against CLDR's availableFormats by a distance over field types,
// the best pattern's fields are widened to what was asked, a field the
// best pattern lacks is appended with an appendItems pattern, and a date
// and a time part found apart are joined with the dateTimeFormats glue.
// This is that algorithm (ICU's dtptngen.cpp), field table and all, so
// the pattern it finds is the pattern Chrome finds.
const DT_NUMERIC = 0x100, DT_NARROW = -0x101, DT_SHORTER = -0x102, DT_SHORT = -0x103, DT_LONG = -0x104, DT_DELTA = 0x10;
const F_ERA = 0, F_YEAR = 1, F_QUARTER = 2, F_MONTH = 3, F_WEEK_OF_YEAR = 4, F_WEEK_OF_MONTH = 5, F_WEEKDAY = 6,
      F_DAY_OF_YEAR = 7, F_DAY_OF_WEEK_IN_MONTH = 8, F_DAY = 9, F_DAYPERIOD = 10, F_HOUR = 11, F_MINUTE = 12,
      F_SECOND = 13, F_FRACTION = 14, F_ZONE = 15, FIELD_COUNT = 16;
const DATE_MASK = (1 << F_DAYPERIOD) - 1, TIME_MASK = ((1 << FIELD_COUNT) - 1) & ~DATE_MASK;
// [pattern char, field, type, minimum length], ICU's dtTypes in its order:
// rows of one char are adjacent and by minimum length.
const DT_TYPES = [
    ['G', F_ERA, DT_SHORT, 1], ['G', F_ERA, DT_LONG, 4], ['G', F_ERA, DT_NARROW, 5],
    ['y', F_YEAR, DT_NUMERIC, 1], ['Y', F_YEAR, DT_NUMERIC + DT_DELTA, 1], ['u', F_YEAR, DT_NUMERIC + 2 * DT_DELTA, 1],
    ['r', F_YEAR, DT_NUMERIC + 3 * DT_DELTA, 1],
    ['U', F_YEAR, DT_SHORT, 1], ['U', F_YEAR, DT_LONG, 4], ['U', F_YEAR, DT_NARROW, 5],
    ['Q', F_QUARTER, DT_NUMERIC, 1], ['Q', F_QUARTER, DT_SHORT, 3], ['Q', F_QUARTER, DT_LONG, 4], ['Q', F_QUARTER, DT_NARROW, 5],
    ['q', F_QUARTER, DT_NUMERIC + DT_DELTA, 1], ['q', F_QUARTER, DT_SHORT - DT_DELTA, 3],
    ['q', F_QUARTER, DT_LONG - DT_DELTA, 4], ['q', F_QUARTER, DT_NARROW - DT_DELTA, 5],
    ['M', F_MONTH, DT_NUMERIC, 1], ['M', F_MONTH, DT_SHORT, 3], ['M', F_MONTH, DT_LONG, 4], ['M', F_MONTH, DT_NARROW, 5],
    ['L', F_MONTH, DT_NUMERIC + DT_DELTA, 1], ['L', F_MONTH, DT_SHORT - DT_DELTA, 3],
    ['L', F_MONTH, DT_LONG - DT_DELTA, 4], ['L', F_MONTH, DT_NARROW - DT_DELTA, 5],
    ['l', F_MONTH, DT_NUMERIC + DT_DELTA, 1],
    ['w', F_WEEK_OF_YEAR, DT_NUMERIC, 1],
    ['W', F_WEEK_OF_MONTH, DT_NUMERIC, 1],
    ['E', F_WEEKDAY, DT_SHORT, 1], ['E', F_WEEKDAY, DT_LONG, 4], ['E', F_WEEKDAY, DT_NARROW, 5], ['E', F_WEEKDAY, DT_SHORTER, 6],
    ['c', F_WEEKDAY, DT_NUMERIC + 2 * DT_DELTA, 1], ['c', F_WEEKDAY, DT_SHORT - 2 * DT_DELTA, 3],
    ['c', F_WEEKDAY, DT_LONG - 2 * DT_DELTA, 4], ['c', F_WEEKDAY, DT_NARROW - 2 * DT_DELTA, 5],
    ['c', F_WEEKDAY, DT_SHORTER - 2 * DT_DELTA, 6],
    ['e', F_WEEKDAY, DT_NUMERIC + DT_DELTA, 1], ['e', F_WEEKDAY, DT_SHORT - DT_DELTA, 3],
    ['e', F_WEEKDAY, DT_LONG - DT_DELTA, 4], ['e', F_WEEKDAY, DT_NARROW - DT_DELTA, 5],
    ['e', F_WEEKDAY, DT_SHORTER - DT_DELTA, 6],
    ['d', F_DAY, DT_NUMERIC, 1], ['g', F_DAY, DT_NUMERIC + DT_DELTA, 1],
    ['D', F_DAY_OF_YEAR, DT_NUMERIC, 1],
    ['F', F_DAY_OF_WEEK_IN_MONTH, DT_NUMERIC, 1],
    ['a', F_DAYPERIOD, DT_SHORT, 1], ['a', F_DAYPERIOD, DT_LONG, 4], ['a', F_DAYPERIOD, DT_NARROW, 5],
    ['b', F_DAYPERIOD, DT_SHORT - DT_DELTA, 1], ['b', F_DAYPERIOD, DT_LONG - DT_DELTA, 4],
    ['b', F_DAYPERIOD, DT_NARROW - DT_DELTA, 5],
    ['B', F_DAYPERIOD, DT_SHORT - 3 * DT_DELTA, 1], ['B', F_DAYPERIOD, DT_LONG - 3 * DT_DELTA, 4],
    ['B', F_DAYPERIOD, DT_NARROW - 3 * DT_DELTA, 5],
    ['H', F_HOUR, DT_NUMERIC + 10 * DT_DELTA, 1], ['k', F_HOUR, DT_NUMERIC + 11 * DT_DELTA, 1],
    ['h', F_HOUR, DT_NUMERIC, 1], ['K', F_HOUR, DT_NUMERIC + DT_DELTA, 1],
    ['J', F_HOUR, DT_NUMERIC + 5 * DT_DELTA, 1], ['j', F_HOUR, DT_NUMERIC + 6 * DT_DELTA, 1],
    ['C', F_HOUR, DT_NUMERIC + 7 * DT_DELTA, 1],
    ['m', F_MINUTE, DT_NUMERIC, 1],
    ['s', F_SECOND, DT_NUMERIC, 1], ['A', F_SECOND, DT_NUMERIC + DT_DELTA, 1],
    ['S', F_FRACTION, DT_NUMERIC, 1],
    ['v', F_ZONE, DT_SHORT - 2 * DT_DELTA, 1], ['v', F_ZONE, DT_LONG - 2 * DT_DELTA, 4],
    ['z', F_ZONE, DT_SHORT, 1], ['z', F_ZONE, DT_LONG, 4],
    ['Z', F_ZONE, DT_NARROW - DT_DELTA, 1], ['Z', F_ZONE, DT_LONG - DT_DELTA, 4], ['Z', F_ZONE, DT_SHORT - DT_DELTA, 5],
    ['O', F_ZONE, DT_SHORT - DT_DELTA, 1], ['O', F_ZONE, DT_LONG - DT_DELTA, 4],
    ['V', F_ZONE, DT_SHORT - DT_DELTA, 1], ['V', F_ZONE, DT_LONG - DT_DELTA, 2],
    ['V', F_ZONE, DT_LONG - 1 - DT_DELTA, 3], ['V', F_ZONE, DT_LONG - 2 - DT_DELTA, 4],
    ['X', F_ZONE, DT_NARROW - DT_DELTA, 1], ['X', F_ZONE, DT_SHORT - DT_DELTA, 2], ['X', F_ZONE, DT_LONG - DT_DELTA, 4],
    ['x', F_ZONE, DT_NARROW - DT_DELTA, 1], ['x', F_ZONE, DT_SHORT - DT_DELTA, 2], ['x', F_ZONE, DT_LONG - DT_DELTA, 4],
];
// ICU's getCanonicalIndex: the last row of the char whose minimum length
// the field's length reaches.
function canonicalRow(ch, len) {
    let best = -1;
    for (let i = 0; i < DT_TYPES.length; i++) {
        if (DT_TYPES[i][0] !== ch) continue;
        best = i;
        if (i + 1 < DT_TYPES.length && DT_TYPES[i + 1][0] === ch && DT_TYPES[i + 1][3] <= len) continue;
        return i;
    }
    return best;
}
const isPatternLetter = c => (c >= 65 && c <= 90) || (c >= 97 && c <= 122);
// A pattern's items: {ch, len} for a field, {lit} for literal text (quotes
// read: 'at' is the text at, '' an apostrophe).
function tokenize(pattern) {
    const items = [];
    let i = 0;
    while (i < pattern.length) {
        const c = codeAt(pattern, i);
        if (isPatternLetter(c)) {
            let j = i + 1;
            while (j < pattern.length && codeAt(pattern, j) === c) j++;
            push(items, {__proto__: null, ch: fromCharCode(c), len: j - i});
            i = j;
        } else if (c === 39) {
            let text = '', j = i + 1;
            if (j < pattern.length && codeAt(pattern, j) === 39) { text = "'"; i = j + 1; }
            else {
                for (;;) {
                    if (j >= pattern.length) break;
                    if (codeAt(pattern, j) === 39) {
                        if (j + 1 < pattern.length && codeAt(pattern, j + 1) === 39) { text += "'"; j += 2; continue; }
                        j++;
                        break;
                    }
                    text += fromCharCode(codeAt(pattern, j++));
                }
                i = j;
            }
            push(items, {__proto__: null, lit: text, quoted: true});
        } else {
            let j = i + 1;
            while (j < pattern.length && !isPatternLetter(codeAt(pattern, j)) && codeAt(pattern, j) !== 39) j++;
            push(items, {__proto__: null, lit: slice(pattern, i, j)});
            i = j;
        }
    }
    return items;
}
// Items written back as a pattern: literal text quoted where it holds a
// letter or an apostrophe.
function untokenize(items) {
    let out = '';
    for (let i = 0; i < items.length; i++) {
        const it = items[i];
        if (it.ch !== undefined) { for (let k = 0; k < it.len; k++) out += it.ch; continue; }
        let needsQuote = it.quoted === true;
        for (let k = 0; k < it.lit.length && !needsQuote; k++) needsQuote = isPatternLetter(codeAt(it.lit, k)) || codeAt(it.lit, k) === 39;
        if (!needsQuote) { out += it.lit; continue; }
        if (it.lit === "'") { out += "''"; continue; }
        out += "'" + swapChars(it.lit, {__proto__: null, "'": "''"}) + "'";
    }
    return out;
}

// A skeleton (ICU's PtnSkeleton): per field its type, the original field
// (char and length) and the base one (the row's char at its minimum).
function makeSkeleton(pattern) {
    const sk = {__proto__: null, type: [], ch: [], len: [], baseCh: [], baseLen: [], addedDayPeriod: false};
    // Every index made by push, so the writes below land on own properties.
    for (let f = 0; f < FIELD_COUNT; f++) { push(sk.type, 0); push(sk.ch, ''); push(sk.len, 0); push(sk.baseCh, ''); push(sk.baseLen, 0); }
    const items = tokenize(pattern);
    for (let i = 0; i < items.length; i++) {
        const it = items[i];
        if (it.ch === undefined) continue;
        const r = canonicalRow(it.ch, it.len);
        if (r < 0) continue;
        const row = DT_TYPES[r];
        const f = row[1];
        sk.ch[f] = it.ch; sk.len[f] = it.len;
        sk.baseCh[f] = row[0]; sk.baseLen[f] = row[3];
        sk.type[f] = row[2] > 0 ? row[2] + it.len : row[2];
    }
    // A 12-hour field with no day period gains the default one; a 24-hour
    // field drops a day period (ICU's #13183 handling).
    if (sk.ch[F_HOUR] !== '') {
        if (sk.ch[F_HOUR] === 'h' || sk.ch[F_HOUR] === 'K') {
            if (sk.ch[F_DAYPERIOD] === '') {
                sk.ch[F_DAYPERIOD] = 'a'; sk.len[F_DAYPERIOD] = 1; sk.baseCh[F_DAYPERIOD] = 'a'; sk.baseLen[F_DAYPERIOD] = 1;
                sk.type[F_DAYPERIOD] = DT_SHORT;
                sk.addedDayPeriod = true;
            }
        } else if (sk.ch[F_DAYPERIOD] !== '') {
            sk.ch[F_DAYPERIOD] = ''; sk.len[F_DAYPERIOD] = 0; sk.baseCh[F_DAYPERIOD] = ''; sk.baseLen[F_DAYPERIOD] = 0;
            sk.type[F_DAYPERIOD] = 0;
        }
    }
    let original = '', base = '';
    for (let f = 0; f < FIELD_COUNT; f++) {
        for (let k = 0; k < sk.len[f]; k++) original += sk.ch[f];
        for (let k = 0; k < sk.baseLen[f]; k++) base += sk.baseCh[f];
    }
    sk.original = original; sk.base = base;
    return sk;
}
function distance(req, other, includeMask) {
    let result = 0, missing = 0, extra = 0;
    for (let f = 0; f < FIELD_COUNT; f++) {
        const mine = (includeMask & (1 << f)) === 0 ? 0 : req.type[f];
        const theirs = other.type[f];
        if (mine === theirs) continue;
        if (mine === 0) { result += 0x10000; extra |= 1 << f; }
        else if (theirs === 0) { result += 0x1000; missing |= 1 << f; }
        else result += abs(mine - theirs);
    }
    return {__proto__: null, distance: result, missing: missing, extra: extra};
}

// The pattern map, built as ICU builds it: the canonical items, the
// locale's date and time formats (their skeletons derived), then
// availableFormats (their skeletons given, replacing an entry of the same
// skeleton); iterated by the first letter of the base skeleton, A-Z then
// a-z, each bucket in the order its entries arrived. Built by the first
// format that asks, not at install: a realm that never formats a date
// does not pay for it (INTL.md § 6).
let patternMap = null;
function patterns() {
    if (patternMap === null) patternMap = buildPatterns();
    return patternMap;
}
function buildPatterns() {
    const buckets = [];
    for (let b = 0; b < 52; b++) push(buckets, []);
    const bucketOf = s => { const c = codeAt(s, 0); return c >= 65 && c <= 90 ? c - 65 : c - 97 + 26; };
    function add(pattern, skeletonText, override) {
        const sk = makeSkeleton(skeletonText !== null ? skeletonText : pattern);
        if (sk.base === '') return;
        const bucket = buckets[bucketOf(sk.base)];
        let sameBase = null, sameSkeleton = null;
        for (let i = 0; i < bucket.length; i++) {
            if (sameBase === null && bucket[i].sk.base === sk.base) sameBase = bucket[i];
            if (sameSkeleton === null && bucket[i].sk.original === sk.original) sameSkeleton = bucket[i];
        }
        if (sameBase !== null && (!sameBase.specified || (skeletonText !== null && !override)) && !override) return;
        if (sameSkeleton !== null && (!override || (skeletonText !== null && sameSkeleton.specified))) return;
        for (let i = 0; i < bucket.length; i++) {
            if (bucket[i].sk.base === sk.base && bucket[i].sk.original === sk.original) {
                bucket[i].pattern = pattern; bucket[i].specified = skeletonText !== null;
                return;
            }
        }
        push(bucket, {__proto__: null, sk: sk, pattern: pattern, specified: skeletonText !== null});
    }
    const canonical = 'GyQMwWEDFdaHmsSv';
    for (let i = 0; i < canonical.length; i++) add(fromCharCode(codeAt(canonical, i)), null, false);
    const styles = ['full', 'long', 'medium', 'short'];
    for (let i = 0; i < 4; i++) add(own(G.dateFormats, styles[i]), null, false);
    for (let i = 0; i < 4; i++) add(own(G.timeFormats, styles[i]), null, false);
    const available = G.availableFormats;
    const keys = objectKeys(available);
    const seen = [];
    for (let i = 0; i < keys.length; i++) {
        const key = keys[i];
        if (indexOf(key, '-count-', 0) >= 0 || indexOf(key, '-alt-', 0) >= 0) continue;
        add(own(available, key), key, !contains(seen, key));
        push(seen, key);
    }
    const all = [];
    for (let b = 0; b < 52; b++) for (let i = 0; i < buckets[b].length; i++) push(all, buckets[b][i]);
    return all;
}
function objectKeys(o) {
    const out = [];
    for (const k in o) if (hasOwn(o, k)) push(out, k);
    return out;
}

const HOUR_CHAR = 'h';      // en-US's preferred hour (CLDR timeData US)

// ICU's getBestRaw: the nearest entry, ties going to the one that leaves
// more fields missing (ICU's own tie-break), and what it leaves out.
function bestRaw(req, includeMask) {
    const PATTERNS = patterns();
    let best = null, bestDistance = 0x7fffffff, bestMissing = -1, info = null;
    for (let i = 0; i < PATTERNS.length; i++) {
        const d = distance(req, PATTERNS[i].sk, includeMask);
        if (d.distance < bestDistance || (d.distance === bestDistance && bestMissing < d.missing)) {
            bestDistance = d.distance; bestMissing = d.missing;
            best = PATTERNS[i]; info = d;
            if (d.distance === 0) break;
        }
    }
    // The pattern ICU answers is the FIRST entry with the best one's skeleton.
    for (let i = 0; i < PATTERNS.length; i++)
        if (PATTERNS[i].sk.original === best.sk.original) { best = PATTERNS[i]; break; }
    return {__proto__: null, entry: best, missing: info.missing, extra: info.extra};
}

const MATCH_HOUR = 1;
// ICU's adjustFieldTypes: each field of the found pattern widened (or
// narrowed) to what the request asked, by ICU's rules. `hourChar` is the
// locale's hour letter (its hc's, when the locale names one).
function adjustFieldTypes(pattern, specified, req, fixFraction, options, hourChar) {
    const items = tokenize(pattern);
    for (let i = 0; i < items.length; i++) {
        const it = items[i];
        if (it.ch === undefined) continue;
        const r = canonicalRow(it.ch, it.len);
        if (r < 0) continue;
        const row = DT_TYPES[r];
        const f = row[1];
        if (fixFraction && f === F_SECOND) {
            items[i] = {__proto__: null, lit: '', parts: [it, {__proto__: null, lit: '.'}, {__proto__: null, ch: req.ch[F_FRACTION], len: req.len[F_FRACTION]}]};
            continue;
        }
        if (req.type[f] === 0) continue;
        const reqCh = req.ch[f];
        let reqLen = req.len[f];
        if (reqCh === 'E' && reqLen < 3) reqLen = 3;
        let adjLen = reqLen;
        if ((f === F_HOUR && (options & MATCH_HOUR) === 0) || f === F_MINUTE || f === F_SECOND) {
            adjLen = it.len;
        } else if (specified !== null && reqCh !== 'c' && reqCh !== 'e') {
            const skelLen = specified.len[f];
            const patNumeric = row[2] > 0, skelNumeric = specified.type[f] > 0;
            if (skelLen === reqLen || (patNumeric && !skelNumeric) || (skelNumeric && !patNumeric)) adjLen = it.len;
        }
        let c = (f !== F_HOUR && f !== F_MONTH && f !== F_WEEKDAY && (f !== F_YEAR || reqCh === 'Y')) ? reqCh : it.ch;
        if (f === F_HOUR) {
            if (reqCh === hourChar) c = hourChar;
            else if (reqCh === 'h' && hourChar === 'K') c = 'K';
            else if (reqCh === 'H' && hourChar === 'k') c = 'k';
            else if (reqCh === 'k' && hourChar === 'H') c = 'H';
            else if (reqCh === 'K' && hourChar === 'h') c = 'h';
        }
        items[i] = {__proto__: null, ch: c, len: adjLen};
    }
    // Flatten a fraction's expansion.
    const flat = [];
    for (let i = 0; i < items.length; i++) {
        if (items[i].parts !== undefined) for (let k = 0; k < items[i].parts.length; k++) push(flat, items[i].parts[k]);
        else push(flat, items[i]);
    }
    return untokenize(flat);
}

function topBit(mask) { let b = -1; for (let f = 0; f < FIELD_COUNT; f++) if (mask & (1 << f)) b = f; return b; }
const APPEND_ITEM = ['Era', 'Year', 'Quarter', 'Month', 'Week', 'Week', 'Day-Of-Week', 'Day', 'Day', 'Day', '', 'Hour',
                     'Minute', 'Second', '', 'Timezone'];
const FIELD_NAME = ['era', 'year', 'quarter', 'month', 'week', 'week', 'weekday', 'day', 'day', 'day', 'dayperiod', 'hour',
                    'minute', 'second', 'second', 'zone'];
function simpleFormat(template, args) {
    let out = '';
    for (let i = 0; i < template.length; i++) {
        const c = codeAt(template, i);
        if (c === 123 && i + 2 < template.length && codeAt(template, i + 2) === 125) {
            const n = codeAt(template, i + 1) - 48;
            if (n >= 0 && n < args.length) { out += args[n]; i += 2; continue; }
        }
        out += fromCharCode(c);
    }
    return out;
}
// ICU's getBestAppending: the best pattern for `mask`'s fields, and each
// field it lacks appended by the appendItems pattern of the top one found.
function bestAppending(req, mask, options, hourChar) {
    if (mask === 0) return '';
    let raw = bestRaw(req, mask);
    let result = adjustFieldTypes(raw.entry.pattern, raw.entry.specified ? raw.entry.sk : null, req, false, options, hourChar);
    let missing = raw.missing, last = 0;
    const SECOND_AND_FRACTION = (1 << F_SECOND) | (1 << F_FRACTION), FRACTION = 1 << F_FRACTION;
    while (missing !== 0) {
        if (last === missing) break;
        if ((missing & SECOND_AND_FRACTION) === FRACTION && (mask & SECOND_AND_FRACTION) === SECOND_AND_FRACTION) {
            result = adjustFieldTypes(result, raw.entry.specified ? raw.entry.sk : null, req, true, options, hourChar);
            missing &= ~FRACTION;
            continue;
        }
        const starting = missing;
        const temp = bestRaw(req, missing);
        const tempPattern = adjustFieldTypes(temp.entry.pattern, temp.entry.specified ? temp.entry.sk : null, req, false, options, hourChar);
        const found = starting & ~temp.missing;
        const top = topBit(found);
        const item = own(G.appendItems, APPEND_ITEM[top]);
        if (item !== undefined && item !== '')
            result = simpleFormat(item, [result, tempPattern, "'" + own(G.fieldNames, FIELD_NAME[top]) + "'"]);
        last = missing = temp.missing;
    }
    return result;
}
// The glue a date part and a time part are joined with, by the date
// part's month and weekday widths (ICU's getBestPattern).
function glueFor(req) {
    let style = 'short';
    if (req.baseCh[F_MONTH] !== '' && req.len[F_MONTH] === 4) style = req.ch[F_WEEKDAY] !== '' ? 'full' : 'long';
    else if (req.len[F_MONTH] === 3) style = 'medium';
    return own(G.dateTimeFormatsAtTime, style);
}
// ICU's getBestPattern(skeleton, options): what other browsers ask with
// UDATPG_MATCH_HOUR_FIELD_LENGTH and the locale's own hour (the defaults),
// and what ICU's range formatter asks with no options and the format's hc.
function bestPattern(skeletonText, options = MATCH_HOUR, hourChar = HOUR_CHAR) {
    // 'j' is the locale's hour, with its day period.
    let mapped = '';
    for (let i = 0; i < skeletonText.length; i++) {
        const c = fromCharCode(codeAt(skeletonText, i));
        if (c !== 'j') { mapped += c; continue; }
        let extra = 0;
        while (i + 1 < skeletonText.length && codeAt(skeletonText, i + 1) === 106) { extra++; i++; }
        let hourLen = 1 + (extra & 1), periodLen = extra < 2 ? 1 : 3 + (extra >> 1);
        if (hourChar === 'H' || hourChar === 'k') periodLen = 0;
        while (periodLen-- > 0) mapped += 'a';
        while (hourLen-- > 0) mapped += hourChar;
    }
    const req = makeSkeleton(mapped);
    const raw = bestRaw(req, -1);
    if (raw.missing === 0 && raw.extra === 0)
        return adjustFieldTypes(raw.entry.pattern, raw.entry.specified ? raw.entry.sk : null, req, false, options, hourChar);
    let needed = 0;
    for (let f = 0; f < FIELD_COUNT; f++) if (req.type[f] !== 0) needed |= 1 << f;
    const datePattern = bestAppending(req, needed & DATE_MASK, options, hourChar);
    const timePattern = bestAppending(req, needed & TIME_MASK, options, hourChar);
    if (datePattern === '') return timePattern;
    if (timePattern === '') return datePattern;
    return simpleFormat(glueFor(req), [timePattern, datePattern]);
}

// ── DateTimeFormat (ECMA-402 § 11) ──────────────────────────────────────
const DTFS = new WeakMap();
const FALLBACK_SYMBOL = Symbol_('IntlLegacyConstructedSymbol');
const COMPONENTS = [
    ['weekday', ['narrow', 'short', 'long']], ['era', ['narrow', 'short', 'long']],
    ['year', ['2-digit', 'numeric']], ['month', ['2-digit', 'numeric', 'narrow', 'short', 'long']],
    ['day', ['2-digit', 'numeric']], ['dayPeriod', ['narrow', 'short', 'long']],
    ['hour', ['2-digit', 'numeric']], ['minute', ['2-digit', 'numeric']], ['second', ['2-digit', 'numeric']],
    ['fractionalSecondDigits', null],
    ['timeZoneName', ['short', 'long', 'shortOffset', 'longOffset', 'shortGeneric', 'longGeneric']],
];
// The skeleton letters other browsers ask ICU for, per option and value.
const SKELETON = {__proto__: null,
    weekday: {__proto__: null, narrow: 'EEEEE', short: 'EEE', long: 'EEEE'},
    era: {__proto__: null, narrow: 'GGGGG', short: 'G', long: 'GGGG'},
    year: {__proto__: null, '2-digit': 'yy', numeric: 'y'},
    month: {__proto__: null, '2-digit': 'MM', numeric: 'M', narrow: 'MMMMM', short: 'MMM', long: 'MMMM'},
    day: {__proto__: null, '2-digit': 'dd', numeric: 'd'},
    dayPeriod: {__proto__: null, narrow: 'BBBBB', short: 'B', long: 'BBBB'},
    minute: {__proto__: null, '2-digit': 'mm', numeric: 'm'},
    second: {__proto__: null, '2-digit': 'ss', numeric: 's'},
    timeZoneName: {__proto__: null, short: 'z', long: 'zzzz', shortOffset: 'O', longOffset: 'OOOO', shortGeneric: 'v', longGeneric: 'vvvv'},
};
const HOUR_SKELETON = {__proto__: null, h11: 'K', h12: 'h', h23: 'H', h24: 'k'};
const CALENDARS = ['gregory'], NUMBERING = ['latn'];
function dtfLocaleData(locale, key) {
    if (key === 'ca') return CALENDARS;
    if (key === 'nu') return NUMBERING;
    return [null, 'h11', 'h12', 'h23', 'h24'];          // hc: the locale's own is the default
}

// A pattern's hour letters made hc's, outside quotes (V8's
// ReplaceHourCycleInPattern).
function replaceHourCycle(pattern, hc) {
    const to = HOUR_SKELETON[hc];
    let out = '', quoted = false;
    for (let i = 0; i < pattern.length; i++) {
        const c = fromCharCode(codeAt(pattern, i));
        if (c === "'") quoted = !quoted;
        if (!quoted && (c === 'h' || c === 'H' || c === 'k' || c === 'K')) out += to;
        else out += c;
    }
    return out;
}
// The skeleton of a pattern, as ICU's staticGetSkeleton reads it: its
// fields, literal text dropped.
function skeletonOf(pattern) {
    const items = tokenize(pattern);
    let out = '';
    for (let i = 0; i < items.length; i++)
        if (items[i].ch !== undefined) for (let k = 0; k < items[i].len; k++) out += items[i].ch;
    return out;
}
// dateStyle/timeStyle: the locale's own patterns, joined by the date
// style's glue, the hour cycle made hc's when it is not the locale's (V8's
// DateTimeStylePattern).
function stylePattern(dateStyle, timeStyle, hc) {
    let pattern;
    if (dateStyle !== undefined && timeStyle !== undefined)
        pattern = simpleFormat(own(G.dateTimeFormatsAtTime, dateStyle), [own(G.timeFormats, timeStyle), own(G.dateFormats, dateStyle)]);
    else if (dateStyle !== undefined) pattern = own(G.dateFormats, dateStyle);
    else pattern = own(G.timeFormats, timeStyle);
    if (timeStyle === undefined || hc === 'h12') return pattern;
    let skeleton = '';
    const sk = skeletonOf(pattern);
    for (let i = 0; i < sk.length; i++) {
        const c = fromCharCode(codeAt(sk, i));
        if (c === 'a' || c === 'b' || c === 'B') continue;
        skeleton += (c === 'h' || c === 'H' || c === 'k' || c === 'K') ? HOUR_SKELETON[hc] : c;
    }
    return replaceHourCycle(bestPattern(skeleton), hc);
}

// What a script is handed: CLDR's patterns put U+202F NARROW NO-BREAK
// SPACE before AM/PM and U+2009 THIN SPACE round a range's dash, and
// Chrome writes an ASCII space for each, in format(), formatToParts() and
// the ranges alike (INTL.md § 8, its capture). Applied where a string
// leaves Intl, so the patterns stay CLDR's own.
const CLDR_SPACES = {__proto__: null, '\u202f': ' ', '\u2009': ' '};
function asciiSpaces(text) { return swapChars(text, CLDR_SPACES); }

function hourCycleOfPattern(pattern) {
    const items = tokenize(pattern);
    for (let i = 0; i < items.length; i++) {
        const c = items[i].ch;
        if (c === 'h') return 'h12';
        if (c === 'H') return 'h23';
        if (c === 'K') return 'h11';
        if (c === 'k') return 'h24';
    }
    return undefined;
}

// Offset time zone identifiers (ECMA-262 § 21.4.1.33): ±HH, ±HHMM or
// ±HH:MM, minutes precision; normalized to ±HH:MM.
function parseOffsetZone(s) {
    const m = exec(/^([+-])([01][0-9]|2[0-3])(?::?([0-5][0-9]))?$/, s);
    if (m === null) return null;
    const minutes = (m[1] === '-' ? -1 : 1) * (+m[2] * 60 + (m[3] !== undefined ? +m[3] : 0));
    return {__proto__: null, name: (minutes < 0 ? '-' : '+') + pad(String_(floor(abs(minutes) / 60)), 2) + ':' + pad(String_(abs(minutes) % 60), 2),
            minutes: minutes};
}

function CreateDateTimeFormat(dtf, locales, options, required, defaults) {
    loadData();
    const requested = CanonicalizeLocaleList(locales);
    options = CoerceOptionsToObject(options);
    const opt = {__proto__: null};
    GetOption(options, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit');
    const calendar = GetOption(options, 'calendar', 'string', undefined, undefined);
    if (calendar !== undefined && !isTypeSequence(calendar)) throw new RangeError_('Invalid calendar : ' + calendar);
    opt.ca = calendar;
    const numberingSystem = GetOption(options, 'numberingSystem', 'string', undefined, undefined);
    if (numberingSystem !== undefined && !isTypeSequence(numberingSystem))
        throw new RangeError_('Invalid numberingSystem : ' + numberingSystem);
    opt.nu = numberingSystem;
    const hour12 = GetOption(options, 'hour12', 'boolean', undefined, undefined);
    let hourCycle = GetOption(options, 'hourCycle', 'string', ['h11', 'h12', 'h23', 'h24'], undefined);
    if (hour12 !== undefined) hourCycle = null;
    opt.hc = hourCycle;
    const r = ResolveLocale(AVAILABLE, requested, opt, ['ca', 'hc', 'nu'], dtfLocaleData);
    const s = {__proto__: null, locale: r.locale, calendar: r.ca, numberingSystem: r.nu};
    let hc;
    if (hour12 === true) hc = 'h12';
    else if (hour12 === false) hc = 'h23';
    else hc = r.hc === null || r.hc === undefined ? 'h12' : r.hc;
    // The machine's zone is a name or an offset zone, as a given one is.
    let timeZone = options.timeZone;
    timeZone = timeZone === undefined ? zoneHost() : ToString(timeZone);
    const offset = parseOffsetZone(timeZone);
    if (offset !== null) { s.offsetMinutes = offset.minutes; timeZone = offset.name; }
    else {
        const canonical = zoneCanonical(timeZone);
        if (canonical === undefined) throw new RangeError_('Invalid time zone specified: ' + timeZone);
        timeZone = canonical;
    }
    s.timeZone = timeZone;
    const fmt = {__proto__: null};
    let hasExplicit = false;
    for (let i = 0; i < COMPONENTS.length; i++) {
        const prop = COMPONENTS[i][0];
        let value;
        if (prop === 'fractionalSecondDigits') value = GetNumberOption(options, prop, 1, 3, undefined);
        else value = GetOption(options, prop, 'string', COMPONENTS[i][1], undefined);
        fmt[prop] = value;
        if (value !== undefined) hasExplicit = true;
    }
    GetOption(options, 'formatMatcher', 'string', ['basic', 'best fit'], 'best fit');
    const dateStyle = GetOption(options, 'dateStyle', 'string', ['full', 'long', 'medium', 'short'], undefined);
    const timeStyle = GetOption(options, 'timeStyle', 'string', ['full', 'long', 'medium', 'short'], undefined);
    let pattern;
    if (dateStyle !== undefined || timeStyle !== undefined) {
        if (hasExplicit) throw new TypeError_('Can\'t set option ' + 'dateStyle/timeStyle with other date-time components');
        if (required === 'date' && timeStyle !== undefined) throw new TypeError_('Invalid option : timeStyle');
        if (required === 'time' && dateStyle !== undefined) throw new TypeError_('Invalid option : dateStyle');
        s.dateStyle = dateStyle; s.timeStyle = timeStyle;
        pattern = stylePattern(dateStyle, timeStyle, hc);
    } else {
        let needDefaults = true;
        if (required === 'date' || required === 'any') {
            if (fmt.weekday !== undefined || fmt.year !== undefined || fmt.month !== undefined || fmt.day !== undefined) needDefaults = false;
        }
        if (required === 'time' || required === 'any') {
            if (fmt.dayPeriod !== undefined || fmt.hour !== undefined || fmt.minute !== undefined || fmt.second !== undefined ||
                fmt.fractionalSecondDigits !== undefined) needDefaults = false;
        }
        if (needDefaults && (defaults === 'date' || defaults === 'all')) { fmt.year = 'numeric'; fmt.month = 'numeric'; fmt.day = 'numeric'; }
        if (needDefaults && (defaults === 'time' || defaults === 'all')) { fmt.hour = 'numeric'; fmt.minute = 'numeric'; fmt.second = 'numeric'; }
        let skeleton = '';
        for (let i = 0; i < COMPONENTS.length; i++) {
            const prop = COMPONENTS[i][0], value = fmt[prop];
            if (value === undefined) continue;
            if (prop === 'hour') skeleton += value === '2-digit' ? HOUR_SKELETON[hc] + HOUR_SKELETON[hc] : HOUR_SKELETON[hc];
            else if (prop === 'fractionalSecondDigits') for (let k = 0; k < value; k++) skeleton += 'S';
            else skeleton += own(own(SKELETON, prop), value);
        }
        pattern = bestPattern(skeleton);
        if (fmt.hour !== undefined) pattern = replaceHourCycle(pattern, hc);
    }
    s.pattern = pattern;
    s.hourCycle = hourCycleOfPattern(pattern);
    apply(wm_set, DTFS, [dtf, s]);
    return dtf;
}

function DateTimeFormat(locales, options) {
    const made = new.target === undefined ? objectCreate(DateTimeFormatPrototype) : constructed(this, DateTimeFormatPrototype);
    const dtf = CreateDateTimeFormat(made, locales, options, 'any', 'date');
    // The legacy constructor mode (ECMA-402 § 11.1.1 and Annex: the
    // ChainDateTimeFormat other browsers implement).
    if (new.target === undefined && isObject(this) && hasInstance(DateTimeFormat, this)) {
        defineProperty(this, FALLBACK_SYMBOL, {__proto__: null, value: dtf, writable: false, enumerable: false, configurable: false});
        return this;
    }
    return dtf;
}
const DateTimeFormatPrototype = DateTimeFormat.prototype;
defineProperty(DateTimeFormat, 'prototype', {__proto__: null, writable: false, enumerable: false, configurable: false});
defineProperty(DateTimeFormat, 'length', {__proto__: null, value: 0, configurable: true});

function UnwrapDateTimeFormat(dtf, name) {
    let s = slots(DTFS, dtf);
    if (s === undefined && isObject(dtf) && hasInstance(DateTimeFormat, dtf)) s = slots(DTFS, dtf[FALLBACK_SYMBOL]);
    if (s === undefined) throw new TypeError_('Method Intl.DateTimeFormat.prototype.' + name + ' called on incompatible receiver');
    return s;
}

// ECMA-262's TimeClip, for the instant a format is asked for.
function timeClip(x) {
    if (!isFinite_(x) || abs(x) > 8.64e15) throw new RangeError_('Invalid time value');
    return trunc(x) + 0;
}
function toInstant(date) {
    if (date === undefined) return apply(DateNow, Date_, []);
    return timeClip(ToNumber(date));
}

// The proleptic Gregorian date of a count of days from 1970-01-01, and its
// inverse (Howard Hinnant's civil_from_days and days_from_civil): plain
// arithmetic, since a Date cannot hold an instant TimeClip allows once a
// zone's offset moves it past the range (Asia/Tokyo at 8.64e15).
const DAY_MS = 86400000;
function civilFromDays(days) {
    const z = days + 719468, era = floor(z / 146097), doe = z - era * 146097;
    const yoe = floor((doe - floor(doe / 1460) + floor(doe / 36524) - floor(doe / 146096)) / 365);
    const doy = doe - (365 * yoe + floor(yoe / 4) - floor(yoe / 100));
    const mp = floor((5 * doy + 2) / 153), m = mp < 10 ? mp + 3 : mp - 9;
    return {__proto__: null, year: yoe + era * 400 + (m <= 2 ? 1 : 0), month: m - 1, day: doy - floor((153 * mp + 2) / 5) + 1};
}
function daysFromCivil(year, month, day) {
    const y = month <= 1 ? year - 1 : year, era = floor(y / 400), yoe = y - era * 400;
    const m = month + 1, doy = floor((153 * (m > 2 ? m - 3 : m + 9) + 2) / 5) + day - 1;
    return era * 146097 + yoe * 365 + floor(yoe / 4) - floor(yoe / 100) + doy - 719468;
}
// The calendar fields of instant `x` in the format's zone.
function localFields(s, x) {
    const offset = s.offsetMinutes !== undefined ? s.offsetMinutes : zoneOffset(s.timeZone, x);
    const t = x + offset * 60000, days = floor(t / DAY_MS);
    let ms = t - days * DAY_MS;
    const c = civilFromDays(days);
    const hour = floor(ms / 3600000); ms -= hour * 3600000;
    const minute = floor(ms / 60000); ms -= minute * 60000;
    const second = floor(ms / 1000); ms -= second * 1000;
    return {__proto__: null, year: c.year, month: c.month, day: c.day, weekday: ((days % 7) + 11) % 7,
            hour: hour, minute: minute, second: second, ms: ms, offset: offset, instant: x};
}

// ── Time zone names (UTS 35 § 7) ────────────────────────────────────────
// A localized GMT offset: "GMT-5" short, "GMT-05:00" long; other browsers
// write a zero offset "GMT+0", never the bare "GMT".
function gmtOffset(minutes, long) {
    const sign = minutes < 0 ? '-' : '+';
    const h = floor(abs(minutes) / 60), m = abs(minutes) % 60;
    if (long) return 'GMT' + sign + pad(String_(h), 2) + ':' + pad(String_(m), 2);
    return 'GMT' + sign + String_(h) + (m !== 0 ? ':' + pad(String_(m), 2) : '');
}
function isDaylight(s, x, offset) {
    if (s.offsetMinutes !== undefined) return false;
    // Daylight time: the offset is past the zone's January and July minimum.
    const year = civilFromDays(floor(x / DAY_MS)).year;
    const jan = zoneOffset(s.timeZone, daysFromCivil(year, 0, 1) * DAY_MS);
    const jul = zoneOffset(s.timeZone, daysFromCivil(year, 6, 1) * DAY_MS);
    return offset > (jan < jul ? jan : jul);
}
function zoneSpecific(zone, width, kind) {
    const z = own(Z.zones, zone);
    const w = z !== undefined ? own(z, width) : undefined;
    return w !== undefined ? own(w, kind) : undefined;
}
function metazoneName(mz, width, kind) {
    const m = own(Z.metazones, mz);
    const w = m !== undefined ? own(m, width) : undefined;
    return w !== undefined ? own(w, kind) : undefined;
}
// The zone's specific name (z, zzzz): its own, else its metazone's, for
// standard or daylight time; GMT format when it has none.
function specificName(s, f, long) {
    const width = long ? 'long' : 'short';
    const kind = isDaylight(s, f.instant, f.offset) ? 'daylight' : 'standard';
    if (s.offsetMinutes === undefined) {
        const zoneName = s.timeZone === 'UTC' ? 'Etc/UTC' : s.timeZone;
        let name = zoneSpecific(zoneName, width, kind);
        if (name === undefined) {
            const mz = own(Z.current, zoneName);
            if (mz !== undefined) name = metazoneName(mz, width, kind);
        }
        if (name !== undefined) return name;
    }
    return gmtOffset(f.offset, long);
}
// The location format: "{country} Time" for a country's only (or primary)
// zone, else "{city} Time".
function locationName(zone) {
    const country = own(Z.country, zone);
    if (country === undefined) return undefined;
    let zonesInCountry = 0;
    for (const z in Z.country) if (hasOwn(Z.country, z) && own(Z.country, z) === country) zonesInCountry++;
    let place;
    if (zonesInCountry === 1 || own(Z.primary, country) === zone) place = own(Z.countries, country);
    else {
        place = own(own(Z.zones, zone) || {__proto__: null}, 'exemplarCity');
        if (place === undefined) place = swapChars(slice(zone, lastIndexOf(zone, '/') + 1, zone.length), {__proto__: null, _: ' '});
    }
    return place === undefined ? undefined : simpleFormat(own(Z.formats, 'regionFormat'), [place]);
}
// The generic name (v, vvvv), as ICU's TimeZoneGenericNames makes it.
function genericName(s, f, long) {
    if (s.offsetMinutes !== undefined) return gmtOffset(f.offset, long);
    const zone = s.timeZone === 'UTC' ? 'Etc/UTC' : s.timeZone;
    const width = long ? 'long' : 'short';
    let name = zoneSpecific(zone, width, 'generic');
    if (name === undefined) {
        const mz = own(Z.current, zone);
        if (mz !== undefined) {
            // A zone with no daylight time within 184 days uses its
            // standard name, unless that is the metazone's generic one.
            let useStandard = !isDaylight(s, f.instant, f.offset);
            if (useStandard) {
                const day = 86400000;
                for (let k = -184; k <= 184 && useStandard; k += 23)
                    if (isDaylight(s, f.instant + k * day, zoneOffset(s.timeZone, f.instant + k * day))) useStandard = false;
            }
            if (useStandard) {
                const std = zoneSpecific(zone, width, 'standard') || metazoneName(mz, width, 'standard');
                if (std !== undefined && std !== metazoneName(mz, width, 'generic')) name = std;
            }
            if (name === undefined) {
                const generic = metazoneName(mz, width, 'generic');
                if (generic !== undefined) {
                    const golden = own(own(Z.golden, mz) || {__proto__: null}, 'US') || own(own(Z.golden, mz) || {__proto__: null}, '001');
                    if (golden !== undefined && golden !== zone &&
                        zoneOffset(golden, f.instant) !== f.offset) name = undefined;
                    else name = generic;
                }
            }
        }
    }
    if (name === undefined) name = locationName(zone);
    return name !== undefined ? name : gmtOffset(f.offset, long);
}
function zoneName(s, f, ch, len) {
    if (ch === 'z') return specificName(s, f, len >= 4);
    if (ch === 'O') return gmtOffset(f.offset, len >= 4);
    if (ch === 'v') return genericName(s, f, len >= 4);
    return gmtOffset(f.offset, true);
}

// ── Formatting a pattern ────────────────────────────────────────────────
function dayPeriodFlexible(f, width, pattern) {
    const periods = own(own(G.dayPeriods, 'format'), width);
    const hasMinute = indexOf(pattern, 'm', 0) >= 0, hasSecond = indexOf(pattern, 's', 0) >= 0;
    if (f.hour === 12 && (!hasMinute || f.minute === 0) && (!hasSecond || f.second === 0) && own(periods, 'noon') !== undefined)
        return own(periods, 'noon');
    const rules = DAY_PERIOD_RULES;
    const minutes = f.hour * 60 + f.minute;
    for (const name in rules) {
        if (!hasOwn(rules, name)) continue;
        const rule = own(rules, name);
        if (own(rule, '_from') === undefined) continue;
        const from = hm(own(rule, '_from')), before = hm(own(rule, '_before'));
        if (from <= before ? minutes >= from && minutes < before : minutes >= from || minutes < before)
            return own(periods, name);
    }
    return own(periods, f.hour < 12 ? 'am' : 'pm');
}
function hm(s) { const p = split(s, ':'); return +p[0] * 60 + +p[1]; }

// A text field's CLDR width by the field's length: 1-3 abbreviated, 4 wide,
// 5 narrow, 6 short.
const WIDTH_TEXT = ['', 'abbreviated', 'abbreviated', 'abbreviated', 'wide', 'narrow', 'short'];
const PART_TYPE = {__proto__: null, G: 'era', y: 'year', Y: 'year', u: 'year', M: 'month', L: 'month', d: 'day', E: 'weekday',
    c: 'weekday', e: 'weekday', a: 'dayPeriod', b: 'dayPeriod', B: 'dayPeriod', h: 'hour', H: 'hour', K: 'hour',
    k: 'hour', m: 'minute', s: 'second', S: 'fractionalSecond', z: 'timeZoneName', O: 'timeZoneName',
    v: 'timeZoneName', V: 'timeZoneName', Z: 'timeZoneName', X: 'timeZoneName', x: 'timeZoneName'};
// One field's text.
function fieldText(s, f, ch, len, pattern) {
    switch (ch) {
    case 'G': return own(own(G.eras, len === 4 ? 'eraNames' : len === 5 ? 'eraNarrow' : 'eraAbbr'), f.year > 0 ? '1' : '0');
    case 'y': {
        const y = f.year > 0 ? f.year : 1 - f.year;
        if (len === 2) return pad(String_(y % 100), 2);
        return pad(String_(y), len);
    }
    case 'M': case 'L': {
        if (len <= 2) return pad(String_(f.month + 1), len);
        const context = ch === 'L' ? 'stand-alone' : 'format';
        return own(own(own(G.months, context), WIDTH_TEXT[len]), String_(f.month + 1));
    }
    case 'd': return pad(String_(f.day), len);
    case 'E': case 'c': case 'e': {
        const context = ch === 'c' ? 'stand-alone' : 'format';
        const keys = ['sun', 'mon', 'tue', 'wed', 'thu', 'fri', 'sat'];
        return own(own(own(G.days, context), WIDTH_TEXT[len < 3 ? 3 : len]), keys[f.weekday]);
    }
    case 'a': return own(own(own(G.dayPeriods, 'format'), len === 4 ? 'wide' : len === 5 ? 'narrow' : 'abbreviated'),
                         f.hour < 12 ? 'am' : 'pm');
    case 'b': case 'B':
        return dayPeriodFlexible(f, len === 4 ? 'wide' : len === 5 ? 'narrow' : 'abbreviated', pattern);
    case 'h': return pad(String_(f.hour % 12 === 0 ? 12 : f.hour % 12), len);
    case 'K': return pad(String_(f.hour % 12), len);
    case 'H': return pad(String_(f.hour), len);
    case 'k': return pad(String_(f.hour === 0 ? 24 : f.hour), len);
    case 'm': return pad(String_(f.minute), len);
    case 's': return pad(String_(f.second), len);
    case 'S': return slice(pad(String_(f.ms), 3) + '000000', 0, len);
    case 'z': case 'O': case 'v': case 'V': case 'Z': case 'X': case 'x': return zoneName(s, f, ch, len);
    }
    return '';
}
// The parts of `pattern` for instant `x`, adjacent literal text merged.
function formatParts(s, pattern, x) {
    const f = localFields(s, x);
    const items = tokenize(pattern);
    const parts = [];
    for (let i = 0; i < items.length; i++) {
        const it = items[i];
        if (it.ch === undefined) {
            if (it.lit === '') continue;
            const last = parts.length > 0 ? parts[parts.length - 1] : null;
            if (last !== null && last.type === 'literal') last.value += it.lit;
            else push(parts, {__proto__: null, type: 'literal', value: it.lit});
            continue;
        }
        const type = own(PART_TYPE, it.ch);
        if (type === undefined) continue;
        push(parts, {__proto__: null, type: type, value: fieldText(s, f, it.ch, it.len, pattern), ch: it.ch});
    }
    return parts;
}
function partsToString(parts) { let out = ''; for (let i = 0; i < parts.length; i++) out += parts[i].value; return out; }
function partsToArray(parts) {
    const out = [];
    for (let i = 0; i < parts.length; i++) {
        const o = {};
        put(o, 'type', parts[i].type);
        put(o, 'value', asciiSpaces(parts[i].value));
        if (parts[i].source !== undefined) put(o, 'source', parts[i].source);
        push(out, o);
    }
    return out;
}

// ── Ranges (ICU's DateIntervalFormat) ───────────────────────────────────
// Other browsers hand a range to ICU's DateIntervalFormat, made from the
// skeleton of the format's own pattern and the format's hour cycle; this
// is that algorithm, over CLDR's intervalFormats. Its quirks are theirs
// and kept: a 2-digit hour prints as 1 digit, a month-and-day range whose
// years differ prints the years.
// ICU's interval indices, by the calendar field that differs.
const IV_ERA = 0, IV_YEAR = 1, IV_MONTH = 2, IV_DATE = 3, IV_AM_PM = 4, IV_HOUR = 5, IV_MINUTE = 6,
      IV_SECOND = 7, IV_MILLISECOND = 8, IV_COUNT = 9;
const IV_LETTER = ['G', 'y', 'M', 'd', 'a', 'h', 'm', 's', 'S'];
const IV_LEVEL = [0, 10, 20, 30, 40, 50, 60, 70, 80];
// Which index an intervalFormats key letter fills.
const IV_INDEX = {__proto__: null, G: IV_ERA, y: IV_YEAR, M: IV_MONTH, d: IV_DATE, a: IV_AM_PM, B: IV_AM_PM, h: IV_HOUR, H: IV_HOUR,
    m: IV_MINUTE};
// A pattern letter's level (ICU's fgPatternCharToLevel); a letter not
// listed is no level at all.
const LETTER_LEVEL = {__proto__: null, G: 0, y: 10, Y: 10, u: 10, U: 10, r: 10, Q: 20, q: 20, M: 20, L: 20, w: 20, D: 20, W: 30,
    d: 30, E: 30, e: 30, c: 30, F: 30, g: 0, l: 0, a: 40, A: 40, h: 50, H: 50, k: 50, K: 50, m: 60, s: 70, S: 80,
    z: 0, Z: 0, v: 0, V: 0, O: 0, X: 0, x: 0};
// Each letter's count in a skeleton, by letter.
function letterCounts(s) {
    const counts = {__proto__: null};
    for (let i = 0; i < s.length; i++) { const c = fromCharCode(codeAt(s, i)); counts[c] = (own(counts, c) || 0) + 1; }
    return counts;
}
// The intervalFormats skeletons with their letters counted, made by the
// first range that asks.
let intervalSkeletons = null;
function ivSkeletons() {
    if (intervalSkeletons !== null) return intervalSkeletons;
    const out = [];
    for (const k in IV) if (hasOwn(IV, k) && k !== 'intervalFormatFallback') push(out, {__proto__: null, skeleton: k, counts: letterCounts(k)});
    return intervalSkeletons = out;
}
const LETTERS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz';
// ICU's DateIntervalInfo::getBestSkeleton: the nearest intervalFormats
// skeleton, and how near (0 the same, 1 the same fields at other widths,
// 2 only z/k/K/a/b told apart, -1 other fields).
function bestIntervalSkeleton(skeleton) {
    const input = swapChars(skeleton, {__proto__: null, z: 'v', k: 'H', K: 'h', a: '', b: ''});
    const replaced = input !== skeleton;
    const want = letterCounts(input);
    const IV_SKELETONS = ivSkeletons();
    let best = null, bestDistance = 0x7fffffff, info = 0;
    for (let i = 0; i < IV_SKELETONS.length; i++) {
        const have = IV_SKELETONS[i].counts;
        let d = 0, fieldDifference = 1;
        for (let k = 0; k < LETTERS.length; k++) {
            const c = fromCharCode(codeAt(LETTERS, k));
            const a = own(want, c) || 0, b = own(have, c) || 0;
            if (a === b) continue;
            if (a === 0 || b === 0) { fieldDifference = -1; d += 0x1000; }
            else if (c === 'M' && ((a <= 2 && b > 2) || (a > 2 && b <= 2))) d += 0x100;
            else d += abs(a - b);
        }
        if (d < bestDistance) { best = IV_SKELETONS[i].skeleton; bestDistance = d; info = fieldDifference; }
        if (d === 0) { info = 0; break; }
    }
    if (replaced && info !== -1) info = 2;
    return {__proto__: null, skeleton: best, info: info};
}
function intervalPatternOf(skeleton, index) {
    const entry = own(IV, skeleton);
    if (entry === undefined) return '';
    for (const k in entry) if (hasOwn(entry, k) && own(IV_INDEX, k) === index) return own(entry, k);
    return '';
}
// ICU's isFieldUnitIgnored: whether every field of `pattern` is coarser
// than the index's field, so a difference there cannot show.
function fieldUnitIgnored(pattern, index) {
    const items = tokenize(pattern);
    for (let i = 0; i < items.length; i++) {
        if (items[i].ch === undefined) continue;
        const level = own(LETTER_LEVEL, items[i].ch);
        if (level !== undefined && IV_LEVEL[index] <= level) return false;
    }
    return true;
}
// ICU's adjustFieldWidth: an interval pattern's fields widened to the
// requested skeleton's where the matched skeleton was narrower, with the
// letters a near match (info 2) told apart put back.
function adjustFieldWidth(inputSkeleton, bestSkeleton, pattern, info) {
    const input = letterCounts(inputSkeleton), best = letterCounts(bestSkeleton);
    let adjusted = pattern;
    if (info === 2) {
        if (indexOf(inputSkeleton, 'z', 0) >= 0) { best.z = own(best, 'v'); adjusted = swapChars(adjusted, {__proto__: null, v: 'z'}); }
        if (indexOf(inputSkeleton, 'K', 0) >= 0) adjusted = swapChars(adjusted, {__proto__: null, h: 'K'});
        if (indexOf(inputSkeleton, 'k', 0) >= 0) adjusted = swapChars(adjusted, {__proto__: null, H: 'k'});
        if (indexOf(inputSkeleton, 'b', 0) >= 0) adjusted = swapChars(adjusted, {__proto__: null, a: 'b'});
    }
    if (indexOf(adjusted, 'a', 0) >= 0 && !own(best, 'a')) best.a = 1;
    if (indexOf(adjusted, 'b', 0) >= 0 && !own(best, 'b')) best.b = 1;
    const items = tokenize(adjusted);
    for (let i = 0; i < items.length; i++) {
        const it = items[i];
        if (it.ch === undefined) continue;
        const key = it.ch === 'L' ? 'M' : it.ch;
        const have = own(best, key) || 0, want = own(input, key) || 0;
        if (have === it.len && want > have) items[i] = {__proto__: null, ch: it.ch, len: want};
    }
    return untokenize(items);
}
// ICU's splitPatternInto2Part: the first part runs to the first field
// whose letter has already appeared; the second part is the rest.
function splitInterval(pattern) {
    const items = tokenize(pattern);
    const seen = {__proto__: null};
    let first = [], second = [], inSecond = false;
    for (let i = 0; i < items.length; i++) {
        const it = items[i];
        if (!inSecond && it.ch !== undefined) {
            if (own(seen, it.ch)) inSecond = true;
            else seen[it.ch] = true;
        }
        push(inSecond ? second : first, it);
    }
    return {__proto__: null, first: untokenize(first), second: untokenize(second)};
}

// The range formatter of a format, built once: per index the interval
// pattern split in two ({first: ''} is a fallback, its second the whole
// pattern), and the patterns the fallbacks format with.
function intervalFormatOf(s) {
    if (s.interval !== undefined) return s.interval;
    const hourChar = s.hourCycle !== undefined ? HOUR_SKELETON[s.hourCycle] : HOUR_CHAR;
    const best = skeleton => bestPattern(skeleton, 0, hourChar);
    const sk = makeSkeleton(s.pattern);
    // staticGetSkeleton: the default day period it adds is not reported.
    let skeleton = sk.original;
    if (sk.addedDayPeriod) {
        const at = indexOf(skeleton, 'a', 0);
        skeleton = slice(skeleton, 0, at) + slice(skeleton, at + 1, skeleton.length);
    }
    const st = {__proto__: null, p: {__proto__: null}, single: best(skeleton), datePattern: null, timePattern: null, glue: null};
    s.interval = st;
    const converted = normalizeHourLetters(skeleton);
    const t = splitDateTime(converted);
    if (t.time !== '' && t.date !== '') st.glue = own(G.dateTimeFormats, 'medium');
    const timeOnlyFallbacks = () => {
        const pattern = best('yMd' + t.time);
        st.p[IV_DATE] = st.p[IV_MONTH] = st.p[IV_YEAR] = {__proto__: null, first: '', second: pattern};
        st.p[IV_ERA] = {__proto__: null, first: '', second: best('GyMd' + t.time)};
    };
    if (!separateDateTime(st, t, best)) {
        if (t.time !== '' && t.date === '') timeOnlyFallbacks();
        return st;
    }
    if (t.time === '') return st;
    if (t.date === '') { timeOnlyFallbacks(); return st; }
    // A date and a time: a difference in a date field the format does not
    // show falls back to the whole of both ends with that field added; a
    // time difference is the time's interval after the one date.
    let fallback = skeleton;
    const prefixes = [[IV_DATE, 'd'], [IV_MONTH, 'M'], [IV_YEAR, 'y'], [IV_ERA, 'G']];
    for (let i = 0; i < prefixes.length; i++) {
        if (indexOf(t.date, prefixes[i][1], 0) >= 0) continue;
        fallback = prefixes[i][1] + fallback;
        st.p[prefixes[i][0]] = {__proto__: null, first: '', second: best(fallback)};
    }
    const datePattern = best(t.date);
    const timeIndices = [IV_AM_PM, IV_HOUR, IV_MINUTE];
    for (let i = 0; i < timeIndices.length; i++) {
        const ip = st.p[timeIndices[i]];
        if (ip === undefined || ip.first === '') continue;
        st.p[timeIndices[i]] = splitInterval(simpleFormat(st.glue, [ip.first + ip.second, datePattern]));
    }
    return st;
}
// ICU's normalizeHourMetacharacters: the hour field becomes one letter,
// followed (for a 12-hour letter) by the day period at the width the two
// fields' lengths call for; the original day period goes. The letter is
// the skeleton's own: a format's pattern already holds its hc's letter.
function normalizeHourLetters(skeleton) {
    let hourCh = '', hourAt = 0, hourLen = 0, periodCh = '', periodAt = 0, periodLen = 0;
    for (let i = 0; i < skeleton.length; i++) {
        const c = fromCharCode(codeAt(skeleton, i));
        if (c === 'h' || c === 'H' || c === 'k' || c === 'K') {
            if (hourCh === '') { hourCh = c; hourAt = i; }
            hourLen++;
        } else if (c === 'a' || c === 'b' || c === 'B') {
            if (periodCh === '') { periodCh = c; periodAt = i; }
            periodLen++;
        } else if (hourCh !== '' && periodCh !== '') break;
    }
    if (hourCh === '') return skeleton;
    if (periodCh === '') periodCh = 'a';
    let replacement = hourCh;
    if (hourCh === 'h' || hourCh === 'K') {
        const n = periodLen >= 5 || hourLen >= 5 ? 5 : periodLen >= 3 || hourLen >= 3 ? 3 : 1;
        for (let i = 0; i < n; i++) replacement += periodCh;
    }
    let result = slice(skeleton, 0, hourAt) + replacement + slice(skeleton, hourAt + hourLen);
    if (periodLen > 0) {
        if (periodAt > hourAt) periodAt += replacement.length - hourLen;
        result = slice(result, 0, periodAt) + slice(result, periodAt + periodLen);
    }
    return result;
}
// ICU's getDateTimeSkeleton: the date and time halves of a skeleton, and
// their normalized forms (one hour, minute and zone letter; a numeric
// month and a short weekday one letter).
function splitDateTime(skeleton) {
    let date = '', normDate = '', time = '', normTime = '', hourCh = '';
    let E = 0, d = 0, M = 0, y = 0, m = 0, v = 0, z = 0;
    for (let i = 0; i < skeleton.length; i++) {
        const c = fromCharCode(codeAt(skeleton, i));
        switch (c) {
        case 'E': date += c; E++; break;
        case 'd': date += c; d++; break;
        case 'M': date += c; M++; break;
        case 'y': date += c; y++; break;
        case 'G': case 'Y': case 'u': case 'Q': case 'q': case 'L': case 'l': case 'W': case 'w': case 'D':
        case 'F': case 'g': case 'e': case 'c': case 'U': case 'r':
            normDate += c; date += c; break;
        case 'h': case 'H': case 'k': case 'K': time += c; if (hourCh === '') hourCh = c; break;
        case 'm': time += c; m++; break;
        case 'z': time += c; z++; break;
        case 'v': time += c; v++; break;
        case 'a': case 'V': case 'Z': case 'j': case 's': case 'S': case 'A': case 'b': case 'B':
            time += c; normTime += c; break;
        }
    }
    for (let i = 0; i < y; i++) normDate += 'y';
    if (M > 0) { if (M < 3) normDate += 'M'; else for (let i = 0; i < M && i < 5; i++) normDate += 'M'; }
    if (E > 0) { if (E <= 3) normDate += 'E'; else for (let i = 0; i < E && i < 5; i++) normDate += 'E'; }
    if (d > 0) normDate += 'd';
    if (hourCh !== '') normTime += hourCh;
    if (m > 0) normTime += 'm';
    if (z > 0) normTime += 'z';
    if (v > 0) normTime += 'v';
    return {__proto__: null, date: date, normDate: normDate, time: time, normTime: normTime};
}
// ICU's setSeparateDateTimePtn: the interval patterns of the time half
// (or of the date half, when there is no time), and the halves' own
// patterns for the fallbacks. False when no skeleton matches the fields.
function separateDateTime(st, t, best) {
    let skeleton = t.normTime !== '' ? t.normTime : t.normDate;
    const found = bestIntervalSkeleton(skeleton);
    if (found.skeleton === null) return false;
    // The fallbacks' halves are the NORMALIZED halves' patterns.
    if (t.date !== '') st.datePattern = best(t.normDate);
    if (t.time !== '') st.timePattern = best(t.normTime);
    if (found.info === -1) return false;
    let bestSkeleton = found.skeleton;
    if (t.time === '') {
        // Once the month's lookup extends the skeletons, ICU's year and era
        // lookups read the extension's own strings, which each of them
        // extends again ("Md", then "yMd" for the year, "GyMd" for the era).
        const ext = {__proto__: null, skeleton: '', best: ''};
        setIntervalPattern(st, IV_DATE, skeleton, bestSkeleton, found.info, ext, false);
        const extended = setIntervalPattern(st, IV_MONTH, skeleton, bestSkeleton, found.info, ext, false);
        if (extended) { skeleton = ext.skeleton; bestSkeleton = ext.best; }
        setIntervalPattern(st, IV_YEAR, skeleton, bestSkeleton, found.info, ext, extended);
        if (extended) { skeleton = ext.skeleton; bestSkeleton = ext.best; }
        setIntervalPattern(st, IV_ERA, skeleton, bestSkeleton, found.info, ext, extended);
    } else {
        setIntervalPattern(st, IV_MINUTE, skeleton, bestSkeleton, found.info, null, false);
        setIntervalPattern(st, IV_HOUR, skeleton, bestSkeleton, found.info, null, false);
        setIntervalPattern(st, IV_AM_PM, skeleton, bestSkeleton, found.info, null, false);
    }
    return true;
}
// ICU's setIntervalPattern for one index. A date field the matched
// skeleton has no pattern for is looked up with that field's letter
// added to both skeletons ("yMMMd" for a year difference of "MMMd");
// true when that extension found one, so the coarser fields use it too.
// `aliased`: the skeletons ARE the extension's (ICU passes the same
// strings), so extending it extends them.
function setIntervalPattern(st, index, skeleton, bestSkeleton, info, ext, aliased) {
    let pattern = intervalPatternOf(bestSkeleton, index);
    if (pattern === '') {
        if (fieldUnitIgnored(bestSkeleton, index)) return false;
        if (index === IV_AM_PM) {
            // A 24-hour skeleton has no AM/PM pattern: the hour's serves.
            pattern = intervalPatternOf(bestSkeleton, IV_HOUR);
            if (pattern !== '') st.p[index] = splitInterval(adjustFieldWidth(skeleton, bestSkeleton, pattern, info));
            return false;
        }
        if (ext !== null) {
            const letter = IV_LETTER[index];
            ext.skeleton = letter + skeleton;
            ext.best = letter + bestSkeleton;
            if (aliased) { skeleton = ext.skeleton; bestSkeleton = ext.best; }
            pattern = intervalPatternOf(ext.best, index);
            if (pattern === '' && info === 0) {
                const near = bestIntervalSkeleton(ext.best);
                if (near.skeleton !== null && near.info !== -1) {
                    pattern = intervalPatternOf(near.skeleton, index);
                    bestSkeleton = near.skeleton;
                    info = near.info;
                }
            }
        }
    }
    if (pattern === '') return false;
    st.p[index] = splitInterval(info !== 0 ? adjustFieldWidth(skeleton, bestSkeleton, pattern, info) : pattern);
    return ext !== null && ext.skeleton !== '';
}

// The index of the largest calendar field that differs between the two
// ends, in the format's zone; -1 when none does.
function largestDifference(fx, fy) {
    if ((fx.year > 0) !== (fy.year > 0)) return IV_ERA;
    if (fx.year !== fy.year) return IV_YEAR;
    if (fx.month !== fy.month) return IV_MONTH;
    if (fx.day !== fy.day) return IV_DATE;
    if ((fx.hour < 12) !== (fy.hour < 12)) return IV_AM_PM;
    if (fx.hour % 12 !== fy.hour % 12) return IV_HOUR;
    if (fx.minute !== fy.minute) return IV_MINUTE;
    if (fx.second !== fy.second) return IV_SECOND;
    if (fx.ms !== fy.ms) return IV_MILLISECOND;
    return -1;
}
// ICU's fallbackRange: both ends whole, joined by the fallback pattern.
function fallbackRange(s, pattern, x, y, out) {
    const at0 = indexOf(IV_FALLBACK, '{0}', 0), at1 = indexOf(IV_FALLBACK, '{1}', 0);
    appendParts(out, formatParts(s, pattern, x));
    appendParts(out, [{__proto__: null, type: 'literal', value: slice(IV_FALLBACK, at0 + 3, at1)}]);
    appendParts(out, formatParts(s, pattern, y));
}
// ICU's fallbackFormat: on one day, the date once beside the time's
// fallback range, joined by the date-time glue; otherwise both ends whole.
function fallbackFormat(s, st, pattern, x, y, sameDay, out) {
    if (!sameDay || st.datePattern === null || st.timePattern === null) { fallbackRange(s, pattern, x, y, out); return; }
    const at0 = indexOf(st.glue, '{0}', 0), at1 = indexOf(st.glue, '{1}', 0);
    const between = [{__proto__: null, type: 'literal', value: slice(st.glue, (at0 < at1 ? at0 : at1) + 3, at0 < at1 ? at1 : at0)}];
    if (at0 < at1) {
        fallbackRange(s, st.timePattern, x, y, out);
        appendParts(out, between);
        appendParts(out, formatParts(s, st.datePattern, x));
    } else {
        appendParts(out, formatParts(s, st.datePattern, x));
        appendParts(out, between);
        fallbackRange(s, st.timePattern, x, y, out);
    }
}
// Parts appended, literal text that meets merged as other browsers merge it.
function appendParts(out, parts) {
    for (let i = 0; i < parts.length; i++) {
        const last = out.length > 0 ? out[out.length - 1] : null;
        if (parts[i].type === 'literal' && last !== null && last.type === 'literal') last.value += parts[i].value;
        else push(out, parts[i]);
    }
}
// The parts of a range, or null when the ends are "practically equal"
// (the formatter shows no difference, and other browsers format the
// start alone). Sources are ICU's overlap spans: from the first field that
// appears twice to the last, in each half; everything else is shared.
function intervalParts(s, x, y) {
    const st = intervalFormatOf(s);
    const index = largestDifference(localFields(s, x), localFields(s, y));
    if (index < 0) return null;
    const sameDay = index >= IV_AM_PM;
    const ip = st.p[index];
    const out = [];
    if (ip === undefined) {
        if (fieldUnitIgnored(st.single, index)) return null;
        fallbackFormat(s, st, st.single, x, y, sameDay, out);
    } else if (ip.first === '') {
        fallbackFormat(s, st, ip.second, x, y, sameDay, out);
    } else {
        appendParts(out, formatParts(s, ip.first, x));
        if (ip.second !== '') appendParts(out, formatParts(s, ip.second, y));
    }
    let s1a = -1, s1b = -1, s2a = -1, s2b = -1;
    for (let i = 0; i < out.length; i++) {
        if (out[i].ch === undefined) continue;
        for (let j = i + 1; j < out.length; j++) {
            if (out[j].ch !== out[i].ch) continue;
            if (s1a < 0 || i < s1a) s1a = i;
            if (i > s1b) s1b = i;
            if (s2a < 0 || j < s2a) s2a = j;
            if (j > s2b) s2b = j;
            break;
        }
    }
    if (s1a < 0) return null;
    for (let i = 0; i < out.length; i++)
        out[i].source = i >= s1a && i <= s1b ? 'startRange' : i >= s2a && i <= s2b ? 'endRange' : 'shared';
    return out;
}
// A range's parts. When the ends are practically equal, they are the
// start's alone, all shared.
function formatRangeParts(s, x, y) {
    const parts = intervalParts(s, x, y);
    if (parts !== null) return parts;
    const single = formatParts(s, s.pattern, x);
    for (let i = 0; i < single.length; i++) single[i].source = 'shared';
    return single;
}

// ── The prototype ───────────────────────────────────────────────────────
method(DateTimeFormat, 'supportedLocalesOf', methodOf(supportedLocalesOf), 1);
defineProperty(DateTimeFormatPrototype, toStringTag, {__proto__: null, value: 'Intl.DateTimeFormat', writable: false, enumerable: false, configurable: true});
getter(DateTimeFormatPrototype, 'format', function () {
    const s = UnwrapDateTimeFormat(this, 'format');
    if (s.bound === undefined) {
        const bound = (date) => asciiSpaces(partsToString(formatParts(s, s.pattern, toInstant(date))));
        defineProperty(bound, 'name', {__proto__: null, value: '', configurable: true});
        s.bound = bound;
    }
    return s.bound;
});
method(DateTimeFormatPrototype, 'formatToParts', methodOf(function (date) {
    const s = UnwrapDateTimeFormat(this, 'formatToParts');
    return partsToArray(formatParts(s, s.pattern, toInstant(date)));
}), 1);
method(DateTimeFormatPrototype, 'formatRange', methodOf(function (startDate, endDate) {
    const s = UnwrapDateTimeFormat(this, 'formatRange');
    if (startDate === undefined || endDate === undefined) throw new TypeError_('startDate and endDate are required');
    const xn = ToNumber(startDate), yn = ToNumber(endDate);       // both converted, then clipped
    const x = timeClip(xn), y = timeClip(yn);
    return asciiSpaces(partsToString(formatRangeParts(s, x, y)));
}), 2);
method(DateTimeFormatPrototype, 'formatRangeToParts', methodOf(function (startDate, endDate) {
    const s = UnwrapDateTimeFormat(this, 'formatRangeToParts');
    if (startDate === undefined || endDate === undefined) throw new TypeError_('startDate and endDate are required');
    const xn = ToNumber(startDate), yn = ToNumber(endDate);       // both converted, then clipped
    const x = timeClip(xn), y = timeClip(yn);
    return partsToArray(formatRangeParts(s, x, y));
}), 2);
// resolvedOptions: what the pattern holds, read back as other browsers
// read it (a minute is "2-digit" whenever the pattern pads it).
const RESOLVED_FROM = {__proto__: null,
    G: ['era', len => len === 4 ? 'long' : len === 5 ? 'narrow' : 'short'],
    y: ['year', len => len === 2 ? '2-digit' : 'numeric'],
    M: ['month', len => len === 1 ? 'numeric' : len === 2 ? '2-digit' : len === 3 ? 'short' : len === 4 ? 'long' : 'narrow'],
    L: ['month', len => len === 1 ? 'numeric' : len === 2 ? '2-digit' : len === 3 ? 'short' : len === 4 ? 'long' : 'narrow'],
    d: ['day', len => len === 2 ? '2-digit' : 'numeric'],
    E: ['weekday', len => len === 4 ? 'long' : len === 5 ? 'narrow' : 'short'],
    c: ['weekday', len => len === 4 ? 'long' : len === 5 ? 'narrow' : 'short'],
    B: ['dayPeriod', len => len === 4 ? 'long' : len === 5 ? 'narrow' : 'short'],
    h: ['hour', len => len === 2 ? '2-digit' : 'numeric'], H: ['hour', len => len === 2 ? '2-digit' : 'numeric'],
    K: ['hour', len => len === 2 ? '2-digit' : 'numeric'], k: ['hour', len => len === 2 ? '2-digit' : 'numeric'],
    m: ['minute', len => len === 2 ? '2-digit' : 'numeric'],
    s: ['second', len => len === 2 ? '2-digit' : 'numeric'],
    S: ['fractionalSecondDigits', len => len],
    z: ['timeZoneName', len => len === 4 ? 'long' : 'short'],
    O: ['timeZoneName', len => len === 4 ? 'longOffset' : 'shortOffset'],
    v: ['timeZoneName', len => len === 4 ? 'longGeneric' : 'shortGeneric'],
};
method(DateTimeFormatPrototype, 'resolvedOptions', methodOf(function () {
    const s = UnwrapDateTimeFormat(this, 'resolvedOptions');
    const o = {};
    put(o, 'locale', s.locale); put(o, 'calendar', s.calendar); put(o, 'numberingSystem', s.numberingSystem);
    put(o, 'timeZone', s.timeZone);
    if (s.hourCycle !== undefined) { put(o, 'hourCycle', s.hourCycle); put(o, 'hour12', s.hourCycle === 'h11' || s.hourCycle === 'h12'); }
    if (s.dateStyle === undefined && s.timeStyle === undefined) {
        const found = {__proto__: null};
        const items = tokenize(s.pattern);
        for (let i = 0; i < items.length; i++) {
            const r = own(RESOLVED_FROM, items[i].ch);
            if (r !== undefined) found[r[0]] = r[1](items[i].len);
        }
        for (let i = 0; i < COMPONENTS.length; i++) {
            const prop = COMPONENTS[i][0];
            if (own(found, prop) !== undefined) put(o, prop, own(found, prop));
        }
    } else {
        if (s.dateStyle !== undefined) put(o, 'dateStyle', s.dateStyle);
        if (s.timeStyle !== undefined) put(o, 'timeStyle', s.timeStyle);
    }
    return o;
}), 0);

// ── The Intl object ─────────────────────────────────────────────────────
const Intl = {};     // script-visible: its properties are defined, below
defineProperty(Intl, toStringTag, {__proto__: null, value: 'Intl', writable: false, enumerable: false, configurable: true});
defineProperty(Intl, 'Locale', {__proto__: null, value: Locale, writable: true, enumerable: false, configurable: true});
defineProperty(Intl, 'DateTimeFormat', {__proto__: null, value: DateTimeFormat, writable: true, enumerable: false, configurable: true});
return Intl;
})
