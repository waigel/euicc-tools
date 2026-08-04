# eUICC profile packages

Write eUICC profile packages in ASN.1 value notation, with the rules checked as
you save.

The format is the Trusted Connectivity Alliance's *eUICC Profile Package:
Interoperable Format*, version 3.4.1, which the industry calls SAIP.

## What it does

- Highlights `.vn` and `.asn1vn` files, and corrects the guesses from the schema.
- Offers the members allowed at the cursor, with their types.
- Shows a member's type and whether it may be left out, on hover.
- Goes to the line of the ASN.1 that declares a name.
- Reports the errors that `euicc check` reports, as you write them, and offers
  the edit where it knows one.
- Lays the file out: one member to a line, indented by depth.

A profile package fails in two ways. The ASN.1 module states what a value may
hold: a type, a size, a range. The specification states in prose what a package
must look like: one header, and it comes first. Both appear as diagnostics, at
two cadences: what the reader and the constraints say is recomputed at every
pause in typing and costs single milliseconds, and what the rule set says is
recomputed when you open or save. Between saves the rule findings stand as
last computed, the way a type error outlives the syntax error you are in the
middle of making.

## Two layers of colour

A TextMate grammar colours the file the moment it opens, before this extension's
server has started. It reads punctuation: a colon means a CHOICE alternative, a
line start means a member name. That is enough to read by and it guesses.

The server then says what each word actually is, from the schema. `pukCodes` is
an alternative of `ProfileElement` on one line and a member of `PE-PUKCodes` on
another, and nothing in the text says which. An identifier standing for a
number, `pukAppl1`, matches no grammar rule at all.

The token types are pinned to the scopes the grammar already uses, so this
settles what a word is without changing what it looks like. A name the schema
does not know is not reported: a semantic token can add a colour and never
remove one, so a typo is marked by the diagnostic and not by this.

## Completion

The list comes from the schema, which `euicc schema` reads out of asn1c's own
type descriptors. What is offered depends on where the cursor is:

| Where | What is offered |
| --- | --- |
| After `::=` | The alternatives of `ProfileElement`, written `name : ` |
| Inside a SEQUENCE | Its members, mandatory ones first, written `name ` |
| Inside a CHOICE | Its alternatives, written `name : ` |
| Inside a list of CHOICE | Its alternatives, written `name : `, with no braces of their own |
| After a member name | The identifiers that member accepts, where it has any |

A member already written is not offered again. `milenage` and the other named
numbers are there because `euicc` carries a table of the identifiers asn1c
parses and does not keep. The same table lets you write the name instead of the
number, and `euicc show` prints it back.

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
| `euicc.checkOn` | `type` to check as you write, `save` to wait for a save. |
| `euicc.checkDelay` | Milliseconds of quiet before a check while typing. |
| `euicc.docs` | Where the rule pages are. Empty leaves the codes plain. |
| `euicc.skel` | The ISO Schematron transforms, if the compiled-in path does not exist here. |

The extension parses no ASN.1 and evaluates no rule. Every diagnostic comes from
`euicc`, so the editor cannot report something that the build does not.

A parse error is marked where it is, including one the schema catches: a hex
string where the type wants text is a parse error and appears as you write it.
A value that lacks a mandatory member is marked on the brace of that value,
because there is no other token to point at and the object is what is wrong.

A rule failure is marked on the first line of the profile element it belongs
to, because a rule names an element and not a position in a file.

A finding that the schema can say more about is restated in TypeScript's
words, taken from the compiler that ships inside VS Code:

    Property 'mf' is missing in type 'PE-MF'.
      profile-3.4.1.asn(329): 'mf' is declared here.

    Type 'hstring' is not assignable to type 'UTF8String'.
      profile-3.4.1.asn(76): The expected type comes from property
      'profileType' which is declared here on type 'ProfileHeader'

## Going to the schema

A profile is written against a schema in another file, and both ways in are
the ones TypeScript registers:

| | |
| --- | --- |
| Go to Definition | the line that declares this name, or the assignment if it is a type |
| Go to Type Definition | the assignment of the member's type |

On `mf File,` the first goes to that line and the second to `File ::=`. A
built-in type has no assignment, so Go to Type Definition lands on the
declaration instead of nowhere.

## What it does not do

A rule failure carries a link on its code, to the page for that rule: what it
requires, the clause it comes from, and the `.sch` that states it. TypeScript
does not link its diagnostics, and does not need to -- a compiler error is
about the code in front of you. A rule here is about a specification you may
not have open.

`euicc` decides what is wrong; this only says it with what the schema adds.
Where the member cannot be worked out, the finding is shown as `euicc` gave
it. Finding the declaration is a text search of the schema source, not a
parse, so a miss costs the link and nothing else.

Completion finds the type at the cursor by following the member names through
the open braces. It checks nothing: a suggestion in the wrong place is a wrong
suggestion, and what is wrong in the file is what `euicc check` says. This is
where it differs from TypeScript, whose server type-checks the file as you
type. Value notation needs no such machinery, because it has no inference, no
imports and no generics. The type at a point follows from the path of member
names and from nothing else.

## Formatting

`euicc fmt`, which the extension calls. One member to a line, indented by
depth. Nothing is moved but whitespace, and the guarantee is the token list:
after a pass, the identifiers, literals and comments are the same ones in the
same order, or the file is returned untouched.

It is in the tool and not in the extension for the reason everything else here
is: a second reading of the language is a second thing that can disagree with
the first. It was in the extension once, in TypeScript, and that duplicate
shortened a string running over two lines while its own check said nothing had
moved. The same command is what a pre-commit hook or CI would run.

`euicc show` writes canonical value notation and looks like a formatter until
you notice it re-serialises a decoded value: every comment is gone and
`myHeader ProfileElement ::=` comes back as `value1`. Format on save would
delete documentation without a word, so `fmt` reads the text instead -- which
also means it works on a file the reader rejects, the file most in need of it.

Two things it settles by matching the writer. A long hex string wrapped over
lines is left as the writer laid it out -- X.680 12.12 ignores the whitespace
inside one. An OBJECT IDENTIFIER stays on one line, `{ 2 23 143 1 2 1 }`, which
is what a numbers-only brace group is.

## An HCL example

`examples/comparison.tf` is the same profile written as HCL, for putting the two
side by side with the HashiCorp Terraform extension installed. Nothing reads it;
it is there because the differences are easier to see than to describe. HCL has
an `=` between a name and its value, so a name is always a name. Value notation
has nothing there, which is why this extension has two layers of colour.

## Licence

MIT.
