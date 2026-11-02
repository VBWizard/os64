/* THE STREAMS STANDARD, self-hosted (STREAMS.md; streams.spec.whatwg.org).
 *
 * This file is one function expression. libdom's streams.c runs it in each
 * realm at the realm's start and calls it with the INTRINSICS it captured
 * there (`I`, a null-prototype object): the engine's own Promise, its
 * `then`, the error constructors and the rest, taken before any page script
 * ran. Every built-in this file uses is one of those captures, or taken
 * from them as the file loads (which is the realm's start), never a global
 * looked up when an algorithm runs, because a page may replace any
 * global or prototype member afterwards and the standard's algorithms must
 * not notice (WPT's patched-global pages). For the same reason:
 *
 *   - internal algorithms never call a public method: tee, the pipes and
 *     async iteration work on the internal records directly;
 *   - no for-of, no spread, no destructuring of arrays, no method looked up
 *     on an array, and no array as storage: those go through
 *     Array.prototype, which a page owns, and an index an array lacks
 *     reaches a getter or setter a page put on Object.prototype;
 *   - internal records are instances of classes declared here whose every
 *     field is a declared class field: a field is DEFINED on the object at
 *     construction, so neither a read nor a later write reaches
 *     Object.prototype, where a page may have put a getter or a setter (an
 *     assignment to a property the object lacks would call that setter);
 *   - a deferred this file settles with a promise of its own goes through
 *     that promise's captured `then` (resolveWithPromise), never through
 *     the resolve function's lookup of `then`. WebIDL's "a promise
 *     resolved with" (resolvedWith) does resolve a new promise with the
 *     value, so a value a page returned is looked at as the standard looks
 *     at it.
 *
 * Each public object carries ONE private field, the internal record that
 * holds its slots; `#x in o` is the brand check the IDL asks for, and the
 * records point at one another, never at a public object, except through
 * `pub`, the object a page sees.
 *
 * What it returns: the interfaces, which streams.c defines on the global
 * through the Modern Web's table (modern.c), and `door`, the internal
 * object behind libdom's d_stream_* verbs (streams.c), through which a
 * body's stream is made, read, teed and errored. */
(function (I) {
'use strict';

// ── The captures ────────────────────────────────────────────────────────
// The C list (streams.c) and this one must agree: a missing name fails the
// load, which fails the realm's creation, so the DOM host suite fails at its
// first window.
const NEEDED = ['Promise', 'Promise_then', 'Promise_reject', 'Reflect_apply',
    'Object_defineProperty', 'Object_getPrototypeOf', 'Object_getOwnPropertyNames',
    'Object_getOwnPropertyDescriptor', 'Object_create',
    'TypeError', 'RangeError', 'Uint8Array', 'Uint8Array_set', 'queueMicrotask',
    'ArrayBuffer', 'DataView', 'Int8Array', 'Uint8ClampedArray', 'Int16Array', 'Uint16Array', 'Int32Array',
    'Uint32Array', 'Float16Array', 'Float32Array', 'Float64Array', 'BigInt64Array', 'BigUint64Array',
    'structuredClone',
    'Symbol_asyncIterator', 'Symbol_iterator', 'Symbol_toStringTag',
    'AbortController', 'AbortController_abort', 'AbortController_signal',
    'AbortSignal_aborted', 'AbortSignal_reason',
    'EventTarget_addEventListener', 'EventTarget_removeEventListener', 'AsyncIteratorPrototype'];
for (let i = 0; i < NEEDED.length; i++)
    if (!(NEEDED[i] in I)) throw 'os64-streams: the intrinsic ' + NEEDED[i] + ' is missing';

const Promise_ = I.Promise;
const then_ = I.Promise_then;
const reject_ = I.Promise_reject;
const apply = I.Reflect_apply;
const defineProperty = I.Object_defineProperty;
const getPrototypeOf = I.Object_getPrototypeOf;
const getOwnPropertyNames = I.Object_getOwnPropertyNames;
const objectCreate = I.Object_create;
const getOwnPropertyDescriptor = I.Object_getOwnPropertyDescriptor;
const TypeError_ = I.TypeError;
const RangeError_ = I.RangeError;
const Uint8Array_ = I.Uint8Array;
const uint8Set = I.Uint8Array_set;
// %TypedArray%.prototype's getters, which read a typed array's internal
// slots: taken from the captures at load, which is the realm's start.
const typedArrayPrototype = getPrototypeOf(Uint8Array_.prototype);
const typedArrayTag = getOwnPropertyDescriptor(typedArrayPrototype, I.Symbol_toStringTag).get;
const typedArrayByteLength = getOwnPropertyDescriptor(typedArrayPrototype, 'byteLength').get;
const typedArrayByteOffset = getOwnPropertyDescriptor(typedArrayPrototype, 'byteOffset').get;
const typedArrayBuffer = getOwnPropertyDescriptor(typedArrayPrototype, 'buffer').get;
const typedArrayLength = getOwnPropertyDescriptor(typedArrayPrototype, 'length').get;
// A byte stream's buffers: ArrayBuffer's and DataView's own getters and
// transfer(), taken the same way. Each getter checks its receiver's slots,
// so ArrayBuffer's byteLength throws for a SharedArrayBuffer and
// DataView's for anything but a DataView.
const ArrayBuffer_ = I.ArrayBuffer;
const DataView_ = I.DataView;
const arrayBufferByteLength = getOwnPropertyDescriptor(ArrayBuffer_.prototype, 'byteLength').get;
const arrayBufferDetached = getOwnPropertyDescriptor(ArrayBuffer_.prototype, 'detached').get;
const arrayBufferResizable = getOwnPropertyDescriptor(ArrayBuffer_.prototype, 'resizable').get;
const arrayBufferTransfer = getOwnPropertyDescriptor(ArrayBuffer_.prototype, 'transfer').value;
const dataViewByteLength = getOwnPropertyDescriptor(DataView_.prototype, 'byteLength').get;
const dataViewByteOffset = getOwnPropertyDescriptor(DataView_.prototype, 'byteOffset').get;
const dataViewBuffer = getOwnPropertyDescriptor(DataView_.prototype, 'buffer').get;
// [[TypedArrayName]] to the view's constructor and element size (ECMA-262's
// table of the typed array constructors), for the view a BYOB read is
// answered with.
const typedArrays = objectCreate(null);
function typedArrayKind(name, ctor, size) {
    const kind = objectCreate(null);
    kind.ctor = ctor;
    kind.size = size;
    typedArrays[name] = kind;
}
typedArrayKind('Int8Array', I.Int8Array, 1);
typedArrayKind('Uint8Array', Uint8Array_, 1);
typedArrayKind('Uint8ClampedArray', I.Uint8ClampedArray, 1);
typedArrayKind('Int16Array', I.Int16Array, 2);
typedArrayKind('Uint16Array', I.Uint16Array, 2);
typedArrayKind('Int32Array', I.Int32Array, 4);
typedArrayKind('Uint32Array', I.Uint32Array, 4);
typedArrayKind('Float16Array', I.Float16Array, 2);
typedArrayKind('Float32Array', I.Float32Array, 4);
typedArrayKind('Float64Array', I.Float64Array, 8);
typedArrayKind('BigInt64Array', I.BigInt64Array, 8);
typedArrayKind('BigUint64Array', I.BigUint64Array, 8);
const queueMicrotask_ = I.queueMicrotask;
const structuredClone_ = I.structuredClone;
const asyncIteratorSymbol = I.Symbol_asyncIterator;
const iteratorSymbol = I.Symbol_iterator;
const toStringTag = I.Symbol_toStringTag;
const AbortController_ = I.AbortController;
const abortControllerAbort = I.AbortController_abort;
const abortControllerSignal = I.AbortController_signal;
const abortSignalAborted = I.AbortSignal_aborted;
const abortSignalReason = I.AbortSignal_reason;
const addEventListener = I.EventTarget_addEventListener;
const removeEventListener = I.EventTarget_removeEventListener;
const AsyncIteratorPrototype = I.AsyncIteratorPrototype;

// Only this file can name it: a constructor given it as its first argument
// is being called from here, and takes its record from the second.
const INTERNAL = objectCreate(null);

// ── Promises (the standard's § 1 conventions, over the captures) ───────
class Deferred {
    promise;
    resolveFn;
    rejectFn;
    state;
    constructor() {
        this.promise = undefined;
        this.resolveFn = undefined;
        this.rejectFn = undefined;
        this.state = 'pending';
        const self = this;
        this.promise = new Promise_(function (resolve, reject) {
            self.resolveFn = resolve;
            self.rejectFn = reject;
        });
    }
}

function newDeferred() { return new Deferred(); }

// Resolve with a value that is not a promise of this realm's making; a
// promise goes through resolveWithPromise, so no `then` is looked up.
function resolveDeferred(d, value) {
    if (d.state !== 'pending') return;
    d.state = 'fulfilled';
    d.resolveFn(value);
}

function rejectDeferred(d, reason) {
    if (d.state !== 'pending') return;
    d.state = 'rejected';
    d.rejectFn(reason);
}

function resolveWithPromise(d, promise) {
    uponPromise(promise, function (v) { resolveDeferred(d, v); }, function (r) { rejectDeferred(d, r); });
}

// WebIDL's "a promise resolved with": a NEW promise resolved with the
// value, so a promise given takes the adoption's two extra ticks, as the
// standard's timing (and WPT's) assume; Promise.resolve would hand the
// same promise back.
function resolvedWith(value) {
    return new Promise_(function (resolve) { resolve(value); });
}
function rejectedWith(reason) { return apply(reject_, Promise_, [reason]); }

function noop() {}

function setHandled(promise) { apply(then_, promise, [undefined, noop]); }

// "Upon fulfillment / rejection": the derived promise is marked handled,
// so a step that throws is never reported as the page's rejection.
function uponPromise(promise, onFulfilled, onRejected) {
    setHandled(apply(then_, promise, [onFulfilled, onRejected]));
}
function uponFulfillment(promise, onFulfilled) { uponPromise(promise, onFulfilled, undefined); }
function uponRejection(promise, onRejected) { uponPromise(promise, undefined, onRejected); }

// "The result of reacting to": a new promise the caller hands on.
function transformPromise(promise, onFulfilled, onRejected) {
    return apply(then_, promise, [onFulfilled, onRejected]);
}

function rejectedDeferred(reason) {
    const d = newDeferred();
    rejectDeferred(d, reason);
    setHandled(d.promise);
    return d;
}

function resolvedDeferred(value) {
    const d = newDeferred();
    resolveDeferred(d, value);
    return d;
}

// ── WebIDL conversions ──────────────────────────────────────────────────
function isObject(v) { return (typeof v === 'object' && v !== null) || typeof v === 'function'; }

function toNumber(v) { return +v; }

function toBoolean(v) { return !!v; }

function callbackOrUndefined(value, what) {
    if (value === undefined) return undefined;
    if (typeof value !== 'function') throw new TypeError_(what + ' is not a function');
    return value;
}

// A dictionary argument: undefined and null are the empty dictionary,
// anything else that is no object a TypeError.
function dictionarySource(value, what) {
    if (value === undefined || value === null) return undefined;
    if (!isObject(value)) throw new TypeError_(what + ' is not an object');
    return value;
}

function member(source, name) { return source === undefined ? undefined : source[name]; }

class QueuingStrategyDict {
    highWaterMark;
    size;
    constructor(value) {
        const source = dictionarySource(value, 'The strategy');
        const hwm = member(source, 'highWaterMark');
        this.highWaterMark = hwm === undefined ? undefined : toNumber(hwm);
        this.size = callbackOrUndefined(member(source, 'size'), 'The strategy\'s size');
    }
}

function extractHighWaterMark(strategy, defaultHWM) {
    if (strategy.highWaterMark === undefined) return defaultHWM;
    const hwm = strategy.highWaterMark;
    if (hwm !== hwm || hwm < 0) throw new RangeError_('Invalid highWaterMark');
    return hwm;
}

function extractSizeAlgorithm(strategy) {
    const size = strategy.size;
    if (size === undefined) return sizeOne;
    return function (chunk) { return toNumber(apply(size, undefined, [chunk])); };
}

function sizeOne() { return 1; }

// Invoking a callback whose IDL return is a promise: a throw is a rejected
// promise, and what it returns is resolved into one.
function promiseCall(fn, thisArg, args) {
    try {
        return resolvedWith(apply(fn, thisArg, args));
    } catch (e) {
        return rejectedWith(e);
    }
}

function webidlInterface(cls, name, interfaceStatics) {
    const proto = cls.prototype;
    const names = getOwnPropertyNames(proto);
    for (let i = 0; i < names.length; i++) {
        if (names[i] === 'constructor') continue;
        defineProperty(proto, names[i], {enumerable: true});
    }
    if (interfaceStatics)
        for (let i = 0; i < interfaceStatics.length; i++) defineProperty(cls, interfaceStatics[i], {enumerable: true});
    defineProperty(proto, toStringTag, {value: name, writable: false, enumerable: false, configurable: true});
}

// ── The queue with sizes (the standard's § 8.1) ─────────────────────────
// Index-based, with a moving head, so a dequeue is constant time. The
// slots live in null-prototype objects, not arrays: a write to an index an
// array lacks walks to Array.prototype and Object.prototype, where a page
// may have put a setter for "0", and a read there would reach its getter.
// A queue that is never empty (a pipe into a slower sink) would grow by a
// slot per chunk ever passed, so the live slots are moved down once the
// dead ones outnumber them: amortized constant time, and room for what is
// queued, not for what went through.
function slots() { return objectCreate(null); }

function compact(from, head, length) {
    const to = slots();
    for (let i = 0; i < length; i++) to[i] = from[head + i];
    return to;
}

class Queue {
    items;
    sizes;
    head;
    length;
    constructor() {
        this.items = slots();
        this.sizes = slots();
        this.head = 0;
        this.length = 0;
    }
}

function queuePush(q, value, size) {
    const at = q.head + q.length;
    q.items[at] = value;
    q.sizes[at] = size;
    q.length++;
}

function queueShift(q) {
    const at = q.head;
    const pair = new QueuePair(q.items[at], q.sizes[at]);
    q.items[at] = undefined;
    q.length--;
    q.head++;
    if (q.length === 0) {
        q.items = slots();
        q.sizes = slots();
        q.head = 0;
    } else if (q.head > 64 && q.head > q.length) {
        q.items = compact(q.items, q.head, q.length);
        q.sizes = compact(q.sizes, q.head, q.length);
        q.head = 0;
    }
    return pair;
}

class QueuePair {
    value;
    size;
    constructor(value, size) {
        this.value = value;
        this.size = size;
    }
}

function isNonNegativeNumber(v) { return typeof v === 'number' && v === v && v >= 0; }

function dequeueValue(container) {
    const pair = queueShift(container.queue);
    container.queueTotalSize -= pair.size;
    if (container.queueTotalSize < 0) container.queueTotalSize = 0;
    return pair.value;
}

function enqueueValueWithSize(container, value, size) {
    if (!isNonNegativeNumber(size)) throw new RangeError_('The size of a chunk must be a non-negative number');
    if (size === Infinity) throw new RangeError_('The size of a chunk must be finite');
    queuePush(container.queue, value, size);
    container.queueTotalSize += size;
}

function peekQueueValue(container) { return container.queue.items[container.queue.head]; }

function resetQueue(container) {
    container.queue = new Queue();
    container.queueTotalSize = 0;
}

// A simple list (read requests, write requests), kept as the queue is.
class List {
    items;
    head;
    length;
    constructor() {
        this.items = slots();
        this.head = 0;
        this.length = 0;
    }
}

function listPush(l, v) {
    l.items[l.head + l.length] = v;
    l.length++;
}

function listShift(l) {
    const v = l.items[l.head];
    l.items[l.head] = undefined;
    l.length--;
    l.head++;
    if (l.length === 0) {
        l.items = slots();
        l.head = 0;
    } else if (l.head > 64 && l.head > l.length) {
        l.items = compact(l.items, l.head, l.length);
        l.head = 0;
    }
    return v;
}

function listPeek(l) { return l.items[l.head]; }

// ── Records ─────────────────────────────────────────────────────────────
class RSRecord {
    pub;
    state;
    reader;
    storedError;
    disturbed;
    controller;
    constructor() {
        this.pub = undefined;
        this.state = 'readable';
        this.reader = undefined;
        this.storedError = undefined;
        this.disturbed = false;
        this.controller = undefined;
    }
}

// A stream's controller is one of two kinds, told apart by `byte` (never
// by instanceof, which looks up Symbol.hasInstance where a page can put
// one): the default controller's record below, or RBSControllerRecord.
class RSControllerRecord {
    pub;
    byte;
    stream;
    queue;
    queueTotalSize;
    started;
    closeRequested;
    pullAgain;
    pulling;
    strategySizeAlgorithm;
    strategyHWM;
    pullAlgorithm;
    cancelAlgorithm;
    constructor() {
        this.pub = undefined;
        this.byte = false;
        this.stream = undefined;
        this.queue = undefined;
        this.queueTotalSize = 0;
        this.started = false;
        this.closeRequested = false;
        this.pullAgain = false;
        this.pulling = false;
        this.strategySizeAlgorithm = undefined;
        this.strategyHWM = 0;
        this.pullAlgorithm = undefined;
        this.cancelAlgorithm = undefined;
    }
}

// A byte stream's controller (§ 4.7). Its queue holds ByteQueueEntry
// records; pendingPullIntos holds PullIntoDescriptor records, the head
// being the read the next bytes fill.
class RBSControllerRecord {
    pub;
    byte;
    stream;
    autoAllocateChunkSize;
    byobRequest;
    queue;
    queueTotalSize;
    started;
    closeRequested;
    pullAgain;
    pulling;
    strategyHWM;
    pullAlgorithm;
    cancelAlgorithm;
    pendingPullIntos;
    constructor() {
        this.pub = undefined;
        this.byte = true;
        this.stream = undefined;
        this.autoAllocateChunkSize = undefined;
        this.byobRequest = null;
        this.queue = new List();
        this.queueTotalSize = 0;
        this.started = false;
        this.closeRequested = false;
        this.pullAgain = false;
        this.pulling = false;
        this.strategyHWM = 0;
        this.pullAlgorithm = undefined;
        this.cancelAlgorithm = undefined;
        this.pendingPullIntos = new List();
    }
}

class ByteQueueEntry {
    buffer;
    byteOffset;
    byteLength;
    constructor(buffer, byteOffset, byteLength) {
        this.buffer = buffer;
        this.byteOffset = byteOffset;
        this.byteLength = byteLength;
    }
}

class PullIntoDescriptor {
    buffer;
    bufferByteLength;
    byteOffset;
    byteLength;
    bytesFilled;
    minimumFill;
    elementSize;
    viewConstructor;
    readerType;
    constructor(buffer, bufferByteLength, byteOffset, byteLength, minimumFill, elementSize, viewConstructor,
        readerType) {
        this.buffer = buffer;
        this.bufferByteLength = bufferByteLength;
        this.byteOffset = byteOffset;
        this.byteLength = byteLength;
        this.bytesFilled = 0;
        this.minimumFill = minimumFill;
        this.elementSize = elementSize;
        this.viewConstructor = viewConstructor;
        this.readerType = readerType;
    }
}

class BYOBRequestRecord {
    pub;
    controller;
    view;
    constructor(controller, view) {
        this.pub = undefined;
        this.controller = controller;
        this.view = view;
    }
}

// A reader is one of two kinds, told apart by `byob`: a default reader
// keeps readRequests, a BYOB reader readIntoRequests (read requests whose
// steps are given a view).
class RSReaderRecord {
    pub;
    byob;
    stream;
    closed;
    readRequests;
    readIntoRequests;
    constructor(byob) {
        this.pub = undefined;
        this.byob = byob;
        this.stream = undefined;
        this.closed = undefined;
        this.readRequests = new List();
        this.readIntoRequests = new List();
    }
}

class ReadRequest {
    chunkSteps;
    closeSteps;
    errorSteps;
    constructor(chunkSteps, closeSteps, errorSteps) {
        this.chunkSteps = chunkSteps;
        this.closeSteps = closeSteps;
        this.errorSteps = errorSteps;
    }
}

class WSRecord {
    pub;
    state;
    storedError;
    writer;
    controller;
    inFlightWriteRequest;
    closeRequest;
    inFlightCloseRequest;
    pendingAbortRequest;
    writeRequests;
    backpressure;
    constructor() {
        this.pub = undefined;
        this.state = 'writable';
        this.storedError = undefined;
        this.writer = undefined;
        this.controller = undefined;
        this.inFlightWriteRequest = undefined;
        this.closeRequest = undefined;
        this.inFlightCloseRequest = undefined;
        this.pendingAbortRequest = undefined;
        this.writeRequests = new List();
        this.backpressure = false;
    }
}

class PendingAbortRequest {
    deferred;
    reason;
    wasAlreadyErroring;
    constructor(deferred, reason, wasAlreadyErroring) {
        this.deferred = deferred;
        this.reason = reason;
        this.wasAlreadyErroring = wasAlreadyErroring;
    }
}

class WSControllerRecord {
    pub;
    stream;
    queue;
    queueTotalSize;
    started;
    strategyHWM;
    strategySizeAlgorithm;
    writeAlgorithm;
    closeAlgorithm;
    abortAlgorithm;
    abortController;
    constructor() {
        this.pub = undefined;
        this.stream = undefined;
        this.queue = undefined;
        this.queueTotalSize = 0;
        this.started = false;
        this.strategyHWM = 0;
        this.strategySizeAlgorithm = undefined;
        this.writeAlgorithm = undefined;
        this.closeAlgorithm = undefined;
        this.abortAlgorithm = undefined;
        this.abortController = undefined;
    }
}

class WSWriterRecord {
    pub;
    stream;
    ready;
    closed;
    constructor() {
        this.pub = undefined;
        this.stream = undefined;
        this.ready = undefined;
        this.closed = undefined;
    }
}

class TSRecord {
    pub;
    readable;
    writable;
    backpressure;
    backpressureChange;
    controller;
    constructor() {
        this.pub = undefined;
        this.readable = undefined;
        this.writable = undefined;
        this.backpressure = undefined;
        this.backpressureChange = undefined;
        this.controller = undefined;
    }
}

class TSControllerRecord {
    pub;
    stream;
    transformAlgorithm;
    flushAlgorithm;
    cancelAlgorithm;
    finish;
    constructor() {
        this.pub = undefined;
        this.stream = undefined;
        this.transformAlgorithm = undefined;
        this.flushAlgorithm = undefined;
        this.cancelAlgorithm = undefined;
        this.finish = undefined;
    }
}

const closeSentinel = objectCreate(null);
const endOfIteration = objectCreate(null);

// ── ReadableStream (§ 4.2) ───────────────────────────────────────────────
let rsOf, rsReaderOf, rsControllerOf;

class ReadableStream {
    #r;
    static {
        rsOf = function (o) { return isObject(o) && #r in o ? o.#r : undefined; };
    }

    constructor(underlyingSource = undefined, strategy = undefined) {
        if (underlyingSource === INTERNAL) {
            this.#r = strategy;
            strategy.pub = this;
            return;
        }
        if (underlyingSource !== undefined && !isObject(underlyingSource))
            throw new TypeError_('The underlying source is not an object');
        const strategyDict = new QueuingStrategyDict(strategy);
        if (underlyingSource === undefined) underlyingSource = null;
        const source = dictionarySource(underlyingSource, 'The underlying source');
        const autoAllocateValue = member(source, 'autoAllocateChunkSize');
        const autoAllocate = autoAllocateValue === undefined ? undefined : enforceRangeU64(autoAllocateValue);
        const cancel = callbackOrUndefined(member(source, 'cancel'), 'cancel');
        const pull = callbackOrUndefined(member(source, 'pull'), 'pull');
        const start = callbackOrUndefined(member(source, 'start'), 'start');
        const typeValue = member(source, 'type');
        let type;
        if (typeValue !== undefined) {
            type = '' + typeValue;
            if (type !== 'bytes') throw new TypeError_('"' + type + '" is not a ReadableStreamType');
        }
        const r = new RSRecord();
        this.#r = r;
        r.pub = this;
        if (type === 'bytes') {
            if (strategyDict.size !== undefined) throw new RangeError_('A byte stream\'s strategy has no size');
            const byteHWM = extractHighWaterMark(strategyDict, 0);
            setUpRBSControllerFromSource(r, underlyingSource, start, pull, cancel, autoAllocate, byteHWM);
            return;
        }
        const sizeAlgorithm = extractSizeAlgorithm(strategyDict);
        const hwm = extractHighWaterMark(strategyDict, 1);
        setUpRSDefaultControllerFromSource(r, underlyingSource, start, pull, cancel, hwm, sizeAlgorithm);
    }

    static from(asyncIterable) { return readableStreamFromIterable(asyncIterable).pub; }

    get locked() {
        const r = rsOf(this);
        if (r === undefined) throw new TypeError_('Illegal invocation');
        return isReadableStreamLocked(r);
    }

    cancel(reason = undefined) {
        const r = rsOf(this);
        if (r === undefined) return rejectedWith(new TypeError_('Illegal invocation'));
        if (isReadableStreamLocked(r)) return rejectedWith(new TypeError_('The stream is locked'));
        return readableStreamCancel(r, reason);
    }

    getReader(options = undefined) {
        const r = rsOf(this);
        if (r === undefined) throw new TypeError_('Illegal invocation');
        const source = dictionarySource(options, 'The options');
        const modeValue = member(source, 'mode');
        if (modeValue !== undefined) {
            const mode = '' + modeValue;
            if (mode !== 'byob') throw new TypeError_('"' + mode + '" is not a ReadableStreamReaderMode');
            return acquireBYOBReader(r).pub;
        }
        return acquireDefaultReader(r).pub;
    }

    pipeThrough(transform, options = undefined) {
        const r = rsOf(this);
        if (r === undefined) throw new TypeError_('Illegal invocation');
        if (!isObject(transform)) throw new TypeError_('The transform is not an object');
        const readable = transform.readable;
        if (readable === undefined) throw new TypeError_('The transform has no readable');
        const readableRecord = rsOf(readable);
        if (readableRecord === undefined) throw new TypeError_('The transform\'s readable is not a ReadableStream');
        const writable = transform.writable;
        if (writable === undefined) throw new TypeError_('The transform has no writable');
        const writableRecord = wsOf(writable);
        if (writableRecord === undefined) throw new TypeError_('The transform\'s writable is not a WritableStream');
        const opts = new PipeOptions(options);
        if (isReadableStreamLocked(r)) throw new TypeError_('The stream is locked');
        if (isWritableStreamLocked(writableRecord)) throw new TypeError_('The transform\'s writable is locked');
        const promise = readableStreamPipeTo(r, writableRecord, opts.preventClose, opts.preventAbort,
            opts.preventCancel, opts.signal);
        setHandled(promise);
        return readable;
    }

    pipeTo(destination, options = undefined) {
        const r = rsOf(this);
        if (r === undefined) return rejectedWith(new TypeError_('Illegal invocation'));
        const w = wsOf(destination);
        if (w === undefined) return rejectedWith(new TypeError_('The destination is not a WritableStream'));
        let opts;
        try {
            opts = new PipeOptions(options);
        } catch (e) {
            return rejectedWith(e);
        }
        if (isReadableStreamLocked(r)) return rejectedWith(new TypeError_('The stream is locked'));
        if (isWritableStreamLocked(w)) return rejectedWith(new TypeError_('The destination is locked'));
        return readableStreamPipeTo(r, w, opts.preventClose, opts.preventAbort, opts.preventCancel, opts.signal);
    }

    tee() {
        const r = rsOf(this);
        if (r === undefined) throw new TypeError_('Illegal invocation');
        const branches = readableStreamTee(r, false);
        return [branches.branch1.pub, branches.branch2.pub];
    }

    values(options = undefined) {
        const r = rsOf(this);
        if (r === undefined) throw new TypeError_('Illegal invocation');
        const source = dictionarySource(options, 'The options');
        const preventCancel = toBoolean(member(source, 'preventCancel'));
        const reader = acquireDefaultReader(r);
        return newAsyncIterator(new AsyncIteratorRecord(reader, preventCancel));
    }
}

// WebIDL's [EnforceRange] unsigned long long, `what` naming the value.
function enforceRangeU64(value, what = 'autoAllocateChunkSize') {
    const n = toNumber(value);
    if (n !== n || n === Infinity || n === -Infinity) throw new TypeError_(what + ' is not finite');
    const t = n < 0 ? -Math_floor(-n) : Math_floor(n);
    if (t < 0 || t > 9007199254740991) throw new TypeError_(what + ' is out of range');
    return t;
}

function Math_floor(n) { return n - (n % 1); }

class PipeOptions {
    preventAbort;
    preventCancel;
    preventClose;
    signal;
    constructor(value) {
        const source = dictionarySource(value, 'The options');
        this.preventAbort = toBoolean(member(source, 'preventAbort'));
        this.preventCancel = toBoolean(member(source, 'preventCancel'));
        this.preventClose = toBoolean(member(source, 'preventClose'));
        const signal = member(source, 'signal');
        if (signal !== undefined && !isAbortSignal(signal)) throw new TypeError_('The signal is not an AbortSignal');
        this.signal = signal;
    }
}

function isAbortSignal(value) {
    if (!isObject(value)) return false;
    try {
        apply(abortSignalAborted, value, []);
        return true;
    } catch (e) {
        return false;
    }
}

// ── The default reader (§ 4.3) ───────────────────────────────────────────
class ReadableStreamDefaultReader {
    #r;
    static {
        rsReaderOf = function (o) { return isObject(o) && #r in o ? o.#r : undefined; };
    }

    constructor(stream, record = undefined) {
        if (stream === INTERNAL) {
            this.#r = record;
            record.pub = this;
            return;
        }
        const s = rsOf(stream);
        if (s === undefined) throw new TypeError_('The argument is not a ReadableStream');
        const reader = new RSReaderRecord(false);
        this.#r = reader;
        reader.pub = this;
        setUpDefaultReader(reader, s);
    }

    get closed() {
        const reader = rsReaderOf(this);
        if (reader === undefined) return rejectedWith(new TypeError_('Illegal invocation'));
        return reader.closed.promise;
    }

    cancel(reason = undefined) {
        const reader = rsReaderOf(this);
        if (reader === undefined) return rejectedWith(new TypeError_('Illegal invocation'));
        if (reader.stream === undefined) return rejectedWith(new TypeError_('The reader has been released'));
        return readableStreamCancel(reader.stream, reason);
    }

    read() {
        const reader = rsReaderOf(this);
        if (reader === undefined) return rejectedWith(new TypeError_('Illegal invocation'));
        if (reader.stream === undefined) return rejectedWith(new TypeError_('The reader has been released'));
        const d = newDeferred();
        readerRead(reader, new ReadRequest(
            function (chunk) { resolveDeferred(d, {value: chunk, done: false}); },
            function () { resolveDeferred(d, {value: undefined, done: true}); },
            function (e) { rejectDeferred(d, e); }));
        return d.promise;
    }

    releaseLock() {
        const reader = rsReaderOf(this);
        if (reader === undefined) throw new TypeError_('Illegal invocation');
        if (reader.stream === undefined) return;
        defaultReaderRelease(reader);
    }
}

function acquireDefaultReader(stream) {
    const reader = new RSReaderRecord(false);
    setUpDefaultReader(reader, stream);
    new ReadableStreamDefaultReader(INTERNAL, reader);
    return reader;
}

function setUpDefaultReader(reader, stream) {
    if (isReadableStreamLocked(stream)) throw new TypeError_('The stream is locked');
    readerGenericInitialize(reader, stream);
    reader.readRequests = new List();
}

function readerGenericInitialize(reader, stream) {
    reader.stream = stream;
    stream.reader = reader;
    if (stream.state === 'readable') reader.closed = newDeferred();
    else if (stream.state === 'closed') reader.closed = resolvedDeferred(undefined);
    else reader.closed = rejectedDeferred(stream.storedError);
}

function readerGenericRelease(reader) {
    const stream = reader.stream;
    const error = new TypeError_('The reader has been released');
    if (stream.state === 'readable') {
        rejectDeferred(reader.closed, error);
    } else {
        reader.closed = newDeferred();
        rejectDeferred(reader.closed, error);
    }
    setHandled(reader.closed.promise);
    // [[ReleaseSteps]]: a default controller's do nothing.
    if (stream.controller.byte) rbsControllerReleaseSteps(stream.controller);
    stream.reader = undefined;
    reader.stream = undefined;
}

function defaultReaderRelease(reader) {
    readerGenericRelease(reader);
    defaultReaderErrorReadRequests(reader, new TypeError_('The reader has been released'));
}

function defaultReaderErrorReadRequests(reader, e) {
    const requests = reader.readRequests;
    reader.readRequests = new List();
    while (requests.length > 0) listShift(requests).errorSteps(e);
}

function readerRead(reader, readRequest) {
    const stream = reader.stream;
    stream.disturbed = true;
    if (stream.state === 'closed') readRequest.closeSteps();
    else if (stream.state === 'errored') readRequest.errorSteps(stream.storedError);
    else if (stream.controller.byte) rbsControllerPullSteps(stream.controller, readRequest);
    else controllerPullSteps(stream.controller, readRequest);
}

// ── ReadableStream's abstract operations (§ 4.9) ─────────────────────────
function isReadableStreamLocked(stream) { return stream.reader !== undefined; }

function initializeReadableStream(stream) {
    stream.state = 'readable';
    stream.reader = undefined;
    stream.storedError = undefined;
    stream.disturbed = false;
}

function createReadableStream(startAlgorithm, pullAlgorithm, cancelAlgorithm, highWaterMark, sizeAlgorithm) {
    if (highWaterMark === undefined) highWaterMark = 1;
    if (sizeAlgorithm === undefined) sizeAlgorithm = sizeOne;
    const stream = new RSRecord();
    initializeReadableStream(stream);
    new ReadableStream(INTERNAL, stream);
    const controller = new RSControllerRecord();
    setUpRSDefaultController(stream, controller, startAlgorithm, pullAlgorithm, cancelAlgorithm,
        highWaterMark, sizeAlgorithm);
    return stream;
}

function readableStreamCancel(stream, reason) {
    stream.disturbed = true;
    if (stream.state === 'closed') return resolvedWith(undefined);
    if (stream.state === 'errored') return rejectedWith(stream.storedError);
    readableStreamClose(stream);
    const reader = stream.reader;
    if (reader !== undefined && reader.byob) {
        const requests = reader.readIntoRequests;
        reader.readIntoRequests = new List();
        while (requests.length > 0) listShift(requests).closeSteps(undefined);
    }
    const c = stream.controller;
    const sourceCancelPromise = c.byte ? rbsControllerCancelSteps(c, reason) : controllerCancelSteps(c, reason);
    return transformPromise(sourceCancelPromise, noop, undefined);
}

// A BYOB reader's read-into requests are not ended here: the byte
// controller answers them after a close (its source's respond of 0), and
// a cancel, an error or the reader's release ends them.
function readableStreamClose(stream) {
    stream.state = 'closed';
    const reader = stream.reader;
    if (reader === undefined) return;
    resolveDeferred(reader.closed, undefined);
    if (reader.byob) return;
    const requests = reader.readRequests;
    reader.readRequests = new List();
    while (requests.length > 0) listShift(requests).closeSteps();
}

function readableStreamError(stream, e) {
    stream.state = 'errored';
    stream.storedError = e;
    const reader = stream.reader;
    if (reader === undefined) return;
    rejectDeferred(reader.closed, e);
    setHandled(reader.closed.promise);
    if (reader.byob) byobReaderErrorReadIntoRequests(reader, e);
    else defaultReaderErrorReadRequests(reader, e);
}

function fulfillReadRequest(stream, chunk, done) {
    const readRequest = listShift(stream.reader.readRequests);
    if (done) readRequest.closeSteps();
    else readRequest.chunkSteps(chunk);
}

function numReadRequests(stream) { return stream.reader.readRequests.length; }

// ── The default controller (§ 4.6, § 4.9.4) ──────────────────────────────
class ReadableStreamDefaultController {
    #r;
    static {
        rsControllerOf = function (o) { return isObject(o) && #r in o ? o.#r : undefined; };
    }

    constructor(key = undefined, record = undefined) {
        if (key !== INTERNAL) throw new TypeError_('Illegal constructor');
        this.#r = record;
        record.pub = this;
    }

    get desiredSize() {
        const c = rsControllerOf(this);
        if (c === undefined) throw new TypeError_('Illegal invocation');
        return controllerGetDesiredSize(c);
    }

    close() {
        const c = rsControllerOf(this);
        if (c === undefined) throw new TypeError_('Illegal invocation');
        if (!controllerCanCloseOrEnqueue(c)) throw new TypeError_('The stream is not in a state that permits close');
        controllerClose(c);
    }

    enqueue(chunk = undefined) {
        const c = rsControllerOf(this);
        if (c === undefined) throw new TypeError_('Illegal invocation');
        if (!controllerCanCloseOrEnqueue(c)) throw new TypeError_('The stream is not in a state that permits enqueue');
        controllerEnqueue(c, chunk);
    }

    error(e = undefined) {
        const c = rsControllerOf(this);
        if (c === undefined) throw new TypeError_('Illegal invocation');
        controllerError(c, e);
    }
}

function controllerCancelSteps(c, reason) {
    resetQueue(c);
    const result = c.cancelAlgorithm(reason);
    controllerClearAlgorithms(c);
    return result;
}

function controllerPullSteps(c, readRequest) {
    const stream = c.stream;
    if (c.queue.length > 0) {
        const chunk = dequeueValue(c);
        if (c.closeRequested && c.queue.length === 0) {
            controllerClearAlgorithms(c);
            readableStreamClose(stream);
        } else {
            controllerCallPullIfNeeded(c);
        }
        readRequest.chunkSteps(chunk);
    } else {
        listPush(stream.reader.readRequests, readRequest);
        controllerCallPullIfNeeded(c);
    }
}

function controllerCallPullIfNeeded(c) {
    if (!controllerShouldCallPull(c)) return;
    if (c.pulling) {
        c.pullAgain = true;
        return;
    }
    c.pulling = true;
    const pullPromise = c.pullAlgorithm();
    uponPromise(pullPromise, function () {
        c.pulling = false;
        if (c.pullAgain) {
            c.pullAgain = false;
            controllerCallPullIfNeeded(c);
        }
    }, function (e) {
        controllerError(c, e);
    });
}

function controllerShouldCallPull(c) {
    const stream = c.stream;
    if (!controllerCanCloseOrEnqueue(c)) return false;
    if (!c.started) return false;
    if (isReadableStreamLocked(stream) && numReadRequests(stream) > 0) return true;
    return controllerGetDesiredSize(c) > 0;
}

function controllerClearAlgorithms(c) {
    c.pullAlgorithm = undefined;
    c.cancelAlgorithm = undefined;
    c.strategySizeAlgorithm = undefined;
}

function controllerClose(c) {
    if (!controllerCanCloseOrEnqueue(c)) return;
    c.closeRequested = true;
    if (c.queue.length === 0) {
        controllerClearAlgorithms(c);
        readableStreamClose(c.stream);
    }
}

function controllerEnqueue(c, chunk) {
    if (!controllerCanCloseOrEnqueue(c)) return;
    const stream = c.stream;
    if (isReadableStreamLocked(stream) && numReadRequests(stream) > 0) {
        fulfillReadRequest(stream, chunk, false);
    } else {
        let chunkSize;
        try {
            chunkSize = c.strategySizeAlgorithm(chunk);
        } catch (e) {
            controllerError(c, e);
            throw e;
        }
        try {
            enqueueValueWithSize(c, chunk, chunkSize);
        } catch (e) {
            controllerError(c, e);
            throw e;
        }
    }
    controllerCallPullIfNeeded(c);
}

function controllerError(c, e) {
    const stream = c.stream;
    if (stream.state !== 'readable') return;
    resetQueue(c);
    controllerClearAlgorithms(c);
    readableStreamError(stream, e);
}

function controllerGetDesiredSize(c) {
    const state = c.stream.state;
    if (state === 'errored') return null;
    if (state === 'closed') return 0;
    return c.strategyHWM - c.queueTotalSize;
}

function controllerHasBackpressure(c) { return !controllerShouldCallPull(c); }

function controllerCanCloseOrEnqueue(c) { return !c.closeRequested && c.stream.state === 'readable'; }

function setUpRSDefaultController(stream, c, startAlgorithm, pullAlgorithm, cancelAlgorithm, hwm, sizeAlgorithm) {
    c.stream = stream;
    resetQueue(c);
    c.started = false;
    c.closeRequested = false;
    c.pullAgain = false;
    c.pulling = false;
    c.strategySizeAlgorithm = sizeAlgorithm;
    c.strategyHWM = hwm;
    c.pullAlgorithm = pullAlgorithm;
    c.cancelAlgorithm = cancelAlgorithm;
    stream.controller = c;
    if (c.pub === undefined) new ReadableStreamDefaultController(INTERNAL, c);
    const startResult = startAlgorithm();
    uponPromise(resolvedWith(startResult), function () {
        c.started = true;
        controllerCallPullIfNeeded(c);
    }, function (r) {
        controllerError(c, r);
    });
}

function setUpRSDefaultControllerFromSource(stream, source, start, pull, cancel, hwm, sizeAlgorithm) {
    const c = new RSControllerRecord();
    new ReadableStreamDefaultController(INTERNAL, c);
    const startAlgorithm = start === undefined ? noop : function () { return apply(start, source, [c.pub]); };
    const pullAlgorithm = pull === undefined ? resolvedUndefined
        : function () { return promiseCall(pull, source, [c.pub]); };
    const cancelAlgorithm = cancel === undefined ? resolvedUndefined
        : function (reason) { return promiseCall(cancel, source, [reason]); };
    setUpRSDefaultController(stream, c, startAlgorithm, pullAlgorithm, cancelAlgorithm, hwm, sizeAlgorithm);
}

function resolvedUndefined() { return resolvedWith(undefined); }

// ── tee (§ 4.9.1 ReadableStreamTee, ReadableStreamDefaultTee) ───────────
class TeeBranches {
    branch1;
    branch2;
    constructor(branch1, branch2) {
        this.branch1 = branch1;
        this.branch2 = branch2;
    }
}

function readableStreamTee(stream, cloneForBranch2) {
    if (stream.controller.byte) return readableByteStreamTee(stream);
    return readableStreamDefaultTee(stream, cloneForBranch2);
}

function readableStreamDefaultTee(stream, cloneForBranch2) {
    const reader = acquireDefaultReader(stream);
    let reading = false;
    let readAgain = false;
    let canceled1 = false;
    let canceled2 = false;
    let reason1, reason2, branch1, branch2;
    const cancelDeferred = newDeferred();

    function pullAlgorithm() {
        if (reading) {
            readAgain = true;
            return resolvedWith(undefined);
        }
        reading = true;
        readerRead(reader, new ReadRequest(function (chunk) {
            apply(queueMicrotask_, undefined, [function () {
                readAgain = false;
                const chunk1 = chunk;
                let chunk2 = chunk;
                if (!canceled2 && cloneForBranch2) {
                    try {
                        chunk2 = cloneChunk(chunk2);
                    } catch (e) {
                        controllerError(branch1.controller, e);
                        controllerError(branch2.controller, e);
                        resolveWithPromise(cancelDeferred, readableStreamCancel(stream, e));
                        return;
                    }
                }
                if (!canceled1) controllerEnqueue(branch1.controller, chunk1);
                if (!canceled2) controllerEnqueue(branch2.controller, chunk2);
                reading = false;
                if (readAgain) pullAlgorithm();
            }]);
        }, function () {
            reading = false;
            if (!canceled1) controllerClose(branch1.controller);
            if (!canceled2) controllerClose(branch2.controller);
            if (!canceled1 || !canceled2) resolveDeferred(cancelDeferred, undefined);
        }, function () {
            reading = false;
        }));
        return resolvedWith(undefined);
    }

    function cancel1Algorithm(reason) {
        canceled1 = true;
        reason1 = reason;
        if (canceled2) resolveWithPromise(cancelDeferred, readableStreamCancel(stream, [reason1, reason2]));
        return cancelDeferred.promise;
    }

    function cancel2Algorithm(reason) {
        canceled2 = true;
        reason2 = reason;
        if (canceled1) resolveWithPromise(cancelDeferred, readableStreamCancel(stream, [reason1, reason2]));
        return cancelDeferred.promise;
    }

    branch1 = createReadableStream(noop, pullAlgorithm, cancel1Algorithm);
    branch2 = createReadableStream(noop, pullAlgorithm, cancel2Algorithm);
    uponRejection(reader.closed.promise, function (r) {
        controllerError(branch1.controller, r);
        controllerError(branch2.controller, r);
        if (!canceled1 || !canceled2) resolveDeferred(cancelDeferred, undefined);
    });
    return new TeeBranches(branch1, branch2);
}

// StructuredClone, the window's own (the clone row's structuredClone).
function cloneChunk(chunk) { return apply(structuredClone_, undefined, [chunk]); }

// ── Views and buffers (WebIDL's ArrayBufferView; the standard's § 8.3) ───
// A view's internal slots, read through the captured getters. `kind` is
// its typedArrays entry, undefined for a DataView.
class ViewFacts {
    view;
    buffer;
    byteOffset;
    byteLength;
    length;
    kind;
    constructor(view, buffer, byteOffset, byteLength, length, kind) {
        this.view = view;
        this.buffer = buffer;
        this.byteOffset = byteOffset;
        this.byteLength = byteLength;
        this.length = length;
        this.kind = kind;
    }
}

// The facts of a typed array or DataView, undefined for anything else. A
// DataView over a detached buffer has no readable length (its getters
// throw), and is described as empty, as a typed array over one reads. A
// page's view reaches here only through convertView, and each public
// method that takes one checks its buffer for detachment as the standard
// orders; a view streams.js made itself is read while it alone holds it.
function viewFacts(v) {
    if (!isObject(v)) return undefined;
    const tag = apply(typedArrayTag, v, []);
    if (tag !== undefined) {
        return new ViewFacts(v, apply(typedArrayBuffer, v, []), apply(typedArrayByteOffset, v, []),
            apply(typedArrayByteLength, v, []), apply(typedArrayLength, v, []), typedArrays[tag]);
    }
    let buffer;
    try {
        buffer = apply(dataViewBuffer, v, []);
    } catch (e) {
        return undefined;
    }
    if (isDetachedBuffer(buffer)) return new ViewFacts(v, buffer, 0, 0, 0, undefined);
    const byteLength = apply(dataViewByteLength, v, []);
    return new ViewFacts(v, buffer, apply(dataViewByteOffset, v, []), byteLength, byteLength, undefined);
}

// WebIDL's conversion to ArrayBufferView, which allows neither a
// SharedArrayBuffer nor a resizable buffer behind the view.
function convertView(v, what) {
    const facts = viewFacts(v);
    if (facts === undefined) throw new TypeError_(what + ' is not an ArrayBufferView');
    if (isSharedBuffer(facts.buffer)) throw new TypeError_(what + ' is over a SharedArrayBuffer');
    if (apply(arrayBufferResizable, facts.buffer, [])) throw new TypeError_(what + ' is over a resizable buffer');
    return facts;
}

// A view's buffer is an ArrayBuffer or a SharedArrayBuffer, and only the
// first passes ArrayBuffer's own byteLength getter.
function isSharedBuffer(buffer) {
    try {
        apply(arrayBufferByteLength, buffer, []);
        return false;
    } catch (e) {
        return true;
    }
}

function isDetachedBuffer(buffer) { return apply(arrayBufferDetached, buffer, []); }

function bufferByteLength(buffer) { return apply(arrayBufferByteLength, buffer, []); }

// The standard's TransferArrayBuffer: the bytes move to a new buffer and
// the old one is detached.
function transferArrayBuffer(buffer) { return apply(arrayBufferTransfer, buffer, []); }

function copyBytes(to, toOffset, from, fromOffset, n) {
    apply(uint8Set, new Uint8Array_(to, toOffset, n), [new Uint8Array_(from, fromOffset, n), 0]);
}

function cloneArrayBuffer(from, fromOffset, n) {
    const to = new ArrayBuffer_(n);
    copyBytes(to, 0, from, fromOffset, n);
    return to;
}

function cloneAsUint8Array(facts) {
    return new Uint8Array_(cloneArrayBuffer(facts.buffer, facts.byteOffset, facts.byteLength));
}

// ── The BYOB reader (§ 4.4) ──────────────────────────────────────────────
let byobReaderOf;

class ReadableStreamBYOBReader {
    #r;
    static {
        byobReaderOf = function (o) { return isObject(o) && #r in o ? o.#r : undefined; };
    }

    constructor(stream, record = undefined) {
        if (stream === INTERNAL) {
            this.#r = record;
            record.pub = this;
            return;
        }
        const s = rsOf(stream);
        if (s === undefined) throw new TypeError_('The argument is not a ReadableStream');
        const reader = new RSReaderRecord(true);
        this.#r = reader;
        reader.pub = this;
        setUpBYOBReader(reader, s);
    }

    get closed() {
        const reader = byobReaderOf(this);
        if (reader === undefined) return rejectedWith(new TypeError_('Illegal invocation'));
        return reader.closed.promise;
    }

    cancel(reason = undefined) {
        const reader = byobReaderOf(this);
        if (reader === undefined) return rejectedWith(new TypeError_('Illegal invocation'));
        if (reader.stream === undefined) return rejectedWith(new TypeError_('The reader has been released'));
        return readableStreamCancel(reader.stream, reason);
    }

    read(view, options = undefined) {
        const reader = byobReaderOf(this);
        if (reader === undefined) return rejectedWith(new TypeError_('Illegal invocation'));
        let facts, min;
        try {
            facts = convertView(view, 'The view');
            const source = dictionarySource(options, 'The options');
            const minValue = member(source, 'min');
            min = minValue === undefined ? 1 : enforceRangeU64(minValue, 'min');
        } catch (e) {
            return rejectedWith(e);
        }
        if (facts.byteLength === 0) return rejectedWith(new TypeError_('The view is empty'));
        if (bufferByteLength(facts.buffer) === 0) return rejectedWith(new TypeError_('The view\'s buffer is empty'));
        if (isDetachedBuffer(facts.buffer)) return rejectedWith(new TypeError_('The view\'s buffer is detached'));
        if (min === 0) return rejectedWith(new TypeError_('min is 0'));
        if (min > facts.length) return rejectedWith(new RangeError_('min is past the view\'s length'));
        if (reader.stream === undefined) return rejectedWith(new TypeError_('The reader has been released'));
        const d = newDeferred();
        byobReaderRead(reader, facts, min, new ReadRequest(
            function (chunk) { resolveDeferred(d, {value: chunk, done: false}); },
            function (chunk) { resolveDeferred(d, {value: chunk, done: true}); },
            function (e) { rejectDeferred(d, e); }));
        return d.promise;
    }

    releaseLock() {
        const reader = byobReaderOf(this);
        if (reader === undefined) throw new TypeError_('Illegal invocation');
        if (reader.stream === undefined) return;
        byobReaderRelease(reader);
    }
}

function acquireBYOBReader(stream) {
    const reader = new RSReaderRecord(true);
    setUpBYOBReader(reader, stream);
    new ReadableStreamBYOBReader(INTERNAL, reader);
    return reader;
}

function setUpBYOBReader(reader, stream) {
    if (isReadableStreamLocked(stream)) throw new TypeError_('The stream is locked');
    if (!stream.controller.byte) throw new TypeError_('The stream is not a byte stream');
    readerGenericInitialize(reader, stream);
    reader.readIntoRequests = new List();
}

function byobReaderRead(reader, facts, min, readIntoRequest) {
    const stream = reader.stream;
    stream.disturbed = true;
    if (stream.state === 'errored') readIntoRequest.errorSteps(stream.storedError);
    else rbsControllerPullInto(stream.controller, facts, min, readIntoRequest);
}

function byobReaderRelease(reader) {
    readerGenericRelease(reader);
    byobReaderErrorReadIntoRequests(reader, new TypeError_('The reader has been released'));
}

function byobReaderErrorReadIntoRequests(reader, e) {
    const requests = reader.readIntoRequests;
    reader.readIntoRequests = new List();
    while (requests.length > 0) listShift(requests).errorSteps(e);
}

function hasDefaultReader(stream) { return stream.reader !== undefined && !stream.reader.byob; }

function hasBYOBReader(stream) { return stream.reader !== undefined && stream.reader.byob; }

function numReadIntoRequests(stream) { return stream.reader.readIntoRequests.length; }

function fulfillReadIntoRequest(stream, chunk, done) {
    const readIntoRequest = listShift(stream.reader.readIntoRequests);
    if (done) readIntoRequest.closeSteps(chunk);
    else readIntoRequest.chunkSteps(chunk);
}

// ── The byte controller (§ 4.7) and its BYOB request (§ 4.8) ─────────────
let rbsControllerOf, byobRequestOf;

class ReadableByteStreamController {
    #r;
    static {
        rbsControllerOf = function (o) { return isObject(o) && #r in o ? o.#r : undefined; };
    }

    constructor(key = undefined, record = undefined) {
        if (key !== INTERNAL) throw new TypeError_('Illegal constructor');
        this.#r = record;
        record.pub = this;
    }

    get byobRequest() {
        const c = rbsControllerOf(this);
        if (c === undefined) throw new TypeError_('Illegal invocation');
        const request = rbsControllerGetBYOBRequest(c);
        return request === null ? null : request.pub;
    }

    get desiredSize() {
        const c = rbsControllerOf(this);
        if (c === undefined) throw new TypeError_('Illegal invocation');
        return rbsControllerGetDesiredSize(c);
    }

    close() {
        const c = rbsControllerOf(this);
        if (c === undefined) throw new TypeError_('Illegal invocation');
        if (c.closeRequested) throw new TypeError_('The stream is already closing');
        if (c.stream.state !== 'readable') throw new TypeError_('The stream is not readable');
        rbsControllerClose(c);
    }

    enqueue(chunk) {
        const c = rbsControllerOf(this);
        if (c === undefined) throw new TypeError_('Illegal invocation');
        const facts = convertView(chunk, 'The chunk');
        if (facts.byteLength === 0) throw new TypeError_('The chunk is empty');
        if (bufferByteLength(facts.buffer) === 0) throw new TypeError_('The chunk\'s buffer is empty');
        if (c.closeRequested) throw new TypeError_('The stream is closing');
        if (c.stream.state !== 'readable') throw new TypeError_('The stream is not readable');
        rbsControllerEnqueue(c, facts);
    }

    error(e = undefined) {
        const c = rbsControllerOf(this);
        if (c === undefined) throw new TypeError_('Illegal invocation');
        rbsControllerError(c, e);
    }
}

class ReadableStreamBYOBRequest {
    #r;
    static {
        byobRequestOf = function (o) { return isObject(o) && #r in o ? o.#r : undefined; };
    }

    constructor(key = undefined, record = undefined) {
        if (key !== INTERNAL) throw new TypeError_('Illegal constructor');
        this.#r = record;
        record.pub = this;
    }

    get view() {
        const request = byobRequestOf(this);
        if (request === undefined) throw new TypeError_('Illegal invocation');
        return request.view;
    }

    respond(bytesWritten) {
        const request = byobRequestOf(this);
        if (request === undefined) throw new TypeError_('Illegal invocation');
        const n = enforceRangeU64(bytesWritten, 'bytesWritten');
        if (request.controller === undefined) throw new TypeError_('The request has been answered');
        if (isDetachedBuffer(apply(typedArrayBuffer, request.view, []))) throw new TypeError_('The view is detached');
        rbsControllerRespond(request.controller, n);
    }

    respondWithNewView(view) {
        const request = byobRequestOf(this);
        if (request === undefined) throw new TypeError_('Illegal invocation');
        const facts = convertView(view, 'The view');
        if (request.controller === undefined) throw new TypeError_('The request has been answered');
        if (isDetachedBuffer(facts.buffer)) throw new TypeError_('The view is detached');
        rbsControllerRespondWithNewView(request.controller, facts);
    }
}

// [[CancelSteps]], [[PullSteps]] and [[ReleaseSteps]].
function rbsControllerCancelSteps(c, reason) {
    rbsControllerClearPendingPullIntos(c);
    rbsResetQueue(c);
    const result = c.cancelAlgorithm(reason);
    rbsControllerClearAlgorithms(c);
    return result;
}

function rbsControllerPullSteps(c, readRequest) {
    const stream = c.stream;
    if (c.queueTotalSize > 0) {
        rbsControllerFillReadRequestFromQueue(c, readRequest);
        return;
    }
    const autoAllocate = c.autoAllocateChunkSize;
    if (autoAllocate !== undefined) {
        let buffer;
        try {
            buffer = new ArrayBuffer_(autoAllocate);
        } catch (e) {
            readRequest.errorSteps(e);
            return;
        }
        listPush(c.pendingPullIntos,
            new PullIntoDescriptor(buffer, autoAllocate, 0, autoAllocate, 1, 1, Uint8Array_, 'default'));
    }
    listPush(stream.reader.readRequests, readRequest);
    rbsControllerCallPullIfNeeded(c);
}

function rbsControllerReleaseSteps(c) {
    if (c.pendingPullIntos.length === 0) return;
    const first = listPeek(c.pendingPullIntos);
    first.readerType = 'none';
    c.pendingPullIntos = new List();
    listPush(c.pendingPullIntos, first);
}

function rbsResetQueue(c) {
    c.queue = new List();
    c.queueTotalSize = 0;
}

function rbsControllerCallPullIfNeeded(c) {
    if (!rbsControllerShouldCallPull(c)) return;
    if (c.pulling) {
        c.pullAgain = true;
        return;
    }
    c.pulling = true;
    const pullPromise = c.pullAlgorithm();
    uponPromise(pullPromise, function () {
        c.pulling = false;
        if (c.pullAgain) {
            c.pullAgain = false;
            rbsControllerCallPullIfNeeded(c);
        }
    }, function (e) {
        rbsControllerError(c, e);
    });
}

function rbsControllerClearAlgorithms(c) {
    c.pullAlgorithm = undefined;
    c.cancelAlgorithm = undefined;
}

function rbsControllerClearPendingPullIntos(c) {
    rbsControllerInvalidateBYOBRequest(c);
    c.pendingPullIntos = new List();
}

function rbsControllerClose(c) {
    const stream = c.stream;
    if (c.closeRequested || stream.state !== 'readable') return;
    if (c.queueTotalSize > 0) {
        c.closeRequested = true;
        return;
    }
    if (c.pendingPullIntos.length > 0) {
        const first = listPeek(c.pendingPullIntos);
        if (first.bytesFilled % first.elementSize !== 0) {
            const e = new TypeError_('The stream closed inside an element of a pending read');
            rbsControllerError(c, e);
            throw e;
        }
    }
    rbsControllerClearAlgorithms(c);
    readableStreamClose(stream);
}

function rbsControllerCommitPullIntoDescriptor(stream, d) {
    let done = false;
    if (stream.state === 'closed') done = true;
    const filledView = rbsControllerConvertPullIntoDescriptor(d);
    if (d.readerType === 'default') fulfillReadRequest(stream, filledView, done);
    else fulfillReadIntoRequest(stream, filledView, done);
}

function rbsControllerConvertPullIntoDescriptor(d) {
    const buffer = transferArrayBuffer(d.buffer);
    return new d.viewConstructor(buffer, d.byteOffset, d.bytesFilled / d.elementSize);
}

function rbsControllerEnqueue(c, facts) {
    const stream = c.stream;
    if (c.closeRequested || stream.state !== 'readable') return;
    const buffer = facts.buffer;
    const byteOffset = facts.byteOffset;
    const byteLength = facts.byteLength;
    if (isDetachedBuffer(buffer)) throw new TypeError_('The chunk\'s buffer is detached');
    const transferredBuffer = transferArrayBuffer(buffer);
    if (c.pendingPullIntos.length > 0) {
        const first = listPeek(c.pendingPullIntos);
        if (isDetachedBuffer(first.buffer)) throw new TypeError_('The pending read\'s buffer is detached');
        rbsControllerInvalidateBYOBRequest(c);
        first.buffer = transferArrayBuffer(first.buffer);
        if (first.readerType === 'none') rbsControllerEnqueueDetachedPullIntoToQueue(c, first);
    }
    if (hasDefaultReader(stream)) {
        rbsControllerProcessReadRequestsUsingQueue(c);
        if (numReadRequests(stream) === 0) {
            rbsControllerEnqueueChunkToQueue(c, transferredBuffer, byteOffset, byteLength);
        } else {
            if (c.pendingPullIntos.length > 0) rbsControllerShiftPendingPullInto(c);
            fulfillReadRequest(stream, new Uint8Array_(transferredBuffer, byteOffset, byteLength), false);
        }
    } else if (hasBYOBReader(stream)) {
        rbsControllerEnqueueChunkToQueue(c, transferredBuffer, byteOffset, byteLength);
        rbsControllerCommitAll(stream, rbsControllerProcessPullIntoDescriptorsUsingQueue(c));
    } else {
        rbsControllerEnqueueChunkToQueue(c, transferredBuffer, byteOffset, byteLength);
    }
    rbsControllerCallPullIfNeeded(c);
}

function rbsControllerCommitAll(stream, filled) {
    while (filled.length > 0) rbsControllerCommitPullIntoDescriptor(stream, listShift(filled));
}

function rbsControllerEnqueueChunkToQueue(c, buffer, byteOffset, byteLength) {
    listPush(c.queue, new ByteQueueEntry(buffer, byteOffset, byteLength));
    c.queueTotalSize += byteLength;
}

// A copy that fails errors the stream, and the caller hears the failure.
function rbsControllerEnqueueClonedChunkToQueue(c, buffer, byteOffset, byteLength) {
    let clone;
    try {
        clone = cloneArrayBuffer(buffer, byteOffset, byteLength);
    } catch (e) {
        rbsControllerError(c, e);
        throw e;
    }
    rbsControllerEnqueueChunkToQueue(c, clone, 0, byteLength);
}

function rbsControllerEnqueueDetachedPullIntoToQueue(c, d) {
    if (d.bytesFilled > 0) rbsControllerEnqueueClonedChunkToQueue(c, d.buffer, d.byteOffset, d.bytesFilled);
    rbsControllerShiftPendingPullInto(c);
}

function rbsControllerError(c, e) {
    const stream = c.stream;
    if (stream.state !== 'readable') return;
    rbsControllerClearPendingPullIntos(c);
    rbsResetQueue(c);
    rbsControllerClearAlgorithms(c);
    readableStreamError(stream, e);
}

function rbsControllerFillHeadPullIntoDescriptor(c, size, d) {
    d.bytesFilled += size;
}

function rbsControllerFillPullIntoDescriptorFromQueue(c, d) {
    const maxBytesToCopy = c.queueTotalSize < d.byteLength - d.bytesFilled ? c.queueTotalSize
        : d.byteLength - d.bytesFilled;
    const maxBytesFilled = d.bytesFilled + maxBytesToCopy;
    let totalBytesToCopyRemaining = maxBytesToCopy;
    let ready = false;
    const maxAlignedBytes = maxBytesFilled - maxBytesFilled % d.elementSize;
    if (maxAlignedBytes >= d.minimumFill) {
        totalBytesToCopyRemaining = maxAlignedBytes - d.bytesFilled;
        ready = true;
    }
    const queue = c.queue;
    while (totalBytesToCopyRemaining > 0) {
        const head = listPeek(queue);
        const bytesToCopy = totalBytesToCopyRemaining < head.byteLength ? totalBytesToCopyRemaining : head.byteLength;
        copyBytes(d.buffer, d.byteOffset + d.bytesFilled, head.buffer, head.byteOffset, bytesToCopy);
        if (head.byteLength === bytesToCopy) {
            listShift(queue);
        } else {
            head.byteOffset += bytesToCopy;
            head.byteLength -= bytesToCopy;
        }
        c.queueTotalSize -= bytesToCopy;
        rbsControllerFillHeadPullIntoDescriptor(c, bytesToCopy, d);
        totalBytesToCopyRemaining -= bytesToCopy;
    }
    return ready;
}

function rbsControllerFillReadRequestFromQueue(c, readRequest) {
    const entry = listShift(c.queue);
    c.queueTotalSize -= entry.byteLength;
    rbsControllerHandleQueueDrain(c);
    readRequest.chunkSteps(new Uint8Array_(entry.buffer, entry.byteOffset, entry.byteLength));
}

function rbsControllerGetBYOBRequest(c) {
    if (c.byobRequest === null && c.pendingPullIntos.length > 0) {
        const first = listPeek(c.pendingPullIntos);
        const view = new Uint8Array_(first.buffer, first.byteOffset + first.bytesFilled,
            first.byteLength - first.bytesFilled);
        const request = new BYOBRequestRecord(c, view);
        new ReadableStreamBYOBRequest(INTERNAL, request);
        c.byobRequest = request;
    }
    return c.byobRequest;
}

function rbsControllerGetDesiredSize(c) {
    const state = c.stream.state;
    if (state === 'errored') return null;
    if (state === 'closed') return 0;
    return c.strategyHWM - c.queueTotalSize;
}

function rbsControllerHandleQueueDrain(c) {
    if (c.queueTotalSize === 0 && c.closeRequested) {
        rbsControllerClearAlgorithms(c);
        readableStreamClose(c.stream);
    } else {
        rbsControllerCallPullIfNeeded(c);
    }
}

function rbsControllerInvalidateBYOBRequest(c) {
    const request = c.byobRequest;
    if (request === null) return;
    request.controller = undefined;
    request.view = null;
    c.byobRequest = null;
}

// The reads the queue can fill now, in order, each shifted off the
// pending list; committing them is the caller's.
function rbsControllerProcessPullIntoDescriptorsUsingQueue(c) {
    const filled = new List();
    while (c.pendingPullIntos.length > 0) {
        if (c.queueTotalSize === 0) break;
        const d = listPeek(c.pendingPullIntos);
        if (rbsControllerFillPullIntoDescriptorFromQueue(c, d)) {
            rbsControllerShiftPendingPullInto(c);
            listPush(filled, d);
        }
    }
    return filled;
}

function rbsControllerProcessReadRequestsUsingQueue(c) {
    const reader = c.stream.reader;
    while (reader.readRequests.length > 0) {
        if (c.queueTotalSize === 0) return;
        rbsControllerFillReadRequestFromQueue(c, listShift(reader.readRequests));
    }
}

function rbsControllerPullInto(c, facts, min, readIntoRequest) {
    const stream = c.stream;
    let elementSize = 1;
    let ctor = DataView_;
    if (facts.kind !== undefined) {
        elementSize = facts.kind.size;
        ctor = facts.kind.ctor;
    }
    const minimumFill = min * elementSize;
    let buffer;
    try {
        buffer = transferArrayBuffer(facts.buffer);
    } catch (e) {
        readIntoRequest.errorSteps(e);
        return;
    }
    const d = new PullIntoDescriptor(buffer, bufferByteLength(buffer), facts.byteOffset, facts.byteLength,
        minimumFill, elementSize, ctor, 'byob');
    if (c.pendingPullIntos.length > 0) {
        listPush(c.pendingPullIntos, d);
        listPush(stream.reader.readIntoRequests, readIntoRequest);
        return;
    }
    if (stream.state === 'closed') {
        readIntoRequest.closeSteps(new ctor(d.buffer, d.byteOffset, 0));
        return;
    }
    if (c.queueTotalSize > 0) {
        if (rbsControllerFillPullIntoDescriptorFromQueue(c, d)) {
            const filledView = rbsControllerConvertPullIntoDescriptor(d);
            rbsControllerHandleQueueDrain(c);
            readIntoRequest.chunkSteps(filledView);
            return;
        }
        if (c.closeRequested) {
            const e = new TypeError_('The stream closed inside an element of the read');
            rbsControllerError(c, e);
            readIntoRequest.errorSteps(e);
            return;
        }
    }
    listPush(c.pendingPullIntos, d);
    listPush(stream.reader.readIntoRequests, readIntoRequest);
    rbsControllerCallPullIfNeeded(c);
}

function rbsControllerRespond(c, bytesWritten) {
    const first = listPeek(c.pendingPullIntos);
    if (c.stream.state === 'closed') {
        if (bytesWritten !== 0) throw new TypeError_('A closed stream\'s read is answered with 0 bytes');
    } else {
        if (bytesWritten === 0) throw new TypeError_('A readable stream\'s read is answered with bytes');
        if (first.bytesFilled + bytesWritten > first.byteLength)
            throw new RangeError_('bytesWritten is past the view');
    }
    first.buffer = transferArrayBuffer(first.buffer);
    rbsControllerRespondInternal(c, bytesWritten);
}

function rbsControllerRespondInClosedState(c, first) {
    if (first.readerType === 'none') rbsControllerShiftPendingPullInto(c);
    const stream = c.stream;
    if (hasBYOBReader(stream)) {
        const filled = new List();
        while (filled.length < numReadIntoRequests(stream)) listPush(filled, rbsControllerShiftPendingPullInto(c));
        rbsControllerCommitAll(stream, filled);
    }
}

function rbsControllerRespondInReadableState(c, bytesWritten, d) {
    rbsControllerFillHeadPullIntoDescriptor(c, bytesWritten, d);
    if (d.readerType === 'none') {
        rbsControllerEnqueueDetachedPullIntoToQueue(c, d);
        rbsControllerCommitAll(c.stream, rbsControllerProcessPullIntoDescriptorsUsingQueue(c));
        return;
    }
    if (d.bytesFilled < d.minimumFill) return;
    rbsControllerShiftPendingPullInto(c);
    const remainderSize = d.bytesFilled % d.elementSize;
    if (remainderSize > 0) {
        const end = d.byteOffset + d.bytesFilled;
        rbsControllerEnqueueClonedChunkToQueue(c, d.buffer, end - remainderSize, remainderSize);
    }
    d.bytesFilled -= remainderSize;
    const filled = rbsControllerProcessPullIntoDescriptorsUsingQueue(c);
    rbsControllerCommitPullIntoDescriptor(c.stream, d);
    rbsControllerCommitAll(c.stream, filled);
}

function rbsControllerRespondInternal(c, bytesWritten) {
    const first = listPeek(c.pendingPullIntos);
    rbsControllerInvalidateBYOBRequest(c);
    if (c.stream.state === 'closed') rbsControllerRespondInClosedState(c, first);
    else rbsControllerRespondInReadableState(c, bytesWritten, first);
    rbsControllerCallPullIfNeeded(c);
}

function rbsControllerRespondWithNewView(c, facts) {
    const first = listPeek(c.pendingPullIntos);
    if (c.stream.state === 'closed') {
        if (facts.byteLength !== 0) throw new TypeError_('A closed stream\'s read is answered with an empty view');
    } else if (facts.byteLength === 0) {
        throw new TypeError_('A readable stream\'s read is answered with bytes');
    }
    if (first.byteOffset + first.bytesFilled !== facts.byteOffset)
        throw new RangeError_('The view does not start where the read\'s bytes end');
    if (first.bufferByteLength !== bufferByteLength(facts.buffer))
        throw new RangeError_('The view\'s buffer is not the read\'s length');
    if (first.bytesFilled + facts.byteLength > first.byteLength) throw new RangeError_('The view is past the read');
    const viewByteLength = facts.byteLength;
    first.buffer = transferArrayBuffer(facts.buffer);
    rbsControllerRespondInternal(c, viewByteLength);
}

function rbsControllerShiftPendingPullInto(c) { return listShift(c.pendingPullIntos); }

function rbsControllerShouldCallPull(c) {
    const stream = c.stream;
    if (stream.state !== 'readable') return false;
    if (c.closeRequested) return false;
    if (!c.started) return false;
    if (hasDefaultReader(stream) && numReadRequests(stream) > 0) return true;
    if (hasBYOBReader(stream) && numReadIntoRequests(stream) > 0) return true;
    return rbsControllerGetDesiredSize(c) > 0;
}

function setUpRBSController(stream, c, startAlgorithm, pullAlgorithm, cancelAlgorithm, hwm, autoAllocateChunkSize) {
    c.stream = stream;
    c.pullAgain = false;
    c.pulling = false;
    c.byobRequest = null;
    rbsResetQueue(c);
    c.closeRequested = false;
    c.started = false;
    c.strategyHWM = hwm;
    c.pullAlgorithm = pullAlgorithm;
    c.cancelAlgorithm = cancelAlgorithm;
    c.autoAllocateChunkSize = autoAllocateChunkSize;
    c.pendingPullIntos = new List();
    stream.controller = c;
    if (c.pub === undefined) new ReadableByteStreamController(INTERNAL, c);
    const startResult = startAlgorithm();
    uponPromise(resolvedWith(startResult), function () {
        c.started = true;
        rbsControllerCallPullIfNeeded(c);
    }, function (r) {
        rbsControllerError(c, r);
    });
}

function setUpRBSControllerFromSource(stream, source, start, pull, cancel, autoAllocateChunkSize, hwm) {
    const c = new RBSControllerRecord();
    new ReadableByteStreamController(INTERNAL, c);
    const startAlgorithm = start === undefined ? noop : function () { return apply(start, source, [c.pub]); };
    const pullAlgorithm = pull === undefined ? resolvedUndefined
        : function () { return promiseCall(pull, source, [c.pub]); };
    const cancelAlgorithm = cancel === undefined ? resolvedUndefined
        : function (reason) { return promiseCall(cancel, source, [reason]); };
    if (autoAllocateChunkSize === 0) throw new TypeError_('autoAllocateChunkSize is 0');
    setUpRBSController(stream, c, startAlgorithm, pullAlgorithm, cancelAlgorithm, hwm, autoAllocateChunkSize);
}

function createReadableByteStream(startAlgorithm, pullAlgorithm, cancelAlgorithm) {
    const stream = new RSRecord();
    initializeReadableStream(stream);
    new ReadableStream(INTERNAL, stream);
    setUpRBSController(stream, new RBSControllerRecord(), startAlgorithm, pullAlgorithm, cancelAlgorithm, 0,
        undefined);
    return stream;
}

// The standard's "close" for other specifications: the pending reads a
// byte stream holds are answered (with what they have) by a respond of 0.
function rbsCloseForOthers(c) {
    rbsControllerClose(c);
    if (c.pendingPullIntos.length > 0) rbsControllerRespond(c, 0);
}

// ── tee for a byte stream (§ 4.9.1 ReadableByteStreamTee) ────────────────
// Each branch pulls with a BYOB reader when its own reader brought a
// buffer, and with a default reader otherwise; the one reader of the
// source is swapped between the two kinds as the branches ask.
function readableByteStreamTee(stream) {
    let reader = acquireDefaultReader(stream);
    let reading = false;
    let readAgainForBranch1 = false;
    let readAgainForBranch2 = false;
    let canceled1 = false;
    let canceled2 = false;
    let reason1, reason2, branch1, branch2;
    const cancelDeferred = newDeferred();

    function forwardReaderError(thisReader) {
        uponRejection(thisReader.closed.promise, function (r) {
            if (thisReader !== reader) return;
            rbsControllerError(branch1.controller, r);
            rbsControllerError(branch2.controller, r);
            if (!canceled1 || !canceled2) resolveDeferred(cancelDeferred, undefined);
        });
    }

    function pullAgainIfAsked() {
        if (readAgainForBranch1) pull1Algorithm();
        else if (readAgainForBranch2) pull2Algorithm();
    }

    // A branch's close, which the standard's close steps write as one that
    // cannot fail. It fails when a BYOB read pending on the branch holds
    // part of an element as the source ends; the close has errored the
    // branch by then, which is how its reader hears it, so the throw goes
    // no further and the rest of the close steps (the other branch, the
    // cancel promise) still run.
    function closeBranch(branch) {
        try {
            rbsControllerClose(branch.controller);
        } catch (e) {
            // the branch is errored: nothing more to do for it
        }
    }

    function pullWithDefaultReader() {
        if (reader.byob) {
            byobReaderRelease(reader);
            reader = acquireDefaultReader(stream);
            forwardReaderError(reader);
        }
        readerRead(reader, new ReadRequest(function (chunk) {
            apply(queueMicrotask_, undefined, [function () {
                readAgainForBranch1 = false;
                readAgainForBranch2 = false;
                const chunk1 = chunk;
                let chunk2 = chunk;
                if (!canceled1 && !canceled2) {
                    try {
                        chunk2 = cloneAsUint8Array(viewFacts(chunk));
                    } catch (e) {
                        rbsControllerError(branch1.controller, e);
                        rbsControllerError(branch2.controller, e);
                        resolveWithPromise(cancelDeferred, readableStreamCancel(stream, e));
                        return;
                    }
                }
                if (!canceled1) rbsControllerEnqueue(branch1.controller, viewFacts(chunk1));
                if (!canceled2) rbsControllerEnqueue(branch2.controller, viewFacts(chunk2));
                reading = false;
                pullAgainIfAsked();
            }]);
        }, function () {
            reading = false;
            if (!canceled1) closeBranch(branch1);
            if (!canceled2) closeBranch(branch2);
            if (branch1.controller.pendingPullIntos.length > 0) rbsControllerRespond(branch1.controller, 0);
            if (branch2.controller.pendingPullIntos.length > 0) rbsControllerRespond(branch2.controller, 0);
            if (!canceled1 || !canceled2) resolveDeferred(cancelDeferred, undefined);
        }, function () {
            reading = false;
        }));
    }

    function pullWithBYOBReader(view, forBranch2) {
        if (!reader.byob) {
            defaultReaderRelease(reader);
            reader = acquireBYOBReader(stream);
            forwardReaderError(reader);
        }
        const byobBranch = forBranch2 ? branch2 : branch1;
        const otherBranch = forBranch2 ? branch1 : branch2;
        byobReaderRead(reader, viewFacts(view), 1, new ReadRequest(function (chunk) {
            apply(queueMicrotask_, undefined, [function () {
                readAgainForBranch1 = false;
                readAgainForBranch2 = false;
                const byobCanceled = forBranch2 ? canceled2 : canceled1;
                const otherCanceled = forBranch2 ? canceled1 : canceled2;
                if (!otherCanceled) {
                    let clonedChunk;
                    try {
                        clonedChunk = cloneAsUint8Array(viewFacts(chunk));
                    } catch (e) {
                        rbsControllerError(byobBranch.controller, e);
                        rbsControllerError(otherBranch.controller, e);
                        resolveWithPromise(cancelDeferred, readableStreamCancel(stream, e));
                        return;
                    }
                    if (!byobCanceled) rbsControllerRespondWithNewView(byobBranch.controller, viewFacts(chunk));
                    rbsControllerEnqueue(otherBranch.controller, viewFacts(clonedChunk));
                } else if (!byobCanceled) {
                    rbsControllerRespondWithNewView(byobBranch.controller, viewFacts(chunk));
                }
                reading = false;
                pullAgainIfAsked();
            }]);
        }, function (chunk) {
            reading = false;
            const byobCanceled = forBranch2 ? canceled2 : canceled1;
            const otherCanceled = forBranch2 ? canceled1 : canceled2;
            if (!byobCanceled) closeBranch(byobBranch);
            if (!otherCanceled) closeBranch(otherBranch);
            if (chunk !== undefined) {
                // A branch whose close failed is errored, and has no read
                // left to answer.
                if (!byobCanceled && byobBranch.state !== 'errored')
                    rbsControllerRespondWithNewView(byobBranch.controller, viewFacts(chunk));
                if (!otherCanceled && otherBranch.controller.pendingPullIntos.length > 0)
                    rbsControllerRespond(otherBranch.controller, 0);
            }
            if (!byobCanceled || !otherCanceled) resolveDeferred(cancelDeferred, undefined);
        }, function () {
            reading = false;
        }));
    }

    function pull1Algorithm() {
        if (reading) {
            readAgainForBranch1 = true;
            return resolvedWith(undefined);
        }
        reading = true;
        const byobRequest = rbsControllerGetBYOBRequest(branch1.controller);
        if (byobRequest === null) pullWithDefaultReader();
        else pullWithBYOBReader(byobRequest.view, false);
        return resolvedWith(undefined);
    }

    function pull2Algorithm() {
        if (reading) {
            readAgainForBranch2 = true;
            return resolvedWith(undefined);
        }
        reading = true;
        const byobRequest = rbsControllerGetBYOBRequest(branch2.controller);
        if (byobRequest === null) pullWithDefaultReader();
        else pullWithBYOBReader(byobRequest.view, true);
        return resolvedWith(undefined);
    }

    function cancel1Algorithm(reason) {
        canceled1 = true;
        reason1 = reason;
        if (canceled2) resolveWithPromise(cancelDeferred, readableStreamCancel(stream, [reason1, reason2]));
        return cancelDeferred.promise;
    }

    function cancel2Algorithm(reason) {
        canceled2 = true;
        reason2 = reason;
        if (canceled1) resolveWithPromise(cancelDeferred, readableStreamCancel(stream, [reason1, reason2]));
        return cancelDeferred.promise;
    }

    branch1 = createReadableByteStream(noop, pull1Algorithm, cancel1Algorithm);
    branch2 = createReadableByteStream(noop, pull2Algorithm, cancel2Algorithm);
    forwardReaderError(reader);
    return new TeeBranches(branch1, branch2);
}

// ── pipeTo (§ 4.9.1 ReadableStreamPipeTo) ────────────────────────────────
function readableStreamPipeTo(source, dest, preventClose, preventAbort, preventCancel, signal) {
    const reader = acquireDefaultReader(source);
    const writer = acquireDefaultWriter(dest);
    source.disturbed = true;
    let shuttingDown = false;
    let currentWrite = resolvedWith(undefined);
    const result = newDeferred();
    let abortListener;

    if (signal !== undefined) {
        abortListener = function () {
            const error = apply(abortSignalReason, signal, []);
            const actions = slots();
            let count = 0;
            if (!preventAbort) actions[count++] = function () {
                return dest.state === 'writable' ? writableStreamAbort(dest, error) : resolvedWith(undefined);
            };
            if (!preventCancel) actions[count++] = function () {
                return source.state === 'readable' ? readableStreamCancel(source, error) : resolvedWith(undefined);
            };
            shutdownWithAnAction(function () { return waitForAll(actions, count); }, true, error);
        };
        if (apply(abortSignalAborted, signal, [])) {
            abortListener();
            return result.promise;
        }
        apply(addEventListener, signal, ['abort', abortListener]);
    }

    function waitForAll(actions, count) {
        const all = newDeferred();
        let left = count;
        if (left === 0) {
            resolveDeferred(all, undefined);
            return all.promise;
        }
        for (let i = 0; i < count; i++) {
            uponPromise(actions[i](), function () {
                if (--left === 0) resolveDeferred(all, undefined);
            }, function (r) {
                rejectDeferred(all, r);
            });
        }
        return all.promise;
    }

    function pipeStep() {
        if (shuttingDown) return resolvedWith(true);
        return transformPromise(writer.ready.promise, function () {
            const read = newDeferred();
            // The write waits a microtask: a chunk a source enqueues must
            // not reach the sink's write within the enqueue() call.
            readerRead(reader, new ReadRequest(function (chunk) {
                apply(queueMicrotask_, undefined, [function () {
                    currentWrite = transformPromise(writerWrite(writer, chunk), undefined, noop);
                    resolveDeferred(read, false);
                }]);
            }, function () {
                resolveDeferred(read, true);
            }, function (e) {
                rejectDeferred(read, e);
            }));
            return read.promise;
        }, undefined);
    }

    function pipeLoop() {
        const loop = newDeferred();
        function next(done) {
            if (done) resolveDeferred(loop, undefined);
            else uponPromise(pipeStep(), next, function (r) { rejectDeferred(loop, r); });
        }
        next(false);
        return loop.promise;
    }

    function isOrBecomesErrored(stream, promise, action) {
        if (stream.state === 'errored') action(stream.storedError);
        else uponRejection(promise, action);
    }

    function isOrBecomesClosed(stream, promise, action) {
        if (stream.state === 'closed') action();
        else uponFulfillment(promise, action);
    }

    function waitForWritesToFinish() {
        const oldCurrentWrite = currentWrite;
        return transformPromise(currentWrite, function () {
            return oldCurrentWrite !== currentWrite ? waitForWritesToFinish() : undefined;
        }, undefined);
    }

    function shutdownWithAnAction(action, originalIsError, originalError) {
        if (shuttingDown) return;
        shuttingDown = true;
        function doTheRest() {
            uponPromise(action(), function () {
                finalize(originalIsError, originalError);
            }, function (newError) {
                finalize(true, newError);
            });
        }
        if (dest.state === 'writable' && !writableStreamCloseQueuedOrInFlight(dest))
            uponFulfillment(waitForWritesToFinish(), doTheRest);
        else doTheRest();
    }

    function shutdown(isError, error) {
        if (shuttingDown) return;
        shuttingDown = true;
        if (dest.state === 'writable' && !writableStreamCloseQueuedOrInFlight(dest))
            uponFulfillment(waitForWritesToFinish(), function () { finalize(isError, error); });
        else finalize(isError, error);
    }

    function finalize(isError, error) {
        writerRelease(writer);
        defaultReaderRelease(reader);
        if (signal !== undefined) apply(removeEventListener, signal, ['abort', abortListener]);
        if (isError) rejectDeferred(result, error);
        else resolveDeferred(result, undefined);
    }

    // Errors must be propagated forward.
    isOrBecomesErrored(source, reader.closed.promise, function (storedError) {
        if (!preventAbort) shutdownWithAnAction(function () { return writableStreamAbort(dest, storedError); },
            true, storedError);
        else shutdown(true, storedError);
    });
    // Errors must be propagated backward.
    isOrBecomesErrored(dest, writer.closed.promise, function (storedError) {
        if (!preventCancel) shutdownWithAnAction(function () { return readableStreamCancel(source, storedError); },
            true, storedError);
        else shutdown(true, storedError);
    });
    // Closing must be propagated forward.
    isOrBecomesClosed(source, reader.closed.promise, function () {
        if (!preventClose) shutdownWithAnAction(function () { return writerCloseWithErrorPropagation(writer); });
        else shutdown();
    });
    // Closing must be propagated backward.
    if (writableStreamCloseQueuedOrInFlight(dest) || dest.state === 'closed') {
        const destClosed = new TypeError_('The destination writable stream closed before all data was piped to it');
        if (!preventCancel) shutdownWithAnAction(function () { return readableStreamCancel(source, destClosed); },
            true, destClosed);
        else shutdown(true, destClosed);
    }
    setHandled(pipeLoop());
    return result.promise;
}

// ── Async iteration (§ 4.2.5, WebIDL § 3.7.10.2) ─────────────────────────
class AsyncIteratorRecord {
    reader;
    preventCancel;
    ongoing;
    finished;
    constructor(reader, preventCancel) {
        this.reader = reader;
        this.preventCancel = preventCancel;
        this.ongoing = undefined;
        this.finished = false;
    }
}

let asyncIteratorOf;

// The iterator is an ordinary object over the iterator prototype below; a
// constructor that returns its argument lets a subclass put the private
// field on that object, which is the brand check next() and return() make.
class ReturnOverride {
    constructor(o) { return o; }
}

class AsyncIteratorStamp extends ReturnOverride {
    #r;
    static {
        asyncIteratorOf = function (o) { return isObject(o) && #r in o ? o.#r : undefined; };
    }

    constructor(o, record) {
        super(o);
        this.#r = record;
    }
}

function newAsyncIterator(record) {
    const o = objectCreate(asyncIteratorPrototype);
    new AsyncIteratorStamp(o, record);
    return o;
}

function iterResult(value, done) { return {value: value, done: done}; }

function asyncIteratorNext() {
    const it = asyncIteratorOf(this);
    if (it === undefined) return rejectedWith(new TypeError_('Illegal invocation'));
    function nextSteps() {
        if (it.finished) return resolvedWith(iterResult(undefined, true));
        const reader = it.reader;
        const d = newDeferred();
        // The chunk travels boxed: a deferred resolved with a thenable
        // adopts it, and a chunk is the page's value, Promise or not. The
        // box has no prototype, so no `then` a page put anywhere is found.
        readerRead(reader, new ReadRequest(function (chunk) {
            const box = objectCreate(null);
            box.value = chunk;
            resolveDeferred(d, box);
        }, function () {
            defaultReaderRelease(reader);
            resolveDeferred(d, endOfIteration);
        }, function (e) {
            defaultReaderRelease(reader);
            rejectDeferred(d, e);
        }));
        return transformPromise(d.promise, function (next) {
            it.ongoing = undefined;
            if (next === endOfIteration) {
                it.finished = true;
                return iterResult(undefined, true);
            }
            return iterResult(next.value, false);
        }, function (reason) {
            it.ongoing = undefined;
            it.finished = true;
            throw reason;
        });
    }
    it.ongoing = it.ongoing !== undefined ? transformPromise(it.ongoing, nextSteps, nextSteps) : nextSteps();
    return it.ongoing;
}

function asyncIteratorReturn(value) {
    const it = asyncIteratorOf(this);
    if (it === undefined) return rejectedWith(new TypeError_('Illegal invocation'));
    function returnSteps() {
        if (it.finished) return resolvedWith(iterResult(value, true));
        it.finished = true;
        const reader = it.reader;
        if (!it.preventCancel) {
            const result = readableStreamCancel(reader.stream, value);
            defaultReaderRelease(reader);
            return result;
        }
        defaultReaderRelease(reader);
        return resolvedWith(undefined);
    }
    it.ongoing = it.ongoing !== undefined ? transformPromise(it.ongoing, returnSteps, returnSteps) : returnSteps();
    return transformPromise(it.ongoing, function () { return iterResult(value, true); }, undefined);
}

// ── ReadableStream.from (§ 4.9.1 ReadableStreamFromIterable) ─────────────
function getAsyncIterator(obj) {
    let method = obj == null ? undefined : obj[asyncIteratorSymbol];
    if (method !== undefined && method !== null) {
        if (typeof method !== 'function') throw new TypeError_('The async iterator method is not callable');
        const iterator = apply(method, obj, []);
        if (!isObject(iterator)) throw new TypeError_('The async iterator is not an object');
        return new IteratorRecord(iterator, iterator.next);
    }
    method = obj == null ? undefined : obj[iteratorSymbol];
    if (method === undefined || method === null) throw new TypeError_('The argument is not iterable');
    if (typeof method !== 'function') throw new TypeError_('The iterator method is not callable');
    const syncIterator = apply(method, obj, []);
    if (!isObject(syncIterator)) throw new TypeError_('The iterator is not an object');
    return asyncFromSyncIterator(new IteratorRecord(syncIterator, syncIterator.next));
}

class IteratorRecord {
    iterator;
    next;
    constructor(iterator, next) {
        this.iterator = iterator;
        this.next = next;
    }
}

// ECMA-262's CreateAsyncFromSyncIterator, enough for from(): next() and
// return() answered as promises of iterator results, each value awaited.
function asyncFromSyncIterator(syncRecord) {
    const iterator = objectCreate(null);
    function continuation(result) {
        if (!isObject(result)) return rejectedWith(new TypeError_('The iterator result is not an object'));
        const done = toBoolean(result.done);
        const value = result.value;
        let valueWrapper;
        try {
            valueWrapper = resolvedWith(value);
        } catch (e) {
            return rejectedWith(e);
        }
        return transformPromise(valueWrapper, function (v) { return iterResult(v, done); }, function (e) {
            if (!done) {
                const ret = syncRecord.iterator.return;
                if (ret !== undefined && ret !== null) try { apply(ret, syncRecord.iterator, []); } catch (ignored) {}
            }
            throw e;
        });
    }
    iterator.next = function () {
        let result;
        try {
            result = apply(syncRecord.next, syncRecord.iterator, []);
        } catch (e) {
            return rejectedWith(e);
        }
        return continuation(result);
    };
    iterator.return = function (value) {
        let ret, result;
        try {
            ret = syncRecord.iterator.return;
            if (ret === undefined || ret === null) return resolvedWith(iterResult(value, true));
            result = apply(ret, syncRecord.iterator, [value]);
        } catch (e) {
            return rejectedWith(e);
        }
        return continuation(result);
    };
    return new IteratorRecord(iterator, iterator.next);
}

function readableStreamFromIterable(asyncIterable) {
    // WebIDL's async iterable argument: a string, or anything else that is
    // no object, is refused before its iterator is looked for.
    if (!isObject(asyncIterable)) throw new TypeError_('The argument is not an async iterable object');
    let stream;
    const record = getAsyncIterator(asyncIterable);
    function pullAlgorithm() {
        let nextResult;
        try {
            if (typeof record.next !== 'function') throw new TypeError_('The iterator\'s next is not callable');
            nextResult = apply(record.next, record.iterator, []);
        } catch (e) {
            return rejectedWith(e);
        }
        let nextPromise;
        try {
            nextPromise = resolvedWith(nextResult);
        } catch (e) {
            return rejectedWith(e);
        }
        return transformPromise(nextPromise, function (result) {
            if (!isObject(result)) throw new TypeError_('The iterator result is not an object');
            if (toBoolean(result.done)) controllerClose(stream.controller);
            else controllerEnqueue(stream.controller, result.value);
        }, undefined);
    }
    function cancelAlgorithm(reason) {
        const iterator = record.iterator;
        let returnResult;
        try {
            const returnMethod = iterator.return;
            if (returnMethod === undefined || returnMethod === null) return resolvedWith(undefined);
            if (typeof returnMethod !== 'function') throw new TypeError_('The iterator\'s return is not callable');
            returnResult = apply(returnMethod, iterator, [reason]);
        } catch (e) {
            return rejectedWith(e);
        }
        let returnPromise;
        try {
            returnPromise = resolvedWith(returnResult);
        } catch (e) {
            return rejectedWith(e);
        }
        return transformPromise(returnPromise, function (result) {
            if (!isObject(result)) throw new TypeError_('The iterator\'s return result is not an object');
        }, undefined);
    }
    stream = createReadableStream(noop, pullAlgorithm, cancelAlgorithm, 0);
    return stream;
}

// ── WritableStream (§ 5.2) ───────────────────────────────────────────────
let wsOf, wsWriterOf, wsControllerOf;

class WritableStream {
    #r;
    static {
        wsOf = function (o) { return isObject(o) && #r in o ? o.#r : undefined; };
    }

    constructor(underlyingSink = undefined, strategy = undefined) {
        if (underlyingSink === INTERNAL) {
            this.#r = strategy;
            strategy.pub = this;
            return;
        }
        if (underlyingSink !== undefined && !isObject(underlyingSink))
            throw new TypeError_('The underlying sink is not an object');
        const strategyDict = new QueuingStrategyDict(strategy);
        if (underlyingSink === undefined) underlyingSink = null;
        const sink = dictionarySource(underlyingSink, 'The underlying sink');
        const abort = callbackOrUndefined(member(sink, 'abort'), 'abort');
        const close = callbackOrUndefined(member(sink, 'close'), 'close');
        const start = callbackOrUndefined(member(sink, 'start'), 'start');
        const type = member(sink, 'type');
        const write = callbackOrUndefined(member(sink, 'write'), 'write');
        if (type !== undefined) throw new RangeError_('Invalid type is specified');
        const w = new WSRecord();
        this.#r = w;
        w.pub = this;
        const sizeAlgorithm = extractSizeAlgorithm(strategyDict);
        const hwm = extractHighWaterMark(strategyDict, 1);
        setUpWSDefaultControllerFromSink(w, underlyingSink, start, write, close, abort, hwm, sizeAlgorithm);
    }

    get locked() {
        const w = wsOf(this);
        if (w === undefined) throw new TypeError_('Illegal invocation');
        return isWritableStreamLocked(w);
    }

    abort(reason = undefined) {
        const w = wsOf(this);
        if (w === undefined) return rejectedWith(new TypeError_('Illegal invocation'));
        if (isWritableStreamLocked(w)) return rejectedWith(new TypeError_('The stream is locked'));
        return writableStreamAbort(w, reason);
    }

    close() {
        const w = wsOf(this);
        if (w === undefined) return rejectedWith(new TypeError_('Illegal invocation'));
        if (isWritableStreamLocked(w)) return rejectedWith(new TypeError_('The stream is locked'));
        if (writableStreamCloseQueuedOrInFlight(w)) return rejectedWith(new TypeError_('The stream is closing'));
        return writableStreamClose(w);
    }

    getWriter() {
        const w = wsOf(this);
        if (w === undefined) throw new TypeError_('Illegal invocation');
        return acquireDefaultWriter(w).pub;
    }
}

function isWritableStreamLocked(stream) { return stream.writer !== undefined; }

function createWritableStream(startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm, hwm, sizeAlgorithm) {
    const stream = new WSRecord();
    new WritableStream(INTERNAL, stream);
    const c = new WSControllerRecord();
    setUpWSDefaultController(stream, c, startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm, hwm,
        sizeAlgorithm);
    return stream;
}

function writableStreamAbort(stream, reason) {
    if (stream.state === 'closed' || stream.state === 'errored') return resolvedWith(undefined);
    apply(abortControllerAbort, stream.controller.abortController, [reason]);
    const state = stream.state;
    if (state === 'closed' || state === 'errored') return resolvedWith(undefined);
    if (stream.pendingAbortRequest !== undefined) return stream.pendingAbortRequest.deferred.promise;
    let wasAlreadyErroring = false;
    if (state === 'erroring') {
        wasAlreadyErroring = true;
        reason = undefined;
    }
    const d = newDeferred();
    stream.pendingAbortRequest = new PendingAbortRequest(d, reason, wasAlreadyErroring);
    if (!wasAlreadyErroring) writableStreamStartErroring(stream, reason);
    return d.promise;
}

function writableStreamClose(stream) {
    const state = stream.state;
    if (state === 'closed' || state === 'errored')
        return rejectedWith(new TypeError_('The stream is closed or errored'));
    const d = newDeferred();
    stream.closeRequest = d;
    const writer = stream.writer;
    if (writer !== undefined && stream.backpressure && state === 'writable') resolveDeferred(writer.ready, undefined);
    wsControllerClose(stream.controller);
    return d.promise;
}

function writableStreamAddWriteRequest(stream) {
    const d = newDeferred();
    listPush(stream.writeRequests, d);
    return d.promise;
}

function writableStreamDealWithRejection(stream, error) {
    if (stream.state === 'writable') {
        writableStreamStartErroring(stream, error);
        return;
    }
    writableStreamFinishErroring(stream);
}

function writableStreamStartErroring(stream, reason) {
    const c = stream.controller;
    stream.state = 'erroring';
    stream.storedError = reason;
    const writer = stream.writer;
    if (writer !== undefined) writerEnsureReadyPromiseRejected(writer, reason);
    if (!writableStreamHasOperationMarkedInFlight(stream) && c.started) writableStreamFinishErroring(stream);
}

function writableStreamFinishErroring(stream) {
    stream.state = 'errored';
    resetQueue(stream.controller);
    const storedError = stream.storedError;
    const requests = stream.writeRequests;
    stream.writeRequests = new List();
    while (requests.length > 0) rejectDeferred(listShift(requests), storedError);
    if (stream.pendingAbortRequest === undefined) {
        writableStreamRejectCloseAndClosedPromiseIfNeeded(stream);
        return;
    }
    const abortRequest = stream.pendingAbortRequest;
    stream.pendingAbortRequest = undefined;
    if (abortRequest.wasAlreadyErroring) {
        rejectDeferred(abortRequest.deferred, storedError);
        writableStreamRejectCloseAndClosedPromiseIfNeeded(stream);
        return;
    }
    const promise = wsControllerAbortSteps(stream.controller, abortRequest.reason);
    uponPromise(promise, function () {
        resolveDeferred(abortRequest.deferred, undefined);
        writableStreamRejectCloseAndClosedPromiseIfNeeded(stream);
    }, function (reason) {
        rejectDeferred(abortRequest.deferred, reason);
        writableStreamRejectCloseAndClosedPromiseIfNeeded(stream);
    });
}

function writableStreamFinishInFlightWrite(stream) {
    resolveDeferred(stream.inFlightWriteRequest, undefined);
    stream.inFlightWriteRequest = undefined;
}

function writableStreamFinishInFlightWriteWithError(stream, error) {
    rejectDeferred(stream.inFlightWriteRequest, error);
    stream.inFlightWriteRequest = undefined;
    writableStreamDealWithRejection(stream, error);
}

function writableStreamFinishInFlightClose(stream) {
    resolveDeferred(stream.inFlightCloseRequest, undefined);
    stream.inFlightCloseRequest = undefined;
    if (stream.state === 'erroring') {
        stream.storedError = undefined;
        if (stream.pendingAbortRequest !== undefined) {
            resolveDeferred(stream.pendingAbortRequest.deferred, undefined);
            stream.pendingAbortRequest = undefined;
        }
    }
    stream.state = 'closed';
    const writer = stream.writer;
    if (writer !== undefined) resolveDeferred(writer.closed, undefined);
}

function writableStreamFinishInFlightCloseWithError(stream, error) {
    rejectDeferred(stream.inFlightCloseRequest, error);
    stream.inFlightCloseRequest = undefined;
    if (stream.pendingAbortRequest !== undefined) {
        rejectDeferred(stream.pendingAbortRequest.deferred, error);
        stream.pendingAbortRequest = undefined;
    }
    writableStreamDealWithRejection(stream, error);
}

function writableStreamCloseQueuedOrInFlight(stream) {
    return stream.closeRequest !== undefined || stream.inFlightCloseRequest !== undefined;
}

function writableStreamHasOperationMarkedInFlight(stream) {
    return stream.inFlightWriteRequest !== undefined || stream.inFlightCloseRequest !== undefined;
}

function writableStreamMarkCloseRequestInFlight(stream) {
    stream.inFlightCloseRequest = stream.closeRequest;
    stream.closeRequest = undefined;
}

function writableStreamMarkFirstWriteRequestInFlight(stream) {
    stream.inFlightWriteRequest = listShift(stream.writeRequests);
}

function writableStreamRejectCloseAndClosedPromiseIfNeeded(stream) {
    if (stream.closeRequest !== undefined) {
        rejectDeferred(stream.closeRequest, stream.storedError);
        stream.closeRequest = undefined;
    }
    const writer = stream.writer;
    if (writer !== undefined) {
        rejectDeferred(writer.closed, stream.storedError);
        setHandled(writer.closed.promise);
    }
}

function writableStreamUpdateBackpressure(stream, backpressure) {
    const writer = stream.writer;
    if (writer !== undefined && backpressure !== stream.backpressure) {
        if (backpressure) writer.ready = newDeferred();
        else resolveDeferred(writer.ready, undefined);
    }
    stream.backpressure = backpressure;
}

// ── The default writer (§ 5.3) ───────────────────────────────────────────
class WritableStreamDefaultWriter {
    #r;
    static {
        wsWriterOf = function (o) { return isObject(o) && #r in o ? o.#r : undefined; };
    }

    constructor(stream, record = undefined) {
        if (stream === INTERNAL) {
            this.#r = record;
            record.pub = this;
            return;
        }
        const s = wsOf(stream);
        if (s === undefined) throw new TypeError_('The argument is not a WritableStream');
        const writer = new WSWriterRecord();
        this.#r = writer;
        writer.pub = this;
        setUpDefaultWriter(writer, s);
    }

    get closed() {
        const writer = wsWriterOf(this);
        if (writer === undefined) return rejectedWith(new TypeError_('Illegal invocation'));
        return writer.closed.promise;
    }

    get desiredSize() {
        const writer = wsWriterOf(this);
        if (writer === undefined) throw new TypeError_('Illegal invocation');
        if (writer.stream === undefined) throw new TypeError_('The writer has been released');
        return writerGetDesiredSize(writer);
    }

    get ready() {
        const writer = wsWriterOf(this);
        if (writer === undefined) return rejectedWith(new TypeError_('Illegal invocation'));
        return writer.ready.promise;
    }

    abort(reason = undefined) {
        const writer = wsWriterOf(this);
        if (writer === undefined) return rejectedWith(new TypeError_('Illegal invocation'));
        if (writer.stream === undefined) return rejectedWith(new TypeError_('The writer has been released'));
        return writableStreamAbort(writer.stream, reason);
    }

    close() {
        const writer = wsWriterOf(this);
        if (writer === undefined) return rejectedWith(new TypeError_('Illegal invocation'));
        const stream = writer.stream;
        if (stream === undefined) return rejectedWith(new TypeError_('The writer has been released'));
        if (writableStreamCloseQueuedOrInFlight(stream)) return rejectedWith(new TypeError_('The stream is closing'));
        return writableStreamClose(stream);
    }

    releaseLock() {
        const writer = wsWriterOf(this);
        if (writer === undefined) throw new TypeError_('Illegal invocation');
        if (writer.stream === undefined) return;
        writerRelease(writer);
    }

    write(chunk = undefined) {
        const writer = wsWriterOf(this);
        if (writer === undefined) return rejectedWith(new TypeError_('Illegal invocation'));
        if (writer.stream === undefined) return rejectedWith(new TypeError_('The writer has been released'));
        return writerWrite(writer, chunk);
    }
}

function acquireDefaultWriter(stream) {
    const writer = new WSWriterRecord();
    setUpDefaultWriter(writer, stream);
    new WritableStreamDefaultWriter(INTERNAL, writer);
    return writer;
}

function setUpDefaultWriter(writer, stream) {
    if (isWritableStreamLocked(stream)) throw new TypeError_('The stream is locked');
    writer.stream = stream;
    stream.writer = writer;
    const state = stream.state;
    if (state === 'writable') {
        if (!writableStreamCloseQueuedOrInFlight(stream) && stream.backpressure) writer.ready = newDeferred();
        else writer.ready = resolvedDeferred(undefined);
        writer.closed = newDeferred();
    } else if (state === 'erroring') {
        writer.ready = rejectedDeferred(stream.storedError);
        writer.closed = newDeferred();
    } else if (state === 'closed') {
        writer.ready = resolvedDeferred(undefined);
        writer.closed = resolvedDeferred(undefined);
    } else {
        writer.ready = rejectedDeferred(stream.storedError);
        writer.closed = rejectedDeferred(stream.storedError);
    }
}

function writerCloseWithErrorPropagation(writer) {
    const stream = writer.stream;
    const state = stream.state;
    if (writableStreamCloseQueuedOrInFlight(stream) || state === 'closed') return resolvedWith(undefined);
    if (state === 'errored') return rejectedWith(stream.storedError);
    return writableStreamClose(stream);
}

function writerEnsureClosedPromiseRejected(writer, error) {
    if (writer.closed.state === 'pending') rejectDeferred(writer.closed, error);
    else writer.closed = rejectedDeferred(error);
    setHandled(writer.closed.promise);
}

function writerEnsureReadyPromiseRejected(writer, error) {
    if (writer.ready.state === 'pending') rejectDeferred(writer.ready, error);
    else writer.ready = rejectedDeferred(error);
    setHandled(writer.ready.promise);
}

function writerGetDesiredSize(writer) {
    const stream = writer.stream;
    const state = stream.state;
    if (state === 'errored' || state === 'erroring') return null;
    if (state === 'closed') return 0;
    return wsControllerGetDesiredSize(stream.controller);
}

function writerRelease(writer) {
    const stream = writer.stream;
    const releasedError = new TypeError_('The writer has been released');
    writerEnsureReadyPromiseRejected(writer, releasedError);
    writerEnsureClosedPromiseRejected(writer, releasedError);
    stream.writer = undefined;
    writer.stream = undefined;
}

function writerWrite(writer, chunk) {
    const stream = writer.stream;
    const c = stream.controller;
    const chunkSize = wsControllerGetChunkSize(c, chunk);
    if (stream !== writer.stream) return rejectedWith(new TypeError_('The writer has been released'));
    const state = stream.state;
    if (state === 'errored') return rejectedWith(stream.storedError);
    if (writableStreamCloseQueuedOrInFlight(stream) || state === 'closed')
        return rejectedWith(new TypeError_('The stream is closing or closed'));
    if (state === 'erroring') return rejectedWith(stream.storedError);
    const promise = writableStreamAddWriteRequest(stream);
    wsControllerWrite(c, chunk, chunkSize);
    return promise;
}

// ── The default controller (§ 5.4, § 5.5.4) ──────────────────────────────
class WritableStreamDefaultController {
    #r;
    static {
        wsControllerOf = function (o) { return isObject(o) && #r in o ? o.#r : undefined; };
    }

    constructor(key = undefined, record = undefined) {
        if (key !== INTERNAL) throw new TypeError_('Illegal constructor');
        this.#r = record;
        record.pub = this;
    }

    get signal() {
        const c = wsControllerOf(this);
        if (c === undefined) throw new TypeError_('Illegal invocation');
        return apply(abortControllerSignal, c.abortController, []);
    }

    error(e = undefined) {
        const c = wsControllerOf(this);
        if (c === undefined) throw new TypeError_('Illegal invocation');
        if (c.stream.state !== 'writable') return;
        wsControllerError(c, e);
    }
}

function wsControllerAbortSteps(c, reason) {
    const result = c.abortAlgorithm(reason);
    wsControllerClearAlgorithms(c);
    return result;
}

function setUpWSDefaultController(stream, c, startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm, hwm,
                                  sizeAlgorithm) {
    c.stream = stream;
    stream.controller = c;
    resetQueue(c);
    c.abortController = new AbortController_();
    c.started = false;
    c.strategySizeAlgorithm = sizeAlgorithm;
    c.strategyHWM = hwm;
    c.writeAlgorithm = writeAlgorithm;
    c.closeAlgorithm = closeAlgorithm;
    c.abortAlgorithm = abortAlgorithm;
    if (c.pub === undefined) new WritableStreamDefaultController(INTERNAL, c);
    writableStreamUpdateBackpressure(stream, wsControllerGetBackpressure(c));
    const startResult = startAlgorithm();
    uponPromise(resolvedWith(startResult), function () {
        c.started = true;
        wsControllerAdvanceQueueIfNeeded(c);
    }, function (r) {
        c.started = true;
        writableStreamDealWithRejection(stream, r);
    });
}

function setUpWSDefaultControllerFromSink(stream, sink, start, write, close, abort, hwm, sizeAlgorithm) {
    const c = new WSControllerRecord();
    new WritableStreamDefaultController(INTERNAL, c);
    const startAlgorithm = start === undefined ? noop : function () { return apply(start, sink, [c.pub]); };
    const writeAlgorithm = write === undefined ? resolvedUndefined
        : function (chunk) { return promiseCall(write, sink, [chunk, c.pub]); };
    const closeAlgorithm = close === undefined ? resolvedUndefined
        : function () { return promiseCall(close, sink, []); };
    const abortAlgorithm = abort === undefined ? resolvedUndefined
        : function (reason) { return promiseCall(abort, sink, [reason]); };
    setUpWSDefaultController(stream, c, startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm, hwm,
        sizeAlgorithm);
}

function wsControllerAdvanceQueueIfNeeded(c) {
    const stream = c.stream;
    if (!c.started) return;
    if (stream.inFlightWriteRequest !== undefined) return;
    if (stream.state === 'erroring') {
        writableStreamFinishErroring(stream);
        return;
    }
    if (c.queue.length === 0) return;
    const value = peekQueueValue(c);
    if (value === closeSentinel) wsControllerProcessClose(c);
    else wsControllerProcessWrite(c, value);
}

function wsControllerClearAlgorithms(c) {
    c.writeAlgorithm = undefined;
    c.closeAlgorithm = undefined;
    c.abortAlgorithm = undefined;
    c.strategySizeAlgorithm = undefined;
}

function wsControllerClose(c) {
    enqueueValueWithSize(c, closeSentinel, 0);
    wsControllerAdvanceQueueIfNeeded(c);
}

function wsControllerError(c, error) {
    wsControllerClearAlgorithms(c);
    writableStreamStartErroring(c.stream, error);
}

function wsControllerErrorIfNeeded(c, error) {
    if (c.stream.state === 'writable') wsControllerError(c, error);
}

function wsControllerGetBackpressure(c) { return wsControllerGetDesiredSize(c) <= 0; }

function wsControllerGetChunkSize(c, chunk) {
    if (c.strategySizeAlgorithm === undefined) return 1;
    try {
        return c.strategySizeAlgorithm(chunk);
    } catch (e) {
        wsControllerErrorIfNeeded(c, e);
        return 1;
    }
}

function wsControllerGetDesiredSize(c) { return c.strategyHWM - c.queueTotalSize; }

function wsControllerProcessClose(c) {
    const stream = c.stream;
    writableStreamMarkCloseRequestInFlight(stream);
    dequeueValue(c);
    const sinkClosePromise = c.closeAlgorithm();
    wsControllerClearAlgorithms(c);
    uponPromise(sinkClosePromise, function () {
        writableStreamFinishInFlightClose(stream);
    }, function (reason) {
        writableStreamFinishInFlightCloseWithError(stream, reason);
    });
}

function wsControllerProcessWrite(c, chunk) {
    const stream = c.stream;
    writableStreamMarkFirstWriteRequestInFlight(stream);
    const sinkWritePromise = c.writeAlgorithm(chunk);
    uponPromise(sinkWritePromise, function () {
        writableStreamFinishInFlightWrite(stream);
        const state = stream.state;
        dequeueValue(c);
        if (!writableStreamCloseQueuedOrInFlight(stream) && state === 'writable')
            writableStreamUpdateBackpressure(stream, wsControllerGetBackpressure(c));
        wsControllerAdvanceQueueIfNeeded(c);
    }, function (reason) {
        if (stream.state === 'writable') wsControllerClearAlgorithms(c);
        writableStreamFinishInFlightWriteWithError(stream, reason);
    });
}

function wsControllerWrite(c, chunk, chunkSize) {
    try {
        enqueueValueWithSize(c, chunk, chunkSize);
    } catch (e) {
        wsControllerErrorIfNeeded(c, e);
        return;
    }
    const stream = c.stream;
    if (!writableStreamCloseQueuedOrInFlight(stream) && stream.state === 'writable')
        writableStreamUpdateBackpressure(stream, wsControllerGetBackpressure(c));
    wsControllerAdvanceQueueIfNeeded(c);
}

// ── TransformStream (§ 6.2) ──────────────────────────────────────────────
let tsOf, tsControllerOf;

class TransformStream {
    #r;
    static {
        tsOf = function (o) { return isObject(o) && #r in o ? o.#r : undefined; };
    }

    constructor(transformer = undefined, writableStrategy = undefined, readableStrategy = undefined) {
        if (transformer !== undefined && !isObject(transformer))
            throw new TypeError_('The transformer is not an object');
        const writableStrategyDict = new QueuingStrategyDict(writableStrategy);
        const readableStrategyDict = new QueuingStrategyDict(readableStrategy);
        if (transformer === undefined) transformer = null;
        const source = dictionarySource(transformer, 'The transformer');
        const cancel = callbackOrUndefined(member(source, 'cancel'), 'cancel');
        const flush = callbackOrUndefined(member(source, 'flush'), 'flush');
        const readableType = member(source, 'readableType');
        const start = callbackOrUndefined(member(source, 'start'), 'start');
        const transform = callbackOrUndefined(member(source, 'transform'), 'transform');
        const writableType = member(source, 'writableType');
        if (readableType !== undefined) throw new RangeError_('Invalid readableType specified');
        if (writableType !== undefined) throw new RangeError_('Invalid writableType specified');
        const readableHWM = extractHighWaterMark(readableStrategyDict, 0);
        const readableSize = extractSizeAlgorithm(readableStrategyDict);
        const writableHWM = extractHighWaterMark(writableStrategyDict, 1);
        const writableSize = extractSizeAlgorithm(writableStrategyDict);
        const t = new TSRecord();
        this.#r = t;
        t.pub = this;
        const startDeferred = newDeferred();
        initializeTransformStream(t, startDeferred.promise, writableHWM, writableSize, readableHWM, readableSize);
        setUpTSDefaultControllerFromTransformer(t, transformer, transform, flush, cancel);
        if (start !== undefined) resolveDeferred(startDeferred, apply(start, transformer, [t.controller.pub]));
        else resolveDeferred(startDeferred, undefined);
    }

    get readable() {
        const t = tsOf(this);
        if (t === undefined) throw new TypeError_('Illegal invocation');
        return t.readable.pub;
    }

    get writable() {
        const t = tsOf(this);
        if (t === undefined) throw new TypeError_('Illegal invocation');
        return t.writable.pub;
    }
}

function initializeTransformStream(stream, startPromise, writableHWM, writableSize, readableHWM, readableSize) {
    function startAlgorithm() { return startPromise; }
    stream.writable = createWritableStream(startAlgorithm, function (chunk) {
        return tsSinkWriteAlgorithm(stream, chunk);
    }, function () {
        return tsSinkCloseAlgorithm(stream);
    }, function (reason) {
        return tsSinkAbortAlgorithm(stream, reason);
    }, writableHWM, writableSize);
    stream.readable = createReadableStream(startAlgorithm, function () {
        return tsSourcePullAlgorithm(stream);
    }, function (reason) {
        return tsSourceCancelAlgorithm(stream, reason);
    }, readableHWM, readableSize);
    stream.backpressure = undefined;
    stream.backpressureChange = undefined;
    tsSetBackpressure(stream, true);
    stream.controller = undefined;
}

function tsError(stream, e) {
    controllerError(stream.readable.controller, e);
    tsErrorWritableAndUnblockWrite(stream, e);
}

function tsErrorWritableAndUnblockWrite(stream, e) {
    tsControllerClearAlgorithms(stream.controller);
    wsControllerErrorIfNeeded(stream.writable.controller, e);
    tsUnblockWrite(stream);
}

function tsUnblockWrite(stream) {
    if (stream.backpressure) tsSetBackpressure(stream, false);
}

function tsSetBackpressure(stream, backpressure) {
    if (stream.backpressureChange !== undefined) resolveDeferred(stream.backpressureChange, undefined);
    stream.backpressureChange = newDeferred();
    stream.backpressure = backpressure;
}

class TransformStreamDefaultController {
    #r;
    static {
        tsControllerOf = function (o) { return isObject(o) && #r in o ? o.#r : undefined; };
    }

    constructor(key = undefined, record = undefined) {
        if (key !== INTERNAL) throw new TypeError_('Illegal constructor');
        this.#r = record;
        record.pub = this;
    }

    get desiredSize() {
        const c = tsControllerOf(this);
        if (c === undefined) throw new TypeError_('Illegal invocation');
        return controllerGetDesiredSize(c.stream.readable.controller);
    }

    enqueue(chunk = undefined) {
        const c = tsControllerOf(this);
        if (c === undefined) throw new TypeError_('Illegal invocation');
        tsControllerEnqueue(c, chunk);
    }

    error(reason = undefined) {
        const c = tsControllerOf(this);
        if (c === undefined) throw new TypeError_('Illegal invocation');
        tsError(c.stream, reason);
    }

    terminate() {
        const c = tsControllerOf(this);
        if (c === undefined) throw new TypeError_('Illegal invocation');
        tsControllerTerminate(c);
    }
}

function setUpTSDefaultController(stream, c, transformAlgorithm, flushAlgorithm, cancelAlgorithm) {
    c.stream = stream;
    stream.controller = c;
    c.transformAlgorithm = transformAlgorithm;
    c.flushAlgorithm = flushAlgorithm;
    c.cancelAlgorithm = cancelAlgorithm;
}

function setUpTSDefaultControllerFromTransformer(stream, transformer, transform, flush, cancel) {
    const c = new TSControllerRecord();
    new TransformStreamDefaultController(INTERNAL, c);
    let transformAlgorithm = function (chunk) {
        try {
            tsControllerEnqueue(c, chunk);
            return resolvedWith(undefined);
        } catch (e) {
            return rejectedWith(e);
        }
    };
    if (transform !== undefined)
        transformAlgorithm = function (chunk) { return promiseCall(transform, transformer, [chunk, c.pub]); };
    const flushAlgorithm = flush === undefined ? resolvedUndefined
        : function () { return promiseCall(flush, transformer, [c.pub]); };
    const cancelAlgorithm = cancel === undefined ? resolvedUndefined
        : function (reason) { return promiseCall(cancel, transformer, [reason]); };
    setUpTSDefaultController(stream, c, transformAlgorithm, flushAlgorithm, cancelAlgorithm);
}

function tsControllerClearAlgorithms(c) {
    c.transformAlgorithm = undefined;
    c.flushAlgorithm = undefined;
    c.cancelAlgorithm = undefined;
}

function tsControllerEnqueue(c, chunk) {
    const stream = c.stream;
    const rc = stream.readable.controller;
    if (!controllerCanCloseOrEnqueue(rc)) throw new TypeError_('The readable side is not in a state that permits enqueue');
    try {
        controllerEnqueue(rc, chunk);
    } catch (e) {
        tsErrorWritableAndUnblockWrite(stream, e);
        throw stream.readable.storedError;
    }
    const backpressure = controllerHasBackpressure(rc);
    if (backpressure !== stream.backpressure) tsSetBackpressure(stream, true);
}

function tsControllerPerformTransform(c, chunk) {
    const transformed = c.transformAlgorithm(chunk);
    return transformPromise(transformed, undefined, function (r) {
        tsError(c.stream, r);
        throw r;
    });
}

function tsControllerTerminate(c) {
    const stream = c.stream;
    controllerClose(stream.readable.controller);
    tsErrorWritableAndUnblockWrite(stream, new TypeError_('The transform stream has been terminated'));
}

function tsSinkWriteAlgorithm(stream, chunk) {
    const c = stream.controller;
    if (stream.backpressure) {
        return transformPromise(stream.backpressureChange.promise, function () {
            const writable = stream.writable;
            if (writable.state === 'erroring') throw writable.storedError;
            return tsControllerPerformTransform(c, chunk);
        }, undefined);
    }
    return tsControllerPerformTransform(c, chunk);
}

function tsSinkAbortAlgorithm(stream, reason) {
    const c = stream.controller;
    if (c.finish !== undefined) return c.finish.promise;
    const readable = stream.readable;
    c.finish = newDeferred();
    const cancelPromise = c.cancelAlgorithm(reason);
    tsControllerClearAlgorithms(c);
    uponPromise(cancelPromise, function () {
        if (readable.state === 'errored') {
            rejectDeferred(c.finish, readable.storedError);
        } else {
            controllerError(readable.controller, reason);
            resolveDeferred(c.finish, undefined);
        }
    }, function (r) {
        controllerError(readable.controller, r);
        rejectDeferred(c.finish, r);
    });
    return c.finish.promise;
}

function tsSinkCloseAlgorithm(stream) {
    const c = stream.controller;
    if (c.finish !== undefined) return c.finish.promise;
    const readable = stream.readable;
    c.finish = newDeferred();
    const flushPromise = c.flushAlgorithm();
    tsControllerClearAlgorithms(c);
    uponPromise(flushPromise, function () {
        if (readable.state === 'errored') {
            rejectDeferred(c.finish, readable.storedError);
        } else {
            controllerClose(readable.controller);
            resolveDeferred(c.finish, undefined);
        }
    }, function (r) {
        controllerError(readable.controller, r);
        rejectDeferred(c.finish, r);
    });
    return c.finish.promise;
}

function tsSourceCancelAlgorithm(stream, reason) {
    const c = stream.controller;
    if (c.finish !== undefined) return c.finish.promise;
    const writable = stream.writable;
    c.finish = newDeferred();
    const cancelPromise = c.cancelAlgorithm(reason);
    tsControllerClearAlgorithms(c);
    uponPromise(cancelPromise, function () {
        if (writable.state === 'errored') {
            rejectDeferred(c.finish, writable.storedError);
        } else {
            wsControllerErrorIfNeeded(writable.controller, reason);
            tsUnblockWrite(stream);
            resolveDeferred(c.finish, undefined);
        }
    }, function (r) {
        wsControllerErrorIfNeeded(writable.controller, r);
        tsUnblockWrite(stream);
        rejectDeferred(c.finish, r);
    });
    return c.finish.promise;
}

function tsSourcePullAlgorithm(stream) {
    tsSetBackpressure(stream, false);
    return stream.backpressureChange.promise;
}

// ── Queuing strategies (§ 7) ─────────────────────────────────────────────
function strategyInit(init) {
    const source = dictionarySource(init, 'The init');
    const hwm = member(source, 'highWaterMark');
    if (hwm === undefined) throw new TypeError_('highWaterMark is required');
    return toNumber(hwm);
}

const byteLengthSize = (chunk) => chunk.byteLength;
defineProperty(byteLengthSize, 'name', {value: 'size'});
const countSize = () => 1;
defineProperty(countSize, 'name', {value: 'size'});

let blqsOf, cqsOf;

class ByteLengthQueuingStrategy {
    #hwm;
    static {
        blqsOf = function (o) { return isObject(o) && #hwm in o; };
    }

    constructor(init) {
        this.#hwm = strategyInit(init);
    }

    get highWaterMark() {
        if (!blqsOf(this)) throw new TypeError_('Illegal invocation');
        return this.#hwm;
    }

    get size() {
        if (!blqsOf(this)) throw new TypeError_('Illegal invocation');
        return byteLengthSize;
    }
}

class CountQueuingStrategy {
    #hwm;
    static {
        cqsOf = function (o) { return isObject(o) && #hwm in o; };
    }

    constructor(init) {
        this.#hwm = strategyInit(init);
    }

    get highWaterMark() {
        if (!cqsOf(this)) throw new TypeError_('Illegal invocation');
        return this.#hwm;
    }

    get size() {
        if (!cqsOf(this)) throw new TypeError_('Illegal invocation');
        return countSize;
    }
}

// ── The interfaces' shapes (WebIDL) ──────────────────────────────────────
webidlInterface(ReadableStream, 'ReadableStream', ['from']);
// `values` and @@asyncIterator are one function (WebIDL § 3.7.10).
defineProperty(ReadableStream.prototype, asyncIteratorSymbol,
    {value: ReadableStream.prototype.values, writable: true, enumerable: false, configurable: true});
webidlInterface(ReadableStreamDefaultReader, 'ReadableStreamDefaultReader');
webidlInterface(ReadableStreamDefaultController, 'ReadableStreamDefaultController');
webidlInterface(ReadableStreamBYOBReader, 'ReadableStreamBYOBReader');
webidlInterface(ReadableByteStreamController, 'ReadableByteStreamController');
webidlInterface(ReadableStreamBYOBRequest, 'ReadableStreamBYOBRequest');
webidlInterface(WritableStream, 'WritableStream');
webidlInterface(WritableStreamDefaultWriter, 'WritableStreamDefaultWriter');
webidlInterface(WritableStreamDefaultController, 'WritableStreamDefaultController');
webidlInterface(TransformStream, 'TransformStream');
webidlInterface(TransformStreamDefaultController, 'TransformStreamDefaultController');
webidlInterface(ByteLengthQueuingStrategy, 'ByteLengthQueuingStrategy');
webidlInterface(CountQueuingStrategy, 'CountQueuingStrategy');

// The async iterator's prototype: next and return, over
// %AsyncIteratorPrototype%, named as WebIDL names it.
const asyncIteratorPrototype = objectCreate(AsyncIteratorPrototype);
defineProperty(asyncIteratorPrototype, 'next',
    {value: asyncIteratorNext, writable: true, enumerable: true, configurable: true});
defineProperty(asyncIteratorPrototype, 'return',
    {value: asyncIteratorReturn, writable: true, enumerable: true, configurable: true});
defineProperty(asyncIteratorNext, 'name', {value: 'next'});
defineProperty(asyncIteratorReturn, 'name', {value: 'return'});
defineProperty(asyncIteratorPrototype, toStringTag,
    {value: 'ReadableStream AsyncIterator', writable: false, enumerable: false, configurable: true});

// ── The door (libdom's d_stream_* verbs, streams.c) ──────────────────────
// A body's stream over its bytes: a byte stream (Fetch's and the File
// API's "set up with byte reading support"), so a page may read it with a
// BYOB reader. Zero bytes are a stream closed from its creation.
// Otherwise the bytes are handed over at the first pull, as one chunk
// (the Uint8Array d_stream_new made for this stream alone, whose buffer
// the stream takes), and the stream closes behind them: until a reader
// asks, the stream is readable, so a fetch's abort that lands after the
// answer but before the body is read still errors it (the Response's
// abort steps).
//
// As "set up with byte reading support" says, the pull turns a throw into
// a rejected promise. The close throws when a pending BYOB read holds part of an
// element (a Uint16Array read of an odd-length body), after erroring the
// stream; the page's read then rejects instead of throwing.
function doorFromBytes(bytes) {
    let stream;
    function pullAlgorithm() {
        try {
            rbsControllerEnqueue(stream.controller, viewFacts(bytes));
            rbsCloseForOthers(stream.controller);
        } catch (e) {
            return rejectedWith(e);
        }
        return resolvedWith(undefined);
    }
    stream = createReadableByteStream(noop, pullAlgorithm, resolvedUndefined);
    if (apply(typedArrayByteLength, bytes, []) === 0) rbsCloseForOthers(stream.controller);
    return stream.pub;
}

// A body read whole before its stream was made: the stream closed and
// disturbed, and locked when the read kept its reader.
function doorSpent(locked) {
    const stream = createReadableByteStream(noop, resolvedUndefined, resolvedUndefined);
    rbsControllerClose(stream.controller);
    stream.disturbed = true;
    if (locked) acquireDefaultReader(stream);
    return stream.pub;
}

// A body's stream locked and disturbed for good: one a Request made from
// its own takes (Fetch's proxy reads it to its end), or one a body method
// read after an abort errored it (the read kept its reader).
function doorTake(value) {
    const stream = rsOf(value);
    acquireDefaultReader(stream);
    stream.disturbed = true;
}

function doorIs(value) { return rsOf(value) !== undefined; }

function doorLocked(value) { return isReadableStreamLocked(rsOf(value)); }

function doorDisturbed(value) { return rsOf(value).disturbed; }

// Fetch's "fully read" (Streams' "read all bytes"): a reader taken and
// never released, every chunk a Uint8Array, the bytes joined into one. A
// chunk that is not one fails the read and nothing else: the source is not
// cancelled. A loop and not a recursion: the reads a queue answers at once
// would otherwise nest a call per chunk queued, and a page's stream can
// queue more than the stack holds. A chunk is known by its internal slot
// (the typed arrays' own toStringTag and byteLength getters), never by
// instanceof or a byteLength a page can redefine.
function doorReadAll(value) {
    const stream = rsOf(value);
    const reader = acquireDefaultReader(stream);
    const d = newDeferred();
    const chunks = slots();
    let count = 0;
    let total = 0;
    let reading = false;
    let more = false;
    const request = new ReadRequest(function (chunk) {
        if (apply(typedArrayTag, chunk, []) !== 'Uint8Array') {
            rejectDeferred(d, new TypeError_('A body chunk is not a Uint8Array'));
            return;
        }
        chunks[count++] = chunk;
        total += apply(typedArrayByteLength, chunk, []);
        if (reading) more = true;
        else loop();
    }, function () {
        // A chunk's buffer can be taken from it after it was read (a page
        // transfers it), and joining it then throws; the read fails with
        // that, not the controller's close that called these steps.
        try {
            const all = new Uint8Array_(total);
            let at = 0;
            for (let i = 0; i < count; i++) {
                apply(uint8Set, all, [chunks[i], at]);
                at += apply(typedArrayByteLength, chunks[i], []);
            }
            resolveDeferred(d, all);
        } catch (e) {
            rejectDeferred(d, e);
        }
    }, function (e) {
        rejectDeferred(d, e);
    });
    function loop() {
        do {
            more = false;
            reading = true;
            readerRead(reader, request);
            reading = false;
        } while (more);
    }
    loop();
    return d.promise;
}

// The body methods' read: every byte, then `convert` (libdom's, which
// makes the text, the JSON, the Blob...) given them.
function doorReadAllThen(value, convert) {
    return transformPromise(doorReadAll(value), convert, undefined);
}

function doorTee(value) {
    const branches = readableStreamTee(rsOf(value), true);
    return [branches.branch1.pub, branches.branch2.pub];
}

// The Response's abort steps: a readable body's stream errored. The
// stream may be a page's (new Response(stream)), of either kind.
function doorError(value, reason) {
    const stream = rsOf(value);
    if (stream.state !== 'readable') return;
    if (stream.controller.byte) rbsControllerError(stream.controller, reason);
    else controllerError(stream.controller, reason);
}

const door = objectCreate(null);
door.fromBytes = doorFromBytes;
door.spent = doorSpent;
door.take = doorTake;
door.is = doorIs;
door.locked = doorLocked;
door.disturbed = doorDisturbed;
door.readAllThen = doorReadAllThen;
door.tee = doorTee;
door.error = doorError;

const exported = objectCreate(null);
exported.ReadableStream = ReadableStream;
exported.ReadableStreamDefaultReader = ReadableStreamDefaultReader;
exported.ReadableStreamDefaultController = ReadableStreamDefaultController;
exported.ReadableStreamBYOBReader = ReadableStreamBYOBReader;
exported.ReadableByteStreamController = ReadableByteStreamController;
exported.ReadableStreamBYOBRequest = ReadableStreamBYOBRequest;
exported.WritableStream = WritableStream;
exported.WritableStreamDefaultWriter = WritableStreamDefaultWriter;
exported.WritableStreamDefaultController = WritableStreamDefaultController;
exported.TransformStream = TransformStream;
exported.TransformStreamDefaultController = TransformStreamDefaultController;
exported.ByteLengthQueuingStrategy = ByteLengthQueuingStrategy;
exported.CountQueuingStrategy = CountQueuingStrategy;
exported.door = door;
return exported;
})
