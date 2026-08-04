/*
 * Two kinds of finding, two cadences.
 *
 * What the reader and the constraints say is recomputed at every pause in
 * typing; what the rule set says is recomputed on open and save. Between
 * saves the rule findings stand as last computed, the way a type error
 * outlives the syntax error you are in the middle of making -- and a save
 * replaces them, so none survives that the package no longer earns.
 *
 * The document opens with its header in the wrong place (two ordering rules),
 * then an edit breaks the parse, then a save. Three publishes, and each must
 * hold exactly the right mixture.
 */

const { spawn } = require("node:child_process");
const EUICC = process.argv[2] || require("node:path").join(__dirname, "..", "..", "..", "euicc");
const srv = spawn("node", ["out/server.js", "--stdio"], { stdio: ["pipe", "pipe", "inherit"] });

const send = (m) => {
  const b = Buffer.from(JSON.stringify(m), "utf8");
  srv.stdin.write(`Content-Length: ${b.length}\r\n\r\n`);
  srv.stdin.write(b);
};

const GOOD =
  "value1 ProfileElement ::= end : { end-header { mandated NULL, identification 99 } }\n"
  + "value2 ProfileElement ::= header : {\n  major-version 2, minor-version 3,\n"
  + "  iccid '89000123456789012341'H,\n  eUICC-Mandatory-services { usim NULL },\n"
  + "  eUICC-Mandatory-GFSTEList { { 2 23 143 1 2 1 } }\n}\n";
/* The same package with a third value that does not parse. */
const BROKEN = GOOD + "value3 ProfileElement ::= header {\n";

let stage = 0;
let buf = Buffer.alloc(0);
const codes = (d) => d.map((x) => String(x.code)).sort().join(",");

srv.stdout.on("data", (data) => {
  buf = Buffer.concat([buf, data]);
  for (;;) {
    const i = buf.indexOf("\r\n\r\n");
    if (i < 0) return;
    const len = +/Content-Length: (\d+)/.exec(buf.slice(0, i).toString())[1];
    if (buf.length < i + 4 + len) return;
    const msg = JSON.parse(buf.slice(i + 4, i + 4 + len).toString());
    buf = buf.slice(i + 4 + len);
    if (msg.method === "workspace/configuration")
      send({ jsonrpc: "2.0", id: msg.id,
             result: [{ path: EUICC, rules: "", checkOn: "type", checkDelay: 20, docs: "" }] });
    if (msg.method !== "textDocument/publishDiagnostics") continue;
    const d = msg.params.diagnostics;
    if (!d.length) continue;

    if (stage === 0) {
      /* The open ran everything: both ordering rules, nothing else. */
      if (codes(d) !== "SAIP-END-02,SAIP-HDR-02") {
        console.log(`FAIL after open, expected the two ordering rules, got ${codes(d)}`);
        process.exit(1);
      }
      console.log("  open:   " + codes(d));
      stage = 1;
      send({ jsonrpc: "2.0", method: "textDocument/didChange", params: {
        textDocument: { uri: "file:///tmp/l.vn", version: 2 },
        contentChanges: [{ text: BROKEN }] } });
    } else if (stage === 1) {
      /* The pause in typing ran the reader only: its parse finding arrives,
         and the rule findings from the open stand. */
      if (codes(d) !== "SAIP-END-02,SAIP-HDR-02,parse") {
        console.log(`FAIL while typing, expected parse plus the standing rules, got ${codes(d)}`);
        process.exit(1);
      }
      console.log("  typing: " + codes(d));
      stage = 2;
      send({ jsonrpc: "2.0", method: "textDocument/didSave",
             params: { textDocument: { uri: "file:///tmp/l.vn" } } });
    } else if (stage === 2) {
      /* The save ran everything again. The package no longer parses, so no
         rule ran and no rule finding may survive. */
      if (codes(d) !== "parse") {
        console.log(`FAIL after save, expected only the parse finding, got ${codes(d)}`);
        process.exit(1);
      }
      console.log("  save:   " + codes(d));
      console.log("\nreader findings at typing speed, rule findings at save speed");
      process.exit(0);
    }
  }
});

send({ jsonrpc: "2.0", id: 1, method: "initialize",
       params: { processId: process.pid, rootUri: null, capabilities: {} } });
send({ jsonrpc: "2.0", method: "initialized", params: {} });
send({ jsonrpc: "2.0", method: "textDocument/didOpen", params: { textDocument: {
  uri: "file:///tmp/l.vn", languageId: "asn1-vn", version: 1, text: GOOD } } });
setTimeout(() => { console.log(`FAIL stuck at stage ${stage}`); process.exit(1); }, 20000);
