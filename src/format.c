/*
 * format.c -- lay out value notation, one member to a line.
 *
 * The shape is hclwrite's, the library under `terraform fmt`, and the point of
 * that shape is what a token is allowed to carry:
 *
 *     type Token struct {
 *         Type         hclsyntax.TokenType
 *         Bytes        []byte
 *         SpacesBefore int
 *     }
 *     // Formatting must change only whitespace. Specifically, that means
 *     // changing the SpacesBefore attribute on a token while leaving the
 *     // other token attributes unchanged.        -- hclwrite/format.go
 *
 * The bytes of a token are never touched; the passes may write only the
 * whitespace counts in front of it, and the writer emits counts then bytes,
 * verbatim. Losing a character of content is not something a pass can do by
 * mistake, because no pass holds a pen that writes content. The first version
 * of this file built its output character by character instead, and it
 * shortened a cstring running over two lines while its own check said nothing
 * had moved.
 *
 * One deviation from hclwrite, and it is forced by the grammar. HCL separates
 * attributes with newlines, so its formatter never has to insert a line break,
 * and SpacesBefore is the only mutable field. Value notation separates members
 * with commas and a newline means nothing, so `}, ef-dir {` on one line is
 * valid and laying it out means breaking it. Each token therefore carries
 * NewlinesBefore as well.
 *
 * The lexer here is the formatter's own, which hclwrite did not need: it reuses
 * the parser's lexer, whose comment tokens survive. Ours cannot be reused --
 * vn_token.c's vt_skip_filler discards whitespace and comments by design,
 * which is right for a reader and useless for a formatter. The comment rules
 * are the library's, though: X.680 12.6.3 ends a one-line comment at the next
 * "--" or the end of the line, and 12.6.4 nests the bracketed form. An earlier
 * lexer here ran every line comment to the end of the line.
 *
 * It reads text and never goes through the writer. `euicc show` re-serialises
 * a decoded value: every comment is gone and `myHeader ProfileElement ::=`
 * comes back `value1`. Reading text also means a file the reader rejects can
 * still be laid out, which is the file most in need of it.
 *
 * Belt and braces on top of the structure: the token list of the output is
 * compared against the input's, and on any difference the input is returned
 * untouched. With verbatim bytes the only way they can differ is a lexer bug,
 * which is exactly the thing left worth catching.
 */

#include "euicc.h"

#include <ctype.h>

/* ---- tokens -------------------------------------------------------------- */

typedef enum {
    TK_WORD,     /* an identifier, a number, a string, any other lexeme */
    TK_OPEN,     /* { or [ */
    TK_CLOSE,    /* } or ] */
    TK_COMMA,
    TK_LCOMMENT, /* -- ... (to the next -- or the end of the line) */
    TK_BCOMMENT, /* a bracketed comment, nesting */
} tok_kind_e;

typedef struct {
    size_t     off, len; /* the bytes, verbatim, in the input buffer */
    tok_kind_e kind;
    int        nl_before; /* newlines in the gap the author left */
    int        sp_before; /* whitespace bytes since the last of them */
    int        out_nl;    /* what the passes decide */
    int        out_sp;
    int        inlined;   /* part of an all-numeric brace group */
} tok_t;

typedef struct {
    tok_t *v;
    size_t count, cap;
} toks_t;

static tok_t *
tok_add(toks_t *ts) {
    if(ts->count == ts->cap) {
        size_t cap = ts->cap ? ts->cap * 2 : 256;
        tok_t *v = realloc(ts->v, cap * sizeof *v);
        if(!v) return NULL;
        ts->v = v;
        ts->cap = cap;
    }
    tok_t *t = &ts->v[ts->count++];
    memset(t, 0, sizeof *t);
    return t;
}

/*
 * Split the input into tokens. Every byte of the input is either inside a
 * token or whitespace between two, so writing the tokens back out cannot lose
 * content -- the loop consumes what it scanned, and only isspace() bytes are
 * stepped over.
 */
static int
lex(const char *s, size_t n, toks_t *ts) {
    size_t i = 0;
    int nl = 0, sp = 0;

    while(i < n) {
        char c = s[i];

        if(isspace((unsigned char)c)) {
            if(c == '\n') { nl++; sp = 0; }
            else if(c != '\r') sp++;
            i++;
            continue;
        }

        tok_t *t = tok_add(ts);
        if(!t) return -1;
        t->off = i;
        t->nl_before = nl;
        t->sp_before = sp;
        nl = sp = 0;

        if(c == '-' && i + 1 < n && s[i + 1] == '-') {
            /* X.680 12.6.3: to the next "--" or the end of the line. */
            size_t j = i + 2;
            while(j < n && s[j] != '\n') {
                if(s[j] == '-' && j + 1 < n && s[j + 1] == '-') { j += 2; break; }
                j++;
            }
            size_t e = j;
            while(e > i && isspace((unsigned char)s[e - 1])) e--;
            t->kind = TK_LCOMMENT;
            t->len = e - i;
            /* The trimmed tail is whitespace and goes back to the gap. */
            for(size_t k = e; k < j; k++)
                if(s[k] != '\r') sp++;
            i = j;
            continue;
        }
        if(c == '/' && i + 1 < n && s[i + 1] == '*') {
            /* X.680 12.6.4: the bracketed form nests. */
            size_t j = i + 2;
            int depth = 1;
            while(j < n && depth > 0) {
                if(j + 1 < n && s[j] == '/' && s[j + 1] == '*') { depth++; j += 2; }
                else if(j + 1 < n && s[j] == '*' && s[j + 1] == '/') { depth--; j += 2; }
                else j++;
            }
            t->kind = TK_BCOMMENT;
            t->len = j - i;
            i = j;
            continue;
        }
        if(c == '"' || c == '\'') {
            /* One token to the closing quote, however many lines that is.
               The bytes stay verbatim, so a literal the author or the writer
               wrapped keeps its own layout untouched. */
            size_t j = i + 1;
            while(j < n && s[j] != c) j++;
            if(j < n) j++;
            if(c == '\'' && j < n && strchr("HhBb", s[j])) j++;
            t->kind = TK_WORD;
            t->len = j - i;
            i = j;
            continue;
        }
        if(c == '{' || c == '[') { t->kind = TK_OPEN;  t->len = 1; i++; continue; }
        if(c == '}' || c == ']') { t->kind = TK_CLOSE; t->len = 1; i++; continue; }
        if(c == ',')             { t->kind = TK_COMMA; t->len = 1; i++; continue; }

        if(isalnum((unsigned char)c) || c == '-') {
            size_t j = i;
            while(j < n && (isalnum((unsigned char)s[j]) || s[j] == '-')) j++;
            t->kind = TK_WORD;
            t->len = j - i;
            i = j;
            continue;
        }
        t->kind = TK_WORD;
        t->len = 1;
        i++;
    }
    return 0;
}

/* ---- the passes ----------------------------------------------------------- */

static int
all_digits(const char *s, const tok_t *t) {
    for(size_t k = 0; k < t->len; k++)
        if(!isdigit((unsigned char)s[t->off + k])) return 0;
    return t->len > 0;
}

/*
 * An OBJECT IDENTIFIER is a brace list of arcs and the writer keeps it on one
 * line: `{ 2 23 143 1 2 1 }`. Numbers only is what tells such a group from a
 * value, and breaking them put ninety extra lines in a published profile. An
 * empty group is the same shape with nothing in it: `{ }`.
 */
static void
mark_inline_groups(const char *s, toks_t *ts) {
    for(size_t i = 0; i < ts->count; i++) {
        if(ts->v[i].kind != TK_OPEN) continue;
        size_t j = i + 1;
        while(j < ts->count && ts->v[j].kind == TK_WORD && all_digits(s, &ts->v[j]))
            j++;
        if(j < ts->count && ts->v[j].kind == TK_CLOSE) {
            for(size_t k = i; k <= j; k++) ts->v[k].inlined = 1;
            i = j;
        }
    }
}

/* At most one blank line survives from the author's gap. */
static int
kept(int nl) {
    return nl > 2 ? 2 : nl;
}

/*
 * Where the line breaks go. This pass exists because value notation separates
 * members with commas and not with newlines; it is the one thing hclwrite has
 * no counterpart for.
 */
static void
decide_newlines(toks_t *ts) {
    for(size_t i = 0; i < ts->count; i++) {
        tok_t *t = &ts->v[i];
        const tok_t *prev = i ? &ts->v[i - 1] : NULL;

        if(!prev) {
            t->out_nl = kept(t->nl_before);
            continue;
        }
        /* Inside an inline group nothing breaks. */
        if(t->inlined && prev->inlined && prev->kind != TK_CLOSE) {
            t->out_nl = 0;
            continue;
        }
        /* A member starts after the brace that opens its value... */
        if(prev->kind == TK_OPEN && !prev->inlined) {
            t->out_nl = t->nl_before > 1 ? 2 : 1;
            continue;
        }
        /* ...and a closing brace sits at the level of the thing it closes,
           on a line of its own. */
        if(t->kind == TK_CLOSE && !t->inlined) {
            t->out_nl = t->nl_before > 1 ? 2 : 1;
            continue;
        }
        /* One member to a line: the comma ends one. A comment right after
           the comma is about the member it follows and stays with it. */
        if(prev->kind == TK_COMMA) {
            if(t->kind == TK_LCOMMENT && t->nl_before == 0) t->out_nl = 0;
            else t->out_nl = t->nl_before > 1 ? 2 : 1;
            continue;
        }
        /* Nothing continues a line after a one-line comment. */
        if(prev->kind == TK_LCOMMENT) {
            t->out_nl = t->nl_before > 1 ? 2 : 1;
            continue;
        }
        t->out_nl = kept(t->nl_before);
    }
}

/*
 * Indentation for the tokens that start a line, and the author's own spacing
 * for the ones that do not. Interior spacing is the author's on purpose: it is
 * what lines a trailing comment up with its neighbours, and the writer's own
 * alignment has to come back out of this untouched.
 */
static void
decide_spaces(toks_t *ts, int unit) {
    int depth = 0;

    for(size_t i = 0; i < ts->count; i++) {
        tok_t *t = &ts->v[i];
        const tok_t *prev = i ? &ts->v[i - 1] : NULL;

        if(t->kind == TK_CLOSE && !t->inlined && depth > 0) depth--;

        if(t->out_nl > 0 || !prev) {
            t->out_sp = depth * unit;
        } else if(t->kind == TK_COMMA) {
            t->out_sp = 0;
        } else if(t->inlined && (prev->kind == TK_OPEN || t->kind == TK_CLOSE
                                 || prev->inlined)) {
            t->out_sp = 1;
        } else {
            t->out_sp = t->sp_before;
        }

        if(t->kind == TK_OPEN && !t->inlined) depth++;
    }
}

/* ---- writing ------------------------------------------------------------- */

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
buf_fill(buf_t *b, char c, int n) {
    while(n-- > 0)
        if(buf_put(b, &c, 1)) return -1;
    return 0;
}

/* Counts, then bytes, verbatim. This is the whole writer, which is the point:
   there is no place in it where content could change. */
static char *
emit(const char *s, const toks_t *ts) {
    buf_t b = {0};
    for(size_t i = 0; i < ts->count; i++) {
        const tok_t *t = &ts->v[i];
        if(buf_fill(&b, '\n', t->out_nl)
           || buf_fill(&b, ' ', t->out_sp)
           || buf_put(&b, s + t->off, t->len)) {
            free(b.p);
            return NULL;
        }
    }
    if(buf_put(&b, "\n", 1)) { free(b.p); return NULL; }
    return b.p ? b.p : strdup("\n");
}

/* ---- the guarantee --------------------------------------------------------- */

/*
 * The token list as one comparable string. X.680 12.11 and 12.12: the
 * whitespace inside an hstring or a bstring is not part of the value, so two
 * wrappings of one are the same token. A cstring is text and compares whole --
 * getting that distinction wrong is how the previous formatter's check went
 * blind at exactly the place it was needed.
 */
static int
token_string(const char *s, size_t n, buf_t *out) {
    toks_t ts = {0};
    if(lex(s, n, &ts)) { free(ts.v); return -1; }
    for(size_t i = 0; i < ts.count; i++) {
        const tok_t *t = &ts.v[i];
        int hstr = s[t->off] == '\'';
        for(size_t k = 0; k < t->len; k++) {
            char c = s[t->off + k];
            if(hstr && isspace((unsigned char)c)) continue;
            if(buf_put(out, &c, 1)) { free(ts.v); return -1; }
        }
        if(buf_put(out, "\x1f", 1)) { free(ts.v); return -1; }
    }
    free(ts.v);
    return 0;
}

/* ---- entry ----------------------------------------------------------------- */

char *
fmt_layout(const char *text, size_t len, const char *unit, char **why) {
    *why = NULL;
    int unit_len = unit ? (int)strlen(unit) : 4;

    toks_t ts = {0};
    if(lex(text, len, &ts)) { free(ts.v); *why = "out of memory"; return NULL; }

    mark_inline_groups(text, &ts);
    decide_newlines(&ts);
    decide_spaces(&ts, unit_len);

    char *out = emit(text, &ts);
    free(ts.v);
    if(!out) { *why = "out of memory"; return NULL; }

    buf_t a = {0}, b = {0};
    if(token_string(text, len, &a) || token_string(out, strlen(out), &b)) {
        free(a.p); free(b.p); free(out);
        *why = "out of memory";
        return NULL;
    }
    int same = (a.p || b.p) ? (a.p && b.p && !strcmp(a.p, b.p)) : 1;
    free(a.p);
    free(b.p);
    if(!same) {
        free(out);
        *why = "the layout would have changed the file, so it was left alone";
        return NULL;
    }
    return out;
}
