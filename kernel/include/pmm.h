#ifndef NOOS_PMM_H
#define NOOS_PMM_H

#include "types.h"

#define FRAME_SIZE 4096

void pmm_init(u32 mb_info);

u64 pmm_alloc_frame(void);
u64 pmm_alloc_frames(usize count);
void pmm_free_frame(u64 addr);
void pmm_free_frames(u64 addr, usize count);

u64 pmm_total_frames(void);
u64 pmm_avail_frames(void);

#endif
