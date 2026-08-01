#ifndef NOOS_VMM_H
#define NOOS_VMM_H

#include "types.h"

#define VMM_PRESENT 1
#define VMM_WRITE   2
#define VMM_USER    4

/* User region lives in PML4[2] (1 TiB), NOT under PML4[0] which is the
   kernel's supervisor-only identity map. PML4[0] is inherited by every
   process so the kernel stays reachable; the user hierarchy is fresh. */
#define USER_IMAGE_BASE   0x0000010000000000UL
#define USER_SCRIPT_BASE  0x00000100C0000000UL
#define USER_HEAP_BASE    0x00000100D0000000UL
#define USER_STACK_BASE   0x00000100F0000000UL
#define USER_STACK_TOP    (USER_STACK_BASE + 0x10000UL) /* 64 KiB region */
/* Demand-paged read-only model weights: 64 KiB (16 pages) just above the
   user stack. Not mapped at spawn; each page is faulted in read-only on
   first access, charged against the task's model RAM budget, and evicted
   explicitly (the canonical weight copy makes re-faulting always safe). */
#define USER_MODEL_BASE   0x00000100F1000000UL
#define USER_MODEL_END   (USER_MODEL_BASE + 0x10000UL)
#define USER_MODEL_PAGES  16
#define USER_REGION_END   0x0000010100000000UL

u64  vmm_kernel_cr3(void);
u64  vmm_new_address_space(void);
void vmm_map(u64 cr3, u64 vaddr, u64 paddr, u8 flags);
void vmm_unmap(u64 cr3, u64 vaddr);
u64  vmm_alloc_user_pages(u64 cr3, usize pages, u64 vaddr);
void vmm_free_address_space(u64 cr3);
void vmm_load_cr3(u64 cr3);

#endif
