/*
 * card.c -- euicc card: what a physical eUICC says about itself, and
 * whether it will work with this project's test credentials.
 *
 * This is the first command in euicc-tools that talks to anything other
 * than a file: it links euicc-rsp (vendor/euicc-rsp), the SM-DP+ role of
 * SGP.22, for its transport layer -- a real reader over PC/SC, or a
 * recording standing in for one -- and for rsp_card_read_info, which
 * drives the actual ES10 exchange (SELECT the ISD-R, GetEUICCInfo2,
 * GetEID) and hands back what the card said.
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

#include <rsp.h>

/*
 * The SubjectKeyIdentifier of euicc-rsp's testdata/sgp26/ci.der -- the
 * published GSMA SGP.26 test Certificate Issuer that this project's
 * DPauth/DPpb credentials (rsp_pki_dp) chain to. rsp.h has no function
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

/*
 * The transport a card command actually talks to: --replay substitutes a
 * recording for a reader (what lets the happy path run in CI, and what a
 * person debugging with a colleague's capture reaches for too), --record
 * additionally wraps whichever transport was opened so the session is
 * written down as it happens. rsp_record_open takes ownership of the
 * transport it wraps (include/rsp.h); on its own failure that ownership
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
    int rc = rsp_card_read_info(&t, &info);
    t.close(&t);

    if(rc != 0) {
        fprintf(stderr, "euicc: the card %s\n",
                rc == -1 ? "refused to answer" : "could not be asked");
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
 * ProfileInfoListResponse decoding is the same shape rsp_card_read_info
 * already handles for EUICCInfo2, but rsp.h exposes no function that
 * drives that particular ES10 exchange -- only rsp_card_read_info's
 * EUICCInfo2/EID pair. Rather than reach past include/rsp.h into
 * euicc-rsp's own ES10 request layer or its generated codec from this
 * side of the library boundary, this stays an honest gap: it says so and
 * asks for nothing, instead of a command that talks to a card and prints
 * an answer with no library function behind it.
 */
static int
cmd_card_profiles(const char *reader, const char *replay, const char *record,
                   int as_json) {
    (void)reader;
    (void)replay;
    (void)record;
    (void)as_json;
    fprintf(stderr,
            "euicc: card profiles is not implemented -- rsp.h has no "
            "function that reads a card's profile list yet, only "
            "rsp_card_read_info's EUICCInfo2/EID pair\n");
    return 2;
}

int
cmd_card(int argc, char **argv) {
    if(argc < 1) {
        fprintf(stderr,
                "euicc: card needs a subcommand: info or profiles\n");
        return 2;
    }

    const char *sub = argv[0];
    const char *reader = NULL, *replay = NULL, *record = NULL;
    int as_json = 0;

    for(int i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "--reader") && i + 1 < argc) reader = argv[++i];
        else if(!strcmp(argv[i], "--replay") && i + 1 < argc) replay = argv[++i];
        else if(!strcmp(argv[i], "--record") && i + 1 < argc) record = argv[++i];
        else if(!strcmp(argv[i], "--json")) as_json = 1;
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

    fprintf(stderr, "euicc: card %s: unknown subcommand\n", sub);
    return 2;
}
