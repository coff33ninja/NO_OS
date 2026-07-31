#include "noc.h"
#include "noc_os.h"

#define SCRIPT_ADDR 0x00000100C0000000UL

/* Process entry. The kernel has copied the script source to SCRIPT_ADDR
   (NUL-terminated) and mapped a 64 KiB stack. One binary serves every
   script, so all work happens through the shared noc/ runtime. */
int nocproc_main(void)
{
    const char *script = (const char *)SCRIPT_ADDR;

    noc_init();
    noc_exec_line(script);
    return 0;
}
