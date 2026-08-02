/*
 * What the server offers, at a cursor written as | in the sample.
 *
 * The schema comes from `euicc schema`, so this runs against the real one and
 * not against a fixture that can drift from it. The suggester is TypeScript;
 * esbuild turns it into something requirable here rather than adding a third
 * file to the package for the sake of a test.
 *
 * The scanner is the part that can be quietly wrong. A brace inside a comment
 * or a string must not open a level, and a value must not be taken for a
 * member that is already written -- both are checked below, because neither
 * shows up as an error, only as a list with the wrong things in it.
 */

const { execFileSync } = require("node:child_process");
const { buildSync } = require("esbuild");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const EUICC = process.argv[2] || path.join(__dirname, "..", "..", "..", "euicc");

const out = path.join(fs.mkdtempSync(path.join(os.tmpdir(), "euicc-vn-")), "schema.cjs");
buildSync({
  entryPoints: [path.join(__dirname, "..", "src", "schema.ts")],
  outfile: out,
  bundle: true,
  platform: "node",
  format: "cjs",
});
const { analyze, suggest, memberAt, declarationLine } = require(out);

const schema = JSON.parse(execFileSync(EUICC, ["schema"], { maxBuffer: 8 << 20 }));

let pass = 0;
let fail = 0;

/* The sample carries one | and it is the cursor. */
function at(sample) {
  const offset = sample.indexOf("|");
  if (offset < 0) throw new Error("the sample has no cursor");
  const text = sample.slice(0, offset) + sample.slice(offset + 1);
  const ctx = analyze(schema, text, offset);
  return { ctx, items: suggest(schema, ctx) };
}

function check(what, sample, verdict) {
  const { ctx, items } = at(sample);
  const labels = items.map((i) => i.label);
  const why = verdict(labels, items, ctx);
  if (why === true) {
    console.log(`ok   ${what}`);
    pass++;
  } else {
    console.log(`FAIL ${what}\n       ${why}`);
    console.log(`       type=${ctx.type} valueFor=${ctx.valueFor} used=[${ctx.used}]`);
    console.log(`       got: ${labels.slice(0, 12).join(", ")}${labels.length > 12 ? " …" : ""}`);
    fail++;
  }
}

const has = (labels, ...want) => {
  const missing = want.filter((w) => !labels.includes(w));
  return missing.length ? `missing ${missing.join(", ")}` : true;
};
const lacks = (labels, ...unwanted) => {
  const there = unwanted.filter((w) => labels.includes(w));
  return there.length ? `should not offer ${there.join(", ")}` : true;
};

/* ---- where a member name goes -------------------------------------------- */

check("the alternatives of ProfileElement at the top", "value1 ProfileElement ::= |", (l) =>
  has(l, "header", "mf", "usim", "end")
);

check("an alternative is written with its colon", "value1 ProfileElement ::= |", (l, i) => {
  const h = i.find((x) => x.label === "header");
  return h.insert === "header : " ? true : `insert was ${JSON.stringify(h.insert)}`;
});

check("the members of ProfileHeader inside its braces",
  "value1 ProfileElement ::= header : {\n    |", (l) =>
  has(l, "major-version", "minor-version", "iccid", "profileType")
);

check("a SEQUENCE member is written without a colon",
  "value1 ProfileElement ::= header : {\n    |", (l, i) => {
  const m = i.find((x) => x.label === "iccid");
  return m.insert === "iccid " ? true : `insert was ${JSON.stringify(m.insert)}`;
});

check("mandatory members sort above optional ones",
  "value1 ProfileElement ::= header : {\n    |", (l, i) => {
  const sorted = [...i].sort((a, b) => a.sort.localeCompare(b.sort)).map((x) => x.label);
  const iccid = sorted.indexOf("iccid");
  const pol = sorted.indexOf("pol");
  return iccid < pol ? true : `iccid at ${iccid}, the optional pol at ${pol}`;
});

check("a member already written is not offered again",
  "value1 ProfileElement ::= header : {\n    major-version 2,\n    |", (l) =>
  lacks(l, "major-version") === true ? has(l, "minor-version") : lacks(l, "major-version")
);

check("the word being typed is not counted as written",
  "value1 ProfileElement ::= header : {\n    major-ver|", (l) => has(l, "major-version")
);

/* ---- where a value goes --------------------------------------------------- */

check("the identifiers a named number accepts",
  "value1 ProfileElement ::= akaParameter : {\n" +
  "    algoConfiguration algoParameter : {\n        algorithmID |", (l) =>
  has(l, "milenage", "tuak", "usim-test-algorithm")
);

check("no member names where a value goes",
  "value1 ProfileElement ::= akaParameter : {\n" +
  "    algoConfiguration algoParameter : {\n        algorithmID |", (l) =>
  lacks(l, "algorithmOptions", "key", "opc")
);

check("nothing is invented for a value with no identifiers",
  "value1 ProfileElement ::= header : {\n    iccid |", (l) =>
  l.length === 0 ? true : `offered ${l.join(", ")}`
);

/* ---- lists ---------------------------------------------------------------- */

check("an element of File offers the alternatives of its CHOICE",
  "value1 ProfileElement ::= mf : {\n    mf {\n        |", (l) =>
  has(l, "doNotCreate", "fileDescriptor", "fillFileOffset", "fillFileContent")
);

check("an element of a list is wrapped in its own braces",
  "value1 ProfileElement ::= mf : {\n    mf {\n        |", (l, i) => {
  const f = i.find((x) => x.label === "fileDescriptor");
  return f.snippet && f.insert === "{ fileDescriptor : $0 }"
    ? true : `insert was ${JSON.stringify(f.insert)}`;
});

check("inside an element the CHOICE alternative is offered plainly",
  "value1 ProfileElement ::= mf : {\n    mf {\n        { |", (l, i) => {
  const f = i.find((x) => x.label === "fileDescriptor");
  return f && f.insert === "fileDescriptor : " ? true
    : `insert was ${JSON.stringify(f && f.insert)}`;
});

/* ---- the scanner ---------------------------------------------------------- */

check("a brace in a line comment opens nothing",
  "value1 ProfileElement ::= header : {\n    -- a { that is not real\n    |", (l) =>
  has(l, "iccid")
);

check("a brace in a block comment opens nothing",
  "value1 ProfileElement ::= header : {\n    /* a { here */\n    |", (l) =>
  has(l, "iccid")
);

check("a brace in a string opens nothing",
  'value1 ProfileElement ::= header : {\n    profileType "a { b",\n    |', (l) =>
  has(l, "iccid") === true ? lacks(l, "profileType") : has(l, "iccid")
);

check("a hex string is skipped whole",
  "value1 ProfileElement ::= header : {\n" +
  "    iccid '89000123456789012341'H,\n    |", (l) =>
  has(l, "major-version") === true ? lacks(l, "iccid") : has(l, "major-version")
);

check("a closing brace goes back up a level",
  "value1 ProfileElement ::= header : {\n" +
  "    eUICC-Mandatory-services { usim NULL },\n    |", (l) =>
  has(l, "iccid", "major-version") === true
    ? lacks(l, "usim", "javacard")
    : has(l, "iccid", "major-version")
);

check("a second value starts over at the root",
  "value1 ProfileElement ::= header : {\n    major-version 2\n}\n" +
  "value2 ProfileElement ::= |", (l) =>
  has(l, "header", "mf", "end") === true ? lacks(l, "major-version") : has(l, "header")
);

check("a value identifier is not taken for a member",
  "value1 ProfileElement ::= header : {\n" +
  "    eUICC-Mandatory-services { usim NULL, |", (l) =>
  has(l, "javacard") === true ? lacks(l, "NULL") : has(l, "javacard")
);

/* ---- the path ------------------------------------------------------------- */

check("the path names each brace it went through",
  "value1 ProfileElement ::= akaParameter : {\n" +
  "    algoConfiguration algoParameter : {\n        |", (l, i, ctx) =>
  ctx.path.join("/") === "akaParameter/algoConfiguration/algoParameter"
    ? true : `path was ${ctx.path.join("/")}`
);

/* ---- hover ---------------------------------------------------------------- */

function hover(sample, verdict) {
  const offset = sample.indexOf("|");
  const text = sample.slice(0, offset) + sample.slice(offset + 1);
  return memberAt(schema, text, offset);
}

function checkHover(what, sample, verdict) {
  const f = hover(sample);
  const why = verdict(f);
  if (why === true) { console.log(`ok   ${what}`); pass++; }
  else { console.log(`FAIL ${what}\n       ${why}\n       got ${JSON.stringify(f)}`); fail++; }
}

checkHover("a member under the cursor gives its type",
  "value1 ProfileElement ::= header : {\n    icc|id '89'H\n}", (f) =>
  f && f.member.name === "iccid" && f.member.type === "OCTET STRING"
    && f.owner === "ProfileHeader" ? true : "expected ProfileHeader.iccid: OCTET STRING");

checkHover("optional is carried to the hover",
  "value1 ProfileElement ::= header : {\n    profile|Type \"x\"\n}", (f) =>
  f && f.member.optional === true ? true : "profileType is OPTIONAL");

checkHover("a CHOICE alternative resolves too",
  "value1 ProfileElement ::= hea|der : {", (f) =>
  f && f.member.name === "header" && f.owner === "ProfileElement"
    ? true : "expected the alternative header of ProfileElement");

checkHover("a named number reaches the hover",
  "value1 ProfileElement ::= akaParameter : {\n" +
  "    algoConfiguration algoParameter : {\n        algorith|mID 1\n", (f) =>
  f && f.member.names && f.member.names.includes("milenage")
    ? true : "expected the identifiers of algorithmID");

checkHover("a word that names nothing gives nothing",
  "value1 ProfileElement ::= header : {\n    nonsen|se 1\n}", (f) =>
  f === null ? true : "should not invent a member");

/* ---- the declaration a finding points at ---------------------------------- */

const asn = require("node:fs").readFileSync(schema.source, "utf8");
const decl = (t, m) => declarationLine(asn, t, m);
const line = decl("PE-MF", "ef-iccid");
if (line !== null && /^\s*ef-iccid\s/.test(asn.split("\n")[line])) {
  console.log(`ok   the declaration of PE-MF.ef-iccid is found (line ${line + 1})`);
  pass++;
} else { console.log("FAIL the declaration of PE-MF.ef-iccid is found"); fail++; }

if (decl("PE-MF", "ef-imsi") === null) {
  console.log("ok   a member of another type is not claimed"); pass++;
} else { console.log("FAIL a member of another type is not claimed"); fail++; }

if (decl("NoSuchType", "x") === null) {
  console.log("ok   an unknown type finds nothing"); pass++;
} else { console.log("FAIL an unknown type finds nothing"); fail++; }

console.log(`\n${pass} ok, ${fail} failed`);
process.exit(fail ? 1 : 0);
