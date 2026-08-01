#ifndef NOOS_FS_H
#define NOOS_FS_H

#include "types.h"

#define NOOSFS_SIG "NO_OSFS\0"
#define NOOSFS_VERSION 2
#define FS_BLOCK_SIZE 512
#define FS_INODE_COUNT 256
#define FS_NAME_MAX 24
#define FS_MAX_BLOCKS 10 /* direct blocks per inode: 5 KiB max file */

#define DT_FILE 1
#define DT_DIR  2

#define FS_INODE_DIR 0x4000
#define FS_INODE_REG 0x8000

/* On-disk superblock (1 block). Two copies: LBA 0 and LBA 1. */
struct fs_sb {
    u8   sig[8];
    u32  version;
    u32  block_size;
    u64  block_count;    /* clusters in the data area */
    u64  bitmap_start;   /* LBA */
    u64  bitmap_blocks;
    u64  inode_start;    /* LBA */
    u64  inode_count;
    u64  data_start;     /* LBA of cluster 0 */
    u32  root_inode;
    u32  seq;            /* bumped on clean mount */
    u32  flags;          /* bit0 = dirty */
    u8   reserved[448];
};

/* On-disk inode (64 bytes). */
struct fs_inode {
    u16  mode;
    u16  nlinks;
    u64  size;
    u32  mtime;
    u32  flags;
    u32  blocks[FS_MAX_BLOCKS]; /* cluster numbers */
    u8   reserved[4];
};

/* Directory entry (32 bytes). */
struct fs_dirent {
    u32  inode;
    u16  reclen;
    u8   namelen;
    u8   type;
    char name[FS_NAME_MAX];
};

int  fs_init(void);      /* format-if-needed, then mount */
int  fs_format(void);    /* wipe + create root dir (inode 0) */
int  fs_mount(void);     /* read superblock, load bitmap */
u32  fs_alloc_cluster(void);
void fs_free_cluster(u32 cluster);
u64  fs_free_bytes(void);
bool fs_mounted(void);

/* raw data-area I/O used by the FS and self-tests */
int fs_cluster_write(u32 cluster, const void *buf);
int fs_cluster_read(u32 cluster, void *buf);
void fs_sync_bitmap(void);

/* file operations (flat root directory for now) */
int  fs_create(const char *name, u16 mode);                 /* -> inode or -1 */
int  fs_lookup(const char *name);                           /* -> inode or -1 */
int  fs_write_file(u32 ino, const void *data, u64 size);    /* truncate+write */
u64  fs_read_file(u32 ino, void *buf, u64 max);
int  fs_stat(u32 ino, struct fs_inode *out);
int  fs_unlink(const char *name);
int  fs_listdir(void);                                       /* prints via printk */

#endif
