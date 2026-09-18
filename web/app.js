// The Info view. Polls /api/system and renders it.
//
// Every value the API reports may be null -- system_info returns optionals
// precisely so that "the kernel did not tell us" is distinguishable from zero --
// so every render path here has to survive null rather than printing "0".

const POLL_MS = 5000;

const $ = (id) => document.getElementById(id);

// --- authentication --------------------------------------------------------
//
// The board REFUSES to serve /api/ without a bearer token -- the node will not
// even start when token_file is configured and the file is missing -- so the
// browser has to be able to present one. It is typed once and kept in
// localStorage: this page is served from the board's own origin, so the token
// stays on the device it belongs to.
//
// A HEADER, NEVER A COOKIE. Nothing is attached automatically, so cross-site
// request forgery has nothing to forge with.

const TOKEN_KEY = "redline.web_console.token";
let token = localStorage.getItem(TOKEN_KEY) ?? "";

function authHeaders(extra) {
  const headers = { ...(extra ?? {}) };
  if (token) headers.Authorization = `Bearer ${token}`;
  return headers;
}

// ONE PLACE every API call goes through, so a route added later cannot forget
// to authenticate, and a 401 always surfaces as the prompt rather than as
// whatever message each view happens to print.
async function api(path, options = {}) {
  const response = await fetch(path, {
    ...options,
    headers: authHeaders(options.headers),
    cache: options.cache ?? "no-store",
  });
  if (response.status === 401) {
    askForToken();
    throw new Error("this board wants a token");
  }
  return response;
}

function askForToken() {
  $("auth").hidden = false;
  $("auth-input").focus();
}

function bytes(n) {
  if (n === null || n === undefined) return "—";
  const units = ["B", "KB", "MB", "GB", "TB"];
  let value = n, i = 0;
  while (value >= 1024 && i < units.length - 1) { value /= 1024; i++; }
  return `${value.toFixed(value < 10 && i > 0 ? 1 : 0)} ${units[i]}`;
}

function duration(seconds) {
  if (seconds === null || seconds === undefined) return "—";
  const d = Math.floor(seconds / 86400);
  const h = Math.floor((seconds % 86400) / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  if (d > 0) return `${d}d ${h}h ${m}m`;
  if (h > 0) return `${h}h ${m}m`;
  return `${m}m`;
}

function definition(list, term, value) {
  const dt = document.createElement("dt");
  dt.textContent = term;
  const dd = document.createElement("dd");
  dd.textContent = value ?? "—";
  list.append(dt, dd);
}

function renderSystem(data) {
  const list = $("system");
  list.replaceChildren();
  const os = data.os;
  definition(list, "Image", os ? (os.name || os.id || "—") : "no os-release");
  definition(list, "Version", os ? os.version : null);
  // BUILD_ID is what a Yocto image stamps; a desktop distro has none, and the
  // difference is worth showing rather than hiding.
  if (os && os.build_id) definition(list, "Build", os.build_id);
  definition(list, "Uptime", duration(data.uptime_seconds));
  definition(list, "Load", data.load ? data.load.map((n) => n.toFixed(2)).join("  ") : null);

  const memory = data.memory;
  if (memory && memory.total) {
    const used = memory.total - (memory.available ?? memory.free ?? 0);
    definition(list, "Memory", `${bytes(used)} of ${bytes(memory.total)} used`);
  } else {
    definition(list, "Memory", null);
  }
}

function capacityRow(label, total, available) {
  const row = document.createElement("div");
  row.className = "row";
  const head = document.createElement("div");
  head.className = "row-head";
  const name = document.createElement("strong");
  name.textContent = label;
  const figures = document.createElement("span");
  figures.className = "muted";
  figures.textContent = `${bytes(available)} free of ${bytes(total)}`;
  head.append(name, figures);

  const bar = document.createElement("div");
  bar.className = "bar";
  const fill = document.createElement("span");
  const usedFraction = total > 0 ? 1 - available / total : 0;
  fill.style.width = `${(usedFraction * 100).toFixed(1)}%`;
  if (usedFraction > 0.85) bar.classList.add("high");
  bar.append(fill);

  row.append(head, bar);
  return row;
}

function renderStorage(data) {
  const host = $("storage");
  host.replaceChildren();
  for (const fs of data.filesystems ?? []) {
    host.append(capacityRow(fs.mount_point, fs.total_bytes, fs.available_bytes));
  }
}

function renderNetwork(data) {
  const host = $("network");
  host.replaceChildren();
  // Loopback is reported rather than filtered: "only lo is up" is a diagnosis,
  // and a page that omits it looks identical to one with no network at all.
  for (const nic of data.network ?? []) {
    const row = document.createElement("div");
    row.className = "row";
    const head = document.createElement("div");
    head.className = "row-head";
    const name = document.createElement("strong");
    name.textContent = nic.name + (nic.loopback ? " (loopback)" : "");
    const state = document.createElement("span");
    state.className = "muted";
    state.textContent = nic.up ? "up" : "down";
    head.append(name, state);
    const addresses = document.createElement("div");
    addresses.className = "muted";
    addresses.textContent = nic.addresses.join(", ");
    row.append(head, addresses);
    host.append(row);
  }
}

function renderTemperatures(data) {
  const host = $("temperatures");
  host.replaceChildren();
  const readings = data.temperatures ?? [];
  if (readings.length === 0) {
    const empty = document.createElement("div");
    empty.className = "muted";
    empty.textContent = "no hwmon sensors";
    host.append(empty);
    return;
  }
  for (const t of readings) {
    const row = document.createElement("div");
    row.className = "row row-head";
    const name = document.createElement("span");
    name.textContent = `${t.device} · ${t.label}`;
    const value = document.createElement("strong");
    value.textContent = `${(t.milli_celsius / 1000).toFixed(1)} °C`;
    row.append(name, value);
    host.append(row);
  }
}

async function refresh() {
  try {
    const response = await api("/api/system");
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const data = await response.json();

    $("error").hidden = true;
    renderSystem(data);
    renderStorage(data);
    renderNetwork(data);
    renderTemperatures(data);

    const os = data.os;
    $("identity").textContent = os ? `${os.id ?? ""} ${os.version ?? ""}`.trim() : "";
  } catch (error) {
    // A reboot is an expected event here -- reflashing is one of this console's
    // jobs -- so a failed poll says so and keeps trying rather than dying.
    const banner = $("error");
    banner.hidden = false;
    banner.textContent = `Cannot reach the board (${error.message}). Retrying…`;
  }
}

// --- Update view ----------------------------------------------------------

let staged = null;

function pill(text, good) {
  const span = document.createElement("span");
  span.className = `pill ${good ? "good" : "bad"}`;
  span.textContent = text;
  return span;
}

function renderUpdate(data) {
  const status = $("update-status");
  status.replaceChildren();

  if (!data.rauc_available) {
    // The rest of the console still works; say what does not and why.
    status.append(pill("RAUC unreachable", false));
    const why = document.createElement("div");
    why.className = "muted";
    why.textContent = data.error ?? "rauc.service is not on D-Bus";
    status.append(why);
  } else {
    const list = document.createElement("dl");
    definition(list, "Operation", data.operation);
    definition(list, "Booted slot", data.boot_slot);
    definition(list, "Primary", data.primary);
    definition(list, "Compatible", data.compatible);
    if (data.last_error) definition(list, "Last error", data.last_error);
    status.append(list);
  }

  // Boot counting is the bootloader's, not ours: show what it says.
  for (const entry of data.boot_entries ?? []) {
    const row = document.createElement("div");
    row.className = "row row-head";
    const name = document.createElement("span");
    name.textContent = `slot ${entry.slot} \u00b7 ${entry.entry}`;
    const tries = document.createElement("span");
    tries.className = "muted";
    tries.textContent = entry.tries_left === null
      ? "not being counted"
      : `${entry.tries_left} tries left`;
    row.append(name, tries);
    status.append(row);
  }

  staged = data.staged;
  $("staged").textContent = staged
    ? `staged: ${staged.path} (${bytes(staged.bytes)})`
    : "nothing staged";
  $("install").disabled = !staged || !data.rauc_available;

  const host = $("slots");
  host.replaceChildren();
  for (const slot of data.slots ?? []) {
    const row = document.createElement("div");
    row.className = "row";
    const head = document.createElement("div");
    head.className = "row-head";
    const name = document.createElement("strong");
    name.textContent = `${slot.name} (${slot.bootname})`;
    head.append(name, pill(slot.state, slot.state === "booted"));
    const detail = document.createElement("div");
    detail.className = "muted";
    detail.textContent = `${slot.bundle_version || "no version"} \u00b7 boot-status ${slot.boot_status}`;
    row.append(head, detail);
    host.append(row);
  }
}

async function refreshUpdate() {
  try {
    const response = await api("/api/update/status");
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    renderUpdate(await response.json());
  } catch (error) {
    $("update-error").hidden = false;
    $("update-error").textContent = `Cannot read update status (${error.message}).`;
  }
}

function setProgress(percentage, message) {
  const bar = $("progress-bar");
  bar.hidden = false;
  bar.firstElementChild.style.width = `${Math.max(0, Math.min(100, percentage))}%`;
  $("progress-text").textContent = message;
}

// XMLHttpRequest rather than fetch: fetch still cannot report upload progress,
// and a 338 MB bundle over a bench link is exactly when someone needs to see
// that something is happening.
function uploadBundle(file) {
  return new Promise((resolve, reject) => {
    const request = new XMLHttpRequest();
    request.open("POST", "/api/update/bundle");
    // Not api(): XMLHttpRequest is here for upload progress, so the header it
    // would have added has to be set by hand.
    if (token) request.setRequestHeader("Authorization", `Bearer ${token}`);
    request.upload.addEventListener("progress", (event) => {
      if (event.lengthComputable) {
        setProgress((event.loaded / event.total) * 100, `uploading ${bytes(event.loaded)} of ${bytes(event.total)}`);
      }
    });
    request.addEventListener("load", () => {
      if (request.status >= 200 && request.status < 300) resolve();
      else {
        if (request.status === 401) askForToken();
        reject(new Error(errorFrom(request.responseText, request.status)));
      }
    });
    request.addEventListener("error", () => reject(new Error("the connection failed")));
    request.send(file);
  });
}

function errorFrom(text, status) {
  try {
    const parsed = JSON.parse(text);
    if (parsed.error) return parsed.error;
  } catch { /* not JSON; fall through to the status */ }
  return `HTTP ${status}`;
}

function showUpdateError(message) {
  const banner = $("update-error");
  banner.hidden = false;
  banner.textContent = message;
}

function wireUpdate() {
  const picker = $("bundle");
  picker.addEventListener("change", () => {
    $("upload").disabled = picker.files.length === 0;
  });

  $("upload").addEventListener("click", async () => {
    $("update-error").hidden = true;
    $("upload").disabled = true;
    try {
      await uploadBundle(picker.files[0]);
      setProgress(100, "uploaded");
      await refreshUpdate();
    } catch (error) {
      showUpdateError(`Upload failed: ${error.message}`);
      $("upload").disabled = false;
    }
  });

  $("install").addEventListener("click", async () => {
    $("update-error").hidden = true;
    $("install").disabled = true;
    setProgress(0, "asking RAUC to install\u2026");
    try {
      const response = await api("/api/update/install", { method: "POST" });
      if (!response.ok) throw new Error(errorFrom(await response.text(), response.status));
    } catch (error) {
      showUpdateError(`Install refused: ${error.message}`);
      $("install").disabled = false;
    }
  });

  $("mark-good").addEventListener("click", async () => {
    $("update-error").hidden = true;
    try {
      const response = await api("/api/update/mark-good", { method: "POST" });
      if (!response.ok) throw new Error(errorFrom(await response.text(), response.status));
      await refreshUpdate();
    } catch (error) {
      showUpdateError(`Could not mark good: ${error.message}`);
    }
  });

  connectEvents();
}

// Progress is pushed, not polled: an install reports through D-Bus and the node
// forwards it here. EventSource reconnects on its own, which matters because
// reflashing ends in a reboot.
//
// EVENTSOURCE CANNOT SET HEADERS -- no browser offers an API for it -- so this
// one route takes the token as a query parameter, which is why it is rebuilt
// rather than created once: a token typed after the page loaded has to reach
// the stream too.
let events = null;

function connectEvents() {
  if (events) events.close();
  const query = token ? `?access_token=${encodeURIComponent(token)}` : "";
  events = new EventSource(`/api/update/events${query}`);
  events.addEventListener("progress", (event) => {
    const data = JSON.parse(event.data);
    setProgress(data.percentage, data.message);
  });
  events.addEventListener("completed", (event) => {
    const data = JSON.parse(event.data);
    if (data.ok) {
      setProgress(100, "installed \u2014 reboot to run it");
    } else {
      setProgress(0, "failed");
      showUpdateError(data.last_error || "the install failed");
    }
    refreshUpdate();
  });
  events.addEventListener("error", (event) => {
    if (event.data) showUpdateError(JSON.parse(event.data).error ?? "stream error");
  });
}

// --- Health view ----------------------------------------------------------
//
// Served by the node, not read from the bus directly.
//
// The wasm module CAN decode these samples -- that path is built and proven
// (212/212 layout fingerprints, and a NodeHealth sample decoded byte-identically
// to native). What is not proven is zenoh-pico's emscripten WebSocket transport
// reaching a zenohd: it sends a correct upgrade and a correct zenoh InitSyn
// frame, then never receives an InitAck, and upstream's emscripten CI is
// build-only so that path has no evidence of ever having run.
//
// So the node holds the zenoh session and serves /api/health, classified by
// node_health::HealthMonitor -- the same classifier `inspect health` uses. The
// browser renders a verdict it was given rather than one it invented, which was
// the property that actually mattered.

const HEALTH_POLL_MS = 2000;
let healthTimer = null;

function renderHealth(data) {
  $("bus-status").textContent = data.bus_available
    ? `observing the bus \u00b7 revision ${data.revision ?? "?"}`
    : (data.error ?? "no bus session");

  const host = $("nodes");
  host.replaceChildren();

  const list = data.nodes ?? [];
  if (list.length === 0) {
    const empty = document.createElement("div");
    empty.className = "muted";
    empty.textContent = data.bus_available ? "no nodes have reported yet" : "nothing observable";
    host.append(empty);
    return;
  }

  for (const node of list.sort((a, b) => a.name.localeCompare(b.name))) {
    const row = document.createElement("div");
    row.className = "row";
    const head = document.createElement("div");
    head.className = "row-head";
    const label = document.createElement("strong");
    label.textContent = node.name;
    // healthy comes from isHealthy() on the node, not from guessing at the
    // verdict string here.
    head.append(label, pill(node.verdict, node.healthy));

    const detail = document.createElement("div");
    detail.className = "muted";
    const problems = (node.checks ?? [])
      .filter((c) => c.state !== "ok")
      .map((c) => `${c.name}: ${c.detail || c.state}`)
      .join(", ");
    const bits = [];
    if (problems) bits.push(problems);
    if (node.uptime_ms != null) bits.push(`up ${duration(Math.floor(node.uptime_ms / 1000))}`);
    if (node.restarts) bits.push(`${node.restarts} restarts`);
    if (node.age_ms != null) bits.push(`last seen ${(node.age_ms / 1000).toFixed(1)}s ago`);
    detail.textContent = bits.join(" \u00b7 ") || "reporting";

    row.append(head, detail);
    host.append(row);
  }
}

async function refreshHealth() {
  try {
    const response = await api("/api/health");
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    renderHealth(await response.json());
  } catch (error) {
    $("bus-status").textContent = `cannot read health (${error.message})`;
  }
}

// --- Services view --------------------------------------------------------
//
// Switchboard in a browser. Nothing here knows any service: the list comes from
// liveliness, the form is built from the request schema's own description, and
// the call goes through the node's generic dynamic caller -- the same path
// `inspect call` and apps/switchboard use. A service added to the tree tomorrow
// appears here with no change to this file.

let selectedService = null;
let currentFields = {};

function renderServices(data) {
  const host = $("service-list");
  host.replaceChildren();

  if (!data.bus_available) {
    const msg = document.createElement("div");
    msg.className = "muted";
    msg.textContent = data.error ?? "no bus session";
    host.append(msg);
    return;
  }

  const list = data.services ?? [];
  if (list.length === 0) {
    const empty = document.createElement("div");
    empty.className = "muted";
    empty.textContent = "no services are being offered";
    host.append(empty);
    return;
  }

  for (const service of list.sort((a, b) => a.key.localeCompare(b.key))) {
    const row = document.createElement("div");
    row.className = "row clickable";
    const head = document.createElement("div");
    head.className = "row-head";
    const name = document.createElement("strong");
    name.textContent = service.key;
    // Entries are never removed, only marked unreachable -- a service that has
    // gone is more useful shown than hidden.
    head.append(name, pill(service.reachable ? "reachable" : "gone", service.reachable));
    const detail = document.createElement("div");
    detail.className = "muted";
    detail.textContent = `${service.request_schema} \u2192 ${service.response_schema}` +
      (service.owner ? ` \u00b7 ${service.owner}` : "");
    row.append(head, detail);
    row.addEventListener("click", () => selectService(service));
    host.append(row);
  }
}

// The form is built from describeSchema()'s field list -- category-level types,
// which is enough for scalars and strings. Anything it cannot render as an input
// falls back to raw JSON so the call is still possible rather than blocked.
function buildForm(description) {
  const host = $("call-form");
  host.replaceChildren();
  currentFields = {};

  const fields = description.fields?.fields ?? description.fields ?? {};
  const names = Object.keys(fields);
  if (names.length === 0) {
    const none = document.createElement("div");
    none.className = "muted";
    none.textContent = "this request takes no fields";
    host.append(none);
    return;
  }

  for (const name of names) {
    const spec = fields[name] ?? {};
    const wrap = document.createElement("label");
    wrap.className = "field";
    const label = document.createElement("span");
    label.textContent = name;
    const input = document.createElement("input");
    input.type = /int|float|number/i.test(spec.type ?? "") ? "number" : "text";
    input.placeholder = spec.type ?? "";
    input.addEventListener("input", () => {
      const raw = input.value;
      if (raw === "") { delete currentFields[name]; return; }
      currentFields[name] = input.type === "number" ? Number(raw) : raw;
    });
    wrap.append(label, input);
    host.append(wrap);
  }
}

async function selectService(service) {
  selectedService = service;
  $("call-section").hidden = false;
  $("call-title").textContent = service.key;
  $("call-result").textContent = "";
  $("call-doc").textContent = "";

  try {
    const response = await api("/api/schema", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ schema: service.request_schema }),
    });
    const description = await response.json();
    if (!response.ok) throw new Error(description.error ?? `HTTP ${response.status}`);
    $("call-doc").textContent = description.doc || "";
    buildForm(description);
  } catch (error) {
    $("call-form").replaceChildren();
    $("call-result").textContent = `cannot describe ${service.request_schema}: ${error.message}`;
  }
}

async function sendCall() {
  if (!selectedService) return;
  $("call-result").textContent = "calling\u2026";
  try {
    const response = await api("/api/call", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        key: selectedService.key,
        request_schema: selectedService.request_schema,
        response_schema: selectedService.response_schema,
        fields: currentFields,
      }),
    });
    const result = await response.json();
    // Every status is shown, not just success: 504 means nobody answered, 400
    // means the fields did not fit the schema and `errors` says which.
    $("call-result").textContent =
      `${response.status} ${result.status ?? ""}\n` + JSON.stringify(result, null, 2);
  } catch (error) {
    $("call-result").textContent = `call failed: ${error.message}`;
  }
}

async function refreshServices() {
  try {
    const response = await api("/api/services");
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    renderServices(await response.json());
  } catch (error) {
    $("service-list").textContent = `cannot list services (${error.message})`;
  }
}

// --- views ----------------------------------------------------------------

function showView(name) {
  for (const tab of document.querySelectorAll(".tab")) {
    tab.classList.toggle("active", tab.dataset.view === name);
  }
  $("info").hidden = name !== "info";
  $("update").hidden = name !== "update";
  $("health").hidden = name !== "health";
  $("services").hidden = name !== "services";
  if (name === "services") refreshServices();
  if (name === "update") refreshUpdate();
  if (name === "health") {
    refreshHealth();
    // Poll only while the view is showing; a page left on Info should not.
    if (!healthTimer) healthTimer = setInterval(refreshHealth, HEALTH_POLL_MS);
  } else if (healthTimer) {
    clearInterval(healthTimer);
    healthTimer = null;
  }
}

for (const tab of document.querySelectorAll(".tab")) {
  if (!tab.disabled) tab.addEventListener("click", () => showView(tab.dataset.view));
}

$("call-send").addEventListener("click", sendCall);

$("auth-form").addEventListener("submit", (event) => {
  event.preventDefault();
  token = $("auth-input").value.trim();
  localStorage.setItem(TOKEN_KEY, token);
  $("auth").hidden = true;
  // The stream carries the token in its URL, so it has to be reopened.
  connectEvents();
  refresh();
  refreshUpdate();
});

wireUpdate();
refresh();
setInterval(refresh, POLL_MS);
