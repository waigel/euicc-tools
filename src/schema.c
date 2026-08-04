/*
 * schema.c -- print the schema as JSON, so an editor can offer what fits.
 *
 * An editor completing value notation has to answer one question: at this
 * point in the file, which member names are allowed, and how is each written.
 * asn1c has already parsed the schema and the answer sits in its runtime
 * descriptors, so nothing here parses ASN.1. A second parser would be a second
 * thing that can disagree with the first.
 *
 * The output is a map of types rather than a list of paths. euicc-schema
 * writes paths, because documentation wants a page per place a thing occurs;
 * an editor wants the opposite. One File is referenced by a dozen members, and
 * a path list repeats its whole subtree under each of them, while a client
 * completing a file needs it once and follows the name. A type map also has no
 * depth to choose: a type that referred to itself would be a name pointing at
 * an entry, not a branch that has to be cut off somewhere and marked. This
 * schema has no such type -- the graph of the 86 reachable ones is acyclic --
 * but the map costs nothing for the property either way.
 *
 * Three things the client cannot work out for itself, and each changes what it
 * must insert:
 *
 *   kind      a CHOICE alternative is written `name : value` and a SEQUENCE
 *             member `name value`. Only the parent's kind says which.
 *   optional  an OPTIONAL member may be left out; a mandatory one may not,
 *             and the reader rejects the value if it is.
 *   names     INTEGER named numbers and BIT STRING named bits. asn1c parses
 *             these identifiers and keeps none of them, so they come from the
 *             same table the reader and the writer use, looked up with the
 *             same key, because a second key would find a different answer.
 */

#include "euicc.h"

#include <ProfileElement.h>
#include <vn_encoder.h>

#include "vn_internal.h" /* vn_member_key, vn_names_for */

#define VN_OPTAB(name) extern asn_TYPE_operation_t name
VN_OPTAB(asn_OP_CHOICE);
VN_OPTAB(asn_OP_OPEN_TYPE);
VN_OPTAB(asn_OP_SEQUENCE);
VN_OPTAB(asn_OP_SEQUENCE_OF);
VN_OPTAB(asn_OP_SET);
VN_OPTAB(asn_OP_SET_OF);

extern const vn_annotations_t vn_generated_annotations;

#ifndef EUICC_SCHEMA_FILE
#define EUICC_SCHEMA_FILE ""
#endif

/* ---- names --------------------------------------------------------------- */

/*
 * asn1c gives an inline definition the name of its construction, so several
 * unrelated types are all called CHOICE. The same test the writer applies:
 * a real type reference begins with a capital and is not one of these.
 */
static int
is_real_type_name(const char *name) {
    static const char *const anonymous[] = {"SEQUENCE", "SET", "CHOICE",
                                            "SEQUENCE OF", "SET OF"};
    size_t i;

    if(!name || name[0] < 'A' || name[0] > 'Z') return 0;
    for(i = 0; i < sizeof anonymous / sizeof anonymous[0]; i++)
        if(!strcmp(name, anonymous[i])) return 0;
    return 1;
}

static const char *
kind_of(const asn_TYPE_descriptor_t *td) {
    if(td->op == &asn_OP_SEQUENCE) return "SEQUENCE";
    if(td->op == &asn_OP_SET) return "SET";
    if(td->op == &asn_OP_CHOICE) return "CHOICE";
    if(td->op == &asn_OP_OPEN_TYPE) return "OPEN TYPE";
    if(td->op == &asn_OP_SEQUENCE_OF) return "SEQUENCE OF";
    if(td->op == &asn_OP_SET_OF) return "SET OF";
    return td->name && td->name[0] ? td->name : "";
}

static int
is_constructed(const asn_TYPE_descriptor_t *td) {
    return td->elements_count > 0;
}

/* ---- the set of types already written ------------------------------------ */

/*
 * One entry per constructed type. It ends the recursion and it keeps the name
 * of an inline type stable: whichever member reaches it first names it, and
 * every later reference finds that name rather than inventing a second.
 */
typedef struct {
    const asn_TYPE_descriptor_t *td;
    char                         name[VN_MEMBER_KEY_MAX];
    int                          written;
} sc_type_t;

typedef struct {
    sc_type_t *v;
    size_t     count, cap;
} sc_set_t;

static sc_type_t *
sc_find(sc_set_t *s, const asn_TYPE_descriptor_t *td) {
    size_t i;
    for(i = 0; i < s->count; i++)
        if(s->v[i].td == td) return &s->v[i];
    return NULL;
}

/*
 * Give a constructed type its name and remember it. An inline type has no
 * name of its own, so it takes the member key the annotation table would use
 * for it: PE-AKAParameter__algoConfiguration. Returns NULL only on a failure
 * to allocate.
 */
static sc_type_t *
sc_intern(sc_set_t *s, const asn_TYPE_descriptor_t *td, const char *key) {
    sc_type_t *e = sc_find(s, td);
    if(e) return e;

    if(s->count == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 64;
        sc_type_t *v = realloc(s->v, cap * sizeof *v);
        if(!v) return NULL;
        s->v = v;
        s->cap = cap;
    }
    e = &s->v[s->count++];
    e->td = td;
    e->written = 0;
    if(is_real_type_name(td->name))
        snprintf(e->name, sizeof e->name, "%s", td->name);
    else if(key && key[0])
        snprintf(e->name, sizeof e->name, "%s", key);
    else
        snprintf(e->name, sizeof e->name, "%s", "(anonymous)");
    return e;
}

/* ---- JSON ---------------------------------------------------------------- */

static void
json_string(FILE *f, const char *s) {
    fputc('"', f);
    for(; s && *s; s++) {
        switch(*s) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f);  break;
        default:
            if((unsigned char)*s < 0x20)
                fprintf(f, "\\u%04x", (unsigned char)*s);
            else
                fputc(*s, f);
        }
    }
    fputc('"', f);
}

/*
 * The identifiers a member accepts in place of a number, if it has any. The
 * key is built exactly as the writer builds it, because an inline INTEGER
 * shares one descriptor with every other plain INTEGER and its own name would
 * match the wrong table entry or none.
 */
static void
write_names(FILE *f, const char *member_key, const asn_TYPE_descriptor_t *td) {
    const vn_type_names_t *n =
        vn_names_for(&vn_generated_annotations, member_key, td);
    size_t i;

    if(!n || !n->count) return;
    fputs(", \"names\": [", f);
    for(i = 0; i < n->count; i++) {
        if(i) fputs(", ", f);
        json_string(f, n->values[i].name);
    }
    fputc(']', f);
    if(n->is_bit_string) fputs(", \"namedBits\": true", f);
}

/* ---- the walk ------------------------------------------------------------ */

/*
 * Write one type and intern every constructed type it reaches. The scope
 * buffer follows the writer's discipline: an anonymous parent does not restart
 * the key, so it is saved and restored around each member rather than rebuilt.
 */
static int
write_type(FILE *f, sc_set_t *s, sc_type_t *self, char *scope, size_t scopesz) {
    const asn_TYPE_descriptor_t *td = self->td;
    unsigned i;
    int first = 1;

    fputs("  ", f);
    json_string(f, self->name);
    fputs(": {\"kind\": ", f);
    json_string(f, kind_of(td));

    /*
     * A SEQUENCE OF has one member and it carries no name: a value of it is
     * written as a brace list, and what goes inside is the member's type. The
     * client needs the type, not a name there is none of.
     */
    if(td->op == &asn_OP_SEQUENCE_OF || td->op == &asn_OP_SET_OF) {
        const asn_TYPE_member_t *m = &td->elements[0];
        char saved[VN_MEMBER_KEY_MAX];

        memcpy(saved, scope, sizeof saved);
        vn_member_key(scope, scopesz, td->name, "Member");
        fputs(", \"of\": ", f);
        if(is_constructed(m->type)) {
            sc_type_t *e = sc_intern(s, m->type, scope);
            if(!e) return -1;
            json_string(f, e->name);
        } else {
            json_string(f, kind_of(m->type));
        }
        memcpy(scope, saved, sizeof saved);
        fputs("}", f);
        return 0;
    }

    fputs(", \"members\": [", f);
    for(i = 0; i < td->elements_count; i++) {
        const asn_TYPE_member_t *m = &td->elements[i];
        char saved[VN_MEMBER_KEY_MAX];

        if(!m->type || !m->name || !m->name[0]) continue;

        if(!first) fputs(",", f);
        first = 0;
        fputs("\n    {\"name\": ", f);
        json_string(f, m->name);

        memcpy(saved, scope, sizeof saved);
        vn_member_key(scope, scopesz, td->name, m->name);

        fputs(", \"type\": ", f);
        if(is_constructed(m->type)) {
            sc_type_t *e = sc_intern(s, m->type, scope);
            if(!e) return -1;
            json_string(f, e->name);
        } else {
            json_string(f, kind_of(m->type));
            write_names(f, scope, m->type);
        }
        memcpy(scope, saved, sizeof saved);

        fprintf(f, ", \"optional\": %s}", m->optional ? "true" : "false");
    }
    fputs(first ? "]}" : "\n  ]}", f);
    return 0;
}

int
cmd_schema(FILE *f) {
    const asn_TYPE_descriptor_t *root = &asn_DEF_ProfileElement;
    char scope[VN_MEMBER_KEY_MAX] = {0};
    sc_set_t s = {0};
    sc_type_t *e;
    size_t i;
    int rc = 0, first = 1;

    if(!(e = sc_intern(&s, root, ""))) {
        fprintf(stderr, "euicc: out of memory\n");
        return 1;
    }

    fputs("{\n \"root\": ", f);
    json_string(f, e->name);
    /*
     * Where the schema is written down. Not where it is read from -- that is
     * asn1c's descriptors, and stays so -- but an editor showing a member can
     * point at the line that declares it, the way TypeScript points at a
     * property's declaration.
     */
    fputs(",\n \"source\": ", f);
    json_string(f, EUICC_SCHEMA_FILE);
    fputs(",\n \"types\": {\n", f);

    /*
     * Interning grows the set while the loop runs, which is the point: a type
     * reached for the first time is appended and the loop arrives at it. The
     * index cannot be cached and the scan restarts, because writing a type may
     * have added several.
     */
    for(;;) {
        sc_type_t *next = NULL;
        for(i = 0; i < s.count; i++)
            if(!s.v[i].written) { next = &s.v[i]; break; }
        if(!next) break;

        next->written = 1;
        if(!first) fputs(",\n", f);
        first = 0;
        scope[0] = '\0';
        if(write_type(f, &s, next, scope, sizeof scope) != 0) {
            fprintf(stderr, "euicc: out of memory\n");
            rc = 1;
            break;
        }
    }

    fputs("\n }\n}\n", f);
    free(s.v);
    return rc;
}
