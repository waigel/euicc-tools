/*
 * format.c -- lay out value notation, one member to a line.
 *
 * This lives in the tool and not in the editor for the reason everything else
 * here does: a second reading of the language is a second thing that can
 * disagree with the first. It was in the editor once, in TypeScript, and the
 * duplicate lexer promptly shortened a cstring that ran over two lines while
 * its own check said nothing had changed.
 *
 * It reads text and does not go through the writer. `euicc show` produces
 * canonical value notation and looks like a formatter until you notice it
 * re-serialises a decoded value: every comment is gone and
 * `myHeader ProfileElement ::=` comes back `value1`. Format on save would
 * delete documentation without a word.
 *
 * The guarantee is the token list. Comparing text with whitespace collapsed
 * does not work, because a break put where there was nothing at all turns
 * `},ef-dir` into `}, ef-dir`; comparing it with whitespace removed would not
 * notice `major-version 2` becoming `major-version2`. So the tokens are
 * compared, and if they differ the input is returned untouched -- a formatter
 * that loses a comment is worse than one that does nothing.
 */

#include "euicc.h"

#include <ctype.h>

/* ---- a growing buffer ---------------------------------------------------- */

typedef struct {
    char  *p;
    size_t len, cap;
} buf_t;

static int
buf_put(buf_t *b, const char *s, size_t n) {
    if(b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while(cap < b->len + n + 1) cap *= 2;
        char *p = realloc(b->p, cap);
        if(!p) return -1;
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return 0;
}

static int
buf_putc(buf_t *b, char c) {
    return buf_put(b, &c, 1);
}

/* ---- tokens -------------------------------------------------------------- */

/*
 * The lexical items of X.680 clause 12, as one string with each token on its
 * own line. Two files whose token strings match hold the same value notation,
 * whatever the whitespace between them.
 */
static int
fmt_tokens(const char *s, size_t n, buf_t *out) {
    size_t i = 0;

    while(i < n) {
        char c = s[i];

        if(isspace((unsigned char)c)) { i++; continue; }

        if(c == '-' && i + 1 < n && s[i + 1] == '-') {
            size_t j = i;
            while(j < n && s[j] != '\n') j++;
            /* Trailing whitespace is not part of what a comment says. */
            size_t e = j;
            while(e > i && isspace((unsigned char)s[e - 1])) e--;
            if(buf_put(out, s + i, e - i) || buf_putc(out, '\n')) return -1;
            i = j;
            continue;
        }
        if(c == '/' && i + 1 < n && s[i + 1] == '*') {
            size_t j = i + 2;
            while(j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) j++;
            j = (j + 1 < n) ? j + 2 : n;
            if(buf_put(out, s + i, j - i) || buf_putc(out, '\n')) return -1;
            i = j;
            continue;
        }
        if(c == '"' || c == '\'') {
            size_t j = i + 1;
            while(j < n && s[j] != c) j++;
            j = (j < n) ? j + 1 : n;
            if(c == '\'') {
                /*
                 * X.680 12.11 and 12.12: the whitespace inside an hstring or a
                 * bstring is not part of the value, so one may be wrapped over
                 * lines freely and two spellings of it are the same token.
                 */
                for(size_t k = i; k < j; k++)
                    if(!isspace((unsigned char)s[k]) && buf_putc(out, s[k])) return -1;
                if(j < n && strchr("HhBb", s[j])) {
                    if(buf_putc(out, s[j])) return -1;
                    j++;
                }
            } else {
                /* A cstring is text and every character of one counts. */
                if(buf_put(out, s + i, j - i)) return -1;
            }
            if(buf_putc(out, '\n')) return -1;
            i = j;
            continue;
        }
        if(isalnum((unsigned char)c) || c == '-') {
            size_t j = i;
            while(j < n && (isalnum((unsigned char)s[j]) || s[j] == '-')) j++;
            if(buf_put(out, s + i, j - i) || buf_putc(out, '\n')) return -1;
            i = j;
            continue;
        }
        if(buf_putc(out, c) || buf_putc(out, '\n')) return -1;
        i++;
    }
    return 0;
}

/* ---- the layout ---------------------------------------------------------- */

/* Past the spaces and at most one newline after a break we made ourselves, so
   the newline that was already there does not become a blank line. */
static size_t
swallow(const char *s, size_t n, size_t i) {
    while(i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if(i < n && s[i] == '\r') i++;
    if(i < n && s[i] == '\n') {
        i++;
        while(i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    }
    return i;
}

/* An OBJECT IDENTIFIER is a brace list of arcs and the writer keeps it on one
   line: `{ 2 23 143 1 2 1 }`. Numbers only is what tells such a group from a
   value, and breaking them put ninety extra lines in a published profile. */
static size_t
arc_list_end(const char *s, size_t n, size_t open, char close) {
    for(size_t j = open + 1; j < n; j++) {
        if(s[j] == close) return j;
        if(!isdigit((unsigned char)s[j]) && !isspace((unsigned char)s[j])) break;
    }
    return 0;
}

struct layout_state {
    buf_t  out;
    buf_t  line;
    int    depth;
    int    pending;   /* the depth this line is written at, fixed as it starts */
    int    in_block;
    char   in_quote;  /* 0, '"' or '\'' */
    int    verbatim;  /* a continuation line of a comment or a literal */
    const char *unit;
};

static int
flush(struct layout_state *st) {
    const char *p = st->line.p ? st->line.p : "";
    size_t len = st->line.len;

    if(st->verbatim) {
        /* The author's line. Inside a cstring not even the trailing whitespace
           may go, because every character of one counts. */
        size_t e = len;
        if(st->in_quote != '"')
            while(e > 0 && isspace((unsigned char)p[e - 1])) e--;
        if(buf_put(&st->out, p, e)) return -1;
    } else {
        size_t b = 0, e = len;
        while(b < e && isspace((unsigned char)p[b])) b++;
        while(e > b && isspace((unsigned char)p[e - 1])) e--;
        if(e > b) {
            for(int k = 0; k < st->pending; k++)
                if(buf_put(&st->out, st->unit, strlen(st->unit))) return -1;
            if(buf_put(&st->out, p + b, e - b)) return -1;
        }
    }
    if(buf_putc(&st->out, '\n')) return -1;

    st->line.len = 0;
    if(st->line.p) st->line.p[0] = '\0';
    st->verbatim = st->in_block || st->in_quote != 0;
    st->pending = st->depth;
    return 0;
}

/* Only whitespace so far on this line? */
static int
line_blank(const struct layout_state *st) {
    for(size_t k = 0; k < st->line.len; k++)
        if(!isspace((unsigned char)st->line.p[k])) return 0;
    return 1;
}

static char *
lay_out(const char *s, size_t n, const char *unit) {
    struct layout_state st = {0};
    st.unit = unit;

    for(size_t i = 0; i < n; i++) {
        char c = s[i];

        /*
         * A cstring may run over lines, so the whole literal stays in one
         * accumulated line and goes out as written. The newline is handled
         * before the quote state below, which is why the guard is here:
         * flushing at it trimmed the trailing whitespace off the first line of
         * the string, and the check could not see it.
         */
        if(c == '\n') {
            if(st.in_quote == '"') { if(buf_putc(&st.line, c)) goto fail; continue; }
            if(flush(&st)) goto fail;
            continue;
        }
        if(c == '\r') continue;

        if(st.in_block) {
            if(buf_putc(&st.line, c)) goto fail;
            if(c == '/' && i > 0 && s[i - 1] == '*') st.in_block = 0;
            continue;
        }
        if(st.in_quote) {
            if(buf_putc(&st.line, c)) goto fail;
            if(c == st.in_quote) st.in_quote = 0;
            continue;
        }
        if(c == '-' && i + 1 < n && s[i + 1] == '-') {
            while(i < n && s[i] != '\n')
                if(buf_putc(&st.line, s[i++])) goto fail;
            i--;
            continue;
        }
        if(c == '/' && i + 1 < n && s[i + 1] == '*') {
            st.in_block = 1;
            if(buf_put(&st.line, "/*", 2)) goto fail;
            i++;
            continue;
        }
        if(c == '"' || c == '\'') {
            st.in_quote = c;
            if(buf_putc(&st.line, c)) goto fail;
            continue;
        }

        if(c == '}' || c == ']') {
            /* The brace closes something, so it belongs at that level and it
               starts its own line. */
            if(!line_blank(&st) && flush(&st)) goto fail;
            if(st.depth > 0) st.depth--;
            st.pending = st.depth;
            if(buf_putc(&st.line, c)) goto fail;
            continue;
        }
        if(c == '{' || c == '[') {
            size_t close = arc_list_end(s, n, i, c == '{' ? '}' : ']');
            if(close) {
                size_t b = i + 1, e = close;
                while(b < e && isspace((unsigned char)s[b])) b++;
                while(e > b && isspace((unsigned char)s[e - 1])) e--;
                if(buf_putc(&st.line, c)) goto fail;
                if(e > b) {
                    if(buf_putc(&st.line, ' ')) goto fail;
                    if(buf_put(&st.line, s + b, e - b)) goto fail;
                }
                if(buf_putc(&st.line, ' ') || buf_putc(&st.line, s[close])) goto fail;
                i = close;
                continue;
            }
            if(buf_putc(&st.line, c)) goto fail;
            st.depth++;
            if(flush(&st)) goto fail;
            i = swallow(s, n, i + 1) - 1;
            continue;
        }
        if(c == ',') {
            if(buf_putc(&st.line, c)) goto fail;
            size_t j = i + 1;
            while(j < n && (s[j] == ' ' || s[j] == '\t')) j++;
            /* A comment after the comma stays on the line it comments on. */
            if(!(j + 1 < n && s[j] == '-' && s[j + 1] == '-')) {
                if(flush(&st)) goto fail;
                i = swallow(s, n, j) - 1;
            }
            continue;
        }
        if(buf_putc(&st.line, c)) goto fail;
    }
    if(flush(&st)) goto fail;
    free(st.line.p);

    /* No run of blank lines longer than one, and exactly one at the end. */
    char *p = st.out.p ? st.out.p : strdup("");
    size_t w = 0, blanks = 0;
    for(size_t r = 0; p[r]; r++) {
        if(p[r] == '\n') {
            if(++blanks > 2) continue;
        } else {
            blanks = 0;
        }
        p[w++] = p[r];
    }
    while(w > 0 && isspace((unsigned char)p[w - 1])) w--;
    p[w] = '\0';
    buf_t tail = {p, w, st.out.cap};
    if(buf_putc(&tail, '\n')) { free(tail.p); return NULL; }
    return tail.p;

fail:
    free(st.line.p);
    free(st.out.p);
    return NULL;
}

/* ---- the command --------------------------------------------------------- */

char *
fmt_layout(const char *text, size_t len, const char *unit, char **why) {
    *why = NULL;
    char *out = lay_out(text, len, unit ? unit : "    ");
    if(!out) { *why = "out of memory"; return NULL; }

    buf_t a = {0}, b = {0};
    if(fmt_tokens(text, len, &a) || fmt_tokens(out, strlen(out), &b)) {
        free(a.p); free(b.p); free(out);
        *why = "out of memory";
        return NULL;
    }
    int same = a.p && b.p && !strcmp(a.p, b.p);
    free(a.p);
    free(b.p);
    if(!same) {
        free(out);
        *why = "the layout would have changed the file, so it was left alone";
        return NULL;
    }
    return out;
}
