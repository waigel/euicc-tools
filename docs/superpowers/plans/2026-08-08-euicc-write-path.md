# The write path (Plan B) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `euicc card install profile.der` puts a profile on the test eUICC, and `euicc card profiles` then lists it.

**Architecture:** The SM-DP+ role (`euicc-rsp`) gains the three ES9+ functions and the ability to build a BPP a card will accept. The LPA role (`euicc-lpa`) gains the four ES10b functions, profile deletion, session cancellation, and the sequencing that alternates between card and server. `euicc-tools` gains two commands. The ES9+ boundary is real DER in and DER out, called in-process.

**Tech Stack:** C99, mbedTLS (ECDH P-256, AES, CMAC, ECDSA), asn1c 0.9.29 generated codec from `rsp-2.5.asn`, PC/SC, POSIX sh.

## Global Constraints

- Spec: [2026-08-07-euicc-write-round-design.md](../specs/2026-08-07-euicc-write-round-design.md). Plan A landed; this is Plan B.
- Every clause citation is SGP.22 v2.6 and must be checked against the specification text, not recalled. GetProfilesInfo is §5.7.15; §5.7.16 is EnableProfile. Getting this wrong has already happened once.
- C99: `-std=c99 -Wall -Wextra -Wno-unused-parameter -Werror=implicit-function-declaration -Werror=int-conversion`, `-D_DEFAULT_SOURCE`, `-D_DARWIN_C_SOURCE` on Darwin.
- Failure convention, already documented in both headers: `0` success, `-1` the question was asked and the answer is no, `-2` the question was never reached. Every new function that can fail both ways says so at its own declaration.
- Secrets are wiped with `mbedtls_platform_zeroize`, never `memset` — a `memset` on a buffer about to be freed is dead-store-eliminated at `-O2`, proven in this project's own history.
- No Python, no `xxd`, POSIX `sh` only.
- Commits signed. The 1Password agent is intermittent: retry in a loop, never `--no-gpg-sign`. Verify with `git cat-file commit HEAD | grep gpgsig`; `git log --show-signature` reports "no signature" for everything here because `gpg.ssh.allowedSignersFile` is unset.
- Check git's own exit status, never a pipeline's.
- Push with `GIT_SSH_COMMAND='ssh -o BatchMode=yes -o ConnectTimeout=15'`, retrying.
- Test counts may only go up. Baseline: euicc-rsp 112, euicc-lpa 127, euicc-tools 100.
- **Installing a profile changes the card.** Before the first run against real hardware, confirm with the human partner. A profile that somehow becomes enabled cannot be deleted by this round's tooling.

---

## The exchange, for reference

| # | Who | Call | Yields |
|---|---|---|---|
| 1 | lpa → card | `GetEUICCInfo` §5.7.8 | `euiccInfo1` |
| 2 | lpa → card | `GetEUICCChallenge` §5.7.7 | 16-byte challenge |
| 3 | lpa → dp | `InitiateAuthentication` §5.6.1 | `transactionId`, `serverSigned1`, `serverSignature1`, CERT.DPauth |
| 4 | lpa → card | `AuthenticateServer` §5.7.13 | `euiccSigned1`, `euiccSignature1`, CERT.EUICC, CERT.EUM |
| 5 | lpa → dp | `AuthenticateClient` §5.6.3 | ProfileMetadata, `smdpSigned2`, `smdpSignature2`, CERT.DPpb |
| 6 | lpa → card | `PrepareDownload` §5.7.5 | `euiccSigned2` carrying **otPK.EUICC.ECKA**, `euiccSignature2` |
| 7 | lpa → dp | `GetBoundProfilePackage` §5.6.2 | the BPP |
| 8 | lpa → card | `LoadBoundProfilePackage` §5.7.6 | `ProfileInstallationResult` |

Step 6 is the hinge: the card's one-time key does not exist before it, so nothing cryptographic about the BPP can exist before step 7.

---

### Task 1: Deterministic ECDSA

Nothing else in this plan can be recorded and replayed until signatures are a pure function of key and message. `src/rsp_sign.c` currently calls `mbedtls_ecdsa_sign` with a globally seeded DRBG, so the same input signs differently every time.

This is also the safer primitive on its own merits: with a random `k`, one repeated or biased nonce reveals the signing key.

**Files:**
- Modify: `euicc-rsp/src/rsp_sign.c` (the `mbedtls_ecdsa_sign` call and the DRBG that feeds it)
- Modify: `euicc-rsp/include/rsp.h` — `rsp_sign`'s comment currently ends "Every call produces a different sig for the same tbs: mbedtls_ecdsa_sign draws a fresh nonce each time." That becomes false.
- Test: `euicc-rsp/tests/test_sign.c`

**Interfaces:**
- Consumes: nothing new.
- Produces: `int rsp_sign(const rsp_credential_t *c, const uint8_t *tbs, size_t tbs_len, uint8_t sig[64]);` — signature unchanged, behaviour now deterministic.

- [ ] **Step 1: Write the failing test**

Add to `euicc-rsp/tests/test_sign.c`:

```c
/* RFC 6979 makes a signature a pure function of (key, message). Until it
   did, no recording of a session that signs anything could replay: the
   bytes differed every run. This is also the safer primitive -- with a
   random k, a single repeated or biased nonce reveals the private key.

   Both halves matter. Determinism without validity would be satisfied by
   returning a constant; validity without determinism is what we had. */
static void test_signing_is_deterministic(void) {
    rsp_credential_t dp;
    ok("DPpb loads", rsp_pki_dp(1, &dp) == 0);

    static const uint8_t tbs[] = {
        0xBF, 0x37, 0x04, 'h', 'e', 'r', 'e'
    };
    uint8_t a[64], b[64];
    ok("first signature succeeds", rsp_sign(&dp, tbs, sizeof tbs, a) == 0);
    ok("second signature succeeds", rsp_sign(&dp, tbs, sizeof tbs, b) == 0);
    ok("the same key and message sign identically",
       memcmp(a, b, sizeof a) == 0);
    ok("and the signature still verifies",
       rsp_sign_verify(dp.der, dp.der_len, tbs, sizeof tbs, a) == 0);

    /* A different message must not produce the same bytes -- otherwise
       "deterministic" would be satisfied by ignoring the input. */
    static const uint8_t other[] = {
        0xBF, 0x37, 0x04, 'h', 'e', 'r', 'f'
    };
    uint8_t c[64];
    ok("a different message signs differently",
       rsp_sign(&dp, other, sizeof other, c) == 0
       && memcmp(a, c, sizeof a) != 0);

    rsp_credential_free(&dp);
}
```

Call it from `main`.

- [ ] **Step 2: Run it to verify it fails**

Run: `cd ~/git/waigel/euicc-rsp && make check 2>&1 | grep -A1 determinis`
Expected: `FAIL   the same key and message sign identically`

- [ ] **Step 3: Switch to RFC 6979**

In `src/rsp_sign.c`, replace the `mbedtls_ecdsa_sign` call with:

```c
        mbedtls_ecdsa_sign_det_ext(&grp, &r, &s, &d, hash, sizeof hash,
                                    MBEDTLS_MD_SHA256,
                                    mbedtls_ctr_drbg_random, &g_drbg) == 0 &&
```

`mbedtls_ecdsa_sign_det_ext` still takes an RNG, but uses it only for blinding, not for the nonce — the nonce comes from RFC 6979's HMAC-DRBG over the key and message. Keep the existing `g_drbg` plumbing; it is still needed.

Rewrite the comment above the call. It currently explains why an RNG is needed for `k`; that is no longer the reason. Say what RFC 6979 does, that the RNG that remains is for blinding, and why determinism is wanted here (replayable recordings) as well as safer (no nonce reuse).

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd ~/git/waigel/euicc-rsp && make check`
Expected: 112 + 5 = 117 ok, 0 failed.

- [ ] **Step 5: Correct the header**

`include/rsp.h`'s `rsp_sign` comment ends with "Every call produces a different sig for the same tbs: mbedtls_ecdsa_sign draws a fresh nonce each time." Replace with the RFC 6979 statement and the two reasons.

- [ ] **Step 6: Prove the test can fail, both halves**

Two mutations, each applied, run, restored:

(a) revert to `mbedtls_ecdsa_sign` → `FAIL the same key and message sign identically`
(b) make `rsp_sign` return a constant 64 bytes → `FAIL and the signature still verifies` **and** `FAIL a different message signs differently`

(b) is the one that matters: it proves the determinism assertion is not satisfiable by a stub.

- [ ] **Step 7: Commit**

```bash
cd ~/git/waigel/euicc-rsp
git add include/rsp.h src/rsp_sign.c tests/test_sign.c
git commit -m "feat: sign deterministically, so a session can be replayed"
```

Push, then bump `euicc-lpa`'s submodule pointer and push that too.

---

### Task 2: The ES9+ message types and `InitiateAuthentication`

**Files:**
- Create: `euicc-rsp/src/rsp_es9.c`
- Modify: `euicc-rsp/include/rsp.h`
- Test: `euicc-rsp/tests/test_es9.c` (new; the Makefile's `tests/run-%` pattern picks it up)

**Interfaces:**
- Consumes: `rsp_pki_dp`, `rsp_sign` (now deterministic), the generated `AuthenticateServerRequest`, `ServerSigned1`, `EUICCInfo1` types from `dist/`.
- Produces:

```c
/* One RSP session's server-side state, from InitiateAuthentication to
   GetBoundProfilePackage. Carries the transactionId every later step must
   echo, and the eUICC identity learned in step 4. Secret once the session
   keys land in it; wipe with rsp_dp_session_free. */
typedef struct rsp_dp_session rsp_dp_session_t;

int rsp_dp_initiate_authentication(
        const uint8_t *euicc_challenge, size_t challenge_len,
        const uint8_t *euicc_info1, size_t info1_len,
        const uint8_t transaction_id[16],
        rsp_dp_session_t **out,
        uint8_t **resp, size_t *resp_len);

void rsp_dp_session_free(rsp_dp_session_t *s);
```

`transaction_id` is a caller-supplied 16 bytes, not generated inside. Production passes fresh random; the test passes a fixed value, which is what makes a recording replay. There is no default, so there is no test path that can ship by accident.

- [ ] **Step 1: Read the clause before writing anything**

Read SGP.22 §5.6.1 in full. Write down, in the report: what `serverSigned1` contains, in what order, what `serverSignature1` is computed over, and with which key (DPauth, not DPpb). Do not proceed on memory.

- [ ] **Step 2: Write the failing test**

`tests/test_es9.c` pins the response's decoded structure against a fixed `transactionId` and a fixed challenge: that `serverSigned1.transactionId` equals what was passed in, that `serverSigned1.euiccChallenge` equals the challenge, that `serverSignature1` verifies against CERT.DPauth with `rsp_sign_verify`, and that the returned certificate is DPauth's and not DPpb's (compare `der_len` and the first 32 bytes against `rsp_pki_dp(0, …)`).

- [ ] **Step 3: Run it to verify it fails**

Expected: link error, `rsp_dp_initiate_authentication` undefined.

- [ ] **Step 4: Implement**

Build `ServerSigned1` with the generated type, DER-encode it, sign that encoding with DPauth, and assemble the response. Store `transactionId` in the session struct.

- [ ] **Step 5: Run the test to verify it passes**

- [ ] **Step 6: Prove it can fail — three mutations**

(a) sign with DPpb instead of DPauth → the certificate assertion or the verify assertion fails
(b) drop `euiccChallenge` from `serverSigned1` → the challenge assertion fails
(c) return a fixed `transactionId` regardless of the argument → the transactionId assertion fails

(c) is the important one: it proves the caller-supplied transactionId is actually used, which is what the whole replay strategy rests on.

- [ ] **Step 7: Commit**

---

### Task 3: `AuthenticateClient` and `GetBoundProfilePackage`

**Files:**
- Modify: `euicc-rsp/src/rsp_es9.c`, `euicc-rsp/include/rsp.h`
- Test: `euicc-rsp/tests/test_es9.c`

**Interfaces:**
- Consumes: Task 2's `rsp_dp_session_t`.
- Produces:

```c
int rsp_dp_authenticate_client(rsp_dp_session_t *s,
        const uint8_t *auth_server_resp, size_t resp_len,
        const uint8_t *metadata, size_t metadata_len,
        uint8_t **out, size_t *out_len);

int rsp_dp_get_bound_profile_package(rsp_dp_session_t *s,
        const uint8_t *prepare_download_resp, size_t resp_len,
        const uint8_t *upp, size_t upp_len,
        const uint8_t otsk_dp[32],
        uint8_t **bpp, size_t *bpp_len);
```

`otsk_dp` is caller-supplied for the same reason `transaction_id` was.

- [ ] **Step 1: Read §5.6.3 and §5.6.2, and record what you find**

In particular: what `AuthenticateClient` must verify about CERT.EUICC and CERT.EUM before trusting anything, and where the EID comes from. Write it in the report before implementing.

- [ ] **Step 2: Write the failing tests**

Three assertions that must be able to fail independently:
- an `authenticateServerResponse` whose `transactionId` does not match the session's is refused with `-1`, not accepted
- a CERT.EUICC that does not chain to the test CI is refused with `-1`
- on success, the EID recovered from `euiccSigned1` matches the one the test card reports (`89049032123451234512345678901235`)

- [ ] **Step 3: Run to verify they fail**

- [ ] **Step 4: Implement**

Verify the chain with `rsp_pki_verify`'s underlying primitive, check the transactionId, extract the EID, build `smdpSigned2`, sign with DPpb.

For `GetBoundProfilePackage`: extract `otPK.EUICC.ECKA` from `euiccSigned2`, verify `euiccSignature2`, then derive the session with `rsp_session_init` over the Annex G `SharedInfo` — `keyType(1) ‖ keyLen(1) ‖ HostID-LV ‖ EID-LV`. `hostId` must come from **one** named constant that Task 4's `controlRefTemplate` also uses; a second copy is how the two drift apart and derive different keys.

- [ ] **Step 5: Run to verify they pass**

- [ ] **Step 6: Prove they can fail**

Mutate: accept a mismatched transactionId; skip the chain verification; use a different `hostId` in the KDF than in the CRT. The third must break the round trip in Task 4's test, so note it there if it cannot be seen here.

- [ ] **Step 7: Commit**

---

### Task 4: `'87'` and `'88'` protection, and the four BPP gaps

**Files:**
- Modify: `euicc-rsp/src/rsp_crypto.c` (a MAC-only variant beside `rsp_protect`)
- Modify: `euicc-rsp/src/rsp_bpp.c`, `euicc-rsp/include/rsp.h`
- Test: `euicc-rsp/tests/test_scp03t.c`, `euicc-rsp/tests/test_bpp.c`

**Interfaces:**
- Produces: `long rsp_protect_mac_only(rsp_session_t *s, const uint8_t *plain, size_t plain_len, uint8_t tag, uint8_t *out, size_t out_cap);` and an extended `rsp_bpp_input_t` carrying `transaction_id`, `eid`, and the DPpb credential.

- [ ] **Step 1: Read §2.5.4 Table 4 and record the construction**

`'87'` encrypted **and** MAC'd with S-ENC/S-CMAC; `'88'` MAC'd only, explicitly not encrypted; `'86'` encrypted and MAC'd. All three advance the same chain, in that order. §2.5.4's own text: "The encryption counter for ICV calculation is incremented each time a TLV with tag '86', '87' or '88' is received." Confirm what that means for the chaining value before writing code.

- [ ] **Step 2: Write the failing tests**

- a `'88'` segment's plaintext is recoverable without a decryption step, and its MAC covers the tag and length
- the chain after `'87'` then `'88'` then `'86'` differs from the chain after `'86'` alone — proving all three advance it
- `smdpSign` verifies against CERT.DPpb over `remoteOpId ‖ transactionId ‖ controlRefTemplate ‖ smdpOtpk ‖ euiccOtpk` (§5.5.1)
- `hostId` in the CRT is byte-identical to the `hostId` used in the Annex G `SharedInfo`

The last one is the assertion that catches the drift the spec warns about. Write it so it reads both values from where the code actually puts them, not from a test constant.

- [ ] **Step 3: Run to verify they fail**

- [ ] **Step 4: Implement**

- [ ] **Step 5: Run to verify they pass**

- [ ] **Step 6: Prove they can fail**

Mutate: encrypt `'88'`; skip the chain advance for `'87'`; sign `smdpSign` over the wrong concatenation; use a different `hostId` in the two places.

- [ ] **Step 7: Commit**

---

### Task 5: The ES10b functions in euicc-lpa

**Files:**
- Modify: `euicc-lpa/src/rsp_es10.c`, `euicc-lpa/include/lpa.h`
- Test: `euicc-lpa/tests/test_es10b.c` (new)

**Interfaces:**
- Produces:

```c
int rsp_card_get_challenge(rsp_transport_t *t, uint8_t out[16], int *no_isdr);
int rsp_card_get_info1(rsp_transport_t *t, uint8_t **out, size_t *len, int *no_isdr);
int rsp_card_authenticate_server(rsp_transport_t *t,
        const uint8_t *req, size_t req_len, uint8_t **out, size_t *len);
int rsp_card_prepare_download(rsp_transport_t *t,
        const uint8_t *req, size_t req_len, uint8_t **out, size_t *len);
int rsp_card_load_bpp(rsp_transport_t *t,
        const uint8_t *bpp, size_t bpp_len, uint8_t **result, size_t *result_len);
int rsp_card_cancel_session(rsp_transport_t *t,
        const uint8_t transaction_id[16], long reason);
int rsp_card_delete_profile(rsp_transport_t *t,
        const uint8_t iccid[10], long *result);
```

- [ ] **Step 1: Read §5.7.5, §5.7.6, §5.7.7, §5.7.13, §5.7.14 and ES10c §5.7.18**

Record each request's tag and each response's shape in the report. `DeleteProfileRequest` is `[51] CHOICE` tag `BF33`; the response is `[51] SEQUENCE` with `deleteResult INTEGER {ok(0), iccidOrAidNotFound(1), profileNotInDisabledState(2), disallowedByPolicy(3), undefinedError(127)}`.

- [ ] **Step 2: Write the failing tests, against hand-built fixtures**

`rsp_card_load_bpp` is the one that needs care: §2.5.5 segments the BPP so that each TLV up to 255 bytes goes in one APDU and larger TLVs are split, and §5.7.6 says intermediate blocks carry no response data while the last block of a TLV may carry a `ProfileInstallationResult`. Build a fixture with a BPP large enough to need at least three blocks, so the segmentation is actually exercised rather than assumed. A one-block fixture would pass with no segmentation at all.

- [ ] **Step 3–7: as the other tasks** — fail, implement, pass, mutate, commit.

Mutations must include: a BPP short enough to fit one block (must still work), a segmentation off by one block, and a `ProfileInstallationResult` carrying an error (must read as `-1`, not success).

---

### Task 6: The sequencing

**Files:**
- Create: `euicc-lpa/src/lpa_install.c`
- Modify: `euicc-lpa/include/lpa.h`
- Test: `euicc-lpa/tests/test_install.c` (new)

**Interfaces:**
- Produces:

```c
typedef struct {
    const uint8_t *upp;
    size_t         upp_len;
    const uint8_t  transaction_id[16];
    const uint8_t  otsk_dp[32];
} lpa_install_input_t;

int lpa_install_profile(rsp_transport_t *t, const lpa_install_input_t *in,
                        long *install_result, int *no_isdr);
```

- [ ] **Step 1: Write the failing test** driving all eight steps against a recording.
- [ ] **Step 2: Run to verify it fails.**
- [ ] **Step 3: Implement**, cancelling the session on any failure it can still act on (§5.7.14, reason `loadBppExecutionError(5)`).
- [ ] **Step 4: Run to verify it passes.**
- [ ] **Step 5: Prove it can fail** — reorder two steps; skip the cancel on failure; drop the transactionId check between steps.
- [ ] **Step 6: Commit.**

---

### Task 7: The CLI, and the proof against the real card

**Files:**
- Modify: `euicc-tools/src/card.c`, `euicc-tools/src/main.c`, `euicc-tools/tests/run-tests`
- Create fixtures in `euicc-lpa/testdata/cards/`

- [ ] **Step 1: `euicc card delete <iccid>` first**, because it is what makes the install path repeatable. Exit 0 deleted, 1 the card refused with the reason named, 2 could not ask.
- [ ] **Step 2: `euicc card install <profile.der>`.** Exit 0 installed, 1 the eUICC answered and refused, 2 could not ask.
- [ ] **Step 3: Ask the human partner before the first real install.** It changes the card.
- [ ] **Step 4: Record a real installation** with `--record`, twice, and confirm the two captures agree before committing either.
- [ ] **Step 5: Verify `euicc card profiles` now lists it**, and capture that too.
- [ ] **Step 6: Delete it and confirm the list is empty again.**
- [ ] **Step 7: Hand-build the error fixtures** for each `ProfileInstallationResult` failure and for aborts at steps 4 and 6.
- [ ] **Step 8: Every new assertion against a mutation.**
- [ ] **Step 9: Commit, push, reinstall globally, and state the measured numbers.**

---

## Done when

- `euicc card install` puts a profile on the test eUICC and `euicc card profiles` lists it — recorded, and the recording replays in CI.
- `euicc card delete` removes it again.
- All three suites pass, all three CIs green, counts strictly above 112 / 127 / 100.
- The test profile carries obviously worthless key material, and `testdata/cards/README.md` says so where the recording is documented.
- Every new assertion has been watched to fail.
