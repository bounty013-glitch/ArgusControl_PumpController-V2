import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";

const html = fs.readFileSync(new URL("../main/argus_controls.html", import.meta.url), "utf8");
const trajectorySource = fs.readFileSync(new URL("../main/argus_trajectory.c", import.meta.url), "utf8");
const scriptMatch = html.match(/<script>([\s\S]*?)<\/script>/);
assert.ok(scriptMatch, "inline controls script is present");
new vm.Script(scriptMatch[1], { filename: "argus_controls.html" });

const recoverBody = trajectorySource.match(
  /esp_err_t argus_trajectory_recover\(void\)\s*\{([\s\S]*?)\n\}\n\nvoid argus_trajectory_clear_error/,
);
assert.ok(recoverBody, "trajectory recovery implementation is present");
assert.match(recoverBody[1], /clear_error_locked\(\)/);
assert.doesNotMatch(
  recoverBody[1],
  /argus_trajectory_clear_error\(\)/,
  "recovery must not reacquire the non-recursive trajectory mutex",
);

assert.equal((html.match(/fetch\("\/api\/command"/g) || []).length, 1);
assert.equal((html.match(/fetch\("\/api\/status"/g) || []).length, 1);
assert.doesNotMatch(html, /window\.argus(?:Raw|Command|Status)/);
assert.doesNotMatch(html, /authority_(?:owner|generation)\s*:/);

class FakeElement {
  constructor(id) {
    this.id = id;
    this.textContent = "";
    this.value = id === "target-rpm" ? "8.000" : "";
    this.checked = id === "dir-forward";
    this.disabled = false;
    this.className = "";
    this.listeners = new Map();
    this.classList = {
      toggle: (name, enabled) => {
        const names = new Set(this.className.split(/\s+/).filter(Boolean));
        enabled ? names.add(name) : names.delete(name);
        this.className = [...names].join(" ");
      },
    };
  }
  addEventListener(name, callback) { this.listeners.set(name, callback); }
  click() { return this.listeners.get("click")?.({ preventDefault() {} }); }
}

const ids = [...html.matchAll(/id="([^"]+)"/g)].map((match) => match[1]);
const elements = new Map(ids.map((id) => [id, new FakeElement(id)]));
const documentListeners = new Map();
const windowListeners = new Map();
let nextTimer = 1;
const timers = new Map();
const fetchCalls = [];
let pendingCommandResolve;
const commandResponses = [];

const status = {
  machine: {
    state: "UNLOCKED",
    target_rpm_milli: 8000,
    applied_rpm_milli: 0,
    generated_rpm_milli: 0,
    requested_forward: true,
    applied_forward: true,
    driver_enabled: false,
    estop_latched: false,
    ramp_active: false,
    fault_code: 0,
    command_generation: 1,
    feedback_available: false,
    last_rejection_reason: "",
  },
  authority: { mode: "LOCAL_SERVICE", owner: "BROWSER", generation: 7 },
  network: {
    mode: "SERVICE_AP_ONLY",
    ap_started: true,
    sta_state: "STOPPED",
    sta_ip_acquired: false,
    sta_ip_address: "",
  },
  broker: { running: false, stopped: true, observable: true },
};

function response(code, body) {
  return { ok: code >= 200 && code < 300, status: code, text: async () => JSON.stringify(body), json: async () => body };
}

async function fakeFetch(url, options = {}) {
  fetchCalls.push({ url, options });
  if (url === "/api/status") return response(200, status);
  if (url === "/api/identity") {
    return response(200, { device_name: "Argus", firmware_version: "v2-phase4b.5-dev", hardware_uid: "test" });
  }
  if (url === "/api/command") {
    if (commandResponses.length > 0) {
      const next = commandResponses.shift();
      if (next instanceof Error) throw next;
      return next;
    }
    return new Promise((resolve) => { pendingCommandResolve = resolve; });
  }
  throw new Error(`unexpected fetch ${url}`);
}

const context = {
  AbortController,
  console,
  fetch: fakeFetch,
  document: {
    hidden: false,
    getElementById: (id) => elements.get(id),
    addEventListener: (name, callback) => documentListeners.set(name, callback),
  },
  window: {
    addEventListener: (name, callback) => windowListeners.set(name, callback),
  },
  setTimeout: (callback, delay) => {
    const id = nextTimer++;
    timers.set(id, { callback, delay });
    return id;
  },
  clearTimeout: (id) => timers.delete(id),
  setInterval: (callback, delay) => {
    const id = nextTimer++;
    timers.set(id, { callback, delay, interval: true });
    return id;
  },
  clearInterval: (id) => timers.delete(id),
};
vm.createContext(context);
vm.runInContext(scriptMatch[1], context, { filename: "argus_controls.html" });

await new Promise((resolve) => setImmediate(resolve));
await new Promise((resolve) => setImmediate(resolve));

assert.equal(context.rpmStringToMilli("8.125"), 8125);
assert.equal(context.rpmStringToMilli("200"), 200000);
assert.equal(context.rpmStringToMilli("0.001"), 1);
for (const invalid of ["", "-1", "200.001", "8.0001", "1e1", "NaN"]) {
  assert.equal(context.rpmStringToMilli(invalid), null, `reject ${invalid}`);
}

const statusCall = fetchCalls.find((call) => call.url === "/api/status");
assert.equal(statusCall.options.credentials, "same-origin");
assert.equal(statusCall.options.cache, "no-store");
assert.equal(elements.get("cmd-start").disabled, false);

elements.get("cmd-start").click();
elements.get("cmd-start").click();
await new Promise((resolve) => setImmediate(resolve));
const commandCalls = fetchCalls.filter((call) => call.url === "/api/command");
assert.equal(commandCalls.length, 1, "duplicate click dispatches once");
assert.equal(commandCalls[0].options.credentials, "same-origin");
assert.deepEqual(JSON.parse(commandCalls[0].options.body), { command: "start" });
assert.equal(elements.get("machine-state").textContent, "UNLOCKED", "no optimistic state mutation");
pendingCommandResolve(response(200, { ok: true, result: "accepted" }));
await new Promise((resolve) => setImmediate(resolve));
await new Promise((resolve) => setImmediate(resolve));

async function verifyCommandResult(code, body, expectedTitle) {
  commandResponses.push(response(code, body));
  vm.runInContext("runtime.unauthorized = false;", context);
  await vm.runInContext('sendCommand({ command: "stop" }, "Stop")', context);
  assert.match(elements.get("command-result-title").textContent, expectedTitle);
}

await verifyCommandResult(400, { ok: false, error: "invalid_request" }, /rejected/);
await verifyCommandResult(401, { ok: false, error: "unauthorized" }, /unauthorized/);
await verifyCommandResult(403, { ok: false, error: "command_not_admitted" }, /rejected/);
await verifyCommandResult(409, { ok: false, error: "command_conflict" }, /rejected/);
await verifyCommandResult(500, { ok: false, error: "internal_error" }, /rejected/);

commandResponses.push({ ok: true, status: 200, text: async () => "not-json" });
vm.runInContext("runtime.unauthorized = false;", context);
await vm.runInContext('sendCommand({ command: "stop" }, "Stop")', context);
assert.match(elements.get("command-result-title").textContent, /result unknown/);

const transportError = new Error("offline");
commandResponses.push(transportError);
await vm.runInContext('sendCommand({ command: "stop" }, "Stop")', context);
assert.match(elements.get("command-result-title").textContent, /not confirmed/);
assert.match(elements.get("command-result-detail").textContent, /transport failed/i);

const beforeInvalid = fetchCalls.length;
elements.get("target-rpm").value = "200.001";
elements.get("cmd-set-target").click();
assert.equal(fetchCalls.length, beforeInvalid, "invalid target never dispatches");

vm.runInContext('runtime.lastSuccessMs = Date.now() - 4000; setFreshness("stale", "test", "test");', context);
assert.equal(elements.get("cmd-start").disabled, true);
assert.equal(elements.get("cmd-estop").disabled, false, "E-stop remains available with stale status");
vm.runInContext("runtime.unauthorized = true; updateEligibility();", context);
assert.equal(elements.get("cmd-estop").disabled, true, "unauthorized session cannot issue E-stop");

assert.ok(documentListeners.has("visibilitychange"));
assert.ok(windowListeners.has("pagehide"));
console.log("Phase 4B.5 controls host tests: PASS");
