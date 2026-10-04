// Bounded log and batched rendering keep diagnostics inexpensive during uploads.
export class ActivityLog {
  constructor(parent) {
    this.lines = [];
    this.started = performance.now();
    this.phaseStarted = this.started;
    this.activity = "Ready";
    this.active = false;
    this.detail = () => "";
    this.panel = document.createElement("section");
    this.panel.className = "card";
    this.panel.style.marginTop = "1rem";
    this.panel.innerHTML = `<h2>Activity log</h2><p id="route-log-live" role="status"></p>
<p><button id="route-log-clear">Clear</button> <button id="route-log-download">Download log</button></p>
<label><input id="route-log-verbose" type="checkbox"> Detailed USB requests and replies</label>
<textarea id="route-log" readonly aria-label="Activity log" spellcheck="false" style="width:100%;height:230px;margin-top:12px;background:#12121e;color:#e8e8f0;font:12px monospace;resize:vertical"></textarea>`;
    parent.append(this.panel);
    this.output = this.panel.querySelector("textarea");
    this.live = this.panel.querySelector("#route-log-live");
    this.panel.querySelector("#route-log-clear").onclick = () => {
      this.lines.length = 0;
      this.output.value = "";
    };
    this.panel.querySelector("#route-log-download").onclick = () => {
      const url = URL.createObjectURL(
        new Blob([this.lines.join("\n") + "\n"], { type: "text/plain" }),
      );
      const a = document.createElement("a");
      a.href = url;
      a.download = "meshcore-gpx-log.txt";
      a.click();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
    };
    this.panel.querySelector("#route-log-verbose").onchange = () =>
      this.write(
        `Detailed USB logging ${this.verbose() ? "enabled" : "disabled"}`,
      );
    this.timer = setInterval(() => this.refresh(), 500);
    this.write(
      `Page ready. Web Serial: ${navigator.serial ? "available" : "unavailable"}. Origin: ${location.origin}`,
    );
    this.refresh();
  }
  verbose() {
    return this.panel.querySelector("#route-log-verbose").checked;
  }
  write(message, level = "info") {
    const line = `${new Date().toISOString()} +${((performance.now() - this.started) / 1000).toFixed(1)}s [${level.toUpperCase()}] ${message}`;
    this.lines.push(line);
    if (this.lines.length > 500) this.lines.splice(0, this.lines.length - 500);
    this.dirty = true;
    if (level === "error") console.error(line);
    else if (level === "warn") console.warn(line);
    else if (level === "trace") console.debug(line);
    else console.info(line);
  }
  phase(message) {
    this.activity = message;
    this.phaseStarted = performance.now();
    this.active = true;
    this.write(message);
    this.refresh();
  }
  finish(message, level = "info") {
    this.write(
      `${message} (stage ${((performance.now() - this.phaseStarted) / 1000).toFixed(1)}s)`,
      level,
    );
    this.activity = message;
    this.active = false;
    this.refresh();
  }
  refresh() {
    const detail = this.detail();
    this.live.textContent = `${this.activity}${this.active ? ` — ${((performance.now() - this.phaseStarted) / 1000).toFixed(1)}s elapsed` : ""}${detail ? ` · ${detail}` : ""}`;
    if (!this.dirty) return;
    const follow =
      this.output.scrollTop + this.output.clientHeight >=
      this.output.scrollHeight - 30;
    this.output.value = this.lines.join("\n");
    if (follow) this.output.scrollTop = this.output.scrollHeight;
    this.dirty = false;
  }
  destroy() {
    clearInterval(this.timer);
  }
}
