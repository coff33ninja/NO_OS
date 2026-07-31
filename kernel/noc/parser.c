#include "noc.h"
#include "string.h"

#define UNOP_NEG   100
#define UNOP_NOT   101
#define UNOP_BNOT  102
#define INC_PRE    103
#define INC_POST   104

typedef struct {
    tok  *toks;
    usize n, i;
    noc_arena *a;
    const char **err;
} parser;

static void perr(parser *p, const char *msg)
{
    if (p->err && !*p->err)
        *p->err = msg;
}

static tok *cur(parser *p) { return &p->toks[p->i]; }

static bool check(parser *p, tok_t t)
{
    return p->toks[p->i].t == t;
}

static bool accept(parser *p, tok_t t)
{
    if (p->toks[p->i].t == t) {
        p->i++;
        return true;
    }
    return false;
}

static bool expect(parser *p, tok_t t, const char *what)
{
    if (p->toks[p->i].t == t) {
        p->i++;
        return true;
    }
    if (p->err && !*p->err)
        *p->err = what;
    return false;
}

static ast *new_ast(parser *p, ast_t kind)
{
    ast *n = noc_arena_alloc(p->a, sizeof(ast));
    if (!n) {
        perr(p, "out of memory (AST)");
        return NULL;
    }
    memset(n, 0, sizeof(ast));
    n->kind = kind;
    n->slot = -1;
    return n;
}

static u32 type_of(parser *p, tok_t t)
{
    (void)p;
    switch (t) {
    case T_TYPE_U0:   return NTYPE_VOID;
    case T_TYPE_I64:  return NTYPE_I64;
    case T_TYPE_U64:  return NTYPE_U64;
    case T_TYPE_BOOL: return NTYPE_BOOL;
    case T_TYPE_STR:  return NTYPE_STR;
    case T_TYPE_U8:   return NTYPE_U8;
    case T_TYPE_I8:   return NTYPE_I8;
    case T_TYPE_U16:  return NTYPE_U16;
    case T_TYPE_I16:  return NTYPE_I16;
    case T_TYPE_U32:  return NTYPE_U32;
    case T_TYPE_I32:  return NTYPE_I32;
    case T_TYPE_F64:  return NTYPE_F64;
    default:          return NTYPE_I64;
    }
}

static bool is_type_tok(tok_t t)
{
    switch (t) {
    case T_TYPE_U0: case T_TYPE_I64: case T_TYPE_U64: case T_TYPE_BOOL:
    case T_TYPE_STR: case T_TYPE_U8: case T_TYPE_U16: case T_TYPE_U32:
    case T_TYPE_I8: case T_TYPE_I16: case T_TYPE_I32: case T_TYPE_F64:
        return true;
    default:
        return false;
    }
}

static ast *parse_expr(parser *p);

/* ---- precedence climbing ---- */

static ast *parse_primary(parser *p)
{
    tok *t = cur(p);
    switch (t->t) {
    case T_NUM: {
        ast *n = new_ast(p, A_LIT);
        if (n) n->val = t->num;
        p->i++;
        return n;
    }
    case T_STR: {
        ast *n = new_ast(p, A_STRLIT);
        if (n) n->name = t->s;
        p->i++;
        return n;
    }
    case T_TRUE: {
        ast *n = new_ast(p, A_LIT);
        if (n) n->val = 1;
        p->i++;
        return n;
    }
    case T_FALSE: {
        ast *n = new_ast(p, A_LIT);
        if (n) n->val = 0;
        p->i++;
        return n;
    }
    case T_IDENT: {
        ast *n = new_ast(p, A_VAR);
        if (n) n->name = t->s;
        p->i++;
        return n;
    }
    case T_LP: {
        p->i++;
        ast *e = parse_expr(p);
        if (!expect(p, T_RP, "expected ')'"))
            return NULL;
        return e;
    }
    default:
        perr(p, "expected expression");
        return NULL;
    }
}

static ast *parse_postfix(parser *p)
{
    ast *e = parse_primary(p);
    if (!e)
        return NULL;

    for (;;) {
        if (check(p, T_LP)) {
            p->i++;
            ast *c = new_ast(p, A_CALL);
            if (!c)
                return NULL;
            if (e->kind != A_VAR || !e->name) {
                perr(p, "call target must be a function name");
                return NULL;
            }
            c->name = e->name;
            if (!check(p, T_RP)) {
                for (;;) {
                    ast *arg = parse_expr(p);
                    if (!arg)
                        return NULL;
                    if (c->nargs >= NOC_MAX_ARGS) {
                        perr(p, "too many arguments");
                        return NULL;
                    }
                    if (!c->args)
                        c->args = noc_arena_alloc(p->a, sizeof(ast *) * NOC_MAX_ARGS);
                    c->args[c->nargs++] = arg;
                    if (!accept(p, T_COMMA))
                        break;
                }
            }
            if (!expect(p, T_RP, "expected ')'"))
                return NULL;
            e = c;
        } else if (check(p, T_PLUSPLUS) || check(p, T_MINUSMINUS)) {
            bool plus = cur(p)->t == T_PLUSPLUS;
            p->i++;
            if (e->kind != A_VAR) {
                perr(p, "++/-- needs a variable");
                return NULL;
            }
            ast *n = new_ast(p, A_POSTINC);
            if (!n)
                return NULL;
            n->name = e->name;
            n->binop = INC_POST;
            n->val = plus ? 1 : 0;
            n->a = e;
            e = n;
        } else {
            break;
        }
    }
    return e;
}

static ast *parse_unary(parser *p)
{
    tok_t t = cur(p)->t;
    if (t == T_BANG || t == T_MINUS || t == T_TILDE ||
        t == T_PLUSPLUS || t == T_MINUSMINUS) {
        p->i++;
        ast *n = new_ast(p, A_UN);
        if (!n)
            return NULL;
        n->a = parse_unary(p);
        if (!n->a)
            return NULL;
        switch (t) {
        case T_BANG: n->binop = UNOP_NOT; break;
        case T_MINUS: n->binop = UNOP_NEG; break;
        case T_TILDE: n->binop = UNOP_BNOT; break;
        case T_PLUSPLUS: n->binop = INC_PRE; n->val = 1; break;
        default: n->binop = INC_PRE; n->val = 0; break;
        }
        if ((n->binop == INC_PRE) && n->a->kind != A_VAR) {
            perr(p, "++/-- needs a variable");
            return NULL;
        }
        return n;
    }
    return parse_postfix(p);
}

static ast *parse_binop(parser *p, ast *(*higher)(parser *),
                        const tok_t *ops, const u32 *codes, usize nops)
{
    ast *lhs = higher(p);
    if (!lhs)
        return NULL;
    for (;;) {
        tok_t t = cur(p)->t;
        u32 code = 0;
        usize k;
        for (k = 0; k < nops; k++) {
            if (ops[k] == t) {
                code = codes[k];
                break;
            }
        }
        if (k == nops)
            break;
        p->i++;
        ast *rhs = higher(p);
        if (!rhs)
            return NULL;
        ast *n = new_ast(p, A_BIN);
        if (!n)
            return NULL;
        n->binop = code;
        n->a = lhs;
        n->b = rhs;
        lhs = n;
    }
    return lhs;
}

static ast *parse_multiplicative(parser *p)
{
    static const tok_t ops[] = { T_STAR, T_SLASH, T_PCT };
    static const u32 codes[] = { BIN_MUL, BIN_DIV, BIN_MOD };
    return parse_binop(p, parse_unary, ops, codes, 3);
}

static ast *parse_additive(parser *p)
{
    static const tok_t ops[] = { T_PLUS, T_MINUS };
    static const u32 codes[] = { BIN_ADD, BIN_SUB };
    return parse_binop(p, parse_multiplicative, ops, codes, 2);
}

static ast *parse_shift(parser *p)
{
    static const tok_t ops[] = { T_SHL, T_SHR };
    static const u32 codes[] = { BIN_SHL, BIN_SHR };
    return parse_binop(p, parse_additive, ops, codes, 2);
}

static ast *parse_relational(parser *p)
{
    static const tok_t ops[] = { T_LT, T_LE, T_GT, T_GE };
    static const u32 codes[] = { BIN_LT, BIN_LE, BIN_GT, BIN_GE };
    return parse_binop(p, parse_shift, ops, codes, 4);
}

static ast *parse_equality(parser *p)
{
    static const tok_t ops[] = { T_EQEQ, T_NEQ };
    static const u32 codes[] = { BIN_EQ, BIN_NE };
    return parse_binop(p, parse_relational, ops, codes, 2);
}

static ast *parse_band(parser *p)
{
    static const tok_t ops[] = { T_AMP };
    static const u32 codes[] = { BIN_BAND };
    return parse_binop(p, parse_equality, ops, codes, 1);
}

static ast *parse_bxor(parser *p)
{
    static const tok_t ops[] = { T_CARET };
    static const u32 codes[] = { BIN_BXOR };
    return parse_binop(p, parse_band, ops, codes, 1);
}

static ast *parse_bor(parser *p)
{
    static const tok_t ops[] = { T_PIPE };
    static const u32 codes[] = { BIN_BOR };
    return parse_binop(p, parse_bxor, ops, codes, 1);
}

static ast *parse_andand(parser *p)
{
    static const tok_t ops[] = { T_ANDAND };
    static const u32 codes[] = { BIN_ANDAND };
    return parse_binop(p, parse_bor, ops, codes, 1);
}

static ast *parse_oror(parser *p)
{
    static const tok_t ops[] = { T_OROR };
    static const u32 codes[] = { BIN_OROR };
    return parse_binop(p, parse_andand, ops, codes, 1);
}

static ast *parse_assign(parser *p)
{
    ast *lhs = parse_oror(p);
    if (!lhs)
        return NULL;

    static const tok_t ops[] = { T_EQ, T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ };
    static const u32 codes[] = { BIN_ASSIGN, BIN_ADDASSIGN, BIN_SUBASSIGN,
                                 BIN_MULASSIGN, BIN_DIVASSIGN };
    tok_t t = cur(p)->t;
    u32 code = 0;
    usize k;
    for (k = 0; k < 5; k++) {
        if (ops[k] == t) {
            code = codes[k];
            break;
        }
    }
    if (k == 5)
        return lhs;
    if (lhs->kind != A_VAR) {
        perr(p, "assignment target must be a variable");
        return NULL;
    }
    p->i++;
    ast *rhs = parse_assign(p);
    if (!rhs)
        return NULL;
    ast *n = new_ast(p, A_ASSIGN);
    if (!n)
        return NULL;
    n->binop = code;
    n->name = lhs->name;
    n->a = rhs;
    return n;
}

static ast *parse_expr(parser *p)
{
    return parse_assign(p);
}

/* ---- statements ---- */

static ast *parse_stmt(parser *p);

static ast *parse_block(parser *p)
{
    ast *b = new_ast(p, A_BLOCK);
    if (!b)
        return NULL;
    if (!expect(p, T_LB, "expected '{'"))
        return NULL;
    while (!check(p, T_RB)) {
        if (check(p, T_EOF)) {
            perr(p, "unterminated block");
            return NULL;
        }
        ast *s = parse_stmt(p);
        if (!s)
            return NULL;
        if (!b->args) {
            b->args = noc_arena_alloc(p->a, sizeof(ast *) * NOC_MAX_NODES);
            if (!b->args) {
                perr(p, "out of memory (block)");
                return NULL;
            }
        }
        b->args[b->nargs++] = s;
    }
    p->i++; /* '}' */
    return b;
}

static ast *parse_params(parser *p, ast *fn)
{
    if (!expect(p, T_LP, "expected '('"))
        return NULL;
    if (accept(p, T_RP))
        return fn;

    for (;;) {
        if (!is_type_tok(cur(p)->t)) {
            perr(p, "expected parameter type");
            return NULL;
        }
        u32 pt = type_of(p, cur(p)->t);
        p->i++;
        if (!check(p, T_IDENT)) {
            perr(p, "expected parameter name");
            return NULL;
        }
        ast *prm = new_ast(p, A_VARDECL);
        if (!prm)
            return NULL;
        prm->type = pt;
        prm->name = cur(p)->s;
        p->i++;
        if (accept(p, T_EQ)) {
            if (check(p, T_NUM)) {
                prm->a = new_ast(p, A_LIT);
                if (prm->a)
                    prm->a->val = cur(p)->num;
                p->i++;
            } else if (check(p, T_TRUE) || check(p, T_FALSE)) {
                prm->a = new_ast(p, A_LIT);
                if (prm->a)
                    prm->a->val = (cur(p)->t == T_TRUE) ? 1 : 0;
                p->i++;
            } else {
                perr(p, "default arg must be a literal");
                return NULL;
            }
        }
        if (!fn->args) {
            fn->args = noc_arena_alloc(p->a, sizeof(ast *) * NOC_MAX_ARGS);
            if (!fn->args) {
                perr(p, "out of memory (params)");
                return NULL;
            }
        }
        fn->args[fn->nargs++] = prm;
        if (fn->nargs >= NOC_MAX_ARGS) {
            perr(p, "too many parameters");
            return NULL;
        }
        if (!accept(p, T_COMMA))
            break;
    }
    if (!expect(p, T_RP, "expected ')'"))
        return NULL;
    return fn;
}

static ast *parse_stmt(parser *p)
{
    tok_t t = cur(p)->t;

    if (is_type_tok(t)) {
        u32 vt = type_of(p, t);
        p->i++;
        if (!check(p, T_IDENT)) {
            perr(p, "expected name after type");
            return NULL;
        }
        char *name = cur(p)->s;
        p->i++;

        if (check(p, T_LP)) {
            /* function declaration */
            ast *fn = new_ast(p, A_FUNCDECL);
            if (!fn)
                return NULL;
            fn->type = vt;
            fn->name = name;
            if (!parse_params(p, fn))
                return NULL;
            fn->body = parse_block(p);
            if (!fn->body)
                return NULL;
            return fn;
        }

        /* variable declaration (re-parse ident already consumed) */
        ast *n = new_ast(p, A_VARDECL);
        if (!n)
            return NULL;
        n->type = vt;
        n->name = name;
        if (accept(p, T_EQ)) {
            n->a = parse_expr(p);
            if (!n->a)
                return NULL;
        }
        if (!expect(p, T_SEMI, "expected ';'"))
            return NULL;
        return n;
    }

    switch (t) {
    case T_LB:
        return parse_block(p);

    case T_IF: {
        p->i++;
        ast *n = new_ast(p, A_IF);
        if (!n)
            return NULL;
        if (!expect(p, T_LP, "expected '('"))
            return NULL;
        n->a = parse_expr(p);
        if (!n->a)
            return NULL;
        if (!expect(p, T_RP, "expected ')'"))
            return NULL;
        n->b = parse_stmt(p);
        if (!n->b)
            return NULL;
        if (accept(p, T_ELSE)) {
            n->c = parse_stmt(p);
            if (!n->c)
                return NULL;
        }
        return n;
    }

    case T_WHILE: {
        p->i++;
        ast *n = new_ast(p, A_WHILE);
        if (!n)
            return NULL;
        if (!expect(p, T_LP, "expected '('"))
            return NULL;
        n->a = parse_expr(p);
        if (!n->a)
            return NULL;
        if (!expect(p, T_RP, "expected ')'"))
            return NULL;
        n->b = parse_stmt(p);
        if (!n->b)
            return NULL;
        return n;
    }

    case T_FOR: {
        p->i++;
        ast *n = new_ast(p, A_FOR);
        if (!n)
            return NULL;
        if (!expect(p, T_LP, "expected '('"))
            return NULL;
        /* init */
        if (check(p, T_SEMI)) {
            p->i++;
        } else if (is_type_tok(cur(p)->t)) {
            u32 it = type_of(p, cur(p)->t);
            p->i++;
            if (!check(p, T_IDENT)) {
                perr(p, "expected variable name");
                return NULL;
            }
            ast *vd = new_ast(p, A_VARDECL);
            if (!vd)
                return NULL;
            vd->type = it;
            vd->name = cur(p)->s;
            p->i++;
            if (accept(p, T_EQ)) {
                vd->a = parse_expr(p);
                if (!vd->a)
                    return NULL;
            }
            n->a = vd;
            if (!expect(p, T_SEMI, "expected ';'"))
                return NULL;
        } else {
            n->a = parse_expr(p);
            if (!n->a)
                return NULL;
            if (!expect(p, T_SEMI, "expected ';'"))
                return NULL;
        }
        /* cond */
        if (!check(p, T_SEMI)) {
            n->b = parse_expr(p);
            if (!n->b)
                return NULL;
        }
        if (!expect(p, T_SEMI, "expected ';'"))
            return NULL;
        /* post */
        if (!check(p, T_RP)) {
            n->c = parse_expr(p);
            if (!n->c)
                return NULL;
        }
        if (!expect(p, T_RP, "expected ')'"))
            return NULL;
        n->body = parse_stmt(p);
        if (!n->body)
            return NULL;
        return n;
    }

    case T_RETURN: {
        p->i++;
        ast *n = new_ast(p, A_RETURN);
        if (!n)
            return NULL;
        if (!check(p, T_SEMI)) {
            n->a = parse_expr(p);
            if (!n->a)
                return NULL;
        }
        if (!expect(p, T_SEMI, "expected ';'"))
            return NULL;
        return n;
    }

    case T_EOF:
        perr(p, "unexpected end of input");
        return NULL;

    default: {
        ast *n = new_ast(p, A_STMTEXPR);
        if (!n)
            return NULL;
        n->a = parse_expr(p);
        if (!n->a)
            return NULL;
        if (!expect(p, T_SEMI, "expected ';'"))
            return NULL;
        return n;
    }
    }
}

bool noc_parse(tok *toks, usize ntoks, noc_arena *a, ast **out,
               usize *nstmt, const char **err)
{
    parser p;
    memset(&p, 0, sizeof(p));
    p.toks = toks;
    p.n = ntoks;
    p.a = a;
    p.err = err;

    ast *arr = noc_arena_alloc(a, sizeof(ast) * NOC_MAX_NODES);
    if (!arr) {
        if (err) *err = "out of memory (statement list)";
        return false;
    }

    usize count = 0;
    while (!check(&p, T_EOF)) {
        ast *s = parse_stmt(&p);
        if (!s)
            return false;
        arr[count++] = *s;
        if (count >= NOC_MAX_NODES) {
            if (err) *err = "too many statements";
            return false;
        }
    }

    *out = arr;
    *nstmt = count;
    return true;
}
