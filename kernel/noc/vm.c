#include "noc.h"
#include "string.h"
#include "format.h"
#include "noc_os.h"

#ifndef NOOS_USER
#include "sched.h"
#include "pmm.h"
#include "heap.h"
#include "fs.h"
#endif

#define NOC_STACK_SIZE 4096
#define NOC_MAX_FRAMES 256

/* ---- global function registry ---- */

noc_fn *noc_funcs[NOC_MAX_FUNCS];
usize   noc_nfuncs;

static bool noc_name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return false;
        a++;
        b++;
    }
    return *a == *b;
}

noc_fn *noc_lookup(const char *name)
{
    for (usize i = 0; i < noc_nfuncs; i++) {
        if (noc_name_eq(noc_funcs[i]->name, name))
            return noc_funcs[i];
    }
    return NULL;
}

noc_fn *noc_register_user_fn(noc_fn *f)
{
    for (usize i = 0; i < noc_nfuncs; i++) {
        if (strcmp(noc_funcs[i]->name, f->name) == 0) {
            noc_fn *old = noc_funcs[i];
            if (old->code)
                noc_os_free(old->code);
            for (usize k = 0; k < old->nstrings; k++) {
                if (old->strings[k])
                    noc_os_free(old->strings[k]);
            }
            if (old->name)
                noc_os_free(old->name);
            noc_os_free(old);
            noc_funcs[i] = f;
            return f;
        }
    }
    if (noc_nfuncs >= NOC_MAX_FUNCS)
        return NULL;
    noc_funcs[noc_nfuncs++] = f;
    return f;
}

noc_fn *noc_register_builtin(const char *name, u32 ret_type, u32 nargs,
                             u8 variadic, i32 builtin_index)
{
    noc_fn *f = noc_os_alloc(sizeof(noc_fn));
    if (!f)
        return NULL;
    memset(f, 0, sizeof(noc_fn));

    usize len = strlen(name);
    char *nm = noc_os_alloc(len + 1);
    if (!nm) {
        noc_os_free(f);
        return NULL;
    }
    memcpy(nm, name, len + 1);

    f->name = nm;
    f->ret_type = ret_type;
    f->nargs = nargs;
    f->variadic = variadic;
    f->builtin = builtin_index;
    return noc_register_user_fn(f);
}

/* ---- VM state ---- */

static u64   vstack[NOC_STACK_SIZE];
static usize sp;

typedef struct {
    noc_fn *fn;
    u8     *code;
    usize   code_len;
    usize   pc;
    usize   vbase;
    u32     nlocals;
} frame;

static frame frames[NOC_MAX_FRAMES];
static usize nframes;

static char vm_err[128];
static bool has_result;
static u64  result;

const char *noc_vm_error(void)
{
    return vm_err;
}

bool noc_vm_take_result(u64 *out)
{
    if (!has_result)
        return false;
    *out = result;
    return true;
}

static void vmerr(const char *msg)
{
    sprintk(vm_err, sizeof(vm_err), "%s", msg);
}

static bool push(u64 v)
{
    if (sp >= NOC_STACK_SIZE) {
        vmerr("stack overflow");
        return false;
    }
    vstack[sp++] = v;
    return true;
}

static bool pop(u64 *out)
{
    if (sp == 0) {
        vmerr("stack underflow");
        return false;
    }
    *out = vstack[--sp];
    return true;
}

static bool run_loop(void)
{
    u64 iters = 0;
    while (nframes > 0) {
        /* Break out of runaway programs: Esc / Ctrl+C aborts the run. */
        if ((iters++ & 0xFF) == 0) {
            int c = noc_os_kbd_peek();
            if (c == 0x1B || c == 0x03) {
                noc_os_kbd_poll(); /* consume */
                vmerr("interrupted");
                return false;
            }
        }
        frame *f = &frames[nframes - 1];
        if (f->pc >= f->code_len) {
            vmerr("program counter ran off end of code");
            return false;
        }
        u8 op = f->code[f->pc++];

        switch (op) {
        case OP_PUSH: {
            u64 val;
            memcpy(&val, &f->code[f->pc], 8);
            f->pc += 8;
            if (!push(val))
                return false;
            break;
        }
        case OP_PUSHSTR: {
            u32 si;
            memcpy(&si, &f->code[f->pc], 4);
            f->pc += 4;
            if (si >= f->fn->nstrings) {
                vmerr("bad string index");
                return false;
            }
            if (!push((u64)f->fn->strings[si]))
                return false;
            break;
        }
        case OP_DUP: {
            if (sp == 0) {
                vmerr("stack underflow");
                return false;
            }
            if (!push(vstack[sp - 1]))
                return false;
            break;
        }
        case OP_POP: {
            if (sp == 0) {
                vmerr("stack underflow");
                return false;
            }
            sp--;
            break;
        }
        case OP_LOAD: {
            u32 slot;
            memcpy(&slot, &f->code[f->pc], 4);
            f->pc += 4;
            if (slot >= f->nlocals) {
                vmerr("bad local slot");
                return false;
            }
            if (!push(vstack[f->vbase + slot]))
                return false;
            break;
        }
        case OP_STORE: {
            u32 slot;
            memcpy(&slot, &f->code[f->pc], 4);
            f->pc += 4;
            u64 val;
            if (!pop(&val))
                return false;
            if (slot >= f->nlocals) {
                vmerr("bad local slot");
                return false;
            }
            vstack[f->vbase + slot] = val;
            break;
        }
        case OP_CALL: {
            u32 si, count;
            memcpy(&si, &f->code[f->pc], 4);
            memcpy(&count, &f->code[f->pc + 4], 4);
            f->pc += 8;
            if (si >= f->fn->nstrings) {
                vmerr("bad function index");
                return false;
            }
            noc_fn *callee = noc_lookup(f->fn->strings[si]);
            if (!callee) {
                vmerr("unknown function at runtime");
                return false;
            }
            if (sp < count) {
                vmerr("call with too few arguments on stack");
                return false;
            }
            u64 *args = &vstack[sp - count];

            if (callee->builtin >= 0) {
                u64 ret = 0;
                if (!noc_builtins[callee->builtin].fn(NULL, args, count, &ret)) {
                    vmerr("builtin call failed");
                    return false;
                }
                sp -= count;
                if (!push(ret))
                    return false;
                break;
            }

            usize vbase = sp - count;
            if (nframes >= NOC_MAX_FRAMES) {
                vmerr("call nesting too deep");
                return false;
            }
            if (callee->nlocals < count) {
                vmerr("bad callee frame");
                return false;
            }
            if (vbase + callee->nlocals > NOC_STACK_SIZE) {
                vmerr("stack overflow on call");
                return false;
            }
            sp = vbase + callee->nlocals;
            for (u32 k = count; k < callee->nlocals; k++)
                vstack[vbase + k] = 0;

            frames[nframes].fn = callee;
            frames[nframes].code = callee->code;
            frames[nframes].code_len = callee->code_len;
            frames[nframes].pc = 0;
            frames[nframes].vbase = vbase;
            frames[nframes].nlocals = callee->nlocals;
            nframes++;
            break;
        }
        case OP_RET: {
            if (nframes == 1) {
                vmerr("return at top level");
                return false;
            }
            u64 val;
            if (!pop(&val))
                return false;
            frame *cf = &frames[nframes - 1];
            sp = cf->vbase;
            nframes--;
            if (!push(val))
                return false;
            break;
        }
        case OP_JMP: {
            u32 t;
            memcpy(&t, &f->code[f->pc], 4);
            f->pc = t;
            break;
        }
        case OP_JMPF: {
            u32 t;
            memcpy(&t, &f->code[f->pc], 4);
            f->pc += 4;
            u64 v;
            if (!pop(&v))
                return false;
            if (v == 0)
                f->pc = t;
            break;
        }
        case OP_JMPT: {
            u32 t;
            memcpy(&t, &f->code[f->pc], 4);
            f->pc += 4;
            u64 v;
            if (!pop(&v))
                return false;
            if (v != 0)
                f->pc = t;
            break;
        }
        case OP_HALT:
            return true;
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_DIV:
        case OP_MOD:
        case OP_EQ:
        case OP_NE:
        case OP_LT:
        case OP_LE:
        case OP_GT:
        case OP_GE:
        case OP_BAND:
        case OP_BOR:
        case OP_BXOR:
        case OP_SHL:
        case OP_SHR: {
            u64 b, a;
            if (!pop(&b))
                return false;
            if (!pop(&a))
                return false;
            u64 r;
            switch (op) {
            case OP_ADD: r = a + b; break;
            case OP_SUB: r = a - b; break;
            case OP_MUL: r = a * b; break;
            case OP_DIV:
                if (b == 0) {
                    vmerr("division by zero");
                    return false;
                }
                r = (u64)((i64)a / (i64)b);
                break;
            case OP_MOD:
                if (b == 0) {
                    vmerr("division by zero");
                    return false;
                }
                r = (u64)((i64)a % (i64)b);
                break;
            case OP_EQ: r = (a == b) ? 1 : 0; break;
            case OP_NE: r = (a != b) ? 1 : 0; break;
            case OP_LT: r = ((i64)a < (i64)b) ? 1 : 0; break;
            case OP_LE: r = ((i64)a <= (i64)b) ? 1 : 0; break;
            case OP_GT: r = ((i64)a > (i64)b) ? 1 : 0; break;
            case OP_GE: r = ((i64)a >= (i64)b) ? 1 : 0; break;
            case OP_BAND: r = a & b; break;
            case OP_BOR:  r = a | b; break;
            case OP_BXOR: r = a ^ b; break;
            case OP_SHL:  r = a << (b & 63); break;
            default:      r = a >> (b & 63); break;  /* OP_SHR: logical */
            }
            if (!push(r))
                return false;
            break;
        }
        case OP_NOT: {
            u64 a;
            if (!pop(&a))
                return false;
            if (!push(a == 0 ? 1 : 0))
                return false;
            break;
        }
        case OP_NEG: {
            u64 a;
            if (!pop(&a))
                return false;
            if (!push((u64)(-(i64)a)))
                return false;
            break;
        }
        case OP_BNOT: {
            u64 a;
            if (!pop(&a))
                return false;
            if (!push(~a))
                return false;
            break;
        }
        default:
            vmerr("unknown opcode");
            return false;
        }
    }
    vmerr("no active frame");
    return false;
}

bool noc_vm_run(noc_fn *f)
{
    vm_err[0] = '\0';
    has_result = false;
    result = 0;
    sp = 0;
    nframes = 0;

    if (!f || !f->code) {
        vmerr("empty program");
        return false;
    }
    if (f->nlocals > NOC_STACK_SIZE) {
        vmerr("too many locals");
        return false;
    }

    for (u32 k = 0; k < f->nlocals; k++) {
        if (!push(0))
            return false;
    }

    frames[0].fn = f;
    frames[0].code = f->code;
    frames[0].code_len = f->code_len;
    frames[0].pc = 0;
    frames[0].vbase = 0;
    frames[0].nlocals = f->nlocals;
    nframes = 1;

    if (!run_loop())
        return false;

    if (f->last_expr) {
        has_result = true;
        result = vstack[f->nlocals];
    }
    return true;
}

/* ---- noc_format: printf-like over u64 argument array ---- */

bool noc_format(char *out, usize cap, const char *fmt, const u64 *args, usize n)
{
    usize w = 0, a = 0;
    if (!out || cap == 0)
        return false;

    while (*fmt && w + 1 < cap) {
        char c = *fmt++;
        if (c != '%') {
            out[w++] = c;
            continue;
        }
        c = *fmt++;
        if (c == 'l') {
            c = *fmt++;
            if (c == 'l')
                c = *fmt++;
        }
        if (c == '%') {
            out[w++] = '%';
            continue;
        }
        if (a >= n)
            break;
        switch (c) {
        case 'c':
            out[w++] = (char)args[a++];
            break;
        case 's': {
            const char *s = (const char *)args[a++];
            if (!s)
                s = "(null)";
            while (*s && w + 1 < cap)
                out[w++] = *s++;
            break;
        }
        case 'd': {
            i64 v = (i64)args[a++];
            char tmp[24];
            usize i = 0;
            bool neg = v < 0;
            u64 u = neg ? (u64)(-v) : (u64)v;
            if (u == 0)
                tmp[i++] = '0';
            while (u && i < sizeof(tmp)) {
                tmp[i++] = (char)('0' + u % 10);
                u /= 10;
            }
            if (neg && w + 1 < cap)
                out[w++] = '-';
            while (i > 0 && w + 1 < cap)
                out[w++] = tmp[--i];
            break;
        }
        case 'u': {
            u64 v = args[a++];
            char tmp[24];
            usize i = 0;
            if (v == 0)
                tmp[i++] = '0';
            while (v && i < sizeof(tmp)) {
                tmp[i++] = (char)('0' + v % 10);
                v /= 10;
            }
            while (i > 0 && w + 1 < cap)
                out[w++] = tmp[--i];
            break;
        }
        case 'x':
        case 'X': {
            u64 v = args[a++];
            usize nn = aformat(out + w, cap > w ? cap - w : 0, 16, v, c == 'X');
            w += nn;
            break;
        }
        case 'p': {
            u64 v = args[a++];
            out[w++] = '0';
            if (w + 1 < cap)
                out[w++] = 'x';
            usize nn = aformat(out + w, cap > w ? cap - w : 0, 16, v, false);
            w += nn;
            break;
        }
        default:
            out[w++] = '%';
            if (w + 1 < cap)
                out[w++] = c;
            break;
        }
    }
    out[w] = '\0';
    return true;
}

/* ---- builtins ---- */

static bool b_Print(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    if (n < 1)
        return false;
    char buf[512];
    noc_format(buf, sizeof(buf), (const char *)args[0], args + 1, n - 1);
    noc_os_puts(buf);
    *ret = 0;
    return true;
}

static bool b_PrintLn(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    if (n < 1)
        return false;
    char buf[512];
    noc_format(buf, sizeof(buf), (const char *)args[0], args + 1, n - 1);
    noc_os_puts(buf);
    noc_os_putc('\n');
    *ret = 0;
    return true;
}

static bool b_Time(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    (void)args;
    (void)n;
    *ret = noc_os_ticks() * 10; /* PIT is 100 Hz -> 10 ms per tick */
    return true;
}

static bool b_Sleep(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    (void)ret;
    if (n < 1)
        return false;
    noc_os_sleep((usize)args[0]);
    return true;
}

static bool b_KeyGet(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    (void)args;
    (void)n;
    *ret = (u64)noc_os_kbd_wait();
    return true;
}

static bool b_KeyPressed(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    (void)args;
    (void)n;
    *ret = (noc_os_kbd_peek() >= 0) ? 1 : 0;
    return true;
}

static bool b_Alloc(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    if (n < 1)
        return false;
    *ret = (u64)noc_os_alloc((usize)args[0]);
    return true;
}

static bool b_Free(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    (void)ret;
    if (n < 1)
        return false;
    noc_os_free((void *)args[0]);
    return true;
}

static bool b_MemSet(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    (void)ret;
    if (n < 3)
        return false;
    memset((void *)args[0], (int)args[1], (u64)args[2]);
    return true;
}

static bool b_MemCpy(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    (void)ret;
    if (n < 3)
        return false;
    memcpy((void *)args[0], (const void *)args[1], (u64)args[2]);
    return true;
}

static bool b_Len(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    if (n < 1)
        return false;
    *ret = strlen((const char *)args[0]);
    return true;
}

static bool b_Help(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    (void)args;
    (void)n;
    noc_os_puts("commands: Help, Version, MemInfo, Echo, FaultTest, Reboot, "
                "Print, PrintLn, Sleep, Time, KeyGet, KeyPressed, Alloc, Free, "
                "MemSet, MemCpy, Len, Spawn, Ps, Demo, SaveFile, ReadFile, "
                "DeleteFile, ListDir, StatFile, FormatDisk\n");
    *ret = 0;
    return true;
}

#ifndef NOOS_USER

static bool b_Echo(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    (void)ret;
    if (n < 1)
        return false;
    char buf[512];
    noc_format(buf, sizeof(buf), (const char *)args[0], args + 1, n - 1);
    noc_os_puts(buf);
    noc_os_putc('\n');
    return true;
}

static bool b_Version(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    (void)args;
    (void)n;
    noc_os_puts("kernel version: NO_OS v0.1\n");
    *ret = 0;
    return true;
}

static bool b_MemInfo(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    (void)args;
    (void)n;
    u64 total = pmm_total_frames();
    u64 avail = pmm_avail_frames();
    u64 free_mib = avail * FRAME_SIZE / 0x100000;
    u64 tot_mib  = total * FRAME_SIZE / 0x100000;
    char buf[256];
    sprintk(buf, sizeof(buf),
            "mem: %u MiB free of %u MiB total (%u frames)\n",
            (unsigned)free_mib, (unsigned)tot_mib, (unsigned)avail);
    noc_os_puts(buf);
    sprintk(buf, sizeof(buf), "heap: %u bytes used in %u blocks\n",
            (unsigned)heap_used_bytes(), (unsigned)heap_blocks());
    noc_os_puts(buf);
    *ret = 0;
    return true;
}

static bool b_FaultTest(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    (void)args;
    (void)n;
    (void)ret;
    noc_os_puts("triggering deliberate #UD (invalid opcode)\n");
    __asm__ volatile("ud2");
    *ret = 0;
    return true;
}

static bool b_Reboot(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    (void)args;
    (void)n;
    (void)ret;
    noc_os_puts("rebooting...\n");
    __asm__ volatile("cli");
    __asm__ volatile("outb %%al, $0x64" :: "a"((u8)0xFE));
    for (;;)
        __asm__ volatile("hlt");
    *ret = 0;
    return true;
}

static bool b_Spawn(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    if (n < 1)
        return false;
    *ret = (u64)sched_spawn((const char *)args[0], "user");
    return true;
}

static bool b_Ps(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    (void)args;
    (void)n;
    sched_ps();
    *ret = 0;
    return true;
}

static bool b_Demo(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    (void)args;
    (void)n;
    char *pa = noc_os_alloc(256);
    char *pb = noc_os_alloc(256);
    if (!pa || !pb) {
        if (pa)
            noc_os_free(pa);
        if (pb)
            noc_os_free(pb);
        *ret = (u64)-1;
        return true;
    }
    sprintk(pa, 256, "I64 a=0; while (1) { Print(\"A\\n\"); Sleep(500); a++; }");
    sprintk(pb, 256, "I64 b=0; while (1) { Print(\"B\\n\"); Sleep(500); b++; }");
    i64 ida = sched_spawn(pa, "dema");
    i64 idb = sched_spawn(pb, "demb");
    noc_os_free(pa);
    noc_os_free(pb);
    *ret = (u64)ida;
    (void)idb;
    return true;
}

static bool b_SaveFile(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    (void)ret;
    if (n < 2)
        return false;
    const char *name = (const char *)args[0];
    const char *data = (const char *)args[1];
    int ino = fs_lookup(name);
    if (ino >= 0) {
        if (fs_unlink(name) != 0) {
            noc_os_puts("fs: unlink failed\n");
            return true;
        }
    }
    ino = fs_create(name, FS_INODE_REG);
    if (ino < 0) {
        noc_os_puts("fs: create failed\n");
        *ret = (u64)-1;
        return true;
    }
    if (fs_write_file((u32)ino, data, strlen(data)) != 0) {
        noc_os_puts("fs: write failed\n");
        *ret = (u64)-1;
        return true;
    }
    fs_sync_bitmap();
    char buf[64];
    sprintk(buf, sizeof(buf), "fs: saved %s (%u bytes)\n", name,
            (unsigned)strlen(data));
    noc_os_puts(buf);
    *ret = 0;
    return true;
}

static bool b_ReadFile(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    if (n < 1)
        return false;
    const char *name = (const char *)args[0];
    int ino = fs_lookup(name);
    if (ino < 0) {
        noc_os_puts("fs: not found\n");
        *ret = (u64)-1;
        return true;
    }
    struct fs_inode in;
    if (fs_stat((u32)ino, &in) != 0) {
        *ret = (u64)-1;
        return true;
    }
    u64 len = in.size;
    char *buf = noc_os_alloc((usize)len + 1);
    if (!buf) {
        *ret = (u64)-1;
        return true;
    }
    fs_read_file((u32)ino, buf, len);
    buf[len] = '\0';
    *ret = (u64)buf;
    return true;
}

static bool b_DeleteFile(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    if (n < 1)
        return false;
    const char *name = (const char *)args[0];
    if (fs_unlink(name) != 0) {
        noc_os_puts("fs: not found\n");
        *ret = (u64)-1;
        return true;
    }
    fs_sync_bitmap();
    noc_os_puts("fs: deleted\n");
    *ret = 0;
    return true;
}

static bool b_ListDir(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    (void)args;
    (void)n;
    fs_listdir();
    *ret = 0;
    return true;
}

static bool b_StatFile(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    if (n < 1)
        return false;
    const char *name = (const char *)args[0];
    int ino = fs_lookup(name);
    if (ino < 0) {
        noc_os_puts("fs: not found\n");
        *ret = (u64)-1;
        return true;
    }
    struct fs_inode in;
    fs_stat((u32)ino, &in);
    char buf[128];
    sprintk(buf, sizeof(buf), "fs: %s inode=%u size=%u mode=0x%x\n",
            name, ino, (unsigned)in.size, (unsigned)in.mode);
    noc_os_puts(buf);
    *ret = 0;
    return true;
}

static bool b_FormatDisk(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    (void)args;
    (void)n;
    if (fs_format() != 0) {
        noc_os_puts("fs: format failed\n");
        *ret = (u64)-1;
        return true;
    }
    if (fs_mount() != 0) {
        noc_os_puts("fs: remount failed\n");
        *ret = (u64)-1;
        return true;
    }
    noc_os_puts("fs: formatted and remounted\n");
    *ret = 0;
    return true;
}

/* Run executes a saved script by feeding its lines to noc_exec_line, exactly
   like the REPL would. noc_exec_line re-enters noc_vm_run, which resets the
   global vstack/frames/sp, so the enclosing chunk's execution state must be
   snapshotted around the nested run and restored afterwards. */
typedef struct {
    u64   vstack[NOC_STACK_SIZE];
    frame frames[NOC_MAX_FRAMES];
    usize sp;
    usize nframes;
    char  err[128];
    bool  has_result;
    u64   result;
} vm_state_t;

static vm_state_t *vm_state_save(void)
{
    vm_state_t *s = noc_os_alloc(sizeof(vm_state_t));
    if (!s)
        return NULL;
    memcpy(s->vstack, vstack, sizeof(vstack));
    memcpy(s->frames, frames, sizeof(frames));
    s->sp = sp;
    s->nframes = nframes;
    memcpy(s->err, vm_err, sizeof(vm_err));
    s->has_result = has_result;
    s->result = result;
    return s;
}

static void vm_state_restore(vm_state_t *s)
{
    memcpy(vstack, s->vstack, sizeof(vstack));
    memcpy(frames, s->frames, sizeof(frames));
    sp = s->sp;
    nframes = s->nframes;
    memcpy(vm_err, s->err, sizeof(vm_err));
    has_result = s->has_result;
    result = s->result;
    noc_os_free(s);
}

static bool b_Run(void *vm, u64 *args, usize n, u64 *ret)
{
    (void)vm;
    if (n < 1)
        return false;
    const char *name = (const char *)args[0];
    int ino = fs_lookup(name);
    if (ino < 0) {
        noc_os_puts("fs: not found\n");
        *ret = (u64)-1;
        return true;
    }
    struct fs_inode in;
    if (fs_stat((u32)ino, &in) != 0) {
        *ret = (u64)-1;
        return true;
    }
    u64 len = in.size;
    char *src = noc_os_alloc((usize)len + 1);
    if (!src) {
        *ret = (u64)-1;
        return true;
    }
    fs_read_file((u32)ino, src, len);
    src[len] = '\0';

    vm_state_t *sv = vm_state_save();
    if (!sv) {
        noc_os_free(src);
        *ret = (u64)-1;
        return true;
    }

    char buf[64];
    sprintk(buf, sizeof(buf), "fs: run %s\n", name);
    noc_os_puts(buf);

    char *line = src;
    for (usize i = 0; line[i]; i++) {
        if (line[i] == '\n') {
            line[i] = '\0';
            if (i > 0 && line[i - 1] == '\r')
                line[i - 1] = '\0';
            noc_exec_line(line);
            line = &line[i + 1];
            i = 0;
        }
    }
    if (*line) {
        usize ll = strlen(line);
        if (ll > 0 && line[ll - 1] == '\r')
            line[ll - 1] = '\0';
        noc_exec_line(line);
    }

    vm_state_restore(sv);
    noc_os_free(src);
    *ret = 0;
    return true;
}

#endif /* !NOOS_USER */

const noc_builtin noc_builtins[] = {
    { "Print",      NTYPE_VOID, 1, true,  b_Print },
    { "PrintLn",    NTYPE_VOID, 1, true,  b_PrintLn },
    { "Time",       NTYPE_I64,  0, false, b_Time },
    { "Sleep",      NTYPE_VOID, 1, false, b_Sleep },
    { "KeyGet",     NTYPE_I64,  0, false, b_KeyGet },
    { "KeyPressed", NTYPE_I64,  0, false, b_KeyPressed },
    { "Alloc",      NTYPE_STR,  1, false, b_Alloc },
    { "Free",       NTYPE_VOID, 1, false, b_Free },
    { "MemSet",     NTYPE_VOID, 3, false, b_MemSet },
    { "MemCpy",     NTYPE_VOID, 3, false, b_MemCpy },
    { "Len",        NTYPE_I64,  1, false, b_Len },
    { "Help",       NTYPE_VOID, 0, false, b_Help },
#ifndef NOOS_USER
    { "Echo",       NTYPE_VOID, 1, false, b_Echo },
    { "Version",    NTYPE_VOID, 0, false, b_Version },
    { "MemInfo",    NTYPE_VOID, 0, false, b_MemInfo },
    { "FaultTest",  NTYPE_VOID, 0, false, b_FaultTest },
    { "Reboot",     NTYPE_VOID, 0, false, b_Reboot },
    { "Spawn",      NTYPE_I64,  1, false, b_Spawn },
    { "Ps",         NTYPE_VOID, 0, false, b_Ps },
    { "Demo",       NTYPE_VOID, 0, false, b_Demo },
    { "SaveFile",   NTYPE_VOID, 2, false, b_SaveFile },
    { "ReadFile",   NTYPE_STR,  1, false, b_ReadFile },
    { "DeleteFile", NTYPE_VOID, 1, false, b_DeleteFile },
    { "ListDir",    NTYPE_VOID, 0, false, b_ListDir },
    { "StatFile",   NTYPE_VOID, 1, false, b_StatFile },
    { "FormatDisk", NTYPE_VOID, 0, false, b_FormatDisk },
    { "Run",        NTYPE_VOID, 1, false, b_Run },
#endif
};
usize noc_nbuiltins = sizeof(noc_builtins) / sizeof(noc_builtins[0]);

void noc_init(void)
{
    for (usize i = 0; i < noc_nbuiltins; i++) {
        const noc_builtin *b = &noc_builtins[i];
        noc_register_builtin(b->name, b->ret_type, b->nargs, b->variadic,
                             (i32)i);
    }
}
