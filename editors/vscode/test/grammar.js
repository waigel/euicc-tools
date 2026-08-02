/*
 * What the grammar colours, checked against real value notation.
 *
 * The check is by position and not by token text. A TextMate grammar splits a
 * string into three tokens, the quote, the body and the quote, and a run that
 * no rule matches stays one long token. An earlier version of this file looked
 * tokens up by their exact text and reported two failures that were its own.
 *
 * Highlighting says whether text looks like ASN.1. Whether it is a correct
 * profile is what `euicc check` says, and nothing here repeats that.
 */

const fs = require("node:fs");
const path = require("node:path");
const vsctm = require("vscode-textmate");
const oniguruma = require("vscode-oniguruma");

const GRAMMAR = path.join(__dirname, "..", "syntaxes", "asn1-vn.tmLanguage.json");

const SAMPLE = [
  `value1 ProfileElement ::= header : {`,
  `    major-version 2,                 -- a comment`,
  `    profileType "GSMA Test Profile",`,
  `    iccid '89000123456789012341'H,`,
  `    eUICC-Mandatory-services { usim NULL },`,
  `    eUICC-Mandatory-GFSTEList { { 2 23 143 1 2 1 } },`,
  `    flags '1101'B,`,
  `    lcsi [10] OCTET STRING OPTIONAL`,
  `}`,
];

/*
 * Text to find, and the scope that its first character must carry. null means
 * the run is deliberately unstyled: an identifier stays plain here, the way a
 * property name does in TypeScript.
 */
const EXPECT = [
  ["ProfileElement", "entity.name.class"],
  ["::=", "keyword.operator.assignment"],
  ["header", null],
  ["value1", null],
  ["profileType", null],
  ["-- a comment", "comment.line"],
  [`"GSMA Test Profile"`, "string.quoted.double"],
  [`'89000123456789012341'H`, "string.quoted.other.hex"],
  [`'1101'B`, "string.quoted.other.binary"],
  ["NULL", "support.type"],
  ["OCTET STRING", "support.type"],
  ["[10]", "entity.other.attribute-name.tag"],
  ["OPTIONAL", "storage.type"],
  /* A hyphen is a word boundary, so the capitalised part of a lower-case
     identifier used to match the type-reference rule. */
  ["eUICC-Mandatory-services", null],
  ["Mandatory-services", null],
  ["eUICC-Mandatory-GFSTEList", null],
];

function scopeAt(lines, text) {
  for (const { line, tokens } of lines) {
    const at = line.indexOf(text);
    if (at < 0) continue;
    for (const t of tokens) {
      if (at >= t.startIndex && at < t.endIndex) {
        return t.scopes.filter((s) => s !== "source.asn1-vn").join(",");
      }
    }
  }
  return undefined;
}

(async () => {
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
  const lines = [];
  let state = vsctm.INITIAL;
  for (const line of SAMPLE) {
    const r = grammar.tokenizeLine(line, state);
    lines.push({ line, tokens: r.tokens });
    state = r.ruleStack;
  }

  let ok = 0;
  let failed = 0;
  for (const [text, expect] of EXPECT) {
    const got = scopeAt(lines, text);
    if (got === undefined) {
      console.log(`FAIL ${text.padEnd(26)} not in the sample`);
      failed++;
      continue;
    }
    const pass = expect === null ? got === "" : got.includes(expect);
    console.log(`${pass ? "ok  " : "FAIL"} ${text.padEnd(26)} ${got || "(unstyled)"}`);
    pass ? ok++ : failed++;
  }
  console.log(`\n${ok} ok, ${failed} failed`);
  process.exit(failed ? 1 : 0);
})();
