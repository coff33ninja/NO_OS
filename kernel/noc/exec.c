#include "noc.h"
#include "noc_os.h"
#include "format.h"
#include "string.h"

#define NOC_ARENA_SIZE (1024 * 1024)

static void free_chunk(noc_fn *chunk)
{
    if (chunk->code) {
        noc_os_free(chunk->code);
        chunk->code = NULL;
    }
    for (usize i = 0; i < chunk->nstrings; i++) {
        if (chunk->strings[i]) {
            noc_os_free(chunk->strings[i]);
            chunk->strings[i] = NULL;
        }
    }
    chunk->nstrings = 0;
}

static void noc_report(const char *msg)
{
    noc_os_puts("NOC: ");
    noc_os_puts(msg);
    noc_os_putc('\n');
}

/* Lex/parse/compile/run one chunk of NOC. Returns true when the source was
   accepted as NOC (even if it hit a runtime error); false when NOC could
   not compile it. Uses only the noc_os platform layer, so it works both in
   the kernel REPL and inside a ring-3 process. */
bool noc_exec_line(const char *line)
{
    if (!line || !*line)
        return true;

    const char *err = NULL;
    noc_arena arena;
    memset(&arena, 0, sizeof(arena));
    arena.base = noc_os_alloc(NOC_ARENA_SIZE);
    if (!arena.base) {
        noc_report("out of memory (line arena)");
        return true;
    }
    arena.cap = NOC_ARENA_SIZE;

    tok *toks = noc_arena_alloc(&arena, sizeof(tok) * NOC_MAX_TOKS);
    if (!toks) {
        noc_report("out of memory (tokens)");
        noc_os_free(arena.base);
        return true;
    }
    usize ntoks = 0;
    if (!noc_lex(line, &arena, toks, NOC_MAX_TOKS, &ntoks, &err)) {
        noc_report(err ? err : "lex error");
        noc_os_free(arena.base);
        return false;
    }

    ast *stmts = NULL;
    usize nstmt = 0;
    if (!noc_parse(toks, ntoks, &arena, &stmts, &nstmt, &err)) {
        noc_report(err ? err : "parse error");
        noc_os_free(arena.base);
        return false;
    }

    noc_fn chunk;
    memset(&chunk, 0, sizeof(chunk));

    if (!noc_compile(stmts, nstmt, &arena, &chunk, &err)) {
        free_chunk(&chunk);
        noc_report(err ? err : "compile error");
        noc_os_free(arena.base);
        return false;
    }

    if (!noc_vm_run(&chunk)) {
        noc_report(noc_vm_error());
        free_chunk(&chunk);
        noc_os_free(arena.base);
        return true;
    }

    u64 res = 0;
    if (chunk.last_expr && noc_vm_take_result(&res) && res != 0) {
        char buf[32];
        sprintk(buf, sizeof(buf), "%d\n", (i64)res);
        noc_os_puts(buf);
    }

    free_chunk(&chunk);
    noc_os_free(arena.base);
    return true;
}

/* Validate a NOC source line without running it: lex + parse + compile only.
   Returns true when the text is accepted as NOC. Used to gate model-drafted
   programs before spawning them, so a hallucinated draft is rejected instead
   of executed. */
bool noc_check_syntax(const char *line)
{
    if (!line || !*line)
        return false;

    const char *err = NULL;
    noc_arena arena;
    memset(&arena, 0, sizeof(arena));
    arena.base = noc_os_alloc(NOC_ARENA_SIZE);
    if (!arena.base)
        return false;
    arena.cap = NOC_ARENA_SIZE;

    tok *toks = noc_arena_alloc(&arena, sizeof(tok) * NOC_MAX_TOKS);
    if (!toks) {
        noc_os_free(arena.base);
        return false;
    }
    usize ntoks = 0;
    bool ok = false;
    if (noc_lex(line, &arena, toks, NOC_MAX_TOKS, &ntoks, &err)) {
        ast *stmts = NULL;
        usize nstmt = 0;
        if (noc_parse(toks, ntoks, &arena, &stmts, &nstmt, &err)) {
            noc_fn chunk;
            memset(&chunk, 0, sizeof(chunk));
            ok = noc_compile(stmts, nstmt, &arena, &chunk, &err);
            free_chunk(&chunk);
        }
    }
    noc_os_free(arena.base);
    return ok;
}
