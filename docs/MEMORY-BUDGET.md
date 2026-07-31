# Memory Budget — NO_OS

Tracks the RAM budget for NO_OS to keep it suitable for running lightweight AI models on constrained hardware.

## Current Budget (M3 Milestone)

| Component | Size | Notes |
|-----------|------|-------|
| Kernel static (text + rodata + bss) | ~1.2 MB | Measured from `size kernel.elf` after M2.5 |
| Kernel heap | 4 MB | Managed by `kmalloc` in `mm/alloc.c` |
| Per-process stack | 8 KB | Default stack size for user processes |
| Per-process heap | 64 KB | Per-process allocator for user NOC processes |
| Video memory (VGA text) | 8 KB | 80x25x2 bytes for text mode |
| Video memory (VGA graphics) | 150 KB | 640x480x16-color (1 bit per pixel) |
| IDT/GDT/TSS | < 8 KB | Fixed kernel structures |
| Boot structures | < 4 KB | Multiboot, paging tables, etc. |
| **Total reserved for kernel** | **~5.5 MB** | Leaves room for user processes in remaining RAM |

## M4/M5 Roadmap Targets

| Milestone | Target | Notes |
|-----------|--------|-------|
| M4 (Filesystem) | +1 MB kernel | For IDE driver and simple FS |
| M5 (ML/LLM) | +2-4 KB kernel | For model budget syscall and eviction logic |
| M5 (ML/LLM) | 16-32 MB user space | For model weights (8-bit quantized 1-10M parameter model) |
| **Target total RAM usage** | **< 64 MB** | Leaves room on 64MB+ systems for basic operation |

## Model Budget Details (M5)

The `model_budget(n)` syscall will enforce:
- `n` = maximum RAM (in KB) a single LLM process can allocate for weights
- Default: 8192 KB (8 MB) for a ~8M parameter 8-bit model
- Enforced per-process; kernel tracks usage and evicts clean pages under pressure
- Weights stored as demand-paged, read-only pages from disk-backed store

## Rationale

This budget ensures NO_OS remains lightweight enough to run on modest hardware while still capable of executing useful language models. By keeping the kernel under 6 MB and reserving predictable budgets for user processes, we ensure deterministic behavior even under memory pressure.

## Related ADRs

* [[0001-stack-vm]]: Stack VM choice saves ~100KB vs register VM
* [[0002-no-network]]: No network stack saves ~50-100KB and reduces attack surface
* [[0003-elf-reframe-qemu]]: OBJCOPY trick avoids needing separate 32/64-bit toolchains

## Update Cadence

Update this file whenever:
- Kernel static size changes by >10%
- Heap sizes are adjusted
- New major subsystems are added (filesystem, networking, etc)
- Model budget parameters are set in M5