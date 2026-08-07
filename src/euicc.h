/*
 * euicc.h -- what the commands share.
 *
 * euicc builds and checks eUICC profile packages. Two engines sit behind it,
 * and both are libraries rather than programs it runs:
 *
 *   asn1c-vn          reads and writes ASN.1 value notation, and checks the
 *                     subtype constraints of the schema
 *   libxml2/libxslt   runs the Schematron rule set that carries what the
 *                     specification states in prose
 *
 * Neither needs a separate installation, which is the reason this is C. The
 * codec links directly, with no foreign function interface. The rule engine is
 * the same libxslt that Python's lxml wraps, so the rules run unchanged.
 */

#ifndef EUICC_H
#define EUICC_H

#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libxml/tree.h>

/* ---- Schematron ---------------------------------------------------------- */

typedef struct sch_engine sch_engine_t;

typedef struct {
    char *id;         /* the rule identifier, SAIP-HDR-02 */
    char *role;       /* error or warning */
    char *text;       /* the requirement, as the rule states it */
    char *location;   /* an XPath to the place in the document */
    char *rule_file;  /* the .sch file it came from */
} sch_finding_t;

typedef struct {
    sch_finding_t *findings;
    size_t count;
    size_t capacity;
    /*
     * How many rules ran at all. A package that trips nothing is not the same
     * as a rule set that never fired, and a report that gives only the number
     * of failures cannot tell the two apart.
     */
    size_t fired;
} sch_result_t;

sch_engine_t *sch_open(const char *rules_dir, const char *skeldir);
void sch_close(sch_engine_t *e);
int sch_validate(sch_engine_t *e, xmlDocPtr package, sch_result_t *res);
void sch_result_free(sch_result_t *res);

/* ---- diff ---------------------------------------------------------------- */

/*
 * What separates two packages, as text or as one JSON object. Returns how
 * many differences there are -- a pure reordering counts as one, because a
 * package whose elements all match can still be a different package -- or -1
 * on a failure.
 */
struct ProfileElement;
int diff_report(struct ProfileElement **want, int want_n, xmlDocPtr want_xml,
                struct ProfileElement **have, int have_n, xmlDocPtr have_xml,
                int as_json);

/* ---- card ------------------------------------------------------------ */

/*
 * euicc card info / euicc card profiles: what a physical eUICC (or a
 * recording standing in for one, over --replay) says about itself, and
 * whether it trusts this project's test Certificate Issuer. argv[0] is
 * the subcommand; argc/argv cover everything after "card" on the command
 * line, including it. See src/card.c's own header for the exit code's
 * exact meaning -- it carries a verdict, not just an outcome.
 */
int cmd_card(int argc, char **argv);

/* ---- schema -------------------------------------------------------------- */

/*
 * The schema as JSON, read from asn1c's descriptors, for an editor that has to
 * offer the members allowed at a point in a file.
 */
int cmd_schema(FILE *f);

/* ---- format -------------------------------------------------------------- */

/*
 * Value notation laid out, one member to a line. The caller frees it. On a
 * refusal it returns NULL and sets *why, which is a static string.
 *
 * It reads text and never goes through the writer: `euicc show` re-serialises a
 * decoded value and every comment in the file is gone. The guarantee is that
 * the token list is unchanged, checked here, and a file whose tokens would move
 * is returned untouched.
 */
char *fmt_layout(const char *text, size_t len, const char *unit, char **why);

#endif /* EUICC_H */
