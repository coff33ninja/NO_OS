#include "pmm.h"
#include "string.h"
#include "printk.h"

/* Bitmap covers the full identity-mapped 1 GiB window. */
#define PMM_MAX_FRAMES (0x40000000ULL / FRAME_SIZE)

static u8 bitmap[PMM_MAX_FRAMES / 8];
static u64 total_frames;
static u64 free_frames;

static void bitmap_set(u64 idx)   { bitmap[idx / 8] |= (u8)(1u << (idx % 8)); }
static void bitmap_clear(u64 idx) { bitmap[idx / 8] &= (u8)~(1u << (idx % 8)); }
static bool bitmap_test(u64 idx)  { return (bitmap[idx / 8] >> (idx % 8)) & 1u; }

static void pmm_reserve(u64 addr, u64 len)
{
    u64 first = addr / FRAME_SIZE;
    u64 last  = (addr + len + FRAME_SIZE - 1) / FRAME_SIZE;

    if (last > total_frames)
        last = total_frames;
    for (u64 i = first; i < last; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            free_frames--;
        }
    }
}

void pmm_init(u32 mb_info)
{
    u32 flags     = *(u32 *)(usize)mb_info;
    u32 mem_upper = *(u32 *)(usize)(mb_info + 8); /* KiB above 1 MiB */

    u64 mem_bytes = 0x100000ULL + (u64)mem_upper * 1024;
    if (mem_bytes > 0x40000000ULL)
        mem_bytes = 0x40000000ULL;

    total_frames = mem_bytes / FRAME_SIZE;
    free_frames  = total_frames;

    memset(bitmap, 0, sizeof(bitmap));

    /* Low memory (BIOS, VGA, EBDA) */
    pmm_reserve(0, 0x100000);

    /* Kernel image */
    extern char _kernel_start[], _kernel_end[];
    pmm_reserve((u64)_kernel_start, (u64)(_kernel_end - _kernel_start));

    /* Multiboot info area and any non-usable mmap regions */
    pmm_reserve(mb_info, 4096);
    if (flags & 0x40) {
        u64 a   = *(u32 *)(usize)(mb_info + 48);
        u64 end = a + *(u32 *)(usize)(mb_info + 44);
        while (a + 20 <= end) {
            u32 sz   = *(u32 *)a;
            u64 base = *(u64 *)(a + 4);
            u64 len  = *(u64 *)(a + 12);
            u32 type = *(u32 *)(a + 20);
            if (sz < 20)
                break;
            if (type != 1)
                pmm_reserve(base, len);
            a += sz + 4;
        }
    }

    printk("pmm: %u MiB, %u frames, %u free\n",
           (unsigned)(mem_bytes / 0x100000),
           (unsigned)total_frames,
           (unsigned)free_frames);
}

u64 pmm_alloc_frame(void)
{
    for (u64 i = 0; i < total_frames; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            free_frames--;
            return i * FRAME_SIZE;
        }
    }
    return 0;
}

u64 pmm_alloc_frames(usize count)
{
    usize need = 0;
    for (u64 i = 0; i < total_frames; i++) {
        if (bitmap_test(i)) {
            need = 0;
            continue;
        }
        need++;
        if (need == (usize)count) {
            u64 base = (i - need + 1) * FRAME_SIZE;
            for (usize j = 0; j < count; j++) {
                bitmap_set((base / FRAME_SIZE) + j);
                free_frames--;
            }
            return base;
        }
    }
    return 0;
}

void pmm_free_frame(u64 addr)
{
    u64 idx = addr / FRAME_SIZE;
    if (idx < total_frames && bitmap_test(idx)) {
        bitmap_clear(idx);
        free_frames++;
    }
}

void pmm_free_frames(u64 addr, usize count)
{
    for (usize j = 0; j < count; j++)
        pmm_free_frame(addr + j * FRAME_SIZE);
}

u64 pmm_total_frames(void) { return total_frames; }
u64 pmm_avail_frames(void) { return free_frames; }
u64 pmm_used_bytes(void)   { return (total_frames - free_frames) * FRAME_SIZE; }
