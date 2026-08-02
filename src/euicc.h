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

#endif /* EUICC_H */
