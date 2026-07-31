#include "vmm.h"
#include "pmm.h"
#include "types.h"

extern char pml4[]; /* boot PML4 in .bss (boot.s) */

u64 vmm_kernel_cr3(void)
{
    return (u64)pml4;
}

void vmm_load_cr3(u64 cr3)
{
    __asm__ volatile("mov %0, %%cr3" ::"r"(cr3) : "memory");
}

static u64 pt_new(u64 *table, usize idx, u8 flags)
{
    u64 frame = pmm_alloc_frame();
    if (!frame)
        return 0;
    for (usize i = 0; i < 512; i++)
        ((u64 *)frame)[i] = 0;
    table[idx] = frame | flags;
    return frame;
}

/* Allocate a fresh top-level page table. Only the kernel identity map
   (PML4[0], supervisor) is inherited from the boot table, so every new
   address space can reach kernel memory but gets its own user region. */
u64 vmm_new_address_space(void)
{
    u64 frame = pmm_alloc_frame();
    if (!frame)
        return 0;
    u64 *pml4t = (u64 *)frame;
    for (usize i = 0; i < 512; i++)
        pml4t[i] = 0;
    pml4t[0] = ((u64 *)pml4)[0];
    return frame;
}

void vmm_map(u64 cr3, u64 vaddr, u64 paddr, u8 flags)
{
    u64 *pml4t = (u64 *)cr3;
    u64 *pdpt, *pd, *pt;
    usize pml4i = (vaddr >> 39) & 0x1FF;
    usize pdpti = (vaddr >> 30) & 0x1FF;
    usize pdi   = (vaddr >> 21) & 0x1FF;
    usize pti   = (vaddr >> 12) & 0x1FF;
    u64  entry_flags = 0x1 | (flags & 0x6); /* P + W/U bits */
    /* A user-mode access must be permitted at every level of the walk. */
    u8   ient = 0x3 | (flags & VMM_USER ? VMM_USER : 0);

    if (!(pml4t[pml4i] & 1)) {
        if (!pt_new(pml4t, pml4i, ient))
            return;
    }
    pdpt = (u64 *)(pml4t[pml4i] & ~0xFFFUL);
    if (!(pdpt[pdpti] & 1)) {
        if (!pt_new(pdpt, pdpti, ient))
            return;
    }
    pd = (u64 *)(pdpt[pdpti] & ~0xFFFUL);
    if (!(pd[pdi] & 1)) {
        if (!pt_new(pd, pdi, ient))
            return;
    }
    pt = (u64 *)(pd[pdi] & ~0xFFFUL);
    pt[pti] = (paddr & ~0xFFFUL) | entry_flags;
}

void vmm_unmap(u64 cr3, u64 vaddr)
{
    u64 *pml4t = (u64 *)cr3;
    u64 *pdpt, *pd, *pt;
    usize pml4i = (vaddr >> 39) & 0x1FF;
    usize pdpti = (vaddr >> 30) & 0x1FF;
    usize pdi   = (vaddr >> 21) & 0x1FF;
    usize pti   = (vaddr >> 12) & 0x1FF;

    if (!(pml4t[pml4i] & 1))
        return;
    pdpt = (u64 *)(pml4t[pml4i] & ~0xFFFUL);
    if (!(pdpt[pdpti] & 1))
        return;
    pd = (u64 *)(pdpt[pdpti] & ~0xFFFUL);
    if (!(pd[pdi] & 1))
        return;
    pt = (u64 *)(pd[pdi] & ~0xFFFUL);
    pt[pti] = 0;
}

u64 vmm_alloc_user_pages(u64 cr3, usize pages, u64 vaddr)
{
    for (usize i = 0; i < pages; i++) {
        u64 frame = pmm_alloc_frame();
        if (!frame)
            return 0;
        vmm_map(cr3, vaddr + i * 4096, frame, VMM_USER | VMM_WRITE);
    }
    return vaddr;
}

/* Walk the user tree (PML4[2]) and free every mapped frame and page table. */
void vmm_free_address_space(u64 cr3)
{
    u64 *pml4t = (u64 *)cr3;
    u64 *pdpt, *pd, *pt;

    if (!(pml4t[2] & 1))
        return;
    pdpt = (u64 *)(pml4t[2] & ~0xFFFUL);
    for (usize pdpti = 0; pdpti < 512; pdpti++) {
        if (!(pdpt[pdpti] & 1))
            continue;
        pd = (u64 *)(pdpt[pdpti] & ~0xFFFUL);
        for (usize pdi = 0; pdi < 512; pdi++) {
            if (!(pd[pdi] & 1))
                continue;
            pt = (u64 *)(pd[pdi] & ~0xFFFUL);
            for (usize pti = 0; pti < 512; pti++) {
                if (pt[pti] & 1)
                    pmm_free_frame(pt[pti] & ~0xFFFUL);
            }
            pmm_free_frame((u64)pt);
        }
        pmm_free_frame((u64)pd);
    }
    pmm_free_frame((u64)pdpt);
    pmm_free_frame((u64)pml4t);
}
