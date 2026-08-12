/* base64, hexadecimal and the JSON reader src/es9.c speaks ES9+ with.
 *
 * These are the parts where a bug is silent: a base64 decoder that
 * accepts something malformed hands the card a wrong certificate, and a
 * JSON reader that matches the wrong key hands it the wrong field. The
 * HTTP around them fails loudly by comparison. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "es9.h"
#include "rsp.h"

static int fails;
static void ok(const char *what, int good) {
    printf("%s   %s\n", good ? "ok  " : "FAIL", what);
    if (!good) fails++;
}

static int round_trips(const uint8_t *in, size_t len) {
    char *enc = NULL;
    uint8_t *dec = NULL;
    size_t dec_len = 0;
    int good;
    if (es9_b64_encode(in, len, &enc) != 0) return 0;
    good = es9_b64_decode(enc, &dec, &dec_len) == 0
        && dec_len == len && memcmp(dec, in, len) == 0;
    free(enc);
    free(dec);
    return good;
}

int main(void) {
    /* ---- base64, against RFC 4648's own vectors ------------------- */
    {
        char *e = NULL;
        ok("base64 of \"\" is \"\"", es9_b64_encode((const uint8_t *)"", 0, &e) == 0
           && strcmp(e, "") == 0); free(e);
        ok("base64 of \"f\" is \"Zg==\"", es9_b64_encode((const uint8_t *)"f", 1, &e) == 0
           && strcmp(e, "Zg==") == 0); free(e);
        ok("base64 of \"fo\" is \"Zm8=\"", es9_b64_encode((const uint8_t *)"fo", 2, &e) == 0
           && strcmp(e, "Zm8=") == 0); free(e);
        ok("base64 of \"foo\" is \"Zm9v\"", es9_b64_encode((const uint8_t *)"foo", 3, &e) == 0
           && strcmp(e, "Zm9v") == 0); free(e);
        ok("base64 of \"foobar\" is \"Zm9vYmFy\"",
           es9_b64_encode((const uint8_t *)"foobar", 6, &e) == 0
           && strcmp(e, "Zm9vYmFy") == 0); free(e);
    }
    {
        /* Every byte value, and every length modulo 3. */
        uint8_t all[256];
        int i, good = 1;
        for (i = 0; i < 256; i++) all[i] = (uint8_t)i;
        for (i = 0; i <= 256; i++) if (!round_trips(all, (size_t)i)) good = 0;
        ok("base64 round-trips every length from 0 to 256", good);
    }
    {
        uint8_t *d = NULL;
        size_t n = 0;
        ok("a length that is not a multiple of four is refused",
           es9_b64_decode("Zm9", &d, &n) == -1);
        ok("a character outside the alphabet is refused",
           es9_b64_decode("Zm9$", &d, &n) == -1);
        ok("padding in the middle is refused",
           es9_b64_decode("Zg==Zg==", &d, &n) == -1);
        ok("padding inside a group is refused",
           es9_b64_decode("Z=g=", &d, &n) == -1);
        /* Whitespace is not skipped: nothing here wraps its base64, and
           skipping bytes turns a truncated field into a wrong one. */
        ok("whitespace is refused rather than skipped",
           es9_b64_decode("Zm9v YmFy", &d, &n) == -1);
    }

    /* ---- hexadecimal, for transactionId --------------------------- */
    {
        static const uint8_t t[3] = { 0x01, 0xab, 0xff };
        char *e = NULL;
        uint8_t *d = NULL;
        size_t n = 0;
        ok("hex is upper case, as section 6.5.2.6's pattern requires",
           es9_hex_encode(t, 3, &e) == 0 && strcmp(e, "01ABFF") == 0);
        ok("hex decodes upper case", es9_hex_decode("01ABFF", &d, &n) == 0
           && n == 3 && memcmp(d, t, 3) == 0);
        free(d); d = NULL;
        ok("...and lower case too, since a server may send either",
           es9_hex_decode("01abff", &d, &n) == 0 && n == 3 && memcmp(d, t, 3) == 0);
        free(d); d = NULL;
        ok("an odd number of digits is refused", es9_hex_decode("01A", &d, &n) == -1);
        ok("a non-hex digit is refused", es9_hex_decode("01AZ", &d, &n) == -1);
        free(e);
    }

    /* ---- the JSON reader ------------------------------------------ */
    {
        static const char body[] =
            "{\"header\":{\"functionExecutionStatus\":{\"status\":\"Executed-Success\"}},"
            "\"transactionId\":\"0102\",\"serverSigned1\":\"MAA=\"}";
        char *v = NULL;
        ok("a top-level key is found", es9_json_string(body, "transactionId", &v) == 0
           && strcmp(v, "0102") == 0); free(v); v = NULL;
        ok("a nested key is found too", es9_json_string(body, "status", &v) == 0
           && strcmp(v, "Executed-Success") == 0); free(v); v = NULL;
        ok("an absent key is refused", es9_json_string(body, "nope", &v) == -1);

        /* The trap this reader has to avoid: "Id" must not match inside
           "transactionId". Searching for the quoted key is what stops
           it, and this is the test that would catch losing that. */
        ok("a key that is a suffix of another does not match it",
           es9_json_string(body, "Id", &v) == -1);
        ok("a key that is a prefix of another does not match it",
           es9_json_string(body, "transaction", &v) == -1);
    }
    {
        char *v = NULL;
        ok("a value that is not a string is refused",
           es9_json_string("{\"n\":42}", "n", &v) == -1);
        ok("a key with no colon after it is refused",
           es9_json_string("{\"n\" 42}", "n", &v) == -1);
        ok("whitespace around the colon is tolerated",
           es9_json_string("{\"n\" : \"x\"}", "n", &v) == 0 && strcmp(v, "x") == 0);
        free(v); v = NULL;
        ok("an escaped quote inside a value is decoded",
           es9_json_string("{\"n\":\"a\\\"b\"}", "n", &v) == 0
           && strcmp(v, "a\"b") == 0);
        free(v); v = NULL;
        ok("an escape this reader does not claim to handle is refused",
           es9_json_string("{\"n\":\"a\\u0041b\"}", "n", &v) == -1);
        ok("an unterminated string is refused",
           es9_json_string("{\"n\":\"abc", "n", &v) == -1);
    }

    /* ---- reassembling a response, byte for byte ------------------ */
    {
        /* The JSON binding sends an ES9+ answer as separate fields, and
           euicc-lpa's repackers want the encoding the SM-DP+ produced.
           Putting it back together has to be byte-exact or the repacker
           refuses what it would otherwise accept -- which is how the
           first real download failed, on a tag: AuthenticateClientOk is
           reached as [0] over a SEQUENCE, A0, because rsp-2.5.asn is
           AUTOMATIC TAGS and the CHOICE's alternatives carry no tags of
           their own. A plain 30 looks right and is not.

           euicc-rsp records a real session, so the check is exact rather
           than a shape assertion: split its recorded response into
           fields the way a server would send them, put it back, and
           require the same bytes. */
        static const char PATH[] =
            "vendor/euicc-lpa/vendor/euicc-rsp/testdata/session/"
            "authenticate-response.der";
        FILE *f = fopen(PATH, "rb");
        ok("the recorded AuthenticateClient response is readable", f != NULL);
        if(f) {
            uint8_t orig[4096];
            size_t n = fread(orig, 1, sizeof orig, f);
            rsp_dp_authenticate_fields_t g;
            fclose(f);
            memset(&g, 0, sizeof g);
            ok("it splits into the five fields the binding names",
               n > 0 && rsp_dp_authenticate_fields(orig, n, &g) == 0);
            if(n > 0 && g.transaction_id_len) {
                uint8_t *all[5];
                size_t l[5];
                uint8_t *arm = NULL, *choice = NULL;
                size_t arm_len = 0, choice_len = 0;
                static const uint8_t OK_ARM[1] = { 0xa0 };
                static const uint8_t BF3B[2] = { 0xbf, 0x3b };

                all[0] = (uint8_t *)g.transaction_id;   l[0] = g.transaction_id_len;
                all[1] = (uint8_t *)g.profile_metadata; l[1] = g.profile_metadata_len;
                all[2] = (uint8_t *)g.smdp_signed2;     l[2] = g.smdp_signed2_len;
                all[3] = (uint8_t *)g.smdp_signature2;  l[3] = g.smdp_signature2_len;
                all[4] = (uint8_t *)g.smdp_certificate; l[4] = g.smdp_certificate_len;

                ok("the ok arm wraps", es9_wrap(OK_ARM, 1, all, l, 5,
                                                 &arm, &arm_len) == 0);
                ok("the CHOICE wraps", es9_wrap(BF3B, 2, &arm, &arm_len, 1,
                                                 &choice, &choice_len) == 0);
                ok("and the result is the recorded bytes, exactly",
                   choice && choice_len == n && memcmp(choice, orig, n) == 0);
                free(arm);
                free(choice);
            }
        }
    }

    return fails ? 1 : 0;
}
