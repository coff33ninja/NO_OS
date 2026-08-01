#ifndef NOOS_SWAP_H
#define NOOS_SWAP_H

#include "types.h"
#include "sched.h"

/* M5: kernel-managed swap. User pages can be evicted to a reserved tail of
   the disk and are transparently swapped back in on the next page fault.

   Sizing is fully dynamic: swap_geometry() derives the region from the real
   disk capacity (ide_drive_sectors()), reserving 1/4 of the disk, so the same
   binary serves the 32 MiB test image and a multi-GiB baremetal disk with no
   hardcoded sizes. The FS (noosfs.c) sizes its data area to everything below
   the swap tail, so the two never overlap. */

struct swap_geom {
    u64 start;  /* first LBA of the swap region */
    u64 blocks; /* region size in 512-byte blocks */
    u64 pages;  /* number of 4 KiB slots */
};

void swap_geometry(u64 disk_blocks, struct swap_geom *g);

int  swap_init(void);                     /* derive geometry, alloc slot map */
int  swap_out(task_t *t, u64 vaddr);      /* 0 = ok, -1 = refused */
int  swap_in(task_t *t, u64 vaddr);       /* 0 = ok, -1 = not swapped */
int  swap_has(task_t *t, u64 vaddr);      /* 1 = page currently swapped out */
void swap_exit_task(task_t *t);           /* free all slots owned by a task */
u32  swap_slots_used(void);
void swap_reclaim(void);                  /* pressure-driven eviction */
void swap_print_stats(void);

/* Prefetch action, called by the page-fault dispatcher after a swap-in:
   ask the page-access predictor which page it expects next and pre-load it
   if it is currently swapped out for this task. */
void swap_prefetch(task_t *t);

#endif
