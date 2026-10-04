export const HEADER = 96,
  POINT = 16,
  CP = 32,
  PAGE = 416,
  VERSION = 1,
  UNKNOWN = -32768;
export function crc32(bytes, initial = 0xffffffff) {
  let v = initial >>> 0;
  for (const b of bytes) {
    v ^= b;
    for (let i = 0; i < 8; i++) v = (v >>> 1) ^ (v & 1 ? 0xedb88320 : 0);
  }
  return ~v >>> 0;
}
const encoder = new TextEncoder();
export const wrap = (x) => {
  while (x > Math.PI) x -= 2 * Math.PI;
  while (x < -Math.PI) x += 2 * Math.PI;
  return x;
};
export function distance(a, b) {
  const r = Math.PI / 180,
    dl = (b.lat - a.lat) * r,
    do_ = wrap((b.lon - a.lon) * r);
  let q =
    Math.sin(dl / 2) ** 2 +
    Math.cos(a.lat * r) * Math.cos(b.lat * r) * Math.sin(do_ / 2) ** 2;
  return 6371000 * 2 * Math.asin(Math.sqrt(Math.min(1, q)));
}
export function project(p, a, b) {
  const r = Math.PI / 180,
    e = 6371000,
    c = Math.cos(p.lat * r),
    ax = wrap((a.lon - p.lon) * r) * c * e,
    ay = (a.lat - p.lat) * r * e,
    bx = wrap((b.lon - p.lon) * r) * c * e,
    by = (b.lat - p.lat) * r * e,
    dx = bx - ax,
    dy = by - ay;
  const f = Math.max(
    0,
    Math.min(1, -(ax * dx + ay * dy) / (dx * dx + dy * dy || 1)),
  );
  return { fraction: f, lateral: Math.hypot(ax + dx * f, ay + dy * f) };
}
const children = (e, name) =>
  Array.from(e.children).filter(
    (x) => x.localName === name && x.namespaceURI === e.namespaceURI,
  );
const text = (e, name) => children(e, name)[0]?.textContent?.trim() || "";
function coord(e) {
  if (
    !e.hasAttribute("lat") ||
    !e.hasAttribute("lon") ||
    !e.getAttribute("lat").trim() ||
    !e.getAttribute("lon").trim()
  )
    throw Error("A path point is missing latitude or longitude.");
  const lat = Number(e.getAttribute("lat")),
    lon = Number(e.getAttribute("lon"));
  if (
    !Number.isFinite(lat) ||
    !Number.isFinite(lon) ||
    Math.abs(lat) > 90 ||
    Math.abs(lon) > 180
  )
    throw Error("Invalid GPS coordinates.");
  const h = text(e, "ele");
  const alt = h === "" ? null : Number(h);
  if (alt !== null && (!Number.isFinite(alt) || alt < -32767 || alt > 32767))
    throw Error("Invalid elevation.");
  return { lat, lon, alt, name: text(e, "name") };
}
export function parseGpxDocument(doc) {
  if (doc.querySelector("parsererror"))
    throw Error("The GPX XML could not be read.");
  const root = doc.documentElement;
  if (
    root.localName !== "gpx" ||
    ![
      "http://www.topografix.com/GPX/1/0",
      "http://www.topografix.com/GPX/1/1",
    ].includes(root.namespaceURI) ||
    !["1.0", "1.1"].includes(root.getAttribute("version"))
  )
    throw Error("Select a GPX 1.0 or 1.1 file.");
  const paths = [];
  for (const trk of children(root, "trk")) {
    const segments = children(trk, "trkseg")
      .map((s) => children(s, "trkpt").map(coord))
      .filter((s) => s.length);
    if (segments.length)
      paths.push({
        name: text(trk, "name") || `Track ${paths.length + 1}`,
        segments,
      });
  }
  for (const rte of children(root, "rte")) {
    const points = children(rte, "rtept").map(coord);
    if (points.length)
      paths.push({
        name: text(rte, "name") || `Route ${paths.length + 1}`,
        segments: [points],
      });
  }
  if (!paths.length) throw Error("This GPX contains no track or route.");
  return { paths, waypoints: children(root, "wpt").map(coord) };
}
function bounds(points) {
  let minLat = 90,
    minLon = 180,
    maxLat = -90,
    maxLon = -180;
  for (const p of points) {
    minLat = Math.min(minLat, p.lat);
    minLon = Math.min(minLon, p.lon);
    maxLat = Math.max(maxLat, p.lat);
    maxLon = Math.max(maxLon, p.lon);
  }
  if (maxLon - minLon > 180) {
    minLon = -180;
    maxLon = 180;
  }
  return [
    Math.floor(minLat * 1e6),
    Math.floor(minLon * 1e6),
    Math.ceil(maxLat * 1e6),
    Math.ceil(maxLon * 1e6),
  ];
}
const union = (boxes) => [
  Math.min(...boxes.map((b) => b[0])),
  Math.min(...boxes.map((b) => b[1])),
  Math.max(...boxes.map((b) => b[2])),
  Math.max(...boxes.map((b) => b[3])),
];
export function associate(points, waypoints) {
  return waypoints.map((w) => {
    let best = { lateral: Infinity },
      index = 0;
    for (let i = 0; i + 1 < points.length; i++) {
      if (points[i + 1].start) continue;
      const p = project(w, points[i], points[i + 1]);
      if (p.lateral < best.lateral) {
        best = p;
        index = i;
      }
    }
    if (!Number.isFinite(best.lateral))
      throw Error("The selected path has no usable legs.");
    const a = points[index],
      b = points[index + 1];
    return {
      ...w,
      distance: Math.round(
        a.distance + (b.distance - a.distance) * best.fraction,
      ),
      segment: a.segment,
      point: index,
      lateral: Math.round(best.lateral * 100),
    };
  });
}
function simplifySegment(points, horizontal, vertical, pinned) {
  if (horizontal === 0) return points;
  const keep = new Set([0, points.length - 1, ...pinned]);
  const anchors = [...keep].sort((a, b) => a - b),
    stack = [];
  for (let i = 1; i < anchors.length; i++)
    stack.push([anchors[i - 1], anchors[i]]);
  while (stack.length) {
    const [a, b] = stack.pop();
    if (b <= a + 1) continue;
    let worst = 1,
      index = -1;
    for (let i = a + 1; i < b; i++) {
      const p = project(points[i], points[a], points[b]);
      let score = p.lateral / horizontal;
      const aa = points[a].alt,
        bb = points[b].alt,
        hh = points[i].alt;
      if (aa !== null && bb !== null && hh !== null)
        score = Math.max(
          score,
          Math.abs(hh - aa - (bb - aa) * p.fraction) / (vertical || 0.001),
        );
      else if (
        (hh === null) !== (aa === null) ||
        (hh === null) !== (bb === null)
      )
        score = Infinity;
      if (score > worst) {
        worst = score;
        index = i;
      }
    }
    if (index >= 0) {
      keep.add(index);
      stack.push([a, index], [index, b]);
    }
  }
  return points.filter((_, i) => keep.has(i));
}
function flatten(segments) {
  let cm = 0,
    points = [];
  segments.forEach((segment, s) =>
    segment.forEach((p, i) => {
      if (i) cm += distance(segment[i - 1], p) * 100;
      points.push({
        ...p,
        distance: Math.round(cm),
        segment: s,
        start: i === 0,
      });
    }),
  );
  return points;
}
export function convert(path, waypoints = [], options = {}) {
  const progress = (message) => options.onProgress?.(message);
  progress("Validating coordinates and elevation");
  for (const p of path.segments.flat().concat(waypoints))
    if (
      !Number.isFinite(p.lat) ||
      !Number.isFinite(p.lon) ||
      Math.abs(p.lat) > 90 ||
      Math.abs(p.lon) > 180 ||
      (p.alt !== null &&
        (!Number.isFinite(p.alt) || p.alt < -32767 || p.alt > 32767))
    )
      throw Error("Invalid coordinates or elevation.");
  if (path.segments.some((s) => s.length < 2))
    throw Error("Each route segment needs at least two points.");
  progress("Calculating source distances");
  const original = flatten(path.segments);
  let selected = waypoints.concat(
    options.namedCheckpoints === false ? [] : original.filter((p) => p.name),
  );
  progress(
    `Associating ${selected.length} checkpoints with ${original.length} points`,
  );
  let cp = associate(original, selected);
  const horizontal = Number(options.horizontal) || 0,
    vertical = Number(options.vertical) || 0;
  progress("Simplifying geometry (or preserving all points)");
  const segments = path.segments.map((s, seg) => {
    const first = original.findIndex((p) => p.segment === seg);
    const pinned = cp
      .filter((c) => c.segment === seg)
      .flatMap((c) => [c.point - first, c.point - first + 1]);
    return simplifySegment(s, horizontal, vertical, pinned);
  });
  const points = flatten(segments);
  progress("Calculating final distances and checkpoint offsets");
  cp = associate(points, selected);
  // Preview represents segment boundaries and elevation gaps, never adds a connecting leg.
  const required = new Set([0, points.length - 1]);
  for (let i = 0; i < points.length; i++) {
    if (points[i].start) {
      required.add(i);
      if (i) required.add(i - 1);
    }
    if (i && (points[i].alt === null) !== (points[i - 1].alt === null)) {
      required.add(i);
      required.add(i - 1);
    }
  }
  // More discontinuities than the preview budget are shown as separate mini previews; no false bridges.
  let indices = [...required].sort((a, b) => a - b);
  if (indices.length > 64) {
    indices = Array.from(
      { length: 64 },
      (_, i) => indices[Math.round((i * (indices.length - 1)) / 63)],
    );
  }
  for (let i = 0; indices.length < Math.min(64, points.length) && i < 64; i++) {
    const index = Math.round((i * (points.length - 1)) / 63);
    if (!indices.includes(index)) indices.push(index);
  }
  indices.sort((a, b) => a - b);
  const previews = indices.map((i, k) => {
    const skipped = k ? points.slice(indices[k - 1] + 1, i + 1) : [];
    return {
      ...points[i],
      start: points[i].start || skipped.some((p) => p.start),
      heightGap: skipped.some((p) => p.alt === null),
    };
  });
  progress("Building the spatial index and preview");
  const pages = [];
  for (let first = 0; first < points.length; first += 64) {
    const count = Math.min(64, points.length - first),
      bbox = bounds(
        points.slice(first, Math.min(points.length, first + count + 1)),
      );
    pages.push({
      kind: 1,
      first,
      last: first + count - 1,
      bbox,
      entries: [{ bbox, ref: first, extent: count }],
    });
  }
  let level = pages.map((_, i) => i);
  while (level.length > 1) {
    const next = [];
    for (let i = 0; i < level.length; i += 16) {
      const ids = level.slice(i, i + 16),
        kids = ids.map((i) => pages[i]),
        bbox = union(kids.map((p) => p.bbox));
      const node = {
        kind: 0,
        first: kids[0].first,
        last: kids.at(-1).last,
        bbox,
        entries: ids.map((i) => ({ bbox: pages[i].bbox, ref: i, extent: 0 })),
      };
      next.push(pages.length);
      pages.push(node);
    }
    level = next;
  }
  const clean = (s) =>
    (s || "Checkpoint").replace(/[\u0000-\u001f\u007f]/g, " ");
  const names = [
    encoder.encode(clean(path.name || "Planned route")),
    ...cp.map((c) => encoder.encode(clean(c.name))),
  ];
  if (names.some((n) => n.length > 65535))
    throw Error("A route or checkpoint name is too long.");
  let labelBytes = names.reduce((n, b) => n + b.length, 0),
    pointOffset = HEADER,
    cpOffset = pointOffset + points.length * POINT,
    labelOffset = cpOffset + cp.length * CP,
    previewOffset = labelOffset + labelBytes,
    indexOffset = previewOffset + previews.length * POINT,
    bytes = indexOffset + pages.length * PAGE;
  if (bytes > 0xffffffff - 20 || points.at(-1).distance > 0xffffffff)
    throw Error("This route exceeds the format address range.");
  progress(`Serializing ${bytes} bytes and calculating checksum`);
  const data = new Uint8Array(bytes),
    v = new DataView(data.buffer);
  const u16 = (o, n) => v.setUint16(o, n, true),
    u32 = (o, n) => v.setUint32(o, n, true),
    i32 = (o, n) => v.setInt32(o, n, true);
  const writePoint = (o, p) => {
    i32(o, Math.round(p.lat * 1e6));
    i32(o + 4, Math.round(p.lon * 1e6));
    u32(o + 8, p.distance);
    v.setInt16(o + 12, p.alt === null ? UNKNOWN : Math.round(p.alt), true);
    u16(o + 14, (p.start ? 1 : 0) | (p.heightGap ? 2 : 0));
  };
  u32(0, 0x3152434d);
  u16(4, 1);
  u16(6, HEADER);
  [
    bytes,
    points.length,
    cp.length,
    previews.length,
    pages.length,
    pointOffset,
    cpOffset,
    labelOffset,
    previewOffset,
    indexOffset,
    level[0],
    points.at(-1).distance,
    segments.length,
    labelBytes,
    0,
  ].forEach((n, i) => u32(8 + i * 4, n));
  u16(68, names[0].length);
  bounds(points).forEach((n, i) => i32(72 + i * 4, n));
  u32(88, crc32(data.subarray(0, 88)));
  points.forEach((p, i) => writePoint(pointOffset + i * POINT, p));
  let labelCursor = 0;
  names.forEach((n) => {
    data.set(n, labelOffset + labelCursor);
    labelCursor += n.length;
  });
  labelCursor = names[0].length;
  cp.forEach((c, i) => {
    let o = cpOffset + i * CP;
    i32(o, Math.round(c.lat * 1e6));
    i32(o + 4, Math.round(c.lon * 1e6));
    [c.distance, c.segment, c.point, c.lateral, labelCursor].forEach((n, j) =>
      u32(o + 8 + j * 4, n),
    );
    u16(o + 28, names[i + 1].length);
    labelCursor += names[i + 1].length;
  });
  previews.forEach((p, i) => writePoint(previewOffset + i * POINT, p));
  pages.forEach((p, i) => {
    const o = indexOffset + i * PAGE;
    p.bbox.forEach((n, j) => i32(o + j * 4, n));
    data[o + 16] = p.kind;
    data[o + 17] = p.entries.length;
    u32(o + 20, p.first);
    u32(o + 24, p.last);
    p.entries.forEach((e, j) => {
      const at = o + 32 + j * 24;
      e.bbox.forEach((n, k) => i32(at + k * 4, n));
      u32(at + 16, e.ref);
      u32(at + 20, e.extent);
    });
  });
  return {
    data,
    crc: crc32(data),
    points,
    checkpoints: cp,
    previews,
    originalPoints: original.length,
    distance: points.at(-1).distance / 100,
    segments: segments.length,
    elevation: points.some((p) => p.alt !== null),
    horizontal,
    vertical,
  };
}
