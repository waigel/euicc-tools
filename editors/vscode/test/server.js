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
let asked = false;
let edited = false;

/* The same document, with a hex string where the second element wants an
   identifier. Line 9 is the one it is on. */
const BAD =
  "value1 ProfileElement ::= header : {\n  major-version 2, minor-version 3,\n"
+ "  iccid '89000123456789012341'H,\n  eUICC-Mandatory-services { usim NULL },\n"
+ "  eUICC-Mandatory-GFSTEList { { 2 23 143 1 2 1 } }\n}\n"
+ "value2 ProfileElement ::= mf : {\n"
+ "  mf-header { mandated NULL, identification 1 },\n"
+ "  templateID '2F0601'H\n}\n";
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
             result: [{ path: EUICC, rules: "", checkOn: "type", checkDelay: 50 }] });
    /* The server has to say it completes, or the editor never asks. */
    if (msg.id === 1 && msg.result) {
      if (!msg.result.capabilities.completionProvider) {
        console.log("FAIL: the server does not declare completion");
        process.exit(1);
      }
      console.log("  completionProvider is declared");
    }

    if (msg.method === "textDocument/publishDiagnostics" && !asked) {
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
      console.log(`  ${d.length} diagnostics, both ordering rules reported`);

      /*
       * Line 2 column 2 is where major-version begins, inside the header. The
       * suggester has tests of its own; this one asks over the protocol,
       * because a suggester nothing calls is worth nothing.
       */
      asked = true;
      send({ jsonrpc: "2.0", id: 2, method: "textDocument/completion",
             params: { textDocument: { uri: "file:///tmp/x.vn" },
                       position: { line: 2, character: 2 } } });
    }

    if (msg.id === 2) {
      const labels = (msg.result || []).map((x) => x.label);
      const want = ["iccid", "profileType", "eUICC-Mandatory-services"];
      const missing = want.filter((w) => !labels.includes(w));
      if (missing.length) {
        console.log(`\nFAIL: completion missing ${missing.join(", ")}`);
        console.log(`      got ${labels.join(", ") || "nothing"}`);
        process.exit(1);
      }
      console.log(`  completion offered ${labels.length} members of ProfileHeader`);

      /*
       * An edit and no save. Checking as you type is the default, so a value
       * of the wrong type has to arrive without one, and on its own line: the
       * reader counts from the start of the value it was handed, and every
       * failure in the second element used to be reported on line 1.
       */
      edited = true;
      send({ jsonrpc: "2.0", method: "textDocument/didChange", params: {
        textDocument: { uri: "file:///tmp/x.vn", version: 2 },
        contentChanges: [{ text: BAD }] } });
    }

    if (msg.method === "textDocument/publishDiagnostics" && edited) {
      const d = msg.params.diagnostics;
      if (!d.length) return;
      const at = d[0].range.start;
      console.log(`  after an edit and no save: line ${at.line + 1} ` +
                  `column ${at.character + 1}  ${d[0].message}`);
      if (!/wrong|string|expected/i.test(d[0].message)) {
        console.log("FAIL: the wrong type was not reported");
        process.exit(1);
      }
      if (at.line + 1 !== 9) {
        console.log("FAIL: expected it on line 9, the line it is on");
        process.exit(1);
      }
      console.log("\ndiagnostics, completion and checking as you type all answered");
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
