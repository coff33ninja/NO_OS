# NO_OS — A Memory-Constrained AI-Friendly Operating System

NO_OS is a from-scratch, TempleOS-inspired operating system designed specifically as a platform for lightweight AI/ML experimentation and self-evolution, with a strong emphasis on running within tight memory constraints.

## Core Philosophy

**Everything starts as a specification** - All major features begin as detailed design documents before implementation.

**CLI-first, GUI-last** - Focus on building powerful command-line capabilities before considering graphical interfaces.

**Memory-conscious design** - Every architectural decision considers RAM usage, targeting <64MB total system footprint to enable viable AI workloads on modest hardware.

**AI/ML as a first-class use case** - The system evolves to better serve its users through on-device learning and automation.

## Memory Budget Philosophy

The core constraint driving NO_OS's design is maintaining a small memory footprint to leave room for AI workloads. Current targets:

- **Kernel**: <6 MB RAM (leaving ample space for user processes)
- **Total System**: <64 MB RAM (runs comfortably in 64MB+ environments)
- **ML Workloads**: 8-32 MB for model weights (8-bit quantized 1-10M parameter models)

This is achieved through deliberate choices documented in our Architecture Decision Records (ADRs).

## Key Architectural Decisions

See `docs/ADR/` for detailed rationales:

1. **[Stack VM for NOC](docs/ADR/0001-stack-vm.md)** - Saves 50-100KB vs register VM
2. **[No Network Stack](docs/ADR/0002-no-network.md)** - Saves 50-150KB, reduces attack surface
3. **[ELF Reframing for QEMU](docs/ADR/0003-elf-reframe-qemu.md)** - Enables 64-bit kernel development with 32-bit toolchain compatibility

## Documentation Roadmap

### Specifications
- [SPEC.md](docs/SPEC.md) - Overall architecture and design decisions
- [NOC.md](docs/NOC.md) - The NO_OS Command Language (shell and application language)
- [M3-USERMODE.md](docs/M3-USERMODE.md) - User mode and multitasking (complete)
- [M4-FILESYSTEM.md](docs/M4-FILESYSTEM.md) - Persistent storage (current focus)
- [M5-AI.md](docs/M5-AI.md) - Machine learning and self-evolution (AI/ML focus)
- [M8-GRAPHICS.md](docs/M8-GRAPHICS.md) - Graphics subsystem (GUI last)

### Memory & Budget Tracking
- [MEMORY-BUDGET.md](docs/MEMORY-BUDGET.md) - Tracks RAM usage for AI workload feasibility

### Milestones (per [ROADMAP.md](docs/ROADMAP.md))
- [x] M0: Toolchain & first boot
- [x] M1: Core services (GDT/IDT, PIC/PIT, keyboard, memory, printk)
- [x] M2: NOC language (lexer, parser, compiler, VM, REPL)
- [x] M2.5: Shell & stability (line editor, interruptible VM)
- [x] M3: User mode & multitasking
- [ ] M4: Filesystem (next)
- [ ] M5: ML/LLM & self-evolution (AI focus)
- [ ] M6: Performance & self-hosting (stretch)
- [ ] M7: Fun (stretch: audio, games)
- [ ] M8: Graphics (TempleOS soul, GUI last)

## Getting Started

### Prerequisites
- Zig compiler (for cross-compilation)
- NASM assembler
- QEMU (for emulation)
- OBJCOPY (from binutils)

### Building
```powershell
# PowerShell build script
.\scripts\build.ps1 build
```

### Running
```powershell
.\scripts\build.ps1 run
```

### Testing
```powershell
.\scripts\build.ps1 test
```

## Why This Approach for AI?

Traditional OS/ML stacks are heavy:
- Linux + Python + PyTorch: 1-2GB+ RAM just to start
- Even "lightweight" setups often exceed 512MB

NO_OS takes a different approach:
1. **Minimal Base**: Kernel <6MB leaves room for actual workloads
2. **Purpose-Built**: No legacy baggage, POSIX cruft, or unused drivers
3. **Deterministic**: Bounded memory usage enables reliable AI experimentation
4. **Evolvable**: The system can learn to better automate itself
5. **Educational**: Simple enough to understand completely, yet powerful enough for real ML experiments

## Target Use Cases

1. **On-Device Learning**: TinyML-style models learning from user interaction
2. **Code Suggestion Model**: Predicting next command/token from history
3. **Automated Task Generation**: Creating useful NOC scripts from natural language descriptions
4. **Personal Assistants**: Very small footprint voice/text assistants
5. **Edge AI Experiments**: Prototyping algorithms before deploying to larger systems

## Current Status

The system has completed **M3: User Mode & Multitasking** — ring-3 NOC processes with memory protection and preemptive multitasking, validated by the boot-test acceptance suite (`TEST PASS`). The allocator telemetry hooks (`pmm_total_bytes`/`pmm_free_bytes`/`sched_register_idle_hook`) are in place as the foundation for growth.

Next up is **M4 (Filesystem)**, which will provide persistent storage for model weights and training data, enabling **M5 (ML/LLM & Self-Evolution)** where the real AI magic happens.

## Contributing

This is primarily a personal/experimental project, but insights and discussions are welcome. The documentation-first approach means that understanding the *why* behind decisions is as important as the code itself.

See the ADR directory for the reasoning behind key architectural choices that make this system uniquely suited for memory-constrained AI experimentation.

---

*Last updated: 2026-08-01*
*Build status: M3 user-mode multitasking complete, TEST PASS*