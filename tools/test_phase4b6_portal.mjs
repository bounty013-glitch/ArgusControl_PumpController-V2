import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";

const source = fs.readFileSync(new URL("../main/argus_http_server.c", import.meta.url), "utf8");
const marker = "static const char PORTAL_HTML[] =";
const start = source.indexOf(marker);
assert.notEqual(start, -1, "embedded portal declaration is present");

let pos = start + marker.length;
let html = "";
while (pos < source.length) {
  while (/\s/.test(source[pos])) pos++;
  if (source[pos] === ";") break;
  if (source.startsWith("/*", pos)) {
    const end = source.indexOf("*/", pos + 2);
    assert.notEqual(end, -1, "portal comment is terminated");
    pos = end + 2;
    continue;
  }
  assert.equal(source[pos], '"', `expected C string literal at offset ${pos}`);
  const literalStart = pos++;
  let escaped = false;
  while (pos < source.length) {
    const ch = source[pos++];
    if (escaped) {
      escaped = false;
    } else if (ch === "\\") {
      escaped = true;
    } else if (ch === '"') {
      break;
    }
  }
  html += JSON.parse(source.slice(literalStart, pos));
}

const script = html.match(/<script>([\s\S]*?)<\/script>/);
assert.ok(script, "portal inline script is present");
new vm.Script(script[1], { filename: "PORTAL_HTML" });

assert.match(html, /Factory Reset erases identity, identity lock, and Wi-Fi configuration\. The portal password is preserved\./);
assert.match(html, /confirm\(warning\)/);
assert.match(html, /JSON\.stringify\(\{confirm:'FACTORY_RESET'\}\)/);
assert.match(html, /nmode==='SERVICE_AP_ONLY'&&a\.mode==='LOCAL_SERVICE'&&a\.owner==='BROWSER'/);
assert.match(html, /stationarySafe\(m\)/);
assert.match(html, /factory_reset_pending/);
assert.match(html, /lifecyclePending/);
assert.match(html, /Result Unknown/);
assert.match(html, /Reconnect to the Service AP/);
assert.doesNotMatch(html, /Authorization:/);
assert.doesNotMatch(html, /sta_pass/);

const handler = source.match(/static esp_err_t factory_reset_post_handler\([\s\S]*?\n\}/);
assert.ok(handler, "production factory-reset handler is present");
assert.match(handler[0], /check_auth\(req\)/);
assert.match(handler[0], /argus_factory_reset_content_type_valid/);
assert.match(handler[0], /argus_factory_reset_receive_body/);
assert.match(handler[0], /argus_factory_reset_decode/);
assert.match(handler[0], /argus_net_mgr_request_factory_reset/);
assert.doesNotMatch(handler[0], /argus_nvs_config_factory_reset/);
assert.doesNotMatch(handler[0], /argus_http_server_stop/);
assert.doesNotMatch(handler[0], /esp_restart/);

console.log("Phase 4B.6 portal host tests: PASS");
