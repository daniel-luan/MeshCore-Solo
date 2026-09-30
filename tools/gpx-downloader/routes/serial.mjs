export const OP = {
  CAPABILITIES: 1,
  INFO: 2,
  BEGIN: 3,
  DATA: 4,
  STATUS: 5,
  COMMIT: 6,
  ABORT: 7,
  DELETE: 8,
};
const ERROR = [
  "OK",
  "Unsupported command",
  "Device busy",
  "Invalid request",
  "Not enough storage",
  "Storage error",
  "Invalid route",
  "Expired session",
  "Unexpected byte offset",
  "Disconnect BLE before importing",
];
export class RouteSerial {
  constructor(port) {
    this.port = port;
    this.buffer = new Uint8Array();
    this.sequence = 0;
    this.pending = null;
    this.session = 0;
    this.closed = false;
  }
  async open() {
    await this.port.open({ baudRate: 115200 });
    this.writer = this.port.writable.getWriter();
    this.readTask = this.readLoop();
  }
  async readLoop() {
    try {
      while (this.port.readable && !this.closed) {
        this.reader = this.port.readable.getReader();
        try {
          for (;;) {
            const { value, done } = await this.reader.read();
            if (done) return;
            this.receive(value);
          }
        } finally {
          this.reader.releaseLock();
          this.reader = null;
        }
      }
    } catch (e) {
      this.failPending(e);
    } finally {
      this.closed = true;
      this.failPending(Error("Device disconnected."));
    }
  }
  failPending(error) {
    if (!this.pending) return;
    const pending = this.pending;
    clearTimeout(pending.timer);
    this.pending = null;
    pending.reject(error);
  }
  receive(bytes) {
    let combined = new Uint8Array(this.buffer.length + bytes.length);
    combined.set(this.buffer);
    combined.set(bytes, this.buffer.length);
    this.buffer = combined;
    while (this.buffer.length >= 3) {
      if (this.buffer[0] !== 62) {
        this.buffer = this.buffer.subarray(1);
        continue;
      }
      let n = this.buffer[1] | (this.buffer[2] << 8);
      if (!n || n > 176) {
        this.buffer = this.buffer.subarray(1);
        continue;
      }
      if (this.buffer.length < 3 + n) return;
      let p = this.buffer.slice(3, 3 + n);
      this.buffer = this.buffer.slice(3 + n);
      if (
        p.length < 17 ||
        p[0] !== 127 ||
        p[1] !== 83 ||
        p[2] !== 82 ||
        p[3] !== 1
      )
        continue;
      const v = new DataView(p.buffer);
      if (
        this.pending &&
        p[4] === this.pending.op &&
        v.getUint16(5, true) === this.pending.id
      ) {
        clearTimeout(this.pending.timer);
        const pending = this.pending;
        this.pending = null;
        pending.resolve({
          session: v.getUint32(7, true),
          error: p[11],
          state: p[12],
          next: v.getUint32(13, true),
          body: p.slice(17),
        });
      }
    }
  }
  async request(op, body = new Uint8Array(), session = this.session) {
    if (this.closed) throw Error("Disconnected.");
    if (this.pending) throw Error("Another request is active.");
    const id = ++this.sequence & 65535,
      p = new Uint8Array(11 + body.length),
      v = new DataView(p.buffer);
    p.set([240, 83, 82, 1, op]);
    v.setUint16(5, id, true);
    v.setUint32(7, session, true);
    p.set(body, 11);
    if (p.length > 176) throw Error("Frame too large.");
    const frame = new Uint8Array(p.length + 3);
    frame.set([60, p.length & 255, p.length >> 8]);
    frame.set(p, 3);
    let result;
    for (let retry = 0; retry < 4; retry++) {
      try {
        result = await new Promise((resolve, reject) => {
          this.pending = {
            id,
            op,
            resolve,
            reject,
            timer: setTimeout(() => {
              this.pending = null;
              reject(
                Error(
                  "Request timed out. Disconnect BLE and other USB clients, then reconnect.",
                ),
              );
            }, 5000),
          };
          this.writer.write(frame).catch((e) => {
            if (this.pending?.id === id) {
              clearTimeout(this.pending.timer);
              this.pending = null;
              reject(e);
            }
          });
        });
        break;
      } catch (e) {
        if (retry === 3 || this.closed) throw e;
      }
    }
    if (result.error)
      throw Object.assign(Error(ERROR[result.error] || "Device error"), {
        code: result.error,
      });
    return result;
  }
  async close() {
    this.closed = true;
    if (this.pending) {
      clearTimeout(this.pending.timer);
      this.pending.reject(Error("Disconnected."));
      this.pending = null;
    }
    if (this.reader) await this.reader.cancel().catch(() => {});
    await this.readTask?.catch(() => {});
    this.writer?.releaseLock();
    await this.port.close().catch(() => {});
  }
}
export function integers(...values) {
  const data = new Uint8Array(values.length * 4),
    v = new DataView(data.buffer);
  values.forEach((x, i) => v.setUint32(i * 4, x, true));
  return data;
}
