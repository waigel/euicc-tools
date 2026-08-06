# euicc-rsp, the card side: reading

Status: agreed 2026-08-06. Not started.

Extends [the first design](2026-08-05-euicc-rsp-design.md), whose steps 4 to 6
describe the card work. This covers the reading half of step 4 and the part of
step 5 that goes with it. Writing to a card is a later round.

## The goal

`euicc card info` reports what a test eUICC in a reader says about itself: its
EID, the specification versions it implements, and the certificate issuers it
trusts. `euicc card profiles` lists what is already installed.

The first of those answers the question this project has carried unverified
since its design: **does the card trust the GSMA SGP.26 test CI?** Every later
step assumes it does. Nothing has asked the card.

## Why reading first

Writing to a card is the first action in this project that leaves something
behind. A load that breaks off can leave an orphaned ISD-P.

Reading touches the same transport, the same command layer and the same
codec as writing, so it proves nearly all of the machinery. It cannot damage
anything, and it answers the trust-anchor question before the write path is
designed rather than after. What the real card teaches here goes into that
design instead of being guessed at.

## What the card is

A removable card in a PC/SC reader. Not a chip soldered into a modem — that
would need AT or QMI rather than PC/SC and a different transport entirely.
Should the hardware change, the transport interface below is the seam where
that lands.

## Architecture

The work splits across the two repositories along the boundary the first
design drew. The library goes into `euicc-rsp`, which already owns both the
SM-DP+ role and the card transport. The commands go into `euicc-tools`, which
gains `euicc-rsp` as a submodule — the first time anything links against it,
and the same arrangement `euicc-tools` already has with `euicc-schema`.

| Component | Purpose | Knows nothing about |
| --- | --- | --- |
| `rsp_transport` | Three operations: connect, exchange one APDU, disconnect | commands, ES10, the card |
| `rsp_pcsc` | That interface against a real reader | ES10, command assembly |
| `rsp_replay` | The same interface fed from a recording, plus the wrapper that records any transport | that it is not real |
| `rsp_es10` | The commands: build the APDU, drive chaining both ways, reassemble the answer, decode it through `rsp_codec` | which transport is underneath |

The interface in the middle carries the design. `rsp_es10` sees only "send
these bytes, give me those back", so the code that talks to the reader is the
same code CI runs without one.

The commands in this round are read-only:

- `GetEUICCInfo1` and `GetEUICCInfo2` — versions and the accepted CI key
  identifiers. This is the open question.
- `GetEID` — the EID, which is also the value `hostId` currently fills with
  the ICCID as a placeholder.
- `GetProfilesInfo` — what is installed.

The delicate part is the APDU layer, not the reader. ES10 commands travel to
the ISD-R as `STORE DATA`; long commands are chained into blocks, and long
answers arrive as `61xx` with a following `GET RESPONSE`. That chaining in
both directions is where card drivers usually fail, so it is its own unit with
its own tests.

### The dependency this adds

The project's rule is that nothing is installed beyond what a platform ships.
macOS ships PC/SC. Linux does not: building needs the `pcsc-lite` headers,
which is one line in the CI package list and one package for a Linux user.
That is proportionate — it is the system library for smartcards, not a
toolchain zoo. The alternative, declaring the handful of functions ourselves
and loading the library at runtime, is a trick that helps nobody later.

## The flow

`euicc card info`:

1. Open the PC/SC context, list readers, connect. `--reader` picks among
   several; exactly one is taken without asking; none is exit 2 naming the
   problem.
2. Select the ISD-R by AID.
3. Send `GetEUICCInfo2` as `STORE DATA`, drive the chaining.
4. Decode through `rsp_codec`, using the generated types the first half
   already built.
5. Fetch `GetEID`.
6. Report the EID, the versions, the accepted CI key identifiers, and the
   comparison against our test CI as its own field, so the answer does not
   depend on a person comparing hex by eye.

## The recording

Text, one line per direction:

```
> 00A4040010A0000005591010FFFFFFFF8900000100
< 9000
> 81E2910006BF3E035C0100
< BF3E1F5A0A98001032547698103214...9000
```

Greppable, diffable, readable in a review, and parsed without an interpreter —
the same shape as `testdata/nist/ecdh-p256.txt`.

Replay expects the sequence strictly. If the code sends an APDU other than the
one recorded, it stops and reports which was expected and which arrived. The
recording is therefore not a stub but an **absolute pin on the bytes on the
wire** — the kind of assertion whose absence cost this project five separate
findings in the first half. Change the command order and it goes red.

The file carries a header stating what it holds. This round records only
public data — EID, versions, key identifiers — but a recording of a write
session would carry protected material, and nobody should paste one into an
issue without knowing that.

## The commands

```sh
euicc card info                      # EID, versions, CI key ids, verdict
euicc card info --json               # the same as one object
euicc card info --reader "Name"      # when several are attached
euicc card info --record card.log    # capture the exchange
euicc card profiles                  # what is already installed
```

The exit contract gets a useful reading here. **0 means this card can work
with our material.** 1 means the card answered and does not know our test CI —
a real negative answer to the question being asked. 2 means no reader, no
card, or no ISD-R. That makes `euicc card info` usable from a script rather
than only pleasant to read.

`--record` sits on the ordinary command rather than in a developer tool: a
person with a problem sends the recording, and it becomes a test case. The
same reasoning put the external OpenSSL vector into the suite instead of
leaving it in a scratch probe.

This round is the first time `euicc-tools` links against `euicc-rsp`, so it is
also when the deferred naming question is settled: `rsp_verify` becomes
`rsp_sign_verify`, which is ambiguous today beside `rsp_pki_verify`. The
`int`-versus-`long` return conventions stay as they are; a length-returning
function keeps `long`.

## Failures

Cards speak in status words. A tool that prints `6A82` has failed its user, so
each one that this code can provoke gets named.

Where the failure happened matters more, because three places are easy to
confuse:

- **No reader, reader busy, no card** — exit 2, with the fix in the message.
  The most common case in practice is `SCARD_E_SHARING_VIOLATION`: another
  process holds the card, often the operating system's own card service. That
  one is named explicitly, because searching for it blind costs an afternoon.
- **A card, but no ISD-R** — also exit 2, saying what it means: this may not
  be an eUICC at all, or it is locked.
- **The ISD-R answered and refused** — exit 1. The question was asked and the
  answer is no.

Nothing is written this round, so nothing is left to clean up. The transport
disconnects cleanly on every path regardless.

## Tests

| Level | What it proves | In CI |
| --- | --- | --- |
| Unit | Chaining both ways, status word decoding, the recording parser | yes |
| Replay | The whole ES10 path against the committed recording, byte-exact | yes |
| Mutation | One byte changed in a command turns replay red | yes |
| Real card | `make check-card`, refuses to run without a reader | no |

The card comes first and the recording falls out of the same session: build
the transport, run it against the reader, capture while doing so, commit the
capture. Until then the only evidence is one desk; afterwards CI has a net.

The error paths are exercised from hand-edited copies of the recording — a
truncated answer, a `61xx` that never resolves, an unexpected status word in
the middle of a chain. None of those can be provoked from a healthy card. In a
text file they are three lines. That is the reason for the format.

## What this round does not do

No writing, so no `euicc flash`. The `'87'` and `'88'` protection, the real
`transactionId`, the EID as `hostId` and a real `smdpSign` all belong to the
write round, which this one is meant to inform.

## Open points

- The reader model is not yet known. PC/SC abstracts it, so the design does
  not depend on it, but a first card session is easier when it is a common
  one.
- Whether the card trusts the GSMA test CI is the question this round exists
  to answer. If it does not, the write round changes shape, and the answer
  arrives before that design is written.
