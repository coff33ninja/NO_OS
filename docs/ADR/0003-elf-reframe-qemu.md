# 0003-elf-reframe-qemu: OBJCOPY ELF Reframing for QEMU Compatibility

## Status
Accepted

## Context
NO_OS targets x86-64 architecture and uses QEMU for development and testing. During early bringup (M0 milestone), we discovered that QEMU's `-kernel` parameter (which loads a Multiboot-compatible kernel) rejects ELF64 executables, even though we are building a 64-bit kernel.

## Decision
We use a two-step linking process:
1. Link a true 64-bit ELF executable (`kernel.elf64`)
2. Use `objcopy -O elf32-i386` to repackage it as a 32-bit ELF container holding the identical 64-bit code (`kernel.elf`)

## Status
Accepted

## Consequences

### Positive
- **QEMU compatibility**: Works with QEMU's `-kernel` parameter for easy testing
- **No toolchain changes**: Uses standard `zig cc` (clang/LLVM) and `objcopy` (binutils)
- **Maintains 64-bit kernel**: The actual running code remains pure 64-bit long mode
- **Simple build process**: Handled automatically by `scripts/build.ps1`
- **GRUB compatibility**: The resulting 32-bit ELF container also works with legacy GRUB

### Negative
- **Slightly more complex build**: Two-step process instead of direct linking
- **Confusing terminology**: We call it a "32-bit ELF" but it runs 64-bit code
- **Extra build step**: Adds minimal overhead to build time
- **Two binaries**: Produces both `kernel.elf64` and `kernel.elf` artifacts

## Rationale for NO_OS
This approach solves a specific toolchain/QEMU compatibility issue without compromising our architectural goals:
- We still develop and run a true x86-64 long mode kernel
- We avoid maintaining separate 32-bit and 64-bit code paths
- We avoid switching to a different emulator or boot method that would complicate development
- The solution is well-documented and isolated to the build scripts

The technique is documented in the SPEC.md under the "Boot process" and "Toolchain" sections, ensuring future maintainers understand why this approach was chosen.

## Related Decisions
* None directly - this is a toolchain/build-specific decision
* Indirectly related to our choice of `zig cc` as compiler (provides reliable cross-compilation)

## Implementation Details
The build process in `scripts/build.ps1`:
1. Compiles all source files to object files
2. Links `kernel.elf64` using a 64-bit linker script (`linker.ld`)
3. Runs `objcopy -O elf32-i386 kernel.elf64 kernel.elf` to create the QEMU-compatible binary
4. The `kernel.elf64` artifact is retained for debugging and disassembly

The kernel entry code (`boot.s`) handles the transition from 32-bit protected mode (where QEMU starts it) to 64-bit long mode before jumping to the main kernel code (`kmain()` in `kernel.c`).

This approach allows us to use QEMU's convenient `-kernel` parameter while maintaining our commitment to a pure 64-bit kernel architecture.