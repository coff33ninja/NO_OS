# M10 — NOClang TUI (terminal user interface)

NO_OS M10 replaces the scrolling line REPL with a **NOClang TUI**: a
full-screen, text-mode command surface on the 80x25 VGA console where the
NOC language — *NOClang* — is edited, run, and observed in dedicated
regions. This is TempleOS's text-console philosophy: the whole screen is the
shell, split into an **output viewport**, an **input/editor line**, and a
**live status bar**, with function-key commands and a completion popup.

It is a **TUI, not a GUI**: pure text cells (char + color attribute) in
80x25 mode, no windows, no mouse, no bitmap drawing. The graphics mode from
M8 remains for NOC drawing programs; the shell stays text.

## 1. Current state

- VGA text console `kernel/drivers/vga.c`: 80x25, cells are `u16` at
  `0xB8000` (char | color<<8), `vga_color`, linear `vga_scroll()` only
  (`vga.c:34-41`). No direct cell addressing, no cursor API, no region
  model. `vga_set_color` exists (`vga.c:29`) but colors all output.
- Line editor `kernel/kern/line.c`: backspace, left/right/home/end/del,
  up/down history, Ctrl+C/Esc. 16-entry recall history. Dual serial+VGA
  output (`putc2`, `line.c:30-40`).
- REPL `kernel/noc/repl.c`: `line_read` → `cmdhist_add` → capture →
  `noc_exec_line`. One line = one program.
- Keyboard `kernel/include/kbd.h`: extended codes for arrows/home/end/del
  only (`kbd.h:7-13`). **No function keys** — the scancodes are decoded to
  nothing, so F1..F12 need new mappings (see §5).
- Predictor `kernel/noc/predict.c` (`cmdhist_predict`), interaction capture
  `kernel/noc/interact.c` (`[CMD]`/`[OUT]`/`[ERR]`/`[TICK]`), builtin table
  `kernel/noc/vm.c:1435`, M9 service registry (`SvcCount`...`SvcKill`).

## 2. Screen layout

The TUI owns the VGA frame buffer and divides it into three regions:

```
┌──────────────────────────────────────────────────────────────┐
│ F1 Help   F2 Ps   F3 Mem   F5 Run   F9 Mgr   F10 Files       │  ← status bar (row 0)
├──────────────────────────────────────────────────────────────┤
│                                                              │
│   output viewport (rows 1..22, 80 cols)                      │
│   scrollback ring of past output, paged with PgUp/PgDn       │
│                                                              │
├──────────────────────────────────────────────────────────────┤
│ no/os> if (n<2) return n;  return Fib(n-1)+Fib(n-2);         │  ← input line (row 23)
│ ▸ I64 Fib(I64 n) {                                           │  ← editor area (row 24)
└──────────────────────────────────────────────────────────────┘
```

- **Row 0**: status bar — key legends, memory, tick count, running services
  (M9), model loss. Redrawn on every key, never scrolls.
- **Rows 1..22**: the output viewport. Program output and results land here.
  A ring buffer (e.g. 8 KiB) holds the scrollback; PgUp/PgDn page through it.
- **Row 23**: single-line input. For multi-line NOClang (§6) the editor
  expands into **row 24** as a second line, then folds back.
- Rows are cell-exact: the TUI writes `u16` cells directly, so nothing ever
  triggers the old linear scroll except output filling the viewport.

Serial stays the current linear console — the TUI is the VGA surface, and
serial remains a byte-exact mirror of executed commands and their output for
the harness (see §8).

## 3. VGA API v2 (`kernel/drivers/vga.c`, `kernel/include/vga.h`)

The TUI needs cell-level control the current console lacks:

```c
void vga_clear_rect(u8 row0, u8 col0, u8 rows, u8 cols, u8 color);
void vga_putc_at(u8 row, u8 col, char c, u8 color);
void vga_puts_at(u8 row, u8 col, const char *s, u8 color);
void vga_set_cursor(u8 row, u8 col);
void vga_get_cursor(u8 *row, u8 *col);
void vga_scroll_region(u8 row0, u8 row1);   /* scroll only one band */
```

- All coordinates clamp to 80x25; out-of-range calls are no-ops (same rule
  as `gfx_*` in M8), so NOC cannot corrupt the frame through coordinates.
- The existing `vga_putc/vga_write` remain for the serial mirror and for
  programs that just print — they target the viewport band, not the full
  screen, once the TUI owns the layout.
- Colors: per-region default (viewport = white-on-black, status = reversed,
  input = bright), overridable per cell by the TUI.

## 4. Keyboard v2 (`kernel/include/kbd.h`)

Add function-key codes in the same extended namespace as the arrows:

```c
#define KBD_F1    0x90
#define KBD_F2    0x91
#define KBD_F3    0x92
#define KBD_F4    0x93
#define KBD_F5    0x94
#define KBD_F6    0x95
#define KBD_F7    0x96
#define KBD_F8    0x97
#define KBD_F9    0x98
#define KBD_F10   0x99
#define KBD_PGUP  0x9A
#define KBD_PGDN  0x9B
```

Decoded in `drivers/kbd.c` from the existing F1-F12 scancodes (0x3B..0x44)
and PgUp/PgDn (0x49, 0x51), which today fall through as unknown keys.

## 5. Key map

| Key | Action |
|-----|--------|
| F1 | `Help` — grouped help overlay in the viewport |
| F2 | `Ps` — service/process table (M9) |
| F3 | `MemInfo` |
| F4 | `LogDump` — recent interaction log |
| F5 | **Run** — execute the current NOClang program (§6) |
| F6 | Clear viewport |
| F9 | Toggle the service monitor pane (M9, §7) |
| F10 | `ListDir` — filesystem listing |
| Tab | Completion popup (§6) |
| PgUp/PgDn | Scroll the viewport history |
| Up/Down | Recall history (when not in a popup) |
| Ctrl+C/Esc | Clear the current line (as today, `line.c:147-153`) |
| Ctrl+L | Redraw the whole frame |

Function keys are *named bindings to NOC builtins*, not hard-coded logic:
`F2` invokes the `Ps` builtin, `F3` invokes `MemInfo`, and so on. A NOC
`SetKey(I64 fkey, Str builtin)` builtin can rebind them, keeping policy in
NOC. The status bar redraws its legends from the current bindings.

## 6. Input & completion (NOClang editing)

The input region is the NOClang editor. Beyond M10's line editing it gains:

- **Multi-line NOClang** (§5 of the old design, now in-region): a line that
  leaves `{`/`(`/`[` depth open (checked by `line_balanced()`) expands the
  editor to a second row with a continuation glyph (`▸`); F5 runs the whole
  accumulated program as one `noc_exec_line`. Both rows are captured as a
  single interaction record, so M5 trains on whole programs.
- **Completion popup**: Tab shows a small overlay in the viewport listing
  matching names (builtins `vm.c:1435`, user functions, M9 service names, FS
  files). Up/Down move through it, Enter/Tab accepts, Esc dismisses. The
  popup is keyboard-only; it never scrolls the viewport.
- Completion and history recall are never logged as commands — only the
  executed line enters the interaction stream (`predict.c:17-42` model).

## 7. Service monitor pane (M9 integration)

F9 toggles a right-hand pane (rows 1..22, rightmost 28 cols) showing the M9
service table live: name, state, `cpu_ms`, budget, model KB. It is just a
NOC program using `SvcCount/SvcPid/SvcField` in a loop, redrawn on a timer
(Sleep), parked in the status bar. When the pane is closed, the viewport
uses the full width. This makes the task manager (`taskmgr.noc`, M9) a
resident part of the shell rather than a spawned script.

## 8. Serial & harness contract

The TUI is a VGA-only presentation. Serial output stays byte-exact:
every executed command and every line of program output is written to serial
as plain text, exactly as `noc_os_puts` does today. The QEMU harness reads
serial, so it never sees status-bar redraws, popups, or pane toggles — only
real output. This keeps every M-series acceptance assert valid unchanged.

## 9. Startup & status

- The TUI initializes after the service registry (M9) and model init
  (`kernel.c:183-185`), before `noc_repl()`.
- `autoexec.noc` still runs at boot (persisted setup, `SetPrompt`,
  `SetKey` rebinds, auto-start services). The default prompt `no/os>` and
  default key map are the fallbacks.
- The status bar ticks every PIT tick (100 Hz) via the existing
  `sched_on_tick` path and shows: `Time()`, `MemInfo` free bytes, running
  service count, model millibit loss (`trans_info`).

## 10. Acceptance

1. Boot into the TUI: status bar renders (F-key legends), prompt visible at
   row 23, viewport empty.
2. Type a NOClang line, press Enter → executes, output lands in the viewport,
   serial shows the same text.
3. Type `if (n<2) return n;` (open depth), then `return n-1; }` → editor
   expands to a second row, F5 runs the accumulated program, result appears.
4. `Log<Tab>` → completion popup lists `LogInfo LogDump LogSave LogClear
   Log`; selecting and Entering executes it.
5. F2 → service table fills the viewport; F9 → live monitor pane appears;
   F9 again → pane closes, viewport returns to full width.
6. PgUp/PgDn page through previously scrolled output in the viewport.
7. F3 rebind via `SetKey(3, "Ps")` → F3 now prints the process table; the
   status bar legend updates.
8. `SvcKill` from the prompt kills a spawned service and the monitor pane
   reflects the state change on the next redraw.
9. Serial log (harness) contains exactly the executed commands and outputs —
   no status/popup artifacts.

## 11. Non-goals (M10)

- **Not a GUI.** No windows, no mouse, no bitmap/graphics-mode shell, no
  overlapping panels. Pure 80x25 text cells. M8 graphics stays for drawing
  programs only.
- No per-window scrollback or multiple concurrent viewports; one output
  region plus one monitor pane.
- No ANSI/VT100 escape sequences on VGA; no terminal-emulator semantics.
- No remote login over serial; serial is a mirror, not a second session.
- No full grammar completion — names only, no fuzzy matching.
- The old linear `vga_scroll` remains available for non-TUI output paths,
  but the shell never uses it.
