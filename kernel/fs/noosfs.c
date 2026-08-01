#include "fs.h"
#include "ide.h"
#include "heap.h"
#include "pit.h"
#include "printk.h"
#include "string.h"
#include "swap.h"

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
           s->block_size == FS_BLOCK_SIZE &&
           s->version == NOOSFS_VERSION;
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

static u64 disk_total_blocks(void)
{
    u64 sectors = ide_drive_sectors();
    if (!sectors) {
        /* Geometry unknown (IDENTIFY returned no payload): fall back to the
           probe default so a disk-less/unknown boot still formats. */
        sectors = 32 * 1024 * 1024 / SECTOR_SIZE;
    }
    return sectors;
}

/* Blocks available to the filesystem = everything below the swap tail. The
   swap region's size is derived from the same real disk capacity, so the two
   always partition the disk with no hardcoded sizes. */
static u64 fs_usable_blocks(void)
{
    struct swap_geom g;
    swap_geometry(disk_total_blocks(), &g);
    return g.start;
}

int fs_format(void)
{
    u64 total = fs_usable_blocks();
    u64 bitmap_blocks = (total + 8 * 512 - 1) / (8 * 512);
    u64 inode_blocks = (FS_INODE_COUNT * sizeof(struct fs_inode) + FS_BLOCK_SIZE - 1) / FS_BLOCK_SIZE;
    u64 data_start = 2 + bitmap_blocks + inode_blocks;
    u64 nblocks = total - data_start;

    /* Recompute against the final cluster count (bitmap sizing is exact
       only after data_start is known; the loop converges in one pass). */
    bitmap_blocks = (nblocks + 8 * 512 - 1) / (8 * 512);
    data_start = 2 + bitmap_blocks + inode_blocks;
    nblocks = total - data_start;

    memset(&sb, 0, sizeof(sb));
    memcpy(sb.sig, NOOSFS_SIG, 8);
    sb.version = NOOSFS_VERSION;
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

    kfree(cluster_bitmap);
    kfree(inode_bitmap);
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

/* ---------------- directory helpers ---------------- */

#define DIRENT_PER_BLOCK (FS_BLOCK_SIZE / sizeof(struct fs_dirent))

static struct fs_inode root_cache;

static int root_load(void)
{
    return read_inode(sb.root_inode, &root_cache);
}

static int root_save(void)
{
    return write_inode(sb.root_inode, &root_cache);
}

/* Ensure the root directory has a data cluster to store entries in. */
static int root_ensure_block(void)
{
    if (root_cache.size > 0)
        return 0;
    u32 c = fs_alloc_cluster();
    if (c == (u32)-1)
        return -1;
    u8 zero[FS_BLOCK_SIZE];
    memset(zero, 0, sizeof(zero));
    if (fs_cluster_write(c, zero) != 0)
        return -1;
    root_cache.blocks[0] = c;
    root_cache.size = FS_BLOCK_SIZE;
    return root_save();
}

static int dir_add_entry(const char *name, u32 ino, u8 type)
{
    if (root_load() != 0)
        return -1;
    usize nlen = strlen(name);
    if (nlen == 0 || nlen > FS_NAME_MAX)
        return -1;
    if (root_ensure_block() != 0)
        return -1;

    for (u32 bi = 0; bi < FS_MAX_BLOCKS; bi++) {
        if (root_cache.blocks[bi] == 0)
            break;
        struct fs_dirent blk[DIRENT_PER_BLOCK];
        if (fs_cluster_read(root_cache.blocks[bi], blk) != 0)
            return -1;
        for (u32 e = 0; e < DIRENT_PER_BLOCK; e++) {
            if (blk[e].inode == 0) {
                memset(&blk[e], 0, sizeof(blk[e]));
                blk[e].inode = ino;
                blk[e].reclen = sizeof(struct fs_dirent);
                blk[e].namelen = (u8)nlen;
                blk[e].type = type;
                memcpy(blk[e].name, name, nlen);
                return fs_cluster_write(root_cache.blocks[bi], blk);
            }
        }
        if (root_cache.blocks[bi] == 0) {
            /* extend into the next free direct slot */
            u32 c = fs_alloc_cluster();
            if (c == (u32)-1)
                return -1;
            u8 zero[FS_BLOCK_SIZE];
            memset(zero, 0, sizeof(zero));
            if (fs_cluster_write(c, zero) != 0)
                return -1;
            root_cache.blocks[bi] = c;
            root_cache.size += FS_BLOCK_SIZE;
            return root_save();
        }
    }
    return -1; /* directory full */
}

/* ---------------- file operations ---------------- */

int fs_create(const char *name, u16 mode)
{
    if (!mounted)
        return -1;
    if (fs_lookup(name) >= 0)
        return -1; /* exists */
    u32 ino = fs_alloc_inode();
    if (ino == (u32)-1)
        return -1;
    struct fs_inode in;
    memset(&in, 0, sizeof(in));
    in.mode = mode;
    in.nlinks = 1;
    in.mtime = (u32)pit_ticks();
    if (write_inode(ino, &in) != 0)
        return -1;
    if (dir_add_entry(name, ino, mode == FS_INODE_DIR ? DT_DIR : DT_FILE) != 0)
        return -1;
    return (int)ino;
}

int fs_lookup(const char *name)
{
    if (!mounted)
        return -1;
    if (root_load() != 0)
        return -1;
    usize nlen = strlen(name);
    for (u32 bi = 0; bi < FS_MAX_BLOCKS; bi++) {
        if (root_cache.blocks[bi] == 0)
            break;
        struct fs_dirent blk[DIRENT_PER_BLOCK];
        if (fs_cluster_read(root_cache.blocks[bi], blk) != 0)
            return -1;
        for (u32 e = 0; e < DIRENT_PER_BLOCK; e++) {
            if (blk[e].inode != 0 && blk[e].namelen == nlen &&
                memcmp(blk[e].name, name, nlen) == 0)
                return (int)blk[e].inode;
        }
    }
    return -1;
}

int fs_write_file(u32 ino, const void *data, u64 size)
{
    if (!mounted)
        return -1;
    u64 nblocks = (size + FS_BLOCK_SIZE - 1) / FS_BLOCK_SIZE;
    if (nblocks > FS_MAX_BLOCKS)
        return -1;
    struct fs_inode in;
    if (read_inode(ino, &in) != 0)
        return -1;

    /* free old data blocks beyond the new length */
    for (u32 i = (u32)nblocks; i < FS_MAX_BLOCKS; i++) {
        if (in.blocks[i]) {
            fs_free_cluster(in.blocks[i]);
            in.blocks[i] = 0;
        }
    }
    /* allocate and write new data */
    const u8 *p = (const u8 *)data;
    for (u32 i = 0; i < nblocks; i++) {
        if (!in.blocks[i]) {
            u32 c = fs_alloc_cluster();
            if (c == (u32)-1)
                return -1;
            in.blocks[i] = c;
        }
        u8 tmp[FS_BLOCK_SIZE];
        u64 off = (u64)i * FS_BLOCK_SIZE;
        u64 chunk = size - off < FS_BLOCK_SIZE ? size - off : FS_BLOCK_SIZE;
        memcpy(tmp, p + off, chunk);
        if (chunk < FS_BLOCK_SIZE)
            memset(tmp + chunk, 0, FS_BLOCK_SIZE - chunk);
        if (fs_cluster_write(in.blocks[i], tmp) != 0)
            return -1;
    }
    in.size = size;
    in.mtime = (u32)pit_ticks();
    in.nlinks = 1;
    in.mode |= FS_INODE_REG;
    return write_inode(ino, &in);
}

u64 fs_read_file(u32 ino, void *buf, u64 max)
{
    struct fs_inode in;
    if (read_inode(ino, &in) != 0)
        return 0;
    u64 want = in.size < max ? in.size : max;
    u8 *p = (u8 *)buf;
    u64 left = want;
    u8 tmp[FS_BLOCK_SIZE];
    for (u32 i = 0; left && i < FS_MAX_BLOCKS; i++) {
        if (!in.blocks[i])
            break;
        u64 chunk = left < FS_BLOCK_SIZE ? left : FS_BLOCK_SIZE;
        if (fs_cluster_read(in.blocks[i], tmp) != 0)
            return (u64)(p - (u8 *)buf);
        memcpy(p, tmp, chunk);
        p += chunk;
        left -= chunk;
    }
    return want;
}

int fs_stat(u32 ino, struct fs_inode *out)
{
    return read_inode(ino, out);
}

static int dir_find_slot(const char *name, u32 *block_out, u32 *ent_out)
{
    usize nlen = strlen(name);
    for (u32 bi = 0; bi < FS_MAX_BLOCKS; bi++) {
        if (root_cache.blocks[bi] == 0)
            break;
        struct fs_dirent blk[DIRENT_PER_BLOCK];
        if (fs_cluster_read(root_cache.blocks[bi], blk) != 0)
            return -1;
        for (u32 e = 0; e < DIRENT_PER_BLOCK; e++) {
            if (blk[e].inode != 0 && blk[e].namelen == nlen &&
                memcmp(blk[e].name, name, nlen) == 0) {
                *block_out = bi;
                *ent_out = e;
                return 0;
            }
        }
    }
    return -1;
}

int fs_unlink(const char *name)
{
    if (!mounted)
        return -1;
    u32 ino = (u32)fs_lookup(name);
    if (ino == (u32)-1)
        return -1;
    if (root_load() != 0)
        return -1;
    u32 bi, e;
    if (dir_find_slot(name, &bi, &e) != 0)
        return -1;

    struct fs_inode in;
    if (read_inode(ino, &in) == 0) {
        for (u32 i = 0; i < FS_MAX_BLOCKS; i++)
            if (in.blocks[i])
                fs_free_cluster(in.blocks[i]);
    }
    memset(&in, 0, sizeof(in));
    write_inode(ino, &in);
    bit_clr(inode_bitmap, ino);

    struct fs_dirent blk[DIRENT_PER_BLOCK];
    fs_cluster_read(root_cache.blocks[bi], blk);
    memset(&blk[e], 0, sizeof(blk[e]));
    fs_cluster_write(root_cache.blocks[bi], blk);
    return 0;
}

int fs_listdir(void)
{
    if (!mounted)
        return -1;
    if (root_load() != 0)
        return -1;
    printk("dir /:\n");
    u32 count = 0;
    for (u32 bi = 0; bi < FS_MAX_BLOCKS; bi++) {
        if (root_cache.blocks[bi] == 0)
            break;
        struct fs_dirent blk[DIRENT_PER_BLOCK];
        if (fs_cluster_read(root_cache.blocks[bi], blk) != 0)
            continue;
        for (u32 e = 0; e < DIRENT_PER_BLOCK; e++) {
            if (blk[e].inode == 0)
                continue;
            struct fs_inode in;
            if (read_inode(blk[e].inode, &in) != 0)
                continue;
            char nm[FS_NAME_MAX + 1];
            memcpy(nm, blk[e].name, blk[e].namelen);
            nm[blk[e].namelen] = '\0';
            printk("  %s  %u bytes\n", nm, (unsigned)in.size);
            count++;
        }
    }
    printk("  (%u entries)\n", count);
    return count;
}

/* ---------------- init ---------------- */

int fs_init(void)
{
    if (!ide_present())
        return -1;
    /* Format when there is no filesystem, or when the on-disk data area
       spills into the swap tail (a disk formatted before swap reservation,
       or attached to a smaller/different drive). */
    if (read_any_sb() != 0 ||
        (u64)sb.data_start + sb.block_count > fs_usable_blocks()) {
        printk("fs: no filesystem or geometry mismatch, formatting\n");
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
