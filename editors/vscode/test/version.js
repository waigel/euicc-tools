/*
 * The server warns when euicc is older than it needs -- once, as a window
 * message, naming the path and the fix.
 *
 * The failure this guards is the one this project kept hitting itself:
 * install the extension, forget `make install`, and the stale binary fails in
 * ways that point everywhere but at the cause. An old euicc is played by a
 * stub that answers `version` the way binaries did before the command
 * existed: usage on stderr, exit 2.
 */

const { spawn } = require("node:child_process");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const EUICC = process.argv[2] || path.join(__dirname, "..", "..", "..", "euicc");

function run(binary, expectWarning) {
  return new Promise((resolve) => {
    const srv = spawn("node", ["out/server.js", "--stdio"], { stdio: ["pipe", "pipe", "inherit"] });
    const send = (m) => {
      const b = Buffer.from(JSON.stringify(m), "utf8");
      srv.stdin.write(`Content-Length: ${b.length}\r\n\r\n`);
      srv.stdin.write(b);
    };
    let warned = null;
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
                 result: [{ path: binary, rules: "", checkOn: "save", docs: "" }] });
        if (msg.method === "window/showMessageRequest" || msg.method === "window/showMessage") {
          warned = msg.params.message;
        }
      }
    });
    send({ jsonrpc: "2.0", id: 1, method: "initialize",
           params: { processId: process.pid, rootUri: null, capabilities: {} } });
    send({ jsonrpc: "2.0", method: "initialized", params: {} });
    setTimeout(() => {
      srv.kill();
      resolve({ warned });
    }, 2500);
  });
}

(async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "old-euicc-"));
  const stub = path.join(dir, "euicc");
  fs.writeFileSync(stub, "#!/bin/sh\necho 'usage: euicc <command>' >&2\nexit 2\n");
  fs.chmodSync(stub, 0o755);

  let r = await run(stub, true);
  if (!r.warned || !/older than this extension needs/.test(r.warned)) {
    console.log(`FAIL an old euicc produced no warning (${r.warned})`);
    process.exit(1);
  }
  if (!r.warned.includes(stub) || !/make install/.test(r.warned)) {
    console.log(`FAIL the warning names neither the path nor the fix: ${r.warned}`);
    process.exit(1);
  }
  console.log("  an old euicc is named, with the fix, in a window message");

  r = await run(EUICC, false);
  if (r.warned && /older than/.test(r.warned)) {
    console.log(`FAIL the current euicc was called old: ${r.warned}`);
    process.exit(1);
  }
  console.log("  the current euicc passes without a word");

  console.log("\nthe stale-binary failure now names itself");
  process.exit(0);
})();
setTimeout(() => { console.log("FAIL stuck"); process.exit(1); }, 20000);
