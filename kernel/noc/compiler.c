#include "noc.h"
#include "noc_os.h"
#include "string.h"
#include "format.h"

/* Unary operator codes (must match parser.c) */
#define UNOP_NEG   100
#define UNOP_NOT   101
#define UNOP_BNOT  102
#define INC_PRE    103
#define INC_POST   104

typedef struct {
    char *name;
    u32   type;
} sym;

typedef struct {
    sym            syms[NOC_MAX_LOCALS];
    usize          nsyms;
    struct {
        u8  *data;
        usize len;
        usize cap;
    } code;
    noc_fn        *fn;        /* function being compiled into */
    noc_arena     *a;         /* transient arena (error strings) */
    const char   **err;
    bool           in_fn;
} ccomp;

static void cerr(ccomp *c, const char *msg)
{
    if (c->err && !*c->err)
        *c->err = msg;
}

static void cerr_name(ccomp *c, const char *fmt, const char *name)
{
    if (!c->err || *c->err)
        return;
    char *buf = c->a ? noc_arena_alloc(c->a, 128) : NULL;
    if (buf) {
        sprintk(buf, 128, fmt, name);
        *c->err = buf;
    } else {
        *c->err = fmt;
    }
}

static bool buf_ensure(ccomp *c, usize extra)
{
    if (c->code.len + extra <= c->code.cap)
        return true;
    usize ncap = c->code.cap ? c->code.cap * 2 : 1024;
    while (ncap < c->code.len + extra)
        ncap *= 2;
    u8 *nd = noc_os_alloc(ncap);
    if (!nd) {
        cerr(c, "out of memory (code buffer)");
        return false;
    }
    if (c->code.data) {
        memcpy(nd, c->code.data, c->code.len);
        noc_os_free(c->code.data);
    }
    c->code.data = nd;
    c->code.cap = ncap;
    return true;
}

static bool emit1(ccomp *c, u8 op)
{
    if (!buf_ensure(c, 1))
        return false;
    c->code.data[c->code.len++] = op;
    return true;
}

static bool emit1v(ccomp *c, u8 op, u64 v)
{
    if (!buf_ensure(c, 9))
        return false;
    c->code.data[c->code.len++] = op;
    memcpy(c->code.data + c->code.len, &v, 8);
    c->code.len += 8;
    return true;
}

static bool emit1u32(ccomp *c, u8 op, u32 v)
{
    if (!buf_ensure(c, 5))
        return false;
    c->code.data[c->code.len++] = op;
    memcpy(c->code.data + c->code.len, &v, 4);
    c->code.len += 4;
    return true;
}

/* Emit a jump op with a placeholder target; returns the offset of the
   operand so it can be patched once the target PC is known. */
static usize emit_jump(ccomp *c, u8 op)
{
    if (!buf_ensure(c, 5))
        return (usize)-1;
    usize at = c->code.len;
    c->code.data[c->code.len++] = op;
    u32 zero = 0;
    memcpy(c->code.data + c->code.len, &zero, 4);
    c->code.len += 4;
    return at;
}

static void patch_jump(ccomp *c, usize at, usize target)
{
    u32 t = (u32)target;
    memcpy(c->code.data + at + 1, &t, 4);
}

static bool emit_jump_at(ccomp *c, u8 op, usize target)
{
    usize at = emit_jump(c, op);
    if (at == (usize)-1)
        return false;
    patch_jump(c, at, target);
    return true;
}

static usize code_pc(ccomp *c)
{
    return c->code.len;
}

/* ---- string constants ---- */

static i32 add_string(ccomp *c, const char *s)
{
    noc_fn *fn = c->fn;
    for (usize i = 0; i < fn->nstrings; i++) {
        if (strcmp(fn->strings[i], s) == 0)
            return (i32)i;
    }
    if (fn->nstrings >= NOC_MAX_STRINGS) {
        cerr(c, "too many string constants");
        return -1;
    }
    usize len = strlen(s);
    char *copy = noc_os_alloc(len + 1);
    if (!copy) {
        cerr(c, "out of memory (string constant)");
        return -1;
    }
    memcpy(copy, s, len + 1);
    fn->strings[fn->nstrings++] = copy;
    return (i32)(fn->nstrings - 1);
}

/* ---- locals ---- */

static i32 resolve_var(ccomp *c, const char *name)
{
    for (isize i = (isize)c->nsyms - 1; i >= 0; i--) {
        if (strcmp(c->syms[i].name, name) == 0)
            return (i32)i;
    }
    return -1;
}

static i32 add_var(ccomp *c, const char *name, u32 type)
{
    if (resolve_var(c, name) >= 0) {
        cerr_name(c, "variable '%s' already declared", name);
        return -1;
    }
    if (c->nsyms >= NOC_MAX_LOCALS) {
        cerr(c, "too many local variables");
        return -1;
    }
    c->syms[c->nsyms].name = (char *)name;
    c->syms[c->nsyms].type = type;
    return (i32)c->nsyms++;
}

static u32 expr_type(ccomp *c, const ast *n)
{
    switch (n->kind) {
    case A_LIT:
        return NTYPE_I64;
    case A_STRLIT:
        return NTYPE_STR;
    case A_VAR: {
        i32 s = resolve_var(c, n->name);
        return s >= 0 ? c->syms[s].type : NTYPE_I64;
    }
    case A_CALL: {
        noc_fn *f = noc_lookup(n->name);
        return f ? f->ret_type : NTYPE_I64;
    }
    default:
        return NTYPE_I64;
    }
}

/* ---- expressions ---- */

static bool compile_expr(ccomp *c, const ast *n);

static bool compile_call(ccomp *c, const ast *n)
{
    noc_fn *callee = noc_lookup(n->name);
    if (!callee) {
        cerr_name(c, "unknown function '%s'", n->name);
        return false;
    }

    u32 callcount;
    if (callee->variadic) {
        if (n->nargs < callee->nargs) {
            cerr_name(c, "too few arguments for '%s'", n->name);
            return false;
        }
        callcount = n->nargs;
    } else {
        if (n->nargs > callee->nargs) {
            cerr_name(c, "too many arguments for '%s'", n->name);
            return false;
        }
        for (u32 k = n->nargs; k < callee->nargs; k++) {
            if (!callee->has_def[k]) {
                cerr_name(c, "too few arguments for '%s'", n->name);
                return false;
            }
        }
        callcount = callee->nargs;
    }

    for (u32 i = 0; i < n->nargs; i++) {
        if (!compile_expr(c, n->args[i]))
            return false;
    }
    for (u32 k = n->nargs; k < callcount; k++) {
        if (!emit1v(c, OP_PUSH, callee->defaults[k]))
            return false;
    }

    i32 si = add_string(c, n->name);
    if (si < 0)
        return false;
    if (!buf_ensure(c, 9))
        return false;
    c->code.data[c->code.len++] = OP_CALL;
    u32 sidx = (u32)si;
    memcpy(c->code.data + c->code.len, &sidx, 4);
    c->code.len += 4;
    memcpy(c->code.data + c->code.len, &callcount, 4);
    c->code.len += 4;
    return true;
}

static bool compile_bin(ccomp *c, const ast *n)
{
    if (n->binop == BIN_ANDAND || n->binop == BIN_OROR) {
        bool is_and = (n->binop == BIN_ANDAND);
        if (!compile_expr(c, n->a))
            return false;
        usize j1 = emit_jump(c, is_and ? OP_JMPF : OP_JMPT);
        if (j1 == (usize)-1)
            return false;
        if (!compile_expr(c, n->b))
            return false;
        usize j2 = emit_jump(c, is_and ? OP_JMPF : OP_JMPT);
        if (j2 == (usize)-1)
            return false;
        usize l_false_or_true = code_pc(c);
        if (!emit1v(c, OP_PUSH, is_and ? 0 : 1))
            return false;
        usize j3 = emit_jump(c, OP_JMP);
        if (j3 == (usize)-1)
            return false;
        usize l_done = code_pc(c);
        if (!emit1v(c, OP_PUSH, is_and ? 1 : 0))
            return false;
        patch_jump(c, j1, l_false_or_true);
        patch_jump(c, j2, l_false_or_true);
        patch_jump(c, j3, l_done);
        return true;
    }

    if (!compile_expr(c, n->a))
        return false;
    if (!compile_expr(c, n->b))
        return false;

    u8 op;
    switch (n->binop) {
    case BIN_ADD:  op = OP_ADD;  break;
    case BIN_SUB:  op = OP_SUB;  break;
    case BIN_MUL:  op = OP_MUL;  break;
    case BIN_DIV:  op = OP_DIV;  break;
    case BIN_MOD:  op = OP_MOD;  break;
    case BIN_EQ:   op = OP_EQ;   break;
    case BIN_NE:   op = OP_NE;   break;
    case BIN_LT:   op = OP_LT;   break;
    case BIN_LE:   op = OP_LE;   break;
    case BIN_GT:   op = OP_GT;   break;
    case BIN_GE:   op = OP_GE;   break;
    case BIN_BAND: op = OP_BAND; break;
    case BIN_BOR:  op = OP_BOR;  break;
    case BIN_BXOR: op = OP_BXOR; break;
    case BIN_SHL:  op = OP_SHL;  break;
    case BIN_SHR:  op = OP_SHR;  break;
    default:
        cerr(c, "bad binary operator");
        return false;
    }
    return emit1(c, op);
}

static bool compile_assign(ccomp *c, const ast *n)
{
    i32 s = resolve_var(c, n->name);
    if (s < 0) {
        cerr_name(c, "undeclared variable '%s'", n->name);
        return false;
    }

    if (n->binop == BIN_ASSIGN) {
        if (!compile_expr(c, n->a))
            return false;
        if (!emit1(c, OP_DUP))
            return false;
        return emit1u32(c, OP_STORE, (u32)s);
    }

    u8 op;
    switch (n->binop) {
    case BIN_ADDASSIGN: op = OP_ADD; break;
    case BIN_SUBASSIGN: op = OP_SUB; break;
    case BIN_MULASSIGN: op = OP_MUL; break;
    case BIN_DIVASSIGN: op = OP_DIV; break;
    default:
        cerr(c, "bad compound assignment");
        return false;
    }
    if (!emit1u32(c, OP_LOAD, (u32)s))
        return false;
    if (!compile_expr(c, n->a))
        return false;
    if (!emit1(c, op))
        return false;
    if (!emit1(c, OP_DUP))
        return false;
    return emit1u32(c, OP_STORE, (u32)s);
}

static bool compile_expr(ccomp *c, const ast *n)
{
    switch (n->kind) {
    case A_LIT:
        return emit1v(c, OP_PUSH, n->val);

    case A_STRLIT: {
        i32 si = add_string(c, n->name);
        if (si < 0)
            return false;
        return emit1u32(c, OP_PUSHSTR, (u32)si);
    }

    case A_VAR: {
        i32 s = resolve_var(c, n->name);
        if (s < 0) {
            cerr_name(c, "undeclared variable '%s'", n->name);
            return false;
        }
        return emit1u32(c, OP_LOAD, (u32)s);
    }

    case A_CALL:
        return compile_call(c, n);

    case A_BIN:
        return compile_bin(c, n);

    case A_UN: {
        if (!compile_expr(c, n->a))
            return false;
        switch (n->binop) {
        case UNOP_NEG:
            return emit1(c, OP_NEG);
        case UNOP_NOT:
            return emit1(c, OP_NOT);
        case UNOP_BNOT:
            return emit1(c, OP_BNOT);
        case INC_PRE: {
            if (n->a->kind != A_VAR) {
                cerr(c, "++/-- needs a variable");
                return false;
            }
            i32 s = resolve_var(c, n->a->name);
            if (s < 0) {
                cerr_name(c, "undeclared variable '%s'", n->a->name);
                return false;
            }
            if (!emit1u32(c, OP_LOAD, (u32)s))
                return false;
            if (!emit1v(c, OP_PUSH, n->val ? 1 : (u64)-1))
                return false;
            if (!emit1(c, OP_ADD))
                return false;
            if (!emit1(c, OP_DUP))
                return false;
            return emit1u32(c, OP_STORE, (u32)s);
        }
        default:
            cerr(c, "bad unary operator");
            return false;
        }
    }

    case A_POSTINC: {
        i32 s = resolve_var(c, n->name);
        if (s < 0) {
            cerr_name(c, "undeclared variable '%s'", n->name);
            return false;
        }
        if (!emit1u32(c, OP_LOAD, (u32)s))
            return false;
        if (!emit1(c, OP_DUP))
            return false;
        if (!emit1v(c, OP_PUSH, n->val ? 1 : (u64)-1))
            return false;
        if (!emit1(c, OP_ADD))
            return false;
        return emit1u32(c, OP_STORE, (u32)s);
    }

    case A_ASSIGN:
        return compile_assign(c, n);

    case A_STMTEXPR:
        return compile_expr(c, n->a);

    default:
        cerr(c, "bad expression node");
        return false;
    }
}

/* ---- statements ---- */

static bool compile_stmt(ccomp *c, const ast *n, bool last);

static bool compile_stmt(ccomp *c, const ast *n, bool last)
{
    switch (n->kind) {
    case A_STMTEXPR: {
        const ast *e = n->a;
        /* HolyC-style: a bare identifier naming a function is a call with
           default arguments (`version` == `Version()`). A local variable of
           the same name wins. */
        if (e->kind == A_VAR && resolve_var(c, e->name) < 0) {
            noc_fn *f = noc_lookup(e->name);
            if (f) {
                ast call;
                memset(&call, 0, sizeof(call));
                call.kind = A_CALL;
                call.name = e->name;
                if (!compile_call(c, &call))
                    return false;
                if (last && !c->in_fn && f->ret_type != NTYPE_VOID) {
                    c->fn->last_expr = 1;
                    return true;
                }
                return emit1(c, OP_POP);
            }
        }
        bool nonvoid = expr_type(c, e) != NTYPE_VOID;
        if (!compile_expr(c, e))
            return false;
        if (last && nonvoid && !c->in_fn) {
            c->fn->last_expr = 1;
            return true;
        }
        return emit1(c, OP_POP);
    }

    case A_VARDECL: {
        i32 s = add_var(c, n->name, n->type);
        if (s < 0)
            return false;
        if (n->a) {
            if (!compile_expr(c, n->a))
                return false;
            if (!emit1u32(c, OP_STORE, (u32)s))
                return false;
        }
        return true;
    }

    case A_BLOCK: {
        for (u32 i = 0; i < n->nargs; i++) {
            if (!compile_stmt(c, n->args[i], false))
                return false;
        }
        return true;
    }

    case A_IF: {
        if (!compile_expr(c, n->a))
            return false;
        usize je = emit_jump(c, OP_JMPF);
        if (je == (usize)-1)
            return false;
        if (!compile_stmt(c, n->b, false))
            return false;
        usize jd = (usize)-1;
        if (n->c) {
            jd = emit_jump(c, OP_JMP);
            if (jd == (usize)-1)
                return false;
        }
        usize l_else = code_pc(c);
        if (n->c) {
            if (!compile_stmt(c, n->c, false))
                return false;
        }
        usize l_done = code_pc(c);
        patch_jump(c, je, l_else);
        if (n->c)
            patch_jump(c, jd, l_done);
        return true;
    }

    case A_WHILE: {
        usize l_top = code_pc(c);
        if (!compile_expr(c, n->a))
            return false;
        usize je = emit_jump(c, OP_JMPF);
        if (je == (usize)-1)
            return false;
        if (!compile_stmt(c, n->b, false))
            return false;
        if (!emit_jump_at(c, OP_JMP, l_top))
            return false;
        usize l_end = code_pc(c);
        patch_jump(c, je, l_end);
        return true;
    }

    case A_FOR: {
        if (n->a) {
            if (n->a->kind == A_VARDECL) {
                if (!compile_stmt(c, n->a, false))
                    return false;
            } else {
                if (!compile_expr(c, n->a))
                    return false;
                if (!emit1(c, OP_POP))
                    return false;
            }
        }
        usize l_top = code_pc(c);
        usize je = (usize)-1;
        if (n->b) {
            if (!compile_expr(c, n->b))
                return false;
            je = emit_jump(c, OP_JMPF);
            if (je == (usize)-1)
                return false;
        }
        if (!compile_stmt(c, n->body, false))
            return false;
        if (n->c) {
            if (!compile_expr(c, n->c))
                return false;
            if (!emit1(c, OP_POP))
                return false;
        }
        if (!emit_jump_at(c, OP_JMP, l_top))
            return false;
        usize l_end = code_pc(c);
        if (n->b)
            patch_jump(c, je, l_end);
        return true;
    }

    case A_RETURN: {
        if (!c->in_fn) {
            cerr(c, "return outside function");
            return false;
        }
        if (n->a) {
            if (!compile_expr(c, n->a))
                return false;
        } else {
            if (!emit1v(c, OP_PUSH, 0))
                return false;
        }
        return emit1(c, OP_RET);
    }

    case A_FUNCDECL:
        cerr(c, "nested function definitions are not supported");
        return false;

    default:
        cerr(c, "bad statement node");
        return false;
    }
}

/* ---- function definitions ---- */

static void free_sub_resources(noc_fn *nf, ccomp *sub)
{
    if (sub->code.data)
        noc_os_free(sub->code.data);
    for (usize i = 0; i < nf->nstrings; i++) {
        if (nf->strings[i])
            noc_os_free(nf->strings[i]);
    }
    nf->nstrings = 0;
}

static bool compile_funcdecl(ccomp *c, const ast *fn)
{
    if (c->in_fn) {
        cerr(c, "nested function definitions are not supported");
        return false;
    }

    noc_fn *nf = noc_os_alloc(sizeof(noc_fn));
    if (!nf) {
        cerr(c, "out of memory (function)");
        return false;
    }
    memset(nf, 0, sizeof(noc_fn));
    nf->builtin = -1;

    usize len = strlen(fn->name);
    char *nm = noc_os_alloc(len + 1);
    if (!nm) {
        noc_os_free(nf);
        cerr(c, "out of memory (function name)");
        return false;
    }
    memcpy(nm, fn->name, len + 1);
    nf->name = nm;
    nf->ret_type = fn->type;
    nf->nargs = fn->nargs;
    nf->variadic = 0;

    for (u32 k = 0; k < fn->nargs; k++) {
        ast *p = fn->args[k];
        if (p && p->a) {
            nf->has_def[k] = 1;
            nf->defaults[k] = p->a->val;
        }
    }

    ccomp sub;
    memset(&sub, 0, sizeof(sub));
    sub.fn = nf;
    sub.a = c->a;
    sub.err = c->err;
    sub.in_fn = true;

    for (u32 k = 0; k < fn->nargs; k++) {
        sub.syms[k].name = fn->args[k]->name;
        sub.syms[k].type = fn->args[k]->type;
        sub.nsyms++;
    }

    if (fn->body && fn->body->kind == A_BLOCK) {
        for (u32 i = 0; i < fn->body->nargs; i++) {
            if (!compile_stmt(&sub, fn->body->args[i], false)) {
                free_sub_resources(nf, &sub);
                noc_os_free(nf->name);
                noc_os_free(nf);
                return false;
            }
        }
    }

    /* implicit `return 0` for a function that falls off the end */
    if (!emit1v(&sub, OP_PUSH, 0))
        goto fail;
    if (!emit1(&sub, OP_RET))
        goto fail;

    nf->code = sub.code.data;
    nf->code_len = sub.code.len;
    nf->nlocals = (u32)sub.nsyms;
    return noc_register_user_fn(nf) != NULL;

fail:
    free_sub_resources(nf, &sub);
    noc_os_free(nf->name);
    noc_os_free(nf);
    return false;
}

/* ---- top-level compile ---- */

bool noc_compile(ast *stmts, usize nstmt, noc_arena *a, noc_fn *chunk,
                 const char **err)
{
    ccomp c;
    memset(&c, 0, sizeof(c));
    c.fn = chunk;
    c.a = a;
    c.err = err;

    chunk->name = (char *)"<line>";
    chunk->builtin = -1;
    chunk->nstrings = 0;
    chunk->nlocals = 0;
    chunk->last_expr = 0;
    chunk->code = NULL;
    chunk->code_len = 0;
    chunk->ret_type = NTYPE_VOID;
    chunk->nargs = 0;
    chunk->variadic = 0;

    for (usize i = 0; i < nstmt; i++) {
        ast *s = &stmts[i];
        if (s->kind == A_FUNCDECL) {
            if (!compile_funcdecl(&c, s))
                goto fail;
            continue;
        }
        if (!compile_stmt(&c, s, i == nstmt - 1))
            goto fail;
    }

    if (!emit1(&c, OP_HALT))
        goto fail;

    chunk->code = c.code.data;
    chunk->code_len = c.code.len;
    chunk->nlocals = (u32)c.nsyms;
    return true;

fail:
    if (c.code.data)
        noc_os_free(c.code.data);
    c.code.data = NULL;
    for (usize i = 0; i < chunk->nstrings; i++) {
        if (chunk->strings[i])
            noc_os_free(chunk->strings[i]);
    }
    chunk->nstrings = 0;
    return false;
}
