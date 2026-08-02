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
  InitializeParams,
  InsertTextFormat,
  MarkupKind,
  ProposedFeatures,
  TextDocuments,
  TextDocumentSyncKind,
} from "vscode-languageserver/node";
import { TextDocument } from "vscode-languageserver-textdocument";

import {
  analyze,
  declarationLine,
  describe,
  memberAt,
  Schema,
  suggest,
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
}

const DEFAULTS: Settings = {
  path: "euicc",
  rules: "",
  checkOn: "type",
  checkDelay: 300,
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
  for (const d of documents.all()) void check(d);
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
function runEuicc(text: string): { done: Promise<Report | string>; kill: () => void } {
  const args = ["check", "--json", "-t"];
  if (settings.rules) args.push("--rules", settings.rules);

  let child: ReturnType<typeof execFile> | undefined;
  const done = new Promise<Report | string>((resolve) => {
    child = execFile(
      settings.path,
      args,
      { timeout: 20000, maxBuffer: 8 << 20 },
      (err, stdout, stderr) => {
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
  return { done, kill: () => child?.kill() };
}

/*
 * The run in flight for a document, so a newer one can end it. Without this
 * the slower of two overlapping runs wins, and the editor shows findings
 * about text that has since been rewritten. The TypeScript extension cancels
 * its in-flight request on every edit for the same reason.
 */
const running = new Map<string, () => void>();

async function check(doc: TextDocument): Promise<void> {
  if (doc.languageId !== "asn1-vn") return;
  if (settingsReady) await settingsReady;

  running.get(doc.uri)?.();
  const version = doc.version;
  const run = runEuicc(doc.getText());
  running.set(doc.uri, run.kill);

  const report = await run.done;
  if (running.get(doc.uri) === run.kill) running.delete(doc.uri);
  /* The buffer moved on while this ran; its answer is about the old text. */
  if (doc.version !== version) return;
  if (typeof report === "string") {
    // The tool is missing or broke. Say so once, on the first line, rather
    // than leave the file looking clean.
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

  const diagnostics: Diagnostic[] = report.findings.map((f) => {
    const line = Math.max(0, f.line - 1);
    const character = Math.max(0, f.column - 1);
    const lineText = doc.getText({
      start: { line, character: 0 },
      end: { line: line + 1, character: 0 },
    });
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
      source: f.source ? `euicc (${f.source})` : "euicc",
      message: f.message,
    };
    const extra = schema ? explain(schema, f.message) : null;
    if (extra) {
      d.message = extra.message;
      if (extra.related) d.relatedInformation = [extra.related];
    }
    return d;
  });

  connection.sendDiagnostics({ uri: doc.uri, diagnostics });
}

/* ---- what a finding leaves out --------------------------------------------- */

/*
 * The reader names the type and the member it wanted and stops there, because
 * that is all it knows. The schema knows the member's type and the ASN.1 file
 * says where it is declared, and TypeScript reports both:
 *
 *     Property 'test' is missing in type '{}' but required in type 'Thing'.
 *     thing.ts(2, 5): 'test' is declared here.
 *
 * The second line is a separate location, which LSP carries as related
 * information. Adding it costs a text search of the schema source; if the
 * file is not there or the search misses, the finding is passed through as it
 * came and nothing is lost.
 */
function explain(
  schema: Schema,
  message: string
): { message: string; related?: DiagnosticRelatedInformation } | null {
  const m = /^(\S+) is missing mandatory member '([^']+)'/.exec(message);
  if (!m) return null;
  const [, type, name] = m;

  const member = schema.types[type]?.members?.find((x) => x.name === name);
  if (!member) return null;

  const out = `${type} is missing mandatory member '${name}', of type ${member.type}`;
  if (!schema.source) return { message: out };

  let asn: string;
  try {
    asn = readFileSync(schema.source, "utf8");
  } catch {
    return { message: out };
  }
  const line = declarationLine(asn, type, name);
  if (line === null) return { message: out };

  return {
    message: out,
    related: {
      location: {
        uri: pathToFileURL(schema.source).toString(),
        range: {
          start: { line, character: 0 },
          end: { line, character: asn.split("\n")[line].length },
        },
      },
      message: `'${name}' is declared here`,
    },
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
      void check(doc);
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

documents.onDidSave((e) => void check(e.document));
documents.onDidOpen((e) => void check(e.document));
documents.onDidChangeContent((e) => {
  if (settings.checkOn === "type") checkSoon(e.document);
});
documents.onDidClose((e) => {
  const t = pending.get(e.document.uri);
  if (t) clearTimeout(t);
  pending.delete(e.document.uri);
  connection.sendDiagnostics({ uri: e.document.uri, diagnostics: [] });
});

documents.listen(connection);
connection.listen();
