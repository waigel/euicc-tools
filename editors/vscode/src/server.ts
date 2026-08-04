/*
 * A language server for ASN.1 value notation.
 *
 * It runs `euicc check --json` and turns what comes back into diagnostics, and
 * `euicc schema` once for what may be written where the cursor is. Nothing
 * here parses ASN.1 or evaluates a rule. A second implementation of either
 * would be a second thing that can disagree with the first, and the editor
 * would then report something the build does not.
 *
 * The same arrangement VS Code uses for TypeScript: the grammar colours the
 * text and the server answers everything that needs to know the schema. The
 * TypeScript extension contributes no completion in its package.json either;
 * it registers providers at run time against tsserver.
 *
 * The server speaks LSP, so it is not only for VS Code. Neovim and Helix start
 * it the same way.
 */

import { execFile } from "node:child_process";
import { readFileSync } from "node:fs";
import { pathToFileURL } from "node:url";

import {
  CompletionItem,
  CompletionItemKind,
  createConnection,
  Diagnostic,
  DiagnosticSeverity,
  DiagnosticRelatedInformation,
  DidChangeConfigurationNotification,
  DocumentSymbol,
  InitializeParams,
  InsertTextFormat,
  MarkupKind,
  CodeAction,
  CodeActionKind,
  ProposedFeatures,
  SemanticTokensBuilder,
  SymbolKind,
  TextDocuments,
  TextDocumentSyncKind,
  TextEdit,
} from "vscode-languageserver/node";
import { TextDocument } from "vscode-languageserver-textdocument";

import {
  analyze,
  assignmentLine,
  declarationLine,
  describe,
  DocumentModel,
  memberAt,
  model,
  readableType,
  Schema,
  suggest,
  wordAt,
} from "./schema";

interface Finding {
  severity: "error" | "warning";
  code: string;
  message: string;
  line: number;
  column: number;
  source?: string;
}

interface Report {
  findings: Finding[];
  elements: number;
  fired: number;
}

interface Settings {
  path: string;
  rules: string;
  checkOn: "save" | "type";
  checkDelay: number;
  docs: string;
  skel: string;
}

const DEFAULTS: Settings = {
  path: "euicc",
  rules: "",
  checkOn: "type",
  checkDelay: 300,
  docs: "https://euicc.waigel.com",
  skel: "",
};

/*
 * A rule identifier, SAIP-HDR-02, which the documentation gives a page of its
 * own: what the rule requires, the clause it comes from, and the .sch that
 * states it. The editor shows the code of a finding as a link when the
 * diagnostic carries one.
 *
 * TypeScript does not do this. Its diagnostics carry a number and no target,
 * because a compiler error is about the code in front of you. A rule here is
 * about a specification you may not have open, so the citation is worth a
 * click. Set euicc.docs to "" to leave the codes plain.
 */
const RULE_ID = /^[A-Z]+(-[A-Z0-9]+)+$/;

function ruleDocs(code: string): { href: string } | undefined {
  if (!settings.docs || !RULE_ID.test(code)) return undefined;
  return {
    href: `${settings.docs.replace(/\/$/, "")}/rules/${code.toLowerCase()}/`,
  };
}

/*
 * What a word is, decided by the schema rather than by the punctuation around
 * it. The grammar has to guess: a colon says alternative, a line start says
 * member, and an identifier standing for a number looks like nothing at all.
 * It guesses well enough to colour a file the moment it opens, before this
 * server has started -- which is why both layers exist, in TypeScript as here.
 *
 * The types are our own with a standard superType, the arrangement the
 * Terraform extension uses: a theme may style asn1Member directly and falls
 * back to `property` otherwise. package.json pins each to the scope the
 * grammar already uses, so this changes what a word IS and not what it looks
 * like -- measured against both default dark themes, the colours are the same
 * either way. Mapping to `property` and letting the defaults decide would put
 * 39 per cent of a profile in the editor foreground under Dark 2026, which is
 * where this started.
 */
const LEGEND = {
  tokenTypes: ["asn1Member", "asn1Alternative", "asn1Value", "asn1Type"],
  tokenModifiers: [] as string[],
};

const KIND_TO_TOKEN: Record<string, number> = {
  member: 0,
  alternative: 1,
  value: 2,
  type: 3,
};

const connection = createConnection(ProposedFeatures.all);
const documents = new TextDocuments(TextDocument);
let settings: Settings = DEFAULTS;

connection.onInitialize((_params: InitializeParams) => ({
  capabilities: {
    textDocumentSync: TextDocumentSyncKind.Incremental,
    completionProvider: {
      /*
       * A brace or a comma opens a place where a member name goes, and a
       * colon the place after a CHOICE alternative. Typing a letter triggers
       * the editor's own word completion, which is what asks here as well.
       */
      triggerCharacters: ["{", ",", ":"],
      resolveProvider: false,
    },
    hoverProvider: true,
    definitionProvider: true,
    typeDefinitionProvider: true,
    semanticTokensProvider: { legend: LEGEND, full: true },
    documentSymbolProvider: true,
    codeActionProvider: { codeActionKinds: [CodeActionKind.QuickFix] },
    documentFormattingProvider: true,
  },
}));

let settingsReady: Promise<void> | undefined;

async function loadSettings(): Promise<void> {
  const c = await connection.workspace.getConfiguration("euicc");
  settings = { ...DEFAULTS, ...(c ?? {}) };
}

connection.onInitialized(() => {
  connection.client.register(DidChangeConfigurationNotification.type, undefined);
  // A document can open before the configuration arrives. Without this the
  // first check of a session runs with the default path and reports that euicc
  // is missing, on a machine where it is not.
  settingsReady = loadSettings();
});

connection.onDidChangeConfiguration(async () => {
  settingsReady = loadSettings();
  await settingsReady;
  /* The path to euicc may have changed, and with it the schema. */
  schemaOnce = undefined;
  asnCache = null;
  models.clear();
  for (const d of documents.all()) void check(d, true);
});

/*
 * euicc reads standard input when it is given no file, so the buffer goes
 * straight down the pipe. Checking the file on disk instead would report the
 * state before the edit, which is the state nobody is looking at, and a
 * temporary file would mean three filesystem calls for every pause in typing.
 *
 * The child is returned alongside the result so a superseded run can be
 * killed rather than left to finish and answer about text nobody has any
 * more.
 */
function runEuicc(
  text: string,
  full: boolean
): { done: Promise<Report | string | null>; kill: () => void } {
  const args = ["check", "--json", "-t"];
  if (!full) args.push("--no-rules");
  if (settings.rules) args.push("--rules", settings.rules);
  /* euicc compiles in the path of the machine it was built on; on another
     machine the transforms live elsewhere, and until it could be set here the
     only symptom was a check that never answered. */
  if (settings.skel) args.push("--skel", settings.skel);

  let child: ReturnType<typeof execFile> | undefined;
  let killed = false;
  const done = new Promise<Report | string | null>((resolve) => {
    child = execFile(
      settings.path,
      args,
      { timeout: 20000, maxBuffer: 8 << 20 },
      (err, stdout, stderr) => {
        /*
         * A run this server killed has nothing to say. Its child dies with
         * empty stdout, which is the same shape as "the tool is missing", and
         * for as long as the two were not told apart, superseding a run whose
         * document version had not moved -- a save landing while the typing
         * timer was still pending -- published "cannot run euicc" on line 0
         * until the real answer overwrote it.
         */
        if (killed) {
          resolve(null);
          return;
        }
        // A non-zero exit means findings, not a failure to run. Only an empty
        // stdout says the command itself did not work.
        if (!stdout.trim()) {
          resolve(
            (stderr || "").trim() ||
              `cannot run ${settings.path}: ${err?.message ?? "no output"}`
          );
          return;
        }
        try {
          resolve(JSON.parse(stdout) as Report);
        } catch {
          resolve(`${settings.path} did not write JSON`);
        }
      }
    );
    child.stdin?.end(text, "utf8");
  });
  return {
    done,
    kill: () => {
      killed = true;
      child?.kill();
    },
  };
}

/*
 * euicc, with the buffer on standard input and its output as text. Used by the
 * commands whose answer is not JSON.
 */
function runEuiccText(args: string[], text: string): Promise<string | null> {
  return new Promise((resolve) => {
    const child = execFile(
      settings.path,
      args,
      { timeout: 20000, maxBuffer: 32 << 20 },
      (err, stdout, stderr) => {
        if (err || !stdout) {
          const why = (stderr || "").trim() || err?.message || "no output";
          connection.console.warn(`euicc ${args.join(" ")}: ${why}`);
          resolve(null);
          return;
        }
        resolve(stdout);
      }
    );
    child.stdin?.end(text, "utf8");
  });
}

/*
 * The run in flight for a document, so a newer one can end it. Without this
 * the slower of two overlapping runs wins, and the editor shows findings
 * about text that has since been rewritten. The TypeScript extension cancels
 * its in-flight request on every edit for the same reason.
 */
const running = new Map<string, () => void>();

/*
 * One slot per kind of finding, merged at publish -- the arrangement
 * terraform-ls keeps as map[uri]map[source]. What the reader and the
 * constraints say is recomputed at every pause in typing, because it is about
 * the text under the cursor and costs single milliseconds. What the rule set
 * says is recomputed on open and save: the prose rules describe a whole
 * package, rerunning them against text that is mid-edit answers a question
 * nobody asked, and they are the expensive part. Between saves the rule
 * findings stand as last computed, the way a type error outlives the syntax
 * error you are in the middle of making.
 */
interface Slots {
  fast: Diagnostic[];
  rules: Diagnostic[];
}
const slots = new Map<string, Slots>();

/*
 * The version a full check last ran for. TextDocuments fires a content event
 * for the open itself, so without this the open ran twice -- and the second,
 * cheaper run could kill the first before the rule findings ever arrived. A
 * fast run for a version the full lane already covers has nothing to add.
 */
const fullChecked = new Map<string, number>();

async function check(doc: TextDocument, full: boolean): Promise<void> {
  if (doc.languageId !== "asn1-vn") return;
  if (settingsReady) await settingsReady;
  if (full) fullChecked.set(doc.uri, doc.version);
  else if (fullChecked.get(doc.uri) === doc.version) return;

  running.get(doc.uri)?.();
  const version = doc.version;
  const run = runEuicc(doc.getText(), full);
  running.set(doc.uri, run.kill);

  const report = await run.done;
  if (running.get(doc.uri) === run.kill) running.delete(doc.uri);
  /* Superseded, or the buffer moved on: the answer is about old text. */
  if (report === null) return;
  if (doc.version !== version) return;
  if (typeof report === "string") {
    // The tool is missing or broke. Say so once, on the first line, rather
    // than leave stale findings implying the file was checked.
    slots.delete(doc.uri);
    connection.sendDiagnostics({
      uri: doc.uri,
      diagnostics: [
        {
          severity: DiagnosticSeverity.Warning,
          range: { start: { line: 0, character: 0 }, end: { line: 0, character: 1 } },
          message: report,
          source: "euicc",
        },
      ],
    });
    return;
  }

  const schema = await loadSchema();

  const fast: Diagnostic[] = [];
  const rules: Diagnostic[] = [];
  for (const f of report.findings) {
    const line = Math.max(0, f.line - 1);
    const lineText = doc.getText({
      start: { line, character: 0 },
      end: { line: line + 1, character: 0 },
    });
    const character = byteToUtf16(lineText, Math.max(0, f.column - 1));
    const d: Diagnostic = {
      severity:
        f.severity === "warning"
          ? DiagnosticSeverity.Warning
          : DiagnosticSeverity.Error,
      range: {
        start: { line, character },
        end: { line, character: Math.max(character + 1, lineText.trimEnd().length) },
      },
      code: f.code,
      codeDescription: ruleDocs(f.code),
      source: f.source ? `euicc (${f.source})` : "euicc",
      message: f.message,
    };
    /* explain() turns the column back into an offset, so it has to get the
       converted one -- the finding's own is still bytes. */
    const extra = schema
      ? explain(schema, { ...f, line: line + 1, column: character + 1 }, doc)
      : null;
    if (extra) {
      d.message = extra.message;
      if (extra.related) d.relatedInformation = [extra.related];
      if (extra.fix) d.data = extra.fix;
    }
    /* A rule finding names the .sch it came from; the reader's do not. */
    (f.source ? rules : fast).push(d);
  }

  const prev = slots.get(doc.uri);
  const next: Slots = full
    ? { fast, rules }
    : { fast, rules: prev?.rules ?? [] };
  slots.set(doc.uri, next);

  connection.sendDiagnostics({
    uri: doc.uri,
    diagnostics: [...next.fast, ...next.rules],
  });
}

/* ---- what a finding leaves out --------------------------------------------- */

/*
 * The reader says what it wanted and stops, because that is all it has: the
 * descriptor it is walking, and no schema in front of it. The schema knows the
 * member's declared type, and the ASN.1 file says which line declares it.
 *
 * The wording is TypeScript's, read out of the compiler that ships inside VS
 * Code rather than written to look like it:
 *
 *     Type '{0}' is not assignable to type '{1}'.
 *     Property '{0}' is missing in type '{1}'.
 *     '{0}' is declared here.
 *     The expected type comes from property '{0}' which is declared here on
 *     type '{1}'
 *
 * Nothing here decides that a file is wrong; euicc has already decided. This
 * only restates the same finding with what the schema adds, and where it
 * cannot work the member out, the finding passes through as it came.
 */

/* X.680 clause 12: what was written, named by its lexical item. */
const LEXICAL: Array<[RegExp, string]> = [
  [/^'[0-9A-Fa-f\s]*'[Hh]/, "hstring"],
  [/^'[01\s]*'[Bb]/, "bstring"],
  [/^"/, "cstring"],
  [/^-?\d/, "number"],
  [/^\{/, "{ … }"],
  [/^[A-Za-z]/, "identifier"],
];

interface Explained {
  message: string;
  related?: DiagnosticRelatedInformation;
  /* An edit the editor can offer, at the start of the diagnostic's range. */
  fix?: { title: string; insert: string };
}

/*
 * euicc counts a column in bytes from the start of the line; LSP counts UTF-16
 * code units. On a line whose earlier characters are all ASCII the two agree,
 * which is how the difference survived every test so far: one umlaut in a
 * cstring shifts every position after it, and both the squiggle and the quick
 * fix landed beside their token.
 */
function byteToUtf16(lineText: string, byteCol: number): number {
  let bytes = 0;
  let i = 0;
  while (i < lineText.length && bytes < byteCol) {
    const cp = lineText.codePointAt(i)!;
    bytes += cp <= 0x7f ? 1 : cp <= 0x7ff ? 2 : cp <= 0xffff ? 3 : 4;
    i += cp > 0xffff ? 2 : 1;
  }
  return i;
}

/*
 * The ASN.1 the schema is written in, read once and kept with its line index.
 * explain() used to read the 1197-line file again for every finding, and a
 * definition request twice more on top. Invalidated with the schema, because
 * euicc.path decides where both live.
 */
let asnCache: { source: string; text: string; lines: string[] } | null = null;

function loadAsn(schema: Schema): { text: string; lines: string[] } | null {
  if (!schema.source) return null;
  if (asnCache && asnCache.source === schema.source) return asnCache;
  try {
    const text = readFileSync(schema.source, "utf8");
    asnCache = { source: schema.source, text, lines: text.split("\n") };
  } catch {
    return null;
  }
  return asnCache;
}

/* A place in the ASN.1, for the second line of a TypeScript-shaped error. */
function declaredAt(
  schema: Schema,
  type: string,
  name: string,
  message: string
): DiagnosticRelatedInformation | undefined {
  const asn = loadAsn(schema);
  if (!asn || !schema.source) return undefined;
  const line = declarationLine(asn.text, type, name);
  if (line === null) return undefined;
  return {
    location: {
      uri: pathToFileURL(schema.source).toString(),
      range: {
        start: { line, character: 0 },
        end: { line, character: asn.lines[line].length },
      },
    },
    message,
  };
}

function explain(
  schema: Schema,
  finding: Finding,
  doc: TextDocument
): Explained | null {
  const missing = /^(\S+) is missing mandatory member '([^']+)'/.exec(finding.message);
  if (missing) {
    const [, type, name] = missing;
    if (!schema.types[type]?.members?.some((x) => x.name === name)) return null;
    return {
      message: `Property '${name}' is missing in type '${type}'.`,
      related: declaredAt(schema, type, name, `'${name}' is declared here.`),
    };
  }

  /*
   * A CHOICE alternative is written `name : value` and this one has no colon.
   * The reader stops exactly where the colon belongs, so the name is the word
   * before that point and the fix is an insertion at it.
   */
  if (finding.message === "expected : after the alternative name") {
    const text = doc.getText();
    const at = doc.offsetAt({
      line: Math.max(0, finding.line - 1),
      character: Math.max(0, finding.column - 1),
    });
    const found = at > 0 ? memberAt(schema, text, at - 1) : null;
    if (!found) return null;
    return {
      message: `':' expected after the alternative '${found.member.name}'.`,
      related: declaredAt(
        schema,
        found.owner,
        found.member.name,
        `'${found.member.name}' is an alternative of ` +
          `'${readableType(found.owner)}', declared here`
      ),
      /* Whitespace already there takes the colon on its own; without any, the
         insertion has to bring both spaces. */
      fix: {
        title: "Add missing ':'",
        insert: /\s/.test(text[at] ?? "") ? " :" : " : ",
      },
    };
  }

  /*
   * The reader phrases seventeen different failures as an expectation, and
   * only some are about the shape of a value. The rest are punctuation: a
   * comma between members, a colon after an alternative name, a member name
   * or a closing brace. Matching on "expected" alone turned a missing colon
   * into
   *
   *     Type 'hstring' is not assignable to type 'OCTET STRING'.
   *
   * which is false twice over. An hstring is exactly what an OCTET STRING
   * takes, and the value was never what the reader objected to.
   *
   * A list of what may be restated rather than of what may not, so a message
   * added to the reader later is shown as it came instead of being dressed up
   * as something it is not.
   */
  const ABOUT_THE_VALUE = [
    /^expected '\.\.'/,
    /^expected NULL$/,
    /^expected TRUE or FALSE$/,
    /^expected a "/,
    /^expected an integer$/,
    /^expected an enumeration identifier$/,
    /^expected \{ to start /,
  ];
  if (!ABOUT_THE_VALUE.some((re) => re.test(finding.message))) return null;

  const text = doc.getText();
  let offset = doc.offsetAt({
    line: Math.max(0, finding.line - 1),
    character: Math.max(0, finding.column - 1),
  });
  /*
   * The reader stops where the value should have begun, which is the space
   * after the member name and not the token itself. Two things go wrong from
   * a space: no lexical item matches one, and the scan runs backwards over
   * the member name and takes it for a word half typed, so it finds nothing
   * expected at all.
   */
  while (offset < text.length && /\s/.test(text[offset])) offset++;

  const ctx = analyze(schema, text, offset);
  /* ctx.owner, not ctx.type: inside a list of CHOICE the member belongs to
     the element type and the list does not declare it. */
  if (!ctx.expect?.name || !ctx.owner) return null;

  const wrote = LEXICAL.find(([re]) => re.test(text.slice(offset)))?.[1];
  if (!wrote) return null;

  return {
    message: `Type '${wrote}' is not assignable to type '${ctx.expect.type}'.`,
    related: declaredAt(
      schema,
      ctx.owner,
      ctx.expect.name,
      `The expected type comes from property '${ctx.expect.name}' ` +
        `which is declared here on type '${readableType(ctx.owner)}'`
    ),
  };
}

/* ---- completion ----------------------------------------------------------- */

/*
 * The schema does not change while the editor is open, so it is read once.
 * A failure is remembered too: without that, every keystroke would start
 * another euicc that is not there.
 */
let schemaOnce: Promise<Schema | null> | undefined;

function loadSchema(): Promise<Schema | null> {
  if (schemaOnce) return schemaOnce;
  schemaOnce = new Promise((resolve) => {
    execFile(settings.path, ["schema"], { timeout: 20000, maxBuffer: 8 << 20 },
      (err, stdout) => {
        if (err || !stdout.trim()) {
          connection.console.warn(
            `euicc: no schema for completion (${err?.message ?? "no output"})`
          );
          resolve(null);
          return;
        }
        try {
          resolve(JSON.parse(stdout) as Schema);
        } catch {
          connection.console.warn("euicc: the schema was not JSON");
          resolve(null);
        }
      });
  });
  return schemaOnce;
}

connection.onCompletion(async (params): Promise<CompletionItem[]> => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc || doc.languageId !== "asn1-vn") return [];
  if (settingsReady) await settingsReady;

  const schema = await loadSchema();
  if (!schema) return [];

  const text = doc.getText();
  const ctx = analyze(schema, text, doc.offsetAt(params.position));

  return suggest(schema, ctx).map((s) => ({
    label: s.label,
    kind: s.value ? CompletionItemKind.EnumMember : CompletionItemKind.Field,
    detail: s.detail,
    documentation: s.doc
      ? { kind: MarkupKind.Markdown, value: s.doc }
      : undefined,
    insertText: s.insert,
    insertTextFormat: s.snippet
      ? InsertTextFormat.Snippet
      : InsertTextFormat.PlainText,
    sortText: s.sort,
  }));
});

/*
 * Checking as you type means checking a file that is halfway through a word,
 * and euicc is a process, not a library call. A check of a 3000 line profile
 * takes about 150 ms, so one per keystroke is waste; one per pause is not.
 * The timer is per document, and a later edit replaces the pending run rather
 * than adding a second.
 */
const pending = new Map<string, NodeJS.Timeout>();

function checkSoon(doc: TextDocument): void {
  const t = pending.get(doc.uri);
  if (t) clearTimeout(t);
  /*
   * A longer file is slower to check, so it waits longer before starting.
   * The shape is the one the TypeScript extension uses, and its reasoning
   * carries over unchanged:
   *
   *     Math.min(Math.max(Math.ceil(buffer.lineCount / 20), 300), 800)
   *
   * checkDelay sets the floor, so turning it down still bounds a large file
   * rather than checking it on every keystroke.
   */
  const floor = Math.max(0, settings.checkDelay);
  const delay = Math.min(Math.max(Math.ceil(doc.lineCount / 20), floor), 800);
  pending.set(
    doc.uri,
    setTimeout(() => {
      pending.delete(doc.uri);
      void check(doc, false);
    }, delay)
  );
}

/*
 * What the name under the cursor is. TypeScript answers this from the type
 * checker; the same answer is in the schema here, and it is the one thing a
 * profile author cannot get from the file itself: whether a member may be
 * left out, and what it is allowed to hold.
 */
connection.onHover(async (params) => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc || doc.languageId !== "asn1-vn") return null;
  if (settingsReady) await settingsReady;

  const schema = await loadSchema();
  if (!schema) return null;

  const found = memberAt(schema, doc.getText(), doc.offsetAt(params.position));
  if (!found) return null;

  const { member, owner, path } = found;
  const head = `${member.name}: ${member.type}` +
    (member.optional ? "  (optional)" : "");
  const lines = [
    "```asn1-vn",
    head,
    "```",
    describe(member, owner),
  ];
  if (path.length > 1) lines.push(`\nIn \`${path.join(" / ")}\`.`);

  return { contents: { kind: MarkupKind.Markdown, value: lines.join("\n") } };
});

/* ---- laying it out ----------------------------------------------------------- */

/*
 * One edit per line whose indentation is wrong, and nothing else. See
 * indentation() for why this does not go through `euicc show`.
 */
connection.onDocumentFormatting(async (params): Promise<TextEdit[]> => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc || doc.languageId !== "asn1-vn") return [];
  if (settingsReady) await settingsReady;

  const before = doc.getText();
  const after = await runEuiccText(["fmt"], before);
  if (typeof after !== "string" || after === before) return [];

  const end = doc.positionAt(before.length);
  return [{ range: { start: { line: 0, character: 0 }, end }, newText: after }];
});

/* ---- fixing it --------------------------------------------------------------- */

/*
 * The edit a finding already knows about. TypeScript registers each of its
 * seventy-odd fixes against the error codes it repairs; the same idea with one
 * fix, carried on the diagnostic itself rather than worked out again from its
 * text.
 */
connection.onCodeAction((params): CodeAction[] => {
  const out: CodeAction[] = [];
  for (const d of params.context.diagnostics) {
    const fix = d.data as { title?: string; insert?: string } | undefined;
    if (!fix?.title || fix.insert === undefined) continue;
    out.push({
      title: fix.title,
      kind: CodeActionKind.QuickFix,
      diagnostics: [d],
      isPreferred: true,
      edit: {
        changes: {
          [params.textDocument.uri]: [
            { range: { start: d.range.start, end: d.range.start }, newText: fix.insert },
          ],
        },
      },
    });
  }
  return out;
});

/* ---- the document, read once per version ------------------------------------ */

/*
 * One walk of the text yields what every identifier is and the tree of named
 * braces, and both semantic tokens and the outline want it -- the editor asks
 * for tokens at every visible change and for symbols right beside. Cached by
 * document version, dropped on close, cleared when the schema reloads,
 * because the schema is what the walk reads meaning from.
 */
const models = new Map<string, { version: number; m: DocumentModel }>();

async function getModel(doc: TextDocument): Promise<DocumentModel | null> {
  if (settingsReady) await settingsReady;
  const schema = await loadSchema();
  if (!schema) return null;
  const hit = models.get(doc.uri);
  if (hit && hit.version === doc.version) return hit.m;
  const m = model(schema, doc.getText());
  models.set(doc.uri, { version: doc.version, m });
  return m;
}

/* ---- what each word is ------------------------------------------------------ */

connection.languages.semanticTokens.on(async (params) => {
  const doc = documents.get(params.textDocument.uri);
  const builder = new SemanticTokensBuilder();
  if (!doc || doc.languageId !== "asn1-vn") return builder.build();
  if (settingsReady) await settingsReady;

  const m = await getModel(doc);
  if (!m) return builder.build();

  /*
   * Only what the schema recognises is reported. A name it does not know keeps
   * whatever the grammar made of it: a semantic token can add a colour and
   * never take one away, so a typo cannot be made to look like one this way.
   * What marks it is the diagnostic.
   */
  for (const c of m.classified) {
    const at = doc.positionAt(c.offset);
    builder.push(at.line, at.character, c.length, KIND_TO_TOKEN[c.kind], 0);
  }
  return builder.build();
});

/* ---- the outline ------------------------------------------------------------- */

/*
 * The named braces, nested as written: thirty profile elements to navigate a
 * 3235-line package by, each with the structures inside it. Leaf members stay
 * out -- a line the eye finds on its own would bury the elements, 1861 to 30
 * in a published profile.
 */
connection.onDocumentSymbol(async (params) => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc || doc.languageId !== "asn1-vn") return null;
  const m = await getModel(doc);
  if (!m) return null;

  const toSymbol = (n: (typeof m.nodes)[number], depth: number): DocumentSymbol => ({
    name: n.name,
    detail: n.type ?? undefined,
    /* A top-level node is one ProfileElement; everything below is a member. */
    kind: depth === 0 ? SymbolKind.Struct : SymbolKind.Object,
    range: { start: doc.positionAt(n.start), end: doc.positionAt(n.end) },
    selectionRange: {
      start: doc.positionAt(n.nameStart),
      end: doc.positionAt(n.nameStart + n.nameLength),
    },
    children: n.children.map((k) => toSymbol(k, depth + 1)),
  });
  return m.nodes.map((n) => toSymbol(n, 0));
});

/* ---- going to the schema --------------------------------------------------- */

/*
 * A profile is written against a schema that lives in another file, and until
 * now the only way in was a link on a finding. TypeScript registers both a
 * definition and a type definition provider, and both mean something here:
 *
 *   definition       the line of the ASN.1 that declares this name -- the
 *                    member, or the assignment if the name is a type
 *   type definition  the assignment of the member's type, which is where the
 *                    value being written is actually described
 *
 * On `mf File,` the first goes to that line and the second to `File ::=`.
 */
function asnLocation(schema: Schema, line: number | null) {
  if (line === null || !schema.source) return null;
  const asn = loadAsn(schema);
  if (!asn) return null;
  return {
    uri: pathToFileURL(schema.source).toString(),
    range: {
      start: { line, character: 0 },
      end: { line, character: (asn.lines[line] ?? "").length },
    },
  };
}

async function schemaAndAsn(uri: string) {
  if (settingsReady) await settingsReady;
  const doc = documents.get(uri);
  if (!doc || doc.languageId !== "asn1-vn") return null;
  const schema = await loadSchema();
  if (!schema?.source) return null;
  const asn = loadAsn(schema);
  if (!asn) return null;
  return { doc, schema, asn: asn.text };
}

connection.onDefinition(async (params) => {
  const got = await schemaAndAsn(params.textDocument.uri);
  if (!got) return null;
  const { doc, schema, asn } = got;
  const text = doc.getText();
  const offset = doc.offsetAt(params.position);

  const found = memberAt(schema, text, offset);
  if (found)
    return asnLocation(schema, declarationLine(asn, found.owner, found.member.name));

  /* A type reference, which is written where a member name is not. */
  const word = wordAt(text, offset);
  if (word && schema.types[word])
    return asnLocation(schema, assignmentLine(asn, word));
  return null;
});

connection.onTypeDefinition(async (params) => {
  const got = await schemaAndAsn(params.textDocument.uri);
  if (!got) return null;
  const { doc, schema, asn } = got;
  const found = memberAt(schema, doc.getText(), doc.offsetAt(params.position));
  if (!found) return null;
  /* A builtin has no assignment to go to; the declaration is the closest
     thing to one, and going nowhere would be worse than going there. */
  const line = schema.types[found.member.type]
    ? assignmentLine(asn, found.member.type)
    : declarationLine(asn, found.owner, found.member.name);
  return asnLocation(schema, line);
});

documents.onDidSave((e) => void check(e.document, true));
documents.onDidOpen((e) => void check(e.document, true));
documents.onDidChangeContent((e) => {
  if (settings.checkOn === "type") checkSoon(e.document);
});
documents.onDidClose((e) => {
  const t = pending.get(e.document.uri);
  if (t) clearTimeout(t);
  pending.delete(e.document.uri);
  slots.delete(e.document.uri);
  fullChecked.delete(e.document.uri);
  models.delete(e.document.uri);
  connection.sendDiagnostics({ uri: e.document.uri, diagnostics: [] });
});

documents.listen(connection);
connection.listen();
