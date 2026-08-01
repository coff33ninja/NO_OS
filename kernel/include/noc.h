#ifndef NOOS_NOC_H
#define NOOS_NOC_H

#include "types.h"

/* ---- public entry ---- */
void noc_init(void);
void noc_repl(void);
void noc_selftest(void);

/* Shared lex/parse/compile/run of one NOC chunk (exec.c). Uses only the
   noc_os platform layer, so it works in the kernel REPL and in ring 3. */
bool noc_exec_line(const char *line);
/* Validate a NOC line (lex+parse+compile, no run). Gates model drafts. */
bool noc_check_syntax(const char *line);

/* ---- limits ---- */
#define NOC_MAX_ARGS     16
#define NOC_MAX_LOCALS   64
#define NOC_MAX_STRINGS  64
#define NOC_MAX_FUNCS    128
#define NOC_MAX_TOKS     512
#define NOC_MAX_NODES    1024
#define NOC_MAX_PATCHES  256

/* ---- type codes ---- */
#define NTYPE_VOID 0
#define NTYPE_I64  1
#define NTYPE_U64  2
#define NTYPE_BOOL 3
#define NTYPE_STR  4
#define NTYPE_U8   5
#define NTYPE_I8   6
#define NTYPE_U16  7
#define NTYPE_I16  8
#define NTYPE_U32  9
#define NTYPE_I32  10
#define NTYPE_F64  11

/* ---- tokens ---- */
typedef enum {
    T_EOF, T_NUM, T_STR, T_IDENT,
    T_LP, T_RP, T_LB, T_RB, T_SEMI, T_COMMA,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PCT,
    T_EQ, T_EQEQ, T_NEQ, T_LT, T_LE, T_GT, T_GE,
    T_ANDAND, T_OROR, T_BANG, T_AMP, T_PIPE, T_CARET, T_TILDE,
    T_SHL, T_SHR, T_PLUSPLUS, T_MINUSMINUS,
    T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ,
    T_IF, T_ELSE, T_WHILE, T_FOR, T_RETURN, T_TRUE, T_FALSE,
    T_TYPE_U0, T_TYPE_I64, T_TYPE_U64, T_TYPE_BOOL, T_TYPE_STR,
    T_TYPE_U8, T_TYPE_U16, T_TYPE_U32, T_TYPE_I8, T_TYPE_I16, T_TYPE_I32,
    T_TYPE_F64
} tok_t;

typedef struct {
    tok_t t;
    u64   num;
    char *s; /* arena-backed text for T_STR/T_IDENT */
} tok;

/* ---- AST ---- */
typedef enum {
    A_BLOCK, A_STMTEXPR, A_VARDECL, A_IF, A_WHILE, A_FOR, A_RETURN,
    A_FUNCDECL, A_LIT, A_STRLIT, A_VAR, A_BIN, A_UN, A_ASSIGN, A_CALL,
    A_PREINC, A_POSTINC
} ast_t;

typedef struct ast ast;
struct ast {
    ast_t  kind;
    u64    val;      /* literal value / inc-dec direction */
    char  *name;     /* var / function name */
    u32    type;     /* declared type code */
    i32    slot;     /* local slot after resolution, else -1 */
    u32    binop;    /* for A_BIN/A_ASSIGN/A_UN: operator code */
    u32    nargs;
    ast   *a, *b, *c;
    ast  **args;
    ast   *body;
};

/* A_BIN / A_ASSIGN operator codes */
enum {
    BIN_ADD, BIN_SUB, BIN_MUL, BIN_DIV, BIN_MOD,
    BIN_EQ, BIN_NE, BIN_LT, BIN_LE, BIN_GT, BIN_GE,
    BIN_BAND, BIN_BOR, BIN_BXOR, BIN_SHL, BIN_SHR,
    BIN_ANDAND, BIN_OROR,
    BIN_ASSIGN, BIN_ADDASSIGN, BIN_SUBASSIGN, BIN_MULASSIGN, BIN_DIVASSIGN
};

/* ---- bytecode ---- */
enum {
    OP_PUSH, OP_PUSHSTR, OP_DUP, OP_POP,
    OP_LOAD, OP_STORE,
    OP_CALL,
    OP_RET, OP_JMP, OP_JMPF, OP_JMPT,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
    OP_BAND, OP_BOR, OP_BXOR, OP_SHL, OP_SHR,
    OP_NOT, OP_NEG, OP_BNOT,
    OP_HALT
};

/* ---- functions ---- */
typedef struct {
    char  *name;
    u8    *code;
    usize  code_len;
    char  *strings[NOC_MAX_STRINGS];
    usize  nstrings;
    u32    nargs;
    u32    nlocals;
    u32    ret_type;      /* NTYPE_* */
    u64    defaults[NOC_MAX_ARGS];
    u8     has_def[NOC_MAX_ARGS];
    i32    builtin;       /* builtin index or -1 */
    u8     variadic;
    u8     last_expr;     /* top-level chunk: last stmt is non-void expr */
} noc_fn;

/* ---- global function registry ---- */
extern noc_fn *noc_funcs[NOC_MAX_FUNCS];
extern usize   noc_nfuncs;
extern noc_fn *noc_lookup(const char *name);
extern noc_fn *noc_register_builtin(const char *name, u32 ret_type, u32 nargs,
                                    u8 variadic, i32 builtin_index);
/* Register (or replace) a user-defined function. Takes ownership of f. */
extern noc_fn *noc_register_user_fn(noc_fn *f);

/* ---- builtins (defined in vm.c) ---- */
typedef struct {
    const char *name;
    u32         ret_type;
    u32         nargs;
    u8          variadic;
    bool      (*fn)(void *vm, u64 *args, usize n, u64 *ret);
} noc_builtin;
extern const noc_builtin noc_builtins[];
extern usize noc_nbuiltins;
extern bool noc_format(char *out, usize cap, const char *fmt,
                       const u64 *args, usize n);

/* ---- line editor (kern/line.c) ---- */
usize line_read(char *buf, usize cap, const char *prompt);

/* ---- internals (lexer.c / parser.c / compiler.c) ---- */
typedef struct {
    u8 *base;
    usize used;
    usize cap;
} noc_arena;

void *noc_arena_alloc(noc_arena *a, usize n);

bool noc_lex(const char *src, noc_arena *a, tok *toks, usize maxtoks,
             usize *ntoks, const char **err);
bool noc_parse(tok *toks, usize ntoks, noc_arena *a, ast **out,
               usize *nstmt, const char **err);
bool noc_compile(ast *stmts, usize nstmt, noc_arena *a, noc_fn *chunk,
                 const char **err);

/* run one compiled function (fn->builtin must be -1); returns success. */
bool noc_vm_run(noc_fn *f);
/* text of the last VM runtime error ("" if none) */
const char *noc_vm_error(void);
/* copies the last-expression result (set when f->last_expr); returns false if none */
bool noc_vm_take_result(u64 *out);

#endif
