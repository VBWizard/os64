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
 *   I.zoneList       () → every canonical zone name the table holds
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
 * captured strings (INTL.md § 8); numbers, until Chrome's capture holds
 * them, by test262 and by V8 where test262 is silent (§ 9.6). */
(function (I) {
'use strict';

// ── The captures ────────────────────────────────────────────────────────
const Object_ = Object, Array_ = Array, String_ = String, Number_ = Number, Math_ = Math;
const TypeError_ = TypeError, RangeError_ = RangeError, Symbol_ = Symbol, Date_ = Date, BigInt_ = BigInt;
const Float64Array_ = Float64Array;
const apply = Reflect.apply;
const defineProperty = Object_.defineProperty;
const getOwnPropertyDescriptor = Object_.getOwnPropertyDescriptor;
const objectCreate = Object_.create;
const toStringTag = Symbol_.toStringTag, SymbolToPrimitive = Symbol_.toPrimitive;
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
const zoneCanonical = I.zoneCanonical, zoneOffset = I.zoneOffset, zoneHost = I.zoneHost, zoneList = I.zoneList;
// The methods that read a built-in's own value (thisTimeValue and its kin),
// which refuse a receiver without the slot as the engine's own do.
const dateGetTime = Date_.prototype.getTime, numberValueOf = Number_.prototype.valueOf;
const bigintValueOf = BigInt_.prototype.valueOf;

// THE DATA, loaded by the first construction in a realm (INTL.md § 6, the
// § 9.3 ruling): the constructors and prototypes are installed whole and
// eagerly, and a realm that never constructs one never evaluates CLDR's
// tables. Every door into them calls loadData first: the constructors and
// supportedLocalesOf; a method has an instance, so its realm has loaded.
let ALIASES, U_VALUE_ALIASES, LIKELY, LIKELY_REGIONS, G, Z, IV, IV_FALLBACK, DAY_PERIOD_RULES, LISTS, NUMBER_DATA, COLLATION_DATA;
let dataLoaded = false;
function loadData() {
    if (dataLoaded) return;
    const D = I.data();
    ALIASES = D.aliases; U_VALUE_ALIASES = D.uAliases; LIKELY = D.likely; LIKELY_REGIONS = D.likelyRegions; G = D.gregorian; Z = D.zones;
    IV = G.intervalFormats; IV_FALLBACK = own(IV, 'intervalFormatFallback'); DAY_PERIOD_RULES = D.dayPeriods;
    LISTS = D.lists; NUMBER_DATA = D.numbers; COLLATION_DATA = D.collation;
    dataLoaded = true;
}
// The number tables are a deferred part of the data, which the generator
// writes as a function, so a realm that formats only dates never builds
// them: NumberFormat, PluralRules and RelativeTimeFormat build them at
// their first construction.
let NUM, UNITS, PLURALS, RELATIVE, CURRENT_CURRENCIES;
// Collator's elements are a deferred part of the data too, built by the
// first Collator.
// The table is read into a dense array by slot (collationSlot), so a
// compare looks an element up without a property lookup, and the primary
// of each character that is one element with a primary into a typed array
// (SIMPLE_PRIMARY; 0 for any other, SIMPLE_VARIABLE marking the variable
// ones), which compareStrings walks without a call.
let COLLATION, COLLATION_DENSE, SIMPLE_PRIMARY, SIMPLE_VARIABLE;
function loadCollationData() {
    loadData();
    if (COLLATION !== undefined) return;
    const data = apply(COLLATION_DATA, undefined, []), dense = [];
    for (let r = 0; r < COLLATION_RANGES.length; r++)
        for (let cp = COLLATION_RANGES[r][0]; cp <= COLLATION_RANGES[r][1]; cp++) {
            const ces = own(data.singles, String_(cp));
            push(dense, ces === undefined ? null : ces);
        }
    const simple = new Float64Array_(dense.length), variable = new Float64Array_(dense.length);
    for (let slot = 0; slot < dense.length; slot++) {
        const ces = dense[slot];
        if (ces === null || ces.length !== 1 || elementPrimary(ces[0]) === 0) continue;
        simple[slot] = elementPrimary(ces[0]);
        variable[slot] = elementVariable(ces[0]) ? 1 : 0;
    }
    COLLATION_DENSE = dense; SIMPLE_PRIMARY = simple; SIMPLE_VARIABLE = variable;
    COLLATION = data;              // last: it says the table is whole
}
function loadNumberData() {
    loadData();
    if (NUM !== undefined) return;
    const N = apply(NUMBER_DATA, undefined, []);
    UNITS = N.units; PLURALS = N.plurals; RELATIVE = N.relative; CURRENT_CURRENCIES = N.currentCurrencies;
    NUM = N.numbers;      // last: it says the tables are here
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
const zeros = n => n > 0 ? pad('', n) : '';      // n zeros, written natively (n may be large)
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
        // The table writes a region in canonical case (sgn-US), the parse in lowercase.
        if (match === null && t.region !== '' && own(langAliases, t.language + '-' + upper(t.region)) !== undefined)
            match = own(langAliases, t.language + '-' + upper(t.region)), matchedVariant = '';
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
    variantRules(t);
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
                // The region the language and script are likeliest in, when
                // it is among the choices (UTS 35 Annex C.10), else the
                // first. The data holds that region for the languages and
                // scripts it makes a difference to: the language with its
                // script, then alone. Likely subtags would go on to und with
                // the script for a language CLDR does not list; the table
                // cannot say which languages CLDR lists, so und with a
                // script is looked up only for und itself.
                let likely = t.script !== '' ? own(LIKELY_REGIONS, t.language + '-' + titleCase(t.script)) : undefined;
                if (likely === undefined && t.language !== 'und') likely = own(LIKELY_REGIONS, t.language);
                if (likely !== undefined)
                    for (let c = 0; c < choices.length; c++) if (choices[c] === likely) pick = choices[c];
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

// CLDR's language aliases rooted at "und" name variants and apply to any
// language: "und-hepburn-heploc" is "und-alalc97" (both variants go, one
// comes), "und-aaland" is "und-AX" (the variant goes, a region comes where
// the tag has none). A language's own rule ("hy-arevmda" is "hyw") is more
// specific, so these run after the language aliases.
let undVariantRules = null;      // [{source, replacement}], made once a realm
function variantRules(t) {
    if (undVariantRules === null) {
        const rules = [], langAliases = ALIASES.languageAlias;
        for (const key in langAliases) {
            if (!hasOwn(langAliases, key) || slice(key, 0, 4) !== 'und-') continue;
            const source = parseTag(key), replacement = parseTag(own(langAliases, key));
            if (source === null || replacement === null || source.script !== '' || source.region !== '' ||
                source.variants.length === 0) continue;
            push(rules, {__proto__: null, source: source, replacement: replacement});
        }
        undVariantRules = rules;
    }
    for (let i = 0; i < undVariantRules.length; i++) {
        const source = undVariantRules[i].source, r = undVariantRules[i].replacement;
        let all = true;
        for (let v = 0; v < source.variants.length && all; v++) all = contains(t.variants, source.variants[v]);
        if (!all) continue;
        const kept = [];
        for (let v = 0; v < t.variants.length; v++) if (!contains(source.variants, t.variants[v])) push(kept, t.variants[v]);
        for (let v = 0; v < r.variants.length; v++) if (!contains(kept, r.variants[v])) push(kept, r.variants[v]);
        t.variants = kept;
        if (r.language !== 'und' && t.language === 'und') t.language = r.language;
        if (t.script === '' && r.script !== '') t.script = r.script;
        if (t.region === '' && r.region !== '') t.region = r.region;
    }
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
    // Minutes and fractional seconds with no seconds gain seconds (ICU's
    // #20739): a fraction is of a second.
    if (sk.ch[F_MINUTE] !== '' && sk.ch[F_FRACTION] !== '' && sk.ch[F_SECOND] === '') {
        sk.ch[F_SECOND] = 's'; sk.len[F_SECOND] = 1; sk.baseCh[F_SECOND] = 's'; sk.baseLen[F_SECOND] = 1;
        sk.type[F_SECOND] = DT_NUMERIC + 1;
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
// Each field's appendItems key and display name in CLDR; a field CLDR
// gives neither keeps ICU's defaults: the template "{0} ├{2}: {1}┤" and the
// name "F" and its field number (a fractional second is "F14", as Chrome
// prints it).
const APPEND_ITEM = ['Era', 'Year', 'Quarter', 'Month', 'Week', 'Week', 'Day-Of-Week', 'Day', 'Day', 'Day', '', 'Hour',
                     'Minute', 'Second', '', 'Timezone'];
const FIELD_NAME = ['era', 'year', 'quarter', 'month', 'week', 'week', 'weekday', 'day', 'day', 'day', 'dayperiod', 'hour',
                    'minute', 'second', '', 'zone'];
const APPEND_DEFAULT = '{0} \u251c{2}: {1}\u2524';
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
        const name = own(G.fieldNames, FIELD_NAME[top]);
        result = simpleFormat(item !== undefined && item !== '' ? item : APPEND_DEFAULT,
                              [result, tempPattern, "'" + (name !== undefined ? name : 'F' + top) + "'"]);
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

// A receiver's formatter: its own slots, or, for format and
// resolvedOptions alone (UnwrapDateTimeFormat), a legacy wrapper's hidden
// one; the other methods require the slots themselves.
function thisDateTimeFormat(dtf, name) {
    const s = slots(DTFS, dtf);
    if (s === undefined) throw new TypeError_('Method Intl.DateTimeFormat.prototype.' + name + ' called on incompatible receiver');
    return s;
}
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
// A flexible day period: "noon" when the hour is 12 and the minutes and
// seconds the pattern shows (its fields, not its quoted text) are 0.
function dayPeriodFlexible(f, width, pattern) {
    const periods = own(own(G.dayPeriods, 'format'), width);
    let hasMinute = false, hasSecond = false;
    const items = tokenize(pattern);
    for (let i = 0; i < items.length; i++) {
        if (items[i].ch === 'm') hasMinute = true;
        if (items[i].ch === 's') hasSecond = true;
    }
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
// Parts' text, joined once (a list's parts are as many as its elements).
function partsToString(parts) { const values = []; for (let i = 0; i < parts.length; i++) push(values, parts[i].value); return join(values, ''); }
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
    const s = thisDateTimeFormat(this, 'formatToParts');
    return partsToArray(formatParts(s, s.pattern, toInstant(date)));
}), 1);
method(DateTimeFormatPrototype, 'formatRange', methodOf(function (startDate, endDate) {
    const s = thisDateTimeFormat(this, 'formatRange');
    if (startDate === undefined || endDate === undefined) throw new TypeError_('startDate and endDate are required');
    const xn = ToNumber(startDate), yn = ToNumber(endDate);       // both converted, then clipped
    const x = timeClip(xn), y = timeClip(yn);
    return asciiSpaces(partsToString(formatRangeParts(s, x, y)));
}), 2);
method(DateTimeFormatPrototype, 'formatRangeToParts', methodOf(function (startDate, endDate) {
    const s = thisDateTimeFormat(this, 'formatRangeToParts');
    if (startDate === undefined || endDate === undefined) throw new TypeError_('startDate and endDate are required');
    const xn = ToNumber(startDate), yn = ToNumber(endDate);       // both converted, then clipped
    const x = timeClip(xn), y = timeClip(yn);
    return partsToArray(formatRangeParts(s, x, y));
}), 2);
// resolvedOptions: what the pattern holds, read back as V8 reads it: each
// option by the longest run of its letter ANYWHERE in the pattern, quoted
// text included (V8 finds "MMM" by substring, so an appended "'AM/PM'"
// reports a numeric month, and Chrome says so). Within one option the
// letters are asked in V8's order: M before L, H before h, k and K.
const RESOLVED_FROM = [
    ['G', 'era', n => n === 4 ? 'long' : n >= 5 ? 'narrow' : 'short'],
    ['y', 'year', n => n >= 2 ? '2-digit' : 'numeric'],
    ['M', 'month', n => n >= 5 ? 'narrow' : n === 4 ? 'long' : n === 3 ? 'short' : n === 2 ? '2-digit' : 'numeric'],
    ['L', 'month', n => n >= 5 ? 'narrow' : n === 4 ? 'long' : n === 3 ? 'short' : n === 2 ? '2-digit' : 'numeric'],
    ['d', 'day', n => n >= 2 ? '2-digit' : 'numeric'],
    ['E', 'weekday', n => n >= 5 ? 'narrow' : n === 4 ? 'long' : 'short'],
    ['c', 'weekday', n => n >= 5 ? 'narrow' : n === 4 ? 'long' : 'short'],
    ['B', 'dayPeriod', n => n >= 5 ? 'narrow' : n === 4 ? 'long' : 'short'],
    ['H', 'hour', n => n >= 2 ? '2-digit' : 'numeric'], ['h', 'hour', n => n >= 2 ? '2-digit' : 'numeric'],
    ['k', 'hour', n => n >= 2 ? '2-digit' : 'numeric'], ['K', 'hour', n => n >= 2 ? '2-digit' : 'numeric'],
    ['m', 'minute', n => n >= 2 ? '2-digit' : 'numeric'],
    ['s', 'second', n => n >= 2 ? '2-digit' : 'numeric'],
    ['S', 'fractionalSecondDigits', n => n > 3 ? 3 : n],
    ['z', 'timeZoneName', n => n >= 4 ? 'long' : 'short'],
    ['O', 'timeZoneName', n => n >= 4 ? 'longOffset' : 'shortOffset'],
    ['v', 'timeZoneName', n => n >= 4 ? 'longGeneric' : 'shortGeneric'],
];
// The longest run of each character in `text`.
function longestRuns(text) {
    const runs = {__proto__: null};
    for (let i = 0; i < text.length;) {
        let j = i + 1;
        while (j < text.length && codeAt(text, j) === codeAt(text, i)) j++;
        const c = fromCharCode(codeAt(text, i));
        if ((own(runs, c) || 0) < j - i) runs[c] = j - i;
        i = j;
    }
    return runs;
}
method(DateTimeFormatPrototype, 'resolvedOptions', methodOf(function () {
    const s = UnwrapDateTimeFormat(this, 'resolvedOptions');
    const o = {};
    put(o, 'locale', s.locale); put(o, 'calendar', s.calendar); put(o, 'numberingSystem', s.numberingSystem);
    put(o, 'timeZone', s.timeZone);
    if (s.hourCycle !== undefined) { put(o, 'hourCycle', s.hourCycle); put(o, 'hour12', s.hourCycle === 'h11' || s.hourCycle === 'h12'); }
    if (s.dateStyle === undefined && s.timeStyle === undefined) {
        const found = {__proto__: null}, runs = longestRuns(s.pattern);
        for (let i = 0; i < RESOLVED_FROM.length; i++) {
            const ch = RESOLVED_FROM[i][0], prop = RESOLVED_FROM[i][1], value = RESOLVED_FROM[i][2];
            const n = own(runs, ch);
            if (n !== undefined && own(found, prop) === undefined) found[prop] = value(n);
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


// ── Numbers: exact decimals (ECMA-402 § 16.5) ───────────────────────────
// A mathematical value as Intl reads it: a Number by its shortest round-
// tripping decimal (ICU's double-conversion, Number.prototype.toString's
// digits), a BigInt exactly, a numeric string exactly unless it is past
// the Numbers' range (then infinity). {neg, digits, exp}: the
// value digits × 10^exp, digits without leading or trailing zeros ("0"
// for zero); or {neg, nan} / {neg, inf}.
function decimal(neg, digits, exp) {
    let start = 0;
    while (start < digits.length - 1 && codeAt(digits, start) === 48) start++;
    let end = digits.length;
    while (end > start + 1 && codeAt(digits, end - 1) === 48) { end--; exp++; }
    digits = slice(digits, start, end);
    if (digits === '0') exp = 0;
    return {__proto__: null, neg: neg, digits: digits, exp: exp};
}
const isZeroDecimal = d => d.digits === '0';
// Digits of a plain decimal literal: [int][.frac][e±n], already validated.
function decimalOfLiteral(neg, text) {
    let mantissa = text, exp = 0;
    let at = indexOf(text, 'e', 0);
    if (at < 0) at = indexOf(text, 'E', 0);
    if (at >= 0) { mantissa = slice(text, 0, at); exp = +slice(text, at + 1, text.length); }
    const dot = indexOf(mantissa, '.', 0);
    let digits = mantissa;
    if (dot >= 0) {
        const fraction = slice(mantissa, dot + 1, mantissa.length);
        digits = slice(mantissa, 0, dot) + fraction;
        exp -= fraction.length;
    }
    if (digits === '') digits = '0';
    return decimal(neg, digits, exp);
}
function decimalOfNumber(x) {
    if (x !== x) return {__proto__: null, neg: false, nan: true};
    const neg = x < 0 || (x === 0 && 1 / x < 0);
    if (x === Infinity || x === -Infinity) return {__proto__: null, neg: neg, inf: true};
    const d = decimalOfLiteral(neg, String_(abs(x)));
    d.number = true;                          // read from a Number, which a range compares as one
    return d;
}
// StringToNumber's whitespace (ECMA-262 StrWhiteSpaceChar).
function isStrWhiteSpace(c) {
    return c === 9 || c === 10 || c === 11 || c === 12 || c === 13 || c === 32 || c === 0xA0 || c === 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c === 0x2028 || c === 0x2029 || c === 0x202F || c === 0x205F ||
           c === 0x3000 || c === 0xFEFF;
}
// A string's exact value (ECMA-402 StringIntlMV): StringNumericLiteral's
// grammar, "-0" negative zero, NaN for anything else.
function decimalOfString(text) {
    let a = 0, b = text.length;
    while (a < b && isStrWhiteSpace(codeAt(text, a))) a++;
    while (b > a && isStrWhiteSpace(codeAt(text, b - 1))) b--;
    let s = slice(text, a, b);
    if (s === '') return decimal(false, '0', 0);
    const c0 = codeAt(s, 0), c1 = s.length > 1 ? codeAt(s, 1) | 32 : 0;
    if (c0 === 48 && (c1 === 120 || c1 === 111 || c1 === 98)) {          // 0x 0o 0b
        const ok = c1 === 120 ? exec(/^0[xX][0-9a-fA-F]+$/, s) : c1 === 111 ? exec(/^0[oO][0-7]+$/, s) : exec(/^0[bB][01]+$/, s);
        return ok === null ? {__proto__: null, neg: false, nan: true} : decimal(false, String_(BigInt_(s)), 0);
    }
    let neg = false;
    if (c0 === 43 || c0 === 45) { neg = c0 === 45; s = slice(s, 1, s.length); }
    if (s === 'Infinity') return {__proto__: null, neg: neg, inf: true};
    if (exec(/^(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][+-]?[0-9]+)?$/, s) === null) return {__proto__: null, neg: false, nan: true};
    // RoundMVResult's large half: a value past the Numbers is infinity. Its
    // small half, a nonzero value that rounds to a Number's zero, V8 keeps
    // exact ("1e-400" with three significant digits writes 404 characters),
    // and so does this (INTL.md § 9.6).
    if (!isFinite_(+s)) return {__proto__: null, neg: neg, inf: true};
    return decimalOfLiteral(neg, s);
}
// ECMA-262 ToPrimitive(value, number), on intrinsics captured at load.
function ToPrimitiveNumber(o) {
    const exotic = o[SymbolToPrimitive];
    if (exotic !== undefined && exotic !== null) {
        if (typeof exotic !== 'function') throw new TypeError_('Symbol.toPrimitive is not a function');
        const r = apply(exotic, o, ['number']);
        if (isObject(r)) throw new TypeError_('Cannot convert object to primitive value');
        return r;
    }
    const names = ['valueOf', 'toString'];
    for (let i = 0; i < 2; i++) {
        const f = o[names[i]];
        if (typeof f === 'function') {
            const r = apply(f, o, []);
            if (!isObject(r)) return r;
        }
    }
    throw new TypeError_('Cannot convert object to primitive value');
}
// ECMA-402 ToIntlMathematicalValue.
function ToIntlMathematicalValue(value) {
    const prim = isObject(value) ? ToPrimitiveNumber(value) : value;
    if (typeof prim === 'bigint') return decimal(prim < 0n, String_(prim < 0n ? -prim : prim), 0);
    if (typeof prim === 'string') return decimalOfString(prim);
    return decimalOfNumber(ToNumber(prim));
}
// The power of ten of a nonzero value's leading digit.
const magnitudeOf = d => d.digits.length + d.exp - 1;
// A digit string plus one, carrying.
function incrementDigits(s) {
    let out = '', i = s.length - 1, carry = 1;
    for (; i >= 0 && carry; i--) {
        const c = codeAt(s, i) - 48 + carry;
        carry = c === 10 ? 1 : 0;
        out = fromCharCode(48 + (c % 10)) + out;
    }
    return (carry ? '1' : '') + slice(s, 0, i + 1) + out;
}
// GetUnsignedRoundingMode: the mode on |x|, which side "up" is.
function unsignedRoundingMode(mode, negative) {
    switch (mode) {
    case 'ceil': return negative ? 'zero' : 'infinity';
    case 'floor': return negative ? 'infinity' : 'zero';
    case 'expand': return 'infinity';
    case 'trunc': return 'zero';
    case 'halfCeil': return negative ? 'half-zero' : 'half-infinity';
    case 'halfFloor': return negative ? 'half-infinity' : 'half-zero';
    case 'halfExpand': return 'half-infinity';
    case 'halfTrunc': return 'half-zero';
    }
    return 'half-even';
}
// Whether to round away from zero: `half` is -1, 0 or 1 as the dropped
// part is below, at or above half a step; `odd` the kept part's parity.
function roundsUp(urm, half, odd) {
    if (urm === 'infinity') return true;
    if (urm === 'zero') return false;
    if (half !== 0) return half > 0;
    return urm === 'half-infinity' || (urm === 'half-even' && odd);
}
// |d| rounded to a multiple of increment × 10^pos (unsigned).
function roundDecimal(d, pos, increment, urm) {
    if (isZeroDecimal(d) || (increment === 1 && d.exp >= pos)) return d;
    if (increment !== 1) {
        // A value under a tenth of the step is below half of it, whatever
        // the increment: it rounds to 0 or to one step, without the power
        // of ten its exponent would ask for.
        if (magnitudeOf(d) < pos - 1)
            return roundsUp(urm, -1, false) ? decimal(false, String_(increment), pos) : decimal(false, '0', 0);
        const shift = d.exp - pos;
        let n = BigInt_(d.digits), den = BigInt_(increment);
        if (shift >= 0) n *= 10n ** BigInt_(shift); else den *= 10n ** BigInt_(-shift);
        let q = n / den;
        const r = n % den;
        if (r !== 0n) {
            const twice = r * 2n;
            if (roundsUp(urm, twice < den ? -1 : twice > den ? 1 : 0, q % 2n === 1n)) q += 1n;
        }
        return decimal(false, String_(q * BigInt_(increment)), pos);
    }
    const k = pos - d.exp, n = d.digits.length;
    const kept = k >= n ? '0' : slice(d.digits, 0, n - k);
    let half;
    if (k > n) half = -1;
    else {
        const first = codeAt(d.digits, n - k) - 48;
        half = first < 5 ? -1 : first > 5 ? 1 : (k > 1 ? 1 : 0);    // digits end nonzero: a rest is above
    }
    const odd = (codeAt(kept, kept.length - 1) - 48) % 2 === 1;
    return decimal(false, roundsUp(urm, half, odd) ? incrementDigits(kept) : kept, pos);
}
// The digits of |d| × 10^-pos as an integer string (d a multiple of 10^pos).
function integerDigitsAt(d, pos) {
    if (isZeroDecimal(d)) return '0';
    return d.digits + zeros(d.exp - pos);
}
// ToRawPrecision: {string, rounded, intDigits, magnitude}.
function toRawPrecision(x, minP, maxP, urm) {
    const p = maxP;
    let e, n;
    if (isZeroDecimal(x)) { e = 0; n = ''; for (let i = 0; i < p; i++) n += '0'; }
    else {
        e = magnitudeOf(x);
        let r = roundDecimal(x, e - p + 1, 1, urm);
        if (magnitudeOf(r) > e) e++;
        n = integerDigitsAt(r, e - p + 1);
    }
    let m, intDigits;
    if (e >= p - 1) { m = n + zeros(e - p + 1); intDigits = e + 1; }
    else if (e >= 0) { m = slice(n, 0, e + 1) + '.' + slice(n, e + 1, n.length); intDigits = e + 1; }
    else { m = '0.' + zeros(-(e + 1)) + n; intDigits = 1; }
    if (indexOf(m, '.', 0) >= 0 && maxP > minP) m = cutZeros(m, maxP - minP);
    return {__proto__: null, string: m, rounded: decimalOfLiteral(false, m), intDigits: intDigits, magnitude: e - p + 1};
}
// ToRawFixed.
function toRawFixed(x, minF, maxF, increment, urm) {
    const f = maxF;
    const r = roundDecimal(x, -f, increment, urm);
    let m = integerDigitsAt(r, -f), intDigits;
    if (f !== 0) {
        let k = m.length;
        if (k <= f) { let z = ''; for (let i = 0; i < f + 1 - k; i++) z += '0'; m = z + m; k = f + 1; }
        intDigits = k - f;
        m = slice(m, 0, k - f) + '.' + slice(m, k - f, k);
    } else intDigits = m.length;
    m = cutZeros(m, maxF - minF);
    return {__proto__: null, string: m, rounded: decimalOfLiteral(false, m), intDigits: intDigits, magnitude: -f};
}
// Up to `cut` trailing fraction zeros removed, then a bare decimal point.
function cutZeros(m, cut) {
    if (indexOf(m, '.', 0) < 0) return m;
    let end = m.length;
    while (cut > 0 && codeAt(m, end - 1) === 48) { end--; cut--; }
    if (codeAt(m, end - 1) === 46) end--;
    return slice(m, 0, end);
}
// FormatNumericToString on |x|: {string, rounded, intDigits}.
function formatNumericToString(o, x) {
    const urm = unsignedRoundingMode(o.roundingMode, x.neg);
    let result;
    if (o.roundingType === 'significantDigits') result = toRawPrecision(x, o.minSig, o.maxSig, urm);
    else if (o.roundingType === 'fractionDigits') result = toRawFixed(x, o.minFrac, o.maxFrac, o.roundingIncrement, urm);
    else {
        const s = toRawPrecision(x, o.minSig, o.maxSig, urm), f = toRawFixed(x, o.minFrac, o.maxFrac, o.roundingIncrement, urm);
        // morePrecision takes the finer rounding, lessPrecision the coarser;
        // on a tie the fraction digits are not "more precise", so
        // morePrecision takes the significant digits and lessPrecision the
        // fraction digits.
        const fixedIsMorePrecise = f.magnitude < s.magnitude;
        if (o.roundingType === 'morePrecision') result = fixedIsMorePrecise ? f : s;
        else result = fixedIsMorePrecise ? s : f;
    }
    let string = result.string, intDigits = result.intDigits;
    if (o.trailingZeroDisplay === 'stripIfInteger' && result.rounded.exp >= 0) {
        const dot = indexOf(string, '.', 0);
        if (dot >= 0) string = slice(string, 0, dot);
    }
    for (; intDigits < o.minInt; intDigits++) string = '0' + string;
    return {__proto__: null, string: string, rounded: result.rounded, intDigits: intDigits};
}

// ── Digit options (ECMA-402 § 16.1.3 SetNumberFormatDigitOptions) ───────
const ROUNDING_INCREMENTS = [1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000];
const ROUNDING_MODES = ['ceil', 'floor', 'expand', 'trunc', 'halfCeil', 'halfFloor', 'halfExpand', 'halfTrunc', 'halfEven'];
function SetNumberFormatDigitOptions(o, options, mnfdDefault, mxfdDefault, notation) {
    const mnid = GetNumberOption(options, 'minimumIntegerDigits', 1, 21, 1);
    let mnfd = options.minimumFractionDigits, mxfd = options.maximumFractionDigits;
    let mnsd = options.minimumSignificantDigits, mxsd = options.maximumSignificantDigits;
    o.minInt = mnid;
    const increment = GetNumberOption(options, 'roundingIncrement', 1, 5000, 1);
    if (!contains(ROUNDING_INCREMENTS, increment)) throw new RangeError_('roundingIncrement is not one of the allowed values');
    const mode = GetOption(options, 'roundingMode', 'string', ROUNDING_MODES, 'halfExpand');
    const priority = GetOption(options, 'roundingPriority', 'string', ['auto', 'morePrecision', 'lessPrecision'], 'auto');
    const trailing = GetOption(options, 'trailingZeroDisplay', 'string', ['auto', 'stripIfInteger'], 'auto');
    // The specification lowers the maximum fraction digits' default to the
    // minimum's when an increment is given; Chrome keeps the default, so
    // an increment with fraction digits left to their defaults throws
    // below, as Chrome's does (test262 is silent; INTL.md § 9.6).
    o.roundingIncrement = increment; o.roundingMode = mode; o.trailingZeroDisplay = trailing;
    const hasSd = mnsd !== undefined || mxsd !== undefined, hasFd = mnfd !== undefined || mxfd !== undefined;
    let needSd = true, needFd = true;
    if (priority === 'auto') {
        needSd = hasSd;
        if (needSd || (!hasFd && notation === 'compact')) needFd = false;
    }
    if (needSd) {
        if (hasSd) {
            mnsd = DefaultNumberOption(mnsd, 1, 21, 1);
            mxsd = DefaultNumberOption(mxsd, mnsd, 21, 21);
            o.minSig = mnsd; o.maxSig = mxsd;
        } else { o.minSig = 1; o.maxSig = 21; }
    }
    if (needFd) {
        if (hasFd) {
            mnfd = DefaultNumberOption(mnfd, 0, 100, undefined);
            mxfd = DefaultNumberOption(mxfd, 0, 100, undefined);
            if (mnfd === undefined) mnfd = mnfdDefault < mxfd ? mnfdDefault : mxfd;
            else if (mxfd === undefined) mxfd = mxfdDefault > mnfd ? mxfdDefault : mnfd;
            else if (mnfd > mxfd) throw new RangeError_('maximumFractionDigits value is out of range');
            o.minFrac = mnfd; o.maxFrac = mxfd;
        } else { o.minFrac = mnfdDefault; o.maxFrac = mxfdDefault; }
    }
    if (!needSd && !needFd) {
        o.minFrac = 0; o.maxFrac = 0; o.minSig = 1; o.maxSig = 2;
        o.roundingType = 'morePrecision'; o.roundingPriority = 'morePrecision';
    } else if (priority === 'auto') {
        o.roundingType = hasSd ? 'significantDigits' : 'fractionDigits'; o.roundingPriority = 'auto';
    } else { o.roundingType = priority; o.roundingPriority = priority; }
    if (increment !== 1) {
        if (o.roundingType !== 'fractionDigits') throw new TypeError_('roundingIncrement requires fraction digits');
        if (o.maxFrac !== o.minFrac) throw new RangeError_('roundingIncrement requires equal minimum and maximum fraction digits');
    }
}
// The rounding priority V8 reports: it reads the options back from ICU's
// skeleton, which loses the priority when trailing zeros are stripped, so
// V8 reports "auto" (the rounding itself is unchanged; INTL.md § 9.6).
function reportedRoundingPriority(o) { return o.trailingZeroDisplay === 'stripIfInteger' ? 'auto' : o.roundingPriority; }
// The digit options in resolvedOptions' order, those the object has.
function putDigitOptions(out, o) {
    put(out, 'minimumIntegerDigits', o.minInt);
    if (o.minFrac !== undefined) { put(out, 'minimumFractionDigits', o.minFrac); put(out, 'maximumFractionDigits', o.maxFrac); }
    if (o.minSig !== undefined) { put(out, 'minimumSignificantDigits', o.minSig); put(out, 'maximumSignificantDigits', o.maxSig); }
}

// ── Plural rules (UTS 35 § 5.1, CLDR's en rules) ────────────────────────
// A rule's condition parsed once: [[relation...]...], or-ed lists of
// and-ed relations {operand, mod, negate, ranges: [[lo, hi]...]}.
function parsePluralRule(text) {
    const at = indexOf(text, '@', 0);
    const body = at >= 0 ? slice(text, 0, at) : text;
    const ors = [], orParts = split(body, ' or ');
    for (let i = 0; i < orParts.length; i++) {
        const ands = [], andParts = split(orParts[i], ' and ');
        for (let j = 0; j < andParts.length; j++) {
            const rel = trimSpaces(andParts[j]);
            if (rel === '') continue;
            const ne = indexOf(rel, '!=', 0), eq = indexOf(rel, '=', 0);
            const negate = ne >= 0, cut = negate ? ne : eq;
            const left = trimSpaces(slice(rel, 0, cut)), right = trimSpaces(slice(rel, cut + (negate ? 2 : 1), rel.length));
            const pct = indexOf(left, '%', 0);
            const ranges = [], items = split(right, ',');
            for (let k = 0; k < items.length; k++) {
                const item = trimSpaces(items[k]), dots = indexOf(item, '..', 0);
                push(ranges, dots >= 0 ? [+slice(item, 0, dots), +slice(item, dots + 2, item.length)] : [+item, +item]);
            }
            push(ands, {__proto__: null, operand: trimSpaces(pct >= 0 ? slice(left, 0, pct) : left),
                        mod: pct >= 0 ? +slice(left, pct + 1, left.length) : 0, negate: negate, ranges: ranges});
        }
        if (ands.length > 0) push(ors, ands);
    }
    return ors;
}
function trimSpaces(s) {
    let a = 0, b = s.length;
    while (a < b && codeAt(s, a) === 32) a++;
    while (b > a && codeAt(s, b - 1) === 32) b--;
    return slice(s, a, b);
}
// The plural operands of a formatted number's digits with the point moved
// k places right, the value a plural is chosen by when an exponent was
// taken out (UTS 35 § 5.1: "1.2K" is n = 1200), counted without writing
// the moved digits out ("1E-999999" has six digits of exponent, not a
// million zeros). ICU reads n as the value's double and i as the integer's
// last 18 digits, and takes a modulus of those doubles, so a value past a
// double's precision is read as its double is (V8 calls 329606050212053000
// "two" in ordinals: its double ends in 2992). "1.50": n 1.5, i 1, v 2,
// w 1, f 50, t 5.
function pluralOperands(text, k) {
    const dot = indexOf(text, '.', 0);
    const ints = dot >= 0 ? slice(text, 0, dot) : text;
    const digits = ints + (dot >= 0 ? slice(text, dot + 1, text.length) : '');
    const point = ints.length + k;
    let intPart, frac, lead = 0;            // the fraction is `lead` zeros, then `frac`
    if (point >= digits.length) { intPart = digits + zeros(point - digits.length); frac = ''; }
    else if (point <= 0) { intPart = '0'; lead = -point; frac = digits; }
    else { intPart = slice(digits, 0, point); frac = slice(digits, point, digits.length); }
    let end = frac.length;
    while (end > 0 && codeAt(frac, end - 1) === 48) end--;
    return {__proto__: null, n: +(digits + 'e' + String_(point - digits.length)),
            i: +slice(intPart, intPart.length > 18 ? intPart.length - 18 : 0, intPart.length),
            v: frac === '' ? 0 : lead + frac.length, w: end === 0 ? 0 : lead + end,
            f: frac === '' ? 0 : +frac, t: end === 0 ? 0 : +slice(frac, 0, end), c: 0, e: 0};
}
function operandValue(ops, name, mod) {
    const value = own(ops, name);
    return mod === 0 ? value : value % mod;
}
function pluralRuleHolds(rule, ops) {
    for (let i = 0; i < rule.length; i++) {
        let all = true;
        for (let j = 0; j < rule[i].length && all; j++) {
            const rel = rule[i][j], value = operandValue(ops, rel.operand, rel.mod);
            let inRange = false;
            for (let k = 0; k < rel.ranges.length && !inRange; k++) {
                const lo = rel.ranges[k][0], hi = rel.ranges[k][1];
                inRange = lo === hi ? value === lo : value >= lo && value <= hi && value === floor(value);
            }
            all = rel.negate ? !inRange : inRange;
        }
        if (all) return true;
    }
    return false;
}
// Each type's rules in CLDR's order, parsed at their first use.
let pluralTables = null;
function pluralTable(type) {
    if (pluralTables === null) {
        // Published only when whole: a build cut short (a stack overflow in
        // a caller's recursion) leaves nothing half made for later calls.
        const tables = {__proto__: null};
        const kinds = ['cardinal', 'ordinal'];
        for (let k = 0; k < 2; k++) {
            const rules = own(PLURALS, kinds[k]), list = [];
            for (const key in rules) {
                if (!hasOwn(rules, key)) continue;
                push(list, {__proto__: null, category: slice(key, indexOf(key, '-count-', 0) + 7, key.length),
                            rule: parsePluralRule(own(rules, key))});
            }
            tables[kinds[k]] = list;
        }
        pluralTables = tables;
    }
    return own(pluralTables, type);
}
// The category of a formatted number's digits, with the point moved
// `shift` places right (an exponent taken out; none when absent).
function pluralCategory(type, digits, shift) {
    const table = pluralTable(type), ops = pluralOperands(digits, shift === undefined ? 0 : shift);
    for (let i = 0; i < table.length; i++)
        if (table[i].category !== 'other' && pluralRuleHolds(table[i].rule, ops)) return table[i].category;
    return 'other';
}
// The categories a type's rules can give, in the order ECMA-402 lists
// them ("zero", "one", "two", "few", "many", "other"), as Chrome does.
const PLURAL_ORDER = ['zero', 'one', 'two', 'few', 'many'];
function pluralCategories(type) {
    const table = pluralTable(type), out = [];
    for (let k = 0; k < PLURAL_ORDER.length; k++)
        for (let i = 0; i < table.length; i++)
            if (table[i].category === PLURAL_ORDER[k]) { push(out, PLURAL_ORDER[k]); break; }
    push(out, 'other');
    return out;
}

// ── Intl.PluralRules (ECMA-402 § 17) ────────────────────────────────────
const PLURAL_RULES = new WeakMap();
// The locales plural rules are held for: CLDR's rules are a language's,
// and V8 resolves against ICU's plural data, so "en-US" resolves to "en"
// (INTL.md § 9.6).
const PLURAL_AVAILABLE = ['en'];
function PluralRules(locales, options) {
    if (new.target === undefined) throw new TypeError_("Constructor Intl.PluralRules requires 'new'");
    const pr = constructed(this, PluralRulesPrototype);
    loadNumberData();
    const requested = CanonicalizeLocaleList(locales);
    options = CoerceOptionsToObject(options);
    const opt = {__proto__: null};
    opt.localeMatcher = GetOption(options, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit');
    const s = {__proto__: null};
    s.type = GetOption(options, 'type', 'string', ['cardinal', 'ordinal'], 'cardinal');
    s.notation = GetOption(options, 'notation', 'string', ['standard', 'scientific', 'engineering', 'compact'], 'standard');
    const compactDisplay = GetOption(options, 'compactDisplay', 'string', ['short', 'long'], 'short');
    if (s.notation === 'compact') s.compactDisplay = compactDisplay;
    SetNumberFormatDigitOptions(s, options, 0, 3, s.notation);
    s.locale = ResolveLocale(PLURAL_AVAILABLE, requested, opt, [], () => []).locale;
    apply(wm_set, PLURAL_RULES, [pr, s]);
    return pr;
}
const PluralRulesPrototype = PluralRules.prototype;
defineProperty(PluralRules, 'prototype', {__proto__: null, writable: false, enumerable: false, configurable: false});
defineProperty(PluralRules, 'length', {__proto__: null, value: 0, configurable: true});
function thisPluralRules(o, name) {
    const s = slots(PLURAL_RULES, o);
    if (s === undefined) throw new TypeError_('Method Intl.PluralRules.prototype.' + name + ' called on incompatible receiver');
    return s;
}
// ResolvePlural: the category of n as the object's notation and digit
// options show it, chosen by the value shown (a compact "1.5K" is 1500).
function resolvePlural(s, n) {
    if (!isFinite_(n)) return 'other';
    const e = computeExponent(s, decimalOfNumber(n));
    const x = e.value, exponent = e.exponent;
    const r = formatNumericToString(s, decimal(x.neg, x.digits, x.exp - exponent));
    return pluralCategory(s.type, r.string, exponent);
}
method(PluralRules, 'supportedLocalesOf', methodOf(supportedLocalesOf), 1);
defineProperty(PluralRulesPrototype, toStringTag, {__proto__: null, value: 'Intl.PluralRules', writable: false, enumerable: false, configurable: true});
method(PluralRulesPrototype, 'select', methodOf(function (value) {
    const s = thisPluralRules(this, 'select');
    return resolvePlural(s, ToNumber(value));
}), 1);
method(PluralRulesPrototype, 'selectRange', methodOf(function (start, end) {
    const s = thisPluralRules(this, 'selectRange');
    if (start === undefined || end === undefined) throw new TypeError_('start and end are required');
    const x = ToNumber(start), y = ToNumber(end);
    if (x !== x || y !== y) throw new RangeError_('Invalid number range');
    return pluralRange(resolvePlural(s, x), resolvePlural(s, y));
}), 2);
// CLDR's plural ranges (a start and an end category's range category);
// one the data does not list is "other", as ICU resolves it.
function pluralRange(start, end) {
    const r = own(PLURALS.ranges, 'pluralRange-start-' + start + '-end-' + end);
    return r !== undefined ? r : 'other';
}
method(PluralRulesPrototype, 'resolvedOptions', methodOf(function () {
    const s = thisPluralRules(this, 'resolvedOptions');
    const o = {};
    put(o, 'locale', s.locale); put(o, 'type', s.type); put(o, 'notation', s.notation);
    if (s.notation === 'compact') put(o, 'compactDisplay', s.compactDisplay);
    // Chrome reports a plural rules' significant digits when it has any
    // and its fraction digits only when it has none, so a rounding
    // priority's pair reports the significant half (INTL.md § 9.6).
    put(o, 'minimumIntegerDigits', s.minInt);
    if (s.minSig !== undefined) { put(o, 'minimumSignificantDigits', s.minSig); put(o, 'maximumSignificantDigits', s.maxSig); }
    else { put(o, 'minimumFractionDigits', s.minFrac); put(o, 'maximumFractionDigits', s.maxFrac); }
    put(o, 'pluralCategories', pluralCategories(s.type));
    put(o, 'roundingIncrement', s.roundingIncrement); put(o, 'roundingMode', s.roundingMode);
    put(o, 'roundingPriority', reportedRoundingPriority(s)); put(o, 'trailingZeroDisplay', s.trailingZeroDisplay);
    return o;
}), 0);

// ── Intl.NumberFormat (ECMA-402 § 16) ───────────────────────────────────
const NUMBER_FORMATS = new WeakMap();
const SANCTIONED_UNITS = ['acre', 'bit', 'byte', 'celsius', 'centimeter', 'day', 'degree', 'fahrenheit', 'fluid-ounce',
    'foot', 'gallon', 'gigabit', 'gigabyte', 'gram', 'hectare', 'hour', 'inch', 'kilobit', 'kilobyte', 'kilogram',
    'kilometer', 'liter', 'megabit', 'megabyte', 'meter', 'microsecond', 'mile', 'mile-scandinavian', 'milliliter',
    'millimeter', 'millisecond', 'minute', 'month', 'nanosecond', 'ounce', 'percent', 'petabyte', 'pound', 'second',
    'stone', 'terabit', 'terabyte', 'week', 'yard', 'year'];
function isWellFormedUnit(unit) {
    if (contains(SANCTIONED_UNITS, unit)) return true;
    const at = indexOf(unit, '-per-', 0);
    if (at < 0) return false;
    const num = slice(unit, 0, at), den = slice(unit, at + 5, unit.length);
    return contains(SANCTIONED_UNITS, num) && contains(SANCTIONED_UNITS, den);
}
function isWellFormedCurrency(code) {
    if (code.length !== 3) return false;
    for (let i = 0; i < 3; i++) if (!isAlpha(codeAt(code, i))) return false;
    return true;
}
function currencyDigits(code) {
    const d = own(NUM.currencyDigits, code);
    return d !== undefined ? d : 2;
}
// GetBooleanOrStringNumberFormatOption.
function GetBooleanOrStringOption(options, property, values, fallback) {
    let value = options[property];
    if (value === undefined) return fallback;
    if (value === true) return true;
    if (!value) return false;
    value = ToString(value);
    if (value === 'true' || value === 'false') return fallback;
    if (!contains(values, value)) throw new RangeError_('Value ' + value + ' out of range for ' + property);
    return value;
}
function NumberFormat(locales, options) {
    const made = new.target === undefined ? objectCreate(NumberFormatPrototype) : constructed(this, NumberFormatPrototype);
    loadNumberData();
    const requested = CanonicalizeLocaleList(locales);
    options = CoerceOptionsToObject(options);
    const opt = {__proto__: null};
    opt.localeMatcher = GetOption(options, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit');
    const nu = GetOption(options, 'numberingSystem', 'string', undefined, undefined);
    if (nu !== undefined && !isTypeSequence(lower(nu))) throw new RangeError_('Invalid numberingSystem');
    opt.nu = nu === undefined ? undefined : lower(nu);
    const r = ResolveLocale(AVAILABLE, requested, opt, ['nu'], dtfLocaleData);
    const s = {__proto__: null};
    s.locale = r.locale; s.numberingSystem = r.nu;
    // SetNumberFormatUnitOptions.
    s.style = GetOption(options, 'style', 'string', ['decimal', 'percent', 'currency', 'unit'], 'decimal');
    const currency = GetOption(options, 'currency', 'string', undefined, undefined);
    if (currency === undefined) { if (s.style === 'currency') throw new TypeError_('Currency code is required with currency style.'); }
    else if (!isWellFormedCurrency(currency)) throw new RangeError_('Invalid currency code : ' + currency);
    const currencyDisplay = GetOption(options, 'currencyDisplay', 'string', ['code', 'symbol', 'narrowSymbol', 'name'], 'symbol');
    const currencySign = GetOption(options, 'currencySign', 'string', ['standard', 'accounting'], 'standard');
    const unit = GetOption(options, 'unit', 'string', undefined, undefined);
    if (unit === undefined) { if (s.style === 'unit') throw new TypeError_('Unit is required with unit style.'); }
    else if (!isWellFormedUnit(unit)) throw new RangeError_('Invalid unit argument for Intl.NumberFormat(): ' + unit);
    const unitDisplay = GetOption(options, 'unitDisplay', 'string', ['short', 'narrow', 'long'], 'short');
    if (s.style === 'currency') { s.currency = upper(currency); s.currencyDisplay = currencyDisplay; s.currencySign = currencySign; }
    if (s.style === 'unit') { s.unit = unit; s.unitDisplay = unitDisplay; }
    s.notation = GetOption(options, 'notation', 'string', ['standard', 'scientific', 'engineering', 'compact'], 'standard');
    // A currency's own digits are the default in standard notation only.
    let mnfdDefault = 0, mxfdDefault = s.style === 'percent' ? 0 : 3;
    if (s.style === 'currency' && s.notation === 'standard') mnfdDefault = mxfdDefault = currencyDigits(s.currency);
    SetNumberFormatDigitOptions(s, options, mnfdDefault, mxfdDefault, s.notation);
    const compactDisplay = GetOption(options, 'compactDisplay', 'string', ['short', 'long'], 'short');
    let defaultGrouping = 'auto';
    if (s.notation === 'compact') { s.compactDisplay = compactDisplay; defaultGrouping = 'min2'; }
    let grouping = GetBooleanOrStringOption(options, 'useGrouping', ['min2', 'auto', 'always', 'true', 'false'], defaultGrouping);
    if (grouping === 'true' || grouping === 'false') grouping = defaultGrouping;
    if (grouping === true) grouping = 'always';
    s.useGrouping = grouping;
    s.signDisplay = GetOption(options, 'signDisplay', 'string', ['auto', 'never', 'always', 'exceptZero', 'negative'], 'auto');
    apply(wm_set, NUMBER_FORMATS, [made, s]);
    // The legacy constructor mode, as DateTimeFormat's.
    if (new.target === undefined && isObject(this) && hasInstance(NumberFormat, this)) {
        defineProperty(this, FALLBACK_SYMBOL, {__proto__: null, value: made, writable: false, enumerable: false, configurable: false});
        return this;
    }
    return made;
}
const NumberFormatPrototype = NumberFormat.prototype;
defineProperty(NumberFormat, 'prototype', {__proto__: null, writable: false, enumerable: false, configurable: false});
defineProperty(NumberFormat, 'length', {__proto__: null, value: 0, configurable: true});
function thisNumberFormat(o, name) {
    const s = slots(NUMBER_FORMATS, o);
    if (s === undefined) throw new TypeError_('Method Intl.NumberFormat.prototype.' + name + ' called on incompatible receiver');
    return s;
}
function UnwrapNumberFormat(o, name) {
    let s = slots(NUMBER_FORMATS, o);
    if (s === undefined && isObject(o) && hasInstance(NumberFormat, o)) s = slots(NUMBER_FORMATS, o[FALLBACK_SYMBOL]);
    if (s === undefined) throw new TypeError_('Method Intl.NumberFormat.prototype.' + name + ' called on incompatible receiver');
    return s;
}

// ── Formatting a number (ICU's number formatter, over CLDR's en data) ────
// A formatted number is its digits inside three layers, as ICU builds it
// and as a range collapses it: `inner` the scientific exponent, `middle`
// the pattern's affixes (sign, currency, percent, compact), `outer` a
// unit's or a currency name's pattern; each {prefix: [parts], suffix:
// [parts]}. Two numbers whose `middleKey`s match have equivalent middles.

// The compact patterns a format uses, {"1000": {one, other}, ...}: a
// currency symbol's or code's are the short currency ones whatever
// compactDisplay says, as V8 prints them; a currency name's are the
// decimal ones.
let compactTables = null;
function compactTable(s) {
    if (compactTables === null) {
        const tables = {__proto__: null};          // published only when whole, as pluralTables
        const sources = {__proto__: null, short: own(own(NUM.decimal, 'short'), 'decimalFormat'),
                         long: own(own(NUM.decimal, 'long'), 'decimalFormat'), currency: own(own(NUM.currency, 'short'), 'standard')};
        for (const name in sources) {
            const src = own(sources, name), table = {__proto__: null};
            for (const key in src) {
                const at = indexOf(key, '-count-', 0);
                if (indexOf(key, '-alt-', 0) >= 0 || at < 0) continue;
                const power = slice(key, 0, at), category = slice(key, at + 7, key.length);
                if (own(table, power) === undefined) table[power] = {__proto__: null};
                table[power][category] = own(src, key);
            }
            tables[name] = table;
        }
        compactTables = tables;
    }
    return own(compactTables, s.style === 'currency' && s.currencyDisplay !== 'name' ? 'currency' : s.compactDisplay);
}
// The compact table's key for the largest magnitude at or below this one,
// or null below the table.
function compactKeyFor(s, magnitude) {
    const table = compactTable(s);
    let best = null;
    for (const key in table) if (key.length - 1 <= magnitude && (best === null || key.length > best.length)) best = key;
    return best;
}
// ComputeExponentForMagnitude: the power of ten the digits are divided by.
function exponentForMagnitude(s, magnitude) {
    if (s.notation === 'scientific') return magnitude;
    if (s.notation === 'engineering') return floor(magnitude / 3) * 3;
    if (s.notation !== 'compact') return 0;
    const key = compactKeyFor(s, magnitude);
    if (key === null) return 0;
    const pattern = own(own(compactTable(s), key), 'other');
    let zeros = 0;
    for (let i = 0; i < pattern.length; i++) if (codeAt(pattern, i) === 48) zeros++;
    return key.length - 1 - (zeros - 1);
}
// ComputeExponent, against the digits as rounded: {exponent, magnitude,
// value}, the magnitude the exponent was chosen for (which names the
// compact pattern) and the value to format at it. Where the rounding
// carried the value to the next exponent, that value is the rounded one,
// as V8 formats it: 999.9 compact by increments of 2 is 1000, then "2K"
// (ECMA-402 rounds the original again at the new scale: "0K"). The trial
// rounding keeps x's sign, as V8 rounds, so a
// directional mode that carries a negative value up to a power of ten
// takes the next exponent ("-1E1", never "-10E0"; ECMA-402 rounds the
// magnitude; INTL.md § 9.6).
function computeExponent(s, x) {
    if (isZeroDecimal(x)) return {__proto__: null, exponent: 0, magnitude: 0, value: x};
    const magnitude = magnitudeOf(x);
    const exponent = exponentForMagnitude(s, magnitude);
    const r = formatNumericToString(s, decimal(x.neg, x.digits, x.exp - exponent)).rounded;
    if (isZeroDecimal(r) || magnitudeOf(r) === magnitude - exponent)
        return {__proto__: null, exponent: exponent, magnitude: magnitude, value: x};
    return {__proto__: null, exponent: exponentForMagnitude(s, magnitude + 1), magnitude: magnitude + 1,
            value: decimal(x.neg, r.digits, r.exp + exponent)};
}
// The integer digits grouped as #,##0 groups them (threes), when the
// strategy calls for it at that length; "auto" in a compact format is
// "min2", as ICU groups it.
function integerParts(digits, s, out) {
    const min2 = s.useGrouping === 'min2' || (s.useGrouping === 'auto' && s.notation === 'compact');
    const minimum = min2 ? 2 : NUM.minimumGroupingDigits;
    if (s.useGrouping === false || digits.length < 3 + minimum) { push(out, part('integer', digits)); return; }
    let first = digits.length % 3;
    if (first === 0) first = 3;
    push(out, part('integer', slice(digits, 0, first)));
    for (let at = first; at < digits.length; at += 3) {
        push(out, part('group', NUM.symbols.group));
        push(out, part('integer', slice(digits, at, at + 3)));
    }
}
const part = (type, value) => ({__proto__: null, type: type, value: value});
// The sign a value shows, '-', '+' or '': `neg` its sign, `zero` whether it
// rounded to zero (NaN counts as an unsigned zero).
function signOf(s, neg, zero) {
    switch (s.signDisplay) {
    case 'never': return '';
    case 'always': return neg ? '-' : '+';
    case 'exceptZero': return zero ? '' : neg ? '-' : '+';
    case 'negative': return neg && !zero ? '-' : '';
    }
    return neg ? '-' : '';
}
// The currency's text for its display: symbol, narrow symbol or code (a
// name is the outer layer's).
function currencyText(s) {
    const entry = own(NUM.currencies, s.currency);
    if (s.currencyDisplay === 'code' || entry === undefined) return s.currency;
    if (s.currencyDisplay === 'narrowSymbol') {
        const narrow = own(entry, 'symbol-alt-narrow');
        if (narrow !== undefined) return narrow;
    }
    const symbol = own(entry, 'symbol');
    return symbol !== undefined ? symbol : s.currency;
}
// Whether currency text beside the number ends in a letter or digit, not a
// symbol or space, so the pattern puts a no-break space between (CLDR's
// alphaNextToNumber: "USD 1.00", "$1.00").
const alphaNextToNumber = text => exec(/[\p{S}\p{Z}]$/u, text) === null;
const NO_BREAK_SPACE_DROPPED = {__proto__: null, '\u00a0': ''};
// Pattern text as parts: ¤ the currency, % a part of `percentType`, the
// rest literal (quotes dropped); `kind` 'compact' makes letters compact
// parts.
function affixParts(text, currency, kind, percentType, out) {
    for (let i = 0; i < text.length; i++) {
        const c = fromCharCode(codeAt(text, i));
        let type = 'literal', value = c;
        if (c === '¤') { type = 'currency'; value = currency; }
        else if (c === '%' && kind !== 'compact') { type = percentType; value = NUM.symbols.percentSign; }
        else if (c === "'") continue;
        else if (kind === 'compact' && c !== ' ' && c !== ' ') type = 'compact';
        const last = out.length > 0 ? out[out.length - 1] : null;
        if (last !== null && last.type === type && type !== 'currency') last.value += value;
        else push(out, part(type, value));
    }
}
// A number pattern ("¤#,##0.00", "#,##0%", "(¤#,##0.00)") around its body.
function patternAffixes(pattern) {
    let a = 0;
    while (a < pattern.length && indexOf('#0,.', fromCharCode(codeAt(pattern, a)), 0) < 0) a++;
    let b = pattern.length;
    while (b > a && indexOf('#0,.', fromCharCode(codeAt(pattern, b - 1)), 0) < 0) b--;
    return {__proto__: null, prefix: slice(pattern, 0, a), suffix: slice(pattern, b, pattern.length)};
}
// A unit's pattern for a plural category. A unit per a unit CLDR has no
// pattern for puts the numerator's pattern into the denominator's per
// pattern ("{0}/h"), or where it has none into the compound "per" pattern
// with the denominator's name.
function unitPattern(s, category) {
    const table = own(UNITS, s.unitDisplay);
    const whole = own(table, s.unit);
    if (whole !== undefined) return unitCount(whole, category);
    const at = indexOf(s.unit, '-per-', 0);
    const num = unitCount(own(table, slice(s.unit, 0, at)), category), den = own(table, slice(s.unit, at + 5, s.unit.length));
    const per = own(den, 'perUnitPattern');
    if (per !== undefined) return simpleFormat(per, [num]);
    const name = trimSpaces(simpleFormat(unitCount(den, 'one'), ['']));
    return simpleFormat(own(table, 'per'), [num, name]);
}
function unitCount(entry, category) {
    const p = own(entry, 'unitPattern-count-' + category);
    return p !== undefined ? p : own(entry, 'unitPattern-count-other');
}
// An outer pattern ("{0} km", "{0} US dollars") as {prefix, suffix}: the
// spaces beside the number literal, the rest one part of `type`.
function outerParts(pattern, type) {
    const at = indexOf(pattern, '{0}', 0);
    const out = {__proto__: null, prefix: [], suffix: []};
    const before = slice(pattern, 0, at), after = slice(pattern, at + 3, pattern.length);
    let b = before.length;
    while (b > 0 && (codeAt(before, b - 1) === 32 || codeAt(before, b - 1) === 0xA0)) b--;
    if (b > 0) push(out.prefix, part(type, slice(before, 0, b)));
    if (b < before.length) push(out.prefix, part('literal', slice(before, b, before.length)));
    let a = 0;
    while (a < after.length && (codeAt(after, a) === 32 || codeAt(after, a) === 0xA0)) a++;
    if (a > 0) push(out.suffix, part('literal', slice(after, 0, a)));
    if (a < after.length) push(out.suffix, part(type, slice(after, a, after.length)));
    return out;
}
// One number in its layers. `forced`, when given, is the plural category
// the outer layer takes (a collapsed range's); `single` is true when the
// number is formatted alone rather than as a range's end.
function formatLayers(s, x, forced, single) {
    const res = {__proto__: null, digits: [], inner: {__proto__: null, prefix: [], suffix: []},
                 middle: {__proto__: null, prefix: [], suffix: []}, outer: {__proto__: null, prefix: [], suffix: []},
                 category: 'other', middleKey: '', approximate: true, value: null};
    let v = x, exponent = 0, compactKey = null;
    if (!v.nan && !v.inf && s.style === 'percent') v = decimal(v.neg, v.digits, v.exp + 2);
    let zero = true;
    if (v.nan) push(res.digits, part('nan', NUM.symbols.nan));
    else if (v.inf) {
        push(res.digits, part('infinity', NUM.symbols.infinity)); zero = false;
        res.value = {__proto__: null, neg: v.neg, digits: 'Infinity', exp: 0};    // one infinity is another, in a range
    }
    else {
        const e = computeExponent(s, v);
        exponent = e.exponent;
        if (s.notation === 'compact' && exponent !== 0) compactKey = compactKeyFor(s, e.magnitude);
        v = e.value;
        const r = formatNumericToString(s, decimal(v.neg, v.digits, v.exp - exponent));
        zero = isZeroDecimal(r.rounded);
        res.value = decimal(v.neg, r.rounded.digits, r.rounded.exp + exponent);
        const dot = indexOf(r.string, '.', 0);
        integerParts(dot >= 0 ? slice(r.string, 0, dot) : r.string, s, res.digits);
        if (dot >= 0) {
            push(res.digits, part('decimal', NUM.symbols.decimal));
            push(res.digits, part('fraction', slice(r.string, dot + 1, r.string.length)));
        }
        if (s.notation === 'scientific' || s.notation === 'engineering') {
            push(res.inner.suffix, part('exponentSeparator', NUM.symbols.exponential));
            if (exponent < 0) push(res.inner.suffix, part('exponentMinusSign', NUM.symbols.minusSign));
            push(res.inner.suffix, part('exponentInteger', String_(abs(exponent))));
        }
        res.category = pluralCategory('cardinal', r.string, exponent);
    }
    const category = forced !== undefined ? forced : res.category;
    // The middle layer: the sign, then the pattern's affixes. A currency
    // whose text ends in a letter takes the alphaNextToNumber pattern only
    // beside digits (ICU's currency spacing matches a digit). A percent is
    // the percent pattern's, a part typed `unit` in a unit format, except
    // that a compact one is written after the compact pattern, as the
    // outer layer, as V8 writes both (the percent unit's long name is an
    // ordinary unit pattern). A percent per a unit formatted alone, short
    // or narrow and not compact, is the percent pattern too, its
    // denominator dropped ("2%" for percent-per-second), while its ranges
    // and its compact forms keep it ("3–5%/s", "1.2K%/s"): Chrome writes
    // all of them so (INTL.md § 9.6).
    const sign = signOf(s, v.neg && !v.nan, zero);
    const named = s.style === 'currency' && s.currencyDisplay === 'name';
    const currency = s.style === 'currency' && !named ? currencyText(s) : '';
    const alpha = currency !== '' && !v.nan && !v.inf && alphaNextToNumber(currency);
    const accounting = s.style === 'currency' && !named && s.currencySign === 'accounting' && sign === '-' && compactKey === null;
    const percent = s.style === 'percent' || (s.style === 'unit' && s.unitDisplay !== 'long' && (s.unit === 'percent' ||
        (single === true && s.notation !== 'compact' && slice(s.unit, 0, 12) === 'percent-per-')));
    const percentType = s.style === 'percent' ? 'percentSign' : 'unit';
    let pattern;
    if (compactKey !== null) {
        const forms = own(compactTable(s), compactKey);
        pattern = own(forms, res.category);
        if (pattern === undefined) pattern = own(forms, 'other');
        if (alpha) {
            const alt = own(own(own(NUM.currency, 'short'), 'standard'), compactKey + '-count-' + res.category + '-alt-alphaNextToNumber');
            if (alt !== undefined) pattern = alt;
        }
    } else if (percent && s.notation !== 'compact') pattern = own(NUM.percent, 'standard');
    else if (s.style === 'currency' && !named) {
        pattern = own(NUM.currency, (accounting ? 'accounting' : 'standard') + (alpha ? '-alphaNextToNumber' : ''));
        if (accounting) pattern = slice(pattern, indexOf(pattern, ';', 0) + 1, pattern.length);
    } else pattern = own(NUM.decimal, 'standard');
    if (sign !== '' && !accounting) push(res.middle.prefix, part(sign === '-' ? 'minusSign' : 'plusSign', sign === '-' ? NUM.symbols.minusSign : NUM.symbols.plusSign));
    if (compactKey !== null) {
        const zeroAt = indexOf(pattern, '0', 0);
        let zeroEnd = zeroAt;
        while (zeroEnd < pattern.length && codeAt(pattern, zeroEnd) === 48) zeroEnd++;
        affixParts(slice(pattern, 0, zeroAt), currency, 'compact', percentType, res.middle.prefix);
        affixParts(slice(pattern, zeroEnd, pattern.length), currency, 'compact', percentType, res.middle.suffix);
    } else {
        const affixes = patternAffixes(pattern);
        affixParts(affixes.prefix, currency, 'pattern', percentType, res.middle.prefix);
        affixParts(affixes.suffix, currency, 'pattern', percentType, res.middle.suffix);
    }
    // The key is the sign and the affixes' text, as ICU compares two
    // modifiers (compact "0T" and "000T" are one), less the currency's
    // spacing, which ICU inserts into an otherwise identical modifier.
    const affixText = list => alpha ? swapChars(partsToString(list), NO_BREAK_SPACE_DROPPED) : partsToString(list);
    res.middleKey = sign + '|' + affixText(res.middle.prefix) + '|' + affixText(res.middle.suffix);
    // Where a range's approximately sign goes: before the sign, which an
    // accounting negative writes as parentheses, and so has none.
    res.approximate = !accounting;
    // The outer layer: a unit's or a currency name's pattern.
    if (percent && s.notation === 'compact') push(res.outer.suffix, part('unit', NUM.symbols.percentSign));
    else if (s.style === 'unit' && !percent) res.outer = outerParts(unitPattern(s, category), 'unit');
    else if (named) {
        const entry = own(NUM.currencies, s.currency);
        let name = own(entry, 'displayName-count-' + category);
        if (name === undefined) name = own(entry, 'displayName-count-other');
        if (name === undefined) name = s.currency;
        let text = own(NUM.currency, 'unitPattern-count-' + category);
        if (text === undefined) text = own(NUM.currency, 'unitPattern-count-other');
        res.outer = outerParts(simpleFormat(text, ['{0}', name]), 'currency');
    }
    return res;
}
// Parts as ICU reports fields: adjacent literals are one literal.
function addParts(out, list) {
    for (let i = 0; i < list.length; i++) {
        const last = out.length > 0 ? out[out.length - 1] : null;
        if (list[i].type === 'literal' && last !== null && last.type === 'literal') last.value += list[i].value;
        else push(out, {__proto__: null, type: list[i].type, value: list[i].value, source: 'shared'});
    }
}
function addLayers(out, res, inner, middle, outer) {
    if (outer) addParts(out, res.outer.prefix);
    if (middle) addParts(out, res.middle.prefix);
    if (inner) addParts(out, res.inner.prefix);
    addParts(out, res.digits);
    if (inner) addParts(out, res.inner.suffix);
    if (middle) addParts(out, res.middle.suffix);
    if (outer) addParts(out, res.outer.suffix);
}
function numberParts(s, x) {
    const out = [];
    addLayers(out, formatLayers(s, x, undefined, true), true, true, true);
    return out;
}
const codePoints = list => { let n = 0; for (let i = 0; i < list.length; i++) n += countCodePoints(list[i].value); return n; };
function countCodePoints(text) {
    let n = 0;
    for (let i = 0; i < text.length; i++) { const c = codeAt(text, i); if (c < 0xDC00 || c > 0xDFFF) n++; }
    return n;
}
const layerCodePoints = layer => codePoints(layer.prefix) + codePoints(layer.suffix);
const textLength = parts => { let n = 0; for (let i = 0; i < parts.length; i++) n += parts[i].value.length; return n; };
// Whether a range's two values are one, as V8 decides it: the same middle
// (so the same sign shown), and either equal before rounding as the same
// kind of input (two Numbers, or two strings or BigInts; a zero equals a
// zero whatever its sign) or equal as rounded, sign and all. So -0 and 0
// are one, and so are "-0" and "0", but -0 and "0" are not, nor -0 and a
// 0.0001 that rounded to zero (INTL.md § 9.6).
function sameRangeValue(first, second, x, y) {
    if (first.value === null || second.value === null || first.middleKey !== second.middleKey) return false;
    if ((x.number === true) === (y.number === true) && !x.nan && !y.nan) {
        if (x.inf || y.inf) { if (x.inf && y.inf && x.neg === y.neg) return true; }
        else if (x.digits === y.digits && x.exp === y.exp && (x.neg === y.neg || isZeroDecimal(x))) return true;
    }
    const a = first.value, b = second.value;
    return a.neg === b.neg && a.digits === b.digits && a.exp === b.exp;
}
// A range (ICU's NumberRangeFormatter, collapse "auto"): two values the
// same as rounded, with the same middle, are one value approximately;
// otherwise a layer both sides share is written once around the range —
// the outer whenever there is one (within one format it is one unit or
// currency, its words chosen by the range's plural category), the middle
// when its affixes and sign match and it is more than
// one code point, the inner never — and a range that repeats any layer
// gets spaces round its dash.
//
// A part's source is ICU's span it lies wholly inside, else "shared"
// (V8 reads each part against the spans), so a literal that runs from one
// side into the dash is shared. ICU measures the spans before it inserts a
// currency's spacing, so where a collapsed middle ends in the
// alphaNextToNumber no-break space both spans sit one character early and
// the parts at their ends read as shared or as the other side's; V8
// reports them so, and so does this (INTL.md § 9.6).
function numberRangeParts(s, x, y) {
    const first = formatLayers(s, x), second = formatLayers(s, y), out = [];
    if (sameRangeValue(first, second, x, y)) {
        if (first.approximate) {
            addParts(out, first.outer.prefix);
            push(out, {__proto__: null, type: 'approximatelySign', value: NUM.symbols.approximatelySign, source: 'shared'});
            addParts(out, first.middle.prefix); addParts(out, first.inner.prefix); addParts(out, first.digits);
            addParts(out, first.inner.suffix); addParts(out, first.middle.suffix); addParts(out, first.outer.suffix);
        } else addLayers(out, first, true, true, true);
        return out;
    }
    const collapseMiddle = first.middleKey === second.middleKey && layerCodePoints(first.middle) > 1;
    let outer = first.outer;
    if (layerCodePoints(first.outer) > 0) outer = formatLayers(s, y, pluralRange(first.category, second.category)).outer;
    const spaced = (!collapseMiddle && layerCodePoints(first.middle) > 0) || layerCodePoints(first.inner) > 0;
    addParts(out, outer.prefix);
    if (collapseMiddle) addParts(out, first.middle.prefix);
    const start0 = textLength(out);
    addLayers(out, first, true, !collapseMiddle, false);
    const end0 = textLength(out);
    const range = own(NUM.misc, 'range'), at0 = indexOf(range, '{0}', 0), at1 = indexOf(range, '{1}', 0);
    let dash = slice(range, at0 + 3, at1);
    if (spaced) {
        if (codeAt(dash, 0) !== 32) dash = ' ' + dash;
        if (codeAt(dash, dash.length - 1) !== 32) dash += ' ';
    }
    addParts(out, [part('literal', dash)]);
    const start1 = textLength(out);
    addLayers(out, second, true, !collapseMiddle, false);
    const end1 = textLength(out);
    if (collapseMiddle) addParts(out, second.middle.suffix);
    addParts(out, outer.suffix);
    let early = 0;
    if (collapseMiddle && first.middle.prefix.length > 0) {
        const last = first.middle.prefix[first.middle.prefix.length - 1].value;
        if (codeAt(last, last.length - 1) === 0xA0) early = 1;
    }
    for (let i = 0, at = 0; i < out.length; i++) {
        const from = at, to = at + out[i].value.length;
        at = to;
        if (from >= start0 - early && to <= end0 - early) out[i].source = 'startRange';
        else if (from >= start1 - early && to <= end1 - early) out[i].source = 'endRange';
    }
    return out;
}
// Parts as the script sees them: {type, value}, and `source` for a range's.
function numberPartsToArray(parts, withSource) {
    const out = [];
    for (let i = 0; i < parts.length; i++) {
        const o = {};
        put(o, 'type', parts[i].type); put(o, 'value', parts[i].value);
        if (withSource) put(o, 'source', parts[i].source);
        push(out, o);
    }
    return out;
}

// ── Intl.NumberFormat.prototype ─────────────────────────────────────────
method(NumberFormat, 'supportedLocalesOf', methodOf(supportedLocalesOf), 1);
defineProperty(NumberFormatPrototype, toStringTag, {__proto__: null, value: 'Intl.NumberFormat', writable: false, enumerable: false, configurable: true});
getter(NumberFormatPrototype, 'format', function () {
    const s = UnwrapNumberFormat(this, 'format');
    if (s.bound === undefined) {
        const bound = (value) => partsToString(numberParts(s, ToIntlMathematicalValue(value)));
        defineProperty(bound, 'name', {__proto__: null, value: '', configurable: true});
        s.bound = bound;
    }
    return s.bound;
});
method(NumberFormatPrototype, 'formatToParts', methodOf(function (value) {
    const s = thisNumberFormat(this, 'formatToParts');
    return numberPartsToArray(numberParts(s, ToIntlMathematicalValue(value)), false);
}), 1);
function rangeValues(start, end) {
    if (start === undefined || end === undefined) throw new TypeError_('start and end are required');
    const x = ToIntlMathematicalValue(start), y = ToIntlMathematicalValue(end);
    if (x.nan || y.nan) throw new RangeError_('Invalid number range');
    return [x, y];
}
method(NumberFormatPrototype, 'formatRange', methodOf(function (start, end) {
    const s = thisNumberFormat(this, 'formatRange');
    const xy = rangeValues(start, end);
    return partsToString(numberRangeParts(s, xy[0], xy[1]));
}), 2);
method(NumberFormatPrototype, 'formatRangeToParts', methodOf(function (start, end) {
    const s = thisNumberFormat(this, 'formatRangeToParts');
    const xy = rangeValues(start, end);
    return numberPartsToArray(numberRangeParts(s, xy[0], xy[1]), true);
}), 2);
method(NumberFormatPrototype, 'resolvedOptions', methodOf(function () {
    const s = UnwrapNumberFormat(this, 'resolvedOptions');
    const o = {};
    put(o, 'locale', s.locale); put(o, 'numberingSystem', s.numberingSystem); put(o, 'style', s.style);
    // V8 reads currencySign back from ICU's skeleton, which spells an
    // accounting sign that is never shown as plain "never": V8 reports
    // such a format "standard", and so does this (INTL.md § 9.6).
    if (s.style === 'currency') {
        put(o, 'currency', s.currency); put(o, 'currencyDisplay', s.currencyDisplay);
        put(o, 'currencySign', s.signDisplay === 'never' ? 'standard' : s.currencySign);
    }
    if (s.style === 'unit') { put(o, 'unit', s.unit); put(o, 'unitDisplay', s.unitDisplay); }
    putDigitOptions(o, s);
    put(o, 'useGrouping', s.useGrouping); put(o, 'notation', s.notation);
    if (s.notation === 'compact') put(o, 'compactDisplay', s.compactDisplay);
    put(o, 'signDisplay', s.signDisplay);
    put(o, 'roundingIncrement', s.roundingIncrement); put(o, 'roundingMode', s.roundingMode);
    put(o, 'roundingPriority', reportedRoundingPriority(s)); put(o, 'trailingZeroDisplay', s.trailingZeroDisplay);
    return o;
}), 0);


// ── Intl.RelativeTimeFormat (ECMA-402 § 18) ─────────────────────────────
const RELATIVE_TIME_FORMATS = new WeakMap();
const RELATIVE_UNITS = ['second', 'minute', 'hour', 'day', 'week', 'month', 'quarter', 'year'];
function RelativeTimeFormat(locales, options) {
    if (new.target === undefined) throw new TypeError_("Constructor Intl.RelativeTimeFormat requires 'new'");
    const made = constructed(this, RelativeTimeFormatPrototype);
    loadNumberData();
    const requested = CanonicalizeLocaleList(locales);
    options = CoerceOptionsToObject(options);
    const opt = {__proto__: null};
    opt.localeMatcher = GetOption(options, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit');
    const nu = GetOption(options, 'numberingSystem', 'string', undefined, undefined);
    if (nu !== undefined && !isTypeSequence(lower(nu))) throw new RangeError_('Invalid numberingSystem');
    opt.nu = nu === undefined ? undefined : lower(nu);
    const r = ResolveLocale(AVAILABLE, requested, opt, ['nu'], dtfLocaleData);
    const s = {__proto__: null};
    s.locale = r.locale; s.numberingSystem = r.nu;
    s.style = GetOption(options, 'style', 'string', ['long', 'short', 'narrow'], 'long');
    s.numeric = GetOption(options, 'numeric', 'string', ['always', 'auto'], 'always');
    // The NumberFormat and PluralRules a relative time is formatted with:
    // the locale's defaults, as `new Intl.NumberFormat(locale)` has them,
    // except that the value rounds half to even, as V8's (ICU's default
    // formatter's) does: "in 1 day" for 1.0005 (INTL.md § 9.6).
    const nf = {__proto__: null, style: 'decimal', notation: 'standard', signDisplay: 'auto', useGrouping: 'auto'};
    SetNumberFormatDigitOptions(nf, objectCreate(null), 0, 3, 'standard');
    nf.roundingMode = 'halfEven';
    s.nf = nf;
    apply(wm_set, RELATIVE_TIME_FORMATS, [made, s]);
    return made;
}
const RelativeTimeFormatPrototype = RelativeTimeFormat.prototype;
defineProperty(RelativeTimeFormat, 'prototype', {__proto__: null, writable: false, enumerable: false, configurable: false});
defineProperty(RelativeTimeFormat, 'length', {__proto__: null, value: 0, configurable: true});
function thisRelativeTimeFormat(o, name) {
    const s = slots(RELATIVE_TIME_FORMATS, o);
    if (s === undefined) throw new TypeError_('Method Intl.RelativeTimeFormat.prototype.' + name + ' called on incompatible receiver');
    return s;
}
// PartitionRelativeTimePattern: the parts of `value` `unit`s from now. A
// number's parts carry the unit they count, singular.
function relativeTimeParts(s, value, unit, name) {
    if (!isFinite_(value)) throw new RangeError_('Value need to be finite number for Intl.RelativeTimeFormat.prototype.' + name + '()');
    let singular = unit;
    if (unit !== '' && codeAt(unit, unit.length - 1) === 115) singular = slice(unit, 0, unit.length - 1);     // "days"
    if (!contains(RELATIVE_UNITS, singular))
        throw new RangeError_('Invalid unit argument for Intl.RelativeTimeFormat.prototype.' + name + "() '" + unit + "'");
    const fields = own(RELATIVE, s.style === 'long' ? singular : singular + '-' + s.style);
    const out = [];
    // numeric "auto": a value within 2.1 of now is named in words where
    // CLDR has them ("tomorrow", "this hour"), judged as ICU judges it, by the value
    // rounded to hundredths in doubles (so V8 calls 0.004 days "today" and
    // 0.005 "in 0.005 days"; ECMA-402 would match the exact value; § 9.6).
    if (s.numeric === 'auto' && value > -2.1 && value < 2.1) {
        const hundredths = value * 100;
        const offset = trunc(hundredths < 0 ? hundredths - 0.5 : hundredths + 0.5);
        const word = offset % 100 === 0 ? own(fields, 'relative-type-' + String_(offset / 100)) : undefined;
        if (word !== undefined) { push(out, {__proto__: null, type: 'literal', value: word}); return out; }
    }
    const past = value < 0 || (value === 0 && 1 / value < 0);
    const x = decimalOfNumber(past ? -value : value);
    const category = pluralCategory('cardinal', formatNumericToString(s.nf, x).string);
    const patterns = own(fields, past ? 'relativeTime-type-past' : 'relativeTime-type-future');
    let pattern = own(patterns, 'relativeTimePattern-count-' + category);
    if (pattern === undefined) pattern = own(patterns, 'relativeTimePattern-count-other');
    const at = indexOf(pattern, '{0}', 0);
    if (at > 0) push(out, {__proto__: null, type: 'literal', value: slice(pattern, 0, at)});
    const number = numberParts(s.nf, x);
    for (let i = 0; i < number.length; i++) push(out, {__proto__: null, type: number[i].type, value: number[i].value, unit: singular});
    if (at + 3 < pattern.length) push(out, {__proto__: null, type: 'literal', value: slice(pattern, at + 3, pattern.length)});
    return out;
}
method(RelativeTimeFormat, 'supportedLocalesOf', methodOf(supportedLocalesOf), 1);
defineProperty(RelativeTimeFormatPrototype, toStringTag, {__proto__: null, value: 'Intl.RelativeTimeFormat', writable: false, enumerable: false, configurable: true});
method(RelativeTimeFormatPrototype, 'format', methodOf(function (value, unit) {
    const s = thisRelativeTimeFormat(this, 'format');
    const v = ToNumber(value), u = ToString(unit);
    return partsToString(relativeTimeParts(s, v, u, 'format'));
}), 2);
method(RelativeTimeFormatPrototype, 'formatToParts', methodOf(function (value, unit) {
    const s = thisRelativeTimeFormat(this, 'formatToParts');
    const v = ToNumber(value), u = ToString(unit);
    const parts = relativeTimeParts(s, v, u, 'formatToParts'), out = [];
    for (let i = 0; i < parts.length; i++) {
        const o = {};
        put(o, 'type', parts[i].type); put(o, 'value', parts[i].value);
        if (parts[i].unit !== undefined) put(o, 'unit', parts[i].unit);
        push(out, o);
    }
    return out;
}), 2);
method(RelativeTimeFormatPrototype, 'resolvedOptions', methodOf(function () {
    const s = thisRelativeTimeFormat(this, 'resolvedOptions');
    const o = {};
    put(o, 'locale', s.locale); put(o, 'style', s.style); put(o, 'numeric', s.numeric);
    put(o, 'numberingSystem', s.numberingSystem);
    return o;
}), 0);

// ── Intl.ListFormat (ECMA-402 § 13) ─────────────────────────────────────
const LIST_FORMATS = new WeakMap();
// GetOptionsObject: an object as it is, undefined as no options, anything
// else refused, as ECMA-402's newer constructors take their options
// (CoerceOptionsToObject, which the older ones use, wraps a primitive).
function GetOptionsObject(options) {
    if (options === undefined) return objectCreate(null);
    if (isObject(options)) return options;
    throw new TypeError_('Options must be an object');
}
function ListFormat(locales, options) {
    if (new.target === undefined) throw new TypeError_("Constructor Intl.ListFormat requires 'new'");
    const made = constructed(this, ListFormatPrototype);
    loadData();
    const requested = CanonicalizeLocaleList(locales);
    options = GetOptionsObject(options);
    const opt = {__proto__: null};
    opt.localeMatcher = GetOption(options, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit');
    const s = {__proto__: null};
    s.locale = ResolveLocale(AVAILABLE, requested, opt, [], () => []).locale;
    s.type = GetOption(options, 'type', 'string', ['conjunction', 'disjunction', 'unit'], 'conjunction');
    s.style = GetOption(options, 'style', 'string', ['long', 'short', 'narrow'], 'long');
    apply(wm_set, LIST_FORMATS, [made, s]);
    return made;
}
const ListFormatPrototype = ListFormat.prototype;
defineProperty(ListFormat, 'prototype', {__proto__: null, writable: false, enumerable: false, configurable: false});
defineProperty(ListFormat, 'length', {__proto__: null, value: 0, configurable: true});
function thisListFormat(o, name) {
    const s = slots(LIST_FORMATS, o);
    if (s === undefined) throw new TypeError_('Method Intl.ListFormat.prototype.' + name + ' called on incompatible receiver');
    return s;
}
// StringListFromIterable: the iterable's strings, by the iteration ECMA-402
// asks for (its own iterator, closed when an element is not a string).
function StringListFromIterable(iterable) {
    const list = [];
    if (iterable === undefined) return list;
    for (const element of iterable) {
        if (typeof element !== 'string') throw new TypeError_('Intl.ListFormat: an element of the list is not a string');
        push(list, element);
    }
    return list;
}
// CreatePartsFromList: CLDR's patterns ({0} then {1}, as every en pattern
// is) applied from the end of the list: the last two by "end", each
// earlier one by "middle", the first by "start", and two alone by "2".
// Written front to back in one pass, which is linear in the list: each
// pattern's text before {0}, the element, the text between, and once the
// last element is out, the text after {1} of every pattern, innermost
// first. As ICU reports fields, which V8 reads, an empty element is no
// part and the text around it is one literal (INTL.md § 9.6).
function listParts(s, list) {
    const out = [];
    const n = list.length;
    if (n === 0) return out;
    const kind = s.type === 'conjunction' ? 'standard' : s.type === 'disjunction' ? 'or' : 'unit';
    const table = own(LISTS, 'listPattern-type-' + kind + (s.style === 'long' ? '' : '-' + s.style));
    const add = (type, value) => {
        if (value === '') return;
        const last = out.length > 0 ? out[out.length - 1] : null;
        if (type === 'literal' && last !== null && last.type === 'literal') last.value += value;
        else push(out, {__proto__: null, type: type, value: value});
    };
    const after = [];          // each pattern's text after {1}, outermost first
    for (let i = 0; i < n - 1; i++) {
        const pattern = own(table, n === 2 ? '2' : i === 0 ? 'start' : i === n - 2 ? 'end' : 'middle');
        const at0 = indexOf(pattern, '{0}', 0), at1 = indexOf(pattern, '{1}', 0);
        add('literal', slice(pattern, 0, at0));
        add('element', list[i]);
        add('literal', slice(pattern, at0 + 3, at1));
        push(after, slice(pattern, at1 + 3, pattern.length));
    }
    add('element', list[n - 1]);
    for (let i = after.length - 1; i >= 0; i--) add('literal', after[i]);
    return out;
}
method(ListFormat, 'supportedLocalesOf', methodOf(supportedLocalesOf), 1);
defineProperty(ListFormatPrototype, toStringTag, {__proto__: null, value: 'Intl.ListFormat', writable: false, enumerable: false, configurable: true});
method(ListFormatPrototype, 'format', methodOf(function (list) {
    const s = thisListFormat(this, 'format');
    return partsToString(listParts(s, StringListFromIterable(list)));
}), 1);
method(ListFormatPrototype, 'formatToParts', methodOf(function (list) {
    const s = thisListFormat(this, 'formatToParts');
    return numberPartsToArray(listParts(s, StringListFromIterable(list)), false);
}), 1);
method(ListFormatPrototype, 'resolvedOptions', methodOf(function () {
    const s = thisListFormat(this, 'resolvedOptions');
    const o = {};
    put(o, 'locale', s.locale); put(o, 'type', s.type); put(o, 'style', s.style);
    return o;
}), 0);


// ── Intl.Collator (ECMA-402 § 10; the collation is INTL.md § 4.1's) ─────
// CLDR root's collation elements for the characters the data holds (Latin,
// the combining marks, general punctuation; COLLATION in the data), each
// packed into one number; any other character is one element whose
// primary follows every primary in the table, in code-point order (the
// booked gap: DEBTS, full DUCET).
const COLLATORS = new WeakMap();
const IMPLICIT_PRIMARY = 0x10000;
// The code-point ranges the table holds (the generator's), and a code
// point's slot in the dense table, or -1 outside them.
const COLLATION_RANGES = [[0x0000, 0x024F], [0x0300, 0x036F], [0x2000, 0x206F]];
function collationSlot(cp) {
    if (cp <= 0x024F) return cp;
    if (cp >= 0x0300 && cp <= 0x036F) return cp - 0x0300 + 0x0250;
    if (cp >= 0x2000 && cp <= 0x206F) return cp - 0x2000 + 0x02C0;
    return -1;
}
// A packed element's weights (the generator's p * 0x10000 + s * 0x40 + t * 2
// + v, below 2^32).
const elementPrimary = n => (n - n % 0x10000) / 0x10000;
const elementSecondary = n => (n % 0x10000) >> 6;
const elementTertiary = n => ((n % 0x10000) >> 1) & 0x1F;
const elementVariable = n => n % 2 === 1;
const isAsciiDigit = cp => cp >= 48 && cp <= 57;
// ICU's numeric collation (CODAN) reads a run of digits as numbers of at
// most 254 digits each, the run's leading zeros and each number's own not
// counted ("01" and "1" are one number at every level).
const NUMERIC_SEGMENT = 254;
// One unit of a string at `i` (a character, CLDR root's contraction "l·",
// or under numeric a run of digits): its packed elements, or for a run its
// digits' bounds, and where the next unit starts. Written into `unit`, a
// record the caller owns, so walking a string allocates nothing.
function collationUnit(c, text, i, unit) {
    let cp = codeAt(text, i), next = i + 1;
    if (cp >= 0xD800 && cp <= 0xDBFF && next < text.length) {
        const lo = codeAt(text, next);
        if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); next++; }
    }
    unit.digits = false;
    if (c.numeric && isAsciiDigit(cp)) {
        while (next < text.length && isAsciiDigit(codeAt(text, next))) next++;
        unit.digits = true; unit.from = i; unit.to = next; unit.next = next;
        return;
    }
    unit.next = next;
    if (next < text.length && (cp === 0x6C || cp === 0x4C)) {           // the root's contractions start with l or L
        const following = own(COLLATION.pairs, String_(cp));
        const both = following !== undefined ? own(following, String_(codeAt(text, next))) : undefined;
        if (both !== undefined) { unit.elements = both; unit.next = next + 1; unit.implicit = 0; return; }
    }
    const slot = collationSlot(cp);
    const ces = slot >= 0 ? COLLATION_DENSE[slot] : null;
    unit.elements = ces;
    unit.implicit = ces === null ? IMPLICIT_PRIMARY + cp : 0;
}
// The numbers of a digit run text[from, to), each [start, end) with its
// leading zeros skipped (one zero left of an all-zero number), to `visit`.
function numericSegments(text, from, to, visit) {
    while (from < to - 1 && codeAt(text, from) === 48) from++;
    for (let at = from; at < to; at += NUMERIC_SEGMENT) {
        let start = at;
        const end = at + NUMERIC_SEGMENT < to ? at + NUMERIC_SEGMENT : to;
        while (start < end - 1 && codeAt(text, start) === 48) start++;
        visit(start, end);
    }
}
// A cursor over one string's primary weights, filled a unit at a time into
// its own buffer: two strings are compared in step and the comparison stops
// at the first primary that differs, which is where sorting words decides.
// The buffer is a Float64Array, whose elements no page can intercept.
function primaryCursor() {
    return {__proto__: null, text: '', i: 0, buf: new Float64Array_(64), cap: 64, head: 0, tail: 0,
            unit: {__proto__: null, digits: false, from: 0, to: 0, next: 0, elements: null, implicit: 0}};
}
// A cursor between compares holds no caller's string and no buffer a long
// run of digits grew, so a long-lived collator keeps nothing of what it
// compared.
function releaseCursor(cur) {
    cur.text = '';
    if (cur.cap > 64) { cur.buf = new Float64Array_(64); cur.cap = 64; }
}
function cursorPush(cur, p) {
    if (cur.tail === cur.cap) {
        const grown = new Float64Array_(cur.cap * 2);
        for (let k = 0; k < cur.cap; k++) grown[k] = cur.buf[k];
        cur.buf = grown; cur.cap *= 2;
    }
    cur.buf[cur.tail++] = p;
}
function nextPrimary(c, cur) {
    while (cur.head === cur.tail) {
        cur.head = 0; cur.tail = 0;
        if (cur.i >= cur.text.length) return -1;
        const unit = cur.unit, text = cur.text;
        collationUnit(c, text, cur.i, unit);
        cur.i = unit.next;
        if (unit.digits) {
            const zero = elementPrimary(COLLATION_DENSE[48][0]);
            numericSegments(text, unit.from, unit.to, (start, end) => {
                cursorPush(cur, zero); cursorPush(cur, end - start);
                for (let d = start; d < end; d++) cursorPush(cur, zero + codeAt(text, d) - 48);
            });
        } else if (unit.elements === null) cursorPush(cur, unit.implicit);
        else for (let k = 0; k < unit.elements.length; k++) {
            const n = unit.elements[k], p = elementPrimary(n);
            if (p !== 0 && !(c.ignorePunctuation && elementVariable(n))) cursorPush(cur, p);
        }
    }
    return cur.buf[cur.head++];
}
// The tertiary weights of an uppercase character's elements (UCA's
// table of tertiary values: upper, wide, compatibility, font, circled, ...).
const UPPER_TERTIARIES = [0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0E, 0x11, 0x12, 0x1D];
const isUpperTertiary = t => contains(UPPER_TERTIARIES, t);
// The weights of one string below the primary level: level 2
// secondaries, level 3 tertiaries ordered by caseFirst, and the case
// level ("case" sensitivity: case only). Asked only of strings whose
// primaries tie. ignorePunctuation (UCA's "shifted") ignores a variable
// element and every element without a primary after it (the second
// element of a curly quote); a number counts once at each level.
function lowerLevels(c, text) {
    const s = [], t = [], k = [];
    const unit = {__proto__: null, digits: false, from: 0, to: 0, next: 0, elements: null, implicit: 0};
    let shifted = false;
    const tertiary = (w, primary) => {
        const upper = isUpperTertiary(w);
        const rank = c.caseFirst === 'upper' ? (upper ? 0 : 1) : (upper ? 1 : 0);
        push(t, c.caseFirst === 'false' ? w : rank * 0x100 + w);
        if (primary) push(k, rank);
    };
    for (let i = 0; i < text.length; i = unit.next) {
        collationUnit(c, text, i, unit);
        if (unit.digits) {
            const n = COLLATION_DENSE[48][0];
            numericSegments(text, unit.from, unit.to, () => { push(s, elementSecondary(n)); tertiary(elementTertiary(n), true); });
            shifted = false;
            continue;
        }
        if (unit.elements === null) { push(s, 0x20); tertiary(0x02, true); shifted = false; continue; }
        for (let e = 0; e < unit.elements.length; e++) {
            const n = unit.elements[e], p = elementPrimary(n);
            if (c.ignorePunctuation) {
                if (elementVariable(n)) { shifted = true; continue; }
                if (p !== 0) shifted = false;
                else if (shifted) continue;
            }
            if (elementSecondary(n) !== 0) push(s, elementSecondary(n));
            if (elementTertiary(n) !== 0) tertiary(elementTertiary(n), p !== 0);
        }
    }
    return {__proto__: null, s: s, t: t, k: k};
}
function compareWeights(a, b) {
    const n = a.length < b.length ? a.length : b.length;
    for (let i = 0; i < n; i++) if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1;
    return a.length === b.length ? 0 : a.length < b.length ? -1 : 1;
}
// The primary of the character at text[i] when it is a whole unit of one
// element with a primary under these options, else 0: not a digit under
// numeric, not a variable under ignorePunctuation, not the "l" of "l·".
function simplePrimary(c, text, i) {
    const cp = codeAt(text, i), slot = collationSlot(cp);
    if (slot < 0) return 0;
    const p = SIMPLE_PRIMARY[slot];
    if (p === 0 || (c.ignorePunctuation && SIMPLE_VARIABLE[slot] === 1) || (c.numeric && isAsciiDigit(cp))) return 0;
    if ((cp === 0x6C || cp === 0x4C) && i + 1 < text.length && codeAt(text, i + 1) === 0xB7) return 0;
    return p;
}
// CompareStrings: the primaries in step (inline while both characters are
// simple, then through the cursors), then the lower levels the
// sensitivity asks for. The collator's two cursors are its own, reused and
// released on every way out: nothing between here and the result runs a
// page's code.
function compareStrings(c, x, y) {
    if (x === y) return 0;
    let i = 0, j = 0;
    while (i < x.length && j < y.length) {
        const p = simplePrimary(c, x, i), q = simplePrimary(c, y, j);
        if (p === 0 || q === 0) break;
        if (p !== q) return p < q ? -1 : 1;
        i++; j++;
    }
    const a = c.cursors[0], b = c.cursors[1];
    a.text = x; a.i = i; a.head = 0; a.tail = 0;
    b.text = y; b.i = j; b.head = 0; b.tail = 0;
    let order = 0;
    try {
        for (;;) {
            const p = nextPrimary(c, a), q = nextPrimary(c, b);
            if (p !== q) { order = p < q ? -1 : 1; break; }
            if (p === -1) break;
        }
    } finally {
        releaseCursor(a); releaseCursor(b);
    }
    if (order !== 0 || c.sensitivity === 'base') return order;
    const u = lowerLevels(c, x), v = lowerLevels(c, y);
    if (c.sensitivity === 'case') return compareWeights(u.k, v.k);
    const r = compareWeights(u.s, v.s);
    if (r !== 0 || c.sensitivity === 'accent') return r;
    return compareWeights(u.t, v.t);
}

// The locale data a Collator resolves its -u- keys against: no collation
// but the root's, numeric off unless asked, caseFirst off unless asked.
function collatorLocaleData(locale, key) {
    if (key === 'co') return [null];
    if (key === 'kn') return ['false', 'true'];
    return ['false', 'lower', 'upper'];
}
function Collator(locales, options) {
    const made = new.target === undefined ? objectCreate(CollatorPrototype) : constructed(this, CollatorPrototype);
    loadCollationData();
    const requested = CanonicalizeLocaleList(locales);
    options = CoerceOptionsToObject(options);
    const c = {__proto__: null};
    c.usage = GetOption(options, 'usage', 'string', ['sort', 'search'], 'sort');
    const opt = {__proto__: null};
    opt.localeMatcher = GetOption(options, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit');
    const collation = GetOption(options, 'collation', 'string', undefined, undefined);
    if (collation !== undefined && !isTypeSequence(lower(collation))) throw new RangeError_('Invalid collation : ' + collation);
    opt.co = collation === undefined ? undefined : lower(collation);
    let numeric = GetOption(options, 'numeric', 'boolean', undefined, undefined);
    if (numeric !== undefined) numeric = ToString(numeric);
    opt.kn = numeric;
    opt.kf = GetOption(options, 'caseFirst', 'string', ['upper', 'lower', 'false'], undefined);
    const r = ResolveLocale(AVAILABLE, requested, opt, ['co', 'kf', 'kn'], collatorLocaleData);
    c.locale = r.locale;
    c.collation = r.co === null || r.co === undefined ? 'default' : r.co;
    c.numeric = r.kn === 'true';
    c.caseFirst = r.kf;
    c.sensitivity = GetOption(options, 'sensitivity', 'string', ['base', 'accent', 'case', 'variant'], 'variant');
    const ignore = GetOption(options, 'ignorePunctuation', 'boolean', undefined, undefined);
    c.ignorePunctuation = ignore === undefined ? false : ignore;
    c.cursors = [primaryCursor(), primaryCursor()];
    apply(wm_set, COLLATORS, [made, c]);
    return made;
}
const CollatorPrototype = Collator.prototype;
defineProperty(Collator, 'prototype', {__proto__: null, writable: false, enumerable: false, configurable: false});
defineProperty(Collator, 'length', {__proto__: null, value: 0, configurable: true});
function thisCollator(o, name) {
    const c = slots(COLLATORS, o);
    if (c === undefined) throw new TypeError_('Method Intl.Collator.prototype.' + name + ' called on incompatible receiver');
    return c;
}
method(Collator, 'supportedLocalesOf', methodOf(supportedLocalesOf), 1);
defineProperty(CollatorPrototype, toStringTag, {__proto__: null, value: 'Intl.Collator', writable: false, enumerable: false, configurable: true});
getter(CollatorPrototype, 'compare', function () {
    const c = thisCollator(this, 'compare');
    if (c.bound === undefined) {
        const bound = (x, y) => compareStrings(c, ToString(x), ToString(y));
        defineProperty(bound, 'name', {__proto__: null, value: '', configurable: true});
        c.bound = bound;
    }
    return c.bound;
});
method(CollatorPrototype, 'resolvedOptions', methodOf(function () {
    const c = thisCollator(this, 'resolvedOptions');
    const o = {};
    put(o, 'locale', c.locale); put(o, 'usage', c.usage); put(o, 'sensitivity', c.sensitivity);
    put(o, 'ignorePunctuation', c.ignorePunctuation); put(o, 'collation', c.collation);
    put(o, 'numeric', c.numeric); put(o, 'caseFirst', c.caseFirst);
    return o;
}), 0);

// ── Intl's functions (ECMA-402 § 8.3) ──────────────────────────────────
// A fresh array of a list's elements, as CreateArrayFromList makes one.
function arrayOf(list) { const out = []; for (let i = 0; i < list.length; i++) push(out, list[i]); return out; }
function getCanonicalLocales(locales) { loadData(); return arrayOf(CanonicalizeLocaleList(locales)); }
// supportedValuesOf: what the data truly holds, sorted. The zones are
// every canonical zone, "UTC" and "Etc/GMT+5" among them, as test262 holds
// (V8 lists the location zones alone: INTL.md § 9.6); the currencies are
// those some region uses as tender today (CLDR's), where V8 lists ICU's
// ISO table, which keeps a few just retired; the one collation is the
// root's, which has no name to list.
function supportedValuesOf(key) {
    key = ToString(key);
    let list;
    if (key === 'calendar') list = ['gregory'];
    else if (key === 'collation') list = [];
    else if (key === 'currency') { loadNumberData(); list = arrayOf(CURRENT_CURRENCIES); }
    else if (key === 'numberingSystem') list = ['latn'];
    else if (key === 'timeZone') list = arrayOf(zoneList());
    else if (key === 'unit') list = arrayOf(SANCTIONED_UNITS);
    else throw new RangeError_('Invalid key : ' + key);
    sortStrings(list);
    return list;
}

// ── The built-ins ECMA-402 redefines (§ 19, § 20) ───────────────────────
// Each formats through Intl's own constructors; with neither locales nor
// options, through one made once a realm (V8 keeps the same), since a page
// may call them in a loop.
const DEFAULT_FORMATS = {__proto__: null};
function defaultSlots(key, make) {
    let s = own(DEFAULT_FORMATS, key);
    if (s === undefined) { s = make(); DEFAULT_FORMATS[key] = s; }
    return s;
}
function dateTimeSlots(locales, options, required, defaults) {
    const make = () => { const key = {__proto__: null}; CreateDateTimeFormat(key, locales, options, required, defaults); return slots(DTFS, key); };
    return locales === undefined && options === undefined ? defaultSlots('date ' + required + ' ' + defaults, make) : make();
}
// Date.prototype.toLocaleString and its date and time halves (§ 20.4).
function dateToLocale(date, locales, options, required, defaults) {
    const x = apply(dateGetTime, date, []);
    if (x !== x) return 'Invalid Date';
    const s = dateTimeSlots(locales, options, required, defaults);
    return asciiSpaces(partsToString(formatParts(s, s.pattern, x)));
}
function numberSlots(locales, options) {
    const make = () => slots(NUMBER_FORMATS, new NumberFormat(locales, options));
    return locales === undefined && options === undefined ? defaultSlots('number', make) : make();
}
function collatorSlots(locales, options) {
    const make = () => slots(COLLATORS, new Collator(locales, options));
    return locales === undefined && options === undefined ? defaultSlots('collator', make) : make();
}
// Array.prototype.toLocaleString (§ 19.5.1): each element's own
// toLocaleString, given the locales and options. An array already being
// written (one that holds itself, at any depth) writes as nothing, as V8's
// and SpiderMonkey's do ([1, a] is "1,"), where recursing would overflow
// the stack.
const LOCALE_JOINS = [];
let localeJoinDepth = 0;
function arrayToLocaleString(locales, options) {
    const array = ToObject(this);
    for (let i = 0; i < localeJoinDepth; i++) if (LOCALE_JOINS[i] === array) return '';
    put(LOCALE_JOINS, localeJoinDepth++, array);
    try {
        const len = ToLength(array.length), out = [];
        for (let k = 0; k < len; k++) {
            const element = array[k];
            if (element === undefined || element === null) { push(out, ''); continue; }
            const f = element.toLocaleString;
            if (typeof f !== 'function') throw new TypeError_('toLocaleString is not a function');
            push(out, ToString(apply(f, element, [locales, options])));
        }
        return join(out, ',');
    } finally {
        put(LOCALE_JOINS, --localeJoinDepth, undefined);
    }
}

// ── The Intl object ─────────────────────────────────────────────────────
const Intl = {};     // script-visible: its properties are defined, below
defineProperty(Intl, toStringTag, {__proto__: null, value: 'Intl', writable: false, enumerable: false, configurable: true});
// The constructors in Chrome's order, so a page listing Intl's names reads
// these in the order it would there.
method(Intl, 'getCanonicalLocales', methodOf(getCanonicalLocales), 1);
method(Intl, 'supportedValuesOf', methodOf(supportedValuesOf), 1);
const CONSTRUCTORS = [['DateTimeFormat', DateTimeFormat], ['NumberFormat', NumberFormat], ['Collator', Collator], ['PluralRules', PluralRules],
                      ['RelativeTimeFormat', RelativeTimeFormat], ['ListFormat', ListFormat], ['Locale', Locale]];
for (let i = 0; i < CONSTRUCTORS.length; i++)
    defineProperty(Intl, CONSTRUCTORS[i][0], {__proto__: null, value: CONSTRUCTORS[i][1], writable: true, enumerable: false, configurable: true});
// The redefined built-ins, last: once they are defined the realm's
// toLocale* and localeCompare are Intl's.
const DateProto = Date_.prototype;
method(DateProto, 'toLocaleString', methodOf(function (locales, options) { return dateToLocale(this, locales, options, 'any', 'all'); }), 0);
method(DateProto, 'toLocaleDateString', methodOf(function (locales, options) { return dateToLocale(this, locales, options, 'date', 'date'); }), 0);
method(DateProto, 'toLocaleTimeString', methodOf(function (locales, options) { return dateToLocale(this, locales, options, 'time', 'time'); }), 0);
method(Number_.prototype, 'toLocaleString', methodOf(function (locales, options) {
    const x = apply(numberValueOf, this, []);
    return partsToString(numberParts(numberSlots(locales, options), decimalOfNumber(x)));
}), 0);
method(BigInt_.prototype, 'toLocaleString', methodOf(function (locales, options) {
    const x = apply(bigintValueOf, this, []);
    return partsToString(numberParts(numberSlots(locales, options), ToIntlMathematicalValue(x)));
}), 0);
method(StringProto, 'localeCompare', methodOf(function (that, locales, options) {
    if (this === undefined || this === null) throw new TypeError_('String.prototype.localeCompare called on null or undefined');
    const s = ToString(this), t = ToString(that);
    return compareStrings(collatorSlots(locales, options), s, t);
}), 1);
method(Array_.prototype, 'toLocaleString', methodOf(arrayToLocaleString), 0);
// toLocaleUpperCase and toLocaleLowerCase (§ 19.1.2-3): the locales are
// validated, and every locale the data holds cases as the engine's
// locale-free mapping does (Turkish, Azeri and Lithuanian, which case
// otherwise, are not held: INTL.md § 7).
method(StringProto, 'toLocaleUpperCase', methodOf(function (locales) {
    if (this === undefined || this === null) throw new TypeError_('String.prototype.toLocaleUpperCase called on null or undefined');
    const s = ToString(this);
    loadData(); CanonicalizeLocaleList(locales);
    return apply(s_toUpperCase, s, []);
}), 0);
method(StringProto, 'toLocaleLowerCase', methodOf(function (locales) {
    if (this === undefined || this === null) throw new TypeError_('String.prototype.toLocaleLowerCase called on null or undefined');
    const s = ToString(this);
    loadData(); CanonicalizeLocaleList(locales);
    return lower(s);
}), 0);
return Intl;
})
