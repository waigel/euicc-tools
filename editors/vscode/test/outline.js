/*
 * The outline, over the protocol: the named braces of the document, nested as
 * written, with the selection on the name. Leaf members stay out -- 1861 of
 * them against 30 elements in a published profile would bury the thing a
 * reader navigates by.
 */

const { spawn } = require("node:child_process");
const EUICC = process.argv[2] || require("node:path").join(__dirname, "..", "..", "..", "euicc");
const srv = spawn("node", ["out/server.js", "--stdio"], { stdio: ["pipe", "pipe", "inherit"] });

const send = (m) => {
  const b = Buffer.from(JSON.stringify(m), "utf8");
  srv.stdin.write(`Content-Length: ${b.length}\r\n\r\n`);
  srv.stdin.write(b);
};

const LINES = [
  "value1 ProfileElement ::= header : {",
  "    major-version 2,",
  "    eUICC-Mandatory-services { usim NULL }",
  "}",
  "value2 ProfileElement ::= akaParameter : {",
  "    aka-header { mandated NULL, identification 1 },",
  "    algoConfiguration algoParameter : {",
  "        algorithmID milenage",
  "    }",
  "}",
];
const DOC = LINES.join("\n") + "\n";

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
    if (msg.id === 1 && msg.result) {
      if (!msg.result.capabilities.documentSymbolProvider) {
        console.log("FAIL the server does not declare documentSymbolProvider");
        process.exit(1);
      }
      send({ jsonrpc: "2.0", id: 2, method: "textDocument/documentSymbol",
             params: { textDocument: { uri: "file:///tmp/o.vn" } } });
    }
    if (msg.id === 2) {
      const syms = msg.result || [];
      const brief = (n) => n.name + (n.children.length ? "(" + n.children.map(brief).join(",") + ")" : "");
      const got = syms.map(brief).join(" ");
      console.log("  " + got);
      const want = "header(eUICC-Mandatory-services)"
        + " akaParameter(aka-header,algoConfiguration : algoParameter)";
      if (got !== want) {
        console.log(`FAIL expected ${want}`);
        process.exit(1);
      }
      /* The selection is the name, and the range runs to the closing brace. */
      const header = syms[0];
      if (header.selectionRange.start.line !== 0
          || header.selectionRange.start.character !== LINES[0].indexOf("header")
          || header.range.end.line !== 3) {
        console.log(`FAIL ranges: ${JSON.stringify(header.selectionRange)} ${JSON.stringify(header.range)}`);
        process.exit(1);
      }
      if (header.detail !== "ProfileHeader" || header.children[0].detail !== "ServicesList") {
        console.log(`FAIL details: ${header.detail}, ${header.children[0].detail}`);
        process.exit(1);
      }
      console.log("\nthe outline is the named braces, nested as written");
      process.exit(0);
    }
  }
});

send({ jsonrpc: "2.0", id: 1, method: "initialize",
       params: { processId: process.pid, rootUri: null, capabilities: {} } });
send({ jsonrpc: "2.0", method: "initialized", params: {} });
send({ jsonrpc: "2.0", method: "textDocument/didOpen", params: { textDocument: {
  uri: "file:///tmp/o.vn", languageId: "asn1-vn", version: 1, text: DOC } } });
setTimeout(() => { console.log("FAIL no symbols arrived"); process.exit(1); }, 15000);
