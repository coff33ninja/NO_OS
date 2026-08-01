# M9 — Services & Task Manager (NOC)

NO_OS M9 turns the scheduler into a service host. Every background
component — the interaction logger, the idle trainer, the predictor, the
filesystem, and every NOC program spawned by the M5 pipeline or by the
user — becomes a **named service** with a lifecycle, and a NOC program
(`taskmgr.noc`) becomes the system's service manager: list, start, stop,
restart, kill, set CPU/memory budgets, and inspect status. The model follows
Windows services (SCM) and Linux daemons/systemd units, adapted to NO_OS's
single-kernel, NOC-first design. The AI/ML components are services like any
other — not a special class.

## 1. What exists today

- Scheduler: `kernel/kern/sched.c` — 16 slots (`TASK_MAX`), task 0 is the
  kernel REPL, tasks 1..15 are user processes. Round-robin preemption via
  `sched_preempt()` on the PIT tick (`sched.c:104`), block via
  `sched_block_user()` (`sched.c:111`), exit via `sched_exit_user()`
  (`sched.c:120`), spawn via `sched_spawn()` (`sched.c:177`).
- Background components exist but are **not** services: the interaction
  logger (`kernel/noc/interact.c`), the idle trainer (`kernel/noc/train.c`,
  driven by the idle hook `sched_register_idle_hook` + the keyboard idle
  poll at `drivers/kbd.c:166`), the character predictor (`kernel/noc/predict.c`),
  and the filesystem (`fs_init()` at `kernel/kern/kernel.c:166`). Each is
  hard-wired into boot or the REPL; none can be listed, stopped, restarted,
  or budgeted from outside.
- `sched_ps()` (`sched.c:277`) prints a task table, but only to serial via
  `printk` — kernel-only, not reachable from user processes.
- Syscalls 0..15 exist (`kernel/include/syscall.h`); none enumerate tasks,
  read CPU time, or control another process.
- `task_t` (`kernel/include/sched.h:23-45`) carries pid, state, name, heap,
  and model-weight accounting — but **no CPU accounting** and **no burst
  budget** (audit finding #7).
- NOC builtins register in `kernel/noc/vm.c:1435` and reach the OS through
  the `noc_os_*` abstraction (`kernel/include/noc_os.h`), which maps to
  `int 0x80` in user mode (`user/noc_os.c`) and direct calls in the kernel.

## 2. Service model

A **service** is a named, governable unit of system work. Two classes:

| Class | Examples | Governable? |
|---|---|---|
| **Kernel-attached** | interact logger, idle trainer, predictor, filesystem | List, pause/resume, budget; not killable (part of the kernel) |
| **User** | any `sched_spawn`ed NOC program (M5 drafts, user scripts, automation) | Full lifecycle: start, stop, restart, kill, budget |

A fixed-size **service registry** gives every service an identity beyond a
raw task slot. Kernel services register at boot; user services register
either at boot (auto-start) or when spawned with a name.

```c
#define SVC_MAX        16

typedef enum {
    SVC_KERNEL,            /* kernel-attached subsystem */
    SVC_USER,              /* NOC program running as a ring-3 task */
} svc_kind_t;

typedef enum {
    SVC_RESTART_NEVER,     /* don't restart on exit */
    SVC_RESTART_ON_FAIL,   /* restart only on non-zero exit */
    SVC_RESTART_ALWAYS,    /* always restart */
} svc_restart_t;

typedef struct {
    const char *name;       /* registry key, e.g. "interact", "trainer", "fs" */
    svc_kind_t  kind;
    u8          autostart;  /* 1 = spawned at boot */
    svc_restart_t restart;  /* user services only */
    const char *source;     /* NOC script source for SVC_USER */
    void      (*pause)(int paused);  /* kernel-attached pause hook */
    u32         pid;        /* 0 = not running */
    u32         restarts;   /* lifetime restart count */
} svc_t;
```

**Lifecycle states** (per service, derived from the backing task or the
pause hook):

```
stopped ──start──▶ starting ──▶ running ──pause──▶ paused
  ▲                      │            │            │
  │                      │ exit       │ crash      │ resume
  └────────restart────────◀────────────┴────────────┘
```

- `starting`/`running`/`stopped`: the backing task is RUNNABLE/RUNNING, then
  ZOMBIE+reaped.
- `paused`: user services get `budget_paused` (see §3); kernel services get
  their `pause(1)` hook called (e.g. trainer stops firing on idle).
- `crashed`: a user service exited non-zero; per its restart policy the
  scheduler re-spawns it from `source` and increments `restarts`.

Boot sequence: kernel registers `interact`, `trainer`, `predictor`, `fs`
(kind `SVC_KERNEL`, `pause` hooks) and any auto-start user services from a
persisted boot list, then `sched_spawn`s each user service. This replaces
the current hard-wired `train_init()`/`trans_init()`/`il_load()` ordering in
`kernel.c:183-185` with the registry as the single source of truth.

## 3. CPU accounting & burst budget (all services, not just ML)

Two new per-task fields in `task_t`:

```c
u64  cpu_ticks;        /* PIT ticks this task has actually run */
u32  cpu_budget_ms;    /* 0 = unlimited; else max runtime before pause */
u8   budget_paused;    /* 1 = suspended by the burst budget */
```

`cpu_ticks` is charged once per PIT tick in `sched_on_tick()` (`sched.c:134`)
for the currently running user task (`current && current->user`). At 100 Hz
this is exact to 10 ms and costs one counter increment per tick.

Budget enforcement, in the same tick handler after preemption:

- If a task has `cpu_budget_ms > 0` and `cpu_ticks * 10 >= cpu_budget_ms`,
  suspend it: `state = TASK_BLOCKED`, `budget_paused = 1`, `wake_ticks = 0`.
- A suspended task never runs again until its budget is raised (`SYS_SVC_BUDGET`)
  or it is restarted/killed. One tick of grace at most.

This bounds *total* CPU for any service — a runaway NOC automation, a
misbehaving M5 draft, a spin loop — while round-robin keeps the rest of the
system responsive. It is the audit finding #7 guardrail.

## 4. Syscall surface (new)

Numbers 16..21 in `kernel/include/syscall.h`:

| # | Name | Args | Returns |
|---|------|------|---------|
| 16 | `SYS_SVC_COUNT`   | —          | number of registered services |
| 17 | `SYS_SVC_PID`     | `i` (index)| pid of i-th registered service (-1 if not running, or kernel-attached) |
| 18 | `SYS_SVC_FIELD`   | `pid, field` | one scalar field, -1 on bad pid/field |
| 19 | `SYS_SVC_BUDGET`  | `pid, ms`  | previous budget in ms, -1 on bad pid |
| 20 | `SYS_SVC_PAUSE`   | `pid, on`  | pause (1) or resume (0); 0 ok, -1 bad pid |
| 21 | `SYS_SVC_KILL`    | `pid`      | 0 ok, -1 if pid is invalid/self/task 0/kernel-attached |

`SYS_SVC_PID` makes enumeration a scalar loop — NOC M2 has no arrays or
structs, so the manager iterates indexes `0..SYS_SVC_COUNT-1`.

`SYS_SVC_FIELD` field codes:

| field | meaning |
|-------|---------|
| 0 | state: 0=stopped 1=starting 2=running 3=paused 4=crashed |
| 1 | `cpu_ticks` (PIT ticks actually run) |
| 2 | `heap_used` bytes |
| 3 | `model_weights_kb` |
| 4 | `model_budget_kb` |
| 5 | `wake_ticks` (0 if not asleep) |
| 6 | `cpu_budget_ms` (0 = unlimited) |
| 7 | `budget_paused` |
| 8 | 1 if pid == calling task (self-identification) |
| 9 | kind: 0=kernel-attached, 1=user |

Dispatch rules in `syscall.c`:

- `SYS_SVC_COUNT`/`SYS_SVC_PID`: walk the registry; count entries; for index
  i return the running task's pid (user services), or -1 for kernel services
  or stopped ones.
- `SYS_SVC_FIELD`: resolve pid to a task; read plain fields (no user memory
  access), so only pid/field bounds need checking. For kernel-attached
  services the caller passes the special sentinel pid `(u32)-1` + field to
  read state/pause from the hook — or simpler, `SYS_SVC_PID` returns 0 for
  kernel services and the manager treats field reads against pid 0 as
  registry queries. (Chosen at implementation; the NOC contract is: every
  registry entry is readable, running user tasks additionally expose CPU/heap.)
- `SYS_SVC_BUDGET`: set `cpu_budget_ms`; if raising above 0 and
  `budget_paused`, clear the flag and set state back to `TASK_RUNNABLE`.
  Return the old value.
- `SYS_SVC_PAUSE`: user service → same as budget pause (set `budget_paused`,
  block, or unblock); kernel-attached → call the service's `pause(on)` hook.
- `SYS_SVC_KILL`: reject pid 0, the caller's own pid, unknown pids, and any
  kernel-attached service (return -1). Otherwise mark `TASK_ZOMBIE` with
  `exit_code=-1`, fire `il_event_exit()`, call `model_exit_task()`, free the
  address space. The target is not running when killed (only the caller
  runs), so freeing its kernel stack and address space is safe.

**Restart enforcement** (kernel, in `sched_exit_user` `sched.c:120`): if the
exiting task is a registered user service with `restart != NEVER`, the
scheduler re-spawns it from `source` and increments `restarts` instead of
letting the slot go idle. The policy (which services restart, on what
conditions) is NOC-configurable via the registry.

## 5. NOC builtins (`kernel/noc/vm.c`)

Thin wrappers over the new syscalls, added to the builtin registry:

| Signature | Effect |
|-----------|--------|
| `I64 SvcCount()`         | Number of registered services |
| `I64 SvcPid(I64 i)`      | Pid of the i-th service (-1 if stopped/kernel) |
| `I64 SvcField(I64 pid, I64 field)` | One scalar field of a service/task |
| `I64 SvcBudget(I64 pid, I64 ms)`   | Set/raise a service's CPU budget; returns old budget |
| `I64 SvcPause(I64 pid, I64 on)`    | Pause or resume a service |
| `I64 SvcKill(I64 pid)`    | Terminate a user service |
| `U0 Ps()`                 | Server-side table dump via `noc_os_puts` |

`Ps()` is the REPL convenience (immediate answer, no script), matching the
kernel's `sched_ps()` but emitting through `noc_os_puts` so it works in both
kernel and user builds. The full manager is `taskmgr.noc` below.

## 6. NOC service manager program

`taskmgr.noc` is the milestone deliverable: a NOC program, spawnable with
`sched_spawn()`, that owns service-management policy. It can be run
interactively from the REPL or registered as an auto-start user service.

One-shot inventory:

```
PrintLn("svc pid state cpu_ms heap budget self");
for (I64 i = 0; i < SvcCount(); i++) {
    I64 p = SvcPid(i);
    PrintLn("%d %d %d %d %d %d",
        p, SvcField(p,0), SvcField(p,1)*10,
        SvcField(p,2), SvcField(p,6), SvcField(p,8));
}
```

Interactive manager loop (budget runaway services, pause noisy ones, kill
crashed ones, restart):

```
U0 Tmgr() {
    while (1) {
        for (I64 i = 0; i < SvcCount(); i++) {
            I64 p = SvcPid(i);
            if (p <= 0) continue;                       /* stopped / kernel */
            if (SvcField(p, 1) > 200 && SvcField(p, 6) == 0)
                SvcBudget(p, 500);                      /* cap runaway at 500 ms */
            if (SvcField(p, 7) == 1)                    /* paused by budget */
                SvcPause(p, 1);                         /* keep paused / reschedule */
            PrintLn("%d %d", p, SvcField(p, 1) * 10);
        }
        Sleep(1000);
    }
}
Tmgr();
```

This is deliberately policy-in-NOC: the kernel only accounts, budgets, and
enforces; the thresholds, the pause-vs-kill decisions, and the refresh
cadence are editable NOC source — which is exactly what the M5 corpus can
propose and validate.

## 7. Service examples beyond AI/ML

The same mechanisms govern ordinary system work — the point of M9 is that
nothing is special-cased:

- **interact logger** (kernel): `SvcPause(0, 1)` halts interaction capture
  (the `il_cap_on` flag in `interact.c`); `SvcField` reads log length.
- **idle trainer** (kernel): paused = idle retrain disabled (`train.c`
  gate); resumed = re-armed. A NOC automation could pause it during a
  batch job and resume after.
- **Predictor** (kernel): pause suppresses next-key prediction.
- **NOC user service** ("cron-like"): a NOC program `while(1){ Job(); Sleep(60000); }`
  registered with `SVC_RESTART_ALWAYS` — if it crashes, the scheduler brings
  it back; the task manager budgets it so a buggy job can't burn CPU
  forever.
- **M5 pipeline**: drafts run as user services with tight budgets and
  `SVC_RESTART_NEVER`; a draft that exceeds its burst budget gets paused and
  reaped by the manager — the CPU-budget finding #7 becomes an enforcement
  point for self-evolution safety.

## 8. Acceptance

1. Boot: `Ps()` lists the kernel-attached services (`interact`, `trainer`,
   `predictor`, `fs`, pid 0) and any auto-start user services.
2. Spawn two scripts: `while(1){};` (spin) and `Sleep(1000);` (idle). `Ps()`
   shows both, the spinner accumulating `cpu_ms`, the sleeper not.
3. `SvcBudget(<spin pid>, 500);` — within ~2 ticks the spinner's state flips
   to paused and `cpu_ms` freezes at ≈500.
4. `SvcBudget(<spin pid>, 5000);` — it resumes (running, `cpu_ms` grows).
5. `SvcPause(<trainer pid>, 1);` — trainer stops firing on idle; `SvcPause(...,0)`
   re-arms it (assert via a `train:` output line that stops then resumes).
6. Register a demo NOC service with `SVC_RESTART_ALWAYS`; force it to exit
   non-zero; assert it is re-spawned and `SvcField(p, ...)` shows a restart.
7. `SvcKill` the spinner — it disappears from `Ps()`, its slot is reused.
8. `SvcKill(0); SvcKill(<own pid>); SvcKill(<bogus pid>); SvcKill(<kernel svc>)`
   all return -1 — the manager cannot kill the REPL, itself, or the kernel.

The harness (`scripts/build.ps1` M-series block) feeds the script text via
`PrintLn` lines and asserts on serial output patterns, mirroring the
existing M3 `AAAA`/bigram harness at `build.ps1:985-1073`.

## 9. Non-goals (M9)

- No priorities or nice values — round-robin stays flat.
- No per-task instruction counting in the NOC VM; the PIT-tick charge is the
  unit of accounting.
- No inter-process signals or message passing — `SvcPause`/`SvcKill` and the
  restart policy are the only control channels (a future `SendSignal`-style
  syscall is out of scope).
- No CPU-percentage sampling display (needs time-averaging state); `Ps`
  shows raw cumulative ticks, which NOC formats as ms.
- No persistent service registry or process table across reboot (the M4 FS
  could later persist the auto-start boot list).
- Kernel-attached services cannot be killed or replaced in place; they can
  only be paused/resumed and budgeted.
- The REPL (task 0) is never enumerable, pausable, or killable.
