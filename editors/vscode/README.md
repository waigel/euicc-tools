# eUICC profile packages

Write eUICC profile packages in ASN.1 value notation, with the rules checked
as you type.

The format is *eUICC Profile Package: Interoperable Format*, version 3.4.1,
from the Trusted Connectivity Alliance. The industry calls this format SAIP.

## What it does

- It colors `.vn` and `.asn1vn` files, and it corrects the colors with the
  schema.
- It offers the members that are allowed at the cursor, with their types.
- It shows the type of a member on hover, and whether the member is optional.
- Go to Definition opens the line of the ASN.1 that declares a name. Go to
  Type Definition opens the assignment of its type.
- It outlines the document: every named brace, nested as written. You can
  navigate a package of thirty elements from the sidebar and the breadcrumbs.
- It reports the errors that `euicc check` reports, as you type them. Where it
  knows an edit, it offers the edit.
- It formats: one member to a line, indented by depth. Comments stay.

## What it needs

The `euicc` command, from
[euicc-tools](https://github.com/waigel/euicc-tools):

```sh
git clone --recurse-submodules https://github.com/waigel/euicc-tools.git
cd euicc-tools && make && make install PREFIX=~/.local
```

If `euicc` is in a different location, set `euicc.path`.

## Diagnostics

A profile package fails in two ways, and both appear as you type. The ASN.1
module states what a value can hold: a type, a size, a range. The
specification states in prose what a package must look like: one header, and
the header comes first.

A parse error is marked at its position. A value that lacks a mandatory
member is marked on the brace of that value. A rule failure is marked on the
first line of its profile element. Its code links to the page for that rule:
what it requires and the clause it comes from.

Where the schema can say more, the message says it plainly and points into
the schema:

    Property 'mf' is missing in type 'PE-MF'.
      profile-3.4.1.asn(329): 'mf' is declared here.

Every diagnostic comes from `euicc`. The extension parses no ASN.1 and
evaluates no rule of its own, so it cannot report a fault that the build does
not report.

## Completion

What is offered depends on where the cursor is:

| Where | What is offered |
| --- | --- |
| After `::=` | The alternatives of `ProfileElement`, written `name : ` |
| Inside a SEQUENCE | Its members, mandatory ones first, written `name ` |
| Inside a CHOICE | Its alternatives, written `name : ` |
| After a member name | The identifiers that the member accepts, where it has any |

A member that is already written is not offered again. Named numbers are
offered by name: write `algorithmID milenage`, not `algorithmID 1`, and
`euicc show` prints the name back.

## Settings

| Setting | Effect |
| --- | --- |
| `euicc.path` | The command. A path, or a name on PATH. |
| `euicc.rules` | The rule set. Empty uses the one compiled into `euicc`. |
| `euicc.checkOn` | `type` to check as you type, `save` to wait for a save. |
| `euicc.checkDelay` | Milliseconds of quiet before a check while you type. |
| `euicc.docs` | Where the rule pages are. Empty leaves the codes plain. |
| `euicc.skel` | The ISO Schematron transforms, if the compiled-in path does not exist here. |

## Examples

`examples/minimal.vn` is the smallest legal package: a header and an end.
Move the second value above the first and save. Two ordering rules appear.

`examples/profile.vn` is a complete package. `euicc check -s` accepts it
without a finding, and every value comes from the GSMA TS48 test profile. The
comments in the file say which rule pinned each decision.

## Other editors

The server speaks LSP over stdio. Neovim and Helix start
`node out/server.js --stdio` the same way VS Code does.

## License

MIT.
