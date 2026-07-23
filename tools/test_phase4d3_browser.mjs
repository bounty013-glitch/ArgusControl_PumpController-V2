import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";

const files = [
  "argus_login.html",
  "argus_controls.html",
  "argus_commission.html",
];
const pages = new Map();
for (const file of files) {
  const html = fs.readFileSync(new URL(`../main/${file}`, import.meta.url), "utf8");
  const script = html.match(/<script>([\s\S]*?)<\/script>/);
  assert.ok(script, `${file} inline script is present`);
  new vm.Script(script[1], { filename: file });
  assert.doesNotMatch(html, /localStorage|sessionStorage/);
  assert.doesNotMatch(html, /Authorization\s*:/);
  pages.set(file, html);
}

const login = pages.get("argus_login.html");
assert.match(login, /fetch\("\/api\/auth\/login"/);
assert.match(login, /credentials:\s*"same-origin"/);
assert.match(login, /passwordInput\.value\s*=\s*""/);
assert.match(login, /Login does not grant operating authority/);

const controls = pages.get("argus_controls.html");
assert.match(controls, /nativeFetch\("\/api\/auth\/session"/);
assert.match(controls, /X-Argus-CSRF/);
assert.match(controls, /runtime\.caps\.has\("motion"\)/);
assert.match(controls, /runtime\.caps\.has\("software_estop"\)/);
assert.match(controls, /fetch\("\/api\/auth\/logout"/);

const commission = pages.get("argus_commission.html");
assert.match(commission, /request\("\/api\/auth\/session"\)/);
assert.match(commission, /X-Argus-CSRF/);
assert.match(commission, /data-cap="manage_users"/);
assert.match(commission, /data-cap="manage_roles"/);
assert.match(commission, /data-cap="view_audit"/);
assert.match(commission, /data-all-cap="manage_network,change_ap_secret"/);
assert.match(commission, /post\("\/api\/security\/recovery\/exit"/);

const server = fs.readFileSync(
  new URL("../main/argus_http_server.c", import.meta.url),
  "utf8",
);
assert.doesNotMatch(server, /WWW-Authenticate|Basic realm|check_auth\s*\(/);
assert.match(server, /\.uri\s*=\s*"\/api\/auth\/login"/);
assert.match(server, /\.uri\s*=\s*"\/api\/auth\/session"/);
assert.match(server, /\.uri\s*=\s*"\/api\/auth\/logout"/);

console.log("Phase 4D.3 browser host tests: PASS");
