/*
 * Bundling the extension and the server into one file each.
 *
 * Without it the package carries 196 files from node_modules, almost all of
 * them the language client and server libraries. vsce warns about that, and
 * the warning is about start-up time: an editor loads what it is given.
 *
 * `vscode` stays external. The editor supplies it at run time and it is not on
 * disk to bundle.
 */
import { build } from "esbuild";

const common = {
  bundle: true,
  platform: "node",
  target: "node20",
  format: "cjs",
  external: ["vscode"],
  minify: process.argv.includes("--minify"),
  sourcemap: !process.argv.includes("--minify"),
};

await build({ ...common, entryPoints: ["src/extension.ts"], outfile: "out/extension.js" });
await build({ ...common, entryPoints: ["src/server.ts"], outfile: "out/server.js" });
console.log("bundled out/extension.js and out/server.js");
