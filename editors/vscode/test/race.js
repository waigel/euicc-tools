/*
 * A superseded check says nothing.
 *
 * A save landing while the typing timer is pending starts a second check at
 * the same document version and kills the first. The killed child dies with
 * empty stdout, which is the same shape as "the tool is missing", and until
 * the two were told apart the editor showed "cannot run euicc" on line 0
 * until the real answer overwrote it -- a warning that flickered on every
 * save, about a tool that was never absent.
 *
 * didOpen and didSave go out in one flush, so the first run is still alive
 * when the second one kills it.
 */

const { spawn } = require("node:child_process");
const EUICC = process.argv[2] || require("node:path").join(__dirname, "..", "..", "..", "euicc");
const srv = spawn("node", ["out/server.js", "--stdio"], { stdio: ["pipe", "pipe", "inherit"] });

const send = (m) => {
  const b = Buffer.from(JSON.stringify(m), "utf8");
  srv.stdin.write(`Content-Length: ${b.length}\r\n\r\n`);
  srv.stdin.write(b);
};

const DOC =
  "value1 ProfileElement ::= end : { end-header { mandated NULL, identification 99 } }\n"
  + "value2 ProfileElement ::= header : {\n  major-version 2, minor-version 3,\n"
  + "  iccid '89000123456789012341'H,\n  eUICC-Mandatory-services { usim NULL },\n"
  + "  eUICC-Mandatory-GFSTEList { { 2 23 143 1 2 1 } }\n}\n";

let real = 0;
let buf = Buffer.alloc(0);
srv.stdout.on("data", (d) => {
  buf = Buffer.concat([buf, d]);
  for (;;) {
    const i = buf.indexOf("\r\n\r\n");
    if (i < 0) return;
    const len = +/Content-Length: (\d+)/.exec(buf.slice(0, i).toString())[1];
    if (buf.length < i + 4 + len) return;
    const msg = JSON.parse(buf.slice(i + 4, i + 4 + len).toString());
    buf = buf.slice(i + 4 + len);
    if (msg.method === "workspace/configuration")
      send({ jsonrpc: "2.0", id: msg.id,
             result: [{ path: EUICC, rules: "", checkOn: "save", docs: "" }] });
    if (msg.method === "textDocument/publishDiagnostics") {
      for (const d of msg.params.diagnostics) {
        if (/cannot run/.test(d.message)) {
          console.log(`FAIL a superseded run published: ${d.message}`);
          process.exit(1);
        }
      }
      if (msg.params.diagnostics.length) real++;
    }
  }
});

send({ jsonrpc: "2.0", id: 1, method: "initialize",
       params: { processId: process.pid, rootUri: null, capabilities: {} } });
send({ jsonrpc: "2.0", method: "initialized", params: {} });
send({ jsonrpc: "2.0", method: "textDocument/didOpen", params: { textDocument: {
  uri: "file:///tmp/r.vn", languageId: "asn1-vn", version: 1, text: DOC } } });
/* The save arrives before the open's check has finished, and kills it. */
send({ jsonrpc: "2.0", method: "textDocument/didSave",
       params: { textDocument: { uri: "file:///tmp/r.vn" } } });
send({ jsonrpc: "2.0", method: "textDocument/didSave",
       params: { textDocument: { uri: "file:///tmp/r.vn" } } });

setTimeout(() => {
  if (!real) { console.log("FAIL no real diagnostics arrived at all"); process.exit(1); }
  console.log(`ok   ${real} real publishes, and no superseded run said a word`);
  process.exit(0);
}, 2500);
