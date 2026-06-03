/* shade-runtime.js — browser/V8 emulator of the lamp's on-device PLBC
 * runtime. Lets a client preview a `shade()` script (the contract the
 * device compiler in lampos/components/plbc expects) without a round-trip
 * to the lamp.
 *
 * THIS FILE IS THE CANONICAL COPY. The website loads it directly; the
 * Android and Windows clients embed a verbatim copy as an app resource
 * (see android .../ai/ and windows .../Ai/). Keep those copies in sync.
 *
 * Contract emulated (must match lampos/components/plbc/compile.c + vm.c):
 *   - one `function shade(x, y, idx, frame, base, params)` — a per-pixel
 *     shader that calls emit()/emitBright() once per LED.
 *   - reserved names in scope: x, y, idx, frame, w, h, base, time,
 *     playStart, params.
 *   - `// @param NAME MIN..MAX = DEF [DESC]`  -> params.NAME
 *   - `// @state NAME : DEF`                  -> bare NAME, frame-scoped
 *   - `// @state.pixel NAME : DEF`            -> NAME.pixel, per-LED state
 *   - built-ins: sinLUT, cosLUT, floor, ceil, round, abs, sqrt, pow, min,
 *     max, clamp01, hash, random, emit, emitBright.
 *
 * Numerics mirror vm.c exactly (256-entry sin LUT, Wang-mix hash, div/mod
 * by zero -> 0) so a preview here looks like the lamp.
 */
var PLShade = (function () {
  "use strict";

  /* ---- built-ins (match lampos/components/plbc/vm.c) ------------------ */

  var SIN_N = 256;
  var SIN_SCALE = SIN_N / (2 * Math.PI);
  var SIN_LUT = [];
  for (var _i = 0; _i < SIN_N; _i++) {
    SIN_LUT.push(Math.sin(2 * Math.PI * _i / SIN_N));
  }
  function sinLUT(x) {
    var idx = (x * SIN_SCALE) | 0;            // (int32) truncation
    return SIN_LUT[(idx >>> 0) & (SIN_N - 1)];
  }
  function cosLUT(x) { return sinLUT(x + Math.PI / 2); }

  function hash(v) {
    // Thomas Wang 32-bit mixer on (uint32)(int32)v -> [0,1).
    var x = (v | 0) >>> 0;
    x = (x ^ 61) ^ (x >>> 16);
    x = (x + (x << 3)) >>> 0;
    x = x ^ (x >>> 4);
    x = Math.imul(x, 0x27d4eb2d) >>> 0;
    x = x ^ (x >>> 15);
    return (x >>> 0) * (1.0 / 4294967296.0);
  }

  function clamp01(v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
  function pdiv(a, b) { return b === 0 ? 0 : a / b; }       // PLBC_DIV
  function pmod(a, b) { return b === 0 ? 0 : a % b; }       // PLBC_MOD

  var BUILTINS = {
    sinLUT: sinLUT, cosLUT: cosLUT,
    floor: Math.floor, ceil: Math.ceil, round: Math.round,
    abs: Math.abs, sqrt: Math.sqrt, pow: Math.pow,
    min: Math.min, max: Math.max,
    clamp01: clamp01, hash: hash, random: Math.random,
  };

  /* ---- annotation parsing -------------------------------------------- */

  // `// @param NAME MIN..MAX = DEF [DESC]`
  var RE_PARAM = /^\s*\/\/\s*@param\s+([A-Za-z_]\w*)\s+(-?[\d.eE+]+)\s*\.\.\s*(-?[\d.eE+]+)\s*=\s*(-?[\d.eE+]+)\s*(.*)$/;
  // `// @param NAME switch = DEF [DESC]` — a 0/1 toggle (Phase 41).
  var RE_PARAM_SWITCH = /^\s*\/\/\s*@param\s+([A-Za-z_]\w*)\s+switch\s*=\s*(-?[\d.eE+]+)\s*(.*)$/;
  // `// @mode strip|mirror` — declared wormhole render mode (Phase 41).
  var RE_MODE = /^\s*\/\/\s*@mode\s+(strip|mirror)\b/;
  // `// @modeSwitch` — the effect works in both modes, so the user may change
  // it; absent means it's locked to @mode (Phase 41).
  var RE_MODESWITCH = /^\s*\/\/\s*@modeSwitch\b/;
  // `// @state.pixel NAME : DEF`  /  `// @state NAME : DEF`
  var RE_PSTATE = /^\s*\/\/\s*@state\.pixel\s+([A-Za-z_]\w*)\s*:\s*(-?[\d.eE+]+)/;
  var RE_STATE  = /^\s*\/\/\s*@state\s+([A-Za-z_]\w*)\s*:\s*(-?[\d.eE+]+)/;

  function parseAnnotations(source) {
    var lines = String(source).split("\n");
    var params = [], frameState = [], pixelState = [], mode = null, modeSwitch = false;
    for (var i = 0; i < lines.length; i++) {
      var m = RE_PARAM.exec(lines[i]);
      if (m) {
        params.push({
          name: m[1], min: parseFloat(m[2]), max: parseFloat(m[3]),
          def: parseFloat(m[4]), desc: (m[5] || "").trim(), type: "range",
        });
        continue;
      }
      m = RE_PARAM_SWITCH.exec(lines[i]);
      if (m) {
        params.push({
          name: m[1], min: 0, max: 1,
          def: parseFloat(m[2]), desc: (m[3] || "").trim(), type: "switch",
        });
        continue;
      }
      m = RE_MODE.exec(lines[i]);
      if (m) { mode = m[1]; continue; }
      if (RE_MODESWITCH.test(lines[i])) { modeSwitch = true; continue; }
      m = RE_PSTATE.exec(lines[i]);
      if (m) { pixelState.push({ name: m[1], def: parseFloat(m[2]) }); continue; }
      m = RE_STATE.exec(lines[i]);
      if (m) { frameState.push({ name: m[1], def: parseFloat(m[2]) }); }
    }
    return { params: params, frameState: frameState, pixelState: pixelState,
             mode: mode, modeSwitch: modeSwitch };
  }

  function clampByte(v) {
    v = v | 0;                                // (int) truncation
    return v < 0 ? 0 : (v > 255 ? 255 : v);
  }

  /* ---- runner -------------------------------------------------------- */

  // Build a reusable runner for one script. Throws on a syntax error or a
  // missing `shade` function. The runner keeps @state / @state.pixel
  // buffers across render() calls, just like the device.
  function makeRunner(source) {
    var ann = parseAnnotations(source);

    var fsDecl = "", fsLoad = "", fsStore = "";
    for (var i = 0; i < ann.frameState.length; i++) {
      var n = ann.frameState[i].name;
      fsDecl  += "var " + n + ";\n";
      fsLoad  += n + " = __io.fs[" + i + "];\n";
      fsStore += "__io.fs[" + i + "] = " + n + ";\n";
    }
    var psDecl = "", psLoad = "", psStore = "";
    for (var j = 0; j < ann.pixelState.length; j++) {
      var pn = ann.pixelState[j].name;
      psDecl  += "var " + pn + " = { pixel: 0 };\n";
      psLoad  += "    " + pn + ".pixel = __io.ps[" + j + "][idx];\n";
      psStore += "    __io.ps[" + j + "][idx] = " + pn + ".pixel;\n";
    }

    var body =
      "var w = __io.w, h = __io.h, frame = __io.frame, time = __io.time;\n" +
      "var playStart = __io.playStart;\n" +
      "var base = __io.base, params = __io.params, out = __io.out;\n" +
      "var sinLUT = __b.sinLUT, cosLUT = __b.cosLUT, floor = __b.floor,\n" +
      "    ceil = __b.ceil, round = __b.round, abs = __b.abs, sqrt = __b.sqrt,\n" +
      "    pow = __b.pow, min = __b.min, max = __b.max, clamp01 = __b.clamp01,\n" +
      "    hash = __b.hash, random = __b.random;\n" +
      fsDecl + psDecl +
      "var __r = 0, __g = 0, __b2 = 0;\n" +
      "function emit(r, g, b) { __r = __cb(r); __g = __cb(g); __b2 = __cb(b); }\n" +
      "function emitBright(v) {\n" +
      "  v = v < 0 ? 0 : (v > 1 ? 1 : v);\n" +
      "  __r = (base.r * v) | 0; __g = (base.g * v) | 0; __b2 = (base.b * v) | 0;\n" +
      "}\n" +
      "/* ---- user script ---- */\n" + source + "\n/* ---- end user script ---- */\n" +
      "if (typeof shade !== 'function')\n" +
      "  throw new Error('script must define a function named shade');\n" +
      fsLoad +
      "for (var idx = 0; idx < w * h; idx++) {\n" +
      "  var x = idx % w, y = (idx - x) / w;\n" +
      psLoad +
      "  __r = 0; __g = 0; __b2 = 0;\n" +
      "  shade(x, y, idx, frame, base, params);\n" +
      "  out[idx * 3] = __r; out[idx * 3 + 1] = __g; out[idx * 3 + 2] = __b2;\n" +
      psStore +
      "}\n" +
      fsStore;

    var fn = new Function("__io", "__b", "__cb", body);

    var fs = [];
    for (var k = 0; k < ann.frameState.length; k++) fs.push(ann.frameState[k].def);
    var ps = [];          // allocated lazily in render() once w*h is known
    var psW = 0, psH = 0;
    // Per-playback seed — `playStart` is ms-since-boot at /api/play on the
    // device; here, a stable random integer so stateless scripts (fade.js)
    // vary between preview sessions. reset() reseeds it.
    var playStart = (Math.random() * 0x7fffffff) | 0;

    return {
      params: ann.params,
      frameState: ann.frameState,
      pixelState: ann.pixelState,

      // Reset @state / @state.pixel buffers to their declared defaults.
      reset: function () {
        fs = [];
        for (var a = 0; a < ann.frameState.length; a++) fs.push(ann.frameState[a].def);
        ps = []; psW = 0; psH = 0;
        playStart = (Math.random() * 0x7fffffff) | 0;
      },

      // Render one frame. opts: { w, h, frame, time, base:{r,g,b}, params }.
      // Returns a flat [r,g,b,r,g,b,...] array of length w*h*3.
      render: function (opts) {
        var w = opts.w | 0, h = opts.h | 0;
        if (w <= 0 || h <= 0) throw new Error("invalid panel size");
        if (w !== psW || h !== psH || ps.length !== ann.pixelState.length) {
          ps = [];
          for (var s = 0; s < ann.pixelState.length; s++) {
            var buf = [];
            for (var p = 0; p < w * h; p++) buf.push(ann.pixelState[s].def);
            ps.push(buf);
          }
          psW = w; psH = h;
        }
        var params = {};
        for (var q = 0; q < ann.params.length; q++) {
          var pp = ann.params[q];
          var ov = opts.params ? opts.params[pp.name] : undefined;
          params[pp.name] = (typeof ov === "number") ? ov : pp.def;
        }
        var base = opts.base || { r: 128, g: 128, b: 128 };
        var io = {
          w: w, h: h, frame: opts.frame | 0, time: +(opts.time || 0),
          playStart: (typeof opts.playStart === "number") ? opts.playStart : playStart,
          base: { r: base.r | 0, g: base.g | 0, b: base.b | 0 },
          params: params, out: new Array(w * h * 3), fs: fs, ps: ps,
        };
        fn(io, BUILTINS, clampByte);
        return io.out;
      },
    };
  }

  return {
    parseAnnotations: parseAnnotations,
    makeRunner: makeRunner,
    clampByte: clampByte,
  };
})();

if (typeof module !== "undefined" && module.exports) module.exports = PLShade;
