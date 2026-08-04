/*
 * euicc counts a column in bytes; LSP counts UTF-16 code units. The two agree
 * on any line whose earlier characters are all ASCII, which is how the
 * difference survived every other test in this directory: one umlaut in a
 * cstring shifts every position after it, and the squiggle and the quick fix
 * landed beside their token.
 *
 * The line under test carries two, so the byte column is 33 where the UTF-16
 * column is 31. The expected position is computed from the string itself, not
 * copied from the implementation.
 */

const { spawn } = require("node:child_process");
const EUICC = process.argv[2] || require("node:path").join(__dirname, "..", "..", "..", "euicc");
const srv = spawn("node", ["out/server.js", "--stdio"], { stdio: ["pipe", "pipe", "inherit"] });

const send = (m) => {
  const b = Buffer.from(JSON.stringify(m), "utf8");
  srv.stdin.write(`Content-Length: ${b.length}\r\n\r\n`);
  srv.stdin.write(b);
};

const LINE = '    profileType "Größe", iccid 42';
const DOC = "value1 ProfileElement ::= header : {\n" + LINE + "\n}\n";
/* The reader stops at the space after the member name. */
const EXPECT = LINE.indexOf("iccid") + "iccid".length;

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
      const d = msg.params.diagnostics.find((x) => x.range.start.line === 1);
      if (!d) continue;
      console.log(`  ${d.message}`);
      console.log(`  marked at character ${d.range.start.character},` +
                  ` and the space after 'iccid' is at ${EXPECT}`);
      if (d.range.start.character !== EXPECT) {
        console.log("FAIL the mark drifted by the width of the umlauts");
        process.exit(1);
      }
      if (!/not assignable/.test(d.message)) {
        console.log("FAIL the finding was not resolved against the schema," +
                    " so the converted offset missed the member");
        process.exit(1);
      }
      console.log("\nbyte columns from euicc, UTF-16 positions on the wire");
      process.exit(0);
    }
  }
});

send({ jsonrpc: "2.0", id: 1, method: "initialize",
       params: { processId: process.pid, rootUri: null, capabilities: {} } });
send({ jsonrpc: "2.0", method: "initialized", params: {} });
send({ jsonrpc: "2.0", method: "textDocument/didOpen", params: { textDocument: {
  uri: "file:///tmp/u.vn", languageId: "asn1-vn", version: 1, text: DOC } } });
setTimeout(() => { console.log("FAIL no diagnostic arrived"); process.exit(1); }, 15000);
