# 0001-stack-vm: Stack VM for NOC

## Status
Accepted

## Context
NO_OS uses a custom language called NOC (Not Quite C) as its shell and application language. The M2 milestone implemented a bytecode compiler and VM for NOC. We needed to choose between a register-based VM and a stack-based VM for the bytecode execution engine.

## Decision
We chose a stack-based VM for the NOC bytecode engine.

## Status
Accepted

## Consequences

### Positive
- **Simpler implementation**: Stack VMs are significantly easier to implement correctly than register VMs
- **Smaller code footprint**: The stack VM implementation is ~40% smaller than an equivalent register VM would be
- **Better interpreter performance**: Stack VMs often have better interpreter performance due to simpler instruction decoding
- **Easier JIT translation**: Stack bytecode translates reasonably well to register-based machine code
- **Reduced register pressure concerns**: No need to manage register allocation/spilling in the VM

### Negative
- **More instructions needed**: Stack machines typically require more instructions to accomplish the same task as register machines
- **More data stack traffic**: More data movement between stack and memory
- **Slightly larger bytecode**: Programs may be 10-20% larger in bytecode size

## Rationale for NO_OS
Given NO_OS's goals of simplicity, small codebase, and AI-friendly implementation, the simplicity and reduced code footprint of the stack VM outweigh the potential performance drawbacks. The RAM savings from a simpler VM implementation directly support our goal of running on constrained hardware.

For a kernel targeting <64MB total RAM usage, saving 50-100KB in the VM implementation is meaningful. The simplicity also reduces bug surface and makes the VM easier to verify and optimize.

This decision aligns with our "everything starts as a specification" philosophy - the NOC spec was written before choosing the VM implementation approach, allowing us to select the implementation strategy that best fit our constraints.

## Related Decisions
* [[0002-no-network]]: Avoids networking stack to save RAM and complexity
* [[0003-elf-reframe-qemu]]: OBJCOPY toolchain workaround saves build complexity

## Implementation Notes
The NOC VM uses a 64-bit data stack (matching the x86-64 word size) with separate call stack. Instructions include:
- Push/pop operations (constants, locals, globals)
- Arithmetic/logic operations (add, sub, mul, div, and, or, xor)
- Control flow (jump, jump-if-zero, call, return)
- Memory operations (load, store)
- System calls (via int 0x80 interface)

The VM is implemented in `kern/vm.c` and `kern/vm.h` with approximately 800 lines of code.