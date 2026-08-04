# euicc-rsp, first half: everything provable without hardware

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A C library that turns a profile package (UPP) into a Bound Profile
Package, with every part verified on a machine that has no card reader.

**Architecture:** Four components with one edge between them. `rsp_codec` is
the SGP.22 RSP ASN.1 module through asn1c. `rsp_pki` mints DP certificates
under the published GSMA SGP.26 test CI. `rsp_crypto` derives the session
keys and protects the segments, and it uses `rsp_codec` because protecting a
message means encoding it first. `rsp_card` and `rsp_flow` are the second
half of the project and appear nowhere in this plan.

**Tech Stack:** C99, asn1c (invoked as a path, not vendored), mbedTLS 3.x as
a submodule, POSIX `sh` for the test harness, GNU make, GitHub Actions.

## Global Constraints

- Language is C99. The build passes `-Wall -Wextra
  -Werror=implicit-function-declaration -Werror=int-conversion` and
  `-D_DEFAULT_SOURCE`, copied from `euicc-tools/Makefile`. On Linux, glibc
  hides `strdup` and friends without `_DEFAULT_SOURCE`, and an implicit
  declaration truncates a pointer.
- No interpreter in the test chain. Tests are C binaries plus POSIX `sh`.
  Python must not become a build or test dependency.
- Nothing is installed system-wide except what a platform already ships.
  mbedTLS is a submodule; asn1c is passed in as `ASN1C=<path>`.
- Exit contract, everywhere a program answers: `0` done, `1` a real negative
  answer, `2` could not answer.
- Private keys and session keys never appear in output, in JSON, or in a log.
- Every test must be able to fail. Each task's last verification step
  includes a mutation: break one byte, watch the test go red, restore.
- The SGP.26 material is test material. Every file that carries it, and the
  README, states that it works on test cards only.
- Commit messages follow the house style: a lowercase type prefix, then what
  changed and why it matters, in full sentences.

---

### Task 1: The repository, the build, and proof that mbedTLS is linked

**Files:**
- Create: `~/git/waigel/euicc-rsp/Makefile`
- Create: `~/git/waigel/euicc-rsp/include/rsp.h`
- Create: `~/git/waigel/euicc-rsp/src/rsp_version.c`
- Create: `~/git/waigel/euicc-rsp/tests/test_link.c`
- Create: `~/git/waigel/euicc-rsp/tests/run-tests`
- Create: `~/git/waigel/euicc-rsp/README.md`
- Create: `~/git/waigel/euicc-rsp/.github/workflows/ci.yml`
- Create: `~/git/waigel/euicc-rsp/.gitignore`

**Interfaces:**
- Consumes: nothing.
- Produces: `const char *rsp_version(void)` returning the library version
  string. The test harness contract: `tests/run-tests` runs every
  `tests/run-*` binary, prints `ok`/`FAIL` lines, and exits non-zero if any
  failed. Later tasks add binaries to this directory and nothing else.

- [ ] **Step 1: Create the repository and pin mbedTLS**

```bash
mkdir -p ~/git/waigel/euicc-rsp && cd ~/git/waigel/euicc-rsp
git init
git submodule add https://github.com/Mbed-TLS/mbedtls.git vendor/mbedtls
cd vendor/mbedtls && git checkout v3.6.2 && cd ../..
git submodule update --init --recursive
```

Pin the tag explicitly. mbedTLS 3.x made structure fields private and renamed
interfaces between minor versions, so an unpinned submodule breaks the build
on someone else's clone.

- [ ] **Step 2: Write the failing test**

`tests/test_link.c` proves two things at once: the library links, and mbedTLS
inside it computes a known answer. The vector is the SHA-256 of `"abc"` from
FIPS 180-4.

```c
/* Does the library link, and does the crypto inside it work? A build that
   compiles but resolves no mbedTLS symbol passes every other test in this
   suite, because every other test would be skipped. This one is the floor. */
#include <stdio.h>
#include <string.h>
#include "rsp.h"
#include "mbedtls/sha256.h"

static int fails;
static void ok(const char *what, int good) {
    printf("%s   %s\n", good ? "ok  " : "FAIL", what);
    if(!good) fails++;
}

int main(void) {
    static const unsigned char want[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    unsigned char got[32];
    mbedtls_sha256((const unsigned char *)"abc", 3, got, 0);
    ok("mbedTLS computes the FIPS 180-4 SHA-256 of \"abc\"",
       memcmp(got, want, 32) == 0);
    ok("the library reports a version", rsp_version() && rsp_version()[0]);
    return fails ? 1 : 0;
}
```

- [ ] **Step 3: Run it and watch it fail**

Run: `make check`

Expected: the build fails, because `Makefile`, `include/rsp.h` and
`src/rsp_version.c` do not exist yet.

- [ ] **Step 4: Write the header, the one source file, and the harness**

`include/rsp.h`:

```c
/*
 * rsp.h -- the SM-DP+ role of SGP.22, as a library.
 *
 * It builds a Bound Profile Package for one eUICC. It does not speak to a
 * card and it opens no socket: the caller supplies what the card said, and
 * gets back what to send. That split is what makes the whole path testable
 * without hardware.
 */
#ifndef RSP_H
#define RSP_H

#include <stddef.h>
#include <stdint.h>

/* The library version, for a bug report. */
const char *rsp_version(void);

#endif /* RSP_H */
```

`src/rsp_version.c`:

```c
#include "rsp.h"

#ifndef RSP_VERSION
#define RSP_VERSION "0.1"
#endif

const char *rsp_version(void) { return RSP_VERSION; }
```

`tests/run-tests`:

```sh
#!/bin/sh
# Runs every test binary in this directory and sums the verdicts. The
# binaries print their own ok/FAIL lines; this only decides the exit code.
set -u
here=$(dirname "$0")
status=0
for t in "$here"/run-*; do
    [ -x "$t" ] || continue
    "$t" || status=1
done
exit $status
```

`Makefile`:

```make
# euicc-rsp -- the SM-DP+ role of SGP.22 as a library.
#
#     make          the library
#     make check    the tests
#     make clean    everything the build produced
#
# asn1c is not vendored here: it is already a submodule of euicc-schema, and
# euicc-tools passes the one it built. Standalone, any asn1c on PATH does.

CC      ?= cc
CFLAGS  ?= -O2 -g
STD     := -std=c99
WARN    := -Wall -Wextra -Wno-unused-parameter \
           -Werror=implicit-function-declaration -Werror=int-conversion

VERSION := 0.1
MBED    := vendor/mbedtls

INC     := -Iinclude -Isrc -I$(MBED)/include
DEF     := -DRSP_VERSION='"$(VERSION)"'
EXTRA   := -D_DEFAULT_SOURCE
ifeq ($(shell uname -s),Darwin)
EXTRA   += -D_DARWIN_C_SOURCE
endif

ALL_CFLAGS = $(STD) $(WARN) $(CFLAGS) $(EXTRA) $(INC) $(DEF)

SRCS    := $(wildcard src/*.c)
OBJS    := $(SRCS:.c=.o)
LIB     := librsp.a

MBED_LIBS := $(MBED)/library/libmbedx509.a $(MBED)/library/libmbedcrypto.a

TESTS   := tests/run-link

.PHONY: all check clean mbedtls

all: $(LIB)

# mbedTLS builds only the two libraries this needs. libmbedtls (the TLS
# stack) is never linked: there is no socket in this project.
$(MBED_LIBS):
	@test -e $(MBED)/.git || { \
	    echo "the submodule is missing: git submodule update --init --recursive" >&2; \
	    exit 1; }
	$(MAKE) -C $(MBED)/library libmbedcrypto.a libmbedx509.a

mbedtls: $(MBED_LIBS)

%.o: %.c $(MBED_LIBS)
	$(CC) $(ALL_CFLAGS) -c $< -o $@

$(LIB): $(OBJS)
	ar rcs $@ $(OBJS)

tests/run-%: tests/test_%.c $(LIB) $(MBED_LIBS)
	$(CC) $(ALL_CFLAGS) $< $(LIB) $(MBED_LIBS) -o $@

check: $(TESTS)
	./tests/run-tests

clean:
	rm -f $(OBJS) $(LIB) $(TESTS)
	$(MAKE) -C $(MBED)/library clean 2>/dev/null || true
```

`.gitignore`:

```
*.o
*.a
tests/run-*
!tests/run-tests
```

- [ ] **Step 5: Run the test and watch it pass**

Run: `chmod +x tests/run-tests && make check`

Expected:

```
ok    mbedTLS computes the FIPS 180-4 SHA-256 of "abc"
ok    the library reports a version
```

- [ ] **Step 6: Prove the test can fail**

Change one byte of `want` in `tests/test_link.c`, run `make check`, and
confirm the first line reads `FAIL`. Restore the byte and confirm it passes
again. A vector test that cannot go red is a decoration.

- [ ] **Step 7: Write the README with the test-material warning**

`README.md`:

```markdown
# euicc-rsp

The SM-DP+ role of SGP.22, as a C library. It builds a Bound Profile Package
for one eUICC. It is not a server: no HTTPS, no ES2+, no SM-DS, no activation
code. [euicc-tools](https://github.com/waigel/euicc-tools) is the command
that uses it.

| Repository | Role |
| --- | --- |
| [asn1c-vn](https://github.com/waigel/asn1c-vn) | the language |
| [euicc-schema](https://github.com/waigel/euicc-schema) | the vocabulary |
| [euicc-tools](https://github.com/waigel/euicc-tools) | the command |
| `euicc-rsp` (this one) | the protocol |

## Test material only

The certificates and keys in this repository are the published GSMA SGP.26
**test** material. They work on test eUICCs and nowhere else. A production
eUICC rejects them, and a production profile must never be loaded with them.
The private CI key is public by design, so nothing here is a secret and
nothing here is safe.

## Build

```sh
git clone --recurse-submodules https://github.com/waigel/euicc-rsp.git
cd euicc-rsp
make
make check
```

Tests need no card reader. Everything in this repository is provable on a
machine with no hardware attached.
```

- [ ] **Step 8: Add CI**

`.github/workflows/ci.yml`:

```yaml
# Everything here runs without a card reader. That is the point of the first
# half of this project: the crypto is provable on a plain runner.
name: ci

on:
  push:
    branches: [main]
  pull_request:
  workflow_call:

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive
      - name: build and test
        run: |
          make
          make check
```

- [ ] **Step 9: Commit**

```bash
cd ~/git/waigel/euicc-rsp
git add -A
git commit -m "feat: the library builds, and a vector proves mbedTLS is in it

A build that compiles but resolves no crypto symbol would pass an empty
suite. The floor test is the FIPS 180-4 SHA-256 of \"abc\", so the first
green run already means something. mbedTLS is pinned to v3.6.2: 3.x made
struct fields private and renames interfaces between minor versions."
```

Create the GitHub repository and push:

```bash
gh repo create waigel/euicc-rsp --public --source=. --remote=origin --push
```

---

### Task 2: The RSP ASN.1 module

**Files:**
- Create: `~/git/waigel/euicc-rsp/rsp-2.5.asn`
- Modify: `~/git/waigel/euicc-rsp/Makefile` (add the `dist` target and the generated objects)
- Create: `~/git/waigel/euicc-rsp/tests/test_codec.c`
- Modify: `~/git/waigel/euicc-rsp/.gitignore` (add `dist/`)

**Interfaces:**
- Consumes: the harness contract from Task 1.
- Produces: the generated asn1c types in `dist/`, notably
  `StoreMetadataRequest_t`, `BoundProfilePackage_t` and
  `ProfileInstallationResult_t`, each with the usual asn1c surface
  (`asn_DEF_<Type>`, `der_encode`, `ber_decode`, `ASN_STRUCT_FREE`). Later
  tasks include `dist/<Type>.h` and link `dist/*.o`.

- [ ] **Step 1: Obtain the module and prove it is the right one**

The authoritative source is the ASN.1 of SGP.22, the `RSPDefinitions` module.
Extract it from the specification text into `rsp-2.5.asn`, keeping the module
header and the version comment intact.

Verification, all three must hold before continuing:

```bash
grep -c "RSPDefinitions" rsp-2.5.asn                       # at least 1
grep -E "BoundProfilePackage|StoreMetadataRequest" rsp-2.5.asn   # both present
asn1c -S "$(brew --prefix asn1c)/share/asn1c" -E rsp-2.5.asn >/dev/null && echo "parses"
```

`asn1c -E` parses and pretty-prints without generating. If it reports a
syntax error, the extraction lost a line; fix the extraction, do not edit the
grammar to suit the parser.

The module imports PKIX types (`Certificate`, `SubjectPublicKeyInfo`). Those
come from the same `rfc3280` extraction that `euicc-schema` already performs,
so the generation step below passes both files.

- [ ] **Step 2: Write the failing test**

A codec is proven by a round trip: encode a value, decode it, encode again,
require byte-identity. `tests/test_codec.c`:

```c
/* A codec that loses a field usually still encodes something. Comparing the
   second encoding against the first is what catches the loss. */
#include <stdio.h>
#include <string.h>
#include "StoreMetadataRequest.h"

static int fails;
static void ok(const char *what, int good) {
    printf("%s   %s\n", good ? "ok  " : "FAIL", what);
    if(!good) fails++;
}

static int collect(const void *buf, size_t n, void *key) {
    struct { unsigned char *p; size_t len; } *out = key;
    memcpy(out->p + out->len, buf, n);
    out->len += n;
    return 0;
}

int main(void) {
    static const unsigned char iccid[10] = {
        0x98,0x00,0x10,0x32,0x54,0x76,0x98,0x10,0x32,0x14
    };
    unsigned char buf1[512], buf2[512];
    struct { unsigned char *p; size_t len; } o1 = { buf1, 0 }, o2 = { buf2, 0 };

    StoreMetadataRequest_t md;
    memset(&md, 0, sizeof md);
    OCTET_STRING_fromBuf(&md.iccid, (const char *)iccid, sizeof iccid);
    OCTET_STRING_fromBuf(&md.serviceProviderName, "euicc-tools", 11);
    OCTET_STRING_fromBuf(&md.profileName, "example", 7);
    md.profileClass = 2; /* operational */

    ok("encoding succeeds",
       der_encode(&asn_DEF_StoreMetadataRequest, &md, collect, &o1).encoded > 0);

    StoreMetadataRequest_t *back = NULL;
    asn_dec_rval_t r = ber_decode(NULL, &asn_DEF_StoreMetadataRequest,
                                  (void **)&back, buf1, o1.len);
    ok("decoding consumes the whole encoding",
       r.code == RC_OK && r.consumed == o1.len);

    if(back) {
        ok("re-encoding succeeds",
           der_encode(&asn_DEF_StoreMetadataRequest, back, collect, &o2).encoded > 0);
        ok("the two encodings are identical",
           o1.len == o2.len && memcmp(buf1, buf2, o1.len) == 0);
        ASN_STRUCT_FREE(asn_DEF_StoreMetadataRequest, back);
    }
    ASN_STRUCT_RESET(asn_DEF_StoreMetadataRequest, &md);
    return fails ? 1 : 0;
}
```

If a field name in `StoreMetadataRequest` differs from the one used here,
take the name from the generated `dist/StoreMetadataRequest.h`, not from this
plan. The generated header is the truth about the module.

- [ ] **Step 3: Run it and watch it fail**

Run: `make check`

Expected: the compile fails with `StoreMetadataRequest.h: No such file`,
because nothing generates it yet.

- [ ] **Step 4: Add the generation step to the Makefile**

Insert after the `MBED_LIBS` definition:

```make
# The codec, generated from the RSP module. asn1c is a path, not a submodule:
# euicc-schema already vendors one and euicc-tools passes it in. Standalone,
# ASN1C defaults to whatever is on PATH.
ASN1C   ?= asn1c
SKELDIR ?= $(shell dirname $$(command -v $(ASN1C)))/../share/asn1c
RSP_ASN := rsp-2.5.asn
DIST    := dist

$(DIST)/BoundProfilePackage.h: $(RSP_ASN)
	mkdir -p $(DIST)
	$(ASN1C) -S $(SKELDIR) -pdu=auto -fcompound-names -D $(DIST) $(RSP_ASN)

# Generated code is compiled with warnings off. It is not ours to correct.
# -idirafter, never -I: the PKIX types generate a Time.h that would hide the
# system <time.h> on a case-insensitive filesystem.
$(DIST)/.stamp: $(DIST)/BoundProfilePackage.h
	cd $(DIST) && $(CC) $(STD) $(CFLAGS) $(EXTRA) -w -I. -c *.c
	@touch $@

codec: $(DIST)/.stamp
```

Then extend the include path, the test link line and `clean`:

```make
INC     := -Iinclude -Isrc -I$(MBED)/include
GEN_INC := -idirafter $(DIST)

tests/run-%: tests/test_%.c $(LIB) $(MBED_LIBS) $(DIST)/.stamp
	$(CC) $(ALL_CFLAGS) $(GEN_INC) $< $(LIB) $(DIST)/*.o $(MBED_LIBS) -o $@

TESTS   := tests/run-link tests/run-codec
```

and in `clean`, add `rm -rf $(DIST)`.

- [ ] **Step 5: Run the test and watch it pass**

Run: `make check`

Expected: four `ok` lines from `run-codec`, and `run-link` still green.

- [ ] **Step 6: Prove the test can fail**

In `tests/test_codec.c`, change `md.profileClass = 2;` to `= 1;` **after**
the first encoding by editing `back->profileClass` before the re-encode. The
identity assertion must go red. Remove the edit afterwards.

- [ ] **Step 7: Commit**

```bash
git add rsp-2.5.asn Makefile tests/test_codec.c .gitignore
git commit -m "feat: the RSP module, generated by the same pipeline as the schema

asn1c comes in as a path rather than a second submodule: euicc-schema
vendors one already and euicc-tools passes the one it built. The test is
a round trip, because a codec that loses a field still encodes
something -- only the second encoding shows the loss."
```

---

### Task 3: Certificates under the SGP.26 test CI

**Files:**
- Create: `~/git/waigel/euicc-rsp/testdata/sgp26/README.md`
- Create: `~/git/waigel/euicc-rsp/testdata/sgp26/ci.der`, `ci-key.der`
- Create: `~/git/waigel/euicc-rsp/src/rsp_pki.c`
- Modify: `~/git/waigel/euicc-rsp/include/rsp.h`
- Create: `~/git/waigel/euicc-rsp/tests/test_pki.c`

**Interfaces:**
- Consumes: nothing from earlier tasks except the build.
- Produces:

```c
typedef struct {
    uint8_t *der;      /* the certificate, DER, owned by the struct */
    size_t   der_len;
    uint8_t  sk[32];   /* the private key, a big-endian P-256 scalar */
} rsp_credential_t;

/* The published SGP.26 test CI, compiled in. Returns 0, or -1 if absent. */
int  rsp_pki_test_ci(const uint8_t **der, size_t *len);

/* Mint a DP credential under the test CI. role is 0 for DPauth
   (authentication) and 1 for DPpb (profile binding). Returns 0 or -1. */
int  rsp_pki_mint(const char *common_name, int role, rsp_credential_t *out);

void rsp_credential_free(rsp_credential_t *c);
```

- [ ] **Step 1: Place the test material and label it**

Put the SGP.26 test CI certificate and its private key in
`testdata/sgp26/` as DER. Write `testdata/sgp26/README.md`:

```markdown
# GSMA SGP.26 test material

These are the published test certificates and keys of SGP.26. The private
key of the CI is public by design, so that anyone can act as an SM-DP+
towards a test eUICC.

Nothing here is a secret and nothing here is safe. A production eUICC
rejects these certificates. Never load a production profile with them.
```

Verify the material is what it claims to be before continuing:

```bash
openssl x509 -inform der -in testdata/sgp26/ci.der -noout -subject -issuer
```

A CI certificate is self-signed: subject and issuer must be identical.

- [ ] **Step 2: Write the failing test**

`tests/test_pki.c`:

```c
/* A minted certificate is worthless unless a verifier accepts the chain.
   This test is that verifier, so a mistake in the extensions or the curve
   shows here and not at the card, where the answer is a status word. */
#include <stdio.h>
#include <string.h>
#include "rsp.h"
#include "mbedtls/x509_crt.h"

static int fails;
static void ok(const char *what, int good) {
    printf("%s   %s\n", good ? "ok  " : "FAIL", what);
    if(!good) fails++;
}

int main(void) {
    const uint8_t *ci_der; size_t ci_len;
    ok("the test CI is compiled in", rsp_pki_test_ci(&ci_der, &ci_len) == 0);

    rsp_credential_t dp;
    memset(&dp, 0, sizeof dp);
    ok("minting a DPauth credential succeeds",
       rsp_pki_mint("euicc-tools test DPauth", 0, &dp) == 0);
    ok("the credential carries a certificate", dp.der && dp.der_len > 0);

    mbedtls_x509_crt ci, leaf;
    mbedtls_x509_crt_init(&ci);
    mbedtls_x509_crt_init(&leaf);
    ok("the CI parses", mbedtls_x509_crt_parse_der(&ci, ci_der, ci_len) == 0);
    ok("the minted certificate parses",
       mbedtls_x509_crt_parse_der(&leaf, dp.der, dp.der_len) == 0);

    uint32_t flags = 0;
    ok("the chain verifies against the test CI",
       mbedtls_x509_crt_verify(&leaf, &ci, NULL, NULL, &flags, NULL, NULL) == 0);

    ok("the key is on P-256",
       mbedtls_pk_get_bitlen(&leaf.pk) == 256);

    mbedtls_x509_crt_free(&leaf);
    mbedtls_x509_crt_free(&ci);
    rsp_credential_free(&dp);
    return fails ? 1 : 0;
}
```

- [ ] **Step 3: Run it and watch it fail**

Run: `make check`

Expected: the compile fails on `rsp_pki_test_ci` being undeclared.

- [ ] **Step 4: Implement `rsp_pki`**

Add the three declarations and the struct from the Interfaces block to
`include/rsp.h`. Then write `src/rsp_pki.c` using mbedTLS:

- Embed `testdata/sgp26/ci.der` and `ci-key.der` as byte arrays. Generate the
  arrays with `xxd -i` at build time into `build/sgp26_material.c`, so the
  DER files stay the single source and no one hand-edits a C array.
- `rsp_pki_mint` generates a P-256 key pair with `mbedtls_ecp_gen_key`, then
  builds the certificate with the `mbedtls_x509write_crt_*` family: set the
  subject and issuer names, the serial, the validity, the subject key, and
  sign with the CI key through `mbedtls_x509write_crt_set_issuer_key`.
- The certificate profile of SGP.22 requires specific extensions and a policy
  OID per role. Take them from SGP.22 Annex A at implementation time; the
  test above proves the chain, and the card proves the profile in the second
  half of the project.

Add to the Makefile, before `$(LIB)`:

```make
build/sgp26_material.c: testdata/sgp26/ci.der testdata/sgp26/ci-key.der
	@mkdir -p build
	@{ echo '/* Generated from testdata/sgp26. Do not edit. */'; \
	   echo '#include <stddef.h>'; \
	   xxd -i -n rsp_sgp26_ci_der      < testdata/sgp26/ci.der; \
	   xxd -i -n rsp_sgp26_ci_key_der  < testdata/sgp26/ci-key.der; \
	 } > $@

SRCS    := $(wildcard src/*.c) build/sgp26_material.c
```

- [ ] **Step 5: Run the test and watch it pass**

Run: `make check`

Expected: seven `ok` lines from `run-pki`.

- [ ] **Step 6: Prove the test can fail**

Flip one byte in the middle of the embedded CI key by editing
`testdata/sgp26/ci-key.der` in a scratch copy, rebuild, and confirm that
"the chain verifies against the test CI" goes red. Restore the file.

- [ ] **Step 7: Commit**

```bash
git add testdata Makefile src/rsp_pki.c include/rsp.h tests/test_pki.c
git commit -m "feat: mint DP certificates under the published SGP.26 test CI

The card refuses anything it cannot chain to a CI it knows, so this is
the gate before any crypto matters. The test is a real verification with
mbedTLS rather than a shape check: a wrong curve or a missing extension
shows here, instead of at the card, where the answer is a status word.
The material is labelled as test-only in three places, because a public
private key invites exactly one mistake."
```

---

### Task 4: Session keys from the one-time key agreement

**Files:**
- Create: `~/git/waigel/euicc-rsp/src/rsp_crypto.c`
- Modify: `~/git/waigel/euicc-rsp/include/rsp.h`
- Create: `~/git/waigel/euicc-rsp/tests/test_kdf.c`

**Interfaces:**
- Consumes: the build and the harness.
- Produces:

```c
typedef struct {
    uint8_t s_enc[16];
    uint8_t s_mac[16];
    uint8_t chain[16];   /* the MAC chaining value; rsp_protect advances it */
} rsp_session_t;

/* ECDH on P-256. pk is an uncompressed point, 65 bytes starting with 0x04.
   Writes the 32-byte x coordinate of the shared point. Returns 0 or -1. */
int rsp_ecdh_p256(const uint8_t sk[32], const uint8_t pk[65], uint8_t z[32]);

/* The X9.63 key derivation with SHA-256. Returns 0 or -1. */
int rsp_kdf_x963(const uint8_t *z, size_t z_len,
                 const uint8_t *info, size_t info_len,
                 uint8_t *out, size_t out_len);

/* Both together, filling a session. Returns 0 or -1. */
int rsp_session_init(const uint8_t otsk_dp[32], const uint8_t otpk_euicc[65],
                     const uint8_t *shared_info, size_t shared_info_len,
                     rsp_session_t *out);
```

- [ ] **Step 1: Place a published ECDH vector as a file**

Hard-coded byte arrays in a test invite two mistakes: a typo that nobody
sees, and a copy that nobody can trace to a source. Put the vector in a file
instead, one hex string per line, in this order: the private scalar of party
A, the uncompressed public point of party B, the expected shared x
coordinate.

Take one case from the NIST CAVP key-agreement vectors for P-256, or from
any published P-256 ECDH test case that gives all three values. Write it to
`testdata/nist/ecdh-p256.txt`:

```
<64 hex chars: the private scalar of A>
<130 hex chars: 04 followed by the public point of B>
<64 hex chars: the expected shared x coordinate>
```

Verify the shape before continuing:

```bash
awk 'NR==1&&length($0)!=64{e=1} NR==2&&length($0)!=130{e=1} NR==3&&length($0)!=64{e=1}
     END{exit (NR==3 && !e) ? 0 : 1}' testdata/nist/ecdh-p256.txt \
  && echo "the vector has the right shape"
```

Add a `testdata/nist/README.md` naming the exact document and test case the
values came from. A vector whose origin nobody can check is a magic number.

- [ ] **Step 2: Write the failing test**

Three levels. ECDH is pinned to the published vector. Symmetry is a property
that needs no vector at all. The derivation gets a regression check, and the
test says so plainly rather than implying more confidence than exists.

`tests/test_kdf.c`:

```c
/* ECDH is pinned to a published vector, and separately to the property that
   both parties reach the same secret. The derivation is pinned only to its
   own output: that catches a change, not a mistake. The card settles the
   derivation in the second half of the project. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsp.h"

static int fails;
static void ok(const char *what, int good) {
    printf("%s   %s\n", good ? "ok  " : "FAIL", what);
    if(!good) fails++;
}

/* One hex line from the vector file into bytes. Returns the byte count. */
static size_t hexline(FILE *f, uint8_t *out, size_t cap) {
    char line[300];
    if(!fgets(line, sizeof line, f)) return 0;
    size_t n = strspn(line, "0123456789abcdefABCDEF") / 2;
    if(n > cap) return 0;
    for(size_t i = 0; i < n; i++) {
        unsigned v;
        if(sscanf(line + 2 * i, "%2x", &v) != 1) return 0;
        out[i] = (uint8_t)v;
    }
    return n;
}

int main(void) {
    uint8_t sk[32], pk[65], want[32], z[32];
    FILE *f = fopen("testdata/nist/ecdh-p256.txt", "r");
    ok("the vector file is readable", f != NULL);
    if(!f) return 1;
    ok("the vector holds three values of the right length",
       hexline(f, sk, sizeof sk) == 32
       && hexline(f, pk, sizeof pk) == 65
       && hexline(f, want, sizeof want) == 32);
    fclose(f);

    ok("ECDH P-256 matches the published vector",
       rsp_ecdh_p256(sk, pk, z) == 0 && memcmp(z, want, 32) == 0);

    /* The derivation is deterministic: the same inputs always give the same
       keys, whatever the right answer turns out to be. */
    static const uint8_t info[]  = { 0x00, 0x00, 0x00, 0x01 };
    static const uint8_t info2[] = { 0x00, 0x00, 0x00, 0x02 };
    rsp_session_t a, b, c;
    ok("session derivation succeeds",
       rsp_session_init(sk, pk, info, sizeof info, &a) == 0);
    ok("session derivation repeats",
       rsp_session_init(sk, pk, info, sizeof info, &b) == 0);
    ok("the two derivations agree", memcmp(&a, &b, sizeof a) == 0);

    /* The three keys must differ. A derivation that returns the same block
       three times passes every other check in this file. */
    ok("S-ENC and S-MAC differ", memcmp(a.s_enc, a.s_mac, 16) != 0);
    ok("S-MAC and the chaining value differ", memcmp(a.s_mac, a.chain, 16) != 0);

    ok("a different sharedInfo gives different keys",
       rsp_session_init(sk, pk, info2, sizeof info2, &c) == 0
       && memcmp(&a, &c, sizeof a) != 0);
    return fails ? 1 : 0;
}
```

- [ ] **Step 3: Run it and watch it fail**

Run: `make check`

Expected: compile failure on `rsp_ecdh_p256` being undeclared.

- [ ] **Step 4: Implement the two primitives**

In `src/rsp_crypto.c`:

- `rsp_ecdh_p256`: load `MBEDTLS_ECP_DP_SECP256R1` with
  `mbedtls_ecp_group_load`, read the point with `mbedtls_ecp_point_read_binary`,
  the scalar with `mbedtls_mpi_read_binary`, then `mbedtls_ecdh_compute_shared`,
  and write the result with `mbedtls_mpi_write_binary` into 32 bytes.
- `rsp_kdf_x963`: the X9.63 construction is a counter loop. For
  `i = 1, 2, 3, ...` compute `SHA-256(Z || counter_be32(i) || info)` and
  concatenate until `out_len` bytes exist. Use `mbedtls_sha256`.
- `rsp_session_init`: ECDH, then derive 48 bytes, then split them into
  `s_enc`, `s_mac` and `chain` in the order SGP.22 §2.6.4 states. Confirm the
  order and the sharedInfo composition against the clause; the split below is
  the shape, not the authority.

Add `tests/run-kdf` to `TESTS`.

- [ ] **Step 5: Run the test and watch it pass**

Run: `make check`

Expected: nine `ok` lines from `run-kdf`.

- [ ] **Step 6: Prove the test can fail**

Change the last hex digit of the third line in `testdata/nist/ecdh-p256.txt`.
"ECDH P-256 matches the published vector" must go red while the shape check
stays green. Restore the digit.

- [ ] **Step 7: Commit**

```bash
git add src/rsp_crypto.c include/rsp.h Makefile tests/test_kdf.c testdata/nist
git commit -m "feat: session keys from the one-time key agreement

ECDH is pinned to a published NIST vector, so that half is anchored
outside this repository. The derivation is pinned to its own output,
which catches a change and not a mistake -- the test says so in a
comment rather than implying more confidence than exists. The card
settles it in the second half."
```

---

### Task 5: SCP03t, protect and unprotect

**Files:**
- Modify: `~/git/waigel/euicc-rsp/src/rsp_crypto.c`
- Modify: `~/git/waigel/euicc-rsp/include/rsp.h`
- Create: `~/git/waigel/euicc-rsp/tests/test_scp03t.c`

**Interfaces:**
- Consumes: `rsp_session_t`, `rsp_session_init` from Task 4.
- Produces:

```c
/* Protect one ES8+ command into one SCP03t segment. Advances s->chain.
   Returns the number of bytes written to out, or -1. */
long rsp_protect(rsp_session_t *s, const uint8_t *plain, size_t plain_len,
                 uint8_t *out, size_t out_cap);

/* The inverse, for the round trip and for the self-check before sending.
   Advances s->chain the same way. Returns bytes written, or -1 when the MAC
   does not match. */
long rsp_unprotect(rsp_session_t *s, const uint8_t *seg, size_t seg_len,
                   uint8_t *out, size_t out_cap);

/* AES-CMAC over one block chain, exposed because it has published vectors
   and therefore deserves its own test. Returns 0 or -1. */
int rsp_cmac(const uint8_t key[16], const uint8_t *msg, size_t len,
             uint8_t mac[16]);
```

- [ ] **Step 1: Write the failing test**

`tests/test_scp03t.c`:

```c
/* Two things at once. CMAC is pinned to a published vector, because it has
   one. The framing is proven by inversion: whatever protect() builds,
   unprotect() must give back exactly what went in. That catches padding,
   segmentation and the chaining, which is where these implementations
   actually break. */
#include <stdio.h>
#include <string.h>
#include "rsp.h"

static int fails;
static void ok(const char *what, int good) {
    printf("%s   %s\n", good ? "ok  " : "FAIL", what);
    if(!good) fails++;
}

/* NIST SP 800-38B, AES-128 CMAC, the empty-message case. */
static const uint8_t CMAC_KEY[16] = {
    0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c
};
static const uint8_t CMAC_EMPTY[16] = {
    0xbb,0x1d,0x69,0x29,0xe9,0x59,0x37,0x28,0x7f,0xa3,0x7d,0x12,0x9b,0x75,0x67,0x46
};

static void session(rsp_session_t *s) {
    memset(s, 0, sizeof *s);
    for(int i = 0; i < 16; i++) { s->s_enc[i] = i; s->s_mac[i] = 0x40 + i; }
}

int main(void) {
    uint8_t mac[16];
    ok("AES-128 CMAC matches SP 800-38B for the empty message",
       rsp_cmac(CMAC_KEY, NULL, 0, mac) == 0 && memcmp(mac, CMAC_EMPTY, 16) == 0);

    /* Inversion, over lengths that straddle the block size: an off-by-one in
       the padding hides at exactly 16 bytes and nowhere else. */
    static const size_t lens[] = { 1, 15, 16, 17, 255, 256, 1024 };
    for(size_t i = 0; i < sizeof lens / sizeof lens[0]; i++) {
        size_t n = lens[i];
        uint8_t plain[1024], seg[2048], back[2048];
        for(size_t k = 0; k < n; k++) plain[k] = (uint8_t)(k * 7 + 1);

        rsp_session_t s1, s2;
        session(&s1); session(&s2);

        long enc = rsp_protect(&s1, plain, n, seg, sizeof seg);
        long dec = enc > 0 ? rsp_unprotect(&s2, seg, (size_t)enc, back, sizeof back) : -1;

        char what[64];
        snprintf(what, sizeof what, "%zu bytes survive protect and unprotect", n);
        ok(what, dec == (long)n && memcmp(plain, back, n) == 0);

        snprintf(what, sizeof what, "%zu bytes: the chaining value advanced", n);
        ok(what, memcmp(s1.chain, s2.chain, 16) == 0
                 && memcmp(s1.chain, (uint8_t[16]){0}, 16) != 0);
    }

    /* A tampered segment must be refused, not silently decrypted. */
    {
        uint8_t plain[32], seg[256], back[256];
        memset(plain, 0xA5, sizeof plain);
        rsp_session_t s1, s2;
        session(&s1); session(&s2);
        long enc = rsp_protect(&s1, plain, sizeof plain, seg, sizeof seg);
        ok("a segment was produced", enc > 0);
        seg[enc / 2] ^= 0x01;
        ok("a tampered segment is refused",
           rsp_unprotect(&s2, seg, (size_t)enc, back, sizeof back) < 0);
    }
    return fails ? 1 : 0;
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `make check`

Expected: compile failure on `rsp_cmac` and `rsp_protect` being undeclared.

- [ ] **Step 3: Implement the three functions**

In `src/rsp_crypto.c`:

- `rsp_cmac`: `mbedtls_cipher_cmac` with
  `mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB)`.
- `rsp_protect`: pad the plaintext per SGP.22 §5.5.2, derive the ICV from the
  chaining value, encrypt with `mbedtls_aes_crypt_cbc`, compute the MAC over
  the chaining value and the ciphertext, append the leading bytes of the MAC,
  and set `s->chain` to the full MAC. Take the tag, the length encoding and
  the MAC length from the clause.
- `rsp_unprotect`: the inverse, and it verifies the MAC **before** it strips
  the padding. Compare with `memcmp` on a fixed length; do not return early
  on the first differing byte.

Add `tests/run-scp03t` to `TESTS`.

- [ ] **Step 4: Run the test and watch it pass**

Run: `make check`

Expected: one CMAC line, fourteen inversion lines, two tamper lines, all `ok`.

- [ ] **Step 5: Prove the test can fail**

In `rsp_unprotect`, skip the MAC comparison temporarily. "a tampered segment
is refused" must go red while every inversion line stays green — which is the
proof that the tamper case tests something the inversion does not. Restore
the comparison.

- [ ] **Step 6: Commit**

```bash
git add src/rsp_crypto.c include/rsp.h Makefile tests/test_scp03t.c
git commit -m "feat: SCP03t protection, proven by inversion

CMAC has a published vector and gets one. The framing has none, so the
proof is that unprotect returns exactly what protect was given, across
lengths that straddle the block size -- a padding off-by-one hides at
exactly 16 bytes. A tampered segment must be refused, and that case is
verified to fail on its own when the MAC check is removed."
```

---

### Task 6: Build the Bound Profile Package, and the round trip that guards it

**Files:**
- Create: `~/git/waigel/euicc-rsp/src/rsp_bpp.c`
- Modify: `~/git/waigel/euicc-rsp/include/rsp.h`
- Create: `~/git/waigel/euicc-rsp/tests/test_bpp.c`
- Create: `~/git/waigel/euicc-rsp/testdata/profile.der`

**Interfaces:**
- Consumes: `rsp_session_t`, `rsp_protect`, `rsp_unprotect` from Tasks 4 and
  5; the generated `BoundProfilePackage_t` from Task 2.
- Produces:

```c
typedef struct {
    const uint8_t *upp;        /* the profile package, DER */
    size_t         upp_len;
    const uint8_t *otpk_dp;    /* our one-time public key, 65 bytes */
    const uint8_t *iccid;      /* 10 bytes */
    const char    *profile_name;
    const char    *service_provider_name;
} rsp_bpp_input_t;

/* Build the BPP. *out is malloc'ed and belongs to the caller. The session is
   advanced as segments are protected. Returns 0 or -1. */
int rsp_bpp_build(rsp_session_t *s, const rsp_bpp_input_t *in,
                  uint8_t **out, size_t *out_len);

/* Recover the UPP from a BPP with the same session keys. This exists for the
   self-check before sending and for the test below. Returns 0 or -1. */
int rsp_bpp_recover(rsp_session_t *s, const uint8_t *bpp, size_t bpp_len,
                    uint8_t **upp, size_t *upp_len);
```

- [ ] **Step 1: Place a real profile as the input**

```bash
cd ~/git/waigel/euicc-rsp
euicc build ~/git/waigel/euicc-tools/editors/vscode/examples/profile.vn \
      -o testdata/profile.der
euicc check testdata/profile.der && echo "the input is a clean package"
```

Using the example profile is deliberate: it is the package that
`euicc check -s` accepts without a finding, so a failure in this task is
about the binding and never about the profile.

- [ ] **Step 2: Write the failing test**

`tests/test_bpp.c`:

```c
/* The whole first half in one assertion: a real profile goes in, a BPP comes
   out, and the same session keys give the profile back byte for byte. What
   this does not prove is that a card agrees -- that is the second half, and
   the first BPP a card accepts becomes the golden vector here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsp.h"

static int fails;
static void ok(const char *what, int good) {
    printf("%s   %s\n", good ? "ok  " : "FAIL", what);
    if(!good) fails++;
}

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if(!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc((size_t)n);
    if(p && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f);
    if(p) *len = (size_t)n;
    return p;
}

static void session(rsp_session_t *s) {
    memset(s, 0, sizeof *s);
    for(int i = 0; i < 16; i++) { s->s_enc[i] = i; s->s_mac[i] = 0x40 + i; }
}

int main(void) {
    size_t upp_len = 0;
    uint8_t *upp = slurp("testdata/profile.der", &upp_len);
    ok("the test profile is readable", upp && upp_len > 0);
    if(!upp) return 1;

    static const uint8_t iccid[10] = {
        0x98,0x00,0x10,0x32,0x54,0x76,0x98,0x10,0x32,0x14
    };
    uint8_t otpk[65]; memset(otpk, 0, sizeof otpk); otpk[0] = 0x04;

    rsp_bpp_input_t in = {
        .upp = upp, .upp_len = upp_len, .otpk_dp = otpk, .iccid = iccid,
        .profile_name = "example", .service_provider_name = "euicc-tools"
    };

    rsp_session_t s1, s2;
    session(&s1); session(&s2);

    uint8_t *bpp = NULL; size_t bpp_len = 0;
    ok("the BPP is built", rsp_bpp_build(&s1, &in, &bpp, &bpp_len) == 0);
    ok("the BPP is larger than the profile", bpp_len > upp_len);

    uint8_t *back = NULL; size_t back_len = 0;
    ok("the profile is recovered",
       rsp_bpp_recover(&s2, bpp, bpp_len, &back, &back_len) == 0);
    ok("the recovered profile has the original length", back_len == upp_len);
    ok("the recovered profile is identical",
       back && memcmp(upp, back, upp_len) == 0);

    /* A BPP built with different keys must not decrypt with these. */
    rsp_session_t s3; session(&s3); s3.s_enc[0] ^= 0xFF;
    uint8_t *other = NULL; size_t other_len = 0;
    rsp_session_t s4; session(&s4);
    ok("a BPP under different keys is refused",
       rsp_bpp_build(&s3, &in, &other, &other_len) == 0
       && rsp_bpp_recover(&s4, other, other_len, &back, &back_len) < 0);

    free(upp); free(bpp); free(other);
    return fails ? 1 : 0;
}
```

- [ ] **Step 3: Run it and watch it fail**

Run: `make check`

Expected: compile failure on `rsp_bpp_build` being undeclared.

- [ ] **Step 4: Implement the assembly**

In `src/rsp_bpp.c`:

- Build `initialiseSecureChannel` with our one-time public key, then
  `configureISDP`, then `storeMetadata` from the input fields, using the
  generated types from Task 2.
- Split the UPP into segments no larger than the limit SGP.22 §2.5.3 states,
  protect each with `rsp_protect`, and place them in the sequence.
- Encode the whole `BoundProfilePackage` with `der_encode`.
- `rsp_bpp_recover` walks the segments, calls `rsp_unprotect` on each, and
  concatenates. It returns -1 as soon as one segment fails its MAC.

Add `tests/run-bpp` to `TESTS`. The test reads `testdata/profile.der` from
the working directory, so `check` runs from the repository root, which it
already does.

- [ ] **Step 5: Run the test and watch it pass**

Run: `make check`

Expected: seven `ok` lines from `run-bpp`, and every earlier suite still green.

- [ ] **Step 6: Prove the test can fail**

In `rsp_bpp_build`, drop the last segment before encoding. "the recovered
profile has the original length" must go red. Restore it.

- [ ] **Step 7: Commit and wire CI**

```bash
git add src/rsp_bpp.c include/rsp.h Makefile tests/test_bpp.c testdata/profile.der
git commit -m "feat: build a Bound Profile Package, guarded by its own inverse

A real profile goes in -- the example package that euicc check -s
accepts without a finding -- and the same session keys give it back
byte for byte. That proves the segmentation, the padding and the MAC
chaining without a card. What it does not prove is that a card agrees;
the first BPP a card accepts becomes the golden vector here."
git push
```

Confirm CI is green on the pushed commit before declaring the first half
done:

```bash
gh run watch "$(gh run list --limit 1 --json databaseId -q '.[0].databaseId')" --exit-status
```

---

### Task 7: The handshake signatures

Independent of Tasks 4 to 6: it needs the codec and the certificates, not the
session. It can be built at any point after Task 3.

**Files:**
- Create: `~/git/waigel/euicc-rsp/src/rsp_sign.c`
- Modify: `~/git/waigel/euicc-rsp/include/rsp.h`
- Create: `~/git/waigel/euicc-rsp/tests/test_sign.c`

**Interfaces:**
- Consumes: `rsp_credential_t`, `rsp_pki_mint`, `rsp_pki_test_ci` from Task 3;
  the generated types from Task 2.
- Produces:

```c
/* Sign a DER-encoded structure with a credential. The signature is written
   as the plain 64-byte r||s pair that SGP.22 uses, not as a DER SEQUENCE.
   Returns 0 or -1. */
int rsp_sign(const rsp_credential_t *c, const uint8_t *tbs, size_t tbs_len,
             uint8_t sig[64]);

/* Verify such a signature against the public key inside a certificate.
   Returns 0 when it holds, -1 when it does not. */
int rsp_verify(const uint8_t *cert_der, size_t cert_len,
               const uint8_t *tbs, size_t tbs_len, const uint8_t sig[64]);
```

- [ ] **Step 1: Write the failing test**

There is no card here, so the test plays both sides: it mints a credential
that stands in for the eUICC, signs with it, and verifies with the
certificate. That covers steps 4, 6 and 7 of the flow without hardware.

`tests/test_sign.c`:

```c
/* Both sides of the handshake, played locally. A signature that verifies
   against the wrong key, or a tampered message that still verifies, is the
   failure that matters -- and neither shows up if the test only signs and
   checks the happy path. */
#include <stdio.h>
#include <string.h>
#include "rsp.h"

static int fails;
static void ok(const char *what, int good) {
    printf("%s   %s\n", good ? "ok  " : "FAIL", what);
    if(!good) fails++;
}

int main(void) {
    rsp_credential_t dp, card;
    memset(&dp, 0, sizeof dp);
    memset(&card, 0, sizeof card);
    ok("a DP credential is minted", rsp_pki_mint("test DPauth", 0, &dp) == 0);
    ok("a stand-in eUICC credential is minted",
       rsp_pki_mint("test eUICC", 1, &card) == 0);

    static const uint8_t msg[] = "serverSigned1 stands in for the real one";
    uint8_t sig[64];

    ok("signing succeeds", rsp_sign(&dp, msg, sizeof msg - 1, sig) == 0);
    ok("the signature verifies with the matching certificate",
       rsp_verify(dp.der, dp.der_len, msg, sizeof msg - 1, sig) == 0);
    ok("it does not verify with another certificate",
       rsp_verify(card.der, card.der_len, msg, sizeof msg - 1, sig) != 0);

    uint8_t bad[sizeof msg - 1];
    memcpy(bad, msg, sizeof bad);
    bad[0] ^= 0x01;
    ok("a tampered message does not verify",
       rsp_verify(dp.der, dp.der_len, bad, sizeof bad, sig) != 0);

    sig[0] ^= 0x01;
    ok("a tampered signature does not verify",
       rsp_verify(dp.der, dp.der_len, msg, sizeof msg - 1, sig) != 0);

    /* ECDSA is randomised: two signatures over the same message differ, and
       both must verify. An implementation that returns a constant passes
       every check above. */
    uint8_t sig2[64], sig3[64];
    rsp_sign(&dp, msg, sizeof msg - 1, sig2);
    rsp_sign(&dp, msg, sizeof msg - 1, sig3);
    ok("two signatures over the same message differ",
       memcmp(sig2, sig3, 64) != 0);
    ok("both of them verify",
       rsp_verify(dp.der, dp.der_len, msg, sizeof msg - 1, sig2) == 0
       && rsp_verify(dp.der, dp.der_len, msg, sizeof msg - 1, sig3) == 0);

    rsp_credential_free(&dp);
    rsp_credential_free(&card);
    return fails ? 1 : 0;
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `make check`

Expected: compile failure on `rsp_sign` being undeclared.

- [ ] **Step 3: Implement signing and verification**

In `src/rsp_sign.c`:

- `rsp_sign`: hash the input with `mbedtls_sha256`, sign with
  `mbedtls_ecdsa_sign` on `MBEDTLS_ECP_DP_SECP256R1`, then write `r` and `s`
  with `mbedtls_mpi_write_binary` into 32 bytes each. SGP.22 carries the pair
  plain, so do not wrap it in a DER SEQUENCE.
- `rsp_verify`: parse the certificate with `mbedtls_x509_crt_parse_der`, take
  the key out of `crt.pk`, read `r` and `s` back from the 64 bytes, and call
  `mbedtls_ecdsa_verify`.
- Both need an entropy source for the nonce: `mbedtls_ctr_drbg_seed` over
  `mbedtls_entropy_func`, set up once in a static initialiser guarded so it
  runs a single time.

Add `tests/run-sign` to `TESTS`.

- [ ] **Step 4: Run the test and watch it pass**

Run: `make check`

Expected: ten `ok` lines from `run-sign`.

- [ ] **Step 5: Prove the test can fail**

In `rsp_verify`, return 0 unconditionally. Four lines must go red at once:
the two wrong-key and tampered cases, and both negative checks. That the
happy-path lines stay green is the point — they never tested anything on
their own. Restore the implementation.

- [ ] **Step 6: Commit**

```bash
git add src/rsp_sign.c include/rsp.h Makefile tests/test_sign.c
git commit -m "feat: the handshake signatures, with both sides played locally

Steps 4, 6 and 7 of the flow need no card to prove: mint a credential
that stands in for the eUICC, sign with one side, verify with the other.
The negative cases carry the weight -- a verify that always returns
true passes every happy-path assertion, so the test checks that wrong
keys, tampered messages and tampered signatures are all refused."
```

---

## What the second half adds

Not in this plan, listed so no one looks for it: `rsp_card` (PC/SC, ISD-R,
the ES10b commands), `rsp_flow` (the order of the procedure), the recorded
card for CI, the `euicc card`, `euicc pki` and `euicc flash` commands in
`euicc-tools`, and the golden vector that replaces the regression vector in
Task 4.
