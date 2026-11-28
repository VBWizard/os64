// mse-common.js: what the Media Source Extensions test pages share
// (docs/design/pending/MSE_DOM.md; MS.3.2). The clips are the fed fixture,
// sync-fed-*.mp4, each with an index beside it (sync-fed-*.json:
// {"init":[offset,length], "fragments":[[offset,length,start,duration],...]}).
//
// Every page logs its events with their times on the page and on yonder's
// output (console.log), and ends with ONE line a harness reads:
//   MSE VERDICT <page>: PASS <what held>
//   MSE VERDICT <page>: FAIL <why>
// The title carries the readyState, the time and the verdict once it is in.

const MSE = (() => {
  const t0 = performance.now();
  const log = document.getElementById('log');
  let decided = false;
  const page = location.pathname.split('/').pop();

  function note(s) {
    const line = (performance.now() - t0).toFixed(0) + ' ms  ' + s;
    if (log) log.textContent += line + '\n';
    console.log(line);
  }

  function verdict(ok, what) {
    if (decided) return;
    decided = true;
    const line = 'MSE VERDICT ' + page + ': ' + (ok ? 'PASS ' : 'FAIL ') + what;
    if (log) log.textContent += line + '\n';
    console.log(line);
    document.title = (ok ? 'PASS ' : 'FAIL ') + page;
  }

  function ranges(r) {
    const out = [];
    for (let i = 0; i < r.length; i++) out.push(r.start(i).toFixed(2) + '-' + r.end(i).toFixed(2));
    return out.join(',') || 'none';
  }

  // A clip and its index: {bytes, index}.
  async function clip(name) {
    const [bytes, index] = await Promise.all([
      fetch(name + '.mp4').then(r => r.arrayBuffer()).then(b => new Uint8Array(b)),
      fetch(name + '.json').then(r => r.json())]);
    return {bytes, index};
  }
  const init = c => c.bytes.subarray(c.index.init[0], c.index.init[0] + c.index.init[1]);
  const fragment = (c, i) => {
    const f = c.index.fragments[i];
    return c.bytes.subarray(f[0], f[0] + f[1]);
  };

  // One append, resolved at its updateend (rejected at an error).
  function append(sb, bytes) {
    return new Promise((resolve, reject) => {
      const done = () => { sb.removeEventListener('error', failed); resolve(); };
      const failed = () => { sb.removeEventListener('updateend', done); reject(new Error('append error')); };
      sb.addEventListener('updateend', done, {once: true});
      sb.addEventListener('error', failed, {once: true});
      sb.appendBuffer(bytes);
    });
  }

  // Bytes appended in pieces cut at the given fractions (never at a box's
  // edge on purpose), each piece its own append.
  async function appendCut(sb, bytes, cuts) {
    let at = 0;
    for (const c of cuts.concat([1])) {
      const end = Math.max(at, Math.min(bytes.length, Math.round(bytes.length * c)));
      if (end > at) await append(sb, bytes.subarray(at, end));
      at = end;
    }
  }

  // The removal, resolved at its updateend.
  function remove(sb, start, end) {
    return new Promise(resolve => {
      sb.addEventListener('updateend', () => resolve(), {once: true});
      sb.remove(start, end);
    });
  }

  // A MediaSource on `video`, open, with a SourceBuffer per type.
  function open(video, types) {
    if (typeof MediaSource === 'undefined') {
      verdict(false, 'no MediaSource (is mse = on in yonder.conf, or --mse=on?)');
      return Promise.reject(new Error('no MediaSource'));
    }
    for (const t of types)
      if (!MediaSource.isTypeSupported(t)) {
        verdict(false, 'isTypeSupported says no to ' + t);
        return Promise.reject(new Error('unsupported'));
      }
    const ms = new MediaSource();
    return new Promise((resolve, reject) => {
      ms.addEventListener('sourceopen', () => {
        note('sourceopen');
        try {
          resolve({ms, buffers: types.map(t => ms.addSourceBuffer(t))});
        } catch (e) {
          verdict(false, 'addSourceBuffer: ' + e.name);
          reject(e);
        }
      }, {once: true});
      ms.addEventListener('sourceended', () => note('sourceended'));
      ms.addEventListener('sourceclose', () => note('sourceclose'));
      video.src = URL.createObjectURL(ms);
    });
  }

  // The element's events in the log, and a watchdog that fails the page if
  // no verdict is in after `seconds`.
  function watch(video, seconds) {
    for (const t of ['loadedmetadata', 'loadeddata', 'canplay', 'playing', 'waiting', 'seeking', 'seeked',
                     'resize', 'ended', 'error', 'durationchange'])
      video.addEventListener(t, () => note(t + '  t ' + video.currentTime.toFixed(2) + '  r' + video.readyState +
        (t === 'resize' ? '  ' + video.videoWidth + 'x' + video.videoHeight : '') +
        (t === 'durationchange' ? '  duration ' + video.duration : '') +
        (t === 'error' && video.error ? '  code ' + video.error.code : '')));
    setInterval(() => {
      if (!decided)
        document.title = 'r' + video.readyState + ' t' + video.currentTime.toFixed(1) + (video.ended ? ' ended' : '');
    }, 500);
    setTimeout(() => verdict(false, 'no verdict after ' + seconds + ' s (t ' + video.currentTime.toFixed(2) +
                              ', r' + video.readyState + ', buffered ' + ranges(video.buffered) + ')'), seconds * 1000);
  }

  // Once a second: buffered, and the playback quality's counts.
  function quality(video) {
    setInterval(() => {
      if (decided) return;
      // Absent with Media Source Extensions off (mse-plain.html's case).
      const q = video.getVideoPlaybackQuality ? video.getVideoPlaybackQuality() : null;
      note('buffered ' + ranges(video.buffered) +
           (q ? '  pictures ' + q.totalVideoFrames + ', dropped ' + q.droppedVideoFrames : ''));
    }, 1000);
  }

  // The fixture's own: H.264 High at level 3.1 (480p) and 3.2 (720p), AAC-LC.
  const VIDEO_480 = 'video/mp4; codecs="avc1.64001f"';
  const VIDEO_720 = 'video/mp4; codecs="avc1.640020"';
  const AUDIO = 'audio/mp4; codecs="mp4a.40.2"';
  return {note, verdict, ranges, clip, init, fragment, append, appendCut, remove, open, watch, quality,
          VIDEO_480, VIDEO_720, AUDIO};
})();
