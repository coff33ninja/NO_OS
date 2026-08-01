/* M5: kernel-managed swap. User pages are evicted to a reserved tail of the
   disk (the "swap region") and transparently swapped back in on the next
   page fault, so a user process keeps running correctly under memory
   pressure. Swap is kernel-driven: user scripts only see SwapOut (explicit
   eviction) and SwapInfo (accounting) through the NOC builtins/syscalls.

   Geometry is derived from the real disk at boot: swap_geometry() reserves
   1/4 of the total capacity as the swap tail and noosfs.c sizes its data
   area to everything below that tail, so no hardcoded sizes exist anywhere
   and the same binary scales from the 32 MiB test image to a multi-GiB
   baremetal disk. */

#include "swap.h"
#include "ide.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "pgpred.h"
#include "printk.h"
#include "string.h"

#define SWAP_PAGE_SECTORS (FRAME_SIZE / SECTOR_SIZE) /* 8 x 512B = 1 page */

static struct swap_geom geom;
static u8 *slots;     /* one bit per slot, kmalloc'd at init */
static bool active;
static u64 swaps_out, swaps_in, reclaims;

static u64 slot_lba(u32 slot)
{
    return geom.start + (u64)slot * SWAP_PAGE_SECTORS;
}

void swap_geometry(u64 disk_blocks, struct swap_geom *g)
{
    u64 blocks = disk_blocks >> 2; /* reserve 1/4 of the disk */
    if (blocks < SWAP_PAGE_SECTORS)
        blocks = SWAP_PAGE_SECTORS; /* at least one page */
    g->start  = disk_blocks - blocks;
    g->blocks = blocks;
    g->pages  = blocks / SWAP_PAGE_SECTORS;
}

int swap_init(void)
{
    u64 disk = ide_drive_sectors();
    if (!disk)
        disk = 32 * 1024 * 1024 / SECTOR_SIZE; /* unknown geometry: probe default */
    swap_geometry(disk, &geom);
    slots = kmalloc((geom.pages + 7) / 8);
    if (!slots)
        return -1;
    memset(slots, 0, (geom.pages + 7) / 8);
    active = true;
    printk("swap: %u slots (%u KB) at LBA %u\n",
           (unsigned)geom.pages, (unsigned)(geom.blocks / 2),
           (unsigned)geom.start);
    return 0;
}

static u32 slot_alloc(void)
{
    if (!active)
        return (u32)-1;
    for (u32 i = 0; i < geom.pages; i++) {
        if (!(slots[i >> 3] & (u8)(1u << (i & 7)))) {
            slots[i >> 3] |= (u8)(1u << (i & 7));
            return i;
        }
    }
    return (u32)-1;
}

static void slot_free(u32 s)
{
    slots[s >> 3] &= (u8)~(1u << (s & 7));
}

static swap_ent_t *ent_find(task_t *t, u64 vaddr)
{
    u64 key = vaddr >> 12;
    for (u16 i = 0; i < t->swap_count; i++)
        if (t->swap_map[i].vkey == key)
            return &t->swap_map[i];
    return NULL;
}

int swap_has(task_t *t, u64 vaddr)
{
    return ent_find(t, vaddr) != NULL;
}

int swap_out(task_t *t, u64 vaddr)
{
    if (!active || !t->user || !t->cr3)
        return -1;
    if (ent_find(t, vaddr))
        return -1;                       /* already swapped */
    if (t->swap_count >= SWAP_MAX_PER_TASK)
        return -1;
    u64 frame = vmm_translate(t->cr3, vaddr);
    if (!frame)
        return -1;                       /* not mapped */
    u32 slot = slot_alloc();
    if (slot == (u32)-1)
        return -1;                       /* swap region full */
    if (ide_write_sectors((u32)slot_lba(slot), SWAP_PAGE_SECTORS,
                          (void *)frame) != 0) {
        slot_free(slot);
        return -1;
    }
    vmm_unmap(t->cr3, vaddr);
    pmm_free_frame(frame);
    if (t == sched_current())
        __asm__ volatile("invlpg (%0)" ::"r"(vaddr));
    t->swap_map[t->swap_count].vkey = vaddr >> 12;
    t->swap_map[t->swap_count].slot = slot;
    t->swap_count++;
    swaps_out++;
    return 0;
}

int swap_in(task_t *t, u64 vaddr)
{
    if (!active || !t->user || !t->cr3)
        return -1;
    swap_ent_t *e = ent_find(t, vaddr);
    if (!e)
        return -1;                       /* not swapped out */
    u64 frame = pmm_alloc_frame();
    if (!frame)
        return -1;
    if (ide_read_sectors((u32)slot_lba(e->slot), SWAP_PAGE_SECTORS,
                         (void *)frame) != 0) {
        pmm_free_frame(frame);
        return -1;
    }
    vmm_map(t->cr3, vaddr, frame, VMM_USER | VMM_WRITE);
    slot_free(e->slot);
    t->swap_map[e - t->swap_map] = t->swap_map[--t->swap_count];
    swaps_in++;
    return 0;
}

void swap_exit_task(task_t *t)
{
    if (!active)
        return;
    for (u16 i = 0; i < t->swap_count; i++)
        slot_free(t->swap_map[i].slot);
    t->swap_count = 0;
}

u32 swap_slots_used(void)
{
    u32 n = 0;
    if (!active)
        return 0;
    for (u32 i = 0; i < geom.pages; i++)
        if (slots[i >> 3] & (u8)(1u << (i & 7)))
            n++;
    return n;
}

void swap_prefetch(task_t *t)
{
    if (!active || !t->user || !t->cr3)
        return;
    u64 p = pgpred_predict();
    if (!p)
        return;
    u64 vaddr = p << 12;
    if (vaddr < USER_IMAGE_BASE || vaddr >= USER_REGION_END)
        return;
    if (!swap_has(t, vaddr))
        return;
    if (swap_in(t, vaddr) == 0)
        printk("swap: prefetch 0x%llx\n", vaddr);
}

/* Pressure-driven eviction. Runs opportunistically from the idle poll path
   (kbd_readc) whenever the free-frame count drops below a low watermark
   derived from total RAM (4%). Evicts the mapped user data pages (script
   scratch, stack, heap) of suspended user tasks; each is transparently
   swapped back in on its next access, so nothing is ever lost. */
static void reclaim_task(task_t *t)
{
    u64 total = pmm_total_frames();
    u64 low = total / 25; /* ~4% low watermark, dynamic with RAM */
    if (pmm_avail_frames() > low)
        return;
    if (!t->user || !t->cr3)
        return;

    /* Script scratch (32 KiB) */
    for (usize i = 0; i < 8; i++) {
        if (swap_out(t, USER_SCRIPT_BASE + i * 4096) == 0) {
            reclaims++;
            if (pmm_avail_frames() > low)
                return;
        }
    }
    /* User stack (64 KiB) */
    for (usize i = 0; i < 16; i++) {
        if (swap_out(t, USER_STACK_BASE + i * 4096) == 0) {
            reclaims++;
            if (pmm_avail_frames() > low)
                return;
        }
    }
    /* User heap */
    for (usize i = 0; i < t->heap_pages; i++) {
        if (swap_out(t, USER_HEAP_BASE + i * 4096) == 0) {
            reclaims++;
            if (pmm_avail_frames() > low)
                return;
        }
    }
}

void swap_reclaim(void)
{
    if (!active)
        return;
    u64 total = pmm_total_frames();
    u64 low = total / 25;
    if (pmm_avail_frames() > low)
        return;
    sched_foreach_user(reclaim_task);
}

void swap_print_stats(void)
{
    printk("swap: %u/%u slots, %u out, %u in, %u reclaim\n",
           (unsigned)swap_slots_used(), (unsigned)geom.pages,
           (unsigned)swaps_out, (unsigned)swaps_in, (unsigned)reclaims);
}
