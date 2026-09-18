// The console's four views. Each polls only while it is showing and the tab is
// visible: every poll wakes an HTTP worker on the board.
//
// Every value the API reports may be null -- system_info returns optionals
// precisely so that "the kernel did not tell us" is distinguishable from zero --
// so every render path here has to survive null rather than printing "0".

const POLL_MS = 5000;

const $ = (id) => document.getElementById(id);

// Every API call goes through here, so none of them is served from cache: the
// values are live readings, and a stale one reads as a board that is fine.
function api(path, options = {}) {
  return fetch(path, { ...options, cache: options.cache ?? "no-store" });
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
  if (memory && memory.total_bytes) {
    const used = memory.total_bytes - (memory.available_bytes ?? memory.free_bytes ?? 0);
    definition(list, "Memory", `${bytes(used)} of ${bytes(memory.total_bytes)} used`);
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
    // Reachable bus, unanswered call: RAUC failed to start, and this says why.
    if (data.error) definition(list, "Error", data.error);
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
    request.upload.addEventListener("progress", (event) => {
      if (event.lengthComputable) {
        setProgress((event.loaded / event.total) * 100, `uploading ${bytes(event.loaded)} of ${bytes(event.total)}`);
      }
    });
    request.addEventListener("load", () => {
      if (request.status >= 200 && request.status < 300) resolve();
      else reject(new Error(errorFrom(request.responseText, request.status)));
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
      // Before the request, so the stream is open when the first progress
      // arrives; cleared by "completed" or by a refusal.
      installing = true;
      syncEvents();
      const response = await api("/api/update/install", { method: "POST" });
      if (!response.ok) throw new Error(errorFrom(await response.text(), response.status));
    } catch (error) {
      installing = false;
      syncEvents();
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
}

// Progress is pushed, not polled: an install reports through D-Bus and the node
// forwards it here. The stream holds a worker on the board for as long as it
// is open, so it is open only on the Update view or while an install runs.
//
// EventSource retries on its own after a dropped connection, but NOT after an
// HTTP error: the 503 a stopping node answers with leaves it CLOSED for good,
// and reflashing ends in exactly that restart. So a closed stream that is still
// wanted is reopened here.
let events = null;
let installing = false;
let reconnectTimer = null;
const RECONNECT_MS = 3000;

function eventsWanted() {
  return installing || currentView === "update";
}

function syncEvents() {
  if (eventsWanted()) {
    if (!events) connectEvents();
  } else {
    clearTimeout(reconnectTimer);
    reconnectTimer = null;
    if (events) events.close();
    events = null;
  }
}

function connectEvents() {
  if (events) events.close();
  events = new EventSource("/api/update/events");
  events.addEventListener("progress", (event) => {
    const data = JSON.parse(event.data);
    setProgress(data.percentage, data.message);
  });
  events.addEventListener("completed", (event) => {
    const data = JSON.parse(event.data);
    installing = false;
    if (data.ok) {
      setProgress(100, "installed \u2014 reboot to run it");
    } else {
      setProgress(0, "failed");
      showUpdateError(data.last_error || "the install failed");
    }
    refreshUpdate();
    syncEvents();
  });
  events.addEventListener("error", (event) => {
    // A frame the node sent ("fell behind") carries data; a failed connection
    // does not.
    if (event.data) showUpdateError(JSON.parse(event.data).error ?? "stream error");
    const source = event.target;
    if (source === events && source.readyState === EventSource.CLOSED && !reconnectTimer) {
      events = null;
      reconnectTimer = setTimeout(() => {
        reconnectTimer = null;
        syncEvents();
      }, RECONNECT_MS);
    }
  });
}

// --- Health view ----------------------------------------------------------
//
// Read from the bus directly: redline.js/redline.wasm hold a zenoh-pico session
// to zenohd's ws/ listener, and the module classifies with
// node_health::HealthTable -- the same table HealthMonitor wraps in `inspect`
// and the Qt app -- so the verdicts here are the node's own code, not a JS
// reimplementation. The console holds no health state of its own: with nobody
// looking, nothing on the board is watching health for this page.
//
// If zenohd is unreachable the view says so and retries; `inspect health` over
// ssh and `systemctl status redline-zenohd` are the way in from there.

const BUS_WS_PORT = 7446;
// How often the view redraws. `late` is a function of time alone, so the page
// redraws on a clock rather than only when a sample arrives.
const RENDER_MS = 1000;
const RETRY_MIN_MS = 2000;
const RETRY_MAX_MS = 30000;

function renderHealth(data) {
  $("bus-status").textContent = `zenoh over ws · revision ${data.revision ?? "?"}`;

  const host = $("nodes");
  host.replaceChildren();

  const list = data.nodes ?? [];
  if (list.length === 0) {
    const empty = document.createElement("div");
    empty.className = "muted";
    empty.textContent = "no nodes have reported yet";
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
    // healthy comes from isHealthy() in the classifier, not from guessing at
    // the verdict string here.
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
    detail.textContent = bits.join(" · ") || "reporting";

    row.append(head, detail);
    host.append(row);
  }
}

function healthUnavailable(message) {
  $("bus-status").textContent = message;
  $("nodes").replaceChildren();
}

// ONE loop owns the module: every bus* call may suspend (ASYNCIFY) and hand
// back a Promise, and two in flight at once corrupt the module -- so
// connecting, subscribing, pumping and closing all happen here, one await at a
// time, and nothing else calls bus*.
const bus = {
  module: null,     // the loaded module, or null
  broken: null,     // why the module cannot be used at all, or null
  running: false,   // the loop is alive
};

const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// redline.js is an Emscripten MODULARIZE script, not an ES module, so it is
// loaded with a script tag.
function loadBusModule() {
  return new Promise((resolve, reject) => {
    const script = document.createElement("script");
    script.src = "redline.js";
    script.onload = () => resolve();
    script.onerror = () => reject(new Error("redline.js is not served"));
    document.head.append(script);
  }).then(() => createRedlineModule())
    .then((module) => {
      if (typeof module.busConnect !== "function") throw new Error("module built without the bus");
      return module;
    });
}

function healthShowing() {
  return currentView === "health" && !document.hidden;
}

async function runHealth() {
  if (bus.broken) { healthUnavailable(`cannot read the bus: ${bus.broken}`); return; }
  if (bus.running) return;
  bus.running = true;
  const endpoint = `ws/${location.hostname}:${BUS_WS_PORT}`;
  let connected = false;
  try {
    if (!bus.module) {
      $("bus-status").textContent = "loading the bus module…";
      bus.module = await loadBusModule();
    }
    const m = bus.module;
    let retryMs = RETRY_MIN_MS;
    let lastRender = 0;

    while (healthShowing()) {
      if (!connected) {
        $("bus-status").textContent = `connecting to zenohd at ${endpoint}…`;
        connected = await m.busConnect(endpoint) && await m.busWatchHealth();
        if (!connected) {
          await m.busClose();
          healthUnavailable(`cannot reach zenohd at ${endpoint}; retrying in ${retryMs / 1000}s`);
          await delay(retryMs);
          retryMs = Math.min(retryMs * 2, RETRY_MAX_MS);
          continue;
        }
        retryMs = RETRY_MIN_MS;
        lastRender = 0;
      }

      if (await m.busPump() < 0) {
        // The router went away or dropped us.
        await m.busClose();
        connected = false;
        healthUnavailable(`lost zenohd at ${endpoint}; reconnecting`);
        continue;
      }

      const now = Date.now();
      if (now - lastRender >= RENDER_MS && healthShowing()) {
        renderHealth(JSON.parse(m.busHealthJson()));
        lastRender = now;
      }
      // busPump() already waits out the link's read timeout when idle; this
      // only yields to the page between pumps.
      await delay(20);
    }

    // Not showing: without pumping, the router would drop the session on
    // lease expiry anyway, so close it now and reconnect on the way back.
    if (connected) await m.busClose();
  } catch (error) {
    // A module that fails to load or throws is not retried: whatever broke it
    // will break it again. Reloading the page starts over.
    bus.broken = error.message ?? String(error);
    healthUnavailable(`cannot read the bus: ${bus.broken}`);
  } finally {
    bus.running = false;
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

// The form is built from describeSchema(): exact types and bounds, enum values,
// nested structs, unions, lists, the schema's defaults and its doc comments.
// Every editor is { el, value() }: value() is what goes in the request, and
// undefined means leave the field out so the schema's default applies.
// Constraint attributes (min/max/step/pattern) do the validating, so the
// browser's own messages and keyboards apply; the node checks again anyway.

let callForm = null;  // the root editor for the selected service

// 64-bit integers do not fit a JS Number, so they travel as BigInt and are
// written into the JSON as bare digits.
const INT64_BOUNDS = {
  int64: [-(2n ** 63n), 2n ** 63n - 1n],
  uint64: [0n, 2n ** 64n - 1n],
};

function requestJson(fields) {
  return JSON.stringify(fields, (_, v) => (typeof v === "bigint" ? `__bigint__${v}` : v))
    .replace(/"__bigint__(-?\d+)"/g, "$1");
}

function typeHint(spec) {
  const t = spec.capnp_type ?? spec.type ?? "";
  if (spec.min != null && spec.max != null && !(t in INT64_BOUNDS)) return `${t} ${spec.min}–${spec.max}`;
  return t;
}

function textInput(spec, { type = "text", inputMode, pattern, placeholder }) {
  const input = document.createElement("input");
  input.type = type;
  if (inputMode) input.inputMode = inputMode;
  if (pattern) input.pattern = pattern;
  const fallback = spec.default != null && spec.default !== "" ? `default ${spec.default}` : "";
  input.placeholder = placeholder ?? fallback;
  return input;
}

function scalarEditor(spec) {
  const t = spec.capnp_type ?? spec.type;
  switch (spec.type) {
    case "bool": {
      const input = document.createElement("input");
      input.type = "checkbox";
      input.checked = spec.default === true;
      return { el: input, value: () => input.checked };
    }
    case "enum": {
      const select = document.createElement("select");
      (spec.values ?? []).forEach((name, i) => {
        const option = document.createElement("option");
        option.value = name;
        option.textContent = name;
        const doc = spec.value_docs?.[i];
        if (doc) option.title = doc;
        select.append(option);
      });
      if (spec.default != null) select.value = spec.default;
      return { el: select, value: () => select.value };
    }
    case "int":
    case "uint": {
      if (t in INT64_BOUNDS) {
        const [min, max] = INT64_BOUNDS[t];
        const input = textInput(spec, { inputMode: "numeric", pattern: t === "uint64" ? "[0-9]+" : "-?[0-9]+" });
        input.addEventListener("input", () => {
          let message = "";
          if (/^-?[0-9]+$/.test(input.value)) {
            const v = BigInt(input.value);
            if (v < min || v > max) message = `${t} takes ${min} to ${max}`;
          }
          input.setCustomValidity(message);
        });
        return { el: input, value: () => (input.value === "" ? undefined : BigInt(input.value)) };
      }
      const input = textInput(spec, { type: "number", inputMode: spec.type === "uint" ? "numeric" : "decimal" });
      input.step = "1";
      if (spec.min != null) input.min = String(spec.min);
      if (spec.max != null) input.max = String(spec.max);
      return { el: input, value: () => (input.value === "" ? undefined : Number(input.value)) };
    }
    case "float": {
      const input = textInput(spec, { type: "number", inputMode: "decimal" });
      input.step = "any";
      return { el: input, value: () => (input.value === "" ? undefined : Number(input.value)) };
    }
    case "data": {
      const input = textInput(spec, { pattern: "\\s*([0-9A-Fa-f]{2}\\s*)*", placeholder: "hex, e.g. 01 ff 7a" });
      input.spellcheck = false;
      return { el: input, value: () => (input.value.trim() === "" ? undefined : input.value.trim()) };
    }
    case "void":
      return { el: document.createElement("span"), value: () => null };
    case "text":
      return (() => {
        const input = textInput(spec, {});
        return { el: input, value: () => (input.value === "" ? undefined : input.value) };
      })();
    default: {
      // Anything with no editor of its own takes raw JSON, so a call is still
      // possible rather than blocked.
      const input = textInput(spec, { placeholder: `${t} as JSON` });
      input.addEventListener("input", () => {
        try { if (input.value) JSON.parse(input.value); input.setCustomValidity(""); }
        catch { input.setCustomValidity("not valid JSON"); }
      });
      return { el: input, value: () => (input.value === "" ? undefined : JSON.parse(input.value)) };
    }
  }
}

// A struct's or group's fields, in declaration order. Union arms become one
// picker: exactly one of them goes in the request.
function structEditor(fields, isUnion) {
  const el = document.createElement("div");
  el.className = "struct";
  const entries = Object.entries(fields ?? {}).sort((a, b) => (a[1].order ?? 0) - (b[1].order ?? 0));
  const plain = entries.filter(([, spec]) => !spec.union_arm && !isUnion);
  const arms = entries.filter(([, spec]) => spec.union_arm || isUnion);
  const editors = [];

  for (const [name, spec] of plain) {
    const editor = makeEditor(spec);
    el.append(fieldRow(name, spec, editor));
    editors.push([name, editor]);
  }

  let union = null;
  if (arms.length > 0) {
    const select = document.createElement("select");
    const slot = document.createElement("div");
    slot.className = "arm";
    const armEditors = new Map();
    for (const [name, spec] of arms) {
      const option = document.createElement("option");
      option.value = name;
      option.textContent = name;
      if (spec.doc) option.title = spec.doc;
      select.append(option);
      armEditors.set(name, { spec, editor: makeEditor(spec) });
    }
    // The schema's active arm, when it has one, is the one it defaults to.
    const active = arms.find(([, spec]) => "default" in spec);
    if (active) select.value = active[0];
    const show = () => {
      const { spec, editor } = armEditors.get(select.value);
      slot.replaceChildren();
      if (spec.type !== "void") slot.append(fieldRow(select.value, spec, editor));
      else if (spec.doc) slot.append(docLine(spec.doc));
    };
    select.addEventListener("change", show);
    show();
    el.append(fieldRow("one of", { capnp_type: "union" }, { el: select }), slot);
    union = () => {
      const { editor } = armEditors.get(select.value);
      const v = editor.value();
      return [select.value, v === undefined ? null : v];
    };
  }

  return {
    el,
    value() {
      const out = {};
      for (const [name, editor] of editors) {
        const v = editor.value();
        if (v !== undefined) out[name] = v;
      }
      if (union) {
        const [name, v] = union();
        out[name] = v;
      }
      return Object.keys(out).length ? out : undefined;
    },
  };
}

// A value for a list slot left empty: a list has no "absent" element.
function emptyElement(spec) {
  if (spec.type === "bool") return false;
  if (spec.type === "enum") return spec.values?.[0];
  if (spec.type === "int" || spec.type === "uint" || spec.type === "float") return 0;
  if (spec.type === "text" || spec.type === "data") return "";
  if (spec.type === "struct") return {};
  return null;
}

function listEditor(spec) {
  const el = document.createElement("div");
  el.className = "list";
  const rows = document.createElement("div");
  const element = spec.element ?? { type: spec.element_type, values: spec.values };
  const items = [];

  const add = () => {
    const editor = makeEditor(element);
    const row = document.createElement("div");
    row.className = "list-row";
    const index = document.createElement("span");
    index.className = "muted";
    row.append(index, editor.el);
    const item = { editor, row, index };
    if (spec.fixed_length == null) {
      const remove = document.createElement("button");
      remove.type = "button";
      remove.className = "small";
      remove.textContent = "remove";
      remove.addEventListener("click", () => {
        items.splice(items.indexOf(item), 1);
        row.remove();
        renumber();
      });
      row.append(remove);
    }
    items.push(item);
    rows.append(row);
    renumber();
  };
  const renumber = () => items.forEach((item, i) => { item.index.textContent = `[${i}]`; });

  el.append(rows);
  if (spec.fixed_length != null) {
    for (let i = 0; i < spec.fixed_length; i++) add();
  } else {
    const addButton = document.createElement("button");
    addButton.type = "button";
    addButton.className = "small";
    addButton.textContent = "add";
    addButton.addEventListener("click", add);
    el.append(addButton);
  }

  return {
    el,
    value() {
      if (items.length === 0) return undefined;
      return items.map(({ editor }) => {
        const v = editor.value();
        return v === undefined ? emptyElement(element) : v;
      });
    },
  };
}

function makeEditor(spec) {
  if (spec.type === "struct" && spec.fields) return structEditor(spec.fields, spec.union === true);
  if (spec.type === "list") return listEditor(spec);
  return scalarEditor(spec);
}

function docLine(text) {
  const doc = document.createElement("div");
  doc.className = "field-doc muted";
  doc.textContent = text;
  return doc;
}

function fieldRow(name, spec, editor) {
  const wrap = document.createElement("div");
  wrap.className = spec.type === "struct" || spec.type === "list" ? "field nested" : "field";
  const label = document.createElement("label");
  const title = document.createElement("span");
  title.textContent = name;
  const hint = document.createElement("span");
  hint.className = "muted type-hint";
  hint.textContent = typeHint(spec);
  label.append(title, hint);
  wrap.append(label, editor.el);
  if (spec.doc) wrap.append(docLine(spec.doc));
  return wrap;
}

function buildForm(description) {
  const host = $("call-form");
  host.replaceChildren();
  const fields = description.fields?.fields ?? {};
  if (Object.keys(fields).length === 0) {
    callForm = null;
    host.append(Object.assign(document.createElement("div"), {
      className: "muted", textContent: "this request takes no fields" }));
    return;
  }
  callForm = structEditor(fields, description.fields?.union === true);
  host.append(callForm.el);
}

async function selectService(service) {
  selectedService = service;
  $("call-section").hidden = false;
  $("call-title").textContent = service.key;
  $("call-result").textContent = "";
  $("call-doc").textContent = "";

  try {
    const response = await api(`/api/schema/${encodeURIComponent(service.request_schema)}`);
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
  // The browser's own validation first: bounds, steps, hex, 64-bit ranges.
  for (const input of $("call-form").querySelectorAll("input, select")) {
    if (!input.checkValidity()) { input.reportValidity(); return; }
  }
  $("call-result").textContent = "calling\u2026";
  try {
    const response = await api("/api/call", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: requestJson({
        key: selectedService.key,
        request_schema: selectedService.request_schema,
        response_schema: selectedService.response_schema,
        fields: callForm?.value() ?? {},
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

let currentView = "info";
let pollTimer = null;

// What the showing view polls, and how often. Update and Services are read
// when shown and after an action; progress arrives on the stream.
function viewPoll() {
  if (currentView === "info") return [refresh, POLL_MS];
  return null;
}

// One timer, for the showing view, and none while the tab is hidden: a
// background tab left on Health would otherwise hit the board every 2 s
// indefinitely.
function syncPolling() {
  clearInterval(pollTimer);
  pollTimer = null;
  const poll = viewPoll();
  if (poll && !document.hidden) pollTimer = setInterval(poll[0], poll[1]);
}

function showView(name) {
  currentView = name;
  for (const tab of document.querySelectorAll(".tab")) {
    tab.classList.toggle("active", tab.dataset.view === name);
  }
  $("info").hidden = name !== "info";
  $("update").hidden = name !== "update";
  $("health").hidden = name !== "health";
  $("services").hidden = name !== "services";
  if (name === "info") refresh();
  if (name === "services") refreshServices();
  if (name === "update") refreshUpdate();
  syncPolling();
  syncEvents();
  if (name === "health") runHealth();
}

for (const tab of document.querySelectorAll(".tab")) {
  if (!tab.disabled) tab.addEventListener("click", () => showView(tab.dataset.view));
}

document.addEventListener("visibilitychange", () => {
  // Coming back, read at once rather than showing a stale page until the
  // next tick.
  if (!document.hidden) {
    const poll = viewPoll();
    if (poll) poll[0]();
    if (currentView === "health") runHealth();
  }
  syncPolling();
});

$("call-send").addEventListener("click", sendCall);

wireUpdate();
showView("info");
