# Card reading Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `euicc card info` reports a test eUICC's EID, versions and trusted certificate issuers, and says whether the card can work with our SGP.26 test material.

**Architecture:** A three-operation transport interface separates the ES10 command layer from what carries the bytes. A PC/SC implementation talks to a real reader; a replay implementation reads a committed text recording, so the identical command path runs in CI without hardware. The commands decode through the asn1c types the first half already generated.

**Tech Stack:** C99, PC/SC (`-framework PCSC` on Darwin, `libpcsclite` on Linux), the vendored mbedTLS and the generated RSP codec already in `euicc-rsp`.

## Global Constraints

- C99. The build passes `-Wall -Wextra -Werror=implicit-function-declaration -Werror=int-conversion` and `-D_DEFAULT_SOURCE`. Builds on Linux and Darwin.
- No interpreter in the build or test chain, and no `xxd`. Tests are C binaries plus POSIX `sh`. Both dependencies were removed at real cost; do not reintroduce either.
- Nothing installed system-wide except what a platform ships, with one agreed exception: Linux needs the `pcsc-lite` headers to build. macOS ships PC/SC.
- Failure convention inside the library, as `include/rsp.h` documents it: `0` done, `-1` a real negative answer, `-2` could not answer. At the CLI these become exit `0`, `1`, `2`.
- Secrets never printed, logged, or emitted. Use `mbedtls_platform_zeroize`, never `memset`, for anything secret; `mbedtls_ct_memcmp`, never `memcmp`, for anything an attacker can influence.
- Every test must be able to fail. Each task's last verification step includes a mutation: break one byte, watch the test go red, restore.
- Add every new test binary's source as `tests/test_<name>.c`; `TESTS` is derived by wildcard, so no Makefile edit is needed for tests. `make` does not relink test binaries — use `make check`.
- Commit messages: a lowercase type prefix, then what changed and why it matters, in full sentences. Check `git commit`'s own exit status, not a pipeline's, and confirm the object carries a `gpgsig` header.
- The repositories are `~/git/waigel/euicc-rsp` and `~/git/waigel/euicc-tools`, both public with signed commits. Push only when the task says to.

## Environment, already verified

- `asn1c` 0.9.29 at `/usr/local/bin/asn1c`, skeletons at `/usr/local/share/asn1c`.
- PC/SC on this Mac: headers at `<SDK>/System/Library/Frameworks/PCSC.framework/Headers`, included as `<PCSC/winscard.h>` and `<PCSC/wintypes.h>`, linked with `-framework PCSC`. A probe compiled, linked and ran: `SCardEstablishContext` returned 0.
- One reader is attached: `OMNIKEY AG Smart Card Reader USB`. No card was inserted at the time of writing; `SCardConnect` returned `0x80100066`.
- The generated codec already contains `EUICCInfo1`, `EUICCInfo2`, `GetEuiccDataRequest`, `GetEuiccDataResponse`, `ProfileInfoListRequest` and `ProfileInfoListResponse` in `dist/`.

## Command shapes, read from `rsp-2.5.asn`, not from memory

| Command | Request DER | Response type |
| --- | --- | --- |
| GetEUICCInfo2 | `BF 22 00` (`GetEuiccInfo2Request ::= [34] SEQUENCE {}`) | `EUICCInfo2`, tag `BF22` |
| GetEID | `BF 3E 03 5C 01 5A` (`GetEuiccDataRequest ::= [62] SEQUENCE { tagList [APPLICATION 28] Octet1 }`, value `5A`) | `GetEuiccDataResponse`, tag `BF3E`, `eidValue [APPLICATION 26] Octet16` |
| GetProfilesInfo | `BF 2D 00` (`ProfileInfoListRequest ::= [45] SEQUENCE { ... }`, every member OPTIONAL) | `ProfileInfoListResponse`, tag `BF2D` |

The ISD-R AID is `A0 00 00 05 59 10 10 FF FF FF FF 89 00 00 01 00`.

## File structure

**In `euicc-rsp`:**

| File | Responsibility |
| --- | --- |
| `include/rsp.h` | extended with the transport interface and the card functions |
| `src/rsp_transport.c` | the recording format: parse, write, and the replay transport |
| `src/rsp_pcsc.c` | the PC/SC transport |
| `src/rsp_es10.c` | APDU assembly, chaining both ways, the three commands |
| `tests/test_recording.c` | the recording parser and the replay transport |
| `tests/test_apdu.c` | chaining, both directions, against synthetic recordings |
| `tests/test_es10.c` | the commands end to end against a recording |
| `tests/test_card.c` | the real card; excluded from `check`, run by `check-card` |
| `testdata/cards/README.md` | what a recording holds and how to make one |
| `testdata/cards/omnikey-info.log` | the captured session, added in Task 5 |

**In `euicc-tools`:**

| File | Responsibility |
| --- | --- |
| `vendor/euicc-rsp` | new submodule |
| `src/card.c` | `euicc card info` and `euicc card profiles` |
| `src/main.c` | the `card` command and its flags |
| `Makefile` | build and link `euicc-rsp` |
| `tests/run-tests` | the CLI's exit contract without a reader |

---

### Task 1: The transport interface and the recording

**Files:**
- Modify: `~/git/waigel/euicc-rsp/include/rsp.h`
- Create: `~/git/waigel/euicc-rsp/src/rsp_transport.c`
- Create: `~/git/waigel/euicc-rsp/tests/test_recording.c`
- Create: `~/git/waigel/euicc-rsp/testdata/cards/README.md`

**Interfaces:**
- Consumes: nothing from this round.
- Produces, and every later task depends on these exact declarations:

```c
/* A transport carries APDUs and knows nothing else. */
typedef struct rsp_transport rsp_transport_t;
struct rsp_transport {
    /* Send one command APDU, receive one response APDU including its two
       status bytes. Returns the response length, -1 if the card answered
       something unusable, -2 if the exchange could not happen at all. */
    long (*transceive)(rsp_transport_t *t, const uint8_t *cmd, size_t cmd_len,
                       uint8_t *resp, size_t resp_cap);
    void (*close)(rsp_transport_t *t);
    void *ctx;
};

/* A transport that answers from a recording. Returns 0, or -2 if the file
   cannot be read or parsed. */
int rsp_replay_open(const char *path, rsp_transport_t *out);

/* Wrap any transport so every exchange is appended to `path`. The wrapper
   takes ownership of `inner` and must itself be closed. Returns 0 or -2. */
int rsp_record_open(rsp_transport_t *inner, const char *path,
                    rsp_transport_t *out);
```

- [ ] **Step 1: Write the failing test**

`tests/test_recording.c`. The suite's `ok()` helper is used by every existing
test; copy its shape from `tests/test_kdf.c`.

```c
/* A recording is a text file so it can be read in a review, diffed, and
   hand-edited into the failure cases a healthy card will not produce. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rsp.h"

static int fails = 0;
static void ok(const char *what, int cond) {
    printf("%s   %s\n", cond ? "ok  " : "FAIL", what);
    if(!cond) fails = 1;
}

static void write_file(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    if(f) { fputs(body, f); fclose(f); }
}

int main(void) {
    const char *path = "/tmp/rsp-test-recording.log";
    write_file(path,
        "# a recording, for the test\n"
        "> 00A4040002A000\n"
        "< 9000\n"
        "> BF2200\n"
        "< BF220102 9000\n");

    rsp_transport_t t;
    ok("a recording opens", rsp_replay_open(path, &t) == 0);

    uint8_t resp[64];
    const uint8_t sel[] = { 0x00,0xA4,0x04,0x00,0x02,0xA0,0x00 };
    long n = t.transceive(&t, sel, sizeof sel, resp, sizeof resp);
    ok("the first exchange returns its recorded answer",
       n == 2 && resp[0] == 0x90 && resp[1] == 0x00);

    /* Whitespace inside a hex line is insignificant: the writer groups the
       status bytes for readability and the reader must not care. */
    const uint8_t info[] = { 0xBF,0x22,0x00 };
    n = t.transceive(&t, info, sizeof info, resp, sizeof resp);
    ok("whitespace in the recorded hex is ignored",
       n == 5 && resp[0] == 0xBF && resp[3] == 0x90 && resp[4] == 0x00);

    /* The strict sequence is the point: a recording is a pin on the bytes,
       not a lenient stub. Sending something else must stop the run. */
    ok("an unexpected command is refused",
       t.transceive(&t, sel, sizeof sel, resp, sizeof resp) < 0);
    t.close(&t);

    /* Running past the end is a failure, not a silent empty answer. */
    ok("a short recording opens", rsp_replay_open(path, &t) == 0);
    t.transceive(&t, sel, sizeof sel, resp, sizeof resp);
    t.transceive(&t, info, sizeof info, resp, sizeof resp);
    ok("running past the end is refused",
       t.transceive(&t, info, sizeof info, resp, sizeof resp) < 0);
    t.close(&t);

    ok("a missing file is could-not-answer",
       rsp_replay_open("/tmp/rsp-no-such-recording", &t) == -2);

    /* Recording a replayed session must reproduce the same file, so the
       writer and the reader are proven inverse to each other. */
    rsp_transport_t inner, rec;
    ok("the inner transport opens", rsp_replay_open(path, &inner) == 0);
    ok("the recorder opens",
       rsp_record_open(&inner, "/tmp/rsp-test-rerecorded.log", &rec) == 0);
    rec.transceive(&rec, sel, sizeof sel, resp, sizeof resp);
    rec.transceive(&rec, info, sizeof info, resp, sizeof resp);
    rec.close(&rec);

    rsp_transport_t again;
    ok("the re-recorded file replays", rsp_replay_open("/tmp/rsp-test-rerecorded.log", &again) == 0);
    n = again.transceive(&again, sel, sizeof sel, resp, sizeof resp);
    ok("and answers the same", n == 2 && resp[0] == 0x90);
    again.close(&again);

    return fails ? 1 : 0;
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd ~/git/waigel/euicc-rsp && make check`
Expected: the build fails, `rsp_replay_open` undeclared.

- [ ] **Step 3: Declare the interface**

Append to `include/rsp.h`, before the closing `#endif`, the three
declarations from the Interfaces block above, each with the comment shown.

- [ ] **Step 4: Implement the recording**

`src/rsp_transport.c`. The format: a line beginning `> ` is a command in hex,
a line beginning `< ` is the response including its status bytes, `#` and
blank lines are ignored, and whitespace inside a hex line is insignificant.
Parse the whole file at open into an array of exchanges; a transport that
allocates while replaying is a transport that can fail for a reason unrelated
to the card.

`transceive` compares the command against the expected one and returns -1 when
they differ, with a message on `stderr` naming the index, the expected bytes
and the received ones. Running past the last exchange is also -1. Nothing here
is secret, so `stderr` is the right channel and no wiping is needed.

The recorder wraps an inner transport: it forwards the call, appends both
lines to the file, and returns what the inner returned. `close` closes the
file and then the inner transport.

- [ ] **Step 5: Run the test and watch it pass**

Run: `cd ~/git/waigel/euicc-rsp && make check`
Expected: all `test_recording` lines `ok`, and every earlier suite still green.

- [ ] **Step 6: Prove the test can fail**

Change the comparison in `transceive` to ignore the command entirely (always
return the next recorded response). Run `make check`: "an unexpected command
is refused" must go red. Restore it, and confirm green again. Record both
outputs in the report.

- [ ] **Step 7: Document the format and commit**

`testdata/cards/README.md` states what a recording is, that this round's
recordings hold only public data, and that a recording of a write session
would carry protected material and must not be pasted into an issue
unexamined.

```bash
cd ~/git/waigel/euicc-rsp
git add include/rsp.h src/rsp_transport.c tests/test_recording.c testdata/cards/README.md
git commit -m "feat: a transport interface, and recordings that pin the wire

The command layer should not know whether a reader or a file is
underneath it, so a transport is three operations and nothing else.
A recording is text, so it can be read in a review and hand-edited into
the failure cases a healthy card will not produce.

Replay expects the sequence strictly. That makes a committed recording
an absolute pin on the bytes on the wire rather than a lenient stub:
change the command order and it goes red."
```

---

### Task 2: APDU chaining, both directions

**Files:**
- Create: `~/git/waigel/euicc-rsp/src/rsp_es10.c`
- Create: `~/git/waigel/euicc-rsp/tests/test_apdu.c`
- Modify: `~/git/waigel/euicc-rsp/include/rsp.h`

**Interfaces:**
- Consumes: `rsp_transport_t` and `rsp_replay_open` from Task 1.
- Produces:

```c
/* Send one ES10 request to the ISD-R and collect the whole answer, driving
   command chaining outward and 61xx/GET RESPONSE inward. `req` is the DER of
   the request; `*out` is malloc'ed and belongs to the caller and holds the
   response without its status bytes. Returns 0; -1 when the card answered
   with a status other than 9000 or 61xx, with *sw set to that status; -2 when
   the exchange could not happen. */
int rsp_es10_send(rsp_transport_t *t, const uint8_t *req, size_t req_len,
                  uint8_t **out, size_t *out_len, unsigned *sw);
```

- [ ] **Step 1: Establish the chaining rules from the specification**

Before writing code, read SGP.22's ES10 clause and record in the fix report,
for each rule, the clause it came from: the CLA and INS of the `STORE DATA`
that carries an ES10 request, how P1 distinguishes an intermediate block from
the last one, what P2 counts, the maximum block size, and how a response
longer than one APDU is retrieved. A rule you cannot cite is a guess, and this
is the layer where a guess produces a card that simply stops answering.

Verification: the report names a clause for every one of those six rules.

- [ ] **Step 2: Write the failing test**

`tests/test_apdu.c`. It writes its own recordings, so the chaining logic is
tested before any real card exists. Use the block size and CLA/P1/P2 values
you established in Step 1 — the bytes below show the *shape* and must be
replaced with the ones the specification gives.

```c
/* Chaining is where card drivers fail, so it is tested in both directions
   before anything talks to hardware. These recordings are synthetic: they
   prove the logic, not the fidelity. Task 5 adds a real capture. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsp.h"

static int fails = 0;
static void ok(const char *what, int cond) {
    printf("%s   %s\n", cond ? "ok  " : "FAIL", what);
    if(!cond) fails = 1;
}

static void write_file(const char *path, const char *body) {
    FILE *f = fopen(path, "w"); if(f) { fputs(body, f); fclose(f); }
}

int main(void) {
    /* A short request and a short answer: no chaining either way. */
    const char *simple = "/tmp/rsp-apdu-simple.log";
    write_file(simple,
        "> 81E291000342220000\n"      /* replace with the real encoding */
        "< BF220102 9000\n");

    rsp_transport_t t;
    ok("the simple recording opens", rsp_replay_open(simple, &t) == 0);

    const uint8_t req[] = { 0xBF, 0x22, 0x00 };
    uint8_t *out = NULL; size_t out_len = 0; unsigned sw = 0;
    ok("a short exchange succeeds",
       rsp_es10_send(&t, req, sizeof req, &out, &out_len, &sw) == 0);
    ok("the status bytes are stripped from the answer", out_len == 4);
    ok("the answer is the response DER",
       out && out[0] == 0xBF && out[1] == 0x22);
    free(out); out = NULL;
    t.close(&t);

    /* A long answer: the card reports 61xx and the rest arrives through
       GET RESPONSE. The driver must join the parts and strip both statuses. */
    const char *chained = "/tmp/rsp-apdu-chained.log";
    write_file(chained,
        "> 81E291000342220000\n"
        "< BF22 06 010203 6103\n"     /* 61 03: three more bytes waiting */
        "> 00C0000003\n"
        "< 040506 9000\n");
    ok("the chained recording opens", rsp_replay_open(chained, &t) == 0);
    ok("a chained answer succeeds",
       rsp_es10_send(&t, req, sizeof req, &out, &out_len, &sw) == 0);
    ok("the parts are joined in order",
       out_len == 8 && out[5] == 0x03 && out[6] == 0x04 && out[7] == 0x06);
    free(out); out = NULL;
    t.close(&t);

    /* A status the driver cannot use is a real negative answer, and the
       caller must be able to see which one it was. */
    const char *refused = "/tmp/rsp-apdu-refused.log";
    write_file(refused,
        "> 81E291000342220000\n"
        "< 6A82\n");
    ok("the refusal recording opens", rsp_replay_open(refused, &t) == 0);
    ok("a refusal is a real negative answer",
       rsp_es10_send(&t, req, sizeof req, &out, &out_len, &sw) == -1);
    ok("and the status word is reported", sw == 0x6A82);
    ok("and nothing is handed back", out == NULL);
    t.close(&t);

    /* A truncated chain: the card promises more and the exchange ends.
       This must fail, not return the part that did arrive. */
    const char *truncated = "/tmp/rsp-apdu-truncated.log";
    write_file(truncated,
        "> 81E291000342220000\n"
        "< BF22 06 010203 6103\n");
    ok("the truncated recording opens", rsp_replay_open(truncated, &t) == 0);
    ok("a truncated chain fails rather than returning a fragment",
       rsp_es10_send(&t, req, sizeof req, &out, &out_len, &sw) < 0);
    ok("and still hands nothing back", out == NULL);
    t.close(&t);

    return fails ? 1 : 0;
}
```

- [ ] **Step 3: Run it and watch it fail**

Run: `cd ~/git/waigel/euicc-rsp && make check`
Expected: the build fails, `rsp_es10_send` undeclared.

- [ ] **Step 4: Implement the chaining**

`src/rsp_es10.c`. Split a request longer than the block size into blocks and
send each with the P1 the specification gives for "more follow" and for
"last". Collect the response: while the status is `61xx`, issue
`00 C0 00 00 xx` and append. On `9000`, return what accumulated. On anything
else, free what accumulated, set `*sw`, leave `*out` NULL and return -1. A
`61xx` that is never followed by data is -1 as well: the caller must never
receive a fragment that looks whole.

`*out` is `malloc`'ed and grows; on every failure path it is freed and set to
NULL, so a caller that forgets to check cannot read a stale pointer.

- [ ] **Step 5: Run the test and watch it pass**

Run: `cd ~/git/waigel/euicc-rsp && make check`
Expected: all `test_apdu` lines `ok`, every earlier suite still green.

- [ ] **Step 6: Prove the test can fail**

Two mutations, each run with `make check` and then restored:
1. Return the accumulated bytes on a non-9000 status instead of freeing them.
   "a refusal is a real negative answer" must go red.
2. Treat a `61xx` with no following exchange as success. "a truncated chain
   fails rather than returning a fragment" must go red.

- [ ] **Step 7: Commit**

```bash
cd ~/git/waigel/euicc-rsp
git add include/rsp.h src/rsp_es10.c tests/test_apdu.c
git commit -m "feat: ES10 request and response chaining, proven without a card

Long requests go out in blocks and long answers come back through
61xx and GET RESPONSE. This is where card drivers usually fail, so it
is tested against hand-written recordings before any hardware exists:
a short exchange, a chained answer, a refusal, and a chain that stops
half way.

The last of those matters most. A truncated chain returns a failure and
frees what arrived, because a fragment that looks like a whole answer
is the error a caller cannot see."
```

---

### Task 3: The three commands

**Files:**
- Create: `~/git/waigel/euicc-rsp/tests/test_es10.c`
- Modify: `~/git/waigel/euicc-rsp/src/rsp_es10.c`
- Modify: `~/git/waigel/euicc-rsp/include/rsp.h`

**Interfaces:**
- Consumes: `rsp_es10_send` from Task 2, the generated types in `dist/`.
- Produces:

```c
/* What a card says about itself. Strings are NUL-terminated; ci_ids holds
   ci_count identifiers of ci_id_len bytes each, concatenated. */
typedef struct {
    uint8_t eid[16];
    int     have_eid;
    char    svn[16];            /* "2.2.0" */
    uint8_t *ci_ids;            /* for verification */
    size_t   ci_count;
    size_t   ci_id_len;
} rsp_card_info_t;

/* Select the ISD-R, then read EUICCInfo2 and the EID. Returns 0, -1 if the
   card refused, -2 if it could not be asked. */
int rsp_card_read_info(rsp_transport_t *t, rsp_card_info_t *out);
void rsp_card_info_free(rsp_card_info_t *i);

/* Does this card accept the issuer whose SubjectKeyIdentifier is `id`?
   Returns 1 for yes, 0 for no. */
int rsp_card_trusts(const rsp_card_info_t *i, const uint8_t *id, size_t id_len);
```

- [ ] **Step 1: Write the failing test**

`tests/test_es10.c`. It needs a recording whose EUICCInfo2 is real DER, so
build one: encode an `EUICCInfo2` with the generated encoder in a throwaway
program, print it as hex, and paste that into the recording with a comment
saying how it was produced. A hand-invented DER blob that the decoder happens
to accept proves nothing about the decoder.

```c
/* The commands, end to end against a recording: select, EUICCInfo2, EID.
   The response DER here was produced with the generated encoder, not written
   by hand -- see testdata/cards/README.md for the command that makes it. */
#include <stdio.h>
#include <string.h>
#include "rsp.h"

static int fails = 0;
static void ok(const char *what, int cond) {
    printf("%s   %s\n", cond ? "ok  " : "FAIL", what);
    if(!cond) fails = 1;
}

int main(void) {
    rsp_transport_t t;
    ok("the recording opens",
       rsp_replay_open("testdata/cards/synthetic-info.log", &t) == 0);

    rsp_card_info_t info;
    memset(&info, 0, sizeof info);
    ok("the card is read", rsp_card_read_info(&t, &info) == 0);
    ok("the EID arrived", info.have_eid);
    ok("the version is parsed", strcmp(info.svn, "2.2.0") == 0);
    ok("at least one issuer is listed", info.ci_count >= 1);

    /* The identifier the recording carries must be recognised, and one that
       differs in a single byte must not be. */
    uint8_t known[20]; memcpy(known, info.ci_ids, info.ci_id_len);
    ok("a listed issuer is trusted",
       rsp_card_trusts(&info, known, info.ci_id_len) == 1);
    known[0] ^= 0xFF;
    ok("an unlisted issuer is not trusted",
       rsp_card_trusts(&info, known, info.ci_id_len) == 0);

    rsp_card_info_free(&info);
    t.close(&t);
    return fails ? 1 : 0;
}
```

- [ ] **Step 2: Build the synthetic recording**

Write a throwaway program under `/tmp` that fills an `EUICCInfo2` and a
`GetEuiccDataResponse` with the generated types, encodes each with
`der_encode`, and prints them as hex. Assemble
`testdata/cards/synthetic-info.log` from the output: the ISD-R SELECT and its
`9000`, then the EUICCInfo2 request and its response, then the EID request and
its response. Put the generating program's source in
`testdata/cards/README.md` so the file can be regenerated rather than trusted.

Verification: `rsp_replay_open` on the new file returns 0, checked by the test
in Step 1 once the code exists.

- [ ] **Step 3: Run it and watch it fail**

Run: `cd ~/git/waigel/euicc-rsp && make check`
Expected: the build fails, `rsp_card_read_info` undeclared.

- [ ] **Step 4: Implement the commands**

Add to `src/rsp_es10.c`: `rsp_card_select_isdr` sending the SELECT with the
AID above; `rsp_card_read_info` calling it, then `rsp_es10_send` with
`BF 22 00` and with `BF 3E 03 5C 01 5A`, decoding each with `ber_decode` and
the generated descriptors; `rsp_card_trusts` comparing with plain `memcmp`,
since both operands are public identifiers and no secret is involved — say so
in a comment, because the project's rule is otherwise `mbedtls_ct_memcmp`.

`svn` is a three-byte `VersionType`; format it as `"%u.%u.%u"`.

Free every decoded structure with `ASN_STRUCT_FREE` on every path.

- [ ] **Step 5: Run the test and watch it pass**

Run: `cd ~/git/waigel/euicc-rsp && make check`
Expected: all `test_es10` lines `ok`, every earlier suite still green.

- [ ] **Step 6: Prove the test can fail**

Change `rsp_card_trusts` to return 1 unconditionally. `make check`: "an
unlisted issuer is not trusted" must go red. Restore and confirm green.

- [ ] **Step 7: Commit**

```bash
cd ~/git/waigel/euicc-rsp
git add include/rsp.h src/rsp_es10.c tests/test_es10.c testdata/cards/
git commit -m "feat: read a card's EID, versions and trusted issuers

The three read-only commands, decoded through the same generated types
the rest of the library uses. rsp_card_trusts answers the question this
whole round exists for: does this card accept the issuer our test
credentials chain to?

The recording they are tested against was produced with the generated
encoder rather than written by hand, and the program that made it is in
testdata/cards/README.md, so the fixture can be regenerated instead of
trusted."
```

---

### Task 4: The PC/SC transport

**Files:**
- Create: `~/git/waigel/euicc-rsp/src/rsp_pcsc.c`
- Create: `~/git/waigel/euicc-rsp/tests/test_card.c`
- Modify: `~/git/waigel/euicc-rsp/Makefile`
- Modify: `~/git/waigel/euicc-rsp/include/rsp.h`
- Modify: `~/git/waigel/euicc-rsp/.github/workflows/ci.yml`

**Interfaces:**
- Consumes: `rsp_transport_t` from Task 1.
- Produces:

```c
/* Connect to a reader. `reader` names one, or is NULL to take the only one
   attached. Returns 0; -2 with a message on stderr when there is no reader,
   no card, or the card is held by another process. */
int rsp_pcsc_open(const char *reader, rsp_transport_t *out);

/* The attached readers, NUL-separated and terminated by an empty string.
   The caller frees. Returns the count, or -2. */
long rsp_pcsc_readers(char **out);
```

- [ ] **Step 1: Make the build find PC/SC**

In `Makefile`, add a platform branch beside the existing Darwin one:

```make
# PC/SC is the system smartcard library. macOS ships it as a framework;
# Linux needs the pcsc-lite headers, the one dependency this project asks a
# Linux user to install.
ifeq ($(shell uname -s),Darwin)
PCSC_LIBS := -framework PCSC
else
PCSC_CFLAGS := $(shell pkg-config --cflags libpcsclite 2>/dev/null)
PCSC_LIBS   := $(shell pkg-config --libs libpcsclite 2>/dev/null || echo -lpcsclite)
endif
```

Add `$(PCSC_CFLAGS)` to the compile flags and `$(PCSC_LIBS)` to every link
line that includes `src/rsp_pcsc.o`. Add `libpcsclite-dev` to the package list
in `.github/workflows/ci.yml`.

Verification: `make clean && make` succeeds, and `nm librsp.a | grep -c SCard`
is non-zero.

- [ ] **Step 2: Write the card test**

`tests/test_card.c` runs only with hardware. It must refuse to be counted as
a pass when no reader is present, and must not be part of `check`.

```c
/* The real card. Not part of `make check`: it needs a reader, and a test
   that silently passes when the hardware is absent is worse than no test.
   Run it with `make check-card`. */
#include <stdio.h>
#include <string.h>
#include "rsp.h"

int main(void) {
    char *readers = NULL;
    long n = rsp_pcsc_readers(&readers);
    if(n <= 0) {
        fprintf(stderr, "no reader attached; this test needs one\n");
        return 2;
    }
    printf("readers:\n");
    for(const char *p = readers; *p; p += strlen(p) + 1) printf("  %s\n", p);

    rsp_transport_t t;
    int rc = rsp_pcsc_open(NULL, &t);
    if(rc != 0) { fprintf(stderr, "cannot open the card: %d\n", rc); return 2; }

    rsp_card_info_t info;
    memset(&info, 0, sizeof info);
    rc = rsp_card_read_info(&t, &info);
    if(rc != 0) { fprintf(stderr, "cannot read the card: %d\n", rc); t.close(&t); return rc == -1 ? 1 : 2; }

    printf("ok   the card answered\n");
    printf("     EID  ");
    for(int i = 0; i < 16; i++) printf("%02X", info.eid[i]);
    printf("\n     SVN  %s\n     issuers %zu\n", info.svn, info.ci_count);

    rsp_card_info_free(&info);
    t.close(&t);
    return 0;
}
```

- [ ] **Step 3: Add the check-card target**

In `Makefile`, beside `check`:

```make
# The real card. Excluded from `check` on purpose: CI has no reader, and a
# hardware test that passes when the hardware is missing proves nothing.
check-card: tests/run-card
	./tests/run-card

tests/run-card: tests/test_card.c $(LIB) $(MBED_LIBS) $(DIST)/.stamp
	$(CC) $(ALL_CFLAGS) $(GEN_INC) $< $(LIB) $(DIST)/*.o $(MBED_LIBS) $(PCSC_LIBS) -o $@
	@rm -rf $@.dSYM
```

`TESTS` is derived from `tests/test_*.c` by wildcard, which would sweep
`test_card.c` into `check`. Exclude it explicitly:

```make
TESTS := $(filter-out tests/run-card,$(patsubst tests/test_%.c,tests/run-%,$(wildcard tests/test_*.c)))
```

Verification: `make check` does not build or run `tests/run-card`;
`make check-card` does.

- [ ] **Step 4: Implement the transport**

`src/rsp_pcsc.c`, including `<PCSC/winscard.h>` and `<PCSC/wintypes.h>` on
Darwin and `<winscard.h>` on Linux, selected with the same `uname` branch the
Makefile uses, via a small `#if defined(__APPLE__)`.

`rsp_pcsc_readers` calls `SCardListReaders` twice, once for the length.
`rsp_pcsc_open` establishes a context, connects with
`SCARD_PROTOCOL_T0|SCARD_PROTOCOL_T1`, and stores the handle and the active
protocol in `ctx`. `transceive` calls `SCardTransmit` with the matching
`SCARD_PCI_T0` or `SCARD_PCI_T1`.

Every failure returns -2 and prints a sentence, not a number. Three deserve
their own message because they are the ones a person actually hits:

- no reader: say that none is attached.
- no card: say the reader is there but empty.
- `SCARD_E_SHARING_VIOLATION`: say another process holds the card, and that on
  macOS this is usually the system's own card services.

- [ ] **Step 5: Run both suites**

Run: `cd ~/git/waigel/euicc-rsp && make check`
Expected: 112 plus this round's new lines, all `ok`, and no `run-card` among
them.

Run: `make check-card` with the reader attached and no card inserted.
Expected: exit 2 and the sentence about an empty reader — not a crash and not
a pass.

- [ ] **Step 6: Prove the messages are reachable**

With the reader attached and no card, `make check-card` must print the
empty-reader sentence. Unplug the reader and run it again: it must print the
no-reader sentence and exit 2. Record both in the report. This is the closest
thing to a mutation test the hardware paths allow, and it is worth doing by
hand once.

- [ ] **Step 7: Commit**

```bash
cd ~/git/waigel/euicc-rsp
git add include/rsp.h src/rsp_pcsc.c tests/test_card.c Makefile .github/workflows/ci.yml
git commit -m "feat: talk to a real reader over PC/SC

The transport that carries the bytes to actual hardware, behind the
same three-operation interface the replay transport implements, so the
command layer cannot tell them apart.

check-card is deliberately outside check: CI has no reader, and a
hardware test that passes when the hardware is absent proves nothing.
The three failures a person actually hits -- no reader, no card, and
another process holding the card -- each say what happened in a
sentence instead of returning a number."
```

---

### Task 5: The first card session

**Files:**
- Create: `~/git/waigel/euicc-rsp/testdata/cards/omnikey-info.log`
- Modify: `~/git/waigel/euicc-rsp/tests/test_es10.c`
- Modify: `~/git/waigel/euicc-rsp/testdata/cards/README.md`

**Interfaces:** consumes everything above; produces the recording that later
rounds replay.

This task needs the card physically inserted. If it is not available, stop and
report — do not fabricate a recording.

- [ ] **Step 1: Read the real card**

With the test eUICC in the OMNIKEY reader:

```bash
cd ~/git/waigel/euicc-rsp && make check-card
```

Expected: the EID, the version and the issuer count. Record the full output.

- [ ] **Step 2: Capture the exchange**

Extend `tests/test_card.c` with an optional first argument: when given, wrap
the PC/SC transport in `rsp_record_open` with that path before reading. Then:

```bash
./tests/run-card testdata/cards/omnikey-info.log
```

Verification: the file exists, begins with the ISD-R SELECT, and every line
matches `^[<>] [0-9A-F ]+$` or is a comment.

- [ ] **Step 3: Answer the question this round exists for**

Compare the identifiers the card listed against the SubjectKeyIdentifier of
our test CI:

```bash
openssl x509 -in testdata/sgp26/ci.der -inform DER -noout -text | grep -A1 "Subject Key Identifier"
```

Record in the report whether the card's list contains it. **If it does not,
stop and report that** — the write round's design depends on this answer, and
a card that trusts a different issuer changes it.

- [ ] **Step 4: Make the recording the test's fixture**

Point `tests/test_es10.c` at `testdata/cards/omnikey-info.log` instead of the
synthetic file, and adjust the expected EID, version and issuer count to what
the real card returned. Keep the synthetic recording and a test that uses it:
the synthetic one exercises shapes the real card happens not to produce.

- [ ] **Step 5: Run both suites and prove the pin bites**

Run: `make check` — the real recording now drives `test_es10`.

Then change one byte of a command in `src/rsp_es10.c` (for instance the second
byte of the EID request's tagList) and run `make check`: replay must refuse,
naming the expected and received APDUs. Restore and confirm green.

- [ ] **Step 6: Commit**

```bash
cd ~/git/waigel/euicc-rsp
git add testdata/cards/omnikey-info.log testdata/cards/README.md tests/test_es10.c tests/test_card.c
git commit -m "test: a real card's answers, captured and pinned

The recording from the first session with an actual eUICC. From here
the command path runs in CI byte for byte as the card answered it, and
a change to any request turns replay red.

It also settles the question the design has carried since the start:
which certificate issuers this card accepts."
```

---

### Task 6: euicc card info

**Files:**
- Create: `~/git/waigel/euicc-tools/src/card.c`
- Modify: `~/git/waigel/euicc-tools/src/main.c`
- Modify: `~/git/waigel/euicc-tools/Makefile`
- Modify: `~/git/waigel/euicc-tools/.gitmodules`
- Modify: `~/git/waigel/euicc-tools/tests/run-tests`
- Modify: `~/git/waigel/euicc-rsp/include/rsp.h` (the rename below)

**Interfaces:** consumes `rsp_pcsc_open`, `rsp_card_read_info`,
`rsp_card_trusts`, `rsp_replay_open`, `rsp_record_open`.

- [ ] **Step 1: Add the submodule and link it**

```bash
cd ~/git/waigel/euicc-tools
git submodule add https://github.com/waigel/euicc-rsp.git vendor/euicc-rsp
git -C vendor/euicc-rsp submodule update --init --recursive
```

Note the ordering trap that bit this project once already: after adding a
submodule, check out the intended commit inside it, `git add` the gitlink, and
only then run `git submodule update` — otherwise the update resets the
submodule to the gitlink recorded at add time.

In `Makefile`, build `euicc-rsp`'s library and link it, following the shape
already used for `vendor/euicc-schema`.

Verification: `make` produces `euicc`, and `nm euicc | grep -c rsp_card_read_info`
is non-zero.

- [ ] **Step 2: Settle the deferred rename**

In `euicc-rsp`, rename `rsp_verify` to `rsp_sign_verify` — beside
`rsp_pki_verify` the short name is ambiguous, and this is the last moment
before anything depends on it. Update `src/rsp_sign.c`, `include/rsp.h` and
`tests/test_sign.c`. Commit it in `euicc-rsp` and move the submodule pointer
in `euicc-tools` to that commit.

Verification: `make check` in `euicc-rsp` gives its full count with no
`rsp_verify` left: `grep -rn '\brsp_verify\b' src include tests` is empty.

- [ ] **Step 3: Write the failing CLI test**

In `euicc-tools/tests/run-tests`, beside the existing cases:

```sh
# card: without a reader the answer is "could not answer", never a verdict.
# This runs in CI, where there is certainly no reader, so it pins the
# distinction the exit contract exists to make.
out=$("$E" card info 2>&1); rc=$?
[ $rc -eq 2 ] && ok "card info without a reader is exit 2" \
              || bad "card info without a reader is exit 2 (got $rc)"
echo "$out" | grep -qi "reader" \
  && ok "card info names the missing reader" \
  || bad "card info names the missing reader"

# A recording stands in for a card, so the happy path is testable in CI.
"$E" card info --replay "$root/vendor/euicc-rsp/testdata/cards/omnikey-info.log" >/dev/null 2>&1 \
  && ok "card info reads a recording" || bad "card info reads a recording"

"$E" card info --replay "$root/vendor/euicc-rsp/testdata/cards/omnikey-info.log" --json 2>/dev/null \
  | grep -q '"eid"' && ok "card info --json names the eid" \
                    || bad "card info --json names the eid"
```

- [ ] **Step 4: Run it and watch it fail**

Run: `cd ~/git/waigel/euicc-tools && make check`
Expected: the three new lines FAIL, `card` being an unknown command.

- [ ] **Step 5: Implement the command**

`src/card.c` with `cmd_card_info` and `cmd_card_profiles`. Flags: `--reader`,
`--json`, `--record FILE`, and `--replay FILE` which substitutes a recording
for a reader — that last one is what makes the command testable in CI and is
worth having in its own right.

Human output names the EID, the version, each issuer identifier, and a final
line saying whether our test CI is among them. `--json` emits one object with
`eid`, `svn`, `ci_ids` and `trusts_test_ci`.

The exit contract: `0` when the card answered and trusts our test CI, `1` when
it answered and does not, `2` when it could not be asked. Document that
reading in `usage()`, because it is a stronger claim than "the command ran".

Wire `card` into `main.c` beside the existing commands.

- [ ] **Step 6: Run the tests and watch them pass**

Run: `cd ~/git/waigel/euicc-tools && make check`
Expected: every existing line still `ok` plus the three new ones.

- [ ] **Step 7: Prove the exit contract can fail**

Point `--replay` at a recording whose issuer list you have edited to remove
our CI identifier, and confirm the command exits 1 rather than 0, with the
human output saying so. Record it. Then check `euicc card info` against the
real reader and confirm the same verdict the library reported in Task 5.

- [ ] **Step 8: Commit and push both repositories**

```bash
cd ~/git/waigel/euicc-rsp && git push
cd ~/git/waigel/euicc-tools
git add .gitmodules vendor/euicc-rsp src/card.c src/main.c Makefile tests/run-tests
git commit -m "feat: euicc card info, and the first link against euicc-rsp

The command that asks a card what it is and whether it will work with
our test credentials. --replay substitutes a recording for a reader, so
the happy path is covered in CI and not only on a desk with hardware.

The exit code carries the verdict rather than just the outcome: 0 means
this card can work with our material, 1 means it answered and cannot,
2 means it could not be asked."
git push
```

---

## Self-review

**Spec coverage.** Transport interface, PC/SC, replay and the recorder: Tasks 1
and 4. Recording format and its strictness: Task 1. The three read-only
commands: Task 3. Chaining both ways: Task 2. The CLI, its flags, `--json` and
the exit-code reading: Task 6. The submodule and the deferred rename: Task 6.
The Linux `pcsc-lite` dependency and CI: Task 4. The real card session and the
trust-anchor answer: Task 5. The error messages for no reader, no card and a
held card: Task 4. Hand-edited recordings for the failure paths: Tasks 2 and 6.
`make check-card` outside `check`: Task 4.

One spec item has no task and is deliberate: `card profiles` is declared in
Task 6's `src/card.c` but its own test is not spelled out, because
`ProfileInfoListResponse` decoding is the same shape as `EUICCInfo2` and the
recording from Task 5 will contain whatever that card holds. If the reviewer
disagrees it should be its own task, split it.

**Placeholder scan.** The one deliberate gap is Task 2 Step 2's APDU bytes,
which are marked as shape-only and must be replaced with the values Step 1
establishes from the specification. That is a citation requirement, not a
placeholder: the plan cannot supply bytes it has not verified, and inventing
them is exactly the failure this project has been avoiding.

**Type consistency.** `rsp_transport_t`, `transceive` returning `long`,
`rsp_replay_open`, `rsp_record_open`, `rsp_es10_send`, `rsp_card_info_t`,
`rsp_card_read_info`, `rsp_card_info_free`, `rsp_card_trusts`,
`rsp_pcsc_open`, `rsp_pcsc_readers` are spelled identically everywhere they
appear. `-1` is a real negative answer and `-2` is could-not-answer
throughout, matching the convention the first half established.
