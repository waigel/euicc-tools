/*
 * diff.c -- what separates a source file from a package.
 *
 * This began as `euicc plan`, on the model of Terraform, and the analogy did
 * not hold. Terraform needs a plan because apply changes live infrastructure,
 * because the current state is not visible without asking, and because one
 * line of configuration can destroy a database. None of the three is true
 * here: build writes a local file, show prints what a package holds, and the
 * value notation is the profile.
 *
 * What survived is the case that git diff cannot cover: one side is text and
 * the other is bytes. A package from a vendor, or from an earlier release,
 * against the source that is supposed to produce it.
 *
 * Identity is the question a diff has to settle first. Two profile elements
 * are the same element when they are the same kind and carry the same
 * identification: the kind is the CHOICE alternative, and clause 8.1.3 makes
 * the identification unique within a package. The Profile Header has no
 * identification, and there is exactly one of it, so its kind alone is enough.
 *
 * The comparison itself is over DER. Two elements that encode to the same
 * bytes are the same value, whatever the source text looked like, so a
 * reformatted file reports no change.
 */

#include "euicc.h"

#include <ProfileElement.h>

#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

/* ---- identity ------------------------------------------------------------ */

/*
 * The key of one element, from its XML. The first child of <ProfileElement>
 * names the alternative; the first identification below it, if any, numbers
 * the instance.
 */
static char *
element_key(xmlNodePtr pe) {
    const char *kind = "?";
    for(xmlNodePtr c = pe->children; c; c = c->next) {
        if(c->type == XML_ELEMENT_NODE) { kind = (const char *)c->name; break; }
    }

    char *id = NULL;
    xmlXPathContextPtr ctx = xmlXPathNewContext(pe->doc);
    ctx->node = pe;
    xmlXPathObjectPtr o = xmlXPathEvalExpression(
        (const xmlChar *)"string((.//identification)[1])", ctx);
    if(o && o->stringval && *o->stringval) id = strdup((char *)o->stringval);
    xmlXPathFreeObject(o);
    xmlXPathFreeContext(ctx);

    char *key = malloc(strlen(kind) + (id ? strlen(id) : 0) + 2);
    sprintf(key, "%s#%s", kind, id ? id : "");
    free(id);
    return key;
}

/* ---- the diff ------------------------------------------------------------ */

typedef struct {
    char *key;
    unsigned char *der;
    size_t der_len;
    int index;      /* 1-based position in its own package */
} diff_item_t;

typedef struct {
    diff_item_t *items;
    int count;
} diff_side_t;

static int
append_der(const void *b, size_t n, void *key) {
    diff_item_t *it = key;
    it->der = realloc(it->der, it->der_len + n);
    memcpy(it->der + it->der_len, b, n);
    it->der_len += n;
    return 0;
}

/* Encode each element and read its key out of the XML of the same package. */
static int
side_build(diff_side_t *side, ProfileElement_t **pe, int n, xmlDocPtr doc) {
    side->items = calloc(n > 0 ? n : 1, sizeof *side->items);
    side->count = 0;

    xmlNodePtr root = xmlDocGetRootElement(doc);
    xmlNodePtr node = root ? root->children : NULL;

    for(int i = 0; i < n; i++) {
        while(node && node->type != XML_ELEMENT_NODE) node = node->next;
        diff_item_t *it = &side->items[side->count];
        it->index = i + 1;
        it->key = node ? element_key(node) : strdup("?#");
        asn_enc_rval_t er = der_encode(&asn_DEF_ProfileElement, pe[i],
                                       append_der, it);
        if(er.encoded == -1) return -1;
        side->count++;
        if(node) node = node->next;
    }
    return 0;
}

static void
side_free(diff_side_t *s) {
    for(int i = 0; i < s->count; i++) {
        free(s->items[i].key);
        free(s->items[i].der);
    }
    free(s->items);
}

static diff_item_t *
find(diff_side_t *s, const char *key) {
    for(int i = 0; i < s->count; i++)
        if(!strcmp(s->items[i].key, key)) return &s->items[i];
    return NULL;
}

/* A key reads better as "kind, identification 7" than as "kind#7". */
static void
print_key(const char *key) {
    const char *hash = strchr(key, '#');
    if(!hash) { printf("%s", key); return; }
    printf("%.*s", (int)(hash - key), key);
    if(hash[1]) printf(", identification %s", hash + 1);
}

int
diff_report(struct ProfileElement **want_, int want_n, xmlDocPtr want_xml,
            struct ProfileElement **have_, int have_n, xmlDocPtr have_xml) {
    ProfileElement_t **want = (ProfileElement_t **)want_;
    ProfileElement_t **have = (ProfileElement_t **)have_;
    diff_side_t w = {0}, h = {0};
    if(side_build(&w, want, want_n, want_xml) != 0) return -1;
    if(side_build(&h, have, have_n, have_xml) != 0) {
        side_free(&w);
        return -1;
    }

    int added = 0, removed = 0, changed = 0, same = 0;

    for(int i = 0; i < w.count; i++) {
        diff_item_t *a = &w.items[i];
        diff_item_t *b = find(&h, a->key);
        if(!b) {
            printf("  + ");
            print_key(a->key);
            printf("  (%zu bytes)\n", a->der_len);
            added++;
        } else if(a->der_len != b->der_len
                  || memcmp(a->der, b->der, a->der_len) != 0) {
            printf("  ~ ");
            print_key(a->key);
            printf("  (%zu -> %zu bytes)\n", b->der_len, a->der_len);
            changed++;
        } else {
            same++;
        }
    }

    for(int i = 0; i < h.count; i++) {
        if(!find(&w, h.items[i].key)) {
            printf("  - ");
            print_key(h.items[i].key);
            printf("  (%zu bytes)\n", h.items[i].der_len);
            removed++;
        }
    }

    /*
     * The order of the elements carries meaning: half the rule set is about
     * what comes before what. A package whose elements are all unchanged can
     * still be a different package.
     */
    int reordered = 0;
    if(added == 0 && removed == 0) {
        for(int i = 0; i < w.count && i < h.count; i++)
            if(strcmp(w.items[i].key, h.items[i].key) != 0) reordered = 1;
        if(reordered) printf("  ! the elements are in a different order\n");
    }

    if(!added && !removed && !changed && !reordered)
        printf("  no difference\n");

    printf("\n%d added, %d changed, %d removed, %d unchanged\n",
           added, changed, removed, same);

    side_free(&w);
    side_free(&h);
    return added + changed + removed + reordered;
}
