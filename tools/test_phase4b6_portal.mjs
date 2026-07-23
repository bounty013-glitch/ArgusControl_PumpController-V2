import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";

const html = fs.readFileSync(
  new URL("../main/argus_commission.html", import.meta.url),
  "utf8",
);
const source = fs.readFileSync(
  new URL("../main/argus_http_server.c", import.meta.url),
  "utf8",
);
const script = html.match(/<script>([\s\S]*?)<\/script>/);
assert.ok(script, "commissioning inline script is present");
new vm.Script(script[1], { filename: "argus_commission.html" });

assert.match(html, /request\("\/api\/auth\/session"\)/);
assert.match(html, /X-Argus-CSRF/);
assert.match(html, /credentials\s*=\s*"same-origin"/);
assert.match(
  html,
  /Erase customer configuration and reboot\? Security credentials are preserved\./,
);
assert.match(html, /confirm:"FACTORY_RESET"/);
assert.doesNotMatch(html, /Authorization\s*:/);
assert.doesNotMatch(html, /localStorage|sessionStorage/);

const handler = source.match(
  /static esp_err_t factory_reset_post_handler\([\s\S]*?\n\}/,
);
assert.ok(handler, "production factory-reset handler is present");
assert.match(handler[0], /require_access\(req, ARGUS_PERMISSION_COMMISSION/);
assert.match(handler[0], /recently_reauthenticated/);
assert.match(handler[0], /argus_factory_reset_content_type_valid/);
assert.match(handler[0], /argus_factory_reset_receive_body/);
assert.match(handler[0], /argus_factory_reset_decode/);
assert.match(handler[0], /argus_net_mgr_request_factory_reset/);
assert.doesNotMatch(handler[0], /argus_nvs_config_factory_reset/);
assert.doesNotMatch(handler[0], /argus_http_server_stop/);
assert.doesNotMatch(handler[0], /esp_restart/);

console.log("Phase 4B.6 portal/session host tests: PASS");
