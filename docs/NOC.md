# NOC — NO_OS Command language

NOC is the shell and application language of NO_OS, a HolyC-inspired,
C-like language that compiles to bytecode and runs on a stack VM inside the
kernel. This document specifies the M2 subset (interactive REPL + scripts).

## 1. Execution model

- Every REPL line is a sequence of top-level statements.
- Each line is lexed, parsed into an AST, and compiled to bytecode for a
  stack VM, then executed immediately.
- The compiler is a single pass: functions must be defined before they are
  called (no forward references in M2).
- Functions defined at the REPL persist for the rest of the session; local
  variables and ASTs are discarded after each line.
- After a line runs, if its last statement is a non-`U0` expression whose
  value is non-zero, the value is printed (calculator style).

## 2. Values and types

All runtime values are 64-bit. Declared types are checked loosely.

| Type | Meaning |
|---|---|
| `U0`  | void (no value) |
| `I64` | signed 64-bit |
| `U64` | unsigned 64-bit |
| `I32` `U32` `I16` `U16` `I8` `U8` | narrowed integer types (stored as 64-bit) |
| `Bool` | 0 or 1 |
| `Str` | pointer to a NUL-terminated string |
| `F64` | reserved (not implemented in M2) |

## 3. Literals

- Decimal integers: `42`, `0`, `-7`.
- Hex integers: `0x1F`.
- Booleans: `true`, `false`.
- Strings: `"hi"` with escapes `\n`, `\t`, `\\`, `\"`.
- Comments: `// line` and `/* block */`.

## 4. Variables

```c
I64 x = 40 + 2;
Str s = "hello";
Bool done = false;
U8 *p = Alloc(64);   /* not in M2; use U8* via Alloc() */
```

Local variables are scoped to the enclosing function (flat namespace; no
shadowing). A variable used before declaration is a compile error.

## 5. Operators (precedence, high to low)

```
()              call
++ --           postfix increment/decrement
- ! ~ ++ --     unary
* / %           multiplicative
+ -             additive
<< >>           shift
< <= > >=       relational
== !=           equality
&               bitwise and
^               bitwise xor
|               bitwise or
&&              logical and (short-circuit)
||              logical or (short-circuit)
= += -= *= /=   assignment (right-assoc)
```

Relational, equality, and logical operators yield `1`/`0`.
Division or modulo by zero is a runtime error that aborts the line.

## 6. Statements

```c
I64 x = 5;            /* variable declaration */
x = x + 1;            /* expression statement */
x += 2;               /* compound assignment */
if (x > 7) { ... } else { ... }
while (x < 100) { ... }
for (I64 i = 0; i < 10; i++) { ... }   /* for(;;) also allowed */
return x;             /* inside functions; return; for U0 */
{ ... }               /* block */
```

## 7. Functions

```c
I64 Mul2(I64 x, I64 y = 2) {
    return x * y;
}
U0 Greet(Str name) {
    PrintLn("hello " + name);   /* concatenation not in M2; use Print */
}
```

- Parameters may have default literal arguments (`I64 y = 2`). Callers may
  pass fewer arguments than declared; defaults fill the tail.
- Return type `U0` means the function yields no value.
- A non-`U0` function implicitly returns `0` if it falls off the end.
- Functions are registered in the global table when the defining line runs;
  later lines may call them.

## 8. Builtins

| Signature | Effect |
|---|---|
| `U0 Print(Str fmt, ...)` | Formatted output, no newline. `%d %u %x %X %c %s %lld %llu %llx %p` |
| `U0 PrintLn(Str fmt, ...)` | Same, with trailing newline |
| `I64 Time()` | Milliseconds since boot |
| `U0 Sleep(I64 ms)` | Block for ms (PIT sleep, interrupts on) |
| `I64 KeyGet()` | Blocking read of a key; returns char code |
| `I64 KeyPressed()` | 1 if a key is buffered, else 0 |
| `U8 *Alloc(I64 n)` | `kmalloc` (kernel heap) |
| `U0 Free(U8 *p)` | `kfree` |
| `U0 MemSet(U8 *p, I64 v, I64 n)` | Fill memory |
| `U0 MemCpy(U8 *d, U8 *s, I64 n)` | Copy memory |
| `I64 Len(Str s)` | String length |
| `U0 Version()` | Print kernel version |
| `U0 MemInfo()` | Print physical memory + heap usage |
| `U0 FaultTest()` | Deliberately raise `#UD` (fault handler must trap it) |
| `U0 Reboot()` | Reset via the keyboard controller |

## 9. M2 limitations

- No strings as first-class mutable objects (no concatenation or indexing).
- No arrays, structs, pointers-to-locals, or global variables.
- No forward references; functions must be defined before use.
- Default arguments must be integer or boolean literals.
- No `else if` short form beyond nesting.
- No `.noc` file loading yet (REPL only).
