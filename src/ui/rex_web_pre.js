// rexglue web runtime pre-js (linked with --pre-js by rexglue_configure_target). Runs in the page
// and in every pthread Worker, before the module starts. Two jobs:
//
// 1. Crash reports (rex/crash_report.h). In a Worker: Module.onAbort (every std::abort: FatalError,
//    asserts, REX_FATAL, unhandled guest exceptions, std::terminate) and the Worker's 'error' event
//    (wasm traps) call the rex_web_crash_collect export, which posts a JSON report to the page
//    thread. On the page: Module.__rexFatal receives it, adds page facts and a plain-text copy, and
//    calls Module.rexOnFatal(report) (or, if the page has none, Module.onAbort(text)). A Worker that
//    died without a report is caught by a window 'error' fallback after 1.5 s.
// 2. Page lifecycle (rex/web_page.h): visibilitychange / freeze / resume / pagehide / pageshow go into
//    a shared word the C++ side reads without a lock, and to Module.rexOnLifecycle(state, info).
//    Module.rexOnDiscarded() runs when this load follows a tab discard (Memory Saver), and
//    Module.rexOnDiscardRisk(info) when the page is hidden while no audio plays (Chrome's Memory
//    Saver and intensive throttling spare audible tabs only).
//
// Written for the default (non-closure) Emscripten output: Module, out, err, HEAPU32 and the
// _rex_web_crash_collect export are module-scope names here.
// (Leading ';': the generated code before a pre-js may end without one.)
;(function () {
  'use strict';
  var isPthread = typeof ENVIRONMENT_IS_PTHREAD != 'undefined' && ENVIRONMENT_IS_PTHREAD;
  var inWorker = typeof WorkerGlobalScope != 'undefined' && self instanceof WorkerGlobalScope;

  // Out of memory. Emscripten aborts with 'OOM' (release) or 'Cannot enlarge memory ...' (with
  // ASSERTIONS) when the wasm memory can not grow: either the build's limit is reached
  // (rexglue_configure_target WEB_MAXIMUM_MEMORY; fixed-size builds: the initial size) or the
  // browser / OS refused to commit more. Turn that into a message a player can act on.
  var MiB = function (n) {
    return Math.round(Number(n) / 1048576);
  };
  function isOom(what) {
    var s = String(what);
    return s === 'OOM' || /Cannot enlarge memory|^OOM\b|out of memory/i.test(s);
  }
  function oomText(what) {
    var size = 0;
    var max = 0;
    try {
      size = wasmMemory.buffer.byteLength;
    } catch (e) {}
    try {
      if (typeof getHeapMax == 'function') max = getHeapMax();
    } catch (e) {}
    var atLimit = max > 0 && size + (128 << 20) >= max;
    return 'out of memory: the wasm memory could not grow beyond ' + MiB(size) + ' MiB (limit ' +
      (max ? MiB(max) + ' MiB' : 'unknown') + '). ' +
      (atLimit
        ? 'The game needs more than this build allows: raise the limit (rexglue_configure_target ' +
          'WEB_MAXIMUM_MEMORY / REXGLUE_WEB_MAXIMUM_MEMORY).'
        : 'The browser refused to commit more memory: close other tabs or programs and reload.') +
      ' [' + String(what) + ']';
  }

  if (isPthread || inWorker) {
    // ---- Worker: collect on the crashing thread ----
    var sent = false;
    var collect = function (code, message, stack) {
      if (sent) return;
      sent = true;
      try {
        Module['__rexCrashJsJson'] = JSON.stringify({ message: String(message), stack: String(stack || '') });
        if (typeof _rex_web_crash_collect == 'function') _rex_web_crash_collect(code);
      } catch (e) {
        // The report itself failed (corrupt heap?); the page's window 'error' fallback still fires.
      }
    };
    // Defined here, so Emscripten does not install its proxy of the page's onAbort in this Worker:
    // the page side calls the page's own onAbort when it has no rexOnFatal.
    Module['onAbort'] = function (what) {
      collect(1, isOom(what) ? oomText(what) : what, new Error().stack);
    };
    self.addEventListener('error', function (ev) {
      collect(2, ev.message || (ev.error && ev.error.message) || 'error', ev.error && ev.error.stack);
    });
    return;
  }
  if (typeof window == 'undefined' || typeof document == 'undefined') return;

  // ---- Page: crash report delivery ----
  var pageAbort = Module['onAbort'];
  var delivered = null;
  var t0 = Date.now();

  function formatReport(r) {
    var L = [];
    L.push('ReXGlue crash report');
    L.push('kind: ' + r.kind);
    L.push('message: ' + r.message);
    if (r.note && r.note !== r.message) L.push('note: ' + r.note);
    if (r.js && r.js.message && r.js.message !== r.message) L.push('js: ' + r.js.message);
    if (r.thread) L.push('thread: tid ' + r.thread.tid + ', id ' + r.thread.id);
    if (r.uptime_s !== undefined) L.push('uptime: ' + Number(r.uptime_s).toFixed(1) + ' s');
    if (r.build) L.push('build: sdk ' + r.build.sdk + ', compiled ' + r.build.compiled);
    if (r.page) {
      L.push('page: ' + r.page.url);
      L.push('script: ' + r.page.script);
      L.push('userAgent: ' + r.page.userAgent);
      L.push('time: ' + r.page.time + (r.page.hidden ? ' (page hidden)' : ''));
    }
    if (r.cvars) {
      var names = Object.keys(r.cvars);
      L.push('cvars (' + names.length + ' modified):');
      for (var i = 0; i < names.length; ++i) L.push('  ' + names[i] + '=' + r.cvars[names[i]]);
    }
    L.push('stack:');
    L.push(r.stack || '(none)');
    if (r.threads) {
      L.push('threads:');
      L.push(r.threads);
    }
    if (r.log) {
      L.push('last log lines:');
      L.push(r.log);
    }
    if (r.followups && r.followups.length) L.push('later errors: ' + r.followups.join(' | '));
    return L.join('\n');
  }

  function deliver(r) {
    if (delivered) {
      (delivered.followups = delivered.followups || []).push(r.kind + ': ' + r.message);
      return;
    }
    delivered = r;
    r.js = r.js || {};
    r.message = r.note || r.js.message || r.message || r.kind;
    r.stack = r.stack || r.js.stack || '';
    var scripts = document.getElementsByTagName('script');
    var script = '';
    for (var i = 0; i < scripts.length; ++i) {
      if (/\.js(\?|$)/.test(scripts[i].src) && !/loader|gamedata/.test(scripts[i].src)) script = scripts[i].src;
    }
    r.page = {
      url: location.href,
      script: script,
      userAgent: navigator.userAgent,
      time: new Date().toISOString(),
      pageAge_s: (Date.now() - t0) / 1000,
      hidden: document.visibilityState === 'hidden',
    };
    r.text = formatReport(r);
    Module['rexLastFatal'] = r;
    console.error('[rex] fatal error report\n' + r.text);
    try {
      if (typeof Module['rexOnFatal'] == 'function') {
        Module['rexOnFatal'](r);
      } else if (typeof pageAbort == 'function') {
        pageAbort(r.kind + ': ' + r.message);
      }
    } catch (e) {
      console.error('[rex] rexOnFatal threw', e);
    }
  }

  // From rex_web_crash_collect (MAIN_THREAD_ASYNC_EM_ASM): ptr/len of the JSON in wasm memory.
  Module['__rexFatal'] = function (ptr, len) {
    var r;
    try {
      r = JSON.parse(UTF8ToString(ptr, len));
    } catch (e) {
      r = { kind: 'fatal', message: '(unreadable crash report: ' + e + ')' };
    }
    deliver(r);
  };
  // An abort on the page thread itself (rare: the runtime runs main() on a pthread).
  Module['onAbort'] = function (what) {
    if (isOom(what)) what = oomText(what);
    if (typeof pageAbort == 'function') {
      try {
        pageAbort(what);
      } catch (e) {}
    }
    var saved = pageAbort;
    pageAbort = null;  // already told
    deliver({ kind: 'abort', message: String(what), stack: new Error().stack });
    pageAbort = saved;
  };
  // A Worker that died before (or while) building its report: Emscripten rethrows the Worker's
  // ErrorEvent here.
  window.addEventListener('error', function (ev) {
    var m = String(ev.message || '');
    if (!/RuntimeError|ErrorEvent|worker sent an error|Aborted|unreachable|out of bounds|null function|memory access/i.test(m)) return;
    setTimeout(function () {
      if (!delivered) {
        deliver({
          kind: 'trap',
          message: m + (ev.filename ? ' at ' + ev.filename + ':' + ev.lineno : ''),
          stack: (ev.error && ev.error.stack) || '',
          note: '',
        });
      }
    }, 1500);
  });
  // The page creates the wasm memory before the module starts, and the browser commits all of its
  // initial size at once. When it can not, the constructor throws a RangeError and nothing would
  // start; report it as out of memory. One shot: only the module's own memory goes through this.
  if (typeof WebAssembly == 'object' && typeof WebAssembly.Memory == 'function') {
    var NativeMemory = WebAssembly.Memory;
    var CheckedMemory = function Memory(desc) {
      WebAssembly.Memory = NativeMemory;
      try {
        return new NativeMemory(desc);
      } catch (e) {
        var bytes = desc && desc.initial !== undefined ? Number(desc.initial) * 65536 : 0;
        deliver({
          kind: 'oom',
          message: 'out of memory: the browser could not allocate the ' + MiB(bytes) +
            ' MiB initial wasm memory (' + e + '). Close other tabs or programs and reload; ' +
            'a build with a smaller initial size (rexglue_configure_target WEB_INITIAL_MEMORY) ' +
            'needs less up front.',
          stack: (e && e.stack) || '',
        });
        throw e;
      }
    };
    CheckedMemory.prototype = NativeMemory.prototype;
    WebAssembly.Memory = CheckedMemory;
  }

  // ---- Page: lifecycle ----
  var stateAddr = 0;
  var hidden = document.visibilityState === 'hidden';
  var frozen = false;
  var hiddenCount = hidden ? 1 : 0;
  var hiddenAt = hidden ? performance.now() : 0;

  function publish() {
    if (!stateAddr) return;
    var view = typeof HEAPU32 != 'undefined' ? HEAPU32 : null;
    if (!view) return;
    var i = stateAddr / 4;
    Atomics.store(view, i, (hidden ? 1 : 0) | (frozen ? 2 : 0));
    Atomics.store(view, i + 1, hiddenCount);
  }
  Module['__rexSetPageStateAddr'] = function (addr) {
    stateAddr = Number(addr);
    publish();
  };
  function audible() {
    var a = Module['rexAudio'];
    return !!(a && a.ctx && a.ctx.state === 'running');
  }
  function tell(state, info) {
    publish();
    var msg = '[rex] page ' + state + (info && info.hidden_s !== undefined ? ' (was hidden ' + info.hidden_s.toFixed(1) + ' s)' : '');
    if (typeof out == 'function') out(msg);
    else console.log(msg);
    if (typeof Module['rexOnLifecycle'] == 'function') {
      try {
        Module['rexOnLifecycle'](state, info || {});
      } catch (e) {
        console.error('[rex] rexOnLifecycle threw', e);
      }
    }
  }
  document.addEventListener('visibilitychange', function () {
    var h = document.visibilityState === 'hidden';
    if (h === hidden) return;
    hidden = h;
    if (h) {
      ++hiddenCount;
      hiddenAt = performance.now();
      var info = { audible: audible() };
      tell('hidden', info);
      if (!info.audible && typeof Module['rexOnDiscardRisk'] == 'function') {
        try {
          Module['rexOnDiscardRisk']({ reason: 'hidden without audio: Memory Saver may discard this tab' });
        } catch (e) {}
      }
    } else {
      tell('visible', { hidden_s: (performance.now() - hiddenAt) / 1000 });
    }
  });
  document.addEventListener('freeze', function () {
    frozen = true;
    tell('frozen', {});
  });
  document.addEventListener('resume', function () {
    frozen = false;
    tell('resumed', {});
  });
  window.addEventListener('pagehide', function (ev) {
    tell('pagehide', { persisted: !!ev.persisted });
  });
  window.addEventListener('pageshow', function (ev) {
    if (ev.persisted) tell('pageshow', { persisted: true });
  });
  // ---- Page: opt-in touch gamepad (Module.rexTouchGamepad) ----
  // A virtual Standard Gamepad drawn over the canvas and driven by Pointer Events. It is added to
  // navigator.getGamepads(), so the SDK's gamepad poll (dom_input_bridge.cpp) reads it exactly like
  // a real pad (lock-free into the XInput slots; a pad that is used takes XInput user 0).
  //   Module.rexTouchGamepad = true | {
  //     show: 'touch' (default: on the first touch anywhere) | 'always',
  //     canvas: CSS selector of the element to cover (default Module.canvas / '#canvas'),
  //     opacity: 0.45, size: 1 (scale),
  //     layout: [{type:'stick', id:'ls'|'rs', x, y, r} | {type:'button', id, x, y, r, label}, ...]
  //   }
  // x, y, r are fractions of the covered element's height (x from the left edge when >= 0, from
  // the right edge when < 0). Button ids: a b x y lb rb lt rt back start ls rs up down left right
  // guide. Module.rexTouchGamepadState() returns the current {buttons, axes} (tests).
  var touchCfg = Module['rexTouchGamepad'];
  if (touchCfg) {
    installTouchGamepad(touchCfg === true ? {} : touchCfg);
  }
  function installTouchGamepad(cfg) {
    var BTN = { a: 0, b: 1, x: 2, y: 3, lb: 4, rb: 5, lt: 6, rt: 7, back: 8, start: 9, ls: 10, rs: 11,
      up: 12, down: 13, left: 14, right: 15, guide: 16 };
    var layout = cfg.layout || [
      { type: 'stick', id: 'ls', x: 0.22, y: 0.72, r: 0.17 },
      { type: 'stick', id: 'rs', x: -0.42, y: 0.75, r: 0.15 },
      { type: 'button', id: 'a', x: -0.13, y: 0.78, r: 0.065, label: 'A' },
      { type: 'button', id: 'b', x: -0.04, y: 0.66, r: 0.065, label: 'B' },
      { type: 'button', id: 'x', x: -0.22, y: 0.66, r: 0.065, label: 'X' },
      { type: 'button', id: 'y', x: -0.13, y: 0.54, r: 0.065, label: 'Y' },
      { type: 'button', id: 'lt', x: 0.08, y: 0.12, r: 0.07, label: 'LT' },
      { type: 'button', id: 'lb', x: 0.08, y: 0.30, r: 0.06, label: 'LB' },
      { type: 'button', id: 'rt', x: -0.08, y: 0.12, r: 0.07, label: 'RT' },
      { type: 'button', id: 'rb', x: -0.08, y: 0.30, r: 0.06, label: 'RB' },
      { type: 'button', id: 'back', x: 0.40, y: 0.08, r: 0.045, label: '\u25C1' },
      { type: 'button', id: 'start', x: -0.40, y: 0.08, r: 0.045, label: '\u25B7' },
      { type: 'button', id: 'up', x: 0.50, y: 0.70, r: 0.045, label: '\u25B2' },
      { type: 'button', id: 'down', x: 0.50, y: 0.88, r: 0.045, label: '\u25BC' },
      { type: 'button', id: 'left', x: 0.42, y: 0.79, r: 0.045, label: '\u25C0' },
      { type: 'button', id: 'right', x: 0.58, y: 0.79, r: 0.045, label: '\u25B6' },
    ];
    var scale = cfg.size || 1;
    var buttons = [];
    for (var i = 0; i < 17; ++i) buttons.push({ pressed: false, touched: false, value: 0 });
    var axes = [0, 0, 0, 0];
    var pad = { id: 'ReXGlue touch gamepad (STANDARD GAMEPAD)', index: 0, connected: true, mapping: 'standard',
      timestamp: performance.now(), axes: axes, buttons: buttons, vibrationActuator: null };
    var active = false;
    var root = null;
    var controls = [];
    var origGet = navigator.getGamepads ? navigator.getGamepads.bind(navigator) : function () { return []; };
    navigator.getGamepads = function () {
      var list = Array.prototype.slice.call(origGet() || []);
      if (!active) return list;
      while (list.length < 4) list.push(null);
      var slot = -1;
      for (var i = list.length - 1; i >= 0; --i) if (!list[i]) { slot = i; break; }
      if (slot < 0) return list;
      pad.index = slot;
      list[slot] = pad;
      return list;
    };
    Module['rexTouchGamepadState'] = function () {
      return { active: active, index: pad.index, buttons: buttons.map(function (b) { return b.value; }), axes: axes.slice() };
    };
    function target() {
      return (cfg.canvas && document.querySelector(cfg.canvas)) || Module['canvas'] || document.getElementById('canvas');
    }
    function setButton(id, on) {
      var b = buttons[BTN[id]];
      if (!b) return;
      b.pressed = b.touched = on;
      b.value = on ? 1 : 0;
      pad.timestamp = performance.now();
    }
    function place() {
      var el = target();
      if (!root || !el) return;
      var rc = el.getBoundingClientRect();
      root.style.left = rc.left + 'px';
      root.style.top = rc.top + 'px';
      root.style.width = rc.width + 'px';
      root.style.height = rc.height + 'px';
      for (var i = 0; i < controls.length; ++i) {
        var c = controls[i];
        var d = 2 * c.def.r * scale * rc.height;
        var cx = c.def.x >= 0 ? c.def.x * rc.height : rc.width + c.def.x * rc.height;
        var cy = c.def.y * rc.height;
        c.el.style.width = c.el.style.height = d + 'px';
        c.el.style.left = cx - d / 2 + 'px';
        c.el.style.top = cy - d / 2 + 'px';
        c.el.style.fontSize = d * 0.38 + 'px';
      }
    }
    function makeControl(def) {
      var el = document.createElement('div');
      el.dataset.rexTouch = def.id;
      el.style.cssText = 'position:absolute;border-radius:50%;pointer-events:auto;touch-action:none;user-select:none;' +
        'display:flex;align-items:center;justify-content:center;color:#fff;font:600 14px system-ui,sans-serif;' +
        'background:rgba(255,255,255,0.18);border:2px solid rgba(255,255,255,0.55);box-sizing:border-box';
      var c = { def: def, el: el, pointer: -1, knob: null };
      if (def.type === 'stick') {
        var knob = document.createElement('div');
        knob.style.cssText = 'width:45%;height:45%;border-radius:50%;background:rgba(255,255,255,0.5);pointer-events:none';
        el.appendChild(knob);
        c.knob = knob;
      } else {
        el.textContent = def.label || def.id.toUpperCase();
      }
      var axisBase = def.id === 'rs' ? 2 : 0;
      function moveStick(ev) {
        var rc = el.getBoundingClientRect();
        var r = rc.width / 2;
        var dx = (ev.clientX - (rc.left + r)) / r;
        var dy = (ev.clientY - (rc.top + r)) / r;
        var len = Math.hypot(dx, dy);
        if (len > 1) {
          dx /= len;
          dy /= len;
        }
        axes[axisBase] = dx;
        axes[axisBase + 1] = dy;
        pad.timestamp = performance.now();
        c.knob.style.transform = 'translate(' + dx * r * 0.6 + 'px,' + dy * r * 0.6 + 'px)';
      }
      el.addEventListener('pointerdown', function (ev) {
        if (c.pointer >= 0) return;
        c.pointer = ev.pointerId;
        try {
          el.setPointerCapture(ev.pointerId);
        } catch (e) {}
        ev.preventDefault();
        el.style.background = 'rgba(255,255,255,0.4)';
        if (def.type === 'stick') moveStick(ev);
        else setButton(def.id, true);
      });
      el.addEventListener('pointermove', function (ev) {
        if (ev.pointerId === c.pointer && def.type === 'stick') moveStick(ev);
      });
      var up = function (ev) {
        if (ev.pointerId !== c.pointer) return;
        c.pointer = -1;
        el.style.background = 'rgba(255,255,255,0.18)';
        if (def.type === 'stick') {
          axes[axisBase] = axes[axisBase + 1] = 0;
          pad.timestamp = performance.now();
          c.knob.style.transform = '';
        } else {
          setButton(def.id, false);
        }
      };
      el.addEventListener('pointerup', up);
      el.addEventListener('pointercancel', up);
      el.addEventListener('lostpointercapture', up);
      return c;
    }
    function show() {
      if (active) return;
      active = true;
      root = document.createElement('div');
      root.id = 'rex-touch-gamepad';
      root.style.cssText = 'position:fixed;z-index:15;pointer-events:none;opacity:' + (cfg.opacity || 0.45);
      for (var i = 0; i < layout.length; ++i) {
        var c = makeControl(layout[i]);
        controls.push(c);
        root.appendChild(c.el);
      }
      document.body.appendChild(root);
      place();
      window.addEventListener('resize', place);
      window.addEventListener('scroll', place, true);
      if (typeof ResizeObserver == 'function' && target()) new ResizeObserver(place).observe(target());
      if (typeof out == 'function') out('[rex] touch gamepad shown (' + layout.length + ' controls)');
    }
    if (cfg.show === 'always') {
      if (document.body) show();
      else document.addEventListener('DOMContentLoaded', show);
    } else {
      window.addEventListener('pointerdown', function first(ev) {
        if (ev.pointerType !== 'touch') return;
        window.removeEventListener('pointerdown', first, true);
        show();
      }, true);
    }
  }

  if (document.wasDiscarded) {
    setTimeout(function () {
      tell('restored-after-discard', {});
      if (typeof Module['rexOnDiscarded'] == 'function') {
        try {
          Module['rexOnDiscarded']();
        } catch (e) {}
      }
    }, 0);
  }
})();
