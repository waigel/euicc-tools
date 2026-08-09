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
                "euicc: card needs a subcommand: info, profiles or "
                "delete\n");
        return 2;
    }

    const char *sub = argv[0];
    const char *reader = NULL, *replay = NULL, *record = NULL;
    const char *arg = NULL;   /* the one positional a subcommand may take */
    int as_json = 0;

    for(int i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "--reader") && i + 1 < argc) reader = argv[++i];
        else if(!strcmp(argv[i], "--replay") && i + 1 < argc) replay = argv[++i];
        else if(!strcmp(argv[i], "--record") && i + 1 < argc) record = argv[++i];
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

    fprintf(stderr, "euicc: card %s: unknown subcommand\n", sub);
    return 2;
}
