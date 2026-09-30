import test from "node:test";
import assert from "node:assert/strict";
import { convert, crc32, project, distance, HEADER, PAGE } from "./format.mjs";
import { RouteSerial } from "./serial.mjs";
import fs from "node:fs";
const p = (lat, lon = 0, alt = 100, name = "") => ({ lat, lon, alt, name });
test("preserves geometry, segment gaps, missing heights and UTF-8 checkpoints", () => {
  const r = convert(
    {
      name: "山 trail",
      segments: [
        [p(0), p(0.001, 0, null)],
        [p(0.01), p(0.011)],
      ],
    },
    [p(0.0005, 0.0001, 110, "Water 水")],
  );
  const v = new DataView(r.data.buffer);
  assert.equal(v.getUint32(0, true), 0x3152434d);
  assert.equal(v.getUint32(12, true), 4);
  assert.equal(v.getUint32(56, true), 2);
  assert.equal(v.getUint32(88, true), crc32(r.data.subarray(0, 88)));
  assert.ok(r.distance > 200 && r.distance < 225);
  assert.ok(r.checkpoints[0].lateral > 1000);
  assert.equal(r.points[2].distance, r.points[1].distance);
  assert.equal(r.points[1].alt, null);
  assert.equal(r.points[2].start, true);
});
test("routes above 32KB are supported and indexes have backward-only children", () => {
  const r = convert({
    name: "Long hike",
    segments: [
      Array.from({ length: 5000 }, (_, i) =>
        p(i * 0.0001, Math.sin(i / 20) * 0.0001, 100 + i / 50),
      ),
    ],
  });
  assert.ok(r.data.length > 32768);
  assert.equal(r.points.length, 5000);
  const v = new DataView(r.data.buffer),
    offset = v.getUint32(44, true),
    pages = v.getUint32(24, true);
  for (let i = 0; i < pages; i++) {
    let base = offset + i * PAGE;
    if (r.data[base + 16] === 0)
      for (let j = 0; j < r.data[base + 17]; j++)
        assert.ok(v.getUint32(base + 32 + j * 24 + 16, true) < i);
  }
});
test("simplification retains a crest and named checkpoint", () => {
  const points = Array.from({ length: 100 }, (_, i) =>
    p(i * 0.0001, 0, 100 + 50 - Math.abs(50 - i), i === 20 ? "Camp" : ""),
  );
  const r = convert({ name: "Crest", segments: [points] }, [], {
    horizontal: 5,
    vertical: 2,
  });
  assert.ok(r.points.length < 100);
  assert.ok(r.points.some((p) => p.name === "Camp"));
  assert.ok(Math.max(...r.points.map((p) => p.alt)) >= 149);
  assert.equal(r.checkpoints.length, 1);
});
test("negative heights, duplicate coordinates, and antimeridian geometry", () => {
  const r = convert({
    name: "Coast",
    segments: [[p(1, 179.999, -20), p(1, 179.999, -20), p(1, -179.999, -10)]],
  });
  assert.ok(r.distance < 300);
  const q = project(p(1, 180), r.points[1], r.points[2]);
  assert.ok(q.lateral < 1);
  assert.ok(distance(r.points[1], r.points[2]) < 300);
});
test("rejects nonfinite coordinates, invalid elevation and isolated one-point segments", () => {
  assert.throws(() => convert({ name: "Bad", segments: [[p(0)]] }));
  // DOM parser rejects malformed coordinates before conversion; public converter must too.
  assert.throws(() => convert({ name: "Bad", segments: [[p(NaN), p(1)]] }));
});
test("serial parser handles fragmented frames and ignores ordinary companion pushes", () => {
  const s = new RouteSerial({});
  let result;
  s.pending = { op: 5, id: 7, resolve: (r) => (result = r), reject: () => {} };
  const payload = new Uint8Array(17);
  payload.set([127, 83, 82, 1, 5, 7, 0]);
  payload[12] = 1;
  const f = new Uint8Array(20);
  f.set([62, 17, 0]);
  f.set(payload, 3);
  s.receive(new Uint8Array([62, 1, 0, 128]));
  s.receive(f.subarray(0, 8));
  assert.equal(result, undefined);
  s.receive(f.subarray(8));
  assert.equal(result.state, 1);
});
// Canonical fixture consumed by the C++ native tests: one continuous route and two segments.
if (process.env.ROUTE_FIXTURE_DIR) {
  fs.mkdirSync(process.env.ROUTE_FIXTURE_DIR, { recursive: true });
  for (const [name, path] of Object.entries({
    straight: {
      name: "Water 水 route",
      segments: [
        Array.from({ length: 140 }, (_, i) => p(i * 0.0001, 0, 100 + i / 3)),
      ],
    },
    segments: {
      name: "Two segments",
      segments: [
        [p(0), p(0.001), p(0.002)],
        [p(0.02), p(0.021), p(0.022)],
      ],
    },
    large: {
      name: "Large",
      segments: [Array.from({ length: 5000 }, (_, i) => p(i * 0.0001))],
    },
    crossing: {
      name: "Crossing",
      segments: [
        [
          p(-0.001, -0.001),
          p(0.001, 0.001),
          p(0.001, -0.001),
          p(-0.001, 0.001),
        ],
      ],
    },
    switchbacks: {
      name: "Switchbacks",
      segments: [
        [
          p(0, 0),
          p(0.001, 0),
          p(0.001, 0.0001),
          p(0, 0.0001),
          p(0, 0.0002),
          p(0.001, 0.0002),
        ],
      ],
    },
  })) {
    const r = convert(path, [p(0.0005, 0.0002, 100, "Spring 水")]);
    fs.writeFileSync(`${process.env.ROUTE_FIXTURE_DIR}/${name}.bin`, r.data);
  }
}

test("USB retries retain request ID and exactly reuse the chunk", async (t) => {
  t.mock.timers.enable({ apis: ["setTimeout"] });
  const s = new RouteSerial({}),
    sent = [];
  s.writer = {
    write: async (frame) => {
      sent.push(frame.slice());
      if (sent.length === 4) {
        const reply = new Uint8Array(17),
          v = new DataView(reply.buffer);
        reply.set([127, 83, 82, 1, frame[7], frame[8], frame[9]]);
        reply[12] = 3;
        v.setUint32(13, 144, true);
        const wire = new Uint8Array(20);
        wire.set([62, 17, 0]);
        wire.set(reply, 3);
        s.receive(wire);
      }
    },
  };
  const request = s.request(4, new Uint8Array(148));
  for (let i = 0; i < 3; i++) {
    t.mock.timers.tick(5000);
    await Promise.resolve();
  }
  const result = await request;
  assert.equal(result.next, 144);
  assert.equal(sent.length, 4);
  for (const frame of sent) assert.deepEqual(frame, sent[0]);
});
test("disconnect closes pending requests without retrying a released writer", async () => {
  const s = new RouteSerial({ close: async () => {} });
  let writes = 0;
  s.writer = {
    write: async () => {
      writes++;
    },
    releaseLock: () => {},
  };
  const pending = s.request(5);
  const rejected = assert.rejects(pending, /Disconnected/);
  await s.close();
  await rejected;
  assert.equal(writes, 1);
  assert.equal(s.pending, null);
});

test("missing elevation breaks only the profile, not the route map", () => {
  const r = convert({
    name: "Partial height",
    segments: [[p(0), p(0.001, 0, null), p(0.002)]],
  });
  assert.equal(r.previews[1].start, false);
  assert.equal(r.previews[1].heightGap, true);
});
