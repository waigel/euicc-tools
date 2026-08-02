/*
 * The language server, driven over LSP.
 *
 * It speaks the protocol on stdin and stdout, so a test needs no editor. The
 * document below puts PE-End before the header, which two ordering rules must
 * catch.
 *
 * This found a real fault once: the server checked before the configuration
 * arrived, so the first check of a session used the default path and reported
 * that euicc was missing on a machine where it was not.
 */

const { spawn } = require("node:child_process");
const EUICC = process.argv[2] || require("node:path").join(__dirname, "..", "..", "..", "euicc");
const srv = spawn("node", ["out/server.js", "--stdio"], { stdio: ["pipe", "pipe", "inherit"] });

const send = (msg) => {
  const b = Buffer.from(JSON.stringify(msg), "utf8");
  srv.stdin.write(`Content-Length: ${b.length}\r\n\r\n`);
  srv.stdin.write(b);
};

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
      send({ jsonrpc: "2.0", id: msg.id, result: [{ path: EUICC, rules: "", checkOn: "save" }] });
    if (msg.method === "textDocument/publishDiagnostics") {
      const d = msg.params.diagnostics;
      const codes = d.map((x) => x.code);
      for (const x of d)
        console.log(`  line ${x.range.start.line + 1}  ${x.code}  ${x.message}`);
      const want = ["SAIP-HDR-02", "SAIP-END-02"];
      const missing = want.filter((c) => !codes.includes(c));
      if (missing.length) {
        console.log(`\nFAIL: expected ${want.join(" and ")}, missing ${missing.join(" ")}`);
        process.exit(1);
      }
      console.log(`\n${d.length} diagnostics, both ordering rules reported`);
      process.exit(0);
    }
  }
});

send({ jsonrpc: "2.0", id: 1, method: "initialize",
       params: { processId: process.pid, rootUri: null, capabilities: {} } });
send({ jsonrpc: "2.0", method: "initialized", params: {} });
send({ jsonrpc: "2.0", method: "textDocument/didOpen", params: { textDocument: {
  uri: "file:///tmp/x.vn", languageId: "asn1-vn", version: 1,
  text: "value1 ProfileElement ::= end : { end-header { mandated NULL, identification 99 } }\n"
      + "value2 ProfileElement ::= header : {\n  major-version 2, minor-version 3,\n"
      + "  iccid '89000123456789012341'H,\n  eUICC-Mandatory-services { usim NULL },\n"
      + "  eUICC-Mandatory-GFSTEList { { 2 23 143 1 2 1 } }\n}\n" } } });
setTimeout(() => { console.log("FAIL: no diagnostics arrived"); process.exit(1); }, 30000);
