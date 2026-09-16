"use strict";
const $ = (s) => document.querySelector(s),
  $$ = (s) => [...document.querySelectorAll(s)];
let page = "",
  stream = null,
  range = "today",
  chartRows = [],
  frameBuffer = [],
  frameCursor = 0,
  lastSniffer = null,
  config = null,
  selectedSerial = "",
  latestInverters = [];
const fmt = (n, d = 1) =>
  n === null || n === undefined || !Number.isFinite(n)
    ? "—"
    : new Intl.NumberFormat("fr-FR", { maximumFractionDigits: d }).format(n);
// Below 1 kWh, showing 2-decimal kWh rounds small-but-real values (a few Wh of sparse passive
// capture) down to a misleading "0". Switch units instead of just adding more decimals.
const energyDisplay = (wh) =>
  wh === null || wh === undefined || !Number.isFinite(wh)
    ? { value: "—", unit: "kWh" }
    : Math.abs(wh) < 1000
      ? { value: fmt(wh, 1), unit: "Wh" }
      : { value: fmt(wh / 1000, 2), unit: "kWh" };
const set = (id, value) => {
  const el = document.getElementById(id);
  if (el) el.textContent = value;
};
function notice(text) {
  set("notice", text);
  $("#notice").hidden = !text;
}
async function api(path, options = {}) {
  const headers = { ...options.headers };
  if (options.body && !(options.body instanceof Blob)) headers["Content-Type"] = "application/json";
  const r = await fetch(path, { ...options, headers });
  let data;
  try {
    data = await r.json();
  } catch {
    throw Error("Réponse du module illisible");
  }
  if (!r.ok) {
    throw Error(data.error || `HTTP ${r.status}`);
  }
  return data;
}
const post = (path, value) => api(path, { method: "POST", body: JSON.stringify(value) });
function inverterLabel(v) {
  return v.name ? `${v.name} · ${v.serial}` : v.serial;
}
function configuredChannels(v) {
  const model = v.model || "AUTO";
  if (model === "QS1") return 4;
  if (model === "DS3" || model === "YC600") return 2;
  return String(v.serial || "").startsWith("8") ? 4 : 2;
}
function inverterSelectors(rows) {
  latestInverters = rows;
  const choices = rows.map((v) => ({ value: v.serial, label: inverterLabel(v) }));
  if (selectedSerial && selectedSerial !== "all" && !rows.some((v) => v.serial === selectedSerial))
    selectedSerial = rows[0]?.serial || "";
  for (const id of ["history-inverter", "stats-inverter"]) {
    const el = $("#" + id);
    const list = rows.length > 1 ? [{ value: "all", label: "Tous les onduleurs (cumulé)" }, ...choices] : choices;
    const key = JSON.stringify(list);
    if (el.dataset.choices !== key) {
      el.replaceChildren(
        ...list.map((v) => {
          const o = document.createElement("option");
          o.value = v.value;
          o.textContent = v.label;
          return o;
        }),
      );
      el.dataset.choices = key;
    }
    el.value = selectedSerial;
  }
}
function inverterCard(v, index, panelOffset) {
  const card = document.createElement("article");
  card.className = "card inverter";
  card.dataset.serial = v.serial;
  const heading = document.createElement("div");
  heading.className = "card-heading";
  const title = document.createElement("h2");
  title.textContent = v.name || `Onduleur ${index + 1}`;
  const serial = document.createElement("span");
  serial.className = "eyebrow";
  serial.textContent = `${v.model || "APsystems"} · ${v.serial}`;
  heading.append(title, serial);
  const status = document.createElement("p");
  status.className = "muted inverter-status";
  status.textContent =
    {
      measured: "Mesure reçue",
      awaiting_second_sample: "Réponse reçue · attente de la suivante pour calculer la puissance",
      stale: "Mesure périmée · en attente d’une nouvelle réponse",
      no_data: "Aucune mesure reçue",
    }[v.status] || (v.online ? "Mesure reçue" : "Aucune mesure reçue");
  if (v.simulated) status.textContent += " · SIMULATION";
  if (!v.configured) status.textContent += " · Découvert, à enregistrer dans Réglages";
  const panels = document.createElement("div");
  panels.className = "pv-grid";
  const channels = v.channels || [v.pv1 || {}, v.pv2 || {}];
  for (let i = 1; i <= (v.channelCount || channels.length); i++) {
    const pv = channels[i - 1] || {};
    const panel = document.createElement("div");
    panel.className = "panel";
    panel.dataset.pv = i;
    const label = document.createElement("span");
    label.className = "eyebrow";
    label.textContent = `PV ${i} · Panneau ${panelOffset + i}`;
    const power = document.createElement("strong");
    power.textContent = `${fmt(pv.power)} W`;
    const readings = document.createElement("div");
    for (const [field, unit] of [
      ["voltage", "V"],
      ["current", "A"],
    ]) {
      const value = document.createElement("span");
      value.textContent = `${fmt(pv[field])} ${unit}`;
      readings.append(value);
    }
    panel.append(label, power, readings);
    panels.append(panel);
  }
  const readings = document.createElement("div");
  readings.className = "readings inverter-readings";
  for (const [label, value] of [
    ["Tension AC", fmt(v.acVoltage) + " V"],
    ["Fréquence AC", fmt(v.acFrequency) + " Hz"],
    ["Température", fmt(v.temperature) + " °C"],
    ["Dernière mesure", v.last_seen ? new Date(v.last_seen * 1000).toLocaleString("fr-FR") : "Jamais"],
  ]) {
    const line = document.createElement("div");
    const name = document.createElement("span");
    name.textContent = label;
    const data = document.createElement("strong");
    data.textContent = value;
    line.append(name, data);
    readings.append(line);
  }
  card.append(heading, status, panels, readings);
  return card;
}
function live(s) {
  const rows = s.inverters || [];
  set("power", fmt(s.totalPower ?? s.measuredPower, 0));
  set(
    "power-label",
    s.totalPower == null && s.knownPowerCount ? "PUISSANCE CONNUE · PARTIELLE" : "PRODUCTION ACTUELLE",
  );
  {
    const e = energyDisplay(s.todayWh);
    set("today-energy", e.value);
    set("today-energy-unit", e.unit + " mesurés");
  }
  set(
    "inverter-status",
    `${rows.length} onduleurs · ${s.pvCount || 0} PV · ${s.knownPowerCount || 0}/${rows.length} puissances connues`,
  );
  let panelOffset = 0;
  $("#inverter-grid").replaceChildren(
    ...rows.map((v, index) => {
      const card = inverterCard(v, index, panelOffset);
      panelOffset += v.channelCount || configuredChannels(v);
      return card;
    }),
  );
  $("#inverter-empty").hidden = rows.length > 0;
  if (!$("#history-inverter").dataset.choices) selectedSerial = rows[0]?.serial || "";
  inverterSelectors(rows);
  if (s.simulated) {
    $("#mode-banner").hidden = false;
    set("mode-banner", "MODE MOCK — Toutes les valeurs de production affichées sont simulées.");
  }
}
function inventoryRow(v = {}) {
  const row = document.createElement("div");
  row.className = "inventory-row";
  for (const [field, label, value] of [
    ["serial", "Numéro de série APsystems", v.serial || ""],
    ["name", "Nom (facultatif)", v.name || ""],
    ["address", "Adresse courte décimale (0 si inconnue)", v.address || 0],
  ]) {
    const l = document.createElement("label");
    l.textContent = label;
    const el = document.createElement("input");
    el.dataset.field = field;
    el.value = value;
    if (field === "serial") {
      el.required = true;
      el.maxLength = 12;
      el.pattern = "[0-9a-fA-F]{12}";
      el.autocomplete = "off";
      el.oninput = inventoryCount;
    }
    if (field === "name") el.maxLength = 32;
    if (field === "address") {
      el.type = "number";
      el.min = 0;
      el.max = 65527;
      el.required = true;
    }
    l.append(el);
    row.append(l);
  }
  const modelLabel = document.createElement("label");
  modelLabel.textContent = "Modèle";
  const model = document.createElement("select");
  model.dataset.field = "model";
  for (const value of ["AUTO", "DS3", "YC600", "QS1"]) {
    const option = document.createElement("option");
    option.value = option.textContent = value;
    model.append(option);
  }
  model.value = v.model || "AUTO";
  model.onchange = inventoryCount;
  modelLabel.append(model);
  row.append(modelLabel);
  const remove = document.createElement("button");
  remove.type = "button";
  remove.className = "quiet";
  remove.textContent = "Retirer";
  remove.onclick = () => {
    row.remove();
    inventoryCount();
  };
  row.append(remove);
  $("#inventory-editor").append(row);
  inventoryCount();
}
function inventoryCount() {
  const count = $$(".inventory-row").length;
  const max = config?.maxInverters || 16;
  const pv = $$(".inventory-row").reduce((sum, row) => {
    const serial = row.querySelector('[data-field="serial"]')?.value || "";
    const model = row.querySelector('[data-field="model"]')?.value || "AUTO";
    return sum + configuredChannels({ serial, model });
  }, 0);
  set("inventory-count", `${count} onduleurs · ${pv} PV (maximum ${max})`);
  $("#add-inverter").disabled = count >= max;
}
function readInventory() {
  const serials = new Set();
  return $$(".inventory-row").map((row) => {
    const v = {};
    for (const el of row.querySelectorAll("input,select"))
      v[el.dataset.field] = el.dataset.field === "address" ? Number(el.value) : el.value.trim();
    v.serial = v.serial.toUpperCase();
    if (serials.has(v.serial)) throw Error("Chaque numéro de série doit être unique.");
    if (new TextEncoder().encode(v.name).length > 32)
      throw Error("Nom de l'onduleur trop long (32 octets maximum).");
    serials.add(v.serial);
    return v;
  });
}
function openStream(sniffer = false) {
  if (stream) stream.close();
  stream = new EventSource(sniffer ? `/api/sniffer/events?after=${frameCursor}` : "/api/events");
  stream.addEventListener(sniffer ? "frames" : "live", (event) => {
    try {
      const data = JSON.parse(event.data);
      if (sniffer) renderSniffer(data);
      else live(data);
      set("connection", "Connecté au module");
    } catch (e) {
      notice(e.message);
    }
  });
  stream.onerror = () => set("connection", "Reconnexion…");
}
async function history() {
  const data = await api("/api/history?range=" + range + "&serial=" + encodeURIComponent(selectedSerial));
  chartRows = data.records;
  drawChart();
  set(
    "chart-note",
    chartRows.length
      ? `${chartRows.length} points mesurés · ${range === "today" ? "Puissance en W par entrée PV" : "Énergie en kWh"} · Les périodes sans données restent inconnues.`
      : "Aucune mesure pour cette période. Une heure NTP valide est nécessaire pour dater l’historique.",
  );
}
function drawChart() {
  const canvas = $("#production-chart"),
    rect = canvas.getBoundingClientRect();
  if (!rect.width) return;
  const ratio = devicePixelRatio || 1;
  canvas.width = rect.width * ratio;
  canvas.height = 255 * ratio;
  const c = canvas.getContext("2d");
  c.scale(ratio, ratio);
  const w = rect.width,
    h = 255,
    left = 46,
    right = w - 12,
    top = 20,
    bottom = 220;
  c.clearRect(0, 0, w, h);
  c.font = "10px system-ui";
  c.fillStyle = "#87927c";
  const daily = range === "today";
  const values = chartRows.map((r) => (daily ? r.totalPower : r.energyWh / 1000)).filter(Number.isFinite);
  const max = Math.max(daily ? 100 : 1, ...values) * 1.15;
  for (let i = 0; i < 5; i++) {
    const y = bottom - ((bottom - top) * i) / 4;
    c.strokeStyle = "#edf0e7";
    c.beginPath();
    c.moveTo(left, y);
    c.lineTo(right, y);
    c.stroke();
    c.fillText(fmt((max * i) / 4, 0), 2, y + 3);
  }
  if (!chartRows.length) {
    c.fillText("Les premières mesures apparaîtront ici.", left + 20, 130);
    return;
  }
  const minTime = chartRows[0].timestamp,
    maxTime = chartRows.at(-1).timestamp;
  const x = (i) =>
    daily
      ? left + ((right - left) * (chartRows[i].timestamp - minTime)) / Math.max(60, maxTime - minTime)
      : left + ((right - left) * (i + 0.5)) / chartRows.length;
  if (daily) {
    for (const [key, color, channel] of [
      ["totalPower", "#285e3c", -1],
      ["channels", "#8eb36b", 0],
      ["channels", "#d4a86d", 1],
      ["channels", "#5b8fb9", 2],
      ["channels", "#b06d9d", 3],
    ]) {
      c.beginPath();
      c.strokeStyle = color;
      c.lineWidth = channel < 0 ? 2.3 : 1.5;
      let previous = false;
      chartRows.forEach((r, i) => {
        const value = channel < 0 ? r[key] : r.channels?.[channel];
        if (!Number.isFinite(value)) {
          previous = false;
          return;
        }
        const y = bottom - (value / max) * (bottom - top);
        if (!previous || (i && r.timestamp - chartRows[i - 1].timestamp > 90)) c.moveTo(x(i), y);
        else c.lineTo(x(i), y);
        previous = true;
      });
      c.stroke();
    }
  } else {
    const bw = Math.max(2, ((right - left) / chartRows.length) * 0.57);
    chartRows.forEach((r, i) => {
      const y = bottom - (r.energyWh / 1000 / max) * (bottom - top);
      c.fillStyle = "#6e9455";
      c.fillRect(x(i) - bw / 2, y, bw, bottom - y);
    });
  }
  for (let i = 0; i < chartRows.length; i += Math.max(1, Math.ceil(chartRows.length / 6))) {
    const d = new Date(chartRows[i].timestamp * 1000);
    const text = daily
      ? d.toLocaleTimeString("fr-FR", { hour: "2-digit", minute: "2-digit" })
      : range === "12m"
        ? d.toLocaleDateString("fr-FR", { month: "short" })
        : d.toLocaleDateString("fr-FR", { day: "2-digit", month: "2-digit" });
    c.fillText(text, x(i) - 13, 246);
  }
}
function metric(label, value, unit = "") {
  const el = document.createElement("article");
  el.className = "card metric";
  const title = document.createElement("span");
  title.className = "eyebrow";
  title.textContent = label;
  const strong = document.createElement("strong");
  strong.textContent = value;
  const small = document.createElement("small");
  small.textContent = unit;
  el.append(title, strong, small);
  return el;
}
async function stats() {
  if (!latestInverters.length) {
    const data = await api("/api/live");
    selectedSerial = data.inverters?.[0]?.serial || "";
    inverterSelectors(data.inverters || []);
  }
  const s = await api("/api/stats?serial=" + encodeURIComponent(selectedSerial));
  const root = $("#stats-grid");
  root.replaceChildren();
  for (const [label, key] of [
    ["Aujourd’hui", "todayWh"],
    ["Hier", "yesterdayWh"],
    ["Cette semaine", "weekWh"],
    ["Ce mois", "monthWh"],
    ["Cette année", "yearWh"],
    ["Total mesuré", "totalWh"],
    ["Meilleure journée", "bestDayWh"],
    ["Moyenne quotidienne", "dailyAverageWh"],
  ]) {
    const e = energyDisplay(s[key]);
    root.append(metric(label, e.value, e.unit));
  }
  root.append(
    metric(
      "Pic aujourd’hui",
      fmt(s.peakToday, 0) + " W",
      s.peakTime ? new Date(s.peakTime * 1000).toLocaleString("fr-FR") : "",
    ),
  );
}
async function settings() {
  config = await api("/api/config");
  const f = $("#settings-form");
  for (const el of f.elements) {
    if (!el.name || config[el.name] === undefined) continue;
    if (el.type === "checkbox") el.checked = config[el.name];
    else el.value = config[el.name];
  }
  const normalOption = f.elements.mode?.querySelector('option[value="NORMAL"]');
  if (normalOption) {
    normalOption.disabled = config.normalModeAvailable === false;
    normalOption.textContent = config.normalModeAvailable === false
      ? "NORMAL · Indisponible dans ce firmware"
      : "NORMAL · Acquisition active";
  }
  $("#pair-button").disabled = config.mode === "SNIFFER";
  $("#inventory-editor").replaceChildren();
  for (const v of config.inverters || []) inventoryRow(v);
  inventoryCount();
}
function showObject(title, obj) {
  const card = document.createElement("article");
  card.className = "card";
  const h = document.createElement("h2");
  h.textContent = title;
  const pre = document.createElement("pre");
  pre.textContent = JSON.stringify(obj, null, 2);
  card.append(h, pre);
  return card;
}
async function system() {
  const s = await api("/api/system");
  const root = $("#system-content");
  root.replaceChildren(
    showObject("Firmware & mémoire", {
      firmware: s.firmware,
      build: s.build,
      idf: s.idf,
      uptimeSeconds: s.uptimeSeconds,
      freeHeap: s.freeHeap,
      minimumHeap: s.minimumHeap,
      timeValid: s.timeValid,
      timezone: s.timezone,
      mode: s.mode,
    }),
    showObject("Direct Access Point", s.wifi.ap),
    showObject("Home Network", s.wifi.sta),
    showObject("Onduleurs APsystems", s.live),
    showObject("Radio", s.radio),
    showObject("Flash & historique", { flashBytes: s.flashBytes, ...s.storage }),
  );
}
async function debug() {
  const logs = await api("/api/debug");
  set(
    "logs",
    logs
      .map(
        (l) =>
          `${(l.ms / 1000).toFixed(3)} [${["ERROR", "WARN", "INFO", "DEBUG", "TRACE"][l.level]}] ${l.message}`,
      )
      .join("\n"),
  );
}
function table(headers, rows) {
  const t = document.createElement("table"),
    head = t.createTHead().insertRow();
  for (const h of headers) {
    const th = document.createElement("th");
    th.textContent = h;
    head.append(th);
  }
  const body = t.createTBody();
  for (const row of rows) {
    const tr = body.insertRow();
    for (const value of row) {
      const td = tr.insertCell();
      td.textContent = value;
    }
  }
  return t;
}
function renderSniffer(s) {
  lastSniffer = s;
  set(
    "capture-state",
    !s.active
      ? "Activer SNIFFER dans Réglages"
      : s.scanning
        ? "Scan en cours"
        : s.paused
          ? "Capture en pause / terminée"
          : `Canal ${s.channel} · RX ONLY`,
  );
  const aps = s.apsMessages || {};
  const metrics = $("#sniffer-metrics");
  metrics.replaceChildren();
  for (const [label, value] of [
    ["Trames", fmt(s.totalFrames, 0)],
    ["APsystems", fmt(s.apsFrames, 0)],
    ["Autres / inconnues", fmt(s.unknownFrames, 0)],
    ["RAM", `${s.bufferCount} / ${s.bufferCapacity}`],
    ["APS réassemblés", fmt(aps.reassembled, 0)],
    ["Fragments orphelins", fmt(aps.orphanFragments, 0)],
    ["Doublons", fmt(aps.duplicateFragments, 0)],
    ["Timeouts réassemblage", fmt(aps.timeouts, 0)],
  ]) {
    const el = document.createElement("div");
    el.textContent = label;
    const b = document.createElement("strong");
    b.textContent = value;
    el.append(b);
    metrics.append(el);
  }
  set(
    "probable-channel",
    s.probableChannel
      ? `Canal APsystems probable : ${s.probableChannel} · ${s.confidence}`
      : "Aucun canal APsystems identifié pour le moment.",
  );
  const channels = $("#channel-results");
  channels.replaceChildren();
  for (const ch of s.channels) {
    const el = document.createElement("div");
    if (ch.channel === s.probableChannel) el.className = "probable";
    el.textContent = `Canal ${ch.channel}`;
    const strong = document.createElement("strong");
    strong.textContent = ch.frames;
    const small = document.createElement("small");
    small.textContent = `${fmt(ch.framesPerSecond)} /s · ${ch.apsFrames} APS`;
    el.title = JSON.stringify(ch, null, 2);
    el.append(strong, small);
    channels.append(el);
  }
  // RSSI moyen is only ever the direct-hop physical reading (mac === nwkAddress); a device heard
  // only via relay reports '—', never a value blended from a relay's signal.
  $("#devices").replaceChildren(
    table(
      [
        "Rôle",
        "État",
        "PAN / adresse",
        "Série possible",
        "Trames",
        "RSSI direct",
        "Polling",
        "Délai réponse",
        "Complets",
        "Fragments",
        "Orphelins",
        "Relais",
      ],
      s.devices.map((d) => [
        d.role,
        d.confidence,
        `${d.pan.toString(16)} / ${d.address}`,
        d.possibleSerial || "—",
        d.frames,
        fmt(d.averageRssi) + " dBm",
        fmt(d.pollingIntervalMs) + " ms",
        fmt(d.responseDelayMs) + " ms",
        fmt(d.completeMessages, 0),
        fmt(d.fragmentedMessages, 0),
        fmt(d.orphanFragments, 0),
        (d.relays || []).map((r) => `${r.mac}:${r.frames}`).join(", ") || "—",
      ]),
    ),
  );
  $("#physical-transmitters").replaceChildren(
    table(
      ["MAC", "Canal", "Trames", "RSSI moyen", "RSSI min", "RSSI max", "LQI moyen"],
      (s.physicalTransmitters || []).map((t) => [
        t.mac,
        t.channel,
        t.frames,
        fmt(t.averageRssi) + " dBm",
        fmt(t.minRssi) + " dBm",
        fmt(t.maxRssi) + " dBm",
        fmt(t.averageLqi),
      ]),
    ),
  );
  for (const f of s.frames) {
    if (f.id <= frameCursor) continue;
    frameCursor = f.id;
    frameBuffer.push(f);
  }
  frameBuffer = frameBuffer.slice(-200);
  renderFrames();
}
function renderFrames() {
  const filter = $("#frame-filter").value,
    src = $("#source-filter").value.toUpperCase(),
    dst = $("#destination-filter").value.toUpperCase(),
    ch = $("#channel-filter").value,
    rssi = $("#rssi-filter").value;
  const body = $("#frame-rows");
  body.replaceChildren();
  for (const f of [...frameBuffer].reverse()) {
    const aps = f.protocol.startsWith("APSYSTEMS");
    if (
      (filter === "aps" && !aps) ||
      (filter === "unknown" && aps) ||
      (filter === "ecu" && !["APSYSTEMS_POLL", "APSYSTEMS_ECU_TO_INVERTER"].includes(f.protocol)) ||
      (filter === "inverter" && !["APSYSTEMS_RESPONSE", "APSYSTEMS_INVERTER_TO_ECU"].includes(f.protocol)) ||
      (src && !(f.src || "").includes(src)) ||
      (dst && !(f.dst || "").includes(dst)) ||
      (ch && f.channel !== Number(ch)) ||
      (rssi && f.rssi < Number(rssi))
    )
      continue;
    const tr = body.insertRow();
    for (const value of [
      f.timestampUs
        ? new Date(f.timestampUs / 1000).toLocaleTimeString("fr-FR")
        : (f.monotonicUs / 1000000).toFixed(3) + " s",
      f.channel,
      f.rssi,
      f.src || "—",
      f.dst || "—",
      f.protocol.replace("APSYSTEMS_", "APS "),
      f.length,
    ])
      tr.insertCell().textContent = value;
    tr.tabIndex = 0;
    const detail = () => {
      const ascii = (f.hex.match(/../g) || [])
        .map((x) => {
          const n = parseInt(x, 16);
          return n >= 32 && n < 127 ? String.fromCharCode(n) : ".";
        })
        .join("");
      set(
        "frame-detail",
        JSON.stringify({ ...f, hex: undefined }, null, 2) +
          "\n\nHEX\n" +
          (f.hex.match(/.{1,32}/g) || []).map((x) => x.match(/../g).join(" ")).join("\n") +
          "\n\nASCII\n" +
          ascii,
      );
      $("#frame-dialog").showModal();
    };
    tr.onclick = detail;
    tr.onkeydown = (e) => {
      if (e.key === "Enter") detail();
    };
  }
}
async function navigate() {
  page =
    location.hash.slice(1) ||
    (location.pathname.includes("sniffer")
      ? "sniffer"
      : location.pathname === "/debug"
        ? "debug"
        : "dashboard");
  if (!document.getElementById(page)) page = "dashboard";
  for (const el of $$(".page")) el.hidden = el.id !== page;
  for (const a of $$("nav a")) a.classList.toggle("active", a.hash === "#" + page);
  set(
    "page-title",
    {
      dashboard: "Vue d’ensemble",
      statistics: "Statistiques",
      sniffer: "Radio sniffer",
      settings: "Réglages",
      system: "Système",
      debug: "Journal",
    }[page],
  );
  if (stream) {
    stream.close();
    stream = null;
  }
  try {
    if (page === "dashboard") {
      live(await api("/api/live"));
      await history();
      openStream();
    }
    if (page === "statistics") await stats();
    if (page === "settings") await settings();
    if (page === "system") await system();
    if (page === "debug") await debug();
    if (page === "sniffer") {
      renderSniffer(await api("/api/sniffer?after=" + frameCursor));
      openStream(true);
    }
  } catch (e) {
    notice(e.message);
  }
}
function action(fn) {
  return async (e) => {
    e?.preventDefault();
    try {
      await fn(e);
    } catch (error) {
      notice(error.message);
    }
  };
}
$("#close-frame").onclick = () => $("#frame-dialog").close();
$("#settings-form").onsubmit = action(async (e) => {
  const data = {};
  for (const el of e.target.elements) {
    if (!el.name) continue;
    if (el.type === "password" && !el.value) continue;
    data[el.name] =
      el.type === "checkbox"
        ? el.checked
        : el.type === "number" || el.name === "logLevel"
          ? Number(el.value)
          : el.value;
  }
  data.inverters = readInventory();
  const result = await post("/api/config", data);
  notice(result.message + " Reconnectez-vous au Wi-Fi du module si nécessaire.");
  if (stream) stream.close();
  setTimeout(() => location.reload(), 10000);
});
$("#add-inverter").onclick = () => inventoryRow();
for (const id of ["history-inverter", "stats-inverter"])
  $("#" + id).onchange = action(async (e) => {
    selectedSerial = e.target.value;
    inverterSelectors(latestInverters);
    if (page === "dashboard") await history();
    else await stats();
  });
$("#ranges").onclick = action(async (e) => {
  if (!e.target.dataset.range) return;
  range = e.target.dataset.range;
  for (const b of $$("#ranges button")) b.classList.toggle("selected", b === e.target);
  await history();
});
$("#wifi-scan").onclick = action(async () => {
  set("wifi-scan-result", "Recherche…");
  let s = await api("/api/wifi/scan");
  await new Promise((r) => setTimeout(r, 2500));
  s = await api("/api/wifi/scan");
  const list = $("#wifi-networks");
  list.replaceChildren();
  for (const net of s.networks) {
    const option = document.createElement("option");
    option.value = net.ssid;
    option.label = `${net.rssi} dBm`;
    list.append(option);
  }
  set(
    "wifi-scan-result",
    s.networks.map((n) => `${n.ssid} (${n.rssi} dBm)`).join(" · ") ||
      "Aucun réseau trouvé. Réessayez après la connexion STA.",
  );
});
$("#pair-button").onclick = action(async () => {
  if (
    !confirm("Appairer le premier onduleur de la liste à OpenSolECU ? Cela remplace son association ECU actuelle.")
  )
    return;
  notice((await post("/api/pair", {})).message);
});
$("#export-form").onsubmit = action(async (e) => {
  const d = new FormData(e.target);
  const from = d.get("from") ? Math.floor(new Date(d.get("from") + "T00:00:00").getTime() / 1000) : 0;
  const to = d.get("to")
    ? Math.floor(new Date(d.get("to") + "T23:59:59").getTime() / 1000)
    : Math.floor(Date.now() / 1000);
  location.href = `/api/export.csv?from=${from}&to=${to}&resolution=${d.get("resolution")}&serial=${encodeURIComponent(selectedSerial)}`;
});
$("#ota-form").onsubmit = action(async (e) => {
  const file = e.target.firmware.files[0];
  $("#ota-progress").hidden = false;
  const xhr = new XMLHttpRequest();
  xhr.open("POST", "/api/ota");
  xhr.setRequestHeader("Content-Type", "application/octet-stream");
  xhr.upload.onprogress = (e) => {
    $("#ota-progress").value = e.lengthComputable ? (100 * e.loaded) / e.total : 0;
  };
  xhr.onload = () => {
    try {
      const data = JSON.parse(xhr.responseText);
      notice(data.message || data.error);
      if (xhr.status === 200) setTimeout(() => location.reload(), 12000);
    } catch {
      notice("Réponse OTA illisible.");
    }
  };
  xhr.onerror = () => notice("Connexion interrompue pendant la mise à jour.");
  xhr.send(file);
});
for (let ch = 11; ch <= 26; ch++) {
  const o = document.createElement("option");
  o.value = ch;
  o.textContent = ch;
  o.selected = ch === 16;
  $("#sniffer-channel").append(o);
}
const control = async (data) => {
  notice((await post("/api/sniffer/control", data)).message);
};
$("#capture-form").onsubmit = action(async (e) =>
  control({
    action: "capture",
    channel: Number(e.target.channel.value),
    duration: Number(e.target.duration.value),
  }),
);
$("#fixed-channel").onclick = action(async () =>
  control({ action: "channel", channel: Number($("#sniffer-channel").value) }),
);
$("#scan-channels").onclick = action(async () =>
  control({ action: "scan", dwell: Number($("#scan-dwell").value) }),
);
for (const [id, cmd] of [
  ["pause-capture", "pause"],
  ["resume-capture", "resume"],
  ["clear-capture", "clear"],
])
  $("#" + id).onclick = action(async () => {
    await control({ action: cmd });
    if (cmd === "clear") {
      frameBuffer = [];
      renderFrames();
    }
  });
for (const el of $$(".filters input,.filters select")) el.oninput = renderFrames;
window.addEventListener("hashchange", navigate);
window.addEventListener("resize", drawChart);
document.addEventListener("visibilitychange", () => {
  if (document.hidden) {
    stream?.close();
    stream = null;
  } else navigate();
});
setInterval(() => {
  if (document.hidden) return;
  if (page === "debug") debug().catch(() => {});
  if (page === "system") system().catch(() => {});
}, 5000);
setInterval(() => {
  if (page === "dashboard" && !document.hidden) history().catch(() => {});
}, 60000);
api("/api/config")
  .then((c) => {
    config = c;
    set("installation", c.installation.toUpperCase());
    if (c.mode === "SNIFFER") {
      set(
        "mode-banner",
        "SNIFFER PASSIF · Aucune transmission IEEE 802.15.4. Ouvrez Radio sniffer pour observer votre installation.",
      );
      $("#mode-banner").hidden = false;
    }
    if (!c.ssid && !location.hash) {
      location.hash = "settings";
      notice("Premier démarrage : configurez votre accès réseau. Le point d’accès direct reste disponible.");
    }
  })
  .catch((e) => notice(e.message));
navigate();
