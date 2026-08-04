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
- It goes to the line of the ASN.1 that declares a name.
- It outlines the document: every named brace, nested as written. You can
  navigate a package of thirty elements from the sidebar and the breadcrumbs.
- It reports the errors that `euicc check` reports, as you type them. Where it
  knows an edit, it offers the edit.
- It lays the file out: one member to a line, indented by depth.

A profile package fails in two ways. The ASN.1 module states what a value can
hold: a type, a size, a range. The specification states in prose what a
package must look like: one header, and the header comes first. Both appear as
diagnostics, at two cadences. What the reader and the constraints say is
computed again at every pause, in single milliseconds. What the rule set says
is computed again when you open or save. Between saves, the last rule findings
stay visible. A type error outlives the syntax error that you type through, in
the same way.

## Two layers of color

A TextMate grammar colors the file the moment it opens, before the server of
this extension has started. It reads punctuation: a colon means a CHOICE
alternative, and a line start means a member name. That is enough to read by,
and it guesses.

The server then says what each word is, from the schema. `pukCodes` is an
alternative of `ProfileElement` on one line and a member of `PE-PUKCodes` on
another. Nothing in the text says which. An identifier that stands for a
number, `pukAppl1`, matches no grammar rule at all.

The token types are pinned to the scopes that the grammar already uses. Thus
the server settles what a word is and does not change what it looks like. The
names are negotiated. In VS Code, the extension advertises them, and the
pinned scopes decide the color. An editor that does not know them, Neovim or
Helix, gets `property`, `enumMember`, and `type` instead. Then its own
highlighting applies. terraform-ls resolves every token through the same
pairing. A name that the schema does not know is not reported. A semantic
token can add a color and can never remove one, so the diagnostic marks the
typo, not the color.

## Completion

The list comes from the schema, which `euicc schema` reads out of the type
descriptors of asn1c. What is offered depends on where the cursor is:

| Where | What is offered |
| --- | --- |
| After `::=` | The alternatives of `ProfileElement`, written `name : ` |
| Inside a SEQUENCE | Its members, mandatory ones first, written `name ` |
| Inside a CHOICE | Its alternatives, written `name : ` |
| Inside a list of CHOICE | Its alternatives, written `name : `, with no braces of their own |
| After a member name | The identifiers that the member accepts, where it has any |

A member that is already written is not offered again. `milenage` and the
other named numbers are in the list because `euicc` carries a table of the
identifiers that asn1c parses and does not keep. The same table lets you write
the name instead of the number, and `euicc show` prints the name back.

## What it needs

The `euicc` command, from
[euicc-tools](https://github.com/waigel/euicc-tools):

```sh
git clone --recurse-submodules https://github.com/waigel/euicc-tools.git
cd euicc-tools && make && make install PREFIX=~/.local
```

If `euicc` is in a different location, set `euicc.path`.

## Settings

| Setting | Effect |
| --- | --- |
| `euicc.path` | The command. A path, or a name on PATH. |
| `euicc.rules` | The rule set. Empty uses the one compiled into `euicc`. |
| `euicc.checkOn` | `type` to check as you type, `save` to wait for a save. |
| `euicc.checkDelay` | Milliseconds of quiet before a check while you type. |
| `euicc.docs` | Where the rule pages are. Empty leaves the codes plain. |
| `euicc.skel` | The ISO Schematron transforms, if the compiled-in path does not exist here. |

The extension parses no ASN.1 and evaluates no rule. Every diagnostic comes
from `euicc`. Thus the editor cannot report a fault that the build does not
report.

A parse error is marked at its position, and that includes one that the schema
catches. A hex string where the type wants text is a parse error, and it
appears as you type it. A value that lacks a mandatory member is marked on the
brace of that value. There is no other token to point at, and the object is
what is wrong.

A rule failure is marked on the first line of its profile element. A rule
names an element, not a position in a file.

Where the schema can say more about a finding, the finding is restated in the
words of TypeScript. The words come from the compiler that ships inside
VS Code:

    Property 'mf' is missing in type 'PE-MF'.
      profile-3.4.1.asn(329): 'mf' is declared here.

    Type 'hstring' is not assignable to type 'UTF8String'.
      profile-3.4.1.asn(76): The expected type comes from property
      'profileType' which is declared here on type 'ProfileHeader'

## Going to the schema

A profile is written against a schema in another file. Both ways in are the
ones that TypeScript registers:

| | |
| --- | --- |
| Go to Definition | the line that declares this name, or the assignment if it is a type |
| Go to Type Definition | the assignment of the type of the member |

On `mf File,` the first goes to that line, and the second goes to `File ::=`.
A built-in type has no assignment, so Go to Type Definition lands on the
declaration and not nowhere.

## What it does not do

A rule failure carries a link on its code, to the page for that rule. The page
shows what the rule requires, the clause, and the `.sch` that states it.
TypeScript
does not link its diagnostics, and it does not need to. A compiler error is
about the code in front of you. A rule here is about a specification, and the
specification is often not open.

`euicc` decides what is wrong. This extension only says it, with what the
schema adds. Where the member cannot be found, the finding is shown as `euicc`
gave it. The search for the declaration is a text search of the schema source,
not a parse. Thus a miss costs the link and nothing else.

Completion finds the type at the cursor when it follows the member names
through the open braces. It checks nothing. A suggestion in the wrong place is
a wrong suggestion, and what is wrong in the file is what `euicc check` says.
Here it differs from TypeScript, whose server checks the types as you type.
Value notation needs no such machinery. It has no inference, no imports, and
no generics. The type at a point follows from the path of member names and
from nothing else.

## Formatting

The extension calls `euicc fmt`. The layout is one member to a line, indented
by depth. Only whitespace moves, and the guarantee is the token list. After a
pass, the identifiers, literals, and comments are the same ones in the same
order, or the file comes back untouched.

The formatter is in the tool and not in the extension, for the reason that
everything else here is. A second reading of the language is a second thing
that can disagree with the first. The extension had a duplicate once, in
TypeScript. That duplicate made a string that ran over two lines shorter,
while its own comparison said that nothing had moved. `euicc fmt -l *.vn` is
the form for pre-commit and CI. It names the files whose layout differs, it
answers in the exit code, and it writes nothing.

`euicc show` writes canonical value notation and looks like a formatter, until
you notice that it writes a decoded value again. Every comment is gone, and
`myHeader ProfileElement ::=` comes back as `value1`. A format on save that
uses `show` deletes documentation without a word. Thus `fmt` reads the text
instead. It
therefore also works on a file that the reader rejects, the file that needs it
most.

Two decisions follow the writer. A long hex string that wraps over lines stays
as the writer laid it out. X.680 12.12 ignores the whitespace inside one. An
OBJECT IDENTIFIER stays on one line, `{ 2 23 143 1 2 1 }`, because that is
what a numbers-only brace group is.

## Examples

`examples/minimal.vn` is the smallest legal package: a header and an end. Move
the second value above the first and save. Two ordering rules appear.

`examples/profile.vn` is a complete package. `euicc check -s` accepts it
without a finding, and every value comes from the GSMA TS48 test profile. The
comments in the file say which rule pinned each decision.

`examples/comparison.tf` is the same profile written as HCL, for a comparison
with the HashiCorp Terraform extension installed. Nothing reads it. It is
there because the differences are easier to see than to describe. HCL has an
`=` between a name and its value, so a name is always a name. Value notation
has nothing there, and that is why this extension has two layers of color.

## License

MIT.
