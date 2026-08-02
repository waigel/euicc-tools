# eUICC profile packages

Write eUICC profile packages in ASN.1 value notation, with the rules checked as
you save.

The format is the Trusted Connectivity Alliance's *eUICC Profile Package:
Interoperable Format*, version 3.4.1, which the industry calls SAIP.

## What it does

- Highlights `.vn` and `.asn1vn` files.
- Reports the errors that `euicc check` reports, in the editor.

A profile package fails in two ways. The ASN.1 module states what a value may
hold: a type, a size, a range. The specification states in prose what a package
must look like: one header, and it comes first. Both appear as diagnostics.

## What it needs

The `euicc` command, from
[euicc-tools](https://github.com/waigel/euicc-tools):

```sh
git clone --recurse-submodules https://github.com/waigel/euicc-tools.git
cd euicc-tools && make && make install PREFIX=~/.local
```

If `euicc` is somewhere else, set `euicc.path`.

## Settings

| Setting | Effect |
| --- | --- |
| `euicc.path` | The command. A path, or a name on PATH. |
| `euicc.rules` | The rule set. Empty uses the one compiled into `euicc`. |
| `euicc.checkOn` | `save`, or `type` to check on every keystroke. |

## What it does not do

The extension parses no ASN.1 and evaluates no rule. Every diagnostic comes from
`euicc`, so the editor cannot report something that the build does not.

A parse error is marked where it is. A rule failure is marked on the first line
of the profile element it belongs to, because a rule names an element and not a
position in a file.

## Licence

MIT.
