# M4 — Filesystem

NO_OS M4 adds persistent storage capability, enabling the system to retain data across reboots. This milestone is crucial for M5's self-evolution capabilities, as it provides the storage needed for model weights, interaction logs, and the evolving NOC corpus.

## 1. Overview

M4 implements a simple, journaling-inspired filesystem inspired by TempleOS's RedSea, optimized for:
- **Simplicity**: Easy to implement, verify, and recover from corruption
- **Wear Leveling**: Important for flash-backed storage (QEMU IDE, future SD/NVMe)
- **Power Loss Resilience**: Minimal metadata updates per write
- **Low RAM Overhead**: Minimal buffering and caching requirements
- **Deterministic Performance**: Predictable latency for real-time characteristics

The filesystem stores:
- Model weights (M5)
- Interaction logs (M5)
- NOC corpus (M5)
- User scripts and data
- System configuration

## 2. Design Philosophy

### 2.1. RedSea Inspiration
Like TempleOS's RedSea, NO_OS's filesystem:
- Uses a simple linear layout with minimal metadata
- Stores files contiguously when possible
- Uses extensible extents for files that grow
- Maintains two copies of critical structures (superblock, allocation bitmap)
- Avoids complex data structures like B-trees or extent trees

### 2.2. Key Design Goals
| Goal | Implementation |
|------|----------------|
| **Simplicity** | Single contiguous allocation bitmap, linear directory scan |
| **Power Safety** | Journal-like metadata updates, dual superblocks |
| **Wear Leveling** | Circular allocation strategy, optional wear-leveling layer |
| **Low Overhead** | <5KiB kernel memory footprint for FS structures |
| **Predictability** | O(1) file open, O(n) directory scan (n = small) |
| **Portability** | Works with IDE, future SATA/NVMe, SD card |

## 3. Filesystem Layout

### 3.1. On-Disk Structure
```
[0]   Protective MBR (1 sector)        # Legacy BIOS compatibility
[1]   GPT Header (1 sector)            # Optional, for modern boot
[2]   Partition Entry Array (sectors)  # GPT partition table
[32]  Filesystem Start (configurable)  # Typically LBA 64
        ├── Superblock Copy 0          # 1 sector
        ├── Superblock Copy 1          # 1 sector (at offset 0x200)
        ├── Allocation Bitmap          # N sectors (1 bit per 4KiB cluster)
        ├── Inode Table                # M sectors (fixed-size entries)
        ├── Journal Area               # Optional write-ahead log
        └── Data Area                  # Remaining space for files
```

### 3.2. Cluster Size
- **Default**: 4096 bytes (4 KiB)
- **Min**: 512 bytes (1 sector)
- **Max**: 32768 bytes (32 KiB)
- Chosen at format time based on device size:
  - <128 MB: 512B clusters
  - 128MB-4GB: 4KiB clusters
  - >4GB: 16KiB or 32KiB clusters

### 3.3. Superblock (1 sector = 512 bytes)
```c
struct sb {
    u8  signature[8];      // "NO_OSFS\0"
    u64 version;           // 0x10000 = version 1.0
    u64 block_size;        // Usually 4096
    u64 block_count;       // Total blocks in filesystem
    u64 bitmap_start;      // LBA of allocation bitmap
    u64 bitmap_blocks;     // Number of bitmap blocks
    u64 inode_start;       // LBA of inode table
    u64 inode_count;       // Number of inodes
    u64 root_dir_inode;    // Inode number of root directory
    u64 volume_id;         // Random UUID
    u64 flags;             // Read-only, dirty, etc.
    u8  reserved[384];     // Future use, zeroed
};
```
- Two copies: primary at LBA start, secondary at LBA start + 0x200
- Updated atomically: write to secondary, flip active bit, write to primary

### 3.4. Allocation Bitmap
- 1 bit per cluster (4 KiB by default)
- 0 = free, 1 = allocated
- Stored as little-endian bits within bytes
- Example: 100 GiB disk with 4K clusters = 25M clusters = ~3.125 MB bitmap
- Located immediately after superblocks for fast access

### 3.5. Inode Structure (64 bytes)
```c
struct dinode {
    u16  mode;             // File type and permissions
    u16  nlinks;           // Hard link count
    u64  size;             // File size in bytes
    u64  atime;            // Access time (ticks since boot)
    u64  mtime;            // Modify time
    u64  ctime;            // Change time
    u64  blocks[12];       // Direct block pointers (48 bytes)
    u32  flags;            // Immutable, append-only, etc.
    u32  gen;              // Generation number (for NFS)
    u16  uid;              // User ID (unused in NO_OS)
    u16  gid;              // Group ID (unused)
    u8   reserved[6];      // Padding to 64 bytes
};
```
- Fixed size enables simple inode table indexing
- 12 direct pointers support files up to 48 KiB without indirection
- Larger files use indirect blocks (future extension)

### 3.6. Directory Entry (32 bytes)
```c
struct dirent {
    u64  inode;            // Inode number
    u16  reclen;           // Length of this record
    u8   namelen;          // Length of name (excluding null)
    u8   file_type;        // DT_REG, DIR, LINK, etc.
    char name[24];         // Filename (not null-terminated if fills field)
};
```
- Variable-length names up to 255 bytes (spanning multiple entries)
- Simple linear array within directory file's data blocks
- No hashing or b-trees - linear scan acceptable for small directories

## 4. Core Operations

### 4.1. File Creation
1. Allocate free inode from inode table (bitmap scan)
2. Initialize inode with mode, size=0, timestamps
3. Allocate first data cluster if needed (O_APPEND or O_WRITE)
4. Add directory entry to parent directory
5. Update parent directory mtime/ctime
6. Mark superblock dirty (write to secondary then primary)

### 4.2. Write Operation
1. Verify file descriptor and permissions
2. Calculate target cluster offset
3. If extending file:
   a. Allocate new cluster(s) from bitmap
   b. Update inode's block pointers
   c. Update i_size and mtime/ctime
4. Copy data to cluster(s) via DMA or PIO
5. Mark affected clusters in-memory dirty (for flush)

### 4.3. Flush & Sync
- **fsync()**: Write all dirty metadata and data to disk
- **msync()**: Synchronize memory-mapped region
- Background flush daemon writes dirty buffers periodically
- On unclean shutdown: replay intent log (if implemented)

## 5. Journaling Approach (Simple)

Instead of a full journal, M4 uses a lightweight approach:

### 5.1. Write-Ahead for Critical Metadata
Before modifying critical structures (inode allocation, directory updates):
1. Prepare changes in memory buffer
2. Write intent log entry to reserved journal area
3. Mark log entry as "committed" (flush)
4. Apply changes to main structures
5. Mark log entry as "done" (optional cleanup)

### 5.2. Log Structure
- Fixed circular buffer in reserved disk area
- Each entry: [timestamp][type][struct crc][data...]
- Types: INODE_ALLOC, DIR_UPDATE, TRUNCATE, etc.
- On mount: replay incomplete transactions from log

### 5.3. Recovery Process
1. Read both superblocks, use valid one with higher sequence
2. Scan allocation bitmap for inconsistencies (optional slow fsck)
3. Scan journal for committed but not applied transactions
4. Apply those transactions to main filesystem
5. Mark filesystem as clean

## 6. Implementation Details

### 6.1. Kernel Structures
```c
// Superblock in kernel memory
struct sb {
    // ... same as on-disk version ...
    u64     s_flags;      // Mount flags: RDONLY, DIRTY, etc.
    struct rwlock s_lock; // Protects superblock updates
};

// Open file table entry
struct file {
    struct dinode *f_inode;
    u64            f_offset;
    u32            f_flags;  // O_RDONLY, etc.
    refcount_t     f_count;
};

// Inode in core (loaded from disk)
struct inode {
    struct dinode  i_disk;   // On-disk copy
    struct rwlock  i_lock;   // Protects this inode
    struct list    i_dirty;  // For writeback
    bool           i_valid;  // Validated from disk
};
```

### 6.2. Block Device Interface
- Abstracted via `struct block_device`
- Supports PIO and DMA modes
- Queue depth of 1 (simple, predictable)
- Block size must match filesystem cluster size

### 6.3. Caching Strategy
- **Page Cache**: Optional, can be disabled for deterministic timing
- **Inode Cache**: LRU of recently used inodes (default: 64 entries)
- **Directory Cache**: Negative caching for ENOENT (recent misses)
- **Buffer Cache**: For metadata blocks (superblock, bitmap, inode table)

### 6.4. Synchronization
- **Superblock**: RW lock (readers: stat, writers: creat/unlink)
- **Inode**: RW lock per inode (readers: read, writers: write/truncate)
- **Directory**: Spinlock for directory buffer (simple linear scan)
- **Allocator**: Bitmap lock (could be sharded by cylinder group)

## 7. Usage & API

### 7.1. VFS Layer (Simplified)
NO_OS implements a minimal VFS layer:
- `int vfs_open(const char *path, int flags, mode_t mode)`
- `ssize_t vfs_read(int fd, void *buf, size_t count)`
- `ssize_t vfs_write(int fd, const void *buf, size_t count)`
- `int vfs_close(int fd)`
- `off_t vfs_lseek(int fd, off_t offset, int whence)`
- `int vfs_stat(const char *path, struct stat *buf)`

### 7.2. File Descriptors
- Per-process FD table (inherited across fork/exec)
- Standard descriptors: 0=stdin, 1=stdout, 2=stderr
- Maximum per process: 64 (configurable)

### 7.3. Special Files
- `/dev/null`, `/dev/zero`, `/dev/random` (basic implementations)
- `/dev/console` (VGA + serial)
- `/dev/tty` (current terminal)
- `/proc/` (process information, if implemented)
- `/sys/` (system information, if implemented)

## 8. Performance Characteristics

### 8.1. Space Overhead
| Component | Overhead | Notes |
|-----------|----------|-------|
| Superblock | 1024 bytes | 2 copies × 512B |
| Bitmap | 0.03% of disk size | 1 bit per 4KiB cluster |
| Inode Table | ~1% of disk size | 64B per inode, 1:16384 ratio |
| Directory Entry | ~8 bytes/filename | 24B name + 8B overhead |
| **Total** | **~1-2% typically** | Depends on file size distribution |

### 8.2. Time Complexity
| Operation | Complexity | Notes |
|-----------|------------|-------|
| File Create | O(1) + O(bitmap scan) | Bitmap scan optimized with hint |
| File Open | O(1) | Inode cache hit common |
| Sequential Read | O(n) | n = sectors transferred |
| Random Read | O(1) seek + O(transfer) | Seek dominates |
| Directory List | O(n) | n = directory entries |
| File Extend | O(1) + O(bitmap scan) | Amortized O(1) with preallocation |

### 8.3. Expected Performance (SATA SSD)
- Sequential Read: 200-300 MB/s
- Sequential Write: 150-250 MB/s
- 4K Random Read: 20-50 IOPS (no NCQ)
- 4K Random Write: 10-30 IOPS (no NCQ)
- Latency: 0.1-0.5 ms read, 0.5-2 ms write

## 9. Format Tool (`mkfs.noos`)
```
Usage: mkfs.noos [options] <device>
Options:
  -b <size>  : Block size in bytes (512, 1024, 2048, 4096, 8192, 16384, 32768)
  -L <label> : Volume label (max 11 chars)
  -f         : Force overwrite existing filesystem
  -q         : Quiet mode
  -v         : Verbose output
```
Steps:
1. Read existing MBR/GPT (if preserving)
2. Calculate layout based on device size and block size
3. Write protective MBR (optional)
4. Initialize superblock copies
5. Initialize all-bit-zero allocation bitmap
6. Initialize empty inode table
7. Create root directory (inode 1)
8. Write all structures to disk
9. Optional: verify with read-back

## 10. Integration with M5 (ML/LLM)

### 10.1. Model Storage
- Location: `/model/weight.bin`
- Format: Flat binary array of int8 weights
- Access: Memory-mapped via `mmap()` (if implemented) or explicit read
- Updates: Atomic swap via rename (`weight.new` → `weight.bin`)

### 10.2. Interaction Log
- Location: `/var/interact.log`
- Format: Circular binary log (pre-allocated fixed size)
- Size: 64 KiB (configurable via kernel parameter)
- Access: Sequential append, circular overwrite

### 10.3. NOC Corpus
- Location: `/corpus/`
- Structure:
  ```
  /corpus/
      0001.noc
      0002.noc
      ...
      last_good.noc -> 0042.noc
      metadata/
          0001.noc.meta
          0002.noc.meta
  ```
- Metadata file format:
  ```
  @@ SIZE: 1234
  @@ CREATED: 2026-08-01T12:34:56Z
  @@ GENERATED: YES/NO
  @@ PROMPT_SHA256: abcd...
  @@ SCORE: 0.85
  @@ PARENT: 0033.noc
  ```

### 10.4. Space Allocation Guidelines
| Component | Recommended Size | Notes |
|-----------|------------------|-------|
| Model Weights | 8-32 MB | Based on `model_budget` |
| Interaction Log | 64 KB | Circular buffer |
| NOC Corpus | 1-10 MB | Grows with use |
| System Space | 4 MB | Kernel logs, temp, etc. |
| User Data | Remaining | Available for general use |

## 11. Reliability & Recovery

### 11.1. Power Loss Safety
- **Metadata Updates**: Written to secondary superblock first, then primary
- **Atomic Sectors**: Assuming 512B atomic writes (standard)
- **Journal Log**: Optional write-ahead log for critical operations
- **Consistent State**: Filesystem always recoverable to known state

### 11.2. Corruption Scenarios
| Scenario | Detection | Recovery |
|----------|-----------|----------|
| Power loss during write | Incomplete sector | Retry on read, or use journal |
| Superblock corruption | Bad signature/magic | Use secondary copy |
| Bitmap inconsistency | Allocated/free mismatch | Rebuild from inodes (fsck) |
| Orphaned inode | Inode not in any directory | Move to `/lost+found` |
| Cross-linked file | Two inodes point to block | Duplicate block, notify user |

### 11.3. Filesystem Check (`fsck.noos`)
Pass 1: Check inodes and blocks
Pass 2: Check directory structure
Pass 3: Check reference counts
Pass 4: Check link counts
Pass 5: Check bitmap: Check cylinder groups (if implemented)
Repair: Salvage what possible, report losses

## 12. Future Extensions

### 12.1. Post-M4 Enhancements
- **Extent-Based Allocation**: Replace bitmap with extent trees for large files
- **Online Defragmentation**: Consolidate fragmented files during idle
- **Snapshot Support**: Copy-on-write for point-in-time copies
- **Compression**: Transparent LZ4 compression for compressible files
- **Encryption**: AES-XTS for full-disk encryption (software accelerated)

### 12.2. Hardware Integration
- **TRIM Support**: Notify SSD of free blocks for wear leveling
- **SMART Monitoring**: Report disk health via sysfs
- **Hot Plug**: Handle USB/SD card insertion/removal
- **Partition Resizing**: Online grow/shrink of filesystem

### 12.3. Advanced Features
- **Hard Links**: Multiple directory entries to same inode
- **Symbolic Links**: Pointer to another path
- **Access Control Lists**: Beyond basic rwx permissions
- **Quotas**: Per-user or per-group space limits
- **Extended Attributes**: Key-value metadata storage

## 13. Usage Example

```c
// Format a disk
$ mkfs.noos -b 4096 -L "NOOS_ROOT" /dev/sda

// Mount during boot (in kernel cmdline or init)
// root=/dev/sda1 ro

// Use from NOC
Str model_path = "/model/weight.bin";
Fd model_fd = open(model_path, O_RDONLY);
Stat st;
fstat(model_fd, &st);
U8* model_data = malloc(st.size);
read(model_fd, model_data, st.size);
close(model_fd);
// ... use model_data ...

// Save a generated NOC program
Fd corpus_fd = open("/corpus/0123.noc", O_WRONLY|O_CREAT|O_TRUNC, 0644);
write(corpus_fd, generated_noc, strlen(generated_noc));
close(corpus_fd);

// Update interaction log (kernel does this automatically)
// Users can read it for debugging:
Fd log_fd = open("/var/interact.log", O_RDONLY);
```

## 14. Relationship to Other Milestones

| Milestone | Dependency | Purpose |
|-----------|------------|---------|
| **M1** | None | Provides paging and disk driver foundation |
| **M2** | None | Provides NOC for userspace utilities |
| **M3** | M1 | Provides syscalls for file operations |
| **M4** | **M1, M3** | **Enables persistent storage for M5** |
| **M5** | **M4, M3** | **Requires filesystem for model weights, logs, corpus** |
| **M6** | M4 | Benefits from faster storage for JIT cache |
| **M7** | M4 | Game assets, demo data persistence |
| **M8** | M4 | Texture/font storage for graphics |

## 15. Conclusion

M4 provides the essential persistence layer that transforms NO_OS from a transient educational system into a practical platform for long-term experimentation and evolution. Its simplicity ensures reliability and understandability, while its efficiency makes it suitable for the memory-constrained goals of the project.

The filesystem design directly enables M5's self-evolution capabilities by providing:
- Reliable storage for model weights that survive power cycles
- Persistent interaction logs for training data
- A growing corpus of approved NOC programs
- Space for user data and experimentation

Together with M3's process isolation and M2's NOC language, M4 creates the foundation for a system that can safely learn, adapt, and improve over time — all while fitting within tight memory constraints.