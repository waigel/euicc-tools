/*
 * schematron.c -- run a Schematron rule set with libxslt.
 *
 * The rules of saip-validator are ISO Schematron. The reference way to run
 * them is the ISO skeleton: three transforms turn a .sch file into an XSLT
 * stylesheet, and that stylesheet validates a document and reports in SVRL.
 *
 * Python's lxml does exactly this, and lxml is a binding to libxml2 and
 * libxslt. Doing it here in C removes the Python runtime and changes nothing
 * else: the same skeleton, the same stylesheet, the same SVRL.
 *
 * It also removes the obstacle that a rewrite would have hit. 24 of the
 * expressions in the rule set call generate-id() and three call current().
 * Both are XSLT functions, not XPath, so an XPath evaluator cannot run them.
 * libxslt is an XSLT processor and has both.
 */

#include "euicc.h"

#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>  /* xmlXPathRegisterNs */
#include <libxslt/transform.h>
#include <libxslt/xsltInternals.h>
#include <libxslt/xsltutils.h>

#define SVRL_NS "http://purl.oclc.org/dsdl/svrl"

/* The three transforms of the ISO skeleton, in the order they are applied. */
static const char *const SKELETON[] = {
    "iso_dsdl_include.xsl",
    "iso_abstract_expand.xsl",
    "iso_svrl_for_xslt1.xsl",
};

struct sch_engine {
    xsltStylesheetPtr *compiled;  /* one validator per rule file */
    char **names;
    size_t count;
};

static xmlDocPtr
apply_step(const char *skeldir, const char *step, xmlDocPtr in) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", skeldir, step);

    xsltStylesheetPtr xsl = xsltParseStylesheetFile((const xmlChar *)path);
    if(!xsl) {
        fprintf(stderr, "euicc: cannot read the skeleton transform %s\n", path);
        return NULL;
    }
    xmlDocPtr out = xsltApplyStylesheet(xsl, in, NULL);
    xsltFreeStylesheet(xsl);
    return out;
}

/*
 * A .sch file becomes a stylesheet that validates. The skeleton needs the
 * document on disk rather than in memory for its first step, because
 * iso_dsdl_include.xsl resolves includes against the location of the file.
 */
static xsltStylesheetPtr
compile_rules(const char *skeldir, const char *sch_path) {
    xmlDocPtr doc = xmlReadFile(sch_path, NULL, XML_PARSE_NONET);
    if(!doc) {
        fprintf(stderr, "euicc: cannot read the rule file %s\n", sch_path);
        return NULL;
    }
    for(size_t i = 0; i < sizeof SKELETON / sizeof *SKELETON; i++) {
        xmlDocPtr next = apply_step(skeldir, SKELETON[i], doc);
        xmlFreeDoc(doc);
        if(!next) return NULL;
        doc = next;
    }
    xsltStylesheetPtr validator = xsltParseStylesheetDoc(doc);
    /* validator owns doc from here; freeing the stylesheet frees both. */
    return validator;
}

sch_engine_t *
sch_open(const char *rules_dir, const char *skeldir) {
    glob_t g;
    char pattern[4096];
    snprintf(pattern, sizeof pattern, "%s/*.sch", rules_dir);

    if(glob(pattern, 0, NULL, &g) != 0 || g.gl_pathc == 0) {
        fprintf(stderr, "euicc: no rule file in %s\n", rules_dir);
        globfree(&g);
        return NULL;
    }

    sch_engine_t *e = calloc(1, sizeof *e);
    e->compiled = calloc(g.gl_pathc, sizeof *e->compiled);
    e->names = calloc(g.gl_pathc, sizeof *e->names);

    for(size_t i = 0; i < g.gl_pathc; i++) {
        e->compiled[e->count] = compile_rules(skeldir, g.gl_pathv[i]);
        if(!e->compiled[e->count]) {
            globfree(&g);
            sch_close(e);
            return NULL;
        }
        const char *base = strrchr(g.gl_pathv[i], '/');
        e->names[e->count] = strdup(base ? base + 1 : g.gl_pathv[i]);
        e->count++;
    }
    globfree(&g);
    return e;
}

void
sch_close(sch_engine_t *e) {
    if(!e) return;
    for(size_t i = 0; i < e->count; i++) {
        if(e->compiled[i]) xsltFreeStylesheet(e->compiled[i]);
        free(e->names[i]);
    }
    free(e->compiled);
    free(e->names);
    free(e);
}

/* Read the failed assertions out of one SVRL report. */
static void
collect(xmlDocPtr svrl, const char *rule_file, sch_result_t *res) {
    xmlXPathContextPtr ctx = xmlXPathNewContext(svrl);
    xmlXPathRegisterNs(ctx, (const xmlChar *)"svrl", (const xmlChar *)SVRL_NS);

    xmlXPathObjectPtr fired = xmlXPathEvalExpression(
        (const xmlChar *)"//svrl:fired-rule", ctx);
    if(fired && fired->nodesetval) res->fired += fired->nodesetval->nodeNr;
    xmlXPathFreeObject(fired);

    xmlXPathObjectPtr bad = xmlXPathEvalExpression(
        (const xmlChar *)"//svrl:failed-assert", ctx);
    if(bad && bad->nodesetval) {
        for(int i = 0; i < bad->nodesetval->nodeNr; i++) {
            xmlNodePtr n = bad->nodesetval->nodeTab[i];
            if(res->count == res->capacity) {
                res->capacity = res->capacity ? res->capacity * 2 : 16;
                res->findings = realloc(res->findings,
                                        res->capacity * sizeof *res->findings);
            }
            sch_finding_t *f = &res->findings[res->count++];
            memset(f, 0, sizeof *f);
            f->id = (char *)xmlGetProp(n, (const xmlChar *)"id");
            f->role = (char *)xmlGetProp(n, (const xmlChar *)"role");
            f->location = (char *)xmlGetProp(n, (const xmlChar *)"location");
            f->rule_file = strdup(rule_file);

            xmlXPathContextPtr nc = xmlXPathNewContext(svrl);
            xmlXPathRegisterNs(nc, (const xmlChar *)"svrl", (const xmlChar *)SVRL_NS);
            nc->node = n;
            xmlXPathObjectPtr t = xmlXPathEvalExpression(
                (const xmlChar *)"normalize-space(svrl:text)", nc);
            if(t && t->stringval) f->text = strdup((char *)t->stringval);
            xmlXPathFreeObject(t);
            xmlXPathFreeContext(nc);
        }
    }
    xmlXPathFreeObject(bad);
    xmlXPathFreeContext(ctx);
}

int
sch_validate(sch_engine_t *e, xmlDocPtr package, sch_result_t *res) {
    memset(res, 0, sizeof *res);
    for(size_t i = 0; i < e->count; i++) {
        xmlDocPtr svrl = xsltApplyStylesheet(e->compiled[i], package, NULL);
        if(!svrl) {
            fprintf(stderr, "euicc: %s did not run\n", e->names[i]);
            return -1;
        }
        collect(svrl, e->names[i], res);
        xmlFreeDoc(svrl);
    }
    return 0;
}

void
sch_result_free(sch_result_t *res) {
    for(size_t i = 0; i < res->count; i++) {
        xmlFree(res->findings[i].id);
        xmlFree(res->findings[i].role);
        xmlFree(res->findings[i].location);
        free(res->findings[i].text);
        free(res->findings[i].rule_file);
    }
    free(res->findings);
    memset(res, 0, sizeof *res);
}
