// ABOUTME: tt7d control panel logic: polls /api/v1/state every 2 s, renders every section with textContent,
// ABOUTME: and sends mutations with the bearer token kept in sessionStorage (per tab; never in URLs or logs).
"use strict";

const TOKEN_KEY = "tt7d-token";
const POLL_MS = 2000;
const SYSTEM_EVERY_MS = 30000;

// Fallback when sessionStorage is blocked: the token then lasts until reload.
let memoryToken = null;

const $ = (id) => document.getElementById(id);

// ---- small helpers -----------------------------------------------------------

function el(tag, text, cls) {
  const e = document.createElement(tag);
  if (text !== undefined && text !== null) e.textContent = String(text);
  if (cls) e.className = cls;
  return e;
}

// Fill a <dl> from [label, value, optional class] rows. Values are text only.
function fillList(dl, rows) {
  dl.replaceChildren();
  for (const [label, value, cls] of rows) {
    dl.append(el("dt", label), el("dd", value === null || value === undefined ? "unknown" : value, cls));
  }
}

function duration(seconds) {
  if (seconds === null || seconds === undefined) return null;
  const s = Math.floor(seconds);
  const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
  if (d) return `${d}d ${String(h).padStart(2, "0")}h ${String(m).padStart(2, "0")}m`;
  if (h) return `${h}h ${String(m).padStart(2, "0")}m`;
  if (m) return `${m}m ${String(s % 60).padStart(2, "0")}s`;
  return `${s}s`;
}

function quantity(q) {
  if (!q || q.value === null || q.value === undefined) return null;
  if (q.unit === "kibibyte") return `${(q.value / 1024).toFixed(0)} MiB`;
  if (q.unit === "byte") return `${(q.value / 1048576).toFixed(0)} MiB`;
  return `${q.value} ${q.unit}`;
}

// The panel has no time sync yet; a year before 2024 means the clock was never set.
function clockText(iso) {
  if (!iso) return [null, ""];
  const year = parseInt(iso.slice(0, 4), 10);
  return year < 2024 ? [`${iso} (clock not set)`, "warn"] : [iso, ""];
}

// ---- token ----------------------------------------------------------------

function token() {
  try { return sessionStorage.getItem(TOKEN_KEY); } catch (e) { return null; }
}

function setToken(t) {
  try {
    if (t) sessionStorage.setItem(TOKEN_KEY, t);
    else sessionStorage.removeItem(TOKEN_KEY);
  } catch (e) { /* storage blocked: memoryToken below still holds it */ }
  memoryToken = t;
  renderLock();
}

function currentToken() { return token() || memoryToken; }

function renderLock(message) {
  const unlocked = !!currentToken();
  const badge = $("lock-state");
  badge.textContent = unlocked ? "Unlocked" : "Locked";
  badge.className = "badge " + (unlocked ? "unlocked" : "locked");
  $("lock-text").textContent = unlocked
    ? "Changes are allowed. The token is kept for this tab only."
    : "Read-only. Paste the token to change settings.";
  $("unlock-form").hidden = unlocked;
  $("lock").hidden = !unlocked;
  for (const c of document.querySelectorAll("[data-needs-token]")) {
    c.disabled = !unlocked;
    c.title = unlocked ? "" : "Unlock first";
  }
  showMsg("auth-msg", message || "", !!message);
  if (!unlocked) {
    $("logs-msg").textContent = "Unlock to read the logs.";
    $("log-tt7d").textContent = "";
    $("log-kernel").textContent = "";
    $("log-path").textContent = "";
  }
}

function showMsg(id, text, isError) {
  const m = $(id);
  m.textContent = text;
  m.className = "msg" + (isError ? " error" : "");
}

// ---- HTTP -----------------------------------------------------------------

async function getJSON(path, withToken) {
  const headers = {};
  if (withToken) headers.Authorization = "Bearer " + currentToken();
  const r = await fetch(path, { headers, cache: "no-store" });
  const doc = await r.json().catch(() => null);
  if (r.status === 401 && withToken) lockWith("The token was refused. Unlock again.");
  if (!r.ok) throw new Error((doc && doc.error) || `HTTP ${r.status}`);
  return doc;
}

// POST/PUT with the token. An empty body still sends Content-Length: 0, which tt7d requires.
async function mutate(method, path, body) {
  const headers = { Authorization: "Bearer " + currentToken() };
  if (body !== undefined) headers["Content-Type"] = "application/json";
  const r = await fetch(path, {
    method, headers, cache: "no-store", body: body === undefined ? "" : JSON.stringify(body),
  });
  const doc = await r.json().catch(() => null);
  if (r.status === 401) lockWith("The token was refused. Unlock again.");
  if (!r.ok) throw new Error((doc && (doc.message || doc.error)) || `HTTP ${r.status}`);
  return doc;
}

function lockWith(message) {
  setToken(null);
  renderLock(message);
}

// ---- rendering ---------------------------------------------------------------

let info = null;
let lastSeen = 0;
let previewKey = null;
let draggingBrightness = false;

function renderPreview(state) {
  const d = state.display;
  const have = d.frame_age_s !== null && d.frame_age_s !== undefined;
  // frame_id changes on every accepted or deduplicated frame; accepted counts redraws.
  const key = have ? `${d.frame_id || "restored"}:${state.frames.accepted}` : null;
  if (key === previewKey) return;
  previewKey = key;
  for (const img of document.querySelectorAll(".preview-img")) {
    img.hidden = !have;
    if (have) img.src = "/api/v1/frame/image?v=" + encodeURIComponent(key);
    else img.removeAttribute("src");
  }
  for (const cap of document.querySelectorAll(".preview-cap")) {
    cap.textContent = have ? `Frame ${d.frame_id || "(restored, id unknown)"}` : "No frame yet";
  }
}

function brightnessText(b) {
  return b && b.available ? `${b.value}%` : null;
}

function powerText(p) {
  const src = { external: "External power", battery: "Battery" }[p.source] || null;
  return src;
}

function batteryText(p) {
  const b = p.battery_percent;
  if (!b || !b.available) return null;
  let t = `${b.value}% (estimate)`;
  if (p.battery_status) t += `, ${p.battery_status}`;
  if (p.battery_voltage && p.battery_voltage.available) t += `, ${p.battery_voltage.value} V`;
  return t;
}

function networkRows(net) {
  const rows = [];
  for (const [name, i] of Object.entries(net.interfaces || {})) {
    rows.push([name, `${i.ipv4 || "no IPv4"} (${i.operstate || "state unknown"})`]);
  }
  return rows;
}

function renderState(state) {
  const d = state.display;
  const [timeText, timeCls] = clockText(state.time);
  fillList($("overview-list"), [
    ["Frame", d.frame_id || (d.frame_age_s === null ? "none" : "restored (id unknown)")],
    ["Frame age", duration(d.frame_age_s) || "no frame"],
    ["Display", d.on === null ? null : d.on ? "on" : "blank"],
    ["Brightness", brightnessText(d.brightness)],
    ["Power", powerText(state.power)],
    ["Battery", batteryText(state.power)],
    ["Uptime", duration(state.uptime_s)],
    ...networkRows(state.network),
    ["Frames", `${state.frames.accepted} shown, ${state.frames.deduplicated} duplicate, ${state.frames.rejected} refused`],
    ["Last error", state.frames.last_error || "none"],
    ["Time (UTC)", timeText, timeCls],
  ]);
  renderPreview(state);
  renderDisplay(state);
}

function renderDisplay(state) {
  const d = state.display;
  const rows = [
    ["Frame", d.frame_id || "none"],
    ["Frame age", duration(d.frame_age_s) || "no frame"],
    ["Display", d.on === null ? null : d.on ? "on" : "blank"],
  ];
  if (info) {
    const disp = info.display, n = disp.native;
    rows.push(
      ["Resolution", `${disp.width}×${disp.height} logical`],
      ["Native", `${n.width}×${n.height} ${n.format}, ${n.bits_per_pixel} bpp, stride ${n.stride} bytes`],
      ["Rotation", `${disp.rotation}° clockwise onto the native framebuffer`],
      ["Formats", disp.frame_formats.join(", ")],
    );
  }
  fillList($("display-list"), rows);
  const b = d.brightness;
  $("brightness-out").textContent = brightnessText(b) || "unknown";
  if (b && b.available && !draggingBrightness) $("brightness").value = b.value;
}

function renderOnline() {
  const badge = $("online");
  const fresh = lastSeen && Date.now() - lastSeen < POLL_MS * 3;
  badge.textContent = fresh ? "Online" : lastSeen ? `Offline (last seen ${duration((Date.now() - lastSeen) / 1000)} ago)` : "Offline";
  badge.className = "badge " + (fresh ? "online" : "offline");
}

function renderInfo() {
  $("model").textContent = info.name || info.model || "TT7";
  $("device-id").textContent = info.device_id || "";
  document.title = `${info.device_id || info.model} control panel`;
}

function renderSystem(sys) {
  const [timeText, timeCls] = clockText(sys.time.now);
  const k = sys.kernel;
  const rows = [
    ["Firmware", `${sys.firmware_version} (build ${sys.build})`],
    ["Kernel", k.release ? `${k.release} ${k.version || ""} ${k.machine || ""}` : null],
    ["Uptime", duration(sys.uptime_s)],
    ["Memory total", quantity(sys.memory.total)],
    ["Memory free", quantity(sys.memory.free)],
  ];
  if (sys.memory.available.value !== null) rows.push(["Memory available", quantity(sys.memory.available)]);
  for (const s of sys.storage) {
    rows.push([`Storage ${s.path}`, `${quantity(s.available) || "?"} free of ${quantity(s.total) || "?"}`]);
  }
  rows.push(["Time (UTC)", timeText, timeCls], ["Time sync", "none yet (nothing sets the clock)"]);
  fillList($("system-list"), rows);
}

// A generic, text-only view of /api/v1/hardware: nested objects become
// collapsible definition lists, so new fields show up without UI changes.
function renderTree(value) {
  if (value === null || value === undefined) return el("span", "null", "null");
  if (Array.isArray(value)) {
    if (!value.length) return el("span", "none", "null");
    if (value.every((v) => typeof v !== "object" || v === null)) return el("span", value.join(", "));
    const box = el("div");
    value.forEach((v, i) => {
      const item = el("div", undefined, "item");
      const name = v && (v.name || v.device || v.sysfs);
      item.append(el("strong", name ? String(name) : `#${i}`), renderTree(v));
      box.append(item);
    });
    return box;
  }
  if (typeof value === "object") {
    const dl = el("dl", undefined, "kv");
    for (const [k, v] of Object.entries(value)) {
      const dd = el("dd");
      dd.append(renderTree(v));
      dl.append(el("dt", k), dd);
    }
    return dl;
  }
  return el("span", String(value));
}

function renderHardware(hw) {
  const body = $("hardware-body");
  body.className = "tree";
  body.replaceChildren();
  for (const [section, value] of Object.entries(hw)) {
    const det = el("details");
    det.open = section === "display" || section === "input";
    det.append(el("summary", section), renderTree(value));
    body.append(det);
  }
}

// ---- loading ------------------------------------------------------------------

async function loadInfo() {
  info = await getJSON("/api/v1/info");
  renderInfo();
}

async function loadSystem() {
  try { renderSystem(await getJSON("/api/v1/system")); } catch (e) { /* the state poll shows offline */ }
}

async function loadHardware() {
  try {
    renderHardware(await getJSON("/api/v1/hardware"));
  } catch (e) {
    $("hardware-body").textContent = `Could not load: ${e.message}`;
  }
}

async function loadLogs() {
  if (!currentToken()) return;
  try {
    const doc = await getJSON("/api/v1/logs?lines=200", true);
    $("log-path").textContent = doc.tt7d.path;
    $("log-tt7d").textContent = doc.tt7d.available ? doc.tt7d.lines.join("\n") : "(log file not readable)";
    $("log-kernel").textContent = doc.kernel.available ? doc.kernel.lines.join("\n") : `(unavailable: ${doc.kernel.error})`;
    $("logs-msg").textContent = `Last ${doc.lines_requested} lines, read ${new Date().toLocaleTimeString()}.`;
    $("logs-msg").className = "msg muted";
  } catch (e) {
    showMsg("logs-msg", `Could not load the logs: ${e.message}`, true);
  }
}

async function poll() {
  try {
    const state = await getJSON("/api/v1/state");
    const wasOffline = !lastSeen || Date.now() - lastSeen >= POLL_MS * 3;
    lastSeen = Date.now();
    if (wasOffline || !info) {
      await loadInfo().catch(() => {});
      loadSystem();
    }
    renderState(state);
  } catch (e) {
    /* offline: the badge says so */
  }
  renderOnline();
  setTimeout(poll, POLL_MS);
}

// ---- actions ------------------------------------------------------------------

async function displayAction(label, method, path, body) {
  showMsg("display-msg", `${label}…`, false);
  try {
    const doc = await mutate(method, path, body);
    showMsg("display-msg", `${label}: done.`, false);
    return doc;
  } catch (e) {
    showMsg("display-msg", `${label} failed: ${e.message}`, true);
    return null;
  }
}

function wire() {
  $("unlock-form").addEventListener("submit", async (ev) => {
    ev.preventDefault();
    const t = $("token").value.trim();
    $("token").value = "";
    if (!t) return;
    // Check the token against an endpoint that needs it before keeping it.
    try {
      const r = await fetch("/api/v1/logs?lines=1", { headers: { Authorization: "Bearer " + t }, cache: "no-store" });
      if (r.status === 401) {
        renderLock("That token was refused.");
        return;
      }
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
    } catch (e) {
      renderLock(`Could not check the token: ${e.message}`);
      return;
    }
    setToken(t);
    loadLogs();
  });
  $("lock").addEventListener("click", () => lockWith(""));

  const slider = $("brightness");
  slider.addEventListener("input", () => {
    draggingBrightness = true;
    $("brightness-out").textContent = `${slider.value}%`;
  });
  slider.addEventListener("change", async () => {
    await displayAction("Brightness", "PUT", "/api/v1/display/brightness", { value: Number(slider.value), unit: "percent" });
    draggingBrightness = false;
  });
  $("wake").addEventListener("click", () => displayAction("Wake", "POST", "/api/v1/display/wake"));
  $("blank").addEventListener("click", () => displayAction("Blank", "POST", "/api/v1/display/blank"));
  $("test-pattern").addEventListener("click", () => displayAction("Test pattern", "POST", "/api/v1/display/test-pattern"));

  $("hardware-refresh").addEventListener("click", loadHardware);
  $("logs-refresh").addEventListener("click", loadLogs);

  $("reboot").addEventListener("click", async () => {
    if (!window.confirm("Reboot the panel now? It will be unreachable for about a minute.")) return;
    showMsg("actions-msg", "Asking the panel to reboot…", false);
    try {
      await mutate("POST", "/api/v1/system/reboot");
      showMsg("actions-msg", "Rebooting. This page reconnects by itself when the panel is back.", false);
    } catch (e) {
      showMsg("actions-msg", `Reboot failed: ${e.message}`, true);
    }
  });
}

wire();
renderLock();
poll();
loadHardware();
loadLogs();
setInterval(loadSystem, SYSTEM_EVERY_MS);
setInterval(renderOnline, 1000);
