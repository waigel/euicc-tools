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
  /* An hstring is a value and not text, and colouring it as text put it and
     the member names, 69 per cent of a real profile between them, in two
     blues 19 apart in the Dark 2026 theme. */
  [`'89000123456789012341'H`, "constant.numeric.hex"],
  [`'1101'B`, "constant.numeric.binary"],
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
   * A scope is not a colour, and a colour is not a distinguishable colour.
   * Two scopes a theme resolves near each other look the same on screen
   * whatever the rules say, and that is the failure a reader sees. It has
   * happened twice here: the assignment reference next to an alternative in
   * two blues, and member names next to hex strings in two more.
   *
   * Checked against every default dark theme, because they disagree. Dark
   * Modern paints a string #CE9178 and Dark 2026 paints it #A5D6FF, which is
   * 19 from the #9CDCFE of a member name. A pair that reads clearly in one is
   * not evidence about the other.
   */
  for (const theme of ["dark_modern.json", "2026-dark.json"]) {
    const c = themeColours(theme);
    if (!c) {
      console.log(`\nskip  no VS Code here, so ${theme} was not resolved`);
      continue;
    }
    console.log(`\n${theme}`);
    /*
     * Third column is the family. Two things of one family may share a
     * colour: a type reference and a builtin are both types, and VS Code
     * paints `string` and a class name alike for that reason. An hstring is
     * a value, so it shares with a number by design.
     */
    const kinds = [
      ["a type", "ProfileElement", "type"],
      ["a builtin", "NULL", "type"],
      /* Module syntax, and it stays plain. Listed so that colouring it again
         fails here: variable.other.constant and variable.other.enummember
         are one colour, and the two words share the assignment line. */
      ["the reference", "value1", "syntax"],
      ["an alternative", "header", "alternative"],
      ["a member", "profileType", "member"],
      ["a number", "2", "literal"],
      ["a hex string", `'89000123456789012341'H`, "literal"],
      ["a binary string", `'1101'B`, "literal"],
      ["a string", `"GSMA Test Profile"`, "literal"],
      ["a comment", "-- a comment", "comment"],
    ];
    const at = new Map();
    for (const [what, text, family] of kinds)
      at.set(what, { col: c(scopeAt(lines, text).split(",")), family, text });

    for (const [aName, a] of at) {
      for (const [bName, b] of at) {
        if (aName >= bName || a.family === b.family) continue;
        const d = apart(a.col, b.col);
        /*
         * A real cstring is 3 runs in 4731 of a published profile, and
         * TypeScript's own object keys sit the same 19 from a string in this
         * theme. Recorded rather than hidden: it is accepted, not unseen.
         */
        const known = aName === "a member" && bName === "a string";
        const bad = d < 40 && !known;
        if (bad || d < 40)
          console.log(`${bad ? "FAIL" : "ok  "} ${String(d).padStart(3)} apart` +
            ` ${aName} ${a.col} and ${bName} ${b.col}` +
            (known ? "  (accepted, and TypeScript is the same here)" : ""));
        bad ? failed++ : ok++;
      }
    }
    const groups = new Map();
    for (const [name, v] of at) {
      if (!groups.has(v.col)) groups.set(v.col, []);
      groups.get(v.col).push(name);
    }
    for (const [col, g] of groups) console.log(`     ${col} ${g.join(", ")}`);
  }

  console.log(`\n${ok} ok, ${failed} failed`);
  process.exit(failed ? 1 : 0);
})();

/*
 * A theme, as a function from a token's scopes to a colour.
 *
 * Two mistakes in writing this, and each reported the wrong answer without
 * looking wrong:
 *
 *   A selector may name ancestors. "string variable" is a variable inside a
 *   string, not the two of them, and splitting it on whitespace produced a
 *   bare `string` rule that overrode the real one. Every string came back as
 *   the colour of the thing nested in one.
 *
 *   A selector matches any scope in the stack, not only the innermost, and
 *   the deeper match wins. Checking only the innermost missed every rule a
 *   theme writes against an outer scope, which is how Dark 2026 styles an
 *   object member.
 */
function themeColours(file) {
  const dir =
    "/Applications/Visual Studio Code.app/Contents/Resources/app/" +
    "extensions/theme-defaults/themes/";
  if (!fs.existsSync(dir + file)) return null;

  const rules = [];
  (function load(name) {
    const d = JSON.parse(fs.readFileSync(dir + name, "utf8"));
    if (d.include) load(d.include.replace("./", ""));
    for (const r of d.tokenColors ?? []) {
      if (!r.settings.foreground) continue;
      const sel = typeof r.scope === "string" ? r.scope.split(",") : r.scope ?? [];
      for (const one of sel) {
        const parts = one.trim().split(/\s+/).filter(Boolean);
        if (parts.length)
          rules.push({ parts, fg: r.settings.foreground.toUpperCase() });
      }
    }
  })(file);

  const hits = (selector, scope) =>
    scope === selector || scope.startsWith(selector + ".");

  /* Dark 2026 sets editor.foreground; the older themes leave it to #D4D4D4. */
  const plain = file.startsWith("2026") ? "#C9D1D9" : "#D4D4D4";

  return (scopes) => {
    let best = plain;
    let rank = -1;
    for (const r of rules) {
      const last = r.parts[r.parts.length - 1];
      let depth = -1;
      for (let i = 0; i < scopes.length; i++) if (hits(last, scopes[i])) depth = i;
      if (depth < 0) continue;
      if (!r.parts.slice(0, -1).every((p) => scopes.some((s) => hits(p, s))))
        continue;
      const score = depth * 1000 + last.length;
      if (score >= rank) { best = r.fg; rank = score; }
    }
    return best;
  };
}

/*
 * How far apart two colours look, weighted for the eye rather than for the
 * bytes: #9CDCFE and #A5D6FF differ in every channel and are still one colour
 * to a reader. Below about 40 they cannot be told apart in running text.
 */
function apart(a, b) {
  const ch = (h) => [1, 3, 5].map((i) => parseInt(h.substr(i, 2), 16));
  const [r1, g1, b1] = ch(a);
  const [r2, g2, b2] = ch(b);
  const rm = (r1 + r2) / 2;
  return Math.round(
    Math.sqrt(
      (2 + rm / 256) * (r1 - r2) ** 2 +
        4 * (g1 - g2) ** 2 +
        (2 + (255 - rm) / 256) * (b1 - b2) ** 2
    )
  );
}
