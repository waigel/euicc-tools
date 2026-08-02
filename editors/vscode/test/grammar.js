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
  `    pinStatus enabled,`,
  `    lcsi [10] OCTET STRING OPTIONAL`,
  `}`,
];

/*
 * Text to find, and the scope that its first character must carry. null means
 * the run is deliberately unstyled. A {not: scope} entry says only that one
 * scope is wrong, for a case where what matters is what the text must not be
 * taken for.
 */
const KEY = "meta.object-literal.key";
const ALT = "variable.other.enummember";

const EXPECT = [
  ["ProfileElement", "entity.name.class"],
  ["::=", "keyword.operator.assignment"],
  /* `valueN Type ::=` is module syntax around the value and not part of it;
     euicc steps over it when reading. Colouring the reference put a second
     blue next to the alternative on the same line, near enough to it to be
     indistinguishable. */
  ["value1", null],
  /* A CHOICE alternative selects one of a fixed set, which is what an enum
     member is, and it is not the same thing as a member name. The colon is
     what separates the two, here and in the reader. */
  ["header", ALT],
  /* Every member name. These are object literal keys and a theme colours
     them; leaving them plain left a profile almost entirely white, because
     almost every word in one is a member name. */
  ["major-version", KEY],
  ["profileType", KEY],
  ["usim", KEY],
  ["eUICC-Mandatory-services", KEY],
  ["eUICC-Mandatory-GFSTEList", KEY],
  ["pinStatus", KEY],
  /* An identifier that is itself the value stays plain, as one does in
     TypeScript. The comma after it is what separates the two cases. */
  ["enabled,", null],
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
  ["Mandatory-services", { not: "entity.name.class" }],
  ["GFSTEList", { not: "entity.name.class" }],
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
    const pass =
      expect === null
        ? got === ""
        : typeof expect === "object"
          ? !got.includes(expect.not)
          : got.includes(expect);
    console.log(`${pass ? "ok  " : "FAIL"} ${text.padEnd(26)} ${got || "(unstyled)"}`);
    pass ? ok++ : failed++;
  }

  /*
   * A scope is not a colour. Two scopes that a theme resolves to the same
   * value look the same on screen, whatever the rules say, and that is the
   * failure a reader actually sees: with the assignment reference coloured,
   * `value1` and the alternative `header` sat next to each other in two blues
   * nobody could tell apart.
   *
   * Resolved against the editor's own default theme, so this needs VS Code
   * installed and says so rather than passing quietly when it is not.
   */
  const c = themeColours();
  if (!c) {
    console.log("\nskip  no VS Code here, so the colours were not resolved");
  } else {
    /*
     * Third column is the family. Two things of one family may share a
     * colour: a type reference and a builtin are both types, and VS Code's
     * own theme paints `string` and a class name alike for that reason. Two
     * families sharing one is the failure.
     */
    const kinds = [
      ["a type", "ProfileElement", "type"],
      ["a builtin", "NULL", "type"],
      /* The reference is module syntax and stays plain. Listed so that
         colouring it again fails here too: variable.other.constant and
         variable.other.enummember are the same #4FC1FF, and the two words
         sit next to each other on the assignment line. */
      ["the reference", "value1", "syntax"],
      ["an alternative", "header", "alternative"],
      ["a member", "profileType", "member"],
      ["a number", "2", "literal"],
      ["a string", `"GSMA Test Profile"`, "literal"],
      ["a hex string", `'89000123456789012341'H`, "literal"],
      ["a comment", "-- a comment", "comment"],
    ];
    const seen = new Map();
    for (const [what, text, family] of kinds) {
      const col = c(scopeAt(lines, text).split(","));
      if (!seen.has(col)) seen.set(col, []);
      seen.get(col).push({ what, text, family });
    }
    for (const [col, group] of seen) {
      const families = new Set(group.map((g) => g.family));
      const clash = families.size > 1;
      const names = group.map((g) => `${g.what} (${g.text})`).join(" and ");
      console.log(`${clash ? "FAIL" : "ok  "} ${col} ${names}`);
      clash ? failed++ : ok++;
    }
  }

  console.log(`\n${ok} ok, ${failed} failed`);
  process.exit(failed ? 1 : 0);
})();

/*
 * The default dark theme, as a function from a token's scopes to a colour.
 * dark_modern includes dark_plus includes dark_vs, so the files are read
 * innermost first and a later rule of the same length wins -- getting that
 * tie-break backwards reported enum members as the colour of a number.
 */
function themeColours() {
  const dir =
    "/Applications/Visual Studio Code.app/Contents/Resources/app/" +
    "extensions/theme-defaults/themes/";
  if (!fs.existsSync(dir + "dark_modern.json")) return null;

  const rules = [];
  (function load(name) {
    const d = JSON.parse(fs.readFileSync(dir + name, "utf8"));
    if (d.include) load(d.include.replace("./", ""));
    for (const r of d.tokenColors ?? []) {
      const scopes =
        typeof r.scope === "string" ? r.scope.split(/[,\s]+/) : r.scope ?? [];
      for (const s of scopes) if (s && r.settings.foreground)
        rules.push({ scope: s, fg: r.settings.foreground.toUpperCase() });
    }
  })("dark_modern.json");

  return (scopes) => {
    for (const sc of [...scopes].reverse()) {
      let best = null;
      let len = -1;
      for (const r of rules)
        if (sc === r.scope || sc.startsWith(r.scope + "."))
          if (r.scope.length >= len) { best = r.fg; len = r.scope.length; }
      if (best) return best;
    }
    return "#D4D4D4";
  };
}
