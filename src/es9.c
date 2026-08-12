/* See es9.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "es9.h"
#include "lpa.h"

/* ---- base64 ------------------------------------------------------- */

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int es9_b64_encode(const uint8_t *in, size_t in_len, char **out)
{
    size_t i, o = 0;
    char *b;

    if (!in || !out) return -1;
    if (in_len > (SIZE_MAX - 1) / 4 * 3) return -1;
    b = malloc((in_len + 2) / 3 * 4 + 1);
    if (!b) return -1;

    for (i = 0; i + 2 < in_len; i += 3) {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) | in[i + 2];
        b[o++] = B64[(v >> 18) & 0x3f];
        b[o++] = B64[(v >> 12) & 0x3f];
        b[o++] = B64[(v >> 6) & 0x3f];
        b[o++] = B64[v & 0x3f];
    }
    if (i < in_len) {
        unsigned v = (unsigned)in[i] << 16;
        int two = (i + 1 < in_len);
        if (two) v |= (unsigned)in[i + 1] << 8;
        b[o++] = B64[(v >> 18) & 0x3f];
        b[o++] = B64[(v >> 12) & 0x3f];
        b[o++] = two ? B64[(v >> 6) & 0x3f] : '=';
        b[o++] = '=';
    }
    b[o] = '\0';
    *out = b;
    return 0;
}

/* Reverse table: 0..63 for a base64 digit, -1 for padding, -2 for
   anything else. Whitespace is not accepted -- nothing in this protocol
   wraps its base64, and quietly skipping bytes is how a truncated field
   becomes a silently wrong one. */
static int b64_val(char c)
{
    const char *p;
    if (c == '=') return -1;
    p = memchr(B64, c, 64);
    return p ? (int)(p - B64) : -2;
}

int es9_b64_decode(const char *in, uint8_t **out, size_t *out_len)
{
    size_t len, i, o = 0;
    uint8_t *b;

    if (!in || !out || !out_len) return -1;
    len = strlen(in);
    if (len % 4 != 0) return -1;
    b = malloc(len / 4 * 3 + 1);
    if (!b) return -1;

    for (i = 0; i < len; i += 4) {
        int v[4], k, pad = 0;
        for (k = 0; k < 4; k++) {
            v[k] = b64_val(in[i + k]);
            if (v[k] == -2) { free(b); return -1; }
            if (v[k] == -1) { pad++; v[k] = 0; }
            else if (pad) { free(b); return -1; } /* padding inside a group */
        }
        if (pad && i + 4 != len) { free(b); return -1; } /* padding not last */
        if (pad > 2) { free(b); return -1; }
        b[o++] = (uint8_t)((v[0] << 2) | (v[1] >> 4));
        if (pad < 2) b[o++] = (uint8_t)((v[1] << 4) | (v[2] >> 2));
        if (pad < 1) b[o++] = (uint8_t)((v[2] << 6) | v[3]);
    }
    *out = b;
    *out_len = o;
    return 0;
}

/* ---- hexadecimal, for transactionId ------------------------------- */

int es9_hex_encode(const uint8_t *in, size_t in_len, char **out)
{
    size_t i;
    char *b;
    if (!in || !out) return -1;
    b = malloc(in_len * 2 + 1);
    if (!b) return -1;
    for (i = 0; i < in_len; i++) sprintf(b + i * 2, "%02X", in[i]);
    *out = b;
    return 0;
}

int es9_hex_decode(const char *in, uint8_t **out, size_t *out_len)
{
    size_t len, i;
    uint8_t *b;
    if (!in || !out || !out_len) return -1;
    len = strlen(in);
    if (len == 0 || len % 2 != 0) return -1;
    b = malloc(len / 2);
    if (!b) return -1;
    for (i = 0; i < len; i += 2) {
        unsigned hi, lo;
        const char *d = "0123456789abcdef";
        const char *p1 = memchr(d, in[i] | 0x20, 16);
        const char *p2 = memchr(d, in[i + 1] | 0x20, 16);
        if (!p1 || !p2) { free(b); return -1; }
        hi = (unsigned)(p1 - d);
        lo = (unsigned)(p2 - d);
        b[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    *out = b;
    *out_len = len / 2;
    return 0;
}

/* ---- the smallest JSON reader that answers this client's questions -- */

int es9_json_string(const char *json, const char *key, char **out)
{
    size_t klen;
    const char *p;
    char pattern[64];

    if (!json || !key || !out) return -1;
    klen = strlen(key);
    if (klen + 3 >= sizeof pattern) return -1;
    pattern[0] = '"';
    memcpy(pattern + 1, key, klen);
    pattern[klen + 1] = '"';
    pattern[klen + 2] = '\0';

    p = strstr(json, pattern);
    if (!p) return -1;
    p += klen + 2;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return -1;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return -1;
    p++;

    {
        const char *start = p;
        size_t n = 0;
        char *b;
        while (*p && *p != '"') {
            if (*p == '\\') {
                /* Only the two escapes a value here could legitimately
                   contain. Anything else means this is not the kind of
                   string this reader claims to handle. */
                if (p[1] != '"' && p[1] != '\\') return -1;
                p += 2;
            } else {
                p++;
            }
            n++;
        }
        if (*p != '"') return -1;
        b = malloc(n + 1);
        if (!b) return -1;
        {
            const char *q = start;
            size_t o = 0;
            while (q < p) {
                if (*q == '\\') { b[o++] = q[1]; q += 2; }
                else { b[o++] = *q++; }
            }
            b[o] = '\0';
        }
        *out = b;
    }
    return 0;
}

/* ---- HTTP --------------------------------------------------------- */

void es9_client_free(es9_client_t *c)
{
    if (!c) return;
    free(c->base_url);
    free(c->ca_file);
    free(c->last_error);
    memset(c, 0, sizeof *c);
}

struct body {
    char *p;
    size_t len;
};

static size_t collect(void *data, size_t sz, size_t n, void *key)
{
    struct body *b = key;
    size_t add = sz * n;
    char *grown = realloc(b->p, b->len + add + 1);
    if (!grown) return 0;
    b->p = grown;
    memcpy(b->p + b->len, data, add);
    b->len += add;
    b->p[b->len] = '\0';
    return add;
}

int es9_post(es9_client_t *c, const char *function, const char *body,
             char **resp)
{
    CURL *h;
    struct curl_slist *hdr = NULL;
    struct body got = { NULL, 0 };
    char url[1024];
    long status = 0;
    CURLcode rc;
    int ret = -2;

    if (!c || !c->base_url || !function || !body || !resp) return -2;
    if (snprintf(url, sizeof url, "%s/gsma/rsp2/es9plus/%s",
                 c->base_url, function) >= (int)sizeof url) {
        return -2;
    }

    h = curl_easy_init();
    if (!h) return -2;

    /* Section 6.2 requires both of these on the request, and names the
       User-Agent value exactly: an LPA in a device is gsma-rsp-lpad. */
    hdr = curl_slist_append(hdr, "Content-Type: application/json");
    hdr = curl_slist_append(hdr, "X-Admin-Protocol: gsma/rsp/v2.6.0");
    hdr = curl_slist_append(hdr, "User-Agent: gsma-rsp-lpad");
    if (!hdr) goto out;

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &got);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, c->timeout_s ? c->timeout_s : 30L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
    /* TLS 1.2 is what SGP.22 section 6.1 mandates; nothing older. */
    curl_easy_setopt(h, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
    if (c->ca_file) curl_easy_setopt(h, CURLOPT_CAINFO, c->ca_file);

    rc = curl_easy_perform(h);
    if (rc != CURLE_OK) {
        free(c->last_error);
        c->last_error = strdup(curl_easy_strerror(rc));
        goto out;
    }
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    /* Section 6.3: a synchronous function answers 200 whether it
       succeeded or failed. Anything else is the transport, not the
       function, and is therefore -2 rather than a refusal. */
    if (status != 200) {
        char msg[64];
        snprintf(msg, sizeof msg, "HTTP %ld", status);
        free(c->last_error);
        c->last_error = strdup(msg);
        goto out;
    }
    if (!got.p) goto out;

    *resp = got.p;
    got.p = NULL;
    ret = 0;

out:
    free(got.p);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(h);
    return ret;
}

/* Did the server say it executed the function? Returns 0 on success,
   -1 on a Failed/Expired status, -2 when the answer has no status at
   all. On -1 the server's own message, if any, lands in last_error. */
static int check_status(es9_client_t *c, const char *resp)
{
    char *status = NULL, *msg = NULL;
    int ret;

    if (es9_json_string(resp, "status", &status) != 0) return -2;
    if (strcmp(status, "Executed-Success") == 0 ||
        strcmp(status, "Executed-WithWarning") == 0) {
        ret = 0;
    } else {
        ret = -1;
        if (es9_json_string(resp, "message", &msg) == 0) {
            free(c->last_error);
            c->last_error = msg;
        } else {
            free(c->last_error);
            c->last_error = status;
            status = NULL;
        }
    }
    free(status);
    return ret;
}

/* Reassemble an ES9+ response's fields back into the encoding the SM-DP+
   produced, so euicc-lpa's repackers can be handed the same bytes they
   would have seen in-process. Every field arrives as its own complete
   TLV, so this only puts the outer tag and length back. */
static int wrap(const uint8_t *tag, size_t tag_len,
                uint8_t *const *fields, const size_t *lens, size_t n,
                uint8_t **out, size_t *out_len)
{
    size_t body = 0, i, o;
    uint8_t len_oct[8];
    size_t len_n = 0;
    uint8_t *b;

    for (i = 0; i < n; i++) body += lens[i];

    /* DER definite length, minimal, same rule euicc-rsp encodes with. */
    if (body < 0x80) { len_oct[0] = (uint8_t)body; len_n = 1; }
    else if (body <= 0xFF) { len_oct[0] = 0x81; len_oct[1] = (uint8_t)body; len_n = 2; }
    else if (body <= 0xFFFF) {
        len_oct[0] = 0x82;
        len_oct[1] = (uint8_t)(body >> 8);
        len_oct[2] = (uint8_t)body;
        len_n = 3;
    } else {
        len_oct[0] = 0x83;
        len_oct[1] = (uint8_t)(body >> 16);
        len_oct[2] = (uint8_t)(body >> 8);
        len_oct[3] = (uint8_t)body;
        len_n = 4;
    }

    b = malloc(tag_len + len_n + body);
    if (!b) return -1;
    memcpy(b, tag, tag_len);
    o = tag_len;
    memcpy(b + o, len_oct, len_n);
    o += len_n;
    for (i = 0; i < n; i++) { memcpy(b + o, fields[i], lens[i]); o += lens[i]; }
    *out = b;
    *out_len = o;
    return 0;
}

static void free_fields(uint8_t **f, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) free(f[i]);
}

int es9_initiate_authentication(es9_client_t *c,
                                const uint8_t *euicc_challenge, size_t challenge_len,
                                const uint8_t *euicc_info1, size_t info1_len,
                                const char *smdp_address,
                                char **transaction_id_hex,
                                uint8_t **auth_server_req, size_t *req_len)
{
    char *chal = NULL, *info = NULL, *body = NULL, *resp = NULL;
    static const char *KEYS[4] = { "serverSigned1", "serverSignature1",
                                   "euiccCiPKIdToBeUsed", "serverCertificate" };
    uint8_t *f[4] = { NULL, NULL, NULL, NULL };
    size_t l[4] = { 0, 0, 0, 0 };
    uint8_t *ok = NULL;
    size_t ok_len = 0;
    int ret = -2, i;

    if (!c || !transaction_id_hex || !auth_server_req || !req_len) return -2;

    if (es9_b64_encode(euicc_challenge, challenge_len, &chal) != 0 ||
        es9_b64_encode(euicc_info1, info1_len, &info) != 0) {
        goto out;
    }
    /* No <JSON requestHeader>: section 6.5.1.1 forbids one on ES9+.
       None of these three values can contain a character JSON would
       need escaped -- two are base64, the third an FQDN. */
    if (asprintf(&body,
                 "{\"euiccChallenge\":\"%s\",\"euiccInfo1\":\"%s\","
                 "\"smdpAddress\":\"%s\"}", chal, info, smdp_address) < 0) {
        body = NULL;
        goto out;
    }
    if (es9_post(c, "initiateAuthentication", body, &resp) != 0) goto out;
    ret = check_status(c, resp);
    if (ret != 0) goto out;
    ret = -2;

    if (es9_json_string(resp, "transactionId", transaction_id_hex) != 0) goto out;
    for (i = 0; i < 4; i++) {
        char *v = NULL;
        if (es9_json_string(resp, KEYS[i], &v) != 0) goto out;
        if (es9_b64_decode(v, &f[i], &l[i]) != 0) { free(v); goto out; }
        free(v);
    }

    /* InitiateAuthenticationOkEs9 is a bare SEQUENCE, and its
       transactionId is the first member -- carried here as hexadecimal
       rather than base64, so it is put back as a TLV of its own. */
    {
        uint8_t *tid = NULL;
        size_t tid_len = 0;
        uint8_t *all[5];
        size_t alllen[5];
        uint8_t *tidtlv = NULL;
        size_t tidtlv_len = 0;
        static const uint8_t T0[1] = { 0x80 };
        static const uint8_t SEQ[1] = { 0x30 };

        if (es9_hex_decode(*transaction_id_hex, &tid, &tid_len) != 0) goto out;
        if (wrap(T0, 1, &tid, &tid_len, 1, &tidtlv, &tidtlv_len) != 0) {
            free(tid);
            goto out;
        }
        free(tid);
        all[0] = tidtlv; alllen[0] = tidtlv_len;
        for (i = 0; i < 4; i++) { all[i + 1] = f[i]; alllen[i + 1] = l[i]; }
        if (wrap(SEQ, 1, all, alllen, 5, &ok, &ok_len) != 0) {
            free(tidtlv);
            goto out;
        }
        free(tidtlv);
    }

    ret = rsp_lpa_repack_authenticate_server(ok, ok_len, auth_server_req, req_len);

out:
    free(chal);
    free(info);
    free(body);
    free(resp);
    free(ok);
    free_fields(f, 4);
    return ret;
}

int es9_authenticate_client(es9_client_t *c, const char *transaction_id_hex,
                            const uint8_t *auth_server_resp, size_t resp_len,
                            uint8_t **prepare_download_req, size_t *req_len)
{
    char *asr = NULL, *body = NULL, *resp = NULL;
    static const char *KEYS[4] = { "profileMetadata", "smdpSigned2",
                                   "smdpSignature2", "smdpCertificate" };
    uint8_t *f[4] = { NULL, NULL, NULL, NULL };
    size_t l[4] = { 0, 0, 0, 0 };
    uint8_t *ok = NULL, *choice = NULL;
    size_t ok_len = 0, choice_len = 0;
    int ret = -2, i;

    if (!c || !transaction_id_hex || !prepare_download_req || !req_len) return -2;

    if (es9_b64_encode(auth_server_resp, resp_len, &asr) != 0) goto out;
    if (asprintf(&body,
                 "{\"transactionId\":\"%s\",\"authenticateServerResponse\":\"%s\"}",
                 transaction_id_hex, asr) < 0) {
        body = NULL;
        goto out;
    }
    if (es9_post(c, "authenticateClient", body, &resp) != 0) goto out;
    ret = check_status(c, resp);
    if (ret != 0) goto out;
    ret = -2;

    for (i = 0; i < 4; i++) {
        char *v = NULL;
        if (es9_json_string(resp, KEYS[i], &v) != 0) goto out;
        if (es9_b64_decode(v, &f[i], &l[i]) != 0) { free(v); goto out; }
        free(v);
    }

    /* AuthenticateClientResponseEs9 is the CHOICE, tag 'BF3B', around an
       authenticateClientOk SEQUENCE -- two levels, unlike the first
       function's bare SEQUENCE. */
    {
        uint8_t *tid = NULL, *tidtlv = NULL;
        size_t tid_len = 0, tidtlv_len = 0;
        uint8_t *all[5];
        size_t alllen[5];
        static const uint8_t T0[1] = { 0x80 };
        static const uint8_t SEQ[1] = { 0x30 };
        static const uint8_t BF3B[2] = { 0xbf, 0x3b };

        if (es9_hex_decode(transaction_id_hex, &tid, &tid_len) != 0) goto out;
        if (wrap(T0, 1, &tid, &tid_len, 1, &tidtlv, &tidtlv_len) != 0) {
            free(tid);
            goto out;
        }
        free(tid);
        all[0] = tidtlv; alllen[0] = tidtlv_len;
        for (i = 0; i < 4; i++) { all[i + 1] = f[i]; alllen[i + 1] = l[i]; }
        if (wrap(SEQ, 1, all, alllen, 5, &ok, &ok_len) != 0) {
            free(tidtlv);
            goto out;
        }
        free(tidtlv);
        if (wrap(BF3B, 2, &ok, &ok_len, 1, &choice, &choice_len) != 0) goto out;
    }

    ret = rsp_lpa_repack_prepare_download(choice, choice_len,
                                          prepare_download_req, req_len);

out:
    free(asr);
    free(body);
    free(resp);
    free(ok);
    free(choice);
    free_fields(f, 4);
    return ret;
}

int es9_get_bound_profile_package(es9_client_t *c, const char *transaction_id_hex,
                                  const uint8_t *prepare_download_resp, size_t resp_len,
                                  uint8_t **bpp, size_t *bpp_len)
{
    char *pdr = NULL, *body = NULL, *resp = NULL, *v = NULL;
    int ret = -2;

    if (!c || !transaction_id_hex || !bpp || !bpp_len) return -2;

    if (es9_b64_encode(prepare_download_resp, resp_len, &pdr) != 0) goto out;
    if (asprintf(&body,
                 "{\"transactionId\":\"%s\",\"prepareDownloadResponse\":\"%s\"}",
                 transaction_id_hex, pdr) < 0) {
        body = NULL;
        goto out;
    }
    if (es9_post(c, "getBoundProfilePackage", body, &resp) != 0) goto out;
    ret = check_status(c, resp);
    if (ret != 0) goto out;
    ret = -2;

    if (es9_json_string(resp, "boundProfilePackage", &v) != 0) goto out;
    if (es9_b64_decode(v, bpp, bpp_len) != 0) goto out;
    ret = 0;

out:
    free(pdr);
    free(body);
    free(resp);
    free(v);
    return ret;
}
