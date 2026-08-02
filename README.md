# euicc-tools

`euicc` builds and checks eUICC profile packages. It is one binary. It needs no
Python, no separate codec, and no schema on the side.

```sh
euicc build profile.vn -o profile.der   # value notation in, DER out
euicc show  profile.der                 # DER in, value notation out
euicc check profile.der                 # a verdict
euicc diff  profile.vn vendor.der       # what separates the two
```

## Why one binary

A profile package fails in two ways, and until now two tools reported them.

The ASN.1 module states what a value may hold: a type, a size, a range. The
specification states in prose what a package must look like: one header, and it
comes first. A package that satisfies one can violate the other.

`euicc check` reports both:

```
$ euicc check profile.vn
  error    SAIP-HDR-02   The Profile Header shall be the first ProfileElement.
           at            /ProfilePackage
  error    SAIP-END-02   The PE-End shall be the last ProfileElement.
           at            /ProfilePackage

profile.vn: 2 errors, 0 warnings, 7 rule instances fired over 2 profile elements
```

The count is not decoration. A rule whose context does not occur in a package
did not pass. It did not run.

## Why C

Two engines sit behind the command, and C reaches both without a boundary.

The codec is [asn1c-vn](https://github.com/waigel/asn1c-vn), a C library. It
links directly, and no foreign function interface stands between.

The rules are ISO Schematron, and the reference way to run them is XSLT.
Python's `lxml` does that, and `lxml` is a binding to libxml2 and libxslt. The
same libxslt is linked here, so the rules run unchanged. This matters more than
it sounds: 24 expressions in the rule set call `generate-id()` and three call
`current()`. Both are XSLT functions and not XPath, so an XPath evaluator
cannot run them. libxslt is an XSLT processor and has both.

## Build

```sh
git clone --recurse-submodules https://github.com/waigel/euicc-tools.git
cd euicc-tools
make
```

The build compiles asn1c from the submodule, generates the codec from the
schema, and links everything. You need libxml2 and libxslt, which macOS and
every Linux distribution ship.

```sh
make check
make install PREFIX=~/.local     # or PREFIX=/usr/local with sudo
```

The rule set and the Schematron transforms are compiled in as absolute paths, so
an installed binary reads them where they are. Moving the checkout breaks that,
and `--rules` and `--skel` override it.

## What it holds

| Submodule | Supplies |
| --- | --- |
| `vendor/euicc-profile-tool` | the schema, asn1c, and asn1c-vn |
| `vendor/saip-validator` | the 112 rules, as ISO Schematron |

The rules are not copied here. They stay where they are written and are read
from the submodule, so a correction there needs no change in this repository.

## diff, and the plan that was not

`euicc diff` compares a source file against a package. It writes nothing and
judges nothing.

```
$ euicc diff profile.vn vendor.der
profile.vn against vendor.der:

  ~ header  (34 -> 36 bytes)
  + rfm, identification 29  (58 bytes)

0 added, 1 changed, 0 removed, 1 unchanged
```

The order of the elements is reported on its own, because half the rule set is
about what comes before what, and a hexdump does not show it.

This started as `euicc plan`, on the model of Terraform, and the analogy did not
hold. Terraform needs a plan because apply changes live infrastructure, because
the current state is invisible until you ask, and because one line of
configuration can destroy a database. Here `build` writes a local file, `show`
prints what a package holds, and the value notation is the profile. `git diff`
covers the rest.

One case it does not cover: text on one side, bytes on the other. A package from
a vendor, or from an earlier release, against the source that should produce it.
That is what is left, and `diff` is its name.

## In an editor

`editors/vscode` holds an extension and a language server. The server runs
`euicc check --json` and turns the report into diagnostics. It parses no ASN.1
and evaluates no rule of its own, so the editor cannot report something that the
build does not.

```sh
cd editors/vscode
npm install
npm test        # the grammar and the server, without an editor
npm run package # euicc-vn-0.1.0.vsix, 10 files
```

To install the package:

```sh
code --install-extension editors/vscode/euicc-vn-0.1.0.vsix
```

`npm test` checks two things without an editor. It tokenises value notation and
compares the scopes against what the grammar promises. It then drives the server
over LSP with a package whose header is in the wrong place, and requires both
ordering rules in the answer.

To see it in an editor, open `editors/vscode` in VS Code and press F5. A window
opens with the extension loaded. `examples/minimal.vn` is there to try: move the
second value above the first and save.

What the tests do not cover is the activation of the extension itself, which
needs a running editor.

The server speaks LSP over stdio, so it is not only for VS Code. Neovim and
Helix start `node out/server.js --stdio` the same way.

A parse error is marked where it is, because the reader states a line and a
column. A rule failure is marked on the first line of the profile element it
belongs to: a rule names an element, and not a position in a file.

## What it does not do

`euicc` does not read the specification. Each rule cites the clause it comes
from, and the tool prints that reference without resolving it. Read the
[eUICC Profile Reference](https://euicc.waigel.com) for the clause and for the
element the rule is about.

The counter is rule instances and not distinct rules. A package of 30 elements
fires the same rule many times. `saip-validator` reports the other number, "89
of 112 assertions evaluated", which answers a different question.

## Licence

MIT. The submodules carry their own terms.
