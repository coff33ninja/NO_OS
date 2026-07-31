#include "noc.h"
#include "heap.h"
#include "string.h"
#include "printk.h"

void *noc_arena_alloc(noc_arena *a, usize n)
{
    n = (n + 15) & ~(usize)15;
    if (!a->base || a->used + n > a->cap)
        return NULL;
    void *p = a->base + a->used;
    a->used += n;
    return p;
}

static bool is_ident_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_ident_char(char c)
{
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static tok_t keyword_type(const char *s, usize len)
{
    struct { const char *kw; tok_t t; } map[] = {
        { "U0", T_TYPE_U0 }, { "I64", T_TYPE_I64 }, { "U64", T_TYPE_U64 },
        { "Bool", T_TYPE_BOOL }, { "Str", T_TYPE_STR },
        { "U8", T_TYPE_U8 }, { "U16", T_TYPE_U16 }, { "U32", T_TYPE_U32 },
        { "I8", T_TYPE_I8 }, { "I16", T_TYPE_I16 }, { "I32", T_TYPE_I32 },
        { "F64", T_TYPE_F64 },
        { "if", T_IF }, { "else", T_ELSE }, { "while", T_WHILE },
        { "for", T_FOR }, { "return", T_RETURN },
        { "true", T_TRUE }, { "false", T_FALSE },
    };
    for (usize i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strlen(map[i].kw) == len && memcmp(map[i].kw, s, len) == 0)
            return map[i].t;
    }
    return T_IDENT;
}

static bool lex_escape(const char **p, char *out)
{
    char c = *(*p)++;
    switch (c) {
    case 'n': *out = '\n'; return true;
    case 't': *out = '\t'; return true;
    case 'r': *out = '\r'; return true;
    case '\\': *out = '\\'; return true;
    case '"': *out = '"'; return true;
    case '\'': *out = '\''; return true;
    default: *out = c; return true;
    }
}

bool noc_lex(const char *src, noc_arena *a, tok *toks, usize maxtoks,
             usize *ntoks, const char **err)
{
    const char *p = src;
    usize n = 0;

    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;

        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n')
                p++;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/'))
                p++;
            if (*p)
                p += 2;
            continue;
        }

        if (n >= maxtoks) {
            if (err) *err = "too many tokens";
            return false;
        }

        char c = *p;
        if (!c) {
            toks[n].t = T_EOF;
            toks[n].s = NULL;
            n++;
            *ntoks = n;
            return true;
        }

        if ((c >= '0' && c <= '9') || (c == '0' && (p[1] == 'x' || p[1] == 'X'))) {
            u64 v = 0;
            bool hex = (c == '0' && (p[1] == 'x' || p[1] == 'X'));
            if (hex)
                p += 2;
            while (*p && ((hex && ((*p >= '0' && *p <= '9') ||
                                   (*p >= 'a' && *p <= 'f') ||
                                   (*p >= 'A' && *p <= 'F'))) ||
                          (!hex && (*p >= '0' && *p <= '9')))) {
                char d = *p;
                u64 dv;
                if (d >= '0' && d <= '9')
                    dv = (u64)(d - '0');
                else if (d >= 'a' && d <= 'f')
                    dv = (u64)(d - 'a' + 10);
                else
                    dv = (u64)(d - 'A' + 10);
                v = v * (hex ? 16 : 10) + dv;
                p++;
            }
            toks[n].t = T_NUM;
            toks[n].num = v;
            toks[n].s = NULL;
            n++;
            continue;
        }

        if (is_ident_start(c)) {
            const char *start = p;
            while (is_ident_char(*p))
                p++;
            usize len = (usize)(p - start);
            char *s = noc_arena_alloc(a, len + 1);
            if (!s) {
                if (err) *err = "out of memory (identifier)";
                return false;
            }
            memcpy(s, start, len);
            s[len] = '\0';
            toks[n].t = keyword_type(s, len);
            toks[n].s = s;
            toks[n].num = 0;
            n++;
            continue;
        }

        if (c == '"') {
            p++;
            /* estimate max length, decode inline into arena */
            char *buf = noc_arena_alloc(a, 256);
            usize len = 0;
            bool ok = true;
            while (*p && *p != '"') {
                if (*p == '\\') {
                    char e = 0;
                    p++;
                    lex_escape(&p, &e);
                    buf[len++] = e;
                } else {
                    buf[len++] = *p++;
                }
                if (len >= 256) {
                    ok = false;
                    break;
                }
            }
            if (!ok) {
                if (err) *err = "string too long";
                return false;
            }
            if (*p == '"')
                p++;
            buf[len] = '\0';
            toks[n].t = T_STR;
            toks[n].s = buf;
            toks[n].num = 0;
            n++;
            continue;
        }

        /* operators */
        struct { const char *op; tok_t t; } ops[] = {
            { "==", T_EQEQ }, { "!=", T_NEQ }, { "<=", T_LE }, { ">=", T_GE },
            { "&&", T_ANDAND }, { "||", T_OROR }, { "<<", T_SHL }, { ">>", T_SHR },
            { "++", T_PLUSPLUS }, { "--", T_MINUSMINUS },
            { "+=", T_PLUSEQ }, { "-=", T_MINUSEQ },
            { "*=", T_STAREQ }, { "/=", T_SLASHEQ },
            { "(", T_LP }, { ")", T_RP }, { "{", T_LB }, { "}", T_RB },
            { ";", T_SEMI }, { ",", T_COMMA }, { "+", T_PLUS }, { "-", T_MINUS },
            { "*", T_STAR }, { "/", T_SLASH }, { "%", T_PCT }, { "=", T_EQ },
            { "<", T_LT }, { ">", T_GT }, { "!", T_BANG }, { "&", T_AMP },
            { "|", T_PIPE }, { "^", T_CARET }, { "~", T_TILDE },
        };
        for (usize i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
            usize len = strlen(ops[i].op);
            if (memcmp(p, ops[i].op, len) == 0) {
                p += len;
                toks[n].t = ops[i].t;
                toks[n].s = NULL;
                toks[n].num = 0;
                n++;
                goto next;
            }
        }

        if (err) {
            char *msg = noc_arena_alloc(a, 48);
            if (msg) {
                char bad[2] = { c, '\0' };
                sprintk(msg, 48, "unexpected character '%s'", bad);
                *err = msg;
            } else {
                *err = "unexpected character";
            }
        }
        return false;
    next:
        ;
    }
}
