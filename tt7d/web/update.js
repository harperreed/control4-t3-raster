// ABOUTME: The control panel's Update section: uploads a tt7d bundle with progress, shows the releases, trial
// ABOUTME: and history from /api/v1/system/update, and rolls back. Loaded after panel.js and uses its helpers.
"use strict";

const UPDATE_REFRESH_MS = 10000;
const UPDATE_AFTER_RESTART_MS = 4000; // tt7d exits a second after replying; tt7-app restarts it in about two

let updateStatus = null;

function releaseText(r) {
  let t = r.release;
  if (r.version) t += ` (version ${r.version})`;
  if (!r.usable) t += ": files missing";
  return t;
}

function historyText(h) {
  return `${h.time}  ${h.event}  ${h.release}${h.detail ? "  " + h.detail : ""}`;
}

function renderUpdate(s) {
  updateStatus = s;
  const run = s.running;
  fillList($("update-list"), [
    ["Running", `${run.release ? "release " + run.release : "the image's own build"}: tt7d ${run.version} (${run.build})`],
    ["Current release", s.current ? releaseText(s.current) : "none: the image's own build runs"],
    ["Previous release", s.previous ? releaseText(s.previous) : "none"],
    ["Trial", s.trial ? `${s.trial.release}: not confirmed yet, ${s.trial.starts} start(s) so far` : "none",
      s.trial ? "warn" : ""],
    ["Last result", s.last_result ? historyText(s.last_result) : "none"],
    ["Last refused request", s.last_error ? `${s.last_error.error} (HTTP ${s.last_error.status}) at ${s.last_error.time}` : "none"],
    ["Releases on the panel", s.releases.length ? s.releases.join(", ") : "none"],
    ["Signed bundles", s.policy.signature_required ? "required (a key is set)" : "not required"],
    ["Largest bundle", `${(s.policy.max_bytes / 1048576).toFixed(0)} MiB`],
  ]);
  const hist = $("update-history");
  hist.replaceChildren();
  for (const h of s.history.slice().reverse()) hist.append(el("li", historyText(h)));
  if (!s.history.length) hist.append(el("li", "Nothing installed through the web yet.", "muted"));
  $("update-rollback").textContent = s.previous ? `Roll back to ${s.previous.release}` : "Roll back (no previous release)";
}

async function loadUpdate() {
  try {
    renderUpdate(await getJSON("/api/v1/system/update"));
  } catch (e) {
    /* offline: the badge says so */
  }
}

function errorText(doc, status) {
  if (!doc) return `HTTP ${status}`;
  let t = `${doc.error}: ${doc.message}`;
  if (doc.member) t += ` (${doc.member})`;
  return t;
}

// XMLHttpRequest, not fetch: only it reports upload progress.
function uploadBundle(file) {
  const bar = $("update-progress");
  const button = $("update-upload");
  bar.value = 0;
  bar.hidden = false;
  button.disabled = true;
  showMsg("update-msg", `Uploading ${file.name} (${(file.size / 1024).toFixed(0)} KiB)…`, false);
  const xhr = new XMLHttpRequest();
  xhr.open("PUT", "/api/v1/system/update");
  xhr.setRequestHeader("Authorization", "Bearer " + currentToken());
  xhr.setRequestHeader("Content-Type", "application/x-tar");
  xhr.upload.addEventListener("progress", (ev) => {
    if (ev.lengthComputable) bar.value = ev.loaded / ev.total;
  });
  xhr.upload.addEventListener("load", () => showMsg("update-msg", "Uploaded. The panel is checking and installing it…", false));
  xhr.addEventListener("load", () => {
    button.disabled = !currentToken();
    bar.hidden = true;
    let doc = null;
    try { doc = JSON.parse(xhr.responseText); } catch (e) { /* not JSON */ }
    if (xhr.status === 401) {
      lockWith("The token was refused. Unlock again.");
      return;
    }
    if (xhr.status === 202 && doc) {
      showMsg("update-msg", `Installed ${doc.release} (version ${doc.version}) in ${doc.install_ms} ms. ` +
        "tt7d is restarting into it. It stays on trial until it has run for a while; if it fails, " +
        "the panel goes back to the previous release by itself.", false);
      setTimeout(loadUpdate, UPDATE_AFTER_RESTART_MS);
    } else {
      showMsg("update-msg", `Not installed: ${errorText(doc, xhr.status)}`, true);
      loadUpdate();
    }
  });
  xhr.addEventListener("error", () => {
    button.disabled = !currentToken();
    bar.hidden = true;
    showMsg("update-msg", "Upload failed: the panel did not answer.", true);
  });
  xhr.send(file);
}

function wireUpdate() {
  $("update-refresh").addEventListener("click", loadUpdate);
  $("update-upload").addEventListener("click", () => {
    const file = $("update-file").files[0];
    if (!file) {
      showMsg("update-msg", "Choose a bundle file first (tt7-bundle-*.tar from `make bundle`).", true);
      return;
    }
    if (updateStatus && file.size > updateStatus.policy.max_bytes) {
      showMsg("update-msg", `That file is larger than the ${(updateStatus.policy.max_bytes / 1048576).toFixed(0)} MiB limit.`, true);
      return;
    }
    uploadBundle(file);
  });
  $("update-rollback").addEventListener("click", async () => {
    if (!updateStatus || !updateStatus.previous) {
      showMsg("update-msg", "There is no previous release to roll back to.", true);
      return;
    }
    const to = updateStatus.previous.release;
    if (!window.confirm(`Roll back to ${to}? tt7d restarts, and the screen may flicker for a few seconds.`)) return;
    showMsg("update-msg", `Rolling back to ${to}…`, false);
    try {
      const doc = await mutate("POST", "/api/v1/system/update/rollback");
      showMsg("update-msg", `Rolling back to ${doc.release}; tt7d is restarting.`, false);
      setTimeout(loadUpdate, UPDATE_AFTER_RESTART_MS);
    } catch (e) {
      showMsg("update-msg", `Rollback failed: ${e.message}`, true);
    }
  });
}

wireUpdate();
loadUpdate();
setInterval(loadUpdate, UPDATE_REFRESH_MS);
