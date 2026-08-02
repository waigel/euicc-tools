/*
 * main.c -- euicc: build and check eUICC profile packages.
 *
 * Three commands, and each one is a step a profile author repeats:
 *
 *   euicc build   value notation in, DER out
 *   euicc show    DER in, value notation out
 *   euicc check   either in, a verdict out
 *
 * check is the one that needed a program rather than a pipeline. A profile
 * package fails in two ways, and until now two tools reported them. The schema
 * states what a value may hold, and the specification states in prose what a
 * package must look like. A profile that satisfies one can violate the other.
 */

#include "euicc.h"

#include <ProfileElement.h>
#include <vn_encoder.h>
#include <libxml/xmlIO.h>
#include <libxslt/xslt.h>

#ifndef EUICC_RULES_DIR
#define EUICC_RULES_DIR "vendor/saip-validator/rules"
#endif
#ifndef EUICC_SKEL_DIR
#define EUICC_SKEL_DIR ""
#endif

#define MAX_PE 4096

/* The reader fails at one place at a time, and both the text report and the
   JSON report want the same detail. */
static char g_parse_error[512];
static int g_parse_element;

/* ---- input --------------------------------------------------------------- */

static unsigned char *
slurp(const char *path, size_t *len) {
    FILE *f = path ? fopen(path, "rb") : stdin;
    if(!f) {
        fprintf(stderr, "euicc: cannot open %s\n", path);
        return NULL;
    }
    size_t cap = 1 << 16, n = 0;
    unsigned char *buf = malloc(cap);
    for(;;) {
        if(n == cap) buf = realloc(buf, cap *= 2);
        size_t got = fread(buf + n, 1, cap - n, f);
        if(got == 0) break;
        n += got;
    }
    if(path) fclose(f);
    *len = n;
    return buf;
}

/*
 * Value notation is text and DER is not. A file that starts with a printable
 * character, a brace or a letter is text; DER starts with a tag byte. This is
 * a guess, and -i settles it when the guess is wrong.
 */
static int
looks_like_text(const unsigned char *b, size_t n) {
    for(size_t i = 0; i < n && i < 64; i++) {
        if(b[i] == '\n' || b[i] == '\r' || b[i] == '\t') continue;
        if(b[i] < 0x20 || b[i] > 0x7e) return 0;
    }
    return n > 0;
}

/* The 1-based line that an offset falls on. */
static int
line_at(const unsigned char *buf, size_t off) {
    int line = 1;
    for(size_t i = 0; i < off; i++)
        if(buf[i] == '\n') line++;
    return line;
}

/*
 * Every ProfileElement in the input, decoded. Returns the count, or -1.
 *
 * The line each value starts on is recorded as well. A constraint failure and
 * a rule failure both name an element and not a position, and an editor needs
 * a position to mark. The line of the element is the closest true answer.
 */
static int
read_package(const unsigned char *buf, size_t len, int as_text,
             ProfileElement_t **out, int *lines, size_t max) {
    size_t off = 0;
    int n = 0;

    while(off < len) {
        /* Trailing whitespace after the last value is not another value. */
        if(as_text) {
            while(off < len && (buf[off] == '\n' || buf[off] == '\r'
                                || buf[off] == ' ' || buf[off] == '\t')) off++;
            if(off >= len) break;
        }
        /* A file of values carries `valueN <Type> ::= ` before each one. That
           is module syntax, and the library steps over it. */
        if(as_text) off = vn_skip_assignment((const char *)buf, len, off);

        if(n == (int)max) {
            fprintf(stderr, "euicc: more than %zu profile elements\n", max);
            return -1;
        }
        if(lines) lines[n] = as_text ? line_at(buf, off) : 1;
        out[n] = NULL;
        asn_dec_rval_t rv;
        if(as_text) {
            char reason[512] = {0};
            vn_read_options_t ro = {0};
            ro.flags = VN_RF_EOF;
            ro.errbuf = reason;
            ro.errlen = sizeof reason;
            rv = vn_decode(0, &asn_DEF_ProfileElement, (void **)&out[n], &ro,
                           buf + off, len - off);
            if(rv.code != RC_OK) {
                snprintf(g_parse_error, sizeof g_parse_error, "%s",
                         reason[0] ? reason : "not value notation");
                g_parse_element = n + 1;
                return -1;
            }
        } else {
            rv = ber_decode(0, &asn_DEF_ProfileElement, (void **)&out[n],
                            buf + off, len - off);
            if(rv.code != RC_OK) {
                fprintf(stderr, "euicc: cannot decode profile element %d\n", n + 1);
                return -1;
            }
        }
        off += rv.consumed;
        n++;
        if(rv.consumed == 0) break;
    }
    return n;
}

/* ---- output -------------------------------------------------------------- */

static int
write_bytes(const void *b, size_t n, void *key) {
    return fwrite(b, 1, n, (FILE *)key) == n ? 0 : -1;
}

/*
 * The rule set reads one <ProfilePackage> that holds every <ProfileElement>.
 * asn1c writes one element at a time, so the wrapper is added here. The Python
 * validator has a shell script that does the same, and both exist because the
 * schema has no type for a whole package.
 */
static xmlDocPtr
package_to_xml(ProfileElement_t **pe, int n) {
    char *xer = NULL;
    size_t xer_len = 0;
    FILE *mem = open_memstream(&xer, &xer_len);
    if(!mem) return NULL;

    /* asn1c names the outer element after the type, so each value already
       arrives as <ProfileElement>…</ProfileElement>. Adding another wrapper
       here nested them twice, and every rule that looks for
       /ProfilePackage/ProfileElement/header then found nothing. */
    fputs("<ProfilePackage>", mem);
    for(int i = 0; i < n; i++) {
        asn_enc_rval_t er = xer_encode(&asn_DEF_ProfileElement, pe[i],
                                       XER_F_BASIC, write_bytes, mem);
        if(er.encoded == -1) {
            fclose(mem);
            free(xer);
            return NULL;
        }
    }
    fputs("</ProfilePackage>", mem);
    fclose(mem);

    xmlDocPtr doc = xmlReadMemory(xer, (int)xer_len, "package.xml", NULL,
                                  XML_PARSE_NONET | XML_PARSE_NOBLANKS);
    free(xer);
    return doc;
}

/* ---- machine-readable report --------------------------------------------- */

static void
json_str(const char *v) {
    putchar('"');
    for(const char *c = v ? v : ""; *c; c++) {
        if(*c == '"' || *c == '\\') printf("\\%c", *c);
        else if((unsigned char)*c < 0x20) printf("\\u%04x", *c);
        else putchar(*c);
    }
    putchar('"');
}

/*
 * An SVRL location is an XPath, and the part this needs is which profile
 * element it points at: /ProfilePackage/ProfileElement[2]/... yields 2. A
 * location without an index belongs to the package and gets element 0.
 */
static int
element_of(const char *location) {
    const char *m = location ? strstr(location, "ProfileElement[") : NULL;
    return m ? atoi(m + strlen("ProfileElement[")) : 0;
}

static void
json_finding(int first, const char *severity, const char *code,
             const char *text, int line, int column, const char *source) {
    printf("%s\n  {", first ? "" : ",");
    printf("\"severity\": "); json_str(severity);
    printf(", \"code\": ");   json_str(code);
    printf(", \"message\": "); json_str(text);
    printf(", \"line\": %d, \"column\": %d", line, column);
    if(source) { printf(", \"source\": "); json_str(source); }
    putchar('}');
}

/* ---- commands ------------------------------------------------------------ */

static int
cmd_build(const char *in, const char *out, int as_text) {
    size_t len = 0;
    unsigned char *buf = slurp(in, &len);
    if(!buf) return 1;
    if(as_text < 0) as_text = looks_like_text(buf, len);

    ProfileElement_t *pe[MAX_PE];
    int lines[MAX_PE];
    int n = read_package(buf, len, as_text, pe, lines, MAX_PE);
    free(buf);
    if(n < 0) return 1;

    /* A value that breaks a SIZE or a range is caught before anything is
       written, so a rejected build leaves no half-correct file behind. */
    for(int i = 0; i < n; i++) {
        char reason[512];
        size_t rlen = sizeof reason;
        if(vn_check_constraints(&asn_DEF_ProfileElement, pe[i], reason, &rlen) != 0) {
            fprintf(stderr, "euicc: profile element %d violates the schema: %s\n",
                    i + 1, reason);
            return 1;
        }
    }

    FILE *f = out ? fopen(out, "wb") : stdout;
    if(!f) {
        fprintf(stderr, "euicc: cannot write %s\n", out);
        return 1;
    }
    for(int i = 0; i < n; i++) {
        asn_enc_rval_t er = der_encode(&asn_DEF_ProfileElement, pe[i],
                                       write_bytes, f);
        if(er.encoded == -1) {
            fprintf(stderr, "euicc: cannot encode profile element %d\n", i + 1);
            return 1;
        }
    }
    if(out) fclose(f);
    fprintf(stderr, "euicc: %d profile element%s written\n", n, n == 1 ? "" : "s");
    return 0;
}

static int
cmd_show(const char *in, int as_text, int annotated) {
    size_t len = 0;
    unsigned char *buf = slurp(in, &len);
    if(!buf) return 1;
    if(as_text < 0) as_text = looks_like_text(buf, len);

    ProfileElement_t *pe[MAX_PE];
    int lines[MAX_PE];
    int n = read_package(buf, len, as_text, pe, lines, MAX_PE);
    free(buf);
    if(n < 0) return 1;

    vn_options_t o = {0};
    o.mode = annotated ? VN_MODE_ANNOTATED : VN_MODE_PRETTY;
    for(int i = 0; i < n; i++) {
        printf("value%d ProfileElement ::= ", i + 1);
        if(vn_fprint(stdout, &asn_DEF_ProfileElement, pe[i], &o) < 0) return 1;
        putchar('\n');
    }
    return 0;
}

static int
cmd_check(const char *in, int as_text, const char *rules, const char *skel,
          int strict, int as_json) {
    size_t len = 0;
    unsigned char *buf = slurp(in, &len);
    if(!buf) return 1;
    if(as_text < 0) as_text = looks_like_text(buf, len);

    ProfileElement_t *pe[MAX_PE];
    int lines[MAX_PE];
    int n = read_package(buf, len, as_text, pe, lines, MAX_PE);
    if(n < 0) {
        /* The reader states a line and a column. An editor can mark that
           exactly, which is the one case where it can. */
        int line = 1, column = 1;
        sscanf(g_parse_error, "line %d column %d", &line, &column);
        const char *colon = strstr(g_parse_error, ": ");
        const char *msg = colon ? colon + 2 : g_parse_error;
        if(as_json) {
            printf("{\n \"findings\": [");
            json_finding(1, "error", "parse", msg, line, column, NULL);
            printf("\n ],\n \"elements\": 0,\n \"fired\": 0\n}\n");
        } else {
            fprintf(stderr, "euicc: cannot read profile element %d: %s\n",
                    g_parse_element, g_parse_error);
        }
        free(buf);
        return 1;
    }
    free(buf);

    int bad = 0, first = 1;
    if(as_json) printf("{\n \"findings\": [");
    for(int i = 0; i < n; i++) {
        char reason[512];
        size_t rlen = sizeof reason;
        if(vn_check_constraints(&asn_DEF_ProfileElement, pe[i], reason, &rlen) != 0) {
            if(as_json) {
                json_finding(first, "error", "schema", reason, lines[i], 1, NULL);
                first = 0;
            } else {
                printf("  schema   profile element %d: %s\n", i + 1, reason);
            }
            bad++;
        }
    }

    sch_engine_t *e = sch_open(rules, skel);
    if(!e) return 2;

    xmlDocPtr doc = package_to_xml(pe, n);
    if(!doc) {
        fprintf(stderr, "euicc: cannot render the package as XML\n");
        sch_close(e);
        return 2;
    }

    sch_result_t res;
    if(sch_validate(e, doc, &res) != 0) {
        xmlFreeDoc(doc);
        sch_close(e);
        return 2;
    }

    int errors = 0, warnings = 0;
    for(size_t i = 0; i < res.count; i++) {
        sch_finding_t *f = &res.findings[i];
        int is_warning = f->role && !strcmp(f->role, "warning");
        if(is_warning) warnings++; else errors++;
        if(as_json) {
            int idx = element_of(f->location);
            json_finding(first, is_warning ? "warning" : "error",
                         f->id ? f->id : "?", f->text ? f->text : "",
                         idx > 0 && idx <= n ? lines[idx - 1] : 1, 1,
                         f->rule_file);
            first = 0;
        } else {
            printf("  %-8s %-13s %s\n", is_warning ? "warning" : "error",
                   f->id ? f->id : "?", f->text ? f->text : "");
            if(f->location) printf("           %-13s %s\n", "at", f->location);
        }
    }

    if(as_json) {
        printf("\n ],\n \"elements\": %d,\n \"fired\": %zu\n}\n", n, res.fired);
        sch_result_free(&res);
        xmlFreeDoc(doc);
        sch_close(e);
        return (errors + bad) || (strict && warnings) ? 1 : 0;
    }

    /* The count is not decoration. A rule whose context does not occur in a
       package did not pass: it did not run. This counts rule instances, which
       is how many times a context matched, and not how many distinct rules
       exist. A package of 30 elements fires the same rule many times. */
    printf("\n%s: %d error%s, %d warning%s, %zu rule instances fired over "
           "%d profile element%s\n",
           in ? in : "stdin", errors + bad, (errors + bad) == 1 ? "" : "s",
           warnings, warnings == 1 ? "" : "s", res.fired, n, n == 1 ? "" : "s");

    sch_result_free(&res);
    xmlFreeDoc(doc);
    sch_close(e);
    return (errors + bad) || (strict && warnings) ? 1 : 0;
}

/* ---- entry --------------------------------------------------------------- */

static void
usage(void) {
    fputs(
        "usage: euicc <command> [options] [file]\n"
        "\n"
        "  build    value notation in, DER out\n"
        "  show     DER in, value notation out\n"
        "  check    either in, a verdict out\n"
        "\n"
        "options:\n"
        "  -o FILE     write here instead of to stdout\n"
        "  -t          the input is value notation\n"
        "  -b          the input is DER or BER\n"
        "  -a          show: add X.680 comments\n"
        "  -s          check: a warning fails the run too\n"
        "  --json      check: one JSON object, for an editor or a script\n"
        "  --rules DIR the rule set, default " EUICC_RULES_DIR "\n"
        "  --skel DIR  the ISO Schematron transforms\n"
        "\n"
        "Without -t or -b the input is read as value notation when it is text.\n",
        stderr);
}

int
main(int argc, char **argv) {
    if(argc < 2) { usage(); return 2; }

    const char *cmd = argv[1];
    const char *in = NULL, *out = NULL;
    const char *rules = EUICC_RULES_DIR, *skel = EUICC_SKEL_DIR;
    int as_text = -1, annotated = 0, strict = 0, as_json = 0;

    for(int i = 2; i < argc; i++) {
        if(!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if(!strcmp(argv[i], "--rules") && i + 1 < argc) rules = argv[++i];
        else if(!strcmp(argv[i], "--skel") && i + 1 < argc) skel = argv[++i];
        else if(!strcmp(argv[i], "-t")) as_text = 1;
        else if(!strcmp(argv[i], "-b")) as_text = 0;
        else if(!strcmp(argv[i], "-a")) annotated = 1;
        else if(!strcmp(argv[i], "-s")) strict = 1;
        else if(!strcmp(argv[i], "--json")) as_json = 1;
        else if(argv[i][0] == '-' && argv[i][1]) { usage(); return 2; }
        else in = argv[i];
    }

    xmlInitParser();
    int rc;
    if(!strcmp(cmd, "build")) rc = cmd_build(in, out, as_text);
    else if(!strcmp(cmd, "show")) rc = cmd_show(in, as_text, annotated);
    else if(!strcmp(cmd, "check")) rc = cmd_check(in, as_text, rules, skel, strict, as_json);
    else { usage(); rc = 2; }

    xsltCleanupGlobals();
    xmlCleanupParser();
    return rc;
}
