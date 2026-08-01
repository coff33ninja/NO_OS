#include "heap.h"
#include "pmm.h"
#include "string.h"
#include "printk.h"

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

static void heap_validate(const char *after)
{
    usize steps = 0;
    /* invariant: boot-time builtin struct at 0x152000 must stay 720/allocated */
    block *fmt = (block *)0x152000;
    if (fmt->magic == HDR_MAGIC &&
        (fmt->size != 720 || fmt->free != false)) {
        printk("heap: BUILTIN-STRUCT CHANGED after %s: size=%u free=%d "
               "next=%p\n", after, (u32)fmt->size, fmt->free,
               (void *)fmt->next);
    }
    for (block *p = head; p; p = p->next) {
        if (p->next && (u8 *)p->next < (u8 *)p) {
            printk("heap: LIST BACKWARD after %s, block=%p next=%p\n",
                   after, p, (void *)p->next);
            u8 *raw = (u8 *)p;
            printk("  fields size=%u magic=%x free=%d\n", (u32)p->size,
                   (u32)p->magic, p->free);
            for (int i = 0; i < 96; i += 8)
                printk("  mem %p: %x %x %x %x  %x %x %x %x\n", raw + i,
                       raw[i], raw[i + 1], raw[i + 2], raw[i + 3],
                       raw[i + 4], raw[i + 5], raw[i + 6], raw[i + 7]);
            return;
        }
        if (++steps > 200) {
            printk("heap: LIST CORRUPT (cycle) after %s, head=%p\n",
                   after, (void *)head);
            steps = 0;
            for (block *q = head; q && steps < 60; q = q->next, steps++)
                printk("  %p size=%u free=%d next=%p\n", q,
                       (u32)q->size, q->free, (void *)q->next);
            return;
        }
    }
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
        heap_validate("kmalloc");
        printk("heap: kmalloc %u -> %p\n", (u32)size, (void *)(b + 1));
        return (void *)(b + 1);
    }

    block *nb = region_alloc(size);
    if (!nb)
        return NULL;
    void *r = kmalloc(size);
    heap_validate("kmalloc-return");
    return r;
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
    printk("heap: kfree %p size=%u next=%p nextfree=%d\n", ptr,
           (u32)b->size, (void *)b->next, b->next ? b->next->free : -1);
    /* merge with the next block if adjacent and free */
    if (b->next && b->next->free && adjacent(b, b->next)) {
        b->size += sizeof(block) + b->next->size;
        b->next = b->next->next;
        block_count--;
        heap_validate("kfree-merge-next");
    }

    /* merge with the previous block: find it via linear scan */
    {
        u64 guard = 0;
        void *ring[64];
        usize ri = 0;
        for (block *p = head; p && p != b; p = p->next) {
            ring[ri++ % 64] = p;
            if (++guard > 100000) {
                printk("heap: cycle detected freeing %p; trail:\n", ptr);
                for (usize k = 0; k < 64; k++) {
                    block *q = ring[(ri + k) % 64];
                    printk("  [%d] %p size=%u free=%d next=%p\n", (int)k, q,
                           (u32)q->size, q->free, (void *)q->next);
                }
                break;
            }
            if (p->next == b && p->free && adjacent(p, b)) {
                p->size += sizeof(block) + b->size;
                p->next = b->next;
                block_count--;
                heap_validate("kfree-merge-prev");
                break;
            }
        }
    }
    heap_validate("kfree");
}

usize heap_used_bytes(void) { return used_bytes; }
usize heap_blocks(void)     { return block_count; }
