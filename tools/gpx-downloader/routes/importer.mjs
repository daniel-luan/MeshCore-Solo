import { parseGpxDocument } from "./format.mjs";
import { RouteSerial, OP, integers } from "./serial.mjs";
import { ActivityLog } from "./log.mjs";
const container = document.createElement("section");
container.id = "route-import";
container.className = "card";
container.style.marginTop = "1rem";
container.innerHTML = `<h1>Import planned route</h1><p class="subtitle">GPX stays on this computer. Disconnect BLE companions and other USB applications first.</p>
<label>GPX file <input id="route-file" type="file" accept=".gpx,application/gpx+xml"></label><p><label>Track or route <select id="route-path"></select></label></p>
<p><label>Path tolerance <select id="route-tolerance"><option value="0">Preserve all points</option><option value="2">2 metres</option><option value="5">5 metres</option><option value="10">10 metres</option></select></label> <label>Elevation tolerance <select id="route-height"><option value="1">1 metre</option><option selected value="3">3 metres</option><option value="5">5 metres</option></select></label></p>
<canvas id="route-preview" width="400" height="180" style="width:100%;background:#12121e;border-radius:8px"></canvas><p id="route-summary">Select a file to preview.</p>
<p><label><input id="route-named" type="checkbox" checked> Include named path points</label></p><div id="route-selection"></div><details><summary>Segment boundaries (gaps are not joined)</summary><div id="route-segments"></div></details><details><summary>Checkpoints and offsets</summary><div id="route-checkpoints"></div></details>
<p><button id="route-connect">Connect for import</button> <button id="route-upload" disabled>Import route</button> <button id="route-cancel" disabled>Cancel</button></p>
<p id="route-capacity">Device capacity will appear after connecting.</p><p id="route-status" role="status" aria-live="polite"></p>`;
const exportCard = document.querySelector(".card");
document.body.style.display = "block";
document.body.style.maxWidth = "650px";
document.body.style.margin = "auto";
exportCard.style.maxWidth = "none";
container.style.maxWidth = "none";
document.body.append(container);
const log = new ActivityLog(document.body);
window.gpxLog = (message, level = "info") => log.write(message, level);
window.gpxActivity = (message) => log.phase(message);
window.gpxFinished = (message, level = "info") => log.finish(message, level);
window.addEventListener("error", (e) =>
  log.finish(`Page error: ${e.message} (${e.filename}:${e.lineno})`, "error"),
);
window.addEventListener("unhandledrejection", (e) =>
  log.finish(`Unhandled error: ${e.reason?.stack || e.reason}`, "error"),
);
const tabs = document.createElement("nav");
tabs.style.marginBottom = "1rem";
tabs.innerHTML =
  '<button id="route-export-tab">Export trail</button> <button id="route-import-tab">Import route</button>';
document.body.prepend(tabs);
container.hidden = true;
const $ = (id) => document.getElementById(id);
let gpx = null,
  route = null,
  client = null,
  maximum = 0,
  chunk = 144,
  uploading = false,
  cancel = false,
  connecting = false,
  converting = false,
  conversion = 0,
  conversionStarted = 0;
log.detail = () => {
  if (!client) return "";
  const pending = client.pending;
  return `${client.closed ? "USB closed" : "USB open"}: RX ${client.rxBytes} B / TX ${client.txBytes} B; ${client.frames} frames; ${client.ignoredFrames} ignored; ${client.noiseBytes} non-frame bytes${pending ? `; waiting for ${pending.name} #${pending.id}, attempt ${pending.attempt}/4, ${((performance.now() - pending.started) / 1000).toFixed(1)}s` : client.lastReply ? `; last reply ${((performance.now() - client.lastReply) / 1000).toFixed(1)}s ago` : "; no reply yet"}`;
};
const worker = new Worker(new URL("./worker.mjs", import.meta.url), {
  type: "module",
});
const status = (t) => ($("route-status").textContent = t);
const bytes = (n) => `${(n / 1024).toFixed(1)} KB`;
function controls() {
  $("route-upload").disabled =
    !route ||
    !client ||
    uploading ||
    connecting ||
    converting ||
    route.data.length > maximum;
  $("route-connect").disabled = uploading || connecting;
  $("route-file").disabled = uploading;
  $("route-path").disabled = uploading;
  $("route-tolerance").disabled = uploading;
  $("route-height").disabled = uploading;
  $("route-cancel").disabled = !uploading;
  $("route-named").disabled = uploading;
  document
    .querySelectorAll("[data-waypoint]")
    .forEach((e) => (e.disabled = uploading));
}
$("route-export-tab").onclick = async () => {
  if (uploading) {
    status("Cancel the import before switching tabs.");
    return;
  }
  await disconnect();
  container.hidden = true;
  exportCard.hidden = false;
};
$("route-import-tab").onclick = async () => {
  await window.disconnectGpxExport?.();
  container.hidden = false;
  exportCard.hidden = true;
};
async function disconnect() {
  if (client) {
    await client.close();
    client = null;
  }
  maximum = 0;
  $("route-connect").textContent = "Connect for import";
  controls();
}
$("route-file").onchange = async () => {
  const file = $("route-file").files[0],
    fileId = ++conversion;
  route = null;
  gpx = null;
  converting = false;
  controls();
  try {
    if (!file) return;
    log.phase(`Reading ${file.name} (${file.size} bytes)`);
    status("Reading GPX on this computer…");
    const xml = await file.text();
    if (fileId !== conversion) return;
    log.phase(`Parsing GPX XML (${xml.length} characters)`);
    await new Promise((r) => setTimeout(r, 0));
    if (fileId !== conversion) return;
    const doc = new DOMParser().parseFromString(xml, "application/xml");
    log.phase("Inspecting GPX tracks, segments and waypoints");
    await new Promise((r) => setTimeout(r, 0));
    if (fileId !== conversion) return;
    gpx = parseGpxDocument(doc);
    log.write(
      `GPX parsed: ${gpx.paths.length} tracks/routes, ${gpx.waypoints.length} waypoints`,
    );
    $("route-path").replaceChildren(
      ...gpx.paths.map((p, i) => {
        const o = document.createElement("option");
        o.value = i;
        o.textContent = p.name;
        return o;
      }),
    );
    $("route-selection").replaceChildren(
      ...gpx.waypoints.map((p, i) => {
        const label = document.createElement("label"),
          box = document.createElement("input");
        box.type = "checkbox";
        box.checked = true;
        box.dataset.waypoint = i;
        box.onchange = rebuild;
        label.append(
          box,
          document.createTextNode(p.name || `Waypoint ${i + 1}`),
        );
        label.style.display = "block";
        return label;
      }),
    );
    rebuild();
  } catch (e) {
    if (fileId !== conversion) return;
    gpx = null;
    route = null;
    $("route-path").replaceChildren();
    log.finish(e.stack || e.message, "error");
    status(e.message);
    controls();
  }
};
$("route-path").onchange = rebuild;
$("route-tolerance").onchange = rebuild;
$("route-height").onchange = rebuild;
$("route-named").onchange = rebuild;
function rebuild() {
  if (!gpx) return;
  converting = true;
  route = null;
  controls();
  conversionStarted = performance.now();
  const path = gpx.paths[Number($("route-path").value)];
  log.phase(
    `Converting "${path.name}" (${path.segments.length} segments); horizontal tolerance ${$("route-tolerance").value}m, elevation tolerance ${$("route-height").value}m`,
  );
  status("Converting route on this computer…");
  worker.postMessage({
    id: ++conversion,
    path: gpx.paths[Number($("route-path").value)],
    waypoints: gpx.waypoints.filter(
      (_, i) => document.querySelector(`[data-waypoint="${i}"]`).checked,
    ),
    options: {
      horizontal: Number($("route-tolerance").value),
      vertical: Number($("route-height").value),
      namedCheckpoints: $("route-named").checked,
    },
  });
}
worker.onmessage = async ({ data }) => {
  if (data.id !== conversion) return;
  if (data.progress) {
    log.phase(data.progress);
    return;
  }
  log.phase("Rendering route preview and checkpoint list");
  await new Promise((r) => setTimeout(r, 0));
  if (data.id !== conversion) return;
  converting = false;
  try {
    if (data.error) throw Error(data.error);
    route = data.route;
    $("route-summary").textContent =
      `${(route.distance / 1000).toFixed(2)} km · ${route.segments} segment(s) · ${route.points.length} / ${route.originalPoints} points · ${bytes(route.data.length)} · ${route.elevation ? "Elevation included (gaps stay missing)" : "No elevation"}${route.horizontal ? ` · tolerances: ${route.horizontal}m path / ${route.vertical}m height` : ""}`;
    $("route-segments").replaceChildren(
      ...route.points
        .filter((p) => p.start)
        .map((p, i) => {
          const end = route.points.findLast((q) => q.segment === p.segment),
            label = document.createElement("p");
          label.textContent = `Segment ${i + 1}: ${((end.distance - p.distance) / 100000).toFixed(2)} km, ${p.lat.toFixed(5)}, ${p.lon.toFixed(5)} → ${end.lat.toFixed(5)}, ${end.lon.toFixed(5)}`;
          return label;
        }),
    );
    $("route-checkpoints").replaceChildren(
      ...route.checkpoints.map((c) => {
        const p = document.createElement("p");
        p.textContent = `${c.name || "Checkpoint"}: ${(c.distance / 100000).toFixed(2)} km from start, ${(c.lateral / 100).toFixed(0)} m off path, segment ${c.segment + 1}`;
        return p;
      }),
    );
    draw(route);
    log.finish(
      `Conversion complete: ${route.points.length}/${route.originalPoints} points, ${route.checkpoints.length} checkpoints, ${route.data.length} bytes, CRC32 0x${route.crc.toString(16).padStart(8, "0")}; total ${((performance.now() - conversionStarted) / 1000).toFixed(2)}s`,
    );
    status(
      client && route.data.length > maximum
        ? "This converted route exceeds available storage. Simplify or select another track. The route will not be truncated."
        : "",
    );
  } catch (e) {
    route = null;
    log.finish(e.stack || e.message, "error");
    status(e.message);
  }
  controls();
};
worker.onerror = (e) => {
  converting = false;
  log.finish(`Conversion worker failed: ${e.message}`, "error");
  status(e.message);
  controls();
};
function draw(r) {
  const canvas = $("route-preview"),
    ctx = canvas.getContext("2d");
  ctx.clearRect(0, 0, 400, 180);
  let minLat = 90,
    maxLat = -90,
    minLon = 180,
    maxLon = -180;
  for (const p of r.points) {
    minLat = Math.min(minLat, p.lat);
    maxLat = Math.max(maxLat, p.lat);
    minLon = Math.min(minLon, p.lon);
    maxLon = Math.max(maxLon, p.lon);
  }
  let cos = Math.cos((((minLat + maxLat) / 2) * Math.PI) / 180),
    scale = Math.min(
      380 / Math.max((maxLon - minLon) * cos, 1e-6),
      160 / Math.max(maxLat - minLat, 1e-6),
    );
  ctx.strokeStyle = "#4ecca3";
  ctx.beginPath();
  for (const p of r.points) {
    const x = 10 + (p.lon - minLon) * cos * scale,
      y = 170 - (p.lat - minLat) * scale;
    if (p.start) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  }
  ctx.stroke();
}
function verificationDetails(reply) {
  if (reply.body.length < 23 || reply.body[5] !== 0xd1) return "";
  const v = new DataView(
    reply.body.buffer,
    reply.body.byteOffset,
    reply.body.byteLength,
  );
  const stages = {
    0: "idle",
    1: "header",
    2: "saved completion footer",
    3: "payload checksum",
    4: "route points",
    5: "checkpoints",
    6: "preview geometry",
    7: "index pages",
    8: "index relationships/bounds",
    10: "writing completion footer",
    11: "loading preview",
    13: "reading completion footer back",
    14: "checkpoint association",
  };
  const phase = reply.body[6];
  return `stage=${stages[phase] || phase} (${phase}), record=${v.getUint32(7, true)}, scanned bytes=${v.getUint32(11, true)}, expected CRC=0x${v.getUint32(15, true).toString(16).padStart(8, "0")}, scanned CRC=0x${v.getUint32(19, true).toString(16).padStart(8, "0")}`;
}
async function capacity() {
  let result = await client.request(OP.CAPABILITIES),
    v = new DataView(
      result.body.buffer,
      result.body.byteOffset,
      result.body.byteLength,
    );
  chunk = v.getUint16(0, true);
  maximum = v.getUint32(18, true);
  $("route-capacity").textContent =
    `Available for replacement: ${bytes(maximum)}. Total filesystem: ${bytes(v.getUint32(2, true))}; allocated: ${bytes(v.getUint32(6, true))}; reserved: ${bytes(v.getUint32(10, true))}; reclaimable inactive slot: ${bytes(v.getUint32(14, true))}.`;
  log.write(
    `Capacity: total=${v.getUint32(2, true)}, used=${v.getUint32(6, true)}, reserve=${v.getUint32(10, true)}, inactive reclaim=${v.getUint32(14, true)}, upload maximum=${maximum} bytes; chunk=${chunk}`,
  );
  controls();
}
$("route-connect").onclick = async () => {
  if (connecting) return;
  if (client) {
    await disconnect();
    return;
  }
  connecting = true;
  controls();
  try {
    if (!navigator.serial)
      throw Error("Use desktop Chrome over HTTPS or localhost.");
    await window.disconnectGpxExport?.();
    log.phase("Waiting for USB port selection");
    const port = await navigator.serial.requestPort();
    log.write(`Selected USB port: ${JSON.stringify(port.getInfo?.() || {})}`);
    log.phase("Connecting to device");
    client = new RouteSerial(port, {
      onLog: (message, level) => log.write(message, level),
      verbose: () => log.verbose(),
    });
    await client.open();
    $("route-connect").textContent = "Disconnect";
    log.phase("Querying device route status");
    let previousCheck = -1,
      lastRouteStatus = null;
    for (;;) {
      const s = await client.request(OP.STATUS, new Uint8Array(), 0);
      lastRouteStatus = s;
      const percent = new DataView(s.body.buffer, s.body.byteOffset).getUint32(
        0,
        true,
      );
      if (percent !== previousCheck) {
        log.write(
          `Boot verification: state=${s.state}, checksum scan=${percent}%, error=${s.body[4]}, ${verificationDetails(s)}`,
        );
        previousCheck = percent;
      }
      if (s.state !== 0) break;
      status(
        `Checking saved route: ${new DataView(s.body.buffer, s.body.byteOffset).getUint32(0, true)}%`,
      );
      await new Promise((r) => setTimeout(r, 250));
    }
    await capacity();
    const info = await client.request(OP.INFO);
    if (info.body.length >= 21)
      status(
        `Device route: ${new TextDecoder().decode(info.body.subarray(21, 21 + info.body[20]))}`,
      );
    else status("Device ready.");
    if (lastRouteStatus?.state === 5) {
      const message = `Previous route import failed (error ${lastRouteStatus.body[4]}${verificationDetails(lastRouteStatus) ? `; ${verificationDetails(lastRouteStatus)}` : ""}). ${info.body.length ? "Previous saved route remains available." : "No saved route is active."}`;
      status(message);
      log.finish(message, "warn");
    } else log.finish("Device connected and ready for import");
  } catch (e) {
    log.finish(e.stack || e.message, "error");
    status(e.message);
    await disconnect();
  } finally {
    connecting = false;
    controls();
  }
};
$("route-cancel").onclick = () => {
  cancel = true;
  log.write(
    "Cancellation requested; waiting for the current USB request",
    "warn",
  );
  status("Cancelling after the current request…");
};
$("route-upload").onclick = async () => {
  uploading = true;
  cancel = false;
  controls();
  const uploadStarted = performance.now();
  let lastProgress = uploadStarted;
  log.phase(
    `Starting upload: ${route.data.length} bytes, CRC32 0x${route.crc.toString(16).padStart(8, "0")}`,
  );
  try {
    const start = await client.request(
      OP.BEGIN,
      integers(route.data.length, route.crc),
      0,
    );
    client.session = start.session;
    log.phase(
      `Transferring route (session ${start.session}, chunk size ${chunk} bytes)`,
    );
    let offset = start.next;
    while (offset < route.data.length) {
      if (cancel) throw Error("Import cancelled.");
      const data = route.data.subarray(
          offset,
          Math.min(offset + chunk, route.data.length),
        ),
        body = new Uint8Array(4 + data.length);
      body.set(integers(offset));
      body.set(data, 4);
      const reply = await client.request(OP.DATA, body);
      if (reply.next !== offset + data.length)
        throw Error("Unexpected transfer offset.");
      offset = reply.next;
      const now = performance.now();
      if (now - lastProgress >= 1000 || offset === route.data.length) {
        const seconds = Math.max((now - uploadStarted) / 1000, 0.001),
          rate = offset / seconds;
        log.write(
          `Upload: ${offset}/${route.data.length} bytes (${((offset * 100) / route.data.length).toFixed(1)}%); ${rate.toFixed(0)} B/s; estimated ${Math.ceil((route.data.length - offset) / Math.max(rate, 1))}s remaining`,
        );
        lastProgress = now;
      }
      status(`Importing: ${bytes(offset)} / ${bytes(route.data.length)}`);
    }
    if (cancel) throw Error("Import cancelled.");
    log.phase("Committing uploaded route and starting device verification");
    await client.request(OP.COMMIT);
    log.phase("Waiting for route verification and activation");
    let previousVerify = "";
    for (;;) {
      if (cancel) throw Error("Import cancelled.");
      const s = await client.request(OP.STATUS);
      const progress = new DataView(s.body.buffer, s.body.byteOffset).getUint32(
        0,
        true,
      );
      const details = verificationDetails(s);
      const key = `${s.state}/${progress}/${s.body[4]}/${s.body[6] ?? ""}`;
      if (key !== previousVerify) {
        log.write(
          `Verification: state=${s.state}, checksum scan=${progress}%, error=${s.body[4]}, next=${s.next}, ${details}`,
        );
        previousVerify = key;
      }
      if (s.state === 1) break;
      if (s.state === 5 || s.body[4])
        throw Error(
          `Route verification failed (error ${s.body[4]}${details ? `; ${details}` : ""}). Any previous saved route is retained.`,
        );
      status(
        progress === 100
          ? "Checksum scan complete; device is validating route sections and index…"
          : `Verifying route: ${progress}%`,
      );
      await new Promise((r) => setTimeout(r, 100));
    }
    const saved = await client.request(OP.INFO);
    if (saved.body.length < 21)
      throw Error(
        "Device reported completion but no saved route metadata. Download this log.",
      );
    const savedView = new DataView(
      saved.body.buffer,
      saved.body.byteOffset,
      saved.body.byteLength,
    );
    if (
      savedView.getUint32(0, true) !== route.data.length ||
      savedView.getUint32(4, true) !== route.points.length
    )
      throw Error(
        "Device metadata does not match the uploaded route. Download this log.",
      );
    log.write(
      `Confirmed saved route: ${savedView.getUint32(0, true)} bytes, ${savedView.getUint32(4, true)} points`,
    );
    status("Route imported and verified. Open Tools → Location → Routes.");
    await capacity();
    log.finish(
      `Route imported and activated in ${((performance.now() - uploadStarted) / 1000).toFixed(1)}s`,
    );
  } catch (e) {
    log.write(`Upload stopped: ${e.message}`, "error");
    if (client?.session) {
      try {
        await client.request(OP.ABORT);
      } catch (abortError) {
        log.write(`ABORT reply: ${abortError.message}`, "warn");
      }
    }
    log.finish(e.stack || e.message, "error");
    status(e.message);
  } finally {
    uploading = false;
    controls();
  }
};
window.addEventListener("beforeunload", () => {
  client?.close();
  worker.terminate();
  log.destroy();
});
