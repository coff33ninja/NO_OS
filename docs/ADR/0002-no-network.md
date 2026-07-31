# 0002-no-network: No Network Stack

## Status
Accepted

## Context
NO_OS is designed as a simple, educational operating system with a focus on AI integration and self-evolution capabilities. Early in the design process, we needed to decide whether to include network stack support.

## Decision
We decided to omit network stack support from NO_OS.

## Status
Accepted

## Consequences

### Positive
- **Significant RAM savings**: A TCP/IP stack typically consumes 50-150KB of RAM
- **Reduced complexity**: No need to implement or debug complex networking protocols
- **Reduced attack surface**: No network-exposed vulnerabilities
- **Simplified security model**: No need for sandboxing network access
- **Faster boot times**: No network initialization delays
- **Deterministic behavior**: No external dependencies affecting timing

### Negative
- **No network capabilities**: Cannot download updates, packages, or data over network
- **No network-based applications**: No web browsers, SSH clients, etc.
- **Limited interoperability**: Cannot easily share data with other systems
- **No remote debugging**: Must rely on serial/USB for debugging

## Rationale for NO_OS
NO_OS's primary goals are:
1. Being a platform for AI/ML experimentation (M5 milestone)
2. Being simple enough to understand completely (educational)
3. Running on constrained hardware
4. Providing a CLI-first environment for self-evolution

Networking does not directly support these core goals. The AI/ML workloads in M5 are designed to run locally, learning from local interaction history. The CLI-first approach means all interaction happens locally via keyboard and display.

For networking needs, users can:
- Use serial/USB transfer for file movement
- Run NO_OS in a VM and use host networking for transfers
- Add networking in a future fork or extension if needed

This decision supports our RAM budget goals by saving valuable memory that can be used for user processes and AI models instead.

## Related Decisions
* [[0001-stack-vm]]: Stack VM saves ~50-100KB vs register VM
* [[0003-elf-reframe-qemu]]: OBJCOPY workaround avoids toolchain complexity

## Implementation Notes
NO_OS currently includes:
- PS/2 keyboard driver (for input)
- VGA text/graphics drivers (for output)
- Serial driver (for debugging/host communication)
- PIT timer (for timekeeping)

No network device drivers exist in the codebase, and no networking subsystem has been implemented. The kernel would reject any attempts to add network device drivers without significant architectural changes.