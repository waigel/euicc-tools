/*
 * The VS Code side. It starts the language server and does nothing else: every
 * diagnostic comes from euicc, through the server.
 */

import { join } from "node:path";
import { ExtensionContext } from "vscode";
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  TransportKind,
} from "vscode-languageclient/node";

let client: LanguageClient | undefined;

export function activate(context: ExtensionContext): void {
  const module = context.asAbsolutePath(join("out", "server.js"));
  const server: ServerOptions = {
    run: { module, transport: TransportKind.ipc },
    debug: {
      module,
      transport: TransportKind.ipc,
      options: { execArgv: ["--nolazy", "--inspect=6009"] },
    },
  };
  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: "file", language: "asn1-vn" }],
  };
  client = new LanguageClient("euicc", "eUICC profile packages", server, clientOptions);
  void client.start();
}

export function deactivate(): Thenable<void> | undefined {
  return client?.stop();
}
