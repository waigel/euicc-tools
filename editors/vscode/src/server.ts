/*
 * A language server for ASN.1 value notation.
 *
 * It runs `euicc check --json` and turns what comes back into diagnostics.
 * Nothing here parses ASN.1 or evaluates a rule. A second implementation of
 * either would be a second thing that can disagree with the first, and the
 * editor would then report something the build does not.
 *
 * The server speaks LSP, so it is not only for VS Code. Neovim and Helix start
 * it the same way.
 */

import { execFile } from "node:child_process";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

import {
  createConnection,
  Diagnostic,
  DiagnosticSeverity,
  DidChangeConfigurationNotification,
  InitializeParams,
  ProposedFeatures,
  TextDocuments,
  TextDocumentSyncKind,
} from "vscode-languageserver/node";
import { TextDocument } from "vscode-languageserver-textdocument";

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
}

const DEFAULTS: Settings = { path: "euicc", rules: "", checkOn: "save" };

const connection = createConnection(ProposedFeatures.all);
const documents = new TextDocuments(TextDocument);
let settings: Settings = DEFAULTS;

connection.onInitialize((_params: InitializeParams) => ({
  capabilities: {
    textDocumentSync: TextDocumentSyncKind.Incremental,
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
  for (const d of documents.all()) void check(d);
});

/*
 * euicc reads a file. An unsaved buffer has no file, so the text goes to a
 * temporary one. Checking the file on disk instead would report the state
 * before the edit, which is the state nobody is looking at.
 */
async function runEuicc(text: string): Promise<Report | string> {
  const dir = await mkdtemp(join(tmpdir(), "euicc-"));
  const file = join(dir, "buffer.vn");
  try {
    await writeFile(file, text, "utf8");
    const args = ["check", "--json", "-t"];
    if (settings.rules) args.push("--rules", settings.rules);
    args.push(file);

    return await new Promise((resolve) => {
      execFile(settings.path, args, { timeout: 20000 }, (err, stdout, stderr) => {
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
      });
    });
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
}

async function check(doc: TextDocument): Promise<void> {
  if (doc.languageId !== "asn1-vn") return;
  if (settingsReady) await settingsReady;

  const report = await runEuicc(doc.getText());
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

  const diagnostics: Diagnostic[] = report.findings.map((f) => {
    const line = Math.max(0, f.line - 1);
    const character = Math.max(0, f.column - 1);
    const lineText = doc.getText({
      start: { line, character: 0 },
      end: { line: line + 1, character: 0 },
    });
    return {
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
  });

  connection.sendDiagnostics({ uri: doc.uri, diagnostics });
}

documents.onDidSave((e) => void check(e.document));
documents.onDidOpen((e) => void check(e.document));
documents.onDidChangeContent((e) => {
  if (settings.checkOn === "type") void check(e.document);
});
documents.onDidClose((e) =>
  connection.sendDiagnostics({ uri: e.document.uri, diagnostics: [] })
);

documents.listen(connection);
connection.listen();
