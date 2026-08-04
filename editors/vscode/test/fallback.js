/*
 * The semantic tokens, for an editor that has never heard of ours.
 *
 * Neovim and Helix highlight a legend by name, and asn1Member is not a name
 * they know: before the server negotiated, they received four unknown types
 * and rendered the semantic layer as nothing at all -- while the README
 * promised exactly those editors. terraform-ls resolves every token through
 * a pair of names for the same reason (token_encoder.go,
 * firstSupportedTokenType): the custom one where the client advertises it,
 * the standard one otherwise.
 *
 * Two clients, one document. The first speaks only the standard types and
 * must get property/enumMember/type -- with member and alternative still
 * distinct. The second advertises no semantic tokens at all and must get an
 * empty legend rather than an error.
 */

const { spawn } = require("node:child_process");
const EUICC = process.argv[2] || require("node:path").join(__dirname, "..", "..", "..", "euicc");

const DOC = [
  "value1 ProfileElement ::= pukCodes : {",
  "    puk-Header { mandated NULL, identification 2 },",
  "    pukCodes {",
  "        { keyReference pukAppl1, pukValue '3131313131313131'H }",
  "    }",
  "}",
].join("\n") + "\n";

function run(caps, verdict) {
  return new Promise((resolve) => {
    const srv = spawn("node", ["out/server.js", "--stdio"], { stdio: ["pipe", "pipe", "inherit"] });
    const send = (m) => {
      const b = Buffer.from(JSON.stringify(m), "utf8");
      srv.stdin.write(`Content-Length: ${b.length}\r\n\r\n`);
      srv.stdin.write(b);
    };
    let legend = [];
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
          legend = msg.result.capabilities.semanticTokensProvider.legend.tokenTypes;
          send({ jsonrpc: "2.0", id: 2, method: "textDocument/semanticTokens/full",
                 params: { textDocument: { uri: "file:///tmp/f.vn" } } });
        }
        if (msg.id === 2) {
          const data = (msg.result || {}).data || [];
          const kinds = [];
          for (let k = 0; k < data.length; k += 5) kinds.push(legend[data[k + 3]]);
          srv.kill();
          resolve(verdict(legend, kinds));
        }
      }
    });
    send({ jsonrpc: "2.0", id: 1, method: "initialize",
           params: { processId: process.pid, rootUri: null, capabilities: caps } });
    send({ jsonrpc: "2.0", method: "initialized", params: {} });
    send({ jsonrpc: "2.0", method: "textDocument/didOpen", params: { textDocument: {
      uri: "file:///tmp/f.vn", languageId: "asn1-vn", version: 1, text: DOC } } });
  });
}

(async () => {
  const standardOnly = { textDocument: { semanticTokens: {
    tokenTypes: ["namespace", "type", "class", "enum", "property", "enumMember",
                 "variable", "function"],
    tokenModifiers: [], formats: ["relative"] } } };
  let why = await run(standardOnly, (legend, kinds) => {
    if (legend.join(",") !== "property,enumMember,type,variable")
      return `legend was ${legend.join(",")}`;
    if (!kinds.includes("property") || !kinds.includes("enumMember"))
      return `kinds were ${[...new Set(kinds)].join(",")}`;
    /* pukCodes appears as both member and alternative; the fallback must not
       collapse the two. */
    return true;
  });
  if (why !== true) { console.log(`FAIL standard-only client: ${why}`); process.exit(1); }
  console.log("  a standard-only client gets property/enumMember/type");

  why = await run({}, (legend, kinds) => {
    if (legend.length !== 0) return `legend was ${legend.join(",")}`;
    if (kinds.length !== 0) return `tokens arrived against an empty legend`;
    return true;
  });
  if (why !== true) { console.log(`FAIL capability-free client: ${why}`); process.exit(1); }
  console.log("  a client without the capability gets an empty legend and no tokens");

  console.log("\nour names where they are known, the standard ones where they are not");
  process.exit(0);
})();
setTimeout(() => { console.log("FAIL stuck"); process.exit(1); }, 20000);
