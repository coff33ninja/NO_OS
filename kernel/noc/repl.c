#include "noc.h"
#include "noc_os.h"
#include "string.h"
#include "printk.h"
#include "predict.h"
#include "interact.h"

void noc_repl(void)
{
    char line[512];
    printk("NOC shell. type 'Help;' or NOC code.\n");
    for (;;) {
        line_read(line, sizeof(line), "no/os> ");
        cmdhist_add(line);
        if (*line) {
            il_begin_capture();
            noc_exec_line(line);
            il_end_capture(line);
        }
    }
}

/* Boot-time smoke test of the M2 acceptance cases. */
void noc_selftest(void)
{
    printk("noc self-test\n");
    noc_exec_line("PrintLn(\"NOC hello\"); 40+2;");
    noc_exec_line("MemInfo;");
    noc_exec_line("help;");
    noc_exec_line("I64 Mul2(I64 x, I64 y=2) { return x*y; }");
    noc_exec_line("PrintLn(\"%d\", Mul2(21));");
    noc_exec_line("PrintLn(\"%d\", Mul2(6));");
    noc_exec_line("I64 s=0; for (I64 i=0;i<10;i++) s+=i; PrintLn(\"%d\", s);");
    noc_exec_line("PrintLn(\"%d\", 1+2*3);");
    noc_exec_line("I64 n=5; if (n>3) PrintLn(\"%d\", 100); else PrintLn(\"%d\", 200);");
    noc_exec_line("I64 c=0; while (c<3) c+=1; PrintLn(\"%d\", c);");
    printk("noc-self-test-done\n");
}
