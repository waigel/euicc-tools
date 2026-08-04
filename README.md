# euicc-tools

`euicc` builds and checks eUICC profile packages. It is one binary. It needs no
Python, no separate codec, and no schema on the side.

```sh
euicc build profile.vn -o profile.der   # value notation in, DER out
euicc show  profile.der                 # DER in, value notation out
euicc check profile.der                 # a verdict
euicc diff  profile.vn vendor.der       # what separates the two
euicc fmt   -w profile.vn               # the canonical layout, in place
euicc schema                            # the schema as JSON
```

## Why one binary

A profile package fails in two ways. Before this tool, two programs reported
them.

The ASN.1 module states what a value can hold: a type, a size, a range. The
specification states in prose what a package must look like: one header, and
the header comes first. A package can satisfy one and violate the other.

`euicc check` reports both:

```
$ euicc check profile.vn
  error    SAIP-HDR-02   The Profile Header shall be the first ProfileElement.
           at            /ProfilePackage
  error    SAIP-END-02   The PE-End shall be the last ProfileElement.
           at            /ProfilePackage

profile.vn: 2 errors, 0 warnings, 7 rule instances fired over 2 profile elements
```

The count at the end is data. A rule fires only when its context occurs in the
package. A rule that did not fire did not pass. It did not run.

## Why C

Two engines sit behind the command, and C links to both directly.

The codec is [asn1c-vn](https://github.com/waigel/asn1c-vn), a C library. It
links directly. No foreign function interface stands between the tool and the
codec.

The rules are ISO Schematron. The reference method to run them is XSLT. The
Python package `lxml` runs them that way, and `lxml` is a binding to libxml2
and libxslt. `euicc` links to the same libxslt, so the rules run unchanged.
This point is important: 24 expressions in the rule set call `generate-id()`,
and three call `current()`. These two functions are XSLT functions, not XPath
functions. An XPath evaluator cannot run them. libxslt is an XSLT processor
and has both.

## Build

```sh
git clone --recurse-submodules https://github.com/waigel/euicc-tools.git
cd euicc-tools
make
```

The build compiles asn1c from the submodule, generates the codec from the
schema, and links everything. You need libxml2 and libxslt. macOS and every
Linux distribution ship them.

```sh
make check
make install PREFIX=~/.local     # or PREFIX=/usr/local with sudo
```

The build finds the ISO Schematron transforms in an installed `python3-lxml`
package. The interpreter never runs after the build. If the transforms are in
a different location, name it with `--skel`.

The build compiles the path of the rule set and the path of the transforms
into the binary, as absolute paths. An installed binary reads them where they
are. If you move the checkout, these paths break. The `--rules` and `--skel`
options override them.

## What it holds

| Submodule | Supplies |
| --- | --- |
| `vendor/euicc-profile-tool` | the schema, asn1c, and asn1c-vn |
| `vendor/saip-validator` | the 112 rules, as ISO Schematron |

The rules are not copied here. They stay in the submodule, where they are
written. A correction there needs no change in this repository.

## diff, and the plan that was not

`euicc diff` compares a source file against a package. It writes nothing and
changes nothing.

```
$ euicc diff profile.vn vendor.der
profile.vn against vendor.der:

  ~ header  (34 -> 36 bytes)
  + rfm, identification 29  (58 bytes)

0 added, 1 changed, 0 removed, 1 unchanged
```

The order of the elements is reported separately. Half the rule set is about
the order, and a hexdump does not show the order. The exit code is the answer:
0 for no difference, 1 for a difference, 2 for a failure.

The command started as `euicc plan`, on the model of Terraform. The analogy
did not hold. Terraform needs a plan because apply changes live
infrastructure, and one line of configuration can destroy a database. Here
`build` writes a local file, and `show` prints what a package holds. The value
notation is the profile, so `git diff` covers the rest.

One case remains: text on one side, bytes on the other. For example, a package
from a vendor against the source that must produce it. `diff` covers that
case, and that is its name.

## In an editor

`editors/vscode` holds an extension and a language server. The server runs
`euicc check --json` and turns the report into diagnostics. It parses no ASN.1
and evaluates no rule of its own. Thus the editor cannot report a fault that
the build does not report.

```sh
cd editors/vscode
npm install
npm test        # the grammar and the server, without an editor
npm run package # writes euicc-vn-<version>.vsix
```

To install the package:

```sh
code --install-extension editors/vscode/euicc-vn-<version>.vsix
```

`npm test` runs without an editor. It divides value notation into tokens and
compares the scopes against what the grammar promises. It then drives the
server over LSP and requires the correct diagnostics, positions, tokens, and
completions.

To see the extension in an editor, open `editors/vscode` in VS Code and press
F5. A window opens with the extension loaded. Open `examples/minimal.vn`, move
the second value above the first, and save. Two ordering rules appear.

The tests do not cover the activation of the extension. Activation needs a
live editor.

The server speaks LSP over stdio, so it is not only for VS Code. Neovim and
Helix start `node out/server.js --stdio` the same way.

A parse error is marked at its position, because the reader states a line and
a column. A rule failure is marked on the first line of its profile element. A
rule names an element, not a position in a file.

## Releases

CI builds the binary and runs both test suites on each push
(`.github/workflows/ci.yml`). A tag `vscode-vX.Y.Z` runs the same tests, then
publishes the extension to the Visual Studio Marketplace and attaches the
`.vsix` to a GitHub release (`.github/workflows/release-extension.yml`).

To release the extension:

1. Increase `version` in `editors/vscode/package.json` and commit.
2. Create the tag: `git tag vscode-v<version>`.
3. Push the tag: `git push origin vscode-v<version>`.

The tag must name the version that `package.json` carries. If the two differ,
the workflow stops before it publishes. Publication needs one repository
secret, `VSCE_PAT`: an Azure DevOps token with the Marketplace publish scope.

## What it does not do

`euicc` does not read the specification. Each rule cites the clause it comes
from, and the tool prints that citation without resolution. The
[eUICC Profile Reference](https://euicc.waigel.com) shows the clause and the
element that the rule is about.

The counter counts rule instances, not distinct rules. A package of 30
elements fires the same rule many times. `saip-validator` reports a different
number, "89 of 112 assertions evaluated". That number answers a different
question.

## License

MIT. The submodules carry their own terms.
