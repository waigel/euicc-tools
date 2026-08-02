# euicc-tools

`euicc` builds and checks eUICC profile packages. It is one binary. It needs no
Python, no separate codec, and no schema on the side.

```sh
euicc build profile.vn -o profile.der   # value notation in, DER out
euicc show  profile.der                 # DER in, value notation out
euicc check profile.der                 # a verdict
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
```

## What it holds

| Submodule | Supplies |
| --- | --- |
| `vendor/euicc-profile-tool` | the schema, asn1c, and asn1c-vn |
| `vendor/saip-validator` | the 112 rules, as ISO Schematron |

The rules are not copied here. They stay where they are written and are read
from the submodule, so a correction there needs no change in this repository.

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
