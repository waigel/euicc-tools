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

A profile package fails in two ways. The ASN.1 module states what a value can
hold: a type, a size, a range. The specification states in prose what a
package must look like: one header, and the header comes first. A package can
satisfy one and violate the other.

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

## Build

```sh
git clone --recurse-submodules https://github.com/waigel/euicc-tools.git
cd euicc-tools
make
make check
make install PREFIX=~/.local     # or PREFIX=/usr/local with sudo
```

You need libxml2 and libxslt. macOS and every Linux distribution ship them.
The build finds the ISO Schematron transforms in an installed `python3-lxml`
package. If the transforms are in a different location, name it with `--skel`.

The build compiles the path of the rule set and the path of the transforms
into the binary, as absolute paths. If you move the checkout, use `--rules`
and `--skel`.

## What it holds

| Submodule | Supplies |
| --- | --- |
| `vendor/euicc-schema` | the schema, asn1c, and asn1c-vn |
| `vendor/saip-validator` | the 112 rules, as ISO Schematron |

The rules are not copied here. They stay in the submodule, so a correction
there needs no change in this repository.

## diff

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

## In an editor

`editors/vscode` holds an extension and a language server: diagnostics as you
type, completion from the schema, hover, outline, and formatting. Its
[README](editors/vscode/README.md) has the details.

```sh
cd editors/vscode
npm install
npm run package
code --install-extension euicc-vn-<version>.vsix
```

Every diagnostic comes from `euicc check --json`. The extension parses no
ASN.1 and evaluates no rule of its own, so the editor cannot report a fault
that the build does not report.

The server speaks LSP over stdio, so it is not only for VS Code. Neovim and
Helix start `node out/server.js --stdio` the same way.

## Releases

CI builds the binary and runs both test suites on each push. To release the
extension:

1. Increase `version` in `editors/vscode/package.json` and commit.
2. Create the tag and push it: `git tag vscode-v<version> && git push origin vscode-v<version>`.

The workflow runs the tests, publishes to the Visual Studio Marketplace, and
attaches the `.vsix` to a GitHub release. The tag must name the version that
`package.json` carries. Publication needs one repository secret, `VSCE_PAT`:
an Azure DevOps token with the Marketplace publish scope.

## What it does not do

`euicc` does not read the specification. Each rule cites the clause it comes
from, and the tool prints that citation without resolution. The
[eUICC Profile Reference](https://euicc.waigel.com) shows the clause and the
element that the rule is about.

## License

MIT. The submodules carry their own terms.
