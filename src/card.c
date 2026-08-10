/*
 * card.c -- euicc card: what a physical eUICC says about itself, and
 * whether it will work with this project's test credentials.
 *
 * This is the first command in euicc-tools that talks to anything other
 * than a file: it links euicc-lpa (vendor/euicc-lpa), the LPA role of
 * SGP.22, for its transport layer -- a real reader over PC/SC, or a
 * recording standing in for one -- and for rsp_card_read_info, which
 * drives the actual ES10 exchange (SELECT the ISD-R, GetEUICCInfo2,
 * GetEID) and hands back what the card said. The symbols keep their rsp_
 * prefix on purpose: it names the Remote SIM Provisioning standard as a
 * whole, not the SM-DP+ role in particular, and moving the card side to
 * euicc-lpa did not change what these functions are called.
 *
 * The exit code is the verdict, not just the outcome, and it is a
 * stronger claim than most commands here make:
 *
 *   0  the card answered, and trusts this project's test Certificate
 *      Issuer -- our test DPauth/DPpb credentials will work with it
 *   1  the card answered, and does not trust it
 *   2  the question could not be asked at all: no reader, no card, no
 *      ISD-R, or an answer this project's decoder cannot make sense of
 *
 * "Answered" means rsp_card_read_info returned 0. Its own -1 ("the card
 * refused") and -2 ("the exchange could not happen") both read as this
 * command's 2: neither is a trust verdict, since without EUICCInfo2 there
 * is nothing for rsp_card_trusts to ask a question about.
 */
#include "euicc.h"

#include <lpa.h>
#include <rsp.h>
#include "ProfileElement.h"

/*
 * The SubjectKeyIdentifier of euicc-rsp's testdata/sgp26/ci.der -- the
 * published GSMA SGP.26 test Certificate Issuer that this project's
 * DPauth/DPpb credentials (rsp_pki_dp) chain to. lpa.h has no function
 * that extracts a SubjectKeyIdentifier from a certificate; it only has
 * rsp_card_trusts, which answers a question about an identifier the
 * caller already has. So this is a fixed value, not something recomputed
 * from rsp_pki_test_ci's DER at run time -- verified once with:
 *
 *   openssl x509 -in testdata/sgp26/ci.der -inform DER -noout -text \
 *     | grep -A1 "Subject Key Identifier"
 *
 * against euicc-rsp's own copy of that file (see its testdata/sgp26/
 * README.md and testdata/cards/README.md, which records the same value
 * against this project's own test eUICC).
 */
static const uint8_t test_ci_ski[20] = {
    0xF5, 0x41, 0x72, 0xBD, 0xF9, 0x8A, 0x95, 0xD6, 0x5C, 0xBE,
    0xB8, 0x8A, 0x38, 0xA1, 0xC1, 0x1D, 0x80, 0x0A, 0x85, 0xC3
};

static void
print_hex(const uint8_t *b, size_t n) {
    for(size_t i = 0; i < n; i++) printf("%02X", b[i]);
}

static void
json_hex(const uint8_t *b, size_t n) {
    putchar('"');
    print_hex(b, n);
    putchar('"');
}

/* card info never printed a string a card chose the content of -- svn is
   this program's own snprintf ("%u.%u.%u"), digits and dots only. card
   profiles is the first command whose JSON output embeds text the card
   itself sent (profileNickname, serviceProviderName, profileName, all
   UTF8String), so it is also the first that needs to escape it: a
   nickname containing '"' or '\' would otherwise end the JSON string
   early or corrupt it, and control characters are not valid inside a
   JSON string unescaped either (RFC 8259 section 7). */
static void
json_string(const char *s) {
    putchar('"');
    for(const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch(*p) {
        case '"':  fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\n': fputs("\\n", stdout); break;
        case '\r': fputs("\\r", stdout); break;
        case '\t': fputs("\\t", stdout); break;
        default:
            if(*p < 0x20) printf("\\u%04x", *p);
            else putchar(*p);
        }
    }
    putchar('"');
}

/*
 * The transport a card command actually talks to: --replay substitutes a
 * recording for a reader (what lets the happy path run in CI, and what a
 * person debugging with a colleague's capture reaches for too), --record
 * additionally wraps whichever transport was opened so the session is
 * written down as it happens. rsp_record_open takes ownership of the
 * transport it wraps (include/lpa.h); on its own failure that ownership
 * was never transferred, so the raw transport is this function's to close.
 *
 * Returns 0 with *out set, or -1 with a message already on stderr.
 */
static int
open_card(const char *reader, const char *replay, const char *record,
          rsp_transport_t *out) {
    rsp_transport_t raw;
    int rc;

    if(replay) {
        rc = rsp_replay_open(replay, &raw);
        if(rc != 0) {
            fprintf(stderr, "euicc: cannot open the recording %s\n", replay);
            return -1;
        }
    } else {
        rc = rsp_pcsc_open(reader, &raw);
        if(rc != 0) {
            /* rsp_pcsc_open already named the specific reason (no reader
               attached, no card in it, another process holding it) on
               stderr; this line is what the CI test without a reader
               greps for, and what makes that reason findable even if a
               future rsp_pcsc failure mode forgets to say "reader". */
            fprintf(stderr, "euicc: no card reader is available\n");
            return -1;
        }
    }

    if(record) {
        rsp_transport_t recorded;
        rc = rsp_record_open(&raw, record, &recorded);
        if(rc != 0) {
            fprintf(stderr, "euicc: cannot open %s to record to\n", record);
            raw.close(&raw);
            return -1;
        }
        *out = recorded;
    } else {
        *out = raw;
    }
    return 0;
}

static int
cmd_card_info(const char *reader, const char *replay, const char *record,
              int as_json) {
    rsp_transport_t t;
    if(open_card(reader, replay, record, &t) != 0) return 2;

    rsp_card_info_t info;
    memset(&info, 0, sizeof info);
    /* Three outcomes, not two. A card whose ISD-R will not even be selected
       is very likely not an eUICC, or has it locked; an eUICC that accepted
       the selection and then refused a request is a different situation
       with a different next step. Saying "the card refused to answer" to
       both, as this did, sends the reader looking in the wrong place. */
    int no_isdr = 0;
    int rc = rsp_card_read_info(&t, &info, &no_isdr);
    t.close(&t);

    if(rc != 0) {
        if(no_isdr) {
            fprintf(stderr, "euicc: the card refused to select the ISD-R. It "
                            "may not be an eUICC, or its ISD-R is locked.\n");
        } else if(rc == -1) {
            fprintf(stderr, "euicc: the eUICC refused one of the requests it "
                            "was asked.\n");
        } else {
            fprintf(stderr, "euicc: the card could not be asked.\n");
        }
        return 2;
    }

    int trusts = rsp_card_trusts(&info, test_ci_ski, sizeof test_ci_ski);

    if(as_json) {
        printf("{\n \"eid\": ");
        if(info.have_eid) json_hex(info.eid, sizeof info.eid);
        else fputs("null", stdout);
        printf(",\n \"svn\": \"%s\",\n \"ci_ids\": [", info.svn);
        for(size_t i = 0; i < info.ci_count; i++) {
            if(i) fputs(", ", stdout);
            json_hex(info.ci_ids + i * info.ci_id_len, info.ci_id_len);
        }
        printf("],\n \"trusts_test_ci\": %s\n}\n", trusts ? "true" : "false");
    } else {
        fputs("EID  ", stdout);
        if(info.have_eid) print_hex(info.eid, sizeof info.eid);
        else fputs("(none)", stdout);
        putchar('\n');
        printf("SVN  %s\n", info.svn);
        for(size_t i = 0; i < info.ci_count; i++) {
            printf("CI[%zu] ", i);
            print_hex(info.ci_ids + i * info.ci_id_len, info.ci_id_len);
            putchar('\n');
        }
        printf(trusts
               ? "this card trusts our test CI: our test credentials will "
                 "work with it\n"
               : "this card does not trust our test CI: our test "
                 "credentials will not work with it\n");
    }

    rsp_card_info_free(&info);
    return trusts ? 0 : 1;
}

/*
 * card profiles: the list of profiles already installed on the eUICC,
 * from rsp_card_read_profiles (lpa.h) -- GetProfilesInfo asked with no
 * search criteria, "every profile."
 *
 * card info's exit code is a verdict about trust; this one is not, and
 * that difference is deliberate, not an oversight: card info's 0 means
 * "our test credentials will work with this card," a question with a
 * real yes/no answer. Nothing about the profile list is a verdict this
 * project holds an opinion on -- an empty list is not a worse answer
 * than a full one, it is a card with nothing installed, which is a
 * complete and useful thing to know. So here 0 means the list was
 * retrieved, whatever it contains, including empty; 1 means the ISD-R
 * answered and refused the request (rsp_card_read_profiles' -1, a
 * decoded ProfileInfoListError -- see *err below); 2 means the question
 * could not be asked at all: no reader, no card, no ISD-R, or an answer
 * this project's decoder cannot make sense of.
 */
static int
profile_state_name(long v, const char **name) {
    if(v == 0) { *name = "disabled"; return 1; }
    if(v == 1) { *name = "enabled"; return 1; }
    *name = NULL;
    return 0;
}

static int
profile_class_name(long v, const char **name) {
    if(v == 0) { *name = "test"; return 1; }
    if(v == 1) { *name = "provisioning"; return 1; }
    if(v == 2) { *name = "operational"; return 1; }
    *name = NULL;
    return 0;
}

/* incorrectInputValues(1) and undefinedError(127) are ProfileInfoListError's
   only two named values (rsp-2.5.asn, restated in include/lpa.h's own
   comment on rsp_card_read_profiles); anything else is an extension this
   project's decoder does not name either, so it prints the bare number
   rather than guess at a label. */
static const char *
profile_list_error_name(long err) {
    if(err == 1) return "incorrectInputValues";
    if(err == 127) return "undefinedError";
    return NULL;
}

static void
print_profile_human(const rsp_profile_info_t *p) {
    fputs("ICCID    ", stdout);
    if(p->have_iccid) print_hex(p->iccid, sizeof p->iccid);
    else fputs("(none)", stdout);
    putchar('\n');

    fputs("isdpAid  ", stdout);
    if(p->have_isdp_aid) print_hex(p->isdp_aid, p->isdp_aid_len);
    else fputs("(none)", stdout);
    putchar('\n');

    fputs("state    ", stdout);
    if(p->have_profile_state) {
        const char *name;
        if(profile_state_name(p->profile_state, &name)) printf("%s", name);
        else printf("%ld", p->profile_state);
    } else fputs("(none)", stdout);
    putchar('\n');

    fputs("class    ", stdout);
    if(p->have_profile_class) {
        const char *name;
        if(profile_class_name(p->profile_class, &name)) printf("%s", name);
        else printf("%ld", p->profile_class);
    } else fputs("(none)", stdout);
    putchar('\n');

    printf("nickname %s\n", p->profile_nickname ? p->profile_nickname : "(none)");
    printf("provider %s\n", p->service_provider_name ? p->service_provider_name : "(none)");
    printf("name     %s\n", p->profile_name ? p->profile_name : "(none)");
}

static void
print_profile_json(const rsp_profile_info_t *p) {
    printf("  {\n   \"iccid\": ");
    if(p->have_iccid) json_hex(p->iccid, sizeof p->iccid); else fputs("null", stdout);
    printf(",\n   \"isdpAid\": ");
    if(p->have_isdp_aid) json_hex(p->isdp_aid, p->isdp_aid_len); else fputs("null", stdout);
    printf(",\n   \"profileState\": ");
    if(p->have_profile_state) printf("%ld", p->profile_state); else fputs("null", stdout);
    printf(",\n   \"profileClass\": ");
    if(p->have_profile_class) printf("%ld", p->profile_class); else fputs("null", stdout);
    printf(",\n   \"profileNickname\": ");
    if(p->profile_nickname) json_string(p->profile_nickname); else fputs("null", stdout);
    printf(",\n   \"serviceProviderName\": ");
    if(p->service_provider_name) json_string(p->service_provider_name); else fputs("null", stdout);
    printf(",\n   \"profileName\": ");
    if(p->profile_name) json_string(p->profile_name); else fputs("null", stdout);
    printf("\n  }");
}

static int
cmd_card_profiles(const char *reader, const char *replay, const char *record,
                   int as_json) {
    rsp_transport_t t;
    if(open_card(reader, replay, record, &t) != 0) return 2;

    rsp_profile_info_t *profiles = NULL;
    size_t count = 0;
    long err = 0;
    int no_isdr = 0;
    int rc = rsp_card_read_profiles(&t, &profiles, &count, &err, &no_isdr);
    t.close(&t);

    if(rc != 0) {
        if(no_isdr) {
            fprintf(stderr, "euicc: the card refused to select the ISD-R. It "
                            "may not be an eUICC, or its ISD-R is locked.\n");
        } else if(rc == -1) {
            const char *name = profile_list_error_name(err);
            if(name) {
                fprintf(stderr, "euicc: the eUICC refused the profile list "
                                "request: %s.\n", name);
            } else {
                fprintf(stderr, "euicc: the eUICC refused the profile list "
                                "request (error %ld).\n", err);
            }
        } else {
            fprintf(stderr, "euicc: the card could not be asked.\n");
        }
        return rc == -1 ? 1 : 2;
    }

    if(as_json) {
        printf("{\n \"profiles\": [\n");
        for(size_t i = 0; i < count; i++) {
            if(i) fputs(",\n", stdout);
            print_profile_json(&profiles[i]);
        }
        printf("\n ]\n}\n");
    } else if(count == 0) {
        fputs("no profiles are installed on this card\n", stdout);
    } else {
        for(size_t i = 0; i < count; i++) {
            if(i) putchar('\n');
            print_profile_human(&profiles[i]);
        }
    }

    rsp_card_profiles_free(profiles, count);
    return 0;
}


/*
 * cmd_card_install -- build a Bound Profile Package for this card and
 * load it. The whole point of the project, and the first command that
 * leaves a profile behind.
 *
 * The ICCID comes out of the profile's own header PE rather than from a
 * flag. StoreMetadataRequest (SGP.22 v2.6 section 5.5.3) carries it, and
 * the eUICC checks it against what the profile actually contains --
 * CancelSessionReason has metadataMismatch(4) for the disagreement. A
 * flag would put a twenty-digit number in a person's hands twice and
 * make a mismatch a plausible typo; reading it from the file makes the
 * two agree by construction.
 *
 * notificationConfigurationInfo is deliberately absent. Section 5.7.18's
 * note that the eUICC "SHALL generate as many Notifications as
 * configured in its metadata" is what lets this round leave the whole
 * notification path out: configure none, none are generated, and nothing
 * accumulates in a list this tool cannot drain.
 */

static uint8_t *
read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if(!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if(n <= 0) { fclose(f); return NULL; }
    uint8_t *p = malloc((size_t)n);
    if(p && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f);
    if(p) *len = (size_t)n;
    return p;
}

/* The ICCID out of the profile's first element, which SAIP requires to
   be the header. Returns 0 with iccid filled, or -1 having said why. */
static int
profile_iccid(const uint8_t *upp, size_t upp_len, uint8_t iccid[10]) {
    ProfileElement_t *pe = NULL;
    asn_dec_rval_t dr = ber_decode(NULL, &asn_DEF_ProfileElement,
                                   (void **)&pe, upp, upp_len);
    if(dr.code != RC_OK || !pe) {
        if(pe) ASN_STRUCT_FREE(asn_DEF_ProfileElement, pe);
        fprintf(stderr, "euicc: the profile's first element does not "
                        "decode\n");
        return -1;
    }
    if(pe->present != ProfileElement_PR_header) {
        ASN_STRUCT_FREE(asn_DEF_ProfileElement, pe);
        fprintf(stderr, "euicc: the profile does not start with a header "
                        "element; SAIP requires it to\n");
        return -1;
    }
    if(pe->choice.header.iccid.size != 10) {
        ASN_STRUCT_FREE(asn_DEF_ProfileElement, pe);
        fprintf(stderr, "euicc: the profile's ICCID is not ten bytes\n");
        return -1;
    }
    memcpy(iccid, pe->choice.header.iccid.buf, 10);
    ASN_STRUCT_FREE(asn_DEF_ProfileElement, pe);
    return 0;
}

/* BF25 <len> 5A 0A <iccid> 91 <len> <spn> 92 <len> <name> [95 01 <class>].
   Built by hand because all these fields are short-form and there is
   nothing an encoder could get right that eleven constant-shaped bytes
   plus two strings get wrong. Returns the length written, or 0 if it
   would not fit.

   profile_class is a ProfileClass value, or -1 to leave the field out.
   Leaving it out is not the same as saying "operational": the field
   carries DEFAULT operational, so DER omits it for that value and an
   eUICC reads an absent field as operational either way. It has to be
   present for a Test Profile, though, and that is not cosmetic -- SGP.22
   v2.6 section 2.4.5.3: "A Test Profile SHALL have its Profile Class set
   to 'test' in its Profile Metadata", because a Test Profile is allowed
   network authentication keys an Operational Profile is not. A test
   profile installed with the class left at operational is a profile
   whose metadata and content disagree, and a real eUICC refuses it with
   installFailedDueToDataMismatch(13). */
static size_t
build_metadata(uint8_t *out, size_t cap, const uint8_t iccid[10],
               const char *spn, const char *name, int profile_class) {
    size_t sl = strlen(spn), nl = strlen(name);
    if(sl > 32 || nl > 64) return 0;             /* the ASN.1 SIZE bounds */
    size_t content = 12 + 2 + sl + 2 + nl + (profile_class >= 0 ? 3 : 0);
    if(content > 127 || content + 3 > cap) return 0;
    size_t i = 0;
    out[i++] = 0xBF; out[i++] = 0x25; out[i++] = (uint8_t)content;
    out[i++] = 0x5A; out[i++] = 0x0A; memcpy(out + i, iccid, 10); i += 10;
    out[i++] = 0x91; out[i++] = (uint8_t)sl; memcpy(out + i, spn, sl); i += sl;
    out[i++] = 0x92; out[i++] = (uint8_t)nl; memcpy(out + i, name, nl); i += nl;
    if(profile_class >= 0) {
        out[i++] = 0x95; out[i++] = 0x01; out[i++] = (uint8_t)profile_class;
    }
    return i;
}

static int
cmd_card_install(const char *path, const char *spn, const char *name,
                 int profile_class,
                 const char *reader, const char *replay, const char *record) {
    if(!path) {
        fprintf(stderr, "euicc: card install needs a profile package\n");
        return 2;
    }

    size_t upp_len = 0;
    uint8_t *upp = read_file(path, &upp_len);
    if(!upp) {
        fprintf(stderr, "euicc: cannot read %s\n", path);
        return 2;
    }

    uint8_t iccid[10];
    if(profile_iccid(upp, upp_len, iccid) != 0) { free(upp); return 2; }

    /* The two sides of this boundary spell an ICCID differently, and the
       eUICC checks one against the other. SAIP's ProfileHeader carries the
       digits in reading order; RSP's Iccid is "ICCID as coded in EFiccid"
       (rsp-2.5.asn's own comment on the type), which is 3GPP's nibble-
       swapped BCD -- the same bytes EFiccid inside the profile holds.
       SGP.22 v2.6 section 5.5.5 is explicit that the eUICC "SHALL ignore
       the ICCID value provided in the 'ProfileHeader' PE" and instead
       requires "The ICCID provided in the Profile Metadata is identical
       to the value of EFICCID". Handing the header's bytes straight to
       StoreMetadata therefore compares an unswapped value against a
       swapped one, and a real eUICC refuses the finished install with
       installFailedDueToDataMismatch(13) -- after every Profile Element
       has already been processed successfully, which is what makes it
       read like a profile fault rather than a metadata one. */
    uint8_t iccid_ef[10];
    for(size_t k = 0; k < sizeof iccid; k++) {
        iccid_ef[k] = (uint8_t)((iccid[k] >> 4) | (iccid[k] << 4));
    }

    uint8_t metadata[128];
    size_t metadata_len = build_metadata(metadata, sizeof metadata, iccid_ef,
                                          spn ? spn : "euicc-tools",
                                          name ? name : "test profile",
                                          profile_class);
    if(metadata_len == 0) {
        fprintf(stderr, "euicc: the profile name or provider is too long "
                        "(32 and 64 bytes are the limits)\n");
        free(upp);
        return 2;
    }

    /* Fresh for every run, as SGP.22 requires: section 5.5.1 has the
       transactionId unique within the SM-DP+'s lifetime, explicitly to
       stop a CancelSession being replayed, and otSK.DP is a one-time key
       whose reuse would undo the forward secrecy the exchange exists
       for. Both are parameters of rsp_lpa_install rather than values it
       invents, so a test can pin them; here they must not be pinned. */
    uint8_t transaction_id[16], otsk_dp[32];
    {
        FILE *ur = fopen("/dev/urandom", "rb");
        int got = ur
            && fread(transaction_id, 1, sizeof transaction_id, ur)
                 == sizeof transaction_id
            && fread(otsk_dp, 1, sizeof otsk_dp, ur) == sizeof otsk_dp;
        if(ur) fclose(ur);
        if(!got) {
            fprintf(stderr, "euicc: cannot read /dev/urandom\n");
            free(upp);
            return 2;
        }
    }

    rsp_transport_t t;
    if(open_card(reader, replay, record, &t) != 0) { free(upp); return 2; }

    uint8_t *result = NULL;
    size_t result_len = 0;
    int step = 0, no_isdr = 0;
    int rc = rsp_lpa_install(&t, upp, upp_len, metadata, metadata_len,
                             transaction_id, otsk_dp, &result, &result_len,
                             &step, &no_isdr);
    t.close(&t);
    free(upp);

    if(rc != 0) {
        static const char *what[] = {
            "before anything was asked", "asking the eUICC about itself",
            "asking the eUICC for a challenge", "opening the session",
            "having the eUICC check our certificate",
            "having the server check the eUICC's",
            "asking the eUICC for its one-time key",
            "building the profile package", "loading the package"
        };
        const char *at = (step >= 0 && step <= 8) ? what[step] : "somewhere";
        if(no_isdr) {
            fprintf(stderr, "euicc: the card refused to select the ISD-R. "
                            "It may not be an eUICC, or its ISD-R is "
                            "locked.\n");
        } else if(rc == -1) {
            fprintf(stderr, "euicc: refused at step %d, %s.\n", step, at);
        } else {
            fprintf(stderr, "euicc: could not get past step %d, %s.\n",
                    step, at);
        }
        return rc == -1 ? 1 : 2;
    }

    /* Reaching here means the eUICC took the package and answered. It
       does NOT mean the profile installed -- that is what the result
       says, and reporting success without reading it is exactly the
       mistake this message exists to avoid. */
    printf("the eUICC accepted the package and returned a "
           "ProfileInstallationResult of %zu bytes\n", result_len);
    printf("run `euicc card profiles` to see whether the profile is "
           "there\n");
    free(result);
    return 0;
}

/*
 * cmd_card_delete -- remove a profile by ICCID.
 *
 * This exists so the install path can be practised. Without it every
 * attempt during development burns a slot on the test card, and a card
 * with no free slots ends the work.
 *
 * The exit code carries the card's own answer rather than flattening
 * it: SGP.22 v2.6 section 5.7.18 has the eUICC check the profile's
 * state and its Profile Policy Rules before deleting, and "it is still
 * enabled" sends a reader somewhere entirely different from "there is
 * no such profile".
 */
static int
cmd_card_delete(const char *iccid_hex, const char *reader,
                const char *replay, const char *record) {
    uint8_t iccid[10];

    if(!iccid_hex) {
        fprintf(stderr, "euicc: card delete needs an ICCID\n");
        return 2;
    }
    /* Twenty hex digits, nothing else. An ICCID with a stray separator
       or an odd length would otherwise be padded or truncated into a
       different profile's identifier. */
    if(strlen(iccid_hex) != 20) {
        fprintf(stderr, "euicc: an ICCID is 20 hex digits; got %zu\n",
                strlen(iccid_hex));
        return 2;
    }
    for(int i = 0; i < 10; i++) {
        unsigned v;
        if(sscanf(iccid_hex + 2 * i, "%2x", &v) != 1) {
            fprintf(stderr, "euicc: %s is not hexadecimal\n", iccid_hex);
            return 2;
        }
        iccid[i] = (uint8_t)v;
    }

    rsp_transport_t t;
    if(open_card(reader, replay, record, &t) != 0) return 2;

    long result = 0;
    int no_isdr = 0;
    int rc = rsp_card_delete_profile(&t, iccid, &result, &no_isdr);
    t.close(&t);

    if(rc == 0) {
        printf("deleted %s\n", iccid_hex);
        return 0;
    }
    if(rc == -1 && no_isdr) {
        fprintf(stderr, "euicc: the card refused to select the ISD-R. It "
                        "may not be an eUICC, or its ISD-R is locked.\n");
        return 2;
    }
    if(rc == -1) {
        const char *why;
        switch(result) {
        case 1:  why = "there is no profile with that ICCID"; break;
        case 2:  why = "the profile is still enabled; it must be disabled "
                       "first"; break;
        case 3:  why = "the profile's own policy rules forbid deleting it";
                 break;
        default: why = "the eUICC gave no reason"; break;
        }
        fprintf(stderr, "euicc: the eUICC refused to delete %s: %s.\n",
                iccid_hex, why);
        return 1;
    }
    fprintf(stderr, "euicc: the card could not be asked.\n");
    return 2;
}

int
cmd_card(int argc, char **argv) {
    if(argc < 1) {
        fprintf(stderr,
                "euicc: card needs a subcommand: info, profiles, "
                "install or delete\n");
        return 2;
    }

    const char *sub = argv[0];
    const char *reader = NULL, *replay = NULL, *record = NULL;
    const char *arg = NULL;   /* the one positional a subcommand may take */
    const char *spn = NULL, *name = NULL;
    int as_json = 0;
    /* -1 means "leave the field out", which an eUICC reads as operational
       -- see build_metadata. Named rather than numeric on the command
       line: the numbers are ProfileClass's, and a person installing a
       test profile should not have to know that test is 0. */
    int profile_class = -1;

    for(int i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "--reader") && i + 1 < argc) reader = argv[++i];
        else if(!strcmp(argv[i], "--replay") && i + 1 < argc) replay = argv[++i];
        else if(!strcmp(argv[i], "--record") && i + 1 < argc) record = argv[++i];
        else if(!strcmp(argv[i], "--provider") && i + 1 < argc) spn = argv[++i];
        else if(!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if(!strcmp(argv[i], "--class") && i + 1 < argc) {
            const char *c = argv[++i];
            if(!strcmp(c, "test")) profile_class = 0;
            else if(!strcmp(c, "provisioning")) profile_class = 1;
            else if(!strcmp(c, "operational")) profile_class = 2;
            else {
                fprintf(stderr, "euicc: card %s: --class takes test, "
                                "provisioning or operational, not %s\n",
                        sub, c);
                return 2;
            }
        }
        else if(!strcmp(argv[i], "--json")) as_json = 1;
        else if(argv[i][0] != '-' && !arg) arg = argv[i];
        else {
            fprintf(stderr, "euicc: card %s: unknown option %s\n", sub,
                    argv[i]);
            return 2;
        }
    }

    if(replay && reader) {
        fprintf(stderr,
                "euicc: card %s: --replay and --reader name two different "
                "transports; pick one\n", sub);
        return 2;
    }

    if(!strcmp(sub, "info")) return cmd_card_info(reader, replay, record, as_json);
    if(!strcmp(sub, "profiles")) return cmd_card_profiles(reader, replay, record, as_json);
    if(!strcmp(sub, "delete")) return cmd_card_delete(arg, reader, replay, record);
    if(!strcmp(sub, "install"))
        return cmd_card_install(arg, spn, name, profile_class,
                                reader, replay, record);

    fprintf(stderr, "euicc: card %s: unknown subcommand\n", sub);
    return 2;
}
