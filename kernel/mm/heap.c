#include "heap.h"
#include "pmm.h"
#include "string.h"

#define HDR_MAGIC 0x4E4F4F534C4C4F43ULL /* NOOSLLOC */

typedef struct block {
    usize      size;  /* payload bytes, excluding header */
    u64        magic;
    bool       free;
    struct block *next;
} block;

static block *head;
static usize  used_bytes;
static usize  block_count;

static usize align16(usize n)
{
    return (n + 15) & ~(usize)15;
}

static bool adjacent(block *a, block *b)
{
    return (u8 *)b == (u8 *)(a + 1) + a->size;
}

static block *region_alloc(usize payload)
{
    usize need   = sizeof(block) + payload;
    usize frames = (need + FRAME_SIZE - 1) / FRAME_SIZE;
    u64 addr     = pmm_alloc_frames(frames);

    if (!addr)
        return NULL;

    block *b      = (block *)(usize)addr;
    b->size       = frames * FRAME_SIZE - sizeof(block);
    b->magic      = HDR_MAGIC;
    b->free       = true;
    b->next       = NULL;

    /* append to the free list */
    if (!head) {
        head = b;
    } else {
        block *t = head;
        while (t->next)
            t = t->next;
        t->next = b;
    }
    block_count++;
    return b;
}

void *kmalloc(usize size)
{
    if (size == 0)
        size = 1;
    size = align16(size);

    for (block *b = head; b; b = b->next) {
        if (!b->free || b->size < size)
            continue;

        /* split when the leftover is big enough for a new block */
        if (b->size - size >= sizeof(block) + 16) {
            block *nb      = (block *)((u8 *)(b + 1) + size);
            nb->size       = b->size - size - sizeof(block);
            nb->magic      = HDR_MAGIC;
            nb->free       = true;
            nb->next       = b->next;
            b->next        = nb;
            b->size        = size;
            block_count++;
        }

        b->free = false;
        used_bytes += b->size;
        return (void *)(b + 1);
    }

    block *nb = region_alloc(size);
    if (!nb)
        return NULL;
    return kmalloc(size);
}

void kfree(void *ptr)
{
    if (!ptr)
        return;

    block *b = (block *)ptr - 1;
    if (b->magic != HDR_MAGIC)
        return;
    if (b->free)
        return;

    b->free = true;
    used_bytes -= b->size;

    /* merge with the next block if adjacent and free */
    if (b->next && b->next->free && adjacent(b, b->next)) {
        b->size += sizeof(block) + b->next->size;
        b->next = b->next->next;
        block_count--;
    }

    /* merge with the previous block: find it via linear scan */
    for (block *p = head; p && p != b; p = p->next) {
        if (p->next == b && p->free && adjacent(p, b)) {
            p->size += sizeof(block) + b->size;
            p->next = b->next;
            block_count--;
            break;
        }
    }
}

usize heap_used_bytes(void) { return used_bytes; }
usize heap_blocks(void)     { return block_count; }
