#include "fs.h"
#include "ide.h"
#include "heap.h"
#include "printk.h"
#include "string.h"

#define FS_INODE_DIR 0x4000
#define FS_INODE_REG 0x8000

#define INODES_PER_BLOCK (FS_BLOCK_SIZE / sizeof(struct fs_inode))

static struct fs_sb sb;
static u8 *inode_bitmap;   /* 1 bit per inode, in RAM */
static u8 *cluster_bitmap; /* 1 bit per cluster, in RAM */
static bool mounted;

/* ---------------- low-level block I/O ---------------- */

static int disk_read_block(u64 lba, void *buf)
{
    return ide_read_sectors((u32)lba, 1, buf);
}

static int disk_write_block(u64 lba, const void *buf)
{
    return ide_write_sectors((u32)lba, 1, buf);
}

/* ---------------- inode table I/O ---------------- */

static u64 inode_lba(u32 ino)
{
    return sb.inode_start + (u64)ino / INODES_PER_BLOCK;
}

static int read_inode(u32 ino, struct fs_inode *in)
{
    u8 block[FS_BLOCK_SIZE];
    if (disk_read_block(inode_lba(ino), block) != 0)
        return -1;
    memcpy(in, block + (ino % INODES_PER_BLOCK) * sizeof(struct fs_inode),
           sizeof(struct fs_inode));
    return 0;
}

static int write_inode(u32 ino, const struct fs_inode *in)
{
    u8 block[FS_BLOCK_SIZE];
    if (disk_read_block(inode_lba(ino), block) != 0)
        return -1;
    memcpy(block + (ino % INODES_PER_BLOCK) * sizeof(struct fs_inode),
           in, sizeof(struct fs_inode));
    return disk_write_block(inode_lba(ino), block);
}

/* ---------------- bitmap helpers ---------------- */

static bool bit_get(const u8 *bitmap, u32 i)
{
    return (bitmap[i >> 3] >> (i & 7)) & 1;
}

static void bit_set(u8 *bitmap, u32 i)
{
    bitmap[i >> 3] |= (u8)(1u << (i & 7));
}

static void bit_clr(u8 *bitmap, u32 i)
{
    bitmap[i >> 3] &= (u8)~(1u << (i & 7));
}

u32 fs_alloc_cluster(void)
{
    if (!mounted)
        return (u32)-1;
    for (u32 c = 1; c < sb.block_count; c++) {
        if (!bit_get(cluster_bitmap, c)) {
            bit_set(cluster_bitmap, c);
            return c;
        }
    }
    return (u32)-1;
}

void fs_free_cluster(u32 cluster)
{
    if (cluster < sb.block_count)
        bit_clr(cluster_bitmap, cluster);
}

u64 fs_free_bytes(void)
{
    u64 n = 0;
    if (!mounted)
        return 0;
    for (u32 c = 0; c < sb.block_count; c++)
        if (!bit_get(cluster_bitmap, c))
            n++;
    return n * FS_BLOCK_SIZE;
}

static u32 fs_alloc_inode(void)
{
    for (u32 i = 1; i < sb.inode_count; i++) {
        if (!bit_get(inode_bitmap, i)) {
            bit_set(inode_bitmap, i);
            return i;
        }
    }
    return (u32)-1;
}

/* ---------------- superblock ---------------- */

static bool sb_valid(const struct fs_sb *s)
{
    return memcmp(s->sig, NOOSFS_SIG, 8) == 0 &&
           s->block_size == FS_BLOCK_SIZE;
}

static void sb_write_all(void)
{
    sb.seq++;
    disk_write_block(1, &sb); /* secondary first */
    disk_write_block(0, &sb); /* primary second  */
}

static int read_any_sb(void)
{
    struct fs_sb sb0, sb1;
    if (disk_read_block(0, &sb0) != 0)
        return -1;
    if (disk_read_block(1, &sb1) != 0)
        return -1;
    bool v0 = sb_valid(&sb0);
    bool v1 = sb_valid(&sb1);
    if (v0 && v1)
        sb = (sb0.seq >= sb1.seq) ? sb0 : sb1;
    else if (v0)
        sb = sb0;
    else if (v1)
        sb = sb1;
    else
        return -1;
    return 0;
}

/* ---------------- format ---------------- */

static u32 disk_total_blocks(void)
{
    /* fixed 32 MiB raw image for now */
    return 32 * 1024 * 1024 / FS_BLOCK_SIZE;
}

int fs_format(void)
{
    u32 total = disk_total_blocks();
    u64 bitmap_blocks = (total + 8 * 512 - 1) / (8 * 512);
    u64 inode_blocks = (FS_INODE_COUNT * sizeof(struct fs_inode) + FS_BLOCK_SIZE - 1) / FS_BLOCK_SIZE;
    u64 data_start = 2 + bitmap_blocks + inode_blocks;
    u64 nblocks = total - data_start;

    /* recompute against final cluster count (stable for 32 MiB) */
    bitmap_blocks = (nblocks + 8 * 512 - 1) / (8 * 512);
    data_start = 2 + bitmap_blocks + inode_blocks;
    nblocks = total - data_start;

    memset(&sb, 0, sizeof(sb));
    memcpy(sb.sig, NOOSFS_SIG, 8);
    sb.version = 1;
    sb.block_size = FS_BLOCK_SIZE;
    sb.block_count = nblocks;
    sb.bitmap_start = 2;
    sb.bitmap_blocks = bitmap_blocks;
    sb.inode_start = 2 + bitmap_blocks;
    sb.inode_count = FS_INODE_COUNT;
    sb.data_start = data_start;
    sb.root_inode = 0;
    sb.seq = 1;
    sb.flags = 0;

    /* allocation bitmap: all free */
    u8 *bitmap = kmalloc(bitmap_blocks * FS_BLOCK_SIZE);
    if (!bitmap)
        return -1;
    memset(bitmap, 0, bitmap_blocks * FS_BLOCK_SIZE);
    for (u64 b = 0; b < bitmap_blocks; b++)
        disk_write_block(sb.bitmap_start + b, bitmap + b * FS_BLOCK_SIZE);
    kfree(bitmap);

    /* inode table: zeroed */
    u8 *zeros = kmalloc(inode_blocks * FS_BLOCK_SIZE);
    if (!zeros)
        return -1;
    memset(zeros, 0, inode_blocks * FS_BLOCK_SIZE);
    for (u64 b = 0; b < inode_blocks; b++)
        disk_write_block(sb.inode_start + b, zeros + b * FS_BLOCK_SIZE);
    kfree(zeros);

    /* root directory: inode 0 */
    struct fs_inode root;
    memset(&root, 0, sizeof(root));
    root.mode = FS_INODE_DIR;
    root.nlinks = 2;
    write_inode(0, &root);

    sb_write_all();
    return 0;
}

/* ---------------- mount ---------------- */

int fs_mount(void)
{
    if (read_any_sb() != 0)
        return -1;

    cluster_bitmap = kmalloc(sb.bitmap_blocks * FS_BLOCK_SIZE);
    inode_bitmap = kmalloc((FS_INODE_COUNT + 7) / 8);
    if (!cluster_bitmap || !inode_bitmap) {
        kfree(cluster_bitmap);
        kfree(inode_bitmap);
        cluster_bitmap = NULL;
        inode_bitmap = NULL;
        return -1;
    }
    for (u64 b = 0; b < sb.bitmap_blocks; b++)
        disk_read_block(sb.bitmap_start + b, cluster_bitmap + b * FS_BLOCK_SIZE);

    memset(inode_bitmap, 0, (FS_INODE_COUNT + 7) / 8);
    bit_set(inode_bitmap, sb.root_inode);

    mounted = true;
    return 0;
}

bool fs_mounted(void)
{
    return mounted;
}

/* ---------------- data-area access ---------------- */

int fs_cluster_write(u32 cluster, const void *buf)
{
    if (cluster >= sb.block_count)
        return -1;
    return disk_write_block(sb.data_start + cluster, buf);
}

int fs_cluster_read(u32 cluster, void *buf)
{
    if (cluster >= sb.block_count)
        return -1;
    return disk_read_block(sb.data_start + cluster, buf);
}

void fs_sync_bitmap(void)
{
    if (!mounted)
        return;
    for (u64 b = 0; b < sb.bitmap_blocks; b++)
        disk_write_block(sb.bitmap_start + b, cluster_bitmap + b * FS_BLOCK_SIZE);
}

/* ---------------- init ---------------- */

int fs_init(void)
{
    if (!ide_present())
        return -1;
    if (read_any_sb() != 0) {
        printk("fs: no filesystem found, formatting\n");
        if (fs_format() != 0) {
            printk("fs: format FAILED\n");
            return -1;
        }
    }
    if (fs_mount() != 0) {
        printk("fs: mount FAILED\n");
        return -1;
    }
    printk("fs: mounted (%u clusters, %u inodes), %u free blocks\n",
           (unsigned)sb.block_count, (unsigned)sb.inode_count,
           (unsigned)(fs_free_bytes() / FS_BLOCK_SIZE));
    return 0;
}
