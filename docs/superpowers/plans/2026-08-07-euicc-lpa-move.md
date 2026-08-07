# euicc-lpa: the move (Plan A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create `euicc-lpa`, move the transport, ES10 and card commands into it, rewire `euicc-tools` to depend on it, and add the guard the Makefile already claims to have.

**Architecture:** Three repositories in a chain — `euicc-tools → euicc-lpa → euicc-rsp`. `euicc-lpa` links against `euicc-rsp`'s `librsp.a` and reuses its generated codec rather than producing a second one. Nothing changes behaviour: this plan moves code and rewires builds, and its whole value is that the same tests still pass afterwards.

**Tech Stack:** C99, GNU make, mbedTLS (via euicc-rsp's submodule), asn1c 0.9.29 generated codec (via euicc-rsp's `dist/`), PC/SC, POSIX sh for test runners.

## Global Constraints

- Spec: [2026-08-07-euicc-write-round-design.md](../specs/2026-08-07-euicc-write-round-design.md). This plan is its Plan A only.
- **No behaviour changes.** Moved code is moved, not improved. If you spot a bug in moved code, note it and leave it; it belongs to a later plan.
- **No test is deleted.** The total across all repositories must not fall below **334** (euicc-rsp 235 + euicc-tools 99).
- C99: `-std=c99 -Wall -Wextra -Wno-unused-parameter -Werror=implicit-function-declaration -Werror=int-conversion`.
- `-D_DEFAULT_SOURCE`, plus `-D_DARWIN_C_SOURCE` on Darwin. Omitting the first is what caused a Linux-only segfault in this project's history.
- No Python anywhere in the build or test chain.
- No `xxd` — `ubuntu-latest` does not ship it.
- `.DELETE_ON_ERROR:` in every Makefile that writes a generated file.
- Commits are signed. A failed signed commit is reported, never worked around.
- The `rsp_` prefix is kept on moved symbols. It stands for Remote SIM Provisioning — the whole of SGP.22, not the SM-DP+ role — so it is correct in both repositories. Renaming during a move would bury real mistakes in mechanical churn.

---

## File Structure

**New repository `euicc-lpa`** (sibling of the others, at `~/git/waigel/euicc-lpa`):

| Path | Responsibility |
|---|---|
| `include/lpa.h` | the public contract: transport, ES10, card reads |
| `src/lpa_version.c` | `lpa_version()`, one line, so the floor test has something to pin |
| `src/rsp_transport.c` | moved verbatim: replay and record transports |
| `src/rsp_pcsc.c` | moved verbatim: the PC/SC transport |
| `src/rsp_es10.c` | moved verbatim: ES10 framing, card reads |
| `tests/test_link.c` | the floor: does this library link and reach both dependencies |
| `tests/test_apdu.c`, `test_card.c`, `test_es10.c`, `test_profiles.c`, `test_recording.c` | moved verbatim |
| `testdata/cards/` | moved verbatim, all seven recordings and the README |
| `Makefile`, `tests/run-tests`, `.github/workflows/ci.yml`, `README.md`, `.gitignore` | new |
| `vendor/euicc-rsp` | submodule |

**`euicc-rsp` after the move:** loses `src/rsp_transport.c`, `src/rsp_pcsc.c`, `src/rsp_es10.c`, the five test programs, `testdata/cards/`, and the transport/ES10/card declarations from `include/rsp.h`. Keeps everything else, including `rsp-2.5.asn`, `dist/`, and `src/rsp_internal.h`.

**`src/rsp_internal.h` stays in `euicc-rsp` and is shared.** It holds only `static inline` helpers (`rsp_growbuf_t`, `rsp_der_length_octets`, `rsp_rng_init`, `rsp_accept_certificate_policies`), and both sides of the split need the first two. `euicc-lpa` reaches it with `-I$(RSP)/src`. Copying it instead would recreate exactly the "two implementations of one rule" duplication this project already had to unpick once, and static inlines create no duplicate symbols.

---

### Task 1: The euicc-lpa skeleton that links

Proves the three-way wiring — new library, `euicc-rsp`'s `librsp.a`, `euicc-rsp`'s generated codec — before a single line of code moves. If this task is wrong, every later task fails for a reason that has nothing to do with the move.

**Files:**
- Create: `~/git/waigel/euicc-lpa/` (new git repository)
- Create: `include/lpa.h`, `src/lpa_version.c`, `tests/test_link.c`, `tests/run-tests`, `Makefile`, `README.md`, `.gitignore`, `.github/workflows/ci.yml`
- Create: `vendor/euicc-rsp` (submodule of `git@github.com:waigel/euicc-rsp.git`, pinned at the current `euicc-rsp` main)

**Interfaces:**
- Consumes: `rsp_version()` from `euicc-rsp`'s `include/rsp.h`; `asn_DEF_EUICCInfo2` from `euicc-rsp`'s `dist/EUICCInfo2.h`.
- Produces: `const char *lpa_version(void);` declared in `include/lpa.h`. Later tasks add to this header.

- [ ] **Step 1: Create the repository and add the submodule**

```bash
mkdir -p ~/git/waigel/euicc-lpa && cd ~/git/waigel/euicc-lpa
git init -b main
git submodule add git@github.com:waigel/euicc-rsp.git vendor/euicc-rsp
git -C vendor/euicc-rsp submodule update --init --recursive
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_link.c`:

```c
/* Does this library link, and does it reach both things it stands on? A
   build that compiles but resolves no euicc-rsp symbol, or that cannot
   see the generated codec, passes every other test in this suite -- because
   every other test would fail to build rather than fail to pass, and a
   missing binary is not a failing one. This is the floor. */
#include <stdio.h>
#include <string.h>

#include "lpa.h"
#include "rsp.h"
#include "EUICCInfo2.h"

/* The same macro the Makefile hands to src/lpa_version.c's build, folded
   into ALL_CFLAGS so every object here sees it. Comparing lpa_version()
   against it, rather than against a second hand-typed copy, is what makes
   this assertion able to fail when the two drift. */
#ifndef LPA_VERSION
#error "LPA_VERSION must be defined by the build"
#endif

static int fails = 0;
static void ok(const char *what, int cond) {
    printf("%s   %s\n", cond ? "ok  " : "FAIL", what);
    if(!cond) fails = 1;
}

int main(void) {
    ok("lpa_version() is not null", lpa_version() != NULL);
    ok("lpa_version() matches what the build compiled in",
       lpa_version() != NULL && strcmp(lpa_version(), LPA_VERSION) == 0);

    /* euicc-rsp resolves: a symbol from librsp.a, not from a header. */
    ok("euicc-rsp is linked in, not just included",
       rsp_version() != NULL && rsp_version()[0] != '\0');

    /* The generated codec resolves: a descriptor object, which lives in
       dist/EUICCInfo2.o and cannot be satisfied by a declaration alone. */
    ok("the generated codec is linked in",
       asn_DEF_EUICCInfo2.name != NULL
       && strcmp(asn_DEF_EUICCInfo2.name, "EUICCInfo2") == 0);

    return fails;
}
```

- [ ] **Step 3: Run it to verify it fails**

Run: `cd ~/git/waigel/euicc-lpa && make check`
Expected: FAIL — `make: *** No targets specified and no makefile found.`

- [ ] **Step 4: Write the Makefile**

Create `Makefile`:

```make
# euicc-lpa -- the LPA role of SGP.22 as a library.
#
#     make          the library
#     make check    the tests that need no reader
#     make check-card   the tests that need a real card in a reader
#     make clean    everything the build produced
#
# This library stands on euicc-rsp for two things: its crypto and PKI
# (librsp.a), and its generated codec (dist/), which is produced from the
# whole of rsp-2.5.asn and serves both roles. euicc-lpa deliberately does
# NOT generate a second copy of that codec -- see the spec's "The generated
# codec is not duplicated" for why a third set of duplicate strong symbols
# would be worse than the two euicc-tools already reconciles.

.DELETE_ON_ERROR:

CC      ?= cc
CFLAGS  ?= -O2 -g
STD     := -std=c99
WARN    := -Wall -Wextra -Wno-unused-parameter \
           -Werror=implicit-function-declaration -Werror=int-conversion

VERSION := 0.1
RSP     := vendor/euicc-rsp
MBED    := $(RSP)/vendor/mbedtls
RSPDIST := $(RSP)/dist

# -I$(RSP)/src is for rsp_internal.h, which holds only static inline
# helpers (rsp_growbuf_t, rsp_der_length_octets) that both sides of the
# repository split need. Copying it here would be a second implementation
# of one rule, which this project has already had to unpick once.
INC     := -Iinclude -Isrc -I$(RSP)/include -I$(RSP)/src -I$(MBED)/include
DEF     := -DLPA_VERSION='"$(VERSION)"'
EXTRA   := -D_DEFAULT_SOURCE
ifeq ($(shell uname -s),Darwin)
EXTRA   += -D_DARWIN_C_SOURCE
endif

ifeq ($(shell uname -s),Darwin)
PCSC_LIBS := -framework PCSC
else
PCSC_CFLAGS := $(shell pkg-config --cflags libpcsclite 2>/dev/null)
PCSC_LIBS   := $(shell pkg-config --libs libpcsclite 2>/dev/null || echo -lpcsclite)
endif

ALL_CFLAGS = $(STD) $(WARN) $(CFLAGS) $(EXTRA) $(INC) $(DEF) $(PCSC_CFLAGS)

SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:.c=.o)
LIB  := liblpa.a

# Both headers against every object: every translation unit here reaches
# one or both, so listing them is an over-approximation, not a guess.
# Without it, touching include/lpa.h changes nothing make can see.
$(OBJS): include/lpa.h $(RSP)/include/rsp.h

.PHONY: all
all: $(LIB)

# euicc-rsp's own Makefile decides whether anything is stale; ar only
# touches librsp.a's mtime when a member actually changed, so a build with
# nothing to do stays cheap.
.PHONY: rsp-force
rsp-force:

$(RSP)/librsp.a: rsp-force
	@test -e $(MBED)/.git || { \
	    echo "euicc-rsp's submodules are missing:" >&2; \
	    echo "  git -C $(RSP) submodule update --init --recursive" >&2; \
	    exit 1; }
	$(MAKE) -C $(RSP) $(if $(ASN1C),ASN1C="$(ASN1C)") $(if $(SKELDIR),SKELDIR="$(SKELDIR)")

%.o: %.c
	$(CC) $(ALL_CFLAGS) -idirafter $(RSPDIST) -c $< -o $@

$(LIB): $(OBJS)
	ar rcs $@ $(OBJS)

# run-card needs a reader and is excluded from "make check" here, the same
# way euicc-rsp excludes its own.
TEST_SRCS  := $(wildcard tests/test_*.c)
TEST_BINS  := $(patsubst tests/test_%.c,tests/run-%,$(TEST_SRCS))
CHECK_BINS := $(filter-out tests/run-card,$(TEST_BINS))

tests/run-%: tests/test_%.c $(LIB) $(RSP)/librsp.a
	$(CC) $(ALL_CFLAGS) -idirafter $(RSPDIST) $< $(LIB) \
	    $(RSP)/librsp.a $(RSPDIST)/*.o \
	    -o $@ $(PCSC_LIBS) -lm

.PHONY: check
check: $(CHECK_BINS)
	./tests/run-tests

.PHONY: check-card
check-card: tests/run-card
	./tests/run-card

.PHONY: clean
clean:
	rm -f $(OBJS) $(LIB) $(TEST_BINS)
```

- [ ] **Step 5: Write the skeleton sources**

Create `include/lpa.h`:

```c
/*
 * lpa.h -- the LPA role of SGP.22, as a library.
 *
 * The LPA sits between an eUICC and an RSP server. This library holds the
 * card side of that: a transport that carries APDUs over a real reader,
 * over a text recording, or over a wrapper that writes one; the ES10
 * command layer on top of it; and the read-only commands a card answers
 * without a secure channel.
 *
 * It stands on euicc-rsp (vendor/euicc-rsp) for crypto, PKI and the
 * generated ASN.1 codec, which is produced from the whole RSP module and
 * belongs to neither role in particular.
 */
#ifndef LPA_H
#define LPA_H

#include <stddef.h>
#include <stdint.h>

/* The library version, for a bug report. */
const char *lpa_version(void);

#endif /* LPA_H */
```

Create `src/lpa_version.c`:

```c
/* The version, compiled in from the Makefile so there is exactly one copy
   of the string. tests/test_link.c pins this against the same macro. */
#include "lpa.h"

const char *lpa_version(void) { return LPA_VERSION; }
```

Create `tests/run-tests`:

```sh
#!/bin/sh
# Runs every test binary in this directory and sums the verdicts. The
# binaries print their own ok/FAIL lines; this only decides the exit code.
set -u
here=$(dirname "$0")
status=0
for t in "$here"/run-*; do
    [ -x "$t" ] || continue
    # "run-*" matches this script's own name. Without this guard the loop
    # invokes itself and recurses until fork() fails.
    [ "$(basename "$t")" = "run-tests" ] && continue
    # run-card needs a real reader. make check does not build it, but a
    # binary left over from "make check-card" would still be in this glob.
    [ "$(basename "$t")" = "run-card" ] && continue
    "$t" || status=1
done
exit $status
```

Then `chmod +x tests/run-tests`.

Create `.gitignore`:

```
*.o
*.a
tests/run-*
!tests/run-tests
.DS_Store
.idea/
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `cd ~/git/waigel/euicc-lpa && make check`
Expected: four `ok` lines, exit 0.

If `asn_DEF_EUICCInfo2` does not resolve, the `$(RSPDIST)/*.o` on the link line is the thing to check — euicc-rsp compiles `dist/` as a side effect of its own build, so `librsp.a` alone does not carry it.

- [ ] **Step 7: Write the README and CI**

Create `README.md`. State: what the library is (the LPA role), that it needs euicc-rsp's submodules initialised recursively, that Linux needs `pcsc-lite` headers and macOS needs nothing, and that `make check` runs without a reader while `make check-card` needs one.

Create the CI workflow by copying euicc-rsp's, which already solves the two
problems a fresh one would hit (ubuntu ships asn1c 0.9.28, which has no `-D`;
and `pcsc-lite` headers are not installed by default):

```bash
mkdir -p .github/workflows
cp vendor/euicc-rsp/.github/workflows/ci.yml .github/workflows/ci.yml
```

Then make exactly these edits to the copy:

1. The `actions/checkout` step keeps `submodules: recursive` — this repository
   needs euicc-rsp *and* euicc-rsp's own mbedTLS.
2. The build step passes the asn1c it built down two levels. Change the `make`
   invocation to:
   `make ASN1C="$RUNNER_TEMP/asn1c-src/asn1c/asn1c" SKELDIR="$RUNNER_TEMP/asn1c-src/skeletons"`
   and the same for `make check`. Confirm the two paths against what euicc-rsp's
   own workflow uses; copy its values rather than these if they differ.
3. Leave `make check-card` out. CI has no reader, and Task 2 Step 9 is where
   that gets checked.

Then push a branch and confirm the workflow is green before Step 9's commit —
a CI file that has never run is not yet a CI file.

- [ ] **Step 8: Verify the floor test can fail**

Three mutations. Apply one, run `make check`, restore, confirm green again
before the next.

**(a) The version assertion.** Changing `VERSION` in the Makefile does *not*
break it — both the function and the test read the same macro, which is
exactly the property that makes it useful. Break the function instead:

```c
const char *lpa_version(void) { return "x"; }
```

Expected: `FAIL   lpa_version() matches what the build compiled in`.

**(b) The euicc-rsp link.** Remove `$(RSP)/librsp.a` from the `tests/run-%`
link line.
Expected: a link error naming `_rsp_version`.

**(c) The codec link.** Remove `$(RSPDIST)/*.o` from the same line.
Expected: a link error naming `_asn_DEF_EUICCInfo2`.

Record all three in the commit message. An assertion nobody has seen fail is
not yet a test — this project has shipped several that could not.

- [ ] **Step 9: Commit**

```bash
cd ~/git/waigel/euicc-lpa
git add -A
git commit -m "feat: the LPA library links against euicc-rsp and its codec"
```

Then create the GitHub repository and push. Publishing is the owner's decision: ask before creating it.

---

### Task 2: Move the transport, ES10 and the card commands

One task, because it is one dependency cluster. `rsp_es10.c` uses the transport; the five test programs use both; `euicc-rsp` will not build its tests with half of this moved. Nothing that stays in `euicc-rsp` uses any of it — verified: `rsp_bpp.c`, `rsp_crypto.c`, `rsp_pki.c`, `rsp_sign.c` and `rsp_version.c` reference no `rsp_transport_t`, `rsp_es10_send` or `rsp_card_*`. The cut is one-way.

**Files:**
- Move to `euicc-lpa/src/`: `rsp_transport.c`, `rsp_pcsc.c`, `rsp_es10.c`
- Move to `euicc-lpa/tests/`: `test_apdu.c`, `test_card.c`, `test_es10.c`, `test_profiles.c`, `test_recording.c`
- Move to `euicc-lpa/testdata/cards/`: all seven `.log` files and `README.md`
- Modify: `euicc-lpa/include/lpa.h` — receives the declarations
- Modify: `euicc-rsp/include/rsp.h` — loses them
- Delete from `euicc-rsp`: the three sources, the five tests, `testdata/cards/`

**Interfaces:**
- Consumes: `rsp_growbuf_t`, `rsp_growbuf_append`, `rsp_growbuf_free`, `rsp_der_length_octets` from `euicc-rsp/src/rsp_internal.h`, reached with `-I$(RSP)/src`. Unchanged, still `static inline`.
- Produces, all moved verbatim into `include/lpa.h`: `rsp_transport_t`; `int rsp_pcsc_open(const char *reader, rsp_transport_t *out);` `int rsp_pcsc_readers(char ***out, size_t *count);` `int rsp_replay_open(const char *path, rsp_transport_t *out);` `int rsp_record_open(rsp_transport_t *inner, const char *path, rsp_transport_t *out);` `int rsp_es10_send(rsp_transport_t *t, const uint8_t *req, size_t req_len, uint8_t **out, size_t *out_len, unsigned *sw);` `rsp_card_info_t`; `int rsp_card_read_info(rsp_transport_t *t, rsp_card_info_t *out, int *no_isdr);` `void rsp_card_info_free(rsp_card_info_t *i);` `int rsp_card_trusts(const rsp_card_info_t *i, const uint8_t *id, size_t id_len);` `rsp_profile_info_t`; `int rsp_card_read_profiles(rsp_transport_t *t, rsp_profile_info_t **out, size_t *out_count, long *err, int *no_isdr);` `void rsp_card_profiles_free(rsp_profile_info_t *profiles, size_t count);`

- [ ] **Step 1: Record the baseline that must be preserved**

```bash
cd ~/git/waigel/euicc-rsp && make clean >/dev/null && make -j8 >/dev/null && make check 2>&1 | grep -c '^ok '
cd ~/git/waigel/euicc-lpa && make clean >/dev/null && make check 2>&1 | grep -c '^ok '
```

Expected: `235` and `4`. Write both numbers down; Step 8 checks the sum.

- [ ] **Step 2: Move the files with git, so history follows**

```bash
cd ~/git/waigel/euicc-rsp
for f in src/rsp_transport.c src/rsp_pcsc.c src/rsp_es10.c \
         tests/test_apdu.c tests/test_card.c tests/test_es10.c \
         tests/test_profiles.c tests/test_recording.c; do
    cp "$f" ~/git/waigel/euicc-lpa/"$f"
done
mkdir -p ~/git/waigel/euicc-lpa/testdata
cp -R testdata/cards ~/git/waigel/euicc-lpa/testdata/
git rm -q src/rsp_transport.c src/rsp_pcsc.c src/rsp_es10.c \
          tests/test_apdu.c tests/test_card.c tests/test_es10.c \
          tests/test_profiles.c tests/test_recording.c
git rm -q -r testdata/cards
```

- [ ] **Step 3: Move the declarations between the headers**

Cut every declaration listed in this task's **Produces** block out of `euicc-rsp/include/rsp.h`, together with its comment block, and paste it into `euicc-lpa/include/lpa.h` unchanged. Keep the order.

Then fix `euicc-rsp/include/rsp.h`'s opening comment, which currently describes the card work as part of this library. Replace the second paragraph with a sentence saying the card side now lives in euicc-lpa. Also correct the failure-convention paragraph: it says "Eight functions are different" and names `rsp_transport_t.transceive`, `rsp_es10_send`, `rsp_card_read_info` and `rsp_card_read_profiles` among them. Four of those move, so the count and the list both change. Count the remaining ones in the file and write that number — do not guess it.

- [ ] **Step 4: Change the include lines, and nothing else**

In each of the eight moved files, replace `#include "rsp.h"` with
`#include "lpa.h"` — every symbol these files declare or call is one that
moved.

Two files may still need `rsp.h` alongside it: `src/rsp_es10.c` includes
`rsp_internal.h`, and `tests/test_card.c` uses euicc-rsp's SGP.26 material.
Do not add it speculatively. Build first; `-Werror=implicit-function-declaration`
names exactly what is missing, and adding an include the file does not need is
itself a change to moved code.

**Include lines are the only edit permitted to moved code in this task.**
Step 7 checks that mechanically. If a moved file will not compile without a
real change, stop and report it rather than making it — it means this plan's
claim that the cut is one-way is wrong, and that is worth knowing before more
is built on it.

- [ ] **Step 5: Point the test fixtures at their new home**

The five moved tests open recordings by relative path, e.g.
`rsp_replay_open("testdata/cards/omnikey-profiles.log", &t)`. Those paths are
unchanged, because `testdata/cards/` moved to the same relative place. Verify
with:

```bash
cd ~/git/waigel/euicc-lpa && grep -rn 'testdata/' tests/*.c | grep -v 'testdata/cards' 
```

Expected: no output. If a moved test references `testdata/sgp26/` or
`testdata/nist/`, those did **not** move — stop and report it, because it means
this plan's file inventory is wrong.

- [ ] **Step 6: Build both and run the suites**

```bash
cd ~/git/waigel/euicc-rsp && make clean >/dev/null && make -j8 && make check
cd ~/git/waigel/euicc-lpa && make clean >/dev/null && make -j8 && make check
```

Expected: both exit 0. `euicc-rsp` now reports fewer than 235; `euicc-lpa` reports the rest plus its own 4.

- [ ] **Step 7: Prove the move changed nothing but includes**

```bash
cd ~/git/waigel/euicc-lpa
for f in src/rsp_transport.c src/rsp_pcsc.c src/rsp_es10.c \
         tests/test_apdu.c tests/test_card.c tests/test_es10.c \
         tests/test_profiles.c tests/test_recording.c; do
    echo "=== $f"
    diff <(git -C ~/git/waigel/euicc-rsp show HEAD~1:"$f") "$f"
done
```

Expected: every hunk is an added or changed `#include` line. Any other
difference is a behaviour change smuggled into a move — revert it and note it
for a later plan.

- [ ] **Step 8: Check the arithmetic**

```bash
a=$(cd ~/git/waigel/euicc-rsp && make check 2>&1 | grep -c '^ok ')
b=$(cd ~/git/waigel/euicc-lpa && make check 2>&1 | grep -c '^ok ')
echo "$a + $b = $((a+b))  (must be >= 239)"
```

239 is 235 plus Task 1's 4. Fewer means a test was lost. Do not proceed.

- [ ] **Step 9: Verify against the real card**

With the OMNIKEY reader and the test eUICC connected:

```bash
cd ~/git/waigel/euicc-lpa && make check-card && ./tests/run-card
```

Expected: the card answers, EID `89049032123451234512345678901235`, and the profile list is empty. The recordings replay in CI; this is the one thing they cannot prove.

- [ ] **Step 10: Commit both repositories**

```bash
cd ~/git/waigel/euicc-lpa && git add -A && git commit -m "feat: the card side moves here from euicc-rsp"
cd ~/git/waigel/euicc-rsp && git add -A && git commit -m "refactor: the card side moves to euicc-lpa"
```

Push `euicc-rsp` first, then bump `euicc-lpa`'s submodule pointer to it and push that. Report the two test counts and their sum in the `euicc-lpa` commit message.

---

### Task 3: Rewire euicc-tools

**Files:**
- Create: `euicc-tools/vendor/euicc-lpa` (submodule)
- Modify: `euicc-tools/Makefile` — the `RSP` block becomes an `LPA` block; `liblpa.a`'s members join the existing single archive, which is renamed `build/libeuicc-full.a`. The link line itself does not change. See Step 3 for why this is better than the third archive the spec described.
- Modify: `euicc-tools/src/card.c:28` — `#include <rsp.h>` becomes `#include <lpa.h>` plus `#include <rsp.h>` if still needed
- Modify: `euicc-tools/tests/run-tests` — the fixture paths under `vendor/euicc-rsp/testdata/cards/` become `vendor/euicc-lpa/testdata/cards/`
- Remove: `euicc-tools/vendor/euicc-rsp` as a direct submodule; it is reached through `vendor/euicc-lpa/vendor/euicc-rsp`

**Interfaces:**
- Consumes: everything in Task 2's **Produces** block, now from `<lpa.h>`.
- Produces: nothing new. `euicc card info` and `euicc card profiles` behave exactly as before.

- [ ] **Step 1: Add the new submodule, drop the old direct one**

```bash
cd ~/git/waigel/euicc-tools
git submodule add git@github.com:waigel/euicc-lpa.git vendor/euicc-lpa
git submodule update --init --recursive vendor/euicc-lpa
git rm -q vendor/euicc-rsp
```

- [ ] **Step 2: Run the suite to verify it fails**

Run: `cd ~/git/waigel/euicc-tools && make`
Expected: FAIL — the Makefile still names `vendor/euicc-rsp`, which is gone.

- [ ] **Step 3: Rewire the Makefile**

In `euicc-tools/Makefile`, rename the variables and retarget the paths:

```make
LPA      := vendor/euicc-lpa
RSP      := $(LPA)/vendor/euicc-rsp
LPA_LIB  := $(LPA)/liblpa.a
RSP_LIB  := $(RSP)/librsp.a
RSP_DIST := $(RSP)/dist
```

Replace the `$(RSP_LIB)` rule's recipe so it builds `euicc-lpa`, which builds
`euicc-rsp` in turn:

```make
.PHONY: lpa-lib-force
lpa-lib-force:

$(LPA_LIB): $(ASN1C)/asn1c/asn1c lpa-lib-force
	@test -e $(RSP)/vendor/mbedtls/.git || { \
	    echo "the euicc-lpa submodule's own submodules are missing:" >&2; \
	    echo "  git -C $(LPA) submodule update --init --recursive" >&2; \
	    exit 1; }
	$(MAKE) -C $(LPA) ASN1C="$(abspath $(ASN1C)/asn1c/asn1c)" SKELDIR="$(abspath $(SKELDIR))"
```

`build/rsp-objs/.stamp` gains `liblpa.a`'s members. Extract it **after**
`librsp.a` so the LPA's own objects sit alongside, and keep the `dist/*.o`
copy:

```make
build/rsp-objs/.stamp: $(LPA_LIB)
	rm -rf build/rsp-objs
	mkdir -p build/rsp-objs
	cd build/rsp-objs && ar x $(abspath $(RSP_LIB))
	cd build/rsp-objs && ar x $(abspath $(LPA_LIB))
	cp $(RSP_DIST)/*.o build/rsp-objs/
	@touch $@
```

**This departs from the spec deliberately.** The spec said the link line would
grow a third archive and that `liblpa.a` must precede `librsp-full.a`. Merging
both into the one archive is better: a single archive has no internal ordering
problem at all, so the constraint disappears instead of being documented and
then forgotten. Nothing else about the spec's reasoning changes.

Rename the target to `build/libeuicc-full.a` — the old name claims it holds
only euicc-rsp, which stops being true here — and update the two places that
reference it.

- [ ] **Step 4: Change the include and the fixture paths**

`euicc-tools/src/card.c`, line 28: `#include <rsp.h>` becomes

```c
#include <lpa.h>
```

`card.c` uses only moved symbols (`rsp_transport_t`, `rsp_pcsc_open`,
`rsp_replay_open`, `rsp_record_open`, `rsp_card_read_info`,
`rsp_card_read_profiles`, `rsp_card_trusts`, and the two free functions), so
`rsp.h` is no longer needed there. If the build says otherwise, add it back.

In `euicc-tools/tests/run-tests`, replace every
`$root/vendor/euicc-rsp/testdata/cards/` with
`$root/vendor/euicc-lpa/testdata/cards/`. There are eight such paths across the
`card info` and `card profiles` assertions.

- [ ] **Step 5: Build and run the suite**

```bash
cd ~/git/waigel/euicc-tools && make clean >/dev/null && make -j8 && make check
```

Expected: `99 ok, 0 failed`. Not 98. If a `card profiles` assertion fails on a
missing recording, a fixture path in Step 4 was missed.

- [ ] **Step 6: Verify against the real card, then reinstall**

```bash
cd ~/git/waigel/euicc-tools
./euicc card info && ./euicc card profiles
make install PREFIX=$HOME/.local
euicc version
```

Expected: the card answers in well under a second, the profile list is empty,
and `euicc version` names the new commit.

- [ ] **Step 7: Commit**

```bash
cd ~/git/waigel/euicc-tools
git add -A
git commit -m "refactor: the card side comes from euicc-lpa now"
```

---

### Task 4: The guard the Makefile already claims to have

`euicc-tools/Makefile:169` says the two generated codecs "agree except for the
known-benign differences the guard below allows". **There is no guard below.**
The comment describes something that was never written, which is worse than
silence: it tells a reader the property is checked when nothing checks it.

The property the archive trick depends on is that `build/gen/*.o`
(euicc-schema's codec, always linked) and `$(RSP_DIST)/*.o` (euicc-rsp's, inside
the archive) define the shared symbols identically — so that it never matters
which copy the linker reaches. With three repositories in one binary, that
stops being obvious.

**Files:**
- Create: `euicc-tools/tools/check-codec-agreement`
- Modify: `euicc-tools/Makefile` — the guard runs as a prerequisite of the binary; fix the lying comment
- Modify: `euicc-tools/tests/run-tests` — one assertion that the guard runs and passes

**Interfaces:**
- Consumes: `build/gen/` and `$(RSP_DIST)/`, both populated by the existing build.
- Produces: a script exiting 0 when the codecs agree, 1 when they do not, naming every disagreeing file.

- [ ] **Step 1: Write the failing test**

Add to `euicc-tools/tests/run-tests`, before the final summary:

```sh
# The archive trick (see the Makefile) links two copies of the asn1c
# runtime and the PKIX types, and works only because the copies are
# identical -- the linker may take either. Nothing checked that until this
# assertion; the Makefile comment claimed a guard that did not exist.
"$root/tools/check-codec-agreement" "$root/build/gen" \
    "$root/vendor/euicc-lpa/vendor/euicc-rsp/dist" >/dev/null 2>&1 \
  && ok "the two generated codecs agree on every shared file" \
  || bad "the two generated codecs agree on every shared file"
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd ~/git/waigel/euicc-tools && make check 2>&1 | grep codec`
Expected: `FAIL the two generated codecs agree on every shared file` — the
script does not exist yet.

- [ ] **Step 3: Write the guard**

Create `euicc-tools/tools/check-codec-agreement`:

```sh
#!/bin/sh
# Two directories of asn1c output. For every .c or .h file present in both,
# the contents must be identical.
#
# This is what makes build/libeuicc-full.a safe. Almost every file in
# euicc-rsp's dist/ also exists in euicc-schema's, under the same name and
# defining the same strong symbols: the generic runtime (BER/DER, INTEGER,
# OCTET_STRING) and the PKIX types both a profile package and an RSP
# handshake reference. Two loose .o files defining one symbol is a link
# error; an archive member is not, because the linker only reaches into an
# archive for a symbol still outstanding. So the second copy is simply
# never used -- which is fine exactly as long as it would have made no
# difference. This checks that.
#
# The two are generated by the same asn1c binary (euicc-tools passes the
# one it built to both submodules), so byte equality is the right bar, not
# a lenient one. A difference means the two projects' ASN.1 modules have
# diverged in a shared type, and the binary's behaviour then depends on
# link order rather than on intent.
set -u
[ $# -eq 2 ] || { echo "usage: $0 <dir-a> <dir-b>" >&2; exit 2; }
a=$1; b=$2
[ -d "$a" ] || { echo "not a directory: $a" >&2; exit 2; }
[ -d "$b" ] || { echo "not a directory: $b" >&2; exit 2; }

shared=0
bad=0
for pa in "$a"/*.c "$a"/*.h; do
    [ -f "$pa" ] || continue
    pb="$b/$(basename "$pa")"
    [ -f "$pb" ] || continue
    shared=$((shared + 1))
    if ! cmp -s "$pa" "$pb"; then
        echo "differs: $(basename "$pa")" >&2
        bad=$((bad + 1))
    fi
done

# No shared files at all would make this script pass for the wrong reason
# -- a mistyped path, or a dist/ that was never generated. The two codecs
# have hundreds of files in common; a handful means something is wrong.
if [ "$shared" -lt 50 ]; then
    echo "only $shared shared files between $a and $b -- expected hundreds;" >&2
    echo "one of these directories is probably not an asn1c dist" >&2
    exit 2
fi

if [ "$bad" -gt 0 ]; then
    echo "$bad of $shared shared generated files differ" >&2
    exit 1
fi
echo "$shared shared generated files, all identical"
exit 0
```

Then `chmod +x tools/check-codec-agreement`.

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd ~/git/waigel/euicc-tools && make check 2>&1 | grep codec`
Expected: `ok   the two generated codecs agree on every shared file`

- [ ] **Step 5: Prove the guard can fail, all three ways**

```bash
cd ~/git/waigel/euicc-tools
D=vendor/euicc-lpa/vendor/euicc-rsp/dist

# (a) a real disagreement
cp $D/INTEGER.c /tmp/INTEGER.c.bak
printf '\n/* injected */\n' >> $D/INTEGER.c
./tools/check-codec-agreement build/gen $D; echo "expect 1, got $?"
cp /tmp/INTEGER.c.bak $D/INTEGER.c

# (b) a path that is not a dist
./tools/check-codec-agreement build/gen /tmp; echo "expect 2, got $?"

# (c) a path that does not exist
./tools/check-codec-agreement build/gen /nonexistent; echo "expect 2, got $?"

# and green again
./tools/check-codec-agreement build/gen $D; echo "expect 0, got $?"
```

All four must print the expected code. (b) is the one that matters most: it is
the check that stops this guard from passing because it compared nothing.

- [ ] **Step 6: Wire it into the build and fix the comment**

Make the guard a prerequisite of the binary in `euicc-tools/Makefile`, so a
divergence fails the build rather than waiting for `make check`:

```make
.PHONY: codec-agreement
codec-agreement: build/gen/.stamp $(LPA_LIB)
	@./tools/check-codec-agreement build/gen $(RSP_DIST)
```

Add `codec-agreement` to the `euicc` target's prerequisites.

Then rewrite the comment at `Makefile:169`. It currently promises a guard in
the future tense of a file that has none. It should now name
`tools/check-codec-agreement` and say what that script checks.

- [ ] **Step 7: Commit**

```bash
cd ~/git/waigel/euicc-tools
git add tools/check-codec-agreement tests/run-tests Makefile
git commit -m "fix: check the codec agreement the Makefile said it checked"
```

---

## Done when

- `euicc-rsp`, `euicc-lpa` and `euicc-tools` all build clean and all suites pass.
- The three test counts sum to at least **340** (334 baseline + Task 1's 4 + Task 4's 1).
- `euicc card info` and `euicc card profiles` answer correctly against the real card, from a freshly installed `~/.local/bin/euicc`.
- CI is green in all three repositories.
- Every moved file differs from its original only in `#include` lines, shown by Task 2 Step 7.
- No moved test lost an assertion.

Plan B — the write path — is written after this lands, against the layout it produces.
