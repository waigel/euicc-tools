/*
 * The VS Code side. It starts the language server and does nothing else: every
 * diagnostic comes from euicc, through the server.
 */

import { readFileSync } from "node:fs";
import { join } from "node:path";
import { ExtensionContext } from "vscode";
import {
  ClientCapabilities,
  FeatureState,
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  StaticFeature,
  TransportKind,
} from "vscode-languageclient/node";

let client: LanguageClient | undefined;

/*
 * Tell the server which semantic token types this editor understands.
 *
 * The language client advertises only the standard types, and VS Code never
 * adds the ones an extension contributes -- the Terraform extension carries
 * the same feature for the same reason. Without it a server that negotiates
 * its legend, as ours now does, would see a client that has never heard of
 * asn1Member and fall back to `property` -- which Dark 2026 resolves to the
 * editor foreground, the grey this extension exists to avoid.
 */
class ContributedTokenTypes implements StaticFeature {
  constructor(private readonly manifestPath: string) {}

  fillClientCapabilities(capabilities: ClientCapabilities): void {
    const st = capabilities.textDocument?.semanticTokens;
    if (!st) return;
    const manifest = JSON.parse(readFileSync(this.manifestPath, "utf8"));
    const contributed: { id: string }[] =
      manifest.contributes?.semanticTokenTypes ?? [];
    st.tokenTypes = st.tokenTypes.concat(contributed.map((t) => t.id));
  }

  getState(): FeatureState {
    return { kind: "static" };
  }

  initialize(): void {}
  clear(): void {}
}

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
  client.registerFeature(new ContributedTokenTypes(context.asAbsolutePath("package.json")));
  void client.start();
}

export function deactivate(): Thenable<void> | undefined {
  return client?.stop();
}
