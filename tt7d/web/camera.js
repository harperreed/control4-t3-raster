// ABOUTME: tt7d control panel, Camera section: on/off switches (PUT /api/v1/config/camera), a snapshot shown as a
// ABOUTME: data: URL (the CSP's img-src allows data:), and presence from /state and the event stream. textContent only.
"use strict";

(() => {
  const CONFIG = "/api/v1/config/camera";
  const SNAPSHOT = "/api/v1/camera/snapshot";
  const SWITCHES = { "camera-enabled": "enabled", "camera-presence": "presence", "camera-wake": "presence_wake" };
  let config = null; // GET /api/v1/config/camera, once unlocked
  let saving = false;

  function badge(c) {
    const b = $("camera-state");
    b.textContent = !c.enabled ? "off" : c.presence_enabled ? "on, presence" : "on";
    b.className = "badge " + (c.enabled ? "online" : "");
  }

  function presentText(c) {
    if (c.present === null || c.present === undefined) return c.presence_enabled ? "not known yet" : "presence detection off";
    return c.present ? "yes" : "no";
  }

  function renderCamera(state) {
    const c = state.camera;
    if (!c) return;
    badge(c);
    fillList($("camera-list"), [
      ["Camera", c.enabled ? "on" : "off"],
      ["Someone there", presentText(c)],
      ["Presence changed", c.presence_changed_at || "never"],
      ["Last snapshot", c.last_snapshot_at || "none since start"],
      ["Worker", c.worker + (c.worker_restarts ? `, ${c.worker_restarts} restart(s)` : "")],
      ["Last error", c.last_error || "none"],
    ]);
    if (!currentToken()) {
      config = null;
      clearSnapshot(); // no picture on screen once locked
    } else if (!config) {
      loadConfig();
    }
    if (!saving) {
      $("camera-enabled").checked = config ? config.enabled : c.enabled;
      $("camera-presence").checked = config ? config.presence : c.presence_enabled;
      $("camera-wake").checked = config ? config.presence_wake : false;
    }
    $("camera-snapshot").disabled = !currentToken() || !c.enabled;
  }

  async function loadConfig() {
    try {
      config = await getJSON(CONFIG, true);
    } catch (e) {
      showMsg("camera-msg", `Could not read the camera settings: ${e.message}`, true);
    }
  }

  function clearSnapshot() {
    const img = $("camera-img");
    img.hidden = true;
    img.removeAttribute("src");
    $("camera-cap").textContent = "No snapshot shown";
  }

  function dataUrl(blob) {
    return new Promise((resolve, reject) => {
      const r = new FileReader();
      r.onload = () => resolve(r.result);
      r.onerror = () => reject(r.error);
      r.readAsDataURL(blob);
    });
  }

  async function takeSnapshot() {
    showMsg("camera-msg", "Taking a snapshot…", false);
    try {
      const r = await fetch(SNAPSHOT, { headers: { Authorization: "Bearer " + currentToken() }, cache: "no-store" });
      if (r.status === 401) lockWith("The token was refused. Unlock again.");
      if (!r.ok) {
        const doc = await r.json().catch(() => null);
        throw new Error((doc && (doc.message || doc.error)) || `HTTP ${r.status}`);
      }
      const img = $("camera-img");
      img.src = await dataUrl(await r.blob());
      img.hidden = false;
      $("camera-cap").textContent = `Taken ${r.headers.get("X-Captured-At") || "just now"} (kept in this page only)`;
      showMsg("camera-msg", "", false);
    } catch (e) {
      showMsg("camera-msg", `Snapshot failed: ${e.message}`, true);
    }
  }

  async function setSwitch(id) {
    saving = true;
    const key = SWITCHES[id];
    try {
      config = await mutate("PUT", CONFIG, { [key]: $(id).checked });
      showMsg("camera-msg", "Saved.", false);
    } catch (e) {
      $(id).checked = !$(id).checked;
      showMsg("camera-msg", `Could not change ${key}: ${e.message}`, true);
    }
    saving = false;
  }

  document.addEventListener("tt7-state", (e) => renderCamera(e.detail));
  document.addEventListener("tt7-event", (e) => {
    const ev = e.detail;
    if (ev.type !== "presence") return;
    $("camera-msg").className = "msg muted";
    $("camera-msg").textContent = `${(ev.timestamp || "").slice(11, 19)} presence ${ev.present ? "detected" : "gone"}`;
  });
  for (const id of Object.keys(SWITCHES)) $(id).addEventListener("change", () => setSwitch(id));
  $("camera-snapshot").addEventListener("click", takeSnapshot);
})();
