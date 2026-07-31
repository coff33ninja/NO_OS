#include "noc.h"
#include "heap.h"
#include "string.h"
#include "printk.h"
#include "prompt.h"

#define NOC_ARENA_SIZE (1024 * 1024)

static void free_chunk(noc_fn *chunk)
{
    if (chunk->code) {
        kfree(chunk->code);
        chunk->code = NULL;
    }
    for (usize i = 0; i < chunk->nstrings; i++) {
        if (chunk->strings[i]) {
            kfree(chunk->strings[i]);
            chunk->strings[i] = NULL;
        }
    }
    chunk->nstrings = 0;
}

/* Lex/parse/compile/run one line of NOC. Returns true when the line was
   accepted as NOC (even if it hit a runtime error); false when NOC could
   not compile it, so the caller may fall back to the legacy prompt. */
static bool noc_exec_line(const char *line)
{
    if (!line || !*line)
        return true;

    const char *err = NULL;
    noc_arena arena;
    memset(&arena, 0, sizeof(arena));
    arena.base = kmalloc(NOC_ARENA_SIZE);
    if (!arena.base) {
        printk("NOC: out of memory (line arena)\n");
        return true;
    }
    arena.cap = NOC_ARENA_SIZE;

    tok *toks = noc_arena_alloc(&arena, sizeof(tok) * NOC_MAX_TOKS);
    if (!toks) {
        printk("NOC: out of memory (tokens)\n");
        kfree(arena.base);
        return true;
    }
    usize ntoks = 0;
    if (!noc_lex(line, &arena, toks, NOC_MAX_TOKS, &ntoks, &err)) {
        printk("NOC: %s\n", err ? err : "lex error");
        kfree(arena.base);
        return false;
    }

    ast *stmts = NULL;
    usize nstmt = 0;
    if (!noc_parse(toks, ntoks, &arena, &stmts, &nstmt, &err)) {
        printk("NOC: %s\n", err ? err : "parse error");
        kfree(arena.base);
        return false;
    }

    noc_fn chunk;
    memset(&chunk, 0, sizeof(chunk));

    if (!noc_compile(stmts, nstmt, &arena, &chunk, &err)) {
        printk("NOC: %s\n", err ? err : "compile error");
        free_chunk(&chunk);
        kfree(arena.base);
        return false;
    }

    if (!noc_vm_run(&chunk)) {
        printk("NOC: %s\n", noc_vm_error());
        free_chunk(&chunk);
        kfree(arena.base);
        return true;
    }

    u64 res = 0;
    if (chunk.last_expr && noc_vm_take_result(&res) && res != 0)
        printk("%d\n", (i64)res);

    free_chunk(&chunk);
    kfree(arena.base);
    return true;
}

void noc_repl(void)
{
    char line[512];
    printk("NOC shell. type 'help' or NOC code.\n");
    for (;;) {
        printk("no/os> ");
        line_read(line, sizeof(line));
        if (!noc_exec_line(line))
            prompt_handle(line);
    }
}

/* Boot-time smoke test of the M2 acceptance cases. */
void noc_selftest(void)
{
    printk("noc self-test\n");
    noc_exec_line("PrintLn(\"NOC hello\"); 40+2;");
    noc_exec_line("I64 Mul2(I64 x, I64 y=2) { return x*y; }");
    noc_exec_line("PrintLn(\"%d\", Mul2(21));");
    noc_exec_line("PrintLn(\"%d\", Mul2(6));");
    noc_exec_line("I64 s=0; for (I64 i=0;i<10;i++) s+=i; PrintLn(\"%d\", s);");
    noc_exec_line("PrintLn(\"%d\", 1+2*3);");
    noc_exec_line("I64 n=5; if (n>3) PrintLn(\"%d\", 100); else PrintLn(\"%d\", 200);");
    noc_exec_line("I64 c=0; while (c<3) c+=1; PrintLn(\"%d\", c);");
    printk("noc-self-test-done\n");
}
