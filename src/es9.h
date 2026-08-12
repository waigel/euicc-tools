/*
 * es9.h -- the ES9+ client side: talking to an SM-DP+ over HTTPS.
 *
 * SGP.22 v2.6 section 6.5's JSON binding, from the LPA's end. The
 * server side of the same three functions is euicc-rsp; this is what
 * reaches one across a network instead of calling it in-process.
 *
 * Every function here returns 0, -1 or -2 on euicc-rsp's own
 * convention: -1 means the question was asked and the answer is no (the
 * server answered and refused), -2 means it was never reached (no
 * connection, an unparseable answer, an allocation failure).
 */
#ifndef EUICC_ES9_H
#define EUICC_ES9_H

#include <stddef.h>
#include <stdint.h>

/* base64, as every ES9+ payload field except transactionId is carried.
   Both allocate; the caller frees. Returns 0 or -1. */
int es9_b64_encode(const uint8_t *in, size_t in_len, char **out);
int es9_b64_decode(const char *in, uint8_t **out, size_t *out_len);

/* transactionId is the one field that is uppercase hexadecimal instead
   (section 6.5.2.6's pattern ^[0-9,A-F]{2,32}$). */
int es9_hex_encode(const uint8_t *in, size_t in_len, char **out);
int es9_hex_decode(const char *in, uint8_t **out, size_t *out_len);

/* Pull a string value out of a JSON object by key.
 *
 * Deliberately not a JSON parser. The six bodies this client exchanges
 * are small, flat enough, and every key it looks for is unique within
 * its body -- so a scan for "key" followed by a string value is enough,
 * and a parser would be several hundred lines of attack surface for no
 * question this code actually asks. It refuses anything it does not
 * understand rather than guessing: escapes other than \\" and \\\\ are
 * not decoded, because no value the specification defines here contains
 * one (they are base64, hexadecimal, or an OID).
 *
 * Returns 0 with *out malloc'ed, or -1 when the key is absent or its
 * value is not a plain string. */
int es9_json_string(const char *json, const char *key, char **out);

/* One ES9+ session against one SM-DP+. */
typedef struct {
    char *base_url;   /* "https://host[:port]", no trailing slash */
    char *ca_file;    /* a PEM bundle to trust instead of the system's,
                         or NULL. A server whose certificate does not
                         chain to a public root needs this. */
    long  timeout_s;
    char *last_error; /* what the server said, when it said no */
} es9_client_t;

void es9_client_free(es9_client_t *c);

/* POST one function's JSON body to <base_url>/gsma/rsp2/es9plus/<fn>.
   *resp is the response body, malloc'ed. Returns 0, or -2 -- an HTTP
   status other than 200 is -2, because SGP.22 section 6.3 says a
   synchronous function answers 200 whether it succeeded or not, so
   anything else is the transport failing rather than the function. */
int es9_post(es9_client_t *c, const char *function, const char *body,
             char **resp);

/* es9_post, with the HTTP status the specification gives that function's
   MEP: 200 for the synchronous three, 204 for HandleNotification. */
int es9_post_expect(es9_client_t *c, const char *function, const char *body,
                    long expect_status, char **resp);

/* The three functions, each taking what the card just said and handing
   back what the card is to be told next -- already repacked into the
   ES10b request, so a caller can pass it straight on.

   On -1 the server answered with a Failed status; c->last_error holds
   its message when it gave one. */
int es9_initiate_authentication(es9_client_t *c,
                                const uint8_t *euicc_challenge, size_t challenge_len,
                                const uint8_t *euicc_info1, size_t info1_len,
                                const char *smdp_address,
                                char **transaction_id_hex,
                                uint8_t **auth_server_req, size_t *req_len);

int es9_authenticate_client(es9_client_t *c, const char *transaction_id_hex,
                            const uint8_t *auth_server_resp, size_t resp_len,
                            uint8_t **prepare_download_req, size_t *req_len);

/* Deliver one notification (ES9+ HandleNotification, SGP.22 v2.6
   section 6.5.2.9). The Notification MEP, so the server answers 204 with
   an empty body and says nothing about what it made of it -- an LPA has
   no key to verify with and nothing to do with a verdict.

   Returns 0 when the server took it, which is the only signal there is
   and the only thing that licenses removing it from the card. -2 when
   the server could not be reached or answered anything other than 204.
   There is no -1: with no body there is nothing for the server to refuse
   in. */
int es9_handle_notification(es9_client_t *c,
                            const uint8_t *pending, size_t pending_len);

int es9_get_bound_profile_package(es9_client_t *c, const char *transaction_id_hex,
                                  const uint8_t *prepare_download_resp, size_t resp_len,
                                  uint8_t **bpp, size_t *bpp_len);

#endif /* EUICC_ES9_H */
