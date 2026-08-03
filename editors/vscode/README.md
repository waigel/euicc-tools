# eUICC profile packages

Write eUICC profile packages in ASN.1 value notation, with the rules checked as
you save.

The format is the Trusted Connectivity Alliance's *eUICC Profile Package:
Interoperable Format*, version 3.4.1, which the industry calls SAIP.

## What it does

- Highlights `.vn` and `.asn1vn` files.
- Offers the members allowed at the cursor, with their types.
- Shows a member's type and whether it may be left out, on hover.
- Goes to the line of the ASN.1 that declares a name.
- Reports the errors that `euicc check` reports, as you write them.

A profile package fails in two ways. The ASN.1 module states what a value may
hold: a type, a size, a range. The specification states in prose what a package
must look like: one header, and it comes first. Both appear as diagnostics.

## Completion

The list comes from the schema, which `euicc schema` reads out of asn1c's own
type descriptors. What is offered depends on where the cursor is:

| Where | What is offered |
| --- | --- |
| After `::=` | The alternatives of `ProfileElement`, written `name : ` |
| Inside a SEQUENCE | Its members, mandatory ones first, written `name ` |
| Inside a CHOICE | Its alternatives, written `name : ` |
| Inside a list | One element, wrapped in its own braces |
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

## Licence

MIT.
