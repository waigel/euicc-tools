/*
 * The grammar against every published profile, checked by invariant.
 *
 * The unit tests in grammar.js tokenize lines somebody thought of. This runs
 * the writer's output of every published package through the grammar and
 * asserts two things that must hold everywhere, whoever thought of them:
 *
 *   1. A token coloured as a type reference names a type the schema knows.
 *      The closing H of a wrapped hstring was teal in every published profile
 *      for two months, and no hand-picked line contained one.
 *
 *   2. No run of plain text is longer than 23 characters. Canonical output is
 *      almost entirely classified -- what stays plain is punctuation, value
 *      references and ENUMERATED identifiers -- so a long colourless run
 *      means a rule failed to reach something. The bodies of those same
 *      wrapped hstrings were 60-character plain runs.
 *
 * hashicorp/syntax tests its Terraform grammar the same way, with a corpus
 * and snapshots, and that is where the idea is taken from.
 */

const { execFileSync } = require("node:child_process");
const fs = require("node:fs");
const path = require("node:path");
const vsctm = require("vscode-textmate");
const oniguruma = require("vscode-oniguruma");

const EUICC = process.argv[2] || path.join(__dirname, "..", "..", "..", "euicc");
const GRAMMAR = path.join(__dirname, "..", "syntaxes", "asn1-vn.tmLanguage.json");
const TESTDATA = path.join(__dirname, "..", "..", "..", "vendor",
  "euicc-profile-tool", "testdata");

(async () => {
  if (!fs.existsSync(TESTDATA) || !fs.existsSync(EUICC)) {
    console.log("skip  no published profiles here, so the corpus was not run");
    process.exit(0);
  }
  const schema = JSON.parse(execFileSync(EUICC, ["schema"], { maxBuffer: 8 << 20 }));

  const wasm = fs.readFileSync(require.resolve("vscode-oniguruma/release/onig.wasm"));
  const onigLib = oniguruma.loadWASM(wasm.buffer).then(() => ({
    createOnigScanner: (s) => new oniguruma.OnigScanner(s),
    createOnigString: (s) => new oniguruma.OnigString(s),
  }));
  const registry = new vsctm.Registry({
    onigLib,
    loadGrammar: async () =>
      vsctm.parseRawGrammar(fs.readFileSync(GRAMMAR, "utf8"), "asn1-vn.tmLanguage.json"),
  });
  const grammar = await registry.loadGrammar("source.asn1-vn");

  let files = 0;
  let tokens = 0;
  let failed = 0;
  for (const der of fs.readdirSync(TESTDATA).filter((f) => f.endsWith(".der"))) {
    const text = execFileSync(EUICC, ["show", path.join(TESTDATA, der)],
      { maxBuffer: 32 << 20 }).toString();
    const lines = text.split("\n");
    let state = vsctm.INITIAL;
    files++;
    for (let ln = 0; ln < lines.length; ln++) {
      const r = grammar.tokenizeLine(lines[ln], state);
      state = r.ruleStack;
      for (const t of r.tokens) {
        tokens++;
        const txt = lines[ln].slice(t.startIndex, t.endIndex);
        const scopes = t.scopes.filter((s) => s !== "source.asn1-vn");
        const sc = scopes.join(",");
        if (sc.includes("entity.name.class")) {
          const name = txt.trim();
          if (name && !schema.types[name] && name !== schema.root) {
            console.log(`FAIL ${der} line ${ln + 1}: type colour on ${JSON.stringify(name)}`);
            failed++;
          }
        }
        if (sc === "" && txt.trim().length > 23) {
          console.log(`FAIL ${der} line ${ln + 1}: ${txt.trim().length} plain chars: ` +
            JSON.stringify(txt.trim().slice(0, 40)));
          failed++;
        }
      }
    }
  }
  console.log(`${failed ? "FAIL" : "ok  "} ${files} profiles, ${tokens} tokens, ` +
    `every type colour is a type and nothing long stayed plain`);
  process.exit(failed ? 1 : 0);
})();
