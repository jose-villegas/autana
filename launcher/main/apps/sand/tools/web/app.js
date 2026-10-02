// The page side of web_sand.c: reads gravity and the pointer, then steps,
// pours and renders once per animation frame, the loop app_sand.c runs on
// the board. Everything the simulation decides stays in C.

const COUNTS_PER_G = 4096; // web_sand.c's WEB_COUNTS_PER_G

const MODE_PAINT = 0;

const canvas = document.getElementById("canvas");
const ctx = canvas.getContext("2d", { alpha: false });
const loadingEl = document.getElementById("loading");
const paletteEl = document.getElementById("palette");
const sourceToggle = document.getElementById("source-toggle");
const clearBtn = document.getElementById("clear-btn");
const tiltBtn = document.getElementById("tilt-btn");
const tiltPad = document.getElementById("tilt-pad");
const tiltHandle = document.getElementById("tilt-handle");
const qualitySelect = document.getElementById("quality");
const simSpeedSelect = document.getElementById("sim-speed");
const colorModeSelect = document.getElementById("color-mode");
const ditherModeSelect = document.getElementById("dither-mode");
const modeButtons = [...document.querySelectorAll(".mode")];
const brushSizeInput = document.getElementById("brush-size");
const brushSizeCaption = document.getElementById("brush-size-caption");
const brushSizeValue = document.getElementById("brush-size-value");
const fsBtn = document.getElementById("fullscreen-btn");
const fsTarget = document.getElementById("app");
const orientationBtn = document.getElementById("orientation-btn");

let Module, sand;
let screenW = 448, screenH = 368;
let pixelsPtr = 0;
let mode = MODE_PAINT;
let brush = 0;

// The board turned a quarter turn, the way it is usually held.
let landscape = true;

let pointerDown = false;
let pointerJustPressed = false;
let pointerX = 0, pointerY = 0;

let tiltEnabled = false;
let tiltAx = 0, tiltAy = COUNTS_PER_G, tiltAz = 0;

let padActive = false;
let padAx = 0, padAy = COUNTS_PER_G, padAz = 0;

function toHex(rgb) {
  return "#" + (rgb >>> 0).toString(16).padStart(6, "0");
}

function titleCase(name) {
  return name.charAt(0) + name.slice(1).toLowerCase();
}

function selectBrush(i) {
  brush = i;
  sand.set_brush(i);
  [...paletteEl.children].forEach((c, ci) => c.classList.toggle("selected", ci === i));
}

function buildPalette() {
  paletteEl.innerHTML = "";
  for (let i = 0; i < sand.brush_count(); i++) {
    const el = document.createElement("div");
    el.className = "swatch";
    el.style.background = toHex(sand.brush_swatch(i));
    el.textContent = titleCase(sand.brush_name(i));
    el.addEventListener("pointerdown", (e) => {
      e.preventDefault();
      selectBrush(i);
    });
    paletteEl.appendChild(el);
  }
  selectBrush(brush);
}

// The device brush screen's captions, by mode.
const SIZE_CAPTIONS = ["Pour size", "Erase size", "Boom size"];

// Each mode keeps its own radius in C, so the slider re-reads it on a mode change.
function syncBrushSize() {
  brushSizeCaption.textContent = SIZE_CAPTIONS[mode];
  if (sand) brushSizeInput.value = sand.radius(mode);
  brushSizeValue.textContent = `${brushSizeInput.value} px`;
}

brushSizeInput.addEventListener("input", () => {
  if (sand) sand.set_radius(mode, Number(brushSizeInput.value));
  syncBrushSize();
});

function setMode(next) {
  mode = next;
  modeButtons.forEach((b) => b.classList.toggle("active", Number(b.dataset.mode) === mode));
  syncBrushSize();
}
modeButtons.forEach((b) => b.addEventListener("click", () => setMode(Number(b.dataset.mode))));
setMode(MODE_PAINT);

clearBtn.addEventListener("click", () => sand && sand.clear());

// A button, because iOS only grants orientation events after a user gesture
// and a permission prompt. It toggles afterwards without asking again, and
// with it off the pad (or plain straight down) supplies gravity.
let tiltListenerAttached = false;

function setTiltEnabled(enabled) {
  tiltEnabled = enabled;
  tiltBtn.textContent = enabled ? "Disable device tilt" : "Use device tilt";
  tiltPad.classList.toggle("disabled", enabled);
}

if (typeof DeviceOrientationEvent !== "undefined") {
  tiltBtn.hidden = false;
  tiltBtn.addEventListener("click", async () => {
    if (tiltListenerAttached) {
      setTiltEnabled(!tiltEnabled);
      return;
    }
    if (typeof DeviceOrientationEvent.requestPermission === "function") {
      try {
        if (await DeviceOrientationEvent.requestPermission() !== "granted") return;
      } catch {
        return;
      }
    }
    window.addEventListener("deviceorientation", onDeviceOrientation);
    tiltListenerAttached = true;
    setTiltEnabled(true);
  });
}

// gamma is the left/right lean and beta the front/back one. An estimate, not
// sensor fusion: enough for the sand to slide the way the phone leans.
function onDeviceOrientation(e) {
  const gx = Math.max(-1, Math.min(1, Math.sin((e.gamma || 0) * Math.PI / 180)));
  const gy = Math.max(-1, Math.min(1, Math.sin((e.beta || 0) * Math.PI / 180)));
  tiltAx = Math.round(gx * COUNTS_PER_G);
  tiltAy = Math.round(gy * COUNTS_PER_G);
  tiltAz = Math.round(Math.sqrt(Math.max(0, 1 - gx * gx - gy * gy)) * COUNTS_PER_G);
}

// A virtual accelerometer: drag toward where gravity should pull (x right,
// y down, as on the screen). It stays where it is released, the way a phone
// held at an angle stays tilted; a double click levels it again.
let padDragging = false;

function padVectorFromEvent(e) {
  const r = tiltPad.getBoundingClientRect();
  const radius = r.width / 2;
  let dx = (e.clientX - (r.left + radius)) / radius;
  let dy = (e.clientY - (r.top + r.height / 2)) / radius;
  const len = Math.hypot(dx, dy);
  if (len > 1) {
    dx /= len;
    dy /= len;
  }
  return [dx, dy];
}

function setPad(dx, dy) {
  padAx = Math.round(dx * COUNTS_PER_G);
  padAy = Math.round(dy * COUNTS_PER_G);
  padAz = Math.round(Math.sqrt(Math.max(0, 1 - dx * dx - dy * dy)) * COUNTS_PER_G);
  padActive = true;
  tiltPad.classList.add("live");
  const radius = tiltPad.getBoundingClientRect().width / 2;
  tiltHandle.style.transform = `translate(${dx * radius}px, ${dy * radius}px)`;
}

function resetPad() {
  padActive = false;
  [padAx, padAy, padAz] = [0, COUNTS_PER_G, 0];
  tiltPad.classList.remove("live");
  tiltHandle.style.transform = "";
}

tiltPad.addEventListener("pointerdown", (e) => {
  if (tiltEnabled) return;
  tiltPad.setPointerCapture(e.pointerId);
  tiltPad.classList.add("active");
  padDragging = true;
  setPad(...padVectorFromEvent(e));
});
tiltPad.addEventListener("pointermove", (e) => {
  if (padDragging) setPad(...padVectorFromEvent(e));
});
["pointerup", "pointercancel"].forEach((ev) =>
  tiltPad.addEventListener(ev, () => {
    padDragging = false;
    tiltPad.classList.remove("active");
  }));
tiltPad.addEventListener("dblclick", resetPad);

function canvasPoint(clientX, clientY) {
  const r = canvas.getBoundingClientRect();
  const x = (clientX - r.left) / r.width * screenW;
  const y = (clientY - r.top) / r.height * screenH;
  return [
    Math.max(0, Math.min(screenW - 1, Math.round(x))),
    Math.max(0, Math.min(screenH - 1, Math.round(y))),
  ];
}

canvas.addEventListener("pointerdown", (e) => {
  canvas.setPointerCapture(e.pointerId);
  [pointerX, pointerY] = canvasPoint(e.clientX, e.clientY);
  pointerDown = true;
  pointerJustPressed = true;
});
canvas.addEventListener("pointermove", (e) => {
  if (pointerDown) [pointerX, pointerY] = canvasPoint(e.clientX, e.clientY);
});
["pointerup", "pointercancel", "pointerleave"].forEach((ev) =>
  canvas.addEventListener(ev, () => { pointerDown = false; }));

// The whole #app goes fullscreen, not the canvas alone, so the controls stay reachable.
const requestFs = fsTarget.requestFullscreen || fsTarget.webkitRequestFullscreen;
const exitFs = document.exitFullscreen || document.webkitExitFullscreen;
if (requestFs) {
  fsBtn.hidden = false;
  fsBtn.addEventListener("click", () => {
    if (document.fullscreenElement || document.webkitFullscreenElement) {
      exitFs.call(document);
    } else {
      requestFs.call(fsTarget).catch(() => {});
    }
  });
  ["fullscreenchange", "webkitfullscreenchange"].forEach((ev) =>
    document.addEventListener(ev, () => {
      const active = !!(document.fullscreenElement || document.webkitFullscreenElement);
      fsBtn.textContent = active ? "Exit fullscreen" : "Fullscreen";
    }));
}

// Anything that rebuilds the grid comes through here, so the canvas size
// always matches the grid that exists.
function reinitSim() {
  sand.init(Number(qualitySelect.value), landscape ? 1 : 0);
  screenW = sand.screen_w();
  screenH = sand.screen_h();
  canvas.width = screenW;
  canvas.height = screenH;
  canvas.style.setProperty("--aspect", String(screenW / screenH));
}

qualitySelect.addEventListener("change", reinitSim);

simSpeedSelect.addEventListener("change", () => {
  sand.set_sim_speed(Math.round(Number(simSpeedSelect.value) * 256));
});

// Dither only means something at 16 colours, as on the device's own menu.
function updateDitherEnabled() {
  ditherModeSelect.disabled = colorModeSelect.value !== "2";
}
colorModeSelect.addEventListener("change", () => {
  sand.set_color_mode(Number(colorModeSelect.value));
  updateDitherEnabled();
});
ditherModeSelect.addEventListener("change", () => {
  sand.set_dither_mode(Number(ditherModeSelect.value));
});
updateDitherEnabled();

orientationBtn.addEventListener("click", () => {
  landscape = !landscape;
  orientationBtn.textContent = landscape ? "Portrait" : "Landscape";
  reinitSim();
});

let lastFrame = 0;

function frame(now) {
  requestAnimationFrame(frame);

  let dt = lastFrame ? now - lastFrame : 16;
  lastFrame = now;
  dt = Math.max(1, Math.min(dt, 100)); // a background tab must not come back to one giant step

  // The sensor first, then the pad, then plain straight down: the board's
  // own fallback when it has no IMU.
  let g = [0, COUNTS_PER_G, 0];
  if (tiltEnabled) {
    g = [tiltAx, tiltAy, tiltAz];
  } else if (padActive) {
    g = [padAx, padAy, padAz];
  }
  sand.step(dt, ...g);

  // A press counts as down on its own frame even if the release already
  // arrived, or a fast tap would never reach the emitter placement.
  const down = pointerDown || pointerJustPressed;
  sand.input(mode, down ? 1 : 0, pointerJustPressed ? 1 : 0, sourceToggle.checked ? 1 : 0, pointerX, pointerY, dt);
  pointerJustPressed = false;

  sand.render();
  // A fresh view each frame: memory growth swaps the buffer under any view held across it.
  const bytes = new Uint8ClampedArray(Module.HEAPU8.buffer, pixelsPtr, screenW * screenH * 4);
  ctx.putImageData(new ImageData(bytes, screenW, screenH), 0, 0);
}

SandModule().then((mod) => {
  Module = mod;
  const fn = (name, ret, args) => Module.cwrap(`web_${name}`, ret, args);
  const n = (k) => Array(k).fill("number");
  sand = {
    init: fn("init", "number", n(2)),
    step: fn("step", null, n(4)),
    input: fn("input", null, n(7)),
    clear: fn("clear", null, []),
    render: fn("render", null, []),
    pixels_ptr: fn("pixels_ptr", "number", []),
    screen_w: fn("screen_w", "number", []),
    screen_h: fn("screen_h", "number", []),
    brush_count: fn("brush_count", "number", []),
    brush_name: fn("brush_name", "string", n(1)),
    brush_swatch: fn("brush_swatch", "number", n(1)),
    set_brush: fn("set_brush", null, n(1)),
    set_radius: fn("set_radius", "number", n(2)),
    radius: fn("radius", "number", n(1)),
    set_sim_speed: fn("set_sim_speed", null, n(1)),
    set_color_mode: fn("set_color_mode", null, n(1)),
    set_dither_mode: fn("set_dither_mode", null, n(1)),
  };
  pixelsPtr = sand.pixels_ptr();

  reinitSim();
  buildPalette();
  sand.set_color_mode(Number(colorModeSelect.value));
  sand.set_dither_mode(Number(ditherModeSelect.value));
  syncBrushSize();

  loadingEl.hidden = true;
  requestAnimationFrame(frame);
});
